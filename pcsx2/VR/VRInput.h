// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include "VR/HeadPose.h"
#include "VR/VRInputState.h"

#include <openxr/openxr.h>

#include <array>
#include <atomic>

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

	private:
		static constexpr int HAND_COUNT = 2;

		bool CreateActionSetAndActions(XrInstance instance);
		bool SuggestBindings(XrInstance instance);
		bool AttachAndCreateSpaces(XrInstance instance, XrSession session);

		bool CreateAction(XrInstance instance, XrActionType type, const char* name,
			const char* localized_name, XrAction* out);

		bool ReadHand(XrSession session, int hand, XrTime time, VRHandState* out);

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
	};

	namespace XRInput
	{
		bool Initialize();

		void Update(XrTime display_time);

		void Shutdown();

		bool IsInitialized();

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

		// The scripted head while test input is live and supplies one.
		bool TestHeadPose(HeadPose::Snapshot* out);
	}
}
