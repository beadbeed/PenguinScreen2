// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

namespace VR::StereoState
{
	struct Params
	{
		bool  enabled     = false;
		float separation  = 0.0f;
		float convergence = 0.0f;

		enum class UvPolicy { Screen, World } uv_policy = UvPolicy::Screen;

		bool pin_uniform_q = false;

		bool z_driven_depth = false;

		float collimate_disparity = 0.0f;

		struct CollimateRule
		{
			s8 prim = -1;
			s8 tme = 1;
			s8 abe = -1;
			s32 min_w = 0;
			s32 max_w = 0;
			s32 min_h = 0;
			s32 max_h = 0;
			float rx0 = 0.0f, ry0 = 0.0f, rx1 = 0.0f, ry1 = 0.0f;
			float tu0 = 0.0f, tv0 = 0.0f, tu1 = 0.0f, tv1 = 0.0f;
			char label[16] = {};
		};
		static constexpr u32 MAX_COLLIMATE_RULES = 4;
		CollimateRule collimate_rules[MAX_COLLIMATE_RULES] = {};
		u32 collimate_rule_count = 0;

		enum class Map : u32
		{
			Linear = 0,
			Bands  = 1,
			Log    = 2,
		};

		Map map = Map::Linear;
		u32 band_count = 1;

		float split_q[3] = {};
		float conv[4] = {};
		float sep[4] = {};
		float bias[4] = {};

		float log_w0 = 0.0f;
		float log_w1 = 0.0f;
		float log_dfar = 0.0f;

		// First-person scene armed (linear map only).
		bool first_person = false;
		// Linear-map disparity floor, in units of separation: 0 keeps the old clamp at the
		// convergence plane, -1 lets content reach crossed disparity -separation.
		float near_floor = 0.0f;
	};

	void Publish(const Params& params);

	Params Get();

	u32 GetCurrentEye();

	float GetCurrentEyeSign();

	void AdvanceEye();

	// Head roll (radians, game camera convention) published by the camera driver each
	// time it writes the look-at. HeadRoll returns false if never set or older than 150 ms.
	void PublishHeadRoll(float roll_rad);
	bool HeadRoll(float* roll_rad);

	// Rotate first-person parallax with head roll (VR.RollParallax).
	void SetRollParallaxEnabled(bool enabled);
	bool RollParallaxEnabled();

	// Offline-only gate for the headset-free stereo dump (false while the network adapter is on).
	void SetDumpAllowed(bool allowed);
	bool DumpAllowed();
}
