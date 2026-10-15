// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/ReloadDriver.h"
#include "VR/CameraDriver.h"
#include "VR/GameFeedback.h"
#include "VR/HudCards.h"
#include "VR/ItemData.h"
#include "VR/ReloadGesture.h"
#include "VR/SplitState.h"
#include "VR/VRInput.h"
#include "VR/VRProfileDB.h"
#include "VR/ZoneMarkers.h"

#include "Config.h"
#include "Memory.h"
#include "MemoryTypes.h"
#include "VMManager.h"

#include "common/Console.h"

#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace VR::ReloadDriver
{
	namespace
	{
		float FiniteOr(float v, float fallback)
		{
			return std::isfinite(v) ? v : fallback;
		}

		// Whatever the caller passed, kept to ranges the driver works in.
		Params Sanitized(const Params& in)
		{
			const Params d{};
			Params p;
			p.lower_min_s = std::clamp(FiniteOr(in.lower_min_s, d.lower_min_s), 0.0f, 1.0f);
			p.lower_max_s = std::clamp(FiniteOr(in.lower_max_s, d.lower_max_s), p.lower_min_s, 2.0f);
			p.stall_s = std::clamp(FiniteOr(in.stall_s, d.stall_s), 0.1f, 5.0f);
			p.start_s = std::clamp(FiniteOr(in.start_s, d.start_s), 0.05f, 5.0f);
			p.hold_max_s = std::clamp(FiniteOr(in.hold_max_s, d.hold_max_s), 0.2f, 20.0f);
			p.round_gap_s = std::clamp(FiniteOr(in.round_gap_s, d.round_gap_s), 0.0f, 5.0f);
			p.hold_ceiling_s = std::clamp(FiniteOr(in.hold_ceiling_s, d.hold_ceiling_s), p.hold_max_s, 30.0f);
			p.release_gap_s = std::clamp(FiniteOr(in.release_gap_s, d.release_gap_s), 0.0f, 1.0f);
			p.stick_dead = std::clamp(FiniteOr(in.stick_dead, d.stick_dead), 0.05f, 1.0f);
			return p;
		}

		// The toasts for a reload that could not run, short enough for the HUD font's biggest sizes.
		constexpr const char* kToastNoAmmo = "No spare ammo";
		constexpr const char* kToastFull = "Magazine full";
		constexpr const char* kToastWalking = "Stand still to reload";

		// Why a reload that loaded nothing could not run, for its toast. No ammo at all in the item cells, an empty gun, or a
		// gun below the most rounds it has been seen holding (so it had room, and the ammo for it was missing) is "No spare
		// ammo"; a gun at its most is taken as full. The magazine's size is not in the item record (research 2a), so the
		// most seen this session stands in for it.
		const char* NothingLoadedToast(bool no_spare, u32 rounds, u32 most_seen)
		{
			if (no_spare || rounds == 0 || rounds < most_seen)
				return kToastNoAmmo;
			return kToastFull;
		}
	}

	const char* PhaseName(Phase phase)
	{
		switch (phase)
		{
			case Phase::Idle:
				return "Idle";
			case Phase::Lowering:
				return "Lowering";
			case Phase::Holding:
				return "Holding";
			case Phase::Releasing:
				return "Releasing";
		}
		return "?";
	}

	const char* ResultName(Result result)
	{
		switch (result)
		{
			case Result::None:
				return "none";
			case Result::Reloaded:
				return "reloaded";
			case Result::NothingLoaded:
				return "nothing loaded";
			case Result::NoSpareAmmo:
				return "no spare ammo";
			case Result::NotLowered:
				return "the weapon stayed raised";
			case Result::Moved:
				return "you moved";
			case Result::Screen:
				return "a game screen opened";
			case Result::Lost:
				return "first person or the gun went away";
			case Result::Raised:
				return "you raised the weapon (R1)";
		}
		return "?";
	}

	Out Step(const In& in, const Params& params, State& st)
	{
		Out out;
		const Params p = Sanitized(params);
		const float dt = std::clamp(FiniteOr(in.dt, 0.0f), 0.0f, 0.25f);
		const float stick = std::clamp(FiniteOr(in.stick, 0.0f), 0.0f, 1.0f);

		// An attempt in flight is over: L1 goes up now, R1 comes back release_gap_s later.
		const auto finish = [&](Result result) {
			out.finished = true;
			out.result = result;
			out.from = st.from;
			out.to = st.last;
			out.seconds = (st.last > st.from) ? st.last_round_s : st.elapsed_s;
			// Into a screen, or with first person gone, R1 held through the end must not come back on its own (a1).
			st.release_first = (result == Result::Screen || result == Result::Lost);
			st.phase = Phase::Releasing;
			st.phase_s = 0.0f;
		};

		switch (st.phase)
		{
			case Phase::Idle:
			{
				// A request needs everything in place; one that arrives without (a screen opening the same poll) is dropped,
				// as the gesture that made it is already over.
				if (!in.request || !in.usable || in.screen)
					break;
				// The game reloads only standing still, so with the player walking nothing is tried: holding R1 back would
				// only drop the aim for a moment.
				const bool walking = stick > p.stick_dead;
				if (walking || in.no_spare)
				{
					out.finished = true;
					out.result = walking ? Result::Moved : Result::NoSpareAmmo;
					out.from = in.ammo;
					out.to = in.ammo;
					break;
				}
				st.Reset();
				st.phase = Phase::Lowering;
				st.weapon = in.weapon;
				st.from = in.ammo;
				st.last = in.ammo;
				out.started = true;
				break;
			}
			case Phase::Lowering:
			case Phase::Holding:
			{
				st.phase_s += dt;
				st.elapsed_s += dt;
				const bool same_gun = in.usable && in.weapon == st.weapon;
				// The rounds first, so a round that arrives on the poll something else ends the hold still counts.
				if (same_gun && st.phase == Phase::Holding)
				{
					st.since_round_s += dt;
					if (in.ammo > st.last)
					{
						out.round = true;
						st.last = in.ammo;
						st.since_round_s = 0.0f;
						st.last_round_s = st.elapsed_s;
					}
					if (in.action_known && in.reloading)
						st.saw_action = true;
				}
				if (!same_gun)
				{
					finish(Result::Lost);
				}
				else if (in.screen)
				{
					finish(Result::Screen);
				}
				else if (in.raise_pressed)
				{
					finish(Result::Raised); // the player's own R1, pressed afresh: they want the weapon up (a4)
				}
				else if (stick > p.stick_dead)
				{
					finish(Result::Moved);
				}
				else if (st.phase == Phase::Lowering)
				{
					if (!in.raised && st.phase_s >= p.lower_min_s)
					{
						st.phase = Phase::Holding;
						st.phase_s = 0.0f;
						st.since_round_s = 0.0f;
					}
					else if (st.phase_s >= p.lower_max_s)
					{
						finish(Result::NotLowered);
					}
				}
				else
				{
					const bool loaded = st.last > st.from;
					const Result done = loaded ? Result::Reloaded : Result::NothingLoaded;
					const bool still_loading = loaded && st.since_round_s < p.round_gap_s;
					if (in.action_known && st.saw_action && !in.reloading)
						finish(done); // the game's own reload is over
					else if (in.action_known && !st.saw_action && !loaded && st.phase_s >= p.start_s)
						finish(Result::NothingLoaded); // the game never started one: full, or no ammo for this gun
					else if (st.since_round_s >= p.stall_s)
						finish(done);
					else if (st.phase_s >= p.hold_ceiling_s || (st.phase_s >= p.hold_max_s && !still_loading))
						finish(done);
				}
				break;
			}
			case Phase::Releasing:
				st.phase_s += dt;
				if (st.phase_s >= p.release_gap_s)
				{
					st.phase = Phase::Idle;
					st.phase_s = 0.0f;
				}
				break;
		}

		out.phase = st.phase;
		out.suppress_raise = st.phase != Phase::Idle;
		out.hold_reload = st.phase == Phase::Holding;
		out.mask_reload = st.phase != Phase::Idle;
		out.raise_release_first = st.phase == Phase::Releasing && st.release_first;
		return out;
	}

	bool StanceHold::Step(bool raised_now, float dt, float hold_s)
	{
		if (raised_now)
		{
			since_s = 0.0f;
			return true;
		}
		since_s = std::min(since_s + std::clamp(FiniteOr(dt, 0.0f), 0.0f, 0.25f), 1.0e9f);
		return since_s < hold_s;
	}

	float ReloadButton(float own, bool hold, bool mask, bool& latched)
	{
		const bool pressed = std::isfinite(own) && own >= 0.5f;
		if (hold || mask)
			latched = pressed;
		else if (!pressed)
			latched = false;
		if (hold)
			return 1.0f;
		if (mask || latched)
			return 0.0f;
		return own;
	}

	float RaiseButton(float own, bool suppress, bool release_first, RaiseLatch& latch, bool* fresh_press)
	{
		const bool pressed = std::isfinite(own) && own >= 0.5f;
		if (suppress)
		{
			if (fresh_press && latch.was_suppressed && pressed && !latch.prev)
				*fresh_press = true;
			latch.was_suppressed = true;
			latch.prev = pressed;
			latch.latched = release_first && pressed;
			return 0.0f;
		}
		latch.was_suppressed = false;
		latch.prev = pressed;
		if (!pressed)
			latch.latched = false;
		return latch.latched ? 0.0f : own;
	}

	SpatialControls::Hands HideHand(const SpatialControls::Hands& hands, int hand)
	{
		// Out of every zone's and pouch's reach, as BeltPouches puts a hand it claims.
		constexpr float kFarAway = 1.0e4f;
		SpatialControls::Hands out = hands;
		if (hand == VRInputSnapshot::LEFT || hand == VRInputSnapshot::RIGHT)
			out.hand[hand].position = {kFarAway, kFarAway, kFarAway};
		return out;
	}

	VRInputSnapshot HideHand(const VRInputSnapshot& snap, int hand)
	{
		constexpr float kFarAway = 1.0e4f;
		VRInputSnapshot out = snap;
		if (hand == VRInputSnapshot::LEFT || hand == VRInputSnapshot::RIGHT)
		{
			for (float& v : out.hands[hand].grip_pose.position_xyz)
				v = kFarAway;
		}
		return out;
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

		// The game's side of a reload as research section 2a measured it, polled at 60 Hz: the stance leaves raised a
		// couple of frames after R1 goes up; L1 with the weapon lowered, standing, room in the gun and ammo for it starts a
		// reload; File #1 loads one round 0.33 s after the press and one every 0.33 s while L1 is held (a tap still gets
		// the first), and its action byte reads reload until 1 s after the last; File #2 loads the whole magazine 0.1 s in
		// and reads reload for 2 s.
		struct FakeGame
		{
			bool whole = false;       // File #2
			bool action_known = true; // the profile reads the action byte (File #1's does)
			u32 ammo = 3;
			u32 cap = 7;
			u32 spare = 14;
			u32 weapon = 87;
			float lower_s = 0.034f;
			// The player: R1 held through the reload (the bumper or the holster grip), R1 held on another pad, the move
			// stick, a game screen, first person and the gun there.
			bool hold_r1 = true;
			bool other_r1 = false;
			float stick = 0.0f;
			bool screen = false;
			bool usable = true;
			// The game.
			bool raised = true;
			float r1_up_s = 0.0f;
			bool loading = false;
			float load_s = 0.0f;
			float next_s = 0.0f;
			float action_s = 0.0f;

			bool Action() const { return loading || action_s > 0.0f; }

			void Tick(bool r1, bool l1, float dt)
			{
				if (r1 || other_r1)
				{
					r1_up_s = 0.0f;
					raised = true;
				}
				else
				{
					r1_up_s += dt;
					if (r1_up_s >= lower_s)
						raised = false;
				}
				action_s = std::max(0.0f, action_s - dt);
				if (!loading)
				{
					if (l1 && !raised && stick < 0.2f && action_s <= 0.0f && ammo < cap && spare > 0)
					{
						loading = true;
						load_s = 0.0f;
						next_s = whole ? 0.10f : 0.33f;
					}
					return;
				}
				load_s += dt;
				if (load_s < next_s)
					return;
				const u32 n = whole ? std::min(cap - ammo, spare) : 1u;
				ammo += n;
				spare -= n;
				if (whole || ammo >= cap || spare == 0 || !l1)
				{
					loading = false;
					action_s = whole ? 1.95f : 1.0f;
				}
				else
				{
					next_s += 0.33f;
				}
			}
		};

		// The driver and the fake game together, with what happened along the way.
		struct Run
		{
			FakeGame g;
			Params p;
			State st;
			Out out;
			bool no_spare = false;
			int polls = 0;
			int starts = 0;
			int finishes = 0;
			Result result = Result::None;
			u32 from = 0;
			u32 to = 0;
			float seconds = 0.0f;
			int rounds = 0;
			bool l1_while_raised = false; // L1 held while the game's stance read raised
			bool l1_with_r1 = false;      // L1 held while R1 reached the game
			bool ever_suppressed = false;
			bool ever_held = false;
			int last_hold_poll = -1;
			int finish_poll = -1;
			int r1_back_poll = -1; // the first poll after the end with R1 no longer held back
			RaiseLatch r1_latch;   // the player's R1 as VRInputSource sends it (RaiseButton)
			bool r1_fresh = false; // a fresh R1 press under the hold-back, for the next Step
			bool r1_sent = false;  // R1 reached the game this poll

			void Poll(bool request = false, float dt = 1.0f / 60.0f)
			{
				In in;
				in.dt = dt;
				in.request = request;
				in.usable = g.usable;
				in.weapon = g.weapon;
				in.screen = g.screen;
				in.raised = g.raised;
				in.ammo = g.ammo;
				in.action_known = g.action_known;
				in.reloading = g.action_known && g.Action();
				in.stick = g.stick;
				in.no_spare = no_spare;
				in.raise_pressed = r1_fresh;
				r1_fresh = false;
				out = Step(in, p, st);
				const bool r1 = RaiseButton(g.hold_r1 ? 1.0f : 0.0f, out.suppress_raise, out.raise_release_first, r1_latch,
				                    &r1_fresh) >= 0.5f;
				r1_sent = r1;
				if (out.hold_reload && g.raised)
					l1_while_raised = true;
				if (out.hold_reload && (r1 || g.other_r1))
					l1_with_r1 = true;
				ever_suppressed = ever_suppressed || out.suppress_raise;
				ever_held = ever_held || out.hold_reload;
				if (out.hold_reload)
					last_hold_poll = polls;
				starts += out.started ? 1 : 0;
				rounds += out.round ? 1 : 0;
				if (out.finished)
				{
					finishes++;
					finish_poll = polls;
					result = out.result;
					from = out.from;
					to = out.to;
					seconds = out.seconds;
					r1_back_poll = -1;
				}
				if (finish_poll >= 0 && r1_back_poll < 0 && !out.suppress_raise)
					r1_back_poll = polls;
				g.Tick(r1, out.hold_reload, dt);
				polls++;
			}

			void Hold(float secs)
			{
				for (int i = 0, n = std::max(1, static_cast<int>(std::lround(secs * 60.0f))); i < n; ++i)
					Poll();
			}

			// A request, then polls until the driver is idle again (at most max_s).
			void Attempt(float max_s = 9.0f)
			{
				Poll(true);
				for (int i = 0; i < static_cast<int>(max_s * 60.0f) && st.phase != Phase::Idle; ++i)
					Poll();
			}

			bool Near(float a, float b, float tol) const { return std::fabs(a - b) <= tol; }
		};

		// The rules every finished attempt keeps: L1 never with the weapon raised or R1 reaching the game, and R1 back
		// only after L1 has been up for the release gap.
		void CheckOrder(Checker& c, const Run& r, const std::string& tag)
		{
			c(!r.l1_while_raised, tag + ": L1 never held with the weapon raised");
			c(!r.l1_with_r1, tag + ": L1 never held while R1 reached the game");
			c(r.st.phase == Phase::Idle && !r.out.suppress_raise && !r.out.hold_reload, tag + ": idle after, R1 given back");
			if (r.ever_held)
				c(r.r1_back_poll - r.last_hold_poll >= 3, tag + ": R1 back only after L1 has been up 0.05 s");
		}

		void TestFile1(Checker& c)
		{
			// 3 of 7 with spare rounds, the action byte read: the example the log line describes.
			Run r;
			r.Attempt();
			c(r.starts == 1 && r.finishes == 1, "file1: one attempt");
			c(r.result == Result::Reloaded && r.from == 3 && r.to == 7 && r.rounds == 4 && r.g.ammo == 7,
				"file1: 3 -> 7, four rounds");
			c(r.Near(r.seconds, 1.4f, 0.1f), "file1: the last round about 1.4 s after the request");
			c(r.g.spare == 10, "file1: four rounds from the spare");
			CheckOrder(c, r, "file1");
			// R1 still held by the player: the weapon comes back up by itself.
			r.Hold(0.2f);
			c(r.g.raised, "file1: the held bumper raises the weapon again");
			// The end came from the rounds stopping (0.8 s) or the action byte, not a cap.
			c(r.finish_poll <= static_cast<int>((1.4f + 0.85f) * 60.0f), "file1: let go soon after the last round");

			// The same without the action byte (the stall alone ends it).
			Run quiet;
			quiet.g.action_known = false;
			quiet.Attempt();
			c(quiet.result == Result::Reloaded && quiet.to == 7 && quiet.rounds == 4, "file1 (no action byte): 3 -> 7");
			CheckOrder(c, quiet, "file1 (no action byte)");

			// R1 let go during the reload: nothing comes back up.
			Run let_go;
			let_go.Poll(true);
			let_go.g.hold_r1 = false;
			for (int i = 0; i < 9 * 60 && let_go.st.phase != Phase::Idle; ++i)
				let_go.Poll();
			let_go.Hold(0.2f);
			c(let_go.result == Result::Reloaded && !let_go.g.raised, "file1: R1 let go meanwhile stays down after");

			// A 15-round Handgun from empty: still loading at 4 s, so the hold goes on to the end (4.95 s of rounds).
			Run hg;
			hg.g.ammo = 0;
			hg.g.cap = 15;
			hg.g.spare = 30;
			hg.Attempt();
			c(hg.result == Result::Reloaded && hg.from == 0 && hg.to == 15 && hg.rounds == 15, "file1: Handgun 0 -> 15 past 4 s");
			CheckOrder(c, hg, "file1 handgun");

			// Rounds that never stop: the ceiling ends it at 6 s.
			Run lots;
			lots.g.ammo = 0;
			lots.g.cap = 99;
			lots.g.spare = 99;
			lots.Attempt();
			const float held_s = static_cast<float>(lots.finish_poll) / 60.0f;
			c(lots.result == Result::Reloaded && held_s > 5.9f && held_s < 6.2f, "file1: the 6 s ceiling");
			CheckOrder(c, lots, "file1 ceiling");
		}

		void TestFile2(Checker& c)
		{
			// The whole magazine at once, no action byte in the profile.
			Run r;
			r.g.whole = true;
			r.g.action_known = false;
			r.g.spare = 7;
			r.Attempt();
			c(r.result == Result::Reloaded && r.from == 3 && r.to == 7 && r.rounds == 1 && r.g.spare == 3, "file2: 3 -> 7 at once");
			c(r.seconds < 0.3f, "file2: loaded within 0.3 s of the request");
			c(r.finish_poll <= static_cast<int>((0.3f + 0.85f) * 60.0f), "file2: let go 0.8 s after the magazine went in");
			CheckOrder(c, r, "file2");

			// A partial spare: the four left go in.
			Run part;
			part.g.whole = true;
			part.g.action_known = false;
			part.g.ammo = 0;
			part.g.spare = 4;
			part.Attempt();
			c(part.result == Result::Reloaded && part.to == 4 && part.g.spare == 0, "file2: a partial spare loads what is left");
		}

		void TestCannotRun(Checker& c)
		{
			// A full magazine with the action byte read: given up at start_s, nothing pressed after.
			Run full;
			full.g.ammo = 7;
			full.Attempt();
			c(full.result == Result::NothingLoaded && full.rounds == 0 && full.g.ammo == 7, "full: nothing loaded");
			c(full.finish_poll <= static_cast<int>((0.05f + 0.35f) * 60.0f) + 3, "full: given up at once with the action byte");
			CheckOrder(c, full, "full");
			c(std::string(NothingLoadedToast(false, 7, 7)) == kToastFull, "full: the toast says full");

			// The same without the action byte: after the stall.
			Run full2;
			full2.g.ammo = 7;
			full2.g.action_known = false;
			full2.Attempt();
			c(full2.result == Result::NothingLoaded && full2.finish_poll >= static_cast<int>(0.8f * 60.0f) &&
				  full2.finish_poll <= static_cast<int>(0.95f * 60.0f),
				"full (no action byte): nothing loaded after 0.8 s");

			// No ammo for it in the game (the cells not known to be empty): nothing loaded, and the toast says ammo.
			Run dry;
			dry.g.spare = 0;
			dry.Attempt();
			c(dry.result == Result::NothingLoaded && dry.g.ammo == 3, "no ammo: nothing loaded");
			c(std::string(NothingLoadedToast(false, 3, 7)) == kToastNoAmmo, "no ammo: below its most, the toast says ammo");
			c(std::string(NothingLoadedToast(true, 7, 7)) == kToastNoAmmo, "no ammo: empty cells say ammo even at its most");
			c(std::string(NothingLoadedToast(false, 0, 0)) == kToastNoAmmo, "no ammo: an empty gun is never full");
			c(std::string(NothingLoadedToast(false, 7, 0)) == kToastFull, "full: never seen higher counts as full");

			// The cells known to hold no ammo: not tried at all, the weapon never lowered.
			Run none;
			none.no_spare = true;
			none.Poll(true);
			c(none.finishes == 1 && none.result == Result::NoSpareAmmo && none.starts == 0, "no spare: not tried");
			c(!none.ever_suppressed && !none.ever_held, "no spare: R1 never held back, L1 never pressed");
			none.Hold(0.2f);
			c(none.g.raised && none.st.phase == Phase::Idle, "no spare: the weapon stays up");

			// R1 held on another pad: the stance never leaves raised, so L1 is never pressed.
			Run other;
			other.g.other_r1 = true;
			other.Attempt();
			c(other.result == Result::NotLowered && !other.ever_held, "not lowered: given up without L1");
			c(other.finish_poll >= static_cast<int>(0.3f * 60.0f) - 1 && other.finish_poll <= static_cast<int>(0.3f * 60.0f) + 2,
				"not lowered: after 0.3 s");
			CheckOrder(c, other, "not lowered");
		}

		void TestStops(Checker& c)
		{
			// Walking when the request comes: not tried, the aim never dropped.
			Run walk;
			walk.g.stick = 0.6f;
			walk.Poll(true);
			c(walk.result == Result::Moved && walk.starts == 0 && !walk.ever_suppressed, "walking: not tried");

			// Walking mid-hold: L1 up at once, the rounds so far kept.
			Run moved;
			moved.Poll(true);
			moved.Hold(0.5f);
			moved.g.stick = 0.6f;
			moved.Poll();
			c(moved.result == Result::Moved && moved.to == 4 && !moved.out.hold_reload && moved.out.suppress_raise,
				"moved: L1 up at once, one round kept");
			for (int i = 0; i < 30 && moved.st.phase != Phase::Idle; ++i)
				moved.Poll();
			CheckOrder(c, moved, "moved");

			// A screen opening mid-hold.
			Run screen;
			screen.Poll(true);
			screen.Hold(0.2f);
			screen.g.screen = true;
			screen.Poll();
			c(screen.result == Result::Screen && !screen.out.hold_reload, "screen: L1 up at once");
			for (int i = 0; i < 30 && screen.st.phase != Phase::Idle; ++i)
				screen.Poll();
			CheckOrder(c, screen, "screen");
			// R1 held through it does not come back into the screen (review a1): only after a let-go and a fresh press.
			screen.Hold(0.5f);
			c(!screen.r1_sent && screen.g.hold_r1, "screen: R1 held through the end stays back (release-first)");
			screen.g.hold_r1 = false;
			screen.Poll();
			screen.g.hold_r1 = true;
			screen.Poll();
			c(screen.r1_sent, "screen: R1 let go and pressed again passes");
			// The same for first person going away.
			Run gone;
			gone.Poll(true);
			gone.Hold(0.6f);
			gone.g.usable = false;
			gone.Poll();
			gone.Hold(0.3f);
			c(gone.result == Result::Lost && !gone.r1_sent, "lost: R1 held through the end stays back");
			// A reload that ran its course gives R1 back as held: the weapon comes up again.
			Run done;
			done.Attempt();
			done.Poll();
			c(done.result == Result::Reloaded && done.r1_sent, "reloaded: R1 held through comes back");

			// A fresh R1 press mid-attempt ends it (review a4); R1 held from before the hold-back is no fresh press.
			Run abort;
			abort.Poll(true);
			abort.Hold(0.5f);
			c(abort.finishes == 0, "abort: R1 held from the start is not a press");
			abort.g.hold_r1 = false;
			abort.Poll();
			abort.g.hold_r1 = true;
			abort.Poll();
			abort.Poll();
			c(abort.result == Result::Raised && !abort.out.hold_reload && abort.out.suppress_raise &&
				  !abort.out.raise_release_first,
				"abort: a fresh R1 ends it, L1 up at once");
			for (int i = 0; i < 30 && abort.st.phase != Phase::Idle; ++i)
				abort.Poll();
			CheckOrder(c, abort, "abort");
			abort.Poll();
			c(abort.r1_sent, "abort: R1 comes back");
			{
				RaiseLatch l;
				bool fresh = false;
				c(RaiseButton(1.0f, false, false, l, &fresh) == 1.0f && !fresh, "raise: passes when not held back");
				c(RaiseButton(1.0f, true, false, l, &fresh) == 0.0f && !fresh, "raise: held back, held from before");
				c(RaiseButton(0.0f, true, false, l, &fresh) == 0.0f && !fresh, "raise: let go under the hold-back");
				c(RaiseButton(1.0f, true, false, l, &fresh) == 0.0f && fresh, "raise: a fresh press is seen");
				fresh = false;
				RaiseLatch n;
				c(RaiseButton(1.0f, true, false, n, &fresh) == 0.0f && !fresh, "raise: a press at the hold-back's start is not fresh");
				c(RaiseButton(1.0f, true, true, n, nullptr) == 0.0f, "raise: release-first armed");
				c(RaiseButton(1.0f, false, false, n, nullptr) == 0.0f, "raise: held after stays back");
				c(RaiseButton(0.0f, false, false, n, nullptr) == 0.0f && RaiseButton(1.0f, false, false, n, nullptr) == 1.0f,
					"raise: let go then pressed passes");
				const float nan = std::numeric_limits<float>::quiet_NaN();
				RaiseLatch z;
				c(RaiseButton(nan, true, true, z, &fresh) == 0.0f && !z.latched, "raise: NaN is not a press");
			}


			// First person going away, and the gun changing.
			Run lost;
			lost.Poll(true);
			lost.Hold(0.6f);
			lost.g.usable = false;
			lost.Poll();
			c(lost.result == Result::Lost && !lost.out.hold_reload, "lost: L1 up at once");
			Run swap;
			swap.Poll(true);
			swap.Hold(0.6f);
			swap.g.weapon = 86;
			swap.Poll();
			c(swap.result == Result::Lost, "lost: another gun equipped");

			// Requests that cannot start, and requests while one runs.
			Run busy;
			busy.Poll(true);
			busy.Hold(0.3f);
			busy.Poll(true);
			busy.Hold(0.3f);
			busy.Poll(true);
			for (int i = 0; i < 9 * 60 && busy.st.phase != Phase::Idle; ++i)
				busy.Poll(i % 7 == 0);
			c(busy.starts == 1 && busy.finishes == 1 && busy.to == 7, "busy: a request while one runs is ignored");
			Run gated;
			gated.g.usable = false;
			gated.Poll(true);
			gated.g.usable = true;
			gated.g.screen = true;
			gated.Poll(true);
			c(gated.starts == 0 && gated.finishes == 0 && !gated.ever_suppressed, "gated: no start without first person or with a screen");

			// A stall is not a timeout, nonsense params are clamped.
			Run stall;
			stall.Poll(true);
			stall.Hold(0.1f);
			stall.Poll(false, 10.0f);
			c(stall.st.phase == Phase::Holding && stall.finishes == 0, "stall: a 10 s gap is not a timeout");
			Run junk;
			const float nan = std::numeric_limits<float>::quiet_NaN();
			junk.p.lower_min_s = nan;
			junk.p.lower_max_s = -4.0f;
			junk.p.stall_s = nan;
			junk.p.start_s = nan;
			junk.p.hold_max_s = nan;
			junk.p.round_gap_s = nan;
			junk.p.hold_ceiling_s = 0.0f;
			junk.p.release_gap_s = nan;
			junk.p.stick_dead = nan;
			junk.Attempt();
			c(junk.result == Result::Reloaded && junk.to == 7, "junk params: still reloads");
			CheckOrder(c, junk, "junk params");
		}

		void TestHelpers(Checker& c)
		{
			StanceHold s;
			c(!s.Step(false, 1.0f / 60.0f), "stance: down from the start");
			c(s.Step(true, 1.0f / 60.0f), "stance: raised now");
			bool all = true;
			for (int i = 0; i < 15; ++i) // 0.25 s
				all = s.Step(false, 1.0f / 60.0f) && all;
			c(all, "stance: still raised 0.25 s after a flicker");
			c(s.Step(true, 1.0f / 60.0f), "stance: back up");
			bool down = false;
			for (int i = 0; i < 21; ++i) // 0.35 s
				down = !s.Step(false, 1.0f / 60.0f);
			c(down, "stance: down 0.35 s after");

			bool latched = false;
			c(ReloadButton(0.0f, true, false, latched) == 1.0f, "button: the driver's hold presses");
			c(ReloadButton(1.0f, false, true, latched) == 0.0f, "button: held back while masked");
			c(ReloadButton(1.0f, false, false, latched) == 0.0f, "button: still held from before stays back");
			c(ReloadButton(0.0f, false, false, latched) == 0.0f && !latched, "button: let go clears it");
			c(ReloadButton(1.0f, false, false, latched) == 1.0f, "button: a fresh press passes");
			bool clean = false;
			ReloadButton(0.0f, false, true, clean);
			c(ReloadButton(1.0f, false, false, clean) == 1.0f, "button: let go before the mask ended, a press passes at once");
			bool under = false;
			ReloadButton(1.0f, true, false, under);
			c(ReloadButton(1.0f, false, false, under) == 0.0f, "button: held under the driver's hold stays back after");

			SpatialControls::Hands h;
			h.hand[0].valid = true;
			h.hand[0].position = {1.0f, 2.0f, 3.0f};
			h.hand[0].squeeze = 0.9f;
			h.hand[1].position = {4.0f, 5.0f, 6.0f};
			const SpatialControls::Hands hid = HideHand(h, VRInputSnapshot::LEFT);
			c(hid.hand[0].position.x > 1000.0f && hid.hand[0].squeeze == 0.9f && hid.hand[0].valid &&
				  hid.hand[1].position.x == 4.0f,
				"hide: the hand far away, its grip kept, the other hand untouched");
			const SpatialControls::Hands kept = HideHand(h, -1);
			c(kept.hand[0].position.x == 1.0f, "hide: -1 hides nothing");
		}
	}

	std::vector<std::string> SelfTest()
	{
		Checker c;
		TestFile1(c);
		TestFile2(c);
		TestCannotRun(c);
		TestStops(c);
		TestHelpers(c);
		return c.failures;
	}

	void LogSelfTests()
	{
		const auto report = [](const char* what, const std::vector<std::string>& failures) {
			if (failures.empty())
			{
				Console.WriteLn("(VR) %s self-test: passed.", what);
				return;
			}
			std::string list;
			for (size_t i = 0; i < failures.size() && i < 8; ++i)
				list += (i ? "; " : "") + failures[i];
			Console.Error("(VR) %s self-test FAIL: %zu check(s): %s%s", what, failures.size(), list.c_str(),
				failures.size() > 8 ? "; ..." : "");
		};
		report("Reload gesture", ReloadGesture::SelfTest());
		report("Reload driver", SelfTest());
	}

	// ---------------------------------------------------------------------------------------------------------------------
	// The glue.
	// ---------------------------------------------------------------------------------------------------------------------
	namespace
	{
		// The magazine hand's cues: a short tick as the magazine is taken, a firmer one at the gun (the zones' entry tick
		// already marks the reach to the hip). Times the Haptic Strength setting.
		constexpr SpatialControls::HapticCue kTickGrabbed{0.40f, 0.02f};
		constexpr SpatialControls::HapticCue kTickReady{0.80f, 0.04f};
		// The first-raise hint's two lines, each this long on screen.
		constexpr float kHintSeconds = 3.5f;

		// CPU thread only (the input poll).
		ReloadGesture::State s_gesture;
		ReloadGesture::Phase s_gesture_phase = ReloadGesture::Phase::Idle;
		State s_driver;
		StanceHold s_stance;
		SpatialControls::ZoneState s_head_yaw; // HeadAnchor's yaw memory, as each zone keeps its own
		// The pad and button of the last profile that had them, so an attempt ending as first person goes away still lets
		// go cleanly, and the release-first latch keeps running.
		int s_pad_port = -1;
		std::string s_reload_button;
		// The arm-time line: the verdict last logged, and for which game.
		std::string s_verdict;
		u32 s_verdict_crc = 0;
		// The most rounds each gun (by item id; guns are ids 0-32) has been seen holding this session: a gun at that when a
		// reload loads nothing is taken as full (NothingLoadedToast).
		std::array<u16, 64> s_most_rounds{};
		// The first-raise hint: shown once per session; the second line's countdown (below 0: none) and the first line, so
		// the second only replaces it, never another toast.
		bool s_hint_shown = false;
		float s_hint_next_s = -1.0f;
		std::string s_hint_first;
		std::string s_hint_second;
		// Reloading(): when the last poll that had a reload running ran (CameraDriver::SteadyNowMs; 0 none).
		std::atomic<u64> s_reloading_ms{0};
		constexpr u64 kReloadingStaleMs = 250;
		// NoteRaisePressed since the last Poll (review a4).
		bool s_raise_pressed = false;

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

		void Pulse(int hand, const SpatialControls::HapticCue& cue)
		{
			const float setting = EmuConfig.VR.HapticStrength;
			const float strength = std::isfinite(setting) ? std::clamp(setting, 0.0f, 1.0f) : 1.0f;
			const float amplitude = cue.amplitude * strength;
			if (amplitude > 0.0f && (hand == VRInputSnapshot::LEFT || hand == VRInputSnapshot::RIGHT))
				XRInput::QueuePulse(hand, amplitude, cue.seconds);
		}

		// The gun hand: camera.lookAt's aim hand (the right without one), as GameFeedback takes it.
		int GunHand(const ProfileDB::Profile& p)
		{
			if (p.camera.has_value() && p.camera->look_at.has_value() && p.camera->look_at->has_aim &&
				(p.camera->look_at->aim_hand == VRInputSnapshot::LEFT || p.camera->look_at->aim_hand == VRInputSnapshot::RIGHT))
				return p.camera->look_at->aim_hand;
			return VRInputSnapshot::RIGHT;
		}

		// The stick the player walks with: walk-and-shoot's (lookAt.aim moveStick), else the left.
		int MoveHand(const ProfileDB::Profile& p)
		{
			if (p.camera.has_value() && p.camera->look_at.has_value() && p.camera->look_at->has_aim &&
				(p.camera->look_at->aim_move_hand == VRInputSnapshot::LEFT ||
					p.camera->look_at->aim_move_hand == VRInputSnapshot::RIGHT))
				return p.camera->look_at->aim_move_hand;
			return VRInputSnapshot::LEFT;
		}

		// A Gamepad control as the player knows it, for the hint; nullptr for one without a short name.
		const char* ControlWords(std::string_view control)
		{
			static constexpr std::pair<const char*, const char*> kWords[] = {
				{"LeftTriggerPress", "left trigger"}, {"LeftTrigger", "left trigger"}, {"RightTriggerPress", "right trigger"},
				{"RightTrigger", "right trigger"}, {"LeftBumper", "left bumper"}, {"RightBumper", "right bumper"},
				{"LeftGripPress", "left grip"}, {"RightGripPress", "right grip"}, {"ButtonA", "A"}, {"ButtonB", "B"},
				{"ButtonX", "X"}, {"ButtonY", "Y"}};
			for (const auto& [name, words] : kWords)
			{
				if (control == name)
					return words;
			}
			return nullptr;
		}

		// What the local character's item cells hold, for "No spare ammo": read through the profile's pouches (their item
		// bytes are the cells) and checked as the pouches name items. None only when every cell was read and none holds an
		// ammo item with rounds left; a record failing its checks, or an item ItemData does not know, makes it Unknown
		// unless another cell already holds ammo.
		enum class Spare : u8
		{
			Unknown,
			None,
			Some,
		};
		Spare ScanSpare(const ProfileDB::Profile& profile, u32 record, GuestReader reader)
		{
			if (!profile.pouches.has_value())
				return Spare::Unknown;
			GameFeedback::ItemTable table = GameFeedback::ItemTableOf(profile);
			if (!table.valid)
				return Spare::Unknown;
			table.slot_from_camera = (reader == nullptr); // live: the debounced local slot (review M1)
			bool unknown = false;
			bool any = false;
			for (const ProfileDB::Pouch& pouch : profile.pouches->list)
			{
				if (!pouch.has_item)
					continue;
				u32 byte = 0;
				if (!Read(reader, record + pouch.item_offset, 1, &byte))
				{
					unknown = true;
					continue;
				}
				any = true;
				if (byte == 0)
					continue;
				const GameFeedback::ItemRecord rec = GameFeedback::ReadItemRecord(table, byte, reader ? reader : DefaultRead);
				const ItemData::Kind kind = rec.valid ? ItemData::KindOf(rec.item_id) : ItemData::Kind::Unknown;
				if (kind == ItemData::Kind::Ammo && rec.count > 0)
					return Spare::Some;
				if (kind == ItemData::Kind::Unknown)
					unknown = true;
			}
			return (unknown || !any) ? Spare::Unknown : Spare::None;
		}

		// One line per attempt, and the toast when it could not run (once per attempt: this is called once).
		void Report(const Out& o, bool no_spare, u32 most_seen)
		{
			if (o.to > o.from)
			{
				const char* stopped = (o.result == Result::Reloaded) ? "" : ResultName(o.result);
				Console.WriteLn("(VR) Reload: %u -> %u rounds in %.1f s (gesture%s%s).", o.from, o.to,
					static_cast<double>(o.seconds), stopped[0] ? "; stopped: " : "", stopped);
				return;
			}
			switch (o.result)
			{
				case Result::NothingLoaded:
				{
					const char* toast = NothingLoadedToast(no_spare, o.from, most_seen);
					Console.WriteLn("(VR) Reload: nothing loaded in %.1f s, %s (%u rounds, the most seen %u; gesture).",
						static_cast<double>(o.seconds), toast == kToastFull ? "the magazine is full" : "no spare ammo for this gun",
						o.from, most_seen);
					HudCards::Toast(toast, 2.0f);
					break;
				}
				case Result::NoSpareAmmo:
					Console.WriteLn("(VR) Reload: not tried, no ammo in the item cells (%u rounds; gesture).", o.from);
					HudCards::Toast(kToastNoAmmo, 2.0f);
					break;
				case Result::Moved:
					Console.WriteLn("(VR) Reload: %s, you were walking (the game reloads standing still; %u rounds; gesture).",
						o.seconds > 0.0f ? "stopped" : "not tried", o.from);
					HudCards::Toast(kToastWalking, 2.0f);
					break;
				case Result::NotLowered:
					Console.WriteLn("(VR) Reload: not tried, the weapon stayed raised with R1 held back for %.1f s (R1 held on "
									"another pad?); L1 was not pressed (gesture).",
						static_cast<double>(o.seconds));
					break;
				default:
					Console.WriteLn("(VR) Reload: stopped, %s (%u rounds; gesture).", ResultName(o.result), o.from);
					break;
			}
		}
	}

	Frame Poll(const VRInputSnapshot& snap, const SpatialControls::Hands& hands, GuestReader reader, float dt)
	{
		Frame frame;
		dt = std::clamp(FiniteOr(dt, 0.0f), 0.0f, 0.25f);

		// The hint's second line, once the first has been read (and only over it, never over another toast).
		if (s_hint_next_s >= 0.0f)
		{
			s_hint_next_s -= dt;
			if (s_hint_next_s < 0.0f)
			{
				HudCards::ToastView now;
				if (!HudCards::CurrentToast(&now) || now.text == s_hint_first)
					HudCards::Toast(s_hint_second, kHintSeconds);
			}
		}

		// Nothing is looked up or read unless first person is armed with the local record resolved (CameraDriver: never
		// online outside private co-op, so never on obsrv), the session is live and focused and the VM runs.
		const bool live = snap.generation != 0 && snap.actions_active;
		u32 record = 0;
		const ProfileDB::Profile* profile = nullptr;
		if (live && VMManager::GetState() == VMState::Running && !SplitState::Active() && VMManager::HasValidVM() &&
			CameraDriver::LocalRecord(&record))
		{
			const std::string serial = VMManager::GetDiscSerial();
			profile = serial.empty() ? nullptr : ProfileDB::Lookup(serial, VMManager::GetDiscCRC());
		}
		const ProfileDB::FeedbackParams* fb =
			(profile && profile->feedback.has_value() && profile->feedback->has_weapon) ? &profile->feedback.value() : nullptr;

		// What the profile gives the reload: the button (feedback.weapon.reloadButton), the VR Gamepad control that presses
		// it on a pad, and the belt zone the magazine spot sits in.
		const ProfileDB::SpatialControlSpec* gamepad = nullptr;
		const char* reload_words = nullptr;
		const ProfileDB::SpatialControlSpec* belt = nullptr;
		if (fb && !fb->weapon_reload_button.empty())
		{
			for (const ProfileDB::SpatialControlSpec& c : profile->controls)
			{
				if (c.kind == SpatialControls::DeviceKind::Gamepad && c.usb_port < 0 && !gamepad)
				{
					for (const ProfileDB::SpatialControlSpec::Bind& b : c.bind)
					{
						if (b.target == fb->weapon_reload_button)
						{
							gamepad = &c;
							reload_words = ControlWords(b.control);
							break;
						}
					}
				}
				if (c.kind == SpatialControls::DeviceKind::Zone && !belt && ZoneMarkers::ZoneOf(c.id) == ZoneMarkers::Zone::Belt)
					belt = &c;
			}
		}
		if (profile)
		{
			if (gamepad)
			{
				s_pad_port = static_cast<int>(gamepad->pad_port);
				s_reload_button = fb->weapon_reload_button;
			}
			else
			{
				s_pad_port = -1;
				s_reload_button.clear();
			}
		}
		const bool setting = EmuConfig.VR.ReloadGesture;
		const int gun_hand = profile ? GunHand(*profile) : VRInputSnapshot::RIGHT;
		const int carry = 1 - gun_hand;

		// The magazine spot: the belt zone's centre, placed exactly as that zone is (from the head, turned with its yaw),
		// with the gesture's own radius; the gesture's default spot (the left hip) without a belt zone.
		ReloadGesture::Params gp;
		gp.gun_hand = gun_hand;
		if (belt)
			gp.spot = {belt->placement.side, belt->placement.height, belt->placement.forward, 0.0f};
		const char* spot_side = (gp.spot.side < 0.0f) ? "left" : "right";

		// The arm-time line: once per game, and again whenever the verdict changes (the setting, a profile edit).
		if (profile)
		{
			std::string verdict;
			if (!fb)
				verdict = "off (the profile has no feedback.weapon: the gun and its rounds cannot be read)";
			else if (fb->weapon_reload_button.empty())
				verdict = "off (the profile's feedback.weapon has no reloadButton)";
			else if (!gamepad)
				verdict = fmt::format("off (no VR Gamepad control is bound to {})", fb->weapon_reload_button);
			else if (!setting)
				verdict = fmt::format("off (the VR ReloadGesture setting); {} with the weapon lowered still reloads",
					fb->weapon_reload_button);
			else
				verdict = fmt::format("on (magazine at the {} hip{}; {} with the weapon lowered)", spot_side,
					belt ? "" : ", the gesture's own spot: the profile has no belt zone", fb->weapon_reload_button);
			const u32 crc = VMManager::GetDiscCRC();
			if (verdict != s_verdict || crc != s_verdict_crc)
			{
				s_verdict = verdict;
				s_verdict_crc = crc;
				Console.WriteLn("(VR) Reload gesture: %s.", verdict.c_str());
			}
		}

		// The equipped gun and its rounds (GameFeedback reads them each vsync, just before this poll).
		const bool env = gamepad != nullptr;
		bool gun = false;
		u32 ammo = 0;
		u32 weapon = 0;
		u16 item_id = 0;
		if (env && GameFeedback::WeaponInHand() == GameFeedback::Hold::Weapon)
		{
			const GameFeedback::WeaponView wv = GameFeedback::GetWeapon();
			if (wv.valid && ItemData::IsGun(ItemData::KindOf(wv.item_id)))
			{
				gun = true;
				ammo = wv.ammo;
				item_id = wv.item_id;
				weapon = (static_cast<u32>(wv.equipped) << 16) | wv.item_id;
				if (item_id < s_most_rounds.size())
					s_most_rounds[item_id] = std::max(s_most_rounds[item_id], wv.ammo);
			}
		}
		// The stance now (the driver judges the lowering on it), and with its hysteresis for the gesture.
		bool stance_ok = false;
		bool raised_now = false;
		if (env)
		{
			u32 v = 0;
			stance_ok = Read(reader, record + fb->weapon_stance_offset, 1, &v);
			raised_now = stance_ok && v == fb->weapon_stance_raised;
		}
		const bool gun_raised = s_stance.Step(raised_now, dt);
		// The action byte, where the profile names it (File #1's).
		bool action_known = false;
		bool reloading = false;
		if (env && fb->has_weapon_reload)
		{
			u32 v = 0;
			action_known = Read(reader, record + fb->weapon_action_offset, 1, &v);
			reloading = action_known && v == fb->weapon_reload_action;
		}
		const bool menu = SpatialControls::MenuMode();
		// A screen opening: MenuMode (pauseWhen 0x720AA0) rises 0.55-0.65 s after the game opens the item screen or the map,
		// and melee.screenFlag (File #1 0x48BF7E, the menu byte) 30-60 ms after the press, so the driver lets go of L1 on
		// either; a flag that cannot be read counts as up (review a2).
		bool screen = menu;
		if (env && !screen && profile->melee.has_value() && profile->melee->screen_flag.has_value())
		{
			u32 v = 0;
			screen = !Read(reader, profile->melee->screen_flag.value(), 1, &v) || v != 0;
		}
		const bool usable = env && setting && gun && stance_ok;

		// The plain controls, the first time a gun is raised in a session: the game's own reload, and the gesture's.
		if (env && gun && raised_now && !menu && !s_hint_shown && reload_words)
		{
			s_hint_shown = true;
			s_hint_first = fmt::format("Reload: lower gun, hold {}", reload_words);
			HudCards::Toast(s_hint_first, kHintSeconds);
			if (setting)
			{
				s_hint_second = fmt::format("Or grab a magazine at your {} hip", spot_side);
				s_hint_next_s = kHintSeconds;
			}
		}

		// The gesture: armed only while the driver is idle, so a new grab cannot start under a reload in flight. Stepped
		// every poll whatever else holds, so a grip's edges are its own and a grip closed meanwhile is never a grab.
		ReloadGesture::In gi;
		gi.dt = dt;
		gi.hands = hands;
		gi.armed = usable && s_driver.phase == Phase::Idle;
		gi.menu = menu;
		gi.gun_raised = gun_raised;
		SpatialControls::Anchor head;
		if (env && SpatialControls::HeadAnchor(snap, s_head_yaw, &head))
		{
			gi.spot_centre = ReloadGesture::SpotCentre(head, gp);
			gi.spot_radius = gp.spot_radius;
		}
		const ReloadGesture::Out go = ReloadGesture::Step(gi, gp, s_gesture);
		if (go.phase_changed)
		{
			// A grab, not a return from the gun (Ready drifting back to Grabbed), ticks.
			if (go.phase == ReloadGesture::Phase::Grabbed && s_gesture_phase != ReloadGesture::Phase::Ready)
				Pulse(carry, kTickGrabbed);
			else if (go.phase == ReloadGesture::Phase::Ready)
				Pulse(carry, kTickReady);
		}
		if (std::string_view(go.reason).starts_with("cancelled"))
			Console.WriteLn("(VR) Reload gesture: %s.", go.reason);
		s_gesture_phase = go.phase;

		// The driver.
		In di;
		di.dt = dt;
		di.request = go.reload_requested;
		di.usable = usable;
		di.weapon = weapon;
		di.screen = screen;
		di.raise_pressed = s_raise_pressed;
		s_raise_pressed = false;
		di.raised = raised_now;
		di.ammo = ammo;
		di.action_known = action_known;
		di.reloading = reloading;
		if (profile)
		{
			const VRHandState& mh = snap.hands[MoveHand(*profile)];
			const float push = std::sqrt(mh.thumbstick_x * mh.thumbstick_x + mh.thumbstick_y * mh.thumbstick_y);
			di.stick = std::isfinite(push) ? push : 0.0f;
		}
		// The cells are read only when they decide something: on the request (not tried without ammo) and below at the end.
		if (di.request && profile && fb)
			di.no_spare = ScanSpare(*profile, record, reader) == Spare::None;
		const Out o = Step(di, Params{}, s_driver);
		if (o.finished)
		{
			const bool no_spare = di.no_spare || (o.result == Result::NothingLoaded && profile &&
			                                         ScanSpare(*profile, record, reader) == Spare::None);
			Report(o, no_spare, (item_id < s_most_rounds.size()) ? s_most_rounds[item_id] : 0u);
		}

		s_reloading_ms.store((o.phase != Phase::Idle) ? std::max<u64>(CameraDriver::SteadyNowMs(), 1) : 0, std::memory_order_release);

		frame.owned_hand = go.owns_left_hand ? carry : -1;
		frame.pad_port = s_pad_port;
		frame.reload_button = s_reload_button;
		frame.suppress_raise = o.suppress_raise;
		frame.hold_reload = o.hold_reload;
		// The magazine hand's reload button is the gesture's too while it owns the hand: a trigger pull at the gun is the
		// reload, not an L1 with the weapon raised (which would switch the lock-on target).
		frame.mask_reload = o.mask_reload || go.owns_left_hand;
		frame.raise_release_first = o.raise_release_first;
		return frame;
	}

	void NoteRaisePressed()
	{
		s_raise_pressed = true;
	}

	void OnStateLoaded()
	{
		// An attempt in flight lets go as an end into a screen does: L1 up now, R1 back after the gap and release-first.
		if (s_driver.phase == Phase::Lowering || s_driver.phase == Phase::Holding)
		{
			Console.WriteLn("(VR) Reload: stopped, a savestate was loaded (gesture).");
			s_driver.phase = Phase::Releasing;
			s_driver.phase_s = 0.0f;
			s_driver.release_first = true;
		}
		s_raise_pressed = false;
		s_gesture.Reset();
		s_gesture_phase = ReloadGesture::Phase::Idle;
		s_stance = StanceHold{};
	}

	bool Reloading()
	{
		const u64 ms = s_reloading_ms.load(std::memory_order_acquire);
		const u64 now = CameraDriver::SteadyNowMs();
		return ms != 0 && (now < ms || now - ms < kReloadingStaleMs);
	}
}
