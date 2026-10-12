// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/VRInput.h"
#include "VR/HudCards.h"
#include "VR/XRSession.h"

#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace VR
{
	namespace
	{
		constexpr const char* HAND_PATH[2] = {"/user/hand/left", "/user/hand/right"};

		constexpr const char* TOUCH_PROFILE = "/interaction_profiles/oculus/touch_controller";
		constexpr const char* SIMPLE_PROFILE = "/interaction_profiles/khr/simple_controller";
		constexpr const char* FRAME_PROFILE = "/interaction_profiles/valve/frame_controller";

		std::mutex s_snapshot_mutex;
		VRInputSnapshot s_snapshot;

		// A publisher that stops (a failed sync, a dead session, a stalled frame loop) must not leave
		// the last held trigger or stick active while emulation keeps running. Normal publishing is
		// once per presented frame, so this is far longer than any healthy gap.
		constexpr std::uint64_t kSnapshotMaxAgeMs = 500;

		std::uint64_t NowMs()
		{
			return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		void PublishSnapshot(VRInputSnapshot snapshot)
		{
			std::lock_guard lock{s_snapshot_mutex};
			snapshot.generation = s_snapshot.generation + 1;
			snapshot.published_ms = NowMs();
			s_snapshot = snapshot;
		}

		// Neutral and inactive: every consumer releases its controls. Only when something was
		// active, so a session that never had input doesn't appear to come alive.
		void PublishNeutralIfActive()
		{
			bool was_active;
			{
				std::lock_guard lock{s_snapshot_mutex};
				was_active = s_snapshot.generation != 0 && s_snapshot.actions_active;
			}
			if (was_active)
				PublishSnapshot(VRInputSnapshot{});
		}

		void ResetSnapshot()
		{
			std::lock_guard lock{s_snapshot_mutex};
			s_snapshot = VRInputSnapshot{};
		}

		VRInput* s_publisher = nullptr;

		// The controllers have been live at least once this session: only then is a return worth a toast
		// (a session often starts unfocused until the runtime's dashboard closes).
		bool s_actions_were_live = false;

		std::atomic<int> s_haptic_request[2] = {-1, -1};
		int s_haptic_applied[2] = {0, 0};

		std::atomic<int> s_pulse_request[2] = {-1, -1};

		// XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED arrived (XRSession::PumpEvents, any thread): the
		// next Update reports the bindings again.
		std::atomic<bool> s_profile_changed{false};

		constexpr const char* HAND_NAME[2] = {"Left", "Right"};
		constexpr const char* HAND_NAME_LOWER[2] = {"left", "right"};

		// Controller presence (VRInput::UpdatePresence). A hand is live when some action on it has a bound
		// source or its grip is tracked; it takes a short steady run to call it live, a longer one to call
		// it gone, so a single dropped frame or a hand resting out of view says nothing.
		constexpr std::uint64_t kHandLiveMs = 500;
		constexpr std::uint64_t kHandAbsentMs = 3000;
		// Focused time before the session-start check says which controllers never showed up.
		constexpr std::uint64_t kStartCheckMs = 5000;
		// A session whose first sync is unfocused (SteamVR dashboard up) and stays so this long gets a toast.
		constexpr std::uint64_t kUnfocusedStartMs = 2000;
		// One evaluated sync adds at most this much focused time (a stalled frame loop doesn't count).
		constexpr std::uint64_t kPresenceMaxStepMs = 250;
		constexpr float kPresenceToastSeconds = 4.0f;
		constexpr std::uint64_t kToastRepeatMs = 20000;

		int PackPulse(float amplitude, float seconds)
		{
			if (!std::isfinite(amplitude))
				amplitude = 0.f;
			if (!std::isfinite(seconds))
				seconds = 0.f;
			const int permille = static_cast<int>(std::lround(std::clamp(amplitude, 0.f, 1.f) * 1000.f));
			const int ms = static_cast<int>(std::lround(std::clamp(seconds, 0.f, 10.f) * 1000.f));
			return permille | (ms << 16);
		}

		bool CheckXR(XrResult res, const char* what)
		{
			if (XR_SUCCEEDED(res))
				return true;

			char buf[XR_MAX_RESULT_STRING_SIZE] = "?";
			if (XRSession::HasInstance())
				xrResultToString(XRSession::GetInstance(), res, buf);
			Console.Error("(VR) %s failed: %s (%d)", what, buf, static_cast<int>(res));
			return false;
		}

		// For diagnostics only: a failure is worth a warning, not an error.
		void WarnXR(XrResult res, const char* what)
		{
			char buf[XR_MAX_RESULT_STRING_SIZE] = "?";
			if (XRSession::HasInstance())
				xrResultToString(XRSession::GetInstance(), res, buf);
			Console.Warning("(VR) %s failed: %s (%d)", what, buf, static_cast<int>(res));
		}

		std::string PathString(XrInstance instance, XrPath path)
		{
			char buf[XR_MAX_PATH_LENGTH] = {};
			uint32_t len = 0;
			if (XR_FAILED(xrPathToString(instance, path, XR_MAX_PATH_LENGTH, &len, buf)))
				return "?";
			buf[XR_MAX_PATH_LENGTH - 1] = '\0';
			return std::string(buf);
		}

		// "Left Hand Steam Frame Controller Trigger"; the raw path when the runtime gives no name.
		std::string SourceName(XrInstance instance, XrSession session, XrPath source)
		{
			XrInputSourceLocalizedNameGetInfo info = {XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO};
			info.sourcePath = source;
			info.whichComponents = XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT |
								   XR_INPUT_SOURCE_LOCALIZED_NAME_INTERACTION_PROFILE_BIT |
								   XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT;
			uint32_t count = 0;
			XrResult res = xrGetInputSourceLocalizedName(session, &info, 0, &count, nullptr);
			if (XR_SUCCEEDED(res) && count > 0)
			{
				std::vector<char> buf(static_cast<size_t>(count) + 1, '\0');
				res = xrGetInputSourceLocalizedName(session, &info, count, &count, buf.data());
				buf.back() = '\0';
				if (XR_SUCCEEDED(res) && buf[0] != '\0')
					return std::string(buf.data());
			}
			if (XR_FAILED(res))
				WarnXR(res, "xrGetInputSourceLocalizedName");
			return PathString(instance, source);
		}

		void CopyPose(const XrPosef& in, VRPose* out)
		{
			out->orientation_xyzw = {in.orientation.x, in.orientation.y, in.orientation.z, in.orientation.w};
			out->position_xyz = {in.position.x, in.position.y, in.position.z};
		}

		// `active`, when given, is ORed with the action's isActive: the runtime has a source bound for it.
		bool ReadBool(XrSession session, XrAction action, XrPath subaction_path, bool* active = nullptr)
		{
			if (action == XR_NULL_HANDLE)
				return false;

			XrActionStateGetInfo get_info = {XR_TYPE_ACTION_STATE_GET_INFO};
			get_info.action = action;
			get_info.subactionPath = subaction_path;

			XrActionStateBoolean state = {XR_TYPE_ACTION_STATE_BOOLEAN};
			if (XR_FAILED(xrGetActionStateBoolean(session, &get_info, &state)))
				return false;

			if (active && state.isActive == XR_TRUE)
				*active = true;
			return state.isActive == XR_TRUE && state.currentState == XR_TRUE;
		}

		float ReadFloat(XrSession session, XrAction action, XrPath subaction_path, bool* active = nullptr)
		{
			if (action == XR_NULL_HANDLE)
				return 0.f;

			XrActionStateGetInfo get_info = {XR_TYPE_ACTION_STATE_GET_INFO};
			get_info.action = action;
			get_info.subactionPath = subaction_path;

			XrActionStateFloat state = {XR_TYPE_ACTION_STATE_FLOAT};
			if (XR_FAILED(xrGetActionStateFloat(session, &get_info, &state)) || state.isActive != XR_TRUE)
				return 0.f;

			if (active)
				*active = true;

			return std::isfinite(state.currentState) ? state.currentState : 0.f;
		}

		XrVector2f ReadVector2(XrSession session, XrAction action, XrPath subaction_path, bool* active = nullptr)
		{
			if (action == XR_NULL_HANDLE)
				return {0.f, 0.f};

			XrActionStateGetInfo get_info = {XR_TYPE_ACTION_STATE_GET_INFO};
			get_info.action = action;
			get_info.subactionPath = subaction_path;

			XrActionStateVector2f state = {XR_TYPE_ACTION_STATE_VECTOR2F};
			if (XR_FAILED(xrGetActionStateVector2f(session, &get_info, &state)) || state.isActive != XR_TRUE)
				return {0.f, 0.f};

			if (active)
				*active = true;

			if (!std::isfinite(state.currentState.x) || !std::isfinite(state.currentState.y))
				return {0.f, 0.f};

			return state.currentState;
		}

		// Scripted test input (XRInput::SetTestInput, from PINE's thread). It lapses on its own: a test
		// runner that dies mid-scenario must not leave a held stick or grip behind.
		// 1 s: long enough that a script's re-send hiccup (a GIL stall, a slow PINE reply) doesn't drop the input.
		constexpr std::uint64_t kTestInputMaxTtlMs = 1000;
		std::mutex s_test_mutex;
		XRInput::TestInput s_test_input;
		std::uint64_t s_test_expiry_ms = 0; // 0: none

		bool LiveTestInput(XRInput::TestInput* out)
		{
			std::lock_guard lock{s_test_mutex};
			if (s_test_expiry_ms == 0)
				return false;
			if (NowMs() >= s_test_expiry_ms)
			{
				s_test_expiry_ms = 0;
				Console.WriteLn("(VR) Test input (PINE) expired; the real controllers are back.");
				return false;
			}
			*out = s_test_input;
			return true;
		}

		// Scripted menu-pointer hit (XRInput::SetTestScreenHit, from PINE's thread), under s_test_mutex like the test
		// input, and lapsing the same way.
		struct TestScreenHit
		{
			int hand = VRInputSnapshot::RIGHT;
			float u = 0.5f;
			float v = 0.5f;
		};
		TestScreenHit s_test_hit;
		std::uint64_t s_test_hit_expiry_ms = 0; // 0: none

		bool LiveTestScreenHit(TestScreenHit* out)
		{
			std::lock_guard lock{s_test_mutex};
			if (s_test_hit_expiry_ms == 0)
				return false;
			if (NowMs() >= s_test_hit_expiry_ms)
			{
				s_test_hit_expiry_ms = 0;
				Console.WriteLn("(VR) Test screen hit (PINE) expired; the aim rays point again.");
				return false;
			}
			*out = s_test_hit;
			return true;
		}

		// A live PINE test hit replaces the rays' hit. Its point is placed on the screen quad when one is published, so
		// the dot can be drawn there; without one (no OpenXR runtime) only u and v mean anything.
		void OverlayTestScreenHit(VRInputSnapshot& snap, const VRScreenTransform& screen)
		{
			TestScreenHit t;
			if (!LiveTestScreenHit(&t))
				return;
			VRScreenHit hit;
			hit.u = t.u;
			hit.v = t.v;
			hit.valid = true;
			hit.hand = t.hand;
			hit.test = true;
			if (screen.valid)
			{
				// center + R * (x, y, 0), with R the quad's orientation (unit quaternion xyzw).
				const float x = (t.u - 0.5f) * 2.0f * screen.half_width;
				const float y = (0.5f - t.v) * 2.0f * screen.half_height;
				const std::array<float, 4>& q = screen.center.orientation_xyzw;
				// v' = v + 2w (q x v) + 2 q x (q x v), with v = (x, y, 0).
				const float cx = -q[2] * y;
				const float cy = q[2] * x;
				const float cz = q[0] * y - q[1] * x;
				const float ccx = q[1] * cz - q[2] * cy;
				const float ccy = q[2] * cx - q[0] * cz;
				const float ccz = q[0] * cy - q[1] * cx;
				const std::array<float, 3>& c = screen.center.position_xyz;
				hit.point = {c[0] + x + 2.0f * (q[3] * cx + ccx), c[1] + y + 2.0f * (q[3] * cy + ccy),
					c[2] + 2.0f * (q[3] * cz + ccz)};
			}
			snap.screen_hit = hit;
		}

		void SanitizePose(VRPose* pose)
		{
			if (!pose->valid)
				return;
			float n2 = 0.f;
			for (const float c : pose->orientation_xyzw)
				n2 += c * c;
			bool finite = std::isfinite(n2) && n2 > 1e-8f;
			for (const float c : pose->position_xyz)
				finite = finite && std::isfinite(c);
			if (!finite)
			{
				*pose = VRPose{};
				return;
			}
			const float inv = 1.f / std::sqrt(n2);
			for (float& c : pose->orientation_xyzw)
				c *= inv;
		}

		float SanitizeAxis(float v, float lo, float hi)
		{
			return std::isfinite(v) ? std::clamp(v, lo, hi) : 0.f;
		}

		// A head-relative pose placed from the head's position and floor yaw: the same frame HeadAnchor
		// and PlaceControl put the zones in, so a hand sent at a zone's offset lands in that zone.
		VRPose FromHead(const VRPose& in, const std::array<float, 3>& origin, float yaw)
		{
			if (!in.valid)
				return in;
			const float c = std::cos(yaw), s = std::sin(yaw);
			const float hc = std::cos(yaw * 0.5f), hs = std::sin(yaw * 0.5f);
			const std::array<float, 3>& p = in.position_xyz;
			const std::array<float, 4>& q = in.orientation_xyzw;
			VRPose out;
			out.position_xyz = {origin[0] + p[0] * c + p[2] * s, origin[1] + p[1], origin[2] - p[0] * s + p[2] * c};
			// yaw(0, hs, 0, hc) * q
			out.orientation_xyzw = {hc * q[0] + hs * q[2], hc * q[1] + hs * q[3], hc * q[2] - hs * q[0], hc * q[3] - hs * q[1]};
			out.valid = true;
			return out;
		}

		// The hands it supplied: bit 0 the left, bit 1 the right (VRInputSnapshot::LEFT / RIGHT).
		int OverlayTestInput(VRInputSnapshot& snap)
		{
			XRInput::TestInput t;
			if (!LiveTestInput(&t))
				return 0;

			snap.actions_active = true;
			snap.generation = std::max<std::uint64_t>(snap.generation, 1);
			if (t.head_pose.valid)
				snap.head_pose = t.head_pose;

			// The head's forward (-Z) flattened onto the floor, as SpatialControls::HeadAnchor takes it.
			std::array<float, 3> origin = {0.f, 0.f, 0.f};
			float yaw = 0.f;
			if (snap.head_pose.valid)
			{
				const std::array<float, 4>& q = snap.head_pose.orientation_xyzw;
				const float fx = -2.0f * (q[3] * q[1] + q[2] * q[0]);
				const float fz = -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]));
				if ((fx * fx + fz * fz) > 1.0e-4f)
					yaw = std::atan2(-fx, -fz);
				origin = snap.head_pose.position_xyz;
			}

			int supplied = 0;
			for (int hand = 0; hand < 2; ++hand)
			{
				const XRInput::TestInput::Hand& in = t.hands[hand];
				if (!in.supplied)
					continue;
				supplied |= 1 << hand;
				VRHandState& out = snap.hands[hand];
				out = VRHandState{};
				const auto bit = [&in](int i) { return (in.buttons & (1u << i)) != 0; };
				out.a = bit(0);
				out.b = bit(1);
				out.x = bit(2);
				out.y = bit(3);
				out.menu = bit(4);
				out.thumbstick_click = bit(5);
				out.dpad_up = bit(6);
				out.dpad_down = bit(7);
				out.dpad_left = bit(8);
				out.dpad_right = bit(9);
				out.bumper = bit(10);
				out.view = bit(11);
				out.trigger = in.trigger;
				out.grip = in.grip;
				out.thumbstick_x = in.thumbstick_x;
				out.thumbstick_y = in.thumbstick_y;
				out.aim_pose = in.head_relative ? FromHead(in.aim_pose, origin, yaw) : in.aim_pose;
				out.grip_pose = in.head_relative ? FromHead(in.grip_pose, origin, yaw) : in.grip_pose;
			}
			return supplied;
		}
	}

	VRInputSnapshot GetInputSnapshot()
	{
		VRInputSnapshot snap;
		{
			std::lock_guard lock{s_snapshot_mutex};
			snap = s_snapshot;
			if (snap.generation != 0 && snap.actions_active && NowMs() - snap.published_ms > kSnapshotMaxAgeMs)
			{
				// Producer went quiet: report neutral, keep the generation so "live" is unchanged.
				const std::uint64_t generation = snap.generation;
				snap = VRInputSnapshot{};
				snap.generation = generation;
				snap.published_ms = s_snapshot.published_ms;
			}
		}
		const int test_hands = OverlayTestInput(snap);

		// The menu pointer: where the right hand's aim ray meets the flat screen quad. The left's stands in only for a right
		// controller that is not tracked, or as a PINE test hand (AimScreenHit), so the left controller (the Steam Frame's
		// D-pad hand) never takes the pointer while the right one merely points off the screen. Then a PINE test hit over
		// it while one is live. Kept tiny (one locked copy, at most two ray-plane tests): the VR pacer may call this every
		// display frame.
		const VRScreenTransform screen = GetScreenTransform();
		snap.screen_hit = AimScreenHit(screen, snap.hands[VRInputSnapshot::LEFT].aim_pose,
			snap.hands[VRInputSnapshot::RIGHT].aim_pose, (test_hands & (1 << VRInputSnapshot::LEFT)) != 0);
		OverlayTestScreenHit(snap, screen);
		return snap;
	}

	VRScreenHit AimScreenHit(const VRScreenTransform& screen, const VRPose& left_aim, const VRPose& right_aim, bool left_test)
	{
		VRScreenHit hit = RayHitScreen(screen, right_aim);
		if (hit.valid)
		{
			hit.hand = VRInputSnapshot::RIGHT;
			return hit;
		}
		if (right_aim.valid && !left_test)
			return VRScreenHit{};
		hit = RayHitScreen(screen, left_aim);
		if (hit.valid)
			hit.hand = VRInputSnapshot::LEFT;
		return hit;
	}

	namespace
	{
		// The flat screen quad the menu pointer aims at (PublishScreenTransform, GS thread). A leaf lock: only the copy is
		// made under it, and nothing is called while it is held.
		std::mutex s_screen_mutex;
		VRScreenTransform s_screen;
		// The compositor publishes every present; older than this, the GS is idle (loading, paused) and nothing is aimed at.
		constexpr std::uint64_t kScreenMaxAgeMs = 250;
	}

	VRScreenTransform GetScreenTransform()
	{
		VRScreenTransform t;
		{
			std::lock_guard lock{s_screen_mutex};
			t = s_screen;
		}
		if (t.valid && NowMs() - t.published_ms > kScreenMaxAgeMs)
			t.valid = false;
		return t;
	}

	void PublishScreenTransform(const VRScreenTransform& transform)
	{
		VRScreenTransform t = transform;
		t.published_ms = NowMs();
		std::lock_guard lock{s_screen_mutex};
		t.generation = s_screen.generation + 1;
		s_screen = t;
	}

	VRScreenHit RayHitScreen(const VRScreenTransform& screen, const VRPose& aim)
	{
		VRScreenHit hit;
		if (!screen.valid || !aim.valid || !(screen.half_width > 0.f) || !(screen.half_height > 0.f))
			return hit;

		const auto norm_quat = [](const std::array<float, 4>& q, std::array<float, 4>& out) {
			const float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
			if (!std::isfinite(n2) || n2 < 1e-8f)
				return false;
			const float inv = 1.f / std::sqrt(n2);
			for (int i = 0; i < 4; ++i)
				out[i] = q[i] * inv;
			return true;
		};
		// v rotated by the unit quaternion q: v + 2w (q x v) + 2 q x (q x v).
		const auto rotate = [](const std::array<float, 4>& q, const std::array<float, 3>& v) {
			const std::array<float, 3> c = {q[1] * v[2] - q[2] * v[1], q[2] * v[0] - q[0] * v[2], q[0] * v[1] - q[1] * v[0]};
			const std::array<float, 3> cc = {q[1] * c[2] - q[2] * c[1], q[2] * c[0] - q[0] * c[2], q[0] * c[1] - q[1] * c[0]};
			return std::array<float, 3>{v[0] + 2.f * (q[3] * c[0] + cc[0]), v[1] + 2.f * (q[3] * c[1] + cc[1]),
				v[2] + 2.f * (q[3] * c[2] + cc[2])};
		};
		const auto dot = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
			return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
		};

		std::array<float, 4> qa;
		std::array<float, 4> qs;
		if (!norm_quat(aim.orientation_xyzw, qa) || !norm_quat(screen.center.orientation_xyzw, qs))
			return hit;
		const std::array<float, 3>& o = aim.position_xyz;
		const std::array<float, 3>& c = screen.center.position_xyz;
		for (int i = 0; i < 3; ++i)
		{
			if (!std::isfinite(o[i]) || !std::isfinite(c[i]))
				return hit;
		}

		const std::array<float, 3> dir = rotate(qa, {0.f, 0.f, -1.f});
		const std::array<float, 3> normal = rotate(qs, {0.f, 0.f, 1.f});
		// Front face only: the ray must run against the face normal.
		const float denom = dot(dir, normal);
		if (!(denom < -1e-3f))
			return hit;
		const std::array<float, 3> to_centre = {c[0] - o[0], c[1] - o[1], c[2] - o[2]};
		const float t = dot(to_centre, normal) / denom;
		if (!(t >= 0.05f && t <= 10.f))
			return hit;

		const std::array<float, 3> p = {o[0] + dir[0] * t, o[1] + dir[1] * t, o[2] + dir[2] * t};
		// Into the quad's own frame: R^T (p - c), the inverse rotation being the conjugate.
		const std::array<float, 4> qs_inv = {-qs[0], -qs[1], -qs[2], qs[3]};
		const std::array<float, 3> local = rotate(qs_inv, {p[0] - c[0], p[1] - c[1], p[2] - c[2]});
		const float u = 0.5f + local[0] / (2.f * screen.half_width);
		const float v = 0.5f - local[1] / (2.f * screen.half_height);
		if (!(u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f))
			return hit;

		hit.u = u;
		hit.v = v;
		hit.valid = true;
		hit.distance = t;
		hit.point = p;
		return hit;
	}

	VRInput::VRInput() = default;

	VRInput::~VRInput()
	{
		Shutdown();
	}

	bool VRInput::CreateAction(XrInstance instance, XrActionType type, const char* name,
		const char* localized_name, XrAction* out)
	{
		XrActionCreateInfo create_info = {XR_TYPE_ACTION_CREATE_INFO};
		create_info.actionType = type;
		std::strncpy(create_info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
		std::strncpy(create_info.localizedActionName, localized_name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
		create_info.countSubactionPaths = static_cast<uint32_t>(m_hand_paths.size());
		create_info.subactionPaths = m_hand_paths.data();

		const XrResult res = xrCreateAction(m_action_set, &create_info, out);
		if (XR_FAILED(res))
		{
			char what[64];
			std::snprintf(what, sizeof(what), "xrCreateAction(%s)", name);
			return CheckXR(res, what);
		}
		return true;
	}

	bool VRInput::CreateActionSetAndActions(XrInstance instance)
	{
		XrActionSetCreateInfo set_info = {XR_TYPE_ACTION_SET_CREATE_INFO};
		std::strncpy(set_info.actionSetName, "pcsx2_vr", XR_MAX_ACTION_SET_NAME_SIZE - 1);
		std::strncpy(set_info.localizedActionSetName, "PCSX2 VR", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
		set_info.priority = 0;
		if (!CheckXR(xrCreateActionSet(instance, &set_info, &m_action_set), "xrCreateActionSet"))
		{
			m_action_set = XR_NULL_HANDLE;
			return false;
		}

		for (int hand = 0; hand < HAND_COUNT; ++hand)
		{
			if (!CheckXR(xrStringToPath(instance, HAND_PATH[hand], &m_hand_paths[hand]), "xrStringToPath(hand)"))
				return false;
		}

		// clang-format off
		return CreateAction(instance, XR_ACTION_TYPE_POSE_INPUT,       "aim_pose",         "Aim Pose",         &m_aim_pose) &&
		       CreateAction(instance, XR_ACTION_TYPE_POSE_INPUT,       "grip_pose",        "Grip Pose",        &m_grip_pose) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_a",         "A Button",         &m_button_a) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_b",         "B Button",         &m_button_b) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_x",         "X Button",         &m_button_x) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_y",         "Y Button",         &m_button_y) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_menu",      "Menu Button",      &m_button_menu) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_view",      "View Button",      &m_button_view) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "thumbstick_click", "Thumbstick Click", &m_thumbstick_click) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "dpad_up",          "D-pad Up",         &m_dpad_up) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "dpad_down",        "D-pad Down",       &m_dpad_down) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "dpad_left",        "D-pad Left",       &m_dpad_left) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "dpad_right",       "D-pad Right",      &m_dpad_right) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "bumper",           "Bumper",           &m_bumper) &&
		       CreateAction(instance, XR_ACTION_TYPE_FLOAT_INPUT,      "trigger",          "Trigger",          &m_trigger) &&
		       CreateAction(instance, XR_ACTION_TYPE_FLOAT_INPUT,      "grip",             "Grip",             &m_grip) &&
		       CreateAction(instance, XR_ACTION_TYPE_VECTOR2F_INPUT,   "thumbstick",       "Thumbstick",       &m_thumbstick) &&
		       CreateAction(instance, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic",           "Haptic Feedback",  &m_haptic);
		// clang-format on
	}

	bool VRInput::SuggestBindings(XrInstance instance)
	{
		struct Suggestion
		{
			XrAction action;
			const char* path;
		};

		const auto suggest = [&](const char* profile, const std::vector<Suggestion>& suggestions) {
			std::vector<XrActionSuggestedBinding> bindings;
			bindings.reserve(suggestions.size());
			for (const Suggestion& suggestion : suggestions)
			{
				XrPath path = XR_NULL_PATH;
				if (XR_FAILED(xrStringToPath(instance, suggestion.path, &path)))
				{
					Console.Warning("(VR) xrStringToPath failed for %s", suggestion.path);
					continue;
				}
				bindings.push_back({suggestion.action, path});
			}

			XrPath profile_path = XR_NULL_PATH;
			if (!CheckXR(xrStringToPath(instance, profile, &profile_path), "xrStringToPath(profile)"))
				return false;

			XrInteractionProfileSuggestedBinding suggested = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
			suggested.interactionProfile = profile_path;
			suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
			suggested.suggestedBindings = bindings.data();

			if (XR_FAILED(xrSuggestInteractionProfileBindings(instance, &suggested)))
			{
				Console.Warning("(VR) Runtime rejected the suggested bindings for %s", profile);
				return false;
			}

			Console.WriteLn("(VR) Suggested %u controller bindings for %s", static_cast<unsigned>(bindings.size()), profile);
			return true;
		};

		const bool touch_ok = suggest(TOUCH_PROFILE, {
			{m_aim_pose,         "/user/hand/left/input/aim/pose"},
			{m_aim_pose,         "/user/hand/right/input/aim/pose"},
			{m_grip_pose,        "/user/hand/left/input/grip/pose"},
			{m_grip_pose,        "/user/hand/right/input/grip/pose"},
			{m_button_a,         "/user/hand/right/input/a/click"},
			{m_button_b,         "/user/hand/right/input/b/click"},
			{m_button_x,         "/user/hand/left/input/x/click"},
			{m_button_y,         "/user/hand/left/input/y/click"},
			{m_button_menu,      "/user/hand/left/input/menu/click"},
			{m_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
			{m_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
			{m_trigger,          "/user/hand/left/input/trigger/value"},
			{m_trigger,          "/user/hand/right/input/trigger/value"},
			{m_grip,             "/user/hand/left/input/squeeze/value"},
			{m_grip,             "/user/hand/right/input/squeeze/value"},
			{m_thumbstick,       "/user/hand/left/input/thumbstick"},
			{m_thumbstick,       "/user/hand/right/input/thumbstick"},
			{m_haptic,           "/user/hand/left/output/haptic"},
			{m_haptic,           "/user/hand/right/output/haptic"},
		});

		const bool simple_ok = suggest(SIMPLE_PROFILE, {
			{m_aim_pose,    "/user/hand/left/input/aim/pose"},
			{m_aim_pose,    "/user/hand/right/input/aim/pose"},
			{m_grip_pose,   "/user/hand/left/input/grip/pose"},
			{m_grip_pose,   "/user/hand/right/input/grip/pose"},
			{m_trigger,     "/user/hand/left/input/select/click"},
			{m_trigger,     "/user/hand/right/input/select/click"},
			{m_button_a,    "/user/hand/left/input/select/click"},
			{m_button_a,    "/user/hand/right/input/select/click"},
			{m_button_menu, "/user/hand/left/input/menu/click"},
			{m_button_menu, "/user/hand/right/input/menu/click"},
			{m_haptic,      "/user/hand/left/output/haptic"},
			{m_haptic,      "/user/hand/right/output/haptic"},
		});

		// Valve Steam Frame (SteamVR) is gamepad-shaped: A/B/X/Y and Menu are on the right
		// controller, the D-pad and View on the left. Without its own bindings SteamVR remaps the
		// Touch ones onto it and X/Y come out as B. A runtime rejects a whole suggestion over one
		// path it doesn't support, so if the full set fails, retry without the D-pad, bumpers and
		// View. The Frame is optional: only Touch and Simple decide whether input works at all.
		const std::vector<Suggestion> frame_minimal = {
			{m_aim_pose,         "/user/hand/left/input/aim/pose"},
			{m_aim_pose,         "/user/hand/right/input/aim/pose"},
			{m_grip_pose,        "/user/hand/left/input/grip/pose"},
			{m_grip_pose,        "/user/hand/right/input/grip/pose"},
			{m_button_a,         "/user/hand/right/input/a/click"},
			{m_button_b,         "/user/hand/right/input/b/click"},
			{m_button_x,         "/user/hand/right/input/x/click"},
			{m_button_y,         "/user/hand/right/input/y/click"},
			{m_button_menu,      "/user/hand/right/input/menu/click"},
			{m_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
			{m_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
			{m_trigger,          "/user/hand/left/input/trigger/value"},
			{m_trigger,          "/user/hand/right/input/trigger/value"},
			{m_grip,             "/user/hand/left/input/squeeze/value"},
			{m_grip,             "/user/hand/right/input/squeeze/value"},
			{m_thumbstick,       "/user/hand/left/input/thumbstick"},
			{m_thumbstick,       "/user/hand/right/input/thumbstick"},
			{m_haptic,           "/user/hand/left/output/haptic"},
			{m_haptic,           "/user/hand/right/output/haptic"},
		};
		const std::vector<Suggestion> frame_extra = {
			{m_button_view, "/user/hand/left/input/view/click"},
			{m_dpad_up,     "/user/hand/left/input/dpad_up/click"},
			{m_dpad_down,   "/user/hand/left/input/dpad_down/click"},
			{m_dpad_left,   "/user/hand/left/input/dpad_left/click"},
			{m_dpad_right,  "/user/hand/left/input/dpad_right/click"},
			{m_bumper,      "/user/hand/left/input/bumper/click"},
			{m_bumper,      "/user/hand/right/input/bumper/click"},
		};
		std::vector<Suggestion> frame_full = frame_minimal;
		frame_full.insert(frame_full.end(), frame_extra.begin(), frame_extra.end());

		// The profile only exists with XR_VALVE_frame_controller_interaction (SteamVR); other runtimes skip it.
		if (XRSession::HasFrameControllerInteraction())
		{
			const bool frame_full_ok = suggest(FRAME_PROFILE, frame_full);
			const bool frame_minimal_ok = !frame_full_ok && suggest(FRAME_PROFILE, frame_minimal);
			if (frame_full_ok)
				Console.WriteLn("(VR) Steam Frame controller: full bindings accepted (D-pad, bumpers and View included).");
			else if (frame_minimal_ok)
				Console.WriteLn("(VR) Steam Frame controller: minimal bindings accepted (no D-pad, bumpers or View).");
			else
				Console.Warning("(VR) Steam Frame controller bindings not accepted (only matters with Steam Frame controllers).");
		}

		if (!touch_ok && !simple_ok)
		{
			Console.Error("(VR) No interaction profile bindings were accepted; controllers will produce nothing.");
			return false;
		}

		return true;
	}

	bool VRInput::AttachAndCreateSpaces(XrInstance instance, XrSession session)
	{
		XrSessionActionSetsAttachInfo attach_info = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
		attach_info.countActionSets = 1;
		attach_info.actionSets = &m_action_set;
		if (!CheckXR(xrAttachSessionActionSets(session, &attach_info), "xrAttachSessionActionSets"))
			return false;

		for (int hand = 0; hand < HAND_COUNT; ++hand)
		{
			XrActionSpaceCreateInfo space_info = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
			space_info.subactionPath = m_hand_paths[hand];
			space_info.poseInActionSpace.orientation.w = 1.0f;

			space_info.action = m_aim_pose;
			if (!CheckXR(xrCreateActionSpace(session, &space_info, &m_aim_spaces[hand]), "xrCreateActionSpace(aim)"))
				return false;

			space_info.action = m_grip_pose;
			if (!CheckXR(xrCreateActionSpace(session, &space_info, &m_grip_spaces[hand]), "xrCreateActionSpace(grip)"))
				return false;
		}

		XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
		rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
		rsci.poseInReferenceSpace.orientation.w = 1.0f;
		if (XR_FAILED(xrCreateReferenceSpace(session, &rsci, &m_view_space)))
		{
			m_view_space = XR_NULL_HANDLE;
			Console.Warning("(VR) VIEW space for the input snapshot failed; head_pose stays invalid.");
		}

		return true;
	}

	bool VRInput::Initialize()
	{
		if (m_action_set != XR_NULL_HANDLE)
		{
			Console.Error("(VR) VRInput::Initialize called twice.");
			return false;
		}
		if (!XRSession::HasInstance() || !XRSession::HasSession())
		{
			Console.Error("(VR) VRInput::Initialize needs a live instance and session.");
			return false;
		}

		const XrInstance instance = XRSession::GetInstance();
		if (!CreateActionSetAndActions(instance) || !SuggestBindings(instance) ||
			!AttachAndCreateSpaces(instance, XRSession::GetSession()))
		{
			Shutdown();
			return false;
		}

		m_attached = true;
		m_locate_warned = false;
		m_last_actions_active = -1;
		s_actions_were_live = false;
		s_profile_changed.store(false, std::memory_order_release);
		ResetPresence();
		s_publisher = this;
		ResetSnapshot();
		Console.WriteLn("(VR) Controller action set attached (spatial controls can read the hands).");
		return true;
	}

	void VRInput::Shutdown()
	{
		const bool session_alive = m_attached && XRSession::HasSession();
		const bool instance_alive = m_attached && XRSession::HasInstance();

		for (int hand = 0; hand < HAND_COUNT; ++hand)
		{
			if (m_aim_spaces[hand] != XR_NULL_HANDLE)
			{
				if (session_alive)
					xrDestroySpace(m_aim_spaces[hand]);
				m_aim_spaces[hand] = XR_NULL_HANDLE;
			}
			if (m_grip_spaces[hand] != XR_NULL_HANDLE)
			{
				if (session_alive)
					xrDestroySpace(m_grip_spaces[hand]);
				m_grip_spaces[hand] = XR_NULL_HANDLE;
			}
		}
		if (m_view_space != XR_NULL_HANDLE)
		{
			if (session_alive)
				xrDestroySpace(m_view_space);
			m_view_space = XR_NULL_HANDLE;
		}

		for (XrAction* action : {&m_aim_pose, &m_grip_pose, &m_button_a, &m_button_b, &m_button_x, &m_button_y,
				 &m_button_menu, &m_button_view, &m_thumbstick_click, &m_dpad_up, &m_dpad_down, &m_dpad_left,
				 &m_dpad_right, &m_bumper, &m_trigger, &m_grip, &m_thumbstick, &m_haptic})
		{
			if (*action != XR_NULL_HANDLE)
			{
				if (instance_alive)
					xrDestroyAction(*action);
				*action = XR_NULL_HANDLE;
			}
		}

		if (m_action_set != XR_NULL_HANDLE)
		{
			if (instance_alive)
				xrDestroyActionSet(m_action_set);
			m_action_set = XR_NULL_HANDLE;
			Console.WriteLn("(VR) Controller action set destroyed.");
		}

		m_hand_paths = {};
		m_attached = false;
		ResetPresence();
		if (s_publisher == this)
		{
			s_publisher = nullptr;
			ResetSnapshot();
		}
	}

	bool VRInput::ReadHand(XrSession session, int hand, XrTime time, VRHandState* out)
	{
		const XrPath hand_path = m_hand_paths[hand];

		bool active = false;
		out->a = ReadBool(session, m_button_a, hand_path, &active);
		out->b = ReadBool(session, m_button_b, hand_path, &active);
		out->x = ReadBool(session, m_button_x, hand_path, &active);
		out->y = ReadBool(session, m_button_y, hand_path, &active);
		out->menu = ReadBool(session, m_button_menu, hand_path, &active);
		out->thumbstick_click = ReadBool(session, m_thumbstick_click, hand_path, &active);
		out->dpad_up = ReadBool(session, m_dpad_up, hand_path, &active);
		out->dpad_down = ReadBool(session, m_dpad_down, hand_path, &active);
		out->dpad_left = ReadBool(session, m_dpad_left, hand_path, &active);
		out->dpad_right = ReadBool(session, m_dpad_right, hand_path, &active);
		out->bumper = ReadBool(session, m_bumper, hand_path, &active);
		out->view = ReadBool(session, m_button_view, hand_path, &active);
		out->trigger = ReadFloat(session, m_trigger, hand_path, &active);
		out->grip = ReadFloat(session, m_grip, hand_path, &active);

		const XrVector2f thumbstick = ReadVector2(session, m_thumbstick, hand_path, &active);
		out->thumbstick_x = thumbstick.x;
		out->thumbstick_y = thumbstick.y;
		out->input_active = active;

		if (time == 0)
			return false;

		const XrSpace base_space = XRSession::GetSpace();
		constexpr XrSpaceLocationFlags POSE_VALID =
			XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;

		bool ok = true;

		XrSpaceLocation aim_location = {XR_TYPE_SPACE_LOCATION};
		if (XR_SUCCEEDED(xrLocateSpace(m_aim_spaces[hand], base_space, time, &aim_location)))
		{
			if ((aim_location.locationFlags & POSE_VALID) == POSE_VALID)
			{
				CopyPose(aim_location.pose, &out->aim_pose);
				out->aim_pose.valid = true;
			}
		}
		else
		{
			ok = false;
		}

		XrSpaceVelocity grip_velocity = {XR_TYPE_SPACE_VELOCITY};
		XrSpaceLocation grip_location = {XR_TYPE_SPACE_LOCATION, &grip_velocity};
		if (XR_SUCCEEDED(xrLocateSpace(m_grip_spaces[hand], base_space, time, &grip_location)))
		{
			if ((grip_location.locationFlags & POSE_VALID) == POSE_VALID)
			{
				CopyPose(grip_location.pose, &out->grip_pose);
				out->grip_pose.valid = true;
			}
			if ((grip_velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0)
			{
				out->grip_linear_velocity = {grip_velocity.linearVelocity.x, grip_velocity.linearVelocity.y,
					grip_velocity.linearVelocity.z};
			}
		}
		else
		{
			ok = false;
		}

		return ok;
	}

	bool VRInput::Update(XrTime display_time)
	{
		if (m_action_set == XR_NULL_HANDLE || !m_attached)
			return false;

		const XrSession session = XRSession::GetSession();
		if (session == XR_NULL_HANDLE || !XRSession::IsSessionRunning())
		{
			PublishNeutralIfActive();
			return false;
		}

		XrActiveActionSet active_set = {m_action_set, XR_NULL_PATH};
		XrActionsSyncInfo sync_info = {XR_TYPE_ACTIONS_SYNC_INFO};
		sync_info.countActiveActionSets = 1;
		sync_info.activeActionSets = &active_set;
		const XrResult sync_result = xrSyncActions(session, &sync_info);
		if (!CheckXR(sync_result, "xrSyncActions"))
		{
			PublishNeutralIfActive();
			return false;
		}
		const bool actions_active = sync_result == XR_SUCCESS;
		if (m_last_actions_active != static_cast<int>(actions_active))
		{
			// The runtime took or gave back input focus (dashboard, headset off and on): say why the hands
			// stopped or came back. Not for the first sync of a session.
			if (m_last_actions_active == 1 && !actions_active)
				HudCards::Toast("Controllers paused");
			else if (m_last_actions_active == 0 && actions_active && s_actions_were_live)
				HudCards::Toast("Controllers back");
			if (actions_active)
				s_actions_were_live = true;
			// A session that opens on the SteamVR dashboard reads nothing yet: say so if it lasts.
			if (m_last_actions_active == -1 && sync_result == XR_SESSION_NOT_FOCUSED)
				m_unfocused_start_ms = NowMs();
			m_last_actions_active = static_cast<int>(actions_active);
			Console.WriteLn("(VR) Controller actions %s (session %sfocused).",
				actions_active ? "active" : "inactive", actions_active ? "" : "not ");
		}

		if (actions_active)
		{
			m_unfocused_start_ms = 0;
		}
		else if (m_unfocused_start_ms != 0 && NowMs() - m_unfocused_start_ms >= kUnfocusedStartMs)
		{
			m_unfocused_start_ms = 0;
			Console.WriteLn("(VR) The session started unfocused and is still unfocused after 2 s (SteamVR menu open?): controllers paused.");
			ToastLimited("SteamVR menu open: controllers paused");
		}

		const bool profile_changed = s_profile_changed.exchange(false, std::memory_order_acq_rel);
		if (profile_changed || (actions_active && !m_bindings_reported))
		{
			ReportBindings(profile_changed ? "interaction profile changed" : "first focused sync");
			if (actions_active)
				m_bindings_reported = true;
		}

		const XrTime time = display_time;

		VRInputSnapshot snapshot;
		snapshot.actions_active = actions_active;
		bool located = true;
		for (int hand = 0; hand < HAND_COUNT; ++hand)
			located = ReadHand(session, hand, time, &snapshot.hands[hand]) && located;

		if (time != 0 && m_view_space != XR_NULL_HANDLE)
		{
			XrSpaceLocation head_location = {XR_TYPE_SPACE_LOCATION};
			if (XR_SUCCEEDED(xrLocateSpace(m_view_space, XRSession::GetSpace(), time, &head_location)))
			{
				constexpr XrSpaceLocationFlags POSE_VALID =
					XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
				if ((head_location.locationFlags & POSE_VALID) == POSE_VALID)
				{
					CopyPose(head_location.pose, &snapshot.head_pose);
					snapshot.head_pose.valid = true;
				}
			}
			else
			{
				located = false;
			}
		}

		if (!located && !m_locate_warned)
		{
			m_locate_warned = true;
			if (time == 0)
			{
				Console.Warning("(VR) Input update without a display time; poses are published invalid, "
								"buttons/axes stay live. Logged once.");
			}
			else
			{
				Console.Warning("(VR) xrLocateSpace failed (time %lld); poses will be published invalid "
								"until it recovers. Logged once.",
					static_cast<long long>(time));
			}
		}

		UpdatePresence(snapshot, actions_active);

		snapshot.display_time = time;
		PublishSnapshot(snapshot);
		return true;
	}

	bool VRInput::ToastLimited(const std::string& text)
	{
		const std::uint64_t now = NowMs();
		bool found = false;
		for (std::pair<std::string, std::uint64_t>& entry : m_toast_times)
		{
			if (entry.first != text)
				continue;
			if (now - entry.second < kToastRepeatMs)
				return false;
			entry.second = now;
			found = true;
			break;
		}
		if (!found)
			m_toast_times.emplace_back(text, now);
		HudCards::Toast(text, kPresenceToastSeconds);
		return true;
	}

	void VRInput::ResetPresence()
	{
		m_presence.fill(HandPresence{});
		m_presence_paused = true;
		m_presence_last_ms = 0;
		m_focused_ms = 0;
		m_start_checked = false;
		m_unfocused_start_ms = 0;
		m_bindings_reported = false;
		m_touch_toasted = false;
		m_toast_times.clear();
	}

	void VRInput::UpdatePresence(const VRInputSnapshot& snapshot, bool actions_active)
	{
		// An unfocused session reads nothing from either hand, and scripted PINE hands stand in for the
		// real ones: neither says anything about the hardware, so the clocks stop.
		XRInput::TestInput test;
		if (!actions_active || LiveTestInput(&test))
		{
			m_presence_paused = true;
			return;
		}

		const std::uint64_t now = NowMs();
		bool live_now[HAND_COUNT];
		for (int hand = 0; hand < HAND_COUNT; ++hand)
		{
			const VRHandState& h = snapshot.hands[hand];
			live_now[hand] = h.input_active || h.grip_pose.valid;
		}

		if (m_presence_paused)
		{
			// Back from a pause: the runs start over, so paused time counts toward neither state.
			for (int hand = 0; hand < HAND_COUNT; ++hand)
			{
				m_presence[hand].run_live = live_now[hand];
				m_presence[hand].run_start_ms = now;
			}
		}
		else
		{
			m_focused_ms += std::min<std::uint64_t>(now - m_presence_last_ms, kPresenceMaxStepMs);
		}
		m_presence_paused = false;
		m_presence_last_ms = now;

		bool to_live[HAND_COUNT] = {false, false};
		bool to_absent[HAND_COUNT] = {false, false};
		for (int hand = 0; hand < HAND_COUNT; ++hand)
		{
			HandPresence& p = m_presence[hand];
			if (live_now[hand] != p.run_live)
			{
				p.run_live = live_now[hand];
				p.run_start_ms = now;
			}
			const std::uint64_t run_ms = now - p.run_start_ms;
			to_live[hand] = p.run_live && p.state != HandLiveness::Live && run_ms >= kHandLiveMs;
			to_absent[hand] = !p.run_live && p.state != HandLiveness::Absent && run_ms >= kHandAbsentMs;
		}

		// Both hands lost in the same sync: one toast covers them.
		const bool both_lost = to_absent[0] && to_absent[1] && m_presence[0].state == HandLiveness::Live &&
							   m_presence[1].state == HandLiveness::Live;
		bool both_lost_toasted = false;

		for (int hand = 0; hand < HAND_COUNT; ++hand)
		{
			HandPresence& p = m_presence[hand];
			const int other = 1 - hand;
			if (to_live[hand])
			{
				p.state = HandLiveness::Live;
				p.ever_live = true;
				Console.WriteLn("(VR) %s controller: live", HAND_NAME[hand]);
				if (p.announced_absent)
				{
					p.announced_absent = false;
					char text[64];
					std::snprintf(text, sizeof(text), "%s controller back", HAND_NAME[hand]);
					ToastLimited(text);
				}
			}
			else if (to_absent[hand])
			{
				const bool was_live = p.state == HandLiveness::Live;
				p.state = HandLiveness::Absent;
				Console.WriteLn("(VR) %s controller: absent (no bound input and no tracked grip for 3.0 s)", HAND_NAME[hand]);
				if (!was_live)
					continue;
				if (both_lost)
				{
					if (!both_lost_toasted)
					{
						both_lost_toasted = true;
						if (ToastLimited("Controllers not tracking - check SteamVR"))
						{
							m_presence[0].announced_absent = true;
							m_presence[1].announced_absent = true;
						}
					}
					continue;
				}
				const bool other_live = m_presence[other].state == HandLiveness::Live && !to_absent[other];
				char text[64];
				if (other_live)
					std::snprintf(text, sizeof(text), "%s controller asleep - press a button on it", HAND_NAME[hand]);
				else
					std::snprintf(text, sizeof(text), "Controllers not tracking - check SteamVR");
				if (ToastLimited(text))
					p.announced_absent = true;
			}
		}

		// Session start: which controllers never showed up. Once per session; a late hand still gets
		// its "back" toast above.
		if (!m_start_checked && m_focused_ms >= kStartCheckMs)
		{
			m_start_checked = true;
			const bool left = m_presence[VRInputSnapshot::LEFT].ever_live;
			const bool right = m_presence[VRInputSnapshot::RIGHT].ever_live;
			if (!left && !right)
			{
				Console.Warning("(VR) No controller reported input or tracking in the first 5 s of focus.");
				if (ToastLimited("No VR controllers connected"))
				{
					m_presence[0].announced_absent = true;
					m_presence[1].announced_absent = true;
				}
			}
			else if (!left || !right)
			{
				const int present = left ? VRInputSnapshot::LEFT : VRInputSnapshot::RIGHT;
				const int missing = 1 - present;
				Console.Warning("(VR) Only the %s controller reported input or tracking in the first 5 s of focus.",
					HAND_NAME_LOWER[present]);
				char text[64];
				std::snprintf(text, sizeof(text), "Only the %s controller is connected", HAND_NAME_LOWER[present]);
				if (ToastLimited(text))
					m_presence[missing].announced_absent = true;
			}
			else
			{
				Console.WriteLn("(VR) Both controllers live at session start.");
			}
		}
	}

	void VRInput::ReportBindings(const char* why)
	{
		if (m_action_set == XR_NULL_HANDLE || !m_attached || !XRSession::HasInstance() || !XRSession::HasSession())
			return;
		const XrInstance instance = XRSession::GetInstance();
		const XrSession session = XRSession::GetSession();
		if (session == XR_NULL_HANDLE)
			return;

		Console.WriteLn("(VR) Controller bindings (%s):", why ? why : "requested");

		XrPath touch_path = XR_NULL_PATH;
		XrPath frame_path = XR_NULL_PATH;
		if (XR_FAILED(xrStringToPath(instance, TOUCH_PROFILE, &touch_path)))
			touch_path = XR_NULL_PATH;
		if (XR_FAILED(xrStringToPath(instance, FRAME_PROFILE, &frame_path)))
			frame_path = XR_NULL_PATH;

		bool touch_in_use = false;
		bool frame_in_use = false;
		for (int hand = 0; hand < HAND_COUNT; ++hand)
		{
			XrInteractionProfileState state = {XR_TYPE_INTERACTION_PROFILE_STATE};
			const XrResult res = xrGetCurrentInteractionProfile(session, m_hand_paths[hand], &state);
			if (XR_FAILED(res))
			{
				WarnXR(res, "xrGetCurrentInteractionProfile");
				continue;
			}
			const XrPath profile = state.interactionProfile;
			const std::string name = (profile == XR_NULL_PATH) ? std::string("none") : PathString(instance, profile);
			Console.WriteLn("(VR) %s hand profile: %s", HAND_NAME[hand], name.c_str());
			if (profile != XR_NULL_PATH && profile == touch_path)
				touch_in_use = true;
			if (profile != XR_NULL_PATH && profile == frame_path)
				frame_in_use = true;
		}

		struct NamedAction
		{
			XrAction action;
			const char* name;
		};
		const NamedAction actions[] = {
			{m_trigger, "trigger"},
			{m_grip, "grip"},
			{m_thumbstick, "thumbstick"},
			{m_button_a, "A"},
			{m_button_y, "Y"},
			{m_dpad_up, "D-pad up"},
			{m_button_view, "View"},
		};

		bool dpad_checked = false;
		bool dpad_bound = false;
		for (const NamedAction& named : actions)
		{
			if (named.action == XR_NULL_HANDLE)
				continue;

			XrBoundSourcesForActionEnumerateInfo info = {XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO};
			info.action = named.action;
			uint32_t count = 0;
			XrResult res = xrEnumerateBoundSourcesForAction(session, &info, 0, &count, nullptr);
			std::vector<XrPath> sources;
			if (XR_SUCCEEDED(res) && count > 0)
			{
				sources.resize(count);
				res = xrEnumerateBoundSourcesForAction(session, &info, count, &count, sources.data());
				if (XR_SUCCEEDED(res))
					sources.resize(std::min<size_t>(sources.size(), count));
				else
					sources.clear();
			}
			if (XR_FAILED(res))
			{
				WarnXR(res, "xrEnumerateBoundSourcesForAction");
				continue;
			}

			std::string line;
			for (const XrPath source : sources)
			{
				if (!line.empty())
					line += "; ";
				line += SourceName(instance, session, source);
			}
			Console.WriteLn("(VR) Bound %s: %s", named.name, line.empty() ? "(none)" : line.c_str());

			if (named.action == m_dpad_up)
			{
				dpad_checked = true;
				dpad_bound = !sources.empty();
			}
		}

		if (XRSession::HasFrameControllerInteraction() && touch_in_use)
		{
			Console.Warning("(VR) SteamVR is using the Touch bindings: X/Y/Menu and the D-pad may be remapped; "
							"check SteamVR > Controller bindings for PenguinScreen2.");
			if (!m_touch_toasted)
			{
				m_touch_toasted = true;
				ToastLimited("Touch bindings in use - check SteamVR");
			}
		}

		if (frame_in_use && dpad_checked && !dpad_bound)
			Console.Warning("(VR) D-pad not bound: the Steam Frame profile is active but SteamVR gave the D-pad no source.");
	}

	void VRInput::TriggerHaptic(int hand, float duration_seconds, float frequency_hz, float amplitude)
	{
		if (m_haptic == XR_NULL_HANDLE || !m_attached || !XRSession::IsSessionRunning())
			return;
		if (hand < 0 || hand >= HAND_COUNT)
			return;

		XrHapticVibration vibration = {XR_TYPE_HAPTIC_VIBRATION};
		vibration.duration = duration_seconds > 0.f ?
								 static_cast<XrDuration>(duration_seconds * 1'000'000'000.0) :
								 XR_MIN_HAPTIC_DURATION;
		vibration.frequency = frequency_hz > 0.f ? frequency_hz : XR_FREQUENCY_UNSPECIFIED;
		vibration.amplitude = std::clamp(amplitude, 0.f, 1.f);

		XrHapticActionInfo action_info = {XR_TYPE_HAPTIC_ACTION_INFO};
		action_info.action = m_haptic;
		action_info.subactionPath = m_hand_paths[hand];

		xrApplyHapticFeedback(XRSession::GetSession(), &action_info,
			reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
	}

	void VRInput::StopHaptic(int hand)
	{
		if (m_haptic == XR_NULL_HANDLE || !m_attached || !XRSession::IsSessionRunning())
			return;
		if (hand < 0 || hand >= HAND_COUNT)
			return;

		XrHapticActionInfo action_info = {XR_TYPE_HAPTIC_ACTION_INFO};
		action_info.action = m_haptic;
		action_info.subactionPath = m_hand_paths[hand];

		xrStopHapticFeedback(XRSession::GetSession(), &action_info);
	}

	namespace XRInput
	{
		namespace
		{
			VRInput s_input;
			constexpr float kHapticPulseSeconds = 0.05f;
			// Display time a pulse ends at, per hand: until then the continuous rumble (re-triggered every frame)
			// holds off, or it would cut a hit or shot pulse down to one frame.
			XrTime s_pulse_end[2] = {0, 0};
			// Held for each Update. With headset-rate submission Update runs on the VR pacer thread while Initialize
			// and Shutdown run on the GS thread; a pacer stuck inside the runtime mid-update keeps holding it, and
			// then Shutdown leaves s_input alone rather than tear its state down under that thread.
			std::mutex s_update_mutex;
		}

		bool Initialize()
		{
			std::unique_lock<std::mutex> update_lock(s_update_mutex, std::try_to_lock);
			if (!update_lock.owns_lock())
			{
				Console.Warning("(VR) Controller input not initialized: a VR thread is still inside an input update.");
				return false;
			}
			for (std::atomic<int>& r : s_haptic_request)
				r.store(-1, std::memory_order_relaxed);
			for (std::atomic<int>& r : s_pulse_request)
				r.store(-1, std::memory_order_relaxed);
			s_haptic_applied[0] = s_haptic_applied[1] = 0;
			s_pulse_end[0] = s_pulse_end[1] = 0;
			return s_input.Initialize();
		}

		void Update(XrTime display_time)
		{
			std::lock_guard<std::mutex> update_lock(s_update_mutex);
			if (!s_input.IsInitialized())
				return;
			s_input.Update(display_time);

			for (int hand = 0; hand < 2; ++hand)
			{
				const int requested = s_haptic_request[hand].exchange(-1, std::memory_order_acq_rel);
				if (requested >= 0)
					s_haptic_applied[hand] = requested;

				// A pulse at least as strong as the rumble plays out in full; a weaker one is drowned by the
				// rumble anyway and is dropped. Each haptic call replaces the one playing on that hand.
				const int pulse = s_pulse_request[hand].exchange(-1, std::memory_order_acq_rel);
				if (pulse >= 0 && (pulse & 0xFFFF) >= s_haptic_applied[hand])
				{
					const int ms = pulse >> 16;
					s_input.TriggerHaptic(hand, static_cast<float>(ms) / 1000.f, 0.f, static_cast<float>(pulse & 0xFFFF) / 1000.f);
					s_pulse_end[hand] = display_time + static_cast<XrTime>(ms) * 1000000;
				}
				else if (display_time >= s_pulse_end[hand])
				{
					if (s_haptic_applied[hand] > 0)
						s_input.TriggerHaptic(hand, kHapticPulseSeconds, 0.f, static_cast<float>(s_haptic_applied[hand]) / 1000.f);
					else if (requested == 0)
						s_input.StopHaptic(hand);
				}
			}
		}

		void Shutdown()
		{
			std::unique_lock<std::mutex> update_lock(s_update_mutex, std::try_to_lock);
			if (!update_lock.owns_lock())
			{
				// The VR pacer is stuck inside the runtime in the middle of an input update: its XR handles are
				// left as they are (the runtime is not answering) and the hands read neutral from here on.
				Console.Warning("(VR) Controller input shutdown skipped: the VR thread is stuck inside an input update.");
				ResetSnapshot();
				return;
			}
			s_input.Shutdown();
		}

		bool IsInitialized()
		{
			return s_input.IsInitialized();
		}

		void OnInteractionProfileChanged()
		{
			s_profile_changed.store(true, std::memory_order_release);
		}

		void QueueHaptic(int hand, float amplitude)
		{
			if (hand < 0 || hand >= 2)
				return;
			if (!std::isfinite(amplitude))
				amplitude = 0.f;
			const int permille = static_cast<int>(std::lround(std::clamp(amplitude, 0.f, 1.f) * 1000.f));
			s_haptic_request[hand].store(permille, std::memory_order_release);
		}

		void QueuePulse(int hand, float amplitude, float seconds)
		{
			if (hand < 0 || hand >= 2)
				return;
			// One slot per hand, taken once a frame: a weaker cue queued in the same frame (a zone tick, a
			// heartbeat) must not replace a stronger one still waiting (a bite), so the stronger one stays.
			const int packed = PackPulse(amplitude, seconds);
			int pending = s_pulse_request[hand].load(std::memory_order_acquire);
			while (pending < 0 || (pending & 0xFFFF) < (packed & 0xFFFF))
			{
				if (s_pulse_request[hand].compare_exchange_weak(pending, packed, std::memory_order_acq_rel,
						std::memory_order_acquire))
					break;
			}
		}

		bool TakePendingPulseForTest(int hand, float* amplitude, float* seconds)
		{
			if (hand < 0 || hand >= 2)
				return false;
			const int pulse = s_pulse_request[hand].exchange(-1, std::memory_order_acq_rel);
			if (pulse < 0)
				return false;
			if (amplitude)
				*amplitude = static_cast<float>(pulse & 0xFFFF) / 1000.f;
			if (seconds)
				*seconds = static_cast<float>(pulse >> 16) / 1000.f;
			return true;
		}

		void PublishSnapshotForTest(const VRInputSnapshot& snapshot)
		{
			PublishSnapshot(snapshot);
		}

		void ResetSnapshotForTest()
		{
			ResetSnapshot();
		}

		void SetTestInput(const TestInput& input)
		{
			TestInput t = input;
			SanitizePose(&t.head_pose);
			for (TestInput::Hand& hand : t.hands)
			{
				SanitizePose(&hand.aim_pose);
				SanitizePose(&hand.grip_pose);
				hand.trigger = SanitizeAxis(hand.trigger, 0.f, 1.f);
				hand.grip = SanitizeAxis(hand.grip, 0.f, 1.f);
				hand.thumbstick_x = SanitizeAxis(hand.thumbstick_x, -1.f, 1.f);
				hand.thumbstick_y = SanitizeAxis(hand.thumbstick_y, -1.f, 1.f);
			}

			const std::uint64_t now = NowMs();
			std::lock_guard lock{s_test_mutex};
			const bool was_live = s_test_expiry_ms != 0 && now < s_test_expiry_ms;
			if (t.ttl_ms == 0)
			{
				s_test_expiry_ms = 0;
				if (was_live)
					Console.WriteLn("(VR) Test input (PINE) cleared; the real controllers are back.");
				return;
			}
			s_test_input = t;
			s_test_expiry_ms = now + std::min<std::uint64_t>(t.ttl_ms, kTestInputMaxTtlMs);
			if (!was_live)
			{
				Console.Warning("(VR) TEST INPUT ACTIVE (PINE): scripted head %s, left hand %s, right hand %s.",
					t.head_pose.valid ? "on" : "off", t.hands[0].supplied ? "on" : "off", t.hands[1].supplied ? "on" : "off");
			}
		}

		void SetTestScreenHit(int hand, float u, float v, u32 ttl_ms)
		{
			const std::uint64_t now = NowMs();
			std::lock_guard lock{s_test_mutex};
			const bool was_live = s_test_hit_expiry_ms != 0 && now < s_test_hit_expiry_ms;
			if (ttl_ms == 0 || !std::isfinite(u) || !std::isfinite(v) || u < 0.f)
			{
				s_test_hit_expiry_ms = 0;
				if (was_live)
					Console.WriteLn("(VR) Test screen hit (PINE) cleared; the aim rays point again.");
				return;
			}
			s_test_hit.hand = (hand == VRInputSnapshot::LEFT) ? VRInputSnapshot::LEFT : VRInputSnapshot::RIGHT;
			s_test_hit.u = std::clamp(u, 0.f, 1.f);
			s_test_hit.v = std::clamp(v, 0.f, 1.f);
			s_test_hit_expiry_ms = now + std::min<std::uint64_t>(ttl_ms, kTestInputMaxTtlMs);
			if (!was_live)
			{
				Console.Warning("(VR) TEST SCREEN HIT ACTIVE (PINE): %s hand at u %.3f, v %.3f.",
					HAND_NAME_LOWER[s_test_hit.hand], s_test_hit.u, s_test_hit.v);
			}
		}

		bool TestHeadPose(HeadPose::Snapshot* out)
		{
			TestInput t;
			if (!LiveTestInput(&t) || !t.head_pose.valid)
				return false;
			HeadPose::Snapshot p;
			p.orientation_x = t.head_pose.orientation_xyzw[0];
			p.orientation_y = t.head_pose.orientation_xyzw[1];
			p.orientation_z = t.head_pose.orientation_xyzw[2];
			p.orientation_w = t.head_pose.orientation_xyzw[3];
			p.position_x = t.head_pose.position_xyz[0];
			p.position_y = t.head_pose.position_xyz[1];
			p.position_z = t.head_pose.position_xyz[2];
			p.position_valid = true;
			p.valid = true;
			// Taken now, like a fresh headset pose, so the camera's prediction horizon measures from here.
			p.publish_ms = NowMs();
			*out = p;
			return true;
		}
	}
}
