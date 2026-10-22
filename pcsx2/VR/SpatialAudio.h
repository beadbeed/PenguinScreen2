// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

// [VR] SpatialAudio (File #1, offline): positioned sounds pan round the whole head. The game pans a sound by where it
// lands across the screen (FUN_001B7B50: the emitter through the view and the screen matrix, x 0-640 -> pan 0-127, a
// sound behind mirrored), so only sounds inside the ~52 deg view pan at all. A code cave (camera.silence entries with
// whenSetting: SpatialAudio, at 0xFC000; the site is the jal 0x191A10 at 0x1B7C28) runs the game's projection as before,
// then, while this host block at 0xFF400 holds the magic 'SPA1', replaces x with 322.5 + 320 * sign * sin(azimuth) of the
// emitter in view space (the view 0x3062E0, which follows the head in first person) and w with +1: straight ahead and
// straight behind centred, beside you fully left or right. Volume is the game's own (distance only). The block also
// keeps the cave's last values (the game's x and w, ours, xv, zv, a count) for the log.
namespace VR::SpatialAudio
{
	// The pan law (pure): the x the cave writes for view-space (xv, zv), the camera looking down -z.
	float PanX(float xv, float zv, float sign);
	// The game's pan byte for an x (FUN_001B7B50: clamp 0-640, / 5.0394).
	int PanByte(float x);
	// CPU thread, every vsync from CameraDriver::Apply: live = first person armed this vsync.
	void Poll(u32 crc, bool live);
	int SelfTest(); // failures
} // namespace VR::SpatialAudio
