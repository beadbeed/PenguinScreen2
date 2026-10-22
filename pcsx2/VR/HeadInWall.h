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

	// The player's filter (FUN_005E6050 with mask 0x0001FFFF): door_open(slot) says whether that door reads open.
	bool BlocksPlayer(u32 flags, u32 ignore_mask, const std::function<bool(u32 slot)>& door_open);

	// Live (CPU thread, from CameraDriver::ApplyLookAt while first person writes the eye): File #1 only, the switch on.
	// head: the character's head (its position + the eye height); eye in, the stopped eye out. Returns true when it ran.
	bool Apply(u32 crc, const float head[3], float eye[3]);
	// The view dim it wants now (0-1, eased), for HudCards' view dim.
	float CurrentFade();
	void Reset();

	int SelfTest(); // failures
} // namespace VR::HeadInWall
