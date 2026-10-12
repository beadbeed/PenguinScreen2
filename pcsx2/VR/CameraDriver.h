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

	// Comfort vignette: how much the view is moving without the head right now, 0-1. The larger of a smooth stick
	// turn's rate against lookAt.smoothTurnDegPerSec and walking (the move stick's push, or the character's own
	// speed against a brisk walk), eased in over about 0.15 s and out over 0.3 s. Snap turns count nothing (the
	// comfort blink covers them). Published by Apply (CPU thread) only while the VR ComfortVignette setting is on
	// and camera.lookAt drives the camera; 0 at once otherwise, after a savestate load, and when Apply stops
	// running (pause, stall). Any thread.
	float ComfortMotion();

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

	// GS thread, alongside RenderPose: the vertical FOV (degrees) the frame being presented was rendered with, from the
	// same matched write. False when not known (no matched frame, or camera.fov does not size the first-person screen).
	bool RenderFov(float* fov_deg);

	// First-person field of view (VR setting FirstPersonFov). camera.fov is taken as the game camera's vertical FOV in
	// degrees when the profile's flat screen.firstPerson is sized to it (ScreenMatchesFov); the first-person screen then
	// follows the FOV each frame was rendered with. Auto fits the FOV to the headset on each first-person arm
	// (AutoFirstPersonFovDeg), easing it in from camera.fov over kFovEaseVsyncs; it changes while armed only once, when
	// the headset's eyes first arrive. Profile writes camera.fov unchanged. The maths below is pure.
	inline constexpr float kMaxAutoFovDeg = 130.0f;
	inline constexpr float kDefaultFovMarginDeg = 6.0f;
	inline constexpr u32 kFovEaseVsyncs = 30;

	// The vertical FOV that covers both eyes plus margin_deg. fov_deg[eye][L, R, U, D] in degrees as
	// XRCompositor::EyeInfo has them (left and down negative). The game's projection is symmetric about its axis, so it
	// takes twice the further of the eyes' furthest up and down reach (each plus half the margin), widened if a picture
	// `aspect` times as wide as tall would not reach their furthest left or right (plus half the margin) either. Clamped
	// to [profile_fov_deg, max(profile_fov_deg, kMaxAutoFovDeg)]; profile_fov_deg when the eyes are not usable.
	float AutoFirstPersonFovDeg(const float fov_deg[2][4], float aspect, float margin_deg, float profile_fov_deg);

	// The height of a flat screen distance_m away that spans fov_deg vertically: 2 * distance * tan(fov / 2). 0 for a
	// FOV outside 1-179 deg or a distance not above 0.
	float ScreenHeightForFov(float distance_m, float fov_deg);

	// True when a flat screen distance_m away and height_m tall spans fov_deg (within 2%): the profile's first-person
	// screen is sized to camera.fov read as a vertical FOV in degrees, so that FOV may be refitted and the screen follows.
	bool ScreenMatchesFov(float distance_m, float height_m, float fov_deg);

	// from + (to - from) * smoothstep(step / kFovEaseVsyncs).
	float EaseFov(float from, float to, u32 step);

	// What camera.fov writes, for the compositor (the first-person screen's size, its log) and auto stereo. Any thread.
	struct FovState
	{
		bool sizes_screen = false; // camera.fov is a vertical FOV in degrees that sizes the first-person screen
		bool auto_fit = false;     // the target is fitted to the headset (FirstPersonFov = Auto, eyes known)
		float fov_deg = 0.0f;      // written on the latest armed vsync (0: none yet)
		float target_deg = 0.0f;   // what this arm eases to
		float profile_deg = 0.0f;  // camera.fov's own value (the floor)
		float margin_deg = 0.0f;   // FirstPersonFovMarginDeg as applied
		float aspect = 0.0f;       // the first-person screen's aspect the fit used
		float reach_up_deg = 0.0f, reach_down_deg = 0.0f, reach_side_deg = 0.0f; // the eyes' furthest reach (auto_fit)
		const char* source = "";   // why: "auto", or the reason the profile's value is used
	};
	FovState GetFovState();

	// The measured head-pose-to-present horizon in seconds (clamped to 0-0.12, 0 until measured): how far
	// ahead lookAt.predict leads the head. Any thread.
	float HeadPredictionHorizon();

	// How fast RenderPose's turn_yaw is changing right now, in radians per second of OpenXR yaw: a smooth stick turn
	// in progress (0 without one, or while first person is not active). The VR pacer turns the first-person screen
	// on by it toward each display time between GS frames. Any thread.
	float ScreenTurnRate();

	// PCSX2_VR_FAKE_HEADPOSE: the fake head's yaw (radians, OpenXR, CCW positive) at time_ms on the
	// SteadyNowMs() clock, so the compositor can compare where a frame is placed with where the fake head is
	// when it is shown. False (yaw untouched) when the fake head is off. Any thread.
	bool FakeHeadYawAt(u64 time_ms, float* yaw);

	// Any thread. The recenter happens on the next armed vsync, which then shows a "Recentered" toast
	// unless toast is false (for a caller that announces it itself, like the runtime-recenter follow).
	void RequestRecenter(bool toast = true);

	// CPU thread, after Apply: true while first person is armed (guards holding, head pose valid) and this
	// vsync resolved camera.base; *base (may be null) is that record, the local character's in Outbreak.
	// guards_now (optional): this vsync's camera guards held too, not just the disarm grace after they failed
	// (overlay data such as the GAME overlay's tables is only resident then).
	bool LocalRecord(u32* base, bool* guards_now = nullptr);

	// CPU thread, after Apply: true while the game runs, the camera guards pass (or failed for no longer than
	// the profile's disarm grace) and this vsync resolved camera.base, whether or not first person is armed. Valid while grabbed, down or dead too (it ignores
	// releaseWhen, the head pose and the network mode). For reads only (GameFeedback's hurt and weapon cues, and the
	// grab breakout's grabbed checks); resolved only for profiles with a feedback or breakout block.
	bool LocalRecordAny(u32* base);

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
		OnlineRestricted = 7, // online, and not in private co-op (obsrv, an unknown server, or the setting is off)
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
