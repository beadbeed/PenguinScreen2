// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/XRCompositor.h"
#include "VR/CameraDriver.h"
#include "VR/SplitState.h"
#include "VR/ControlQuads.h"
#include "VR/HandModel.h"
#include "VR/GameFeedback.h"
#include "VR/HudCards.h"
#include "VR/MenuPointer.h"
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
#include "VMManager.h"

#ifdef ENABLE_VULKAN
#include "GS/GS.h"
#include "GS/Renderers/Vulkan/GSTextureVK.h"
#endif

#include "common/Console.h"
#include "common/Threading.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
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
			// Menu tablet (VR setting MenuTablet, profile screen.menu): while a menu is up in first-person play, the menu
			// goes on a small flat screen this far ahead of the eyes and this far below them, tilted back to face them
			// (menu_tilt_deg below 0: exactly at the eyes, atan2(drop, distance), about 23 deg).
			bool has_menu = false;
			float menu_distance = 0.70f;
			float menu_height = 0.36f;
			float menu_drop = 0.30f;
			float menu_tilt_deg = -1.0f;
		};
		bool s_fp_screen_was_active = false;
		// The menu tablet is the screen in use (GS thread, like s_fp_screen_was_active; never both).
		bool s_tablet_was_active = false;
		std::mutex s_screen_mutex;
		ScreenParams s_screen_params;

		std::atomic<bool> s_reanchor_requested{false};

		ScreenAnchor s_anchor_snapshot;

		bool s_xrfault_begin1_fired = false;

		// GetEyeInfo: what the runtime last reported, and what the Eyes line last logged.
		std::mutex s_eye_mutex;
		EyeInfo s_eye_info;
		EyeInfo s_eye_logged;
		// GetFirstPersonScreenInfo: the first-person screen as last placed (guarded by s_screen_mutex).
		FirstPersonScreenInfo s_fp_screen_info;
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
		// All of one present's swapchain image waits together (both eyes, levers, hands, HUD cards) stay under this, so
		// a runtime that is slow to hand images back (measured on the null HMD: 5 ms per chain, 15-20 ms per present)
		// costs the GS at most this much per present; a chain whose image isn't back in time keeps its previous one.
		constexpr int64_t kPresentWaitBudgetNs = 3000000;
		int64_t s_present_wait_left_ns = kPresentWaitBudgetNs;

		// xrWaitSwapchainImage for at most max_ns and at most what is left of this present's budget (0: just poll).
		XrResult BudgetedWaitSwapchainImage(XrSwapchain swapchain, int64_t max_ns)
		{
			XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wi.timeout = std::min<int64_t>(max_ns, std::max<int64_t>(0, s_present_wait_left_ns));
			const auto start = std::chrono::steady_clock::now();
			const XrResult res = xrWaitSwapchainImage(swapchain, &wi);
			s_present_wait_left_ns -= std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
			return res;
		}
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
		// cards, two hands and the six HUD cards (laser or menu beam, wrist, vignette, toast, blink, menu pointer dot).
		// The submit array adds the background under them; SteamVR takes at most 16 a frame.
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

		// A layer that moves with a controller (a hand impostor, the laser, the wrist card): the controller pose it was placed
		// for. With [VR] HandsDisplayRate the pacer moves it rigidly with that hand to the pose located for each display frame.
		struct HandAttach
		{
			bool attached = false;
			u8 hand = 0; // HandModel::kLeft / kRight
			float ref_pos[3] = {0.0f, 0.0f, 0.0f}; // the grip position
			float ref_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // the orientation the layer was placed by (aim, or grip)
			float ref_eye[3] = {0.0f, 0.0f, 0.0f}; // the eye the image was drawn from
			XrTime ref_time = 0; // the display time that pose was located for; 0 = unknown
		};

		// Which part of the menu pointer a published layer is (PublishedSet::Entry::pointer), so the pacer can place it
		// again each display frame (PlaceMenuPointerNow).
		constexpr u8 kPointerPartNone = 0;
		constexpr u8 kPointerPartBeam = 1; // the menu beam, in the laser's slot
		constexpr u8 kPointerPartDot = 2;

		// What the pacer needs to place the menu pointer's beam and dot again each display frame: the flat screen quad
		// they were placed on (the world screen or the menu tablet, as GetScreenTransform() gave it to PlaceMenuPointer),
		// whether a hand's aim ray placed them (a PINE test hit has no hand to follow) and which hand's, and whether the
		// laser's slot holds the menu beam this present. BuildHudLayers fills it (GS thread) and the publish copies it into
		// the set.
		struct PointerPublish
		{
			bool screen_valid = false;
			float screen_pos[3] = {0.0f, 0.0f, 0.0f};
			float screen_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // +Z the screen's face normal
			float half_width = 0.0f;
			float half_height = 0.0f;
			bool ray = false;
			s8 hand = -1; // with ray, the hand whose aim placed them (HandModel::kLeft / kRight)
			bool beam = false;
		};

		// The layers the GS built at its last present, copied by value: the pacer submits them every display period
		// until the next present replaces them (the runtime keeps showing each swapchain's last released image).
		struct PublishedSet
		{
			struct Entry
			{
				u8 kind;
				u8 role;
				u8 pointer; // kPointerPart*: the menu pointer's beam or dot, placed again by the pacer
				XrCompositionLayerQuad quad;
				XrCompositionLayerCylinderKHR cyl;
				HandAttach att; // not attached unless the layer rides on a controller
			};
			Entry e[kLayerCapacity];
			u32 count;
			PointerPublish pointer; // how the menu pointer's beam and dot in e were placed
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
			// [VR] HandsDisplayRate and the hands' test aids (PCSX2_VR_HANDS_RERASTER=always, PCSX2_VR_FAKE_HANDS: 1, or 2 for
			// "gun", PCSX2_VR_FAKE_HANDS_SWEEP, PCSX2_VR_HANDS_GRIP_ORIENT), read once at Initialize before the pacer starts,
			// so the pacer reads them without a lock.
			bool hands_display_rate = true;
			bool hands_reraster_always = false;
			int fake_hands = 0;
			bool fake_sweep = false;
			bool grip_orient = false;

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
				XrTime time = 0; // the display time it was located for
			};
			std::mutex head_mutex;
			PacerHead pacer_head;

			// Lock order: submit_mutex, then the Vulkan queue lock (VR::ScopedXrQueueLock); never the reverse.
			// The pacer holds submit_mutex from copying the published layers through xrEndFrame; a swapchain is
			// destroyed under it, after its entries are dropped. publish_mutex only covers copies (no XR call inside).
			std::timed_mutex submit_mutex;
			std::mutex publish_mutex;
			PublishedSet published = {};
			// Headset rate: a present whose release and publish found the pacer inside xrEndFrame keeps its images acquired
			// and its layers here instead of waiting (the GS, and so the emulation, never waits for the runtime). The
			// next present that gets submit_mutex publishes them, then releases the images, so each image keeps its own
			// pose. Only the GS thread touches these.
			PublishedSet deferred = {};
			bool deferred_valid = false;
			// SteadyNowMs when the deferral was made, restarted each time the pacer's heartbeat has moved past it; 0 = none.
			u64 deferred_since_ms = 0;
			std::atomic<u32> stat_publish_deferred{0};

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
			// Hand images drawn and uploaded (new), and hands shown again from their last image (kept), for the Hands line.
			std::atomic<u32> stat_hand_new{0};
			std::atomic<u32> stat_hand_kept{0};

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
				// The acquired image (pending_index) is waited and ours to copy into, but its copy is held back: in layered
				// stereo both eyes are copied in the same present or neither is (WaitEye), so a ready eye whose partner
				// was not ready keeps its image for the next present. Waiting on it again would be a call-order error.
				bool waited = false;
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
			// What a hand swapchain's image shows: the impostor quad and the hand it was drawn for, the eye it was drawn
			// from, the display time of that hand pose and when (SteadyNowMs) it was drawn. GS thread only.
			struct HandImage
			{
				bool valid = false;
				HandModel::ImpostorQuad iq;
				HandModel::HandState hs;
				float eye[3] = {0.0f, 0.0f, 0.0f};
				XrTime pose_time = 0;
				u64 raster_ms = 0;
			};
			HandImage hand_image[kHandSlots];         // what each hand swapchain's last RELEASED image shows
			HandImage hand_image_pending[kHandSlots]; // drawn this present, committed at its release (ReleaseAndPublish)

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
			// Menu pointer: the beam's and the dot's fades (the same clock), and whether it was logged yet this session.
			float hud_beam_opacity = 0.0f, hud_pointer_opacity = 0.0f;
			bool hud_pointer_logged = false;
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
			XrTime head_pose_time = 0; // the display time head_pose was located for (GS thread)

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

		// How long the GS waits for a pacer inside the runtime before VR is abandoned (as VRVulkanBridge's queue lock):
		// rides out a runtime hiccup, and the emulation never waits longer than this on a runtime that stopped answering.
		constexpr auto kRuntimeStuckTimeout = std::chrono::milliseconds(500);

		// Destroys a swapchain the pacer may be submitting: waits for the pacer's submit in progress (submit_mutex),
		// drops the published entries that show it, then destroys it. Bounded like the queue lock: a pacer stuck in
		// xrEndFrame for 500 ms means the runtime stopped answering, so VR is abandoned and the swapchain left alive.
		// With the compositor abandoned the handle is only forgotten.
		void DestroySwapchainSynced(XrSwapchain swapchain)
		{
			if (swapchain == XR_NULL_HANDLE || s.abandoned.load(std::memory_order_acquire))
				return;
			// A deferred publish may show this swapchain: drop it (its images, if still acquired, are released with
			// the next publish, which keeps the previous Screen entries).
			s.deferred_valid = false;
			s.deferred_since_ms = 0;
			std::unique_lock<std::timed_mutex> submit(s.submit_mutex, std::defer_lock);
			if (!submit.try_lock_for(kRuntimeStuckTimeout))
			{
				Console.Error("(VR) The VR pacer has been inside xrEndFrame for over 500 ms (runtime not answering): "
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
				chain.waited = false;
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

		// Copies layer src_layers[k] of src into swapchain image dsts[k], for each of the count (1 or 2) images, in one
		// command buffer and one submit, so the copies go in together or not at all (both eyes in layered stereo).
		bool RecordAndSubmitCopies(GSTextureVK* src, const u32* src_layers, const VkImage* dsts, u32 count)
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

			for (u32 k = 0; k < count; k++)
			{
				const VkImage dst = dsts[k];
				ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

				VkImageCopy region = {};
				region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, src_layers[k], 1};
				region.srcOffset = {0, 0, 0};
				region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				region.dstOffset = {0, 0, 0};
				region.extent = {s.swapchain_width, s.swapchain_height, 1};
				vkCmdCopyImage(cmd, src->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

				ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
					VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
			}

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

		// What waiting for an eye chain's image gave: Ready (acquired and waited: its copy may go in), or what the present
		// reports for that eye instead, as CopyResult does: Skipped (nothing acquired, or the image still waits for its
		// release) or TimeoutZeroLayer (acquired but not handed back by the runtime yet; the next present waits again).
		enum class EyeWait
		{
			Ready,
			Skipped,
			TimeoutZeroLayer
		};

		// Acquires the chain's next image (or keeps the one acquired earlier) and waits for it within this present's
		// budget. An image waited at an earlier present, whose copy was held back for the other eye, is Ready at once.
		EyeWait WaitEye(decltype(s.chains[0])& chain)
		{
			// An image still waiting for its release (the last publish found the pacer inside xrEndFrame and was
			// deferred): nothing is copied into the chain until the next publish releases it.
			if (chain.release_pending)
				return EyeWait::Skipped;
			if (chain.waited)
				return EyeWait::Ready;

			if (!chain.wait_pending)
			{
				uint32_t index = 0;
				const XrResult acquire_res = LockedAcquire(chain.swapchain, &index);
				if (XR_FAILED(acquire_res))
				{
					if (!s.warned_acquire)
					{
						s.warned_acquire = true;
						Console.Error("(VR) xrAcquireSwapchainImage failed (%d).", static_cast<int>(acquire_res));
					}
					return EyeWait::Skipped;
				}
				chain.pending_index = index;
				chain.wait_pending = true;
			}

			const XrResult wait_res = BudgetedWaitSwapchainImage(chain.swapchain, kSwapchainWaitNs);
			if (wait_res == XR_TIMEOUT_EXPIRED)
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
				return EyeWait::TimeoutZeroLayer;
			}
			if (XR_FAILED(wait_res))
			{
				if (!s.warned_wait_fail)
				{
					s.warned_wait_fail = true;
					Console.Error("(VR) xrWaitSwapchainImage failed (%d).", static_cast<int>(wait_res));
				}
				return EyeWait::TimeoutZeroLayer;
			}
			chain.wait_pending = false;
			chain.waited = true;
			return EyeWait::Ready;
		}

		// What an eye whose copy did not go in this present reports: a ready eye held back for its partner, Skipped.
		CopyResult NotCopied(EyeWait w)
		{
			return (w == EyeWait::TimeoutZeroLayer) ? CopyResult::TimeoutZeroLayer : CopyResult::Skipped;
		}

		// Copies src's layer src_layers[k] into eye chain chain_ids[k], for count (1 or 2) chains that WaitEye made Ready,
		// in one submit, then hands each image back. Legacy: releases it. Headset rate: leaves it acquired
		// (release_pending); EndOfFrame releases it under submit_mutex together with publishing the layers that show it
		// (ReleaseAndPublish), so the pacer can never show the new image at the previous present's pose. Copied then means
		// "copied, release still to come"; a failed release turns it into Skipped there. A waited image goes back even
		// when the copy was not submitted. Each chain's result goes in out[k].
		void CopyEyes(GSTextureVK* src, const u32* src_layers, const u32* chain_ids, u32 count, CopyResult* out)
		{
			VkImage dsts[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
			count = std::min<u32>(count, 2);
			for (u32 k = 0; k < count; k++)
			{
				const auto& chain = s.chains[chain_ids[k]];
				dsts[k] = chain.images[chain.pending_index].image;
			}
			const bool submitted = RecordAndSubmitCopies(src, src_layers, dsts, count);

			for (u32 k = 0; k < count; k++)
			{
				auto& chain = s.chains[chain_ids[k]];
				chain.waited = false;
				out[k] = submitted ? CopyResult::Copied : CopyResult::Skipped;
				if (s.headset_rate)
				{
					// Released in ReleaseAndPublish.
					chain.release_pending = true;
					continue;
				}
				const XrResult res = LockedRelease(chain.swapchain);
				if (XR_FAILED(res))
				{
					if (!s.warned_release)
					{
						s.warned_release = true;
						Console.Error("(VR) xrReleaseSwapchainImage failed (%d).", static_cast<int>(res));
					}
					out[k] = CopyResult::Skipped;
				}
			}
		}

		// One chain on its own (mono, or one eye of alternate-eye stereo): waits for its image and copies src's
		// src_layer into it.
		CopyResult CopyToSwapchain(GSTextureVK* src, u32 src_layer, u32 chain_id)
		{
			const EyeWait w = WaitEye(s.chains[chain_id]);
			if (w != EyeWait::Ready)
				return NotCopied(w);
			CopyResult r = CopyResult::Skipped;
			CopyEyes(src, &src_layer, &chain_id, 1, &r);
			return r;
		}

		// Layered stereo: the two eyes are one unit. Both images are waited first, and both copied only when both are
		// ready, in one submit; otherwise neither is copied this present and a ready one keeps its waited image for the
		// next (WaitEye). A lone eye is never copied or released, so the headset can't show one eye's new game frame
		// beside the other's old one, each at the previous pose (the keep_screen in ReleaseAndPublish keeps the old pair
		// together).
		void CopyEyePair(GSTextureVK* src, CopyResult* out)
		{
			const EyeWait w0 = WaitEye(s.chains[0]);
			const EyeWait w1 = WaitEye(s.chains[1]);
			if (w0 == EyeWait::Ready && w1 == EyeWait::Ready)
			{
				static constexpr u32 kEyes[2] = {0, 1};
				CopyEyes(src, kEyes, kEyes, 2, out);
				return;
			}
			out[0] = NotCopied(w0);
			out[1] = NotCopied(w1);
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
				// XR_TIMEOUT_EXPIRED is a success code: the image isn't ours yet, so wait again next frame.
				const XrResult lwr = BudgetedWaitSwapchainImage(lc.swapchain, kSwapchainWaitNs);
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

		// PCSX2_VR_FAKE_HANDS: a test hand held at its offset from the head (HandModel::FakeHandState), swept on the
		// display clock t with PCSX2_VR_FAKE_HANDS_SWEEP. The GS thread and the pacer both call it (it is pure), so for
		// the same head and display time both get the same hand.
		HandModel::HandState FakeHand(int hand, const float eye[3], const float head_quat[4], bool gun, XrTime t)
		{
			HandModel::HandState h = HandModel::FakeHandState(hand, eye, head_quat, gun);
			if (s.fake_sweep && h.valid)
				HandModel::ApplyFakeSweep(hand, static_cast<s64>(t), &h);
			return h;
		}

		// Records in att[slot] that a layer was placed for this hand pose (pos: the grip position; quat: the
		// orientation it was placed by), drawn from eye, located for display time t.
		void AttachToHand(HandAttach* att, int slot, int hand, const float pos[3], const float quat[4], const float eye[3],
			XrTime t)
		{
			if (!att)
				return;
			HandAttach& a = att[slot];
			a.attached = true;
			a.hand = static_cast<u8>(hand);
			for (int k = 0; k < 3; k++)
			{
				a.ref_pos[k] = pos[k];
				a.ref_eye[k] = eye[k];
			}
			for (int k = 0; k < 4; k++)
				a.ref_quat[k] = quat[k];
			a.ref_time = t;
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
			// The swapchains are gone, and with them the images a kept hand would show.
			for (int i = 0; i < kHandSlots; i++)
			{
				s.hand_image[i].valid = false;
				s.hand_image_pending[i].valid = false;
			}
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

		// The hands are drawn while camera.lookAt drives the game camera, and while a menu is up in first-person play
		// (menu mode) on the world screen or the menu tablet, where they point at it.
		bool HandsWanted()
		{
			return CameraDriver::LookAtActive() || (SpatialControls::MenuMode() && !s_fp_screen_was_active);
		}

		// First-person hands. Each presented frame, while camera.lookAt drives the game camera and a
		// controller's grip pose is valid, its hand is drawn on the CPU as seen from the head onto a
		// quad that faces the head, uploaded, and submitted after the screen and lever layers. The quad
		// is flat at the hand's depth, so the runtime's reprojection keeps it in place as the head moves.
		u32 BuildHandLayers(XrCompositionLayerQuad* hand_quads, const XrCompositionLayerBaseHeader** layers,
			u32& layer_count, u32 layer_capacity, HandAttach* att)
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
			if (!fake && (!s.head_position_valid || !HandsWanted()))
				return 0;

			// The head pose was located in XRSession::GetSpace() at this frame's display time, the same
			// space and time XRInput::Update just located the controllers in.
			const float eye[3] = {s.head_position_valid ? s.head_pose.position.x : 0.0f,
				s.head_position_valid ? s.head_pose.position.y : 0.0f, s.head_position_valid ? s.head_pose.position.z : 0.0f};
			const float head_quat[4] = {s.head_pose.orientation.x, s.head_pose.orientation.y, s.head_pose.orientation.z,
				s.head_pose.orientation.w};

			HandModel::HandState hs[kHandSlots];
			// The display time the hands' poses were located for (the head's for the test hands), carried with their
			// images so the pacer knows which pose each image was drawn for.
			XrTime pose_time = s.head_pose_time;
			if (fake)
			{
				for (int i = 0; i < kHandSlots; i++)
					hs[i] = FakeHand(i, eye, head_quat, s_fake_hands == 2, s.head_pose_time);
			}
			else
			{
				const VRInputSnapshot snap = GetInputSnapshot();
				if (snap.generation == 0 || !snap.actions_active)
					return 0;
				if (snap.display_time != 0)
					pose_time = snap.display_time;
				// The weapon only in first person: hands shown anywhere else (a menu) are empty, so they get no
				// recoil, flash or slide either. What it is comes with it (HandModel::HeldFor: the equipped item's kind).
				const HandModel::WeaponHeld held_weapon = HandModel::GetWeaponHeld();
				const bool gun = held_weapon.raised && CameraDriver::LookAtActive();
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
					if (d.gun)
					{
						d.held = held_weapon.held;
						d.item_id = held_weapon.item_id;
					}
				}
			}
			// Shot feedback on a pistol or a long gun (fake hands too, for headset-free captures): recoil, a muzzle
			// flash and the slide cycling after each shot, and the slide held back on an empty pistol. The laser keeps
			// the true aim. A melee weapon, something to throw or a spray can shows none.
			const HandModel::Held right_held = hs[HandModel::kRight].held;
			if (hs[HandModel::kRight].valid && hs[HandModel::kRight].gun &&
				(right_held == HandModel::Held::Pistol || right_held == HandModel::Held::LongGun))
			{
				const GameFeedback::WeaponView wv = GameFeedback::GetWeapon();
				const u64 shot_now_ms = CameraDriver::SteadyNowMs();
				const bool shot = wv.valid && wv.shot_ms != 0;
				const double age = (shot_now_ms > wv.shot_ms) ? static_cast<double>(shot_now_ms - wv.shot_ms) : 0.0;
				HandModel::HandState& d = hs[HandModel::kRight];
				d.kick = (shot && age < 300.0) ? static_cast<float>(std::exp(-age / 60.0)) : 0.0f;
				d.flash = shot && age < 45.0;
				d.slide = (shot && age < 120.0) ? static_cast<float>(1.0 - age / 120.0) : 0.0f;
				d.slide_locked = wv.valid && wv.ammo == 0 && right_held == HandModel::Held::Pistol;
			}
			// The left hand closes round a long gun's fore-end once it comes near it (OffHandSupports lets go a little
			// further out than it takes hold, so it does not flicker on the edge).
			static bool s_left_supporting = false;
			s_left_supporting = HandModel::OffHandSupports(hs[HandModel::kRight], hs[HandModel::kLeft], s_left_supporting);
			hs[HandModel::kLeft].support = s_left_supporting;
			if (!hs[0].valid && !hs[1].valid)
				return 0;
			if (!EnsureHandResources(fake, s_grip_orient))
				return 0;

			HandModel::ImpostorQuad iq[kHandSlots];
			bool want[kHandSlots] = {false, false};
			for (int i = 0; i < kHandSlots; i++)
				want[i] = hs[i].valid && HandModel::PlaceImpostor(i, hs[i], eye, &iq[i]);

			// Headset rate with [VR] HandsDisplayRate: the pacer moves each hand's image with its controller every display
			// frame, so an image is drawn again only once the view of the hand has drifted from the one it was drawn for,
			// grip or trigger moved, a shot plays, or it is old; until then its last released image is shown again (kept).
			// Otherwise every visible hand is drawn every present, its image going out at this present's pose.
			const bool keep_images = s.headset_rate && s.hands_display_rate && !s.hands_reraster_always;
			const u64 now_ms = CameraDriver::SteadyNowMs();
			bool redraw[kHandSlots] = {false, false};
			for (int i = 0; i < kHandSlots; i++)
			{
				if (!want[i])
					continue;
				const auto& img = s.hand_image[i];
				redraw[i] = !keep_images || !img.valid ||
				            HandModel::NeedsReraster(img.hs, img.eye, img.iq.centre, hs[i], eye,
				                (now_ms > img.raster_ms) ? (now_ms - img.raster_ms) : 0);
			}

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
				if (!redraw[i])
				{
					// Kept: nothing acquired or drawn; the append below shows its last image again.
					s.stat_hand_kept.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
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
				// XR_TIMEOUT_EXPIRED is a success code: the image is not ours yet, so wait again next frame.
				const XrResult wr = BudgetedWaitSwapchainImage(hc.swapchain, kHandWaitNs);
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
				// What this image shows, committed as the hand's image when its release goes through.
				auto& drawn = s.hand_image_pending[i];
				drawn.valid = true;
				drawn.iq = iq[i];
				drawn.hs = hs[i];
				for (int k = 0; k < 3; k++)
					drawn.eye[k] = eye[k];
				drawn.pose_time = pose_time;
				drawn.raster_ms = now_ms;
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
						if (submitted)
							s.stat_hand_new.fetch_add(1, std::memory_order_relaxed);
						continue;
					}
					if (XR_SUCCEEDED(LockedRelease(hc.swapchain)) && submitted)
					{
						hc.ever_released = true;
						uploaded[pending[k]] = true;
						s.stat_hand_new.fetch_add(1, std::memory_order_relaxed);
					}
				}
			}

			// Each hand goes with the image it shows and the pose that image was drawn for: drawn this present, or kept
			// (its last released image, also for a hand whose redraw got no swapchain image this present). Without kept
			// images only hands drawn this present go: last present's image on this present's quad would be from the
			// wrong place. A hand whose image is about to be released without a finished drawing in it
			// (release_pending without an upload) goes without a layer this present.
			u32 appended = 0;
			for (int i = 0; i < kHandSlots; i++)
			{
				const auto& hc = s.hand_chains[i];
				const auto& img = s.hand_image[i];
				const bool show_kept = keep_images && want[i] && !uploaded[i] && img.valid && !hc.release_pending;
				if ((!uploaded[i] && !show_kept) || layer_count >= layer_capacity)
					continue;
				const HandModel::ImpostorQuad& shown = uploaded[i] ? iq[i] : img.iq;
				XrCompositionLayerQuad& quad = hand_quads[i];
				quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
				quad.next = nullptr;
				quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
				quad.space = XRSession::GetSpace();
				quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				quad.subImage = {hc.swapchain,
					{{0, 0}, {static_cast<s32>(HandModel::kImageSize), static_cast<s32>(HandModel::kImageSize)}}, 0};
				quad.pose.orientation = {shown.quat[0], shown.quat[1], shown.quat[2], shown.quat[3]};
				quad.pose.position = {shown.centre[0], shown.centre[1], shown.centre[2]};
				quad.size = {shown.size_m, shown.size_m};
				layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
				appended++;
				// The controller pose the image was drawn for, so the pacer can move it with the hand.
				if (uploaded[i])
					AttachToHand(att, i, i, hs[i].pos, hs[i].quat, eye, pose_time);
				else
					AttachToHand(att, i, i, img.hs.pos, img.hs.quat, img.eye, img.pose_time);
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
			if (card == HudCards::kPointer)
				return {HudCards::kPointerImageSize, HudCards::kPointerImageSize};
			return {0, 0};
		}

		// The menu pointer this frame (BuildHudLayers): a beam from the pointing hand to where its aim meets the flat
		// screen, and a dot lying on the screen there.
		struct MenuPointerPlacement
		{
			bool beam = false; // a ray's hit: a PINE test hit has no aim pose to start a beam from
			float beam_pos[3] = {0.0f, 0.0f, 0.0f};
			float beam_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
			float beam_length = 0.0f;
			float dot_pos[3] = {0.0f, 0.0f, 0.0f};
			float dot_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
			float dot_size = 0.0f;
			bool on_cell = false; // MenuPointer counts it as over a cell it can steer to
			int hand = -1;
			bool tablet = false;
			bool test = false;
			// The flat screen quad they were placed on, for the pacer (PointerPublish).
			float screen_pos[3] = {0.0f, 0.0f, 0.0f};
			float screen_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
			float half_width = 0.0f;
			float half_height = 0.0f;
		};

		// The dot for a hit at hit_point on the flat screen whose orientation is screen_quat, and, from the aim pose at
		// aim_pos (null: a PINE test hit, no beam), the beam: from kBeamStartM ahead of the aim pose to the hit, along the
		// ray that made it. Seen from eye. The maths PlaceMenuPointer (GS thread) and the pacer (PlaceMenuPointerNow) share.
		void PlaceMenuPointerParts(const float hit_point[3], const float screen_quat[4], const float* aim_pos, const float eye[3],
			MenuPointerPlacement* mp)
		{
			HudCards::PlacePointer(hit_point, screen_quat, eye, mp->dot_pos, mp->dot_quat, &mp->dot_size);
			mp->beam = false;
			mp->beam_length = 0.0f;
			if (!aim_pos)
				return;
			const float dir[3] = {hit_point[0] - aim_pos[0], hit_point[1] - aim_pos[1], hit_point[2] - aim_pos[2]};
			const float dir_len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
			if (!std::isfinite(dir_len) || !(dir_len > 1.0e-4f))
				return;
			const float start[3] = {aim_pos[0] + dir[0] / dir_len * HudCards::kBeamStartM,
				aim_pos[1] + dir[1] / dir_len * HudCards::kBeamStartM, aim_pos[2] + dir[2] / dir_len * HudCards::kBeamStartM};
			mp->beam_length = dir_len - HudCards::kBeamStartM;
			mp->beam = mp->beam_length > 0.01f && HudCards::PlaceBeam(start, dir, mp->beam_length, eye, mp->beam_pos, mp->beam_quat);
		}

		// On while the MenuPointer setting is on, a menu is up in first-person play (menu mode), the first-person screen
		// is not in use, this present published a flat screen quad and a hand's aim (or a PINE test hit) meets it.
		bool PlaceMenuPointer(MenuPointerPlacement* out)
		{
			if (!HudCards::MenuPointerEnabled() || !SpatialControls::MenuMode() || s_fp_screen_was_active ||
				!s.head_pose_valid)
				return false;
			const VRScreenTransform screen = GetScreenTransform();
			if (!screen.valid)
				return false;
			const VRInputSnapshot snap = GetInputSnapshot();
			const VRScreenHit& hit = snap.screen_hit;
			if (!hit.valid || !std::isfinite(hit.point[0]) || !std::isfinite(hit.point[1]) || !std::isfinite(hit.point[2]))
				return false;
			const float eye[3] = {s.head_position_valid ? s.head_pose.position.x : 0.0f,
				s.head_position_valid ? s.head_pose.position.y : 0.0f, s.head_position_valid ? s.head_pose.position.z : 0.0f};
			const float hit_point[3] = {hit.point[0], hit.point[1], hit.point[2]};
			MenuPointerPlacement mp;
			for (int k = 0; k < 3; k++)
				mp.screen_pos[k] = screen.center.position_xyz[k];
			for (int k = 0; k < 4; k++)
				mp.screen_quat[k] = screen.center.orientation_xyzw[k];
			mp.half_width = screen.half_width;
			mp.half_height = screen.half_height;
			// The beam only from a hand's aim ray: a PINE test hit has no aim pose to start one from.
			const bool from_hand = !hit.test && hit.hand >= 0 && hit.hand < 2 && snap.hands[hit.hand].aim_pose.valid;
			const float aim_pos[3] = {from_hand ? snap.hands[hit.hand].aim_pose.position_xyz[0] : 0.0f,
				from_hand ? snap.hands[hit.hand].aim_pose.position_xyz[1] : 0.0f,
				from_hand ? snap.hands[hit.hand].aim_pose.position_xyz[2] : 0.0f};
			PlaceMenuPointerParts(hit_point, mp.screen_quat, from_hand ? aim_pos : nullptr, eye, &mp);
			mp.on_cell = MenuPointer::HoveredCell() >= 0;
			mp.hand = hit.hand;
			mp.tablet = screen.tablet;
			mp.test = hit.test;
			*out = mp;
			return true;
		}

		// The menu pointer as this present placed it, for the publish (FillPublishedSet). GS thread only; BuildHudLayers
		// clears it at its start and fills it once the beam and the dot are decided.
		PointerPublish s_pointer_pub;

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

		// HUD cards, submitted after everything else so they draw on top, except the laser sight (or menu beam), the
		// menu pointer's dot and the toast: they go in at under_from, under the layers appended from there on (the
		// hands, nearer the eye than any of them; HudCards::PlaceCardLayer has the order). A card
		// is rastered at full opacity when its content changes and uploaded only when its content or fade step
		// does; between uploads the runtime keeps showing the last released image.
		u32 BuildHudLayers(XrCompositionLayerQuad* hud_quads, const XrCompositionLayerBaseHeader** layers,
			u32& layer_count, u32 layer_capacity, u32 under_from, HandAttach* att)
		{
			s_pointer_pub = PointerPublish{};
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
					const HandModel::HandState fake = FakeHand(HandModel::kLeft, eye, head_quat, false, s.head_pose_time);
					if (fake.valid)
					{
						HudCards::PlaceWrist(fake.pos, fake.quat, eye, true, wrist_pos, wrist_quat, &wrist_facing);
						wrist_placed = true;
						AttachToHand(att, HudCards::kWrist, HandModel::kLeft, fake.pos, fake.quat, eye, s.head_pose_time);
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
						AttachToHand(att, HudCards::kWrist, HandModel::kLeft, hand_pos, hand_quat, eye,
							(snap.display_time != 0) ? snap.display_time : s.head_pose_time);
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

			// Laser sight: from the gun's muzzle along the right hand's aim while a gun is raised in first person (a
			// melee weapon, something to throw or a spray can has none: HandModel::HeldFor), with the LaserSight
			// setting on; PCSX2_VR_FAKE_HANDS=gun shows it on the fake right hand whatever the game is doing, while its
			// gun is one (PCSX2_VR_FAKE_WEAPON; the other fake hands hold no gun, so they get no beam). Placed each
			// frame from the pose the right hand is drawn with (BuildHandLayers: grip position, aim orientation), from
			// just past the muzzle of the gun drawn there (a long gun's runs on past the pistol's), so it needs no new
			// image as it moves: one unchanging image, uploaded again only while it fades in. It fades in with the
			// raised gun and goes at once with it, as the gun leaves the hand.
			static const bool s_fake_gun = [] {
				const char* v = std::getenv("PCSX2_VR_FAKE_HANDS");
				return v && std::strcmp(v, "gun") == 0;
			}();
			const HandModel::WeaponHeld laser_weapon = s_fake_hands ? HandModel::FakeWeapon() : HandModel::GetWeaponHeld();
			const bool laser_gun = laser_weapon.raised && laser_weapon.laser;
			const bool laser_raised = s_fake_hands ? (s_fake_gun && laser_gun) :
			                                         (laser_gun && CameraDriver::LookAtActive() && s.head_position_valid);
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
				XrTime laser_time = s.head_pose_time; // the display time hand_pos/hand_quat were located for
				if (s_fake_gun)
				{
					const float head_quat[4] = {s.head_pose.orientation.x, s.head_pose.orientation.y, s.head_pose.orientation.z,
						s.head_pose.orientation.w};
					const HandModel::HandState fake = FakeHand(HandModel::kRight, eye, head_quat, true, s.head_pose_time);
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
						if (snap.display_time != 0)
							laser_time = snap.display_time;
					}
				}
				// Looking straight down the beam no side of the quad faces the eye: that frame goes without it. For the
				// pistol this is HudCards::PlaceLaser's beam.
				float beam_from[3] = {0.0f, 0.0f, 0.0f};
				float beam_dir[3] = {0.0f, 0.0f, -1.0f};
				laser_placed = laser_hand &&
				               HandModel::LaserFrom(laser_weapon.held, laser_weapon.item_id, hand_pos, hand_quat,
				                   HudCards::kLaserMuzzleGapM, beam_from, beam_dir) &&
				               HudCards::PlaceBeam(beam_from, beam_dir, HudCards::kLaserLengthM, eye, laser_pos, laser_quat);
				// Only the gun's beam rides on the hand: anything else shown in the laser's slot stays unattached.
				if (laser_placed)
					AttachToHand(att, HudCards::kLaser, HandModel::kRight, hand_pos, hand_quat, eye, laser_time);
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

			// Menu pointer: with the MenuPointer setting on, while a menu is up in first-person play on a flat screen (the
			// world screen or the menu tablet), a beam from the pointing hand to where its aim meets the screen, in the
			// laser's slot only while the laser is not placed (PCSX2_VR_FAKE_HANDS=gun places it even in menus), and a dot
			// lying on the screen there, warm with a ring over a cell point-to-select can steer to. Each has its own fade.
			MenuPointerPlacement menu_pointer;
			const bool pointer_on = PlaceMenuPointer(&menu_pointer);
			const bool beam_on = pointer_on && menu_pointer.beam && !laser_placed;
			if (!beam_on)
				s.hud_beam_opacity = 0.0f;
			else
				s.hud_beam_opacity = std::min(1.0f, s.hud_beam_opacity + fade_step);
			if (!pointer_on)
				s.hud_pointer_opacity = 0.0f;
			else
				s.hud_pointer_opacity = std::min(1.0f, s.hud_pointer_opacity + fade_step);
			if (beam_on && s.hud_beam_opacity > 0.0f)
			{
				Want& wb = want[HudCards::kLaser];
				wb.on = true;
				wb.content = 2;
				wb.level = HudLevel(s.hud_beam_opacity);
			}
			if (pointer_on && s.hud_pointer_opacity > 0.0f)
			{
				Want& wp = want[HudCards::kPointer];
				wp.on = true;
				wp.content = menu_pointer.on_cell ? 2 : 1;
				wp.level = HudLevel(s.hud_pointer_opacity);
				if (!s.hud_pointer_logged)
				{
					s.hud_pointer_logged = true;
					Console.WriteLn("(VR) HUD: menu pointer shown (%s hand on the %s screen).",
						menu_pointer.test ? "PINE test" : (menu_pointer.hand == HandModel::kLeft ? "left" : "right"),
						menu_pointer.tablet ? "tablet" : "world");
				}
			}
			// With the hands at display rate the pacer places the beam and the dot again every display frame from the aim
			// located for it (PlaceMenuPointerNow); this is what it needs from this present.
			if (pointer_on)
			{
				PointerPublish& pp = s_pointer_pub;
				pp.screen_valid = true;
				for (int k = 0; k < 3; k++)
					pp.screen_pos[k] = menu_pointer.screen_pos[k];
				for (int k = 0; k < 4; k++)
					pp.screen_quat[k] = menu_pointer.screen_quat[k];
				pp.half_width = menu_pointer.half_width;
				pp.half_height = menu_pointer.half_height;
				pp.ray = !menu_pointer.test && menu_pointer.hand >= 0 && menu_pointer.hand < 2;
				pp.hand = pp.ray ? static_cast<s8>(menu_pointer.hand) : static_cast<s8>(-1);
				pp.beam = want[HudCards::kLaser].on && want[HudCards::kLaser].content == 2;
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
					else if (i == HudCards::kLaser && wv.content == 2)
						HudCards::RasterBeam(raster.pixels);
					else if (i == HudCards::kLaser)
						HudCards::RasterLaser(raster.pixels);
					else if (i == HudCards::kVignette)
						HudCards::RasterVignette(raster.pixels);
					else if (i == HudCards::kPointer)
						HudCards::RasterPointer(raster.pixels, wv.content == 2);
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
				// XR_TIMEOUT_EXPIRED is a success code: the image is not ours yet, so wait again next frame.
				const XrResult wr = BudgetedWaitSwapchainImage(hc.swapchain, kSwapchainWaitNs);
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
				// The toast's image must hold this toast's text (the previous one's would be wrong), and the laser's slot
				// the laser or the menu beam it is placed as; being a fade step behind is fine.
				if ((i == HudCards::kToast || i == HudCards::kLaser) && hc.shown_content != wv.content)
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
				else if (i == HudCards::kLaser && wv.content == 2)
				{
					// The menu beam: in the base space with the hands, from just ahead of the pointing hand to the dot.
					quad.space = XRSession::GetSpace();
					quad.pose.orientation = {menu_pointer.beam_quat[0], menu_pointer.beam_quat[1], menu_pointer.beam_quat[2],
						menu_pointer.beam_quat[3]};
					quad.pose.position = {menu_pointer.beam_pos[0], menu_pointer.beam_pos[1], menu_pointer.beam_pos[2]};
					quad.size = {HudCards::kBeamWidthM, menu_pointer.beam_length};
				}
				else if (i == HudCards::kLaser)
				{
					// In the base space with the hands, its long side along the beam (+Y toward the far end).
					quad.space = XRSession::GetSpace();
					quad.pose.orientation = {laser_quat[0], laser_quat[1], laser_quat[2], laser_quat[3]};
					quad.pose.position = {laser_pos[0], laser_pos[1], laser_pos[2]};
					quad.size = {HudCards::kLaserWidthM, HudCards::kLaserLengthM};
				}
				else if (i == HudCards::kPointer)
				{
					// The menu pointer's dot: in the base space with the screen it lies on.
					quad.space = XRSession::GetSpace();
					quad.pose.orientation = {menu_pointer.dot_quat[0], menu_pointer.dot_quat[1], menu_pointer.dot_quat[2],
						menu_pointer.dot_quat[3]};
					quad.pose.position = {menu_pointer.dot_pos[0], menu_pointer.dot_pos[1], menu_pointer.dot_pos[2]};
					quad.size = {menu_pointer.dot_size, menu_pointer.dot_size};
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
				// The laser, the dot and the toast go in under the hands (the dot over the beam, the toast over both), the
				// rest on top; layer_count < capacity, checked above.
				const XrCompositionLayerBaseHeader* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
				HudCards::PlaceCardLayer(i, layers, layer_count, under_from, layer);
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

		// The flat first-person screen's height: sized to fov_deg, a vertical FOV camera.fov writes (CameraDriver), as
		// 2 * distance * tan(fov / 2), so the picture spans exactly the angle the game drew it with; the profile's
		// screen.firstPerson height when no such FOV is known (fov_deg 0) or the screen is curved (its arc decides).
		float FirstPersonHeight(const ScreenParams& sp, float fov_deg)
		{
			const bool curved = (sp.fp_arc_deg >= 5.0f) && XRSession::HasCylinderLayer();
			const float from_fov = curved ? 0.0f : CameraDriver::ScreenHeightForFov(std::max(sp.fp_distance, 0.01f), fov_deg);
			return (from_fov > 0.0f) ? from_fov : sp.fp_height;
		}

		// The FOV that sizes the first-person screen (0: the profile's height): the one camera.fov writes now, or (target)
		// the one this arm settles to once its half-second ease is done.
		float ScreenFov(bool target)
		{
			const CameraDriver::FovState st = CameraDriver::GetFovState();
			return st.sizes_screen ? (target ? st.target_deg : st.fov_deg) : 0.0f;
		}

		// The first-person screen's geometry for a vertical FOV fov_deg (0: the profile's height): one source for the
		// placement, LogFirstPersonScreen and GetFirstPersonScreenInfo, so the log and the size auto stereo uses can never
		// disagree with the screen as placed.
		FirstPersonScreenInfo ComputeFirstPersonScreen(const ScreenParams& sp, float fov_deg)
		{
			constexpr float kDeg = 180.0f / 3.14159265f;
			FirstPersonScreenInfo info;
			info.valid = sp.has_fp;
			info.distance_m = std::max(sp.fp_distance, 0.01f);
			info.aspect = ComputeAspect();
			info.curved = (sp.fp_arc_deg >= 5.0f) && XRSession::HasCylinderLayer();
			if (info.curved)
			{
				info.width_m = info.distance_m * sp.fp_arc_deg / kDeg;
				info.height_m = info.width_m / std::max(info.aspect, 0.01f);
			}
			else
			{
				info.height_m = FirstPersonHeight(sp, fov_deg);
				info.width_m = info.height_m * info.aspect;
			}
			return info;
		}

		// Refreshes GetFirstPersonScreenInfo from the current screen parameters, aspect and the FOV this arm settles to
		// (every present), the same size the 'First-person screen' line logs: auto stereo then retargets once per FOV
		// choice, in step with the FOV's own ease (and its own ease in from zero on each arm), instead of chasing every
		// step of it. The placement sizes each frame by the FOV that frame was rendered with, which is this once eased in.
		void PublishFirstPersonScreen()
		{
			ScreenParams sp;
			{
				std::lock_guard<std::mutex> lock(s_screen_mutex);
				sp = s_screen_params;
			}
			const FirstPersonScreenInfo info = ComputeFirstPersonScreen(sp, ScreenFov(true));
			{
				std::lock_guard<std::mutex> lock(s_screen_mutex);
				s_fp_screen_info = info;
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
			if (VMManager::IsOnlineLocked())
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
			// Canted displays: half the yaw from the left view to the right one, outward positive (the left view turned
			// left, the right one right); 0 for parallel views. q and -q are the same turn.
			{
				const XrQuaternionf& a = views[0].pose.orientation;
				const XrQuaternionf& b = views[1].pose.orientation;
				const float rw = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
				const float ry = a.w * b.y - a.y * b.w - a.z * b.x + a.x * b.z;
				const float sign = (rw < 0.0f) ? -1.0f : 1.0f;
				const float cant = -std::atan2(sign * ry, sign * rw) * kDeg;
				now.cant_deg = (std::isfinite(cant) && std::abs(cant) < 30.0f) ? cant : 0.0f;
			}

			bool changed = false;
			{
				std::lock_guard<std::mutex> lock(s_eye_mutex);
				changed = !s_eye_logged.valid || std::abs(now.ipd_m - s_eye_logged.ipd_m) > 0.0005f ||
				          std::abs(now.cant_deg - s_eye_logged.cant_deg) > 0.5f;
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
			char cant[48] = "";
			if (std::abs(now.cant_deg) >= 0.05f)
				std::snprintf(cant, sizeof(cant), "; views canted out %.1f deg", now.cant_deg);
			Console.WriteLn("(VR) Eyes: IPD %.1f mm; left fov L/R/U/D %.1f/%.1f/%.1f/%.1f, right %.1f/%.1f/%.1f/%.1f; "
							"union H +-%.1f V +%.1f/-%.1f deg%s; display %.1f Hz",
				now.ipd_m * 1000.0f, now.fov_deg[0][0], now.fov_deg[0][1], now.fov_deg[0][2], now.fov_deg[0][3],
				now.fov_deg[1][0], now.fov_deg[1][1], now.fov_deg[1][2], now.fov_deg[1][3], union_h, union_up, union_down,
				cant, now.display_hz);
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

		// Edge fill ([VR] EdgeFill, on by default; needs the opaque background). Behind the first-person screen the
		// background layer shows a dim, blurred continuation of the picture instead of black, so a head turn faster than
		// the screen's margin shows more of the room rather than a black border.
		//
		// Head-locked, in the background's own slot (no extra layer): the background is a projection layer the runtime
		// draws at each eye's own view, and SteamVR is not relied on to take a projection view wider than the eye's (which
		// a world-locked fill would need to reach past the screen), nor may a quad replace it (SteamVR's own scene would
		// show around the screens again). So each eye's half of the fill holds the picture where the first-person screen
		// sits in that eye's view when the head is where the frame was rendered from (half the IPD to the side, turned by
		// any display cant), its edge texels stretched over the rest of the view. Still, it lines up with the screen; in a
		// fast turn the strip the screen uncovers shows that blurred room a few degrees off, like a coloured comfort
		// vignette, and the screen's own margin hides most of it.
		//
		// Built on the GS thread from the presented frame with GPU copies and blits only, recorded in its own command
		// buffer and submitted on the compositor's queue (no CPU readback): the frame box-filtered down to
		// kFillSmallWidth wide (a first scale of 1 to 2, then exact halvings, so it is stable and does not shimmer), dimmed
		// to half by averaging each texel with a black one (copied into the even columns of an image whose odd columns stay
		// black, then halved across), scaled back up kFillUpscale times, cut and stretched into each eye's half of a canvas,
		// and copied into the fill's swapchain. Never waits: a fill still on the GPU, or a swapchain image not back yet,
		// skips this present's update and the runtime keeps showing the last one. Black behind anything else, and on any
		// failure (the fill then stays off for the session).
		constexpr u32 kFillEyeSize = 128;    // each eye's half of the canvas, square: the runtime stretches it over the view
		constexpr u32 kFillSmallWidth = 32;  // the blur
		constexpr u32 kFillUpscale = 5;      // the cut's resolution: about half a degree
		constexpr u32 kFillMaxRegions = 9;   // per eye: the picture, its four edges stretched, its four corners
		constexpr int64_t kFillWaitNs = 1000000;
		constexpr u32 kFillBusyReport = 120; // skipped updates (the GPU behind) before the log says so, once

		struct FillPlan
		{
			u32 l0_w = 0, l0_h = 0; // the chain's first level: the frame scaled down by 1 to 2
			u32 levels = 0;         // the chain's levels (0: no plan); the last is small_w x small_h
			u32 small_w = 0, small_h = 0;
		};

		// The box-filter chain for a src_w x src_h frame: a first level small_w * 2^k wide (at most the frame's width,
		// above half of it), then halvings down to small_w (kFillSmallWidth, or the frame's width if narrower).
		FillPlan PlanFill(u32 src_w, u32 src_h)
		{
			FillPlan p;
			if (src_w < 2 || src_h < 2 || src_w > 16384 || src_h > 16384)
				return p;
			p.small_w = std::min(kFillSmallWidth, src_w);
			p.small_h = std::max(1u, static_cast<u32>(std::lround(static_cast<double>(src_h) * p.small_w / src_w)));
			u32 k = 0;
			while (k < 12 && (p.small_w << (k + 1)) <= src_w)
				k++;
			p.l0_w = p.small_w << k;
			p.l0_h = p.small_h << k;
			p.levels = k + 1;
			return p;
		}

		// One blit of the fill: a rectangle of the scaled-up picture into a rectangle of one eye's canvas (texels).
		struct FillRegion
		{
			s32 sx0, sy0, sx1, sy1;
			s32 dx0, dy0, dx1, dy1;
		};

		// One eye's view and the picture in it, as tangents: left, right, up, down (left and down negative).
		struct FillEye
		{
			float view[4];
			float picture[4];
		};

		// The picture where the first-person screen (width_m x height_m, distance_m ahead of the head) sits in eye e's view
		// with the head where its frame was rendered from: seen from half the IPD to that side, and turned by the eye's
		// outward cant (its edges exactly, the inside linearly). False when either is unusable.
		bool FillEyeTangents(int eye, const EyeInfo& eyes, float distance_m, float width_m, float height_m, FillEye* out)
		{
			constexpr float kRad = 3.14159265f / 180.0f;
			if (!std::isfinite(distance_m) || !std::isfinite(width_m) || !std::isfinite(height_m) || !(distance_m > 0.0f) ||
				!(width_m > 0.0f) || !(height_m > 0.0f))
				return false;
			for (int k = 0; k < 4; k++)
			{
				const float a = eyes.fov_deg[eye][k];
				if (!std::isfinite(a) || std::abs(a) >= 89.0f)
					return false;
				out->view[k] = std::tan(a * kRad);
			}
			const float ipd = (std::isfinite(eyes.ipd_m) && eyes.ipd_m > 0.0f && eyes.ipd_m < 0.1f) ? eyes.ipd_m : 0.0f;
			const float side = (eye == 0) ? -0.5f * ipd : 0.5f * ipd; // metres to the right of the head
			const float cant = std::isfinite(eyes.cant_deg) ? std::clamp(eyes.cant_deg, -30.0f, 30.0f) * kRad : 0.0f;
			const float turn = (eye == 0) ? cant : -cant; // the eye's yaw off the head, left positive
			const float a_l = std::atan((-0.5f * width_m - side) / distance_m) + turn;
			const float a_r = std::atan((0.5f * width_m - side) / distance_m) + turn;
			if (std::abs(a_l) >= 1.55f || std::abs(a_r) >= 1.55f)
				return false;
			out->picture[0] = std::tan(a_l);
			out->picture[1] = std::tan(a_r);
			out->picture[2] = 0.5f * height_m / distance_m;
			out->picture[3] = -0.5f * height_m / distance_m;
			return out->view[1] > out->view[0] && out->view[2] > out->view[3];
		}

		// The blits that fill one eye's cw x ch canvas from the pw x ph picture: the picture where it lands in the view
		// (cut to it), its first and last columns stretched left and right of it, its top and bottom rows above and below,
		// its corner texels into the corners. They tile the canvas exactly. 0 for unusable input.
		u32 PlanFillBlits(const FillEye& fe, u32 pw, u32 ph, u32 cw, u32 ch, FillRegion* out)
		{
			const float* v = fe.view;
			const float* p = fe.picture;
			for (int k = 0; k < 4; k++)
			{
				if (!std::isfinite(v[k]) || !std::isfinite(p[k]))
					return 0;
			}
			if (!(v[1] > v[0]) || !(v[2] > v[3]) || !(p[1] > p[0]) || !(p[2] > p[3]) || pw == 0 || ph == 0 || cw == 0 ||
				ch == 0)
				return 0;
			// The picture's rectangle in canvas texels (x right, y down from the view's top).
			const float x0 = (p[0] - v[0]) / (v[1] - v[0]) * static_cast<float>(cw);
			const float x1 = (p[1] - v[0]) / (v[1] - v[0]) * static_cast<float>(cw);
			const float y0 = (v[2] - p[2]) / (v[2] - v[3]) * static_cast<float>(ch);
			const float y1 = (v[2] - p[3]) / (v[2] - v[3]) * static_cast<float>(ch);
			const auto clamp_round = [](float f, s32 lo, s32 hi) {
				if (!(f > static_cast<float>(lo)))
					return lo;
				if (!(f < static_cast<float>(hi)))
					return hi;
				return std::clamp(static_cast<s32>(std::lround(f)), lo, hi);
			};
			const s32 icw = static_cast<s32>(cw), ich = static_cast<s32>(ch);
			const s32 ipw = static_cast<s32>(pw), iph = static_cast<s32>(ph);
			// Its edges on the canvas, rounded and clipped to it, and the picture texels between them (at least one).
			const s32 cx0 = clamp_round(x0, 0, icw), cx1 = clamp_round(x1, 0, icw);
			const s32 cy0 = clamp_round(y0, 0, ich), cy1 = clamp_round(y1, 0, ich);
			const s32 sx0 = clamp_round((static_cast<float>(cx0) - x0) / (x1 - x0) * static_cast<float>(pw), 0, ipw - 1);
			const s32 sx1 = std::max(clamp_round((static_cast<float>(cx1) - x0) / (x1 - x0) * static_cast<float>(pw), 0, ipw), sx0 + 1);
			const s32 sy0 = clamp_round((static_cast<float>(cy0) - y0) / (y1 - y0) * static_cast<float>(ph), 0, iph - 1);
			const s32 sy1 = std::max(clamp_round((static_cast<float>(cy1) - y0) / (y1 - y0) * static_cast<float>(ph), 0, iph), sy0 + 1);
			u32 n = 0;
			const auto add = [&](s32 ax0, s32 ay0, s32 ax1, s32 ay1, s32 bx0, s32 by0, s32 bx1, s32 by1) {
				if (bx1 > bx0 && by1 > by0 && ax1 > ax0 && ay1 > ay0 && n < kFillMaxRegions)
					out[n++] = FillRegion{ax0, ay0, ax1, ay1, bx0, by0, bx1, by1};
			};
			add(sx0, sy0, sx1, sy1, cx0, cy0, cx1, cy1);                 // the picture
			add(0, sy0, 1, sy1, 0, cy0, cx0, cy1);                        // left of it
			add(ipw - 1, sy0, ipw, sy1, cx1, cy0, icw, cy1);              // right of it
			add(sx0, 0, sx1, 1, cx0, 0, cx1, cy0);                        // above it
			add(sx0, iph - 1, sx1, iph, cx0, cy1, cx1, ich);              // below it
			add(0, 0, 1, 1, 0, 0, cx0, cy0);                              // the corners
			add(ipw - 1, 0, ipw, 1, cx1, 0, icw, cy0);
			add(0, iph - 1, 1, iph, 0, cy1, cx0, ich);
			add(ipw - 1, iph - 1, ipw, iph, cx1, cy1, icw, ich);
			return n;
		}

		// A device image the fill blits through, in GENERAL layout once laid out (UNORM: the frame's bytes as they are,
		// so the blur averages gamma-encoded values like the screen shows them).
		struct FillImage
		{
			VkImage image = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
		};

		struct
		{
			bool enabled = false;  // [VR] EdgeFill with the opaque background (Initialize)
			bool disabled = false; // a failure turned it off for the session
			// The swapchain's last released image is a fill from the present first-person spell: written on the GS thread,
			// read by the pacer (with the swapchain handle) under submit_mutex as it builds the background.
			std::atomic_bool live{false};
			XrSwapchain swapchain = XR_NULL_HANDLE; // 2 * kFillEyeSize x kFillEyeSize, both eyes side by side
			std::vector<XrSwapchainImageVulkan2KHR> images;
			bool wait_pending = false;
			uint32_t pending_index = 0;
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			VkFence fence = VK_NULL_HANDLE;
			bool fence_submitted = false;
			// For a src_w x src_h frame (plan): the box-filter chain (mip levels), misc (the dim's interleave at (0, 0),
			// 2 * small_w x small_h, and the canvas below it at (0, small_h)), the dimmed small picture and its upscale.
			FillPlan plan;
			u32 src_w = 0, src_h = 0;
			FillImage chain, misc, dimmed, up;
			bool images_ready = false; // in GENERAL, misc cleared to black
			bool live_logged = false;
			u32 busy_skips = 0;
			bool busy_logged = false;
		} s_fill;

		void DestroyFillImages(const Internal::VulkanHandles& h)
		{
			FillImage* const all[4] = {&s_fill.chain, &s_fill.misc, &s_fill.dimmed, &s_fill.up};
			for (FillImage* img : all)
			{
				if (h.IsValid())
				{
					if (img->image != VK_NULL_HANDLE)
						vkDestroyImage(h.device, img->image, nullptr);
					if (img->memory != VK_NULL_HANDLE)
						vkFreeMemory(h.device, img->memory, nullptr);
				}
				*img = FillImage{};
			}
			s_fill.plan = FillPlan{};
			s_fill.src_w = 0;
			s_fill.src_h = 0;
			s_fill.images_ready = false;
		}

		// Shutdown (the pacer gone): every fill resource. With the compositor abandoned the swapchain is only forgotten.
		void DestroyFillResources()
		{
			s_fill.live.store(false, std::memory_order_release);
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			if (h.IsValid() && s_fill.fence != VK_NULL_HANDLE && s_fill.fence_submitted &&
				vkWaitForFences(h.device, 1, &s_fill.fence, VK_TRUE, kTeardownFenceNs) == VK_TIMEOUT)
			{
				Console.Warning("(VR) Edge fill: its blit fence was still busy after 250 ms during teardown.");
			}
			s_fill.fence_submitted = false;
			if (s_fill.swapchain != XR_NULL_HANDLE && !s.abandoned.load(std::memory_order_acquire))
				xrDestroySwapchain(s_fill.swapchain);
			s_fill.swapchain = XR_NULL_HANDLE;
			s_fill.images.clear();
			s_fill.wait_pending = false;
			s_fill.pending_index = 0;
			DestroyFillImages(h);
			if (h.IsValid())
			{
				if (s_fill.fence != VK_NULL_HANDLE)
					vkDestroyFence(h.device, s_fill.fence, nullptr);
				if (s_fill.cmd != VK_NULL_HANDLE && s.cmd_pool != VK_NULL_HANDLE)
					vkFreeCommandBuffers(h.device, s.cmd_pool, 1, &s_fill.cmd);
			}
			s_fill.fence = VK_NULL_HANDLE;
			s_fill.cmd = VK_NULL_HANDLE;
		}

		// Any failure: the fill stays off for the session and the background black.
		void FailFill(const char* what, int code)
		{
			s_fill.disabled = true;
			s_fill.live.store(false, std::memory_order_release);
			Console.Warning("(VR) Edge fill: %s failed (%d); the background stays black for this session.", what, code);
		}

		bool CreateFillImage(const Internal::VulkanHandles& h, u32 width, u32 height, u32 levels, FillImage* img)
		{
			VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			ci.imageType = VK_IMAGE_TYPE_2D;
			ci.format = VK_FORMAT_R8G8B8A8_UNORM;
			ci.extent = {width, height, 1};
			ci.mipLevels = levels;
			ci.arrayLayers = 1;
			ci.samples = VK_SAMPLE_COUNT_1_BIT;
			ci.tiling = VK_IMAGE_TILING_OPTIMAL;
			ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkResult vr = vkCreateImage(h.device, &ci, nullptr, &img->image);
			if (vr != VK_SUCCESS)
			{
				img->image = VK_NULL_HANDLE;
				FailFill("vkCreateImage", static_cast<int>(vr));
				return false;
			}
			VkMemoryRequirements mr = {};
			vkGetImageMemoryRequirements(h.device, img->image, &mr);
			VkPhysicalDeviceMemoryProperties mp = {};
			vkGetPhysicalDeviceMemoryProperties(h.physical_device, &mp);
			// Device-local if there is one that fits, else any that does.
			u32 type = UINT32_MAX;
			for (int pass = 0; pass < 2 && type == UINT32_MAX; pass++)
			{
				for (u32 i = 0; i < mp.memoryTypeCount; i++)
				{
					if ((mr.memoryTypeBits & (1u << i)) &&
						(pass == 1 || (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
					{
						type = i;
						break;
					}
				}
			}
			if (type == UINT32_MAX)
			{
				FailFill("image memory type lookup", 0);
				return false;
			}
			VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			mai.allocationSize = mr.size;
			mai.memoryTypeIndex = type;
			vr = vkAllocateMemory(h.device, &mai, nullptr, &img->memory);
			if (vr != VK_SUCCESS)
			{
				img->memory = VK_NULL_HANDLE;
				FailFill("vkAllocateMemory", static_cast<int>(vr));
				return false;
			}
			vr = vkBindImageMemory(h.device, img->image, img->memory, 0);
			if (vr != VK_SUCCESS)
			{
				FailFill("vkBindImageMemory", static_cast<int>(vr));
				return false;
			}
			return true;
		}

		// The fill's command buffer, fence and swapchain (once), and its images for a src_w x src_h frame (again when the
		// frame's size changes; the caller has made sure the previous fill is off the GPU). False to skip this present;
		// a failure also turns the fill off (FailFill).
		bool EnsureFillResources(const Internal::VulkanHandles& h, u32 src_w, u32 src_h)
		{
			if (s_fill.cmd == VK_NULL_HANDLE)
			{
				VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
				cai.commandPool = s.cmd_pool;
				cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
				cai.commandBufferCount = 1;
				const VkResult vr = vkAllocateCommandBuffers(h.device, &cai, &s_fill.cmd);
				if (vr != VK_SUCCESS)
				{
					s_fill.cmd = VK_NULL_HANDLE;
					FailFill("vkAllocateCommandBuffers", static_cast<int>(vr));
					return false;
				}
			}
			if (s_fill.fence == VK_NULL_HANDLE)
			{
				VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
				const VkResult vr = vkCreateFence(h.device, &fci, nullptr, &s_fill.fence);
				if (vr != VK_SUCCESS)
				{
					s_fill.fence = VK_NULL_HANDLE;
					FailFill("vkCreateFence", static_cast<int>(vr));
					return false;
				}
				s_fill.fence_submitted = false;
			}
			if (s_fill.swapchain == XR_NULL_HANDLE)
			{
				// The screen's format: the canvas's bytes are copied in as they are, like the screen's.
				XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
				ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
				ci.format = static_cast<int64_t>(s.swapchain_format);
				ci.sampleCount = 1;
				ci.width = 2 * kFillEyeSize;
				ci.height = kFillEyeSize;
				ci.faceCount = 1;
				ci.arraySize = 1;
				ci.mipCount = 1;
				XrResult res = xrCreateSwapchain(XRSession::GetSession(), &ci, &s_fill.swapchain);
				if (XR_FAILED(res))
				{
					s_fill.swapchain = XR_NULL_HANDLE;
					FailFill("xrCreateSwapchain", static_cast<int>(res));
					return false;
				}
				uint32_t count = 0;
				res = xrEnumerateSwapchainImages(s_fill.swapchain, 0, &count, nullptr);
				if (XR_FAILED(res) || count == 0)
				{
					FailFill("xrEnumerateSwapchainImages", static_cast<int>(res));
					return false;
				}
				s_fill.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
				res = xrEnumerateSwapchainImages(s_fill.swapchain, count, &count,
					reinterpret_cast<XrSwapchainImageBaseHeader*>(s_fill.images.data()));
				if (XR_FAILED(res))
				{
					FailFill("xrEnumerateSwapchainImages", static_cast<int>(res));
					return false;
				}
				s_fill.wait_pending = false;
			}
			if (s_fill.chain.image != VK_NULL_HANDLE && s_fill.src_w == src_w && s_fill.src_h == src_h)
				return true;

			DestroyFillImages(h);
			const FillPlan p = PlanFill(src_w, src_h);
			if (p.levels == 0)
				return false;
			if (!CreateFillImage(h, p.l0_w, p.l0_h, p.levels, &s_fill.chain) ||
				!CreateFillImage(h, std::max(2 * p.small_w, 2 * kFillEyeSize), p.small_h + kFillEyeSize, 1, &s_fill.misc) ||
				!CreateFillImage(h, p.small_w, p.small_h, 1, &s_fill.dimmed) ||
				!CreateFillImage(h, p.small_w * kFillUpscale, p.small_h * kFillUpscale, 1, &s_fill.up))
			{
				return false;
			}
			s_fill.plan = p;
			s_fill.src_w = src_w;
			s_fill.src_h = src_h;
			s_fill.images_ready = false;
			Console.WriteLn("(VR) Edge fill: blur chain for a %ux%u frame: %ux%u, %u level(s) down to %ux%u, dimmed and "
							"scaled up to %ux%u.",
				src_w, src_h, p.l0_w, p.l0_h, p.levels, p.small_w, p.small_h, p.small_w * kFillUpscale,
				p.small_h * kFillUpscale);
			return true;
		}

		// Everything recorded so far reads and writes before anything after it (the fill's images stay in GENERAL).
		void FillBarrier(VkCommandBuffer cmd)
		{
			VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
			mb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
			mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0,
				nullptr);
		}

		VkImageBlit FillBlit(u32 src_level, s32 sx0, s32 sy0, s32 sx1, s32 sy1, u32 dst_level, s32 dx0, s32 dy0, s32 dx1,
			s32 dy1)
		{
			VkImageBlit b = {};
			b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, src_level, 0, 1};
			b.srcOffsets[0] = {sx0, sy0, 0};
			b.srcOffsets[1] = {sx1, sy1, 1};
			b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dst_level, 0, 1};
			b.dstOffsets[0] = {dx0, dy0, 0};
			b.dstOffsets[1] = {dx1, dy1, 1};
			return b;
		}

		// This present's fill (GS thread, after the screen copies and before the layers are published): from the frame
		// src (its first layer), for the flat first-person screen as placed this present. Clears the fill's live flag
		// when it cannot be built; leaves it as it is when this present's update is only skipped (the last one stays).
		void UpdateEdgeFill(GSTextureVK* src, float distance_m, float width_m, float height_m)
		{
			if (!s_fill.enabled || s_fill.disabled || !s.bg_ready)
				return;
			const EyeInfo eyes = GetEyeInfo();
			FillEye fe[2];
			if (!eyes.valid || !FillEyeTangents(0, eyes, distance_m, width_m, height_m, &fe[0]) ||
				!FillEyeTangents(1, eyes, distance_m, width_m, height_m, &fe[1]))
			{
				s_fill.live.store(false, std::memory_order_release);
				return;
			}
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			if (!h.IsValid() || s.cmd_pool == VK_NULL_HANDLE)
				return;
			const u32 src_w = static_cast<u32>(src->GetWidth());
			const u32 src_h = static_cast<u32>(src->GetHeight());

			// The last fill must be off the GPU before its images are written again: never wait for it.
			if (s_fill.fence_submitted)
			{
				const VkResult fr = vkGetFenceStatus(h.device, s_fill.fence);
				if (fr == VK_NOT_READY)
				{
					if (++s_fill.busy_skips == kFillBusyReport && !s_fill.busy_logged)
					{
						s_fill.busy_logged = true;
						Console.WriteLn("(VR) Edge fill: %u updates skipped so far with the last one still on the GPU (the "
										"fill then shows the one before).",
							kFillBusyReport);
					}
					return;
				}
				if (fr != VK_SUCCESS)
				{
					FailFill("vkGetFenceStatus", static_cast<int>(fr));
					return;
				}
				vkResetFences(h.device, 1, &s_fill.fence);
				s_fill.fence_submitted = false;
			}
			if (!EnsureFillResources(h, src_w, src_h))
				return;

			const FillPlan& p = s_fill.plan;
			const u32 up_w = p.small_w * kFillUpscale;
			const u32 up_h = p.small_h * kFillUpscale;
			FillRegion regions[2][kFillMaxRegions];
			u32 counts[2] = {0, 0};
			for (int e = 0; e < 2; e++)
				counts[e] = PlanFillBlits(fe[e], up_w, up_h, kFillEyeSize, kFillEyeSize, regions[e]);
			if (counts[0] == 0 || counts[1] == 0)
			{
				s_fill.live.store(false, std::memory_order_release);
				return;
			}

			uint32_t index = 0;
			if (s_fill.wait_pending)
			{
				index = s_fill.pending_index;
			}
			else
			{
				const XrResult ar = LockedAcquire(s_fill.swapchain, &index);
				if (XR_FAILED(ar) || index >= s_fill.images.size())
				{
					FailFill("xrAcquireSwapchainImage", static_cast<int>(ar));
					return;
				}
				s_fill.pending_index = index;
				s_fill.wait_pending = true;
			}
			// XR_TIMEOUT_EXPIRED is a success code: the image is not ours yet; the next present waits on it again.
			const XrResult wr = BudgetedWaitSwapchainImage(s_fill.swapchain, kFillWaitNs);
			if (wr == XR_TIMEOUT_EXPIRED)
				return;
			if (XR_FAILED(wr))
			{
				FailFill("xrWaitSwapchainImage", static_cast<int>(wr));
				return;
			}
			s_fill.wait_pending = false;
			const VkImage dst = s_fill.images[index].image;

			// From here the waited image must go back to the runtime whatever happens.
			const VkCommandBuffer cmd = s_fill.cmd;
			VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			VkResult vr = vkBeginCommandBuffer(cmd, &bi);
			if (vr != VK_SUCCESS)
			{
				LockedRelease(s_fill.swapchain);
				FailFill("vkBeginCommandBuffer", static_cast<int>(vr));
				return;
			}
			const VkImage chain = s_fill.chain.image;
			const VkImage misc = s_fill.misc.image;
			const VkImage dimmed = s_fill.dimmed.image;
			const VkImage up = s_fill.up.image;
			if (!s_fill.images_ready)
			{
				// First use: every image to GENERAL for good, misc cleared to black (the interleave's odd columns are
				// never written again, so they stay black).
				const VkImage imgs[4] = {chain, misc, dimmed, up};
				const u32 levels[4] = {p.levels, 1, 1, 1};
				VkImageMemoryBarrier ib[4];
				for (int i = 0; i < 4; i++)
				{
					ib[i] = VkImageMemoryBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
					ib[i].srcAccessMask = 0;
					ib[i].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
					ib[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
					ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
					ib[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					ib[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					ib[i].image = imgs[i];
					ib[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels[i], 0, 1};
				}
				vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
					nullptr, 4, ib);
				VkClearColorValue black = {};
				black.float32[3] = 1.0f;
				const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
				vkCmdClearColorImage(cmd, misc, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
				s_fill.images_ready = true;
			}
			// After whatever the last fill read and wrote (an earlier submission on this queue) and the clear.
			FillBarrier(cmd);
			src->TransitionToLayout(cmd, GSTextureVK::Layout::TransferSrc);

			// 1. The frame to the chain's first level (a scale of 1 to 2: every texel counts).
			const s32 sw = static_cast<s32>(src_w), sh = static_cast<s32>(src_h);
			const s32 smw = static_cast<s32>(p.small_w), smh = static_cast<s32>(p.small_h);
			VkImageBlit b = FillBlit(0, 0, 0, sw, sh, 0, 0, 0, static_cast<s32>(p.l0_w), static_cast<s32>(p.l0_h));
			vkCmdBlitImage(cmd, src->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, chain, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
				VK_FILTER_LINEAR);
			// 2. Exact halvings, each texel the mean of four.
			for (u32 lvl = 1; lvl < p.levels; lvl++)
			{
				FillBarrier(cmd);
				b = FillBlit(lvl - 1, 0, 0, static_cast<s32>(std::max(1u, p.l0_w >> (lvl - 1))),
					static_cast<s32>(std::max(1u, p.l0_h >> (lvl - 1))), lvl, 0, 0, static_cast<s32>(std::max(1u, p.l0_w >> lvl)),
					static_cast<s32>(std::max(1u, p.l0_h >> lvl)));
				vkCmdBlitImage(cmd, chain, VK_IMAGE_LAYOUT_GENERAL, chain, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
			}
			// 3. The dim: each small column into an even column of the interleave (its odd columns black).
			FillBarrier(cmd);
			VkImageCopy cols[kFillSmallWidth] = {};
			for (u32 x = 0; x < p.small_w; x++)
			{
				cols[x].srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, p.levels - 1, 0, 1};
				cols[x].srcOffset = {static_cast<s32>(x), 0, 0};
				cols[x].dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				cols[x].dstOffset = {static_cast<s32>(2 * x), 0, 0};
				cols[x].extent = {1, p.small_h, 1};
			}
			vkCmdCopyImage(cmd, chain, VK_IMAGE_LAYOUT_GENERAL, misc, VK_IMAGE_LAYOUT_GENERAL, p.small_w, cols);
			// 4. Halved across: each texel sampled exactly between its own and the black beside it, so half as bright.
			FillBarrier(cmd);
			b = FillBlit(0, 0, 0, 2 * smw, smh, 0, 0, 0, smw, smh);
			vkCmdBlitImage(cmd, misc, VK_IMAGE_LAYOUT_GENERAL, dimmed, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
			// 5. Scaled back up, smooth, for a fine cut.
			FillBarrier(cmd);
			b = FillBlit(0, 0, 0, smw, smh, 0, 0, 0, static_cast<s32>(up_w), static_cast<s32>(up_h));
			vkCmdBlitImage(cmd, dimmed, VK_IMAGE_LAYOUT_GENERAL, up, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
			// 6. Each eye's half of the canvas (below the interleave in misc): the picture cut to that eye's view, its
			// edges stretched past it.
			FillBarrier(cmd);
			VkImageBlit eb[2 * kFillMaxRegions];
			u32 nb = 0;
			for (int e = 0; e < 2; e++)
			{
				const s32 ox = e * static_cast<s32>(kFillEyeSize);
				for (u32 i = 0; i < counts[e]; i++)
				{
					const FillRegion& r = regions[e][i];
					eb[nb++] = FillBlit(0, r.sx0, r.sy0, r.sx1, r.sy1, 0, ox + r.dx0, smh + r.dy0, ox + r.dx1, smh + r.dy1);
				}
			}
			vkCmdBlitImage(cmd, up, VK_IMAGE_LAYOUT_GENERAL, misc, VK_IMAGE_LAYOUT_GENERAL, nb, eb, VK_FILTER_LINEAR);
			// 7. The canvas into the swapchain image, byte for byte.
			FillBarrier(cmd);
			ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkImageCopy canvas = {};
			canvas.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			canvas.srcOffset = {0, smh, 0};
			canvas.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			canvas.dstOffset = {0, 0, 0};
			canvas.extent = {2 * kFillEyeSize, kFillEyeSize, 1};
			vkCmdCopyImage(cmd, misc, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &canvas);
			ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

			vr = vkEndCommandBuffer(cmd);
			if (vr != VK_SUCCESS)
			{
				LockedRelease(s_fill.swapchain);
				FailFill("vkEndCommandBuffer", static_cast<int>(vr));
				return;
			}
			VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
			si.commandBufferCount = 1;
			si.pCommandBuffers = &cmd;
			vr = LockedQueueSubmit(h.queue, si, s_fill.fence);
			if (vr != VK_SUCCESS)
			{
				LockedRelease(s_fill.swapchain);
				FailFill("vkQueueSubmit", static_cast<int>(vr));
				return;
			}
			s_fill.fence_submitted = true;
			const XrResult rr = LockedRelease(s_fill.swapchain);
			if (XR_FAILED(rr))
			{
				FailFill("xrReleaseSwapchainImage", static_cast<int>(rr));
				return;
			}
			s_fill.live.store(true, std::memory_order_release);
			if (!s_fill.live_logged)
			{
				s_fill.live_logged = true;
				constexpr float kDeg = 180.0f / 3.14159265f;
				Console.WriteLn("(VR) Edge fill: on behind the first-person screen: head-locked, %ux%u per eye, from a %ux%u "
								"box blur of the %ux%u frame at half brightness; picture +-%.1f x +-%.1f deg at %.2f m; %u + "
								"%u blit region(s) for the eyes.",
					kFillEyeSize, kFillEyeSize, p.small_w, p.small_h, src_w, src_h,
					std::atan(0.5f * width_m / distance_m) * kDeg, std::atan(0.5f * height_m / distance_m) * kDeg, distance_m,
					counts[0], counts[1]);
			}
		}

		// PCSX2_VR_SELFTEST: the edge fill's plan (the chain's sizes, each eye's picture, the blits tiling the canvas).
		void SelfTestEdgeFill()
		{
			int pass = 0, fail = 0;
			const auto check = [&](bool ok, const char* name) {
				if (ok)
					pass++;
				else
				{
					fail++;
					Console.WriteLn("(VR) Edge fill self-test FAIL: %s", name);
				}
			};
			const auto plan_is = [](const FillPlan& p, u32 l0w, u32 l0h, u32 levels, u32 sw, u32 sh) {
				return p.l0_w == l0w && p.l0_h == l0h && p.levels == levels && p.small_w == sw && p.small_h == sh;
			};
			check(plan_is(PlanFill(1536, 1344), 1024, 896, 6, 32, 28), "3x frame: 1.5 down, then five halvings");
			check(plan_is(PlanFill(512, 448), 512, 448, 5, 32, 28), "1x frame: four halvings");
			check(plan_is(PlanFill(640, 448), 512, 352, 5, 32, 22), "640 wide: 1.25 down, then four halvings");
			check(plan_is(PlanFill(24, 20), 24, 20, 1, 24, 20), "a frame narrower than the blur");
			check(PlanFill(1, 448).levels == 0 && PlanFill(512, 0).levels == 0, "no plan for an empty frame");

			// The blits tile the canvas exactly and stay inside the picture.
			const auto tiles = [](const FillRegion* g, u32 count, s32 cw, s32 ch, s32 pw, s32 ph) {
				u64 area = 0;
				for (u32 i = 0; i < count; i++)
				{
					if (g[i].dx0 < 0 || g[i].dy0 < 0 || g[i].dx1 > cw || g[i].dy1 > ch || g[i].dx1 <= g[i].dx0 ||
						g[i].dy1 <= g[i].dy0 || g[i].sx0 < 0 || g[i].sy0 < 0 || g[i].sx1 > pw || g[i].sy1 > ph ||
						g[i].sx1 <= g[i].sx0 || g[i].sy1 <= g[i].sy0)
						return false;
					area += static_cast<u64>(g[i].dx1 - g[i].dx0) * static_cast<u64>(g[i].dy1 - g[i].dy0);
					for (u32 j = 0; j < i; j++)
					{
						if (!(g[i].dx1 <= g[j].dx0 || g[j].dx1 <= g[i].dx0 || g[i].dy1 <= g[j].dy0 || g[j].dy1 <= g[i].dy0))
							return false;
					}
				}
				return area == static_cast<u64>(cw) * static_cast<u64>(ch);
			};
			FillRegion r[kFillMaxRegions];
			// Covering the view (Auto FOV): one cut, the middle of the picture.
			FillEye cover = {{-1.0f, 1.0f, 1.0f, -1.0f}, {-1.5f, 1.5f, 1.2f, -1.2f}};
			u32 n = PlanFillBlits(cover, 160, 140, 128, 128, r);
			check(n == 1 && tiles(r, n, 128, 128, 160, 140) && r[0].sx0 == 27 && r[0].sx1 == 133 && r[0].sy0 == 12 &&
					  r[0].sy1 == 128,
				"a picture covering the view: one cut");
			// Inside the view (a narrower profile FOV): the picture and eight stretched edges and corners.
			FillEye inside = {{-1.5f, 1.5f, 1.5f, -1.5f}, {-1.0f, 1.0f, 0.8f, -0.8f}};
			n = PlanFillBlits(inside, 160, 140, 128, 128, r);
			check(n == 9 && tiles(r, n, 128, 128, 160, 140) && r[0].sx0 == 0 && r[0].sx1 == 160 && r[0].sy0 == 0 &&
					  r[0].sy1 == 140,
				"a picture inside the view: nine blits");
			// Off to one side and above, and an asymmetric eye.
			FillEye off = {{-1.0f, 1.0f, 1.0f, -1.0f}, {-3.0f, -2.0f, 3.0f, 2.0f}};
			n = PlanFillBlits(off, 160, 140, 128, 128, r);
			check(n >= 1 && tiles(r, n, 128, 128, 160, 140), "a picture outside the view: its corner stretched over it");
			FillEye asym = {{-1.2f, 0.9f, 1.1f, -1.3f}, {-1.1f, 1.3f, 1.0f, -1.0f}};
			n = PlanFillBlits(asym, 160, 140, 128, 128, r);
			check(n >= 2 && tiles(r, n, 128, 128, 160, 140), "an asymmetric eye");
			FillEye bad = {{-1.0f, 1.0f, 1.0f, -1.0f}, {std::numeric_limits<float>::quiet_NaN(), 1.0f, 1.0f, -1.0f}};
			check(PlanFillBlits(bad, 160, 140, 128, 128, r) == 0 && PlanFillBlits(cover, 0, 140, 128, 128, r) == 0,
				"no blits for unusable input");

			// Each eye's picture: the screen seen from half the IPD to that side (and turned by a cant).
			EyeInfo eyes;
			eyes.valid = true;
			for (int e = 0; e < 2; e++)
			{
				eyes.fov_deg[e][0] = -45.0f;
				eyes.fov_deg[e][1] = 45.0f;
				eyes.fov_deg[e][2] = 45.0f;
				eyes.fov_deg[e][3] = -45.0f;
			}
			FillEye l = {};
			FillEye rt = {};
			check(FillEyeTangents(0, eyes, 1.5f, 4.767f, 3.575f, &l) && std::abs(l.picture[0] + 1.589f) < 2e-3f &&
					  std::abs(l.picture[1] - 1.589f) < 2e-3f && std::abs(l.picture[2] - 1.1917f) < 1e-3f &&
					  std::abs(l.view[0] + 1.0f) < 1e-4f,
				"no IPD: the screen's own tangents");
			eyes.ipd_m = 0.064f;
			check(FillEyeTangents(0, eyes, 1.5f, 4.767f, 3.575f, &l) && FillEyeTangents(1, eyes, 1.5f, 4.767f, 3.575f, &rt) &&
					  std::abs(l.picture[0] - (-1.589f + 0.02133f)) < 2e-3f && std::abs(rt.picture[1] - (1.589f - 0.02133f)) < 2e-3f,
				"the left eye sees the screen further right, the right eye further left");
			eyes.ipd_m = 0.0f;
			eyes.cant_deg = 5.0f;
			check(FillEyeTangents(0, eyes, 1.5f, 3.0f, 2.0f, &l) && std::abs(l.picture[1] - std::tan(std::atan(1.0f) + 5.0f * 3.14159265f / 180.0f)) < 1e-3f,
				"an outward cant turns the left eye's picture right");
			check(!FillEyeTangents(0, eyes, 0.0f, 3.0f, 2.0f, &l), "no picture at zero distance");
			Console.WriteLn("(VR) Edge fill self-test: %d check(s) passed, %d failed.", pass, fail);
		}

		// The background under every other layer: a projection layer over both eyes' views that shows the one image
		// cleared to black at Initialize, or (fill: behind the first-person screen, with a live edge fill) each eye's half
		// of the edge fill.
		void MakeBackgroundLayer(const XrView* views, XrCompositionLayerProjectionView* pv, XrCompositionLayerProjection* layer,
			bool fill)
		{
			fill = fill && s_fill.swapchain != XR_NULL_HANDLE;
			for (u32 i = 0; i < 2; i++)
			{
				pv[i] = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
				pv[i].pose = views[i].pose;
				pv[i].fov = views[i].fov;
				if (fill)
				{
					pv[i].subImage.swapchain = s_fill.swapchain;
					pv[i].subImage.imageRect.offset = {static_cast<s32>(i * kFillEyeSize), 0};
					pv[i].subImage.imageRect.extent = {static_cast<s32>(kFillEyeSize), static_cast<s32>(kFillEyeSize)};
				}
				else
				{
					pv[i].subImage.swapchain = s.bg_swapchain;
					pv[i].subImage.imageRect.offset = {0, 0};
					pv[i].subImage.imageRect.extent = {static_cast<s32>(kBackgroundSize), static_cast<s32>(kBackgroundSize)};
				}
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

		// What the first-person screen was last logged with: the vertical FOV (0: the profile's height), why that FOV, and
		// whether the headset's field of view was known, so a change while it is up (the headset's eyes arriving after the
		// arm) is logged again with its coverage. GS thread.
		float s_fp_logged_fov = -1.0f;
		const char* s_fp_logged_source = nullptr;
		bool s_fp_logged_eyes = false;

		// When the first-person screen turns on (and when the FOV sizing it changes while it is up): the camera FOV, how
		// much of the view the screen covers against the headset's field of view, and its pixel density at the centre
		// against the headset's. Logged for the FOV this arm settles to, so the margin is the one that holds once the
		// half-second ease is done.
		void LogFirstPersonScreen(const ScreenParams& sp)
		{
			constexpr float kDeg = 180.0f / 3.14159265f;
			const CameraDriver::FovState fov = CameraDriver::GetFovState();
			const float fov_deg = fov.sizes_screen ? fov.target_deg : 0.0f;
			s_fp_logged_fov = fov_deg;
			s_fp_logged_source = fov.sizes_screen ? fov.source : nullptr;
			char fov_text[128] = "sized by the profile";
			if (fov_deg > 0.0f)
				std::snprintf(fov_text, sizeof(fov_text), "camera FOV %.1f deg (%s)", fov_deg, fov.source);
			const FirstPersonScreenInfo geo = ComputeFirstPersonScreen(sp, fov_deg);
			const float d = geo.distance_m;
			const bool curved = geo.curved;
			const float width_m = geo.width_m;
			const float height_m = geo.height_m;
			const float half_w_deg = curved ? (0.5f * sp.fp_arc_deg) : (std::atan(0.5f * width_m / d) * kDeg);
			const float half_h_deg = std::atan(0.5f * height_m / d) * kDeg;
			const u32 w_px = s.swapchain_width;
			const u32 h_px = s.swapchain_height;
			const float px_deg_h = curved ? (static_cast<float>(w_px) / std::max(sp.fp_arc_deg, 1.0f)) :
			                                (static_cast<float>(w_px) * d / std::max(width_m, 0.001f) / kDeg);
			const float px_deg_v = static_cast<float>(h_px) * d / std::max(height_m, 0.001f) / kDeg;

			const EyeInfo eyes = GetEyeInfo();
			s_fp_logged_eyes = eyes.valid;
			if (!eyes.valid)
			{
				Console.WriteLn("(VR) First-person screen: +-%.1f x +-%.1f deg at %.2f m (%.2f x %.2f m), %s; headset field "
								"of view not known yet; centre %.1f x %.1f px/deg from %ux%u",
					half_w_deg, half_h_deg, d, width_m, height_m, fov_text, px_deg_h, px_deg_v, w_px, h_px);
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
			Console.WriteLn("(VR) First-person screen: +-%.1f x +-%.1f deg at %.2f m (%.2f x %.2f m), %s; margin vs "
							"headset: yaw %+.1f up %+.1f down %+.1f deg; centre %.1f x %.1f px/deg from %ux%u; headset about "
							"%.1f px/deg",
				half_w_deg, half_h_deg, d, width_m, height_m, fov_text, half_w_deg - union_h, half_h_deg - union_up,
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
			// The controller pose each hand and each HUD card was placed for, by slot (HandAttach).
			const HandAttach* hand_att;
			const HandAttach* hud_att;
			// How the menu pointer's beam and dot were placed this present (null: no pointer parts marked).
			const PointerPublish* pointer = nullptr;
		};

		// Copies each pointed-to layer by value, in order, with its role and, for a hand or a HUD card that rides on a
		// controller, the pose it was placed for.
		void FillPublishedSet(PublishedSet* out, const XrCompositionLayerBaseHeader* const* layers, u32 layer_count,
			const LayerSources& src)
		{
			out->count = 0;
			out->pointer = src.pointer ? *src.pointer : PointerPublish{};
			for (u32 i = 0; i < layer_count && out->count < kLayerCapacity; i++)
			{
				const void* const p = layers[i];
				PublishedSet::Entry& e = out->e[out->count];
				e.att = HandAttach{};
				e.pointer = kPointerPartNone;
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
						if (src.hand_att)
							e.att = src.hand_att[k];
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
						if (src.hud_att)
							e.att = src.hud_att[k];
						// The dot's slot holds only the dot; the laser's holds the menu beam when this present put it there.
						if (k == HudCards::kPointer)
							e.pointer = kPointerPartDot;
						else if (k == HudCards::kLaser && out->pointer.beam)
							e.pointer = kPointerPartBeam;
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
		// The set a present publishes, given the set published before it (prev): with keep_screen, prev's Screen entries
		// stay (their images were not replaced this present) and this present's other entries are added.
		PublishedSet BuildPublishedSet(const XrCompositionLayerBaseHeader* const* layers, u32 layer_count, const LayerSources& src,
			bool keep_screen, bool fp_screen, const XrVector3f& fp_head, u64 fp_image, const PublishedSet& prev)
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
				// The menu pointer's beam and dot are this present's entries, placed on this present's screen quad.
				merged.pointer = next.pointer;
				merged.publish_display_time = prev.publish_display_time;
				merged.fp_screen = prev.fp_screen;
				merged.fp_head = prev.fp_head;
				merged.turn_rate = prev.fp_screen ? turn_rate : 0.0f;
				merged.fp_image = prev.fp_image;
				merged.serial = prev.serial + 1;
				return merged;
			}
			next.serial = prev.serial + 1;
			return next;
		}

		void PublishLayers(const XrCompositionLayerBaseHeader* const* layers, u32 layer_count, const LayerSources& src,
			bool keep_screen, bool fp_screen, const XrVector3f& fp_head, u64 fp_image)
		{
			s.stat_gs_frames.fetch_add(1, std::memory_order_relaxed);
			std::lock_guard<std::mutex> lock(s.publish_mutex);
			s.published = BuildPublishedSet(layers, layer_count, src, keep_screen, fp_screen, fp_head, fp_image, s.published);
		}

		// Headset rate: releases the screen and hand images this present copied (left acquired by CopyEyes and
		// BuildHandLayers) and publishes the layers that show them, all under submit_mutex. The runtime shows a
		// swapchain's last released image, so a release before the publish would let the pacer end a frame with the
		// new image at the previous present's pose (and, in layered stereo, one eye new and the other old). Lock order
		// as documented at s.submit_mutex: submit_mutex, then the queue lock (LockedRelease). Bounded like
		// DestroySwapchainSynced: a pacer that has not moved (its heartbeat) for over 500 ms while inside xrEndFrame
		// means the runtime stopped answering.
		// cl_copy: each eye's CopyResult this present (-1: none tried); a failed release turns Copied into Skipped.
		// chain_was_released: each eye chain's ever_released before this present's copies, which set it on Copied
		// ahead of the release so this present's layers are built; restored when the release fails.
		void ReleaseAndPublish(const XrCompositionLayerBaseHeader* const* layers, u32 layer_count, const LayerSources& src,
			int* cl_copy, const bool* chain_was_released, bool fp_screen, const XrVector3f& fp_head, u64 fp_image)
		{
			// Never waits: with the pacer inside xrEndFrame the GS would wait for the runtime (measured on the null HMD:
			// the emulation fell to about 80% speed). This present's images stay acquired (release_pending, so nothing
			// more is copied into them) and its layers wait in s.deferred for the next present.
			std::unique_lock<std::timed_mutex> submit(s.submit_mutex, std::try_to_lock);
			if (!submit.owns_lock())
			{
				const u64 now_ms = CameraDriver::SteadyNowMs();
				if (!s.deferred_valid)
				{
					bool d_attempted = false;
					bool d_all_copied = true;
					for (u32 l = 0; l < 2; l++)
					{
						if (cl_copy[l] < 0)
							continue;
						d_attempted = true;
						if (cl_copy[l] != static_cast<int>(CopyResult::Copied))
							d_all_copied = false;
					}
					PublishedSet prev;
					{
						std::lock_guard<std::mutex> publish(s.publish_mutex);
						prev = s.published;
					}
					s.deferred = BuildPublishedSet(layers, layer_count, src, !d_attempted || !d_all_copied, fp_screen, fp_head,
						fp_image, prev);
					s.deferred_valid = true;
					s.deferred_since_ms = now_ms;
				}
				s.stat_publish_deferred.fetch_add(1, std::memory_order_relaxed);
				const u64 stuck_ms = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(kRuntimeStuckTimeout).count());
				// Stuck means the pacer itself has not moved for that long, not that the deferral is that old: its heartbeat
				// (stored at the top of every pacer loop and when xrWaitFrame returns, both before it takes submit_mutex)
				// stands still only while it is inside a runtime call, here xrEndFrame with submit_mutex held. A deferral
				// left over from a GS gap (an offline pause with no presents, a GS hitch, a session restart) whose next
				// present happens to meet xrEndFrame again would otherwise tear VR down under a healthy pacer.
				const u64 beat = s.pacer_heartbeat_ms.load(std::memory_order_acquire);
				if (beat != 0 && now_ms > beat + stuck_ms && now_ms > s.deferred_since_ms + stuck_ms)
				{
					// The images stay acquired: the loss teardown destroys the swapchains (or leaves them alive with the
					// pacer detached).
					Console.Error("(VR) The VR pacer has been inside xrEndFrame for over 500 ms (runtime not answering): "
								  "frame not published, abandoning VR; emulation continues.");
					XRSession::MarkLost();
				}
				else if (beat > s.deferred_since_ms)
				{
					// The pacer has looped since the deferral (or since the clock last restarted): the clock starts again.
					s.deferred_since_ms = now_ms;
				}
				return;
			}

			// A present deferred earlier: its layers go out first, with the images released below, and this present
			// (which copied nothing into those still-acquired chains) publishes nothing of its own.
			const bool had_deferred = s.deferred_valid;
			if (had_deferred)
			{
				s.stat_gs_frames.fetch_add(1, std::memory_order_relaxed);
				std::lock_guard<std::mutex> publish(s.publish_mutex);
				s.deferred.serial = s.published.serial + 1;
				s.published = s.deferred;
				s.deferred_valid = false;
			}
			s.deferred_since_ms = 0;

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
				// What the runtime shows for this hand from now on: this present's drawing (committed with the pose it was
				// drawn for, in the same section that publishes it), or an image no kept hand may go with.
				if (XR_SUCCEEDED(res) && hc.pending_submitted)
					s.hand_image[i] = s.hand_image_pending[i];
				else
					s.hand_image[i].valid = false;
				s.hand_image_pending[i].valid = false;
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
			if (!had_deferred)
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

		// A hand's view drifted this far from the one its image was drawn for while the pacer moved it: counted on the
		// Hands line (the image is drawn again from kRerasterDriftDeg on, so this means images are not keeping up).
		constexpr float kHandDriftWarnDeg = 15.0f;

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
			double end_sum_ms = 0.0;
			u32 end_n = 0;
			u32 layers = 0;
			u64 last_serial = 0;
			// The Hands line (the pacer, headset rate only): layers riding on a controller placed, and how many of them
			// moved to the controller pose of their display frame; how old the controller pose each went out with was;
			// how far the controller had moved since the pose the GS placed it for; and the hand images' view drift.
			u32 hand_placements = 0;
			u32 hand_reposed = 0;
			double hand_age_sum_ms = 0.0;
			u32 hand_age_n = 0;
			float hand_age_max_ms = 0.0f;
			double hand_moved_sum_mm = 0.0;
			double hand_moved_sum_deg = 0.0;
			u32 hand_moved_n = 0;
			float hand_moved_max_mm = 0.0f;
			float hand_moved_max_deg = 0.0f;
			float hand_drift_max_deg = 0.0f;
			u32 hand_drift_over = 0;
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
							"shouldRender=false %u, wait->begin mean %.1f max %.1f ms, xrEndFrame mean %.1f max %.1f ms, queue-lock "
							"wait max %u us total %.1f ms/s, copies skipped %u timed out %u, publishes deferred %u, layers %u, mode %s",
				st.submitted / secs, hz, gs_frames / secs, st.resent / secs, st.stale, st.no_render,
				(st.wait_begin_n > 0) ? (st.wait_begin_sum_ms / st.wait_begin_n) : 0.0, st.wait_begin_max_ms,
				(st.end_n > 0) ? (st.end_sum_ms / st.end_n) : 0.0, st.end_max_ms,
				VR::TakeXrQueueLockMaxWaitUs(), static_cast<double>(VR::TakeXrQueueLockTotalWaitUs()) / 1000.0 / secs, skipped, timed_out, s.stat_publish_deferred.exchange(0, std::memory_order_relaxed),
				st.layers,
				s.headset_rate ? "headset rate (VR pacer)" : "legacy (one per GS present)");
			// Only the pacer places hands (legacy submits each frame for the display time it located the hands for).
			const u32 hand_new = s.stat_hand_new.exchange(0, std::memory_order_relaxed);
			const u32 hand_kept = s.stat_hand_kept.exchange(0, std::memory_order_relaxed);
			if (st.hand_placements > 0)
			{
				Console.WriteLn("(VR) Hands: placements %u, re-posed %u, age mean %.1f max %.1f ms, moved since the GS pose mean "
								"%.1f max %.1f mm / mean %.1f max %.1f deg, image drift max %.1f deg (%u over %.0f), GS images "
								"new %.1f/s kept %.1f/s, mode %s",
					st.hand_placements, st.hand_reposed,
					(st.hand_age_n > 0) ? (st.hand_age_sum_ms / st.hand_age_n) : 0.0, static_cast<double>(st.hand_age_max_ms),
					(st.hand_moved_n > 0) ? (st.hand_moved_sum_mm / st.hand_moved_n) : 0.0,
					static_cast<double>(st.hand_moved_max_mm),
					(st.hand_moved_n > 0) ? (st.hand_moved_sum_deg / st.hand_moved_n) : 0.0,
					static_cast<double>(st.hand_moved_max_deg), static_cast<double>(st.hand_drift_max_deg), st.hand_drift_over,
					static_cast<double>(kHandDriftWarnDeg), hand_new / secs, hand_kept / secs,
					s.hands_display_rate ? "display rate (VR pacer)" : "GS present (HandsDisplayRate off)");
			}
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

		// The hands as the pacer places them for one display frame (LocateHandsNow), with the eye the GS convention
		// takes (the head position, or the origin when it is not tracked).
		struct HandsNow
		{
			struct H
			{
				bool valid = false;
				float pos[3] = {0.0f, 0.0f, 0.0f};          // the grip position
				float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // aim orientation, or grip (PCSX2_VR_HANDS_GRIP_ORIENT)
				XrTime time = 0;                           // the display time this pose was located for
				// The aim pose itself (the controllers only, not the test hands): the ray the menu pointer follows and
				// where its beam starts (PlaceMenuPointerNow). Valid without a grip pose too.
				bool aim_valid = false;
				float aim_pos[3] = {0.0f, 0.0f, 0.0f};
				float aim_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
			};
			H hand[kHandSlots];
			float eye[3] = {0.0f, 0.0f, 0.0f};
		};

		// What PlaceHandAttached did in one display frame, for the Hands line and the sync log. The jerk keys are the
		// left hand, the right hand (HandModel::kLeft / kRight) and the gun's laser.
		constexpr int kJerkKeys = 3;
		constexpr int kJerkLaser = 2;
		struct HandFrameInfo
		{
			u32 placements = 0;
			u32 reposed = 0;
			double age_sum_ms = 0.0;
			u32 age_n = 0;
			float age_max_ms = 0.0f;
			double moved_sum_mm = 0.0;
			double moved_sum_deg = 0.0;
			u32 moved_n = 0;
			float moved_max_mm = 0.0f;
			float moved_max_deg = 0.0f;
			float drift_max_deg = 0.0f;
			u32 drift_over = 0;
			// The quad centre submitted per jerk key this frame.
			bool have[kJerkKeys] = {false, false, false};
			float centre[kJerkKeys][3] = {};
		};

		// Headset rate: the hands for display time t, in the base space, as the GS places them (BuildHandLayers): the
		// test hands from the head pose located for t, else the controllers XRInput::Update just located for t (grip
		// position; aim orientation, or grip with PCSX2_VR_HANDS_GRIP_ORIENT). Called outside submit_mutex: the snapshot
		// takes only leaf locks.
		void LocateHandsNow(XrTime t, const XrPosef& head, bool head_valid, bool head_position_valid, HandsNow* out)
		{
			*out = HandsNow{};
			out->eye[0] = head_position_valid ? head.position.x : 0.0f;
			out->eye[1] = head_position_valid ? head.position.y : 0.0f;
			out->eye[2] = head_position_valid ? head.position.z : 0.0f;
			if (s.fake_hands != 0)
			{
				if (!head_valid)
					return;
				const float head_quat[4] = {head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w};
				for (int i = 0; i < kHandSlots; i++)
				{
					const HandModel::HandState h = FakeHand(i, out->eye, head_quat, false, t);
					if (!h.valid)
						continue;
					HandsNow::H& d = out->hand[i];
					for (int k = 0; k < 3; k++)
						d.pos[k] = h.pos[k];
					for (int k = 0; k < 4; k++)
						d.quat[k] = h.quat[k];
					d.time = t;
					d.valid = true;
				}
				return;
			}
			const VRInputSnapshot snap = GetInputSnapshot();
			if (snap.generation == 0 || !snap.actions_active)
				return;
			for (int i = 0; i < kHandSlots; i++)
			{
				const auto& src = snap.hands[i];
				HandsNow::H& d = out->hand[i];
				if (src.aim_pose.valid)
				{
					for (int k = 0; k < 3; k++)
						d.aim_pos[k] = src.aim_pose.position_xyz[k];
					for (int k = 0; k < 4; k++)
						d.aim_quat[k] = src.aim_pose.orientation_xyzw[k];
					d.aim_valid = true;
				}
				if (!src.grip_pose.valid)
					continue;
				const auto& orient = (!s.grip_orient && src.aim_pose.valid) ? src.aim_pose : src.grip_pose;
				for (int k = 0; k < 3; k++)
					d.pos[k] = src.grip_pose.position_xyz[k];
				for (int k = 0; k < 4; k++)
					d.quat[k] = orient.orientation_xyzw[k];
				d.time = (snap.display_time != 0) ? snap.display_time : t;
				d.valid = true;
			}
		}

		// The angle between two orientations (xyzw, normalised here), in degrees: 2 acos |a . b|.
		float RotationBetweenDeg(const float a[4], const float b[4])
		{
			const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3]);
			const float lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2] + b[3] * b[3]);
			if (!(la > 1.0e-6f) || !(lb > 1.0e-6f))
				return 0.0f;
			const float d = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) / (la * lb);
			const float deg = 2.0f * std::acos(std::min(d, 1.0f)) * (180.0f / 3.14159265f);
			return std::isfinite(deg) ? deg : 0.0f;
		}

		// Headset rate: moves each layer that rides on a controller (an attached HandAttach: the hands, the gun's laser,
		// the wrist card) rigidly with its hand, from the pose it was placed for to the pose located for this display
		// frame, in the pacer's own copy of the published set. Always from the published pose, never cumulatively, and
		// s.published is never written. apply false ([VR] HandsDisplayRate off), or no pose for the hand this frame,
		// sends the published pose unchanged. Layers that do not ride on a controller are never touched. Records what it
		// did in info.
		void PlaceHandAttached(PublishedSet& set, const HandsNow& now, XrTime display_time, bool apply, HandFrameInfo* info)
		{
			*info = HandFrameInfo{};
			for (u32 i = 0; i < set.count; i++)
			{
				PublishedSet::Entry& e = set.e[i];
				const HandAttach& a = e.att;
				if (!a.attached || e.kind != kKindQuad || a.hand >= kHandSlots)
					continue;
				const HandsNow::H& h = now.hand[a.hand];
				XrPosef& pose = e.quad.pose;
				const float published_centre[3] = {pose.position.x, pose.position.y, pose.position.z};
				const float published_quat[4] = {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
				XrTime used_time = a.ref_time;
				info->placements++;
				if (apply && h.valid)
				{
					float centre[3];
					float quat[4];
					HandModel::RePoseRigid(a.ref_pos, a.ref_quat, h.pos, h.quat, published_centre, published_quat, centre, quat);
					pose.position = {centre[0], centre[1], centre[2]};
					pose.orientation = {quat[0], quat[1], quat[2], quat[3]};
					used_time = h.time;
					info->reposed++;
				}

				// How old the controller pose this layer goes out with is.
				if (used_time != 0)
				{
					const float age_ms =
						static_cast<float>(std::max(0.0, static_cast<double>(display_time - used_time) / 1.0e6));
					info->age_sum_ms += age_ms;
					info->age_n++;
					info->age_max_ms = std::max(info->age_max_ms, age_ms);
				}
				// How far the controller moved since the pose the GS placed this layer for.
				if (h.valid)
				{
					const float dx = h.pos[0] - a.ref_pos[0];
					const float dy = h.pos[1] - a.ref_pos[1];
					const float dz = h.pos[2] - a.ref_pos[2];
					const float mm = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0f;
					const float deg = RotationBetweenDeg(a.ref_quat, h.quat);
					if (std::isfinite(mm))
					{
						info->moved_sum_mm += mm;
						info->moved_sum_deg += deg;
						info->moved_n++;
						info->moved_max_mm = std::max(info->moved_max_mm, mm);
						info->moved_max_deg = std::max(info->moved_max_deg, deg);
					}
					// A hand image seen from where the eye is now: how far the view drifted from the one it was drawn for.
					if (e.role == kRoleHand)
					{
						const HandModel::ViewDrift drift = HandModel::MeasureViewDrift(
							a.ref_pos, a.ref_quat, a.ref_eye, published_centre, h.pos, h.quat, now.eye);
						if (drift.valid)
						{
							info->drift_max_deg = std::max(info->drift_max_deg, drift.angle_deg);
							if (drift.angle_deg > kHandDriftWarnDeg)
								info->drift_over++;
						}
					}
				}

				// The centre submitted, for the jerk. Only the laser riding on the gun hand counts as the laser.
				int key = -1;
				if (e.role == kRoleHand)
					key = a.hand;
				else if (e.role == kRoleHudLaser)
					key = kJerkLaser;
				if (key >= 0 && key < kJerkKeys)
				{
					info->have[key] = true;
					info->centre[key][0] = pose.position.x;
					info->centre[key][1] = pose.position.y;
					info->centre[key][2] = pose.position.z;
				}
			}
		}

		// What PlaceMenuPointerNow did with one display frame.
		constexpr int kPointerNowNone = 0;    // the set holds no beam or dot
		constexpr int kPointerNowKept = 1;    // the GS placement went out (a PINE test hit, or no aim pose this frame)
		constexpr int kPointerNowPlaced = 2;  // placed from this frame's aim
		constexpr int kPointerNowDropped = 3; // this frame's aim misses the screen: the beam and the dot left out

		// Headset rate, with [VR] HandsDisplayRate: the menu pointer's beam and dot placed again for this display frame in
		// the pacer's copy of the set, from the aim pose LocateHandsNow located for it and the flat screen quad the set was
		// published with, by the ray of the hand the GS placed them by and only that one (PlaceMenuPointer takes
		// GetInputSnapshot's hit: the right hand's ray, the left's only standing in for a right controller that is not
		// tracked, or as a PINE test hand), so the other hand's ray never takes them over between presents. So the beam
		// stays on the hand and the dot under the aim between GS presents, like the hands, instead of stepping at the GS
		// rate. The GS placement goes out unchanged for a PINE test hit (no hand to follow) and when no aim pose was
		// located for that hand this frame (none, or the test hands). Its aim missing the screen this frame leaves the
		// beam and the dot out of it (the next present drops them too), and a beam that cannot be placed (the eye on its
		// line, the hand at the screen) goes alone. Only poses and sizes change and entries only ever go, so the layer
		// count never grows; s.published is never written.
		int PlaceMenuPointerNow(PublishedSet& set, const HandsNow& now)
		{
			int beam = -1;
			int dot = -1;
			for (u32 i = 0; i < set.count; i++)
			{
				const PublishedSet::Entry& e = set.e[i];
				if (e.kind != kKindQuad)
					continue;
				if (e.pointer == kPointerPartBeam)
					beam = static_cast<int>(i);
				else if (e.pointer == kPointerPartDot)
					dot = static_cast<int>(i);
			}
			if (beam < 0 && dot < 0)
				return kPointerNowNone;
			const PointerPublish& pp = set.pointer;
			if (!pp.screen_valid || !pp.ray || pp.hand < 0 || pp.hand >= kHandSlots)
				return kPointerNowKept;
			const HandsNow::H& hn = now.hand[pp.hand];
			if (!hn.aim_valid)
				return kPointerNowKept;

			VRScreenTransform screen;
			screen.valid = true;
			screen.center.valid = true;
			screen.center.position_xyz = {pp.screen_pos[0], pp.screen_pos[1], pp.screen_pos[2]};
			screen.center.orientation_xyzw = {pp.screen_quat[0], pp.screen_quat[1], pp.screen_quat[2], pp.screen_quat[3]};
			screen.half_width = pp.half_width;
			screen.half_height = pp.half_height;

			VRPose aim;
			aim.valid = true;
			aim.position_xyz = {hn.aim_pos[0], hn.aim_pos[1], hn.aim_pos[2]};
			aim.orientation_xyzw = {hn.aim_quat[0], hn.aim_quat[1], hn.aim_quat[2], hn.aim_quat[3]};
			const VRScreenHit hit = RayHitScreen(screen, aim);
			const int hand =
				(hit.valid && std::isfinite(hit.point[0]) && std::isfinite(hit.point[1]) && std::isfinite(hit.point[2])) ?
					static_cast<int>(pp.hand) :
					-1;

			bool keep_beam = false;
			bool keep_dot = false;
			if (hand >= 0)
			{
				MenuPointerPlacement mp;
				const float hit_point[3] = {hit.point[0], hit.point[1], hit.point[2]};
				PlaceMenuPointerParts(hit_point, pp.screen_quat, now.hand[hand].aim_pos, now.eye, &mp);
				if (dot >= 0)
				{
					XrCompositionLayerQuad& dq = set.e[dot].quad;
					dq.pose.orientation = {mp.dot_quat[0], mp.dot_quat[1], mp.dot_quat[2], mp.dot_quat[3]};
					dq.pose.position = {mp.dot_pos[0], mp.dot_pos[1], mp.dot_pos[2]};
					dq.size = {mp.dot_size, mp.dot_size};
					keep_dot = true;
				}
				if (beam >= 0 && mp.beam)
				{
					XrCompositionLayerQuad& bq = set.e[beam].quad;
					bq.pose.orientation = {mp.beam_quat[0], mp.beam_quat[1], mp.beam_quat[2], mp.beam_quat[3]};
					bq.pose.position = {mp.beam_pos[0], mp.beam_pos[1], mp.beam_pos[2]};
					bq.size = {HudCards::kBeamWidthM, mp.beam_length};
					keep_beam = true;
				}
			}
			// What could not be placed for this frame goes out of it; the rest keep their order (the dot over the beam,
			// both under the hands).
			u32 kept = 0;
			for (u32 i = 0; i < set.count; i++)
			{
				const int idx = static_cast<int>(i);
				if ((idx == beam && !keep_beam) || (idx == dot && !keep_dot))
					continue;
				if (kept != i)
					set.e[kept] = set.e[i];
				kept++;
			}
			set.count = kept;
			return (hand >= 0) ? kPointerNowPlaced : kPointerNowDropped;
		}

		// The pacer's menu pointer placements, logged every 5 s while there were any: "(VR) Menu pointer (pacer): N display
		// frame(s) placed from their own aim, M with the GS placement, K left out (aim off the screen) in 5.0 s". Pacer
		// thread only.
		void NoteMenuPointerNow(int result)
		{
			static u32 s_now_placed = 0;
			static u32 s_now_kept = 0;
			static u32 s_now_dropped = 0;
			static u64 s_now_window_ms = 0;
			if (result == kPointerNowPlaced)
				s_now_placed++;
			else if (result == kPointerNowKept)
				s_now_kept++;
			else if (result == kPointerNowDropped)
				s_now_dropped++;
			const u64 now_ms = CameraDriver::SteadyNowMs();
			if (s_now_window_ms == 0)
				s_now_window_ms = now_ms;
			if (now_ms < s_now_window_ms + 5000)
				return;
			if (s_now_placed + s_now_kept + s_now_dropped > 0)
			{
				Console.WriteLn("(VR) Menu pointer (pacer): %u display frame(s) placed from their own aim, %u with the GS "
								"placement, %u left out (aim off the screen) in %.1f s.",
					s_now_placed, s_now_kept, s_now_dropped, static_cast<double>(now_ms - s_now_window_ms) / 1000.0);
			}
			s_now_placed = 0;
			s_now_kept = 0;
			s_now_dropped = 0;
			s_now_window_ms = now_ms;
		}

		// PCSX2_VR_SELFTEST: PlaceMenuPointerNow against hand-made sets (the screen, the beam, the dot and a hand) and
		// hand-made aims. Pure maths, no XR call: run from Initialize before the session.
		bool SelfTestMenuPointerNow()
		{
			int checks = 0;
			int failures = 0;
			const char* first = nullptr;
			const auto check = [&](bool ok, const char* name) {
				checks++;
				if (ok)
					return;
				failures++;
				if (!first)
					first = name;
			};
			const auto near3 = [](const XrVector3f& a, const float b[3], float tol) {
				return std::fabs(a.x - b[0]) < tol && std::fabs(a.y - b[1]) < tol && std::fabs(a.z - b[2]) < tol;
			};
			// An aim pose at from whose -Z runs toward to: yaw about +Y, then pitch about +X.
			const auto aim_at = [](HandsNow::H& hn, const float from[3], const float to[3]) {
				const float d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
				const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
				const float yaw = std::atan2(-d[0], -d[2]);
				const float pitch = std::asin(std::clamp(d[1] / len, -1.0f, 1.0f));
				const float syh = std::sin(0.5f * yaw), cyh = std::cos(0.5f * yaw);
				const float sph = std::sin(0.5f * pitch), cph = std::cos(0.5f * pitch);
				hn.aim_valid = true;
				for (int k = 0; k < 3; k++)
					hn.aim_pos[k] = from[k];
				hn.aim_quat[0] = cyh * sph;
				hn.aim_quat[1] = syh * cph;
				hn.aim_quat[2] = -syh * sph;
				hn.aim_quat[3] = cyh * cph;
			};
			// The world screen 2.5 m out (1.73 x 1.3 m), and a set as the GS publishes it in a menu: the screen, the menu
			// beam, the dot and a hand, the beam and the dot at a GS placement far from anything this test aims at.
			const auto make_set = [](PublishedSet& out) {
				out = PublishedSet{};
				PointerPublish& pp = out.pointer;
				pp.screen_valid = true;
				pp.screen_pos[1] = 1.5f;
				pp.screen_pos[2] = -2.5f;
				pp.half_width = 0.867f;
				pp.half_height = 0.65f;
				pp.ray = true;
				pp.hand = static_cast<s8>(HandModel::kRight);
				pp.beam = true;
				static constexpr u8 kRoles[4] = {kRoleScreen, kRoleHudLaser, kRoleHudView, kRoleHand};
				static constexpr u8 kParts[4] = {kPointerPartNone, kPointerPartBeam, kPointerPartDot, kPointerPartNone};
				for (u32 i = 0; i < 4; i++)
				{
					PublishedSet::Entry& e = out.e[i];
					e.kind = kKindQuad;
					e.role = kRoles[i];
					e.pointer = kParts[i];
					e.quad = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
					e.quad.pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {9.0f, 9.0f, 9.0f}};
					e.quad.size = {0.5f, 0.5f};
				}
				out.count = 4;
			};
			const float eye[3] = {0.0f, 1.6f, 0.0f};
			// u 0.7, v 0.4 on the world screen.
			const float target[3] = {0.2f * 2.0f * 0.867f, 1.5f + 0.1f * 2.0f * 0.65f, -2.5f};
			const float dot_at[3] = {target[0], target[1], target[2] + HudCards::kPointerLiftM};
			const float right_from[3] = {0.2f, 1.2f, -0.3f};
			const float left_from[3] = {-0.2f, 1.2f, -0.3f};
			const float away[3] = {0.2f, 1.2f, 2.7f}; // straight back, away from the screen
			const float gs_pos[3] = {9.0f, 9.0f, 9.0f};

			// From the right hand: the dot where this frame's aim meets the screen, the beam from just ahead of the hand.
			{
				PublishedSet set;
				make_set(set);
				HandsNow now;
				for (int k = 0; k < 3; k++)
					now.eye[k] = eye[k];
				aim_at(now.hand[HandModel::kRight], right_from, target);
				check(PlaceMenuPointerNow(set, now) == kPointerNowPlaced && set.count == 4,
					"the beam and the dot are placed from this frame's aim");
				check(near3(set.e[2].quad.pose.position, dot_at, 1.0e-3f),
					"the dot lies on the screen (lifted off its face) where this frame's aim meets it");
				check(set.e[2].quad.size.width >= HudCards::kPointerMinM && set.e[2].quad.size.width == set.e[2].quad.size.height,
					"the dot is sized from the eye");
				const float d[3] = {target[0] - right_from[0], target[1] - right_from[1], target[2] - right_from[2]};
				const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
				const float along = HudCards::kBeamStartM + 0.5f * (len - HudCards::kBeamStartM);
				const float mid[3] = {right_from[0] + d[0] / len * along, right_from[1] + d[1] / len * along,
					right_from[2] + d[2] / len * along};
				check(std::fabs(set.e[1].quad.size.height - (len - HudCards::kBeamStartM)) < 1.0e-3f &&
						  set.e[1].quad.size.width == HudCards::kBeamWidthM && near3(set.e[1].quad.pose.position, mid, 1.0e-3f),
					"the beam runs from just ahead of the hand to the dot");
				check(set.e[0].role == kRoleScreen && set.e[3].role == kRoleHand && near3(set.e[0].quad.pose.position, gs_pos, 1.0e-6f) &&
						  near3(set.e[3].quad.pose.position, gs_pos, 1.0e-6f),
					"the screen and the hand are left alone");
			}

			// Placed by the right hand, as GetInputSnapshot takes the hit: its aim off the screen leaves them out of the
			// frame even with the left hand's aim on it, and with no right aim this frame the GS placement stays. The left
			// controller never takes the pointer from the right one.
			{
				PublishedSet set;
				make_set(set);
				HandsNow now;
				for (int k = 0; k < 3; k++)
					now.eye[k] = eye[k];
				aim_at(now.hand[HandModel::kRight], right_from, away);
				aim_at(now.hand[HandModel::kLeft], left_from, target);
				check(PlaceMenuPointerNow(set, now) == kPointerNowDropped && set.count == 2,
					"placed by the right hand, its aim off the screen drops them, whatever the left hand points at");
				make_set(set);
				now.hand[HandModel::kRight] = HandsNow::H{};
				check(PlaceMenuPointerNow(set, now) == kPointerNowKept && set.count == 4 &&
						  near3(set.e[2].quad.pose.position, gs_pos, 1.0e-6f),
					"placed by the right hand with no right aim this frame, the GS placement stays");
			}

			// Placed by the left hand (the right controller not tracked, or a PINE test hand): the left hand's aim places
			// them, even with the right hand's on the screen this frame.
			{
				PublishedSet set;
				make_set(set);
				set.pointer.hand = static_cast<s8>(HandModel::kLeft);
				HandsNow now;
				for (int k = 0; k < 3; k++)
					now.eye[k] = eye[k];
				aim_at(now.hand[HandModel::kLeft], left_from, target);
				aim_at(now.hand[HandModel::kRight], right_from, target);
				const float d[3] = {target[0] - left_from[0], target[1] - left_from[1], target[2] - left_from[2]};
				const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
				check(PlaceMenuPointerNow(set, now) == kPointerNowPlaced && near3(set.e[2].quad.pose.position, dot_at, 1.0e-3f) &&
						  std::fabs(set.e[1].quad.size.height - (len - HudCards::kBeamStartM)) < 1.0e-3f,
					"placed by the left hand, the left hand's aim places them");
			}

			// Every aim misses: the beam and the dot are left out of this frame, the rest in order.
			{
				PublishedSet set;
				make_set(set);
				HandsNow now;
				aim_at(now.hand[HandModel::kRight], right_from, away);
				check(PlaceMenuPointerNow(set, now) == kPointerNowDropped && set.count == 2 && set.e[0].role == kRoleScreen &&
						  set.e[1].role == kRoleHand,
					"an aim off the screen leaves the beam and the dot out of the frame, the rest in order");
			}

			// No aim this frame, or a PINE test hit: the GS placement goes out unchanged.
			{
				PublishedSet set;
				make_set(set);
				HandsNow now;
				check(PlaceMenuPointerNow(set, now) == kPointerNowKept && set.count == 4 &&
						  near3(set.e[1].quad.pose.position, gs_pos, 1.0e-6f) && near3(set.e[2].quad.pose.position, gs_pos, 1.0e-6f),
					"with no aim this frame the GS placement stays");
				make_set(set);
				set.pointer.ray = false;
				aim_at(now.hand[HandModel::kRight], right_from, target);
				check(PlaceMenuPointerNow(set, now) == kPointerNowKept && set.count == 4 &&
						  near3(set.e[2].quad.pose.position, gs_pos, 1.0e-6f),
					"a dot for a PINE test hit stays where the GS put it");
				make_set(set);
				set.pointer.screen_valid = false;
				check(PlaceMenuPointerNow(set, now) == kPointerNowKept && near3(set.e[2].quad.pose.position, gs_pos, 1.0e-6f),
					"with no screen published the GS placement stays");
			}

			// The eye on the beam's line: the beam goes alone, the dot is still placed.
			{
				PublishedSet set;
				make_set(set);
				HandsNow now;
				aim_at(now.hand[HandModel::kRight], right_from, target);
				const float d[3] = {target[0] - right_from[0], target[1] - right_from[1], target[2] - right_from[2]};
				const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
				for (int k = 0; k < 3; k++)
					now.eye[k] = right_from[k] - d[k] / len * 0.3f;
				check(PlaceMenuPointerNow(set, now) == kPointerNowPlaced && set.count == 3 && set.e[1].pointer == kPointerPartDot &&
						  near3(set.e[1].quad.pose.position, dot_at, 1.0e-3f),
					"looking straight down the beam, the beam goes alone and the dot stays");
			}

			// A set with no pointer parts is not touched.
			{
				PublishedSet set;
				make_set(set);
				set.e[1].pointer = kPointerPartNone;
				set.e[2].pointer = kPointerPartNone;
				HandsNow now;
				aim_at(now.hand[HandModel::kRight], right_from, target);
				check(PlaceMenuPointerNow(set, now) == kPointerNowNone && set.count == 4 && near3(set.e[2].quad.pose.position, gs_pos, 1.0e-6f),
					"a set without the beam or the dot is not touched");
			}

			if (failures == 0)
				Console.WriteLn(Color_StrongGreen, "(VR) Menu pointer (pacer) self-test: passed (%d checks).", checks);
			else
				Console.WriteLn(Color_StrongRed, "(VR) Menu pointer (pacer) self-test FAIL: %s (%d of %d checks failed).", first,
					failures, checks);
			return failures == 0;
		}

		// The Hands line's numbers: one display frame's placement folded into the 5 s window.
		void AddHandFrame(FrameStats& st, const HandFrameInfo& f)
		{
			st.hand_placements += f.placements;
			st.hand_reposed += f.reposed;
			st.hand_age_sum_ms += f.age_sum_ms;
			st.hand_age_n += f.age_n;
			st.hand_age_max_ms = std::max(st.hand_age_max_ms, f.age_max_ms);
			st.hand_moved_sum_mm += f.moved_sum_mm;
			st.hand_moved_sum_deg += f.moved_sum_deg;
			st.hand_moved_n += f.moved_n;
			st.hand_moved_max_mm = std::max(st.hand_moved_max_mm, f.moved_max_mm);
			st.hand_moved_max_deg = std::max(st.hand_moved_max_deg, f.moved_max_deg);
			st.hand_drift_max_deg = std::max(st.hand_drift_max_deg, f.drift_max_deg);
			st.hand_drift_over += f.drift_over;
		}

		// PCSX2_VR_SYNCLOG: how old the controller pose the hand-borne layers go out with is, per display frame (the mean
		// over the frame's layers), and how smoothly the hands and the gun's laser move in the headset: the jerk
		// |p(n) - 2 p(n-1) + p(n-2)| of each one's submitted quad centre, a rigid point on the hand (an impostor's centre
		// is its bounding box's), so a redrawn image does not move it. Counted only over three display frames in a row
		// whose display-time steps are each within 25% of the display period. Logged once a second.
		void NoteHandPlacement(const HandFrameInfo& f, XrTime display_time)
		{
			static const bool s_synclog = (std::getenv("PCSX2_VR_SYNCLOG") != nullptr);
			if (!s_synclog)
				return;
			struct Track
			{
				int n = 0;                 // frames held below (0-2)
				float p[2][3] = {};        // [0] the frame before, [1] the one before that
				XrTime t = 0;              // the display time of p[0]
			};
			static Track s_track[kJerkKeys];
			static double s_age_sum = 0.0;
			static float s_age_max = 0.0f;
			static u32 s_age_frames = 0;
			static double s_jerk_sq[2] = {0.0, 0.0}; // [0] the hands, [1] the laser
			static float s_jerk_max[2] = {0.0f, 0.0f};
			static u32 s_jerk_n[2] = {0, 0};
			static u64 s_log_ms = 0;

			if (f.age_n > 0)
			{
				s_age_sum += f.age_sum_ms / static_cast<double>(f.age_n);
				s_age_max = std::max(s_age_max, f.age_max_ms);
				s_age_frames++;
			}
			const XrDuration period = s.display_period_ns.load(std::memory_order_acquire);
			for (int k = 0; k < kJerkKeys; k++)
			{
				Track& tr = s_track[k];
				if (!f.have[k])
				{
					tr.n = 0;
					continue;
				}
				// A display frame missed (or a step far off the period) starts the run again.
				const XrTime step = display_time - tr.t;
				const bool step_ok = period > 0 && step > 0 &&
				                     std::abs(static_cast<double>(step - period)) <= 0.25 * static_cast<double>(period);
				if (tr.n > 0 && !step_ok)
					tr.n = 0;
				const float* c = f.centre[k];
				if (tr.n == 2)
				{
					const float jx = c[0] - 2.0f * tr.p[0][0] + tr.p[1][0];
					const float jy = c[1] - 2.0f * tr.p[0][1] + tr.p[1][1];
					const float jz = c[2] - 2.0f * tr.p[0][2] + tr.p[1][2];
					const float jerk_mm = std::sqrt(jx * jx + jy * jy + jz * jz) * 1000.0f;
					const int b = (k == kJerkLaser) ? 1 : 0;
					if (std::isfinite(jerk_mm))
					{
						s_jerk_sq[b] += static_cast<double>(jerk_mm) * static_cast<double>(jerk_mm);
						s_jerk_max[b] = std::max(s_jerk_max[b], jerk_mm);
						s_jerk_n[b]++;
					}
				}
				for (int j = 0; j < 3; j++)
				{
					tr.p[1][j] = tr.p[0][j];
					tr.p[0][j] = c[j];
				}
				tr.t = display_time;
				tr.n = std::min(tr.n + 1, 2);
			}

			const u64 now = CameraDriver::SteadyNowMs();
			if (s_log_ms == 0)
				s_log_ms = now;
			if (now - s_log_ms < 1000)
				return;
			if (s_age_frames > 0 || s_jerk_n[0] > 0 || s_jerk_n[1] > 0)
			{
				Console.WriteLn("(VR) pacer: hands placement age per display frame mean %.1f max %.1f ms over %u frames; jerk "
								"hand rms %.2f max %.2f mm, laser rms %.2f max %.2f mm (jerk samples: hand %u, laser %u)",
					(s_age_frames > 0) ? (s_age_sum / static_cast<double>(s_age_frames)) : 0.0, static_cast<double>(s_age_max),
					s_age_frames, (s_jerk_n[0] > 0) ? std::sqrt(s_jerk_sq[0] / static_cast<double>(s_jerk_n[0])) : 0.0,
					static_cast<double>(s_jerk_max[0]),
					(s_jerk_n[1] > 0) ? std::sqrt(s_jerk_sq[1] / static_cast<double>(s_jerk_n[1])) : 0.0,
					static_cast<double>(s_jerk_max[1]), s_jerk_n[0], s_jerk_n[1]);
			}
			s_log_ms = now;
			s_age_sum = 0.0;
			s_age_max = 0.0f;
			s_age_frames = 0;
			for (int b = 0; b < 2; b++)
			{
				s_jerk_sq[b] = 0.0;
				s_jerk_max[b] = 0.0f;
				s_jerk_n[b] = 0;
			}
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
				HandsNow hands_now;
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
						s.pacer_head.time = fs.predictedDisplayTime;
					}

					// The hands for this frame's display time (outside submit_mutex: the input snapshot takes only leaf
					// locks), for the layers riding on them.
					LocateHandsNow(fs.predictedDisplayTime, head, head_valid, head_position_valid, &hands_now);

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
				HandFrameInfo hand_frame;
				int pointer_now = kPointerNowNone;
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
						// The hands, the gun's laser and the wrist card move with their controllers to this frame's pose
						// ([VR] HandsDisplayRate), in this copy only. A stale set drops them below.
						if (!stale)
							PlaceHandAttached(set, hands_now, fs.predictedDisplayTime, s.hands_display_rate, &hand_frame);
						// The menu pointer's beam and dot follow this frame's aim with the hands (PlaceMenuPointerNow).
						if (!stale && s.hands_display_rate)
							pointer_now = PlaceMenuPointerNow(set, hands_now);
						float turn = 0.0f;
						if (set.fp_screen && set.turn_rate != 0.0f && set.publish_display_time != 0)
						{
							const double dt = std::clamp(
								static_cast<double>(fs.predictedDisplayTime - set.publish_display_time) * 1.0e-9, 0.0, 0.1);
							turn = set.turn_rate * static_cast<float>(dt);
						}
						if (views_ok)
							MakeBackgroundLayer(views, bg_views, &bg, set.fp_screen && s_fill.live.load(std::memory_order_acquire));
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
				stats.end_sum_ms += end_ms;
				stats.end_n++;
				stats.layers = fs.shouldRender ? submit_count : 0;
				if (fs.shouldRender && XR_SUCCEEDED(eres))
				{
					NoteScreenYaw(set, fs.predictedDisplayTime);
					NoteHandPlacement(hand_frame, fs.predictedDisplayTime);
					AddHandFrame(stats, hand_frame);
				}
				NoteMenuPointerNow(pointer_now);
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

		// [VR] HandsDisplayRate and the hands' test aids, read once like HeadsetRateSubmit: all before the pacer starts,
		// which then reads them without a lock. PCSX2_VR_FAKE_HANDS is parsed as BuildHandLayers does.
		s.hands_display_rate = EmuConfig.VR.HandsDisplayRate;
		s.grip_orient = (std::getenv("PCSX2_VR_HANDS_GRIP_ORIENT") != nullptr);
		{
			const char* fake = std::getenv("PCSX2_VR_FAKE_HANDS");
			s.fake_hands = fake ? ((std::strcmp(fake, "gun") == 0) ? 2 : 1) : 0;
			const char* reraster = std::getenv("PCSX2_VR_HANDS_RERASTER");
			s.hands_reraster_always = (reraster != nullptr && std::strcmp(reraster, "always") == 0);
		}
		s.fake_sweep = false;
		if (std::getenv("PCSX2_VR_FAKE_HANDS_SWEEP") != nullptr)
		{
			if (VMManager::IsOnlineLocked())
				Console.Warning("(VR) PCSX2_VR_FAKE_HANDS_SWEEP refused: the network adapter is on (offline test aid).");
			else if (s.fake_hands == 0)
				Console.Warning("(VR) PCSX2_VR_FAKE_HANDS_SWEEP ignored: PCSX2_VR_FAKE_HANDS is not set.");
			else
			{
				s.fake_sweep = true;
				Console.WriteLn("(VR) Fake hands sweep: +-12 cm, +-20 deg yaw at 0.5 Hz (offline test aid).");
			}
		}
		for (int i = 0; i < kHandSlots; i++)
		{
			s.hand_image[i].valid = false;
			s.hand_image_pending[i].valid = false;
		}
		s.stat_hand_new.store(0, std::memory_order_relaxed);
		s.stat_hand_kept.store(0, std::memory_order_relaxed);
		// Pure maths, so it runs even when the session below fails.
		if (std::getenv("PCSX2_VR_HANDS_SELFTEST") != nullptr)
			HandModel::SelfTest();
		// The pacer's menu pointer placement, with the other menu pointer self-tests, and the edge fill's plan.
		if (std::getenv("PCSX2_VR_SELFTEST") != nullptr)
		{
			SelfTestMenuPointerNow();
			SelfTestEdgeFill();
		}

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
		s.hud_beam_opacity = 0.0f;
		s.hud_pointer_opacity = 0.0f;
		s.hud_pointer_logged = false;
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
			chain.waited = false;
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
			std::lock_guard<std::mutex> lock(s_screen_mutex);
			s_fp_screen_info = FirstPersonScreenInfo();
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

		// [VR] EdgeFill, read once like OpaqueBackground: it shows in the background's slot, so it needs that layer. Its
		// swapchain and images are made the first time the first-person screen is up.
		s_fill.enabled = EmuConfig.VR.EdgeFill && s.opaque_bg && s.bg_ready;
		s_fill.disabled = false;
		s_fill.live.store(false, std::memory_order_release);
		s_fill.live_logged = false;
		s_fill.busy_skips = 0;
		s_fill.busy_logged = false;
		if (s_fill.enabled)
			Console.WriteLn("(VR) Edge fill: enabled ([VR] EdgeFill): behind the first-person screen the background shows the "
							"picture's edges continued, blurred and at half brightness, rebuilt on the GPU each GS frame.");
		else
			Console.WriteLn("(VR) Edge fill: off (%s); the background stays black.",
				!EmuConfig.VR.EdgeFill ? "[VR] EdgeFill = false" : "needs the opaque background");

		s.initialized = true;
		s.pacer = std::thread(PacerThreadMain, s.pacer_gen.load(std::memory_order_acquire));

		Console.WriteLn("(VR) Frame submission: %s",
			s.headset_rate ? "headset rate (VR pacer thread)" : "once per GS present (legacy)");
		if (!s.headset_rate)
			Console.WriteLn("(VR) Hands: placed once per GS present (legacy frame submission)");
		else if (!s.hands_display_rate)
			Console.WriteLn("(VR) Hands: placed once per GS present ([VR] HandsDisplayRate off)");
		else if (s.hands_reraster_always)
			Console.WriteLn("(VR) Hands: placed every display frame on the VR pacer (images redrawn every present: "
							"PCSX2_VR_HANDS_RERASTER=always)");
		else
			Console.WriteLn("(VR) Hands: placed every display frame on the VR pacer (images redrawn past %.1f deg view drift)",
				static_cast<double>(HandModel::kRerasterDriftDeg));
		Console.WriteLn("(VR) Compositor initialized (%u copy command buffers; swapchain created on first frame).",
			NUM_CMD_BUFFERS);
		return true;
	}

	void EndOfFrame(GSTexture* current, u32 eye)
	{
		if (!s.initialized)
			return;
		s_present_wait_left_ns = kPresentWaitBudgetNs;
		PublishFirstPersonScreen();

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
			{
				// A publish deferred before the session stopped would wait out the whole stop and go out stale after it:
				// drop it, as DestroySwapchainSynced does. Its images, still acquired (release_pending), are released by
				// the next publish that gets submit_mutex, which keeps the previous Screen entries.
				s.deferred_valid = false;
				s.deferred_since_ms = 0;
				return;
			}
			// The pacer's latest head pose becomes this present's (head_pose and friends are the GS thread's own copy).
			{
				std::lock_guard<std::mutex> lock(s.head_mutex);
				s.head_pose = s.pacer_head.pose;
				s.head_pose_valid = s.pacer_head.valid;
				s.head_position_valid = s.pacer_head.position_valid;
				s.head_pose_time = s.pacer_head.time;
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
			s.head_pose_time = fs.predictedDisplayTime;

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
					// Both eyes copied this present or neither (CopyEyePair).
					CopyResult pair_result[2] = {CopyResult::Skipped, CopyResult::Skipped};
					CopyEyePair(src, pair_result);
					for (u32 l = 0; l < 2; l++)
					{
						const CopyResult r = pair_result[l];
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
					const CopyResult r = CopyToSwapchain(src, 0, cl_ci);
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
		// Worst case 14 layers: four for the screen (split screen in stereo, two viewports per eye; five slots
		// kept), two lever cards, two hands and the six HUD cards (laser or menu beam, wrist, vignette, toast, blink,
		// menu pointer dot), in a capacity of 15 (kLayerCapacity). The background goes under them in the submit array;
		// SteamVR takes at most 16 a frame.
		XrCompositionLayerQuad lever_quads[ControlQuads::kSlots] = {
			{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
		XrCompositionLayerQuad hand_quads[kHandSlots] = {
			{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
		XrCompositionLayerQuad hud_quads[HudCards::kCardCount] = {};
		// The controller pose each hand and HUD card was placed for (not attached unless it rides on a controller).
		HandAttach hand_att[kHandSlots] = {};
		HandAttach hud_att[HudCards::kCardCount] = {};
		const XrCompositionLayerBaseHeader* layers[kLayerCapacity] = {};
		u32 layer_count = 0;
		bool cl_split = false;
		// The flat screen quad the menu pointer aims at this present (VR::PublishScreenTransform); invalid unless one is
		// placed below.
		VRScreenTransform pub{};
		// The flat first-person screen as placed this present, for the edge fill behind it.
		bool fill_geo = false;
		float fill_distance = 0.0f, fill_width = 0.0f, fill_height = 0.0f;
		if (!force_zero_layers)
		{
			ScreenParams sp;
			{
				std::lock_guard<std::mutex> lock(s_screen_mutex);
				sp = s_screen_params;
			}
			// First person: head-locked screen sized to the game camera's FOV. Anything else (menus,
			// item screen, cutscenes, doors) goes back to the world screen, re-anchored in front of the
			// head each time first person hands over so it never appears behind the player. With the VR
			// MenuTablet setting on, a menu that comes up in first-person play (menu mode) goes on the menu
			// tablet instead: a small flat screen ahead of the chest, tilted back to the eyes, anchored the same way.
			// With the comfort blink on, a change of screen waits until the blink has gone black
			// (kBlinkRiseMs), so the jump in placement is never seen; the blink then holds black while the
			// frames from the new camera, about 85-100 ms behind the change, reach the screen.
			// The screen modes: 0 the world screen, 1 the menu tablet, 2 first person.
			const bool fp_want = sp.has_fp && CameraDriver::LookAtActive() && s.head_pose_valid;
			const bool tablet_want = !fp_want && sp.has_menu && HudCards::MenuTabletEnabled() &&
			                         SpatialControls::MenuMode() && !SplitState::Active() && s.head_pose_valid;
			const int mode_want = fp_want ? 2 : (tablet_want ? 1 : 0);
			const int mode_was = s_fp_screen_was_active ? 2 : (s_tablet_was_active ? 1 : 0);
			// The tablet's tilt back toward the eyes; below 0, exactly at them.
			const float tablet_tilt_deg = (sp.menu_tilt_deg >= 0.0f) ?
			                                  sp.menu_tilt_deg :
			                                  std::atan2(sp.menu_drop, sp.menu_distance) * (180.0f / 3.14159265f);
			bool switch_now = false;
			if (mode_want == mode_was)
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
				static constexpr const char* kModeBlink[3] = {"world screen", "menu tablet", "first-person screen"};
				if (!s.hud_disabled && s.view_space != XR_NULL_HANDLE && HudCards::Blink(kModeBlink[mode_want]))
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
				// Both world modes are placed in front of the head as they take over.
				if (mode_want != 2 && s.head_pose_valid)
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
				if (mode_want == 1)
				{
					Console.WriteLn("(VR) Screen: tablet (menu): %.2f m ahead, %.2f m below the eyes, %.2f m tall, tilted %.0f deg.",
						sp.menu_distance, sp.menu_drop, sp.menu_height, tablet_tilt_deg);
				}
				else
				{
					Console.WriteLn("(VR) Screen: %s.", (mode_want == 2) ? "first person (placed at the render pose, roll-level)" : "world-locked");
				}
				s_fp_screen_was_active = (mode_want == 2);
				s_tablet_was_active = (mode_want == 1);
				if (mode_want == 2)
					LogFirstPersonScreen(sp);
			}
			else if (s_fp_screen_was_active)
			{
				// The FOV sizing the first-person screen changed while it is up, or the headset's field of view became
				// known (both when its eyes arrive after the arm): its line again, with the coverage.
				const CameraDriver::FovState fov_state = CameraDriver::GetFovState();
				const bool fov_changed = fov_state.sizes_screen && (std::abs(fov_state.target_deg - s_fp_logged_fov) > 0.05f ||
																	   fov_state.source != s_fp_logged_source);
				if (fov_changed || (!s_fp_logged_eyes && GetEyeInfo().valid))
					LogFirstPersonScreen(sp);
			}
			const bool fp_screen = s_fp_screen_was_active;
			const bool tablet = !fp_screen && s_tablet_was_active;
			pub_fp_screen = fp_screen;

			// First-person screen: placed in the room (base space) in front of the head pose the shown
			// frame was rendered from (matched through the game's view matrix when the profile names it,
			// else the pose camera.lookAt used about poseLagMs ago), along its yaw and
			// pitch but never its roll. The runtime then reprojects it for the head's motion since, so the
			// world holds still while the head turns instead of an old frame being glued to the face.
			XrPosef fp_pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
			XrVector3f fp_head = {0.0f, 0.0f, 0.0f};
			// Sized to the FOV the shown frame was rendered with (matched with its render pose), else to the one camera.fov
			// writes now; 0: the profile's screen.firstPerson height (FirstPersonHeight).
			float fp_fov_deg = 0.0f;
			if (fp_screen && !CameraDriver::RenderFov(&fp_fov_deg))
				fp_fov_deg = ScreenFov(false);
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
			// The menu tablet is always a flat quad, without the vertical offset.
			const float distance = fp_screen ? sp.fp_distance : (tablet ? sp.menu_distance : sp.distance);
			const float height = fp_screen ? FirstPersonHeight(sp, fp_fov_deg) : (tablet ? sp.menu_height : sp.height);
			const float arc_deg = fp_screen ? sp.fp_arc_deg : (tablet ? 0.0f : sp.arc_deg);
			const float voffset = (fp_screen || tablet) ? 0.0f : sp.voffset;
			const float aspect = ComputeAspect();
			const bool curved = (arc_deg >= 5.0f) && XRSession::HasCylinderLayer();
			if (fp_screen && !curved && distance > 0.0f)
			{
				fill_geo = true;
				fill_distance = distance;
				fill_height = height;
				fill_width = height * aspect;
			}

			const float ayaw_sin = std::sin(s.world_anchor_yaw);
			const float ayaw_cos = std::cos(s.world_anchor_yaw);
			const XrQuaternionf anchor_quat = {
				0.0f, std::sin(s.world_anchor_yaw * 0.5f), 0.0f, std::cos(s.world_anchor_yaw * 0.5f)};

			// The menu tablet: anchor + R(yaw) * (0, -drop, -distance), turned anchor_quat * Rx(-tilt) so its face (+Z)
			// points back up at the eyes (the convention of HudCards::ToastPose).
			const float tilt_half = -0.5f * tablet_tilt_deg * (3.14159265f / 180.0f);
			const float tilt_s = std::sin(tilt_half), tilt_c = std::cos(tilt_half);
			const float yaw_s = std::sin(s.world_anchor_yaw * 0.5f), yaw_c = std::cos(s.world_anchor_yaw * 0.5f);
			const XrQuaternionf tablet_quat = {yaw_c * tilt_s, yaw_s * tilt_c, -yaw_s * tilt_s, yaw_c * tilt_c};
			const XrVector3f tablet_pos = {s.world_anchor_x - sp.menu_distance * ayaw_sin, s.world_anchor_y - sp.menu_drop,
				s.world_anchor_z - sp.menu_distance * ayaw_cos};

			const bool follow = !fp_screen && !tablet && sp.follow_head && (s.view_space != XR_NULL_HANDLE);
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
					else if (tablet)
					{
						quad.pose.orientation = tablet_quat;
						quad.pose.position = tablet_pos;
					}
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

			// The flat quad the menu pointer aims at: the world screen or the menu tablet, world-locked in the base space
			// (never the first-person, curved or head-following screen). Each eye's quad has the same pose. Logged as the
			// pointer's screen changes (0: none logged since the last present without one, 1 world, 2 tablet).
			static int s_pub_logged = 0;
			const auto publish_quad = [&]() {
				if (fp_screen || curved || follow || layer_count == 0)
					return;
				const XrCompositionLayerQuad& q = quads[layer_count - 1];
				pub.center.orientation_xyzw = {q.pose.orientation.x, q.pose.orientation.y, q.pose.orientation.z,
					q.pose.orientation.w};
				pub.center.position_xyz = {q.pose.position.x, q.pose.position.y, q.pose.position.z};
				pub.center.valid = true;
				pub.half_width = 0.5f * q.size.width;
				pub.half_height = 0.5f * q.size.height;
				pub.tablet = tablet;
				pub.valid = true;
				const int logged = tablet ? 2 : 1;
				if (s_pub_logged != logged)
				{
					s_pub_logged = logged;
					Console.WriteLn("(VR) Screen transform (menu pointer): %s centred at (%.2f, %.2f, %.2f), %.2f x %.2f m.",
						tablet ? "menu tablet" : "world screen", q.pose.position.x, q.pose.position.y, q.pose.position.z,
						q.size.width, q.size.height);
				}
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
				publish_quad();
			}
			else if (l0 || l1)
			{
				make_layer(l0 ? 0 : 1, XR_EYE_VISIBILITY_BOTH);
				publish_quad();
			}
			if (!pub.valid)
				s_pub_logged = 0;
		}

		if (s_chainlog)
		{
			const char* branch = (layer_count == 0) ? "Z" :
				cl_split ? "SP" :
				(layer_count == 2) ? "S" :
				(s.chains[0].ever_released ? "B0" : "B1");
			GSTextureVK* cur = static_cast<GSTextureVK*>(current);
			Console.WriteLn("(VR) CHAINLOG v=%llu render=%d cur=%s%ux%u/%uL layered=%d stereo=%d "
							"c0={r=%d er=%d wp=%d wd=%d} c1={r=%d er=%d wp=%d wd=%d} branch=%s layers=%u",
				static_cast<unsigned long long>(s_chainlog_visit++), should_render ? 1 : 0,
				current ? "" : "NULL:", cur ? static_cast<u32>(cur->GetWidth()) : 0,
				cur ? static_cast<u32>(cur->GetHeight()) : 0, cur ? cur->GetArrayLayers() : 0,
				layered ? 1 : 0, stereo ? 1 : 0,
				cl_copy[0], s.chains[0].ever_released ? 1 : 0, s.chains[0].wait_pending ? 1 : 0, s.chains[0].waited ? 1 : 0,
				cl_copy[1], s.chains[1].ever_released ? 1 : 0, s.chains[1].wait_pending ? 1 : 0, s.chains[1].waited ? 1 : 0,
				branch, layer_count);
		}

		// Every present, valid or not, so the menu pointer never aims at a screen that has gone.
		VR::PublishScreenTransform(pub);
		if (!force_zero_layers)
			BuildLeverLayers(lever_quads, layers, layer_count, kLayerCapacity, cl_split);

		// Hands go last so they draw over the screen and the lever cards.
		const u32 hands_from = layer_count;
		if (!force_zero_layers && should_render)
			BuildHandLayers(hand_quads, layers, layer_count, kLayerCapacity, hand_att);

		// HUD cards last of all, over the hands and everything else; only the laser sight (or menu beam), the menu
		// pointer's dot and the toast go in under the hands.
		if (!force_zero_layers && should_render)
			BuildHudLayers(hud_quads, layers, layer_count, kLayerCapacity, hands_from, hud_att);

		for (u32 l = 0; l < 2; l++)
		{
			if (cl_copy[l] == static_cast<int>(CopyResult::Skipped))
				s.stat_copy_skipped.fetch_add(1, std::memory_order_relaxed);
			else if (cl_copy[l] == static_cast<int>(CopyResult::TimeoutZeroLayer))
				s.stat_copy_timeout.fetch_add(1, std::memory_order_relaxed);
		}

		// The edge fill behind the first-person screen, released here, before the layers that go with it are published
		// (anything else has the black background behind it).
		if (fill_geo && should_render && current)
			UpdateEdgeFill(static_cast<GSTextureVK*>(current), fill_distance, fill_width, fill_height);
		else
			s_fill.live.store(false, std::memory_order_release);

		const LayerSources sources = {quads, cyls, 5, lever_quads, hand_quads, hud_quads, hand_att, hud_att, &s_pointer_pub};

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
			MakeBackgroundLayer(eye_views, bg_views, &bg, pub_fp_screen && s_fill.live.load(std::memory_order_acquire));
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
		s_legacy_stats.end_sum_ms += end_ms;
		s_legacy_stats.end_n++;
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
		DestroyFillResources();

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
			chain.waited = false;
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

		{
			std::lock_guard<std::mutex> lock(s_screen_mutex);
			s_fp_screen_info = FirstPersonScreenInfo();
		}
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

	FirstPersonScreenInfo GetFirstPersonScreenInfo()
	{
		std::lock_guard<std::mutex> lock(s_screen_mutex);
		return s_fp_screen_info;
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

	void UpdateMenuScreen(float distance_m, float height_m, float drop_m, float tilt_deg)
	{
		const auto finite_or = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
		std::lock_guard<std::mutex> lock(s_screen_mutex);
		s_screen_params.has_menu = true;
		s_screen_params.menu_distance = std::clamp(finite_or(distance_m, 0.70f), 0.3f, 2.0f);
		s_screen_params.menu_height = std::clamp(finite_or(height_m, 0.36f), 0.15f, 1.0f);
		s_screen_params.menu_drop = std::clamp(finite_or(drop_m, 0.30f), -0.5f, 0.8f);
		s_screen_params.menu_tilt_deg = (std::isfinite(tilt_deg) && tilt_deg >= 0.0f) ? std::min(tilt_deg, 89.0f) : -1.0f;
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
