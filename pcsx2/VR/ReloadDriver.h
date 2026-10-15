// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include "VR/SpatialControls.h"
#include "VR/VRInputState.h"

#include <string>
#include <vector>

// Reloading with the hands, wired into the input poll. VR::ReloadGesture is the detector (the magazine hand takes a magazine
// at a spot on the body, brings it to the gun, lets go there: one request); this is what a request does, and the glue that
// feeds the detector from the game and the headset.
//
// Outbreak's own reload (tools/outbreak-vr/research/2026-10-12-reload-timing-research.md, section 2a): hold L1 with the
// weapon LOWERED (R1 not held) and the character standing still. File #1 loads one round 0.31-0.36 s after the press, then
// one every 0.33 s while L1 is held, and stops by itself at full; File #2 loads the whole magazine at once. Nothing happens
// with R1 held, while walking, without spare ammo or with a full magazine; L1 works 0.06 s after R1 is let go. So a request
// holds back R1 (whatever presses it: the right bumper, the holster or backpack zone) until the local record's stance has
// left raised, then holds L1 on the player's own pad until the rounds stop rising, and then lets go of L1 and gives R1 back:
// a bumper or a holster grip still held raises the weapon again by itself, exactly as the player's own hand would. Nothing
// else is ever pressed, and nothing is written to the game: only the player's own R1 is held back and their own L1 pressed,
// so it runs wherever first person runs (offline and private co-op; never on obsrv, where first person never arms).
namespace VR::ReloadDriver
{
	// The button Outbreak raises the weapon with (held = aimed), held back while a reload runs.
	inline constexpr const char* kRaiseButton = "R1";

	// --- The driver's state machine: pure, so the self-test can run it on scripted game reads. ---

	struct Params
	{
		// R1 is held back at least this long before L1 goes down (the game takes L1 0.06 s after R1 is let go), and the
		// stance must leave raised within lower_max_s: if it does not, something else holds R1 (another pad) and the
		// reload is given up without pressing L1, which with the weapon raised would switch the lock-on target instead.
		float lower_min_s = 0.05f;
		float lower_max_s = 0.30f;
		// L1 is let go once the rounds have not risen for stall_s since the press or the last round (File #1 loads one per
		// 0.33 s, File #2 all at once). With the action byte known (feedback.weapon action/reload: File #1), a reload that
		// has not shown the reload action start_s after the press is a reload that will not run (full, no ammo), given up at
		// once; and the action ending after it was seen ends the hold too.
		float stall_s = 0.80f;
		float start_s = 0.35f;
		// A hold ends after hold_max_s unless a round came in the last round_gap_s (File #1 is still loading: a 15-round
		// Handgun from empty takes 4.9 s), and after hold_ceiling_s whatever happens.
		float hold_max_s = 4.0f;
		float round_gap_s = 0.45f;
		float hold_ceiling_s = 6.0f;
		// After L1 is let go, R1 stays held back this much longer, so the game sees L1 up before R1 comes back down.
		float release_gap_s = 0.05f;
		// The move stick pushed past this (0-1) is the player walking: the game reloads only standing still, so the hold
		// ends and the player keeps control.
		float stick_dead = 0.25f;
	};

	enum class Phase : u8
	{
		Idle,
		Lowering,  // R1 held back, waiting for the stance to leave raised
		Holding,   // R1 held back, L1 held
		Releasing, // L1 let go, R1 still held back for release_gap_s
	};
	const char* PhaseName(Phase phase);

	// How an attempt ended.
	enum class Result : u8
	{
		None,
		Reloaded,      // rounds went in (all the way, or until the rounds stopped)
		NothingLoaded, // L1 was held and no round went in (a full magazine, or no ammo for this gun)
		NoSpareAmmo,   // not tried: the item cells hold no ammo at all
		NotLowered,    // not tried: the weapon stayed raised with R1 held back (another pad holds R1)
		Moved,         // the player walked (the move stick)
		Screen,        // a game screen opened
		Lost,          // first person, the session, the gun or its record went away (or the gun changed)
		Raised,        // the player pressed R1 afresh mid-attempt: the weapon comes back up (review a4)
	};
	const char* ResultName(Result result);

	struct In
	{
		float dt = 0.0f; // seconds since the last Step; clamped to 0-0.25
		// ReloadGesture's reload_requested this poll. Taken only in Idle.
		bool request = false;
		// First person armed with the local record, the VR session live and focused, the setting on, a gun equipped
		// whose rounds are read. A Step without it ends an attempt in flight (Lost).
		bool usable = false;
		// The equipped gun, as its record index and item id (any value naming it): a change mid-attempt is Lost.
		u32 weapon = 0;
		// A game screen is up (the item screen, the map, pause).
		bool screen = false;
		// The local record's stance reads raised, now (no hysteresis: the lowering is judged on the game's own byte).
		bool raised = false;
		// The gun's rounds.
		u32 ammo = 0;
		// The action byte is read (the profile's feedback.weapon action/reload) and reads the reload action.
		bool action_known = false;
		bool reloading = false;
		// The move stick's push (0-1).
		float stick = 0.0f;
		// Read on the request poll only: the item cells hold no ammo at all, so the reload is not tried.
		bool no_spare = false;
		// The player pressed R1 afresh (a press that began while it was held back; RaiseButton's fresh_press) since the
		// last Step: ends an attempt in Lowering or Holding (Result::Raised), so R1 can always abort a reload (review a4).
		bool raise_pressed = false;
	};

	struct Out
	{
		Phase phase = Phase::Idle;
		// This poll's outputs: hold back every control bound to R1, press the reload button.
		bool suppress_raise = false;
		bool hold_reload = false;
		// The player's own reload button is held back (the driver has the button; ReloadButton below keeps it back until
		// it has been let go once after).
		bool mask_reload = false;
		// The attempt ended on a screen or with first person lost: a press of R1 still held when the hold-back ends stays
		// back until it has been let go once (RaiseButton), so R1 never comes back into a screen that just opened
		// (review a1). True through Releasing.
		bool raise_release_first = false;
		bool started = false;   // a request was taken this poll (the lowering began)
		bool finished = false;  // an attempt ended this poll: result, from, to, seconds
		bool round = false;     // a round went in this poll
		Result result = Result::None;
		u32 from = 0;
		u32 to = 0;
		// From the request to the last round in (or, with none, to the end).
		float seconds = 0.0f;
	};

	struct State
	{
		Phase phase = Phase::Idle;
		float phase_s = 0.0f;     // seconds in this phase
		float elapsed_s = 0.0f;   // seconds since the request
		float since_round_s = 0.0f;
		float last_round_s = 0.0f; // elapsed_s at the last round in
		bool saw_action = false;
		bool release_first = false; // set by an end on Result::Screen or Result::Lost
		u32 weapon = 0;
		u32 from = 0;
		u32 last = 0;
		void Reset() { *this = State{}; }
	};

	// One input poll. Never fails: nonsense in params is clamped (in a copy).
	Out Step(const In& in, const Params& params, State& st);

	// The stance "raised" for the gesture, with its own hysteresis: raised now, or within hold_s (0.3 s), so a stance that
	// flickers for a frame (a shot, the game re-deciding the aim) never cancels a reload gesture in flight.
	struct StanceHold
	{
		float since_s = 1.0e9f;
		bool Step(bool raised_now, float dt, float hold_s = 0.30f);
	};

	// One control bound to the reload button, as the Gamepad sends it this poll. own: the player's press of it (its composed
	// value). While the driver holds the button it reads 1; while it is held back (mask) it reads 0; and a press of the
	// player's own still held when either ends stays at 0 until it has been let go once (release-first, as after an input
	// gap), so the end of a reload never turns into a press the player did not make. latched is per control, kept by the
	// caller.
	float ReloadButton(float own, bool hold, bool mask, bool& latched);

	// One control bound to R1 (kRaiseButton), as it goes out this poll. own: the player's press (its composed value). While
	// suppress it reads 0. When release_first was set as the suppression ended, a press still held stays at 0 until it has
	// been let go once (review a1); otherwise R1 comes back as held (a reload that ran its course raises the weapon again).
	// fresh_press (may be null) is set when a press began while suppressed, not one held from before the hold-back started
	// (review a4: the driver ends the attempt on it). latch is per control, kept by the caller.
	struct RaiseLatch
	{
		bool was_suppressed = false;
		bool prev = false;
		bool latched = false;
	};
	float RaiseButton(float own, bool suppress, bool release_first, RaiseLatch& latch, bool* fresh_press);

	// The driver on scripted game reads (File #1's round-by-round reload and File #2's whole magazine, the lowering, every
	// stop rule, R1 coming back after L1, the toasts' cases), the stance hysteresis and the reload button's release-first.
	// Returns the name of every failed check; empty is a pass.
	std::vector<std::string> SelfTest();

	// Runs ReloadGesture::SelfTest and SelfTest and logs "(VR) Reload gesture self-test: passed." and "(VR) Reload driver
	// self-test: passed." (or "... FAIL: N check(s): <names>"). From CameraDriver's PCSX2_VR_SELFTEST runner.
	void LogSelfTests();

	// --- The glue (VRInputSource::PollEvents). ---

	using GuestReader = bool (*)(u32 address, u8 width, u32* value);

	// What the input poll does with the controls this poll.
	struct Frame
	{
		// The magazine hand while the gesture owns its grip (VRInputSnapshot::LEFT or RIGHT; -1 none): the Zone controls
		// and the belt pouches must not see it this poll, so a squeeze taking a magazine at the hip opens no item screen.
		int owned_hand = -1;
		// The pad the reload works on (the profile's VR Gamepad's port; -1: none this poll). Only controls on that pad are
		// touched.
		int pad_port = -1;
		bool suppress_raise = false; // every control bound to kRaiseButton reads 0
		bool hold_reload = false;    // the Gamepad's controls bound to reload_button read 1
		bool mask_reload = false;    // ... read 0 (see ReloadButton)
		bool raise_release_first = false; // R1 held through the end stays back until let go (see RaiseButton)
		std::string reload_button;   // feedback.weapon.reloadButton ("L1"); empty without one
	};

	// VRInputSource::ApplyReload, when a Gamepad control bound to R1 was pressed afresh under the hold-back: the next Poll
	// ends the attempt (Result::Raised). CPU thread.
	void NoteRaisePressed();

	// A savestate was loaded (GameFeedback::OnStateLoaded; offline only): an attempt in flight belongs to the old scene, so
	// it lets go at once (L1 up, R1 back release-first after the gap), and the gesture and the stance start over. CPU
	// thread (review a3).
	void OnStateLoaded();

	// CPU thread, every input poll, before BeltPouches::Poll and before the controls compose. hands: the snapshot's
	// (SpatialControls::HandsFromSnapshot); reader reads guest memory (null: main RAM); dt in seconds. Steps the gesture and
	// the driver, plays their haptics and toasts, writes their log lines.
	Frame Poll(const VRInputSnapshot& snap, const SpatialControls::Hands& hands, GuestReader reader, float dt);

	// A reload is running (lowering, holding L1, or letting go), as the last Poll left it; false once that is more than
	// 250 ms old (the polls stopped). The game holds the weapon lowered meanwhile, so CameraDriver keeps the first-person
	// hand showing the gun the magazine is going into instead of putting it back in the holster. Any thread.
	bool Reloading();

	// A copy of hands with the hand `hand` (-1: none) moved out of every zone's and pouch's reach. Its grip is kept, so a
	// zone still tracks it closing and opening and a grip held through the hiding never reads as a fresh squeeze.
	SpatialControls::Hands HideHand(const SpatialControls::Hands& hands, int hand);
	// The same for a snapshot (its grip pose), for the belt pouches.
	VRInputSnapshot HideHand(const VRInputSnapshot& snap, int hand);
}
