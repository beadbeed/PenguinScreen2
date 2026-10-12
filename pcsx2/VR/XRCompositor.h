// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

class GSTexture;

namespace VR::XRCompositor
{
	bool Initialize();

	void Shutdown();

	inline constexpr u32 MonoEye = 2;

	void EndOfFrame(GSTexture* current, u32 eye);

	void UpdateScreenParams(float distance_m, float height_m, float arc_deg, float vertical_offset_m,
		bool follow_head = false);

	// Head-locked screen used while CameraDriver::LookAtActive() (profile screen.firstPerson).
	void UpdateFirstPersonScreen(bool enabled, float distance_m, float height_m, float arc_deg, float pose_lag_ms);

	// The menu tablet (VR setting MenuTablet, profile screen.menu): while a menu is up in first-person play it is shown on
	// a small flat screen distance_m ahead of the eyes and drop_m below them, height_m tall, tilted back tilt_deg (below 0:
	// to face the eyes exactly). Clamped to 0.3-2.0 m, 0.15-1.0 m, -0.5-0.8 m and 0-89 deg.
	void UpdateMenuScreen(float distance_m, float height_m, float drop_m, float tilt_deg);

	void RequestScreenReanchor();

	bool IsReanchorRequested();
	void ClearReanchorRequestForTest();

	struct ScreenAnchor
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
		float yaw = 0.0f;
		u32 generation = 0;
	};
	ScreenAnchor GetScreenAnchor();

	// True after Shutdown had to leave the VR pacer stuck inside the runtime (detached): nothing may call into the
	// runtime again, so the XR handles are left alive until the process exits. GS thread.
	bool Abandoned();

	// True while the VR pacer submits the frames at the headset's rate ([VR] HeadsetRateSubmit) and so also pumps
	// the session events; false with legacy one-frame-per-present submission or no compositor. GS thread.
	bool PacerOwnsFrames();

	// True while PacerOwnsFrames() but the VR pacer has not come round its loop for about 250 ms (stuck in
	// xrWaitFrame or another runtime call: the runtime died or stopped pacing), or has exited. VRManager then pumps
	// the session events itself so a loss still reaches the teardown, and EndOfFrame skips the screen, hand and HUD
	// uploads.
	// GS thread.
	bool PacerStalled();

	// The headset's eyes as the runtime last located them (xrLocateViews): IPD, each eye's field of view in degrees
	// (left, right, up, down, signed as OpenXR reports them: left and down negative), how far each view is turned outward
	// from straight ahead (canted displays; 0 for parallel views) and the display rate. Any thread.
	struct EyeInfo
	{
		bool valid = false;
		float ipd_m = 0.0f;
		float fov_deg[2][4] = {};
		float cant_deg = 0.0f;
		float display_hz = 0.0f;
	};
	EyeInfo GetEyeInfo();

	// The first-person screen as placed (the same geometry the 'First-person screen' line logs), refreshed every
	// present: its distance, real width (the arc length for a cylinder) and height in metres, the aspect it was sized
	// with, and whether it is a cylinder. A flat screen is sized to the FOV camera.fov settles to on this first-person arm
	// when that FOV sizes it (CameraDriver::GetFovState().target_deg), else to the profile's screen.firstPerson height;
	// the screen as placed follows each frame's own FOV, the same once the arm's half-second ease is done. valid = the
	// profile has a first-person screen. Any thread.
	struct FirstPersonScreenInfo
	{
		bool valid = false;
		float distance_m = 0.0f, width_m = 0.0f, height_m = 0.0f, aspect = 0.0f;
		bool curved = false;
	};
	FirstPersonScreenInfo GetFirstPersonScreenInfo();
}
