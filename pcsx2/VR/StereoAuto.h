// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

// First-person auto stereo: separation and convergence worked out from the headset's IPD and the first-person
// screen as placed, instead of from hand-typed profile numbers. Pure maths (no globals beyond the self-test latch);
// VRManager's ApplySceneStereo builds the Inputs every vsync and eases the published values in.
//
// On a screen W metres wide at d metres, the shader's disparity between the eyes is sep * W * (1 - conv / w) metres
// for a draw at view depth w (game units). Orthostereo: far content lands exactly one IPD apart (sep = IPD / W) and
// zero disparity sits on the screen (conv = d * unitsPerMeter). Strength s scales it: below 1 the baseline shrinks
// with convergence held on the screen (depth flattens toward the screen), above 1 convergence moves out while far
// disparity stays one IPD, so distant objects never pass parallel.
namespace VR::StereoAuto
{
	// Rails. Outside the IPD, width or distance rails the profile numbers are used instead.
	inline constexpr float kMinIpdM = 0.045f;
	inline constexpr float kMaxIpdM = 0.080f;
	inline constexpr float kMinScreenWidthM = 1.0f;
	inline constexpr float kMaxScreenWidthM = 12.0f;
	inline constexpr float kMinScreenDistanceM = 0.5f;
	inline constexpr float kMaxScreenDistanceM = 5.0f;
	inline constexpr float kMaxSeparation = 0.03f;
	inline constexpr float kMinAutoConvergence = 20.0f;
	inline constexpr float kMaxAutoConvergence = 600.0f;
	inline constexpr float kMinNearFloor = -2.0f;
	inline constexpr float kMinStrength = 0.5f;
	inline constexpr float kMaxStrength = 1.5f;
	inline constexpr float kMinNdcWidthRatio = 0.5f;
	inline constexpr float kMaxNdcWidthRatio = 2.0f;
	inline constexpr float kDefaultUnitsPerMeter = 100.0f;

	// Every first-person arm, and every later target change, eases over this many vsyncs (0.5 s at 59.94 Hz).
	inline constexpr u32 kEaseVsyncs = 30;

	enum class Source : u8
	{
		Auto,
		Profile,
	};

	// Why the profile numbers were used (None with Source::Auto).
	enum class Reason : u8
	{
		None,
		ProfileNoAuto,
		SettingOff,
		NoEyes,
		IpdOutOfRange,
		NoScreen,
		ScreenOutOfRange,
	};

	const char* ReasonText(Reason reason);

	struct Inputs
	{
		bool profile_auto = false; // the profile has stereo.firstPerson.auto
		bool setting_auto = true; // [VR] FirstPersonAutoStereo
		bool eyes_valid = false; // XRCompositor::GetEyeInfo().valid
		float ipd_m = 0.0f;
		bool ipd_is_test = false; // PCSX2_VR_FAKE_IPD_MM replaced the runtime's IPD
		bool screen_valid = false; // XRCompositor::GetFirstPersonScreenInfo().valid
		float screen_distance_m = 0.0f;
		float screen_width_m = 0.0f; // the arc length for a cylinder
		float screen_height_m = 0.0f;
		bool screen_curved = false;
		float units_per_meter = kDefaultUnitsPerMeter; // camera.lookAt.unitsPerMeter
		float profile_sep = 0.0f; // stereo.firstPerson separation (the fallback)
		float profile_conv = 0.0f; // stereo.firstPerson convergence (the fallback)
		float nearest_m = 0.0f; // 0 = off
		float strength = 1.0f; // [VR] FirstPersonStereoStrength
		float ndc_width_ratio = 1.0f; // stereo.firstPerson.ndcWidthRatio
	};

	struct Result
	{
		Source source = Source::Profile;
		Reason reason = Reason::ProfileNoAuto;
		float sep = 0.0f;
		float conv = 0.0f;
		float near_floor = 0.0f;
		float strength = 1.0f; // clamped
		float s_eff = 1.0f; // separation as a fraction of orthostereo (auto) or min(strength, 1) (profile)
		float zero_m = 0.0f; // where zero disparity sits, metres
		float ipd_m = 0.0f;
		float width_m = 0.0f;
		float height_m = 0.0f;
		float distance_m = 0.0f;
		float ndc_ratio = 1.0f; // the NDC-to-display width ratio applied (auto only)
		bool ipd_is_test = false;
		bool curved = false;
		bool sep_railed = false;
		bool conv_railed = false;
		bool floor_railed = false;
	};

	Result Derive(const Inputs& in);

	// 3t^2 - 2t^3 with t clamped to [0, 1].
	float Smoothstep(float t);

	// from + (to - from) * Smoothstep(step / kEaseVsyncs).
	float EaseValue(float from, float to, u32 step);

	// True when the target moved enough to ease to it again: the source changed, the separation by over 1% or the
	// convergence by over 0.5. A near_floor-only change is not a retarget.
	bool NeedsRetarget(const Result& latched, const Result& now);

	// Once per process when PCSX2_VR_SELFTEST is set: checks Derive, the ease and the retarget rule.
	void MaybeRunSelfTest();
} // namespace VR::StereoAuto
