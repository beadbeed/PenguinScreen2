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

	// [VR] GunObjectAim (on top of GunRayAim; section 13 of the vr-melee note): FUN_0062CF10 also takes the muzzle flash
	// (0x5AC8D0), the shell (0x55E400) and 0x6801A0 from the gun's weapon object (s0 at 0x62CF98: matrix +0x1C0,
	// position +0x200). Cave O (0xFB000) hands the shot a fake object laid over the object block (fake + 0x1C0 = block
	// + 0x40), so all of them come from the VR gun. The block (main RAM, the profile's melee.gunObject.block): +0x00 magic,
	// +0x04 the counter stamp (u8), +0x08 the local record, +0x0C shots cave O took over (cave O counts), +0x10 the game's
	// flashes seen for that record (cave O's flash stub counts), +0x14 the flash's origin as the game passed it, +0x40 the
	// fake object's matrix (rows 0-2 the rotation, row 3 (position, 1)), +0x80 its position.
	constexpr u32 kObjBlockBytes = 0x90;
	constexpr u32 kObjOffMagic = 0x00, kObjOffStamp = 0x04, kObjOffRecord = 0x08, kObjOffShots = 0x0C, kObjOffFlashes = 0x10,
				  kObjOffFlashPos = 0x14, kObjOffMatrix = 0x40, kObjOffPos = 0x80;
	// The fake object's base: the block + 0x40 - 0x1C0 (only +0x1C0 and +0x200 of it are read).
	constexpr u32 kObjFakeBias = 0x180;

	// The weapon object whose shot is shot_m / muzzle_g as FUN_0062CF10 builds it. The game turns the identity by
	// FUN_0018FB70(-pi/2) (A = A x Ry: rows (c, 0, -s), (0, 1, 0), (s, 0, c)) and FUN_0018FAD0(+pi/2) (A = A x Rx:
	// rows (1, 0, 0), (0, c, s), (0, -s, c)), then times the object's matrix: frame = Rot x M with Rot rows (0, -1, 0),
	// (0, 0, 1), (-1, 0, 0). So M's rows are (-f2, -f0, f1). The muzzle is local (the weapon type's +0x38) x M plus the
	// position, so the position is muzzle_g - local x M. Row 3 is (position, 1) (the shell reads row 3 as its position).
	// False on a non-finite input.
	bool ObjectFrame(const float shot_m[16], const float muzzle_g[3], const float local[3], float out_matrix[16],
		float out_pos[3]);

	// The evidence that the game's own muzzle flash came from the VR gun (late review m1, m4). Cave O counts the shots it
	// took over (+0x0C); the flash stub counts every flash for the record (+0x10), whether or not cave O swapped that
	// shot. A flash counts as the VR gun's only when cave O's shot count went up in the same poll or within
	// kSameShotMs before it (both run inside one FUN_0062CF10 call). After a falling-back shot, a skipped flash (not
	// aiming) or no shot yet, there is no evidence, and the hands draw their own flash.
	struct FlashEvidence
	{
		static constexpr u64 kSameShotMs = 100;
		static constexpr u64 kSuppressMs = 150; // the hands' own flash lasts 45 ms after the shot
		enum class Seen
		{
			Nothing,
			VrFlash,    // a flash from the VR muzzle (cave O took the shot)
			OtherFlash, // a flash counted for the record without cave O's shot (the stub alone)
		};
		u32 shots = 0, flashes = 0;
		u64 shot_ms = 0, flash_ms = 0;
		// (Re)armed: the block's counts start from zero.
		void Reset() { *this = FlashEvidence{}; }
		Seen Update(u64 now_ms, u32 shots_now, u32 flashes_now);
		// The hands leave out their own flash now.
		bool Suppress(u64 now_ms) const { return flash_ms != 0 && now_ms >= flash_ms && now_ms - flash_ms <= kSuppressMs; }
	};

	// True while cave O's block is armed and the game's own muzzle flash for the latest shot came from the VR gun
	// (FlashEvidence): the hands then draw no flash of their own.
	bool GameFlashOnVrGun();

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
