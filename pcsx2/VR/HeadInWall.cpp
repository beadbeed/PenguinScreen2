// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/HeadInWall.h"

#include "Config.h"
#include "Memory.h"

#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>

namespace VR::HeadInWall
{
	namespace
	{
		Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
		Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
		Vec3 Mul(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
		float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		Vec3 Cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
		float Len(const Vec3& a) { return std::sqrt(Dot(a, a)); }
		bool Finite(const Vec3& a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

		// FUN_005E58E0's question: is p (on the plane) inside the triangle, edges included. Either winding: the three
		// edge tests only have to agree in sign.
		bool InsideTriangle(const Face& f, const Vec3& p)
		{
			bool pos = false, neg = false;
			for (int i = 0; i < 3; i++)
			{
				const Vec3& a = f.v[i];
				const Vec3& b = f.v[(i + 1) % 3];
				const float s = Dot(Cross(Sub(b, a), Sub(p, a)), f.n);
				pos = pos || s > 1.0e-3f;
				neg = neg || s < -1.0e-3f;
			}
			return !(pos && neg);
		}

		// File #1 (SLPM-65428), every CRC the profile lists.
		bool IsFile1(u32 crc)
		{
			return crc == 0x32088394u || crc == 0x198EFDC1u || crc == 0x03C8F393u || crc == 0x32097C02u || crc == 0x03C90C05u;
		}

		constexpr u32 kGridA = 0x3938A8u;     // wall grid header
		constexpr u32 kGridB = 0x3938C8u;     // floor grid header
		constexpr u32 kIgnoreMask = 0x3938E8u; // u32 global ignore mask
		constexpr u32 kDynFaces = 0x70E750u;   // moving faces, 0x38 apart
		constexpr u32 kDynCount = 0x23BEC4u;   // s32 their count
		constexpr u32 kDoorState = 0x4747C1u;  // u8 per door slot, 0x60 apart: 2 = open
		constexpr u32 kStatus = 0x48BF60u;     // u8 scenario status, 2 = in game
		constexpr u32 kRoom = 0x3065ACu;       // u16 room
		constexpr u32 kOverlay = 0x540004u;    // GAME overlay resident: 3
		constexpr u32 kFaceSize = 0x38u;
		constexpr u32 kMaxDyn = 64;

		bool ReadGuest(u32 address, u32 size, void* out)
		{
			const u64 a = address & 0x1FFFFFFFu;
			if (a + size > Ps2MemSize::MainRam || (a & 3) != 0 || (size & 3) != 0)
				return false;
			u8* o = static_cast<u8*>(out);
			for (u32 i = 0; i < size; i += 4)
			{
				const u32 w = memRead32(static_cast<u32>(a) + i);
				std::memcpy(o + i, &w, 4);
			}
			return true;
		}

		struct Live
		{
			u32 crc = 0;
			u16 room = 0xFFFF;
			u8 header_a[0x20] = {};
			u8 header_b[0x20] = {};
			u32 age = 0;
			bool valid = false;
			Grid a, b;
			u64 last_ms = 0;
			float fade = 0.0f;
			bool clamping = false;
			u64 log_ms = 0;
			u32 clamps = 0;
			float max_pushed = 0.0f;
			bool logged_grid = false;
		};
		Live s_live;
		std::atomic<float> s_fade{0.0f};

		u64 NowMs()
		{
			return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}
	} // namespace

	bool DecodeFace(const u8* bytes, Face* out)
	{
		Face f;
		std::memcpy(&f.flags, bytes, 4);
		float v[13];
		std::memcpy(v, bytes + 4, sizeof(v));
		for (int i = 0; i < 3; i++)
			f.v[i] = {v[i * 3 + 0], v[i * 3 + 1], v[i * 3 + 2]};
		f.n = {v[9], v[10], v[11]};
		f.d = v[12];
		if (!Finite(f.v[0]) || !Finite(f.v[1]) || !Finite(f.v[2]) || !Finite(f.n) || !std::isfinite(f.d))
			return false;
		const float l = Len(f.n);
		if (!(l > 0.98f && l < 1.02f))
			return false;
		for (const Vec3& c : f.v)
			if (std::abs(c.x) > 1.0e6f || std::abs(c.y) > 1.0e6f || std::abs(c.z) > 1.0e6f)
				return false;
		*out = f;
		return true;
	}

	bool SegmentHitsFace(const Face& f, const Vec3& a, const Vec3& b, float* t)
	{
		const float da = Dot(f.n, a) + f.d;
		const float db = Dot(f.n, b) + f.d;
		if (!(da >= 0.0f) || !(db < 0.0f))
			return false;
		const float tt = da / (da - db);
		if (!(tt >= 0.0f && tt <= 1.0f))
			return false;
		const Vec3 p = Add(a, Mul(Sub(b, a), tt));
		if (!InsideTriangle(f, p))
			return false;
		if (t)
			*t = tt;
		return true;
	}

	Vec3 ClosestPointOnFace(const Face& f, const Vec3& p)
	{
		// Ericson, Real-Time Collision Detection 5.1.5.
		const Vec3& a = f.v[0];
		const Vec3& b = f.v[1];
		const Vec3& c = f.v[2];
		const Vec3 ab = Sub(b, a), ac = Sub(c, a), ap = Sub(p, a);
		const float d1 = Dot(ab, ap), d2 = Dot(ac, ap);
		if (d1 <= 0.0f && d2 <= 0.0f)
			return a;
		const Vec3 bp = Sub(p, b);
		const float d3 = Dot(ab, bp), d4 = Dot(ac, bp);
		if (d3 >= 0.0f && d4 <= d3)
			return b;
		const float vc = d1 * d4 - d3 * d2;
		if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
			return Add(a, Mul(ab, d1 / (d1 - d3)));
		const Vec3 cp = Sub(p, c);
		const float d5 = Dot(ab, cp), d6 = Dot(ac, cp);
		if (d6 >= 0.0f && d5 <= d6)
			return c;
		const float vb = d5 * d2 - d1 * d6;
		if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
			return Add(a, Mul(ac, d2 / (d2 - d6)));
		const float va = d3 * d6 - d5 * d4;
		if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
			return Add(b, Mul(Sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
		const float denom = 1.0f / (va + vb + vc);
		return Add(a, Add(Mul(ab, vb * denom), Mul(ac, vc * denom)));
	}

	Result Clamp(const std::vector<Face>& walls, const std::vector<Face>& floors, const Vec3& head, const Vec3& eye,
		const Params& p)
	{
		Result r;
		r.eye = eye;
		Vec3 e = eye;
		// 1. The segment from the head to the eye, nearest crossing (FUN_005491E0 on every blocking face).
		const auto segment = [&](const Vec3& to, Vec3* out) {
			float best = 2.0f;
			int idx = -1;
			for (size_t i = 0; i < walls.size(); i++)
			{
				float t = 0.0f;
				if (SegmentHitsFace(walls[i], head, to, &t) && t < best)
				{
					best = t;
					idx = static_cast<int>(i);
				}
			}
			if (idx < 0)
				return -1;
			const Vec3 d = Sub(to, head);
			const float len = Len(d);
			const float keep = std::max(0.0f, best * len - p.margin);
			*out = (len > 1.0e-4f) ? Add(head, Mul(d, keep / len)) : head;
			return idx;
		};
		r.hit_face = segment(e, &e);
		// 2. A sphere round the eye: off every face it is in front of and nearer than radius.
		for (int it = 0; it < 3; it++)
		{
			bool moved = false;
			for (const Face& f : walls)
			{
				const float dist = Dot(f.n, e) + f.d;
				if (!(dist >= 0.0f) || dist >= p.radius)
					continue;
				const Vec3 c = ClosestPointOnFace(f, e);
				const Vec3 v = Sub(e, c);
				const float l = Len(v);
				if (l >= p.radius)
					continue;
				e = (l > 1.0e-3f) ? Add(e, Mul(v, (p.radius - l) / l)) : Add(e, Mul(f.n, p.radius - dist));
				moved = true;
			}
			if (!moved)
				break;
		}
		// 3. The push must not have taken it through another wall.
		Vec3 again;
		if (segment(e, &again) >= 0)
			e = again;
		// 4. The floor under the head (grid B: the highest upward face below the head point).
		float floor_y = -1.0e30f;
		for (const Face& f : floors)
		{
			if (!(f.n.y > 0.7f))
				continue;
			const float y = -(f.n.x * e.x + f.n.z * e.z + f.d) / f.n.y;
			if (!std::isfinite(y) || y > head.y)
				continue;
			const Vec3 on{e.x, y, e.z};
			if (InsideTriangle(f, on))
				floor_y = std::max(floor_y, y);
		}
		if (floor_y > -1.0e29f && e.y < floor_y + p.floor_clear)
			e.y = floor_y + p.floor_clear;
		r.eye = e;
		r.pushed = Len(Sub(e, eye));
		r.clamped = r.pushed > 0.01f;
		return r;
	}

	float FadeFor(float pushed, const Params& p)
	{
		if (!(pushed > p.fade_from) || !(p.fade_full > p.fade_from))
			return 0.0f;
		const float t = std::clamp((pushed - p.fade_from) / (p.fade_full - p.fade_from), 0.0f, 1.0f);
		return p.fade_max * t * t * (3.0f - 2.0f * t);
	}

	bool ReadGrid(const Reader& read, u32 base, Grid* out)
	{
		u32 h[8];
		if (!read(base, sizeof(h), h))
			return false;
		Grid g;
		g.cell_x = static_cast<s32>(h[0]);
		g.cell_z = static_cast<s32>(h[1]);
		g.nx = h[2];
		g.nz = h[3];
		g.origin_x = static_cast<s32>(h[4]);
		g.origin_z = static_cast<s32>(h[5]);
		const u32 cells = h[6];
		if (g.nx < 1 || g.nx > 32 || g.nz < 1 || g.nz > 32 || g.cell_x <= 0 || g.cell_z <= 0)
			return false;
		std::vector<u32> addrs;
		for (u32 c = 0; c < g.nx * g.nz; c++)
		{
			u32 list = 0;
			if (!read(cells + c * 4, 4, &list))
				return false;
			u32 n = 0;
			for (;; n++)
			{
				if (n >= 4096)
					return false;
				u32 fa = 0;
				if (!read(list + n * 4, 4, &fa))
					return false;
				if (fa == 0xFFFFFFFFu)
					break;
				addrs.push_back(fa);
			}
		}
		std::sort(addrs.begin(), addrs.end());
		addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
		if (addrs.size() > 8192)
			return false;
		for (u32 fa : addrs)
		{
			u8 b[kFaceSize];
			Face f;
			if (!read(fa, kFaceSize, b) || !DecodeFace(b, &f))
				return false;
			g.face_addr.push_back(fa);
			g.faces.push_back(f);
		}
		*out = std::move(g);
		return true;
	}

	bool BlocksPlayer(u32 flags, u32 ignore_mask, const std::function<bool(u32 slot)>& door_open)
	{
		const u32 hi = flags & 0xFFFF0000u;
		if ((hi & 0x00010000u) == 0) // the player's attribute mask 0x0001
			return false;
		if ((hi & ignore_mask) != 0)
			return false;
		const u32 cls = flags & 0xFFu;
		if (cls >= 16) // the class mask 0xFFFF: classes 0-15
			return false;
		if (cls == 1)
		{
			const u32 attr = flags >> 16;
			const u32 slot = (attr >> 8) & 0xFu;
			if (door_open && door_open(slot) && (attr & 0x40u) == 0)
				return false;
		}
		return true;
	}

	void Reset()
	{
		s_live = Live{};
		s_fade.store(0.0f, std::memory_order_release);
	}

	float CurrentFade()
	{
		return s_fade.load(std::memory_order_acquire);
	}

	bool Apply(u32 crc, const float head[3], float eye[3])
	{
		Live& L = s_live;
		const u64 now = NowMs();
		const float dt = (L.last_ms != 0 && now > L.last_ms) ? std::min(0.1f, static_cast<float>(now - L.last_ms) / 1000.0f) : 0.0f;
		L.last_ms = now;
		const auto ease_to = [&](float target) {
			// About 0.1 s in and out.
			const float k = std::min(1.0f, dt * 10.0f);
			L.fade += (target - L.fade) * k;
			if (L.fade < 0.002f && target == 0.0f)
				L.fade = 0.0f;
			s_fade.store(L.fade, std::memory_order_release);
		};
		if (!EmuConfig.VR.HeadInWall || !IsFile1(crc) || memRead32(kOverlay) != 3 || memRead8(kStatus) != 2)
		{
			L.valid = false;
			ease_to(0.0f);
			return false;
		}
		const Reader read = [](u32 a, u32 n, void* o) { return ReadGuest(a, n, o); };
		u8 ha[0x20], hb[0x20];
		const u16 room = static_cast<u16>(memRead16(kRoom));
		if (!ReadGuest(kGridA, 0x20, ha) || !ReadGuest(kGridB, 0x20, hb))
			return false;
		if (!L.valid || L.crc != crc || L.room != room || std::memcmp(ha, L.header_a, 0x20) != 0 ||
			std::memcmp(hb, L.header_b, 0x20) != 0 || ++L.age >= 120)
		{
			L.crc = crc;
			L.room = room;
			std::memcpy(L.header_a, ha, 0x20);
			std::memcpy(L.header_b, hb, 0x20);
			L.age = 0;
			Grid a, b;
			const bool ok_a = ReadGrid(read, kGridA, &a);
			const bool ok_b = ReadGrid(read, kGridB, &b);
			if (!ok_a)
			{
				if (L.valid || !L.logged_grid)
					Console.Warning("(VR) HeadInWall: room %u's wall grid does not read as expected; the eye is left alone.", room);
				L.logged_grid = true;
				L.valid = false;
				ease_to(0.0f);
				return false;
			}
			const bool changed = !L.valid || a.face_addr != L.a.face_addr;
			L.a = std::move(a);
			L.b = ok_b ? std::move(b) : Grid{};
			if (changed)
				Console.WriteLn("(VR) HeadInWall: room %u, wall grid %ux%u (%d x %d cm cells), %zu wall faces, %zu floor faces.", room,
					L.a.nx, L.a.nz, L.a.cell_x, L.a.cell_z, L.a.faces.size(), L.b.faces.size());
			L.valid = true;
			L.logged_grid = false;
		}
		const u32 ignore = memRead32(kIgnoreMask);
		const auto door_open = [](u32 slot) { return memRead8(kDoorState + slot * 0x60u) == 2; };
		std::vector<Face> walls;
		walls.reserve(L.a.faces.size() + 16);
		for (const Face& f : L.a.faces)
			if (BlocksPlayer(f.flags, ignore, door_open))
				walls.push_back(f);
		const s32 dyn = static_cast<s32>(memRead32(kDynCount));
		for (s32 i = 0; i < std::min<s32>(dyn, kMaxDyn); i++)
		{
			u8 b[kFaceSize];
			Face f;
			if (ReadGuest(kDynFaces + static_cast<u32>(i) * kFaceSize, kFaceSize, b) && DecodeFace(b, &f) &&
				BlocksPlayer(f.flags, ignore, door_open))
				walls.push_back(f);
		}
		const Params p;
		const Vec3 h{head[0], head[1], head[2]};
		const Vec3 want{eye[0], eye[1], eye[2]};
		if (!Finite(h) || !Finite(want))
			return false;
		const Result r = Clamp(walls, L.b.faces, h, want, p);
		if (!Finite(r.eye))
			return false;
		eye[0] = r.eye.x;
		eye[1] = r.eye.y;
		eye[2] = r.eye.z;
		ease_to(FadeFor(r.pushed, p));
		if (r.clamped)
		{
			L.max_pushed = std::max(L.max_pushed, r.pushed);
			if (!L.clamping || now - L.log_ms > 2000)
			{
				L.clamps++;
				L.log_ms = now;
				Console.WriteLn("(VR) HeadInWall: eye stopped %.1f cm from where the head is (most this episode %.1f), %s; "
								"eye (%.1f, %.1f, %.1f), head point (%.1f, %.1f, %.1f), fade %.2f, %zu blocking faces.",
					static_cast<double>(r.pushed), static_cast<double>(L.max_pushed),
					r.hit_face >= 0 ? "the head-to-eye line crossed a wall" : "kept off a wall face near it",
					static_cast<double>(r.eye.x), static_cast<double>(r.eye.y), static_cast<double>(r.eye.z),
					static_cast<double>(h.x), static_cast<double>(h.y), static_cast<double>(h.z), static_cast<double>(L.fade),
					walls.size());
			}
			L.clamping = true;
		}
		else if (L.clamping)
		{
			Console.WriteLn("(VR) HeadInWall: the eye is free again (it was stopped up to %.1f cm).",
				static_cast<double>(L.max_pushed));
			L.clamping = false;
			L.max_pushed = 0.0f;
		}
		return true;
	}

	int SelfTest()
	{
		int fail = 0;
		const auto check = [&fail](bool ok, const char* name) {
			if (!ok)
			{
				++fail;
				Console.Error("(VR) HeadInWall self-test FAIL: %s", name);
			}
		};
		const auto near = [](float a, float b, float tol = 0.05f) { return std::abs(a - b) <= tol; };
		// A wall at z = 100 facing the room (-z), x -200..200, y -50..300: two triangles, n = (0,0,-1), d = 100.
		const auto wall_at = [](float z) {
			std::vector<Face> w(2);
			for (Face& f : w)
			{
				f.flags = 0x00010000u;
				f.n = {0.0f, 0.0f, -1.0f};
				f.d = z;
			}
			// Counter-clockwise seen from the front (-z side looking +z): edge cross products along n.
			w[0].v[0] = {-200.0f, -50.0f, z};
			w[0].v[1] = {-200.0f, 300.0f, z};
			w[0].v[2] = {200.0f, -50.0f, z};
			w[1].v[0] = {200.0f, -50.0f, z};
			w[1].v[1] = {-200.0f, 300.0f, z};
			w[1].v[2] = {200.0f, 300.0f, z};
			return w;
		};
		std::vector<Face> walls = wall_at(100.0f);
		// The winding the test triangles use must read inside for a point in the middle.
		float t = 0.0f;
		check(SegmentHitsFace(walls[0], {-100.0f, 50.0f, 0.0f}, {-100.0f, 50.0f, 200.0f}, &t) && near(t, 0.5f, 1.0e-4f),
			"segment through the middle of a face: hit half way");
		check(!SegmentHitsFace(walls[0], {-100.0f, 50.0f, 200.0f}, {-100.0f, 50.0f, 0.0f}, &t),
			"one-sided: from behind the face no hit (FUN_005491E0)");
		check(!SegmentHitsFace(walls[0], {500.0f, 50.0f, 0.0f}, {500.0f, 50.0f, 200.0f}, &t), "outside the triangle: no hit");
		check(!SegmentHitsFace(walls[0], {-100.0f, 50.0f, 0.0f}, {-100.0f, 50.0f, 90.0f}, &t), "short of the plane: no hit");
		const Vec3 c = ClosestPointOnFace(walls[0], {-100.0f, 50.0f, 40.0f});
		check(near(c.x, -100.0f) && near(c.y, 50.0f) && near(c.z, 100.0f), "closest point: straight onto the face");
		const Vec3 c2 = ClosestPointOnFace(walls[0], {-300.0f, 400.0f, 90.0f});
		check(near(c2.x, -200.0f) && near(c2.y, 300.0f) && near(c2.z, 100.0f), "closest point: a corner");

		const Params p;
		const Vec3 head{0.0f, 155.0f, 0.0f};
		Result r = Clamp(walls, {}, head, {0.0f, 155.0f, 130.0f}, p);
		check(r.clamped && r.hit_face >= 0 && near(r.eye.z, 90.0f) && near(r.eye.x, 0.0f) && near(r.eye.y, 155.0f),
			"head 30 cm into a wall: the eye stops 9 cm short, then 10 cm off it (z 90)");
		check(near(r.pushed, 40.0f) && near(FadeFor(r.pushed, p), p.fade_max), "40 cm stopped: fade near full");
		r = Clamp(walls, {}, head, {0.0f, 155.0f, 50.0f}, p);
		check(!r.clamped && near(r.eye.z, 50.0f), "50 cm off the wall: the eye is left alone");
		r = Clamp(walls, {}, head, {0.0f, 155.0f, 95.0f}, p);
		check(r.clamped && r.hit_face < 0 && near(r.eye.z, 90.0f) && near(FadeFor(r.pushed, p), 0.0f),
			"5 cm off the wall: kept 10 cm off by the sphere, no fade");
		r = Clamp(walls, {}, {0.0f, 155.0f, 150.0f}, {0.0f, 155.0f, 120.0f}, p);
		check(!r.clamped, "behind a one-sided wall (head and eye): left alone");
		// A corner: a second wall at x = 100 facing -x; the eye pushed toward both.
		std::vector<Face> corner = walls;
		for (Face f : wall_at(100.0f))
		{
			for (Vec3& v : f.v)
				v = {v.z, v.y, -v.x};
			f.n = {-1.0f, 0.0f, 0.0f};
			f.d = 100.0f;
			corner.push_back(f);
		}
		r = Clamp(corner, {}, head, {120.0f, 155.0f, 120.0f}, p);
		check(r.clamped && r.eye.x <= 91.0f && r.eye.z <= 91.0f && r.eye.x > 50.0f && r.eye.z > 50.0f,
			"a corner: the eye stays inside both walls");
		// The floor: y 0 facing up; an eye at y 5 is lifted to 20.
		std::vector<Face> floors(1);
		floors[0].flags = 0;
		floors[0].n = {0.0f, 1.0f, 0.0f};
		floors[0].d = 0.0f;
		floors[0].v[0] = {-500.0f, 0.0f, -500.0f};
		floors[0].v[1] = {-500.0f, 0.0f, 500.0f};
		floors[0].v[2] = {500.0f, 0.0f, -500.0f};
		r = Clamp({}, floors, head, {-100.0f, 5.0f, -100.0f}, p);
		check(near(r.eye.y, 20.0f), "the floor under the head: the eye kept 20 cm above it");
		r = Clamp({}, floors, head, {-100.0f, 60.0f, -100.0f}, p);
		check(!r.clamped, "well above the floor: left alone");
		check(FadeFor(10.0f, p) == 0.0f && near(FadeFor(25.0f, p), p.fade_max * 0.5f, 0.01f) && near(FadeFor(80.0f, p), p.fade_max),
			"fade: none below 15 cm, half at 25, full past 35");

		// The player's filter (FUN_005E6050, mask 0x0001FFFF).
		const auto shut = [](u32) { return false; };
		const auto open = [](u32 slot) { return slot == 2; };
		check(BlocksPlayer(0x00010000u, 0, shut) && !BlocksPlayer(0x00200000u, 0, shut), "attribute 0x1 blocks, 0x20 (enemies) not");
		check(!BlocksPlayer(0x00010000u, 0x00010000u, shut), "the global ignore mask lets it through");
		check(!BlocksPlayer(0x00010010u, 0, shut), "a class past 15 is never matched");
		check(BlocksPlayer(0x02010001u, 0, shut) && !BlocksPlayer(0x02010001u, 0, open) && BlocksPlayer(0x02410001u, 0, open) &&
				  BlocksPlayer(0x03010001u, 0, open),
			"a door wall (class 1, slot (attr >> 8) & 15): open when its door reads 2 unless attr 0x40");

		// Grid parsing on a synthetic grid A: 2 x 2 cells, faces shared between cells, the list end 0xFFFFFFFF.
		std::vector<u8> ram(0x4000, 0);
		const auto put32 = [&ram](u32 a, u32 v) { std::memcpy(&ram[a], &v, 4); };
		const auto putf = [&ram](u32 a, float v) { std::memcpy(&ram[a], &v, 4); };
		const u32 hdr = 0x100, cells = 0x200, lists = 0x300, faces = 0x1000;
		put32(hdr + 0x00, 300);
		put32(hdr + 0x04, 300);
		put32(hdr + 0x08, 2);
		put32(hdr + 0x0C, 2);
		put32(hdr + 0x10, 7750);
		put32(hdr + 0x14, 12200);
		put32(hdr + 0x18, cells);
		put32(hdr + 0x1C, faces);
		for (u32 i = 0; i < 3; i++)
		{
			const u32 f = faces + i * kFaceSize;
			put32(f, 0x00010000u | i);
			for (int k = 0; k < 9; k++)
				putf(f + 4 + 4 * k, static_cast<float>(k * 10 + i));
			putf(f + 0x28, 0.0f);
			putf(f + 0x2C, 0.0f);
			putf(f + 0x30, -1.0f);
			putf(f + 0x34, 100.0f + i);
		}
		// cell 0: faces 0, 1; cell 1: face 1; cell 2: none; cell 3: faces 2, 0.
		const u32 l0 = lists, l1 = lists + 0x20, l2 = lists + 0x40, l3 = lists + 0x60;
		put32(cells + 0, l0);
		put32(cells + 4, l1);
		put32(cells + 8, l2);
		put32(cells + 12, l3);
		put32(l0, faces);
		put32(l0 + 4, faces + kFaceSize);
		put32(l0 + 8, 0xFFFFFFFFu);
		put32(l1, faces + kFaceSize);
		put32(l1 + 4, 0xFFFFFFFFu);
		put32(l2, 0xFFFFFFFFu);
		put32(l3, faces + 2 * kFaceSize);
		put32(l3 + 4, faces);
		put32(l3 + 8, 0xFFFFFFFFu);
		const Reader rd = [&ram](u32 a, u32 n, void* o) {
			if (static_cast<u64>(a) + n > ram.size())
				return false;
			std::memcpy(o, &ram[a], n);
			return true;
		};
		Grid g;
		check(ReadGrid(rd, hdr, &g) && g.nx == 2 && g.nz == 2 && g.cell_z == 300 && g.origin_x == 7750 && g.faces.size() == 3 &&
				  g.face_addr[0] == faces && near(g.faces[2].d, 102.0f) && (g.faces[1].flags & 0xFF) == 1 &&
				  near(g.faces[2].v[1].x, 32.0f),
			"grid: every face once, decoded (flags, vertices, plane)");
		put32(hdr + 0x08, 0);
		check(!ReadGrid(rd, hdr, &g), "grid: 0 cells refused");
		put32(hdr + 0x08, 2);
		put32(l2, faces + 0x3000); // past the synthetic RAM
		check(!ReadGrid(rd, hdr, &g), "grid: a face address that does not read refuses the grid");
		put32(l2, 0xFFFFFFFFu);
		putf(faces + kFaceSize + 0x30, 0.5f);
		check(!ReadGrid(rd, hdr, &g), "grid: a face without a unit normal refuses the grid");
		Console.WriteLn("(VR) HeadInWall self-test: %d failure(s).", fail);
		return fail;
	}
} // namespace VR::HeadInWall
