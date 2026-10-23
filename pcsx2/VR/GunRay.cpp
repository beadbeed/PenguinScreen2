// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/GunRay.h"
#include "VR/NetGate.h"
#include "VR/CameraDriver.h"
#include "VR/GameFeedback.h"
#include "VR/HandModel.h"
#include "VR/HeadInWall.h"
#include "VR/ItemData.h"
#include "VR/PhysicalMelee.h"
#include "VR/SplitState.h"
#include "VR/VRProfileDB.h"

#include "Config.h"
#include "Memory.h"
#include "MemoryTypes.h"
#include "VMManager.h"

#include "common/Console.h"

#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace VR::GunRay
{
	namespace
	{
		bool Finite3(const float v[3])
		{
			return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
		}

		float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

		void Cross(const float a[3], const float b[3], float out[3])
		{
			out[0] = a[1] * b[2] - a[2] * b[1];
			out[1] = a[2] * b[0] - a[0] * b[2];
			out[2] = a[0] * b[1] - a[1] * b[0];
		}

		bool Normalize(float v[3])
		{
			const float n = std::sqrt(Dot(v, v));
			if (!std::isfinite(n) || n < 1.0e-6f)
				return false;
			for (int k = 0; k < 3; k++)
				v[k] /= n;
			return true;
		}
	}

	bool RayToGame(const float origin_vr[3], const float dir_vr[3], PointMap map, const void* ctx, float origin_g[3],
		float dir_g[3])
	{
		if (!map || !Finite3(origin_vr) || !Finite3(dir_vr))
			return false;
		float d[3] = {dir_vr[0], dir_vr[1], dir_vr[2]};
		if (!Normalize(d))
			return false;
		const float far_vr[3] = {origin_vr[0] + d[0], origin_vr[1] + d[1], origin_vr[2] + d[2]};
		float o[3], f[3];
		if (!map(ctx, origin_vr, o) || !map(ctx, far_vr, f) || !Finite3(o) || !Finite3(f))
			return false;
		float g[3] = {f[0] - o[0], f[1] - o[1], f[2] - o[2]};
		if (!Normalize(g))
			return false;
		for (int k = 0; k < 3; k++)
		{
			origin_g[k] = o[k];
			dir_g[k] = g[k];
		}
		return true;
	}

	bool ShotFrame(const float muzzle_g[3], const float dir_g[3], float back, float out_matrix[16], float out_origin[3])
	{
		if (!Finite3(muzzle_g) || !Finite3(dir_g) || !std::isfinite(back))
			return false;
		float z[3] = {dir_g[0], dir_g[1], dir_g[2]};
		if (!Normalize(z))
			return false;
		// Row 1 as near the game's up (+Y) as it can be; straight up or down the barrel, the game's +Z instead.
		float up[3] = {0.0f, 1.0f, 0.0f};
		if (std::fabs(z[1]) > 0.999f)
		{
			up[1] = 0.0f;
			up[2] = 1.0f;
		}
		float x[3], y[3];
		Cross(up, z, x);
		if (!Normalize(x))
			return false;
		Cross(z, x, y); // x = y cross z, so the rows are right-handed (determinant +1)
		for (int k = 0; k < 3; k++)
		{
			out_matrix[0 + k] = x[k];
			out_matrix[4 + k] = y[k];
			out_matrix[8 + k] = z[k];
			out_matrix[12 + k] = 0.0f;
			out_origin[k] = muzzle_g[k] + back * z[k];
		}
		out_matrix[3] = out_matrix[7] = out_matrix[11] = 0.0f;
		out_matrix[15] = 1.0f;
		return true;
	}

	bool WallShot(const float chest_g[3], const float muzzle_g[3], float t, const float normal_g[3], float back,
		float out_origin[3], float out_dir[3])
	{
		if (!Finite3(chest_g) || !Finite3(muzzle_g) || !Finite3(normal_g) || !std::isfinite(t) || !std::isfinite(back) ||
			!(t > 0.0f && t <= 1.0f))
			return false;
		float d[3] = {muzzle_g[0] - chest_g[0], muzzle_g[1] - chest_g[1], muzzle_g[2] - chest_g[2]};
		const float len = std::sqrt(Dot(d, d));
		if (!(len > 1.0e-3f) || !Normalize(d))
			return false;
		float n[3] = {normal_g[0], normal_g[1], normal_g[2]};
		if (!Normalize(n))
			return false;
		const float wall = t * len; // the wall along chest -> muzzle
		// The game's offset before the wall (never past it), and at least kWallShotGap off the plane: along the line that
		// is the gap over the cosine to the normal (a grazing line: from much further back, at worst from the chest).
		const float cosine = std::fabs(Dot(d, n));
		const float gap_along = (cosine > 1.0e-3f) ? kWallShotGap / cosine : wall;
		const float along = std::max(0.0f, std::min(wall - kWallShotBefore + std::min(back, 0.0f), wall - gap_along));
		for (int k = 0; k < 3; k++)
		{
			out_origin[k] = chest_g[k] + d[k] * along;
			out_dir[k] = d[k];
		}
		return true;
	}

	bool ObjectFrame(const float shot_m[16], const float muzzle_g[3], const float local[3], float out_matrix[16],
		float out_pos[3])
	{
		for (int k = 0; k < 16; k++)
			if (!std::isfinite(shot_m[k]))
				return false;
		if (!Finite3(muzzle_g) || !Finite3(local))
			return false;
		// M = Rot^T x F (Rot a signed permutation): row 0 = -f2, row 1 = -f0, row 2 = f1.
		for (int k = 0; k < 3; k++)
		{
			out_matrix[0 + k] = -shot_m[8 + k];
			out_matrix[4 + k] = -shot_m[0 + k];
			out_matrix[8 + k] = shot_m[4 + k];
		}
		out_matrix[3] = out_matrix[7] = out_matrix[11] = 0.0f;
		for (int k = 0; k < 3; k++)
		{
			out_pos[k] = muzzle_g[k] - (local[0] * out_matrix[0 + k] + local[1] * out_matrix[4 + k] + local[2] * out_matrix[8 + k]);
			out_matrix[12 + k] = out_pos[k];
		}
		out_matrix[15] = 1.0f;
		return true;
	}

	// -----------------------------------------------------------------------------------------------------------------
	// Self-test.
	// -----------------------------------------------------------------------------------------------------------------
	namespace
	{
		struct TestMap
		{
			float yaw = 0.7f, u = 100.0f, off[3] = {8000.0f, 155.0f, 13000.0f};
			bool fail = false;
		};
		bool TestMapFn(const void* ctx, const float vr[3], float game[3])
		{
			const TestMap& m = *static_cast<const TestMap*>(ctx);
			if (m.fail)
				return false;
			const float c = std::cos(m.yaw), s = std::sin(m.yaw);
			game[0] = m.off[0] + m.u * (c * vr[0] + s * vr[2]);
			game[1] = m.off[1] + m.u * vr[1];
			game[2] = m.off[2] + m.u * (-s * vr[0] + c * vr[2]);
			return true;
		}
		bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }
	}

	FlashEvidence::Seen FlashEvidence::Update(u64 now_ms, u32 shots_now, u32 flashes_now)
	{
		const bool shot = shots_now != shots;
		if (shot)
		{
			shots = shots_now;
			shot_ms = now_ms;
		}
		if (flashes_now == flashes)
			return Seen::Nothing;
		flashes = flashes_now;
		if (shot || (shot_ms != 0 && now_ms >= shot_ms && now_ms - shot_ms <= kSameShotMs))
		{
			flash_ms = now_ms;
			return Seen::VrFlash;
		}
		return Seen::OtherFlash;
	}

	std::vector<std::string> SelfTest()
	{
		std::vector<std::string> fail;
		const auto check = [&](bool ok, const char* name) {
			if (!ok)
				fail.emplace_back(name);
		};
		{
			// FlashEvidence (late review m1, m4).
			FlashEvidence e;
			check(!e.Suppress(1000), "flash: no evidence before any shot (the hands draw their own)");
			check(e.Update(1000, 0, 0) == FlashEvidence::Seen::Nothing && !e.Suppress(1000), "flash: nothing counted");
			check(e.Update(1016, 1, 1) == FlashEvidence::Seen::VrFlash && e.Suppress(1016) && e.Suppress(1060),
				"flash: cave O's shot and the flash in one poll: the game's flash is the VR gun's");
			check(!e.Suppress(1016 + FlashEvidence::kSuppressMs + 1), "flash: the evidence runs out");
			check(e.Update(2000, 2, 1) == FlashEvidence::Seen::Nothing && !e.Suppress(2000),
				"flash: a shot without a flash (not aiming) leaves the hands' own");
			check(e.Update(2033, 2, 2) == FlashEvidence::Seen::VrFlash && e.Suppress(2040),
				"flash: a flash two polls after cave O's shot still counts");
			check(e.Update(3000, 2, 3) == FlashEvidence::Seen::OtherFlash && !e.Suppress(3000),
				"flash: a flash with no cave O shot (it fell back) is not the VR gun's");
			e.Reset();
			check(!e.Suppress(3001) && e.Update(3010, 0, 0) == FlashEvidence::Seen::Nothing, "flash: a re-arm starts afresh");
		}
		{
			// WallShot: chest (0, 130, 0), muzzle 100 cm ahead along +Z, the wall at 60 % facing the chest (normal -Z).
			const float chest[3] = {0.0f, 130.0f, 0.0f};
			const float muzzle[3] = {0.0f, 130.0f, 100.0f};
			const float facing[3] = {0.0f, 0.0f, -1.0f};
			float wp[3], wd[3];
			check(WallShot(chest, muzzle, 0.6f, facing, -35.0f, wp, wd) && Near(wp[0], 0.0f, 1e-4f) && Near(wp[1], 130.0f, 1e-4f) &&
					  Near(wp[2], 60.0f - kWallShotBefore - 35.0f, 1e-3f) && Near(wd[2], 1.0f, 1e-5f) && Near(wd[0], 0.0f, 1e-5f),
				"wall shot: the game's back offset before a point 1 cm before the wall, along chest -> muzzle");
			float m[16], org[3];
			check(WallShot(chest, muzzle, 0.6f, facing, -35.0f, wp, wd) && ShotFrame(wp, wd, 0.0f, m, org) &&
					  Near(org[2], 24.0f, 1e-3f) && Near(m[10], 1.0f, 1e-5f),
				"wall shot: the frame's barrel into the wall, its origin the wall shot's (back 0)");
			// Evening review m3: bounds on the origin.
			check(WallShot(chest, muzzle, 0.6f, facing, 0.0f, wp, wd) && Near(wp[2], 60.0f - kWallShotGap, 1e-3f),
				"wall shot: back 0 (unread) still starts kWallShotGap off the wall, not 1 cm");
			check(WallShot(chest, muzzle, 0.6f, facing, 30.0f, wp, wd) && Near(wp[2], 60.0f - kWallShotGap, 1e-3f),
				"wall shot: a positive back is not taken (it would start past the wall)");
			check(WallShot(chest, muzzle, 0.25f, facing, -35.0f, wp, wd) && Near(wp[2], 0.0f, 1e-4f),
				"wall shot: a wall 25 cm from the chest starts at the chest, not behind it");
			{
				// The line 10 deg off the wall's plane (z = 60 sin 10 deg at 60 %), the gap along the normal.
				const float s10 = std::sin(10.0f * 3.14159265f / 180.0f), c10 = std::cos(10.0f * 3.14159265f / 180.0f);
				const float graze[3] = {0.0f, 130.0f + 100.0f * c10, 100.0f * s10};
				check(WallShot(chest, graze, 0.6f, facing, 0.0f, wp, wd) && Near(60.0f * s10 - wp[2], kWallShotGap, 1e-2f),
					"wall shot: at a grazing 10 deg the start is still kWallShotGap off the plane, not 1.7 mm");
				check(WallShot(chest, graze, 0.3f, facing, 0.0f, wp, wd) && Near(wp[2], 0.0f, 1e-4f) && Near(wp[1], 130.0f, 1e-4f),
					"wall shot: grazing, with the chest nearer the plane than the gap: from the chest");
			}
			const float zero[3] = {0.0f, 0.0f, 0.0f};
			check(!WallShot(chest, chest, 0.5f, facing, 0.0f, wp, wd) && !WallShot(chest, muzzle, 0.0f, facing, 0.0f, wp, wd) &&
					  !WallShot(chest, muzzle, 1.5f, facing, 0.0f, wp, wd) &&
					  !WallShot(chest, muzzle, std::nanf(""), facing, 0.0f, wp, wd) &&
					  !WallShot(chest, muzzle, 0.6f, zero, 0.0f, wp, wd) &&
					  !WallShot(chest, muzzle, 0.6f, facing, std::nanf(""), wp, wd),
				"wall shot: no line, t 0, t past the muzzle, NaN, a zero normal and a NaN back refused");
		}
		TestMap tm;
		{
			// A gun 1.2 m up, pointing -Z (ahead) and a little down, through a turned, scaled map.
			const float o[3] = {0.1f, 1.2f, -0.3f};
			const float d[3] = {0.0f, -0.2f, -1.0f};
			float og[3], dg[3];
			check(RayToGame(o, d, TestMapFn, &tm, og, dg), "ray: mapped");
			float want_o[3];
			TestMapFn(&tm, o, want_o);
			check(Near(og[0], want_o[0], 1e-3f) && Near(og[1], want_o[1], 1e-3f) && Near(og[2], want_o[2], 1e-3f),
				"ray: the origin is the mapped muzzle");
			check(Near(Dot(dg, dg), 1.0f, 1e-4f), "ray: the direction is unit");
			// A point 5 m down the VR ray lands on the game ray 500 game units out.
			float dn[3] = {d[0], d[1], d[2]};
			Normalize(dn);
			const float p_vr[3] = {o[0] + 5.0f * dn[0], o[1] + 5.0f * dn[1], o[2] + 5.0f * dn[2]};
			float p_g[3];
			TestMapFn(&tm, p_vr, p_g);
			check(Near(p_g[0], og[0] + 500.0f * dg[0], 0.05f) && Near(p_g[1], og[1] + 500.0f * dg[1], 0.05f) &&
					  Near(p_g[2], og[2] + 500.0f * dg[2], 0.05f),
				"ray: a point 5 m down the VR ray is on the game ray");
			check(dg[1] < -0.15f && dg[1] > -0.25f, "ray: pointing down stays pointing down (the map keeps up)");

			float m[16], org[3];
			check(ShotFrame(og, dg, -35.0f, m, org), "frame: built");
			check(Near(m[8], dg[0], 1e-5f) && Near(m[9], dg[1], 1e-5f) && Near(m[10], dg[2], 1e-5f),
				"frame: row 2 is the barrel (the capsule's +Z)");
			const float r0[3] = {m[0], m[1], m[2]}, r1[3] = {m[4], m[5], m[6]}, r2[3] = {m[8], m[9], m[10]};
			check(Near(Dot(r0, r0), 1.0f, 1e-4f) && Near(Dot(r1, r1), 1.0f, 1e-4f) && Near(Dot(r0, r1), 0.0f, 1e-4f) &&
					  Near(Dot(r0, r2), 0.0f, 1e-4f) && Near(Dot(r1, r2), 0.0f, 1e-4f),
				"frame: rows orthonormal");
			float c12[3];
			Cross(r1, r2, c12);
			check(Near(Dot(r0, c12), 1.0f, 1e-4f), "frame: right-handed (determinant +1)");
			check(r1[1] > 0.9f, "frame: row 1 near the game's up");
			check(m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f && m[12] == 0.0f && m[13] == 0.0f && m[14] == 0.0f &&
					  m[15] == 1.0f,
				"frame: row 3 is (0, 0, 0, 1) and the w column zero, as the game's");
			check(Near(org[0], og[0] - 35.0f * dg[0], 1e-3f) && Near(org[1], og[1] - 35.0f * dg[1], 1e-3f) &&
					  Near(org[2], og[2] - 35.0f * dg[2], 1e-3f),
				"frame: the origin is 35 units back along the barrel");
			// The capsule as the game places it (P (0,0,0) + D (0,0,1) * 1000 through the matrix, row vectors) ends 10 m down
			// the VR ray from the moved-back origin.
			const float end[3] = {org[0] + 1000.0f * m[8], org[1] + 1000.0f * m[9], org[2] + 1000.0f * m[10]};
			check(Near(end[0], og[0] + 965.0f * dg[0], 0.01f) && Near(end[2], og[2] + 965.0f * dg[2], 0.01f),
				"frame: the game's 10 m capsule runs down the VR ray");
		}
		{
			// Straight up and straight down still give a frame.
			const float mz[3] = {0.0f, 0.0f, 0.0f};
			const float upd[3] = {0.0f, 1.0f, 0.0f}, downd[3] = {0.0f, -3.0f, 0.0f};
			float m[16], org[3];
			check(ShotFrame(mz, upd, -35.0f, m, org) && Near(m[9], 1.0f, 1e-5f), "frame: straight up");
			check(ShotFrame(mz, downd, 0.0f, m, org) && Near(m[9], -1.0f, 1e-5f) && Near(org[1], 0.0f, 1e-6f),
				"frame: straight down, no back offset");
		}
		{
			// Junk is refused.
			const float o[3] = {0.0f, 1.0f, 0.0f};
			const float zero[3] = {0.0f, 0.0f, 0.0f};
			const float nan3[3] = {std::nanf(""), 0.0f, 0.0f};
			float og[3], dg[3], m[16], org[3];
			check(!RayToGame(o, zero, TestMapFn, &tm, og, dg), "ray: a zero direction fails");
			check(!RayToGame(nan3, o, TestMapFn, &tm, og, dg), "ray: a NaN origin fails");
			check(!RayToGame(o, o, nullptr, &tm, og, dg), "ray: no map fails");
			TestMap bad;
			bad.fail = true;
			check(!RayToGame(o, o, TestMapFn, &bad, og, dg), "ray: a failing map fails");
			check(!ShotFrame(o, zero, -35.0f, m, org), "frame: a zero direction fails");
			check(!ShotFrame(o, o, std::nanf(""), m, org), "frame: a NaN back fails");
		}
		{
			// The fake weapon object: the game's own math on it (FUN_0062CF10: frame = Ry(-pi/2) then Rx(+pi/2) applied to
			// the identity, times the object's matrix; muzzle = local x M + position) gives back the VR shot's frame and
			// the VR muzzle.
			const float o[3] = {0.1f, 1.2f, -0.3f};
			const float d[3] = {0.3f, -0.2f, -1.0f};
			float og[3], dg[3], f[16], org[3];
			check(RayToGame(o, d, TestMapFn, &tm, og, dg) && ShotFrame(og, dg, -35.0f, f, org), "object: a shot frame");
			const float local[3] = {-9.5f, 0.0f, 5.1f}; // the 45 Auto's muzzle in its model
			float mo[16], pos[3];
			check(ObjectFrame(f, og, local, mo, pos), "object: built");
			// The game's rotation, built as its helpers do (rows; A = A x R).
			const auto mul3 = [](const float a[9], const float b[9], float out[9]) {
				for (int r = 0; r < 3; r++)
					for (int c = 0; c < 3; c++)
						out[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c] + a[r * 3 + 1] * b[1 * 3 + c] + a[r * 3 + 2] * b[2 * 3 + c];
			};
			const float pi2 = 1.5707963705062866f;
			float sy = std::sin(-pi2), cy = std::cos(-pi2), sx = std::sin(pi2), cx = std::cos(pi2);
			const float ry[9] = {cy, 0.0f, -sy, 0.0f, 1.0f, 0.0f, sy, 0.0f, cy};
			const float rx[9] = {1.0f, 0.0f, 0.0f, 0.0f, cx, sx, 0.0f, -sx, cx};
			const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
			float a1[9], rot[9];
			mul3(id, ry, a1);
			mul3(a1, rx, rot);
			const float m3[9] = {mo[0], mo[1], mo[2], mo[4], mo[5], mo[6], mo[8], mo[9], mo[10]};
			float g[9];
			mul3(rot, m3, g);
			bool same = true;
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 3; c++)
					same = same && Near(g[r * 3 + c], f[r * 4 + c], 1e-5f);
			check(same, "object: the game's frame from the fake object is the VR shot's frame");
			float mz[3];
			for (int k = 0; k < 3; k++)
				mz[k] = local[0] * mo[0 + k] + local[1] * mo[4 + k] + local[2] * mo[8 + k] + pos[k];
			check(Near(mz[0], og[0], 1e-3f) && Near(mz[1], og[1], 1e-3f) && Near(mz[2], og[2], 1e-3f),
				"object: the game's muzzle from the fake object is the VR muzzle");
			// The game's bullet origin: muzzle + (0, 0, back) x frame, as cave I's.
			const float bo[3] = {mz[0] - 35.0f * g[6], mz[1] - 35.0f * g[7], mz[2] - 35.0f * g[8]};
			check(Near(bo[0], org[0], 1e-2f) && Near(bo[1], org[1], 1e-2f) && Near(bo[2], org[2], 1e-2f),
				"object: the game's bullet origin is cave I's");
			check(mo[12] == pos[0] && mo[13] == pos[1] && mo[14] == pos[2] && mo[15] == 1.0f && mo[3] == 0.0f && mo[7] == 0.0f &&
					  mo[11] == 0.0f,
				"object: row 3 is (position, 1), the w column zero");
			float c12[3];
			const float r0[3] = {mo[0], mo[1], mo[2]}, r1[3] = {mo[4], mo[5], mo[6]}, r2[3] = {mo[8], mo[9], mo[10]};
			Cross(r1, r2, c12);
			check(Near(Dot(r0, c12), 1.0f, 1e-4f), "object: a rotation (determinant +1)");
			const float bad[3] = {std::nanf(""), 0.0f, 0.0f};
			check(!ObjectFrame(f, og, bad, mo, pos) && !ObjectFrame(f, bad, local, mo, pos), "object: NaN refused");
		}
		{
			// Co-op aim (research/2026-10-22-coop-ready.md): the aim point, the heading solve, the elevation trim.
			const float o[3] = {0.0f, 100.0f, 0.0f}, d[3] = {0.0f, 0.0f, 1.0f};
			const float cs[3][3] = {{30.0f, 100.0f, 300.0f}, {10.0f, 100.0f, 200.0f}, {0.0f, 100.0f, -100.0f}};
			const float rs[3] = {15.0f, 15.0f, 15.0f};
			float t[3];
			// Sphere 1 (10 cm off the ray, t 200) is the nearest within r + pad; sphere 0 (30 off) would be too; sphere 2 is
			// behind the muzzle.
			check(AimPoint(o, d, cs, rs, 3, 24.0f, 1000.0f, 600.0f, t) && std::fabs(t[2] - 200.0f) < 1e-3f && t[0] == 0.0f,
				"co-op aim: the nearest enemy sphere along the ray, the point on the ray beside it");
			const float far_c[1][3] = {{80.0f, 100.0f, 200.0f}};
			check(!AimPoint(o, d, far_c, rs, 1, 24.0f, 1000.0f, 600.0f, t) && std::fabs(t[2] - 600.0f) < 1e-3f,
				"co-op aim: a sphere farther off the ray than r + pad: the far point");
			const float beyond[1][3] = {{0.0f, 100.0f, 1200.0f}};
			check(!AimPoint(o, d, beyond, rs, 1, 24.0f, 1000.0f, 600.0f, t), "co-op aim: a sphere past the shot's reach is ignored");
			check(!AimPoint(o, d, cs, rs, 0, 24.0f, 1000.0f, 600.0f, t) && std::fabs(t[2] - 600.0f) < 1e-3f,
				"co-op aim: no enemies: the far point");
			// The heading solve: a gun 20 cm right of the centre (heading 0 = +z, right = +x), barrel along the heading.
			const float gun_rel[2] = {20.0f, 50.0f};
			const AimPose p = PoseFrom(0.0f, 0.0f, gun_rel);
			check(std::fabs(p.lateral - 20.0f) < 1e-3f && std::fabs(p.forward - 50.0f) < 1e-3f && std::fabs(p.delta) < 1e-6f,
				"co-op aim: the pose's lateral and forward offsets");
			// A target straight ahead of the centre, 300 cm away: the barrel's line (20 cm right) must turn left onto it.
			const float target[2] = {0.0f, 300.0f};
			float h = 0.0f, lat = 0.0f;
			check(SolveHeading(p, target, &h, &lat) && h < 0.0f, "co-op aim: a gun right of the centre turns left onto a target ahead");
			// Check the solved heading: the gun turned by h, its line passes through the target.
			{
				const float sh = std::sin(h), ch = std::cos(h);
				const float gx = gun_rel[0] * ch + gun_rel[1] * sh, gz = -gun_rel[0] * sh + gun_rel[1] * ch; // rotated by h
				const float ux = sh, uz = ch;
				const float vx = target[0] - gx, vz = target[1] - gz;
				check(std::fabs(vx * uz - vz * ux) < 0.05f, "co-op aim: the solved heading's barrel line passes through the target");
			}
			// The same pose read in a turn (heading 90 deg, everything turned with it) gives the same offsets.
			const float turned[2] = {50.0f, -20.0f};
			const AimPose q = PoseFrom(1.5707963f, 1.5707963f, turned);
			check(std::fabs(q.lateral - 20.0f) < 1e-3f && std::fabs(q.forward - 50.0f) < 1e-3f && std::fabs(q.delta) < 1e-5f,
				"co-op aim: the pose's offsets turn with the heading");
			AimPose sm = p;
			const AimPose wrapped{20.0f, 50.0f, 3.1f}, other{20.0f, 50.0f, -3.1f};
			sm = wrapped;
			SmoothPose(sm, other, 0.5f);
			check(std::fabs(std::fabs(sm.delta) - 3.14159f) < 0.01f, "co-op aim: the yaw offset smooths across +-180 deg");
			const float none[2] = {0.0f, 0.0f};
			check(!SolveHeading(p, none, &h, &lat), "co-op aim: no heading for a target at the centre");
			// The elevation trim: held when told to, learnt (and clamped) otherwise.
			Steer st;
			float cmd[2];
			const float want[2] = {0.0f, 0.1f}, now[2] = {0.0f, 0.0f};
			st.Step(want, now, true, 0.5f, 0.2f, cmd);
			check(cmd[1] == 0.1f && st.trim_elev == 0.0f, "co-op aim: the first step is the wanted elevation");
			st.Step(want, now, false, 0.5f, 0.2f, cmd);
			check(st.trim_elev == 0.0f, "co-op aim: a held step learns nothing");
			for (int i = 0; i < 20; i++)
				st.Step(want, now, true, 0.5f, 0.2f, cmd);
			check(std::fabs(st.trim_elev - 0.2f) < 1e-6f && std::fabs(cmd[1] - 0.3f) < 1e-6f, "co-op aim: the trim is clamped");
			st.Reset();
			check(!st.valid && st.trim_elev == 0.0f, "co-op aim: reset");
		}
		return fail;
	}

	// Co-op aim's pure parts.
	bool AimPoint(const float o[3], const float d[3], const float (*centres)[3], const float* radii, int n, float pad, float reach,
		float far_t, float out[3])
	{
		float best = -1.0f;
		for (int i = 0; i < n; i++)
		{
			const float v[3] = {centres[i][0] - o[0], centres[i][1] - o[1], centres[i][2] - o[2]};
			const float t = v[0] * d[0] + v[1] * d[1] + v[2] * d[2];
			if (!std::isfinite(t) || t <= 0.0f || t > reach || !std::isfinite(radii[i]) || radii[i] <= 0.0f)
				continue;
			const float q[3] = {v[0] - t * d[0], v[1] - t * d[1], v[2] - t * d[2]};
			const float miss = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
			if (miss <= radii[i] + pad && (best < 0.0f || t < best))
				best = t;
		}
		const float t = best > 0.0f ? best : far_t;
		for (int k = 0; k < 3; k++)
			out[k] = o[k] + t * d[k];
		return best > 0.0f;
	}

	void Steer::Step(const float desired[2], const float actual[2], bool learn, float gain, float max_trim, float out_cmd[2])
	{
		const auto wrap = [](float a) {
			a = std::fmod(a + 3.14159265f, 6.28318531f);
			if (a < 0.0f)
				a += 6.28318531f;
			return a - 3.14159265f;
		};
		if (valid && learn)
		{
			// The barrel's error against what is wanted now: the trim moves by gain of it (one poll late, as the game
			// poses the gun after the heading and pitch it was given).
			trim_yaw = std::clamp(trim_yaw + gain * wrap(desired[0] - actual[0]), -max_trim, max_trim);
			trim_elev = std::clamp(trim_elev + gain * (desired[1] - actual[1]), -max_trim, max_trim);
		}
		valid = true;
		out_cmd[0] = wrap(desired[0] + trim_yaw);
		out_cmd[1] = desired[1] + trim_elev;
	}

	AimPose PoseFrom(float barrel_yaw, float heading, const float gun_rel[2])
	{
		AimPose p;
		const float su = std::sin(barrel_yaw), cu = std::cos(barrel_yaw);
		p.lateral = gun_rel[0] * cu - gun_rel[1] * su; // . (cos, -sin): right of the barrel
		p.forward = gun_rel[0] * su + gun_rel[1] * cu; // . (sin, cos)
		p.delta = std::remainder(barrel_yaw - heading, 6.28318531f);
		return p;
	}

	void SmoothPose(AimPose& s, const AimPose& now, float k)
	{
		s.lateral += k * (now.lateral - s.lateral);
		s.forward += k * (now.forward - s.forward);
		s.delta = std::remainder(s.delta + k * std::remainder(now.delta - s.delta, 6.28318531f), 6.28318531f);
	}

	bool SolveHeading(const AimPose& p, const float target_rel[2], float* heading, float* lateral)
	{
		const float r = std::sqrt(target_rel[0] * target_rel[0] + target_rel[1] * target_rel[1]);
		if (!std::isfinite(r) || !std::isfinite(p.lateral) || !std::isfinite(p.delta) || r < 1.0f)
			return false;
		const float alpha = std::atan2(target_rel[0], target_rel[1]);
		// The barrel's line, lateral off the centre, passes through the target when its yaw is alpha - asin(lateral / r);
		// a target closer than that offset (inside the gun's own reach) is faced straight on.
		const float theta = (std::fabs(p.lateral) < r) ? alpha - std::asin(p.lateral / r) : alpha;
		*heading = std::remainder(theta - p.delta, 6.28318531f);
		*lateral = p.lateral;
		return std::isfinite(*heading);
	}

	// -----------------------------------------------------------------------------------------------------------------
	// The glue.
	// -----------------------------------------------------------------------------------------------------------------
	namespace
	{
		// Co-op aim: the command CameraDriver's aim writes while fresh, and the steering state.
		std::mutex s_coop_mutex;
		float s_coop_yaw = 0.0f, s_coop_elev = 0.0f;
		u64 s_coop_ms = 0;
		Steer s_steer;
		AimPose s_pose;
		bool s_pose_valid = false;
		bool s_prev_valid = false;
		float s_prev_heading = 0.0f, s_prev_barrel = 0.0f;
		constexpr float kSteadyRad = 0.0105f; // 0.6 deg between polls
		bool s_coop_said = false;
		float s_coop_up_s = 0.0f; // how long co-op aim has steered without a break
		constexpr float kLearnWithin = 0.21f; // rad (12 deg)
		constexpr float kSettleS = 0.3f;
		constexpr float kArmPivot = 55.0f; // game cm behind the weapon object along the barrel: the shoulder
		float s_coop_log_s = 0.0f;
		// The equipped item's weapon object pointer (File #1: item record +0x1C, as FUN_0062CF10 reads it).
		constexpr u32 kItemObjectOffset = 0x1C;
		constexpr u32 kObjMatrix = 0x1C0, kObjPos = 0x200;

		bool s_armed = false; // the block may hold our words (written since the last clear)
		u32 s_addr = 0;
		u32 s_magic = 0;
		u32 s_crc = 0;
		bool s_stale = true;  // the next write starts the block afresh (cave I's shot count from zero)
		u32 s_last_shots = 0;
		std::string s_verdict;
		u32 s_verdict_crc = 0;
		// After a shot along the ray: the enemies' HP watched this long for the log.
		float s_watch_s = 0.0f;
		std::array<s16, 64> s_hp{};
		std::array<bool, 64> s_present{};
		float s_shot_origin[3] = {};
		float s_shot_dir[3] = {};
		// Cave O's block ([VR] GunObjectAim).
		bool s_obj_armed = false;
		u32 s_obj_addr = 0;
		u32 s_obj_magic = 0;
		u32 s_obj_crc = 0;
		bool s_obj_stale = true;
		FlashEvidence s_obj_flash;
		std::atomic<bool> s_obj_live{false};
		std::atomic<u64> s_obj_flash_ms{0}; // s_obj_flash.flash_ms, for the hands on the GS thread
		bool s_load_pending = false;         // a state was loaded: zero any block of ours it holds (late review m2)
		bool s_refused_logged = false, s_obj_refused_logged = false;
		float s_obj_muzzle[3] = {};
		std::string s_obj_verdict;

		bool DefaultRead(u32 address, u8 width, u32* value)
		{
			if (!VMManager::HasValidVM() || static_cast<u64>(address) + width > Ps2MemSize::MainRam)
				return false;
			switch (width)
			{
				case 1: *value = memRead8(address); return true;
				case 2: *value = memRead16(address); return true;
				case 4: *value = memRead32(address); return true;
				default: return false;
			}
		}
		bool Read(GuestReader reader, u32 address, u8 width, u32* value)
		{
			return (reader ? reader : DefaultRead)(address, width, value);
		}

		void Clear()
		{
			if (s_armed && VMManager::HasValidVM() && s_addr + kBlockBytes <= Ps2MemSize::MainRam &&
				VMManager::GetDiscCRC() == s_crc && static_cast<u32>(memRead32(s_addr)) == s_magic)
			{
				for (u32 off = 0; off < kBlockBytes; off += 4)
					memWrite32(s_addr + off, 0);
			}
			s_armed = false;
			s_watch_s = 0.0f;
		}

		void ClearObject()
		{
			if (s_obj_armed && VMManager::HasValidVM() && s_obj_addr + kObjBlockBytes <= Ps2MemSize::MainRam &&
				VMManager::GetDiscCRC() == s_obj_crc && static_cast<u32>(memRead32(s_obj_addr)) == s_obj_magic)
			{
				for (u32 off = 0; off < kObjBlockBytes; off += 4)
					memWrite32(s_obj_addr + off, 0);
			}
			s_obj_armed = false;
			s_obj_live.store(false, std::memory_order_relaxed);
			s_obj_flash_ms.store(0, std::memory_order_relaxed);
		}

		// A loaded state's own blocks: zeroed when they read our magic, whether or not this session armed them (late
		// review m2; as HandPickup does), so a pose saved with the state is never taken for a fresh one.
		void ClearLoadedBlocks(const ProfileDB::MeleeParams& mp)
		{
			const auto zero_if_ours = [](u32 addr, u32 bytes, u32 magic, const char* what) {
				if (magic == 0 || addr == 0 || static_cast<u64>(addr) + bytes > Ps2MemSize::MainRam ||
					static_cast<u32>(memRead32(addr)) != magic)
					return;
				for (u32 off = 0; off < bytes; off += 4)
					memWrite32(addr + off, 0);
				Console.WriteLn("(VR) Gun ray: a loaded state's %s block at 0x%X cleared.", what, addr);
			};
			if (mp.has_gun_ray)
				zero_if_ours(mp.gun_block, kBlockBytes, mp.gun_magic, "gun ray");
			if (mp.has_gun_object)
				zero_if_ours(mp.gun_object_block, kObjBlockBytes, mp.gun_object_magic, "gun object");
		}

		void WriteF32(u32 address, float v)
		{
			u32 w;
			std::memcpy(&w, &v, 4);
			memWrite32(address, w);
		}

		bool MapPoint(const void* ctx, const float vr[3], float game[3])
		{
			return CameraDriver::GameFromVr(*static_cast<const CameraDriver::Mapping*>(ctx), vr, game);
		}

		// The right hand as the hands draw it (position from the grip pose, orientation from the aim pose).
		bool RightHand(const VRInputSnapshot& snap, float pos[3], float quat[4])
		{
			const VRHandState& h = snap.hands[VRInputSnapshot::RIGHT];
			if (!h.grip_pose.valid)
				return false;
			const VRPose& orient = h.aim_pose.valid ? h.aim_pose : h.grip_pose;
			for (int k = 0; k < 3; k++)
				pos[k] = h.grip_pose.position_xyz[k];
			for (int k = 0; k < 4; k++)
				quat[k] = orient.orientation_xyzw[k];
			return Finite3(pos) && std::isfinite(quat[0]) && std::isfinite(quat[1]) && std::isfinite(quat[2]) &&
			       std::isfinite(quat[3]);
		}

		bool Log()
		{
			static const bool s_log = [] {
				const char* e = std::getenv("PCSX2_VR_GUNRAY_LOG");
				return e && e[0] == '1';
			}();
			return s_log;
		}

		void ReadHp(GuestReader reader, const ProfileDB::MeleeParams& mp, std::array<s16, 64>& hp, std::array<bool, 64>& present)
		{
			const u32 n = std::min<u32>(mp.enemy_count, 64u);
			for (u32 i = 0; i < 64; i++)
			{
				present[i] = false;
				hp[i] = 0;
				if (i >= n)
					continue;
				const u32 base = mp.enemy_table + i * mp.enemy_stride;
				u32 p = 0, h = 0;
				if (Read(reader, base + mp.enemy_present, 1, &p) && p != 0 && Read(reader, base + mp.enemy_hp, 2, &h))
				{
					present[i] = true;
					hp[i] = static_cast<s16>(static_cast<u16>(h));
				}
			}
		}
	}

	namespace
	{
		bool ReadF32(GuestReader reader, u32 address, float* out)
		{
			u32 w = 0;
			if (!Read(reader, address, 4, &w))
				return false;
			std::memcpy(out, &w, 4);
			return std::isfinite(*out);
		}

		// Co-op aim's poll: the game barrel's muzzle and direction from the weapon object, the aim point on the VR ray, the
		// heading that puts the barrel's line through it (Solve), the elevation, the command published. False when anything
		// does not read (then the controller's own aim).
		bool SteerCoop(GuestReader reader, const ProfileDB::MeleeParams& mp, const ProfileDB::CameraLookAt& la, u32 record,
			u32 item_rec, u32 type, bool type_ok, const float og[3], const float dg[3], float dt)
		{
			u32 obj = 0;
			if (item_rec == 0 || !la.has_heading || !Read(reader, item_rec + kItemObjectOffset, 4, &obj) || obj == 0 ||
				static_cast<u64>(obj) + kObjPos + 12 > Ps2MemSize::MainRam)
				return false;
			float M[16], pos[3], K[3];
			for (u32 k = 0; k < 16; k++)
				if (!ReadF32(reader, obj + kObjMatrix + 4 * k, &M[k]))
					return false;
			for (u32 k = 0; k < 3; k++)
				if (!ReadF32(reader, obj + kObjPos + 4 * k, &pos[k]) ||
					!ReadF32(reader, static_cast<u32>(record + la.position_offset + 4 * k), &K[k]))
					return false;
			u32 hb = 0;
			if (!Read(reader, static_cast<u32>(record + la.heading_offset), 2, &hb))
				return false;
			const float heading = static_cast<float>(static_cast<s16>(static_cast<u16>(hb))) * (6.28318531f / 65536.0f);
			// The barrel: FUN_0062CF10's frame is Rot x M with Rot's row 2 (-1, 0, 0), so the barrel is -(M's row 0).
			float B[3] = {-M[0], -M[1], -M[2]};
			const float bl = std::sqrt(B[0] * B[0] + B[1] * B[1] + B[2] * B[2]);
			if (!std::isfinite(bl) || bl < 1e-4f)
				return false;
			for (float& c : B)
				c /= bl;
			// The game's muzzle: the weapon type's model-space muzzle through the object (row vectors), else the object.
			float G[3] = {pos[0], pos[1], pos[2]};
			if (type_ok && mp.has_gun_object)
			{
				u32 lw[3] = {};
				float local[3] = {};
				bool ok = true;
				for (u32 k = 0; k < 3 && ok; k++)
					ok = Read(reader, mp.shape_types + mp.shape_type_stride * type + mp.gun_object_muzzle + 4 * k, 4, &lw[k]);
				std::memcpy(local, lw, sizeof(local));
				if (ok && Finite3(local) && std::fabs(local[0]) < 200.0f && std::fabs(local[1]) < 200.0f && std::fabs(local[2]) < 200.0f)
					for (int j = 0; j < 3; j++)
						G[j] = pos[j] + local[0] * M[0 + j] + local[1] * M[4 + j] + local[2] * M[8 + j];
			}
			// The enemies' hurt spheres (present, alive; File #1's melee.contact fields).
			float centres[64][3];
			float radii[64];
			int n = 0;
			if (mp.has_contact)
			{
				const u32 count = std::min<u32>(mp.enemy_count, 64u);
				for (u32 i = 0; i < count && n < 64; i++)
				{
					const u32 base = mp.enemy_table + i * mp.enemy_stride;
					u32 present = 0, hp = 0, sc = 0, shapes = 0, cen = 0;
					if (!Read(reader, base + mp.enemy_present, 1, &present) || present == 0 || !Read(reader, base + mp.enemy_hp, 2, &hp) ||
						static_cast<s16>(static_cast<u16>(hp)) <= 0 || !Read(reader, base + mp.sphere_count, 1, &sc) ||
						!Read(reader, base + mp.sphere_shapes, 4, &shapes) || !Read(reader, base + mp.sphere_centres, 4, &cen) ||
						shapes == 0 || cen == 0 || shapes >= Ps2MemSize::MainRam || cen >= Ps2MemSize::MainRam)
						continue;
					for (u32 k = 0; k < std::min<u32>(sc, 8u) && n < 64; k++)
					{
						float r = 0.0f;
						if (!ReadF32(reader, shapes + 20u * k + 0x0C, &r) || !(r > 0.0f))
							break; // the list ends at a radius of -1
						if (!ReadF32(reader, cen + 12u * k, &centres[n][0]) || !ReadF32(reader, cen + 12u * k + 4, &centres[n][1]) ||
							!ReadF32(reader, cen + 12u * k + 8, &centres[n][2]))
							break;
						radii[n] = r;
						n++;
					}
				}
			}
			float T[3];
			const bool on_enemy = AimPoint(og, dg, centres, radii, n, kAimPad, kAimReach, kAimFar, T);

			// The gun moves with the character: the barrel's line keeps its lateral offset c from the character's centre
			// and its yaw offset from the heading (smoothed: the pose lags the heading a frame or two in a turn).
			const float gun_rel[2] = {G[0] - K[0], G[2] - K[2]};
			const AimPose ap = PoseFrom(std::atan2(B[0], B[2]), heading, gun_rel);
			const float target_rel[2] = {T[0] - K[0], T[2] - K[2]};
			float heading_cmd = 0.0f, lateral = 0.0f;
			// Learnt only while the heading and the barrel both hold still between polls: in a turn the gun's pose lags
			// the heading, and that lag is not the gun's offset. Until a first steady read: the barrel along the heading
			// from the centre (the controller-style aim).
			const bool steady = s_prev_valid && std::fabs(std::remainder(heading - s_prev_heading, 6.28318531f)) < kSteadyRad &&
			                    std::fabs(std::remainder(ap.delta + heading - s_prev_barrel, 6.28318531f)) < kSteadyRad;
			s_prev_valid = true;
			s_prev_heading = heading;
			s_prev_barrel = ap.delta + heading;
			if (steady)
			{
				if (!s_pose_valid)
					s_pose = ap;
				else
					SmoothPose(s_pose, ap, 0.3f);
				s_pose_valid = true;
			}
			if (!SolveHeading(s_pose_valid ? s_pose : AimPose{}, target_rel, &heading_cmd, &lateral))
				return false;
			// The elevation: the aim pitch swings the arm, so the gun turns about the shoulder, kArmPivot back along the
			// barrel from the weapon object (measured offline: 32 cm of travel for 34 deg of pitch); seen from there the
			// muzzle's own rise and fall with the pitch drops out, so the elevation from the pivot to the aim point is the
			// one that puts the barrel's line through it. Plus a trim learnt from the barrel (the aim pitch's blend of aim
			// clips is not exactly 1:1), only from a steady, consistent pose: the matrix's row 3 (its position) the same as
			// +0x200, the barrel within kLearnWithin, the gun up for kSettleS.
			const float P[3] = {pos[0] - kArmPivot * B[0], pos[1] - kArmPivot * B[1], pos[2] - kArmPivot * B[2]};
			const float hd = std::sqrt((T[0] - P[0]) * (T[0] - P[0]) + (T[2] - P[2]) * (T[2] - P[2]));
			const float elev_want = std::atan2(T[1] - P[1], std::max(hd, 1.0f));
			const float elev_now = std::asin(std::clamp(B[1], -1.0f, 1.0f));
			const float dpos = std::fabs(M[12] - pos[0]) + std::fabs(M[13] - pos[1]) + std::fabs(M[14] - pos[2]);
			s_coop_up_s += dt;
			const bool learn = dpos < 2.0f && std::fabs(elev_want - elev_now) < kLearnWithin && s_coop_up_s >= kSettleS;
			const float desired[2] = {heading_cmd, elev_want};
			const float actual[2] = {heading_cmd, elev_now}; // the heading comes from SolveHeading, not trimmed
			float cmd[2];
			s_steer.Step(desired, actual, learn, 0.12f, 0.26f, cmd);
			if (!std::isfinite(cmd[0]) || !std::isfinite(cmd[1]))
			{
				s_steer.Reset();
				return false;
			}
			{
				std::lock_guard<std::mutex> lock(s_coop_mutex);
				s_coop_yaw = cmd[0];
				s_coop_elev = cmd[1];
				s_coop_ms = CameraDriver::SteadyNowMs();
			}
			if (!s_coop_said)
			{
				s_coop_said = true;
				Console.WriteLn("(VR) Gun ray: co-op aim steering the local record 0x%X (the game's gun object 0x%X).", record, obj);
			}
			s_coop_log_s += dt;
			if (Log() && s_coop_log_s >= 0.25f)
			{
				s_coop_log_s = 0.0f;
				// How far the game barrel's line passes from the aim point now (the check: about 0 once it has turned).
				const float v[3] = {T[0] - G[0], T[1] - G[1], T[2] - G[2]};
				const float t = v[0] * B[0] + v[1] * B[1] + v[2] * B[2];
				const float q[3] = {v[0] - t * B[0], v[1] - t * B[1], v[2] - t * B[2]};
				Console.WriteLn("(VR) Gun ray co-op: aim point (%.1f, %.1f, %.1f)%s, game muzzle (%.1f, %.1f, %.1f) barrel (%.3f, %.3f, "
								"%.3f): its line passes %.1f cm from the aim point; heading %.1f -> %.1f deg (lateral %.1f), elevation "
								"%.1f want %.1f, trim %.2f deg%s.",
					static_cast<double>(T[0]), static_cast<double>(T[1]), static_cast<double>(T[2]), on_enemy ? " on an enemy" : "",
					static_cast<double>(G[0]), static_cast<double>(G[1]), static_cast<double>(G[2]), static_cast<double>(B[0]),
					static_cast<double>(B[1]), static_cast<double>(B[2]),
					static_cast<double>(std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2])),
					static_cast<double>(heading * 57.2957795f), static_cast<double>(cmd[0] * 57.2957795f), static_cast<double>(lateral),
					static_cast<double>(elev_now * 57.2957795f), static_cast<double>(elev_want * 57.2957795f),
					static_cast<double>(s_steer.trim_elev * 57.2957795f), learn ? "" : " (held)");
			}
			return true;
		}
	} // namespace

	bool CoopAim(float* yaw, float* elev)
	{
		std::lock_guard<std::mutex> lock(s_coop_mutex);
		const u64 now = CameraDriver::SteadyNowMs();
		if (s_coop_ms == 0 || now < s_coop_ms || now - s_coop_ms > 100)
			return false;
		*yaw = s_coop_yaw;
		*elev = s_coop_elev;
		return true;
	}


	void OnStateLoaded()
	{
		// A block saved armed is ours to clear (or rewrite) at the next poll; the count starts again. Armed only while the
		// loaded block reads our magic (evening review m4): a block holding someone else's words is checked
		// (RegionFreeOrOurs) before the next write instead of being written over.
		s_armed = s_addr != 0 && VMManager::GetDiscCRC() == s_crc && PhysicalMelee::ArmedAfterLoad(PhysicalMelee::ReadMainRam, s_addr, s_magic);
		s_stale = true;
		s_watch_s = 0.0f;
		s_obj_armed = s_obj_addr != 0 && VMManager::GetDiscCRC() == s_obj_crc &&
		              PhysicalMelee::ArmedAfterLoad(PhysicalMelee::ReadMainRam, s_obj_addr, s_obj_magic);
		s_obj_stale = true;
		s_obj_flash_ms.store(0, std::memory_order_relaxed);
		s_load_pending = true;
	}

	bool GameFlashOnVrGun()
	{
		if (!s_obj_live.load(std::memory_order_relaxed))
			return false;
		FlashEvidence e;
		e.flash_ms = s_obj_flash_ms.load(std::memory_order_relaxed);
		return e.Suppress(CameraDriver::SteadyNowMs());
	}

	void Poll(const VRInputSnapshot& snap, GuestReader reader, float dt)
	{
		dt = std::isfinite(dt) ? std::clamp(dt, 0.0f, 0.25f) : 0.0f;
		if (s_load_pending && VMManager::HasValidVM())
		{
			// Before anything else, live or not: the game may shoot before the hands are back.
			s_load_pending = false;
			const std::string lserial = VMManager::GetDiscSerial();
			const ProfileDB::Profile* lp = lserial.empty() ? nullptr : ProfileDB::Lookup(lserial, VMManager::GetDiscCRC());
			if (lp && lp->melee.has_value())
				ClearLoadedBlocks(lp->melee.value());
		}
		const bool live = snap.generation != 0 && snap.actions_active;
		u32 record = 0;
		const ProfileDB::Profile* profile = nullptr;
		if (live && VMManager::GetState() == VMState::Running && !SplitState::Active() && VMManager::HasValidVM() &&
			CameraDriver::LocalRecord(&record))
		{
			const std::string serial = VMManager::GetDiscSerial();
			profile = serial.empty() ? nullptr : ProfileDB::Lookup(serial, VMManager::GetDiscCRC());
		}
		const ProfileDB::MeleeParams* mp =
			(profile && profile->melee.has_value() && profile->melee->has_gun_ray) ? &profile->melee.value() : nullptr;
		const ProfileDB::FeedbackParams* fb =
			(profile && profile->feedback.has_value() && profile->feedback->has_weapon) ? &profile->feedback.value() : nullptr;
		const bool setting = EmuConfig.VR.GunRayAim;
		const bool offline = NetGate::StrictlyOffline();
		// Co-op aim: private co-op (behind [VR] PrivateCoopFirstPerson), never online-safe; the same GunRayAim switch.
		const bool coop = !offline && CameraDriver::PrivateCoop() && !EmuConfig.VR.OnlineSafe;
		u32 slot = 0;
		const bool slot_known = profile && profile->camera.has_value() && profile->camera->base.has_value() &&
		                        profile->camera->base->is_indexed && CameraDriver::LocalSlot(&slot);
		static u32 s_prev_record = 0;
		const bool record_steady = profile && record != 0 && record == s_prev_record;
		s_prev_record = profile ? record : 0;

		if (profile)
		{
			const std::string verdict = !mp ? "off (the profile has no melee.gunRay)" :
			                            !fb ? "off (the profile has no feedback.weapon)" :
			                            !setting ? "off (the VR GunRayAim setting)" :
			                            (!offline && coop) ? "co-op aim (online the room's master decides a hit from the character's "
			                                                 "own aim: with the gun raised, your character's heading and aim pitch "
			                                                 "turn its own gun to where the laser points; the game's own shot)" :
			                            !offline ? "off (online: the room's master decides a hit from the character's own aim)" :
			                                       "on (a shot hits along the VR gun's ray, through the game's own shot)";
			const u32 crc = VMManager::GetDiscCRC();
			if (verdict != s_verdict || crc != s_verdict_crc)
			{
				s_verdict = verdict;
				s_verdict_crc = crc;
				Console.WriteLn("(VR) Gun ray: %s.", verdict.c_str());
			}
		}

		// A gun in hand: the local record's equipped item record, behind the weapon reader's own checks.
		bool gun = false;
		u16 item_id = 0;
		u32 item_rec = 0;
		if (mp && fb && setting && (offline || coop) && slot_known && record_steady)
		{
			u32 eq = 0, present = 0, index = 0, owner = 0, id = 0;
			if (Read(reader, record + fb->weapon_equipped_offset, 1, &eq) && eq != 0)
			{
				const u64 r = static_cast<u64>(fb->weapon_table) + static_cast<u64>(eq) * fb->weapon_stride;
				if (r + fb->weapon_stride <= Ps2MemSize::MainRam && Read(reader, static_cast<u32>(r), 1, &present) &&
					Read(reader, static_cast<u32>(r + 2), 1, &index) &&
					Read(reader, static_cast<u32>(r + fb->weapon_owner_offset), 1, &owner) &&
					Read(reader, static_cast<u32>(r + fb->weapon_id_offset), 2, &id) && present == 1 && index == eq &&
					owner == slot + 1)
				{
					item_id = static_cast<u16>(id);
					item_rec = static_cast<u32>(r);
					gun = ItemData::IsGun(ItemData::KindOf(item_id));
				}
			}
		}

		bool wrote = false;
		bool obj_wrote = false;
		bool steered = false;
		float pos[3] = {}, quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		if (gun && RightHand(snap, pos, quat))
		{
			// The laser's ray: from the game model's muzzle (GameWeaponModels) or the placeholder's, along the barrel.
			HandModel::HandState hs;
			hs.valid = true;
			hs.gun = true;
			hs.item_id = item_id;
			hs.held = ItemData::KindOf(item_id) == ItemData::Kind::LongGun ? HandModel::Held::LongGun : HandModel::Held::Pistol;
			const HandModel::WeaponHeld wh = HandModel::GetWeaponHeld();
			if (wh.known && wh.item_id == item_id)
				hs.held = wh.held;
			for (int k = 0; k < 3; k++)
				hs.pos[k] = pos[k];
			for (int k = 0; k < 4; k++)
				hs.quat[k] = quat[k];
			hs.mesh = EmuConfig.VR.GameWeaponModels;
			if (hs.mesh)
			{
				const GameFeedback::WeaponView wv = GameFeedback::GetWeapon();
				if (wv.valid && wv.item_id == item_id && wv.attach.valid)
					hs.attach = wv.attach;
				else
					GameFeedback::AttachFor(item_id, &hs.attach);
			}
			float from[3], dir[3];
			const bool ray = HandModel::ModelLaserFrom(hs, 0.0f, from, dir) ||
			                 HandModel::LaserFrom(hs.held, item_id, pos, quat, 0.0f, from, dir);
			CameraDriver::Mapping map;
			float og[3], dg[3];
			// The game's own back offset: by the weapon type's class and the character.
			u32 type = 0, cls = 0, chr = 0, back_bits = 0;
			float back = 0.0f;
			const bool back_ok = Read(reader, record + mp->weapon_type_offset, 2, &type) && type < 0x100 &&
			                     Read(reader, mp->shape_types + mp->shape_type_stride * type + mp->gun_class_offset, 1, &cls) &&
			                     Read(reader, record + mp->gun_character_offset, 1, &chr) && chr < 8 && cls < 64 &&
			                     Read(reader, mp->gun_back_table + mp->gun_back_stride * cls + 4u * chr, 4, &back_bits);
			if (back_ok)
			{
				std::memcpy(&back, &back_bits, 4);
				if (!std::isfinite(back) || back < -200.0f || back > 50.0f)
					back = 0.0f;
			}
			u32 counter = 0;
			float m[16], origin[3];
			// Before the first write the block must read zero or ours (night review m4).
			const bool blk_free = s_armed || PhysicalMelee::RegionFreeOrOurs(reader, mp->gun_block, mp->gun_block + kBlockBytes,
											 mp->gun_block + kOffMagic, mp->gun_magic);
			if (!blk_free && !s_refused_logged)
			{
				s_refused_logged = true;
				Console.Warning("(VR) Gun ray: the block at 0x%X holds someone else's words; the gun ray stays off while it does.",
					mp->gun_block);
			}
			bool mapped = (coop || blk_free) && ray && CameraDriver::GetMapping(&map) && RayToGame(from, dir, MapPoint, &map, og, dg);
			// Late review m6: a VR muzzle beyond a wall. When the line from the character's chest (bone 11, where the
			// game's own sight starts) to the VR muzzle crosses a face that stops the game's bullets (attribute 0x2: room
			// walls, shut door walls and the moving faces, not furniture), the shot (cave I) starts on the chest's side of
			// that wall and goes into it (WallShot), so the game's own wall test cuts it there; and cave O is not armed, so
			// the shell and FUN_006801A0's object come from the game's own gun object. Unknown (no chest, no grid): as before.
			bool muzzle_walled = false;
			float sg[3] = {og[0], og[1], og[2]}, sd[3] = {dg[0], dg[1], dg[2]};
			if (mapped)
			{
				static bool s_walled_said = false;
				float chest[3] = {};
				HeadInWall::LineResult lr;
				static bool s_chest_said = false;
				if (HeadInWall::CharacterChest(VMManager::GetDiscCRC(), record, chest))
				{
					lr = HeadInWall::LineBlocked(VMManager::GetDiscCRC(), chest, og, HeadInWall::kMaskShot);
					s_chest_said = false;
				}
				else if (!s_chest_said)
				{
					// Unknown, so the shot goes as before the wall test: said once until the chest reads again (evening
					// review m1).
					s_chest_said = true;
					Console.Warning("(VR) Line of sight: the character's chest (bone 11) does not read; the gun is not checked "
									"against walls.");
				}
				muzzle_walled = lr.known && lr.blocked;
				// No usable wall shot (cannot happen for a blocked line): cave I's block is not written, the game's own shot.
				if (muzzle_walled && !WallShot(chest, og, lr.t, lr.normal, back, sg, sd))
					mapped = false;
				if (muzzle_walled && !s_walled_said)
					Console.WriteLn("(VR) Gun ray: the VR muzzle (%.1f, %.1f, %.1f) is beyond a wall from the character's chest "
									"(%.1f, %.1f, %.1f) (face 0x%08X at %.0f%% of the line); a shot starts at (%.1f, %.1f, %.1f) "
									"into the wall, and the game's own gun object takes the effects, until it is back.",
						static_cast<double>(og[0]), static_cast<double>(og[1]), static_cast<double>(og[2]),
						static_cast<double>(chest[0]), static_cast<double>(chest[1]), static_cast<double>(chest[2]), lr.flags,
						static_cast<double>(lr.t * 100.0f), static_cast<double>(sg[0]), static_cast<double>(sg[1]),
						static_cast<double>(sg[2]));
				else if (!muzzle_walled && s_walled_said)
					Console.WriteLn("(VR) Gun ray: the VR muzzle is back on the character's side of the wall.");
				s_walled_said = muzzle_walled;
			}
			// Co-op aim: nothing is written but the command CameraDriver's aim turns the character by (its heading and aim
			// pitch, the local record only). A VR muzzle beyond a wall: the controller's own aim, as before.
			if (coop && mapped && !muzzle_walled)
				steered = profile->camera.has_value() && profile->camera->look_at.has_value() &&
				          SteerCoop(reader, *mp, *profile->camera->look_at, record, item_rec, type, back_ok, og, dg, dt);
			// A wall shot's origin is WallShot's own, the back offset already in it (evening review m3).
			if (!coop && mapped && ShotFrame(sg, sd, muzzle_walled ? 0.0f : back, m, origin) && Read(reader, mp->counter, 1, &counter))
			{
				const u32 b = mp->gun_block;
				if (!s_armed || s_stale)
				{
					s_stale = false;
					memWrite32(b + kOffShots, 0);
					s_last_shots = 0;
				}
				for (int k = 0; k < 16; k++)
				{
					u32 w;
					std::memcpy(&w, &m[k], 4);
					memWrite32(b + kOffMatrix + 4u * static_cast<u32>(k), w);
				}
				for (int k = 0; k < 3; k++)
				{
					u32 w;
					std::memcpy(&w, &origin[k], 4);
					memWrite32(b + kOffOrigin + 4u * static_cast<u32>(k), w);
				}
				memWrite32(b + kOffRecord, record);
				memWrite32(b + kOffStamp, counter & 0xFFu);
				memWrite32(b + kOffMagic, mp->gun_magic);
				s_armed = true;
				s_addr = b;
				s_magic = mp->gun_magic;
				s_crc = VMManager::GetDiscCRC();
				wrote = true;

				// Cave O ([VR] GunObjectAim): the fake weapon object posed so the game's own muzzle (the weapon type's
				// model-space muzzle through the object) is the VR muzzle and its shot frame is cave I's.
				u32 lm[3] = {};
				float local[3] = {};
				float om[16], opos[3];
				// Before the first write the object block must read zero or ours (late review m3).
				const u32 ob0 = mp->gun_object_block;
				const bool obj_free = s_obj_armed || !mp->has_gun_object ||
				                      PhysicalMelee::RegionFreeOrOurs(reader, ob0, ob0 + kObjBlockBytes, ob0 + kObjOffMagic,
										  mp->gun_object_magic);
				if (!obj_free && EmuConfig.VR.GunObjectAim && !s_obj_refused_logged)
				{
					s_obj_refused_logged = true;
					Console.Warning("(VR) Gun object: the block at 0x%X holds someone else's words; GunObjectAim stays off while it "
									"does.", ob0);
				}
				const bool obj_ok = obj_free && !muzzle_walled && EmuConfig.VR.GunObjectAim && mp->has_gun_object && back_ok &&
				                    Read(reader, mp->shape_types + mp->shape_type_stride * type + mp->gun_object_muzzle, 4, &lm[0]) &&
				                    Read(reader, mp->shape_types + mp->shape_type_stride * type + mp->gun_object_muzzle + 4, 4, &lm[1]) &&
				                    Read(reader, mp->shape_types + mp->shape_type_stride * type + mp->gun_object_muzzle + 8, 4, &lm[2]);
				if (obj_ok)
					std::memcpy(local, lm, sizeof(local));
				if (obj_ok && Finite3(local) && std::fabs(local[0]) < 200.0f && std::fabs(local[1]) < 200.0f &&
					std::fabs(local[2]) < 200.0f && ObjectFrame(m, og, local, om, opos))
				{
					const u32 ob = mp->gun_object_block;
					if (!s_obj_armed || s_obj_stale)
					{
						s_obj_stale = false;
						memWrite32(ob + kObjOffShots, 0);
						memWrite32(ob + kObjOffFlashes, 0);
						s_obj_flash.Reset();
						s_obj_flash_ms.store(0, std::memory_order_relaxed);
					}
					for (int k = 0; k < 16; k++)
						WriteF32(ob + kObjOffMatrix + 4u * static_cast<u32>(k), om[k]);
					for (int k = 0; k < 3; k++)
						WriteF32(ob + kObjOffPos + 4u * static_cast<u32>(k), opos[k]);
					memWrite32(ob + kObjOffRecord, record);
					memWrite32(ob + kObjOffStamp, counter & 0xFFu);
					memWrite32(ob + kObjOffMagic, mp->gun_object_magic);
					s_obj_armed = true;
					s_obj_addr = ob;
					s_obj_magic = mp->gun_object_magic;
					s_obj_crc = VMManager::GetDiscCRC();
					s_obj_live.store(true, std::memory_order_relaxed);
					obj_wrote = true;
					u32 oshots = 0, flashes = 0;
					const bool counts = Read(reader, ob + kObjOffShots, 4, &oshots) && Read(reader, ob + kObjOffFlashes, 4, &flashes);
					const u32 prev_shots = s_obj_flash.shots;
					const FlashEvidence::Seen seen =
						counts ? s_obj_flash.Update(CameraDriver::SteadyNowMs(), oshots, flashes) : FlashEvidence::Seen::Nothing;
					s_obj_flash_ms.store(s_obj_flash.flash_ms, std::memory_order_relaxed);
					if (counts && oshots != prev_shots)
					{
						std::memcpy(s_obj_muzzle, og, sizeof(og));
						Console.WriteLn("(VR) Gun object: shot %u from the VR gun object (muzzle (%.1f, %.1f, %.1f), type %u, "
										"model muzzle (%.1f, %.1f, %.1f)).", oshots, static_cast<double>(og[0]),
							static_cast<double>(og[1]), static_cast<double>(og[2]), type, static_cast<double>(local[0]),
							static_cast<double>(local[1]), static_cast<double>(local[2]));
					}
					if (seen == FlashEvidence::Seen::OtherFlash)
						Console.WriteLn("(VR) Gun object: the game's muzzle flash %u came without cave O's shot (the game's own "
										"gun object); the hands draw their own flash.", flashes);
					else if (seen == FlashEvidence::Seen::VrFlash)
					{
						u32 fw[3] = {};
						float fp[3] = {};
						if (Read(reader, ob + kObjOffFlashPos, 4, &fw[0]) && Read(reader, ob + kObjOffFlashPos + 4, 4, &fw[1]) &&
							Read(reader, ob + kObjOffFlashPos + 8, 4, &fw[2]))
						{
							std::memcpy(fp, fw, sizeof(fp));
							const float dx = fp[0] - s_obj_muzzle[0], dy = fp[1] - s_obj_muzzle[1], dz = fp[2] - s_obj_muzzle[2];
							Console.WriteLn("(VR) Gun object: the game's muzzle flash %u spawned at (%.1f, %.1f, %.1f); the VR "
											"muzzle at the shot (%.1f, %.1f, %.1f), %.2f units apart.", flashes,
								static_cast<double>(fp[0]), static_cast<double>(fp[1]), static_cast<double>(fp[2]),
								static_cast<double>(s_obj_muzzle[0]), static_cast<double>(s_obj_muzzle[1]),
								static_cast<double>(s_obj_muzzle[2]), static_cast<double>(std::sqrt(dx * dx + dy * dy + dz * dz)));
						}
					}
				}

				// A shot cave I sent along the ray: logged, and the enemies' HP watched for 0.3 s.
				u32 shots = 0;
				if (Read(reader, b + kOffShots, 4, &shots) && shots != s_last_shots)
				{
					s_last_shots = shots;
					std::memcpy(s_shot_origin, origin, sizeof(origin));
					std::memcpy(s_shot_dir, sd, sizeof(sd));
					Console.WriteLn("(VR) Gun ray: shot %u %s from (%.0f, %.0f, %.0f) toward (%.3f, %.3f, %.3f) "
									"(back %.0f).", shots, muzzle_walled ? "into the wall the VR muzzle is beyond" : "along the VR ray",
						static_cast<double>(origin[0]), static_cast<double>(origin[1]),
						static_cast<double>(origin[2]), static_cast<double>(sd[0]), static_cast<double>(sd[1]),
						static_cast<double>(sd[2]), static_cast<double>(back));
					ReadHp(reader, *mp, s_hp, s_present);
					s_watch_s = 0.3f;
				}
				else if (s_watch_s > 0.0f)
				{
					std::array<s16, 64> hp{};
					std::array<bool, 64> present{};
					ReadHp(reader, *mp, hp, present);
					for (int i = 0; i < 64; i++)
					{
						if (s_present[i] && present[i] && hp[i] < s_hp[i])
							Console.WriteLn("(VR) Gun ray: hit, enemy %d HP %d -> %d.", i, s_hp[i], hp[i]);
					}
					s_hp = hp;
					s_present = present;
					s_watch_s -= dt;
				}
				if (Log())
				{
					static float s_log_s = 0.0f;
					s_log_s += dt;
					if (s_log_s >= 0.5f)
					{
						s_log_s = 0.0f;
						Console.WriteLn("(VR) Gun ray diag: item %u, origin (%.1f, %.1f, %.1f), dir (%.3f, %.3f, %.3f), back %.1f, "
										"shots %u.", static_cast<unsigned>(item_id), static_cast<double>(origin[0]),
							static_cast<double>(origin[1]), static_cast<double>(origin[2]), static_cast<double>(dg[0]),
							static_cast<double>(dg[1]), static_cast<double>(dg[2]), static_cast<double>(back), shots);
					}
				}
			}
		}
		if (!wrote)
			Clear();
		if (!obj_wrote)
			ClearObject();
		if (!steered)
		{
			s_steer.Reset();
			s_pose_valid = false;
			s_prev_valid = false;
			s_coop_up_s = 0.0f;
			std::lock_guard<std::mutex> lock(s_coop_mutex);
			s_coop_ms = 0;
		}
		if (profile)
		{
			const std::string ov = !mp || !mp->has_gun_object ? "off (the profile has no melee.gunObject)" :
			                       !EmuConfig.VR.GunObjectAim ? "off (the VR GunObjectAim setting)" :
			                       !setting ? "off (needs GunRayAim)" :
			                       !offline ? "off (online)" :
			                                  "on (the game's own muzzle flash and shell come from the VR gun)";
			if (ov != s_obj_verdict)
			{
				s_obj_verdict = ov;
				Console.WriteLn("(VR) Gun object: %s.", ov.c_str());
			}
		}
	}
}
