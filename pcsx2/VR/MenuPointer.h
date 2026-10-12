// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include "VR/VRInputState.h"

namespace VR::ProfileDB
{
	struct MenuPointerParams;
}

// Point-to-select menus in VR. While a menu is up in first-person play (menu mode) and the profile's menuPointer block
// matches it, the cell the controller points at on the menu screen is hovered, and the game's own cursor is steered
// there with closed-loop D-pad presses from this player's own pad, checked against the game's cursor struct after
// every press. The trigger stays the player's confirm: one pulled before the cursor has arrived is held back until it
// has, or never reaches the game, and nothing but the D-pad is ever pressed for the player. Online it steers only where
// the profile says coop: true (first person, and so menu mode, runs online only in private co-op). A belt pouch squeezed
// on the player's body asks for its cell the same way (RequestCell). The beam, the dot and the pointing hands are
// XRCompositor's; this module only reads, decides and steers.
//
// Submenus (the profile's suspendWhen: an item's command submenu, EQUIP/USE/PLACE/...): while one is open nothing is
// pressed (a press under way is let go that poll), the trigger is the player's (a fresh pull passes straight through to
// the submenu; one already held back stays back only until it is let go, so it never turns into a submenu confirm the
// player did not make), and once it has closed, steering waits for the dot to reach another cell, so closing it never
// yanks the cursor. After any confirm reaches the game steering also waits kAfterConfirmMs, so a submenu that confirm
// opens has its flag up before another press could go out.
//
// The player's own moves stand: a stick, the D-pad, a grip, B or Menu, or a cursor move the pointer did not make, stops
// steering until the dot settles on a cell other than the one it was on (the controller's jiggle while a thumb works a
// stick never counts, nor a neighbour the push had already moved the dot onto as the override engaged). A pull is judged
// by where the dot was resting as it began, so the dip a trigger pull gives the controller never sends the confirm to
// another cell, nor does a cell the dot only crossed on its way; a confirm with no telling which cell it was for is
// swallowed, and no press goes out on the poll a confirm arrives. A confirm still held as the controls come back from a
// gap (the SteamVR dashboard, a stalled snapshot) is never taken for a fresh one.
namespace VR::MenuPointer
{
	using GuestReader = bool (*)(u32 address, u8 width, u32* value);

	// CPU thread, every input poll (VRInputSource::PollEvents), before the Gamepad composes, so the D-pad press and the
	// confirm hold it publishes (SpatialControls::SetPointerDpad / SetPointerConfirmHold) reach the game this same poll.
	// reader reads guest memory (null: the default reader); dt is the time since the last poll in seconds.
	void Poll(const VRInputSnapshot& snap, GuestReader reader, float dt);

	// The cell (an index into the profile's menuPointer.cells) under the pointer while point-to-select can act on it
	// (steer the cursor there, or confirm it when the cursor is already there); -1 otherwise. Any thread.
	int HoveredCell();

	// The item screen (the profile's menuPointer menu) is up and live as the last Poll decided, by the same test that
	// opens point-to-select's gate (its when guards, and its liveWhen guards wherever Poll takes them), in menu mode
	// with the VM running, whether or not point-to-select may steer. The cursor struct alone stays set after the screen closes (the map and pause pass the
	// when guards too), so callers take this rather than evaluating when themselves. One poll late for a caller that
	// runs before Poll (BeltPouches). Any thread.
	bool ItemScreenLive();

	// A belt pouch (VR::BeltPouches) asks for the cursor on cell_index (an index into the profile's menuPointer.cells). It
	// is steered there with the same closed-loop D-pad presses as a pointed-at cell, once point-to-select runs (the item
	// screen may still be opening) and the screen has been up armDelayMs, under the same rules: refused while steering is
	// suspended (suspendWhen, the profile's pouches.suspendWhen, or the latch after a confirm), after a failed self-check,
	// with the cursor off the mapped cells or while the player takes over; nothing is ever confirmed. The pointer then
	// waits for its hover to change before it steers again. The latest request replaces an earlier one, and one that
	// point-to-select has not taken within 2.5 s lapses (or at once if the screen closes first). hand (0 left, 1 right,
	// -1 none) feels a tick when it is refused. CPU thread (the input poll, before Poll).
	void RequestCell(int cell_index, int hand);

	// The hands whose grip belongs to a belt pouch this poll (inside one, or still closed after a squeeze there): their
	// grip is not the override a squeezed grip otherwise is, so a pouch's own squeeze does not stop the steer it asked
	// for. CPU thread (the input poll, before Poll); BeltPouches::Poll sets it every poll.
	void SetPouchGrips(bool left, bool right);

	// The grip a user override counts: the more squeezed of the hands whose grip does not belong to a pouch. Pure.
	float OverrideGrip(float left, float right, bool left_pouch, bool right_pouch);

	// What the last Poll saw, plus the screen transform the compositor last published, for PINE (MsgVRMenuPointer op 0)
	// and the tests. Any thread.
	struct Telemetry
	{
		// The screen quad the pointer aims at (GetScreenTransform()).
		bool transform_valid = false;
		bool tablet = false;
		float centre[3] = {0.0f, 0.0f, 0.0f};
		float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		float half_width = 0.0f;
		float half_height = 0.0f;
		// The pointer's hit on it (the profile's hand).
		bool hit_valid = false;
		bool hit_right = false;
		bool test_hit = false;
		float u = 0.0f;
		float v = 0.0f;
		float hit_distance = 0.0f;
		// Steering.
		bool menu_mode = false;
		bool gate = false;             // point-to-select is running (the menu is up, allowed and switched on)
		bool in_flight = false;        // a steer toward target is under way
		bool confirm_held = false;     // the confirm controls are held back
		bool suspended = false;        // a suspendWhen guard (a submenu), the wait or the latch after a confirm, holds steering
		bool selfcheck_failed = false; // the cursor struct disagreed with a cell this opening of the menu
		bool refused_online = false;   // online, and the profile does not say coop: true
		s8 hovered = -1;
		s8 current = -1;
		s8 target = -1;
		u8 presses_target = 0;
		s8 dir = -1; // the D-pad pressed this poll (0 up, 1 down, 2 left, 3 right), -1 none
		u32 node_raw = 0;
		// Counts since the emulator started.
		u32 presses_total = 0;
		u32 arrivals = 0;
		u32 aborts = 0;
		u32 ticks = 0; // landing ticks
		u32 confirms_passed = 0;
		u32 confirms_swallowed = 0;
		// Belt pouch requests (RequestCell).
		s8 request = -1; // waiting for point-to-select, -1 none
		u32 requests = 0;
		u32 requests_started = 0;
		u32 requests_refused = 0;
		u32 requests_lapsed = 0;
	};
	Telemetry GetTelemetry();

	// The cell at (u, v) (fractions of the game picture): entered only inside a cell's rect shrunk by enterInset of its
	// size on each side, and kept while inside prev's full rect. -1 for none. Pure.
	int HoverCell(const ProfileDB::MenuPointerParams& p, float u, float v, int prev);

	// The first D-pad press (0 up, 1 down, 2 left, 3 right) on a shortest way from cell `from` to cell `to` over the
	// profile's neighbours (breadth first, trying up, down, left, right in turn); -1 when there is none, from == to, or a
	// cell is out of range. Pure.
	int PlanStep(const ProfileDB::MenuPointerParams& p, int from, int to);

	// Offline always; online only in menu mode with the profile's coop: true. Pure.
	bool SteeringAllowed(bool online_locked, bool menu_mode, bool coop);

	// The menu proper (the profile's liveWhen). The when guards can pass on another screen too (File #1's item-screen
	// cursor struct keeps reading 1 after the screen closes, and the menu flag is also up for the map and pause), so the
	// menu counts as up only once a liveWhen guard has passed (live_now) since when_up last rose; *seen carries that
	// between polls. when_up false clears *seen and returns false. Pure.
	bool LiveGate(bool when_up, bool live_now, bool* seen);

	// A steer stopped because the cursor did not answer a press this many times in one opening of the menu turns
	// point-to-select off until the menu opens again: a cursor that does not answer is not this menu's (a stale cursor
	// struct under another screen, or a wrong table), and every press it ignores may be moving something else.
	constexpr u32 kMaxUnansweredPresses = 2;

	// One poll of point-to-select, everything it depends on passed in, so the self-test can drive it with a scripted game.
	struct StepIn
	{
		bool gate = false;      // point-to-select may run this poll: the menu is up, allowed, switched on, the VM running
		// The profile's menu is up (its when guards pass), whether or not point-to-select may run. With the gate down it
		// keeps the latch after a confirm alive (the item screen and a submenu can stay up behind the SteamVR dashboard,
		// a pause or the setting going off); Poll evaluates it while the gate is up or the latch is held.
		bool screen_up = false;
		bool cursor_ok = false; // the cursor struct was read
		u32 node = 0;
		u16 index = 0;
		u16 x = 0;
		u16 y = 0;
		bool hit = false; // the pointer is on the screen at (u, v)
		float u = 0.0f;
		float v = 0.0f;
		bool confirm = false;     // right trigger at least half pulled, or A: the controls bound to the game's confirm
		float trigger = 0.0f;     // how far the right trigger is pulled (0 to 1), A counting as a full pull at once
		// The controls read live (the session focused, the snapshot fresh); false through a gap (the SteamVR dashboard, a
		// stalled snapshot), when everything reads released.
		bool actions_live = true;
		bool back = false;        // B (the game's cancel)
		bool menu = false;        // Menu
		bool dpad = false;        // any real D-pad button on either hand
		float right_stick = 0.0f; // how far each stick is pushed
		float left_stick = 0.0f;
		float grip = 0.0f;     // the more squeezed grip (OverrideGrip: not a belt pouch's)
		bool suspend = false;  // one of the profile's suspendWhen guards passes (a submenu is open)
		float dt_ms = 1000.0f / 60.0f;
		int request = -1;           // a belt pouch's request made this poll (RequestCell), -1 none
		bool pouch_suspend = false; // one of the profile's pouches.suspendWhen guards passes (an item's command submenu)
	};

	// With suspendWhen, steering waits this long after a confirm reaches the game: a submenu the confirm opens has its
	// flag up well within it (a 30 fps game shows it within two or three frames), so no press can slip in before the
	// flag is seen.
	constexpr float kAfterConfirmMs = 400.0f;

	// The hover over the last polls (State::hover), for the cell the dot was on before a pull began or an override.
	constexpr u32 kHoverHistory = 32;
	struct HoverSample
	{
		s8 under = -1;  // the hovered cell (HoverCell, with its hysteresis), -1 none
		bool hit = false;
		float u = 0.0f;
		float v = 0.0f;
		float dt_ms = 0.0f;   // the time from the poll before it
		float held_ms = 0.0f; // how long the hover had stayed on under by then (State::under_ms): resting, or brushing
	};

	struct StepOut
	{
		int dir = -1;              // the D-pad to press this poll
		bool confirm_hold = false; // hold the confirm controls back this poll
		int current = -1;          // the cell the cursor is on, -1 none
		int hovered = -1;          // HoveredCell()
		int target = -1;
		u32 presses = 0; // toward target
		bool in_flight = false;
		bool suspended = false;
		bool submenu = false; // a suspendWhen guard passes, or did within its tail
		bool selfcheck_failed = false;
		// What happened this poll.
		bool opened = false;
		bool closed = false;
		bool landed = false;  // the cursor reached another cell, whatever moved it
		bool pressed = false; // a D-pad press began
		bool arrived = false;
		int arrived_from = -1;
		float arrived_ms = 0.0f;
		u32 arrived_presses = 0;
		const char* aborted = nullptr; // why a steer stopped
		int aborted_target = -1;
		u32 aborted_presses = 0;
		bool confirm_passed = false; // a confirm reached the game
		int confirm_cell = -1;
		bool latched = false;           // and paused steering (no suspendWhen)
		bool confirm_swallowed = false; // a confirm that will never reach the game
		bool confirm_unsure = false;    // swallowed because there was no telling which cell it was for
		bool selfcheck_failed_now = false;
		bool unresponsive_now = false; // kMaxUnansweredPresses reached this poll: off for the rest of the opening
		// A belt pouch's request this poll.
		int request_started = -1; // steering to this cell began (or the cursor was already on it)
		int request_from = -1;    // from this cell
		const char* request_refused = nullptr;
		int request_refused_cell = -1;
		int request_lapsed = -1; // this cell's request lapsed
	};

	struct State
	{
		// Each opening of the menu.
		bool open = false;
		float open_ms = 0.0f;
		bool selfcheck_failed = false;
		u32 unanswered = 0;        // steers stopped because the cursor did not answer a press
		bool unresponsive = false; // unanswered reached kMaxUnansweredPresses
		int last_cell = -1;
		int under = -1; // the cell under the pointer, with HoverCell's hysteresis
		float under_ms = 0.0f;
		// The hover over the last kHoverHistory polls, newest at hover_next - 1 (a ring).
		HoverSample hover[kHoverHistory];
		u32 hover_count = 0;
		u32 hover_next = 0;
		int ignore = -2;        // the hover the menu opened with, ignored until it changes (-2: none)
		bool disarmed = false;  // after an override, a submenu or an abort: no steering until the hover changes
		// The hover then. After an override, the cell the dot was on as it began (or had just left), or the one it comes
		// to rest on that it was on meanwhile; never one it only brushed, and -1 only with no cell at all.
		int disarm_under = -1;
		// After an override or a submenu: only the dot settling on a cell other than disarm_under arms again, not the dot
		// leaving the cells and coming back, so the cursor never follows the dot left where the player took over or the
		// submenu closed.
		bool disarm_strict = false;
		// After an override, the settle: re-arming waits kOverrideSettleMs from the last poll of it and then until the dot
		// has rested kRestMs (kOverrideSettleMaxMs at most), while the controller springs back from the thumb's push.
		bool settling = false;
		float settle_ms = 0.0f;
		u64 override_cells = 0; // the cells the dot was on through the override (bit per cell)
		// The hover just before the trigger began to move (the pull), for the confirm it brings.
		bool pull_from_valid = false;
		HoverSample pull_from;
		// A confirm reached the game (no suspendWhen): no steering until Cross, the cursor reaching another cell or the
		// menu closing. It lives as long as the menu, not the gate: a gate drop with the menu still up keeps it.
		bool latched = false;
		int latch_cell = -1;
		u32 suspend_polls = 0;  // the suspendWhen tail
		// With suspendWhen: the wait after a confirm reached the game (kAfterConfirmMs), counting down every poll. It is
		// not part of an opening: only the menu closing clears it.
		float after_confirm_ms = 0.0f;
		// Steering.
		int target = -1;
		int phase = 0; // 0 idle (ready to plan the next press), 1 pressing, 2 released
		int dir = -1;
		u32 phase_polls = 0; // polls pressed (pressing) or released (released) so far
		int step_from = -1;  // the cell the current press started from
		u32 presses = 0;
		int steer_from = -1;
		float steer_ms = 0.0f;
		bool pending = false; // a confirm pulled while steering, passed on arrival
		// Across openings.
		bool swallow = false; // the confirm controls are held back until they are let go
		bool confirm_prev = false;
		bool back_prev = false;
		bool menu_prev = false;
		bool actions_were_live = true; // StepIn::actions_live last poll
		bool stick_engaged = false;    // the right stick engaged as the menu stick (held down to its release)
		bool pull = false;             // the trigger past kPullStart
		float pull_ms = 0.0f;          // since it went past
		u32 pull_polls = 0;            // polls since it went past (the hover ring's samples since pull_from)
		// A belt pouch's request waiting for point-to-select (made with the screen closed, or within the arm delay).
		int request = -1;
		float request_ms = 0.0f;
	};

	StepOut Step(const ProfileDB::MenuPointerParams& p, const StepIn& in, State& st);

	// The ray/quad maths, hovering, planning, steering against a scripted game and the online rule. Runs once from Poll
	// when PCSX2_VR_SELFTEST is set; logs "(VR) MenuPointer self-test: passed (...)" or "... FAIL: <first check>".
	bool SelfTest();
}
