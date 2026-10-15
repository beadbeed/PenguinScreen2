// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include "VR/VRInputState.h"

#include <string>
#include <vector>

// [VR] PhysicalMelee: swinging a melee weapon with your own arm (tools/outbreak-vr/research/2026-10-14-melee.md).
//
// What the game does (File #1): with the weapon raised, Circle starts the character's swing (sub-action 3); the swing's
// motion spawns an attack object at one frame and ends it at a later one, and while it lives the game tests a capsule (a
// pipe, a pole) or a sphere (a knife) on the weapon's own world matrix against the enemies, stages the damage and plays
// the enemy's hit reaction. What this does:
//   - a deliberate swing of the right controller (the speed of a point up its handle, over a threshold along a stroke
//     that goes somewhere, one stroke one swing) presses the game's own attack on the player's own pad: Circle, with R1
//     held first if the weapon is down (the game swings only from the raised stance);
//   - every input poll it writes where the VR weapon is, in the game's space, into a block in free RAM; a code cave the
//     profile installs (camera.silence entries with whenSetting: PhysicalMelee) makes the local player's attack object
//     follow that block instead of the canned swing, so the game's own hit test runs where your hand holds the weapon;
//   - a short tick when the game's swing starts, a firmer buzz when an enemy's HP drops during it.
// Never while a game screen is up, while grabbed or hurt, and never online (not even private co-op): the block and the
// cave are offline only, and so are the presses. Only the player's own pad and that block are ever written.
namespace VR::PhysicalMelee
{
	// --- The swing detector: pure, so the self-test can run it on scripted hands. ---

	struct DetectorParams
	{
		// The point whose speed is measured: this far up the controller's handle (the pointing frame's +Y, where the game
		// holds a shaft through the fist), metres. Its speed includes the wrist's turn, as a weapon's head does.
		float tip_m = 0.40f;
		// A stroke starts when that point moves faster than speed_on (m/s, smoothed) and ends once it is slower than
		// speed_off for end_s.
		float speed_on = 2.2f;
		float speed_off = 0.8f;
		float end_s = 0.08f;
		// It counts as a swing (one request) once, within the stroke, the point has travelled travel_m and its straight-line
		// displacement since the stroke began is at least straightness times that path: a waggle or a shake (breaking free of
		// a grab) goes back and forth and never counts.
		float travel_m = 0.20f;
		float straightness = 0.6f;
		// A stroke longer than this without counting is not a swing (a slow sweep); it has to end before another can start.
		float stroke_max_s = 0.6f;
		// The velocity is smoothed over about this long (an exponential, seconds).
		float smooth_s = 0.025f;
		// A jump of the point by more than this in one poll is a tracking glitch, not a swing: the stroke is dropped.
		float jump_m = 0.8f;
	};

	struct DetectorIn
	{
		float dt = 0.0f;           // seconds since the last Step; clamped to 0-0.25
		bool valid = false;        // the hand's pose is tracked this poll
		float tip[3] = {};         // the measured point, metres, in the XR base space
		bool armed = false;        // a swing may count now (a melee weapon, first person, no screen, not grabbed)
	};

	struct DetectorState
	{
		bool have_prev = false;
		float prev[3] = {};
		float vel[3] = {};          // smoothed, m/s
		bool in_stroke = false;
		bool counted = false;       // this stroke already made its request
		float stroke_s = 0.0f;
		float slow_s = 0.0f;
		float path_m = 0.0f;
		float start[3] = {};
		void Reset() { *this = DetectorState{}; }
	};

	struct DetectorOut
	{
		bool swing = false;         // true for exactly one Step per counted stroke
		bool in_stroke = false;
		float speed = 0.0f;         // smoothed speed this poll, m/s
		float path_m = 0.0f;        // path of the stroke so far
	};

	// One poll. Never fails: NaNs, missing poses and nonsense params are tolerated (params clamped in a copy). While not
	// armed nothing counts, but strokes are still tracked, so a stroke begun before arming never counts half-way.
	DetectorOut Detect(const DetectorIn& in, const DetectorParams& params, DetectorState& st);

	// --- The attack driver: a swing request to the game's own attack input. Pure. ---

	struct DriverParams
	{
		// Circle is held this long (the game reads it at 30-60 Hz), and at most press_max_s waiting for the swing to show.
		float press_s = 0.10f;
		float press_max_s = 0.30f;
		// With the weapon down, R1 is held for at most raise_max_s waiting for the aim stance; given up after.
		float raise_max_s = 0.9f;
		// The game's swing is followed for at most swing_max_s (a pipe's whole swing is 1.43 s, a brush's 1.87 s).
		float swing_max_s = 2.5f;
		// An enemy's HP dropping up to contact_s after the swing ended still counts as this swing's contact.
		float contact_s = 0.25f;
		// After a swing, a new request is taken only cooldown_s later.
		float cooldown_s = 0.25f;
	};

	enum class Phase : u8
	{
		Idle,
		Raising,   // R1 held, waiting for the aim stance (the weapon was down)
		Pressing,  // Circle held, waiting for the game's swing
		Swinging,  // the game's swing runs (R1 still held if we raised the weapon)
		Cooldown,  // the swing is over; contact still counts for contact_s, new requests wait for cooldown_s
	};
	const char* PhaseName(Phase phase);

	enum class Result : u8
	{
		None,
		Swung,        // the game's swing ran (with or without contact)
		NotRaised,    // R1 was held and the aim stance never came (another pad holds something, or the game refused)
		NoSwing,      // Circle was pressed and the game never swung (the weapon broke, a prompt, or it was busy)
		Screen,       // a game screen opened
		Lost,         // first person, the weapon or the stance went away (hurt, grabbed)
	};
	const char* ResultName(Result result);

	struct DriverIn
	{
		float dt = 0.0f;
		bool request = false;   // Detect's swing this poll; taken in Idle only
		bool usable = false;    // first person, a melee weapon, the setting on, offline, stance normal or raised
		bool screen = false;    // a game screen is up
		bool raised = false;    // stance raised (the game's attack works from here)
		bool aim_idle = false;  // ... and its sub-action is aim idle (Circle starts a swing now)
		bool swinging = false;  // the game's melee swing (sub-action attack) runs
		bool hp_drop = false;   // an enemy's HP dropped this poll
	};

	struct DriverOut
	{
		Phase phase = Phase::Idle;
		bool hold_attack = false;  // the pad's attack button (Circle) reads pressed
		bool hold_raise = false;   // the pad's raise button (R1) reads pressed
		bool started = false;      // a request was taken this poll
		bool live = false;         // the game's swing started this poll (the cue)
		bool contact = false;      // this swing's first contact, this poll (the buzz)
		bool finished = false;     // an attempt ended this poll: result, seconds
		Result result = Result::None;
		float latency_s = 0.0f;    // with live: from the request to the game's swing
	};

	struct DriverState
	{
		Phase phase = Phase::Idle;
		float phase_s = 0.0f;
		float since_request_s = 0.0f;
		bool we_raise = false;     // R1 is ours for this attempt
		bool saw_swing = false;
		bool contact = false;
		void Reset() { *this = DriverState{}; }
	};

	DriverOut Step(const DriverIn& in, const DriverParams& params, DriverState& st);

	// --- The weapon's pose in the game's space. Pure. ---

	// The game's object matrix (4 x 4 row vectors, rows the model's x, y, z axes as unit vectors in game space, then the
	// position and 1) from the model's frame in the XR base space (HandModel::ModelFrame) and a point map vr -> game
	// (CameraDriver::GameFromVr with the latest mapping): each axis is mapped as the difference of two mapped points a
	// centimetre apart, so the map's yaw, scale and handedness carry over exactly. False when the map fails or an axis
	// comes out degenerate.
	using PointMap = bool (*)(const void* ctx, const float vr[3], float game[3]);
	bool GameMatrix(const float origin_vr[3], const float axes_vr[3][3], PointMap map, const void* ctx, float out[16]);

	// The detector on scripted strokes (an overhead swing, a side swing, a stab, slow sweeps, a shake, tracking loss and
	// glitches, a stroke begun before arming), the driver on a scripted game (a raised swing, a lowered one, a swing that
	// never starts, a screen, contact inside and after the swing, the cooldown), and GameMatrix on a turned, offset map.
	// Returns the name of every failed check; empty is a pass.
	std::vector<std::string> SelfTest();

	// --- The glue (VRInputSource::PollEvents). ---

	using GuestReader = bool (*)(u32 address, u8 width, u32* value);

	struct Frame
	{
		int pad_port = -1;           // the pad the presses go to (the profile's VR Gamepad); -1: none this poll
		bool hold_attack = false;    // the Gamepad's controls bound to attack_button read 1
		bool hold_raise = false;     // the Gamepad's controls bound to R1 read 1
		std::string attack_button;   // melee.attackButton ("Circle")
	};

	// CPU thread, every input poll, after ReloadDriver::Poll and before the controls compose. reader reads guest memory
	// (null: main RAM); dt in seconds. Writes the override block (main RAM, offline only), plays the cues, logs.
	Frame Poll(const VRInputSnapshot& snap, GuestReader reader, float dt);

	// CPU thread: a loaded state has its own enemies; the HP baseline starts again.
	void OnStateLoaded();
}
