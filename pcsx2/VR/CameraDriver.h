// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/HeadPose.h"

namespace VR::ProfileDB
{
	struct CameraPadLook;
}

namespace VR::CameraDriver
{
	struct PadLookState
	{
		int gate = 0;
		int latch = 0;
	};

	float PadLookDeflection(const ProfileDB::CameraPadLook& pl, float yaw_deg, PadLookState& st);

	void Apply();

	// True while camera.lookAt is driving the game camera (any thread).
	bool LookAtActive();

	// The head pose camera.lookAt drove the game camera with at (or just before) time_ms, on the
	// SteadyNowMs() clock, so the compositor can show each frame where it was rendered from instead
	// of gluing an old frame to the face. False when nothing recent is recorded. Any thread.
	bool FirstPersonPoseAt(u64 time_ms, HeadPose::Snapshot* out);
	u64 SteadyNowMs();

	// GS thread: the head pose the frame being presented was rendered from, matched through the game's
	// view matrix and sent down the GS queue in step with the frames. False when not known (no
	// viewMatrix in the profile, first person not active, or no frame matched yet). turn_yaw (radians,
	// OpenXR yaw, may be null) is how much further round the frame belongs because the view has been
	// turned by the stick (or the character's heading) since it was rendered; about 0 through a smooth
	// turn that lookAt.predict renders ahead. Each call also measures the prediction horizon.
	bool RenderPose(HeadPose::Snapshot* out, float* turn_yaw);

	// The measured head-pose-to-present horizon in seconds (clamped to 0-0.12, 0 until measured): how far
	// ahead lookAt.predict leads the head. Any thread.
	float HeadPredictionHorizon();

	// PCSX2_VR_FAKE_HEADPOSE: the fake head's yaw (radians, OpenXR, CCW positive) at time_ms on the
	// SteadyNowMs() clock, so the compositor can compare where a frame is placed with where the fake head is
	// when it is shown. False (yaw untouched) when the fake head is off. Any thread.
	bool FakeHeadYawAt(u64 time_ms, float* yaw);

	// Any thread. The recenter happens on the next armed vsync, which then shows a "Recentered" toast
	// unless toast is false (for a caller that announces it itself, like the runtime-recenter follow).
	void RequestRecenter(bool toast = true);

	// CPU thread, after Apply: true while first person is armed (guards holding, head pose valid) and this
	// vsync resolved camera.base; *base (may be null) is that record, the local character's in Outbreak.
	bool LocalRecord(u32* base);

	void OnStateLoaded();

	// Why the camera is not armed (the DISARMED log line), for telemetry.
	enum class DisarmReason : u8
	{
		Armed = 0,
		NoVM = 1,
		NoProfile = 2,   // no profile, or it has no camera section
		SwitchedOff = 3, // VR, the head-tracked camera setting, or split-screen
		NotRunning = 4,  // paused
		NoHeadPose = 5,
		GuardFailed = 6,
	};

	// What Apply() saw on the last vsync, for the PINE test runner (MsgVRTelemetry) and later the
	// flight recorder. Any thread.
	struct Telemetry
	{
		u64 vsync = 0;
		bool armed = false;
		DisarmReason disarm_reason = DisarmReason::NoVM;
		u32 guard_fail_vsyncs = 0;
		bool look_at_active = false;
		bool pause_when_hit = false; // lookAt.pauseWhen passed (a menu is open)
		bool weapon_raised = false;  // lookAt.aim stance
		bool aim_moving = false;     // walk-and-shoot owns the move stick
		bool yaw_anchor_valid = false;
		float yaw_anchor = 0.0f;   // game yaw, radians
		float base_yaw_now = 0.0f; // game yaw minus yawSign * the recenter yaw, radians (true, not predicted)
		u32 match_age = 0;         // how many writes back the newest matched frame was (0: none)
		u32 frames_matched = 0;
		bool online_safe = false;
		bool test_head = false;     // the head pose came from PINE test input
		float prediction_ms = 0.0f; // lookAt.predict horizon a smooth turn is rendered ahead by (0: off or not measured)
	};

	Telemetry GetTelemetry();

	bool SelfTestAssembler();

	bool SelfTestMath();
}
