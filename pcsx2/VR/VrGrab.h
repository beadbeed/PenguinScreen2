// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include "VR/VRInputState.h"

#include <array>
#include <bitset>

// Zombie grabs that fit VR, through the game's own systems (File #1 SLPM-65428, [VR] VrGrab;
// tools/outbreak-vr/research/2026-10-27-vr-grab.md has the reverse engineering and the addresses).
//
// - The telegraph (read only): a zombie (update 0x664290) entering its grab lunge (state +0x08 = 2, sub-state +0x09 = 6,
//   motion 413) with the local record as its target (+0x24) buzzes both hands once. The grab connects about 0.23 s
//   later (frame ~13 of the lunge); a game hit on the zombie in that window sends it to its hurt state (0x678750 ->
//   0x6795C0), so the game itself lets a strike interrupt the lunge.
// - The escape: while the local character is held (stance 5), the grabbing zombie is the local record's +0xB24. Its
//   hurt spheres (head, chest) are mapped into the XR space each vsync. A hand thrust into one of them (a shove), a hard
//   thrust (a hard shove, or a strike to the head) or the trigger with the aim ray through the head at point-blank
//   range (a headshot; through the chest a shot) each count as escape progress:
//   * always (pad input, private co-op too): the breakout's own presses (Circle and Cross in turn, the stick waggle)
//     run for burst_s, as if the hand were shaking: the game's own escape counter (0x630FC0) counts them;
//   * offline only: the points come off the game's own escape meter, the low 7 bits of the local record's +0xCDD
//     (30 for a sub-state 1 grab, 0x6E7840), keeping its bit 7 (the knife counter's flag, 0x630E90). A hard shove or a
//     headshot empties it, and the game's own grab handler (0x563AE0) then runs its own successful escape (phase 10:
//     the shove-off, zombie code 4) or, with the knife counter, its counter-stab (phase 20). Nothing else is written:
//     never the zombie's record, a position, the HP or a partner's record.
// Pure parts (Detect, MeterAfter, MeterGate, LungeEdge) run in the self-test; Vsync and InputPoll are the glue.
namespace VR::VrGrab
{
	struct Params
	{
		// The telegraph: both hands, this amplitude (0-1) for cue_s, for a lunge within cue_within game units.
		float cue_amp = 0.7f;
		float cue_s = 0.12f;
		float cue_within = 300.0f;
		// A shove: a hand moving at least shove_speed (m/s) toward a head or chest sphere's centre (the velocity at most
		// toward_deg off that direction) while within reach_m of its surface. hard_speed and up: a hard shove (decisive).
		float shove_speed = 1.4f;
		float hard_speed = 2.4f;
		float toward_deg = 60.0f;
		float reach_m = 0.20f;
		// A shot: the trigger pressed past trigger_on (released below trigger_off first) with that hand's aim ray passing
		// within the sphere's radius + shot_margin_m, its centre at most shot_range_m away. Through the head: a headshot.
		float trigger_on = 0.75f;
		float trigger_off = 0.35f;
		float shot_margin_m = 0.06f;
		float shot_range_m = 1.5f;
		// Escape progress: the breakout's presses run this long after each counted action; offline, the meter points it
		// takes (the game counts 1 for a direction press and 2 for Circle or Cross, out of 30).
		float burst_s = 0.5f;
		u8 shove_points = 10;
		u8 shot_points = 10;
		// One counted action per hand per cooldown_s.
		float cooldown_s = 0.4f;
	};

	static constexpr int kMaxSpheres = 4;

	// The grabbing zombie's hurt spheres in the XR base space (metres).
	struct Target
	{
		bool valid = false;
		int count = 0;
		std::array<std::array<float, 3>, kMaxSpheres> c{};
		std::array<float, kMaxSpheres> r{};
		std::array<bool, kMaxSpheres> head{};
	};

	enum class Kind : u8
	{
		None,
		Shove,     // a hand into the head or chest
		HardShove, // the same, hard (into the head: a strike) - decisive
		Shot,      // the trigger, the ray through the chest
		HeadShot,  // the trigger, the ray through the head - decisive
	};
	const char* KindName(Kind k);
	bool Decisive(Kind k);

	struct Event
	{
		Kind kind = Kind::None;
		int hand = -1;
		int sphere = -1;
		float speed = 0.0f; // m/s (a shove)
		float dist = 0.0f;  // metres: from the surface (a shove) or along the ray (a shot)
	};

	struct HandTrack
	{
		bool have_pos = false;
		std::array<float, 3> pos{};
		float since_move_s = 0.0f;
		std::array<float, 3> vel{};
		float cooldown_s = 0.0f;
		bool trigger_armed = false; // released below trigger_off since the last shot
	};

	struct State
	{
		std::array<HandTrack, 2> hand{};
		void Reset() { *this = State{}; }
	};

	// One input poll: at most one event (the first hand that counts). grabbed false (or an invalid target) counts nothing
	// but keeps tracking, so a thrust already under way when the grab begins is measured from real positions.
	Event Detect(const VRInputSnapshot& snapshot, const Target& target, const Params& p, bool grabbed, float dt, State& st);

	// The game's escape meter after points (bit 7 kept; the low 7 bits less points, never below 0, never raised; a
	// decisive action empties them).
	u8 MeterAfter(u8 meter, int points, bool decisive);
	// The meter may be written: held (stance 5) in a grab whose handler reads it (sub-state 1 or 2: 0x563AE0), in the
	// phases it is counted down in (4-6), alive enough for the game's escape (HP 2 and up, as 0x563CBC asks).
	bool MeterGate(u8 stance, u8 sub, u8 phase, s16 hp);

	// One enemy as the telegraph reads it.
	struct EnemyView
	{
		bool present = false;
		bool zombie = false;        // update 0x664290
		u8 state = 0, sub = 0;      // +0x08, +0x09
		bool targets_local = false; // +0x24 is the local record
		float dist = 0.0f;          // game units, planar, to the local character
	};
	// True on the vsync a grab lunge at the local character starts; lunging is that enemy's flag across vsyncs.
	bool LungeEdge(const EnemyView& e, const Params& p, bool& lunging);

	// CPU thread, every vsync with File #1's profile (CameraDriver::Apply). enabled: [VR] VrGrab and first person may
	// run here (net_ok); offline: no network play (the meter is written only then); base: the local record (0 = none);
	// grabbed: the camera driver's verdict for that record (breakout.grabbed).
	void Vsync(bool enabled, bool offline, u32 base, bool grabbed);
	// Input thread, from SpatialControls::ApplyGrabBreakout each poll: the detector on the published target. Returns
	// the poll's event (None mostly); burst_s is extended for one, and the hand to buzz is the event's.
	Event InputPoll(const VRInputSnapshot& snapshot, bool grabbed, float dt, State& st, float* burst_s);

	bool SelfTest();
}
