// SPDX-FileCopyrightText: 2026 PenguinScreen2 contributors
// SPDX-License-Identifier: GPL-3.0

#include "VR/HandMesh.h"

#include "common/Console.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace VR::HandMesh
{
	namespace
	{
		// The baked poses of both hands, with the CC0 notice of the model they come from: kHandVertexCount,
		// kHandTriangleCount, kHandPoseCount and per side k<Side>Index, k<Side>Position, k<Side>Normal and k<Side>Anchor.
#include "VR/HandMeshData.inc"

		static_assert(kHandPoseCount == static_cast<u32>(kPoseCount), "Pose and the baked poses disagree");
		static_assert(kHandVertexCount <= 0xFFFFu, "the indices are u16");

		constexpr Mesh MakeMesh(const float* position, const float* normal, const u16* index)
		{
			return Mesh{kHandVertexCount, kHandTriangleCount, position, normal, index};
		}

		// By side (kLeft, kRight), then Pose.
		constexpr Mesh kMeshes[2][kPoseCount] = {
			{
				MakeMesh(kLeftPosition[0], kLeftNormal[0], kLeftIndex),
				MakeMesh(kLeftPosition[1], kLeftNormal[1], kLeftIndex),
				MakeMesh(kLeftPosition[2], kLeftNormal[2], kLeftIndex),
				MakeMesh(kLeftPosition[3], kLeftNormal[3], kLeftIndex),
			},
			{
				MakeMesh(kRightPosition[0], kRightNormal[0], kRightIndex),
				MakeMesh(kRightPosition[1], kRightNormal[1], kRightIndex),
				MakeMesh(kRightPosition[2], kRightNormal[2], kRightIndex),
				MakeMesh(kRightPosition[3], kRightNormal[3], kRightIndex),
			},
		};

		constexpr Mesh kNoMesh = {};

		bool Valid(int side, Pose pose)
		{
			return (side == kLeft || side == kRight) && static_cast<int>(pose) >= 0 && static_cast<int>(pose) < kPoseCount;
		}

		// The mesh's size along each axis, in metres, and its bounds.
		struct Bounds
		{
			float lo[3] = {1.0e9f, 1.0e9f, 1.0e9f};
			float hi[3] = {-1.0e9f, -1.0e9f, -1.0e9f};
			float Extent(int axis) const { return hi[axis] - lo[axis]; }
		};

		Bounds BoundsOf(const Mesh& m)
		{
			Bounds b;
			for (u32 i = 0; i < m.vertex_count; i++)
			{
				for (int k = 0; k < 3; k++)
				{
					const float v = m.position[i * 3 + k];
					b.lo[k] = std::min(b.lo[k], v);
					b.hi[k] = std::max(b.hi[k], v);
				}
			}
			return b;
		}
	}

	const Mesh& Get(int side, Pose pose)
	{
		if (!Valid(side, pose))
			return kNoMesh;
		return kMeshes[side][static_cast<int>(pose)];
	}

	Anchors GetAnchors(int side, Pose pose)
	{
		Anchors a;
		if (!Valid(side, pose))
			return a;
		const float* v = (side == kLeft ? kLeftAnchor : kRightAnchor)[static_cast<int>(pose)];
		for (int k = 0; k < 3; k++)
		{
			a.wrist[k] = v[k];
			a.index_tip[k] = v[3 + k];
		}
		return a;
	}

	const char* PoseName(Pose pose)
	{
		switch (pose)
		{
			case Pose::Open:
				return "open";
			case Pose::Grip:
				return "grip";
			case Pose::Pistol:
				return "pistol";
			case Pose::Point:
				return "point";
		}
		return "?";
	}

	const Mesh& Lerp(int side, Pose a, Pose b, float t, Blend& scratch)
	{
		const Mesh& ma = Get(side, a);
		if (!std::isfinite(t) || t <= 0.0f || a == b)
			return ma;
		const Mesh& mb = Get(side, b);
		if (t >= 1.0f)
			return mb;
		if (ma.vertex_count == 0 || ma.vertex_count != mb.vertex_count)
			return ma;

		const size_t n = static_cast<size_t>(ma.vertex_count) * 3;
		scratch.position.resize(n);
		scratch.normal.resize(n);
		for (size_t i = 0; i < n; i++)
			scratch.position[i] = ma.position[i] + (mb.position[i] - ma.position[i]) * t;
		for (size_t i = 0; i < n; i += 3)
		{
			float v[3];
			for (int k = 0; k < 3; k++)
				v[k] = ma.normal[i + k] + (mb.normal[i + k] - ma.normal[i + k]) * t;
			const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			// Two normals that point opposite ways cancel; the first one's will do.
			if (len > 1.0e-4f)
			{
				for (int k = 0; k < 3; k++)
					scratch.normal[i + k] = v[k] / len;
			}
			else
			{
				for (int k = 0; k < 3; k++)
					scratch.normal[i + k] = ma.normal[i + k];
			}
		}
		scratch.mesh.vertex_count = ma.vertex_count;
		scratch.mesh.triangle_count = ma.triangle_count;
		scratch.mesh.position = scratch.position.data();
		scratch.mesh.normal = scratch.normal.data();
		scratch.mesh.index = ma.index; // one index list a side, shared by its poses
		return scratch.mesh;
	}

	int SelfTest(const char** first_failed)
	{
		const char* first = nullptr;
		int failures = 0;
		int checks = 0;
		const auto check = [&](bool ok, const char* name) {
			checks++;
			if (ok)
				return;
			failures++;
			if (!first)
				first = name;
		};

		// The tables themselves.
		check(kHandVertexCount > 0 && kHandTriangleCount > 0 && kHandPoseCount == static_cast<u32>(kPoseCount),
			"the baked counts are set");
		check(!Valid(2, Pose::Open) && !Valid(kLeft, static_cast<Pose>(kPoseCount)) && Get(2, Pose::Open).vertex_count == 0,
			"a side or pose out of range gives no mesh");

		for (int side = kLeft; side <= kRight; side++)
		{
			const Mesh& open = Get(side, Pose::Open);

			// Indices: in range, three distinct vertices to a triangle, the same list for every pose of the side.
			bool in_range = true;
			bool distinct = true;
			for (u32 t = 0; t < open.triangle_count; t++)
			{
				const u16 a = open.index[t * 3];
				const u16 b = open.index[t * 3 + 1];
				const u16 c = open.index[t * 3 + 2];
				if (a >= open.vertex_count || b >= open.vertex_count || c >= open.vertex_count)
					in_range = false;
				if (a == b || b == c || a == c)
					distinct = false;
			}
			check(in_range, "every triangle index is below the vertex count");
			check(distinct, "no triangle repeats a vertex");

			for (int p = 0; p < kPoseCount; p++)
			{
				const Pose pose = static_cast<Pose>(p);
				const Mesh& m = Get(side, pose);
				check(m.vertex_count == kHandVertexCount && m.triangle_count == kHandTriangleCount && m.index == open.index,
					"every pose has the side's vertices and its shared index list");

				bool finite = true;
				bool unit = true;
				for (u32 i = 0; i < m.vertex_count; i++)
				{
					const float* v = m.position + i * 3;
					const float* n = m.normal + i * 3;
					if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]) || !std::isfinite(n[0]) ||
						!std::isfinite(n[1]) || !std::isfinite(n[2]))
					{
						finite = false;
						continue;
					}
					if (std::fabs(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) - 1.0f) > 2.0e-3f)
						unit = false;
				}
				check(finite, "positions and normals are finite");
				check(unit, "the normals are unit length");
				if (!finite)
					continue;

				// The size: the open hand is 18-22 cm along the way the fingers point, none is over 22 cm any way.
				const Bounds b = BoundsOf(m);
				bool sane = true;
				for (int k = 0; k < 3; k++)
				{
					if (!(b.Extent(k) > 0.02f && b.Extent(k) < 0.22f))
						sane = false;
				}
				check(sane, "the bounding box is between 2 and 22 cm along every axis");
				if (pose == Pose::Open)
					check(b.Extent(2) > 0.18f && b.Extent(2) < 0.22f, "the open hand is 18-22 cm long along -Z");

				// Winding: a triangle's face normal points the way its vertex normals do. Skinning folds a few triangles at
				// the knuckles, so most, not all.
				u32 agree = 0;
				for (u32 t = 0; t < m.triangle_count; t++)
				{
					const float* pa = m.position + m.index[t * 3] * 3;
					const float* pb = m.position + m.index[t * 3 + 1] * 3;
					const float* pc = m.position + m.index[t * 3 + 2] * 3;
					const float e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
					const float e2[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
					const float f[3] = {
						e1[1] * e2[2] - e1[2] * e2[1],
						e1[2] * e2[0] - e1[0] * e2[2],
						e1[0] * e2[1] - e1[1] * e2[0],
					};
					float d = 0.0f;
					for (int k = 0; k < 3; k++)
					{
						d += f[k] * (m.normal[m.index[t * 3] * 3 + k] + m.normal[m.index[t * 3 + 1] * 3 + k] +
										m.normal[m.index[t * 3 + 2] * 3 + k]);
					}
					if (d > 0.0f)
						agree++;
				}
				check(agree * 100 >= m.triangle_count * 95, "the triangles are wound the way their normals face");

				// The anchors: the wrist inside the mesh's bounds (a centimetre of slack), the index tip within 2 cm of a
				// vertex.
				const Anchors a = GetAnchors(side, pose);
				bool wrist_in = true;
				for (int k = 0; k < 3; k++)
				{
					if (!(a.wrist[k] > b.lo[k] - 0.01f && a.wrist[k] < b.hi[k] + 0.01f))
						wrist_in = false;
				}
				check(wrist_in, "the wrist anchor is inside the mesh's bounds");
				float nearest = 1.0e9f;
				for (u32 i = 0; i < m.vertex_count; i++)
				{
					float d2 = 0.0f;
					for (int k = 0; k < 3; k++)
					{
						const float d = m.position[i * 3 + k] - a.index_tip[k];
						d2 += d * d;
					}
					nearest = std::min(nearest, d2);
				}
				check(nearest < 0.02f * 0.02f, "the index tip anchor is on the mesh");
			}
		}

		// The left hand is the right one mirrored in X: the open hands' bounds, and the wrist.
		{
			const Bounds l = BoundsOf(Get(kLeft, Pose::Open));
			const Bounds r = BoundsOf(Get(kRight, Pose::Open));
			const float tol = 0.003f;
			check(std::fabs(l.lo[0] + r.hi[0]) < tol && std::fabs(l.hi[0] + r.lo[0]) < tol &&
					std::fabs(l.lo[1] - r.lo[1]) < tol && std::fabs(l.hi[1] - r.hi[1]) < tol &&
					std::fabs(l.lo[2] - r.lo[2]) < tol && std::fabs(l.hi[2] - r.hi[2]) < tol,
				"the left hand's bounds are the right hand's mirrored in X");
			const Anchors la = GetAnchors(kLeft, Pose::Open);
			const Anchors ra = GetAnchors(kRight, Pose::Open);
			check(std::fabs(la.wrist[0] + ra.wrist[0]) < tol && std::fabs(la.wrist[1] - ra.wrist[1]) < tol &&
					std::fabs(la.wrist[2] - ra.wrist[2]) < tol,
				"the wrists mirror");
			// The palm faces -X on the right hand: the middle of the mesh's back (+X side) is further out than its front.
			check(r.hi[0] > 0.0f && l.lo[0] < 0.0f, "the right hand's back is toward +X, the left's toward -X");
		}

		// Lerp: the ends are the stored meshes, the middle is the average, the normals stay unit, bad input is safe.
		{
			Blend blend;
			const Mesh& a = Get(kRight, Pose::Open);
			const Mesh& b = Get(kRight, Pose::Grip);
			check(&Lerp(kRight, Pose::Open, Pose::Grip, 0.0f, blend) == &a, "Lerp at 0 is the first pose");
			check(&Lerp(kRight, Pose::Open, Pose::Grip, 1.0f, blend) == &b, "Lerp at 1 is the second pose");
			const float not_a_number = std::numeric_limits<float>::quiet_NaN();
			check(&Lerp(kRight, Pose::Open, Pose::Grip, not_a_number, blend) == &a, "Lerp of nothing is the first pose");
			check(&Lerp(kRight, Pose::Open, Pose::Open, 0.5f, blend) == &a, "Lerp of one pose is that pose");
			const Mesh& mid = Lerp(kRight, Pose::Open, Pose::Grip, 0.5f, blend);
			bool average = (&mid == &blend.mesh) && mid.vertex_count == a.vertex_count && mid.index == a.index;
			bool unit = true;
			for (u32 i = 0; average && i < mid.vertex_count; i++)
			{
				for (int k = 0; k < 3; k++)
				{
					const float want = 0.5f * (a.position[i * 3 + k] + b.position[i * 3 + k]);
					if (std::fabs(mid.position[i * 3 + k] - want) > 1.0e-6f)
						average = false;
				}
				const float* n = mid.normal + i * 3;
				if (std::fabs(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) - 1.0f) > 2.0e-3f)
					unit = false;
			}
			check(average, "Lerp at 0.5 is the average of the two poses");
			check(unit, "Lerp's normals are unit length");
			check(Lerp(5, Pose::Open, Pose::Grip, 0.5f, blend).vertex_count == 0, "Lerp of a bad side is empty");
		}

		if (failures == 0)
		{
			Console.WriteLn(Color_StrongGreen, "(VR) HandMesh self-test: passed (%d checks: %u triangles and %u vertices a side, "
											   "indices, winding, normals, sizes, mirror, anchors, blending).",
				checks, kHandTriangleCount, kHandVertexCount);
		}
		else
		{
			Console.WriteLn(Color_StrongRed, "(VR) HandMesh self-test FAIL: %s (%d of %d checks failed).", first, failures, checks);
		}
		if (first_failed)
			*first_failed = first;
		return checks;
	}
}
