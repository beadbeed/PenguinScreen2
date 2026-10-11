// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/XRCompositor.h"
#include "VR/CameraDriver.h"
#include "VR/SplitState.h"
#include "VR/ControlQuads.h"
#include "VR/HandModel.h"
#include "VR/HudCards.h"
#include "VR/SpatialControls.h"
#include "VR/VRManager.h"
#include "VR/SeatCast.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "VR/SeatSession.h"
#include "VR/HeadPose.h"
#include "VR/VRInternal.h"
#include "VR/VRInput.h"
#include "VR/XRSession.h"
#include "VR/VRVulkanBridge.h"
#include "Config.h"

#ifdef ENABLE_VULKAN
#include "GS/GS.h"
#include "GS/Renderers/Vulkan/GSTextureVK.h"
#endif

#include "common/Console.h"
#include "common/Threading.h"

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef ENABLE_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>
#endif

namespace VR::XRCompositor
{
	namespace
	{
		struct ScreenParams
		{
			float distance = 2.0f;
			float height = 1.4f;
			float arc_deg = 0.0f;
			float voffset = 0.0f;
			bool follow_head = false;
			// Head-locked screen used while the first-person camera drives the game camera.
			bool has_fp = false;
			float fp_distance = 1.5f;
			float fp_height = 2.5f;
			float fp_arc_deg = 0.0f;
			float fp_pose_lag_ms = 40.0f;
		};
		bool s_fp_screen_was_active = false;
		std::mutex s_screen_mutex;
		ScreenParams s_screen_params;

		std::atomic<bool> s_reanchor_requested{false};

		ScreenAnchor s_anchor_snapshot;

		bool s_xrfault_begin1_fired = false;

		// GetEyeInfo: what the runtime last reported, and what the Eyes line last logged.
		std::mutex s_eye_mutex;
		EyeInfo s_eye_info;
		EyeInfo s_eye_logged;
	}

#ifdef ENABLE_VULKAN
	namespace
	{
		constexpr int64_t ONE_SECOND_NS = 1000000000;
		// Longest the GS thread waits for the runtime to hand back a swapchain image. A healthy compositor
		// has one free at once; when it stops consuming frames (headset asleep, streaming link idle) a long
		// wait here stalls the GS thread and with it the emulator, which online means being dropped. On a
		// timeout the copy is skipped and retried on the next present; the headset keeps the last image.
		constexpr int64_t kSwapchainWaitNs = 5000000;
		constexpr u32 NUM_CMD_BUFFERS = 2;
		// First-person hands: one impostor quad per hand (HandModel::kLeft / kRight).
		constexpr int kHandSlots = 2;
		constexpr VkDeviceSize kHandImageBytes =
			static_cast<VkDeviceSize>(HandModel::kImageSize) * HandModel::kImageSize * 4u;

		// Teardown never waits long on the GPU: a runtime that stopped answering must not hold up the GS thread.
		constexpr int64_t kTeardownFenceNs = 250000000;
		// How long Shutdown waits for the pacer before leaving it stuck in the runtime (detached).
		constexpr int kPacerJoinTimeoutMs = 500;
		// XR_ERROR_RUNTIME_FAILURE from xrBeginFrame/xrEndFrame this many times in a row counts as a lost runtime;
		// so does a legacy begin still owed after this many presents.
		constexpr u32 kMaxRuntimeFailures = 30;
		// The published layers older than this mean the GS is idle (loading, paused offline, stalled): the pacer
		// keeps only the screen and lever cards.
		constexpr u64 kStaleMs = 250;
		// A pacer heartbeat older than this means the pacer is stuck in the runtime (PacerStalled). Its longest
		// deliberate sleep is 100 ms, and a display period is 8-17 ms.
		constexpr u64 kPacerStallMs = 250;
		// The opaque background's swapchain, cleared to black once.
		constexpr u32 kBackgroundSize = 32;

		// Every layer one present can build: up to five screen slots (split screen in stereo uses four), two lever
		// cards, two hands and the five HUD cards (laser, wrist, vignette, toast, blink). The submit array adds the
		// background under them; SteamVR takes at most 16 a frame.
		constexpr u32 kLayerCapacity = 5 + ControlQuads::kSlots + kHandSlots + HudCards::kCardCount;
		static_assert(kLayerCapacity + 1 <= 16, "more composition layers than SteamVR takes in a frame");

		// What a published layer is. Screen entries turn on toward the display time during a smooth stick turn,
		// and only Screen and Lever entries are kept while the GS is idle.
		constexpr u8 kRoleScreen = 0;
		constexpr u8 kRoleLever = 1;
		constexpr u8 kRoleHand = 2;
		constexpr u8 kRoleHudLaser = 3;
		constexpr u8 kRoleHudWrist = 4;
		constexpr u8 kRoleHudView = 5; // view-locked cards: toast, blink, vignette
		constexpr u8 kKindQuad = 0;
		constexpr u8 kKindCylinder = 1;

		// The layers the GS built at its last present, copied by value: the pacer submits them every display period
		// until the next present replaces them (the runtime keeps showing each swapchain's last released image).
		struct PublishedSet
		{
			struct Entry
			{
				u8 kind;
				u8 role;
				XrCompositionLayerQuad quad;
				XrCompositionLayerCylinderKHR cyl;
			};
			Entry e[kLayerCapacity];
			u32 count;
			u64 publish_ms; // SteadyNowMs at publish
			// The publish moment on the display clock (the steady clock moved by the latest frame's lead from its
			// wait to its display time); 0 = unknown. Turns extrapolate from here.
			XrTime publish_display_time;
			bool fp_screen;
			XrVector3f fp_head; // the head position the first-person screen is placed around
			float turn_rate;    // CameraDriver::ScreenTurnRate at publish, first-person screen only
			u64 serial;         // counts publishes, for the re-sent count
			u64 fp_image;       // which game frame the screen entries show (the sync log), 0 = unknown
		};

		struct
		{
			std::thread pacer;
			std::atomic_bool pacer_exit{false};
			std::atomic_bool pacer_done{false};
			// Bumped when a stuck pacer is detached: that thread sees it and touches nothing more if it ever returns.
			std::atomic<u32> pacer_gen{0};
			// Set when Shutdown detached a stuck pacer: no XR call may be made again (handles leak until exit).
			std::atomic_bool abandoned{false};

			std::mutex frame_mutex;
			XrFrameState pending_frame_state{XR_TYPE_FRAME_STATE};
			bool has_pending_frame = false;
			s64 pending_wait_ns = 0; // legacy: steady ns when the parked frame's xrWaitFrame returned
			bool initialized = false;

			bool begin_owed = false;
			XrTime begin_owed_display_time = 0;
			u32 begin_owed_presents = 0;

			// [VR] HeadsetRateSubmit and OpaqueBackground, read once at Initialize.
			bool headset_rate = true;
			bool opaque_bg = true;

			// Written by the pacer after each xrWaitFrame.
			std::atomic<XrTime> latest_display_time{0};
			std::atomic<XrDuration> display_period_ns{0};
			std::atomic<s64> display_clock_offset_ns{0}; // predictedDisplayTime minus steady ns at the wait's return
			std::atomic_bool pacer_should_render{false};
			std::atomic<u64> session_run_began_ms{0}; // SteadyNowMs when the pacer first saw this run of the session

			// Headset rate: the head pose the pacer located for its latest frame. EndOfFrame copies it into head_pose
			// and friends below, which are then the GS thread's own.
			struct PacerHead
			{
				XrPosef pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
				bool valid = false;
				bool position_valid = false;
			};
			std::mutex head_mutex;
			PacerHead pacer_head;

			// Lock order: submit_mutex, then the Vulkan queue lock (VR::ScopedXrQueueLock); never the reverse.
			// The pacer holds submit_mutex from copying the published layers through xrEndFrame; a swapchain is
			// destroyed under it, after its entries are dropped. publish_mutex only covers copies (no XR call inside).
			std::timed_mutex submit_mutex;
			std::mutex publish_mutex;
			PublishedSet published = {};

			// The opaque black background projection layer.
			XrSwapchain bg_swapchain = XR_NULL_HANDLE;
			bool bg_ready = false;
			bool bg_logged = false;

			// The runtime's recommended per-eye image width, for the headset's pixel density (Initialize).
			u32 view_rec_width[2] = {0, 0};

			// GS-side counts for the 5 s Frames line, taken by whichever thread logs it.
			std::atomic<u32> stat_gs_frames{0};
			std::atomic<u32> stat_copy_skipped{0};
			std::atomic<u32> stat_copy_timeout{0};

			// Headset rate: SteadyNowMs at the top of every pacer loop and when its xrWaitFrame returns. An old value
			// means the pacer is stuck in the runtime; the GS thread then pumps the session events (PacerStalled).
			std::atomic<u64> pacer_heartbeat_ms{0};

			// XR_ERROR_RUNTIME_FAILURE in a row from the frame calls (the pacer's xrWaitFrame counts too).
			std::atomic<u32> runtime_failures{0};
			// Whether the screen copies were last logged layered (1) or mono (0); -1 before the first copy.
			int layered_logged = -1;

			// PCSX2_VR_XRFAULT=hangwait: the pacer waits here forever.
			std::mutex hang_mutex;
			std::condition_variable hang_cv;

			struct EyeChain
			{
				XrSwapchain swapchain = XR_NULL_HANDLE;
				std::vector<XrSwapchainImageVulkan2KHR> images;
				bool ever_released = false;
				bool wait_pending = false;
				uint32_t pending_index = 0;
				// Headset rate: this present's image is copied but not yet released. EndOfFrame releases it together
				// with publishing its layers, under submit_mutex, so the pacer never shows it at the previous pose.
				bool release_pending = false;
			};
			EyeChain chains[2];
			u32 swapchain_width = 0;
			u32 swapchain_height = 0;

			struct LeverChain
			{
				XrSwapchain swapchain = XR_NULL_HANDLE;
				std::vector<XrSwapchainImageVulkan2KHR> images;
				bool ever_released = false;
				bool wait_pending = false;
				uint32_t pending_index = 0;
				u32 last_key = 0xFFFFFFFFu;
			};
			LeverChain levers[ControlQuads::kSlots];
			VkBuffer lever_staging = VK_NULL_HANDLE;
			VkDeviceMemory lever_staging_memory = VK_NULL_HANDLE;
			void* lever_staging_map = nullptr;
			VkCommandBuffer lever_cmd = VK_NULL_HANDLE;
			VkFence lever_fence = VK_NULL_HANDLE;
			bool lever_fence_submitted = false;
			bool warned_lever = false;
			int lever_layers_logged = -1;
			u32 lever_screen_logged = 0;

			// First-person hands: CPU-rendered impostors with their own swapchains, staging buffer,
			// command buffer and fence (the lever cards' path, kept separate so neither disturbs the other).
			struct HandChain
			{
				XrSwapchain swapchain = XR_NULL_HANDLE;
				std::vector<XrSwapchainImageVulkan2KHR> images;
				bool ever_released = false;
				bool wait_pending = false;
				uint32_t pending_index = 0;
				// Headset rate: as EyeChain::release_pending; pending_submitted says whether its upload was submitted.
				bool release_pending = false;
				bool pending_submitted = false;
			};
			HandChain hand_chains[kHandSlots];
			VkBuffer hand_staging = VK_NULL_HANDLE;
			VkDeviceMemory hand_staging_memory = VK_NULL_HANDLE;
			void* hand_staging_map = nullptr;
			VkCommandBuffer hand_cmd = VK_NULL_HANDLE;
			VkFence hand_fence = VK_NULL_HANDLE;
			bool hand_fence_submitted = false;
			// Set when creating any of the above fails: hands stay off until the next Initialize().
			bool hands_disabled = false;

			// HUD cards (HudCards: the laser sight, the wrist card, the comfort vignette, the toast and the blink):
			// the same CPU-raster upload path once more, one swapchain per card sharing a staging buffer, command
			// buffer and fence. Created the first time a card shows and kept until Shutdown; any failure turns the
			// cards off for the session.
			struct HudChain
			{
				XrSwapchain swapchain = XR_NULL_HANDLE;
				std::vector<XrSwapchainImageVulkan2KHR> images;
				bool ever_released = false;
				bool wait_pending = false;
				uint32_t pending_index = 0;
				// What the last released image holds: the card's content key and fade step.
				u64 shown_content = 0;
				int shown_level = -1;
			};
			HudChain hud_chains[HudCards::kCardCount];
			VkBuffer hud_staging = VK_NULL_HANDLE;
			VkDeviceMemory hud_staging_memory = VK_NULL_HANDLE;
			void* hud_staging_map = nullptr;
			VkCommandBuffer hud_cmd = VK_NULL_HANDLE;
			VkFence hud_fence = VK_NULL_HANDLE;
			bool hud_fence_submitted = false;
			bool hud_disabled = false;
			// Wrist card: facing the head (with hysteresis), its fade, and the clock the fade steps by.
			bool hud_wrist_facing = false;
			float hud_wrist_opacity = 0.0f;
			u64 hud_last_ms = 0;
			bool hud_wrist_logged = false;
			// Laser sight: its fade in (on the same clock), and whether it was logged yet this session.
			float hud_laser_opacity = 0.0f;
			bool hud_laser_logged = false;
			// Comfort vignette: whether it was logged yet this session (its opacity comes eased from CameraDriver).
			bool hud_vignette_logged = false;
			// Comfort blink on a screen-mode change: when (SteadyNowMs) the change waiting for the blink to
			// go black is made; 0 while none waits.
			u64 fp_switch_due_ms = 0;

			XrSpace view_space = XR_NULL_HANDLE;

			float screen_anchor_x = 0.0f;
			float screen_anchor_y = 0.0f;
			float screen_anchor_z = 0.0f;
			float screen_anchor_yaw = 0.0f;

			// Where the world-locked game screen sits. Follows the user's re-anchor, and is also placed
			// in front of the head when the first-person screen hands over, without touching the
			// screen_anchor_* / s_anchor_snapshot that spatial controls and lever cards hang off.
			float world_anchor_x = 0.0f;
			float world_anchor_y = 0.0f;
			float world_anchor_z = 0.0f;
			float world_anchor_yaw = 0.0f;

			XrPosef head_pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
			bool head_pose_valid = false;
			bool head_position_valid = false;

			VkCommandPool cmd_pool = VK_NULL_HANDLE;
			VkCommandBuffer cmd_buffers[NUM_CMD_BUFFERS] = {};
			VkFence fences[NUM_CMD_BUFFERS] = {};
			bool fence_submitted[NUM_CMD_BUFFERS] = {};
			u32 next_cmd_buffer = 0;

			bool warned_pacer = false;
			bool warned_beginframe = false;
			bool warned_endframe = false;
			bool warned_acquire = false;
			bool warned_wait_timeout = false;
			bool warned_wait_fail = false;
			bool warned_release = false;
			bool warned_fence_timeout = false;
			bool warned_swapchain = false;
			VkFormat swapchain_format = VK_FORMAT_R8G8B8A8_SRGB;
		} s;

		enum class CopyResult
		{
			Copied,
			Skipped,
			TimeoutZeroLayer
		};

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

		void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
			VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
			VkPipelineStageFlags dst_stage)
		{
			VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			barrier.srcAccessMask = src_access;
			barrier.dstAccessMask = dst_access;
			barrier.oldLayout = old_layout;
			barrier.newLayout = new_layout;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = image;
			barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}

		// The XR and Vulkan calls that use the queue the session shares with the GS, each under the queue lock
		// (VRVulkanBridge.h). xrWaitSwapchainImage and xrWaitFrame never take it.
		XrResult LockedAcquire(XrSwapchain swapchain, uint32_t* index)
		{
			XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			const VR::ScopedXrQueueLock lock;
			return xrAcquireSwapchainImage(swapchain, &ai, index);
		}

		XrResult LockedRelease(XrSwapchain swapchain)
		{
			XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			const VR::ScopedXrQueueLock lock;
			return xrReleaseSwapchainImage(swapchain, &ri);
		}

		VkResult LockedQueueSubmit(VkQueue queue, const VkSubmitInfo& si, VkFence fence)
		{
			const VR::ScopedXrQueueLock lock;
			return vkQueueSubmit(queue, 1, &si, fence);
		}

		XrResult LockedBeginFrame(XrSession session)
		{
			XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
			const VR::ScopedXrQueueLock lock;
			return xrBeginFrame(session, &bi);
		}

		XrResult LockedEndFrame(XrSession session, XrTime display_time, u32 layer_count,
			const XrCompositionLayerBaseHeader* const* layers)
		{
			XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
			ei.displayTime = display_time;
			ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
			ei.layerCount = layer_count;
			ei.layers = layer_count ? layers : nullptr;
			const VR::ScopedXrQueueLock lock;
			return xrEndFrame(session, &ei);
		}

		// Drops every published entry that shows this swapchain (publish_mutex held by the caller).
		void ForgetPublishedSwapchain(XrSwapchain swapchain)
		{
			PublishedSet& set = s.published;
			u32 kept = 0;
			for (u32 i = 0; i < set.count; i++)
			{
				const PublishedSet::Entry& entry = set.e[i];
				const XrSwapchain shown = (entry.kind == kKindCylinder) ? entry.cyl.subImage.swapchain : entry.quad.subImage.swapchain;
				if (shown == swapchain)
					continue;
				if (kept != i)
					set.e[kept] = entry;
				kept++;
			}
			set.count = kept;
		}

		// Destroys a swapchain the pacer may be submitting: waits for the pacer's submit in progress (submit_mutex),
		// drops the published entries that show it, then destroys it. Bounded like the queue lock: a pacer stuck in
		// xrEndFrame for 100 ms means the runtime stopped answering, so VR is abandoned and the swapchain left alive.
		// With the compositor abandoned the handle is only forgotten.
		void DestroySwapchainSynced(XrSwapchain swapchain)
		{
			if (swapchain == XR_NULL_HANDLE || s.abandoned.load(std::memory_order_acquire))
				return;
			std::unique_lock<std::timed_mutex> submit(s.submit_mutex, std::defer_lock);
			if (!submit.try_lock_for(std::chrono::milliseconds(100)))
			{
				Console.Error("(VR) The VR pacer has been inside xrEndFrame for over 100 ms (runtime not answering): "
							  "swapchain left alive, abandoning VR; emulation continues.");
				XRSession::MarkLost();
				std::lock_guard<std::mutex> publish(s.publish_mutex);
				ForgetPublishedSwapchain(swapchain);
				return;
			}
			{
				std::lock_guard<std::mutex> publish(s.publish_mutex);
				ForgetPublishedSwapchain(swapchain);
			}
			xrDestroySwapchain(swapchain);
		}

		void WaitAllFences()
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			for (u32 i = 0; i < NUM_CMD_BUFFERS; i++)
			{
				if (!s.fence_submitted[i])
					continue;

				const VkResult res = vkWaitForFences(h.device, 1, &s.fences[i], VK_TRUE, kTeardownFenceNs);
				if (res == VK_TIMEOUT)
					Console.Warning("(VR) Copy fence %u still busy after 250 ms during teardown.", i);
				vkResetFences(h.device, 1, &s.fences[i]);
				s.fence_submitted[i] = false;
			}
		}

		void DestroySwapchains()
		{
			WaitAllFences();
			for (auto& chain : s.chains)
			{
				if (chain.swapchain != XR_NULL_HANDLE)
				{
					DestroySwapchainSynced(chain.swapchain);
					chain.swapchain = XR_NULL_HANDLE;
				}
				chain.images.clear();
				chain.ever_released = false;
				chain.wait_pending = false;
				chain.release_pending = false;
			}
			s.swapchain_width = 0;
			s.swapchain_height = 0;
		}

		bool CreateChain(u32 eye, u32 w, u32 h)
		{
			auto& chain = s.chains[eye];

			XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
			ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
			ci.format = static_cast<int64_t>(s.swapchain_format);
			ci.sampleCount = 1;
			ci.width = w;
			ci.height = h;
			ci.faceCount = 1;
			ci.arraySize = 1;
			ci.mipCount = 1;

			XrResult res = xrCreateSwapchain(XRSession::GetSession(), &ci, &chain.swapchain);
			if (XR_FAILED(res))
			{
				chain.swapchain = XR_NULL_HANDLE;
				if (!s.warned_swapchain)
				{
					s.warned_swapchain = true;
					Console.Error("(VR) xrCreateSwapchain failed (%d) for %ux%u; frames skipped until it succeeds.",
						static_cast<int>(res), w, h);
				}
				return false;
			}

			uint32_t count = 0;
			if (!CheckXR(xrEnumerateSwapchainImages(chain.swapchain, 0, &count, nullptr), "xrEnumerateSwapchainImages") ||
				count == 0)
			{
				return false;
			}

			chain.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
			if (!CheckXR(xrEnumerateSwapchainImages(chain.swapchain, count, &count,
							 reinterpret_cast<XrSwapchainImageBaseHeader*>(chain.images.data())),
					"xrEnumerateSwapchainImages"))
			{
				return false;
			}

			Console.WriteLn("(VR) XR swapchain created (eye %u): %ux%u, %u images (%s).",
				eye, w, h, count,
				s.swapchain_format == VK_FORMAT_R8G8B8A8_UNORM ? "VK_FORMAT_R8G8B8A8_UNORM" :
																  "VK_FORMAT_R8G8B8A8_SRGB");
			return true;
		}

		bool EnsureSwapchains(u32 w, u32 h, bool stereo)
		{
			if (s.swapchain_width != 0 && (s.swapchain_width != w || s.swapchain_height != h))
				DestroySwapchains();

			if (s.chains[0].swapchain == XR_NULL_HANDLE)
			{
				if (!CreateChain(0, w, h))
				{
					DestroySwapchains();
					return false;
				}
			}
			if (stereo && s.chains[1].swapchain == XR_NULL_HANDLE)
			{
				if (!CreateChain(1, w, h))
				{
					DestroySwapchains();
					return false;
				}
			}

			s.swapchain_width = w;
			s.swapchain_height = h;
			s.warned_swapchain = false;
			return true;
		}

		bool RecordAndSubmitCopy(GSTextureVK* src, u32 src_layer, VkImage dst)
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();

			const u32 i = s.next_cmd_buffer;
			s.next_cmd_buffer = (s.next_cmd_buffer + 1) % NUM_CMD_BUFFERS;

			if (s.fence_submitted[i])
			{
				const VkResult wr = vkWaitForFences(h.device, 1, &s.fences[i], VK_TRUE, ONE_SECOND_NS);
				if (wr != VK_SUCCESS)
				{
					if (!s.warned_fence_timeout)
					{
						s.warned_fence_timeout = true;
						Console.Warning("(VR) Copy fence wait returned %d; skipping this frame's copy.",
							static_cast<int>(wr));
					}
					return false;
				}
				vkResetFences(h.device, 1, &s.fences[i]);
				s.fence_submitted[i] = false;
			}

			VkCommandBuffer cmd = s.cmd_buffers[i];
			VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			VkResult vr = vkBeginCommandBuffer(cmd, &bi);
			if (vr != VK_SUCCESS)
			{
				Console.Error("(VR) vkBeginCommandBuffer failed (%d).", static_cast<int>(vr));
				return false;
			}

			src->TransitionToLayout(cmd, GSTextureVK::Layout::TransferSrc);

			ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

			VkImageCopy region = {};
			region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, src_layer, 1};
			region.srcOffset = {0, 0, 0};
			region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			region.dstOffset = {0, 0, 0};
			region.extent = {s.swapchain_width, s.swapchain_height, 1};
			vkCmdCopyImage(cmd, src->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

			ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

			vr = vkEndCommandBuffer(cmd);
			if (vr != VK_SUCCESS)
			{
				Console.Error("(VR) vkEndCommandBuffer failed (%d).", static_cast<int>(vr));
				return false;
			}

			VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
			si.commandBufferCount = 1;
			si.pCommandBuffers = &cmd;
			vr = LockedQueueSubmit(h.queue, si, s.fences[i]);
			if (vr != VK_SUCCESS)
			{
				Console.Error("(VR) vkQueueSubmit failed (%d).", static_cast<int>(vr));
				return false;
			}
			s.fence_submitted[i] = true;
			return true;
		}

		// Legacy: copies and releases. Headset rate: copies and leaves the image acquired (release_pending); EndOfFrame
		// releases it under submit_mutex together with publishing the layers that show it (ReleaseAndPublish), so
		// the pacer can never show the new image at the previous present's pose. Copied then means "copied, release
		// still to come"; a failed release turns it into Skipped there.
		CopyResult CopyToSwapchain(GSTextureVK* src, u32 src_layer, decltype(s.chains[0])& chain)
		{
			// An image still waiting for its release (the last publish could not take submit_mutex, so the runtime
			// was marked lost): leave the chain alone until teardown destroys it.
			if (chain.release_pending)
				return CopyResult::Skipped;

			uint32_t index = 0;
			if (chain.wait_pending)
			{
				index = chain.pending_index;
			}
			else
			{
				XrResult res = LockedAcquire(chain.swapchain, &index);
				if (XR_FAILED(res))
				{
					if (!s.warned_acquire)
					{
						s.warned_acquire = true;
						Console.Error("(VR) xrAcquireSwapchainImage failed (%d).", static_cast<int>(res));
					}
					return CopyResult::Skipped;
				}
				chain.pending_index = index;
				chain.wait_pending = true;
			}

			XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wi.timeout = kSwapchainWaitNs;
			XrResult res = xrWaitSwapchainImage(chain.swapchain, &wi);
			if (res == XR_TIMEOUT_EXPIRED)
			{
				// The image stays acquired (wait_pending); the next present waits on it again.
				static u64 s_wait_timeouts = 0;
				s_wait_timeouts++;
				if (!s.warned_wait_timeout || (s_wait_timeouts % 600) == 0)
				{
					s.warned_wait_timeout = true;
					Console.Warning("(VR) xrWaitSwapchainImage timed out (%llu so far): the runtime isn't taking frames "
									"(headset asleep?); skipping copies, emulation keeps running.",
						static_cast<unsigned long long>(s_wait_timeouts));
				}
				return CopyResult::TimeoutZeroLayer;
			}
			if (XR_FAILED(res))
			{
				if (!s.warned_wait_fail)
				{
					s.warned_wait_fail = true;
					Console.Error("(VR) xrWaitSwapchainImage failed (%d).", static_cast<int>(res));
				}
				return CopyResult::TimeoutZeroLayer;
			}
			chain.wait_pending = false;

			const bool submitted = RecordAndSubmitCopy(src, src_layer, chain.images[index].image);

			if (s.headset_rate)
			{
				// Released (even when the copy was not submitted: a waited image must go back) in ReleaseAndPublish.
				chain.release_pending = true;
				return submitted ? CopyResult::Copied : CopyResult::Skipped;
			}

			res = LockedRelease(chain.swapchain);
			if (XR_FAILED(res))
			{
				if (!s.warned_release)
				{
					s.warned_release = true;
					Console.Error("(VR) xrReleaseSwapchainImage failed (%d).", static_cast<int>(res));
				}
				return CopyResult::Skipped;
			}

			return submitted ? CopyResult::Copied : CopyResult::Skipped;
		}

		void DestroyLeverChains()
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			if (h.IsValid() && s.lever_fence != VK_NULL_HANDLE && s.lever_fence_submitted)
			{
				if (vkWaitForFences(h.device, 1, &s.lever_fence, VK_TRUE, kTeardownFenceNs) == VK_TIMEOUT)
					Console.Warning("(VR) Control quads: lever upload fence still busy after 250 ms during teardown.");
				vkResetFences(h.device, 1, &s.lever_fence);
				s.lever_fence_submitted = false;
			}
			for (auto& lc : s.levers)
			{
				if (lc.swapchain != XR_NULL_HANDLE)
				{
					DestroySwapchainSynced(lc.swapchain);
					lc.swapchain = XR_NULL_HANDLE;
				}
				lc.images.clear();
				lc.ever_released = false;
				lc.wait_pending = false;
				lc.last_key = 0xFFFFFFFFu;
			}
		}

		void DestroyLeverResources()
		{
			DestroyLeverChains();
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			if (h.IsValid())
			{
				if (s.lever_staging_map && s.lever_staging_memory != VK_NULL_HANDLE)
					vkUnmapMemory(h.device, s.lever_staging_memory);
				if (s.lever_staging != VK_NULL_HANDLE)
					vkDestroyBuffer(h.device, s.lever_staging, nullptr);
				if (s.lever_staging_memory != VK_NULL_HANDLE)
					vkFreeMemory(h.device, s.lever_staging_memory, nullptr);
				if (s.lever_fence != VK_NULL_HANDLE)
					vkDestroyFence(h.device, s.lever_fence, nullptr);
				if (s.lever_cmd != VK_NULL_HANDLE && s.cmd_pool != VK_NULL_HANDLE)
					vkFreeCommandBuffers(h.device, s.cmd_pool, 1, &s.lever_cmd);
			}
			s.lever_staging_map = nullptr;
			s.lever_staging = VK_NULL_HANDLE;
			s.lever_staging_memory = VK_NULL_HANDLE;
			s.lever_fence = VK_NULL_HANDLE;
			s.lever_fence_submitted = false;
			s.lever_cmd = VK_NULL_HANDLE;
		}

		bool EnsureLeverResources()
		{
			if (s.lever_staging != VK_NULL_HANDLE && s.lever_cmd != VK_NULL_HANDLE && s.lever_fence != VK_NULL_HANDLE)
				return true;
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			if (!h.IsValid() || s.cmd_pool == VK_NULL_HANDLE)
				return false;
			const auto fail = [&](const char* what, int code) {
				if (!s.warned_lever)
				{
					s.warned_lever = true;
					Console.Error("(VR) Control quads: %s failed (%d); the lever cards will not be drawn this session.", what, code);
				}
				DestroyLeverResources();
				return false;
			};

			constexpr VkDeviceSize kBytes =
				static_cast<VkDeviceSize>(ControlQuads::kWidth) * ControlQuads::kHeight * 4u * ControlQuads::kSlots;
			VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
			bci.size = kBytes;
			bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VkResult vr = vkCreateBuffer(h.device, &bci, nullptr, &s.lever_staging);
			if (vr != VK_SUCCESS)
				return fail("vkCreateBuffer", static_cast<int>(vr));

			VkMemoryRequirements mr = {};
			vkGetBufferMemoryRequirements(h.device, s.lever_staging, &mr);
			VkPhysicalDeviceMemoryProperties mp = {};
			vkGetPhysicalDeviceMemoryProperties(h.physical_device, &mp);
			constexpr VkMemoryPropertyFlags kWant = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
			u32 type = UINT32_MAX;
			for (u32 i = 0; i < mp.memoryTypeCount; i++)
			{
				if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & kWant) == kWant)
				{
					type = i;
					break;
				}
			}
			if (type == UINT32_MAX)
				return fail("host-visible memory type lookup", 0);
			VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			mai.allocationSize = mr.size;
			mai.memoryTypeIndex = type;
			vr = vkAllocateMemory(h.device, &mai, nullptr, &s.lever_staging_memory);
			if (vr != VK_SUCCESS)
				return fail("vkAllocateMemory", static_cast<int>(vr));
			vr = vkBindBufferMemory(h.device, s.lever_staging, s.lever_staging_memory, 0);
			if (vr != VK_SUCCESS)
				return fail("vkBindBufferMemory", static_cast<int>(vr));
			vr = vkMapMemory(h.device, s.lever_staging_memory, 0, VK_WHOLE_SIZE, 0, &s.lever_staging_map);
			if (vr != VK_SUCCESS)
				return fail("vkMapMemory", static_cast<int>(vr));

			VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
			cai.commandPool = s.cmd_pool;
			cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			cai.commandBufferCount = 1;
			vr = vkAllocateCommandBuffers(h.device, &cai, &s.lever_cmd);
			if (vr != VK_SUCCESS)
				return fail("vkAllocateCommandBuffers", static_cast<int>(vr));
			VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			vr = vkCreateFence(h.device, &fci, nullptr, &s.lever_fence);
			if (vr != VK_SUCCESS)
				return fail("vkCreateFence", static_cast<int>(vr));
			Console.WriteLn("(VR) Control quads: lever upload resources created (%u x %u x %d cards, %llu-byte staging).",
				ControlQuads::kWidth, ControlQuads::kHeight, ControlQuads::kSlots, static_cast<unsigned long long>(kBytes));
			return true;
		}

		bool EnsureLeverChain(int slot)
		{
			auto& lc = s.levers[slot];
			if (lc.swapchain != XR_NULL_HANDLE)
				return true;
			XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
			ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
			ci.format = static_cast<int64_t>(s.swapchain_format);
			ci.sampleCount = 1;
			ci.width = ControlQuads::kWidth;
			ci.height = ControlQuads::kHeight;
			ci.faceCount = 1;
			ci.arraySize = 1;
			ci.mipCount = 1;
			const XrResult res = xrCreateSwapchain(XRSession::GetSession(), &ci, &lc.swapchain);
			if (XR_FAILED(res))
			{
				lc.swapchain = XR_NULL_HANDLE;
				if (!s.warned_lever)
				{
					s.warned_lever = true;
					Console.Error("(VR) Control quads: xrCreateSwapchain failed (%d) for lever %d; no card.", static_cast<int>(res), slot);
				}
				return false;
			}
			uint32_t count = 0;
			if (!CheckXR(xrEnumerateSwapchainImages(lc.swapchain, 0, &count, nullptr), "xrEnumerateSwapchainImages (lever)") || count == 0)
				return false;
			lc.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
			if (!CheckXR(xrEnumerateSwapchainImages(lc.swapchain, count, &count,
							 reinterpret_cast<XrSwapchainImageBaseHeader*>(lc.images.data())),
					"xrEnumerateSwapchainImages (lever)"))
			{
				return false;
			}
			Console.WriteLn("(VR) Control quads: lever %d swapchain created (%ux%u, %u images).", slot, ControlQuads::kWidth,
				ControlQuads::kHeight, count);
			return true;
		}

		void RecordLeverUpload(VkCommandBuffer cmd, int slot, VkImage dst, float t, bool grabbed, bool broke_away)
		{
			static std::vector<u32> pixels;
			ControlQuads::RasterLever(pixels, t, grabbed, broke_away, slot);
			const VkDeviceSize bytes = static_cast<VkDeviceSize>(ControlQuads::kWidth) * ControlQuads::kHeight * 4u;
			const VkDeviceSize offset = bytes * static_cast<VkDeviceSize>(slot);
			std::memcpy(static_cast<u8*>(s.lever_staging_map) + offset, pixels.data(), static_cast<size_t>(bytes));

			ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkBufferImageCopy region = {};
			region.bufferOffset = offset;
			region.bufferRowLength = 0;
			region.bufferImageHeight = 0;
			region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			region.imageOffset = {0, 0, 0};
			region.imageExtent = {ControlQuads::kWidth, ControlQuads::kHeight, 1};
			vkCmdCopyBufferToImage(cmd, s.lever_staging, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
		}

		u32 BuildLeverLayers(XrCompositionLayerQuad* lever_quads, const XrCompositionLayerBaseHeader** layers,
			u32& layer_count, u32 layer_capacity, bool split)
		{
			ControlQuads::Slot slots[ControlQuads::kSlots];
			bool any = false;
			for (int i = 0; i < ControlQuads::kSlots; i++)
			{
				slots[i] = ControlQuads::Get(i);
				any = any || slots[i].active;
			}
			if (!any)
			{
				if (s.levers[0].swapchain != XR_NULL_HANDLE || s.levers[1].swapchain != XR_NULL_HANDLE)
				{
					DestroyLeverChains();
					Console.WriteLn("(VR) Control quads: no active control — lever swapchains destroyed.");
				}
				if (s.lever_layers_logged != 0)
				{
					s.lever_layers_logged = 0;
					s.lever_screen_logged = layer_count;
				}
				return 0;
			}
			if (!EnsureLeverResources())
				return 0;

			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			bool recording = false;
			int pending_release[ControlQuads::kSlots];
			u32 pending_key[ControlQuads::kSlots];
			int pending_count = 0;
			for (int i = 0; i < ControlQuads::kSlots; i++)
			{
				if (!slots[i].active || !EnsureLeverChain(i))
					continue;
				auto& lc = s.levers[i];
				const u32 key = ControlQuads::RasterKey(slots[i].t, slots[i].grabbed, slots[i].broke_away);
				if (lc.ever_released && key == lc.last_key)
					continue;

				uint32_t index = 0;
				if (lc.wait_pending)
				{
					index = lc.pending_index;
				}
				else
				{
					if (XR_FAILED(LockedAcquire(lc.swapchain, &index)))
						continue;
					lc.pending_index = index;
					lc.wait_pending = true;
				}
				XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
				wi.timeout = kSwapchainWaitNs;
				// XR_TIMEOUT_EXPIRED is a success code: the image isn't ours yet, so wait again next frame.
				const XrResult lwr = xrWaitSwapchainImage(lc.swapchain, &wi);
				if (lwr == XR_TIMEOUT_EXPIRED || XR_FAILED(lwr))
					continue;
				lc.wait_pending = false;

				if (!recording)
				{
					if (s.lever_fence_submitted)
					{
						if (vkWaitForFences(h.device, 1, &s.lever_fence, VK_TRUE, ONE_SECOND_NS) != VK_SUCCESS)
						{
							LockedRelease(lc.swapchain);
							break;
						}
						vkResetFences(h.device, 1, &s.lever_fence);
						s.lever_fence_submitted = false;
					}
					VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
					bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
					if (vkBeginCommandBuffer(s.lever_cmd, &bi) != VK_SUCCESS)
					{
						LockedRelease(lc.swapchain);
						break;
					}
					recording = true;
				}
				RecordLeverUpload(s.lever_cmd, i, lc.images[index].image, slots[i].t, slots[i].grabbed, slots[i].broke_away);
				pending_release[pending_count] = i;
				pending_key[pending_count] = key;
				pending_count++;
			}
			if (recording)
			{
				bool submitted = false;
				if (vkEndCommandBuffer(s.lever_cmd) == VK_SUCCESS)
				{
					VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
					si.commandBufferCount = 1;
					si.pCommandBuffers = &s.lever_cmd;
					submitted = (LockedQueueSubmit(h.queue, si, s.lever_fence) == VK_SUCCESS);
					s.lever_fence_submitted = submitted;
				}
				for (int k = 0; k < pending_count; k++)
				{
					auto& lc = s.levers[pending_release[k]];
					if (XR_SUCCEEDED(LockedRelease(lc.swapchain)) && submitted)
					{
						lc.ever_released = true;
						lc.last_key = pending_key[k];
					}
				}
			}

			u32 appended = 0;
			for (int i = 0; i < ControlQuads::kSlots; i++)
			{
				if (!slots[i].active || !s.levers[i].ever_released)
					continue;
				if (layer_count >= layer_capacity)
					break;
				SpatialControls::Anchor a;
				a.position = {s.screen_anchor_x, s.screen_anchor_y, s.screen_anchor_z};
				a.yaw = s.screen_anchor_yaw;
				SpatialControls::Placement pl;
				pl.side = slots[i].side;
				pl.height = slots[i].height;
				pl.forward = slots[i].forward;
				pl.yaw_deg = slots[i].yaw_deg;
				const SpatialControls::Frame f = SpatialControls::PlaceControl(a, pl);
				const float pitch = std::atan2(-slots[i].height, std::max(slots[i].forward, 0.05f));
				const SpatialControls::Quat pitch_q = {std::sin(-pitch * 0.5f), 0.0f, 0.0f, std::cos(-pitch * 0.5f)};
				const SpatialControls::Quat q = f.orientation * pitch_q;

				XrCompositionLayerQuad& quad = lever_quads[i];
				quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
				quad.next = nullptr;
				quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
				quad.space = XRSession::GetSpace();
				quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				quad.subImage = {s.levers[i].swapchain,
					{{0, 0}, {static_cast<s32>(ControlQuads::kWidth), static_cast<s32>(ControlQuads::kHeight)}}, 0};
				quad.pose.orientation = {q.x, q.y, q.z, q.w};
				quad.pose.position = {f.pivot.x, f.pivot.y, f.pivot.z};
				quad.size = {ControlQuads::kQuadWidthM, ControlQuads::kQuadHeightM};
				layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
				appended++;
			}

			const u32 screen_layers = layer_count - appended;
			if (static_cast<int>(appended) != s.lever_layers_logged || screen_layers != s.lever_screen_logged)
			{
				s.lever_layers_logged = static_cast<int>(appended);
				s.lever_screen_logged = screen_layers;
				Console.WriteLn("(VR) Control quads: %u lever layer(s) after %u screen layer(s) [%s branch, capacity %u]%s.",
					appended, screen_layers, split ? "split-screen" : "mirror", layer_capacity,
					(appended == 0 && any) ? " — no card released yet or no slot left" : "");
			}
			return appended;
		}

		void DestroyHandResources()
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			if (h.IsValid() && s.hand_fence != VK_NULL_HANDLE && s.hand_fence_submitted)
			{
				if (vkWaitForFences(h.device, 1, &s.hand_fence, VK_TRUE, kTeardownFenceNs) == VK_TIMEOUT)
					Console.Warning("(VR) Hands: upload fence still busy after 250 ms during teardown.");
				vkResetFences(h.device, 1, &s.hand_fence);
			}
			s.hand_fence_submitted = false;
			for (auto& hc : s.hand_chains)
			{
				if (hc.swapchain != XR_NULL_HANDLE)
				{
					DestroySwapchainSynced(hc.swapchain);
					hc.swapchain = XR_NULL_HANDLE;
				}
				hc.images.clear();
				hc.ever_released = false;
				hc.wait_pending = false;
				hc.pending_index = 0;
				hc.release_pending = false;
				hc.pending_submitted = false;
			}
			if (h.IsValid())
			{
				if (s.hand_staging_map && s.hand_staging_memory != VK_NULL_HANDLE)
					vkUnmapMemory(h.device, s.hand_staging_memory);
				if (s.hand_staging != VK_NULL_HANDLE)
					vkDestroyBuffer(h.device, s.hand_staging, nullptr);
				if (s.hand_staging_memory != VK_NULL_HANDLE)
					vkFreeMemory(h.device, s.hand_staging_memory, nullptr);
				if (s.hand_fence != VK_NULL_HANDLE)
					vkDestroyFence(h.device, s.hand_fence, nullptr);
				if (s.hand_cmd != VK_NULL_HANDLE && s.cmd_pool != VK_NULL_HANDLE)
					vkFreeCommandBuffers(h.device, s.cmd_pool, 1, &s.hand_cmd);
			}
			s.hand_staging_map = nullptr;
			s.hand_staging = VK_NULL_HANDLE;
			s.hand_staging_memory = VK_NULL_HANDLE;
			s.hand_fence = VK_NULL_HANDLE;
			s.hand_cmd = VK_NULL_HANDLE;
		}

		// Everything the hands need, created together the first time a hand is shown and kept until
		// Shutdown (first person toggles often; recreating swapchains each time would only churn).
		// Any failure is logged once and turns the hands off for the session; nothing else depends on them.
		bool EnsureHandResources(bool fake, bool grip_orient)
		{
			if (s.hands_disabled)
				return false;
			if (s.hand_staging != VK_NULL_HANDLE && s.hand_cmd != VK_NULL_HANDLE && s.hand_fence != VK_NULL_HANDLE &&
				s.hand_chains[0].swapchain != XR_NULL_HANDLE && s.hand_chains[1].swapchain != XR_NULL_HANDLE)
			{
				return true;
			}
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			if (!h.IsValid() || s.cmd_pool == VK_NULL_HANDLE)
				return false;
			const auto fail = [&](const char* what, int code) {
				s.hands_disabled = true;
				Console.Error("(VR) Hands: %s failed (%d); first-person hands are off for this session.", what, code);
				DestroyHandResources();
				return false;
			};

			constexpr VkDeviceSize kBytes = kHandImageBytes * static_cast<VkDeviceSize>(kHandSlots);
			if (s.hand_staging == VK_NULL_HANDLE)
			{
				VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
				bci.size = kBytes;
				bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
				bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
				VkResult vr = vkCreateBuffer(h.device, &bci, nullptr, &s.hand_staging);
				if (vr != VK_SUCCESS)
				{
					s.hand_staging = VK_NULL_HANDLE;
					return fail("vkCreateBuffer", static_cast<int>(vr));
				}

				VkMemoryRequirements mr = {};
				vkGetBufferMemoryRequirements(h.device, s.hand_staging, &mr);
				VkPhysicalDeviceMemoryProperties mp = {};
				vkGetPhysicalDeviceMemoryProperties(h.physical_device, &mp);
				constexpr VkMemoryPropertyFlags kWant = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
				u32 type = UINT32_MAX;
				for (u32 i = 0; i < mp.memoryTypeCount; i++)
				{
					if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & kWant) == kWant)
					{
						type = i;
						break;
					}
				}
				if (type == UINT32_MAX)
					return fail("host-visible memory type lookup", 0);
				VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
				mai.allocationSize = mr.size;
				mai.memoryTypeIndex = type;
				vr = vkAllocateMemory(h.device, &mai, nullptr, &s.hand_staging_memory);
				if (vr != VK_SUCCESS)
				{
					s.hand_staging_memory = VK_NULL_HANDLE;
					return fail("vkAllocateMemory", static_cast<int>(vr));
				}
				vr = vkBindBufferMemory(h.device, s.hand_staging, s.hand_staging_memory, 0);
				if (vr != VK_SUCCESS)
					return fail("vkBindBufferMemory", static_cast<int>(vr));
				vr = vkMapMemory(h.device, s.hand_staging_memory, 0, VK_WHOLE_SIZE, 0, &s.hand_staging_map);
				if (vr != VK_SUCCESS)
				{
					s.hand_staging_map = nullptr;
					return fail("vkMapMemory", static_cast<int>(vr));
				}
			}

			if (s.hand_cmd == VK_NULL_HANDLE)
			{
				VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
				cai.commandPool = s.cmd_pool;
				cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
				cai.commandBufferCount = 1;
				const VkResult vr = vkAllocateCommandBuffers(h.device, &cai, &s.hand_cmd);
				if (vr != VK_SUCCESS)
				{
					s.hand_cmd = VK_NULL_HANDLE;
					return fail("vkAllocateCommandBuffers", static_cast<int>(vr));
				}
			}
			if (s.hand_fence == VK_NULL_HANDLE)
			{
				VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
				const VkResult vr = vkCreateFence(h.device, &fci, nullptr, &s.hand_fence);
				if (vr != VK_SUCCESS)
				{
					s.hand_fence = VK_NULL_HANDLE;
					return fail("vkCreateFence", static_cast<int>(vr));
				}
				s.hand_fence_submitted = false;
			}

			uint32_t image_count = 0;
			for (auto& hc : s.hand_chains)
			{
				if (hc.swapchain != XR_NULL_HANDLE)
					continue;
				// Same format as the screen (sRGB normally): HandModel writes sRGB-encoded bytes with alpha.
				XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
				ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
				ci.format = static_cast<int64_t>(s.swapchain_format);
				ci.sampleCount = 1;
				ci.width = HandModel::kImageSize;
				ci.height = HandModel::kImageSize;
				ci.faceCount = 1;
				ci.arraySize = 1;
				ci.mipCount = 1;
				XrResult res = xrCreateSwapchain(XRSession::GetSession(), &ci, &hc.swapchain);
				if (XR_FAILED(res))
				{
					hc.swapchain = XR_NULL_HANDLE;
					return fail("xrCreateSwapchain", static_cast<int>(res));
				}
				uint32_t count = 0;
				res = xrEnumerateSwapchainImages(hc.swapchain, 0, &count, nullptr);
				if (XR_FAILED(res) || count == 0)
					return fail("xrEnumerateSwapchainImages", static_cast<int>(res));
				hc.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
				res = xrEnumerateSwapchainImages(hc.swapchain, count, &count,
					reinterpret_cast<XrSwapchainImageBaseHeader*>(hc.images.data()));
				if (XR_FAILED(res))
					return fail("xrEnumerateSwapchainImages", static_cast<int>(res));
				hc.ever_released = false;
				hc.wait_pending = false;
				hc.release_pending = false;
				hc.pending_submitted = false;
				image_count = count;
			}
			Console.WriteLn("(VR) Hands: impostor swapchains created (%d x %ux%u, %u images, %llu-byte staging; "
							"orientation from the %s pose%s).",
				kHandSlots, HandModel::kImageSize, HandModel::kImageSize, image_count, static_cast<unsigned long long>(kBytes),
				grip_orient ? "grip" : "aim", fake ? "; PCSX2_VR_FAKE_HANDS test hands" : "");
			return true;
		}

		void RecordHandUpload(VkCommandBuffer cmd, int slot, VkImage dst)
		{
			ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkBufferImageCopy region = {};
			region.bufferOffset = kHandImageBytes * static_cast<VkDeviceSize>(slot);
			region.bufferRowLength = 0;
			region.bufferImageHeight = 0;
			region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			region.imageOffset = {0, 0, 0};
			region.imageExtent = {HandModel::kImageSize, HandModel::kImageSize, 1};
			vkCmdCopyBufferToImage(cmd, s.hand_staging, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
		}

		// First-person hands. Each presented frame, while camera.lookAt drives the game camera and a
		// controller's grip pose is valid, its hand is drawn on the CPU as seen from the head onto a
		// quad that faces the head, uploaded, and submitted after the screen and lever layers. The quad
		// is flat at the hand's depth, so the runtime's reprojection keeps it in place as the head moves.
		u32 BuildHandLayers(XrCompositionLayerQuad* hand_quads, const XrCompositionLayerBaseHeader** layers,
			u32& layer_count, u32 layer_capacity)
		{
			// PCSX2_VR_FAKE_HANDS (any value): both hands held at fixed offsets from the head and shown
			// even outside first person, to check them on a headset without controllers (SteamVR's null
			// driver). The value "gun" puts the pistol in the right hand.
			static const int s_fake_hands = [] {
				const char* v = std::getenv("PCSX2_VR_FAKE_HANDS");
				if (!v)
					return 0;
				return (std::strcmp(v, "gun") == 0) ? 2 : 1;
			}();
			// PCSX2_VR_HANDS_GRIP_ORIENT: orient the hands by the grip pose instead of the aim pose.
			static const bool s_grip_orient = (std::getenv("PCSX2_VR_HANDS_GRIP_ORIENT") != nullptr);
			// Hands are optional: never hold the frame long for one of their images.
			constexpr int64_t kHandWaitNs = kSwapchainWaitNs;

			if (s.hands_disabled || !s.head_pose_valid)
				return 0;
			const bool fake = (s_fake_hands != 0);
			if (!fake && (!s.head_position_valid || !CameraDriver::LookAtActive()))
				return 0;

			// The head pose was located in XRSession::GetSpace() at this frame's display time, the same
			// space and time XRInput::Update just located the controllers in.
			const float eye[3] = {s.head_position_valid ? s.head_pose.position.x : 0.0f,
				s.head_position_valid ? s.head_pose.position.y : 0.0f, s.head_position_valid ? s.head_pose.position.z : 0.0f};
			const float head_quat[4] = {s.head_pose.orientation.x, s.head_pose.orientation.y, s.head_pose.orientation.z,
				s.head_pose.orientation.w};

			HandModel::HandState hs[kHandSlots];
			if (fake)
			{
				for (int i = 0; i < kHandSlots; i++)
					hs[i] = HandModel::FakeHandState(i, eye, head_quat, s_fake_hands == 2);
			}
			else
			{
				const VRInputSnapshot snap = GetInputSnapshot();
				if (snap.generation == 0 || !snap.actions_active)
					return 0;
				const bool gun = HandModel::GunHeld();
				for (int i = 0; i < kHandSlots; i++)
				{
					const auto& src = snap.hands[i];
					if (!src.grip_pose.valid)
						continue;
					// Position from the grip pose (inside the handle, where the palm closes). Orientation from
					// the aim pose, whose -Z is the controller's pointing direction on every runtime; the grip
					// pose's own -Z runs up the handle at an angle that differs per controller.
					const auto& orient = (!s_grip_orient && src.aim_pose.valid) ? src.aim_pose : src.grip_pose;
					HandModel::HandState& d = hs[i];
					d.valid = true;
					for (int k = 0; k < 3; k++)
						d.pos[k] = src.grip_pose.position_xyz[k];
					for (int k = 0; k < 4; k++)
						d.quat[k] = orient.orientation_xyzw[k];
					d.grip = src.grip;
					d.trigger = src.trigger;
					d.gun = gun && (i == HandModel::kRight);
				}
			}
			if (!hs[0].valid && !hs[1].valid)
				return 0;
			if (!EnsureHandResources(fake, s_grip_orient))
				return 0;

			HandModel::ImpostorQuad iq[kHandSlots];
			bool want[kHandSlots] = {false, false};
			for (int i = 0; i < kHandSlots; i++)
				want[i] = hs[i].valid && HandModel::PlaceImpostor(i, hs[i], eye, &iq[i]);

			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			static std::vector<u32> s_hand_pixels;
			bool recording = false;
			int pending[kHandSlots] = {0, 0};
			int pending_count = 0;
			bool uploaded[kHandSlots] = {false, false};
			for (int i = 0; i < kHandSlots; i++)
			{
				if (!want[i])
					continue;
				auto& hc = s.hand_chains[i];
				// Headset rate: an image whose release never happened (runtime marked lost) is left to teardown.
				if (hc.release_pending)
					continue;
				uint32_t index = 0;
				if (hc.wait_pending)
				{
					index = hc.pending_index;
				}
				else
				{
					if (XR_FAILED(LockedAcquire(hc.swapchain, &index)))
						continue;
					hc.pending_index = index;
					hc.wait_pending = true;
				}
				XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
				wi.timeout = kHandWaitNs;
				// XR_TIMEOUT_EXPIRED is a success code: the image is not ours yet, so wait again next frame.
				const XrResult wr = xrWaitSwapchainImage(hc.swapchain, &wi);
				if (wr == XR_TIMEOUT_EXPIRED || XR_FAILED(wr))
					continue;
				hc.wait_pending = false;

				if (!recording)
				{
					// The staging buffer is shared by both hands: the previous upload must be done with it.
					if (s.hand_fence_submitted)
					{
						if (vkWaitForFences(h.device, 1, &s.hand_fence, VK_TRUE, ONE_SECOND_NS) != VK_SUCCESS)
						{
							// Headset rate: released with the publish (ReleaseAndPublish), never between it and the pacer.
							if (s.headset_rate)
							{
								hc.release_pending = true;
								hc.pending_submitted = false;
							}
							else
							{
								LockedRelease(hc.swapchain);
							}
							break;
						}
						vkResetFences(h.device, 1, &s.hand_fence);
						s.hand_fence_submitted = false;
					}
					VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
					bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
					if (vkBeginCommandBuffer(s.hand_cmd, &bi) != VK_SUCCESS)
					{
						if (s.headset_rate)
						{
							hc.release_pending = true;
							hc.pending_submitted = false;
						}
						else
						{
							LockedRelease(hc.swapchain);
						}
						break;
					}
					recording = true;
				}
				HandModel::RenderImpostor(i, hs[i], eye, iq[i].centre, iq[i].right, iq[i].up, iq[i].size_m, s_hand_pixels);
				if (s_hand_pixels.size() * sizeof(u32) == static_cast<size_t>(kHandImageBytes))
				{
					std::memcpy(static_cast<u8*>(s.hand_staging_map) + kHandImageBytes * static_cast<VkDeviceSize>(i),
						s_hand_pixels.data(), static_cast<size_t>(kHandImageBytes));
				}
				RecordHandUpload(s.hand_cmd, i, hc.images[index].image);
				pending[pending_count] = i;
				pending_count++;
			}
			if (recording)
			{
				bool submitted = false;
				if (vkEndCommandBuffer(s.hand_cmd) == VK_SUCCESS)
				{
					VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
					si.commandBufferCount = 1;
					si.pCommandBuffers = &s.hand_cmd;
					submitted = (LockedQueueSubmit(h.queue, si, s.hand_fence) == VK_SUCCESS);
					s.hand_fence_submitted = submitted;
				}
				for (int k = 0; k < pending_count; k++)
				{
					auto& hc = s.hand_chains[pending[k]];
					if (s.headset_rate)
					{
						// Released with the publish (ReleaseAndPublish); a failed release drops the hand's layer there.
						hc.release_pending = true;
						hc.pending_submitted = submitted;
						uploaded[pending[k]] = submitted;
						continue;
					}
					if (XR_SUCCEEDED(LockedRelease(hc.swapchain)) && submitted)
					{
						hc.ever_released = true;
						uploaded[pending[k]] = true;
					}
				}
			}

			// Only hands drawn this frame: last frame's image on this frame's quad would be from the wrong place.
			u32 appended = 0;
			for (int i = 0; i < kHandSlots; i++)
			{
				if (!uploaded[i] || layer_count >= layer_capacity)
					continue;
				XrCompositionLayerQuad& quad = hand_quads[i];
				quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
				quad.next = nullptr;
				quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
				quad.space = XRSession::GetSpace();
				quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				quad.subImage = {s.hand_chains[i].swapchain,
					{{0, 0}, {static_cast<s32>(HandModel::kImageSize), static_cast<s32>(HandModel::kImageSize)}}, 0};
				quad.pose.orientation = {iq[i].quat[0], iq[i].quat[1], iq[i].quat[2], iq[i].quat[3]};
				quad.pose.position = {iq[i].centre[0], iq[i].centre[1], iq[i].centre[2]};
				quad.size = {iq[i].size_m, iq[i].size_m};
				layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
				appended++;
			}
			return appended;
		}

		// HUD cards. Each card's image size, and where its pixels sit in the shared staging buffer.
		struct HudCardSize
		{
			u32 width;
			u32 height;
		};

		HudCardSize HudSize(int card)
		{
			if (card == HudCards::kToast)
				return {HudCards::kToastWidth, HudCards::kToastHeight};
			if (card == HudCards::kWrist)
				return {HudCards::kWristWidth, HudCards::kWristHeight};
			if (card == HudCards::kBlink)
				return {HudCards::kBlinkImageSize, HudCards::kBlinkImageSize};
			if (card == HudCards::kLaser)
				return {HudCards::kLaserImageWidth, HudCards::kLaserImageHeight};
			if (card == HudCards::kVignette)
				return {HudCards::kVignetteImageSize, HudCards::kVignetteImageSize};
			return {0, 0};
		}

		VkDeviceSize HudImageBytes(int card)
		{
			const HudCardSize sz = HudSize(card);
			return static_cast<VkDeviceSize>(sz.width) * sz.height * 4u;
		}

		VkDeviceSize HudStagingOffset(int card)
		{
			VkDeviceSize offset = 0;
			for (int i = 0; i < card; i++)
				offset += HudImageBytes(i);
			return offset;
		}

		// Opacity is uploaded in this many steps: a card that holds still is never uploaded again, and a
		// 0.15 s fade costs at most one upload a frame.
		constexpr int kHudLevels = 32;

		int HudLevel(float opacity)
		{
			const float o = std::isfinite(opacity) ? std::clamp(opacity, 0.0f, 1.0f) : 0.0f;
			return std::clamp(static_cast<int>(std::lround(o * static_cast<float>(kHudLevels))), 0, kHudLevels);
		}

		// Full-opacity rasters of the cards, redrawn only when a card's content changes; the fades scale
		// their alpha on the way into the staging buffer.
		struct HudRaster
		{
			std::vector<u32> pixels;
			u64 content = 0;
			bool valid = false;
		};
		HudRaster s_hud_raster[HudCards::kCardCount];

		void DestroyHudResources()
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			if (h.IsValid() && s.hud_fence != VK_NULL_HANDLE && s.hud_fence_submitted)
			{
				if (vkWaitForFences(h.device, 1, &s.hud_fence, VK_TRUE, kTeardownFenceNs) == VK_TIMEOUT)
					Console.Warning("(VR) HUD: upload fence still busy after 250 ms during teardown.");
				vkResetFences(h.device, 1, &s.hud_fence);
			}
			s.hud_fence_submitted = false;
			for (auto& hc : s.hud_chains)
			{
				if (hc.swapchain != XR_NULL_HANDLE)
				{
					DestroySwapchainSynced(hc.swapchain);
					hc.swapchain = XR_NULL_HANDLE;
				}
				hc.images.clear();
				hc.ever_released = false;
				hc.wait_pending = false;
				hc.pending_index = 0;
				hc.shown_content = 0;
				hc.shown_level = -1;
			}
			if (h.IsValid())
			{
				if (s.hud_staging_map && s.hud_staging_memory != VK_NULL_HANDLE)
					vkUnmapMemory(h.device, s.hud_staging_memory);
				if (s.hud_staging != VK_NULL_HANDLE)
					vkDestroyBuffer(h.device, s.hud_staging, nullptr);
				if (s.hud_staging_memory != VK_NULL_HANDLE)
					vkFreeMemory(h.device, s.hud_staging_memory, nullptr);
				if (s.hud_fence != VK_NULL_HANDLE)
					vkDestroyFence(h.device, s.hud_fence, nullptr);
				if (s.hud_cmd != VK_NULL_HANDLE && s.cmd_pool != VK_NULL_HANDLE)
					vkFreeCommandBuffers(h.device, s.cmd_pool, 1, &s.hud_cmd);
			}
			s.hud_staging_map = nullptr;
			s.hud_staging = VK_NULL_HANDLE;
			s.hud_staging_memory = VK_NULL_HANDLE;
			s.hud_fence = VK_NULL_HANDLE;
			s.hud_cmd = VK_NULL_HANDLE;
			for (HudRaster& r : s_hud_raster)
				r.valid = false;
		}

		bool EnsureHudResources()
		{
			if (s.hud_disabled)
				return false;
			bool chains_ok = true;
			for (const auto& hc : s.hud_chains)
				chains_ok = chains_ok && (hc.swapchain != XR_NULL_HANDLE);
			if (chains_ok && s.hud_staging != VK_NULL_HANDLE && s.hud_cmd != VK_NULL_HANDLE && s.hud_fence != VK_NULL_HANDLE)
				return true;
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			if (!h.IsValid() || s.cmd_pool == VK_NULL_HANDLE)
				return false;
			const auto fail = [&](const char* what, int code) {
				s.hud_disabled = true;
				Console.Error("(VR) HUD: %s failed (%d); the in-headset cards are off for this session.", what, code);
				DestroyHudResources();
				return false;
			};

			const VkDeviceSize bytes = HudStagingOffset(HudCards::kCardCount);
			if (s.hud_staging == VK_NULL_HANDLE)
			{
				VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
				bci.size = bytes;
				bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
				bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
				VkResult vr = vkCreateBuffer(h.device, &bci, nullptr, &s.hud_staging);
				if (vr != VK_SUCCESS)
				{
					s.hud_staging = VK_NULL_HANDLE;
					return fail("vkCreateBuffer", static_cast<int>(vr));
				}

				VkMemoryRequirements mr = {};
				vkGetBufferMemoryRequirements(h.device, s.hud_staging, &mr);
				VkPhysicalDeviceMemoryProperties mp = {};
				vkGetPhysicalDeviceMemoryProperties(h.physical_device, &mp);
				constexpr VkMemoryPropertyFlags kWant = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
				u32 type = UINT32_MAX;
				for (u32 i = 0; i < mp.memoryTypeCount; i++)
				{
					if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & kWant) == kWant)
					{
						type = i;
						break;
					}
				}
				if (type == UINT32_MAX)
					return fail("host-visible memory type lookup", 0);
				VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
				mai.allocationSize = mr.size;
				mai.memoryTypeIndex = type;
				vr = vkAllocateMemory(h.device, &mai, nullptr, &s.hud_staging_memory);
				if (vr != VK_SUCCESS)
				{
					s.hud_staging_memory = VK_NULL_HANDLE;
					return fail("vkAllocateMemory", static_cast<int>(vr));
				}
				vr = vkBindBufferMemory(h.device, s.hud_staging, s.hud_staging_memory, 0);
				if (vr != VK_SUCCESS)
					return fail("vkBindBufferMemory", static_cast<int>(vr));
				vr = vkMapMemory(h.device, s.hud_staging_memory, 0, VK_WHOLE_SIZE, 0, &s.hud_staging_map);
				if (vr != VK_SUCCESS)
				{
					s.hud_staging_map = nullptr;
					return fail("vkMapMemory", static_cast<int>(vr));
				}
			}

			if (s.hud_cmd == VK_NULL_HANDLE)
			{
				VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
				cai.commandPool = s.cmd_pool;
				cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
				cai.commandBufferCount = 1;
				const VkResult vr = vkAllocateCommandBuffers(h.device, &cai, &s.hud_cmd);
				if (vr != VK_SUCCESS)
				{
					s.hud_cmd = VK_NULL_HANDLE;
					return fail("vkAllocateCommandBuffers", static_cast<int>(vr));
				}
			}
			if (s.hud_fence == VK_NULL_HANDLE)
			{
				VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
				const VkResult vr = vkCreateFence(h.device, &fci, nullptr, &s.hud_fence);
				if (vr != VK_SUCCESS)
				{
					s.hud_fence = VK_NULL_HANDLE;
					return fail("vkCreateFence", static_cast<int>(vr));
				}
				s.hud_fence_submitted = false;
			}

			uint32_t image_count = 0;
			for (int i = 0; i < HudCards::kCardCount; i++)
			{
				auto& hc = s.hud_chains[i];
				if (hc.swapchain != XR_NULL_HANDLE)
					continue;
				// Same format as the screen (sRGB normally): the cards are sRGB-encoded bytes with alpha.
				const HudCardSize sz = HudSize(i);
				XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
				ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
				ci.format = static_cast<int64_t>(s.swapchain_format);
				ci.sampleCount = 1;
				ci.width = sz.width;
				ci.height = sz.height;
				ci.faceCount = 1;
				ci.arraySize = 1;
				ci.mipCount = 1;
				XrResult res = xrCreateSwapchain(XRSession::GetSession(), &ci, &hc.swapchain);
				if (XR_FAILED(res))
				{
					hc.swapchain = XR_NULL_HANDLE;
					return fail("xrCreateSwapchain", static_cast<int>(res));
				}
				uint32_t count = 0;
				res = xrEnumerateSwapchainImages(hc.swapchain, 0, &count, nullptr);
				if (XR_FAILED(res) || count == 0)
					return fail("xrEnumerateSwapchainImages", static_cast<int>(res));
				hc.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
				res = xrEnumerateSwapchainImages(hc.swapchain, count, &count,
					reinterpret_cast<XrSwapchainImageBaseHeader*>(hc.images.data()));
				if (XR_FAILED(res))
					return fail("xrEnumerateSwapchainImages", static_cast<int>(res));
				hc.ever_released = false;
				hc.wait_pending = false;
				hc.shown_content = 0;
				hc.shown_level = -1;
				image_count = count;
			}
			Console.WriteLn("(VR) HUD: card swapchains created (%d card(s), %u images, %llu-byte staging).",
				HudCards::kCardCount, image_count, static_cast<unsigned long long>(bytes));
			return true;
		}

		void RecordHudUpload(VkCommandBuffer cmd, int card, VkImage dst)
		{
			const HudCardSize sz = HudSize(card);
			ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkBufferImageCopy region = {};
			region.bufferOffset = HudStagingOffset(card);
			region.bufferRowLength = 0;
			region.bufferImageHeight = 0;
			region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			region.imageOffset = {0, 0, 0};
			region.imageExtent = {sz.width, sz.height, 1};
			vkCmdCopyBufferToImage(cmd, s.hud_staging, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
		}

		// HUD cards, submitted after everything else so they draw on top, except the laser sight: it goes in at
		// under_from, under the layers appended from there on (the hands, nearer the eye than the beam). A card
		// is rastered at full opacity when its content changes and uploaded only when its content or fade step
		// does; between uploads the runtime keeps showing the last released image.
		u32 BuildHudLayers(XrCompositionLayerQuad* hud_quads, const XrCompositionLayerBaseHeader** layers,
			u32& layer_count, u32 layer_capacity, u32 under_from)
		{
			HudCards::MaybeTestToast();
			HudCards::MaybeTestBlink();
			if (s.hud_disabled)
				return 0;

			struct Want
			{
				bool on = false;
				u64 content = 0;
				int level = 0;
			};
			Want want[HudCards::kCardCount];

			// Toast: view-locked, so it needs the VIEW space.
			HudCards::ToastView toast;
			if (s.view_space != XR_NULL_HANDLE && HudCards::CurrentToast(&toast))
			{
				Want& wt = want[HudCards::kToast];
				wt.on = true;
				wt.content = toast.serial;
				wt.level = HudLevel(toast.opacity);
			}

			// Comfort blink: view-locked too. One unchanging image; only its fade step is uploaded again.
			float blink_opacity = 0.0f;
			if (s.view_space != XR_NULL_HANDLE && HudCards::CurrentBlink(&blink_opacity))
			{
				Want& wb = want[HudCards::kBlink];
				wb.on = true;
				wb.content = 1;
				wb.level = HudLevel(blink_opacity);
			}

			// Comfort vignette: view-locked like the blink, closing in on the edges of the view while first person
			// moves the view without the head (a smooth stick turn, walking): the VignetteStrength setting times the
			// motion CameraDriver publishes, already eased in and out there. Snap turns publish none (the blink
			// covers them), and it goes at once with first person (paused, a menu, a door, a cutscene: LookAtActive
			// lapses) or tracking. PCSX2_VR_VIGNETTE_TEST ramps the motion up and down every 2 s at any time. One
			// unchanging image; only its fade step is uploaded again.
			const float vignette_strength = HudCards::VignetteStrength();
			float vignette_motion = 0.0f;
			const bool vignette_test = HudCards::TestVignetteMotion(&vignette_motion);
			if (!vignette_test)
				vignette_motion = CameraDriver::LookAtActive() ? CameraDriver::ComfortMotion() : 0.0f;
			const float vignette_opacity = vignette_strength * vignette_motion;
			if (s.view_space != XR_NULL_HANDLE && s.head_pose_valid && vignette_opacity > 0.0f)
			{
				Want& wvg = want[HudCards::kVignette];
				wvg.on = true;
				wvg.content = 1;
				wvg.level = HudLevel(vignette_opacity);
				if (!s.hud_vignette_logged && wvg.level > 0)
				{
					s.hud_vignette_logged = true;
					Console.WriteLn("(VR) HUD: comfort vignette shown (%s, strength %.2f).",
						vignette_test ? "PCSX2_VR_VIGNETTE_TEST" : "first person moving",
						static_cast<double>(vignette_strength));
				}
			}

			// Wrist card: on the left hand while first person is active and the profile reads hud.wrist,
			// fading in while its face points at the head. PCSX2_VR_FAKE_HANDS shows it on the fake left
			// hand whatever the angle (turned to the eye; NO DATA without first person), so it can be
			// checked on a headset without controllers.
			static const bool s_fake_hands = (std::getenv("PCSX2_VR_FAKE_HANDS") != nullptr);
			static const bool s_grip_orient = (std::getenv("PCSX2_VR_HANDS_GRIP_ORIENT") != nullptr);
			const u64 now_ms = CameraDriver::SteadyNowMs();
			const float dt = (s.hud_last_ms != 0 && now_ms > s.hud_last_ms) ?
			                     std::min(static_cast<float>(now_ms - s.hud_last_ms) / 1000.0f, 0.1f) :
			                     0.0f;
			s.hud_last_ms = now_ms;
			HudCards::WristData wrist;
			const bool wrist_live = HudCards::GetWrist(&wrist) && CameraDriver::LookAtActive();
			const bool wrist_no_data = !wrist_live;
			bool wrist_placed = false;
			float wrist_pos[3] = {0.0f, 0.0f, 0.0f};
			float wrist_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
			float wrist_facing = -1.0f;
			if ((wrist_live || s_fake_hands) && s.head_pose_valid)
			{
				const float eye[3] = {s.head_position_valid ? s.head_pose.position.x : 0.0f,
					s.head_position_valid ? s.head_pose.position.y : 0.0f, s.head_position_valid ? s.head_pose.position.z : 0.0f};
				if (s_fake_hands)
				{
					const float head_quat[4] = {s.head_pose.orientation.x, s.head_pose.orientation.y, s.head_pose.orientation.z,
						s.head_pose.orientation.w};
					const HandModel::HandState fake = HandModel::FakeHandState(HandModel::kLeft, eye, head_quat, false);
					if (fake.valid)
					{
						HudCards::PlaceWrist(fake.pos, fake.quat, eye, true, wrist_pos, wrist_quat, &wrist_facing);
						wrist_placed = true;
					}
				}
				else if (s.head_position_valid)
				{
					// The pose the left hand is drawn with (BuildHandLayers): grip position, aim orientation.
					const VRInputSnapshot snap = GetInputSnapshot();
					const auto& src = snap.hands[HandModel::kLeft];
					if (snap.generation != 0 && snap.actions_active && src.grip_pose.valid)
					{
						const auto& orient = (!s_grip_orient && src.aim_pose.valid) ? src.aim_pose : src.grip_pose;
						const float hand_pos[3] = {src.grip_pose.position_xyz[0], src.grip_pose.position_xyz[1],
							src.grip_pose.position_xyz[2]};
						const float hand_quat[4] = {orient.orientation_xyzw[0], orient.orientation_xyzw[1],
							orient.orientation_xyzw[2], orient.orientation_xyzw[3]};
						HudCards::PlaceWrist(hand_pos, hand_quat, eye, false, wrist_pos, wrist_quat, &wrist_facing);
						wrist_placed = true;
					}
				}
			}
			// Facing with hysteresis, then a fade toward it; with no hand to hang it on, the card is gone.
			if (!wrist_placed)
				s.hud_wrist_facing = false;
			else if (s_fake_hands)
				s.hud_wrist_facing = true;
			else
				s.hud_wrist_facing = wrist_facing > (s.hud_wrist_facing ? HudCards::kWristHideCos : HudCards::kWristShowCos);
			const float fade_step = dt / HudCards::kFadeSeconds;
			if (!wrist_placed)
				s.hud_wrist_opacity = 0.0f;
			else if (s.hud_wrist_facing)
				s.hud_wrist_opacity = std::min(1.0f, s.hud_wrist_opacity + fade_step);
			else
				s.hud_wrist_opacity = std::max(0.0f, s.hud_wrist_opacity - fade_step);
			if (wrist_placed && s.hud_wrist_opacity > 0.0f)
			{
				Want& ww = want[HudCards::kWrist];
				ww.on = true;
				ww.content = HudCards::WristKey(wrist, wrist_no_data);
				ww.level = HudLevel(s.hud_wrist_opacity);
				if (!s.hud_wrist_logged)
				{
					s.hud_wrist_logged = true;
					Console.WriteLn("(VR) HUD: wrist card shown (%s).",
						s_fake_hands ? "PCSX2_VR_FAKE_HANDS test hand, any angle" : "left wrist turned to the face");
				}
			}

			// Laser sight: from the pistol's muzzle along the right hand's aim while the weapon is raised in first
			// person, with the LaserSight setting on; PCSX2_VR_FAKE_HANDS=gun shows it on the fake right hand
			// whatever the game is doing (the other fake hands hold no pistol, so they get no beam). Placed each
			// frame from the pose the right hand is drawn with (BuildHandLayers: grip position, aim orientation),
			// so it needs no new image as it moves: one unchanging image, uploaded again only while it fades in.
			// It fades in with the raised weapon and goes at once with it, as the pistol leaves the hand.
			static const bool s_fake_gun = [] {
				const char* v = std::getenv("PCSX2_VR_FAKE_HANDS");
				return v && std::strcmp(v, "gun") == 0;
			}();
			const bool laser_raised = s_fake_hands ?
			                              s_fake_gun :
			                              (HandModel::GunHeld() && CameraDriver::LookAtActive() && s.head_position_valid);
			const bool laser_on = HudCards::LaserSightEnabled() && s.head_pose_valid && laser_raised;
			bool laser_hand = false;
			bool laser_placed = false;
			float laser_pos[3] = {0.0f, 0.0f, 0.0f};
			float laser_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
			if (laser_on)
			{
				const float eye[3] = {s.head_position_valid ? s.head_pose.position.x : 0.0f,
					s.head_position_valid ? s.head_pose.position.y : 0.0f, s.head_position_valid ? s.head_pose.position.z : 0.0f};
				float hand_pos[3] = {0.0f, 0.0f, 0.0f};
				float hand_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
				if (s_fake_gun)
				{
					const float head_quat[4] = {s.head_pose.orientation.x, s.head_pose.orientation.y, s.head_pose.orientation.z,
						s.head_pose.orientation.w};
					const HandModel::HandState fake = HandModel::FakeHandState(HandModel::kRight, eye, head_quat, true);
					if (fake.valid)
					{
						for (int k = 0; k < 3; k++)
							hand_pos[k] = fake.pos[k];
						for (int k = 0; k < 4; k++)
							hand_quat[k] = fake.quat[k];
						laser_hand = true;
					}
				}
				else
				{
					const VRInputSnapshot snap = GetInputSnapshot();
					const auto& src = snap.hands[HandModel::kRight];
					if (snap.generation != 0 && snap.actions_active && src.grip_pose.valid)
					{
						const auto& orient = (!s_grip_orient && src.aim_pose.valid) ? src.aim_pose : src.grip_pose;
						for (int k = 0; k < 3; k++)
							hand_pos[k] = src.grip_pose.position_xyz[k];
						for (int k = 0; k < 4; k++)
							hand_quat[k] = orient.orientation_xyzw[k];
						laser_hand = true;
					}
				}
				// Looking straight down the beam no side of the quad faces the eye: that frame goes without it.
				laser_placed = laser_hand && HudCards::PlaceLaser(hand_pos, hand_quat, eye, laser_pos, laser_quat);
			}
			if (!laser_hand)
				s.hud_laser_opacity = 0.0f;
			else
				s.hud_laser_opacity = std::min(1.0f, s.hud_laser_opacity + fade_step);
			if (laser_placed && s.hud_laser_opacity > 0.0f)
			{
				Want& wl = want[HudCards::kLaser];
				wl.on = true;
				wl.content = 1;
				wl.level = HudLevel(s.hud_laser_opacity);
				if (!s.hud_laser_logged)
				{
					s.hud_laser_logged = true;
					Console.WriteLn("(VR) HUD: laser sight shown (%s).",
						s_fake_gun ? "PCSX2_VR_FAKE_HANDS=gun test hand" : "weapon raised in first person");
				}
			}

			bool any = false;
			for (const Want& wv : want)
				any = any || (wv.on && wv.level > 0);
			if (!any || !EnsureHudResources())
				return 0;

			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			bool recording = false;
			int pending[HudCards::kCardCount] = {};
			int pending_count = 0;
			for (int i = 0; i < HudCards::kCardCount; i++)
			{
				const Want& wv = want[i];
				auto& hc = s.hud_chains[i];
				if (!wv.on || wv.level <= 0)
					continue;
				if (hc.ever_released && hc.shown_content == wv.content && hc.shown_level == wv.level)
					continue;

				HudRaster& raster = s_hud_raster[i];
				if (!raster.valid || raster.content != wv.content)
				{
					if (i == HudCards::kToast)
						HudCards::RasterToast(raster.pixels, toast.text);
					else if (i == HudCards::kWrist)
						HudCards::RasterWrist(raster.pixels, wrist, wrist_no_data);
					else if (i == HudCards::kBlink)
						HudCards::RasterBlink(raster.pixels);
					else if (i == HudCards::kLaser)
						HudCards::RasterLaser(raster.pixels);
					else if (i == HudCards::kVignette)
						HudCards::RasterVignette(raster.pixels);
					raster.content = wv.content;
					raster.valid = true;
				}
				const HudCardSize sz = HudSize(i);
				const size_t pixel_count = static_cast<size_t>(sz.width) * sz.height;
				if (raster.pixels.size() != pixel_count)
					continue;

				uint32_t index = 0;
				if (hc.wait_pending)
				{
					index = hc.pending_index;
				}
				else
				{
					if (XR_FAILED(LockedAcquire(hc.swapchain, &index)))
						continue;
					hc.pending_index = index;
					hc.wait_pending = true;
				}
				XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
				wi.timeout = kSwapchainWaitNs;
				// XR_TIMEOUT_EXPIRED is a success code: the image is not ours yet, so wait again next frame.
				const XrResult wr = xrWaitSwapchainImage(hc.swapchain, &wi);
				if (wr == XR_TIMEOUT_EXPIRED || XR_FAILED(wr))
					continue;
				hc.wait_pending = false;

				if (!recording)
				{
					// The staging buffer is shared by the cards: the previous upload must be done with it.
					if (s.hud_fence_submitted)
					{
						if (vkWaitForFences(h.device, 1, &s.hud_fence, VK_TRUE, ONE_SECOND_NS) != VK_SUCCESS)
						{
							LockedRelease(hc.swapchain);
							break;
						}
						vkResetFences(h.device, 1, &s.hud_fence);
						s.hud_fence_submitted = false;
					}
					VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
					bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
					if (vkBeginCommandBuffer(s.hud_cmd, &bi) != VK_SUCCESS)
					{
						LockedRelease(hc.swapchain);
						break;
					}
					recording = true;
				}
				u32* dst = reinterpret_cast<u32*>(static_cast<u8*>(s.hud_staging_map) + HudStagingOffset(i));
				HudCards::ScaleAlpha(raster.pixels.data(), pixel_count,
					static_cast<float>(wv.level) / static_cast<float>(kHudLevels), dst);
				RecordHudUpload(s.hud_cmd, i, hc.images[index].image);
				pending[pending_count] = i;
				pending_count++;
			}
			if (recording)
			{
				bool submitted = false;
				if (vkEndCommandBuffer(s.hud_cmd) == VK_SUCCESS)
				{
					VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
					si.commandBufferCount = 1;
					si.pCommandBuffers = &s.hud_cmd;
					submitted = (LockedQueueSubmit(h.queue, si, s.hud_fence) == VK_SUCCESS);
					s.hud_fence_submitted = submitted;
				}
				for (int k = 0; k < pending_count; k++)
				{
					const int card = pending[k];
					auto& hc = s.hud_chains[card];
					if (XR_SUCCEEDED(LockedRelease(hc.swapchain)) && submitted)
					{
						hc.ever_released = true;
						hc.shown_content = want[card].content;
						hc.shown_level = want[card].level;
					}
				}
			}

			u32 appended = 0;
			for (int i = 0; i < HudCards::kCardCount; i++)
			{
				const Want& wv = want[i];
				const auto& hc = s.hud_chains[i];
				if (!wv.on || wv.level <= 0 || !hc.ever_released || layer_count >= layer_capacity)
					continue;
				// The toast's image must hold this toast's text (the previous one's would be wrong); being a
				// fade step behind is fine.
				if (i == HudCards::kToast && hc.shown_content != wv.content)
					continue;
				const HudCardSize sz = HudSize(i);
				XrCompositionLayerQuad& quad = hud_quads[i];
				quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
				quad.next = nullptr;
				quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
				quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				quad.subImage = {hc.swapchain, {{0, 0}, {static_cast<s32>(sz.width), static_cast<s32>(sz.height)}}, 0};
				if (i == HudCards::kToast)
				{
					float pos[3];
					float q[4];
					HudCards::ToastPose(pos, q);
					quad.space = s.view_space;
					quad.pose.orientation = {q[0], q[1], q[2], q[3]};
					quad.pose.position = {pos[0], pos[1], pos[2]};
					quad.size = {HudCards::kToastWidthM, HudCards::kToastHeightM};
				}
				else if (i == HudCards::kWrist)
				{
					// In the base space, where the hands were located for this frame's display time.
					quad.space = XRSession::GetSpace();
					quad.pose.orientation = {wrist_quat[0], wrist_quat[1], wrist_quat[2], wrist_quat[3]};
					quad.pose.position = {wrist_pos[0], wrist_pos[1], wrist_pos[2]};
					quad.size = {HudCards::kWristWidthM, HudCards::kWristHeightM};
				}
				else if (i == HudCards::kBlink)
				{
					// Just in front of the eyes and far wider than any field of view, so it covers the whole view.
					float pos[3];
					float q[4];
					HudCards::BlinkPose(pos, q);
					quad.space = s.view_space;
					quad.pose.orientation = {q[0], q[1], q[2], q[3]};
					quad.pose.position = {pos[0], pos[1], pos[2]};
					quad.size = {HudCards::kBlinkSizeM, HudCards::kBlinkSizeM};
				}
				else if (i == HudCards::kLaser)
				{
					// In the base space with the hands, its long side along the beam (+Y toward the far end).
					quad.space = XRSession::GetSpace();
					quad.pose.orientation = {laser_quat[0], laser_quat[1], laser_quat[2], laser_quat[3]};
					quad.pose.position = {laser_pos[0], laser_pos[1], laser_pos[2]};
					quad.size = {HudCards::kLaserWidthM, HudCards::kLaserLengthM};
				}
				else if (i == HudCards::kVignette)
				{
					// Just ahead of the eyes, its ring centred on the line of sight and its opaque border past any
					// field of view.
					float pos[3];
					float q[4];
					HudCards::VignettePose(pos, q);
					quad.space = s.view_space;
					quad.pose.orientation = {q[0], q[1], q[2], q[3]};
					quad.pose.position = {pos[0], pos[1], pos[2]};
					quad.size = {HudCards::kVignetteSizeM, HudCards::kVignetteSizeM};
				}
				const XrCompositionLayerBaseHeader* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
				if (i == HudCards::kLaser && under_from < layer_count)
				{
					// In under the hands: the layers from under_from move up one (layer_count < capacity, checked above).
					for (u32 k = layer_count; k > under_from; k--)
						layers[k] = layers[k - 1];
					layers[under_from] = layer;
					layer_count++;
				}
				else
				{
					layers[layer_count++] = layer;
				}
				appended++;
			}
			return appended;
		}

		float ComputeAspect()
		{
			switch (GSConfig.AspectRatio)
			{
				case AspectRatioType::R4_3:
				case AspectRatioType::RAuto4_3_3_2:
					return 4.0f / 3.0f;
				case AspectRatioType::R16_9:
					return 16.0f / 9.0f;
				default:
					return (s.swapchain_height > 0)
							   ? static_cast<float>(s.swapchain_width) / static_cast<float>(s.swapchain_height)
							   : 4.0f / 3.0f;
			}
		}

		// OpenXR yaw (CCW positive) of a quaternion's forward (-Z) direction, as the first-person placement uses.
		float ForwardYaw(float x, float y, float z, float w)
		{
			const float fx = -2.0f * (x * z + w * y);
			const float fz = -(1.0f - 2.0f * (x * x + y * y));
			return std::atan2(-fx, -fz);
		}

		// PCSX2_VR_SYNCLOG: how far the first-person frame's yaw is from where the head is as it is shown,
		// which is how wide the black band on a turn's leading edge gets. The head is the one the camera
		// follows: the env fake head (evaluated now), else PINE's scripted head, else the headset.
		// Logged once a second as RMS and max, with the horizon lookAt.predict leads the head by.
		void LogEdgeError(float placed_yaw)
		{
			static const bool s_edgelog = (std::getenv("PCSX2_VR_SYNCLOG") != nullptr);
			if (!s_edgelog)
				return;
			const u64 now = CameraDriver::SteadyNowMs();
			float head_yaw = 0.0f;
			if (!CameraDriver::FakeHeadYawAt(now, &head_yaw))
			{
				HeadPose::Snapshot test_head;
				if (XRInput::TestHeadPose(&test_head))
					head_yaw = ForwardYaw(test_head.orientation_x, test_head.orientation_y, test_head.orientation_z,
						test_head.orientation_w);
				else
					head_yaw = ForwardYaw(s.head_pose.orientation.x, s.head_pose.orientation.y,
						s.head_pose.orientation.z, s.head_pose.orientation.w);
			}
			const float err_deg = std::remainder(placed_yaw - head_yaw, 2.0f * 3.14159265f) * (180.0f / 3.14159265f);

			static u64 s_edge_log_ms = 0;
			static double s_edge_sq = 0.0;
			static float s_edge_max = 0.0f;
			static u32 s_edge_n = 0;
			if (s_edge_log_ms == 0)
				s_edge_log_ms = now;
			if (std::isfinite(err_deg))
			{
				s_edge_sq += static_cast<double>(err_deg) * static_cast<double>(err_deg);
				s_edge_max = std::max(s_edge_max, std::abs(err_deg));
				s_edge_n++;
			}
			if (now - s_edge_log_ms >= 1000)
			{
				if (s_edge_n > 0)
				{
					Console.WriteLn("(VR) edge: rms %.2f deg max %.2f deg, H %.0f ms",
						std::sqrt(s_edge_sq / static_cast<double>(s_edge_n)), s_edge_max,
						CameraDriver::HeadPredictionHorizon() * 1000.0f);
				}
				s_edge_log_ms = now;
				s_edge_sq = 0.0;
				s_edge_max = 0.0f;
				s_edge_n = 0;
			}
		}

		s64 SteadyNowNs()
		{
			return static_cast<s64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		// Now on the display clock: the steady clock moved by the lead the latest waited frame had from its wait's
		// return to its predicted display time. 0 until the pacer has waited a frame.
		XrTime DisplayClockNow()
		{
			if (s.latest_display_time.load(std::memory_order_acquire) == 0)
				return 0;
			return static_cast<XrTime>(SteadyNowNs() + s.display_clock_offset_ns.load(std::memory_order_acquire));
		}

		// Frame-call results that mean the runtime is gone end VR at once (VRManager tears it down on the GS thread and
		// play goes on flat); kMaxRuntimeFailures XR_ERROR_RUNTIME_FAILURE results in a row count as gone too.
		void NoteFrameCallResult(XrResult res, const char* what)
		{
			if (res == XR_ERROR_SESSION_LOST || res == XR_ERROR_INSTANCE_LOST)
			{
				if (!XRSession::IsLost())
					Console.Error("(VR) %s: the session or the runtime is lost (%d).", what, static_cast<int>(res));
				XRSession::MarkLost();
				s.runtime_failures.store(0, std::memory_order_relaxed);
			}
			else if (res == XR_ERROR_RUNTIME_FAILURE)
			{
				const u32 failures = s.runtime_failures.fetch_add(1, std::memory_order_relaxed) + 1;
				if (failures >= kMaxRuntimeFailures)
				{
					Console.Error("(VR) %s: XR_ERROR_RUNTIME_FAILURE %u times in a row; treating the runtime as lost.", what,
						failures);
					XRSession::MarkLost();
					s.runtime_failures.store(0, std::memory_order_relaxed);
				}
			}
			else if (XR_SUCCEEDED(res))
			{
				s.runtime_failures.store(0, std::memory_order_relaxed);
			}
		}

		// PCSX2_VR_XRFAULT: a comma list of offline test faults, parsed at Initialize and refused while the network
		// adapter is on. begin1: one xrBeginFrame reads XR_ERROR_RUNTIME_FAILURE (the owed-begin recovery). lost@S:
		// from S seconds into the session the next xrBeginFrame reads XR_ERROR_SESSION_LOST. hangwait@S: the pacer
		// blocks forever instead of calling xrWaitFrame (the detach path). exit@S: the session is told to stop, then
		// to exit, as the runtime would.
		struct XrFaults
		{
			bool begin1 = false;
			float lost_s = -1.0f;
			float hangwait_s = -1.0f;
			float exit_s = -1.0f;
			bool lost_fired = false;
			bool exit_fired = false;
		};
		XrFaults s_faults;

		void ParseFaults()
		{
			s_faults = XrFaults();
			s_xrfault_begin1_fired = false;
			const char* env = std::getenv("PCSX2_VR_XRFAULT");
			if (!env || !*env)
				return;
			if (EmuConfig.DEV9.EthEnable)
			{
				Console.Warning("(VR) PCSX2_VR_XRFAULT=%s refused: the network adapter is on (fault injection is offline only).", env);
				return;
			}
			const std::string list(env);
			size_t pos = 0;
			while (pos <= list.size())
			{
				size_t comma = list.find(',', pos);
				if (comma == std::string::npos)
					comma = list.size();
				const std::string token = list.substr(pos, comma - pos);
				pos = comma + 1;
				if (token.empty())
					continue;
				const size_t at = token.find('@');
				const std::string name = token.substr(0, at);
				float seconds = -1.0f;
				if (at != std::string::npos)
				{
					const std::string number = token.substr(at + 1);
					char* end = nullptr;
					const double v = std::strtod(number.c_str(), &end);
					if (end != number.c_str() && *end == '\0' && std::isfinite(v) && v >= 0.0)
						seconds = static_cast<float>(v);
				}
				if (name == "begin1" && at == std::string::npos)
					s_faults.begin1 = true;
				else if (name == "lost" && seconds >= 0.0f)
					s_faults.lost_s = seconds;
				else if (name == "hangwait" && seconds >= 0.0f)
					s_faults.hangwait_s = seconds;
				else if (name == "exit" && seconds >= 0.0f)
					s_faults.exit_s = seconds;
				else
					Console.Warning("(VR) PCSX2_VR_XRFAULT: '%s' not understood (begin1, lost@S, hangwait@S, exit@S); ignored.",
						token.c_str());
			}
			Console.Warning("(VR) PCSX2_VR_XRFAULT active (offline test): begin1 %s, lost@ %.1f s, hangwait@ %.1f s, exit@ %.1f s "
							"(-1 = off).",
				s_faults.begin1 ? "on" : "off", s_faults.lost_s, s_faults.hangwait_s, s_faults.exit_s);
		}

		// Seconds since the pacer first saw this run of the session (it began), or -1 before that.
		float SessionRunSeconds()
		{
			const u64 began = s.session_run_began_ms.load(std::memory_order_acquire);
			if (began == 0)
				return -1.0f;
			const u64 now = CameraDriver::SteadyNowMs();
			return (now > began) ? static_cast<float>(now - began) / 1000.0f : 0.0f;
		}

		// xrBeginFrame (under the queue lock) with the begin1 and lost@ faults played in front of it.
		XrResult BeginFrameWithFaults(XrSession session)
		{
			if (s_faults.begin1 && !s_xrfault_begin1_fired)
			{
				s_xrfault_begin1_fired = true;
				Console.Warning("(VR) XRFAULT=begin1: injecting one xrBeginFrame failure to exercise owed-begin recovery.");
				return XR_ERROR_RUNTIME_FAILURE;
			}
			if (s_faults.lost_s >= 0.0f && !s_faults.lost_fired)
			{
				const float t = SessionRunSeconds();
				if (t >= s_faults.lost_s)
				{
					s_faults.lost_fired = true;
					Console.Warning("(VR) XRFAULT=lost: xrBeginFrame reads XR_ERROR_SESSION_LOST %.1f s into the session.", t);
					return XR_ERROR_SESSION_LOST;
				}
			}
			return LockedBeginFrame(session);
		}

		// The pacer's own faults. True when the pacer should go round again (exit@ just played out).
		bool MaybeInjectPacerFault()
		{
			const float t = SessionRunSeconds();
			if (t < 0.0f)
				return false;
			if (s_faults.hangwait_s >= 0.0f && t >= s_faults.hangwait_s)
			{
				Console.Warning("(VR) XRFAULT=hangwait: the pacer now blocks forever instead of calling xrWaitFrame "
								"(exercises the detach path).");
				std::unique_lock<std::mutex> lock(s.hang_mutex);
				s.hang_cv.wait(lock, [] { return false; });
				return true;
			}
			if (s_faults.exit_s >= 0.0f && !s_faults.exit_fired && t >= s_faults.exit_s)
			{
				s_faults.exit_fired = true;
				Console.Warning("(VR) XRFAULT=exit: the session is told to stop, then to exit, %.1f s into it.", t);
				XRSession::InjectStateForTest(XR_SESSION_STATE_STOPPING);
				XRSession::InjectStateForTest(XR_SESSION_STATE_EXITING);
				return true;
			}
			return false;
		}

		// The head (the VIEW space in the base space) at display time t, with its angular velocity, published for
		// CameraDriver (HeadPose::Publish, or Invalidate when it is not tracked). False when not located.
		bool LocateAndPublishHead(XrTime t, XrPosef* pose, bool* position_valid)
		{
			// The head's angular velocity rides along on the same locate, for camera.lookAt head prediction.
			XrSpaceVelocity vel = {XR_TYPE_SPACE_VELOCITY};
			XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
			loc.next = &vel;
			if (XR_FAILED(xrLocateSpace(s.view_space, XRSession::GetSpace(), t, &loc)) ||
				!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
			{
				HeadPose::Invalidate();
				return false;
			}

			HeadPose::Snapshot p;
			p.orientation_x = loc.pose.orientation.x;
			p.orientation_y = loc.pose.orientation.y;
			p.orientation_z = loc.pose.orientation.z;
			p.orientation_w = loc.pose.orientation.w;
			if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
			{
				p.position_x = loc.pose.position.x;
				p.position_y = loc.pose.position.y;
				p.position_z = loc.pose.position.z;
				p.position_valid = true;
			}
			// Expressed in the base space (the room), which is the frame the prediction rotates in.
			const bool angular = (vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0;
			if (angular && std::isfinite(vel.angularVelocity.x) && std::isfinite(vel.angularVelocity.y) &&
				std::isfinite(vel.angularVelocity.z))
			{
				p.angular_velocity[0] = vel.angularVelocity.x;
				p.angular_velocity[1] = vel.angularVelocity.y;
				p.angular_velocity[2] = vel.angularVelocity.z;
				p.angular_valid = true;
			}
			// Once: whether this runtime reports the head's velocity (SteamVR's null driver reports zeros).
			static bool s_velocity_logged = false;
			if (!s_velocity_logged)
			{
				s_velocity_logged = true;
				Console.WriteLn("(VR) Head pose: runtime velocityFlags 0x%llX (angular %s, linear %s), angular (%.3f, %.3f, %.3f) rad/s.",
					static_cast<unsigned long long>(vel.velocityFlags), angular ? "valid" : "not valid",
					(vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) ? "valid" : "not valid",
					vel.angularVelocity.x, vel.angularVelocity.y, vel.angularVelocity.z);
			}
			p.valid = true;
			HeadPose::Publish(p);
			*pose = loc.pose;
			*position_valid = p.position_valid;
			return true;
		}

		// The Eyes line (first located, then on an IPD change over 0.5 mm or a field-of-view change over 0.5 deg) and
		// GetEyeInfo, from both views as the runtime located them.
		void UpdateEyeInfo(const XrView* views, bool position_valid)
		{
			constexpr float kDeg = 180.0f / 3.14159265f;
			EyeInfo now;
			now.valid = true;
			if (position_valid)
			{
				const float dx = views[1].pose.position.x - views[0].pose.position.x;
				const float dy = views[1].pose.position.y - views[0].pose.position.y;
				const float dz = views[1].pose.position.z - views[0].pose.position.z;
				now.ipd_m = std::sqrt(dx * dx + dy * dy + dz * dz);
			}
			for (int eye = 0; eye < 2; eye++)
			{
				now.fov_deg[eye][0] = views[eye].fov.angleLeft * kDeg;
				now.fov_deg[eye][1] = views[eye].fov.angleRight * kDeg;
				now.fov_deg[eye][2] = views[eye].fov.angleUp * kDeg;
				now.fov_deg[eye][3] = views[eye].fov.angleDown * kDeg;
			}
			const XrDuration period = s.display_period_ns.load(std::memory_order_acquire);
			now.display_hz = (period > 0) ? static_cast<float>(1.0e9 / static_cast<double>(period)) : 0.0f;

			bool changed = false;
			{
				std::lock_guard<std::mutex> lock(s_eye_mutex);
				changed = !s_eye_logged.valid || std::abs(now.ipd_m - s_eye_logged.ipd_m) > 0.0005f;
				for (int eye = 0; eye < 2 && !changed; eye++)
				{
					for (int k = 0; k < 4 && !changed; k++)
						changed = std::abs(now.fov_deg[eye][k] - s_eye_logged.fov_deg[eye][k]) > 0.5f;
				}
				s_eye_info = now;
				if (changed)
					s_eye_logged = now;
			}
			if (!changed)
				return;

			const float union_h = std::max(std::max(-now.fov_deg[0][0], -now.fov_deg[1][0]),
				std::max(now.fov_deg[0][1], now.fov_deg[1][1]));
			const float union_up = std::max(now.fov_deg[0][2], now.fov_deg[1][2]);
			const float union_down = std::max(-now.fov_deg[0][3], -now.fov_deg[1][3]);
			Console.WriteLn("(VR) Eyes: IPD %.1f mm; left fov L/R/U/D %.1f/%.1f/%.1f/%.1f, right %.1f/%.1f/%.1f/%.1f; "
							"union H +-%.1f V +%.1f/-%.1f deg; display %.1f Hz",
				now.ipd_m * 1000.0f, now.fov_deg[0][0], now.fov_deg[0][1], now.fov_deg[0][2], now.fov_deg[0][3],
				now.fov_deg[1][0], now.fov_deg[1][1], now.fov_deg[1][2], now.fov_deg[1][3], union_h, union_up, union_down,
				now.display_hz);
		}

		// xrLocateViews for the background layer and the Eyes line. True when both views have a valid orientation.
		bool LocateEyeViews(XrTime t, XrView* views)
		{
			XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
			li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			li.displayTime = t;
			li.space = XRSession::GetSpace();
			XrViewState vs = {XR_TYPE_VIEW_STATE};
			views[0] = XrView{XR_TYPE_VIEW};
			views[1] = XrView{XR_TYPE_VIEW};
			uint32_t count = 0;
			if (XR_FAILED(xrLocateViews(XRSession::GetSession(), &li, &vs, 2, &count, views)) || count != 2 ||
				!(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
			{
				return false;
			}
			UpdateEyeInfo(views, (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0);
			return true;
		}

		// The black background under every other layer: a projection layer over both eyes' views that always shows
		// the one image cleared at Initialize.
		void MakeBackgroundLayer(const XrView* views, XrCompositionLayerProjectionView* pv, XrCompositionLayerProjection* layer)
		{
			for (u32 i = 0; i < 2; i++)
			{
				pv[i] = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
				pv[i].pose = views[i].pose;
				pv[i].fov = views[i].fov;
				pv[i].subImage.swapchain = s.bg_swapchain;
				pv[i].subImage.imageRect.offset = {0, 0};
				pv[i].subImage.imageRect.extent = {static_cast<s32>(kBackgroundSize), static_cast<s32>(kBackgroundSize)};
				pv[i].subImage.imageArrayIndex = 0;
			}
			*layer = XrCompositionLayerProjection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
			layer->layerFlags = 0;
			layer->space = XRSession::GetSpace();
			layer->viewCount = 2;
			layer->views = pv;
			if (!s.bg_logged)
			{
				s.bg_logged = true;
				Console.WriteLn("(VR) Background: opaque black projection layer under the screens.");
			}
		}

		void DestroyBackground()
		{
			if (s.bg_swapchain != XR_NULL_HANDLE && !s.abandoned.load(std::memory_order_acquire))
				xrDestroySwapchain(s.bg_swapchain);
			s.bg_swapchain = XR_NULL_HANDLE;
			s.bg_ready = false;
		}

		// The background's swapchain: cleared to opaque black here, once, and never touched again (the runtime keeps
		// showing the last released image). Runs before the pacer starts. Any failure is logged and the frames go
		// without it.
		void CreateBackground()
		{
			s.bg_ready = false;
			s.bg_logged = false;
			if (!s.opaque_bg)
			{
				Console.WriteLn("(VR) Background: off ([VR] OpaqueBackground); the runtime's environment shows around the screens.");
				return;
			}
			const auto fail = [](const char* what, int code) {
				Console.Warning("(VR) Background: %s failed (%d); frames go without the black projection layer.", what, code);
				DestroyBackground();
			};

			XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
			ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
			ci.format = static_cast<int64_t>(s.swapchain_format);
			ci.sampleCount = 1;
			ci.width = kBackgroundSize;
			ci.height = kBackgroundSize;
			ci.faceCount = 1;
			ci.arraySize = 1;
			ci.mipCount = 1;
			XrResult res = xrCreateSwapchain(XRSession::GetSession(), &ci, &s.bg_swapchain);
			if (XR_FAILED(res))
			{
				s.bg_swapchain = XR_NULL_HANDLE;
				fail("xrCreateSwapchain", static_cast<int>(res));
				return;
			}
			uint32_t count = 0;
			res = xrEnumerateSwapchainImages(s.bg_swapchain, 0, &count, nullptr);
			if (XR_FAILED(res) || count == 0)
			{
				fail("xrEnumerateSwapchainImages", static_cast<int>(res));
				return;
			}
			std::vector<XrSwapchainImageVulkan2KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
			res = xrEnumerateSwapchainImages(s.bg_swapchain, count, &count,
				reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
			if (XR_FAILED(res))
			{
				fail("xrEnumerateSwapchainImages", static_cast<int>(res));
				return;
			}
			uint32_t index = 0;
			res = LockedAcquire(s.bg_swapchain, &index);
			if (XR_FAILED(res) || index >= count)
			{
				fail("xrAcquireSwapchainImage", static_cast<int>(res));
				return;
			}
			XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wi.timeout = ONE_SECOND_NS;
			res = xrWaitSwapchainImage(s.bg_swapchain, &wi);
			if (res != XR_SUCCESS)
			{
				// Not released: an image whose wait never completed is not ours to release.
				fail("xrWaitSwapchainImage", static_cast<int>(res));
				return;
			}

			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			const VkCommandBuffer cmd = s.cmd_buffers[0];
			const VkImage image = images[index].image;
			VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			VkResult vr = vkBeginCommandBuffer(cmd, &bi);
			if (vr == VK_SUCCESS)
			{
				ImageBarrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
					VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
				VkClearColorValue black = {};
				black.float32[3] = 1.0f;
				const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
				vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
				ImageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
					VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
				vr = vkEndCommandBuffer(cmd);
			}
			bool cleared = false;
			if (vr == VK_SUCCESS)
			{
				VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
				si.commandBufferCount = 1;
				si.pCommandBuffers = &cmd;
				vr = LockedQueueSubmit(h.queue, si, s.fences[0]);
				if (vr == VK_SUCCESS)
				{
					vr = vkWaitForFences(h.device, 1, &s.fences[0], VK_TRUE, ONE_SECOND_NS);
					if (vr == VK_SUCCESS)
					{
						vkResetFences(h.device, 1, &s.fences[0]);
						cleared = true;
					}
					else
					{
						// Still in flight: the first copy on this command buffer waits for it.
						s.fence_submitted[0] = true;
					}
				}
			}
			res = LockedRelease(s.bg_swapchain);
			if (!cleared)
			{
				fail("the clear", static_cast<int>(vr));
				return;
			}
			if (XR_FAILED(res))
			{
				fail("xrReleaseSwapchainImage", static_cast<int>(res));
				return;
			}
			s.bg_ready = true;
			Console.WriteLn("(VR) Background: %ux%u swapchain cleared to opaque black (%u images).", kBackgroundSize,
				kBackgroundSize, count);
		}

		// The runtime's recommended per-eye image size, for the headset's pixel density in the first-person line.
		void QueryViewConfiguration()
		{
			s.view_rec_width[0] = s.view_rec_width[1] = 0;
			uint32_t count = 0;
			if (XR_FAILED(xrEnumerateViewConfigurationViews(XRSession::GetInstance(), XRSession::GetSystemId(),
					XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr)) ||
				count == 0)
			{
				return;
			}
			std::vector<XrViewConfigurationView> views(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
			if (XR_FAILED(xrEnumerateViewConfigurationViews(XRSession::GetInstance(), XRSession::GetSystemId(),
					XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, views.data())))
			{
				return;
			}
			for (uint32_t i = 0; i < count && i < 2; i++)
				s.view_rec_width[i] = views[i].recommendedImageRectWidth;
			Console.WriteLn("(VR) Headset eye images: recommended %ux%u per eye.", views[0].recommendedImageRectWidth,
				views[0].recommendedImageRectHeight);
		}

		// When the first-person screen turns on: how much of the view it covers against the headset's field of view,
		// and its pixel density at the centre against the headset's.
		void LogFirstPersonScreen(const ScreenParams& sp)
		{
			constexpr float kDeg = 180.0f / 3.14159265f;
			const float d = std::max(sp.fp_distance, 0.01f);
			const float aspect = ComputeAspect();
			const bool curved = (sp.fp_arc_deg >= 5.0f) && XRSession::HasCylinderLayer();
			float width_m = 0.0f;
			float height_m = 0.0f;
			float half_w_deg = 0.0f;
			if (curved)
			{
				width_m = d * sp.fp_arc_deg / kDeg;
				height_m = width_m / std::max(aspect, 0.01f);
				half_w_deg = 0.5f * sp.fp_arc_deg;
			}
			else
			{
				height_m = sp.fp_height;
				width_m = height_m * aspect;
				half_w_deg = std::atan(0.5f * width_m / d) * kDeg;
			}
			const float half_h_deg = std::atan(0.5f * height_m / d) * kDeg;
			const u32 w_px = s.swapchain_width;
			const u32 h_px = s.swapchain_height;
			const float px_deg_h = curved ? (static_cast<float>(w_px) / std::max(sp.fp_arc_deg, 1.0f)) :
			                                (static_cast<float>(w_px) * d / std::max(width_m, 0.001f) / kDeg);
			const float px_deg_v = static_cast<float>(h_px) * d / std::max(height_m, 0.001f) / kDeg;

			const EyeInfo eyes = GetEyeInfo();
			if (!eyes.valid)
			{
				Console.WriteLn("(VR) First-person screen: +-%.1f x +-%.1f deg at %.2f m (%.2f x %.2f m); headset field of "
								"view not known yet; centre %.1f x %.1f px/deg from %ux%u",
					half_w_deg, half_h_deg, d, width_m, height_m, px_deg_h, px_deg_v, w_px, h_px);
				return;
			}
			const float union_h = std::max(std::max(-eyes.fov_deg[0][0], -eyes.fov_deg[1][0]),
				std::max(eyes.fov_deg[0][1], eyes.fov_deg[1][1]));
			const float union_up = std::max(eyes.fov_deg[0][2], eyes.fov_deg[1][2]);
			const float union_down = std::max(-eyes.fov_deg[0][3], -eyes.fov_deg[1][3]);
			// The headset at its recommended size: width / (tan right + tan -left) pixels per radian at the centre.
			float headset_px_deg = 0.0f;
			int headset_eyes = 0;
			for (int eye = 0; eye < 2; eye++)
			{
				const float span = std::tan(eyes.fov_deg[eye][1] / kDeg) + std::tan(-eyes.fov_deg[eye][0] / kDeg);
				if (s.view_rec_width[eye] > 0 && span > 0.01f)
				{
					headset_px_deg += static_cast<float>(s.view_rec_width[eye]) / span / kDeg;
					headset_eyes++;
				}
			}
			if (headset_eyes > 0)
				headset_px_deg /= static_cast<float>(headset_eyes);
			Console.WriteLn("(VR) First-person screen: +-%.1f x +-%.1f deg at %.2f m (%.2f x %.2f m); margin vs headset: "
							"yaw %+.1f up %+.1f down %+.1f deg; centre %.1f x %.1f px/deg from %ux%u; headset about %.1f px/deg",
				half_w_deg, half_h_deg, d, width_m, height_m, half_w_deg - union_h, half_h_deg - union_up,
				half_h_deg - union_down, px_deg_h, px_deg_v, w_px, h_px, headset_px_deg);
		}

		// Which game frame a render pose belongs to (its head pose's publish stamp and serial), for the sync log.
		u64 ImageKey(const HeadPose::Snapshot& pose)
		{
			const u64 key = (pose.publish_ms << 16) ^ (pose.frame & 0xFFFFu);
			return (key != 0) ? key : 1;
		}

		// Where the layers of one present live, to tell each layer's role by the array it points into.
		struct LayerSources
		{
			const XrCompositionLayerQuad* quads;
			const XrCompositionLayerCylinderKHR* cyls;
			u32 screen_slots;
			const XrCompositionLayerQuad* lever_quads;
			const XrCompositionLayerQuad* hand_quads;
			const XrCompositionLayerQuad* hud_quads;
		};

		// Copies each pointed-to layer by value, in order, with its role.
		void FillPublishedSet(PublishedSet* out, const XrCompositionLayerBaseHeader* const* layers, u32 layer_count,
			const LayerSources& src)
		{
			out->count = 0;
			for (u32 i = 0; i < layer_count && out->count < kLayerCapacity; i++)
			{
				const void* const p = layers[i];
				PublishedSet::Entry& e = out->e[out->count];
				bool known = false;
				for (u32 k = 0; k < src.screen_slots && !known; k++)
				{
					if (p == static_cast<const void*>(&src.quads[k]))
					{
						e.kind = kKindQuad;
						e.role = kRoleScreen;
						e.quad = src.quads[k];
						known = true;
					}
					else if (p == static_cast<const void*>(&src.cyls[k]))
					{
						e.kind = kKindCylinder;
						e.role = kRoleScreen;
						e.cyl = src.cyls[k];
						known = true;
					}
				}
				for (int k = 0; k < ControlQuads::kSlots && !known; k++)
				{
					if (p == static_cast<const void*>(&src.lever_quads[k]))
					{
						e.kind = kKindQuad;
						e.role = kRoleLever;
						e.quad = src.lever_quads[k];
						known = true;
					}
				}
				for (int k = 0; k < kHandSlots && !known; k++)
				{
					if (p == static_cast<const void*>(&src.hand_quads[k]))
					{
						e.kind = kKindQuad;
						e.role = kRoleHand;
						e.quad = src.hand_quads[k];
						known = true;
					}
				}
				for (int k = 0; k < HudCards::kCardCount && !known; k++)
				{
					if (p == static_cast<const void*>(&src.hud_quads[k]))
					{
						e.kind = kKindQuad;
						e.role = (k == HudCards::kLaser) ? kRoleHudLaser : (k == HudCards::kWrist) ? kRoleHudWrist : kRoleHudView;
						e.quad = src.hud_quads[k];
						known = true;
					}
				}
				if (known)
					out->count++;
			}
		}

		// Headset rate: hands the layers this present built to the pacer, copied by value. keep_screen: the screen
		// images were not all replaced this present (a copy skipped or timed out, or none made), so the previous
		// set's Screen entries stay and each image keeps the pose it was rendered from instead of jumping to this
		// frame's.
		void PublishLayers(const XrCompositionLayerBaseHeader* const* layers, u32 layer_count, const LayerSources& src,
			bool keep_screen, bool fp_screen, const XrVector3f& fp_head, u64 fp_image)
		{
			PublishedSet next = {};
			FillPublishedSet(&next, layers, layer_count, src);
			const float turn_rate = CameraDriver::ScreenTurnRate();
			next.publish_ms = CameraDriver::SteadyNowMs();
			next.publish_display_time = DisplayClockNow();
			next.fp_screen = fp_screen;
			next.fp_head = fp_head;
			next.turn_rate = fp_screen ? turn_rate : 0.0f;
			next.fp_image = fp_image;
			s.stat_gs_frames.fetch_add(1, std::memory_order_relaxed);

			std::lock_guard<std::mutex> lock(s.publish_mutex);
			const PublishedSet& prev = s.published;
			bool prev_screen = false;
			for (u32 i = 0; i < prev.count && !prev_screen; i++)
				prev_screen = (prev.e[i].role == kRoleScreen);
			if (keep_screen && prev_screen)
			{
				PublishedSet merged = {};
				for (u32 i = 0; i < prev.count; i++)
				{
					if (prev.e[i].role == kRoleScreen)
						merged.e[merged.count++] = prev.e[i];
				}
				for (u32 i = 0; i < next.count && merged.count < kLayerCapacity; i++)
				{
					if (next.e[i].role != kRoleScreen)
						merged.e[merged.count++] = next.e[i];
				}
				merged.publish_ms = next.publish_ms;
				merged.publish_display_time = prev.publish_display_time;
				merged.fp_screen = prev.fp_screen;
				merged.fp_head = prev.fp_head;
				merged.turn_rate = prev.fp_screen ? turn_rate : 0.0f;
				merged.fp_image = prev.fp_image;
				merged.serial = prev.serial + 1;
				s.published = merged;
				return;
			}
			next.serial = prev.serial + 1;
			s.published = next;
		}

		// Headset rate: releases the screen and hand images this present copied (left acquired by CopyToSwapchain and
		// BuildHandLayers) and publishes the layers that show them, all under submit_mutex. The runtime shows a
		// swapchain's last released image, so a release before the publish would let the pacer end a frame with the
		// new image at the previous present's pose (and, in layered stereo, one eye new and the other old). Lock order
		// as documented at s.submit_mutex: submit_mutex, then the queue lock (LockedRelease). Bounded like
		// DestroySwapchainSynced: a pacer inside xrEndFrame for over 100 ms means the runtime stopped answering.
		// cl_copy: each eye's CopyResult this present (-1: none tried); a failed release turns Copied into Skipped.
		// chain_was_released: each eye chain's ever_released before this present's copies, which set it on Copied
		// ahead of the release so this present's layers are built; restored when the release fails.
		void ReleaseAndPublish(const XrCompositionLayerBaseHeader* const* layers, u32 layer_count, const LayerSources& src,
			int* cl_copy, const bool* chain_was_released, bool fp_screen, const XrVector3f& fp_head, u64 fp_image)
		{
			std::unique_lock<std::timed_mutex> submit(s.submit_mutex, std::defer_lock);
			if (!submit.try_lock_for(std::chrono::milliseconds(100)))
			{
				// The images stay acquired (release_pending): nothing more is copied into them, and the loss teardown
				// destroys the swapchains (or leaves them alive with the pacer detached).
				Console.Error("(VR) The VR pacer has been inside xrEndFrame for over 100 ms (runtime not answering): "
							  "frame not published, abandoning VR; emulation continues.");
				XRSession::MarkLost();
				return;
			}

			// Swapchains whose release failed and that must not appear in the published set.
			XrSwapchain dropped[2 + kHandSlots] = {};
			u32 dropped_count = 0;
			for (u32 l = 0; l < 2; l++)
			{
				auto& chain = s.chains[l];
				if (!chain.release_pending)
					continue;
				chain.release_pending = false;
				const XrResult res = LockedRelease(chain.swapchain);
				if (XR_SUCCEEDED(res))
					continue;
				if (!s.warned_release)
				{
					s.warned_release = true;
					Console.Error("(VR) xrReleaseSwapchainImage failed (%d).", static_cast<int>(res));
				}
				if (cl_copy[l] == static_cast<int>(CopyResult::Copied))
				{
					cl_copy[l] = static_cast<int>(CopyResult::Skipped);
					s.stat_copy_skipped.fetch_add(1, std::memory_order_relaxed);
				}
				chain.ever_released = chain_was_released[l];
				// A chain never released has no image the runtime could show: no layer may point at it. One released
				// before still shows its previous image, which the kept Screen entries (keep_screen below) go with.
				if (!chain.ever_released)
					dropped[dropped_count++] = chain.swapchain;
			}
			for (int i = 0; i < kHandSlots; i++)
			{
				auto& hc = s.hand_chains[i];
				if (!hc.release_pending)
					continue;
				hc.release_pending = false;
				const XrResult res = LockedRelease(hc.swapchain);
				if (XR_SUCCEEDED(res))
				{
					if (hc.pending_submitted)
						hc.ever_released = true;
				}
				else
				{
					// A hand's layer is only ever this present's image at this present's pose: drop it.
					dropped[dropped_count++] = hc.swapchain;
				}
				hc.pending_submitted = false;
			}

			// The pose stays with its image: unless every screen image was replaced this present (a copy skipped or
			// timed out, a release failed, or none made at all), the previous Screen entries stay, each image at the
			// pose it was rendered from instead of jumping to this frame's.
			bool attempted = false;
			bool all_copied = true;
			for (u32 l = 0; l < 2; l++)
			{
				if (cl_copy[l] < 0)
					continue;
				attempted = true;
				if (cl_copy[l] != static_cast<int>(CopyResult::Copied))
					all_copied = false;
			}
			PublishLayers(layers, layer_count, src, !attempted || !all_copied, fp_screen, fp_head, fp_image);

			if (dropped_count > 0)
			{
				std::lock_guard<std::mutex> publish(s.publish_mutex);
				for (u32 k = 0; k < dropped_count; k++)
					ForgetPublishedSwapchain(dropped[k]);
			}
		}

		// A smooth stick turn between GS frames: a Screen entry turned by a radians of OpenXR yaw about the vertical
		// through head. Left-multiplied by Ry = (0, sin a/2, 0, cos a/2); a cylinder sits at head, so it only turns.
		void ApplyTurn(PublishedSet::Entry& e, const XrVector3f& head, float a)
		{
			const float sh = std::sin(a * 0.5f);
			const float ch = std::cos(a * 0.5f);
			const float sa = std::sin(a);
			const float ca = std::cos(a);
			XrPosef& pose = (e.kind == kKindCylinder) ? e.cyl.pose : e.quad.pose;
			const XrQuaternionf q = pose.orientation;
			pose.orientation = {ch * q.x + sh * q.z, ch * q.y + sh * q.w, ch * q.z - sh * q.x, ch * q.w - sh * q.y};
			const float dx = pose.position.x - head.x;
			const float dz = pose.position.z - head.z;
			pose.position.x = head.x + ca * dx + sa * dz;
			pose.position.z = head.z - sa * dx + ca * dz;
		}

		// The submit array: the background first, then the set's entries (only Screen and Lever while the GS is
		// idle), Screen entries turned by `turn`. Returns the layer count; the pointers point into `set`.
		u32 BuildSubmitArray(PublishedSet& set, const XrCompositionLayerProjection* background, bool stale, float turn,
			const XrCompositionLayerBaseHeader** out)
		{
			u32 n = 0;
			if (background)
				out[n++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(background);
			for (u32 i = 0; i < set.count && n < kLayerCapacity + 1; i++)
			{
				PublishedSet::Entry& e = set.e[i];
				if (stale && e.role != kRoleScreen && e.role != kRoleLever)
					continue;
				if (e.role == kRoleScreen && turn != 0.0f)
					ApplyTurn(e, set.fp_head, turn);
				out[n++] = (e.kind == kKindCylinder) ? reinterpret_cast<const XrCompositionLayerBaseHeader*>(&e.cyl) :
				                                       reinterpret_cast<const XrCompositionLayerBaseHeader*>(&e.quad);
			}
			return n;
		}

		// The Frames line: what reached the runtime over the last 5 s, kept by the thread that ends the frames (the
		// pacer, or the GS thread in legacy mode).
		struct FrameStats
		{
			u64 window_ms = 0;
			u32 submitted = 0;
			u32 resent = 0;
			u32 stale = 0;
			u32 no_render = 0;
			double wait_begin_sum_ms = 0.0;
			u32 wait_begin_n = 0;
			float wait_begin_max_ms = 0.0f;
			float end_max_ms = 0.0f;
			u32 layers = 0;
			u64 last_serial = 0;
		};

		void NoteWaitToBegin(FrameStats& st, s64 wait_ns, s64 begin_ns)
		{
			if (wait_ns == 0 || begin_ns < wait_ns)
				return;
			const float ms = static_cast<float>(static_cast<double>(begin_ns - wait_ns) / 1.0e6);
			st.wait_begin_sum_ms += ms;
			st.wait_begin_n++;
			st.wait_begin_max_ms = std::max(st.wait_begin_max_ms, ms);
		}

		void MaybeLogFrameStats(FrameStats& st)
		{
			const u64 now = CameraDriver::SteadyNowMs();
			if (st.window_ms == 0)
			{
				st.window_ms = now;
				return;
			}
			if (now - st.window_ms < 5000)
				return;
			const double secs = static_cast<double>(now - st.window_ms) / 1000.0;
			const XrDuration period = s.display_period_ns.load(std::memory_order_acquire);
			const double hz = (period > 0) ? 1.0e9 / static_cast<double>(period) : 0.0;
			const u32 gs_frames = s.stat_gs_frames.exchange(0, std::memory_order_relaxed);
			const u32 skipped = s.stat_copy_skipped.exchange(0, std::memory_order_relaxed);
			const u32 timed_out = s.stat_copy_timeout.exchange(0, std::memory_order_relaxed);
			Console.WriteLn("(VR) Frames: submitted %.1f/s (display %.1f Hz), new GS frames %.1f/s, re-sent %.1f/s, stale %u, "
							"shouldRender=false %u, wait->begin mean %.1f max %.1f ms, xrEndFrame max %.1f ms, queue-lock wait "
							"max %u us, copies skipped %u timed out %u, layers %u, mode %s",
				st.submitted / secs, hz, gs_frames / secs, st.resent / secs, st.stale, st.no_render,
				(st.wait_begin_n > 0) ? (st.wait_begin_sum_ms / st.wait_begin_n) : 0.0, st.wait_begin_max_ms, st.end_max_ms,
				VR::TakeXrQueueLockMaxWaitUs(), skipped, timed_out, st.layers,
				s.headset_rate ? "headset rate (VR pacer)" : "legacy (one per GS present)");
			const u64 last_serial = st.last_serial;
			st = FrameStats();
			st.window_ms = now;
			st.last_serial = last_serial;
		}

		// PCSX2_VR_SYNCLOG: how evenly the first-person screen turns through a smooth stick turn, from the Screen
		// entry yaw actually submitted, per display frame (a display frame that showed the previous submit again, in
		// legacy mode, is a step of 0). Only steps within one game frame count: a new game frame shows its own turn in
		// the image instead.
		void NoteScreenYaw(const PublishedSet& set, XrTime display_time)
		{
			static const bool s_synclog = (std::getenv("PCSX2_VR_SYNCLOG") != nullptr);
			if (!s_synclog)
				return;
			static u64 s_prev_image = 0;
			static float s_prev_yaw = 0.0f;
			static XrTime s_prev_time = 0;
			static double s_sum = 0.0;
			static double s_sq = 0.0;
			static u32 s_n = 0;
			static u64 s_log_ms = 0;

			bool have = false;
			float yaw = 0.0f;
			if (set.fp_screen && set.fp_image != 0)
			{
				for (u32 i = 0; i < set.count; i++)
				{
					const PublishedSet::Entry& e = set.e[i];
					if (e.role != kRoleScreen)
						continue;
					const XrQuaternionf& q = (e.kind == kKindCylinder) ? e.cyl.pose.orientation : e.quad.pose.orientation;
					yaw = ForwardYaw(q.x, q.y, q.z, q.w);
					have = true;
					break;
				}
			}
			const XrDuration period = s.display_period_ns.load(std::memory_order_acquire);
			if (have && s_prev_image == set.fp_image && s_prev_time != 0 && period > 0 && display_time > s_prev_time)
			{
				const double frames_f = static_cast<double>(display_time - s_prev_time) / static_cast<double>(period);
				const int frames = std::clamp(static_cast<int>(std::lround(frames_f)), 1, 8);
				const double step = std::remainder(static_cast<double>(yaw - s_prev_yaw), 2.0 * 3.14159265358979) *
				                    (180.0 / 3.14159265358979);
				s_sum += step;
				s_sq += step * step;
				s_n += static_cast<u32>(frames);
			}
			s_prev_image = have ? set.fp_image : 0;
			s_prev_yaw = yaw;
			s_prev_time = display_time;

			const u64 now = CameraDriver::SteadyNowMs();
			if (s_log_ms == 0)
				s_log_ms = now;
			if (now - s_log_ms < 1000)
				return;
			if (s_n > 0)
			{
				const double mean = s_sum / static_cast<double>(s_n);
				const double var = std::max(0.0, s_sq / static_cast<double>(s_n) - mean * mean);
				Console.WriteLn("(VR) pacer: fp screen yaw step per display frame mean %.2f std %.2f deg over %u frames", mean,
					std::sqrt(var), s_n);
			}
			s_log_ms = now;
			s_sum = 0.0;
			s_sq = 0.0;
			s_n = 0;
		}

		bool PacerDetached(u32 gen)
		{
			return s.pacer_gen.load(std::memory_order_acquire) != gen;
		}

		bool PacerShouldStop(u32 gen)
		{
			return s.pacer_exit.load(std::memory_order_acquire) || PacerDetached(gen);
		}

		// The VR pacer. Headset rate: it owns the frame loop, waiting, beginning and ending a frame every display
		// period with the layers the GS published last (sent again while the GS is idle: loads, offline pauses,
		// stalls), and it pumps the session events. Legacy: it only waits each frame and parks it for EndOfFrame.
		// After every call that can block in the runtime it checks whether Shutdown gave up on it (detached), and then
		// touches nothing more: all it uses lives in the static s, which is never freed.
		void PacerThreadMain(u32 gen)
		{
			Threading::SetNameOfCurrentThread("VR Pacer");

			const XrSession session = XRSession::GetSession();
			const bool headset_rate = s.headset_rate;
			FrameStats stats;
			bool was_running = false;
			bool begin_owed = false;
			XrTime owed_display_time = 0;
			bool warned_begin = false;
			bool warned_end = false;
			u64 next_eye_probe_ms = 0;

			while (!PacerShouldStop(gen))
			{
				s.pacer_heartbeat_ms.store(CameraDriver::SteadyNowMs(), std::memory_order_release);

				// In this mode VRManager leaves the events to the pacer (unless it stalls, PacerStalled), so state
				// changes are handled while the GS is idle.
				if (headset_rate)
					XRSession::PumpEvents();

				// VRManager tears VR down on the GS thread; nothing more to submit.
				if (XRSession::IsLost())
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
					continue;
				}
				if (!XRSession::IsSessionRunning())
				{
					was_running = false;
					begin_owed = false;
					s.session_run_began_ms.store(0, std::memory_order_release);
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					continue;
				}
				if (!was_running)
				{
					was_running = true;
					s.session_run_began_ms.store(CameraDriver::SteadyNowMs(), std::memory_order_release);
				}
				if (MaybeInjectPacerFault())
					continue;

				// A frame waited but never begun would block the next xrWaitFrame for good: begin and end it first.
				if (begin_owed)
				{
					const XrResult res = LockedBeginFrame(session);
					if (PacerDetached(gen))
						return;
					NoteFrameCallResult(res, "xrBeginFrame");
					if (XR_SUCCEEDED(res))
					{
						NoteFrameCallResult(LockedEndFrame(session, owed_display_time, 0, nullptr), "xrEndFrame");
						if (PacerDetached(gen))
							return;
						begin_owed = false;
					}
					else if (res == XR_ERROR_SESSION_NOT_RUNNING)
					{
						begin_owed = false;
					}
					else
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(10));
					}
					continue;
				}

				XrFrameState fs = {XR_TYPE_FRAME_STATE};
				const XrResult wres = xrWaitFrame(session, nullptr, &fs);
				const s64 wait_ns = SteadyNowNs();
				if (PacerShouldStop(gen))
					break;
				s.pacer_heartbeat_ms.store(CameraDriver::SteadyNowMs(), std::memory_order_release);
				if (XR_FAILED(wres))
				{
					if (wres == XR_ERROR_SESSION_NOT_RUNNING)
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(5));
						continue;
					}
					NoteFrameCallResult(wres, "xrWaitFrame");
					if (!s.warned_pacer)
					{
						s.warned_pacer = true;
						Console.Error("(VR) xrWaitFrame failed (%d); pacer parked. Session-loss handling "
									  "arrives via events.",
							static_cast<int>(wres));
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					continue;
				}
				s.latest_display_time.store(fs.predictedDisplayTime, std::memory_order_release);
				s.display_period_ns.store(fs.predictedDisplayPeriod, std::memory_order_release);
				s.display_clock_offset_ns.store(static_cast<s64>(fs.predictedDisplayTime) - wait_ns, std::memory_order_release);
				s.pacer_should_render.store(fs.shouldRender != XR_FALSE, std::memory_order_release);

				if (!headset_rate)
				{
					std::lock_guard<std::mutex> lock(s.frame_mutex);
					s.pending_frame_state = fs;
					s.pending_wait_ns = wait_ns;
					s.has_pending_frame = true;
					continue;
				}

				const s64 begin_ns = SteadyNowNs();
				const XrResult bres = BeginFrameWithFaults(session);
				if (PacerDetached(gen))
					return;
				NoteFrameCallResult(bres, "xrBeginFrame");
				if (XR_FAILED(bres))
				{
					if (bres != XR_ERROR_SESSION_NOT_RUNNING && bres != XR_ERROR_SESSION_LOST && bres != XR_ERROR_INSTANCE_LOST)
					{
						if (!warned_begin)
						{
							warned_begin = true;
							Console.Error("(VR) xrBeginFrame failed (%d); the pacer begins that frame again before waiting "
										  "for another.",
								static_cast<int>(bres));
						}
						begin_owed = true;
						owed_display_time = fs.predictedDisplayTime;
					}
					continue;
				}
				NoteWaitToBegin(stats, wait_ns, begin_ns);

				// Shutdown asked the pacer to leave: this frame is still ended (with nothing in it), then it goes.
				const bool exiting = s.pacer_exit.load(std::memory_order_acquire);
				XrView views[2] = {XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
				bool views_ok = false;
				if (!exiting)
				{
					XRInput::Update(fs.predictedDisplayTime);
					if (PacerDetached(gen))
						return;

					XrPosef head = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
					bool head_valid = false;
					bool head_position_valid = false;
					if (s.view_space != XR_NULL_HANDLE)
						head_valid = LocateAndPublishHead(fs.predictedDisplayTime, &head, &head_position_valid);
					if (PacerDetached(gen))
						return;
					{
						std::lock_guard<std::mutex> lock(s.head_mutex);
						s.pacer_head.pose = head;
						s.pacer_head.valid = head_valid;
						s.pacer_head.position_valid = head_position_valid;
					}

					// Every frame for the background; about once a second without it, for the Eyes line alone.
					const u64 probe_ms = CameraDriver::SteadyNowMs();
					if (s.opaque_bg && s.bg_ready)
					{
						views_ok = LocateEyeViews(fs.predictedDisplayTime, views);
					}
					else if (probe_ms >= next_eye_probe_ms)
					{
						next_eye_probe_ms = probe_ms + 1000;
						LocateEyeViews(fs.predictedDisplayTime, views);
					}
					if (PacerDetached(gen))
						return;
				}

				PublishedSet set = {};
				XrCompositionLayerProjectionView bg_views[2] = {};
				XrCompositionLayerProjection bg = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
				const XrCompositionLayerBaseHeader* submit[kLayerCapacity + 1] = {};
				u32 submit_count = 0;
				bool stale = false;
				XrResult eres = XR_SUCCESS;
				float end_ms = 0.0f;
				{
					std::unique_lock<std::timed_mutex> submit_lock(s.submit_mutex);
					if (!exiting)
					{
						{
							std::lock_guard<std::mutex> lock(s.publish_mutex);
							set = s.published;
						}
						stale = (set.count > 0 && set.publish_ms != 0 && CameraDriver::SteadyNowMs() > set.publish_ms + kStaleMs);
						float turn = 0.0f;
						if (set.fp_screen && set.turn_rate != 0.0f && set.publish_display_time != 0)
						{
							const double dt = std::clamp(
								static_cast<double>(fs.predictedDisplayTime - set.publish_display_time) * 1.0e-9, 0.0, 0.1);
							turn = set.turn_rate * static_cast<float>(dt);
						}
						if (views_ok)
							MakeBackgroundLayer(views, bg_views, &bg);
						submit_count = BuildSubmitArray(set, views_ok ? &bg : nullptr, stale, turn, submit);
					}
					const s64 end_start_ns = SteadyNowNs();
					eres = LockedEndFrame(session, fs.predictedDisplayTime, fs.shouldRender ? submit_count : 0, submit);
					end_ms = static_cast<float>(static_cast<double>(SteadyNowNs() - end_start_ns) / 1.0e6);
				}
				if (PacerDetached(gen))
					return;
				NoteFrameCallResult(eres, "xrEndFrame");
				if (XR_FAILED(eres) && !warned_end)
				{
					warned_end = true;
					Console.Error("(VR) xrEndFrame failed (%d).", static_cast<int>(eres));
				}
				if (exiting)
					break;

				if (XR_SUCCEEDED(eres))
					stats.submitted++;
				if (set.serial != 0 && set.serial == stats.last_serial)
					stats.resent++;
				stats.last_serial = set.serial;
				if (stale)
					stats.stale++;
				if (!fs.shouldRender)
					stats.no_render++;
				stats.end_max_ms = std::max(stats.end_max_ms, end_ms);
				stats.layers = fs.shouldRender ? submit_count : 0;
				if (fs.shouldRender && XR_SUCCEEDED(eres))
					NoteScreenYaw(set, fs.predictedDisplayTime);
				MaybeLogFrameStats(stats);
			}

			if (!PacerDetached(gen))
				s.pacer_done.store(true, std::memory_order_release);
		}

		// Legacy: a begin that failed is retried on the next present before anything else.
		bool SettleOwedBegin(XrSession session)
		{
			if (!s.begin_owed)
				return true;

			const XrResult res = LockedBeginFrame(session);
			NoteFrameCallResult(res, "xrBeginFrame");
			if (res == XR_ERROR_SESSION_NOT_RUNNING)
			{
				s.begin_owed = false;
				return true;
			}
			if (XR_FAILED(res))
				return false;

			XRInput::Update(s.begin_owed_display_time);

			NoteFrameCallResult(LockedEndFrame(session, s.begin_owed_display_time, 0, nullptr), "xrEndFrame");
			s.begin_owed = false;
			return true;
		}

		// Legacy: begins and ends the frame the pacer parked, so its next xrWaitFrame returns and it can exit.
		void DrainOnePacedFrameForShutdown()
		{
			XrFrameState fs;
			{
				std::lock_guard<std::mutex> lock(s.frame_mutex);
				if (!s.has_pending_frame)
					return;
				fs = s.pending_frame_state;
				s.has_pending_frame = false;
			}

			if (!XRSession::HasSession())
				return;

			const XrSession session = XRSession::GetSession();
			if (XR_FAILED(LockedBeginFrame(session)))
				return;
			LockedEndFrame(session, fs.predictedDisplayTime, 0, nullptr);
		}
	}

	bool Initialize()
	{
		if (s.initialized)
			return true;

		// A pacer left stuck in the runtime may still hold the compositor's locks: no new session in this process.
		if (s.abandoned.load(std::memory_order_acquire))
		{
			Console.Error("(VR) Compositor: an earlier VR pacer is still stuck inside the runtime; VR stays off until the "
						  "emulator restarts.");
			return false;
		}

		// [VR] HeadsetRateSubmit and OpaqueBackground, read once (the GS thread, before the session runs).
		s.headset_rate = EmuConfig.VR.HeadsetRateSubmit;
		s.opaque_bg = EmuConfig.VR.OpaqueBackground;

		s.screen_anchor_x = 0.0f;
		s.screen_anchor_y = 0.0f;
		s.screen_anchor_z = 0.0f;
		s.screen_anchor_yaw = 0.0f;
		s.world_anchor_x = 0.0f;
		s.world_anchor_y = 0.0f;
		s.world_anchor_z = 0.0f;
		s.world_anchor_yaw = 0.0f;
		s.head_pose_valid = false;
		s.head_position_valid = false;
		s_reanchor_requested.store(false, std::memory_order_release);
		{
			std::lock_guard<std::mutex> lock(s_screen_mutex);
			s_anchor_snapshot.x = s_anchor_snapshot.y = s_anchor_snapshot.z = s_anchor_snapshot.yaw = 0.0f;
			s_anchor_snapshot.generation++;
		}
		s.warned_lever = false;
		s.lever_layers_logged = -1;
		s.lever_screen_logged = 0;
		s.hands_disabled = false;
		s.hud_disabled = false;
		s.hud_wrist_facing = false;
		s.hud_wrist_opacity = 0.0f;
		s.hud_last_ms = 0;
		s.hud_wrist_logged = false;
		s.hud_laser_opacity = 0.0f;
		s.hud_laser_logged = false;
		s.hud_vignette_logged = false;
		s.fp_switch_due_ms = 0;

		if (!XRSession::HasSession())
		{
			Console.Error("(VR) Compositor Initialize: no XR session.");
			return false;
		}

		const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
		if (!h.IsValid())
		{
			Console.Error("(VR) Compositor Initialize: Vulkan handles are not valid.");
			return false;
		}

		const XrSession session = XRSession::GetSession();

		uint32_t fmt_count = 0;
		if (!CheckXR(xrEnumerateSwapchainFormats(session, 0, &fmt_count, nullptr), "xrEnumerateSwapchainFormats") ||
			fmt_count == 0)
		{
			return false;
		}
		std::vector<int64_t> formats(fmt_count);
		if (!CheckXR(xrEnumerateSwapchainFormats(session, fmt_count, &fmt_count, formats.data()),
				"xrEnumerateSwapchainFormats"))
		{
			return false;
		}

		static const bool s_unorm_experiment = (std::getenv("PCSX2_VR_SWAPCHAIN_UNORM") != nullptr);
		bool has_unorm = false, has_srgb = false;
		for (int64_t f : formats)
		{
			if (f == static_cast<int64_t>(VK_FORMAT_R8G8B8A8_UNORM))
				has_unorm = true;
			else if (f == static_cast<int64_t>(VK_FORMAT_R8G8B8A8_SRGB))
				has_srgb = true;
		}
		if (s_unorm_experiment && has_unorm)
		{
			s.swapchain_format = VK_FORMAT_R8G8B8A8_UNORM;
			Console.Warning("(VR) PCSX2_VR_SWAPCHAIN_UNORM experiment ACTIVE: UNORM swapchain — the runtime "
							"treats the GS's gamma-encoded bytes as linear. Expect a brightness shift; this is "
							"an A/B lane, not a shipping mode.");
		}
		else if (has_srgb)
		{
			s.swapchain_format = VK_FORMAT_R8G8B8A8_SRGB;
			if (s_unorm_experiment)
				Console.Warning("(VR) PCSX2_VR_SWAPCHAIN_UNORM requested, but the runtime offers no "
								"VK_FORMAT_R8G8B8A8_UNORM swapchain — using the sRGB default.");
		}
		else if (has_unorm)
		{
			s.swapchain_format = VK_FORMAT_R8G8B8A8_UNORM;
			Console.Warning("(VR) Runtime offers no VK_FORMAT_R8G8B8A8_SRGB swapchain; using UNORM. "
							"Brightness/gamma may be off on this runtime.");
		}
		else
		{
			std::string offered;
			for (int64_t f : formats)
			{
				offered += std::to_string(f);
				offered += ' ';
			}
			Console.Error("(VR) Runtime offers neither VK_FORMAT_R8G8B8A8_SRGB (%d) nor _UNORM (%d). "
						  "Offered VkFormats: %s. Running flat.",
				static_cast<int>(VK_FORMAT_R8G8B8A8_SRGB), static_cast<int>(VK_FORMAT_R8G8B8A8_UNORM),
				offered.c_str());
			return false;
		}

		VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = h.queue_family;
		if (vkCreateCommandPool(h.device, &pci, nullptr, &s.cmd_pool) != VK_SUCCESS)
		{
			Console.Error("(VR) vkCreateCommandPool failed.");
			s.cmd_pool = VK_NULL_HANDLE;
			return false;
		}

		VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cbai.commandPool = s.cmd_pool;
		cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cbai.commandBufferCount = NUM_CMD_BUFFERS;
		if (vkAllocateCommandBuffers(h.device, &cbai, s.cmd_buffers) != VK_SUCCESS)
		{
			Console.Error("(VR) vkAllocateCommandBuffers failed.");
			vkDestroyCommandPool(h.device, s.cmd_pool, nullptr);
			s.cmd_pool = VK_NULL_HANDLE;
			return false;
		}

		for (u32 i = 0; i < NUM_CMD_BUFFERS; i++)
		{
			VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			if (vkCreateFence(h.device, &fci, nullptr, &s.fences[i]) != VK_SUCCESS)
			{
				Console.Error("(VR) vkCreateFence failed.");
				for (u32 j = 0; j < i; j++)
				{
					vkDestroyFence(h.device, s.fences[j], nullptr);
					s.fences[j] = VK_NULL_HANDLE;
				}
				vkFreeCommandBuffers(h.device, s.cmd_pool, NUM_CMD_BUFFERS, s.cmd_buffers);
				vkDestroyCommandPool(h.device, s.cmd_pool, nullptr);
				s.cmd_pool = VK_NULL_HANDLE;
				return false;
			}
			s.fence_submitted[i] = false;
		}

		for (auto& chain : s.chains)
		{
			chain.swapchain = XR_NULL_HANDLE;
			chain.images.clear();
			chain.ever_released = false;
			chain.wait_pending = false;
			chain.release_pending = false;
		}
		s.swapchain_width = 0;
		s.swapchain_height = 0;
		s.next_cmd_buffer = 0;
		s.begin_owed = false;
		s.begin_owed_display_time = 0;
		s.begin_owed_presents = 0;
		s.runtime_failures.store(0, std::memory_order_relaxed);
		s.layered_logged = -1;
		ParseFaults();

		s.warned_pacer = false;
		s.warned_beginframe = false;
		s.warned_endframe = false;
		s.warned_acquire = false;
		s.warned_wait_timeout = false;
		s.warned_wait_fail = false;
		s.warned_release = false;
		s.warned_fence_timeout = false;
		s.warned_swapchain = false;

		s.pacer_exit.store(false, std::memory_order_release);
		s.pacer_done.store(false, std::memory_order_release);
		{
			std::lock_guard<std::mutex> lock(s.frame_mutex);
			s.has_pending_frame = false;
			s.pending_wait_ns = 0;
		}
		s.latest_display_time.store(0, std::memory_order_release);
		s.display_period_ns.store(0, std::memory_order_release);
		s.display_clock_offset_ns.store(0, std::memory_order_release);
		s.pacer_should_render.store(false, std::memory_order_release);
		s.session_run_began_ms.store(0, std::memory_order_release);
		s.pacer_heartbeat_ms.store(CameraDriver::SteadyNowMs(), std::memory_order_release);
		{
			std::lock_guard<std::mutex> lock(s.head_mutex);
			s.pacer_head = decltype(s.pacer_head)();
		}
		{
			std::lock_guard<std::mutex> lock(s.publish_mutex);
			s.published = PublishedSet{};
		}
		s.stat_gs_frames.store(0, std::memory_order_relaxed);
		s.stat_copy_skipped.store(0, std::memory_order_relaxed);
		s.stat_copy_timeout.store(0, std::memory_order_relaxed);
		{
			std::lock_guard<std::mutex> lock(s_eye_mutex);
			s_eye_info = EyeInfo();
			s_eye_logged = EyeInfo();
		}
		{
			XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
			rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
			rsci.poseInReferenceSpace.orientation.w = 1.0f;
			if (XR_FAILED(xrCreateReferenceSpace(session, &rsci, &s.view_space)))
			{
				s.view_space = XR_NULL_HANDLE;
				Console.Warning("(VR) VIEW reference space creation failed; head-tracked camera will stay inert.");
			}
		}

		CreateBackground();
		QueryViewConfiguration();

		s.initialized = true;
		s.pacer = std::thread(PacerThreadMain, s.pacer_gen.load(std::memory_order_acquire));

		Console.WriteLn("(VR) Frame submission: %s",
			s.headset_rate ? "headset rate (VR pacer thread)" : "once per GS present (legacy)");
		Console.WriteLn("(VR) Compositor initialized (%u copy command buffers; swapchain created on first frame).",
			NUM_CMD_BUFFERS);
		return true;
	}

	void EndOfFrame(GSTexture* current, u32 eye)
	{
		if (!s.initialized)
			return;

		// Headset rate: the VR pacer begins and ends a frame every display period; this present only refreshes the
		// images and publishes the layers built from them (PublishLayers). Legacy: this present begins and ends the
		// frame the pacer waited for.
		XrFrameState fs = {XR_TYPE_FRAME_STATE};
		XrSession session = XR_NULL_HANDLE;
		bool should_render = false;
		XrView eye_views[2] = {XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
		bool eye_views_ok = false;
		static FrameStats s_legacy_stats;
		if (s.headset_rate)
		{
			if (!XRSession::IsSessionRunning())
				return;
			// The pacer's latest head pose becomes this present's (head_pose and friends are the GS thread's own copy).
			{
				std::lock_guard<std::mutex> lock(s.head_mutex);
				s.head_pose = s.pacer_head.pose;
				s.head_pose_valid = s.pacer_head.valid;
				s.head_position_valid = s.pacer_head.position_valid;
			}
			should_render = s.pacer_should_render.load(std::memory_order_acquire);
			// A pacer stuck in the runtime (dead or no longer pacing): no screen, hand or HUD swapchain calls into it
			// from this thread either (a lever card only re-uploads when its content changes). The screen layers
			// already published stay; VRManager pumps the events meanwhile, so a loss still reaches the teardown.
			if (PacerStalled())
				should_render = false;
		}
		else
		{
			if (s.begin_owed)
			{
				if (XRSession::IsSessionRunning())
				{
					SettleOwedBegin(XRSession::GetSession());
					// A begin that keeps failing means the runtime is gone: play goes on flat instead of retrying forever.
					if (s.begin_owed && ++s.begin_owed_presents >= kMaxRuntimeFailures)
					{
						Console.Error("(VR) xrBeginFrame still owed after %u presents: treating the runtime as lost.",
							s.begin_owed_presents);
						XRSession::MarkLost();
						s.begin_owed = false;
					}
				}
				else
				{
					s.begin_owed = false;
				}
				return;
			}

			s64 wait_ns = 0;
			{
				std::lock_guard<std::mutex> lock(s.frame_mutex);
				if (!s.has_pending_frame)
					return;
				fs = s.pending_frame_state;
				wait_ns = s.pending_wait_ns;
				s.has_pending_frame = false;
			}

			if (!XRSession::IsSessionRunning())
				return;

			session = XRSession::GetSession();
			const s64 begin_ns = SteadyNowNs();
			const XrResult bres = BeginFrameWithFaults(session);
			NoteFrameCallResult(bres, "xrBeginFrame");
			if (XR_FAILED(bres))
			{
				if (bres == XR_ERROR_SESSION_NOT_RUNNING || bres == XR_ERROR_SESSION_LOST || bres == XR_ERROR_INSTANCE_LOST)
					return;
				if (!s.warned_beginframe)
				{
					s.warned_beginframe = true;
					Console.Error("(VR) xrBeginFrame failed (%d); will retry the begin/end pairing.", static_cast<int>(bres));
				}
				s.begin_owed = true;
				s.begin_owed_display_time = fs.predictedDisplayTime;
				s.begin_owed_presents = 0;
				return;
			}
			NoteWaitToBegin(s_legacy_stats, wait_ns, begin_ns);

			XRInput::Update(fs.predictedDisplayTime);

			if (s.view_space != XR_NULL_HANDLE)
				s.head_pose_valid = LocateAndPublishHead(fs.predictedDisplayTime, &s.head_pose, &s.head_position_valid);

			// Every frame for the background; about once a second without it, for the Eyes line alone.
			static u64 s_next_eye_probe_ms = 0;
			const u64 probe_ms = CameraDriver::SteadyNowMs();
			if (s.opaque_bg && s.bg_ready)
			{
				eye_views_ok = LocateEyeViews(fs.predictedDisplayTime, eye_views);
			}
			else if (probe_ms >= s_next_eye_probe_ms)
			{
				s_next_eye_probe_ms = probe_ms + 1000;
				LocateEyeViews(fs.predictedDisplayTime, eye_views);
			}
			should_render = (fs.shouldRender != XR_FALSE);
		}

		if (s_reanchor_requested.load(std::memory_order_acquire))
		{
			// Headset rate: the head pose the pacer located; legacy: located for this frame's display time.
			XrPosef anchor_pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
			bool anchor_ok = false;
			bool anchor_position = false;
			if (s.headset_rate)
			{
				anchor_ok = s.head_pose_valid;
				anchor_pose = s.head_pose;
				anchor_position = s.head_position_valid;
			}
			else if (s.view_space != XR_NULL_HANDLE)
			{
				XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
				if (XR_SUCCEEDED(xrLocateSpace(s.view_space, XRSession::GetSpace(), fs.predictedDisplayTime, &loc)) &&
					(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
				{
					anchor_ok = true;
					anchor_pose = loc.pose;
					anchor_position = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
				}
			}
			if (anchor_ok)
			{
				const float qx = anchor_pose.orientation.x, qy = anchor_pose.orientation.y;
				const float qz = anchor_pose.orientation.z, qw = anchor_pose.orientation.w;
				const float fx = -2.0f * (qw * qy + qz * qx);
				const float fz = -(1.0f - 2.0f * (qx * qx + qy * qy));
				if ((fx * fx + fz * fz) > 1.0e-4f)
					s.screen_anchor_yaw = std::atan2(-fx, -fz);
				if (anchor_position)
				{
					s.screen_anchor_x = anchor_pose.position.x;
					s.screen_anchor_y = anchor_pose.position.y;
					s.screen_anchor_z = anchor_pose.position.z;
				}
				s_reanchor_requested.store(false, std::memory_order_release);
				s.world_anchor_x = s.screen_anchor_x;
				s.world_anchor_y = s.screen_anchor_y;
				s.world_anchor_z = s.screen_anchor_z;
				s.world_anchor_yaw = s.screen_anchor_yaw;
				Console.WriteLn("(VR) Screen re-anchored: pos (%.2f, %.2f, %.2f) yaw %.1f deg.",
					s.screen_anchor_x, s.screen_anchor_y, s.screen_anchor_z,
					s.screen_anchor_yaw * (180.0f / 3.14159265f));
				{
					std::lock_guard<std::mutex> lock(s_screen_mutex);
					s_anchor_snapshot.x = s.screen_anchor_x;
					s_anchor_snapshot.y = s.screen_anchor_y;
					s_anchor_snapshot.z = s.screen_anchor_z;
					s_anchor_snapshot.yaw = s.screen_anchor_yaw;
					s_anchor_snapshot.generation++;
				}
			}
		}

		// What the PublishedSet records about the first-person screen (filled in below where it is placed).
		bool pub_fp_screen = false;
		XrVector3f pub_fp_head = {0.0f, 0.0f, 0.0f};
		u64 pub_fp_image = 0;

		const bool layered = current && (static_cast<GSTextureVK*>(current)->GetArrayLayers() >= 2);
		const bool stereo = layered || (eye != MonoEye);
		if (current && (layered ? 1 : 0) != s.layered_logged)
		{
			s.layered_logged = layered ? 1 : 0;
			Console.WriteLn("(VR) Screen frames: %s", layered ? "layered stereo (2 views)" : "mono");
		}
		bool force_zero_layers = false;
		static const bool s_chainlog = (std::getenv("PCSX2_VR_CHAINLOG") != nullptr);
		static u64 s_chainlog_visit = 0;
		int cl_copy[2] = {-1, -1};
		// Headset rate: Copied sets ever_released below ahead of the release (ReleaseAndPublish), which restores
		// these if the release fails.
		const bool chain_was_released[2] = {s.chains[0].ever_released, s.chains[1].ever_released};
		if (should_render && current)
		{
			GSTextureVK* src = static_cast<GSTextureVK*>(current);
			const u32 w = static_cast<u32>(src->GetWidth());
			const u32 h = static_cast<u32>(src->GetHeight());
			if (w > 0 && h > 0 && EnsureSwapchains(w, h, stereo))
			{
				if (layered)
				{
					for (u32 l = 0; l < 2; l++)
					{
						const CopyResult r = CopyToSwapchain(src, l, s.chains[l]);
						cl_copy[l] = static_cast<int>(r);
						if (r == CopyResult::Copied)
							s.chains[l].ever_released = true;
						else if (r == CopyResult::TimeoutZeroLayer && !s.chains[l].ever_released)
							force_zero_layers = true;
					}
				}
				else
				{
					const u32 cl_ci = (eye != MonoEye) ? eye : 0;
					auto& chain = s.chains[cl_ci];
					const CopyResult r = CopyToSwapchain(src, 0, chain);
					cl_copy[cl_ci] = static_cast<int>(r);
					if (r == CopyResult::Copied)
						chain.ever_released = true;
					else if (r == CopyResult::TimeoutZeroLayer && !chain.ever_released)
						force_zero_layers = true;
				}
			}
		}

		XrCompositionLayerQuad quads[5] = {{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD},
			{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
		XrCompositionLayerCylinderKHR cyls[5] = {
			{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR}, {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR},
			{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR}, {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR},
			{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR}};
		// Worst case 13 layers: four for the screen (split screen in stereo, two viewports per eye; five slots
		// kept), two lever cards, two hands and the five HUD cards (laser, wrist, vignette, toast, blink), in a
		// capacity of 14 (kLayerCapacity). The background goes under them in the submit array; SteamVR takes at
		// most 16 a frame.
		XrCompositionLayerQuad lever_quads[ControlQuads::kSlots] = {
			{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
		XrCompositionLayerQuad hand_quads[kHandSlots] = {
			{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
		XrCompositionLayerQuad hud_quads[HudCards::kCardCount] = {};
		const XrCompositionLayerBaseHeader* layers[kLayerCapacity] = {};
		u32 layer_count = 0;
		bool cl_split = false;
		if (!force_zero_layers)
		{
			ScreenParams sp;
			{
				std::lock_guard<std::mutex> lock(s_screen_mutex);
				sp = s_screen_params;
			}
			// First person: head-locked screen sized to the game camera's FOV. Anything else (menus,
			// item screen, cutscenes, doors) goes back to the world screen, re-anchored in front of the
			// head each time first person hands over so it never appears behind the player.
			// With the comfort blink on, a change of screen waits until the blink has gone black
			// (kBlinkRiseMs), so the jump in placement is never seen; the blink then holds black while the
			// frames from the new camera, about 85-100 ms behind the change, reach the screen.
			const bool fp_want = sp.has_fp && CameraDriver::LookAtActive() && s.head_pose_valid;
			bool switch_now = false;
			if (fp_want == s_fp_screen_was_active)
			{
				// Wanted back before the change was made (a flicker): nothing to change; the blink plays out.
				s.fp_switch_due_ms = 0;
			}
			else if (!s.head_pose_valid)
			{
				// Tracking lost: straight to the world screen, which needs no head pose.
				switch_now = true;
			}
			else if (s.fp_switch_due_ms == 0)
			{
				// Wait only for a blink that can be drawn: with the HUD cards off for the session (or no VIEW
				// space) the delay would just show the old placement with the new camera for 40 ms.
				if (!s.hud_disabled && s.view_space != XR_NULL_HANDLE &&
					HudCards::Blink(fp_want ? "first-person screen" : "world screen"))
					s.fp_switch_due_ms = CameraDriver::SteadyNowMs() + HudCards::kBlinkRiseMs;
				else
					switch_now = true;
			}
			else if (CameraDriver::SteadyNowMs() >= s.fp_switch_due_ms)
			{
				switch_now = true;
			}
			if (switch_now)
			{
				s.fp_switch_due_ms = 0;
				if (!fp_want && s.head_pose_valid)
				{
					const XrQuaternionf& q = s.head_pose.orientation;
					const float fx = -2.0f * (q.w * q.y + q.z * q.x);
					const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
					if ((fx * fx + fz * fz) > 1.0e-4f)
						s.world_anchor_yaw = std::atan2(-fx, -fz);
					if (s.head_position_valid)
					{
						s.world_anchor_x = s.head_pose.position.x;
						s.world_anchor_y = s.head_pose.position.y;
						s.world_anchor_z = s.head_pose.position.z;
					}
				}
				Console.WriteLn("(VR) Screen: %s.", fp_want ? "first person (placed at the render pose, roll-level)" : "world-locked");
				s_fp_screen_was_active = fp_want;
				if (fp_want)
					LogFirstPersonScreen(sp);
			}
			const bool fp_screen = s_fp_screen_was_active;
			pub_fp_screen = fp_screen;

			// First-person screen: placed in the room (base space) in front of the head pose the shown
			// frame was rendered from (matched through the game's view matrix when the profile names it,
			// else the pose camera.lookAt used about poseLagMs ago), along its yaw and
			// pitch but never its roll. The runtime then reprojects it for the head's motion since, so the
			// world holds still while the head turns instead of an old frame being glued to the face.
			XrPosef fp_pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
			XrVector3f fp_head = {0.0f, 0.0f, 0.0f};
			if (fp_screen)
			{
				XrQuaternionf q = s.head_pose.orientation;
				fp_head = s.head_position_valid ? s.head_pose.position : XrVector3f{0.0f, 0.0f, 0.0f};
				HeadPose::Snapshot stamped;
				float turn_yaw = 0.0f;
				const u64 lag = static_cast<u64>(sp.fp_pose_lag_ms);
				bool placed = false;
				if (CameraDriver::RenderPose(&stamped, &turn_yaw) ||
					CameraDriver::FirstPersonPoseAt(CameraDriver::SteadyNowMs() - lag, &stamped))
				{
					q = {stamped.orientation_x, stamped.orientation_y, stamped.orientation_z, stamped.orientation_w};
					if (stamped.position_valid)
						fp_head = {stamped.position_x, stamped.position_y, stamped.position_z};
					placed = true;
				}
				pub_fp_head = fp_head;
				pub_fp_image = placed ? ImageKey(stamped) : 0;
				const float fx = -2.0f * (q.x * q.z + q.w * q.y);
				const float fy = -2.0f * (q.y * q.z - q.w * q.x);
				const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
				const float hyaw = std::atan2(-fx, -fz) + turn_yaw;
				const float hpitch = std::asin(std::clamp(fy, -1.0f, 1.0f));
				if (placed)
					LogEdgeError(hyaw);
				const float sy2 = std::sin(hyaw * 0.5f), cy2 = std::cos(hyaw * 0.5f);
				const float sp2 = std::sin(hpitch * 0.5f), cp2 = std::cos(hpitch * 0.5f);
				fp_pose.orientation = {cy2 * sp2, sy2 * cp2, -sy2 * sp2, cy2 * cp2};
				const float d = sp.fp_distance;
				fp_pose.position = {fp_head.x - d * std::sin(hyaw) * std::cos(hpitch), fp_head.y + d * std::sin(hpitch),
					fp_head.z - d * std::cos(hyaw) * std::cos(hpitch)};
			}
			const float distance = fp_screen ? sp.fp_distance : sp.distance;
			const float height = fp_screen ? sp.fp_height : sp.height;
			const float arc_deg = fp_screen ? sp.fp_arc_deg : sp.arc_deg;
			const float voffset = fp_screen ? 0.0f : sp.voffset;
			const float aspect = ComputeAspect();
			const bool curved = (arc_deg >= 5.0f) && XRSession::HasCylinderLayer();

			const float ayaw_sin = std::sin(s.world_anchor_yaw);
			const float ayaw_cos = std::cos(s.world_anchor_yaw);
			const XrQuaternionf anchor_quat = {
				0.0f, std::sin(s.world_anchor_yaw * 0.5f), 0.0f, std::cos(s.world_anchor_yaw * 0.5f)};

			const bool follow = !fp_screen && sp.follow_head && (s.view_space != XR_NULL_HANDLE);
			const XrSpace layer_space = follow ? s.view_space : XRSession::GetSpace();
			const XrQuaternionf follow_quat = {0.0f, 0.0f, 0.0f, 1.0f};

			const auto make_layer = [&](u32 chain_idx, XrEyeVisibility vis) {
				const XrSwapchainSubImage sub_image = {
					s.chains[chain_idx].swapchain,
					{{0, 0}, {static_cast<s32>(s.swapchain_width), static_cast<s32>(s.swapchain_height)}},
					0};
				if (curved)
				{
					XrCompositionLayerCylinderKHR& cyl = cyls[layer_count];
					cyl.layerFlags = 0;
					cyl.space = layer_space;
					cyl.eyeVisibility = vis;
					cyl.subImage = sub_image;
					if (fp_screen)
					{
						cyl.pose.orientation = fp_pose.orientation;
						cyl.pose.position = fp_head;
					}
					else if (follow)
					{
						cyl.pose.orientation = follow_quat;
						cyl.pose.position = {0.0f, voffset, 0.0f};
					}
					else
					{
						cyl.pose.orientation = anchor_quat;
						cyl.pose.position = {s.world_anchor_x, s.world_anchor_y + voffset, s.world_anchor_z};
					}
					cyl.radius = distance;
					cyl.centralAngle = arc_deg * (3.14159265f / 180.0f);
					cyl.aspectRatio = aspect;
					layers[layer_count] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cyl);
				}
				else
				{
					XrCompositionLayerQuad& quad = quads[layer_count];
					quad.layerFlags = 0;
					quad.space = layer_space;
					quad.eyeVisibility = vis;
					quad.subImage = sub_image;
					if (fp_screen)
						quad.pose = fp_pose;
					else if (follow)
					{
						quad.pose.orientation = follow_quat;
						quad.pose.position = {0.0f, voffset, -distance};
					}
					else
					{
						quad.pose.orientation = anchor_quat;
						quad.pose.position = {s.world_anchor_x - distance * ayaw_sin,
							s.world_anchor_y + voffset,
							s.world_anchor_z - distance * ayaw_cos};
					}
					quad.size = {height * aspect, height};
					layers[layer_count] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
				}
				layer_count++;
			};

			const SplitState::Snapshot split_snap = SplitState::Get();
			cl_split = split_snap.split_active;
			const auto make_split_layer = [&](u32 chain_idx, XrEyeVisibility vis, int viewport) {
				const s32 full_w = static_cast<s32>(s.swapchain_width);
				const s32 full_h = static_cast<s32>(s.swapchain_height);
				s32 rx = static_cast<s32>(split_snap.rect_x[viewport] * full_w);
				s32 ry = static_cast<s32>(split_snap.rect_y[viewport] * full_h);
				s32 rw = static_cast<s32>(split_snap.rect_w[viewport] * full_w);
				s32 rh = static_cast<s32>(split_snap.rect_h[viewport] * full_h);
				rx += 1; ry += 1; rw -= 2; rh -= 2;
				if (rw < 2 || rh < 2)
					return;
				const XrSwapchainSubImage sub_image = {
					s.chains[chain_idx].swapchain, {{rx, ry}, {rw, rh}}, 0};
				const float rect_aspect = static_cast<float>(rw) / static_cast<float>(rh);
				const bool is_local = (viewport == split_snap.local_view);
				const bool focus = (split_snap.mode == 0);
				const float shape_scale = (is_local || !focus) ? 1.0f : split_snap.side_scale;
				const float mirror_width = height * aspect;
				const float local_half_deg = curved ? (0.5f * arc_deg) :
					(std::atan((0.5f * mirror_width) / distance) * (180.0f / 3.14159265f));
				const float side_half_deg = curved ? (0.5f * arc_deg * shape_scale) :
					(std::atan((0.5f * mirror_width * shape_scale) / distance) * (180.0f / 3.14159265f));
				float eff_side_deg = split_snap.side_angle_deg;
				if (!is_local)
				{
					const float min_sep = local_half_deg + side_half_deg + 2.0f;
					if (std::fabs(eff_side_deg) < min_sep)
						eff_side_deg = (eff_side_deg < 0.0f) ? -min_sep : min_sep;
				}
				const float extra_yaw = is_local ? 0.0f :
					eff_side_deg * (3.14159265f / 180.0f);
				const float yaw = (follow ? 0.0f : s.world_anchor_yaw) + extra_yaw;
				const XrQuaternionf vp_quat = {
					0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
				const float ysin = std::sin(yaw);
				const float ycos = std::cos(yaw);
				const float vp_width = mirror_width * shape_scale;
				const float vp_phys_height = vp_width / rect_aspect;
				if (curved)
				{
					XrCompositionLayerCylinderKHR& cyl = cyls[layer_count];
					cyl.layerFlags = 0;
					cyl.space = layer_space;
					cyl.eyeVisibility = vis;
					cyl.subImage = sub_image;
					cyl.pose.orientation = vp_quat;
					cyl.pose.position = follow ?
						XrVector3f{0.0f, voffset, 0.0f} :
						XrVector3f{s.world_anchor_x, s.world_anchor_y + voffset, s.world_anchor_z};
					cyl.radius = distance;
					cyl.centralAngle = arc_deg * (3.14159265f / 180.0f) * shape_scale;
					cyl.aspectRatio = rect_aspect;
					layers[layer_count] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cyl);
				}
				else
				{
					XrCompositionLayerQuad& quad = quads[layer_count];
					quad.layerFlags = 0;
					quad.space = layer_space;
					quad.eyeVisibility = vis;
					quad.subImage = sub_image;
					quad.pose.orientation = vp_quat;
					quad.pose.position = follow ?
						XrVector3f{-distance * ysin, voffset, -distance * ycos} :
						XrVector3f{s.world_anchor_x - distance * ysin,
							s.world_anchor_y + voffset,
							s.world_anchor_z - distance * ycos};
					quad.size = {vp_width, vp_phys_height};
					layers[layer_count] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
				}
				layer_count++;
			};

			const bool l0 = s.chains[0].ever_released;
			const bool l1 = s.chains[1].ever_released;
			if (split_snap.split_active && (l0 || l1))
			{
				if (VR::SeatCastArmed() && current)
				{
					const int ovp = 1 - split_snap.local_view;
					GSTexture* src = current;
					const s32 full_w = static_cast<s32>(src->GetWidth());
					const s32 full_h = static_cast<s32>(src->GetHeight());
					GSVector4i crop(
						static_cast<int>(split_snap.rect_x[ovp] * full_w),
						static_cast<int>(split_snap.rect_y[ovp] * full_h),
						static_cast<int>((split_snap.rect_x[ovp] + split_snap.rect_w[ovp]) * full_w),
						static_cast<int>((split_snap.rect_y[ovp] + split_snap.rect_h[ovp]) * full_h));
					u32 out_w = static_cast<u32>(crop.width());
					u32 out_h = static_cast<u32>(crop.height());
					static GSTexture* s_cast_rt = nullptr;
					if (out_w > SeatCast::kMaxWidth)
					{
						const float scale = static_cast<float>(SeatCast::kMaxWidth) / out_w;
						out_w = SeatCast::kMaxWidth;
						out_h = static_cast<u32>(out_h * scale);
					}
					if (!s_cast_rt || s_cast_rt->GetWidth() != static_cast<int>(out_w) ||
						s_cast_rt->GetHeight() != static_cast<int>(out_h))
					{
						if (s_cast_rt)
							g_gs_device->Recycle(s_cast_rt);
						s_cast_rt = g_gs_device->CreateRenderTarget(out_w, out_h, GSTexture::Format::Color, false);
					}
					static std::unique_ptr<GSDownloadTexture> s_cast_dl;
					if (s_cast_rt)
					{
						const GSVector4 src_uv(
							static_cast<float>(crop.x) / full_w, static_cast<float>(crop.y) / full_h,
							static_cast<float>(crop.z) / full_w, static_cast<float>(crop.w) / full_h);
						g_gs_device->StretchRect(src, src_uv, s_cast_rt,
							GSVector4(0.0f, 0.0f, static_cast<float>(out_w), static_cast<float>(out_h)),
							ShaderConvertSelector(ShaderConvert::COPY), Filter::Biln);
						if (!s_cast_dl || s_cast_dl->GetWidth() < out_w || s_cast_dl->GetHeight() < out_h)
							s_cast_dl = g_gs_device->CreateDownloadTexture(out_w, out_h, GSTexture::Format::Color);
						const GSVector4i rc(0, 0, out_w, out_h);
						if (s_cast_dl)
						{
							s_cast_dl->CopyFromTexture(rc, s_cast_rt, rc, 0);
							s_cast_dl->Flush();
							if (s_cast_dl->Map(rc))
							{
								SeatCast::Publish(s_cast_dl->GetMapPointer(), out_w, out_h, s_cast_dl->GetMapPitch());
								s_cast_dl->Unmap();
							}
						}
					}
				}
				const bool split_stereo = stereo && split_snap.stereo_on && l0 && l1;
				for (int vp = 0; vp < 2; vp++)
				{
					if (split_snap.mode == 2 && vp != split_snap.local_view)
						continue;
					if (split_stereo)
					{
						make_split_layer(0, XR_EYE_VISIBILITY_LEFT, vp);
						make_split_layer(1, XR_EYE_VISIBILITY_RIGHT, vp);
					}
					else
					{
						make_split_layer(l0 ? 0 : 1, XR_EYE_VISIBILITY_BOTH, vp);
					}
				}
			}
			else if (stereo && l0 && l1)
			{
				make_layer(0, XR_EYE_VISIBILITY_LEFT);
				make_layer(1, XR_EYE_VISIBILITY_RIGHT);
			}
			else if (l0 || l1)
			{
				make_layer(l0 ? 0 : 1, XR_EYE_VISIBILITY_BOTH);
			}
		}

		if (s_chainlog)
		{
			const char* branch = (layer_count == 0) ? "Z" :
				cl_split ? "SP" :
				(layer_count == 2) ? "S" :
				(s.chains[0].ever_released ? "B0" : "B1");
			GSTextureVK* cur = static_cast<GSTextureVK*>(current);
			Console.WriteLn("(VR) CHAINLOG v=%llu render=%d cur=%s%ux%u/%uL layered=%d stereo=%d "
							"c0={r=%d er=%d wp=%d} c1={r=%d er=%d wp=%d} branch=%s layers=%u",
				static_cast<unsigned long long>(s_chainlog_visit++), should_render ? 1 : 0,
				current ? "" : "NULL:", cur ? static_cast<u32>(cur->GetWidth()) : 0,
				cur ? static_cast<u32>(cur->GetHeight()) : 0, cur ? cur->GetArrayLayers() : 0,
				layered ? 1 : 0, stereo ? 1 : 0,
				cl_copy[0], s.chains[0].ever_released ? 1 : 0, s.chains[0].wait_pending ? 1 : 0,
				cl_copy[1], s.chains[1].ever_released ? 1 : 0, s.chains[1].wait_pending ? 1 : 0,
				branch, layer_count);
		}

		if (!force_zero_layers)
			BuildLeverLayers(lever_quads, layers, layer_count, kLayerCapacity, cl_split);

		// Hands go last so they draw over the screen and the lever cards.
		const u32 hands_from = layer_count;
		if (!force_zero_layers && should_render)
			BuildHandLayers(hand_quads, layers, layer_count, kLayerCapacity);

		// HUD cards last of all, over the hands and everything else; only the laser sight goes in under the hands.
		if (!force_zero_layers && should_render)
			BuildHudLayers(hud_quads, layers, layer_count, kLayerCapacity, hands_from);

		for (u32 l = 0; l < 2; l++)
		{
			if (cl_copy[l] == static_cast<int>(CopyResult::Skipped))
				s.stat_copy_skipped.fetch_add(1, std::memory_order_relaxed);
			else if (cl_copy[l] == static_cast<int>(CopyResult::TimeoutZeroLayer))
				s.stat_copy_timeout.fetch_add(1, std::memory_order_relaxed);
		}

		const LayerSources sources = {quads, cyls, 5, lever_quads, hand_quads, hud_quads};

		if (s.headset_rate)
		{
			// The images copied this present are released together with publishing their layers, so the pacer never
			// ends a frame with a new image at the old pose; the pose also stays with its image when a copy failed.
			ReleaseAndPublish(layers, layer_count, sources, cl_copy, chain_was_released, pub_fp_screen, pub_fp_head,
				pub_fp_image);
			return;
		}

		// Legacy: this present ends the frame, with the background under its layers.
		PublishedSet set = {};
		FillPublishedSet(&set, layers, layer_count, sources);
		set.fp_screen = pub_fp_screen;
		set.fp_head = pub_fp_head;
		set.fp_image = pub_fp_image;
		XrCompositionLayerProjectionView bg_views[2] = {};
		XrCompositionLayerProjection bg = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		if (eye_views_ok)
			MakeBackgroundLayer(eye_views, bg_views, &bg);
		const XrCompositionLayerBaseHeader* submit[kLayerCapacity + 1] = {};
		const u32 submit_count = BuildSubmitArray(set, eye_views_ok ? &bg : nullptr, false, 0.0f, submit);

		const s64 end_start_ns = SteadyNowNs();
		const XrResult eres = LockedEndFrame(session, fs.predictedDisplayTime, submit_count, submit);
		const float end_ms = static_cast<float>(static_cast<double>(SteadyNowNs() - end_start_ns) / 1.0e6);
		NoteFrameCallResult(eres, "xrEndFrame");
		if (XR_FAILED(eres) && !s.warned_endframe)
		{
			s.warned_endframe = true;
			Console.Error("(VR) xrEndFrame failed (%d).", static_cast<int>(eres));
		}

		s.stat_gs_frames.fetch_add(1, std::memory_order_relaxed);
		if (XR_SUCCEEDED(eres))
			s_legacy_stats.submitted++;
		if (!should_render)
			s_legacy_stats.no_render++;
		s_legacy_stats.end_max_ms = std::max(s_legacy_stats.end_max_ms, end_ms);
		s_legacy_stats.layers = submit_count;
		if (XR_SUCCEEDED(eres))
			NoteScreenYaw(set, fs.predictedDisplayTime);
		MaybeLogFrameStats(s_legacy_stats);
	}

	void Shutdown()
	{
		if (!s.initialized)
			return;

		s.pacer_exit.store(true, std::memory_order_release);

		// Legacy: the pacer may be parked in xrWaitFrame until the frame it waited is begun and ended; drain it.
		if (!s.headset_rate && XRSession::HasSession())
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (!s.pacer_done.load(std::memory_order_acquire))
			{
				SettleOwedBegin(XRSession::GetSession());
				DrainOnePacedFrameForShutdown();
				if (s.pacer_done.load(std::memory_order_acquire))
					break;
				if (std::chrono::steady_clock::now() >= deadline)
				{
					Console.Warning("(VR) Pacer did not exit within 2s during shutdown.");
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
		}

		// Bounded: a pacer stuck inside the runtime must not hang the GS thread, and with it the emulator.
		const auto join_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kPacerJoinTimeoutMs);
		while (!s.pacer_done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < join_deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		if (s.pacer.joinable())
		{
			if (s.pacer_done.load(std::memory_order_acquire))
			{
				s.pacer.join();
			}
			else
			{
				Console.Error("(VR) Pacer thread stuck in the runtime: detaching it; VR handles stay alive until exit.");
				s.pacer_gen.fetch_add(1, std::memory_order_acq_rel);
				s.abandoned.store(true, std::memory_order_release);
				s.pacer.detach();
				// It may be holding the Vulkan queue lock inside the runtime: the GS goes on without the lock.
				VR::SetXrQueueLockEnabled(false);
			}
		}
		const bool abandoned = s.abandoned.load(std::memory_order_acquire);

		HeadPose::Invalidate();
		if (s.view_space != XR_NULL_HANDLE)
		{
			if (!abandoned)
				xrDestroySpace(s.view_space);
			s.view_space = XR_NULL_HANDLE;
		}

		const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
		const bool vk_alive = h.IsValid();

		if (vk_alive)
			WaitAllFences();

		// With the compositor abandoned these forget their swapchains instead of destroying them.
		DestroyLeverResources();
		DestroyHandResources();
		DestroyHudResources();
		DestroyBackground();

		for (auto& chain : s.chains)
		{
			if (chain.swapchain != XR_NULL_HANDLE)
			{
				DestroySwapchainSynced(chain.swapchain);
				chain.swapchain = XR_NULL_HANDLE;
			}
			chain.images.clear();
			chain.ever_released = false;
			chain.wait_pending = false;
			chain.release_pending = false;
		}
		s.swapchain_width = 0;
		s.swapchain_height = 0;

		if (vk_alive)
		{
			for (u32 i = 0; i < NUM_CMD_BUFFERS; i++)
			{
				if (s.fences[i] != VK_NULL_HANDLE)
				{
					vkDestroyFence(h.device, s.fences[i], nullptr);
					s.fences[i] = VK_NULL_HANDLE;
				}
			}
			if (s.cmd_pool != VK_NULL_HANDLE)
			{
				vkFreeCommandBuffers(h.device, s.cmd_pool, NUM_CMD_BUFFERS, s.cmd_buffers);
				vkDestroyCommandPool(h.device, s.cmd_pool, nullptr);
			}
		}
		else
		{
			for (u32 i = 0; i < NUM_CMD_BUFFERS; i++)
				s.fences[i] = VK_NULL_HANDLE;
		}
		s.cmd_pool = VK_NULL_HANDLE;
		for (u32 i = 0; i < NUM_CMD_BUFFERS; i++)
		{
			s.cmd_buffers[i] = VK_NULL_HANDLE;
			s.fence_submitted[i] = false;
		}
		s.next_cmd_buffer = 0;

		s.initialized = false;
		Console.WriteLn("(VR) Compositor shut down.");
	}

	bool Abandoned()
	{
		return s.abandoned.load(std::memory_order_acquire);
	}

	bool PacerOwnsFrames()
	{
		return s.initialized && s.headset_rate;
	}

	bool PacerStalled()
	{
		if (!s.initialized || !s.headset_rate)
			return false;
		if (s.pacer_done.load(std::memory_order_acquire))
			return true;
		const u64 beat = s.pacer_heartbeat_ms.load(std::memory_order_acquire);
		return beat != 0 && CameraDriver::SteadyNowMs() > beat + kPacerStallMs;
	}
#else
	bool Initialize()
	{
		Console.Error("(VR) Compositor requires the Vulkan renderer; running flat.");
		return false;
	}

	void Shutdown() {}

	void EndOfFrame(GSTexture* , u32 ) {}

	bool Abandoned()
	{
		return false;
	}

	bool PacerOwnsFrames()
	{
		return false;
	}

	bool PacerStalled()
	{
		return false;
	}
#endif

	EyeInfo GetEyeInfo()
	{
		std::lock_guard<std::mutex> lock(s_eye_mutex);
		return s_eye_info;
	}

	void UpdateScreenParams(float distance_m, float height_m, float arc_deg, float vertical_offset_m,
		bool follow_head)
	{
		std::lock_guard<std::mutex> lock(s_screen_mutex);
		s_screen_params.distance = distance_m;
		s_screen_params.height = height_m;
		s_screen_params.arc_deg = arc_deg;
		s_screen_params.voffset = vertical_offset_m;
		s_screen_params.follow_head = follow_head;
	}

	void UpdateFirstPersonScreen(bool enabled, float distance_m, float height_m, float arc_deg, float pose_lag_ms)
	{
		std::lock_guard<std::mutex> lock(s_screen_mutex);
		s_screen_params.has_fp = enabled;
		s_screen_params.fp_distance = distance_m;
		s_screen_params.fp_height = height_m;
		s_screen_params.fp_arc_deg = arc_deg;
		s_screen_params.fp_pose_lag_ms = pose_lag_ms;
	}

	void RequestScreenReanchor()
	{
		s_reanchor_requested.store(true, std::memory_order_release);
	}

	bool IsReanchorRequested()
	{
		return s_reanchor_requested.load(std::memory_order_acquire);
	}

	void ClearReanchorRequestForTest()
	{
		s_reanchor_requested.store(false, std::memory_order_release);
	}

	ScreenAnchor GetScreenAnchor()
	{
		std::lock_guard<std::mutex> lock(s_screen_mutex);
		return s_anchor_snapshot;
	}
}
