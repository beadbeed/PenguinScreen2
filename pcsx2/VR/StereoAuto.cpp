// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/StereoAuto.h"

#include "common/Console.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace VR::StereoAuto
{
	namespace
	{
		bool InRange(float v, float lo, float hi)
		{
			return std::isfinite(v) && v >= lo && v <= hi;
		}
	} // namespace

	const char* ReasonText(Reason reason)
	{
		switch (reason)
		{
			case Reason::None:             return "auto";
			case Reason::ProfileNoAuto:    return "profile has no stereo.firstPerson.auto";
			case Reason::SettingOff:       return "VR FirstPersonAutoStereo off";
			case Reason::NoEyes:           return "no headset eye data yet";
			case Reason::IpdOutOfRange:    return "IPD outside 45-80 mm";
			case Reason::NoScreen:         return "no first-person screen yet";
			case Reason::ScreenOutOfRange: return "first-person screen outside the rails";
			default:                       return "unknown";
		}
	}

	Result Derive(const Inputs& in)
	{
		Result r;
		r.strength = std::isfinite(in.strength) ? std::clamp(in.strength, kMinStrength, kMaxStrength) : 1.0f;
		const float s = r.strength;
		const float units =
			(std::isfinite(in.units_per_meter) && in.units_per_meter > 0.0f) ? in.units_per_meter : kDefaultUnitsPerMeter;
		r.ipd_m = in.ipd_m;
		r.ipd_is_test = in.ipd_is_test;
		r.width_m = in.screen_width_m;
		r.height_m = in.screen_height_m;
		r.distance_m = in.screen_distance_m;
		r.curved = in.screen_curved;

		Reason reason = Reason::None;
		if (!in.profile_auto)
			reason = Reason::ProfileNoAuto;
		else if (!in.setting_auto)
			reason = Reason::SettingOff;
		else if (!in.eyes_valid)
			reason = Reason::NoEyes;
		else if (!InRange(in.ipd_m, kMinIpdM, kMaxIpdM))
			reason = Reason::IpdOutOfRange;
		else if (!in.screen_valid)
			reason = Reason::NoScreen;
		else if (!InRange(in.screen_width_m, kMinScreenWidthM, kMaxScreenWidthM) ||
				 !InRange(in.screen_distance_m, kMinScreenDistanceM, kMaxScreenDistanceM))
			reason = Reason::ScreenOutOfRange;

		const float nearest = (std::isfinite(in.nearest_m) && in.nearest_m > 0.0f) ? in.nearest_m : 0.0f;
		float floor_v = 0.0f;
		if (reason == Reason::None)
		{
			r.source = Source::Auto;
			r.reason = Reason::None;
			r.ndc_ratio = InRange(in.ndc_width_ratio, kMinNdcWidthRatio, kMaxNdcWidthRatio) ? in.ndc_width_ratio : 1.0f;

			// Far content one IPD apart on the displayed picture; never more, whatever the strength.
			const float ortho = in.ipd_m / (in.screen_width_m * r.ndc_ratio);
			float sep = std::min(s, 1.0f) * ortho;
			const float cap = std::min(ortho, kMaxSeparation);
			if (sep > cap)
			{
				sep = cap;
				r.sep_railed = true;
			}
			r.sep = sep;
			r.s_eff = sep / ortho;

			float conv = std::max(s, 1.0f) * in.screen_distance_m * units;
			if (conv < kMinAutoConvergence || conv > kMaxAutoConvergence)
			{
				conv = std::clamp(conv, kMinAutoConvergence, kMaxAutoConvergence);
				r.conv_railed = true;
			}
			r.conv = conv;

			// Zero disparity is seen on the screen, whatever the convergence.
			r.zero_m = in.screen_distance_m;
			if (nearest > 0.0f)
				floor_v = (1.0f - r.zero_m / nearest) / r.s_eff;
		}
		else
		{
			// The authored numbers, scaled the same way; no rails on them.
			r.source = Source::Profile;
			r.reason = reason;
			r.s_eff = std::min(s, 1.0f);
			r.sep = in.profile_sep * r.s_eff;
			r.conv = in.profile_conv * std::max(s, 1.0f);
			r.zero_m = in.profile_conv / units;
			// At strength 1 this is wave 4's 1 - conv / (nearest * units), bit for bit.
			if (nearest > 0.0f)
				floor_v = (1.0f - in.profile_conv / (nearest * units)) / r.s_eff;
		}

		// 'nearest' is the closest perceived distance; the floor never goes below -2 (nor above 0).
		if (!std::isfinite(floor_v))
			floor_v = 0.0f;
		if (floor_v < kMinNearFloor)
		{
			floor_v = kMinNearFloor;
			r.floor_railed = true;
		}
		r.near_floor = std::min(floor_v, 0.0f);
		return r;
	}

	float Smoothstep(float t)
	{
		const float x = std::isfinite(t) ? std::clamp(t, 0.0f, 1.0f) : 1.0f;
		return x * x * (3.0f - 2.0f * x);
	}

	float EaseValue(float from, float to, u32 step)
	{
		return from + (to - from) * Smoothstep(static_cast<float>(step) / static_cast<float>(kEaseVsyncs));
	}

	bool NeedsRetarget(const Result& latched, const Result& now)
	{
		if (latched.source != now.source)
			return true;
		if (std::abs(now.sep - latched.sep) > 0.01f * std::abs(latched.sep))
			return true;
		return std::abs(now.conv - latched.conv) > 0.5f;
	}

	namespace
	{
		bool Near(float got, float want)
		{
			if (!std::isfinite(got))
				return false;
			const float tol = 1e-5f * std::max(std::abs(want), 1e-3f);
			return std::abs(got - want) <= tol;
		}

		Inputs CommonInputs()
		{
			Inputs in;
			in.profile_auto = true;
			in.setting_auto = true;
			in.eyes_valid = true;
			in.ipd_m = 0.063f;
			in.ipd_is_test = false;
			in.screen_valid = true;
			in.screen_distance_m = 1.5f;
			in.screen_width_m = 4.766667f;
			in.screen_height_m = 3.575f;
			in.screen_curved = false;
			in.units_per_meter = 100.0f;
			in.profile_sep = 0.0132f;
			in.profile_conv = 150.0f;
			in.nearest_m = 0.75f;
			in.strength = 1.0f;
			in.ndc_width_ratio = 1.0f;
			return in;
		}

		void RunSelfTest()
		{
			int fail = 0;
			const auto check = [&fail](bool ok, const char* name) {
				if (!ok)
				{
					++fail;
					Console.Error("(VR) Auto-stereo self-test FAIL: %s", name);
				}
			};

			{
				const Result r = Derive(CommonInputs());
				check(r.source == Source::Auto && r.reason == Reason::None, "default inputs give auto");
				check(Near(r.sep, 0.0132168f), "default sep 0.0132168");
				check(Near(r.conv, 150.0f), "default conv 150");
				check(Near(r.near_floor, -1.0f), "default floor -1");
				check(!r.sep_railed && !r.conv_railed && !r.floor_railed, "default inputs hit no rail");
			}
			{
				Inputs in = CommonInputs();
				in.strength = 0.5f;
				const Result r = Derive(in);
				check(Near(r.sep, 0.0066084f) && Near(r.conv, 150.0f) && Near(r.near_floor, -2.0f),
					"strength 0.5: sep 0.0066084, conv 150, floor -2");
			}
			{
				Inputs in = CommonInputs();
				in.strength = 1.5f;
				const Result r = Derive(in);
				check(Near(r.sep, 0.0132168f) && Near(r.conv, 225.0f) && Near(r.near_floor, -1.0f),
					"strength 1.5: sep 0.0132168, conv 225, floor -1");
			}
			{
				Inputs in = CommonInputs();
				in.strength = 3.0f;
				check(Near(Derive(in).strength, 1.5f), "strength 3.0 clamps to 1.5");
				in.strength = std::numeric_limits<float>::quiet_NaN();
				check(Near(Derive(in).strength, 1.0f), "strength NaN becomes 1.0");
			}
			{
				Inputs in = CommonInputs();
				in.screen_width_m = 6.355556f;
				check(Near(Derive(in).sep, 0.0099126f), "16:9 screen: sep 0.0099126");
			}
			{
				Inputs in = CommonInputs();
				in.ipd_m = 0.058f;
				check(Near(Derive(in).sep, 0.0121678f), "IPD 58 mm: sep 0.0121678");
				in.ipd_m = 0.070f;
				check(Near(Derive(in).sep, 0.0146853f), "IPD 70 mm: sep 0.0146853");
			}
			{
				Inputs in = CommonInputs();
				in.ipd_m = 0.0f;
				const Result r = Derive(in);
				// Wave 4's floor, computed the way wave 4 did.
				const float wave4_floor = std::clamp(1.0f - 150.0f / (0.75f * 100.0f), -2.0f, 0.0f);
				check(r.source == Source::Profile && r.reason == Reason::IpdOutOfRange, "IPD 0 falls back (IPD out of range)");
				check(r.sep == 0.0132f && r.conv == 150.0f && r.near_floor == wave4_floor && Near(r.near_floor, -1.0f),
					"IPD 0: sep 0.0132, conv 150, floor -1, equal to wave 4");
			}
			{
				Inputs in = CommonInputs();
				in.eyes_valid = false;
				const Result r = Derive(in);
				check(r.source == Source::Profile && r.reason == Reason::NoEyes, "eyes invalid falls back (no eyes)");
			}
			{
				Inputs in = CommonInputs();
				in.setting_auto = false;
				const Result r = Derive(in);
				check(r.source == Source::Profile && r.reason == Reason::SettingOff, "setting off falls back");
			}
			{
				Inputs in = CommonInputs();
				in.profile_auto = false;
				const Result r = Derive(in);
				check(r.source == Source::Profile && r.reason == Reason::ProfileNoAuto, "profile without auto falls back");
			}
			{
				Inputs in = CommonInputs();
				in.screen_valid = false;
				const Result r = Derive(in);
				check(r.source == Source::Profile && r.reason == Reason::NoScreen, "screen invalid falls back (no screen)");
			}
			{
				Inputs in = CommonInputs();
				in.screen_width_m = 0.5f;
				const Result r = Derive(in);
				check(r.source == Source::Profile && r.reason == Reason::ScreenOutOfRange,
					"0.5 m screen falls back (screen out of range)");
			}
			{
				Inputs in = CommonInputs();
				in.screen_width_m = 1.5f;
				const Result r = Derive(in);
				check(r.source == Source::Auto && Near(r.sep, 0.03f) && r.sep_railed, "1.5 m screen: sep capped at 0.03");
				check(Near(r.s_eff, 0.714286f) && Near(r.near_floor, -1.4f), "1.5 m screen: s_eff 0.714286, floor -1.4");
			}
			{
				Inputs in = CommonInputs();
				in.nearest_m = 0.0f;
				check(Derive(in).near_floor == 0.0f, "nearest 0: floor 0");
			}
			{
				Inputs in = CommonInputs();
				in.nearest_m = 0.6f;
				in.strength = 0.5f;
				const Result r = Derive(in);
				check(r.near_floor == -2.0f && r.floor_railed, "nearest 0.6 at strength 0.5: floor capped at -2");
			}
			{
				Inputs in = CommonInputs();
				in.units_per_meter = 0.0f;
				const Result r = Derive(in);
				check(Near(r.conv, 150.0f) && Near(r.near_floor, -1.0f), "unitsPerMeter 0 is treated as 100");
			}
			{
				Inputs in = CommonInputs();
				in.ndc_width_ratio = 1.25f;
				check(Near(Derive(in).sep, 0.0132168f / 1.25f), "ndcWidthRatio 1.25 divides the auto separation");
				in.ndc_width_ratio = 1.0f;
				check(Near(Derive(in).sep, 0.0132168f), "ndcWidthRatio 1.0 leaves the auto separation alone");
			}
			{
				check(EaseValue(0.0f, 1.0f, 0) == 0.0f, "ease step 0 is 0");
				check(Near(EaseValue(0.0f, 1.0f, 15), 0.5f), "ease step 15 is 0.5");
				check(EaseValue(0.0f, 1.0f, 30) == 1.0f, "ease step 30 is 1");
				check(EaseValue(0.0f, 1.0f, 45) == 1.0f, "ease step 45 is 1");
				bool monotonic = true;
				for (u32 step = 1; step <= kEaseVsyncs; step++)
					monotonic = monotonic && EaseValue(0.0f, 1.0f, step) >= EaseValue(0.0f, 1.0f, step - 1);
				check(monotonic, "ease is monotonic over steps 0-30");
			}
			{
				const Result a = Derive(CommonInputs());
				Result b = a;
				b.sep = a.sep * 1.005f;
				check(!NeedsRetarget(a, b), "sep +0.5% is not a retarget");
				b.sep = a.sep * 1.02f;
				check(NeedsRetarget(a, b), "sep +2% is a retarget");
				b = a;
				b.source = Source::Profile;
				check(NeedsRetarget(a, b), "a source change is a retarget");
				b = a;
				b.near_floor = -2.0f;
				check(!NeedsRetarget(a, b), "a near_floor change alone is not a retarget");
			}

			Console.WriteLn(fail == 0 ? Color_StrongGreen : Color_StrongRed, "(VR) Auto-stereo self-test: %d failure(s).", fail);
		}
	} // namespace

	void MaybeRunSelfTest()
	{
		static bool s_done = false;
		if (s_done)
			return;
		s_done = true;
		if (!std::getenv("PCSX2_VR_SELFTEST"))
			return;
		RunSelfTest();
	}
} // namespace VR::StereoAuto
