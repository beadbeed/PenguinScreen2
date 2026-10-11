// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include "VR/VRInputState.h"

#include <array>

// Breaking free of a grab. While the game holds the local character (a zombie's grab in Outbreak; the camera driver
// reads it from the profile's breakout block and publishes it with SpatialControls::SetGrabbed), shaking either
// controller drives what the game counts as struggling: the left stick waggled left and right, and Circle and Cross
// pressed in turn. This is the shake detector and the press schedule, pure so the Gamepad self-test can run them on
// scripted input; SpatialControls::ApplyGrabBreakout ORs the presses into the Gamepad's controls (never letting go of a
// button the player holds), and VRInputSource buzzes the shaking hand for each press. Nothing is pressed without the
// grab, and the presses stop the poll it ends.
namespace VR::GrabBreakout
{
	// The profile's breakout tuning (ProfileDB parses it; these defaults are the shipped values).
	struct Params
	{
		// Shaking: either hand's grip moving faster than shake_speed (metres per second), or reversing direction
		// `reversals` times within reversal_window_s while moving at least reversal_speed (0 reversals turns that test
		// off). A hand counts as shaking for hold_s after its last sign of it, so the turnaround at the end of each
		// stroke doesn't stop the presses.
		float shake_speed = 1.0f;
		u8 reversals = 3;
		float reversal_window_s = 0.30f;
		float reversal_speed = 0.25f;
		float hold_s = 0.25f;
		// While grabbed and shaking: the left stick's X at +1 then -1, stick_hz full cycles a second (0: no waggle);
		// Circle and Cross in turn, press_hz presses a second, Circle first (0: no presses), each held press_s.
		float stick_hz = 6.0f;
		float press_hz = 8.0f;
		float press_s = 0.06f;
		// A buzz of this amplitude (0-1, 0: none) and length on each shaking hand for every press.
		float buzz = 0.35f;
		float buzz_s = 0.02f;
	};

	// A hand that has never shaken (or lost tracking): far longer ago than any hold.
	static constexpr float kNeverS = 1.0e9f;

	// One hand's shake detector.
	struct HandTrack
	{
		bool have_pos = false;
		std::array<float, 3> pos{}; // the grip position as it last changed
		float since_move_s = 0.0f;  // since it changed
		std::array<float, 3> vel{}; // the velocity in use (m/s): the runtime's, else from the position's changes
		bool have_dir = false;
		std::array<float, 3> dir{}; // the last velocity fast enough to count a reversal against
		// Seconds since each recent reversal, newest first; reversal_count of them are in use.
		std::array<float, 8> reversal_age{};
		u8 reversal_count = 0;
		float since_shake_s = kNeverS; // since this hand last showed a shake
	};

	struct State
	{
		std::array<HandTrack, 2> hand{};
		bool active = false; // grabbed and shaking: the presses run
		float t = 0.0f;      // seconds since they began
		int press = -1;      // the press last begun: Circle on even, Cross on odd
		// Set by SpatialControls::ApplyGrabBreakout every poll, for VRInputSource: the buzz amplitude for a press begun
		// this poll on each hand (0: none) and its length.
		std::array<float, 2> buzz{};
		float buzz_s = 0.0f;
		void Reset() { *this = State{}; }
	};

	struct Output
	{
		bool active = false;           // grabbed and a hand shaking: what follows is due this poll
		float stick_x = 0.0f;          // the waggle, +1 or -1 while active (0 with stick_hz 0)
		bool circle = false;           // a Circle press is on
		bool cross = false;            // a Cross press is on
		bool press_began = false;      // and it began this poll
		std::array<bool, 2> shaking{}; // each hand's own shake, grabbed or not
	};

	// One input poll. Both hands are tracked every poll (a shake already going when the grab begins counts at once);
	// grabbed says whether anything may be pressed. dt is clamped to 0-0.25 s.
	Output Step(const VRInputSnapshot& snapshot, const Params& p, bool grabbed, float dt, State& st);

	// The camera driver publishes the grab every vsync, so one older than kStaleMs is from before a pause, a stall or the
	// end of the game, and counts as not grabbed. published_ms 0 is never fresh.
	static constexpr u64 kStaleMs = 250;
	bool Fresh(u64 published_ms, u64 now_ms);
}
