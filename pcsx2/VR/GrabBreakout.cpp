// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/GrabBreakout.h"
#include "VR/SpatialControls.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

namespace VR::GrabBreakout
{
	namespace
	{
		// A grip position that has not changed for this long has stopped (scripted input re-sent at 20 Hz leaves
		// 50 ms between changes; a real controller never repeats a position exactly).
		constexpr float kStillS = 0.1f;
		// 60 Hz polls land on the schedule's edges; don't lose one to rounding.
		constexpr float kEdgeEps = 1.0e-4f;

		float FiniteOr(float v, float fallback)
		{
			return std::isfinite(v) ? v : fallback;
		}

		float Dot3(const std::array<float, 3>& a, const std::array<float, 3>& b)
		{
			return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
		}

		// Whatever the caller passed, kept to ranges the detector and the schedule work in.
		Params Sanitized(const Params& in)
		{
			const Params d{};
			Params p;
			p.shake_speed = std::clamp(FiniteOr(in.shake_speed, d.shake_speed), 0.05f, 10.0f);
			p.reversals = std::min<u8>(in.reversals, static_cast<u8>(HandTrack{}.reversal_age.size()));
			p.reversal_window_s = std::clamp(FiniteOr(in.reversal_window_s, d.reversal_window_s), 0.05f, 2.0f);
			p.reversal_speed = std::clamp(FiniteOr(in.reversal_speed, d.reversal_speed), 0.01f, 10.0f);
			p.hold_s = std::clamp(FiniteOr(in.hold_s, d.hold_s), 0.0f, 2.0f);
			p.stick_hz = std::clamp(FiniteOr(in.stick_hz, d.stick_hz), 0.0f, 30.0f);
			p.press_hz = std::clamp(FiniteOr(in.press_hz, d.press_hz), 0.0f, 30.0f);
			p.press_s = std::clamp(FiniteOr(in.press_s, d.press_s), 0.005f, 1.0f);
			p.buzz = std::clamp(FiniteOr(in.buzz, d.buzz), 0.0f, 1.0f);
			p.buzz_s = std::clamp(FiniteOr(in.buzz_s, d.buzz_s), 0.001f, 0.5f);
			return p;
		}

		// One poll of one hand's detector. Its velocity is the runtime's when it reports one, else the grip position's
		// change over the time since it last changed (scripted input carries no velocity).
		void TrackHand(const VRHandState& hs, bool live, const Params& p, float dt, HandTrack& h)
		{
			// Age the reversals; those older than the window drop out.
			u8 kept = 0;
			for (u8 i = 0; i < h.reversal_count; ++i)
			{
				const float age = h.reversal_age[i] + dt;
				if (age <= p.reversal_window_s + kEdgeEps)
					h.reversal_age[kept++] = age;
			}
			h.reversal_count = kept;
			h.since_shake_s = std::min(h.since_shake_s + dt, kNeverS);

			// A hand that is not tracked is not shaking, and starts afresh when it is tracked again.
			if (!live || !hs.grip_pose.valid)
			{
				h = HandTrack{};
				return;
			}

			const std::array<float, 3> pos = {FiniteOr(hs.grip_pose.position_xyz[0], 0.0f),
				FiniteOr(hs.grip_pose.position_xyz[1], 0.0f), FiniteOr(hs.grip_pose.position_xyz[2], 0.0f)};
			const std::array<float, 3> runtime_vel = {FiniteOr(hs.grip_linear_velocity[0], 0.0f),
				FiniteOr(hs.grip_linear_velocity[1], 0.0f), FiniteOr(hs.grip_linear_velocity[2], 0.0f)};
			const bool moved = !h.have_pos || pos != h.pos;
			h.since_move_s += dt;
			if (Dot3(runtime_vel, runtime_vel) > 1.0e-8f)
			{
				h.vel = runtime_vel;
			}
			else if (h.have_pos && moved && h.since_move_s > 1.0e-4f)
			{
				const float inv = 1.0f / h.since_move_s;
				h.vel = {(pos[0] - h.pos[0]) * inv, (pos[1] - h.pos[1]) * inv, (pos[2] - h.pos[2]) * inv};
			}
			else if (!h.have_pos || h.since_move_s >= kStillS)
			{
				h.vel = {0.0f, 0.0f, 0.0f};
			}
			// Otherwise the position has not changed since the last poll: keep the last velocity for a moment.
			if (moved)
			{
				h.pos = pos;
				h.have_pos = true;
				h.since_move_s = 0.0f;
			}

			const float speed2 = Dot3(h.vel, h.vel);
			bool shake = speed2 >= p.shake_speed * p.shake_speed;
			if (p.reversals > 0 && speed2 >= p.reversal_speed * p.reversal_speed)
			{
				// Moving against the last fast direction is a reversal (newest first; the oldest falls off a full list).
				if (h.have_dir && Dot3(h.vel, h.dir) < 0.0f)
				{
					const u8 n = std::min<u8>(h.reversal_count, static_cast<u8>(h.reversal_age.size() - 1));
					for (u8 i = n; i > 0; --i)
						h.reversal_age[i] = h.reversal_age[i - 1];
					h.reversal_age[0] = 0.0f;
					h.reversal_count = static_cast<u8>(n + 1);
				}
				h.dir = h.vel;
				h.have_dir = true;
			}
			if (p.reversals > 0 && h.reversal_count >= p.reversals)
				shake = true;
			if (shake)
				h.since_shake_s = 0.0f;
		}
	}

	Output Step(const VRInputSnapshot& snapshot, const Params& params, bool grabbed, float dt, State& st)
	{
		Output out;
		const Params p = Sanitized(params);
		dt = std::clamp(FiniteOr(dt, 0.0f), 0.0f, 0.25f);
		const bool live = snapshot.generation != 0 && snapshot.actions_active;
		for (int i = 0; i < 2; ++i)
		{
			TrackHand(snapshot.hands[i], live, p, dt, st.hand[i]);
			out.shaking[i] = st.hand[i].since_shake_s <= p.hold_s + kEdgeEps;
		}

		out.active = live && grabbed && (out.shaking[0] || out.shaking[1]);
		if (!out.active)
		{
			// The next grab (or shake) starts the schedule over: the stick right and a Circle press first.
			st.active = false;
			st.t = 0.0f;
			st.press = -1;
			return out;
		}
		if (!st.active)
		{
			st.active = true;
			st.t = 0.0f;
			st.press = -1;
		}

		const float t = st.t;
		if (p.stick_hz > 0.0f)
		{
			const float cycles = t * p.stick_hz + kEdgeEps;
			out.stick_x = (cycles - std::floor(cycles) < 0.5f) ? 1.0f : -1.0f;
		}
		if (p.press_hz > 0.0f)
		{
			const int k = static_cast<int>(std::floor(t * p.press_hz + kEdgeEps));
			const float into = t - static_cast<float>(k) / p.press_hz;
			if (into < p.press_s - kEdgeEps)
			{
				if ((k % 2) == 0)
					out.circle = true;
				else
					out.cross = true;
				out.press_began = (k != st.press);
				st.press = k;
			}
		}
		st.t = t + dt;
		return out;
	}

	bool Fresh(u64 published_ms, u64 now_ms)
	{
		// A reader's clock taken just before the publish it then sees reads as fresh.
		return published_ms != 0 && (now_ms <= published_ms || now_ms - published_ms <= kStaleMs);
	}
}

namespace VR::SpatialControls
{
	namespace
	{
		std::atomic_bool s_grabbed{false};
		std::atomic<u64> s_grabbed_ms{0};
		std::mutex s_grab_params_mutex;
		GrabBreakout::Params s_grab_params;

		// A sideways push of the player's own on the left stick at least this far wins over the waggle.
		constexpr float kOwnStickX = 0.5f;

		u64 GrabNowMs()
		{
			return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}
	}

	void SetGrabbed(bool grabbed, const GrabBreakout::Params& params)
	{
		{
			std::lock_guard<std::mutex> lock(s_grab_params_mutex);
			s_grab_params = params;
		}
		s_grabbed_ms.store(GrabNowMs(), std::memory_order_release);
		s_grabbed.store(grabbed, std::memory_order_release);
	}

	bool Grabbed(GrabBreakout::Params* params)
	{
		if (params)
		{
			std::lock_guard<std::mutex> lock(s_grab_params_mutex);
			*params = s_grab_params;
		}
		if (!s_grabbed.load(std::memory_order_acquire))
			return false;
		return GrabBreakout::Fresh(s_grabbed_ms.load(std::memory_order_acquire), GrabNowMs());
	}

	void ApplyGrabBreakout(const VRInputSnapshot& snapshot, const GrabBreakout::Params& p, bool grabbed, bool menu_mode,
		float dt, GrabBreakout::State& st, ControlValues& v)
	{
		st.buzz = {};
		st.buzz_s = 0.0f;
		// Never into a menu: Circle confirms there.
		const GrabBreakout::Output out = GrabBreakout::Step(snapshot, p, grabbed && !menu_mode, dt, st);
		if (!out.active)
			return;

		// The waggle takes the left stick's X unless the player is pushing it sideways; Y stays the player's.
		float& stick_x = v[static_cast<u32>(ControlId::LeftStickX)];
		if (out.stick_x != 0.0f && !(std::fabs(stick_x) >= kOwnStickX))
			stick_x = out.stick_x;

		// ORed in, and only onto a button the player is not holding: a press ending must never let go of theirs. Circle
		// is ButtonA, and the trigger (RightTriggerPress, also bound to Circle, the game's confirm) held is the player's
		// own Circle too, so Circle's presses stay out while either is held; Cross's while ButtonB is.
		const auto held = [&v](ControlId id) { return v[static_cast<u32>(id)] > 0.5f; };
		bool pressed = false;
		if (out.circle && !held(ControlId::ButtonA) && !held(ControlId::RightTriggerPress))
		{
			v[static_cast<u32>(ControlId::ButtonA)] = 1.0f;
			pressed = true;
		}
		if (out.cross && !held(ControlId::ButtonB))
		{
			v[static_cast<u32>(ControlId::ButtonB)] = 1.0f;
			pressed = true;
		}
		if (pressed && out.press_began && p.buzz > 0.0f)
		{
			const float amplitude = std::clamp(p.buzz, 0.0f, 1.0f);
			for (int h = 0; h < 2; ++h)
				st.buzz[h] = out.shaking[h] ? amplitude : 0.0f;
			st.buzz_s = std::clamp(std::isfinite(p.buzz_s) ? p.buzz_s : 0.02f, 0.001f, 0.5f);
		}
	}
}
