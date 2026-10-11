// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/StereoState.h"

#include <atomic>
#include <chrono>
#include <mutex>

namespace VR::StereoState
{
	namespace
	{
		std::mutex s_mutex;
		Params s_params;
	}

	void Publish(const Params& params)
	{
		std::lock_guard lock(s_mutex);
		s_params = params;
	}

	Params Get()
	{
		std::lock_guard lock(s_mutex);
		return s_params;
	}

	namespace
	{
		u32 s_current_eye = 0;
	}

	u32 GetCurrentEye()
	{
		return s_current_eye;
	}

	float GetCurrentEyeSign()
	{
		return (s_current_eye == 0) ? -1.0f : 1.0f;
	}

	void AdvanceEye()
	{
		s_current_eye ^= 1u;
	}

	namespace
	{
		std::atomic<float> s_head_roll{0.0f};
		// Steady-clock milliseconds of the last PublishHeadRoll; 0 = never published.
		std::atomic<s64> s_head_roll_ms{0};
		std::atomic<bool> s_roll_parallax{true};
		std::atomic<bool> s_dump_allowed{false};

		s64 NowMs()
		{
			const s64 ms = static_cast<s64>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
			return (ms != 0) ? ms : 1;
		}

		constexpr s64 HEAD_ROLL_MAX_AGE_MS = 150;
	}

	void PublishHeadRoll(float roll_rad)
	{
		s_head_roll.store(roll_rad, std::memory_order_relaxed);
		s_head_roll_ms.store(NowMs(), std::memory_order_release);
	}

	bool HeadRoll(float* roll_rad)
	{
		const s64 stamp = s_head_roll_ms.load(std::memory_order_acquire);
		if (stamp == 0)
			return false;
		const s64 age = NowMs() - stamp;
		if (age < 0 || age > HEAD_ROLL_MAX_AGE_MS)
			return false;
		if (roll_rad)
			*roll_rad = s_head_roll.load(std::memory_order_relaxed);
		return true;
	}

	void SetRollParallaxEnabled(bool enabled)
	{
		s_roll_parallax.store(enabled, std::memory_order_relaxed);
	}

	bool RollParallaxEnabled()
	{
		return s_roll_parallax.load(std::memory_order_relaxed);
	}

	void SetDumpAllowed(bool allowed)
	{
		s_dump_allowed.store(allowed, std::memory_order_relaxed);
	}

	bool DumpAllowed()
	{
		return s_dump_allowed.load(std::memory_order_relaxed);
	}
}
