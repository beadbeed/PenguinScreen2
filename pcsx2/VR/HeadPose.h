// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

namespace VR::HeadPose
{
	struct Snapshot
	{
		float orientation_x = 0.0f;
		float orientation_y = 0.0f;
		float orientation_z = 0.0f;
		float orientation_w = 1.0f;

		float position_x = 0.0f;
		float position_y = 0.0f;
		float position_z = 0.0f;

		// Orientation can be tracked while position is not (then position_* is 0).
		bool position_valid = false;

		bool valid = false;

		u64 frame = 0;

		// Steady-clock milliseconds when the pose was taken (CameraDriver::SteadyNowMs's clock), so the GS
		// thread can tell how old a frame's pose is by the time the frame is shown. 0 = unknown.
		u64 publish_ms = 0;
	};

	void Publish(const Snapshot& pose);

	void Invalidate();

	Snapshot Get();
}
