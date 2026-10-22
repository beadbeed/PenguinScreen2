// SPDX-FileCopyrightText: 2026 PenguinScreen2 contributors
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

namespace VR::ProfileDB
{
	struct Profile;
}

namespace VR::ItemReach
{
	struct Live;
}

// Real hand pickups ([VR] HandPickup, off by default; File #1 strictly offline; research/2026-10-21-hand-pickup.md).
//
// ItemReach highlights the floor item a hand is on (any item, either hand; its volume's box, a light tick). A deliberate
// squeeze of that hand's grip (it must have been open first, and close past kGripOn) with no game screen up, the weapon
// lowered and the hand in no body zone arms a single-use word in the host block (reach.pickup.block: the magic, the game
// counter's stamp, the local record, the search point under the hand) and presses Circle once on your own pad. The cave
// at 0x61DB94 (camera.silence, whenSetting HandPickup) then makes the game's own Circle search look at that point instead
// of 46 cm in front of the character, for the local record only, and clears the word itself. The host clears it too: on
// a 300 ms timeout, the hand leaving, a screen, a state load, the switch off or the network coming on.
//
// The confirm (Alex's approved exception, 2026-10-21; nothing else is ever confirmed for you): when the game's own take-it
// prompt comes up (overlay u32 1, which 1, sub 2), its search result is this item (the record's status 1 and result its
// record index), you have a free cell, the prompt's cursor sits on one of your own free cells (moved there with the D-pad,
// one closed-loop press at a time, if it rests on a full one) and the grip has been held all the way from the squeeze,
// Circle is pressed once. A full inventory is the game's swap: nothing is pressed. Let go before the prompt, or press any
// button yourself, and the prompt is yours. Any other screen, prompt or item: nothing.
namespace VR::HandPickup
{
	// The squeeze: closed past kGripOn after having been open below kGripOff; held while above kGripOff.
	inline constexpr float kGripOn = 0.75f;
	inline constexpr float kGripOff = 0.35f;

	enum class Phase : u8
	{
		Idle,       // waiting for a squeeze on a highlighted item
		Armed,      // the word is armed and Circle pressed: waiting for the cave to take the word
		Reaching,   // the game searched at the hand: waiting for the take-it prompt (the reach animation, ~1.2-1.5 s)
		Prompt,     // the take-it prompt is up: checking it, settling, then confirming or steering
		Steering,   // one D-pad press out, waiting for the cursor to move
		Confirming, // Circle pressed on the prompt: waiting for it to close and the item to show in a cell
	};
	const char* PhaseName(Phase p);

	struct Tuning
	{
		u64 arm_timeout_ms = 300;    // the cave must take the word within this, or it is cleared
		u64 reach_timeout_ms = 3000; // the take-it prompt must come within this of the search
		u64 settle_ms = 300;         // the prompt is up this long before anything is pressed on it
		u64 press_ms = 100;          // one press (the game reads the pad at 30 Hz)
		u64 steer_wait_ms = 500;     // a D-pad press must move the cursor within this
		u32 max_steer = 3;           // at most this many D-pad presses per prompt
		u64 close_timeout_ms = 2000; // the prompt must close within this of the confirm
		u64 cell_wait_ms = 600;      // and the item show in a cell within this of the close
	};

	// One vsync of what the game and the hands say.
	struct In
	{
		u64 now_ms = 0;
		bool enabled = false;      // the switch, File #1 offline, first person armed, the cave and site in, the block ours
		bool guards_now = false;   // the camera guards held this vsync (not just their grace)
		bool screen_up = false;    // a game screen is drawn (overlay u32), or menu mode
		bool take_prompt = false;  // ... and it is a take-it prompt (which 1, sub 2)
		bool weapon_raised = false;
		float grip[2] = {};        // VRInputSnapshot::LEFT / RIGHT
		bool hand_in_zone[2] = {}; // a body zone (holster, belt, backpack) has that hand: its squeeze is the zone's
		bool player_buttons = false; // the player's own A, B, X, Y, trigger or D-pad: they answer the prompt themselves
		// ItemReach::CurrentPickup
		bool target_highlighted = false;
		int target_hand = -1;
		u32 target_node = 0;
		u32 target_index = 0;
		u16 target_item = 0;
		// Guest memory
		bool block_armed = false;  // the block's armed word reads the magic
		u32 status = 0;            // the local record's last search: status (1 = found) and item record index
		u32 result = 0;
		u8 cells[4] = {};          // the local record's item cells (record index, 0 = empty)
		u32 cell_count = 4;
		bool cursor_in_grid = false; // the prompt's cursor is in your own grid ...
		int cursor_cell = -1;        // ... on this cell (its node and index match), -1 otherwise
	};

	struct Out
	{
		bool arm = false;          // write the block (the target's search point) and arm the word
		bool clear = false;        // clear the armed word
		bool press_circle = false; // one Circle press of press_ms
		int press_dpad = -1;       // one D-pad press of press_ms: 0 up, 1 down, 2 left, 3 right
		int taken_cell = -1;       // the item showed in this cell
		const char* event = nullptr; // what happened, for the log (nullptr: nothing to say)
	};

	struct State
	{
		Phase phase = Phase::Idle;
		bool released[2] = {};     // the grip has been open since its last squeeze (false at first: release first)
		bool held = false;         // the arming hand's grip has stayed closed since its squeeze
		int hand = -1;
		u32 node = 0;
		u32 index = 0;
		u16 item = 0;
		u64 t0 = 0;                // when the phase began
		bool checked = false;      // the prompt's item and cells have been checked
		u32 steers = 0;
		int steer_from = -1;
	};

	// Pure: one vsync. The caller writes the block on arm (and on clear), starts the presses and logs the event.
	Out Step(const Tuning& t, const In& in, State& st);

	// Pure: the D-pad direction from cell `from` toward the nearest free cell in a 2 x 2 grid (0 1 / 2 3; never right of
	// the right column, the partner's panel, nor down from the bottom row, the equipped box), -1 when no free cell or
	// `from` is not a cell.
	int DpadTowardFree(int from, const u8 cells[4], u32 cell_count);

	// CPU thread, every vsync from GameFeedback::Poll after ItemReach::PollHandTarget (live as ItemReach got it).
	void Poll(const ProfileDB::Profile* profile, const ItemReach::Live& live);

	// CPU thread: a loaded state has its own block and its own prompt: start over (the armed word is cleared).
	void OnStateLoaded();

	// The presses this poll, for VRInputSource (the CPU thread's input poll): Circle, or a D-pad direction (0 up, 1 down,
	// 2 left, 3 right; -1 none), on the VR Gamepad's own pad. Only ever presses; the player's own values go out otherwise.
	struct Press
	{
		bool circle = false;
		int dpad = -1;
	};
	Press CurrentPress();

	// The state machine (squeeze latch, arming, timeouts, the prompt's checks, steering, the confirm and every refusal), the
	// D-pad route and the shipped File #1 block; logs "(VR) Hand pickup self-test: passed (N checks)" or "... FAIL: <check>".
	bool SelfTest();
}
