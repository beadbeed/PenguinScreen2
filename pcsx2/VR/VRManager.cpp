// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/VRManager.h"
#include "VR/CameraDriver.h"
#include "VR/HeadPose.h"
#include "VR/StereoAuto.h"
#include "VR/StereoState.h"
#include "VR/VRProfileDB.h"
#include "VR/SplitState.h"
#include "VR/SeatSession.h"
#include "VR/XRCompositor.h"
#include "VR/XRSession.h"
#include "VR/VRVulkanBridge.h"

#include "BuildVersion.h"
#include "Config.h"
#include "Host.h"
#include "MTGS.h"
#include "Memory.h"
#include "VMManager.h"
#include "Input/InputManager.h"
#include "Input/VRInputSource.h"
#include "USB/USB.h"

#include "GS/GSVector.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"

#include "common/Console.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <openxr/openxr.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace VR
{
	namespace
	{
		const char* ResultToString(XrResult result)
		{
			switch (result)
			{
				case XR_SUCCESS:                          return "XR_SUCCESS";
				case XR_ERROR_RUNTIME_UNAVAILABLE:        return "XR_ERROR_RUNTIME_UNAVAILABLE";
				case XR_ERROR_RUNTIME_FAILURE:            return "XR_ERROR_RUNTIME_FAILURE";
				case XR_ERROR_INSTANCE_LOST:              return "XR_ERROR_INSTANCE_LOST";
				case XR_ERROR_INITIALIZATION_FAILED:      return "XR_ERROR_INITIALIZATION_FAILED";
				case XR_ERROR_API_VERSION_UNSUPPORTED:    return "XR_ERROR_API_VERSION_UNSUPPORTED";
				case XR_ERROR_FORM_FACTOR_UNAVAILABLE:    return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
				case XR_ERROR_FORM_FACTOR_UNSUPPORTED:    return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
				case XR_ERROR_OUT_OF_MEMORY:              return "XR_ERROR_OUT_OF_MEMORY";
				case XR_ERROR_LIMIT_REACHED:              return "XR_ERROR_LIMIT_REACHED";
				case XR_ERROR_EXTENSION_NOT_PRESENT:      return "XR_ERROR_EXTENSION_NOT_PRESENT";
				case XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED: return "XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED";
				default:                                  return nullptr;
			}
		}

		std::string FormatResult(XrResult result)
		{
			if (const char* name = ResultToString(result))
				return name;
			return fmt::format("XrResult({})", static_cast<int>(result));
		}
	}

	std::string GetRuntimeInfoReport()
	{
		std::string report;
		const auto append = [&report](std::string_view line) {
			report.append(line);
			report.push_back('\n');
		};

		uint32_t count = 0;
		XrResult res = xrEnumerateApiLayerProperties(0, &count, nullptr);
		std::vector<XrApiLayerProperties> layers;
		if (XR_SUCCEEDED(res) && count > 0)
		{
			layers.resize(count, {XR_TYPE_API_LAYER_PROPERTIES});
			res = xrEnumerateApiLayerProperties(count, &count, layers.data());
		}

		count = 0;
		res = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
		if (res == XR_ERROR_RUNTIME_UNAVAILABLE || res == XR_ERROR_INITIALIZATION_FAILED)
		{
			append(fmt::format("No OpenXR runtime is available ({}).", FormatResult(res)));
			append("Install/start one: SteamVR, Meta Quest Link, Virtual Desktop (VDXR),");
			append("or Monado/WiVRn on Linux, and make sure it is set as the active");
			append("OpenXR runtime in its settings.");
			return report;
		}
		if (XR_FAILED(res))
		{
			append(fmt::format("xrEnumerateInstanceExtensionProperties failed: {}", FormatResult(res)));
			return report;
		}

		std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
		if (count > 0)
			xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data());

		XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
		std::strncpy(ici.applicationInfo.applicationName, "PenguinScreen2", XR_MAX_APPLICATION_NAME_SIZE - 1);
		std::strncpy(ici.applicationInfo.engineName, "PenguinScreen2", XR_MAX_ENGINE_NAME_SIZE - 1);
		ici.applicationInfo.applicationVersion = 1;
		ici.applicationInfo.engineVersion = 1;
		ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;

		XrInstance instance = XR_NULL_HANDLE;
		res = xrCreateInstance(&ici, &instance);
		if (XR_FAILED(res))
		{
			append(fmt::format("xrCreateInstance failed: {}", FormatResult(res)));
			append("An OpenXR runtime appears to be installed but could not be initialized.");
			return report;
		}

		XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
		if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &ip)))
		{
			append(fmt::format("Runtime: {} {}.{}.{}", ip.runtimeName,
				XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
				XR_VERSION_PATCH(ip.runtimeVersion)));
		}

		XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
		sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		XrSystemId system = XR_NULL_SYSTEM_ID;
		res = xrGetSystem(instance, &sgi, &system);
		if (res == XR_ERROR_FORM_FACTOR_UNAVAILABLE)
		{
			append("Headset: none ready (runtime is active, but no HMD is connected/awake).");
			append("If using a Quest: start Link/Air Link or Virtual Desktop first.");
		}
		else if (XR_FAILED(res))
		{
			append(fmt::format("xrGetSystem failed: {}", FormatResult(res)));
		}
		else
		{
			XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
			if (XR_SUCCEEDED(xrGetSystemProperties(instance, system, &sp)))
			{
				append(fmt::format("Headset: {} (vendor 0x{:04X})", sp.systemName, sp.vendorId));
				append(fmt::format("  Max swapchain: {}x{}, max composition layers: {}",
					sp.graphicsProperties.maxSwapchainImageWidth,
					sp.graphicsProperties.maxSwapchainImageHeight,
					sp.graphicsProperties.maxLayerCount));
				append(fmt::format("  Tracking: orientation={} position={}",
					sp.trackingProperties.orientationTracking ? "yes" : "no",
					sp.trackingProperties.positionTracking ? "yes" : "no"));
			}

			count = 0;
			if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(instance, system,
					XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr)) && count > 0)
			{
				std::vector<XrViewConfigurationView> views(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
				if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(instance, system,
						XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, views.data())))
				{
					for (uint32_t i = 0; i < count; i++)
					{
						append(fmt::format("  Eye {}: recommended {}x{} ({}x MSAA), max {}x{}",
							i, views[i].recommendedImageRectWidth, views[i].recommendedImageRectHeight,
							views[i].recommendedSwapchainSampleCount,
							views[i].maxImageRectWidth, views[i].maxImageRectHeight));
					}
				}
			}
		}

		append(fmt::format("API layers ({}):", layers.size()));
		for (const XrApiLayerProperties& layer : layers)
			append(fmt::format("  {} (spec {}.{}.{})", layer.layerName,
				XR_VERSION_MAJOR(layer.specVersion), XR_VERSION_MINOR(layer.specVersion),
				XR_VERSION_PATCH(layer.specVersion)));

		append(fmt::format("Instance extensions ({}):", extensions.size()));
		for (const XrExtensionProperties& ext : extensions)
			append(fmt::format("  {} (v{})", ext.extensionName, ext.extensionVersion));

		xrDestroyInstance(instance);
		return report;
	}

	namespace
	{
		std::mutex s_settings_mutex;
		Pcsx2Config::VROptions s_settings;

		int s_scene_published = -1;
		bool s_scene_memo_valid = false;
		int s_scene_pending = -1;
		u32 s_scene_pending_count = 0;
		constexpr u32 SCENE_DEBOUNCE_VSYNCS = 3;
		// near_floor of the last first-person publish, so a NearDepthMetres change republishes even
		// though the scene itself did not change.
		float s_scene_published_near_floor = 0.0f;
		// Separation and convergence of the last publish (0 unless first person), so each eased step republishes.
		float s_scene_published_sep = 0.0f;
		float s_scene_published_conv = 0.0f;

		// First-person auto stereo ease (StereoAuto). Armed by the publish that puts first person up; any other
		// publish or an early return disarms it, so the next first person eases in again from zero separation. The
		// published values come only from this latched ramp (from -> target over kEaseVsyncs vsyncs), never straight
		// from the per-vsync derive, so IPD jitter cannot republish every vsync.
		bool s_fp_armed = false;
		StereoAuto::Result s_fp_target{};
		float s_fp_from_sep = 0.0f;
		float s_fp_from_conv = 0.0f;
		u32 s_fp_step = 0;
		u64 s_fp_ramp_start_ms = 0;
		bool s_fp_ease_logged = false;
		u64 s_fp_retarget_log_ms = 0;

		// PCSX2_VR_FAKE_IPD_MM=<mm> (40-90): first-person auto stereo uses this IPD instead of the runtime's, for
		// headset-free tests. Offline only: 0 (the runtime's IPD) while the network adapter is on.
		float FakeIpdMetres()
		{
			static const float s_fake_mm = []() {
				const char* env = std::getenv("PCSX2_VR_FAKE_IPD_MM");
				if (!env || env[0] == '\0')
					return 0.0f;
				char* end = nullptr;
				const float mm = std::strtof(env, &end);
				if (end == env || !std::isfinite(mm) || mm < 40.0f || mm > 90.0f)
				{
					Console.Warning("(VR) PCSX2_VR_FAKE_IPD_MM='%s' is not an IPD of 40-90 mm; ignoring it.", env);
					return 0.0f;
				}
				return mm;
			}();
			if (s_fake_mm <= 0.0f)
				return 0.0f;
			if (VMManager::IsOnlineLocked())
			{
				static bool s_refused_logged = false;
				if (!s_refused_logged)
				{
					s_refused_logged = true;
					Console.WriteLn("(VR) PCSX2_VR_FAKE_IPD_MM refused: the network adapter is on (test hooks are offline only).");
				}
				return 0.0f;
			}
			static bool s_used_logged = false;
			if (!s_used_logged)
			{
				s_used_logged = true;
				Console.WriteLn("(VR) PCSX2_VR_FAKE_IPD_MM: first-person auto stereo uses IPD %.1f mm instead of the runtime's (test only).",
					s_fake_mm);
			}
			return s_fake_mm / 1000.0f;
		}

		// The first-person stereo line: the target values, where they came from, then what happens next (tail).
		void LogFirstPersonStereo(const StereoAuto::Result& t, float nearest, float units, bool roll_parallax,
			const std::string& tail)
		{
			std::string line = fmt::format("(VR) First-person stereo: sep {:.4f}, conv {:.0f} ({:.2f} m), ", t.sep, t.conv,
				t.conv / units);
			if (nearest > 0.0f)
				line += fmt::format("nearest {:.2f} m (floor {:.2f})", nearest, t.near_floor);
			else
				line += "nearest off";
			line += fmt::format(", roll parallax {}; ", roll_parallax ? "on" : "off");
			if (t.source == StereoAuto::Source::Auto)
			{
				line += fmt::format("auto from IPD {:.1f} mm ({}) on a {:.2f} x {:.2f} m screen at {:.2f} m, strength {:.2f}",
					t.ipd_m * 1000.0f, t.ipd_is_test ? "test" : "runtime", t.width_m, t.height_m, t.distance_m, t.strength);
				if (t.ndc_ratio != 1.0f)
					line += fmt::format(", NDC width ratio {:.3f}", t.ndc_ratio);
				if (t.sep_railed)
					line += fmt::format(", separation capped at {:.4f} (rail)", t.sep);
				if (t.conv_railed)
					line += fmt::format(", convergence clamped to {:.0f} (rail)", t.conv);
			}
			else
			{
				line += fmt::format("profile values ({}", StereoAuto::ReasonText(t.reason));
				if (t.reason == StereoAuto::Reason::IpdOutOfRange)
					line += fmt::format(": IPD {:.1f} mm", t.ipd_m * 1000.0f);
				line += fmt::format("), strength {:.2f}", t.strength);
			}
			if (t.floor_railed)
				line += ", floor capped at -2 (rail)";
			line += "; ";
			line += tail;
			Console.WriteLn("%s", line.c_str());
		}

		// Presentation lift, published every vsync by ApplySceneStereo (CPU thread) for PresentationLift
		// (GS thread), which must not touch the profile DB or EmuConfig.
		std::atomic<bool> s_lift_has_profile{false};
		std::atomic<bool> s_lift_fp_only{true};
		std::atomic<float> s_lift_gamma{1.0f};
		std::atomic<float> s_lift_brightness{1.0f};
		std::atomic<float> s_lift_user_gamma{1.0f};

		void CopyResolvedMap(StereoState::Params& dst, const ProfileDB::StereoResolvedMap& src)
		{
			static_assert(static_cast<u32>(ProfileDB::StereoMap::Linear) == static_cast<u32>(StereoState::Params::Map::Linear) &&
							  static_cast<u32>(ProfileDB::StereoMap::Bands) == static_cast<u32>(StereoState::Params::Map::Bands) &&
							  static_cast<u32>(ProfileDB::StereoMap::Log) == static_cast<u32>(StereoState::Params::Map::Log),
				"StereoMap and StereoState::Params::Map must agree numerically — the copy below casts through u32");
			dst.map = static_cast<StereoState::Params::Map>(src.map);
			dst.band_count = src.band_count;
			for (u32 i = 0; i < 3; i++)
				dst.split_q[i] = src.split_q[i];
			for (u32 i = 0; i < 4; i++)
			{
				dst.conv[i] = src.conv[i];
				dst.sep[i] = src.sep[i];
				dst.bias[i] = src.bias[i];
			}
			dst.log_w0 = src.log_w0;
			dst.log_w1 = src.log_w1;
			dst.log_dfar = src.log_dfar;
		}

		void CopyCollimate(StereoState::Params& dst, const ProfileDB::StereoParams& src)
		{
			static_assert(ProfileDB::kMaxCollimateRules == StereoState::Params::MAX_COLLIMATE_RULES,
				"The authored rule cap and the GS snapshot's fixed array must agree — otherwise a "
				"profile's last rules are parsed and then silently dropped on the way to the shader");
			dst.collimate_disparity = 0.0f;
			dst.collimate_rule_count = 0;
			if (!src.hud_collimate.has_value())
				return;

			const ProfileDB::HudCollimate& hc = src.hud_collimate.value();
			dst.collimate_disparity = hc.disparity;
			for (const ProfileDB::CollimateRule& r : hc.rules)
			{
				if (dst.collimate_rule_count >= StereoState::Params::MAX_COLLIMATE_RULES)
					break;
				StereoState::Params::CollimateRule& o = dst.collimate_rules[dst.collimate_rule_count++];
				o.prim = r.prim;
				o.tme = r.tme;
				o.abe = r.abe;
				o.min_w = r.min_w;
				o.max_w = r.max_w;
				o.min_h = r.min_h;
				o.max_h = r.max_h;
				o.rx0 = r.rx0;
				o.ry0 = r.ry0;
				o.rx1 = r.rx1;
				o.ry1 = r.ry1;
				o.tu0 = r.tu0;
				o.tv0 = r.tv0;
				o.tu1 = r.tu1;
				o.tv1 = r.tv1;
				StringUtil::Strlcpy(o.label, r.label, sizeof(o.label));
			}
		}

		std::string FormatStereoMap(const ProfileDB::StereoResolvedMap& map, float separation, float convergence)
		{
			const float d_inf = ProfileDB::EvalDisparity(map, separation, convergence, 0.0f);
			if (map.map == ProfileDB::StereoMap::Log)
			{
				return fmt::format("log w {:.4g}->{:.4g} dfar {:.4g} | d(inf) {:.4f}",
					map.log_w0, map.log_w1, map.log_dfar, d_inf);
			}

			std::string out = fmt::format("bands({})", map.band_count);
			for (u32 i = 0; i + 1 < map.band_count && i < 3; i++)
			{
				out += fmt::format("{}{:.4g}", (i == 0) ? " splits w[" : ", ",
					(map.split_q[i] != 0.0f) ? (1.0f / map.split_q[i]) : 0.0f);
			}
			if (map.band_count > 1)
				out += "]";
			for (u32 i = 0; i < map.band_count && i < 4; i++)
				out += fmt::format(" | c{:.4g}/s{:.4g}", map.conv[i], map.sep[i]);
			out += fmt::format(" | d(inf) {:.4f}", d_inf);
			return out;
		}
	}

	static void ApplySpatialControls(const ProfileDB::Profile* profile)
	{
		std::vector<ProfileDB::SpatialControlSpec> specs;
		if (profile && !profile->controls.empty() && WantsVR())
			specs = profile->controls;

		VRInputSource::SetActiveControls(specs);

		std::vector<InputManager::VRBindingOverlayEntry> overlay = VRInputSource::BuildBindingOverlay(specs,
			[](u32 usb_port) { return Host::GetStringSettingValue(USB::GetConfigSection(usb_port).c_str(), "Type", "None"); });
		const size_t count = overlay.size();
		if (InputManager::SetVRBindingOverlay(std::move(overlay)))
		{
			if (count > 0)
				Console.WriteLn("(VR) Spatial controls: %zu control(s), %zu binding(s) overlaid from the profile; reloading input bindings.",
					specs.size(), count);
			else
				Console.WriteLn("(VR) Spatial controls: profile bindings restored; reloading input bindings.");
			VMManager::ReloadInputBindings(true);
		}
	}

	void ClearSpatialControls()
	{
		ApplySpatialControls(nullptr);
	}

	void UpdateSettings()
	{
		const Pcsx2Config::VROptions& new_settings = EmuConfig.VR;

		bool enable_changed;
		{
			std::lock_guard lock(s_settings_mutex);
			enable_changed = (s_settings.Enable != new_settings.Enable);
			s_settings = new_settings;
		}

		ProfileDB::ReloadIfChanged();
		const std::string serial = VMManager::GetDiscSerial();
		const ProfileDB::Profile* profile =
			serial.empty() ? nullptr : ProfileDB::Lookup(serial, VMManager::GetDiscCRC());

		ApplySpatialControls(profile);

		float screen_distance = new_settings.ScreenDistance;
		float screen_height = new_settings.ScreenHeight;
		float screen_arc = new_settings.ScreenArcDeg;
		bool screen_follow_head = false;
		if (profile)
		{
			screen_distance = profile->screen_distance.value_or(screen_distance);
			screen_height = profile->screen_height.value_or(screen_height);
			screen_arc = profile->screen_arc_deg.value_or(screen_arc);
			screen_follow_head = profile->screen_follow_head.value_or(false);
		}
		XRCompositor::UpdateScreenParams(screen_distance, screen_height, screen_arc,
			new_settings.ScreenVerticalOffset, screen_follow_head);
		XRCompositor::UpdateFirstPersonScreen(profile && profile->fp_screen_height.has_value(),
			profile ? profile->fp_screen_distance.value_or(1.5f) : 1.5f,
			profile ? profile->fp_screen_height.value_or(2.5f) : 2.5f,
			profile ? profile->fp_screen_arc_deg.value_or(0.0f) : 0.0f,
			profile ? profile->fp_screen_pose_lag_ms.value_or(40.0f) : 40.0f);
		XRCompositor::UpdateMenuScreen(profile ? profile->menu_screen_distance.value_or(0.70f) : 0.70f,
			profile ? profile->menu_screen_height.value_or(0.36f) : 0.36f,
			profile ? profile->menu_screen_drop.value_or(0.30f) : 0.30f,
			profile ? profile->menu_screen_tilt_deg.value_or(-1.0f) : -1.0f);

		StereoState::Params stereo;
		stereo.separation = new_settings.StereoSeparation;
		stereo.convergence = new_settings.StereoConvergence;
		stereo.uv_policy = StereoState::Params::UvPolicy::Screen;

		bool from_profile = false;
		if (new_settings.StereoUseProfile && profile && profile->stereo)
		{
			stereo.separation = profile->stereo->separation;
			stereo.convergence = profile->stereo->convergence;
			stereo.uv_policy = (profile->stereo->uv_draws == ProfileDB::UvDrawPolicy::World)
									? StereoState::Params::UvPolicy::World
									: StereoState::Params::UvPolicy::Screen;
			stereo.pin_uniform_q = profile->stereo->pin_uniform_q;
			stereo.z_driven_depth = profile->stereo->z_driven_depth;
			CopyCollimate(stereo, *profile->stereo);
			CopyResolvedMap(stereo, profile->stereo->resolved);
			from_profile = true;
		}

		stereo.enabled = StereoGateOpen(new_settings.Enable, LaunchRequestedVR(), StereoRenderArmed(),
			new_settings.StereoMode, from_profile || !new_settings.StereoUseProfile);

		if (MTGS::IsOpen())
			MTGS::RunOnGSThread([stereo]() { StereoState::Publish(stereo); });
		else
			StereoState::Publish(stereo);

		ProfileDB::StereoResolvedMap osd_map;
		if (from_profile)
			osd_map = profile->stereo->resolved;

		static bool s_osd_enabled = false;
		static std::string s_osd_text;
		std::string osd_text;
		if (stereo.enabled)
		{
			const char* provenance = from_profile ? " (game profile)" : "";
			osd_text = (osd_map.map == ProfileDB::StereoMap::Linear)
			               ? fmt::format("Stereo: separation {:.3f}, convergence {:.4g}{}", stereo.separation,
			                     stereo.convergence, provenance)
			               : fmt::format("Stereo: {}{}",
			                     FormatStereoMap(osd_map, stereo.separation, stereo.convergence), provenance);
		}
		if (stereo.enabled != s_osd_enabled || (stereo.enabled && osd_text != s_osd_text))
		{
			if (stereo.enabled)
				Host::AddKeyedOSDMessage("VRStereo", osd_text, 5.0f);
			else if (s_osd_enabled)
				Host::AddKeyedOSDMessage("VRStereo", "Stereo: off", 3.0f);
			s_osd_enabled = stereo.enabled;
			s_osd_text = std::move(osd_text);
		}

		if (enable_changed)
		{
			Console.WriteLn("(VR) VR %s in config; this takes effect when the renderer restarts (Vulkan only).",
				new_settings.Enable ? "enabled" : "disabled");
			if (new_settings.Enable)
				Console.WriteLn("(VR) PenguinScreen2 build %s", BuildVersion::GitRev);
		}

		s_scene_published = -1;
		s_scene_memo_valid = false;
		SplitState::InvalidateMemo();
		s_scene_pending = -1;
		s_scene_pending_count = 0;
	}

	static constexpr int kFirstPersonScene = -2;

	// The lift doesn't depend on stereo, so this runs ahead of the stereo gate. The profile is looked up
	// only while a session runs: flat play does no extra work and always reads neutral.
	static void PublishPresentation(const Pcsx2Config::VROptions& cfg)
	{
		const bool running = XRSession::IsSessionRunning();
		const ProfileDB::Profile* profile = nullptr;
		if (running)
		{
			const std::string serial = VMManager::GetDiscSerial();
			profile = serial.empty() ? nullptr : ProfileDB::Lookup(serial, VMManager::GetDiscCRC());
		}

		const bool has = profile && profile->presentation.has_value();
		const float user = std::isfinite(cfg.Brightness) ? cfg.Brightness : 1.0f;
		s_lift_gamma.store(has ? profile->presentation->gamma : 1.0f, std::memory_order_relaxed);
		s_lift_brightness.store(has ? profile->presentation->brightness : 1.0f, std::memory_order_relaxed);
		s_lift_fp_only.store(has ? profile->presentation->first_person_only : true, std::memory_order_relaxed);
		s_lift_user_gamma.store(running ? user : 1.0f, std::memory_order_relaxed);
		s_lift_has_profile.store(has, std::memory_order_release);
	}

	bool PresentationLift(GSVector4* params)
	{
		// Neutral without a running session, so flat play stays bit-identical.
		if (!XRSession::IsSessionRunning())
			return false;

		// The VR Brightness setting scales gamma everywhere in the headset; the profile's own lift applies
		// only in first person unless it says firstPersonOnly: false.
		float gamma = s_lift_user_gamma.load(std::memory_order_relaxed);
		float brightness = 1.0f;
		if (s_lift_has_profile.load(std::memory_order_acquire) &&
			(!s_lift_fp_only.load(std::memory_order_relaxed) || CameraDriver::LookAtActive()))
		{
			gamma *= s_lift_gamma.load(std::memory_order_relaxed);
			brightness = s_lift_brightness.load(std::memory_order_relaxed);
		}

		if (std::abs(gamma - 1.0f) < 0.001f && std::abs(brightness - 1.0f) < 0.001f)
			return false;

		*params = GSVector4(brightness, 1.0f, 1.0f, std::clamp(gamma, 0.5f, 3.0f));
		return true;
	}

	void ApplySceneStereo()
	{
		StereoAuto::MaybeRunSelfTest();
		const Pcsx2Config::VROptions& cfg = EmuConfig.VR;
		// GS-thread gates, refreshed every vsync: roll parallax follows the setting, and the stereo dump
		// is offline only (never while the network adapter is on, nor under PCSX2_TEST_ONLINE_LOCKS).
		StereoState::SetRollParallaxEnabled(cfg.RollParallax);
		StereoState::SetDumpAllowed(!VMManager::IsOnlineLocked());
		PublishPresentation(cfg);
		if (!StereoGateOpen(cfg.Enable, LaunchRequestedVR(), StereoRenderArmed(), cfg.StereoMode,
				cfg.StereoUseProfile))
		{
			s_scene_published = -1;
			s_scene_memo_valid = false;
			s_scene_pending = -1;
			s_scene_pending_count = 0;
			s_fp_armed = false;
			return;
		}

		ProfileDB::EnsureLoaded();
		const std::string serial = VMManager::GetDiscSerial();
		const ProfileDB::Profile* profile =
			serial.empty() ? nullptr : ProfileDB::Lookup(serial, VMManager::GetDiscCRC());
		const bool has_fp_stereo = profile && profile->stereo.has_value() &&
		                           (profile->stereo->fp_separation.has_value() || profile->stereo->fp_convergence.has_value());
		if (!profile || !profile->stereo.has_value() || (profile->stereo->scenes.empty() && !has_fp_stereo))
		{
			s_scene_published = -1;
			s_scene_memo_valid = false;
			s_scene_pending = -1;
			s_scene_pending_count = 0;
			s_fp_armed = false;
			return;
		}

		const ProfileDB::StereoParams& base = profile->stereo.value();

		// -2 = first person (camera.lookAt driving the camera), which wins over the scene rules.
		int match = -1;
		if (has_fp_stereo && CameraDriver::LookAtActive())
			match = kFirstPersonScene;
		for (size_t i = 0; match == -1 && i < base.scenes.size(); i++)
		{
			const ProfileDB::StereoSceneRule& rule = base.scenes[i];
			u32 value = 0;
			switch (rule.width)
			{
				case 1: value = memRead8(rule.ee_address); break;
				case 2: value = memRead16(rule.ee_address); break;
				default: value = memRead32(rule.ee_address); break;
			}
			if (value == rule.equals)
			{
				match = static_cast<int>(i);
				break;
			}
		}

		// First person: how near true stereo depth reaches (metres, 0 = off). The linear map
		// sep * (1 - conv * q) equals an off-axis camera pair for w >= conv; letting it go negative down to
		// 1 - conv / (nearest * units) extends that to `nearest` instead of flattening onto the conv plane.
		// Separation and convergence come from StereoAuto: the headset IPD over the first-person screen's width and the
		// screen distance (profiles with stereo.firstPerson.auto), else the profile numbers; the floor is generalised
		// so 'nearest' is always the closest perceived distance. Each arm eases the separation in from zero.
		float fp_nearest = 0.0f;
		float fp_units = 100.0f;
		StereoAuto::Result fp_now{};
		float fp_sep = 0.0f;
		float fp_conv = 0.0f;
		float fp_floor = 0.0f;
		if (match == kFirstPersonScene)
		{
			if (cfg.NearDepthMetres > 0.0f)
				fp_nearest = std::clamp(cfg.NearDepthMetres, 0.45f, 1.5f);
			else if (cfg.NearDepthMetres == 0.0f && base.fp_nearest.has_value())
				fp_nearest = base.fp_nearest.value();

			if (profile->camera.has_value() && profile->camera->look_at.has_value())
			{
				const float u = std::abs(profile->camera->look_at->units_per_meter);
				if (std::isfinite(u) && u > 0.0f)
					fp_units = u;
			}

			StereoAuto::Inputs in;
			in.profile_auto = base.fp_auto;
			in.setting_auto = cfg.FirstPersonAutoStereo;
			const XRCompositor::EyeInfo eyes = XRCompositor::GetEyeInfo();
			in.eyes_valid = eyes.valid;
			in.ipd_m = eyes.ipd_m;
			if (eyes.valid)
			{
				const float fake_ipd = FakeIpdMetres();
				if (fake_ipd > 0.0f)
				{
					in.ipd_m = fake_ipd;
					in.ipd_is_test = true;
				}
			}
			const XRCompositor::FirstPersonScreenInfo screen = XRCompositor::GetFirstPersonScreenInfo();
			in.screen_valid = screen.valid;
			in.screen_distance_m = screen.distance_m;
			in.screen_width_m = screen.width_m;
			in.screen_height_m = screen.height_m;
			in.screen_curved = screen.curved;
			in.units_per_meter = fp_units;
			in.profile_sep = base.fp_separation.value_or(base.separation);
			{
				// The profile's separation is tuned for the screen its camera.fov gives. With FirstPersonFov = Auto the
				// screen grows with the fitted FOV, so the same NDC separation would put far content more than an IPD apart
				// (eyes turned outward past parallel) whenever the fallback is used (auto stereo off, an IPD out of range):
				// scale it so its physical disparity stays the profile's.
				const CameraDriver::FovState fov = CameraDriver::GetFovState();
				constexpr float kHalfDegToRad = 3.14159265f / 360.0f;
				if (fov.sizes_screen && fov.profile_deg > 0.0f && fov.target_deg > fov.profile_deg && fov.target_deg < 179.0f)
					in.profile_sep *= std::tan(fov.profile_deg * kHalfDegToRad) / std::tan(fov.target_deg * kHalfDegToRad);
			}
			in.profile_conv = base.fp_convergence.value_or(base.convergence);
			in.nearest_m = fp_nearest;
			in.strength = cfg.FirstPersonStereoStrength;
			in.ndc_width_ratio = base.fp_ndc_width_ratio;
			fp_now = StereoAuto::Derive(in);

			if (s_fp_armed)
			{
				if (StereoAuto::NeedsRetarget(s_fp_target, fp_now))
				{
					// Ease from where the ramp is now to the new target (the IPD arrived, a strength edit, an aspect change).
					s_fp_from_sep = StereoAuto::EaseValue(s_fp_from_sep, s_fp_target.sep, s_fp_step);
					s_fp_from_conv = StereoAuto::EaseValue(s_fp_from_conv, s_fp_target.conv, s_fp_step);
					s_fp_target = fp_now;
					s_fp_step = 0;
					s_fp_ramp_start_ms = CameraDriver::SteadyNowMs();
					s_fp_ease_logged = false;
					if (s_fp_retarget_log_ms == 0 || s_fp_ramp_start_ms >= s_fp_retarget_log_ms + 2000)
					{
						s_fp_retarget_log_ms = s_fp_ramp_start_ms;
						LogFirstPersonStereo(s_fp_target, fp_nearest, fp_units, cfg.RollParallax,
							fmt::format("retargeting over {} vsyncs (target changed)", StereoAuto::kEaseVsyncs));
					}
				}
				else
				{
					// A near_floor-only change (NearDepthMetres edited) applies at once and leaves the ramp alone.
					if (std::abs(fp_now.near_floor - s_fp_target.near_floor) > 1e-4f)
					{
						s_fp_target.near_floor = fp_now.near_floor;
						s_fp_target.floor_railed = fp_now.floor_railed;
						LogFirstPersonStereo(s_fp_target, fp_nearest, fp_units, cfg.RollParallax, "nearest changed");
					}
					if (s_fp_step < StereoAuto::kEaseVsyncs)
						s_fp_step++;
				}
				fp_sep = StereoAuto::EaseValue(s_fp_from_sep, s_fp_target.sep, s_fp_step);
				fp_conv = StereoAuto::EaseValue(s_fp_from_conv, s_fp_target.conv, s_fp_step);
				fp_floor = s_fp_target.near_floor;
			}
			else
			{
				// An arm starts from zero separation, convergence already at its target.
				fp_sep = 0.0f;
				fp_conv = fp_now.conv;
				fp_floor = fp_now.near_floor;
			}
		}

		// First person stays up but what it publishes changed (an eased step, a retarget, a near_floor edit, or the
		// ease just finished and its line is due): republish now, past the scene debounce.
		const bool fp_values_changed = s_scene_memo_valid && match == kFirstPersonScene &&
		                               s_scene_published == kFirstPersonScene &&
		                               (fp_sep != s_scene_published_sep || fp_conv != s_scene_published_conv ||
		                                   fp_floor != s_scene_published_near_floor ||
		                                   (s_fp_armed && s_fp_step >= StereoAuto::kEaseVsyncs && !s_fp_ease_logged));

		if (s_scene_memo_valid && !fp_values_changed)
		{
			if (match == s_scene_published)
			{
				s_scene_pending = match;
				s_scene_pending_count = 0;
				return;
			}
			if (match != s_scene_pending)
			{
				s_scene_pending = match;
				s_scene_pending_count = 1;
				return;
			}
			if (++s_scene_pending_count < SCENE_DEBOUNCE_VSYNCS)
				return;
		}

		StereoState::Params stereo;
		stereo.enabled = true;
		stereo.separation = base.separation;
		stereo.convergence = base.convergence;
		stereo.uv_policy = (base.uv_draws == ProfileDB::UvDrawPolicy::World) ?
		                       StereoState::Params::UvPolicy::World :
		                       StereoState::Params::UvPolicy::Screen;
		stereo.pin_uniform_q = base.pin_uniform_q;
		stereo.z_driven_depth = base.z_driven_depth;
		CopyCollimate(stereo, base);
		CopyResolvedMap(stereo, base.resolved);
		bool fp_arm_now = false;
		if (match >= 0)
		{
			const ProfileDB::StereoSceneRule& rule = base.scenes[static_cast<size_t>(match)];
			stereo.separation = rule.separation.value_or(stereo.separation);
			stereo.convergence = rule.convergence.value_or(stereo.convergence);
			if (rule.map_override.has_value())
				CopyResolvedMap(stereo, *rule.map_override);
			s_fp_armed = false;
		}
		else if (match == kFirstPersonScene)
		{
			if (!s_fp_armed)
			{
				// Arm: separation ramps from 0 to the target over kEaseVsyncs vsyncs, convergence starts at its target.
				fp_arm_now = true;
				s_fp_armed = true;
				s_fp_target = fp_now;
				s_fp_from_sep = 0.0f;
				s_fp_from_conv = fp_now.conv;
				s_fp_step = 0;
				s_fp_ramp_start_ms = CameraDriver::SteadyNowMs();
				s_fp_ease_logged = false;
				fp_sep = 0.0f;
				fp_conv = fp_now.conv;
				fp_floor = fp_now.near_floor;
			}
			stereo.separation = fp_sep;
			stereo.convergence = fp_conv;
			stereo.first_person = true;
			stereo.near_floor = fp_floor;
			stereo.fp_auto = (s_fp_target.source == StereoAuto::Source::Auto);
			stereo.fp_ipd_m = s_fp_target.ipd_m;
			stereo.fp_screen_w_m = s_fp_target.width_m;
			stereo.fp_screen_h_m = s_fp_target.height_m;
			stereo.fp_screen_d_m = s_fp_target.distance_m;
			stereo.fp_strength = s_fp_target.strength;
			stereo.fp_sep_target = s_fp_target.sep;
			stereo.fp_ease = static_cast<float>(s_fp_step) / static_cast<float>(StereoAuto::kEaseVsyncs);
			stereo.fp_ndc_ratio = s_fp_target.ndc_ratio;
		}
		else
		{
			s_fp_armed = false;
		}

		MTGS::RunOnGSThread([stereo]() { StereoState::Publish(stereo); });

		if (match >= 0)
		{
			const ProfileDB::StereoSceneRule& matched = base.scenes[static_cast<size_t>(match)];
			const std::string& label = matched.label;
			const bool scene_map = matched.map_override.has_value();
			const ProfileDB::StereoResolvedMap& eff = scene_map ? *matched.map_override : base.resolved;
			const std::string body =
				(eff.map == ProfileDB::StereoMap::Linear)
					? fmt::format("sep {:.3f}, conv {:.4g}", stereo.separation, stereo.convergence)
					: fmt::format("{} - {}", FormatStereoMap(eff, stereo.separation, stereo.convergence),
						  scene_map ? "scene map" : "base map");
			Host::AddKeyedOSDMessage("VRStereoScene",
				fmt::format("Stereo scene: {} ({})", label.empty() ? "override" : label, body), 3.0f);
		}
		else if (match == kFirstPersonScene)
		{
			// Once per arm: the target values (the ramp itself starts from zero separation).
			if (fp_arm_now)
			{
				const bool fp_is_auto = (s_fp_target.source == StereoAuto::Source::Auto);
				Host::AddKeyedOSDMessage("VRStereoScene",
					fmt::format("Stereo: first person (sep {:.4f}{}, conv {:.4g})", s_fp_target.sep, fp_is_auto ? " auto" : "",
						s_fp_target.conv),
					3.0f);
				LogFirstPersonStereo(s_fp_target, fp_nearest, fp_units, cfg.RollParallax,
					fmt::format("easing in over {} vsyncs", StereoAuto::kEaseVsyncs));
			}
			if (s_fp_step >= StereoAuto::kEaseVsyncs && !s_fp_ease_logged)
			{
				s_fp_ease_logged = true;
				const u64 now_ms = CameraDriver::SteadyNowMs();
				Console.WriteLn("(VR) First-person stereo: eased in to sep %.4f, conv %.0f after %u vsyncs (%llu ms).",
					fp_sep, fp_conv, s_fp_step,
					static_cast<unsigned long long>((now_ms >= s_fp_ramp_start_ms) ? (now_ms - s_fp_ramp_start_ms) : 0));
			}
		}
		else if (s_scene_memo_valid)
		{
			Host::AddKeyedOSDMessage("VRStereoScene", "Stereo scene: base", 3.0f);
		}

		s_scene_published = match;
		const bool fp_published = (match == kFirstPersonScene);
		s_scene_published_sep = fp_published ? fp_sep : 0.0f;
		s_scene_published_conv = fp_published ? fp_conv : 0.0f;
		s_scene_published_near_floor = fp_published ? fp_floor : 0.0f;
		s_scene_memo_valid = true;
		s_scene_pending = match;
		s_scene_pending_count = 0;
	}

	bool EffectiveVREnabled(bool cfg_enable)
	{
		return cfg_enable || LaunchRequestedVR();
	}

	static bool s_launch_requested_vr = false;

	static bool s_stereo_render_armed = false;

	void SetLaunchRequestedVR(bool requested)
	{
		s_launch_requested_vr = requested;
	}

	static int s_launch_seat = 1;

	void SetLaunchSeat(int seat)
	{
		s_launch_seat = seat;
	}

	int GetLaunchSeat()
	{
		return s_launch_seat;
	}

	static int s_seatcast_target = 0;

	void SetSeatCastTarget(int seat)
	{
		s_seatcast_target = seat;
	}

	int SeatCastTarget()
	{
		return s_seatcast_target;
	}

	bool SeatCastArmed()
	{
		return s_seatcast_target > 0;
	}

	static std::string NthColonEntry(const std::string& reg, int idx)
	{
		size_t pos = 0;
		while (idx > 0 && pos != std::string::npos)
		{
			pos = reg.find(':', pos);
			if (pos != std::string::npos)
				pos++;
			idx--;
		}
		if (pos == std::string::npos)
			return {};
		const size_t end = reg.find(':', pos);
		return reg.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
	}

	std::string ResolveSeatRuntimeJson(int seat)
	{
		std::string json;
		if (seat >= 2)
			json = NthColonEntry(EmuConfig.VR.XrSeatRuntimeJsons, seat - 2);
		if (json.empty())
		{
			const char* env = std::getenv("XR_RUNTIME_JSON");
			if (env)
				json = env;
		}
		return json;
	}

	std::string ResolveSeatRuntimeDir(int seat)
	{
		if (seat <= 1)
			return {};
		return NthColonEntry(EmuConfig.VR.XrSeatRuntimeDirs, seat - 2);
	}

	bool LaunchRequestedVR()
	{
		return s_launch_requested_vr;
	}

	void SetStereoRenderArmed(bool armed)
	{
		s_stereo_render_armed = armed;
	}

	bool StereoRenderArmed()
	{
		return s_launch_requested_vr || s_stereo_render_armed;
	}

	bool WantsVR()
	{
		if (!LaunchRequestedVR())
			return false;
		std::lock_guard lock(s_settings_mutex);
		return EffectiveVREnabled(s_settings.Enable);
	}

	bool IsSessionActive()
	{
		return XRSession::HasSession();
	}

	SessionStatus GetSessionStatus()
	{
		bool enabled;
		{
			std::lock_guard lock(s_settings_mutex);
			enabled = EffectiveVREnabled(s_settings.Enable);
		}
		if (!enabled)
			return SessionStatus::DisabledInConfig;
		if (!LaunchRequestedVR())
			return SessionStatus::NotLaunchedForVR;
		if (IsSessionActive())
			return SessionStatus::Active;
		return SessionStatus::Inactive;
	}

	void EnsureFrameSubmitted()
	{
		if (!IsSessionActive())
			return;
		GSDeviceVK::GetInstance()->ExecuteCommandBuffer(false);
	}

	void EndOfFrame(GSTexture* current)
	{
		if (!XRSession::HasSession())
			return;

		if (SeatCastArmed() && !SeatSession::Running())
			SeatSession::Start(SeatCastTarget());

		// With headset-rate submission the VR pacer pumps the events, so state changes are handled while the GS is idle.
		// A pacer stuck in the runtime pumps nothing: then this thread does, so a loss still reaches the teardown below.
		if (!XRCompositor::PacerOwnsFrames() || XRCompositor::PacerStalled())
			XRSession::PumpEvents();

		if (XRSession::IsLost())
		{
			Console.Error("(VR) Session/runtime lost — shutting VR down; flat rendering continues.");
			// First person disarms through its usual path (camera handed back, body shown) on the next vsync.
			HeadPose::Invalidate();
			Host::AddKeyedOSDMessage("VRLost", "VR runtime lost: playing flat", 10.0f);
			XRCompositor::Shutdown();
			bool abandoned = XRCompositor::Abandoned();
#ifdef ENABLE_VULKAN
			abandoned = abandoned || VR::XrQueueLockAbandoned();
#endif
			// A VR thread stuck inside the runtime: calling into it again could hang this thread too.
			if (abandoned)
			{
				XRSession::ForgetWithoutDestroying();
			}
			else
			{
				XRSession::DestroySession();
				XRSession::DestroyInstance();
			}
			return;
		}

		const StereoState::Params st = StereoState::Get();
		static const bool s_interleave_debug = (std::getenv("PCSX2_VR_INTERLEAVE") != nullptr);
		if (st.enabled && s_interleave_debug && !(current && current->GetArrayLayers() >= 2))
		{
			const u32 eye = StereoState::GetCurrentEye();
			XRCompositor::EndOfFrame(current, eye);
			StereoState::AdvanceEye();
		}
		else
		{
			XRCompositor::EndOfFrame(current, XRCompositor::MonoEye);
		}
	}
}
