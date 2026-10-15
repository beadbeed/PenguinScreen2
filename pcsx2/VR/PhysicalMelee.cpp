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
				if (!in.usable)
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
		out.hold_attack = st.phase == Phase::Pressing && st.phase_s < p.press_s;
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
			if (s_block_armed && VMManager::HasValidVM() && s_block_addr + kBlockBytes <= Ps2MemSize::MainRam)
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
		return s_coop_live;
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

	void OnStateLoaded()
	{
		s_enemies_valid = false;
		s_detect.Reset();
		s_drive.Reset();
		// The state's RAM has its own block (zero unless it was saved while armed); write it again before trusting it.
		s_block_armed = false;
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
		// The local slot (the profile's camera.base index byte).
		u32 slot = 0;
		const bool slot_known = profile && profile->camera.has_value() && profile->camera->base.has_value() &&
		                        profile->camera->base->is_indexed && Read(reader, profile->camera->base->index_addr, 1, &slot);

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
			else if (mode == Mode::Offline)
				verdict = fmt::format("on (swing to attack with {}; the hit {}{})", mp->attack_button,
					mp->has_override ? "follows the VR weapon" : "stays on the game's own swing (no melee.override)",
					mp->enemy_count ? "; a buzz when it lands" : "");
			else
				verdict = fmt::format("on online, private co-op (swing to attack with {}; partners see the game's own swing; the "
									  "hit follows the VR weapon only while this console runs the room's enemies{}{})",
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
				line = fmt::format("room {}: this console (slot {}) runs its enemies, so the hit follows the VR weapon", room, slot);
			else if (mode == Mode::CoopPress && master >= 0)
				line = fmt::format("room {}: the console of slot {} runs its enemies (this is slot {}), so the hit is the game's "
								   "own swing (press only)", room, master, slot);
			else if (mode == Mode::CoopPress)
				line = "no room master is known (the network overlay or its table is not read), so the hit is the game's own "
				       "swing (press only)";
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
		const bool screen = SpatialControls::MenuMode();
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
		di.armed = usable && !screen && s_drive.phase == Phase::Idle;
		const DetectorOut dout = Detect(di, mp ? mp->detect : DetectorParams{}, s_detect);
		if (dout.swing)
		{
			s_swing_speed = dout.speed;
			s_swing_path = dout.path_m;
		}

		DriverIn ri;
		ri.dt = dt;
		ri.request = dout.swing;
		ri.usable = usable;
		ri.screen = screen;
		ri.raised = raised;
		ri.aim_idle = aim_idle;
		ri.swinging = swinging;
		ri.hp_drop = hp_drop;
		const DriverOut o = Step(ri, mp ? mp->drive : DriverParams{}, s_drive);
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

		// The override block: where the VR weapon is, in the game's space, every poll it can be known; cleared otherwise.
		bool wrote = false;
		if (usable && Overrides(mode) && mp->has_override && hand && attach.valid)
		{
			CameraDriver::Mapping map;
			float origin[3], axes[3][3], m[16];
			u32 counter = 0;
			if (CameraDriver::GetMapping(&map) &&
				HandModel::ModelFrame(attach, HandModel::Held::Melee, pos, quat, origin, axes) &&
				GameMatrix(origin, axes, MapPoint, &map, m) && Read(reader, mp->counter, 1, &counter))
			{
				const u32 b = mp->block;
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
				wrote = true;
			}
		}
		if (!wrote)
			ClearBlock();

		// PCSX2_VR_MELEE_LOG=1 (tuning, debugging): twice a second, why a swing would or would not attack, and the fastest
		// the measured point moved since the last line.
		static const bool s_diag = [] {
			const char* e = std::getenv("PCSX2_VR_MELEE_LOG");
			return e && e[0] == '1';
		}();
		if (s_diag && profile)
		{
			static float s_diag_s = 0.0f;
			static float s_diag_peak = 0.0f;
			s_diag_peak = std::max(s_diag_peak, dout.speed);
			s_diag_s += dt;
			if (s_diag_s >= 0.5f)
			{
				Console.WriteLn("(VR) Melee diag: %s | env %d melee %d (item %u) raised %d normal %d aim idle %d swinging %d "
								"screen %d hand %d | peak %.2f m/s, stroke %d | %s | block %s",
					ModeName(mode), env, melee, static_cast<unsigned>(item_id), raised, normal, aim_idle, swinging, screen,
					hand, static_cast<double>(s_diag_peak), dout.in_stroke, PhaseName(s_drive.phase), wrote ? "written" : "clear");
				s_diag_s = 0.0f;
				s_diag_peak = 0.0f;
			}
		}

		frame.pad_port = s_pad_port;
		frame.attack_button = s_attack_button;
		frame.hold_attack = o.hold_attack && !screen;
		frame.hold_raise = o.hold_raise && !screen;
		return frame;
	}
}
