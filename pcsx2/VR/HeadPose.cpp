// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/HeadPose.h"

#include <chrono>
#include <mutex>

namespace VR::HeadPose
{
	namespace
	{
		std::mutex s_mutex;
		Snapshot s_pose;

		u64 s_frame = 0;

		// The same clock as CameraDriver's NowMs.
		u64 SteadyMs()
		{
			return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}
	}

	void Publish(const Snapshot& pose)
	{
		const u64 now = SteadyMs();
		std::lock_guard lock(s_mutex);
		s_pose = pose;
		s_pose.frame = ++s_frame;
		s_pose.publish_ms = now;
	}

	void Invalidate()
	{
		std::lock_guard lock(s_mutex);
		s_pose = Snapshot{};
		s_pose.frame = ++s_frame;
	}

	Snapshot Get()
	{
		std::lock_guard lock(s_mutex);
		return s_pose;
	}
}
