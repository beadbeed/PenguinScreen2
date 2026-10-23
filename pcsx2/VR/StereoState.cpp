// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/StereoState.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
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

	namespace
	{
		std::mutex s_cam_mutex;
		float s_cam[4] = {};
		s64 s_cam_ms = 0;
		std::atomic<bool> s_true_stereo{false};
	}

	void PublishGameCamera(float fx, float fy, float cx, float cy)
	{
		std::lock_guard<std::mutex> lock(s_cam_mutex);
		s_cam[0] = fx;
		s_cam[1] = fy;
		s_cam[2] = cx;
		s_cam[3] = cy;
		s_cam_ms = NowMs();
	}

	bool GameCamera(float* fx, float* fy, float* cx, float* cy)
	{
		std::lock_guard<std::mutex> lock(s_cam_mutex);
		const s64 age = NowMs() - s_cam_ms;
		if (s_cam_ms == 0 || age < 0 || age > 250)
			return false;
		*fx = s_cam[0];
		*fy = s_cam[1];
		*cx = s_cam[2];
		*cy = s_cam[3];
		return true;
	}

	namespace
	{
		std::mutex s_cut_mutex;
		CutsceneEyes s_cut_req;
		s64 s_cut_req_ms = 0;
		bool s_cut_used = false;
		float s_cut_used_pos[2][3] = {};
		float s_cut_used_fov[2][4] = {};
	}

	void SetCutsceneRequest(const CutsceneEyes& eyes)
	{
		std::lock_guard<std::mutex> lock(s_cut_mutex);
		s_cut_req = eyes;
		s_cut_req_ms = NowMs();
	}

	bool CutsceneRequest(CutsceneEyes* out)
	{
		std::lock_guard<std::mutex> lock(s_cut_mutex);
		const s64 age = NowMs() - s_cut_req_ms;
		if (!s_cut_req.valid || s_cut_req_ms == 0 || age < 0 || age > 250)
			return false;
		*out = s_cut_req;
		return true;
	}

	void SetCutsceneUsed(const float pos_m[2][3], const float fov_rad[2][4])
	{
		std::lock_guard<std::mutex> lock(s_cut_mutex);
		std::memcpy(s_cut_used_pos, pos_m, sizeof(s_cut_used_pos));
		std::memcpy(s_cut_used_fov, fov_rad, sizeof(s_cut_used_fov));
		s_cut_used = true;
	}

	bool TakeCutsceneUsed(float pos_m[2][3], float fov_rad[2][4])
	{
		std::lock_guard<std::mutex> lock(s_cut_mutex);
		if (!s_cut_used)
			return false;
		std::memcpy(pos_m, s_cut_used_pos, sizeof(s_cut_used_pos));
		std::memcpy(fov_rad, s_cut_used_fov, sizeof(s_cut_used_fov));
		s_cut_used = false;
		return true;
	}

	void SetTrueStereoActive(bool active)
	{
		s_true_stereo.store(active, std::memory_order_relaxed);
	}

	bool TrueStereoActive()
	{
		static const bool s_test = (std::getenv("PCSX2_VR_TRUESTEREO_TEST") != nullptr);
		return s_test || s_true_stereo.load(std::memory_order_relaxed);
	}
}
