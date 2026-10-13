// SPDX-FileCopyrightText: 2026 PenguinScreen2 contributors
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

namespace VR::ProfileDB
{
	struct Profile;
	struct ReachParams;
}

// The "you can pick this up" cue for first-person play (profile reach:, VR setting PickupCue). The game has no flag for
// "Circle would pick something up now": it searches its trigger list only when Circle is pressed. This module runs that
// same search read-only every couple of vsyncs from the local character's position and heading (the probe circle ahead
// of it against the quads of the floor-item triggers, the height rule, the lowest priority wins), so what it says is in
// reach is what a press would take (25 of 25 against the game's own press, File #1; the research is in
// tools/outbreak-vr/research/2026-10-12-ui-flags-research.md, section B). When an item comes into reach the right hand
// gets a light tick and a short toast names it ("Kevin's 45 Auto  (A)"), once per approach: not again while it stays in
// reach, and re-armed after it has been out of reach for half a second. Reads only: nothing is written to the game, so
// it works the same online (first person itself is only armed in private co-op there).
namespace VR::ItemReach
{
	// Reads guest memory: the same shape as BeltPouches::GuestReader. false for an address the reader will not give (the
	// default one reads main RAM only, naturally aligned). Every address is also checked against main RAM before the
	// reader is asked.
	using MemReader = bool (*)(u32 address, u8 width, u32* value);

	// Why a walk was cut short or a probe could not be taken (a fault only shortens or stops the search, never invents an
	// item; each kind is logged once per arm).
	enum class Fault : u8
	{
		None,
		Probe,       // the character's position, heading or reach did not read, or is not a finite number
		ListHead,    // the u32 at the list head address could not be read
		NodePointer, // a node's address is outside main RAM, not 4-byte aligned or unreadable: the walk stopped there
		NodeCap,     // reach.maxNodes nodes were walked and the list went on
		Cycle,       // a node was met twice: the list loops back on itself
	};

	// What one search found.
	struct Reach
	{
		bool found = false;
		u32 index = 0;     // the item record index in the winning node's item field
		u16 item_id = 0;   // that record's item id (ItemData)
		u32 node = 0;      // the winning node's address (logs)
		u32 walked = 0;    // nodes visited
		Fault fault = Fault::None;
	};

	// The read-only copy of the game's press-time floor-item search for the local character record at `record`: walks at
	// most params.max_nodes nodes of the trigger list (cycle-safe, every address range-checked against main RAM) and
	// returns the item the game's Circle would take now, if any. reader null = guest main RAM. Reads only.
	Reach Evaluate(const ProfileDB::ReachParams& params, u32 record, MemReader reader);

	// What the caller knows about this vsync (GameFeedback::Poll fills it from CameraDriver and SpatialControls).
	struct Live
	{
		bool armed = false;        // first person is armed and the local record resolved (CameraDriver::LocalRecord)
		bool guards_now = false;   // and this vsync's camera guards held, not just the disarm grace after they failed
		u32 record = 0;            // the local character's record
		bool screen_up = false;    // a game screen is up (the item screen, map, pause, a take-it prompt): Circle answers it
		bool weapon_raised = false; // the weapon is up: Circle fires
	};

	// CPU thread, every vsync from GameFeedback::Poll with profile null when there is no VR session or no profile. Does
	// nothing without a reach block, with the PickupCue setting off or while first person is not armed (then the next
	// approach cues afresh). Not evaluated (and the state kept, so a prompt declined with Cross is not announced again)
	// while a screen is up, the weapon is raised or the guards are in their grace.
	void Poll(const ProfileDB::Profile* profile, const Live& live, u64 now_ms, float haptic_strength);

	// CPU thread: a loaded state is a new scene, so the next approach cues afresh.
	void OnStateLoaded();

	// What is in reach as of the last evaluation (nothing while not evaluating). serial counts every change of what is in
	// reach (an entry, a switch to another item, a leave), so a reader can tell "changed" without keeping a copy. Any
	// thread.
	struct View
	{
		bool in_reach = false;
		u16 item_id = 0;
		u32 index = 0;
		u32 serial = 0;
	};
	View Current();

	// Overlap and height rules, ties and the walk's safety on synthetic trigger lists in scripted RAM (the bathroom Green
	// Herb's measured numbers among them), the once-per-approach rules, the toast text and the shipped File #1 block;
	// logs "(VR) Pickup cue self-test: passed (N checks ...)" or "... FAIL: <first check>". Runs once from Poll when
	// PCSX2_VR_SELFTEST is set.
	bool SelfTest();
}
