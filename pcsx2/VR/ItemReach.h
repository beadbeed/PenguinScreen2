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

	// --- Hand targeting (VR setting HandTargeting; a read-only prototype, File #1 offline only) ---
	//
	// Where the right hand is against the Handgun's trigger volume, for the J's Bar pistol on the counter. The model is the
	// same list walk as Evaluate (same list head, node filters and node cap), but instead of the character's probe circle it
	// asks about a point, the hand's grip position in game centimetres (CameraDriver::GameFromVr), and only trigger nodes
	// whose item record names item id 0 (the Handgun).

	struct HandTarget
	{
		bool found = false;    // a Handgun node passed the filters: the one nearest the hand (first in the list on a tie)
		u32 node = 0;          // its address
		u32 index = 0;         // its item record index
		u16 item_id = 0;       // that record's item id (0)
		double quad_x[4] = {}; // its floor quad's corners, game cm (x, z pairs as the node holds them)
		double quad_z[4] = {};
		double y_min = 0.0;    // its height range, game cm
		double y_max = 0.0;
		double centre[3] = {}; // the volume's middle: the corners' mean, the range's middle
		double hand[3] = {};   // the point asked about, game cm
		double horizontal_cm = 0.0; // from the hand to the quad on the floor plane, 0 inside it
		double vertical_cm = 0.0;   // from the hand's height to the range, 0 within it
		bool inside = false;   // inside the quad and within the range, no margin
		bool forward_agrees = false; // the game's own forward search (Evaluate) selects this node now
		u32 walked = 0;        // nodes visited
		Fault fault = Fault::None;
	};

	// Reads only. record is the local character's record (the forward search's, for forward_agrees); hand_game is the hand's
	// position, game cm. reader null = guest main RAM.
	// only_centre (may be null): consider only a volume whose centre (the corners' mean, the y range's middle) lies within
	// only_tol_cm of that game point; PollHandTarget passes the J's Bar counter pistol's, so no other Handgun trigger is
	// ever a target.
	// any_item: every floor item counts, not only reach.hand's item id ([VR] HandPickup).
	HandTarget EvaluateHandTarget(const ProfileDB::ReachParams& params, u32 record, const double hand_game[3], MemReader reader,
		const double* only_centre = nullptr, double only_tol_cm = 0.0, bool any_item = false);

	// --- Real hand pickups ([VR] HandPickup, File #1 strictly offline) ---
	//
	// The game's search core FUN_005ABBE0 at a point, as the cave at 0x61DB94 will call it (search mode 0, radius 50): every
	// enabled, not busy node of search mode 0 or 4, of any type (floor items 0x80 / 0x81 by the circle against the quad, 0x85
	// by the character's own position in the quad, every other type by the point in the quad), the height rule, the lowest
	// priority first in the list. Conservative: the core's extra checks on types 0x01, 0x02, 0x81 can only refuse a node, so a
	// node this names may lose to nothing it leaves out. Returns the winning node's address (0 for none, or a walk fault).
	u32 CoreSearchAt(const ProfileDB::ReachParams& params, u32 record, const double point[3], MemReader reader, Fault* fault = nullptr);

	// A point for the cave whose search picks target.node (CoreSearchAt): the hand's own (moved just inside the quad when it
	// is beside it), else the volume's centre, else halfway to each corner; the height the hand's, held inside the volume's
	// range. False when another trigger would win at every one of them (then nothing is armed).
	bool PickupPoint(const ProfileDB::ReachParams& params, u32 record, const HandTarget& target, MemReader reader, double out[3]);

	// The item a hand is on ([VR] HandPickup): refreshed every other vsync by PollHandTarget while HandPickup runs, cleared
	// when it stops. highlighted: the hand is inside the item's volume or within reach.pickup.near of it, and point holds a
	// search point the game's core picks this node from (point_ok). CPU thread only.
	struct PickupTarget
	{
		bool valid = false;        // an item within reach.pickup.show of a hand (the box shows)
		bool highlighted = false;  // ... and squeezable: inside or within near, point_ok
		int hand = -1;             // VRInputSnapshot::LEFT / RIGHT
		u32 node = 0;
		u32 index = 0;             // the item record index the node names
		u16 item_id = 0;
		double distance_cm = 0.0;  // from the hand to the volume (0 inside)
		bool point_ok = false;
		float point[3] = {};       // game cm: the search point for the cave
		u32 serial = 0;            // counts every change of the highlighted node (an entry, a switch, a leave)
	};
	PickupTarget CurrentPickup();

	// Pure: which hand's item is the pickup target this evaluation. cand[h].found / distance for each hand (found false for a
	// hand that is not tracked or found nothing); the nearest within show wins, the right hand on a tie; highlighted when
	// within near (0 inside). Returns the hand (or -1).
	int ChoosePickupHand(const HandTarget cand[2], double near_cm, double show_cm, bool* highlighted);

	// What the caller knows about this vsync (GameFeedback::Poll fills it from CameraDriver and SpatialControls).
	struct Live
	{
		bool armed = false;        // first person is armed and the local record resolved (CameraDriver::LocalRecord)
		bool guards_now = false;   // and this vsync's camera guards held, not just the disarm grace after they failed
		u32 record = 0;            // the local character's record
		bool screen_up = false;    // a game screen is up (the item screen, map, pause, a take-it prompt): Circle answers it
		bool weapon_raised = false; // the weapon is up: Circle fires
		bool hand_game = false;    // the running game is File #1 (SLPM-65428), the only one hand targeting is for
		bool net_offline = false;  // CameraDriver::NetworkOffline: not online, not private co-op
	};

	// CPU thread, every vsync from GameFeedback::Poll with profile null when there is no VR session or no profile. Does
	// nothing without a reach block, with the PickupCue setting off or while first person is not armed (then the next
	// approach cues afresh). Not evaluated (and the state kept, so a prompt declined with Cross is not announced again)
	// while a screen is up or the weapon is raised, nor while the camera guards are in their flicker.
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

	// CPU thread, every vsync from GameFeedback::Poll after Poll. Does nothing (and drops its marker) unless the HandTargeting
	// setting is on, the profile has a reach block, Live says File #1 offline, first person is armed with the camera driving
	// (CameraDriver::GetMapping), no game screen is up and the right hand's grip pose is tracked. Then, every other vsync, it
	// maps the hand into the game, evaluates the Handgun volume (EvaluateHandTarget), publishes the marker, ticks the right
	// hand once when the hand enters the volume (not again until it has left) and logs: the activation and the mapping's
	// self-check, a target found or lost, and once a second the hand, the distances and the projection check. Reads only.
	void PollHandTarget(const ProfileDB::Profile* profile, const Live& live, u64 now_ms, float haptic_strength);

	// The selected volume as the compositor draws it (any thread): its eight corners in the XR base space, metres (the floor
	// quad's four at the lowest height, then the same four at the highest), and how near the hand is: 0 more than 30 cm from
	// the volume, 1 within 30 cm, 2 inside it. node is the node's address (what the image shows). False with no target, with
	// hand targeting off or when the last publish is over 250 ms old.
	struct TargetMark
	{
		float corner[8][3] = {};
		int glow = 0;
		u32 node = 0;
	};
	bool GetTargetMark(TargetMark* out);

	// Overlap and height rules, ties and the walk's safety on synthetic trigger lists in scripted RAM (the bathroom Green
	// Herb's measured numbers among them), the once-per-approach rules, the toast text and the shipped File #1 block;
	// logs "(VR) Pickup cue self-test: passed (N checks ...)" or "... FAIL: <first check>". Run by tests/vr's vr_selftests
	// (CI); nothing in the emulator calls it.
	bool SelfTest();
}
