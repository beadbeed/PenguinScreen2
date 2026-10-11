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
		bool suspended = false;        // a suspendWhen guard, or the latch after a confirm, holds steering
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
		bool back = false;        // B (the game's cancel)
		bool menu = false;        // Menu
		bool dpad = false;        // any real D-pad button on either hand
		float right_stick = 0.0f; // how far each stick is pushed
		float left_stick = 0.0f;
		float grip = 0.0f;     // the more squeezed grip (OverrideGrip: not a belt pouch's)
		bool suspend = false;  // one of the profile's suspendWhen guards passes
		float dt_ms = 1000.0f / 60.0f;
		int request = -1;           // a belt pouch's request made this poll (RequestCell), -1 none
		bool pouch_suspend = false; // one of the profile's pouches.suspendWhen guards passes (an item's command submenu)
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
		bool selfcheck_failed_now = false;
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
		int last_cell = -1;
		int under = -1; // the cell under the pointer, with HoverCell's hysteresis
		float under_ms = 0.0f;
		int ignore = -2;        // the hover the menu opened with, ignored until it changes (-2: none)
		bool disarmed = false;  // after an override or an abort: no steering until the hover changes
		int disarm_under = -1;  // the hover then
		// A confirm reached the game (no suspendWhen): no steering until Cross, the cursor reaching another cell or the
		// menu closing. It lives as long as the menu, not the gate: a gate drop with the menu still up keeps it.
		bool latched = false;
		int latch_cell = -1;
		u32 suspend_polls = 0;  // the suspendWhen tail
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
		// A belt pouch's request waiting for point-to-select (made with the screen closed, or within the arm delay).
		int request = -1;
		float request_ms = 0.0f;
	};

	StepOut Step(const ProfileDB::MenuPointerParams& p, const StepIn& in, State& st);

	// The ray/quad maths, hovering, planning, steering against a scripted game and the online rule. Runs once from Poll
	// when PCSX2_VR_SELFTEST is set; logs "(VR) MenuPointer self-test: passed (...)" or "... FAIL: <first check>".
	bool SelfTest();
}
