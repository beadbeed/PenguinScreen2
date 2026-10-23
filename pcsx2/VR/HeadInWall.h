// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include <functional>
#include <vector>

// [VR] HeadInWall: the VR eye stops short of the game's own walls. A read-only host copy of Outbreak File #1's player
// collision (tools/outbreak-vr/research/2026-10-22-native-feel.md, "Head in wall"): the room's wall grid A (header
// 0x3938A8: cell sizes, counts, origin, a cell array of face lists ending 0xFFFFFFFF) and the moving faces (0x70E750,
// count 0x23BEC4), each face 0x38 bytes (flags, three vertices, the normal and plane d). A face blocks the player as the
// game's filter FUN_005E6050 says for the player mask (attribute bit 0x1; not in the global ignore mask 0x3938E8; a door
// wall, class 1, passes while its door reads open). The segment from the character's head to the VR eye is tested
// against every blocking face the way FUN_005491E0 does (one-sided: from its front, crossing its plane, inside the
// triangle), the eye is put back `margin` before the nearest hit, then kept `radius` off any face near it; the floor under
// it (grid B, the highest upward face below) keeps it above the floor. Nothing is written to the game except the eye
// first person writes anyway. Game units (cm).
namespace VR::HeadInWall
{
	struct Vec3
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
	};

	struct Face
	{
		u32 flags = 0;
		Vec3 v[3];
		Vec3 n;
		float d = 0.0f; // n.p + d = 0 on the plane
	};

	// One game face from its 0x38 bytes (false: not finite or no unit normal).
	bool DecodeFace(const u8* bytes, Face* out);

	// FUN_005491E0's test, one-sided: a in front of the face (n.a + d >= 0), b behind it, the crossing inside the
	// triangle. t: where along a->b (0-1).
	bool SegmentHitsFace(const Face& f, const Vec3& a, const Vec3& b, float* t);
	// The point of the triangle nearest p.
	Vec3 ClosestPointOnFace(const Face& f, const Vec3& p);

	struct Params
	{
		float margin = 9.0f;  // the eye stops this far before a wall it would cross
		float radius = 10.0f; // and keeps this far from any wall face near it
		float floor_clear = 20.0f;
		float fade_from = 15.0f; // the view darkens once the head is this far past where the eye stops
		float fade_full = 35.0f; // to fade_max here
		float fade_max = 0.85f;
	};

	struct Result
	{
		Vec3 eye;
		bool clamped = false;
		float pushed = 0.0f; // how far the eye was moved (cm)
		int hit_face = -1;   // the face the segment hit, or -1
	};

	// The clamp on a set of blocking faces (pure): head -> wanted eye.
	Result Clamp(const std::vector<Face>& walls, const std::vector<Face>& floors, const Vec3& head, const Vec3& eye,
		const Params& p);
	float FadeFor(float pushed, const Params& p);

	// Grid parsing through a reader (address, bytes, out) -> false when unreadable. A grid: the header at `base`, every
	// face of every cell once (by address), checked: 1 <= nx, nz <= 32, face lists end within 4096 entries, faces decode.
	using Reader = std::function<bool(u32 address, u32 size, void* out)>;
	struct Grid
	{
		s32 cell_x = 0, cell_z = 0;
		u32 nx = 0, nz = 0;
		s32 origin_x = 0, origin_z = 0;
		std::vector<u32> face_addr;
		std::vector<Face> faces;
	};
	bool ReadGrid(const Reader& read, u32 base, Grid* out);

	// The masks the game's own callers pass the face filter FUN_005E6050 (its maskA: the class bits low, one bit per class
	// 0-15; the attribute bits high, compared with the face's flags >> 16; maskB 0 in all three). Verified statically on
	// ram_now.bin (tools/outbreak-vr/research/2026-10-22-line-of-sight.md):
	// - the player's movement: attribute 0x1 (rec+0x190 = 0x0001FFFF);
	// - the attack capsule's wall test (FUN_005AAE60 at 0x5AB154, when the attack params' +0x0C has bit 0x10, which every
	//   gun and melee attack in File #1's table 0x6E7C90 has): attribute 0x2. A hit shortens the capsule to the wall before
	//   the enemy test, so bullets stop there;
	// - the zombies' sight (the perception cache FUN_006A3B60 at 0x6A3F04, any-hit FUN_00549860 from the character's chest,
	//   bone 11, to the enemy's sight bone): attribute 0x4.
	// File #1's faces carry attribute 0x2F (room walls: every bit), 0x29 (furniture and counters: the player and enemies
	// only, so sight and shots pass), 0x27 (the moving faces) and the door walls 0x12F / 0x22F.
	constexpr u32 kMaskPlayer = 0x0001FFFFu;
	constexpr u32 kMaskShot = 0x0002FFFFu;
	constexpr u32 kMaskSight = 0x0004FFFFu;

	// FUN_005E6050 with maskB 0 (pure): whether a face with these flags counts for mask_a. door_open(slot) says whether that
	// door reads open (a class-1 face then passes unless its attribute has 0x40).
	bool FaceCounts(u32 flags, u32 mask_a, u32 ignore_mask, const std::function<bool(u32 slot)>& door_open);
	// The player's filter (FUN_005E6050 with mask 0x0001FFFF).
	bool BlocksPlayer(u32 flags, u32 ignore_mask, const std::function<bool(u32 slot)>& door_open);

	// Pure: the nearest face of `faces` that counts for mask_a and that a -> b crosses (SegmentHitsFace, one-sided as the
	// game's ray), or -1; *t where along a -> b. -1 for a non-finite a or b.
	int SegmentBlockedBy(const std::vector<Face>& faces, u32 mask_a, u32 ignore_mask,
		const std::function<bool(u32 slot)>& door_open, const Vec3& a, const Vec3& b, float* t);

	// Live (CPU thread): whether the line a -> b (game cm) crosses a face of the room's wall grid or the moving faces that
	// counts for mask_a (kMaskSight: what the zombies cannot see through; kMaskShot: what stops a bullet). Its own copy of
	// the grid, kept per room (not HeadInWall's switch). known false (blocked false) when it cannot tell: not File #1, not
	// in game, the grid unreadable, a point not finite.
	struct LineResult
	{
		bool known = false;
		bool blocked = false;
		u32 flags = 0;    // the face that blocks
		float t = 0.0f;   // where along a -> b
	};
	LineResult LineBlocked(u32 crc, const float a[3], const float b[3], u32 mask_a);

	// Live (CPU thread): File #1's character record's chest (bone 11, the model at record +0x34, its bone matrix pointers
	// from +0x820, the position at the matrix's +0x30): the point the game's own sight starts from. False if it does not
	// read as a finite point within 3 m of the record's position.
	bool CharacterChest(u32 crc, u32 record, float out[3]);

	// Live (CPU thread, from CameraDriver::ApplyLookAt while first person writes the eye): File #1 only, the switch on.
	// head: the character's head (its position + the eye height); eye in, the stopped eye out. Returns true when it ran.
	bool Apply(u32 crc, const float head[3], float eye[3]);
	// The view dim it wants now (0-1, eased), for HudCards' view dim.
	float CurrentFade();
	void Reset();

	int SelfTest(); // failures
} // namespace VR::HeadInWall
