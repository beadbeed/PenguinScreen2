// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

// Controller haptics for profiles with a feedback: block. Shots, hits and low health are read from game
// memory (reads only, so the same online) and felt in the VR controllers; the game's own DualShock rumble
// is forwarded too. Nothing here runs without a live VR session.
namespace VR::GameFeedback
{
	// CPU thread, every vsync right after CameraDriver::Apply (it reads the record Apply resolved).
	void Poll();

	// CPU thread, from InputManager::SetPadVibrationIntensity with the pad's motor levels (0-1). Acts only
	// on a change, only for the pad port of the profile's VR Gamepad and only while forwarding is on.
	void OnPadRumble(u32 pad, float large_motor, float small_motor);

	// From InputManager::PauseVibration: stops forwarded rumble; the next level the game sends restarts it.
	void PauseRumble();

	// CPU thread: a loaded state has its own HP and counters, so the next reads are a new baseline.
	void OnStateLoaded();

	// Feeds synthetic HP and shot-counter sequences through the cue rules and checks the queued pulses.
	// Runs once from Poll when PCSX2_VR_FEEDBACK_SELFTEST is set.
	bool SelfTest();
}
