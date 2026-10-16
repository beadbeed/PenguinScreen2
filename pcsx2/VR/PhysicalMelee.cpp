// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/PhysicalMelee.h"
#include "VR/CameraDriver.h"
#include "VR/GameFeedback.h"
#include "VR/HandModel.h"
#include "VR/ItemData.h"
#include "VR/SpatialControls.h"
#include "VR/SplitState.h"
#include "VR/VRInput.h"
#include "VR/VRProfileDB.h"

#include "Config.h"
#include "Memory.h"
#include "MemoryTypes.h"
#include "VMManager.h"

#include "common/Console.h"

#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace VR::PhysicalMelee
{
	namespace
	{
		float FiniteOr(float v, float fallback)
		{
			return std::isfinite(v) ? v : fallback;
		}

		bool Finite3(const float v[3])
		{
			return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
		}

		float Len3(float x, float y, float z)
		{
			return std::sqrt(x * x + y * y + z * z);
		}

		DetectorParams SanitizedDetector(const DetectorParams& in)
		{
			const DetectorParams d{};
			DetectorParams p;
			p.tip_m = std::clamp(FiniteOr(in.tip_m, d.tip_m), 0.0f, 1.5f);
			p.speed_on = std::clamp(FiniteOr(in.speed_on, d.speed_on), 0.5f, 10.0f);
			p.speed_off = std::clamp(FiniteOr(in.speed_off, d.speed_off), 0.1f, p.speed_on * 0.95f);
			p.end_s = std::clamp(FiniteOr(in.end_s, d.end_s), 0.01f, 0.5f);
			p.travel_m = std::clamp(FiniteOr(in.travel_m, d.travel_m), 0.02f, 2.0f);
			p.straightness = std::clamp(FiniteOr(in.straightness, d.straightness), 0.0f, 1.0f);
			p.stroke_max_s = std::clamp(FiniteOr(in.stroke_max_s, d.stroke_max_s), 0.1f, 3.0f);
			p.smooth_s = std::clamp(FiniteOr(in.smooth_s, d.smooth_s), 0.0f, 0.2f);
			p.jump_m = std::clamp(FiniteOr(in.jump_m, d.jump_m), 0.1f, 5.0f);
			return p;
		}

		DriverParams SanitizedDriver(const DriverParams& in)
		{
			const DriverParams d{};
			DriverParams p;
			p.press_s = std::clamp(FiniteOr(in.press_s, d.press_s), 0.02f, 0.5f);
			p.press_max_s = std::clamp(FiniteOr(in.press_max_s, d.press_max_s), p.press_s, 1.0f);
			p.raise_max_s = std::clamp(FiniteOr(in.raise_max_s, d.raise_max_s), 0.1f, 3.0f);
			p.swing_max_s = std::clamp(FiniteOr(in.swing_max_s, d.swing_max_s), 0.2f, 6.0f);
			p.contact_s = std::clamp(FiniteOr(in.contact_s, d.contact_s), 0.0f, 1.0f);
			p.cooldown_s = std::clamp(FiniteOr(in.cooldown_s, d.cooldown_s), 0.0f, 2.0f);
			return p;
		}
	}

	// -----------------------------------------------------------------------------------------------------------------
	// The detector.
	// -----------------------------------------------------------------------------------------------------------------

	DetectorOut Detect(const DetectorIn& in, const DetectorParams& params, DetectorState& st)
	{
		DetectorOut out;
		const DetectorParams p = SanitizedDetector(params);
		const float dt = std::clamp(FiniteOr(in.dt, 0.0f), 0.0f, 0.25f);
		// Tracking lost: whatever was going on is dropped (a swing is never counted from a pose it did not see).
		if (!in.valid || !Finite3(in.tip))
		{
			st.Reset();
			return out;
		}
		if (!st.have_prev)
		{
			st.Reset();
			st.have_prev = true;
			std::memcpy(st.prev, in.tip, sizeof(st.prev));
			return out;
		}
		const float dx = in.tip[0] - st.prev[0], dy = in.tip[1] - st.prev[1], dz = in.tip[2] - st.prev[2];
		const float step = Len3(dx, dy, dz);
		if (step > p.jump_m)
		{
			// A glitch (a pose snapping back after a loss, a recenter): no velocity from it, no stroke through it.
			st.Reset();
			st.have_prev = true;
			std::memcpy(st.prev, in.tip, sizeof(st.prev));
			return out;
		}
		const float start[3] = {st.prev[0], st.prev[1], st.prev[2]};
		std::memcpy(st.prev, in.tip, sizeof(st.prev));
		if (dt <= 0.0f)
			return out;
		const float a = (p.smooth_s > 0.0f) ? (1.0f - std::exp(-dt / p.smooth_s)) : 1.0f;
		st.vel[0] += (dx / dt - st.vel[0]) * a;
		st.vel[1] += (dy / dt - st.vel[1]) * a;
		st.vel[2] += (dz / dt - st.vel[2]) * a;
		const float speed = Len3(st.vel[0], st.vel[1], st.vel[2]);
		out.speed = speed;
		if (!st.in_stroke)
		{
			if (speed >= p.speed_on)
			{
				st.in_stroke = true;
				// A stroke that begins while a swing may not count (a screen, the weapon not yet in hand) never counts:
				// arming half-way through a movement is not a swing.
				st.counted = !in.armed;
				st.stroke_s = dt;
				st.slow_s = 0.0f;
				st.path_m = step;
				std::memcpy(st.start, start, sizeof(st.start));
			}
		}
		else
		{
			st.stroke_s += dt;
			st.path_m += step;
			st.slow_s = (speed < p.speed_off) ? (st.slow_s + dt) : 0.0f;
			if (st.slow_s >= p.end_s)
				st.in_stroke = false;
		}
		if (st.in_stroke)
		{
			if (!in.armed || st.stroke_s > p.stroke_max_s)
				st.counted = true; // a screen opening mid-stroke, or a slow sweep: this stroke is spent
			if (!st.counted && st.path_m >= p.travel_m)
			{
				const float disp = Len3(in.tip[0] - st.start[0], in.tip[1] - st.start[1], in.tip[2] - st.start[2]);
				if (disp >= p.straightness * st.path_m)
				{
					st.counted = true;
					out.swing = true;
				}
			}
		}
		out.in_stroke = st.in_stroke;
		out.path_m = st.in_stroke ? st.path_m : 0.0f;
		return out;
	}

	// -----------------------------------------------------------------------------------------------------------------
	// The driver.
	// -----------------------------------------------------------------------------------------------------------------

	const char* PhaseName(Phase phase)
	{
		switch (phase)
		{
			case Phase::Idle: return "Idle";
			case Phase::Raising: return "Raising";
			case Phase::Pressing: return "Pressing";
			case Phase::Swinging: return "Swinging";
			case Phase::Cooldown: return "Cooldown";
		}
		return "?";
	}

	const char* ResultName(Result result)
	{
		switch (result)
		{
			case Result::None: return "none";
			case Result::Swung: return "swung";
			case Result::NotRaised: return "the weapon did not come up";
			case Result::NoSwing: return "the game did not swing";
			case Result::Screen: return "a game screen opened";
			case Result::Lost: return "first person, the weapon or the stance went away";
		}
		return "?";
	}

	DriverOut Step(const DriverIn& in, const DriverParams& params, DriverState& st)
	{
		DriverOut out;
		const DriverParams p = SanitizedDriver(params);
		const float dt = std::clamp(FiniteOr(in.dt, 0.0f), 0.0f, 0.25f);

		const auto finish = [&](Result r) {
			out.finished = true;
			out.result = r;
			st.phase = Phase::Cooldown;
			st.phase_s = 0.0f;
			st.we_raise = false;
		};

		if (st.phase != Phase::Idle)
		{
			st.phase_s += dt;
			st.since_request_s += dt;
		}
		switch (st.phase)
		{
			case Phase::Idle:
			{
				if (!in.request || !in.usable || in.screen)
					break;
				st.Reset();
				out.started = true;
				if (in.raised && in.aim_idle)
				{
					st.phase = Phase::Pressing;
				}
				else
				{
					// The weapon is down (we hold R1 until the swing is over), or still coming up (wait for aim idle).
					st.phase = Phase::Raising;
					st.we_raise = !in.raised;
				}
				break;
			}
			case Phase::Raising:
				if (!in.usable)
					finish(Result::Lost);
				else if (in.screen)
					finish(Result::Screen);
				else if (in.raised && in.aim_idle)
				{
					st.phase = Phase::Pressing;
					st.phase_s = 0.0f;
				}
				else if (st.phase_s >= p.raise_max_s)
					finish(Result::NotRaised);
				break;
			case Phase::Pressing:
				// Lowered inside the press (the player let go of the bumper): Circle in the normal stance is search, pick
				// up, open; the press ends at once (review m1).
				if (!in.usable || !in.raised)
					finish(Result::Lost);
				else if (in.screen)
					finish(Result::Screen);
				else if (in.swinging)
				{
					st.phase = Phase::Swinging;
					st.phase_s = 0.0f;
					st.saw_swing = true;
					out.live = true;
					out.latency_s = st.since_request_s;
				}
				else if (st.phase_s >= p.press_max_s)
					finish(Result::NoSwing);
				break;
			case Phase::Swinging:
				if (in.hp_drop && !st.contact)
				{
					st.contact = true;
					out.contact = true;
				}
				if (in.screen)
					finish(Result::Screen);
				else if (!in.usable)
					finish(Result::Lost);
				else if (!in.swinging || st.phase_s >= p.swing_max_s)
					finish(Result::Swung);
				break;
			case Phase::Cooldown:
				// Contact a moment after the swing ended still belongs to it (the game stages a hit and applies it a frame
				// or two later). Not for an attempt that never swung.
				if (in.hp_drop && !st.contact && st.saw_swing && st.phase_s <= p.contact_s)
				{
					st.contact = true;
					out.contact = true;
				}
				if (st.phase_s >= std::max(p.cooldown_s, p.contact_s))
				{
					st.phase = Phase::Idle;
					st.phase_s = 0.0f;
				}
				break;
		}
		out.phase = st.phase;
		out.hold_attack = st.phase == Phase::Pressing && st.phase_s < p.press_s && in.raised;
		out.hold_raise = st.we_raise && (st.phase == Phase::Raising || st.phase == Phase::Pressing || st.phase == Phase::Swinging);
		return out;
	}

	// -----------------------------------------------------------------------------------------------------------------
	// The weapon's pose in the game.
	// -----------------------------------------------------------------------------------------------------------------

	bool GameMatrix(const float origin_vr[3], const float axes_vr[3][3], PointMap map, const void* ctx, float out[16])
	{
		if (!map || !Finite3(origin_vr))
			return false;
		float og[3];
		if (!map(ctx, origin_vr, og) || !Finite3(og))
			return false;
		float m[16] = {};
		for (int k = 0; k < 3; k++)
		{
			if (!Finite3(axes_vr[k]))
				return false;
			const float tip[3] = {origin_vr[0] + axes_vr[k][0] * 0.01f, origin_vr[1] + axes_vr[k][1] * 0.01f,
				origin_vr[2] + axes_vr[k][2] * 0.01f};
			float g[3];
			if (!map(ctx, tip, g) || !Finite3(g))
				return false;
			const float r[3] = {g[0] - og[0], g[1] - og[1], g[2] - og[2]};
			const float len = Len3(r[0], r[1], r[2]);
			if (!(len > 1.0e-4f) || !std::isfinite(len))
				return false;
			for (int c = 0; c < 3; c++)
				m[k * 4 + c] = r[c] / len;
		}
		m[12] = og[0];
		m[13] = og[1];
		m[14] = og[2];
		m[15] = 1.0f;
		std::memcpy(out, m, sizeof(m));
		return true;
	}

	// -----------------------------------------------------------------------------------------------------------------
	// Contact mode: strength, geometry, the strike logic.
	// -----------------------------------------------------------------------------------------------------------------

	ContactParams SanitizedContact(const ContactParams& in)
	{
		const ContactParams d{};
		ContactParams p;
		p.speed = std::clamp(FiniteOr(in.speed, d.speed), 0.3f, 10.0f);
		p.rearm = std::clamp(FiniteOr(in.rearm, d.rearm), 0.05f, p.speed * 0.95f);
		p.rearm_s = std::clamp(FiniteOr(in.rearm_s, d.rearm_s), 0.0f, 1.0f);
		p.leave_s = std::clamp(FiniteOr(in.leave_s, d.leave_s), 0.0f, 2.0f);
		p.firm = std::clamp(FiniteOr(in.firm, d.firm), p.speed, 15.0f);
		p.heavy = std::clamp(FiniteOr(in.heavy, d.heavy), p.firm, 20.0f);
		p.light = std::clamp(FiniteOr(in.light, d.light), 0.1f, 1.0f);
		p.strong = std::clamp(FiniteOr(in.strong, d.strong), 1.0f, 3.0f);
		p.lead_cm = std::clamp(FiniteOr(in.lead_cm, d.lead_cm), 0.0f, 50.0f);
		p.cooldown_s = std::clamp(FiniteOr(in.cooldown_s, d.cooldown_s), 0.0f, 2.0f);
		p.window_frames = std::clamp(in.window_frames, 1, 30);
		p.hold_s = std::clamp(FiniteOr(in.hold_s, d.hold_s), 0.0f, 0.1f);
		return p;
	}

	float Strength(float speed, const ContactParams& params)
	{
		const ContactParams p = SanitizedContact(params);
		if (!std::isfinite(speed) || speed < p.speed)
			return 0.0f;
		if (speed >= p.heavy)
			return p.strong;
		if (speed >= p.firm)
			return 1.0f;
		const float u = (p.firm > p.speed) ? (speed - p.speed) / (p.firm - p.speed) : 1.0f;
		return p.light + (1.0f - p.light) * std::clamp(u, 0.0f, 1.0f);
	}

	void PlaceShape(const HitShape& s, const float m[16], float a[3], float b[3])
	{
		const auto place = [&](const float l[3], float out[3]) {
			for (int c = 0; c < 3; c++)
				out[c] = m[12 + c] + l[0] * m[c] + l[1] * m[4 + c] + l[2] * m[8 + c];
		};
		float p0[3] = {s.p[0], s.p[1], s.p[2]};
		float p1[3] = {p0[0], p0[1], p0[2]};
		if (s.capsule)
		{
			for (int k = 0; k < 3; k++)
				p1[k] = s.p[k] + s.d[k] * s.length;
			// The far end (from the grip) is b.
			if (Len3(p0[0], p0[1], p0[2]) > Len3(p1[0], p1[1], p1[2]))
				std::swap(p0, p1);
		}
		place(p0, a);
		place(p1, b);
	}

	float SegmentPointDistSq(const float a[3], const float b[3], const float c[3])
	{
		const float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
		const float ac[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
		const float len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
		float t = 0.0f;
		if (len2 > 1e-9f)
			t = std::clamp((ac[0] * ab[0] + ac[1] * ab[1] + ac[2] * ab[2]) / len2, 0.0f, 1.0f);
		const float d[3] = {ac[0] - ab[0] * t, ac[1] - ab[1] * t, ac[2] - ab[2] * t};
		return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
	}

	bool Touches(const float a[3], const float b[3], float r, bool have_prev, const float pa[3], const float pb[3],
		const EnemyVolume& e, float lead, int* sphere)
	{
		if (!Finite3(a) || !Finite3(b) || !std::isfinite(r))
			return false;
		// The shape now, halfway from the last poll, and at the last poll.
		float segs[3][2][3];
		int n = 1;
		std::memcpy(segs[0][0], a, sizeof(float) * 3);
		std::memcpy(segs[0][1], b, sizeof(float) * 3);
		if (have_prev && Finite3(pa) && Finite3(pb))
		{
			for (int k = 0; k < 3; k++)
			{
				segs[1][0][k] = 0.5f * (a[k] + pa[k]);
				segs[1][1][k] = 0.5f * (b[k] + pb[k]);
				segs[2][0][k] = pa[k];
				segs[2][1][k] = pb[k];
			}
			n = 3;
		}
		const int count = std::clamp(e.count, 0, 8);
		for (int s = 0; s < n; s++)
		{
			for (int k = 0; k < count; k++)
			{
				if (!Finite3(e.c[k]) || !std::isfinite(e.r[k]) || e.r[k] <= 0.0f)
					continue;
				const float reach = r + e.r[k] + lead;
				if (SegmentPointDistSq(segs[s][0], segs[s][1], e.c[k]) <= reach * reach)
				{
					if (sphere)
						*sphere = k;
					return true;
				}
			}
		}
		return false;
	}

	StrikeOut StrikeStep(const StrikeIn& in, const ContactParams& params, StrikeState& st)
	{
		StrikeOut out;
		const ContactParams p = SanitizedContact(params);
		const float dt = std::clamp(FiniteOr(in.dt, 0.0f), 0.0f, 0.25f);
		const float speed = FiniteOr(in.speed, 0.0f);
		st.since_strike_s = std::min(st.since_strike_s + dt, 10.0f);
		st.slow_s = (speed < p.rearm) ? std::min(st.slow_s + dt, 10.0f) : 0.0f;
		const bool slow = st.slow_s >= p.rearm_s;
		const auto touching = [&](int id) {
			for (int k = 0; k < in.touching_count && in.touching; k++)
				if (in.touching[k] == id)
					return true;
			return false;
		};
		for (int id = 0; id < static_cast<int>(st.enemies.size()); id++)
		{
			StrikeState::Enemy& e = st.enemies[id];
			e.off_s = touching(id) ? 0.0f : std::min(e.off_s + dt, 10.0f);
			// Re-arm: the weapon slowed (the swing is over) or left this enemy.
			if (e.struck && (slow || e.off_s >= p.leave_s))
				e.struck = false;
		}
		const float last = st.last_speed;
		st.last_speed = speed;
		if (!in.usable || in.live || st.since_strike_s < p.cooldown_s)
		{
			st.pending = -1;
			return out;
		}
		const auto strike = [&](int id, float at_speed) {
			st.enemies[id].struck = true;
			st.since_strike_s = 0.0f;
			st.pending = -1;
			out.enemy = id;
			out.strength = std::max(Strength(at_speed, p), Strength(p.speed, p));
		};
		if (st.pending >= 0)
		{
			// Held: strike once the tip stops speeding up, the hold is over, or the weapon is leaving the enemy.
			const int id = st.pending;
			st.pending_s += dt;
			st.pending_peak = std::max(st.pending_peak, speed);
			if (!touching(id) || speed <= last || st.pending_s >= p.hold_s)
				strike(id, st.pending_peak);
			return out;
		}
		if (Strength(speed, p) <= 0.0f)
			return out;
		for (int k = 0; k < in.touching_count && in.touching; k++)
		{
			const int id = in.touching[k];
			if (id < 0 || id >= static_cast<int>(st.enemies.size()) || st.enemies[id].struck)
				continue;
			if (p.hold_s > 0.0f && speed > last && speed < p.heavy)
			{
				st.pending = id;
				st.pending_s = 0.0f;
				st.pending_peak = speed;
			}
			else
				strike(id, speed);
			break;
		}
		return out;
	}

	// -----------------------------------------------------------------------------------------------------------------
	// Self-test.
	// -----------------------------------------------------------------------------------------------------------------
	namespace
	{
		struct Checker
		{
			std::vector<std::string> failures;
			void operator()(bool ok, const std::string& name)
			{
				if (!ok)
					failures.push_back(name);
			}
		};

		constexpr float kPi = 3.14159265358979f;
		constexpr float kDt = 1.0f / 72.0f; // a headset's input poll

		// Runs a scripted path for the tip: f(t) gives the point at t seconds, for secs seconds; counts the swings.
		template <typename F>
		int RunPath(F f, float secs, const DetectorParams& p, DetectorState& st, bool armed = true, float dt = kDt,
			float* first_swing_t = nullptr)
		{
			int swings = 0;
			const int n = static_cast<int>(secs / dt);
			for (int i = 0; i <= n; i++)
			{
				const float t = static_cast<float>(i) * dt;
				DetectorIn in;
				in.dt = dt;
				in.valid = true;
				in.armed = armed;
				f(t, in.tip);
				const DetectorOut o = Detect(in, p, st);
				if (o.swing)
				{
					if (swings == 0 && first_swing_t)
						*first_swing_t = t;
					swings++;
				}
			}
			return swings;
		}

		// An arc of the tip about a pivot (the shoulder), radius r, from angle a0 to a1 (radians) over dur seconds with an
		// ease in and out, starting after rest_s at rest and resting after; plane: xy (overhead) or xz (side).
		struct Arc
		{
			float r = 0.7f, a0 = 1.4f, a1 = -0.6f, dur = 0.25f, rest_s = 0.2f;
			bool side = false;
			void operator()(float t, float out[3]) const
			{
				float u = std::clamp((t - rest_s) / dur, 0.0f, 1.0f);
				u = u * u * (3.0f - 2.0f * u);
				const float a = a0 + (a1 - a0) * u;
				out[0] = side ? r * std::sin(a) : 0.2f;
				out[1] = side ? 1.2f : 1.4f + r * std::sin(a);
				out[2] = -r * std::cos(a);
			}
		};

		void TestDetector(Checker& c)
		{
			const DetectorParams p;
			{
				// An overhead swing: about 1.4 m of arc in 0.25 s (peak near 8 m/s at the tip) is one swing, counted early.
				DetectorState st;
				float when = -1.0f;
				const int n = RunPath(Arc{}, 1.0f, p, st, true, kDt, &when);
				c(n == 1, "detector: an overhead swing is one swing");
				c(when > 0.2f && when < 0.36f, "detector: counted within the stroke, before it is over");
			}
			{
				DetectorState st;
				Arc side;
				side.side = true;
				side.a0 = -0.9f;
				side.a1 = 0.9f;
				c(RunPath(side, 1.0f, p, st) == 1, "detector: a side swing is one swing");
			}
			{
				// Two swings 0.6 s apart: two; a swing back the other way right after: its own stroke, counted.
				DetectorState st;
				const auto two = [](float t, float out[3]) {
					Arc a;
					Arc b;
					b.a0 = -0.6f;
					b.a1 = 1.4f;
					b.rest_s = 0.8f;
					(t < 0.65f ? a : b)(t, out);
				};
				c(RunPath(two, 1.6f, p, st) == 2, "detector: a swing and a swing back are two");
			}
			{
				// A stab: 35 cm straight ahead in 0.12 s.
				DetectorState st;
				const auto stab = [](float t, float out[3]) {
					float u = std::clamp((t - 0.2f) / 0.12f, 0.0f, 1.0f);
					u = u * u * (3.0f - 2.0f * u);
					out[0] = 0.2f;
					out[1] = 1.2f;
					out[2] = -0.3f - 0.35f * u;
				};
				c(RunPath(stab, 0.8f, p, st) == 1, "detector: a quick stab is a swing");
			}
			{
				// Slow: the same arc over 1.5 s (under 1.5 m/s) never counts; nor does walking the hand about.
				DetectorState st;
				Arc slow;
				slow.dur = 1.5f;
				c(RunPath(slow, 2.5f, p, st) == 0, "detector: a slow sweep is no swing");
				DetectorState st2;
				const auto drift = [](float t, float out[3]) {
					out[0] = 0.2f + 0.3f * std::sin(t * 1.5f);
					out[1] = 1.2f + 0.1f * std::sin(t * 2.0f);
					out[2] = -0.4f;
				};
				c(RunPath(drift, 4.0f, p, st2) == 0, "detector: a hand moving about is no swing");
			}
			{
				// A small fast shake: 12 cm side to side at 6 Hz, going nowhere.
				DetectorState st;
				const auto shake = [](float t, float out[3]) {
					out[0] = 0.2f + 0.06f * std::sin(2.0f * kPi * 6.0f * t);
					out[1] = 1.2f;
					out[2] = -0.4f;
				};
				c(RunPath(shake, 2.0f, p, st) == 0, "detector: a shake is no swing");
			}
			{
				// Not armed (a screen up): nothing; armed half-way through a stroke: that stroke still does not count, the
				// next one does.
				DetectorState st;
				c(RunPath(Arc{}, 1.0f, p, st, false) == 0, "detector: nothing while not armed");
				DetectorState st2;
				int swings = 0;
				for (int i = 0; i <= static_cast<int>(1.8f / kDt); i++)
				{
					const float t = static_cast<float>(i) * kDt;
					DetectorIn in;
					in.dt = kDt;
					in.valid = true;
					in.armed = t > 0.30f; // the first swing is at full speed by then
					Arc a;
					Arc b;
					b.a0 = -0.6f;
					b.a1 = 1.4f;
					b.rest_s = 1.0f;
					(t < 0.8f ? a : b)(t, in.tip);
					swings += Detect(in, p, st2).swing ? 1 : 0;
				}
				c(swings == 1, "detector: a stroke begun before arming never counts half-way; the next does");
			}
			{
				// Tracking lost mid-swing: the stroke is dropped; a jump (a glitch) is never a swing.
				DetectorState st;
				int swings = 0;
				for (int i = 0; i <= static_cast<int>(1.0f / kDt); i++)
				{
					const float t = static_cast<float>(i) * kDt;
					DetectorIn in;
					in.dt = kDt;
					in.valid = !(t > 0.26f && t < 0.33f);
					in.armed = true;
					Arc{}(t, in.tip);
					swings += Detect(in, p, st).swing ? 1 : 0;
				}
				c(swings <= 1, "detector: tracking loss mid-swing never makes two");
				DetectorState st2;
				const auto jump = [](float t, float out[3]) {
					out[0] = (t > 0.5f) ? 1.5f : 0.2f;
					out[1] = 1.2f;
					out[2] = -0.4f;
				};
				c(RunPath(jump, 1.0f, p, st2) == 0, "detector: a pose that jumps a metre is no swing");
				// NaN poses and a zero dt are survived.
				DetectorState st3;
				DetectorIn bad;
				bad.valid = true;
				bad.armed = true;
				bad.dt = std::numeric_limits<float>::quiet_NaN();
				bad.tip[0] = std::numeric_limits<float>::quiet_NaN();
				const DetectorOut o = Detect(bad, p, st3);
				c(!o.swing && !st3.have_prev, "detector: a NaN pose is no pose");
			}
			{
				// Junk params are clamped: still one swing for the overhead swing.
				DetectorParams junk;
				junk.speed_on = std::numeric_limits<float>::quiet_NaN();
				junk.speed_off = 100.0f;
				junk.travel_m = -1.0f;
				junk.straightness = std::numeric_limits<float>::quiet_NaN();
				junk.smooth_s = std::numeric_limits<float>::infinity();
				DetectorState st;
				c(RunPath(Arc{}, 1.0f, junk, st) == 1, "detector: junk params still detect one swing");
			}
		}

		// The game's side of a swing, polled at 60 Hz: the weapon comes up 0.57 s after R1 (stance raised at once, aim idle
		// later), Circle in aim idle starts the swing two frames later, which runs swing_s; contact at hit_s into it.
		struct FakeGame
		{
			bool r1_player = false; // the player holds R1 (the bumper)
			bool raised = false;
			float raise_s = 0.0f;
			bool aim_idle = false;
			bool swinging = false;
			float swing_t = 0.0f;
			float swing_s = 1.43f;
			float hit_s = 0.35f;    // < 0: never hits
			bool hit = false;
			bool refuse = false;    // the game never swings (a broken weapon)
			int circle_frames = 0;
			bool prev_circle = false;

			void Tick(bool circle, bool r1, float dt)
			{
				const bool up = r1 || r1_player;
				if (up)
				{
					if (!raised)
					{
						raised = true;
						raise_s = 0.0f;
					}
					raise_s += dt;
					if (raise_s >= 0.57f && !swinging)
						aim_idle = true;
				}
				else if (!swinging)
				{
					raised = false;
					aim_idle = false;
				}
				hit = false;
				if (swinging)
				{
					const float before = swing_t;
					swing_t += dt;
					if (hit_s >= 0.0f && before < hit_s && swing_t >= hit_s)
						hit = true;
					if (swing_t >= swing_s)
					{
						swinging = false;
						aim_idle = raised;
					}
				}
				else if (circle && !prev_circle && aim_idle && !refuse)
				{
					circle_frames = 2;
				}
				if (circle_frames > 0 && --circle_frames == 0)
				{
					swinging = true;
					swing_t = 0.0f;
					aim_idle = false;
				}
				prev_circle = circle;
			}
		};

		struct DriveRun
		{
			FakeGame g;
			DriverParams p;
			DriverState st;
			DriverOut out;
			bool screen = false;
			bool usable = true;
			int starts = 0, lives = 0, contacts = 0, finishes = 0;
			Result result = Result::None;
			float latency = -1.0f;
			bool circle_while_screen = false;
			bool any_circle = false;
			bool r1_after_end = false;

			void Poll(bool request = false, float dt = 1.0f / 60.0f)
			{
				DriverIn in;
				in.dt = dt;
				in.request = request;
				in.usable = usable;
				in.screen = screen;
				in.raised = g.raised;
				in.aim_idle = g.aim_idle;
				in.swinging = g.swinging;
				in.hp_drop = g.hit;
				out = Step(in, p, st);
				starts += out.started;
				lives += out.live;
				contacts += out.contact;
				if (out.live)
					latency = out.latency_s;
				if (out.finished)
				{
					finishes++;
					result = out.result;
				}
				if (out.hold_attack && screen)
					circle_while_screen = true;
				any_circle = any_circle || out.hold_attack;
				if (st.phase == Phase::Cooldown && out.hold_raise)
					r1_after_end = true;
				g.Tick(out.hold_attack, out.hold_raise, dt);
			}
			void Run(float secs)
			{
				for (int i = 0; i < static_cast<int>(secs * 60.0f); i++)
					Poll();
			}
		};

		void TestDriver(Checker& c)
		{
			{
				// Raised (the player holds the bumper): Circle at once, the swing two frames later, contact, done.
				DriveRun r;
				r.g.r1_player = true;
				r.Run(1.0f);
				r.Poll(true);
				r.Run(2.0f);
				c(r.starts == 1 && r.lives == 1 && r.contacts == 1 && r.finishes == 1 && r.result == Result::Swung,
					"driver: a raised swing presses, swings and lands once");
				c(r.latency >= 0.0f && r.latency < 0.08f, "driver: the game's swing starts within 80 ms when raised");
				c(r.st.phase == Phase::Idle && !r.out.hold_attack && !r.out.hold_raise, "driver: idle after");
			}
			{
				// Lowered: R1 first, Circle once aimed, R1 let go after the swing (the weapon goes down again).
				DriveRun r;
				r.Run(0.5f);
				r.Poll(true);
				c(r.out.hold_raise && !r.out.hold_attack, "driver: lowered, R1 first");
				r.Run(3.0f);
				c(r.lives == 1 && r.result == Result::Swung, "driver: lowered, the swing still runs");
				c(r.latency > 0.55f && r.latency < 0.75f, "driver: lowered, the swing waits for the weapon to come up");
				c(!r.r1_after_end && !r.g.raised, "driver: R1 let go after a swing it raised for");
			}
			{
				// A weapon the game will not swing: Circle given up after press_max_s, nothing else.
				DriveRun r;
				r.g.r1_player = true;
				r.g.refuse = true;
				r.Run(1.0f);
				r.Poll(true);
				r.Run(1.0f);
				c(r.lives == 0 && r.result == Result::NoSwing && r.finishes == 1, "driver: no swing, given up");
			}
			{
				// A screen opening while Circle would be pressed: nothing is pressed in it.
				DriveRun r;
				r.g.r1_player = true;
				r.Run(1.0f);
				r.screen = true;
				r.Poll(true);
				r.Run(0.5f);
				c(r.starts == 0 && !r.any_circle, "driver: no request taken with a screen up");
				DriveRun r2;
				r2.Run(0.3f);
				r2.Poll(true); // lowered: raising
				r2.Run(0.2f);
				r2.screen = true;
				r2.Run(1.0f);
				c(r2.result == Result::Screen && !r2.circle_while_screen && !r2.out.hold_raise,
					"driver: a screen mid-raise ends it, Circle never pressed, R1 let go");
			}
			{
				// Raised by the player, lowered inside the press (review m1): the press ends, Circle never held lowered.
				DriverState st;
				DriverParams p;
				DriverIn in;
				in.dt = 1.0f / 60.0f;
				in.usable = in.raised = in.aim_idle = true;
				in.request = true;
				const DriverOut o1 = Step(in, p, st);
				in.request = false;
				in.raised = in.aim_idle = false; // the normal stance: still usable
				const DriverOut o2 = Step(in, p, st);
				c(o1.hold_attack && !o2.hold_attack && o2.finished && o2.result == Result::Lost,
					"driver: lowered inside the press lets Circle go at once");
			}
			{
				// Hurt or grabbed mid-swing (not usable): ended, everything let go.
				DriveRun r;
				r.Run(0.3f);
				r.Poll(true);
				r.Run(0.9f);
				r.usable = false;
				r.Poll();
				c(r.result == Result::Lost && !r.out.hold_raise && !r.out.hold_attack, "driver: lost mid-swing lets go");
			}
			{
				// A miss: no contact. A hit just after the swing ends still counts; one during the cooldown of a swing that
				// never ran does not.
				DriveRun r;
				r.g.r1_player = true;
				r.g.hit_s = -1.0f;
				r.Run(1.0f);
				r.Poll(true);
				r.Run(2.0f);
				c(r.lives == 1 && r.contacts == 0, "driver: a miss buzzes nothing");
				DriverState st;
				DriverParams p;
				DriverIn in;
				in.dt = 1.0f / 60.0f;
				in.usable = in.raised = in.aim_idle = true;
				in.request = true;
				Step(in, p, st);
				in.request = false;
				in.aim_idle = false;
				in.swinging = true;
				Step(in, p, st);
				in.swinging = false;
				Step(in, p, st); // over: cooldown
				in.hp_drop = true;
				const DriverOut late = Step(in, p, st);
				c(late.contact, "driver: a hit a frame after the swing ends is its contact");
				DriverState st2;
				in = DriverIn{};
				in.dt = 1.0f / 60.0f;
				in.usable = in.raised = in.aim_idle = true;
				in.request = true;
				Step(in, p, st2);
				in.request = false;
				for (int i = 0; i < 30; i++)
					Step(in, p, st2); // the game never swings: given up
				in.hp_drop = true;
				c(!Step(in, p, st2).contact, "driver: no contact for an attempt that never swung");
			}
			{
				// Requests while one runs, and in the cooldown, are ignored; after it one is taken again.
				DriveRun r;
				r.g.r1_player = true;
				r.Run(1.0f);
				r.Poll(true);
				for (int i = 0; i < 60; i++)
					r.Poll(i % 5 == 0);
				c(r.starts == 1, "driver: requests while swinging are ignored");
				r.Run(2.0f);
				r.Poll(true);
				r.Run(2.0f);
				c(r.starts == 2 && r.lives == 2, "driver: the next swing after the cooldown is taken");
			}
		}

		// A test map: turn by yaw about +Y, scale by u, then offset, like CameraDriver's with a recenter at the origin.
		struct TestMap
		{
			float yaw = 0.7f, u = 100.0f, off[3] = {8000.0f, 155.0f, 13000.0f};
		};
		bool TestMapFn(const void* ctx, const float vr[3], float game[3])
		{
			const TestMap& m = *static_cast<const TestMap*>(ctx);
			const float c = std::cos(m.yaw), s = std::sin(m.yaw);
			game[0] = m.off[0] + m.u * (c * vr[0] + s * vr[2]);
			game[1] = m.off[1] + m.u * vr[1];
			game[2] = m.off[2] + m.u * (-s * vr[0] + c * vr[2]);
			return true;
		}

		void TestMatrix(Checker& c)
		{
			const TestMap tm;
			const float origin[3] = {0.1f, 1.2f, -0.4f};
			const float axes[3][3] = {{0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
			float m[16];
			c(GameMatrix(origin, axes, TestMapFn, &tm, m), "matrix: built");
			float og[3];
			TestMapFn(&tm, origin, og);
			c(std::fabs(m[12] - og[0]) < 1e-3f && std::fabs(m[13] - og[1]) < 1e-3f && std::fabs(m[14] - og[2]) < 1e-3f &&
				  m[15] == 1.0f && m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f,
				"matrix: row 3 is the mapped origin");
			// A model point 60 cm down the model's x axis lands where the map puts the VR point.
			const float pv[3] = {origin[0] + 0.6f * axes[0][0], origin[1] + 0.6f * axes[0][1], origin[2] + 0.6f * axes[0][2]};
			float pg[3];
			TestMapFn(&tm, pv, pg);
			const float model[3] = {60.0f, 0.0f, 0.0f};
			float game[3];
			for (int k = 0; k < 3; k++)
				game[k] = model[0] * m[0 * 4 + k] + model[1] * m[1 * 4 + k] + model[2] * m[2 * 4 + k] + m[12 + k];
			c(std::fabs(game[0] - pg[0]) < 0.05f && std::fabs(game[1] - pg[1]) < 0.05f && std::fabs(game[2] - pg[2]) < 0.05f,
				"matrix: a model point (row vectors) lands where the map puts it");
			float n0 = 0.0f, d01 = 0.0f;
			for (int k = 0; k < 3; k++)
			{
				n0 += m[k] * m[k];
				d01 += m[k] * m[4 + k];
			}
			c(std::fabs(n0 - 1.0f) < 1e-4f && std::fabs(d01) < 1e-4f, "matrix: rows unit and orthogonal");
			const float bad_axes[3][3] = {{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
			c(!GameMatrix(origin, bad_axes, TestMapFn, &tm, m), "matrix: a degenerate axis fails");
			c(!GameMatrix(origin, axes, nullptr, &tm, m), "matrix: no map fails");
		}
	}

	std::vector<std::string> SelfTest()
	{
		Checker c;
		TestDetector(c);
		TestDriver(c);
		TestMatrix(c);
		return c.failures;
	}

	// -----------------------------------------------------------------------------------------------------------------
	// The glue.
	// -----------------------------------------------------------------------------------------------------------------
	namespace
	{
		// A tick as the game's swing goes live; a firmer buzz when it lands. Times the Haptic Strength setting.
		constexpr SpatialControls::HapticCue kTickLive{0.35f, 0.025f};
		constexpr SpatialControls::HapticCue kBuzzContact{1.0f, 0.09f};

		DetectorState s_detect;
		DriverState s_drive;
		// The pad and button of the last profile that had them, so an attempt ending as first person goes away still lets
		// go cleanly.
		int s_pad_port = -1;
		std::string s_attack_button;
		// The override block: whether it holds the magic now (so it is cleared once when the override stops), and its size
		// (magic, stamp, record, the matrix at +0x40, the position at +0x80).
		constexpr u32 kBlockBytes = 0x90;
		bool s_block_armed = false;
		u32 s_block_addr = 0;
		// What the armed block holds at +0x00 and the game it was written for: ClearBlock zeroes only a block that still
		// reads our magic in that game, never RAM of a VM started since (review d4) or a state's own block (d5).
		u32 s_block_magic = 0;
		u32 s_block_crc = 0;
		bool s_block_stale = false; // after a state load: the next write starts the strike fields from zero
		// The enemies' HP last poll (present, value), for the contact buzz.
		struct EnemySeen
		{
			bool present = false;
			s16 hp = 0;
		};
		std::array<EnemySeen, 256> s_enemies{};
		bool s_enemies_valid = false;
		// The arm-time line.
		std::string s_verdict;
		u32 s_verdict_crc = 0;
		// The last swing's numbers, for its log line.
		float s_swing_speed = 0.0f;
		float s_swing_path = 0.0f;
		int s_hit_enemy = -1;
		s16 s_hit_from = 0, s_hit_to = 0;
		// Online (private co-op): whether the last poll decided CoopFull (CameraDriver's gate reads it), the line it logged,
		// and whether the caves' free RAM read zero before they first went in (Unknown until it is read while eligible).
		bool s_coop_live = false;
		// CameraDriver::SteadyNowMs of the poll that set s_coop_live: the gate trusts it only while that poll is this
		// recent (250 ms, as ReloadDriver::Reloading; past a hitch), so polls that stop reaching melee (no VR input instance)
		// cannot leave the caves in on a stale decision (review m2, d13).
		u64 s_coop_live_ms = 0;
		constexpr u64 kCoopLiveStaleMs = 250;
		std::string s_mode_line;
		enum class FreeRam : u8
		{
			Unknown,
			Clear,
			Busy,
		};
		FreeRam s_free = FreeRam::Unknown;
		u32 s_free_crc = 0;

		bool DefaultRead(u32 address, u8 width, u32* value)
		{
			if (!VMManager::HasValidVM() || static_cast<u64>(address) + width > Ps2MemSize::MainRam)
				return false;
			switch (width)
			{
				case 1: *value = memRead8(address); break;
				case 2: *value = memRead16(address); break;
				default: *value = memRead32(address); break;
			}
			return true;
		}

		bool Read(GuestReader reader, u32 address, u8 width, u32* value)
		{
			return (reader ? reader : DefaultRead)(address, width, value);
		}

		void Pulse(const SpatialControls::HapticCue& cue)
		{
			const float setting = EmuConfig.VR.HapticStrength;
			const float strength = std::isfinite(setting) ? std::clamp(setting, 0.0f, 1.0f) : 1.0f;
			const float amplitude = cue.amplitude * strength;
			if (amplitude > 0.0f)
				XRInput::QueuePulse(VRInputSnapshot::RIGHT, amplitude, cue.seconds);
		}

		bool StrictlyOffline()
		{
			return !EmuConfig.DEV9.EthEnable && !EmuConfig.VR.OnlineSafe;
		}

		// The whole block back to zero (the magic first), so the caves see nothing and the free RAM reads as it was.
		void ClearBlock()
		{
			if (s_block_armed && VMManager::HasValidVM() && s_block_addr + kBlockBytes <= Ps2MemSize::MainRam &&
				VMManager::GetDiscCRC() == s_block_crc && static_cast<u32>(memRead32(s_block_addr)) == s_block_magic)
			{
				for (u32 off = 0; off < kBlockBytes; off += 4)
					memWrite32(s_block_addr + off, 0);
			}
			s_block_armed = false;
		}

		bool MapPoint(const void* ctx, const float vr[3], float game[3])
		{
			return CameraDriver::GameFromVr(*static_cast<const CameraDriver::Mapping*>(ctx), vr, game);
		}

		// The right hand as the hands draw it: position from the grip pose, orientation from the aim pose (whose -Z is the
		// pointing direction on every runtime), the grip pose's own when the aim pose is missing.
		bool RightHand(const VRInputSnapshot& snap, float pos[3], float quat[4])
		{
			const VRHandState& h = snap.hands[VRInputSnapshot::RIGHT];
			if (!h.grip_pose.valid)
				return false;
			const VRPose& orient = h.aim_pose.valid ? h.aim_pose : h.grip_pose;
			for (int k = 0; k < 3; k++)
				pos[k] = h.grip_pose.position_xyz[k];
			for (int k = 0; k < 4; k++)
				quat[k] = orient.orientation_xyzw[k];
			return Finite3(pos) && std::isfinite(quat[0]) && std::isfinite(quat[1]) && std::isfinite(quat[2]) &&
			       std::isfinite(quat[3]);
		}

		// v rotated by the unit quaternion q (xyzw).
		void RotateByQuat(const float q[4], const float v[3], float out[3])
		{
			const float x = q[0], y = q[1], z = q[2], w = q[3];
			const float tx = 2.0f * (y * v[2] - z * v[1]);
			const float ty = 2.0f * (z * v[0] - x * v[2]);
			const float tz = 2.0f * (x * v[1] - y * v[0]);
			out[0] = v[0] + w * tx + (y * tz - z * ty);
			out[1] = v[1] + w * ty + (z * tx - x * tz);
			out[2] = v[2] + w * tz + (x * ty - y * tx);
		}

		bool TestPartnerSwing()
		{
			static const bool s_test = [] {
				const char* e = std::getenv("PCSX2_VR_TEST_PARTNER_SWING");
				return e && e[0] == '1';
			}();
			return s_test;
		}

		bool Diag()
		{
			static const bool s_diag = [] {
				const char* e = std::getenv("PCSX2_VR_MELEE_LOG");
				return e && e[0] == '1';
			}();
			return s_diag;
		}

		// --- Contact mode's state (CPU thread). ---
		bool s_contact_mode = false;
		StrikeState s_contact;
		u32 s_strike_seq = 0;         // the last strike number written to the block (+0x0C)
		float s_tip_speed = 0.0f;     // the weapon's far hit point, m/s, smoothed
		float s_tip_prev[3] = {};
		bool s_tip_have = false;
		float s_prev_a[3] = {}, s_prev_b[3] = {}; // the shape at the last poll (game units), for the swept test
		bool s_prev_valid = false;
		u32 s_shape_weapon = 0xFFFFFFFFu;         // the weapon type s_shape was read for
		HitShape s_shape;
		// The last strike, until its enemy's HP drops (the game's hit) or it times out (the game's own test found nothing).
		int s_pend_enemy = -1;
		float s_pend_s = 0.0f;
		s16 s_pend_hp = 0;
		float s_pend_strength = 0.0f;
		float s_pend_speed = 0.0f;
		int s_pend_sphere = -1;
		u32 s_strikes = 0, s_strike_hits = 0;
		// Hit jolt ([VR] MeleeHitJolt): the far hit point's velocity (game units a second, smoothed), and the last strike's
		// contact (a point on the struck sphere, the swing's velocity there, the sphere's bone) until its hit lands.
		float s_vel_b[3] = {};
		bool s_vel_have = false;
		bool s_pend_jolt = false;
		float s_pend_contact[3] = {}, s_pend_vel[3] = {}, s_pend_a[3] = {}, s_pend_b[3] = {};
		u32 s_pend_bone = 0;
		bool s_jolt_armed = false; // cave G's block holds the magic (cleared once when the jolt may not run)
		u32 s_jolt_addr = 0;
		u32 s_jolt_magic = 0; // as s_block_magic / s_block_crc (review d4)
		u32 s_jolt_crc = 0;
		int s_jolt_wait_zone = -1; // the sphere the pending strike's jolt was armed for (waiting in cave G for the hit), or -1
		// Review d10: cave G counts an entry down only on its enemy's update through 0x6644D0, so an entry for an enemy
		// that never passes there (another kind-2 enemy's update, or one gone) would keep its slot. Game frames counted
		// from the counter (0x48BF93, 0-59) while the jolt is on, and when each entry was written: an entry older than
		// kJoltExpireFrames is free again (a jolt lasts at most maxFrames updates, 2 game frames each).
		constexpr u32 kJoltExpireFrames = 60;
		u32 s_jolt_frames = 0;
		int s_jolt_last_counter = -1;
		u32 s_jolt_written[kJoltEntries] = {};
		// The spring-damper jolt (melee.jolt.spring): the host keeps each entry's spring and rewrites the entry every poll
		// with the angle for the next update and the slope to the one after (cave G's linear step, unchanged, carries it
		// through a missed poll), a few updates ahead; how far cave G got is read back from the entry's frames left.
		struct SpringSlot
		{
			bool live = false;
			u32 enemy = 0;
			u8 bone = 0;
			SpringBone sb; // the state cave G applies at the entry's next update
			int steps = 0;
			u8 frames_written = 0;
			u32 strike = 0;
		};
		SpringSlot s_spring[kJoltEntries];
		constexpr int kSpringAhead = 4;

		void WriteSpringEntry(const ProfileDB::MeleeParams& mp, int k, bool wait)
		{
			SpringSlot& sl = s_spring[k];
			SpringBone next = sl.sb;
			SpringStep(next, mp.jolt.spring);
			s16 now[3], after[3];
			JoltEuler(sl.sb.angle, now);
			JoltEuler(next.angle, after);
			const int frames = std::clamp(mp.jolt.spring.max_frames - sl.steps, 1, kSpringAhead);
			JoltEntry e;
			e.enemy = sl.enemy;
			e.bone = sl.bone;
			e.frames = static_cast<u8>(frames);
			e.wait = wait ? 1 : 0;
			for (int q = 0; q < 3; q++)
			{
				e.angle[q] = now[q];
				e.rate[q] = static_cast<s16>(std::clamp(static_cast<int>(now[q]) - static_cast<int>(after[q]), -32768, 32767));
			}
			u32 w[6];
			PackJoltEntry(e, w);
			const u32 a = mp.jolt_block + 0x10u + 24u * static_cast<u32>(k);
			// The angles first, the frames last (a poll between two of the zombie's updates; if not, one update is off).
			for (int q = 2; q < 6; q++)
				memWrite32(a + 4u * static_cast<u32>(q), w[q]);
			memWrite32(a, w[0]);
			memWrite32(a + 4, w[1]);
			sl.frames_written = e.frames;
			s_jolt_written[k] = s_jolt_frames;
		}

		// Each poll: every live spring is stepped by the updates cave G applied since it was written, then written again,
		// or ended when settled (or after maxFrames updates, or when its entry is no longer ours).
		void PumpSprings(GuestReader reader, const ProfileDB::MeleeParams& mp)
		{
			static const bool s_log = std::getenv("PCSX2_VR_SPRING_LOG") != nullptr;
			for (int k = 0; k < kJoltEntries; k++)
			{
				SpringSlot& sl = s_spring[k];
				if (!sl.live)
					continue;
				const u32 a = mp.jolt_block + 0x10u + 24u * static_cast<u32>(k);
				u32 who = 0, bf = 0;
				if (!Read(reader, a, 4, &who) || !Read(reader, a + 4, 4, &bf) || who != sl.enemy || (bf & 0xFFu) != sl.bone)
				{
					sl.live = false;
					continue;
				}
				if (((bf >> 16) & 0xFFu) != 0)
					continue; // still waiting for the hit
				const int left = static_cast<int>((bf >> 8) & 0xFFu);
				const int done = std::max(0, static_cast<int>(sl.frames_written) - left);
				if (done == 0)
					continue;
				for (int q = 0; q < done; q++)
					SpringStep(sl.sb, mp.jolt.spring);
				sl.steps += done;
				const bool end = SpringSettled(sl.sb, mp.jolt.spring) || sl.steps >= mp.jolt.spring.max_frames;
				if (s_log || Diag())
					Console.WriteLn("(VR) Melee spring: entry %d bone %u update %d (+%d) angle %.2f deg (%.2f %.2f %.2f) rate %.0f deg/s%s", k,
						sl.bone, sl.steps, done, static_cast<double>(Len3(sl.sb.angle[0], sl.sb.angle[1], sl.sb.angle[2])),
						static_cast<double>(sl.sb.angle[0]), static_cast<double>(sl.sb.angle[1]), static_cast<double>(sl.sb.angle[2]),
						static_cast<double>(Len3(sl.sb.rate[0], sl.sb.rate[1], sl.sb.rate[2])), end ? ", settled: off" : "");
				if (end)
				{
					memWrite32(a + 4, sl.bone);
					sl.live = false;
				}
				else
					WriteSpringEntry(mp, k, false);
			}
		}

		void ClearSprings()
		{
			for (SpringSlot& sl : s_spring)
				sl.live = false;
		}

		void ClearJolt()
		{
			if (s_jolt_armed && VMManager::HasValidVM() && s_jolt_addr + kJoltBlockBytes <= Ps2MemSize::MainRam &&
				VMManager::GetDiscCRC() == s_jolt_crc && static_cast<u32>(memRead32(s_jolt_addr)) == s_jolt_magic)
			{
				for (u32 off = 0; off < kJoltBlockBytes; off += 4)
					memWrite32(s_jolt_addr + off, 0);
			}
			s_jolt_armed = false;
			s_jolt_last_counter = -1;
			ClearSprings();
		}

		void ResetContact()
		{
			s_contact.Reset();
			s_tip_have = false;
			s_tip_speed = 0.0f;
			s_prev_valid = false;
			s_pend_enemy = -1;
			s_vel_have = false;
			s_pend_jolt = false;
			s_jolt_wait_zone = -1;
		}

		bool ReadF(GuestReader reader, u32 address, float* f);

		// A strike's jolt: the chain for the struck sphere's bone, each bone's turn from the strike's contact and velocity
		// in its parent's frame (the game's bone matrices, read now), written into free entries of cave G's block. At the
		// strike (wait, zone_in the sphere the VR weapon touched) the entries wait in cave G for the enemy's hit counter, so
		// the jolt starts on the update the hit lands in; after a hit (zone_in -1: the hit's zone +0x13E) they start at the
		// next update. Entries still waiting from an earlier strike go. True when something was written.
		bool WriteJolt(GuestReader reader, const ProfileDB::MeleeParams& mp, int enemy, int zone_in, bool wait)
		{
			// The struck sphere: the hit's zone (the game's own nearest sphere) or the one touched, its centre and radius
			// now, its bone; the contact is its point nearest the weapon's shape at the strike.
			const u32 ebase = mp.enemy_table + static_cast<u32>(enemy) * mp.enemy_stride;
			u32 zone = zone_in >= 0 ? static_cast<u32>(zone_in) : 0u, count = 0, shapes = 0, centres = 0;
			float cc[3], rad = 0.0f;
			if (!mp.zone_offset || (zone_in < 0 && !Read(reader, ebase + mp.zone_offset, 1, &zone)) ||
				!Read(reader, ebase + mp.sphere_count, 1, &count) ||
				zone >= count || zone >= 8 || !Read(reader, ebase + mp.sphere_shapes, 4, &shapes) ||
				!Read(reader, ebase + mp.sphere_centres, 4, &centres) || shapes == 0 || centres == 0 ||
				shapes + 20u * 8u > Ps2MemSize::MainRam || centres + 12u * 8u > Ps2MemSize::MainRam ||
				!Read(reader, shapes + 20u * zone + 0x10u, 4, &s_pend_bone) || !ReadF(reader, shapes + 20u * zone + 0x0Cu, &rad) ||
				!ReadF(reader, centres + 12u * zone, &cc[0]) || !ReadF(reader, centres + 12u * zone + 4u, &cc[1]) ||
				!ReadF(reader, centres + 12u * zone + 8u, &cc[2]) || rad <= 0.0f)
			{
				Console.WriteLn("(VR) Melee: no jolt: the hit's sphere could not be read.");
				return false;
			}
			{
				const float* a = s_pend_a;
				const float ab[3] = {s_pend_b[0] - a[0], s_pend_b[1] - a[1], s_pend_b[2] - a[2]};
				const float l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
				float t = l2 > 1e-6f ? ((cc[0] - a[0]) * ab[0] + (cc[1] - a[1]) * ab[1] + (cc[2] - a[2]) * ab[2]) / l2 : 0.0f;
				t = std::clamp(t, 0.0f, 1.0f);
				const float d[3] = {a[0] + ab[0] * t - cc[0], a[1] + ab[1] * t - cc[1], a[2] + ab[2] * t - cc[2]};
				const float dl = Len3(d[0], d[1], d[2]);
				for (int q = 0; q < 3; q++)
					s_pend_contact[q] = dl > 1e-3f ? cc[q] + d[q] / dl * std::min(rad, dl) : cc[q];
			}
			const JoltChain* ch = JoltChainFor(mp.jolt, s_pend_bone);
			if (!ch)
			{
				Console.WriteLn("(VR) Melee: no jolt for a hit on bone %u (no chain in the profile).", s_pend_bone);
				return false;
			}
			u32 hook = 0;
			if (!Read(reader, mp.jolt_hook, 4, &hook) || hook != mp.jolt_hook_on)
			{
				Console.WriteLn("(VR) Melee: no jolt: cave G is not in.");
				return false;
			}
			const u32 base = mp.enemy_table + static_cast<u32>(enemy) * mp.enemy_stride;
			u32 model = 0;
			if (!Read(reader, base + mp.jolt_bones, 4, &model) || model == 0 || model + mp.jolt_bone_table + 4u * 256u > Ps2MemSize::MainRam)
				return false;
			// The model's own bone count (review d10): a smaller model's table holds stale pointers past it (seen: 23 bones,
			// 36 pointers). Cave G checks the same before each apply.
			u32 bones_real = mp.jolt_bone_count;
			if (mp.jolt_bone_real != 0)
			{
				u32 n = 0;
				if (!Read(reader, model + mp.jolt_bone_real, 2, &n))
					return false;
				bones_real = std::min(bones_real, n);
			}
			// The bone's world matrix (4 x 4 row vectors, row 3 its joint).
			const auto bone_world = [&](u32 bone, float w[16]) {
				u32 ptr = 0;
				if (bone >= bones_real || !Read(reader, model + mp.jolt_bone_table + 4u * bone, 4, &ptr) || ptr == 0 ||
					ptr + 64u > Ps2MemSize::MainRam)
					return false;
				for (u32 k = 0; k < 16; k++)
					if (!ReadF(reader, ptr + 4u * k, &w[k]))
						return false;
				return true;
			};
			const JoltSize size = JoltMagnitude(s_pend_speed, mp.jolt);
			const bool spring = mp.jolt.spring_on;
			const u32 blk = mp.jolt_block;
			// This enemy's old entries go, and any still waiting for an earlier strike's hit; then the free ones are taken
			// in order.
			u8 frames_left[kJoltEntries] = {};
			for (int k = 0; k < kJoltEntries; k++)
			{
				const u32 e = blk + 0x10u + 24u * static_cast<u32>(k);
				u32 who = 0, bf = 0;
				Read(reader, e, 4, &who);
				Read(reader, e + 4, 4, &bf);
				frames_left[k] = static_cast<u8>(bf >> 8);
				const bool expired = s_jolt_frames - s_jolt_written[k] > kJoltExpireFrames;
				const bool waiting = ((bf >> 16) & 0xFFu) != 0;
				// A running spring stays (a new hit on its bone adds to it); the linear jolt starts over for this enemy.
				const bool keep = spring && s_jolt_armed && s_spring[k].live && !waiting && !expired && who == s_spring[k].enemy;
				if (!keep && (!s_jolt_armed || who == base || waiting || (expired && frames_left[k] != 0)))
				{
					memWrite32(e + 4, 0);
					frames_left[k] = 0;
				}
				if (!keep)
					s_spring[k].live = false;
			}
			std::string line;
			int slot = 0;
			for (int i = 0; i < ch->count && spring; i++)
			{
				// The spring: an impulse along the swing's turn; a bone already swinging for this enemy takes it on top.
				if (((ch->hard >> i) & 1) && !(s_pend_speed >= mp.jolt.spring.hard))
					continue;
				float bw[16], pw[16], axis[3];
				if (!bone_world(ch->bones[i], bw) || !bone_world(ch->parents[i], pw) ||
					!JoltAxis(s_pend_contact, &bw[12], s_pend_vel, pw, axis, (ch->below >> i) & 1))
					continue;
				const float dps = SpringImpulse(s_pend_speed, ch->weights[i], mp.jolt.spring);
				int k = 0;
				while (k < kJoltEntries && !(s_spring[k].live && s_spring[k].enemy == base && s_spring[k].bone == ch->bones[i]))
					k++;
				const bool add = k < kJoltEntries;
				if (add && s_spring[k].strike == s_strikes)
					continue; // this strike's impulse is in already (written at the strike, the hit landed elsewhere)
				if (!add)
				{
					while (slot < kJoltEntries && (frames_left[slot] != 0 || s_spring[slot].live))
						slot++;
					if (slot >= kJoltEntries)
						break;
					k = slot;
					s_spring[k] = SpringSlot{};
					s_spring[k].live = true;
					s_spring[k].enemy = base;
					s_spring[k].bone = ch->bones[i];
					s_spring[k].sb.loose = ((ch->loose >> i) & 1) != 0;
				}
				SpringSlot& sl = s_spring[k];
				sl.strike = s_strikes;
				SpringKick(sl.sb, axis, dps);
				if (!add)
					SpringStep(sl.sb, mp.jolt.spring); // the first update shows one step of it
				WriteSpringEntry(mp, k, wait && !add);
				frames_left[k] = sl.frames_written;
				line += fmt::format(" bone {} {}{:.0f} deg/s{}", ch->bones[i], add ? "+" : "", dps, sl.sb.loose ? " (loose)" : "");
			}
			for (int i = 0; i < ch->count && !spring; i++)
			{
				float bw[16], pw[16];
				s16 ang[3];
				if (!bone_world(ch->bones[i], bw) || !bone_world(ch->parents[i], pw) ||
					!JoltAngles(s_pend_contact, &bw[12], s_pend_vel, pw, size.deg * ch->weights[i], ang, (ch->below >> i) & 1))
					continue;
				while (slot < kJoltEntries && frames_left[slot] != 0)
					slot++;
				if (slot >= kJoltEntries)
					break;
				const JoltEntry je = MakeJoltEntry(base, ch->bones[i], ang, size.frames, wait);
				u32 w[6];
				PackJoltEntry(je, w);
				const u32 e = blk + 0x10u + 24u * static_cast<u32>(slot);
				for (int k = 0; k < 6; k++)
					memWrite32(e + 4u * static_cast<u32>(k), w[k]);
				frames_left[slot] = je.frames;
				s_jolt_written[slot] = s_jolt_frames;
				line += fmt::format(" bone {} ({}, {}, {})", ch->bones[i], ang[0], ang[1], ang[2]);
			}
			if (line.empty())
				return false;
			memWrite32(blk, mp.jolt_magic);
			s_jolt_armed = true;
			s_jolt_addr = blk;
			s_jolt_magic = mp.jolt_magic;
			s_jolt_crc = VMManager::GetDiscCRC();
			Console.WriteLn("(VR) Melee: jolt%s%s, enemy %d struck on bone %u at %.1f m/s, %.1f deg over %d frames:%s (%s); swing (%.0f, "
							"%.0f, %.0f) cm/s at (%.0f, %.0f, %.0f).",
				spring ? " (spring)" : "", wait ? " armed at the strike (starts on the hit's update)" : " after the hit", enemy,
				s_pend_bone, static_cast<double>(s_pend_speed), static_cast<double>(spring ? 0.0f : size.deg), spring ? 0 : size.frames,
				line.c_str(), spring ? "impulses about each bone's swing turn, parent frame" : "1/65536 turn, about the parent's x y z",
				static_cast<double>(s_pend_vel[0]), static_cast<double>(s_pend_vel[1]), static_cast<double>(s_pend_vel[2]),
				static_cast<double>(s_pend_contact[0]), static_cast<double>(s_pend_contact[1]), static_cast<double>(s_pend_contact[2]));
			return true;
		}

		// Entries still waiting for a hit (a strike that made none) go, so a later hit on that enemy cannot start them.
		void DisarmJolt(GuestReader reader, const ProfileDB::MeleeParams& mp)
		{
			s_jolt_wait_zone = -1;
			if (!s_jolt_armed)
				return;
			for (int k = 0; k < kJoltEntries; k++)
			{
				const u32 e = mp.jolt_block + 0x10u + 24u * static_cast<u32>(k);
				u32 bf = 0;
				if (Read(reader, e + 4, 4, &bf) && ((bf >> 16) & 0xFFu) != 0)
					memWrite32(e + 4, 0);
			}
		}

		// How many of this enemy's entries cave G has started (wait cleared, frames left) and how many still wait.
		void JoltStarted(GuestReader reader, const ProfileDB::MeleeParams& mp, int enemy, int* started, int* waiting)
		{
			*started = *waiting = 0;
			const u32 base = mp.enemy_table + static_cast<u32>(enemy) * mp.enemy_stride;
			for (int k = 0; k < kJoltEntries; k++)
			{
				const u32 e = mp.jolt_block + 0x10u + 24u * static_cast<u32>(k);
				u32 who = 0, bf = 0;
				if (!Read(reader, e, 4, &who) || !Read(reader, e + 4, 4, &bf) || who != base || ((bf >> 8) & 0xFFu) == 0)
					continue;
				(((bf >> 16) & 0xFFu) != 0 ? *waiting : *started) += 1;
			}
		}

		bool ReadF(GuestReader reader, u32 address, float* f)
		{
			u32 w = 0;
			if (!Read(reader, address, 4, &w))
				return false;
			std::memcpy(f, &w, 4);
			return std::isfinite(*f);
		}

		// The game's hit shape for a weapon type, read from its tables (research/2026-10-14-vr-melee.md): the type's attack
		// id, that attack's shape index, the shape entry (type 3 capsule or 2 sphere) and its data; only shapes on the
		// attack object's own matrix (bone 0xFFFF), which every melee weapon uses.
		bool ReadShape(GuestReader reader, const ProfileDB::MeleeParams& mp, u32 type, HitShape* out, std::string* why)
		{
			*out = HitShape{};
			u32 id = 0, index = 0, kind = 0, data = 0;
			if (type >= 0x100 || !Read(reader, mp.shape_types + type * mp.shape_type_stride + mp.shape_attack, 1, &id) ||
				!Read(reader, mp.shape_params + id * mp.shape_param_stride + mp.shape_index, 1, &index) ||
				!Read(reader, mp.shape_table + index * 8u, 4, &kind) || !Read(reader, mp.shape_table + index * 8u + 4u, 4, &data) ||
				data == 0 || data >= Ps2MemSize::MainRam)
			{
				*why = "its shape could not be read";
				return false;
			}
			HitShape s;
			bool ok = ReadF(reader, data, &s.p[0]) && ReadF(reader, data + 4, &s.p[1]) && ReadF(reader, data + 8, &s.p[2]);
			u32 r = 0, len = 0, bone = 0;
			if (kind == 3)
			{
				s.capsule = true;
				ok = ok && ReadF(reader, data + 0x0C, &s.d[0]) && ReadF(reader, data + 0x10, &s.d[1]) &&
				     ReadF(reader, data + 0x14, &s.d[2]) && Read(reader, data + 0x18, 2, &r) && Read(reader, data + 0x1A, 2, &len) &&
				     Read(reader, data + 0x1C, 2, &bone);
				s.length = static_cast<float>(len);
			}
			else if (kind == 2)
				ok = ok && Read(reader, data + 0x0C, 2, &r) && Read(reader, data + 0x0E, 2, &bone);
			else
			{
				*why = fmt::format("its shape is of type {} (not a capsule or a sphere)", kind);
				return false;
			}
			s.radius = static_cast<float>(r);
			if (!ok || bone != 0xFFFFu || s.radius <= 0.0f || s.radius > 200.0f || s.length > 500.0f)
			{
				*why = ok ? "its shape is not on the weapon's own matrix" : "its shape could not be read";
				return false;
			}
			s.valid = true;
			*out = s;
			return true;
		}

		// An enemy's hurt spheres this poll (present, an enemy, alive), false otherwise.
		bool ReadEnemy(GuestReader reader, const ProfileDB::MeleeParams& mp, u32 i, EnemyVolume* e, s16* hp_out)
		{
			const u32 base = mp.enemy_table + i * mp.enemy_stride;
			u32 present = 0, kind = 0, hp = 0, count = 0, shapes = 0, centres = 0;
			if (!Read(reader, base + mp.enemy_present, 1, &present) || present == 0 || !Read(reader, base + mp.kind_offset, 1, &kind) ||
				kind != mp.enemy_kind || !Read(reader, base + mp.enemy_hp, 2, &hp) ||
				static_cast<s16>(static_cast<u16>(hp)) <= 0 || !Read(reader, base + mp.sphere_count, 1, &count) || count == 0 ||
				!Read(reader, base + mp.sphere_shapes, 4, &shapes) || !Read(reader, base + mp.sphere_centres, 4, &centres) ||
				shapes == 0 || centres == 0 || shapes >= Ps2MemSize::MainRam || centres >= Ps2MemSize::MainRam)
				return false;
			e->id = static_cast<int>(i);
			e->count = 0;
			for (u32 k = 0; k < std::min<u32>(count, 8); k++)
			{
				float r = 0.0f;
				if (!ReadF(reader, shapes + 20u * k + 0x0C, &r) || r <= 0.0f)
					break; // the list ends at a radius of -1
				float* c = e->c[e->count];
				if (!ReadF(reader, centres + 12u * k, &c[0]) || !ReadF(reader, centres + 12u * k + 4, &c[1]) ||
					!ReadF(reader, centres + 12u * k + 8, &c[2]))
					break;
				e->r[e->count] = r;
				e->count++;
			}
			*hp_out = static_cast<s16>(static_cast<u16>(hp));
			return e->count > 0;
		}

		const char* ZombieReaction(u32 cls)
		{
			switch (cls)
			{
				case 0: return "a flinch";
				case 1: return "a stagger (a flinch the first time)";
				case 2: return "a knockdown";
				default: return "?";
			}
		}

		// Contact mode, every input poll with the block written this poll (can): the weapon's shape against the enemies'
		// hurt spheres, a strike asked of cave D on first contact at speed, the cue, and the result logged.
		void PollContact(GuestReader reader, const ProfileDB::MeleeParams& mp, u32 record, bool can, const float m[16],
			const float origin[3], const float axes[3][3], u32 counter, float dt, bool jolt)
		{
			u32 wt = 0;
			if (!Read(reader, record + mp.weapon_type_offset, 2, &wt))
				can = false;
			if (can && wt != s_shape_weapon)
			{
				s_shape_weapon = wt;
				std::string why;
				if (ReadShape(reader, mp, wt, &s_shape, &why))
				{
					if (s_shape.capsule)
						Console.WriteLn("(VR) Melee: contact shape for weapon type %u: the game's capsule, %.0f long, radius %.0f.",
							wt, static_cast<double>(s_shape.length), static_cast<double>(s_shape.radius));
					else
						Console.WriteLn("(VR) Melee: contact shape for weapon type %u: the game's sphere, radius %.0f.", wt,
							static_cast<double>(s_shape.radius));
				}
				else
					Console.WriteLn("(VR) Melee: no contact for weapon type %u: %s.", wt, why.c_str());
			}
			const ContactParams cp = SanitizedContact(mp.ContactFor(static_cast<u16>(wt)));
			can = can && s_shape.valid;
			u32 hook = 0;
			const bool hooked = Read(reader, mp.hook, 4, &hook) && hook == mp.hook_on;

			float a[3] = {}, b[3] = {};
			if (can)
			{
				PlaceShape(s_shape, m, a, b);
				// The far hit point's velocity in the game's space (the jolt's direction).
				if (s_prev_valid && dt > 0.0f && Len3(b[0] - s_prev_b[0], b[1] - s_prev_b[1], b[2] - s_prev_b[2]) < 200.0f)
				{
					const float k = 1.0f - std::exp(-dt / 0.02f);
					for (int q = 0; q < 3; q++)
					{
						const float inst = (b[q] - s_prev_b[q]) / dt;
						s_vel_b[q] = s_vel_have ? s_vel_b[q] + (inst - s_vel_b[q]) * k : inst;
					}
					s_vel_have = true;
				}
				else
					s_vel_have = false;
				// The far hit point in the headset's space (a model point in centimetres), for the speed.
				float fm[3] = {s_shape.p[0], s_shape.p[1], s_shape.p[2]};
				if (s_shape.capsule)
				{
					const float e[3] = {s_shape.p[0] + s_shape.d[0] * s_shape.length, s_shape.p[1] + s_shape.d[1] * s_shape.length,
						s_shape.p[2] + s_shape.d[2] * s_shape.length};
					if (Len3(e[0], e[1], e[2]) > Len3(fm[0], fm[1], fm[2]))
						std::memcpy(fm, e, sizeof(fm));
				}
				float tip[3];
				for (int c = 0; c < 3; c++)
					tip[c] = origin[c] + 0.01f * (fm[0] * axes[0][c] + fm[1] * axes[1][c] + fm[2] * axes[2][c]);
				if (s_tip_have && dt > 0.0f)
				{
					const float v = Len3(tip[0] - s_tip_prev[0], tip[1] - s_tip_prev[1], tip[2] - s_tip_prev[2]) / dt;
					// A jump of more than a metre in one poll is a tracking glitch, not a swing.
					if (v * dt < 1.0f)
						s_tip_speed += (v - s_tip_speed) * (1.0f - std::exp(-dt / 0.02f));
					else
						s_tip_speed = 0.0f;
				}
				std::memcpy(s_tip_prev, tip, sizeof(tip));
				s_tip_have = true;
			}
			else
			{
				s_tip_have = false;
				s_tip_speed = 0.0f;
				s_vel_have = false;
			}
			const bool sweep = can && s_prev_valid &&
			                   Len3(a[0] - s_prev_a[0], a[1] - s_prev_a[1], a[2] - s_prev_a[2]) < 200.0f;

			int touching[16];
			int sphere_of[16];
			int nt = 0;
			if (can)
			{
				for (u32 i = 0; i < std::min<u32>(mp.enemy_count, static_cast<u32>(s_contact.enemies.size())) && nt < 16; i++)
				{
					EnemyVolume e;
					s16 hp = 0;
					int sph = -1;
					if (ReadEnemy(reader, mp, i, &e, &hp) && Touches(a, b, s_shape.radius, sweep, s_prev_a, s_prev_b, e, cp.lead_cm, &sph))
					{
						touching[nt] = static_cast<int>(i);
						sphere_of[nt] = sph;
						nt++;
					}
				}
			}
			u32 left = 0;
			const bool live = Read(reader, mp.block + 0x20, 4, &left) && left != 0 && left <= 30;

			StrikeIn si;
			si.dt = dt;
			si.usable = can && hooked;
			si.speed = s_tip_speed;
			si.live = live;
			si.touching = touching;
			si.touching_count = nt;
			const StrikeOut so = StrikeStep(si, cp, s_contact);
			if (so.enemy >= 0)
			{
				const u32 blk = mp.block;
				u32 w;
				memWrite32(blk + 0x18, static_cast<u32>(cp.window_frames));
				std::memcpy(&w, &so.strength, 4);
				memWrite32(blk + 0x1C, w);
				memWrite32(blk + 0x28, counter & 0xFFu);
				s_strike_seq++;
				memWrite32(blk + 0x0C, s_strike_seq);
				// The weight of it: a firm buzz now, harder for a harder swing (times the Haptic Strength setting).
				const float k = std::clamp((so.strength - cp.light) / std::max(cp.strong - cp.light, 0.01f), 0.0f, 1.0f);
				Pulse(SpatialControls::HapticCue{0.65f + 0.35f * k, 0.05f + 0.06f * k});
				if (s_pend_enemy >= 0)
					Console.WriteLn("(VR) Melee: strike on enemy %d made no hit before the next strike.", s_pend_enemy);
				s16 hp = 0;
				EnemyVolume e;
				ReadEnemy(reader, mp, static_cast<u32>(so.enemy), &e, &hp);
				int sph = -1;
				for (int q = 0; q < nt; q++)
					if (touching[q] == so.enemy)
						sph = sphere_of[q];
				s_pend_enemy = so.enemy;
				s_pend_s = 0.0f;
				s_pend_hp = hp;
				s_pend_strength = so.strength;
				s_pend_speed = s_tip_speed;
				s_pend_sphere = sph;
				// The jolt: the weapon's shape and the swing's velocity now; the sphere is the one the game's hit records.
				s_pend_jolt = jolt && s_vel_have && Finite3(a) && Finite3(b) && Finite3(s_vel_b);
				for (int q = 0; q < 3; q++)
				{
					s_pend_a[q] = a[q];
					s_pend_b[q] = b[q];
					s_pend_vel[q] = s_vel_b[q];
				}
				s_strikes++;
				Console.WriteLn("(VR) Melee: strike, enemy %d (sphere %d) at %.1f m/s, strength %.2f.", so.enemy, sph,
					static_cast<double>(s_tip_speed), static_cast<double>(so.strength));
				// Armed now, on the sphere touched: cave G starts it on the update the hit lands in.
				s_jolt_wait_zone = -1;
				if (s_pend_jolt && sph >= 0 && WriteJolt(reader, mp, so.enemy, sph, true))
					s_jolt_wait_zone = sph;
				else if (jolt)
					DisarmJolt(reader, mp);
			}
			else if (s_pend_enemy >= 0)
			{
				s_pend_s += dt;
				EnemyVolume e;
				s16 hp = 0;
				const u32 base = mp.enemy_table + static_cast<u32>(s_pend_enemy) * mp.enemy_stride;
				u32 raw = 0;
				if (Read(reader, base + mp.enemy_hp, 2, &raw))
					hp = static_cast<s16>(static_cast<u16>(raw));
				if (hp < s_pend_hp)
				{
					u32 cls = 0xFF, zone = 0xFF;
					if (mp.reaction_offset)
					{
						Read(reader, base + mp.reaction_offset, 1, &cls);
						Read(reader, base + mp.zone_offset, 1, &zone);
					}
					s_strike_hits++;
					Console.WriteLn("(VR) Melee: hit, enemy %d HP %d -> %d, reaction class %u (%s for a zombie), zone %u, %.0f ms "
									"after the strike (%u of %u strikes landed).",
						s_pend_enemy, s_pend_hp, hp, cls, ZombieReaction(cls), zone, static_cast<double>(s_pend_s * 1000.0f),
						s_strike_hits, s_strikes);
					if (jolt && s_pend_jolt)
					{
						// Armed on the sphere the game hit: cave G has started it on the hit's update. Otherwise (another
						// sphere, or not armed) it is written now and starts at the next update.
						int started = 0, waiting = 0;
						if (s_jolt_wait_zone >= 0)
							JoltStarted(reader, mp, s_pend_enemy, &started, &waiting);
						if (s_jolt_wait_zone >= 0 && zone == static_cast<u32>(s_jolt_wait_zone) && started > 0)
							Console.WriteLn("(VR) Melee: jolt started on the hit's own update (%d bone(s), armed at the strike).", started);
						else
						{
							if (s_jolt_wait_zone >= 0)
								Console.WriteLn("(VR) Melee: jolt armed for sphere %d, the hit is on sphere %u (%d started, %d waiting): "
												"written again for the hit.",
									s_jolt_wait_zone, zone, started, waiting);
							WriteJolt(reader, mp, s_pend_enemy, -1, false);
						}
					}
					s_jolt_wait_zone = -1;
					s_pend_jolt = false;
					s_pend_enemy = -1;
				}
				else if (s_pend_s > 0.4f)
				{
					if (s_jolt_wait_zone >= 0)
						DisarmJolt(reader, mp);
					Console.WriteLn("(VR) Melee: strike on enemy %d made no hit (the game's own test found no touch in %d frames; "
									"%u of %u strikes landed).",
						s_pend_enemy, cp.window_frames, s_strike_hits, s_strikes);
					s_pend_enemy = -1;
				}
			}
			if (Diag() && nt > 0)
				Console.WriteLn("(VR) Melee diag: touching %d enemy(ies), first %d sphere %d, tip %.2f m/s%s%s.", nt, touching[0],
					sphere_of[0], static_cast<double>(s_tip_speed), live ? ", a strike live" : "", hooked ? "" : ", cave D not in");
			std::memcpy(s_prev_a, a, sizeof(a));
			std::memcpy(s_prev_b, b, sizeof(b));
			s_prev_valid = can;
		}
	}

	const char* ModeName(Mode mode)
	{
		switch (mode)
		{
			case Mode::Offline: return "offline";
			case Mode::CoopPress: return "private co-op, the game's own swing";
			case Mode::CoopFull: return "private co-op, the hit on the VR weapon";
			default: return "off";
		}
	}

	Mode Decide(const ModeIn& in)
	{
		if (!in.setting)
			return Mode::Off;
		if (in.strictly_offline)
			return Mode::Offline;
		// Online: only the separate setting, only private co-op, never online-safe.
		if (!in.online_setting || !in.private_coop || in.online_safe)
			return Mode::Off;
		return (in.has_online && in.room_master && in.free_clear) ? Mode::CoopFull : Mode::CoopPress;
	}

	int RoomMasterSlot(GuestReader reader, const OnlineParams& p, u32* room_out)
	{
		u32 tag = 0, room = 0, slot = 0;
		if (!Read(reader, p.tag_addr, 4, &tag) || tag != p.tag || !Read(reader, p.room, 2, &room) || room >= 128)
			return -1;
		if (room_out)
			*room_out = room;
		if (!Read(reader, p.masters + room * 4u, 1, &slot) || slot >= 4)
			return -1; // 0xFF: the room has nobody listed
		return static_cast<int>(slot);
	}

	bool RegionZero(GuestReader reader, u32 lo, u32 hi, u32* busy_addr)
	{
		for (u32 a = lo & ~3u; a < hi; a += 4)
		{
			u32 w = 0;
			if (!Read(reader, a, 4, &w) || w != 0)
			{
				if (busy_addr)
					*busy_addr = a;
				return false;
			}
		}
		return true;
	}

	bool CoopOverrideLive()
	{
		if (!s_coop_live)
			return false;
		const u64 now = CameraDriver::SteadyNowMs();
		return now >= s_coop_live_ms && now - s_coop_live_ms <= kCoopLiveStaleMs;
	}

	namespace
	{
		// A scripted guest memory for SelfTestOnline (word granular, little endian like the EE).
		std::array<u32, 64> s_test_mem{};
		u32 s_test_base = 0;
		bool TestRead(u32 address, u8 width, u32* value)
		{
			if (address < s_test_base || address + width > s_test_base + s_test_mem.size() * 4)
				return false;
			const u32 off = address - s_test_base;
			const u32 w = s_test_mem[off / 4] >> ((off % 4) * 8);
			*value = width == 1 ? (w & 0xFFu) : width == 2 ? (w & 0xFFFFu) : w;
			return true;
		}
	}

	std::vector<std::string> SelfTestOnline()
	{
		Checker c;
		ModeIn in;
		c(Decide(in) == Mode::Off, "mode: the setting off is off");
		in.setting = true;
		in.strictly_offline = true;
		c(Decide(in) == Mode::Offline, "mode: strictly offline is offline, the online setting not needed");
		in.strictly_offline = false;
		c(Decide(in) == Mode::Off, "mode: online without PhysicalMeleeOnline is off");
		in.online_setting = true;
		c(Decide(in) == Mode::Off, "mode: online, not private co-op (obsrv, restricted) is off");
		in.private_coop = true;
		c(Decide(in) == Mode::CoopPress, "mode: private co-op, no master: presses only");
		in.room_master = true;
		c(Decide(in) == Mode::CoopPress, "mode: master, but no melee.online: presses only");
		in.has_online = true;
		c(Decide(in) == Mode::CoopPress, "mode: master, free RAM not read clear: presses only");
		in.free_clear = true;
		c(Decide(in) == Mode::CoopFull, "mode: master, free RAM clear: the hit on the VR weapon");
		in.online_safe = true;
		c(Decide(in) == Mode::Off, "mode: online-safe is off online");
		in.online_safe = false;
		in.room_master = false;
		c(Decide(in) == Mode::CoopPress, "mode: the partner is master: presses only");
		in.setting = false;
		c(Decide(in) == Mode::Off, "mode: PhysicalMelee off wins over the online setting");
		c(!Overrides(Mode::CoopPress) && Overrides(Mode::CoopFull) && Overrides(Mode::Offline) && !Overrides(Mode::Off) &&
			  Presses(Mode::CoopPress) && !Presses(Mode::Off),
			"mode: what each mode does");
		c(PartnerSwingFrames(Mode::CoopFull, false) == kPartnerSwingFrames && PartnerSwingFrames(Mode::Offline, false) == 0 &&
			  PartnerSwingFrames(Mode::CoopPress, false) == 0 && PartnerSwingFrames(Mode::Off, false) == 0 &&
			  PartnerSwingFrames(Mode::CoopPress, true) == 0 && PartnerSwingFrames(Mode::Off, true) == 0 &&
			  PartnerSwingFrames(Mode::Offline, true) == kPartnerSwingFrames && kPartnerSwingFrames > 0 && kPartnerSwingFrames < 60,
			"partner swing: online with the hit on the VR weapon only (or the offline test switch), under a second");

		// The room table: tag word at base, the room u16 at base + 8, four rows of 4 bytes from base + 0x10.
		s_test_base = 0x820000;
		s_test_mem.fill(0);
		OnlineParams op;
		op.tag_addr = 0x820000;
		op.tag = 0x6174656E;
		op.room = 0x820008;
		op.masters = 0x820010;
		s_test_mem[0] = 0x6174656E;
		s_test_mem[2] = 0x00000002;             // room 2
		s_test_mem[4 + 2] = 0xFFFF0201;         // room 2: slot 1 first, then 2
		u32 room = 99;
		c(RoomMasterSlot(TestRead, op, &room) == 1 && room == 2, "master: the first slot of the room's row");
		s_test_mem[4 + 2] = 0xFFFFFF00;
		c(RoomMasterSlot(TestRead, op) == 0, "master: slot 0");
		s_test_mem[4 + 2] = 0xFFFFFFFF;
		c(RoomMasterSlot(TestRead, op) == -1, "master: an empty row is nobody");
		s_test_mem[4 + 2] = 0x00000003;
		s_test_mem[0] = 0x6C676E69; // "ingl": SINGLE.BIN's header word, not NETAQ's
		c(RoomMasterSlot(TestRead, op) == -1, "master: another overlay resident is nobody");
		s_test_mem[0] = 0x6174656E;
		s_test_mem[2] = 0x00000080; // room 128
		c(RoomMasterSlot(TestRead, op) == -1, "master: a room past the table is nobody");
		s_test_mem[2] = 0x00000002;
		OnlineParams far = op;
		far.masters = 0x8200F8; // room 2's row at 0x820100, past the scripted memory
		c(RoomMasterSlot(TestRead, far) == -1, "master: a row that cannot be read is nobody");
		c(RoomMasterSlot(TestRead, op) == 3, "master: slot 3");

		u32 busy = 0;
		s_test_mem.fill(0);
		c(RegionZero(TestRead, 0x820000, 0x820100), "free RAM: all zero");
		s_test_mem[0x20] = 1;
		c(!RegionZero(TestRead, 0x820000, 0x820100, &busy) && busy == 0x820080, "free RAM: a word in use is found");
		c(RegionZero(TestRead, 0x820000, 0x820080), "free RAM: the range ends before it");
		c(!RegionZero(TestRead, 0x820000, 0x820200), "free RAM: unreadable memory is not clear");
		return c.failures;
	}

	namespace
	{
		// A File #1 zombie standing at (x, 0, z): its hurt spheres as the game places them (head r 25 at 170, chest r 32 at
		// 120, hips r 30 at 55; game units, centimetres).
		EnemyVolume TestZombie(int id, float x, float z)
		{
			EnemyVolume e;
			e.id = id;
			e.count = 3;
			const float y[3] = {170.0f, 120.0f, 55.0f}, r[3] = {25.0f, 32.0f, 30.0f};
			for (int k = 0; k < 3; k++)
			{
				e.c[k][0] = x;
				e.c[k][1] = y[k];
				e.c[k][2] = z;
				e.r[k] = r[k];
			}
			return e;
		}

		// The iron pipe's capsule (the game's shape 5: from x = +40 to -60 of the model, r 30).
		HitShape TestPipe()
		{
			HitShape s;
			s.valid = true;
			s.capsule = true;
			s.p[0] = 40.0f;
			s.d[0] = -1.0f;
			s.length = 100.0f;
			s.radius = 30.0f;
			return s;
		}

		// A game matrix: model -X (the pipe's far end) pointing along angle a (radians) in the x-z plane from a grip at
		// (gx, gy, gz); rows unit and orthogonal.
		void TestMatrixAt(float a, float gx, float gy, float gz, float m[16])
		{
			const float fx = std::sin(a), fz = std::cos(a); // where the far end points
			const float mm[16] = {-fx, 0.0f, -fz, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, fz, 0.0f, -fx, 0.0f, gx, gy, gz, 1.0f};
			std::memcpy(m, mm, sizeof(mm));
		}

		struct SwingResult
		{
			int strikes = 0;
			int first_enemy = -1;
			float first_t = -1.0f;
			float first_strength = 0.0f;
			std::vector<int> enemies;
		};

		// A horizontal swing of the pipe about a grip at (0, 120, 0): from angle a0 to a1 (radians) in dur seconds (an
		// ease in and out), rest_s at rest first and after; polls of dt. Tip speed from the far end's motion (100 game
		// units a metre). The zombies given; live: whether a strike's object is live (always false here, or always true).
		SwingResult RunSwing(float a0, float a1, float dur, float dt, const std::vector<EnemyVolume>& zombies,
			StrikeState& st, const ContactParams& p = ContactParams{}, bool live = false, float rest_s = 0.2f,
			float grip_y = 120.0f)
		{
			SwingResult res;
			const HitShape pipe = TestPipe();
			float pa[3] = {}, pb[3] = {};
			bool have_prev = false;
			const int n = static_cast<int>((dur + 2.0f * rest_s) / dt);
			float speed = 0.0f;
			for (int i = 0; i <= n; i++)
			{
				const float t = static_cast<float>(i) * dt;
				float u = std::clamp((t - rest_s) / dur, 0.0f, 1.0f);
				u = u * u * (3.0f - 2.0f * u);
				float m[16], a[3], b[3];
				TestMatrixAt(a0 + (a1 - a0) * u, 0.0f, grip_y, 0.0f, m);
				PlaceShape(pipe, m, a, b);
				if (have_prev)
				{
					const float v = Len3(b[0] - pb[0], b[1] - pb[1], b[2] - pb[2]) / dt / 100.0f;
					speed += (v - speed) * (1.0f - std::exp(-dt / 0.02f));
				}
				int touching[8];
				int nt = 0;
				for (const EnemyVolume& z : zombies)
					if (nt < 8 && Touches(a, b, pipe.radius, have_prev, pa, pb, z, p.lead_cm))
						touching[nt++] = z.id;
				StrikeIn in;
				in.dt = dt;
				in.usable = true;
				in.speed = speed;
				in.live = live;
				in.touching = touching;
				in.touching_count = nt;
				const StrikeOut o = StrikeStep(in, p, st);
				if (o.enemy >= 0)
				{
					if (res.strikes == 0)
					{
						res.first_enemy = o.enemy;
						res.first_t = t;
						res.first_strength = o.strength;
					}
					res.strikes++;
					res.enemies.push_back(o.enemy);
				}
				std::memcpy(pa, a, sizeof(pa));
				std::memcpy(pb, b, sizeof(pb));
				have_prev = true;
			}
			return res;
		}
	}

	std::vector<std::string> SelfTestContact()
	{
		Checker c;
		const ContactParams p;
		// Strength.
		c(Strength(p.speed - 0.01f, p) == 0.0f, "strength: below the speed nothing");
		c(std::fabs(Strength(p.speed, p) - p.light) < 1e-5f, "strength: at the speed, light");
		c(std::fabs(Strength(p.firm, p) - 1.0f) < 1e-5f && std::fabs(Strength(p.heavy - 0.01f, p) - 1.0f) < 1e-5f,
			"strength: firm to heavy, the game's own hit");
		c(std::fabs(Strength(p.heavy, p) - p.strong) < 1e-5f && std::fabs(Strength(30.0f, p) - p.strong) < 1e-5f,
			"strength: heavy and above, strong");
		{
			const float mid = Strength(0.5f * (p.speed + p.firm), p);
			c(mid > p.light && mid < 1.0f, "strength: rises between speed and firm");
		}
		c(Strength(std::numeric_limits<float>::quiet_NaN(), p) == 0.0f, "strength: NaN is none");
		{
			ContactParams bad;
			bad.speed = std::numeric_limits<float>::quiet_NaN();
			bad.firm = 0.1f;
			bad.heavy = -3.0f;
			bad.strong = 0.2f;
			bad.window_frames = 1000;
			const ContactParams s = SanitizedContact(bad);
			c(s.speed == p.speed && s.firm >= s.speed && s.heavy >= s.firm && s.strong >= 1.0f && s.window_frames == 30 &&
				  s.rearm < s.speed,
				"strength: nonsense tuning is clamped into order");
		}
		// Geometry.
		{
			const float a[3] = {0, 0, 0}, b[3] = {10, 0, 0}, q[3] = {5, 3, 0}, e[3] = {-4, 0, 3};
			c(std::fabs(SegmentPointDistSq(a, b, q) - 9.0f) < 1e-4f, "segment: distance to the middle");
			c(std::fabs(SegmentPointDistSq(a, b, e) - 25.0f) < 1e-4f, "segment: distance past an end");
			c(std::fabs(SegmentPointDistSq(a, a, q) - 34.0f) < 1e-4f, "segment: a point segment (a sphere)");
			float m[16], pa[3], pb[3];
			TestMatrixAt(0.0f, 100.0f, 120.0f, 0.0f, m);
			PlaceShape(TestPipe(), m, pa, pb);
			// The far end (-60 along the model's X, which points the far end along +z here) at z = +60, the near end at -40.
			c(std::fabs(pb[2] - 60.0f) < 1e-3f && std::fabs(pa[2] + 40.0f) < 1e-3f && std::fabs(pb[0] - 100.0f) < 1e-3f &&
				  std::fabs(pb[1] - 120.0f) < 1e-3f,
				"shape: the pipe placed, the far end b");
			HitShape knife;
			knife.valid = true;
			knife.p[0] = -10.0f;
			knife.radius = 30.0f;
			PlaceShape(knife, m, pa, pb);
			c(pa[0] == pb[0] && pa[2] == pb[2] && std::fabs(pb[2] - 10.0f) < 1e-3f, "shape: a sphere's ends are its centre");
			const EnemyVolume z = TestZombie(5, 0.0f, 150.0f);
			const float k0[3] = {0, 120, 150 - 32 - 30 + 5}, k1[3] = {0, 120, 150 - 32 - 30 - 5};
			int sph = -1;
			c(Touches(k0, k0, 30.0f, false, k0, k0, z, 0.0f, &sph) && sph == 1, "touch: a knife sphere at the chest");
			c(!Touches(k1, k1, 30.0f, false, k1, k1, z, 0.0f) && Touches(k1, k1, 30.0f, false, k1, k1, z, 8.0f),
				"touch: 5 cm short misses, with the 8 cm lead it touches");
			const float f0[3] = {-200, 120, 150}, f1[3] = {-150, 120, 150}, g0[3] = {150, 120, 150}, g1[3] = {200, 120, 150};
			c(!Touches(g0, g1, 30.0f, false, f0, f1, z, 0.0f) && Touches(g0, g1, 30.0f, true, f0, f1, z, 0.0f),
				"touch: swept, a shape that jumps past the zombie between polls still touches");
			EnemyVolume none = z;
			none.count = 0;
			c(!Touches(k0, k0, 30.0f, false, k0, k0, none, 50.0f), "touch: an enemy without spheres is never touched");
		}
		// Swings. The zombie 1.1 m ahead (+z) of a grip at 1.2 m, the pipe swung across in front of it.
		const std::vector<EnemyVolume> one = {TestZombie(7, 0.0f, 110.0f)};
		{
			StrikeState st;
			const SwingResult r = RunSwing(-1.4f, 1.4f, 0.25f, 1.0f / 72.0f, one, st);
			c(r.strikes == 1 && r.first_enemy == 7, "swing: a fast side swing strikes the zombie once");
			c(r.first_strength >= 1.0f, "swing: a fast swing is at least the game's own hit");
		}
		{
			// The hold: the same swing struck at first touch (no hold) is weaker than with the hold, which waits for the
			// tip to stop speeding up, and comes a few polls later at most.
			ContactParams nohold = p;
			nohold.hold_s = 0.0f;
			StrikeState a1, a2;
			const SwingResult h0 = RunSwing(-1.4f, 1.4f, 0.4f, 1.0f / 72.0f, one, a1, nohold);
			const SwingResult h1 = RunSwing(-1.4f, 1.4f, 0.4f, 1.0f / 72.0f, one, a2, p);
			c(h0.strikes == 1 && h1.strikes == 1 && h1.first_strength > h0.first_strength &&
				  h1.first_t > h0.first_t && h1.first_t - h0.first_t <= p.hold_s + 1.5f / 72.0f,
				"hold: the strike waits for the swing's speed, at most hold_s");
		}
		{
			StrikeState st;
			const SwingResult r = RunSwing(-1.4f, 1.4f, 0.12f, 1.0f / 72.0f, one, st);
			c(r.strikes == 1 && std::fabs(r.first_strength - p.strong) < 1e-5f, "swing: a very fast swing is strong");
		}
		{
			StrikeState st;
			const SwingResult r = RunSwing(-1.4f, 1.4f, 0.25f, 1.0f / 20.0f, one, st);
			c(r.strikes == 1, "swing: at 20 polls a second the swept test still strikes");
		}
		{
			StrikeState st;
			const SwingResult r = RunSwing(-1.4f, 1.4f, 2.5f, 1.0f / 72.0f, one, st);
			c(r.strikes == 0, "swing: a slow sweep through the zombie never strikes");
		}
		{
			// Small moves: the pipe resting inside the zombie, waggled 10 degrees each way at 2 Hz.
			StrikeState st;
			int strikes = 0;
			float pa[3] = {}, pb[3] = {}, speed = 0.0f;
			bool have_prev = false;
			const float dt = 1.0f / 72.0f;
			for (int i = 0; i < 72 * 3; i++)
			{
				const float t = static_cast<float>(i) * dt;
				float m[16], a[3], b[3];
				TestMatrixAt(0.17f * std::sin(2.0f * kPi * 2.0f * t), 0.0f, 120.0f, 0.0f, m);
				PlaceShape(TestPipe(), m, a, b);
				if (have_prev)
					speed += (Len3(b[0] - pb[0], b[1] - pb[1], b[2] - pb[2]) / dt / 100.0f - speed) *
					         (1.0f - std::exp(-dt / 0.02f));
				int touching[1];
				const int nt = Touches(a, b, 30.0f, have_prev, pa, pb, one[0], p.lead_cm) ? 1 : 0;
				touching[0] = 7;
				StrikeIn in;
				in.dt = dt;
				in.usable = true;
				in.speed = speed;
				in.touching = touching;
				in.touching_count = nt;
				strikes += StrikeStep(in, p, st).enemy >= 0 ? 1 : 0;
				std::memcpy(pa, a, sizeof(pa));
				std::memcpy(pb, b, sizeof(pb));
				have_prev = true;
			}
			c(strikes == 0, "small moves: a waggle inside the zombie never strikes");
		}
		{
			// A swing, then straight back through the zombie without slowing: no second strike; then a rest (re-armed)
			// and another swing: a second strike.
			StrikeState st;
			const SwingResult r1 = RunSwing(-1.4f, 1.4f, 0.25f, 1.0f / 72.0f, one, st, p, false, 0.0f);
			const SwingResult r2 = RunSwing(1.4f, -1.4f, 0.25f, 1.0f / 72.0f, one, st, p, false, 0.0f);
			c(r1.strikes == 1 && r2.strikes == 1, "re-arm: the back swing after the weapon left the zombie strikes again");
			StrikeState st2;
			const SwingResult s1 = RunSwing(-1.4f, 0.0f, 0.12f, 1.0f / 72.0f, one, st2, p, false, 0.0f);
			// Stopped inside the zombie at speed, then jabbed on at once: the same swing, so no second strike.
			const SwingResult s2 = RunSwing(0.0f, 0.15f, 0.03f, 1.0f / 72.0f, one, st2, p, false, 0.0f);
			c(s1.strikes == 1 && s2.strikes == 0, "re-arm: still inside the zombie at speed is the same swing");
			StrikeState st3;
			RunSwing(-1.4f, 1.4f, 0.25f, 1.0f / 72.0f, one, st3);
			const SwingResult again = RunSwing(-1.4f, 1.4f, 0.25f, 1.0f / 72.0f, one, st3);
			c(again.strikes == 1, "re-arm: after a rest the next swing strikes");
		}
		{
			// Two zombies side by side, one swing: each struck once, the first before the second.
			const std::vector<EnemyVolume> two = {TestZombie(3, -60.0f, 100.0f), TestZombie(4, 60.0f, 100.0f)};
			ContactParams q = p;
			q.cooldown_s = 0.0f;
			StrikeState st;
			const SwingResult r = RunSwing(-1.4f, 1.4f, 0.4f, 1.0f / 72.0f, two, st, q);
			c(r.strikes == 2 && r.enemies.size() == 2 && r.enemies[0] == 3 && r.enemies[1] == 4,
				"two enemies: one swing strikes each once, in order");
		}
		{
			StrikeState st;
			const SwingResult r = RunSwing(-1.4f, 1.4f, 0.25f, 1.0f / 72.0f, one, st, p, true);
			c(r.strikes == 0, "live: no strike while the last strike's attack object lives");
			StrikeState st2;
			StrikeIn in;
			in.dt = 0.01f;
			in.usable = false;
			in.speed = 8.0f;
			const int t7 = 7;
			in.touching = &t7;
			in.touching_count = 1;
			c(StrikeStep(in, p, st2).enemy < 0 && !st2.enemies[7].struck, "usable: nothing strikes while not usable");
			const int bad = 99;
			in.usable = true;
			in.touching = &bad;
			c(StrikeStep(in, p, st2).enemy < 0, "an enemy id past the table is ignored");
		}
		{
			// The grip above the zombie's head (the pipe passes over it): no strike.
			StrikeState st;
			const SwingResult r = RunSwing(-1.4f, 1.4f, 0.2f, 1.0f / 72.0f, one, st, p, false, 0.2f, 260.0f);
			c(r.strikes == 0, "a swing over the zombie's head misses");
		}
		return c.failures;
	}

	// --- Hit jolt (pure). ---

	JoltSize JoltMagnitude(float speed, const JoltParams& p)
	{
		const float span = p.fast - p.slow;
		float t = (std::isfinite(speed) && span > 0.0f) ? (speed - p.slow) / span : 0.0f;
		t = std::clamp(std::isfinite(t) ? t : 0.0f, 0.0f, 1.0f);
		JoltSize s;
		s.deg = p.min_deg + (p.max_deg - p.min_deg) * t;
		if (!std::isfinite(s.deg))
			s.deg = 0.0f;
		s.frames = std::max(1, static_cast<int>(std::lround(static_cast<float>(p.min_frames) +
		                                                    static_cast<float>(p.max_frames - p.min_frames) * t)));
		return s;
	}

	const JoltChain* JoltChainFor(const JoltParams& p, u32 hit_bone)
	{
		for (const JoltChain& c : p.chains)
			if (c.hit_bone == hit_bone && c.count > 0)
				return &c;
		return nullptr;
	}

	bool JoltAxis(const float contact[3], const float pivot[3], const float vel[3], const float pw[16], float out[3], bool below)
	{
		out[0] = out[1] = out[2] = 0.0f;
		const float vl = Len3(vel[0], vel[1], vel[2]);
		if (!(vl >= 1.0f))
			return false;
		const float v[3] = {vel[0] / vl, vel[1] / vl, vel[2] / vl};
		float r[3] = {contact[0] - pivot[0], contact[1] - pivot[1], contact[2] - pivot[2]};
		// A hit on the far side of the joint from the part it moves (a belly hit below the waist) would fold that part
		// toward the hitter; mirrored, the part recoils with the swing (the twist, from the lever's level part, stays).
		r[1] = below ? -std::fabs(r[1]) : std::fabs(r[1]);
		const float rl = Len3(r[0], r[1], r[2]);
		if (!std::isfinite(rl))
			return false;
		const auto cross = [](const float a[3], const float b[3], float o[3]) {
			o[0] = a[1] * b[2] - a[2] * b[1];
			o[1] = a[2] * b[0] - a[0] * b[2];
			o[2] = a[0] * b[1] - a[1] * b[0];
		};
		float ax[3];
		cross(r, v, ax);
		// A short lever, or one along the swing, says nothing about the turn: the part above the joint goes with the swing.
		if (rl < 8.0f || Len3(ax[0], ax[1], ax[2]) < 0.2f * rl)
		{
			const float up[3] = {0.0f, below ? -1.0f : 1.0f, 0.0f};
			cross(up, v, ax);
		}
		float al = Len3(ax[0], ax[1], ax[2]);
		if (!(al > 0.05f))
			return false;
		for (float& x : ax)
			x /= al;
		// Into the parent's frame: rows of the parent's 3 x 3 are its axes in the world (with its scale), v_world = v_parent
		// * P, so an axis in the parent's frame is a_world * P^-1.
		const float a = pw[0], b = pw[1], c = pw[2], d = pw[4], e = pw[5], f = pw[6], g = pw[8], h = pw[9], i = pw[10];
		const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
		if (!std::isfinite(det) || std::fabs(det) < 1e-6f)
			return false;
		const float inv[9] = {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det, (f * g - d * i) / det,
			(a * i - c * g) / det, (c * d - a * f) / det, (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
		float ap[3];
		for (int k = 0; k < 3; k++)
			ap[k] = ax[0] * inv[0 + k] + ax[1] * inv[3 + k] + ax[2] * inv[6 + k];
		al = Len3(ap[0], ap[1], ap[2]);
		if (!(al > 1e-6f))
			return false;
		for (int k = 0; k < 3; k++)
			out[k] = ap[k] / al;
		return true;
	}

	void JoltEuler(const float rv[3], s16 out[3])
	{
		out[0] = out[1] = out[2] = 0;
		const float deg = Len3(rv[0], rv[1], rv[2]);
		if (!std::isfinite(deg) || deg < 1e-6f)
			return;
		const float u[3] = {rv[0] / deg, rv[1] / deg, rv[2] / deg};
		// The turn about that axis as FUN_001903E0 composes it: x, then y, then z about the parent's axes, R = Rz * Ry * Rx
		// (column vectors). The small-angle split (x, y, z = the axis times the angle) leaned a slanted 20 deg turn's axis
		// by about 5 deg; this is exact (the decay, all three taken down together, stays close to the same axis).
		const float th = deg * (3.14159265358979f / 180.0f);
		const float cs = std::cos(th), sn = std::sin(th), vc = 1.0f - cs;
		const float r00 = cs + u[0] * u[0] * vc, r10 = u[1] * u[0] * vc + u[2] * sn, r20 = u[2] * u[0] * vc - u[1] * sn;
		const float r21 = u[2] * u[1] * vc + u[0] * sn, r22 = cs + u[2] * u[2] * vc;
		const float eul[3] = {std::atan2(r21, r22), std::asin(std::clamp(-r20, -1.0f, 1.0f)), std::atan2(r10, r00)};
		const float units = 65536.0f / (2.0f * 3.14159265358979f);
		for (int k = 0; k < 3; k++)
		{
			const float v = std::isfinite(eul[k]) ? std::clamp(units * eul[k], -16384.0f, 16384.0f) : 0.0f;
			out[k] = static_cast<s16>(std::lround(v));
		}
	}

	bool JoltAngles(const float contact[3], const float pivot[3], const float vel[3], const float pw[16], float deg, s16 out[3],
		bool below)
	{
		out[0] = out[1] = out[2] = 0;
		float u[3];
		if (!std::isfinite(deg) || !JoltAxis(contact, pivot, vel, pw, u, below))
			return false;
		const float rv[3] = {u[0] * deg, u[1] * deg, u[2] * deg};
		JoltEuler(rv, out);
		return out[0] != 0 || out[1] != 0 || out[2] != 0;
	}

	// --- The spring-damper jolt (pure). ---

	void SpringKick(SpringBone& b, const float axis[3], float dps)
	{
		const float l = Len3(axis[0], axis[1], axis[2]);
		if (!std::isfinite(l) || l < 1e-6f || !std::isfinite(dps))
			return;
		for (int k = 0; k < 3; k++)
			b.rate[k] += axis[k] / l * dps;
	}

	void SpringStep(SpringBone& b, const SpringParams& p, float dt)
	{
		const float kk = b.loose ? p.loose_stiffness : p.stiffness, cc = b.loose ? p.loose_damping : p.damping;
		const float lim = b.loose ? p.loose_max_deg : p.max_deg;
		if (!std::isfinite(dt) || dt <= 0.0f)
			return;
		const float h = dt / 4.0f;
		for (int s = 0; s < 4; s++)
		{
			// Semi-implicit Euler: the velocity from the force now, then the angle from the new velocity.
			for (int k = 0; k < 3; k++)
			{
				b.rate[k] += (-kk * b.angle[k] - cc * b.rate[k]) * h;
				b.angle[k] += b.rate[k] * h;
			}
			const float a = Len3(b.angle[0], b.angle[1], b.angle[2]);
			if (a > lim && a > 0.0f)
			{
				float out = 0.0f;
				for (int k = 0; k < 3; k++)
				{
					b.angle[k] *= lim / a;
					out += b.rate[k] * b.angle[k] / lim;
				}
				if (out > 0.0f)
					for (int k = 0; k < 3; k++)
						b.rate[k] -= out * b.angle[k] / lim;
			}
		}
		for (int k = 0; k < 3; k++)
			if (!std::isfinite(b.angle[k]) || !std::isfinite(b.rate[k]))
				b.angle[0] = b.angle[1] = b.angle[2] = b.rate[0] = b.rate[1] = b.rate[2] = 0.0f;
	}

	bool SpringSettled(const SpringBone& b, const SpringParams& p)
	{
		return Len3(b.angle[0], b.angle[1], b.angle[2]) < p.settle_deg && Len3(b.rate[0], b.rate[1], b.rate[2]) < p.settle_rate;
	}

	float SpringImpulse(float speed, float w, const SpringParams& p)
	{
		if (!std::isfinite(speed) || !std::isfinite(w))
			return 0.0f;
		return p.impulse * std::clamp(speed, 0.0f, p.max_speed) * w;
	}

	JoltEntry MakeJoltEntry(u32 enemy, u8 bone, const s16 angle[3], int frames, bool wait)
	{
		JoltEntry e;
		e.enemy = enemy;
		e.bone = bone;
		e.wait = wait ? 1 : 0;
		e.frames = static_cast<u8>(std::clamp(frames, 1, 255));
		for (int k = 0; k < 3; k++)
		{
			e.angle[k] = angle[k];
			e.rate[k] = static_cast<s16>(static_cast<int>(angle[k]) / static_cast<int>(e.frames));
		}
		return e;
	}

	void PackJoltEntry(const JoltEntry& e, u32 w[6])
	{
		const auto h = [](s16 x) { return static_cast<u32>(static_cast<u16>(x)); };
		w[0] = e.enemy;
		w[1] = static_cast<u32>(e.bone) | (static_cast<u32>(e.frames) << 8) | (static_cast<u32>(e.wait) << 16);
		w[2] = h(e.angle[0]) | (h(e.angle[1]) << 16);
		w[3] = h(e.angle[2]);
		w[4] = h(e.rate[0]) | (h(e.rate[1]) << 16);
		w[5] = h(e.rate[2]);
	}

	std::vector<std::string> SelfTestJolt()
	{
		Checker c;
		const JoltParams p;
		// Magnitude.
		{
			const JoltSize lo = JoltMagnitude(p.slow - 1.0f, p), at = JoltMagnitude(p.slow, p), hi = JoltMagnitude(p.fast + 3.0f, p);
			const JoltSize mid = JoltMagnitude(0.5f * (p.slow + p.fast), p);
			c(lo.deg == p.min_deg && lo.frames == p.min_frames && at.deg == p.min_deg, "magnitude: at and below slow, the least");
			c(hi.deg == p.max_deg && hi.frames == p.max_frames, "magnitude: from fast, the most");
			c(std::fabs(mid.deg - 0.5f * (p.min_deg + p.max_deg)) < 1e-4f &&
				  mid.frames == static_cast<int>(std::lround(0.5f * (p.min_frames + p.max_frames))),
				"magnitude: linear between");
			const JoltSize nan = JoltMagnitude(std::numeric_limits<float>::quiet_NaN(), p);
			c(nan.deg == p.min_deg && nan.frames >= 1, "magnitude: NaN is the least, never NaN");
		}
		// Chains.
		{
			JoltParams q;
			JoltChain head;
			head.hit_bone = 21;
			head.count = 2;
			head.bones[0] = 20;
			head.bones[1] = 11;
			JoltChain empty;
			empty.hit_bone = 10;
			q.chains = {empty, head};
			c(JoltChainFor(q, 21) == &q.chains[1] && JoltChainFor(q, 10) == nullptr && JoltChainFor(q, 3) == nullptr,
				"chains: by the struck sphere's bone; an empty one is none");
		}
		// Angles. An upright zombie facing +z (identity parent), the neck at 137 and a head hit at 140 from its left (-x)
		// moving +x: the lever is short and along the swing, so straight up x swing = (0, 1, 0) x (1, 0, 0) = -z: a negative
		// turn about z, which carries a point above the joint toward +x.
		const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
		const float neck[3] = {0.0f, 137.0f, 0.0f};
		{
			const float hit[3] = {-20.0f, 140.0f, 0.0f}, vel[3] = {500.0f, 0.0f, 0.0f};
			s16 a[3];
			c(JoltAngles(hit, neck, vel, id, 20.0f, a) && a[0] == 0 && a[1] == 0 && std::abs(a[2] + 3641) <= 1,
				"angles: a side hit level with the joint leans the part away, 20 deg about -z");
			// Check the sense: rotate the point (0, 30, 0) above the joint by that z turn (row vectors, v * Rz).
			const float th = static_cast<float>(a[2]) * (6.2831853f / 65536.0f);
			const float x = 0.0f * std::cos(th) - 30.0f * std::sin(th);
			c(x > 9.0f, "angles: a point above the joint moves along the swing");
		}
		{
			// A frontal hit on the head (the zombie faces +z, the swing goes -z, into it), 20 above the neck: about -x...
			// (0, 20, 0) x (0, 0, -1) = (-20, 0, 0): the head tips back (toward -z).
			const float hit[3] = {0.0f, 157.0f, 10.0f}, vel[3] = {0.0f, 0.0f, -600.0f};
			s16 a[3];
			c(JoltAngles(hit, neck, vel, id, 10.0f, a) && a[0] < -1800 && a[1] == 0 && a[2] == 0, "angles: a frontal hit tips it back");
			const float th = static_cast<float>(a[0]) * (6.2831853f / 65536.0f);
			// v * Rx with Rx rows (1,0,0), (0,c,s), (0,-s,c): (0, 30, 0) -> (0, 30c, 30s).
			c(30.0f * std::sin(th) < -4.0f, "angles: the head goes the swing's way (-z)");
		}
		{
			// A low hit (a belly hit below the waist): mirrored, so the body above recoils with the swing (tips back, -x)
			// rather than folding toward the hitter; for the legs (below) the legs go with the swing: (0, -72, 25) x (0, 0, -1)
			// = (72, 0, 0), and (0, -72, 0) turned about +x goes to -z.
			const float pivot[3] = {0.0f, 105.0f, 0.0f}, hit[3] = {0.0f, 90.0f, 25.0f}, vel[3] = {0.0f, 0.0f, -400.0f};
			s16 a[3];
			c(JoltAngles(hit, pivot, vel, id, 10.0f, a) && a[0] < -1800 && a[1] == 0 && a[2] == 0,
				"angles: a hit below the joint pushes the body above it back");
			const float knee[3] = {0.0f, 32.0f, 25.0f};
			c(JoltAngles(knee, pivot, vel, id, 10.0f, a, true) && a[0] > 1800, "angles: the legs go with the swing");
			const float th = static_cast<float>(a[0]) * (6.2831853f / 65536.0f);
			c(-72.0f * std::sin(th) < -10.0f, "angles: a point below the joint moves along the swing");
		}
		{
			// The parent turned 90 deg about y (its x axis is the world's -z): a world axis of -z is its +x.
			const float turned[16] = {0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1};
			const float hit[3] = {-20.0f, 140.0f, 0.0f}, vel[3] = {500.0f, 0.0f, 0.0f};
			s16 a[3];
			c(JoltAngles(hit, neck, vel, turned, 20.0f, a) && std::abs(a[0] - 3641) <= 1 && a[1] == 0 && a[2] == 0,
				"angles: the axis goes into the parent's frame");
			// Its scale does not change the angle.
			float scaled[16];
			for (int k = 0; k < 16; k++)
				scaled[k] = (k < 12 && (k % 4) < 3) ? turned[k] * 1.03f : turned[k];
			s16 b[3];
			c(JoltAngles(hit, neck, vel, scaled, 20.0f, b) && b[0] == a[0], "angles: the parent's scale does not matter");
		}
		{
			// A slanted axis: the three angles, composed as the game does (x, then y, then z about the parent's axes:
			// Rz * Ry * Rx), give one turn of the asked size about the asked axis (the small-angle split was about 5 deg off).
			const float hit[3] = {-14.0f, 160.0f, 12.0f}, vel[3] = {380.0f, 0.0f, -330.0f};
			s16 a[3];
			const bool ok = JoltAngles(hit, neck, vel, id, 22.0f, a);
			const float k = 6.2831853f / 65536.0f;
			const float x = a[0] * k, y = a[1] * k, z = a[2] * k;
			const float cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y), cz = std::cos(z), sz = std::sin(z);
			// R = Rz * Ry * Rx (column vectors).
			const float R[3][3] = {{cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
				{sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx}, {-sy, cy * sx, cy * cx}};
			const float tr = std::clamp((R[0][0] + R[1][1] + R[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f);
			const float got = std::acos(tr) * (180.0f / 3.14159265f);
			float ax[3] = {R[2][1] - R[1][2], R[0][2] - R[2][0], R[1][0] - R[0][1]};
			// The asked axis: (joint -> contact) x velocity, normalised.
			const float r[3] = {hit[0] - neck[0], std::fabs(hit[1] - neck[1]), hit[2] - neck[2]};
			float want[3] = {r[1] * vel[2] - r[2] * vel[1], r[2] * vel[0] - r[0] * vel[2], r[0] * vel[1] - r[1] * vel[0]};
			const float la = Len3(ax[0], ax[1], ax[2]), lw = Len3(want[0], want[1], want[2]);
			const float dot = (ax[0] * want[0] + ax[1] * want[1] + ax[2] * want[2]) / std::max(la * lw, 1e-9f);
			const float off = std::acos(std::clamp(dot, -1.0f, 1.0f)) * (180.0f / 3.14159265f);
			c(ok && std::abs(got - 22.0f) < 0.2f && off < 0.5f && a[0] != 0 && a[1] != 0 && a[2] != 0,
				"angles: a slanted turn is exact as the game composes it");
		}
		{
			const float hit[3] = {0.0f, 150.0f, 0.0f}, still[3] = {0.0f, 0.0f, 0.5f}, down[3] = {0.0f, -500.0f, 0.0f};
			const float flat[16] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
			const float side[3] = {300.0f, 0.0f, 0.0f};
			s16 a[3];
			c(!JoltAngles(hit, neck, still, id, 20.0f, a), "angles: no velocity, no turn");
			c(!JoltAngles(neck, neck, down, id, 20.0f, a), "angles: straight down on the joint, no axis");
			c(!JoltAngles(hit, neck, side, flat, 20.0f, a), "angles: a singular parent, none");
			c(!JoltAngles(hit, neck, side, id, std::numeric_limits<float>::quiet_NaN(), a), "angles: NaN degrees, none");
		}
		// Entries.
		{
			const s16 ang[3] = {3641, -1000, 7};
			const JoltEntry e = MakeJoltEntry(0x429530, 20, ang, 12);
			c(e.frames == 12 && e.rate[0] == 303 && e.rate[1] == -83 && e.rate[2] == 0, "entry: rates are the angle over the frames");
			// The cave's sequence: applied with frames left, then the angle less the rate; the last applied is about one rate.
			int x = e.angle[0], applied = 0, last = 0;
			for (int f = e.frames; f > 0; f--)
			{
				last = x;
				applied++;
				x -= e.rate[0];
			}
			c(applied == 12 && last == 3641 - 11 * 303 && std::abs(last) <= 2 * e.rate[0], "entry: twelve frames, back to about nothing");
			u32 w[6];
			PackJoltEntry(e, w);
			c(w[0] == 0x429530 && w[1] == 0x0C14 && w[2] == (0x0E39u | (0xFC18u << 16)) && w[3] == 7 && w[4] == (303u | (0xFFADu << 16)) &&
				  w[5] == 0,
				"entry: packed as the cave reads it");
			const JoltEntry z = MakeJoltEntry(1, 2, ang, 0);
			c(z.frames == 1 && z.rate[0] == 3641, "entry: at least one frame");
			// Armed at the strike: byte +6 set (cave G holds the entry until the enemy's hit counter is set).
			const JoltEntry wt = MakeJoltEntry(0x429530, 20, ang, 12, true);
			u32 ww[6];
			PackJoltEntry(wt, ww);
			c(wt.wait == 1 && ww[1] == 0x010C14 && ww[2] == w[2] && ww[4] == w[4] && e.wait == 0, "entry: armed, it waits for the hit");
		}
		return c.failures;
	}

	std::vector<std::string> SelfTestSpring()
	{
		Checker c;
		const SpringParams p;
		const float ax[3] = {1.0f, 0.0f, 0.0f};
		// A 5 m/s hit on a weight-1 bone: the angle per update.
		const auto series = [&](float dps, bool loose, int n, std::vector<float>* out, int* settled_at) {
			SpringBone b;
			b.loose = loose;
			SpringKick(b, ax, dps);
			*settled_at = -1;
			for (int u = 1; u <= n; u++)
			{
				SpringStep(b, p);
				out->push_back(b.angle[0]);
				if (*settled_at < 0 && SpringSettled(b, p))
					*settled_at = u;
			}
		};
		{
			std::vector<float> a;
			int st = -1;
			const float dps = SpringImpulse(5.0f, 1.0f, p);
			series(dps, false, 45, &a, &st);
			float peak = 0.0f, low = 0.0f, late = 0.0f;
			int peak_at = -1, low_at = -1;
			for (int u = 0; u < static_cast<int>(a.size()); u++)
			{
				if (a[u] > peak)
					peak = a[u], peak_at = u + 1;
				if (a[u] < low)
					low = a[u], low_at = u + 1;
			}
			// After the overshoot's lobe (from the first update back at or above zero).
			int back = low_at;
			while (back < static_cast<int>(a.size()) && a[back] < 0.0f)
				back++;
			for (int u = back; u < static_cast<int>(a.size()); u++)
				late = std::max(late, std::fabs(a[u]));
			c(std::fabs(dps - 600.0f) < 1e-3f, "spring: the impulse is impulse x speed x weight");
			c(a[0] > 8.0f && peak > 18.0f && peak < 26.0f && peak_at >= 2 && peak_at <= 4, "spring: a 5 m/s hit peaks near 20 deg in 2-4 updates");
			c(low < -0.1f * peak && low > -0.35f * peak && low_at > peak_at, "spring: it overshoots once, by a tenth to a third");
			c(late < 0.08f * peak, "spring: after the overshoot, nothing more to speak of");
			c(st >= 12 && st <= 20, "spring: settled in 0.4-0.67 s (12-20 updates)");
			std::vector<float> l;
			int lst = -1;
			series(dps * 0.4f, false, 45, &l, &lst);
			c(lst > 0 && lst <= st, "spring: a lighter hit settles no later");
		}
		// The clamps: a huge kick holds at maxDeg (looseMaxDeg for a loose bone) and comes back.
		{
			std::vector<float> a, l;
			int st = -1, lst = -1;
			series(20000.0f, false, 45, &a, &st);
			series(20000.0f, true, 60, &l, &lst);
			float ma = 0.0f, ml = 0.0f;
			for (float x : a)
				ma = std::max(ma, std::fabs(x));
			for (float x : l)
				ml = std::max(ml, std::fabs(x));
			c(ma <= p.max_deg + 1e-3f && ma > 0.8f * p.max_deg && std::fabs(a.back()) < p.settle_deg, "spring: clamped at maxDeg, then back");
			c(ml <= p.loose_max_deg + 1e-3f && ml > p.max_deg, "spring: a loose bone clamps at looseMaxDeg");
		}
		// Loose bones swing slower: their first peak comes later.
		{
			std::vector<float> a, l;
			int st, lst;
			series(400.0f, false, 20, &a, &st);
			series(400.0f, true, 20, &l, &lst);
			const auto peak_at = [](const std::vector<float>& v) {
				return static_cast<int>(std::max_element(v.begin(), v.end()) - v.begin());
			};
			c(peak_at(l) > peak_at(a), "spring: a loose bone peaks later");
		}
		// Accumulation: two kicks add (the same as one of both, unclamped); a second hit mid-swing adds to it; any number of
		// hits stays inside the clamp; any axis length.
		{
			SpringBone one, two, mid;
			SpringKick(one, ax, 500.0f);
			const float ax2[3] = {3.0f, 0.0f, 0.0f};
			SpringKick(two, ax2, 250.0f);
			SpringKick(two, ax, 250.0f);
			SpringStep(one, p);
			SpringStep(two, p);
			c(std::fabs(one.angle[0] - two.angle[0]) < 1e-3f && one.angle[0] > 0.0f, "spring: impulses add");
			SpringKick(mid, ax, 300.0f);
			SpringStep(mid, p);
			const float before = mid.angle[0];
			SpringBone ctl = mid;
			SpringKick(mid, ax, 300.0f);
			SpringStep(mid, p);
			SpringStep(ctl, p);
			c(mid.angle[0] > ctl.angle[0] + 3.0f && mid.angle[0] > before, "spring: a second hit adds to the swing");
			SpringBone many;
			const float diag[3] = {0.0f, 1.0f, 1.0f};
			float mx = 0.0f;
			for (int u = 0; u < 20; u++)
			{
				SpringKick(many, diag, 2000.0f);
				SpringStep(many, p);
				mx = std::max(mx, Len3(many.angle[0], many.angle[1], many.angle[2]));
			}
			c(mx <= p.max_deg + 1e-3f && std::fabs(many.angle[0]) < 1e-4f, "spring: many hits stay inside the clamp, on their axis");
		}
		// Guards and the angles.
		{
			SpringBone b;
			const float zero[3] = {};
			SpringKick(b, zero, 500.0f);
			SpringKick(b, ax, std::numeric_limits<float>::quiet_NaN());
			SpringStep(b, p);
			c(SpringSettled(b, p) && b.angle[0] == 0.0f, "spring: no axis or a NaN kick, nothing");
			c(SpringImpulse(30.0f, 1.0f, p) == p.impulse * p.max_speed && SpringImpulse(std::numeric_limits<float>::quiet_NaN(), 1.0f, p) == 0.0f &&
				  SpringImpulse(-2.0f, 1.0f, p) == 0.0f && SpringImpulse(2.0f, -0.5f, p) < 0.0f,
				"spring: speed clamped, NaN none, weights signed");
			const float rv[3] = {10.0f, 0.0f, 0.0f}, rz[3] = {0.0f, 0.0f, -20.0f};
			s16 e[3], z[3];
			JoltEuler(rv, e);
			JoltEuler(rz, z);
			c(e[0] == 1820 && e[1] == 0 && e[2] == 0 && z[0] == 0 && z[1] == 0 && z[2] == -3641, "spring: the angles as the cave applies them");
		}
		return c.failures;
	}

	void OnStateLoaded()
	{
		s_enemies_valid = false;
		s_detect.Reset();
		s_drive.Reset();
		// The state's RAM has its own block (zero unless it was saved while armed): a block saved armed (our magic, this
		// game) is zeroed by the next poll that does not write it (review d5), anything else is left alone; either way it
		// is written afresh before the caves trust it.
		s_block_armed = s_block_addr != 0;
		s_block_stale = true;
		s_jolt_armed = s_jolt_addr != 0;
		ClearSprings();
		ResetContact();
	}

	Frame Poll(const VRInputSnapshot& snap, GuestReader reader, float dt)
	{
		Frame frame;
		dt = std::clamp(FiniteOr(dt, 0.0f), 0.0f, 0.25f);

		// Nothing is looked up or read unless first person is armed with the local record resolved (CameraDriver: never on
		// obsrv), the session is live and focused and the VM runs.
		const bool live = snap.generation != 0 && snap.actions_active;
		u32 record = 0;
		const ProfileDB::Profile* profile = nullptr;
		if (live && VMManager::GetState() == VMState::Running && !SplitState::Active() && VMManager::HasValidVM() &&
			CameraDriver::LocalRecord(&record))
		{
			const std::string serial = VMManager::GetDiscSerial();
			profile = serial.empty() ? nullptr : ProfileDB::Lookup(serial, VMManager::GetDiscCRC());
		}
		const ProfileDB::MeleeParams* mp = (profile && profile->melee.has_value()) ? &profile->melee.value() : nullptr;
		const ProfileDB::FeedbackParams* fb =
			(profile && profile->feedback.has_value() && profile->feedback->has_weapon) ? &profile->feedback.value() : nullptr;

		// The VR Gamepad control that presses the attack button on a pad.
		const ProfileDB::SpatialControlSpec* gamepad = nullptr;
		if (mp && fb)
		{
			for (const ProfileDB::SpatialControlSpec& c : profile->controls)
			{
				if (c.kind != SpatialControls::DeviceKind::Gamepad || c.usb_port >= 0)
					continue;
				for (const ProfileDB::SpatialControlSpec::Bind& b : c.bind)
				{
					if (b.target == mp->attack_button)
					{
						gamepad = &c;
						break;
					}
				}
				if (gamepad)
					break;
			}
		}
		if (profile)
		{
			if (gamepad)
			{
				s_pad_port = static_cast<int>(gamepad->pad_port);
				s_attack_button = mp->attack_button;
			}
			else
			{
				s_pad_port = -1;
				s_attack_button.clear();
			}
		}
		const bool setting = EmuConfig.VR.PhysicalMelee;
		const bool offline = StrictlyOffline();
		// The local slot: CameraDriver's debounced one (the one `record` was resolved from), never the index byte read
		// here. Offline the game puts a partner's slot in that byte while it runs the partners, and a partner's record
		// and its weapon would then pass every check below (review M1).
		u32 slot = 0;
		const bool slot_known = profile && profile->camera.has_value() && profile->camera->base.has_value() &&
		                        profile->camera->base->is_indexed && CameraDriver::LocalSlot(&slot);
		// And nothing is written for a record that was not also the local record on the previous poll (a second guard
		// on the same rule: the block, the caves and a strike act on one steady record).
		static u32 s_prev_record = 0;
		const bool record_steady = profile && record != 0 && record == s_prev_record;
		s_prev_record = profile ? record : 0;

		// Where it may act (tools/outbreak-vr/research/2026-10-14-melee-coop.md). Offline in full. Online only with
		// PhysicalMeleeOnline in private co-op: every console runs every character's swing from the state partners send,
		// and the console that runs the room's enemies (the first in NETAQ.BIN's list for the room) decides their HP,
		// deaths and reactions and sends them; so the presses (the game's own swing, which partners see) always, and the
		// block and the caves only while that console is this one and the caves' free RAM read zero before they went in.
		ModeIn mi;
		mi.setting = setting;
		mi.online_setting = EmuConfig.VR.PhysicalMeleeOnline;
		mi.strictly_offline = offline;
		mi.private_coop = EmuConfig.DEV9.EthEnable && CameraDriver::PrivateCoop();
		mi.online_safe = EmuConfig.VR.OnlineSafe;
		mi.has_online = mp && mp->has_online && mp->has_override;
		const bool coop_eligible = mi.setting && mi.online_setting && mi.private_coop && !mi.online_safe && mi.has_online &&
		                           fb && gamepad && slot_known;
		{
			const u32 crc = VMManager::GetDiscCRC();
			if (!coop_eligible || crc != s_free_crc)
			{
				s_free = FreeRam::Unknown;
				s_free_crc = crc;
			}
		}
		int master = -1;
		u32 room = 0;
		if (coop_eligible)
		{
			OnlineParams op;
			op.room = mp->online_room;
			op.masters = mp->online_masters;
			op.tag_addr = mp->online_tag_addr;
			op.tag = mp->online_tag;
			master = RoomMasterSlot(reader, op, &room);
			mi.room_master = master >= 0 && static_cast<u32>(master) == slot;
			if (mi.room_master && s_free == FreeRam::Unknown)
			{
				u32 busy = 0, word = 0;
				if (RegionZero(reader, mp->online_free, mp->block + kBlockBytes, &busy))
					s_free = FreeRam::Clear;
				else
				{
					s_free = FreeRam::Busy;
					Read(reader, busy, 4, &word);
					Console.WriteLn("(VR) Melee: online, the caves' free RAM is in use (0x%08X at 0x%X), so the hit stays on "
									"the game's own swing.", word, busy);
				}
			}
		}
		mi.free_clear = s_free == FreeRam::Clear;
		const Mode mode = (mp && fb && gamepad) ? Decide(mi) : Mode::Off;
		s_coop_live = mode == Mode::CoopFull;
		s_coop_live_ms = CameraDriver::SteadyNowMs();

		// The arm-time line: once per game, and again whenever the verdict changes.
		if (profile)
		{
			std::string verdict;
			if (!mp)
				verdict = "off (the profile has no melee block)";
			else if (!fb)
				verdict = "off (the profile has no feedback.weapon: the weapon and its stance cannot be read)";
			else if (!gamepad)
				verdict = fmt::format("off (no VR Gamepad control is bound to {})", mp->attack_button);
			else if (!setting)
				verdict = "off (the VR PhysicalMelee setting)";
			else if (mode == Mode::Off)
				verdict = mi.online_safe     ? "off (online-safe)" :
				          !mi.private_coop   ? "off (online, and not private co-op)" :
				          !mi.online_setting ? "off (online: [VR] PhysicalMeleeOnline is off)" :
				                               "off";
			else if (mode == Mode::Offline && mp->has_contact && mp->has_override &&
			         EmuConfig.VR.PhysicalMeleeMode == Pcsx2Config::VROptions::PhysicalMeleeModeType::Contact)
				verdict = "on, contact (the VR weapon touching an enemy at speed hits through the game's own code: no attack "
				          "action, nothing pressed; harder swings hit harder)";
			else if (mode == Mode::Offline)
				verdict = fmt::format("on (swing to attack with {}; the hit {}{})", mp->attack_button,
					mp->has_override ? "follows the VR weapon" : "stays on the game's own swing (no melee.override)",
					mp->enemy_count ? "; a buzz when it lands" : "");
			else
				verdict = fmt::format("on online, private co-op (nothing is pressed online, attack with {} yourself; the "
									  "hit follows the VR weapon only by contact, PhysicalMeleeMode = Contact, and only while this "
									  "console runs the room's enemies{}{})",
					mp->attack_button, mi.has_online ? "" : ", so never: the profile has no melee.online",
					mp->enemy_count ? "; a buzz when it lands" : "");
			const u32 crc = VMManager::GetDiscCRC();
			if (verdict != s_verdict || crc != s_verdict_crc)
			{
				s_verdict = verdict;
				s_verdict_crc = crc;
				Console.WriteLn("(VR) Physical melee: %s.", verdict.c_str());
			}
			// Online: who runs this room's enemies, whenever that changes.
			std::string line;
			if (mode == Mode::CoopFull)
				line = fmt::format("room {}: this console (slot {}) runs its enemies, so the hit follows the VR weapon{}", room, slot,
					(mp->has_contact && EmuConfig.VR.PhysicalMeleeMode == Pcsx2Config::VROptions::PhysicalMeleeModeType::Contact) ?
						" (contact: no attack action, so partners see no swing)" :
						"");
			else if (mode == Mode::CoopPress && master >= 0)
				line = fmt::format("room {}: the console of slot {} runs its enemies (this is slot {}), so the hit is the game's "
								   "own swing (nothing pressed online)", room, master, slot);
			else if (mode == Mode::CoopPress)
				line = "no room master is known (the network overlay or its table is not read), so the hit is the game's own "
				       "swing (nothing pressed online)";
			if (line != s_mode_line)
			{
				s_mode_line = line;
				if (!line.empty())
					Console.WriteLn("(VR) Melee: online, %s.", line.c_str());
			}
		}

		const bool env = mp && fb && gamepad && Presses(mode);
		// A melee weapon in hand: the local record's equipped item record, behind the same checks the weapon reader makes
		// (present, its own index, owned by the local slot), read here so it works with or without a headset session; how
		// the game attaches its model, from the weapon reader (or the scenario's item table).
		bool melee = false;
		u16 item_id = 0;
		HandModel::GameAttach attach;
		if (env)
		{
			u32 eq = 0, present = 0, index = 0, owner = 0, id = 0;
			if (slot_known && Read(reader, record + fb->weapon_equipped_offset, 1, &eq) && eq != 0)
			{
				const u64 r = static_cast<u64>(fb->weapon_table) + static_cast<u64>(eq) * fb->weapon_stride;
				if (r + fb->weapon_stride <= Ps2MemSize::MainRam && Read(reader, static_cast<u32>(r), 1, &present) &&
					Read(reader, static_cast<u32>(r + 2), 1, &index) &&
					Read(reader, static_cast<u32>(r + fb->weapon_owner_offset), 1, &owner) &&
					Read(reader, static_cast<u32>(r + fb->weapon_id_offset), 2, &id) && present == 1 && index == eq &&
					owner == slot + 1)
				{
					item_id = static_cast<u16>(id);
					melee = ItemData::KindOf(item_id) == ItemData::Kind::Melee;
				}
			}
			if (melee)
			{
				const GameFeedback::WeaponView wv = GameFeedback::GetWeapon();
				if (wv.valid && wv.item_id == item_id && wv.attach.valid)
					attach = wv.attach;
				else
					GameFeedback::AttachFor(item_id, &attach);
			}
		}
		// The stance and the sub-action.
		bool raised = false, normal = false, aim_idle = false, swinging = false;
		if (env)
		{
			u32 stance = 0, sub = 0;
			const bool ok = Read(reader, record + fb->weapon_stance_offset, 1, &stance) &&
			                Read(reader, record + mp->sub_offset, 1, &sub);
			raised = ok && stance == fb->weapon_stance_raised;
			normal = ok && stance == mp->stance_normal;
			aim_idle = raised && sub == mp->sub_idle;
			swinging = raised && sub == mp->sub_attack;
		}
		// A screen up or opening (never-confirm rule; review d3): MenuMode (pauseWhen 0x720AA0) rises 0.55-0.65 s after the
		// game opens the item screen or the map, so melee.screenFlag (0x48BF7E, up at once) counts too, and a press
		// starts only once both have been clear kScreenClearS (screen_block); one in flight lets go as either rises.
		u32 screen_flag = 0;
		const bool screen_now = SpatialControls::MenuMode() ||
		                        (mp && mp->screen_flag.has_value() &&
		                            (!Read(reader, mp->screen_flag.value(), 1, &screen_flag) || screen_flag != 0));
		static float s_screen_clear_s = 0.0f;
		s_screen_clear_s = screen_now ? 0.0f : std::min(s_screen_clear_s + dt, 10.0f);
		const bool screen = screen_now;
		const bool screen_block = screen_now || s_screen_clear_s < kScreenClearS;
		const bool usable = env && melee && (raised || normal);

		// The enemies' HP: a drop since the last poll (read only).
		bool hp_drop = false;
		if (env && mp->enemy_count > 0)
		{
			const u32 n = std::min<u32>(mp->enemy_count, static_cast<u32>(s_enemies.size()));
			for (u32 i = 0; i < n; i++)
			{
				const u32 base = mp->enemy_table + i * mp->enemy_stride;
				u32 present = 0, hp = 0;
				EnemySeen now;
				if (Read(reader, base + mp->enemy_present, 1, &present) && present != 0 && Read(reader, base + mp->enemy_hp, 2, &hp))
				{
					now.present = true;
					now.hp = static_cast<s16>(static_cast<u16>(hp));
				}
				EnemySeen& was = s_enemies[i];
				if (s_enemies_valid && was.present && now.present && now.hp < was.hp && was.hp > 0)
				{
					hp_drop = true;
					s_hit_enemy = static_cast<int>(i);
					s_hit_from = was.hp;
					s_hit_to = now.hp;
				}
				was = now;
			}
			s_enemies_valid = true;
		}
		else
		{
			s_enemies_valid = false;
		}

		// The right hand: the detector's point up the handle, and the model's frame for the override.
		float pos[3] = {}, quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		const bool hand = env && RightHand(snap, pos, quat);

		// Contact mode ([VR] PhysicalMeleeMode = Contact): only where the caves may run (Overrides), with the profile's
		// melee.contact; otherwise the press (CoopPress online, a profile without it, or the setting).
		const bool contact = env && Overrides(mode) && mp->has_override && mp->has_contact &&
		                     EmuConfig.VR.PhysicalMeleeMode == Pcsx2Config::VROptions::PhysicalMeleeModeType::Contact;
		if (contact != s_contact_mode)
		{
			s_contact_mode = contact;
			Console.WriteLn("(VR) Melee: %s.", contact ?
				"contact mode (the VR weapon touching an enemy hits through the game's own code; nothing is pressed)" :
				"press mode (a swing presses the game's own attack)");
		}

		DetectorOut dout;
		DriverOut o;
		if (!contact)
		{
			DetectorIn di;
			di.dt = dt;
			di.valid = hand;
			if (hand)
			{
				const float up[3] = {0.0f, mp->detect.tip_m, 0.0f};
				float r[3];
				RotateByQuat(quat, up, r);
				for (int k = 0; k < 3; k++)
					di.tip[k] = pos[k] + r[k];
			}
			di.armed = usable && !screen_block && s_drive.phase == Phase::Idle;
			dout = Detect(di, mp ? mp->detect : DetectorParams{}, s_detect);
			if (dout.swing)
			{
				s_swing_speed = dout.speed;
				s_swing_path = dout.path_m;
			}

			DriverIn ri;
			ri.dt = dt;
			ri.request = dout.swing;
			ri.usable = usable;
			// Idle takes no request until the screens have been clear kScreenClearS; an attempt in flight ends (Screen) the
			// moment one rises (screen_block is then true too).
			ri.screen = screen_block;
			ri.raised = raised;
			ri.aim_idle = aim_idle;
			ri.swinging = swinging;
			ri.hp_drop = hp_drop;
			o = Step(ri, mp ? mp->drive : DriverParams{}, s_drive);
			if (o.live)
			{
				Pulse(kTickLive);
				Console.WriteLn("(VR) Melee: swing at %.1f m/s (%.2f m of stroke); the game's swing started %.0f ms later%s.",
					static_cast<double>(s_swing_speed), static_cast<double>(s_swing_path), static_cast<double>(o.latency_s * 1000.0f),
					s_drive.we_raise ? " (raised for it)" : "");
			}
			if (o.contact)
			{
				Pulse(kBuzzContact);
				Console.WriteLn("(VR) Melee: contact, enemy %d HP %d -> %d.", s_hit_enemy, s_hit_from, s_hit_to);
			}
			if (o.finished && o.result != Result::Swung)
				Console.WriteLn("(VR) Melee: swing not made: %s.", ResultName(o.result));
		}
		else
		{
			// Nothing is pressed in contact mode; a press attempt in flight lets go.
			s_drive.Reset();
			s_detect.Reset();
		}

		// The override block: where the VR weapon is, in the game's space, every poll it can be known; cleared otherwise.
		bool wrote = false;
		float m[16] = {};
		float origin[3] = {}, axes[3][3] = {};
		u32 counter = 0;
		if (usable && record_steady && Overrides(mode) && mp->has_override && hand && attach.valid)
		{
			CameraDriver::Mapping map;
			if (CameraDriver::GetMapping(&map) &&
				HandModel::ModelFrame(attach, HandModel::Held::Melee, pos, quat, origin, axes) &&
				GameMatrix(origin, axes, MapPoint, &map, m) && Read(reader, mp->counter, 1, &counter))
			{
				const u32 b = mp->block;
				if (!s_block_armed || s_block_stale)
				{
					s_block_stale = false;
					// A fresh block: the strike fields (and the caves' own) start from zero, whatever RAM held.
					for (u32 off = 0x0C; off < 0x40; off += 4)
						memWrite32(b + off, 0);
					memWrite32(b + 0x8C, 0); // cave F's count of partner-swing messages
					s_strike_seq = 0;
				}
				// Cave F: partners see the start of a swing after each strike, online only (CoopFull).
				memWrite32(b + 0x30, PartnerSwingFrames(mode, TestPartnerSwing()));
				for (int k = 0; k < 16; k++)
				{
					u32 w;
					std::memcpy(&w, &m[k], 4);
					memWrite32(b + 0x40 + 4u * static_cast<u32>(k), w);
				}
				for (int k = 0; k < 3; k++)
				{
					u32 w;
					std::memcpy(&w, &m[12 + k], 4);
					memWrite32(b + 0x80 + 4u * static_cast<u32>(k), w);
				}
				memWrite32(b + 0x08, record);
				memWrite32(b + 0x04, counter & 0xFFu);
				memWrite32(b, mp->magic);
				s_block_armed = true;
				s_block_addr = b;
				s_block_magic = mp->magic;
				s_block_crc = VMManager::GetDiscCRC();
				wrote = true;
			}
		}
		if (!wrote)
			ClearBlock();

		// The hit jolt ([VR] MeleeHitJolt): strictly offline, with contact; otherwise its block goes back to zero.
		const bool jolt = contact && mode == Mode::Offline && EmuConfig.VR.MeleeHitJolt && mp->has_jolt;
		if (!jolt)
			ClearJolt();
		else
		{
			u32 c = 0;
			if (Read(reader, mp->counter, 1, &c))
			{
				if (s_jolt_last_counter >= 0)
					s_jolt_frames += (c + 60u - static_cast<u32>(s_jolt_last_counter)) % 60u;
				s_jolt_last_counter = static_cast<int>(c);
			}
			if (mp->jolt.spring_on && s_jolt_armed)
				PumpSprings(reader, *mp);
		}
		if (contact)
			PollContact(reader, *mp, record, wrote && !screen && !swinging, m, origin, axes, counter, dt, jolt);
		else
			s_contact.Reset();

		// PCSX2_VR_MELEE_LOG=1 (tuning, debugging): twice a second, why a swing would or would not attack, and the fastest
		// the measured point moved since the last line.
		if (Diag() && profile)
		{
			static float s_diag_s = 0.0f;
			static float s_diag_peak = 0.0f;
			s_diag_peak = std::max(s_diag_peak, contact ? s_tip_speed : dout.speed);
			s_diag_s += dt;
			if (s_diag_s >= 0.5f)
			{
				Console.WriteLn("(VR) Melee diag: %s%s | env %d melee %d (item %u) raised %d normal %d aim idle %d swinging %d "
								"screen %d hand %d | peak %.2f m/s, stroke %d | %s | block %s",
					ModeName(mode), contact ? ", contact" : "", env, melee, static_cast<unsigned>(item_id), raised, normal,
					aim_idle, swinging, screen, hand, static_cast<double>(s_diag_peak), dout.in_stroke,
					PhaseName(s_drive.phase), wrote ? "written" : "clear");
				s_diag_s = 0.0f;
				s_diag_peak = 0.0f;
			}
		}

		frame.pad_port = s_pad_port;
		frame.attack_button = s_attack_button;
		// Online nothing is pressed at all (review d3: a partner's take-it prompt can open between two polls, and the
		// press would confirm it); contact mode as the room's master needs no press.
		frame.hold_attack = o.hold_attack && !screen_block && mode == Mode::Offline;
		frame.hold_raise = o.hold_raise && !screen_block && mode == Mode::Offline;
		return frame;
	}
}
