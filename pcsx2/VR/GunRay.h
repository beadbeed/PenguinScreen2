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

	// Late review m6 (cave I): the shot of a VR muzzle beyond a wall. The game's faces are one-sided (they face the room),
	// so a shot that starts behind one is not stopped by the capsule's wall test (FUN_005AAE60 at 0x5AB154). Instead the
	// shot starts on the chest's side of the wall, where the chest-to-muzzle line meets it (t along chest -> muzzle, then
	// kWallShotBefore back), and goes along that line into the wall, so the game's own wall test cuts it there.
	// Evening review m3: out_origin is the shot's origin itself (ShotFrame with back 0): kWallShotBefore plus the game's
	// back offset (min(back, 0): a positive one would put it past the wall) before the wall, but at least kWallShotGap off
	// the wall's plane along its normal (normal_g, the face's; a grazing line starts that much further back), and never
	// behind the chest (then at the chest, which is in front of the wall: the line leaves the chest through the face's
	// front). The game's wall test is a segment cast from the origin (FUN_005494C0), so the gap keeps its start clearly in
	// front of the face. False for a non-finite input, t outside (0, 1], chest and muzzle together or a zero normal.
	constexpr float kWallShotBefore = 1.0f; // game cm
	constexpr float kWallShotGap = 10.0f;   // game cm, along the face's normal
	bool WallShot(const float chest_g[3], const float muzzle_g[3], float t, const float normal_g[3], float back,
		float out_origin[3], float out_dir[3]);

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

	// --- Co-op aim ([VR] GunRayAim in private co-op; tools/outbreak-vr/research/2026-10-22-coop-ready.md) ---
	// Online the room's master decides a hit from the character's own aim, so cave I (the VR ray as the shot's frame) stays
	// strictly offline. Instead, with the gun raised, the LOCAL character's heading (+0x92) and aim pitch (+0xBC8), the two
	// things the game's shot pose follows and its type-1 message sends (heading +0x14, pitch +0x20, read at the call, one
	// sent as you fire), are steered so the game's own gun barrel (the equipped item's weapon object: matrix +0x1C0,
	// barrel = -row 0, the model-space muzzle through it) points at the laser's aim point: where the VR ray passes nearest
	// an enemy's hurt sphere it comes within kAimPad of (the closest point on the ray), else kAimFar along it. So the game's
	// own bullet, on this console and on the master's copy, goes where the laser points.
	constexpr float kAimPad = 24.0f;   // game cm: the pistol shot's capsule radius
	constexpr float kAimReach = 1000.0f; // game cm: the shot's 10 m
	constexpr float kAimFar = 600.0f;  // game cm: no enemy on the ray
	// Pure: the aim point on the ray (o, unit d): the closest point to the nearest (smallest t > 0, t <= reach) sphere centre
	// whose distance from the ray is within its radius + pad; else o + far * d. True when a sphere was found.
	bool AimPoint(const float o[3], const float d[3], const float (*centres)[3], const float* radii, int n, float pad, float reach,
		float far_t, float out[3]);
	// Pure: one steering step. desired / actual: (yaw, elevation) radians of the wanted and the game barrel's direction
	// (yaw = atan2(x, z), the game's heading convention; elevation = asin(y)). The command is desired + trim, the trim
	// learning the barrel's offset from the command (gain, clamped to max_trim): cmd -> desired + offset converges.
	struct Steer
	{
		float trim_yaw = 0.0f, trim_elev = 0.0f;
		bool valid = false;
		void Reset() { *this = Steer{}; }
		// learn false: the trim is held (the pose read is not one to learn from), the command still follows desired.
		void Step(const float desired[2], const float actual[2], bool learn, float gain, float max_trim, float out_cmd[2]);
	};
	// The gun's pose against the character, from one read (pure): the barrel line's lateral offset from the character's
	// centre (game cm, + right of the barrel), the muzzle's forward offset along it, and the barrel's yaw less the heading.
	// The gun turns rigidly with the heading, so these hold through a turn and the heading that aims it can be solved.
	struct AimPose
	{
		float lateral = 0.0f, forward = 0.0f, delta = 0.0f;
	};
	// barrel_yaw, heading: radians (atan2(x, z)); gun_rel: the muzzle less the character's position (x, z).
	AimPose PoseFrom(float barrel_yaw, float heading, const float gun_rel[2]);
	// s moves k of the way to now (delta wrapped).
	void SmoothPose(AimPose& s, const AimPose& now, float k);
	// The heading (radians) that puts the barrel's line through target_rel (the target less the character's position,
	// x, z): yaw alpha - asin(lateral / r) for the barrel, less delta. False on non-finite input or a target at the centre.
	bool SolveHeading(const AimPose& p, const float target_rel[2], float* heading, float* lateral);
	// CPU thread (CameraDriver's aim, with the weapon raised): the steered heading (game radians) and elevation (radians,
	// up positive) when co-op aim published one in the last 100 ms. False otherwise: the controller's own aim.
	bool CoopAim(float* yaw, float* elev);
}
