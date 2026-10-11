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

	// The headset's eyes as the runtime last located them (xrLocateViews): IPD, each eye's field of view in degrees
	// (left, right, up, down, signed as OpenXR reports them: left and down negative) and the display rate. Any thread.
	struct EyeInfo
	{
		bool valid = false;
		float ipd_m = 0.0f;
		float fov_deg[2][4] = {};
		float display_hz = 0.0f;
	};
	EyeInfo GetEyeInfo();
}
