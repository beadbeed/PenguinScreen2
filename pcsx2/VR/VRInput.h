// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include "VR/HeadPose.h"
#include "VR/VRInputState.h"

#include <openxr/openxr.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace VR
{
	class VRInput final
	{
	public:
		VRInput();
		~VRInput();

		VRInput(const VRInput&) = delete;
		VRInput& operator=(const VRInput&) = delete;
		VRInput(VRInput&&) = delete;
		VRInput& operator=(VRInput&&) = delete;

		bool Initialize();

		void Shutdown();

		bool IsInitialized() const { return m_action_set != XR_NULL_HANDLE; }

		bool Update(XrTime display_time);

		void TriggerHaptic(int hand, float duration_seconds = 0.f, float frequency_hz = 0.f, float amplitude = 1.f);
		void StopHaptic(int hand);

		// Logs each hand's current interaction profile and what SteamVR bound the main actions to, and
		// warns when the Steam Frame is running on the Touch bindings or without a D-pad. `why` goes in
		// the log line (first focused sync, profile changed).
		void ReportBindings(const char* why);

	private:
		static constexpr int HAND_COUNT = 2;

		bool CreateActionSetAndActions(XrInstance instance);
		bool SuggestBindings(XrInstance instance);
		bool AttachAndCreateSpaces(XrInstance instance, XrSession session);

		bool CreateAction(XrInstance instance, XrActionType type, const char* name,
			const char* localized_name, XrAction* out);

		bool ReadHand(XrSession session, int hand, XrTime time, VRHandState* out);

		// Controller presence: says in the log and the headset when a hand goes quiet or comes back.
		void UpdatePresence(const VRInputSnapshot& snapshot, bool actions_active);
		void ResetPresence();
		// HudCards::Toast for 4 s, but each text at most once per 20 s. False when held back.
		bool ToastLimited(const std::string& text);

		bool m_attached = false;
		XrActionSet m_action_set = XR_NULL_HANDLE;
		std::array<XrPath, HAND_COUNT> m_hand_paths{};

		XrAction m_aim_pose = XR_NULL_HANDLE;
		XrAction m_grip_pose = XR_NULL_HANDLE;
		XrAction m_button_a = XR_NULL_HANDLE;
		XrAction m_button_b = XR_NULL_HANDLE;
		XrAction m_button_x = XR_NULL_HANDLE;
		XrAction m_button_y = XR_NULL_HANDLE;
		XrAction m_button_menu = XR_NULL_HANDLE;
		XrAction m_button_view = XR_NULL_HANDLE;
		XrAction m_thumbstick_click = XR_NULL_HANDLE;
		XrAction m_dpad_up = XR_NULL_HANDLE;
		XrAction m_dpad_down = XR_NULL_HANDLE;
		XrAction m_dpad_left = XR_NULL_HANDLE;
		XrAction m_dpad_right = XR_NULL_HANDLE;
		XrAction m_bumper = XR_NULL_HANDLE;
		XrAction m_trigger = XR_NULL_HANDLE;
		XrAction m_grip = XR_NULL_HANDLE;
		XrAction m_thumbstick = XR_NULL_HANDLE;
		XrAction m_haptic = XR_NULL_HANDLE;

		std::array<XrSpace, HAND_COUNT> m_aim_spaces{};
		std::array<XrSpace, HAND_COUNT> m_grip_spaces{};
		XrSpace m_view_space = XR_NULL_HANDLE;

		int m_last_actions_active = -1;
		bool m_locate_warned = false;

		enum class HandLiveness : u8
		{
			Unknown,
			Live,
			Absent,
		};

		struct HandPresence
		{
			HandLiveness state = HandLiveness::Unknown;
			bool run_live = false;          // live_now over the current run
			std::uint64_t run_start_ms = 0; // when the current run of run_live began
			bool ever_live = false;         // became Live at least once this session
			bool announced_absent = false;  // a toast said this hand is missing: its return gets one too
		};

		std::array<HandPresence, HAND_COUNT> m_presence{};
		bool m_presence_paused = true;        // last sync was unfocused or under PINE test input
		std::uint64_t m_presence_last_ms = 0; // last evaluated sync
		std::uint64_t m_focused_ms = 0;       // focused, evaluated time this session
		bool m_start_checked = false;         // the session-start controller check has run
		std::uint64_t m_unfocused_start_ms = 0; // the session's first sync was unfocused, at this time
		bool m_bindings_reported = false;     // ReportBindings ran for this session's first focused sync
		bool m_touch_toasted = false;
		std::vector<std::pair<std::string, std::uint64_t>> m_toast_times;
	};

	namespace XRInput
	{
		bool Initialize();

		void Update(XrTime display_time);

		void Shutdown();

		bool IsInitialized();

		// The runtime's interaction profile changed (XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED): the
		// next Update reports the bindings again. Any thread.
		void OnInteractionProfileChanged();

		void QueueHaptic(int hand, float amplitude);

		void QueuePulse(int hand, float amplitude, float seconds);

		bool TakePendingPulseForTest(int hand, float* amplitude, float* seconds);

		void PublishSnapshotForTest(const VRInputSnapshot& snapshot);

		void ResetSnapshotForTest();

		// Scripted controllers for the headset-free test runner (PINE MsgVRFakeInput, only with
		// PCSX2_VR_TEST_INPUT set and never online). GetInputSnapshot() lays it over the real input
		// until it expires, so every consumer (camera, zones, gamepad, hands) takes the real path.
		struct TestInput
		{
			struct Hand
			{
				bool supplied = false;      // replace this hand's real state; otherwise it is left alone
				bool head_relative = false; // poses are in the head's floor-yaw frame, positions from the head
				VRPose aim_pose;            // .valid: tracked
				VRPose grip_pose;
				float trigger = 0.f;
				float grip = 0.f;
				float thumbstick_x = 0.f;
				float thumbstick_y = 0.f;
				// Bit i is the i-th button in VRHandState order: a, b, x, y, menu, thumbstick_click,
				// dpad_up, dpad_down, dpad_left, dpad_right, bumper, view.
				u32 buttons = 0;
			};

			VRPose head_pose; // replaces the real head while .valid
			std::array<Hand, 2> hands;
			u32 ttl_ms = 0; // capped at 1000 ms; 0 drops any test input now
		};

		void SetTestInput(const TestInput& input);

		// A scripted menu-pointer hit for the headset-free tests (PINE MsgVRMenuPointer op 1, only with PCSX2_VR_TEST_INPUT
		// set and never online): GetInputSnapshot() reports a screen hit at (u, v) (fractions of the game picture) for
		// `hand` (0 left, 1 right) over the aim rays' until it lapses after ttl_ms (capped at 1000 ms). With a screen
		// transform published, the hit's point is placed on that quad. u below 0, or ttl_ms 0, drops it now.
		void SetTestScreenHit(int hand, float u, float v, u32 ttl_ms);

		// The scripted head while test input is live and supplies one.
		bool TestHeadPose(HeadPose::Snapshot* out);
	}
}
