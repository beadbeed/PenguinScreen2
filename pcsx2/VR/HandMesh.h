// SPDX-FileCopyrightText: 2026 PenguinScreen2 contributors
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Types.h"

#include <vector>

// The hand's model: Godot XR Tools' low-poly hand (CC0 1.0; HandMeshData.inc carries the licence and says where it comes
// from), baked by tools/outbreak-vr/convert_hand_gltf.py into four fixed poses per side, as plain triangle data for
// HandModel's rasteriser. 938 triangles and 603 vertices a side, the same vertices in every pose, so two poses blend
// vertex by vertex. No XR or Vulkan types here, so it stays self-contained.
//
// The frame is HandModel's pointing frame, the one HandState::pos and quat place (metres): +X right, +Y up, -Z forward
// along the barrel, the origin at the controller's grip position inside the handle. The right hand's fingers point forward
// (-Z), its thumb is on the +Y side and its palm faces -X; the left hand is the same with X mirrored (the palm faces +X).
// The header of HandMeshData.inc has the rest: where the knuckles, the wrist and the middle of the palm are.
namespace VR::HandMesh
{
	// The same values as HandModel::kLeft and kRight.
	static constexpr int kLeft = 0;
	static constexpr int kRight = 1;

	enum class Pose : u8
	{
		Open,   // the relaxed, slightly curled rest pose of the model
		Grip,   // a fist round a handle or a shaft, the thumb over the fingers
		Pistol, // the index in front of the trigger inside the guard, the other fingers round the grip, the thumb along the frame
		Point,  // the index straight out, the other fingers closed (the menu pointer)
	};
	static constexpr int kPoseCount = 4;

	// A hand's triangles. position and normal are three floats a vertex (metres; unit normals), index three u16 a triangle,
	// counter-clockwise seen from outside. Pointers into static data (or, from Lerp, into its Blend); never null for a mesh
	// that Get returns with vertices in it.
	struct Mesh
	{
		u32 vertex_count = 0;
		u32 triangle_count = 0;
		const float* position = nullptr;
		const float* normal = nullptr;
		const u16* index = nullptr;
	};

	// Where the wrist joint and the index fingertip are in a pose, in the same frame as the mesh, for a cuff at the wrist
	// and a pointing ray from the fingertip.
	struct Anchors
	{
		float wrist[3] = {0.0f, 0.0f, 0.0f};
		float index_tip[3] = {0.0f, 0.0f, 0.0f};
	};

	// The mesh of a hand (kLeft or kRight) in a pose. Valid for the life of the program, any thread. A side or pose out of
	// range gives a mesh with no vertices, which draws nothing.
	const Mesh& Get(int side, Pose pose);
	Anchors GetAnchors(int side, Pose pose);
	// "open", "grip", "pistol" or "point", for the log.
	const char* PoseName(Pose pose);

	// Room for one blended mesh, so a hand that blends every frame allocates once. Not copyable (its mesh points into its own
	// vectors).
	struct Blend
	{
		std::vector<float> position;
		std::vector<float> normal;
		Mesh mesh;

		Blend() = default;
		Blend(const Blend&) = delete;
		Blend& operator=(const Blend&) = delete;
	};

	// The hand t of the way from pose a to pose b (t 0: a, 1: b; clamped, and not a number counts as 0), for a finger curl that
	// follows the grip or the trigger: every vertex moves along a straight line between its two places, the normals are
	// blended and made unit again. A fingertip cuts across the curl on the way, so between two poses far apart (open to grip)
	// the fingers shorten in the middle; blend between near poses, or pick the pose by the nearer end. At the ends, or for the
	// same pose twice, it gives the stored mesh without copying; otherwise the mesh is in scratch (valid until the next Lerp
	// with it). Any thread, with its own Blend.
	const Mesh& Lerp(int side, Pose a, Pose b, float t, Blend& scratch);

	// Checks the baked data: every index in range and no triangle with a repeated vertex, the triangles wound the way the
	// normals face, the normals unit length, every number finite, the hand's size (the open hand 18-22 cm along the way the
	// fingers point, no pose over 22 cm), the left hand the right one mirrored, the anchors where the mesh puts them, and
	// Lerp's ends and middle. Returns how many checks it ran; the name of the first one that failed goes to *first_failed
	// (null when all pass). Logs a line with the verdict as the other self-tests do. Any thread.
	int SelfTest(const char** first_failed = nullptr);
}
