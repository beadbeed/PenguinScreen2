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
	// turned by the stick (or the character's heading) since it was rendered.
	bool RenderPose(HeadPose::Snapshot* out, float* turn_yaw);

	void RequestRecenter();

	void OnStateLoaded();

	bool SelfTestAssembler();

	bool SelfTestMath();
}
