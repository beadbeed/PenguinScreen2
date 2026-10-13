// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/ReloadGesture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace VR::ReloadGesture
{
	namespace
	{
		using SpatialControls::Vec3;

		// What Out::reason says. Literals shared with the self-test, which compares against them.
		constexpr const char* kReasonReach = "hand in the magazine spot";
		constexpr const char* kReasonGrabbed = "grabbed a magazine";
		constexpr const char* kReasonReady = "magazine at the gun";
		constexpr const char* kReasonLeftGun = "magazine left the gun";
		constexpr const char* kReasonOpened = "grip opened";
		constexpr const char* kReasonReloadRelease = "reload: released at the gun";
		constexpr const char* kReasonReloadTrigger = "reload: trigger at the gun";
		constexpr const char* kReasonNotArmed = "cancelled: not armed";
		constexpr const char* kReasonMenu = "cancelled: a menu is up";
		constexpr const char* kReasonLowered = "cancelled: gun lowered";
		constexpr const char* kReasonLost = "cancelled: tracking lost";
		constexpr const char* kReasonReleasedLost = "cancelled: released while tracking was lost";
		constexpr const char* kReasonReleasedEarly = "cancelled: released before reaching the gun";
		constexpr const char* kReasonTooSlow = "cancelled: too slow";
		constexpr const char* kReasonHeldTooLong = "cancelled: held at the gun too long";

		float FiniteOr(float v, float fallback)
		{
			return std::isfinite(v) ? v : fallback;
		}

		bool FiniteVec(const Vec3& v)
		{
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
		}

		float Distance(const Vec3& a, const Vec3& b)
		{
			const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		// Whatever the caller passed, kept to ranges the detector works in (the thresholds keep a gap between on and off, so
		// the hysteresis cannot collapse into chatter).
		Params Sanitized(const Params& in)
		{
			const Params d{};
			Params p;
			p.spot_exit_margin = std::clamp(FiniteOr(in.spot_exit_margin, d.spot_exit_margin), 0.0f, 0.5f);
			p.gun_radius = std::clamp(FiniteOr(in.gun_radius, d.gun_radius), 0.02f, 1.0f);
			p.gun_exit_margin = std::clamp(FiniteOr(in.gun_exit_margin, d.gun_exit_margin), 0.0f, 0.5f);
			p.gun_drop = std::clamp(FiniteOr(in.gun_drop, d.gun_drop), -0.5f, 0.5f);
			p.grip_on = std::clamp(FiniteOr(in.grip_on, d.grip_on), 0.05f, 1.0f);
			p.grip_off = std::clamp(FiniteOr(in.grip_off, d.grip_off), 0.0f, p.grip_on - 0.05f);
			p.trigger_on = std::clamp(FiniteOr(in.trigger_on, d.trigger_on), 0.05f, 1.0f);
			p.trigger_off = std::clamp(FiniteOr(in.trigger_off, d.trigger_off), 0.0f, p.trigger_on - 0.05f);
			p.fire_on_trigger = in.fire_on_trigger;
			p.move_timeout_s = std::clamp(FiniteOr(in.move_timeout_s, d.move_timeout_s), 0.2f, 10.0f);
			p.ready_timeout_s = std::clamp(FiniteOr(in.ready_timeout_s, d.ready_timeout_s), 0.2f, 30.0f);
			p.loss_grace_s = std::clamp(FiniteOr(in.loss_grace_s, d.loss_grace_s), 0.0f, 2.0f);
			p.gun_hand = (in.gun_hand == VRInputSnapshot::LEFT) ? VRInputSnapshot::LEFT : VRInputSnapshot::RIGHT;
			return p;
		}
	}

	const char* PhaseName(Phase phase)
	{
		switch (phase)
		{
			case Phase::Idle:
				return "Idle";
			case Phase::Reach:
				return "Reach";
			case Phase::Grabbed:
				return "Grabbed";
			case Phase::Ready:
				return "Ready";
			case Phase::WaitOpen:
				return "WaitOpen";
		}
		return "?";
	}

	Out Step(const In& in, const Params& params, State& st)
	{
		Out out;
		const Params p = Sanitized(params);
		const float dt = std::clamp(FiniteOr(in.dt, 0.0f), 0.0f, 0.25f);
		const SpatialControls::HandInput& carry = in.hands.hand[1 - p.gun_hand];
		const SpatialControls::HandInput& gun = in.hands.hand[p.gun_hand];
		const bool carry_ok = carry.valid && FiniteVec(carry.position);
		const bool gun_ok = gun.valid && FiniteVec(gun.position);
		const bool poses_ok = carry_ok && gun_ok;
		const bool enabled = in.armed && !in.menu && in.gun_raised;

		// The magazine hand's grip and trigger, read every poll whatever else is going on (even with the pose lost, or with
		// the detector disabled), so a squeeze that closes while nothing can use it has its edge then and not later, and a
		// grip already closed when the detector wakes is not a grab.
		const float squeeze = std::clamp(FiniteOr(carry.squeeze, 0.0f), 0.0f, 1.0f);
		const float trigger = std::clamp(FiniteOr(carry.trigger, 0.0f), 0.0f, 1.0f);
		const bool was_squeezed = st.squeezed;
		const bool was_trigger = st.trigger_down;
		st.squeezed = st.squeezed ? squeeze > p.grip_off : squeeze >= p.grip_on;
		st.trigger_down = st.trigger_down ? trigger > p.trigger_off : trigger >= p.trigger_on;
		const bool first_poll = !st.have_prev;
		st.have_prev = true;
		const bool rise = !first_poll && st.squeezed && !was_squeezed;
		const bool fall = !first_poll && !st.squeezed && was_squeezed;
		const bool pull = !first_poll && st.trigger_down && !was_trigger;

		// Where the hand is, with hysteresis on both radii. The spot is the hand's alone; the gun's point moves with the gun hand.
		if (carry_ok && in.spot_radius > 0.0f && FiniteVec(in.spot_centre))
		{
			const float d = Distance(carry.position, in.spot_centre);
			st.in_spot = d <= (st.in_spot ? in.spot_radius + p.spot_exit_margin : in.spot_radius);
		}
		else
		{
			st.in_spot = false;
		}
		if (poses_ok)
		{
			const Vec3 well{gun.position.x, gun.position.y - p.gun_drop, gun.position.z};
			const float d = Distance(carry.position, well);
			st.at_gun = d <= (st.at_gun ? p.gun_radius + p.gun_exit_margin : p.gun_radius);
		}

		const Phase was_phase = st.phase;
		const char* reason = "";

		// The gesture is over, reloaded or not: back to idle, or to waiting for the grip to open if it is still closed.
		const auto finish = [&](const char* why) {
			reason = why;
			st.phase = st.squeezed ? Phase::WaitOpen : Phase::Idle;
			st.phase_s = 0.0f;
			st.lost_s = 0.0f;
			st.left_spot = false;
			st.at_gun = false;
		};

		if (st.phase == Phase::Grabbed || st.phase == Phase::Ready)
		{
			st.phase_s += dt;
			if (!enabled)
			{
				finish(!in.armed ? kReasonNotArmed : (in.menu ? kReasonMenu : kReasonLowered));
			}
			else if (!poses_ok)
			{
				// Nothing is known about where the hands are: wait a moment for tracking, but never fire from here.
				st.lost_s += dt;
				if (fall)
					finish(kReasonReleasedLost);
				else if (st.lost_s > p.loss_grace_s)
					finish(kReasonLost);
			}
			else
			{
				st.lost_s = 0.0f;
				// Where the hand is moves the phase first, so a release in the very poll the hand arrives still counts.
				if (st.phase == Phase::Grabbed)
				{
					if (!st.in_spot)
						st.left_spot = true;
					if (st.left_spot && st.at_gun)
					{
						st.phase = Phase::Ready;
						st.phase_s = 0.0f;
						reason = kReasonReady;
					}
				}
				else if (!st.at_gun)
				{
					st.phase = Phase::Grabbed;
					st.phase_s = 0.0f;
					reason = kReasonLeftGun;
				}

				if (fall)
				{
					if (st.phase == Phase::Ready)
					{
						out.reload_requested = true;
						finish(kReasonReloadRelease);
					}
					else
					{
						finish(kReasonReleasedEarly);
					}
				}
				else if (st.phase == Phase::Ready && p.fire_on_trigger && pull)
				{
					out.reload_requested = true;
					finish(kReasonReloadTrigger);
				}
				else if (st.phase == Phase::Grabbed && st.phase_s > p.move_timeout_s)
				{
					finish(kReasonTooSlow);
				}
				else if (st.phase == Phase::Ready && st.phase_s > p.ready_timeout_s)
				{
					finish(kReasonHeldTooLong);
				}
			}
		}
		else
		{
			if (st.phase == Phase::WaitOpen && !st.squeezed)
			{
				st.phase = Phase::Idle;
				reason = kReasonOpened;
			}
			if (st.phase == Phase::Idle || st.phase == Phase::Reach)
			{
				if (!enabled || !carry_ok)
				{
					st.phase = Phase::Idle;
				}
				else if (rise && st.in_spot && poses_ok)
				{
					st.phase = Phase::Grabbed;
					st.phase_s = 0.0f;
					st.lost_s = 0.0f;
					st.left_spot = false;
					st.at_gun = false;
					reason = kReasonGrabbed;
				}
				else
				{
					const Phase next = (st.in_spot && !st.squeezed) ? Phase::Reach : Phase::Idle;
					if (next == Phase::Reach && st.phase != Phase::Reach)
						reason = kReasonReach;
					st.phase = next;
				}
			}
		}

		out.phase = st.phase;
		out.phase_changed = st.phase != was_phase;
		out.owns_left_hand = st.phase == Phase::Grabbed || st.phase == Phase::Ready || st.phase == Phase::WaitOpen;
		out.reason = reason;
		return out;
	}

	SpatialControls::Vec3 SpotCentre(const SpatialControls::Anchor& head, const Params& params)
	{
		const Params d{};
		const SpatialControls::Placement at{FiniteOr(params.spot.side, d.spot.side), FiniteOr(params.spot.height, d.spot.height),
			FiniteOr(params.spot.forward, d.spot.forward), 0.0f};
		return SpatialControls::PlaceControl(head, at).pivot;
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// Self-test.
	// ---------------------------------------------------------------------------------------------------------------------
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

		// A seated player in the room frame (metres, +Y up, facing -Z), the head at kHead: the default spot is the left hip, the
		// gun hand holds the gun out in front of the chest, and the magazine hand rests on the lap. kBesideGun is a hand cupped
		// under and beside the gun (about 9 cm from the magazine well: inside the 15 cm radius); a left-handed player is the same
		// scene mirrored in X.
		constexpr Vec3 kHead{0.0f, 1.2f, 0.0f};
		constexpr Vec3 kSpot{-0.22f, 0.70f, -0.12f};
		constexpr Vec3 kRest{-0.28f, 0.88f, -0.05f};
		constexpr Vec3 kGun{0.18f, 1.02f, -0.45f};
		constexpr Vec3 kBesideGun{0.10f, 0.98f, -0.40f};

		Vec3 Lerp(const Vec3& a, const Vec3& b, float t)
		{
			return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
		}

		// Scripted hands polled at 60 Hz, with what the detector reported along the way.
		struct Sim
		{
			Params p;
			int gun; // the gun hand as Step reads it (VRInputSnapshot::LEFT or RIGHT)
			bool mirrored;
			State st;
			In in;
			Out out;
			int reloads = 0;
			int grabs = 0;  // phase changes into Grabbed by a squeeze (not a return from Ready)
			int readys = 0; // phase changes into Ready
			std::vector<std::string> cancels;
			std::vector<Phase> phases; // every phase change

			explicit Sim(const Params& params = Params{})
				: p(params)
				, gun(params.gun_hand == VRInputSnapshot::LEFT ? VRInputSnapshot::LEFT : VRInputSnapshot::RIGHT)
				, mirrored(params.gun_hand == VRInputSnapshot::LEFT)
			{
				in.armed = true;
				in.gun_raised = true;
				in.spot_centre = At(kSpot);
				in.spot_radius = Params{}.spot_radius;
				Carry().valid = true;
				Carry().position = At(kRest);
				Gun().valid = true;
				Gun().position = At(kGun);
			}

			Vec3 At(const Vec3& v) const { return {mirrored ? -v.x : v.x, v.y, v.z}; }
			SpatialControls::HandInput& Carry() { return in.hands.hand[1 - gun]; }
			SpatialControls::HandInput& Gun() { return in.hands.hand[gun]; }

			static int Polls(float seconds) { return std::max(1, static_cast<int>(std::lround(seconds * 60.0f))); }

			void Poll(float dt = 1.0f / 60.0f)
			{
				in.dt = dt;
				out = Step(in, p, st);
				if (out.reload_requested)
					++reloads;
				if (out.phase_changed)
				{
					phases.push_back(out.phase);
					if (out.phase == Phase::Grabbed && std::strcmp(out.reason, kReasonGrabbed) == 0)
						++grabs;
					if (out.phase == Phase::Ready)
						++readys;
				}
				if (std::strncmp(out.reason, "cancelled", 9) == 0)
					cancels.emplace_back(out.reason);
			}

			void Hold(float seconds)
			{
				for (int i = 0, n = Polls(seconds); i < n; ++i)
					Poll();
			}

			// A straight move of the magazine hand (or the gun hand) over `seconds`, to a point of the right-handed scene.
			void MoveCarry(const Vec3& to, float seconds)
			{
				const Vec3 from = Carry().position;
				const Vec3 end = At(to);
				for (int i = 1, n = Polls(seconds); i <= n; ++i)
				{
					Carry().position = Lerp(from, end, static_cast<float>(i) / static_cast<float>(n));
					Poll();
				}
			}

			void MoveGun(const Vec3& to, float seconds)
			{
				const Vec3 from = Gun().position;
				const Vec3 end = At(to);
				for (int i = 1, n = Polls(seconds); i <= n; ++i)
				{
					Gun().position = Lerp(from, end, static_cast<float>(i) / static_cast<float>(n));
					Poll();
				}
			}

			// The magazine hand's grip, ramped to `to` over `seconds`.
			void Grip(float to, float seconds)
			{
				const float from = Carry().squeeze;
				for (int i = 1, n = Polls(seconds); i <= n; ++i)
				{
					Carry().squeeze = from + (to - from) * static_cast<float>(i) / static_cast<float>(n);
					Poll();
				}
			}

			// The usual start of a try: the magazine hand into the spot and a squeeze.
			void TakeMagazine()
			{
				MoveCarry(kSpot, 0.4f);
				Grip(1.0f, 0.1f);
			}

			bool OnlyCancel(const char* reason) const { return cancels.size() == 1 && cancels[0] == reason; }
		};

		// 1. A clean reload.
		void CleanReload(Checker& c, Sim& s, const std::string& tag)
		{
			s.Hold(0.2f);
			c(s.out.phase == Phase::Idle && s.reloads == 0, tag + ": idle at rest");
			s.MoveCarry(kSpot, 0.5f);
			c(s.out.phase == Phase::Reach, tag + ": reaching into the spot ticks");
			s.Grip(1.0f, 0.1f);
			c(s.out.phase == Phase::Grabbed && s.grabs == 1, tag + ": a squeeze in the spot grabs once");
			c(s.out.owns_left_hand, tag + ": the magazine hand belongs to the gesture");
			s.MoveCarry(kBesideGun, 0.8f);
			c(s.out.phase == Phase::Ready && s.readys == 1, tag + ": at the gun is ready");
			c(s.reloads == 0, tag + ": nothing before the release");
			s.Grip(0.0f, 0.1f);
			c(s.reloads == 1, tag + ": the release is one reload");
			c(s.out.phase == Phase::Idle && !s.out.owns_left_hand, tag + ": idle after");
			s.Hold(1.0f);
			c(s.reloads == 1 && s.cancels.empty(), tag + ": no second reload, no cancel");
		}

		void TestClean(Checker& c)
		{
			Sim right;
			CleanReload(c, right, "clean");

			Params lefty;
			lefty.gun_hand = VRInputSnapshot::LEFT;
			Sim left(lefty);
			CleanReload(c, left, "clean (left-handed)");

			// Nonsense params are clamped, not trusted: NaNs fall back to the defaults, an off threshold above the on one is
			// pulled below it.
			Params bad;
			const float nan = std::numeric_limits<float>::quiet_NaN();
			bad.spot.side = nan;
			bad.spot_radius = nan;
			bad.spot_exit_margin = nan;
			bad.gun_radius = nan;
			bad.gun_exit_margin = nan;
			bad.gun_drop = nan;
			bad.grip_on = nan;
			bad.grip_off = 0.9f;
			bad.trigger_on = nan;
			bad.trigger_off = 2.0f;
			bad.move_timeout_s = nan;
			bad.ready_timeout_s = nan;
			bad.loss_grace_s = nan;
			bad.gun_hand = 7;
			Sim junk(bad);
			CleanReload(c, junk, "clean (nonsense params)");

			// A stall is not a timeout: a 10 s gap counts as at most 0.25 s.
			Sim stall;
			stall.TakeMagazine();
			stall.Poll(10.0f);
			c(stall.out.phase == Phase::Grabbed && stall.cancels.empty(), "clean: a stall is not a timeout");

			// The spot from the head anchor: the placement is metres from the head, forward being -Z at yaw 0, turned with the
			// yaw (a half turn flips X and Z).
			SpatialControls::Anchor head;
			head.position = kHead;
			head.yaw = 0.0f;
			const Vec3 a = SpotCentre(head, Params{});
			c(std::fabs(a.x - kSpot.x) < 1.0e-3f && std::fabs(a.y - kSpot.y) < 1.0e-3f && std::fabs(a.z - kSpot.z) < 1.0e-3f,
				"spot: placed from the head");
			head.yaw = 3.14159265f;
			const Vec3 b = SpotCentre(head, Params{});
			c(std::fabs(b.x + kSpot.x) < 1.0e-3f && std::fabs(b.y - kSpot.y) < 1.0e-3f && std::fabs(b.z + kSpot.z) < 1.0e-3f,
				"spot: turns with the head's yaw");
		}

		// 2. A move that takes too long.
		void TestTooSlow(Checker& c)
		{
			Sim s;
			s.TakeMagazine();
			s.Hold(0.5f); // dawdles at the hip
			s.MoveCarry(kBesideGun, 1.4f);
			c(s.reloads == 0 && s.out.phase != Phase::Ready, "too slow: never ready");
			c(!s.cancels.empty() && s.cancels[0] == kReasonTooSlow, "too slow: cancelled as too slow");
			c(s.out.phase == Phase::WaitOpen, "too slow: waits for the grip to open");
			s.Grip(0.0f, 0.1f);
			c(s.reloads == 0, "too slow: the release at the gun does nothing");
			c(s.out.phase == Phase::Idle, "too slow: idle once the grip is open");
			s.MoveCarry(kSpot, 0.6f);
			s.Grip(1.0f, 0.1f);
			s.MoveCarry(kBesideGun, 0.7f);
			s.Grip(0.0f, 0.1f);
			c(s.reloads == 1, "too slow: a quick retry reloads");

			// 1.4 s from the squeeze to the release is still inside the limit.
			Sim inside;
			inside.MoveCarry(kSpot, 0.4f);
			inside.Grip(1.0f, 0.05f);
			inside.MoveCarry(kBesideGun, 1.4f);
			inside.Grip(0.0f, 0.05f);
			c(inside.reloads == 1, "too slow: 1.4 s still counts");
		}

		// 3. The gun lowered, first person disarmed, or a menu up mid-gesture.
		void TestLowered(Checker& c)
		{
			for (int what = 0; what < 3; ++what)
			{
				const std::string tag = what == 0 ? "gun lowered" : (what == 1 ? "disarmed" : "menu up");
				const char* reason = what == 0 ? kReasonLowered : (what == 1 ? kReasonNotArmed : kReasonMenu);
				Sim s;
				s.TakeMagazine();
				s.MoveCarry(Lerp(kSpot, kBesideGun, 0.5f), 0.3f);
				c(s.out.phase == Phase::Grabbed, tag + ": carrying");
				if (what == 0)
					s.in.gun_raised = false;
				else if (what == 1)
					s.in.armed = false;
				else
					s.in.menu = true;
				s.Poll();
				c(s.OnlyCancel(reason), tag + ": cancelled with its reason");
				c(s.out.phase == Phase::WaitOpen && s.out.owns_left_hand, tag + ": the grip is still the gesture's until it opens");
				// Back to normal and finish the move: nothing, the gesture is gone.
				s.in.gun_raised = true;
				s.in.armed = true;
				s.in.menu = false;
				s.MoveCarry(kBesideGun, 0.4f);
				s.Grip(0.0f, 0.1f);
				c(s.reloads == 0, tag + ": finishing the move does nothing");
				// A fresh gesture works.
				s.MoveCarry(kSpot, 0.5f);
				s.Grip(1.0f, 0.1f);
				s.MoveCarry(kBesideGun, 0.7f);
				s.Grip(0.0f, 0.1f);
				c(s.reloads == 1, tag + ": a fresh gesture reloads");
			}

			// Lowered just before the release.
			Sim late;
			late.TakeMagazine();
			late.MoveCarry(kBesideGun, 0.7f);
			late.in.gun_raised = false;
			late.Grip(0.0f, 0.1f);
			c(late.reloads == 0, "gun lowered: lowered before the release, no reload");

			// Nothing starts with the gun down.
			Sim down;
			down.in.gun_raised = false;
			down.MoveCarry(kSpot, 0.4f);
			c(down.out.phase == Phase::Idle, "gun lowered: no reach tick with the gun down");
			down.Grip(1.0f, 0.1f);
			c(down.out.phase == Phase::Idle && down.grabs == 0, "gun lowered: no grab with the gun down");
			// ...and raising it over a closed grip is not a grab.
			down.in.gun_raised = true;
			down.Poll();
			c(down.out.phase == Phase::Idle && down.grabs == 0, "gun lowered: raising the gun over a closed grip is no grab");
		}

		// 4. No repeat until the grip has opened.
		void TestRetrigger(Checker& c)
		{
			Sim s;
			s.TakeMagazine();
			s.MoveCarry(kBesideGun, 0.7f);
			c(s.out.phase == Phase::Ready, "retrigger: ready");
			s.Carry().trigger = 1.0f;
			s.Poll();
			c(s.reloads == 1, "retrigger: a trigger pull at the gun reloads");
			c(s.out.phase == Phase::WaitOpen, "retrigger: then waits for the grip to open");
			// Pull again and again, and carry the magazine round again with the grip held.
			for (int i = 0; i < 3; ++i)
			{
				s.Carry().trigger = 0.0f;
				s.Hold(0.1f);
				s.Carry().trigger = 1.0f;
				s.Hold(0.1f);
			}
			s.Carry().trigger = 0.0f;
			s.MoveCarry(kSpot, 0.5f);
			s.Grip(0.8f, 0.1f); // eases but stays closed
			s.Grip(1.0f, 0.1f);
			s.MoveCarry(kBesideGun, 0.6f);
			s.Carry().trigger = 1.0f;
			s.Hold(0.2f);
			c(s.reloads == 1, "retrigger: nothing repeats while the grip stays closed");
			c(s.out.phase == Phase::WaitOpen, "retrigger: still waiting");
			s.Carry().trigger = 0.0f;
			s.Grip(0.0f, 0.1f);
			c(s.out.phase == Phase::Idle && s.reloads == 1, "retrigger: the grip opening ends the wait without a reload");
			// A full new gesture after the grip opened.
			s.MoveCarry(kSpot, 0.5f);
			s.Grip(1.0f, 0.1f);
			s.MoveCarry(kBesideGun, 0.7f);
			s.Grip(0.0f, 0.1f);
			c(s.reloads == 2, "retrigger: a new gesture after the grip opened reloads again");

			// Held closed at the gun for good: dropped, and the late release does nothing.
			Sim hold;
			hold.TakeMagazine();
			hold.MoveCarry(kBesideGun, 0.7f);
			hold.Hold(3.0f);
			c(hold.reloads == 0 && !hold.cancels.empty() && hold.cancels.back() == kReasonHeldTooLong,
				"retrigger: a long hold at the gun is dropped");
			hold.Grip(0.0f, 0.1f);
			c(hold.reloads == 0, "retrigger: releasing after the long hold does nothing");

			// A trigger already held on arrival is not a pull.
			Sim held;
			held.Carry().trigger = 1.0f;
			held.TakeMagazine();
			held.MoveCarry(kBesideGun, 0.7f);
			held.Hold(0.2f);
			c(held.reloads == 0 && held.out.phase == Phase::Ready, "retrigger: a trigger held from before is no pull");
			held.Carry().trigger = 0.0f;
			held.Hold(0.1f);
			held.Carry().trigger = 1.0f;
			held.Poll();
			c(held.reloads == 1, "retrigger: let go and pull reloads");
			held.Grip(0.0f, 0.1f);
			c(held.reloads == 1, "retrigger: the later release does not reload again");

			// With fire_on_trigger off a pull does nothing; the release still reloads.
			Params quiet;
			quiet.fire_on_trigger = false;
			Sim mute(quiet);
			mute.TakeMagazine();
			mute.MoveCarry(kBesideGun, 0.7f);
			mute.Carry().trigger = 1.0f;
			mute.Hold(0.2f);
			c(mute.reloads == 0, "retrigger: fire_on_trigger off ignores the trigger");
			mute.Grip(0.0f, 0.1f);
			c(mute.reloads == 1, "retrigger: fire_on_trigger off still reloads on the release");
		}

		// 5. Grip noise near the thresholds.
		void TestNoise(Checker& c)
		{
			// Never reaching the squeeze threshold in the spot: no grab.
			Sim s;
			s.MoveCarry(kSpot, 0.4f);
			for (const float v : {0.45f, 0.58f, 0.5f, 0.55f, 0.59f, 0.52f, 0.57f, 0.3f, 0.58f})
			{
				s.Carry().squeeze = v;
				s.Poll();
			}
			c(s.grabs == 0 && s.out.phase == Phase::Reach, "noise: below the threshold nothing grabs");
			// A noisy rise across it is one grab.
			for (const float v : {0.62f, 0.57f, 0.61f, 0.59f, 0.63f, 0.58f, 0.6f})
			{
				s.Carry().squeeze = v;
				s.Poll();
			}
			c(s.grabs == 1 && s.out.phase == Phase::Grabbed, "noise: a noisy rise is one grab");
			// A noisy grip on the way, dipping toward but never to the release threshold, keeps the magazine.
			const float wobble[] = {0.45f, 0.9f, 0.5f, 0.85f, 0.42f, 0.7f};
			const Vec3 from = s.Carry().position;
			for (int k = 1; k <= 48; ++k)
			{
				s.Carry().position = Lerp(from, kBesideGun, static_cast<float>(k) / 48.0f);
				s.Carry().squeeze = wobble[k % 6];
				s.Poll();
			}
			c(s.cancels.empty() && s.out.phase == Phase::Ready, "noise: dips above the release threshold keep the magazine");
			// A noisy release is one reload.
			for (const float v : {0.45f, 0.38f, 0.5f, 0.36f, 0.45f, 0.58f, 0.2f, 0.55f, 0.3f, 0.0f})
			{
				s.Carry().squeeze = v;
				s.Poll();
			}
			c(s.reloads == 1, "noise: a noisy release is exactly one reload");
			c(s.out.phase == Phase::Idle, "noise: idle after the noisy release");
			s.Hold(0.5f);
			c(s.reloads == 1 && s.grabs == 1, "noise: nothing more");

			// A wobble around the middle of the band while waiting in the spot grabs nothing.
			Sim wob;
			wob.MoveCarry(kSpot, 0.4f);
			for (int k = 0; k < 120; ++k)
			{
				wob.Carry().squeeze = 0.5f + 0.09f * std::sin(static_cast<float>(k) * 1.7f);
				wob.Poll();
			}
			c(wob.grabs == 0, "noise: a wobble around 0.5 grabs nothing");

			// One poll over the threshold is a squeeze, but cannot become a reload: it is cancelled as it opens.
			Sim spike;
			spike.MoveCarry(kSpot, 0.4f);
			spike.Carry().squeeze = 0.65f;
			spike.Poll();
			spike.Carry().squeeze = 0.0f;
			spike.Poll();
			c(spike.grabs == 1 && spike.OnlyCancel(kReasonReleasedEarly) && spike.reloads == 0,
				"noise: a one-poll spike grabs and is cancelled, never a reload");
		}

		// 6. The gun hand moving while the other approaches.
		void TestGunMoves(Checker& c)
		{
			// The gun moves away before the magazine arrives where it was: not ready, and the release does nothing.
			Sim away;
			away.TakeMagazine();
			away.MoveGun({0.18f, 1.30f, -0.30f}, 0.2f);
			away.MoveCarry(kBesideGun, 0.6f);
			c(away.out.phase == Phase::Grabbed && away.readys == 0, "gun hand: the gun moved away, not ready");
			away.Grip(0.0f, 0.1f);
			c(away.reloads == 0, "gun hand: the release away from the gun does nothing");

			// Both hands drift toward each other and meet in the middle.
			Sim meet;
			meet.TakeMagazine();
			{
				const Vec3 l0 = meet.Carry().position, r0 = meet.Gun().position;
				const Vec3 mid{-0.02f, 0.95f, -0.35f};
				const Vec3 gun_end{mid.x + 0.06f, mid.y + 0.05f, mid.z - 0.05f};
				for (int k = 1; k <= 60; ++k)
				{
					meet.Carry().position = Lerp(l0, mid, static_cast<float>(k) / 60.0f);
					meet.Gun().position = Lerp(r0, gun_end, static_cast<float>(k) / 60.0f);
					meet.Poll();
				}
			}
			c(meet.out.phase == Phase::Ready, "gun hand: meeting in the middle is ready");
			meet.Grip(0.0f, 0.1f);
			c(meet.reloads == 1, "gun hand: and reloads");

			// Ready, then the gun swings away: back to Grabbed, and the release does nothing.
			Sim swing;
			swing.TakeMagazine();
			swing.MoveCarry(kBesideGun, 0.7f);
			c(swing.out.phase == Phase::Ready, "gun hand: ready before the swing");
			swing.MoveGun({0.45f, 1.25f, -0.50f}, 0.3f);
			c(swing.out.phase == Phase::Grabbed, "gun hand: the gun swung away drops back to Grabbed");
			swing.Grip(0.0f, 0.1f);
			c(swing.reloads == 0, "gun hand: releasing while the gun is away does nothing");

			// Jitter around the edge of the gun's reach, inside the exit radius, does not flap the phase.
			Sim edge;
			edge.TakeMagazine();
			const Vec3 well{kGun.x, kGun.y - Params{}.gun_drop, kGun.z};
			edge.MoveCarry({well.x + 0.14f, well.y, well.z}, 0.7f);
			c(edge.out.phase == Phase::Ready, "gun hand: at the edge of its reach is ready");
			const std::size_t changes = edge.phases.size();
			for (int k = 0; k < 90; ++k)
			{
				const float d = 0.14f + 0.02f * std::sin(static_cast<float>(k) * 2.3f); // 0.12-0.16, exit radius is 0.19
				edge.Carry().position = {well.x + d, well.y, well.z};
				edge.Poll();
			}
			c(edge.phases.size() == changes && edge.out.phase == Phase::Ready, "gun hand: jitter on the edge does not flap");

			// The gun hand moving about all the while, the magazine hand following it in.
			Sim follow;
			follow.TakeMagazine();
			{
				const Vec3 a = follow.Carry().position;
				for (int k = 1; k <= 60; ++k)
				{
					const float ph = static_cast<float>(k) / 60.0f;
					follow.Gun().position = {kGun.x + 0.05f * std::sin(ph * 6.0f), kGun.y + 0.04f * std::sin(ph * 4.0f), kGun.z};
					const Vec3 target{follow.Gun().position.x - 0.05f, follow.Gun().position.y - 0.06f, follow.Gun().position.z + 0.05f};
					follow.Carry().position = Lerp(a, target, std::min(1.0f, ph * 1.3f));
					follow.Poll();
				}
			}
			c(follow.out.phase == Phase::Ready, "gun hand: following a moving gun gets ready");
			follow.Grip(0.0f, 0.1f);
			c(follow.reloads == 1, "gun hand: and reloads");
		}

		// 7. Tracking loss.
		void TestTrackingLoss(Checker& c)
		{
			// A long loss of the magazine hand mid-carry cancels.
			Sim s;
			s.TakeMagazine();
			s.MoveCarry(Lerp(kSpot, kBesideGun, 0.4f), 0.3f);
			s.Carry().valid = false;
			s.Hold(0.5f);
			c(s.OnlyCancel(kReasonLost), "loss: a long loss of the magazine hand cancels");
			s.Carry().valid = true;
			s.Carry().position = kBesideGun;
			s.Hold(0.2f);
			s.Grip(0.0f, 0.1f);
			c(s.reloads == 0, "loss: recovering and releasing at the gun does nothing");

			// A short one is waited out.
			Sim brief;
			brief.TakeMagazine();
			brief.MoveCarry(Lerp(kSpot, kBesideGun, 0.4f), 0.3f);
			brief.Carry().valid = false;
			brief.Hold(0.1f);
			brief.Carry().valid = true;
			brief.MoveCarry(kBesideGun, 0.4f);
			c(brief.out.phase == Phase::Ready && brief.cancels.empty(), "loss: a 0.1 s dropout is waited out");
			brief.Grip(0.0f, 0.1f);
			c(brief.reloads == 1, "loss: and the reload still happens");

			// A release while the pose is missing is not trusted.
			Sim blind;
			blind.TakeMagazine();
			blind.MoveCarry(kBesideGun, 0.7f);
			c(blind.out.phase == Phase::Ready, "loss: ready before the blackout");
			blind.Carry().valid = false;
			blind.Grip(0.0f, 0.1f);
			c(blind.reloads == 0 && blind.OnlyCancel(kReasonReleasedLost), "loss: a release while the pose is missing does nothing");
			blind.Carry().valid = true;
			blind.Hold(0.3f);
			c(blind.reloads == 0 && blind.out.phase == Phase::Idle, "loss: idle after");

			// The gun hand lost for long while ready.
			Sim gun;
			gun.TakeMagazine();
			gun.MoveCarry(kBesideGun, 0.7f);
			gun.Gun().valid = false;
			gun.Hold(0.5f);
			c(gun.OnlyCancel(kReasonLost), "loss: the gun hand lost for long cancels");
			gun.Gun().valid = true;
			gun.Grip(0.0f, 0.1f);
			c(gun.reloads == 0, "loss: no reload after");

			// A pose with NaNs is a lost pose.
			Sim lost_nan;
			lost_nan.TakeMagazine();
			lost_nan.MoveCarry(kBesideGun, 0.7f);
			lost_nan.Carry().position = {std::numeric_limits<float>::quiet_NaN(), 1.0f, 0.0f};
			lost_nan.Hold(0.5f);
			c(lost_nan.OnlyCancel(kReasonLost), "loss: a NaN position is a lost pose");

			// A grip that closed while the pose was missing is not a grab when it comes back.
			Sim closed;
			closed.MoveCarry(kSpot, 0.4f);
			closed.Carry().valid = false;
			closed.Grip(1.0f, 0.1f);
			closed.Carry().valid = true;
			closed.Hold(0.2f);
			c(closed.grabs == 0, "loss: a grip that closed while the pose was missing is not a grab");

			// No grab while the gun hand's pose is missing.
			Sim nogun;
			nogun.MoveCarry(kSpot, 0.4f);
			nogun.Gun().valid = false;
			nogun.Grip(1.0f, 0.1f);
			c(nogun.grabs == 0, "loss: no grab while the gun hand is lost");

			// A reach drops back to idle when the pose is lost.
			Sim reach;
			reach.MoveCarry(kSpot, 0.4f);
			c(reach.out.phase == Phase::Reach, "loss: reaching");
			reach.Carry().valid = false;
			reach.Poll();
			c(reach.out.phase == Phase::Idle, "loss: a reach ends when the pose is lost");
		}

		// 8. Edge cases around the spot.
		void TestSpot(Checker& c)
		{
			// A grip already closed on the way into the spot does nothing.
			Sim s;
			s.Carry().squeeze = 1.0f;
			s.Hold(0.1f);
			s.MoveCarry(kSpot, 0.4f);
			s.Hold(0.3f);
			c(s.grabs == 0 && s.out.phase == Phase::Idle, "spot: a grip closed on the way in does nothing");

			// A spot that overlaps the gun's reach: squeezing and letting go in place is not a reload, the hand has to leave.
			Sim overlap;
			overlap.Gun().position = {kSpot.x, kSpot.y + Params{}.gun_drop, kSpot.z};
			overlap.MoveCarry(kSpot, 0.4f);
			overlap.Grip(1.0f, 0.1f);
			overlap.Hold(0.2f);
			c(overlap.out.phase == Phase::Grabbed, "spot: overlapping the gun's reach, not ready until the hand leaves");
			overlap.Grip(0.0f, 0.1f);
			c(overlap.reloads == 0, "spot: squeezing and letting go in the spot does not reload");

			// No spot given (radius 0): nothing can be grabbed.
			Sim none;
			none.in.spot_radius = 0.0f;
			none.MoveCarry(kSpot, 0.4f);
			none.Grip(1.0f, 0.1f);
			c(none.grabs == 0 && none.out.phase == Phase::Idle, "spot: no spot, no grab");

			// Jitter around the spot's edge with an open hand is one tick, not a chatter.
			Sim edge;
			edge.MoveCarry(kSpot, 0.4f);
			edge.MoveCarry({kSpot.x + 0.11f, kSpot.y, kSpot.z}, 0.2f);
			const std::size_t changes = edge.phases.size();
			c(edge.out.phase == Phase::Reach, "spot: in the spot at its edge");
			for (int k = 0; k < 90; ++k)
			{
				const float d = 0.11f + 0.03f * std::sin(static_cast<float>(k) * 2.9f); // 0.08-0.14: past the radius, inside the exit
				edge.Carry().position = {kSpot.x + d, kSpot.y, kSpot.z};
				edge.Poll();
			}
			c(edge.phases.size() == changes && edge.out.phase == Phase::Reach, "spot: jitter on the edge does not chatter");
			edge.Carry().position = {kSpot.x + 0.2f, kSpot.y, kSpot.z};
			edge.Poll();
			c(edge.out.phase == Phase::Idle, "spot: past the exit radius the reach ends");
		}
	}

	std::vector<std::string> SelfTest()
	{
		Checker c;
		TestClean(c);
		TestTooSlow(c);
		TestLowered(c);
		TestRetrigger(c);
		TestNoise(c);
		TestGunMoves(c);
		TestTrackingLoss(c);
		TestSpot(c);
		return c.failures;
	}
}
