// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include "VR/SpatialControls.h"
#include "VR/VRInputState.h"

#include <string>
#include <vector>

// Reloading with the hands, for a seated player with a gun raised in one hand (the gun hand, the right by default). The
// other hand reaches to where the magazines are (a spot on the body: the left hip, or the chest pouches; it is a
// parameter), squeezes its grip there to take one, carries it up to the gun, and lets go of the grip (or pulls its trigger)
// there: that is ONE reload request. This is the pure detector, in the style of GrabBreakout: it reads no globals and no
// game memory, so the self-test can run it on scripted hands. What a request does (the game's reload button) and the cues
// for each phase are the caller's; nothing here presses anything.
//
// The gesture, one Step per input poll:
//   Idle      nothing going on (also while not armed, a menu is up, or the gun is down).
//   Reach     the other hand is in the magazine spot with the grip open (a light tick, the way the belt pouches tick).
//   Grabbed   the grip closed inside the spot (a tick: "got a magazine"). The magazine has to reach the gun within
//             move_timeout_s, and the hand has to leave the spot on the way.
//   Ready     the hand is within gun_radius of the gun (a second tick). Letting go of the grip, or a fresh pull of the
//             trigger, now is the reload. Drifting back out of the gun's reach returns to Grabbed.
//   WaitOpen  the gesture ended (reloaded, cancelled or timed out) with the grip still closed: nothing starts again until
//             it has opened, so one squeeze can never reload twice.
// The gesture is cancelled, with the reason in Out, when the gun is lowered, first person stops being armed, a menu comes
// up, the move takes too long, the magazine is held at the gun too long, the grip lets go away from the gun, or a hand's
// tracking is lost for longer than loss_grace_s. A grip that was already closed on the way into the spot, or when the gun
// was raised, or when tracking came back, is not a grab: only a squeeze that closes inside the spot is (as with the Zone
// controls), and so a reload needs the grip to have opened and closed again.
//
// Wiring notes. Step it every input poll, before the Zone controls and the belt pouches read the hands: while
// Out::owns_left_hand is set the magazine hand's squeeze belongs to the reload, and the hip zone (which opens the item
// screen on a squeeze) and the pouches must not see it. The stance (gun_raised) should come from something with its own
// hysteresis: the gun hand moves toward the magazine hand during the gesture, and a stance that flickers there cancels it.
// The spot and the gun are checked in the room frame the hands are in, so pass poses from one frame (HandsFromSnapshot's).
namespace VR::ReloadGesture
{
	struct Params
	{
		// The magazine spot, placed from the head the way a Zone control is (metres: side + right, height + up, forward +
		// ahead; HeadAnchor's position and yaw). Step reads the spot from In (spot_centre and spot_radius), so a caller
		// copies SpotCentre's result and spot_radius there, or passes some other spot instead (a chest pouch's centre). The
		// default is the left hip, where the profile's belt zone is: a hand squeezing there while the gun is raised is
		// reloading, not opening the item screen (see owns_left_hand).
		SpatialControls::Placement spot{-0.22f, -0.50f, 0.12f, 0.0f};
		// A hand within the spot's radius of its centre is in the spot; once in, it stays in until that plus the margin, so
		// a hand on the edge does not tick over and over. (Smaller than the belt zone's 0.20: the zone has to catch every
		// squeeze there, the spot only has to catch the reach for a magazine.)
		float spot_radius = 0.12f;
		float spot_exit_margin = 0.03f;

		// The gun's magazine well is a point gun_drop metres straight down from the gun hand's grip pose, where the
		// magazine goes in under the hand. Straight down in the room frame rather than along the grip pose's own axes, so it
		// does not depend on how a runtime orients that pose. A hand within gun_radius of it is at the gun, and stays there
		// until gun_radius + the margin. The radius is generous: the palm of the hand cupping the gun sits several
		// centimetres from the gun hand's palm, and the tracked grip poses are the palms.
		float gun_radius = 0.15f;
		float gun_exit_margin = 0.04f;
		float gun_drop = 0.04f;

		// The magazine hand's grip counts as squeezed from grip_on, and as open again only at or below grip_off (the Zone
		// controls' numbers), so a noisy analogue value near the threshold cannot grab twice or let go early. The trigger
		// the same: a pull is its rising edge past trigger_on, re-armed below trigger_off.
		float grip_on = 0.6f;
		float grip_off = 0.4f;
		float trigger_on = 0.7f;
		float trigger_off = 0.4f;
		// A fresh trigger pull on the magazine hand at the gun reloads as well as a release does (for a player who would
		// rather click than open a hand). The trigger is never looked at except at the gun with a magazine in hand.
		bool fire_on_trigger = true;

		// From the grab to the gun: longer than this and the magazine is dropped (cancelled; the grip has to open before
		// another try). A hand that leaves the gun's reach and comes back gets the whole time again.
		float move_timeout_s = 1.5f;
		// At the gun with the grip still closed and no pull: dropped after this long, so a hand holding a magazine "in
		// reserve" (or squeezing for some other reason) is not left one release away from a surprise reload.
		float ready_timeout_s = 2.0f;
		// Either hand's pose missing for up to this long mid-gesture is waited out (a hand at the hip can drop out of the
		// headset cameras' view for a moment); longer cancels. Nothing fires while a pose is missing, and a grip that opens
		// meanwhile cancels: the release happened somewhere unknown.
		float loss_grace_s = 0.2f;

		// Which hand holds the gun (VRInputSnapshot::LEFT or RIGHT); the other carries the magazines. Put the spot on the
		// magazine hand's side.
		int gun_hand = VRInputSnapshot::RIGHT;
	};

	enum class Phase : u8
	{
		Idle,
		Reach,
		Grabbed,
		Ready,
		WaitOpen,
	};

	// "Idle", "Reach", "Grabbed", "Ready", "WaitOpen"; for logs.
	const char* PhaseName(Phase phase);

	struct In
	{
		// Seconds since the last Step; clamped to 0-0.25, so a stall is not a timeout.
		float dt = 0.0f;
		// Both hands' grip poses (position and orientation, valid), grip and trigger, as SpatialControls::HandsFromSnapshot
		// gives them, indexed by VRInputSnapshot::LEFT / RIGHT. Only positions, valid, squeeze and trigger are read: the
		// orientation is carried for the caller's convenience. A pose that is not valid, or not finite, counts as lost.
		SpatialControls::Hands hands;
		// First person is armed and driving the camera.
		bool armed = false;
		// A menu is up (the item screen, the map, pause): nothing runs, and a gesture in flight is cancelled.
		bool menu = false;
		// A gun is raised: the stance the camera driver works out. Lowered cancels a gesture in flight.
		bool gun_raised = false;
		// The magazine spot's centre and radius, in the same frame as the hands. A radius of 0 (the default) means no spot:
		// nothing can be grabbed.
		SpatialControls::Vec3 spot_centre;
		float spot_radius = 0.0f;
	};

	struct State
	{
		Phase phase = Phase::Idle;
		float phase_s = 0.0f;      // seconds in Grabbed or Ready, for the timeouts
		float lost_s = 0.0f;       // seconds a pose has been missing mid-gesture
		bool have_prev = false;    // the grip and trigger below were read at least once (so a first poll has no edge)
		bool squeezed = false;     // the magazine hand's grip, with hysteresis
		bool trigger_down = false; // its trigger, with hysteresis
		bool in_spot = false;      // its hand in the magazine spot, with hysteresis
		bool left_spot = false;    // since the grab, its hand has been outside the spot: a spot that overlaps the gun's reach
		                           // cannot be a reload by squeezing and letting go in place
		bool at_gun = false;       // its hand at the gun, with hysteresis
		void Reset() { *this = State{}; }
	};

	struct Out
	{
		// The reload: true for exactly one Step per gesture (the poll the grip opened, or the trigger was pulled, at the gun).
		bool reload_requested = false;
		Phase phase = Phase::Idle;
		// The phase differs from the previous Step's: the poll to play a cue on (a tick for Reach, Grabbed and Ready).
		bool phase_changed = false;
		// Grabbed, Ready or WaitOpen: the magazine hand's grip belongs to the reload, so the Zone controls and the belt
		// pouches should not read a squeeze from it this poll.
		bool owns_left_hand = false;
		// What happened this poll, when something did (a phase change, a reload, a cancel with its cause); "" otherwise.
		// A string literal, for logs.
		const char* reason = "";
	};

	// One input poll. Never fails: a missing pose, NaNs, or nonsense in params are tolerated (params are clamped to ranges
	// the detector works in, in a copy).
	Out Step(const In& in, const Params& params, State& st);

	// The magazine spot's centre in the hands' frame, from the head anchor (SpatialControls::HeadAnchor) and params.spot.
	SpatialControls::Vec3 SpotCentre(const SpatialControls::Anchor& head, const Params& params);

	// The detector on scripted hands: a clean reload (right- and left-handed), a too-slow move, the gun lowered or a menu
	// raised mid-gesture, a re-trigger blocked until the grip opens, grip noise near the thresholds, the gun hand moving
	// while the other approaches, tracking loss mid-gesture, and the edge cases around them. Returns the name of every
	// failed check; empty is a pass.
	std::vector<std::string> SelfTest();
}
