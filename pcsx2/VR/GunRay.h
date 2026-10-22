// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include "VR/VRInputState.h"

#include <string>
#include <vector>

// [VR] GunRayAim: a shot hits along the VR gun's own ray (tools/outbreak-vr/research/2026-10-14-vr-melee.md section 11).
//
// What the game does (File #1): the shot FUN_0062CF10 takes the gun's weapon object (the character's hand), puts the
// bullet's origin at the muzzle moved back along the barrel (0x6E6D30: 35 cm for a pistol in Kevin's hands) and its frame
// on the barrel, and calls the attack spawn FUN_005521B0, which makes a type-1 attack object there (life 3). The frame's
// hit test runs the gun's attack node on it: a capsule 10 m along the object's +Z, r 24 cm, against every enemy's hurt
// spheres; on a miss the object adds a second node (attack id + 256: a line test of r 2 m, type 4). Damage, reaction and
// sound follow as for any hit.
// What this does: every input poll, with a gun equipped, it writes the VR gun's ray in the game's space (the laser's: the
// gun model's muzzle and barrel, CameraDriver::GameFromVr) into a block in free RAM; cave I (the profile's camera.silence
// entries with whenSetting: GunRayAim) hands that frame to the spawn instead of the game's own, for the local record only.
// So the character's facing and the aim pitch no longer decide where a bullet goes. Strictly offline: online the room's
// master console decides every hit from the aim the partners are sent (the character's heading and pitch), so a VR ray
// would apply only on one console and disagree with what the others show.
namespace VR::GunRay
{
	// The block cave I reads (main RAM, the profile's melee.gunRay.block): +0x00 magic, +0x04 the game counter stamp (u8),
	// +0x08 the local record, +0x0C shots taken along it (cave I counts), +0x10 the 4 x 4 matrix (row vectors; row 2 the
	// barrel), +0x50 the origin.
	constexpr u32 kBlockBytes = 0x60;
	constexpr u32 kOffMagic = 0x00, kOffStamp = 0x04, kOffRecord = 0x08, kOffShots = 0x0C, kOffMatrix = 0x10, kOffOrigin = 0x50;

	// A point map vr -> game (CameraDriver::GameFromVr with the latest mapping).
	using PointMap = bool (*)(const void* ctx, const float vr[3], float game[3]);

	// The ray in the game's space from the ray in the XR base space (origin, metres; dir, any length): the origin mapped,
	// the direction as the difference of the origin and a point a metre along it, mapped, normalised. False when the map
	// fails or the direction is degenerate.
	bool RayToGame(const float origin_vr[3], const float dir_vr[3], PointMap map, const void* ctx, float origin_g[3],
		float dir_g[3]);

	// The frame cave I hands the spawn, as FUN_0062CF10 builds its own: rows 0-2 a right-handed orthonormal basis with
	// row 2 the barrel (dir_g, normalised; row 1 as near the game's +Y as it can be), row 3 (0, 0, 0, 1); the origin the
	// muzzle moved by back (game units, the game's own offset, negative: behind the muzzle) along the barrel. False on a
	// zero or non-finite direction, a non-finite muzzle or back.
	bool ShotFrame(const float muzzle_g[3], const float dir_g[3], float back, float out_matrix[16], float out_origin[3]);

	// RayToGame and ShotFrame on scripted maps and rays. Returns the failed checks.
	std::vector<std::string> SelfTest();

	using GuestReader = bool (*)(u32 address, u8 width, u32* value);

	// CPU thread, every input poll (after PhysicalMelee::Poll). Writes the block while it may (the setting, strictly
	// offline, first person with a steady local record, a gun equipped and the right hand tracked); clears it otherwise.
	// Logs the shots cave I sends along the ray.
	void Poll(const VRInputSnapshot& snap, GuestReader reader, float dt);

	// CPU thread: a loaded state's RAM has its own block; it is cleared or written afresh by the next poll.
	void OnStateLoaded();
}
