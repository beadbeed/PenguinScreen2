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
#include <map>

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
		// All the face-list entries of a grid together (a face can sit in several cells; the unique faces stop at 8192).
		constexpr u32 kMaxGridEntries = 65536u;
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

		// A copy of the room's grids (A walls, B floors), read again on a new room, game or header and every 120 uses.
		struct GridCache
		{
			u32 crc = 0;
			u16 room = 0xFFFF;
			u8 header_a[0x20] = {};
			u8 header_b[0x20] = {};
			u32 age = 0;
			bool valid = false;
			Grid a, b;
			bool logged_grid = false;
		};

		enum class Refresh
		{
			Ok,
			NoHeader, // the headers did not read
			BadGrid,  // grid A did not read as expected
		};

		// CPU thread. changed: grid A's faces are new (a new room, or the first read).
		Refresh RefreshGrid(GridCache& c, u32 crc, bool* changed)
		{
			if (changed)
				*changed = false;
			const Reader read = [](u32 a, u32 n, void* o) { return ReadGuest(a, n, o); };
			u8 ha[0x20], hb[0x20];
			const u16 room = static_cast<u16>(memRead16(kRoom));
			if (!ReadGuest(kGridA, 0x20, ha) || !ReadGuest(kGridB, 0x20, hb))
			{
				c.valid = false;
				return Refresh::NoHeader;
			}
			if (c.valid && c.crc == crc && c.room == room && std::memcmp(ha, c.header_a, 0x20) == 0 &&
				std::memcmp(hb, c.header_b, 0x20) == 0 && ++c.age < 120)
				return Refresh::Ok;
			c.crc = crc;
			c.room = room;
			std::memcpy(c.header_a, ha, 0x20);
			std::memcpy(c.header_b, hb, 0x20);
			c.age = 0;
			Grid a, b;
			const bool ok_a = ReadGrid(read, kGridA, &a);
			const bool ok_b = ReadGrid(read, kGridB, &b);
			if (!ok_a)
			{
				c.valid = false;
				return Refresh::BadGrid;
			}
			if (changed)
				*changed = !c.valid || a.face_addr != c.a.face_addr;
			c.a = std::move(a);
			c.b = ok_b ? std::move(b) : Grid{};
			c.valid = true;
			return Refresh::Ok;
		}

		// The moving faces (0x70E750), read every use.
		void ReadDynamic(std::vector<Face>* out)
		{
			const s32 dyn = static_cast<s32>(memRead32(kDynCount));
			for (s32 i = 0; i < std::min<s32>(dyn, kMaxDyn); i++)
			{
				u8 b[kFaceSize];
				Face f;
				if (ReadGuest(kDynFaces + static_cast<u32>(i) * kFaceSize, kFaceSize, b) && DecodeFace(b, &f))
					out->push_back(f);
			}
		}

		struct Live
		{
			GridCache g;
			u64 last_ms = 0;
			float fade = 0.0f;
			bool clamping = false;
			u64 log_ms = 0;
			u32 clamps = 0;
			float max_pushed = 0.0f;
		};
		Live s_live;
		GridCache s_lines; // LineBlocked's own copy
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
		// Every list entry of every cell counts toward one cap (night review m9): a malformed grid stops after
		// kMaxGridEntries guest reads, not 32 x 32 x 4096.
		u32 total = 0;
		for (u32 c = 0; c < g.nx * g.nz; c++)
		{
			u32 list = 0;
			if (!read(cells + c * 4, 4, &list))
				return false;
			u32 n = 0;
			for (;; n++)
			{
				if (n >= 4096 || ++total > kMaxGridEntries)
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
		return FaceCounts(flags, kMaskPlayer, ignore_mask, door_open);
	}

	int SegmentBlockedBy(const std::vector<Face>& faces, u32 mask_a, u32 ignore_mask,
		const std::function<bool(u32 slot)>& door_open, const Vec3& a, const Vec3& b, float* t)
	{
		if (!Finite(a) || !Finite(b))
			return -1;
		float best = 2.0f;
		int idx = -1;
		for (size_t i = 0; i < faces.size(); i++)
		{
			float tt = 0.0f;
			if (SegmentHitsFace(faces[i], a, b, &tt) && tt < best && FaceCounts(faces[i].flags, mask_a, ignore_mask, door_open))
			{
				best = tt;
				idx = static_cast<int>(i);
			}
		}
		if (idx >= 0 && t)
			*t = best;
		return idx;
	}

	bool FaceCounts(u32 flags, u32 mask_a, u32 ignore_mask, const std::function<bool(u32 slot)>& door_open)
	{
		const u32 cls = flags & 0xFFu;
		// The class: one bit per class 0-15 in the mask's low half (a class past 15 never matches).
		if (cls >= 16 || (mask_a & (1u << cls)) == 0)
			return false;
		const u32 hi = flags & 0xFFFF0000u;
		if ((hi & mask_a & 0xFFFF0000u) == 0) // the attribute bits the caller asks for
			return false;
		if ((hi & ignore_mask) != 0)
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
			L.g.valid = false;
			ease_to(0.0f);
			return false;
		}
		const bool was_valid = L.g.valid;
		bool changed = false;
		const Refresh rf = RefreshGrid(L.g, crc, &changed);
		if (rf != Refresh::Ok)
		{
			// The dim eases off too (night review m9), as for any other reason the eye is left alone.
			if (rf == Refresh::BadGrid && (was_valid || !L.g.logged_grid))
			{
				Console.Warning("(VR) HeadInWall: room %u's wall grid does not read as expected; the eye is left alone.", L.g.room);
				L.g.logged_grid = true;
			}
			ease_to(0.0f);
			return false;
		}
		L.g.logged_grid = false;
		if (changed)
			Console.WriteLn("(VR) HeadInWall: room %u, wall grid %ux%u (%d x %d cm cells), %zu wall faces, %zu floor faces.", L.g.room,
				L.g.a.nx, L.g.a.nz, L.g.a.cell_x, L.g.a.cell_z, L.g.a.faces.size(), L.g.b.faces.size());
		const u32 ignore = memRead32(kIgnoreMask);
		const auto door_open = [](u32 slot) { return memRead8(kDoorState + slot * 0x60u) == 2; };
		std::vector<Face> walls;
		walls.reserve(L.g.a.faces.size() + 16);
		for (const Face& f : L.g.a.faces)
			if (BlocksPlayer(f.flags, ignore, door_open))
				walls.push_back(f);
		std::vector<Face> dyn;
		ReadDynamic(&dyn);
		for (const Face& f : dyn)
			if (BlocksPlayer(f.flags, ignore, door_open))
				walls.push_back(f);
		const Params p;
		const Vec3 h{head[0], head[1], head[2]};
		const Vec3 want{eye[0], eye[1], eye[2]};
		if (!Finite(h) || !Finite(want))
			return false;
		const Result r = Clamp(walls, L.g.b.faces, h, want, p);
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

	LineResult LineBlocked(u32 crc, const float a[3], const float b[3], u32 mask_a)
	{
		LineResult r;
		const Vec3 va{a[0], a[1], a[2]};
		const Vec3 vb{b[0], b[1], b[2]};
		if (!Finite(va) || !Finite(vb) || !IsFile1(crc) || memRead32(kOverlay) != 3 || memRead8(kStatus) != 2)
		{
			s_lines.valid = false;
			return r;
		}
		if (RefreshGrid(s_lines, crc, nullptr) != Refresh::Ok)
			return r;
		const u32 ignore = memRead32(kIgnoreMask);
		const auto door_open = [](u32 slot) { return memRead8(kDoorState + slot * 0x60u) == 2; };
		r.known = true;
		float t = 0.0f;
		const int i = SegmentBlockedBy(s_lines.a.faces, mask_a, ignore, door_open, va, vb, &t);
		if (i >= 0)
		{
			r.blocked = true;
			const Face& f = s_lines.a.faces[static_cast<size_t>(i)];
			r.flags = f.flags;
			r.t = t;
			r.normal[0] = f.n.x;
			r.normal[1] = f.n.y;
			r.normal[2] = f.n.z;
		}
		std::vector<Face> dyn;
		ReadDynamic(&dyn);
		float td = 0.0f;
		const int j = SegmentBlockedBy(dyn, mask_a, ignore, door_open, va, vb, &td);
		if (j >= 0 && (!r.blocked || td < r.t))
		{
			r.blocked = true;
			const Face& f = dyn[static_cast<size_t>(j)];
			r.flags = f.flags;
			r.t = td;
			r.normal[0] = f.n.x;
			r.normal[1] = f.n.y;
			r.normal[2] = f.n.z;
		}
		return r;
	}

	bool ChestFrom(const Reader& read, u32 record, float out[3])
	{
		if (record == 0)
			return false;
		const auto in_ram = [](u32 p) { return (p & 3) == 0 && (p & 0x1FFFFFFFu) >= 0x100000u && (p & 0x1FFFFFFFu) + 0x40 <= Ps2MemSize::MainRam; };
		constexpr u32 kChestBone = 11;
		if (!in_ram(record))
			return false;
		u32 model = 0, count_word = 0, bone = 0;
		if (!read(record + 0x34, 4, &model) || !in_ram(model) || !read(model + 0x814, 4, &count_word) ||
			kChestBone >= (count_word >> 16) || !read(model + 0x820 + kChestBone * 4, 4, &bone) || !in_ram(bone))
			return false;
		u32 w[3], pw[3];
		if (!read(bone + 0x30, 12, w) || !read(record + 0x38, 12, pw))
			return false;
		Vec3 c, pos;
		std::memcpy(&c, w, 12);
		std::memcpy(&pos, pw, 12);
		if (!Finite(c) || !Finite(pos) || Len(Sub(c, pos)) > 300.0f)
			return false;
		out[0] = c.x;
		out[1] = c.y;
		out[2] = c.z;
		return true;
	}

	bool CharacterChest(u32 crc, u32 record, float out[3])
	{
		// Every read masked and bounded (ReadGuest; evening review m5): a garbage pointer (the record mid-change) is refused
		// instead of reaching the EE's TLB.
		if (!IsFile1(crc))
			return false;
		const Reader read = [](u32 a, u32 n, void* o) { return ReadGuest(a, n, o); };
		return ChestFrom(read, record, out);
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

		// The game's other masks on File #1's own attribute words: 0x2F room walls, 0x29 furniture, 0x27 moving faces.
		check(FaceCounts(0x002F0000u, kMaskSight, 0, shut) && FaceCounts(0x002F0000u, kMaskShot, 0, shut) &&
				  FaceCounts(0x002F0000u, kMaskPlayer, 0, shut),
			"a room wall (0x2F) stops sight, shots and the player");
		check(!FaceCounts(0x00290000u, kMaskSight, 0, shut) && !FaceCounts(0x00290000u, kMaskShot, 0, shut) &&
				  FaceCounts(0x00290000u, kMaskPlayer, 0, shut),
			"furniture (0x29) fences the player only: sight and shots pass");
		check(FaceCounts(0x00270000u, kMaskSight, 0, shut) && FaceCounts(0x00270000u, kMaskShot, 0, shut),
			"a moving face (0x27) stops sight and shots");
		check(FaceCounts(0x022F0001u, kMaskSight, 0, shut) && !FaceCounts(0x022F0001u, kMaskSight, 0, open) &&
				  !FaceCounts(0x002F0000u, kMaskSight, 0x00040000u, shut) && !FaceCounts(0x002F0000u, 0x0004FFFEu, 0, shut),
			"sight: a shut door wall stops it, an open one not; the ignore mask and the class bits apply");

		// Line of sight (night review m10): the slot 7 case, a head over a desk front (furniture, the face the player's
		// filter stopped the Handgun on) and a room wall farther on, both facing the head.
		{
			std::vector<Face> scene = wall_at(67.0f);
			for (Face& f : scene)
				f.flags = 0x00290000u;
			for (Face f : wall_at(200.0f))
			{
				f.flags = 0x002F0000u;
				scene.push_back(f);
			}
			const Vec3 eye_pt{0.0f, 155.0f, 0.0f};
			float tl = 0.0f;
			check(SegmentBlockedBy(scene, kMaskSight, 0, shut, eye_pt, {0.0f, 140.0f, 90.0f}, &tl) < 0,
				"line of sight: an item on the desk is seen past its front (furniture)");
			const int by_player = SegmentBlockedBy(scene, kMaskPlayer, 0, shut, eye_pt, {0.0f, 140.0f, 90.0f}, &tl);
			check(by_player >= 0 && scene[static_cast<size_t>(by_player)].flags == 0x00290000u,
				"line of sight: the player's filter would have stopped it on the desk front (the reverted try)");
			const int by_wall = SegmentBlockedBy(scene, kMaskSight, 0, shut, eye_pt, {0.0f, 140.0f, 230.0f}, &tl);
			check(by_wall >= 0 && scene[static_cast<size_t>(by_wall)].flags == 0x002F0000u && near(tl, 200.0f / 230.0f, 1.0e-3f),
				"line of sight: an item behind the room wall is not seen (the wall, 200 of 230 cm along)");
			check(SegmentBlockedBy(scene, kMaskShot, 0, shut, {0.0f, 130.0f, 150.0f}, {0.0f, 130.0f, 215.0f}, &tl) >= 0 &&
					  SegmentBlockedBy(scene, kMaskShot, 0, shut, {0.0f, 130.0f, 150.0f}, {0.0f, 130.0f, 190.0f}, &tl) < 0,
				"shot mask: a muzzle 15 cm past the wall is beyond it, 10 cm short of it not");
			check(SegmentBlockedBy(scene, kMaskSight, 0, shut, {0.0f, 140.0f, 230.0f}, eye_pt, &tl) < 0 &&
					  SegmentBlockedBy(scene, kMaskSight, 0, shut, eye_pt, {std::nanf(""), 140.0f, 230.0f}, &tl) < 0 &&
					  SegmentBlockedBy(scene, kMaskSight, 0, shut, eye_pt, {450.0f, 140.0f, 230.0f}, &tl) < 0,
				"line of sight: one-sided (from behind no hit), NaN, and round the wall's end not blocked");
		}

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
		{
			// A malformed 32 x 32 grid whose every list runs 4000 entries without an end: refused after the total cap,
			// not after 32 x 32 x 4000 reads (night review m9).
			u64 reads = 0;
			const Reader bad = [&reads, faces](u32 a, u32 n, void* o) {
				reads++;
				if (a == 0x1000 && n == 0x20)
				{
					const u32 h[8] = {300, 300, 32, 32, 0, 0, 0x2000, 0};
					std::memcpy(o, h, sizeof(h));
					return true;
				}
				const u32 v = (a >= 0x2000 && a < 0x3000) ? 0x10000u : faces; // a cell's list, or a face (never the end)
				std::memcpy(o, &v, 4);
				return n == 4;
			};
			Grid gb;
			check(!ReadGrid(bad, 0x1000, &gb) && reads <= kMaxGridEntries + 64, "grid: a list without an end stops at the total cap");
		}
		Console.WriteLn("(VR) HeadInWall self-test: %d failure(s).", fail);
		{
			// ChestFrom (evening review m5): the record at 0x200000, its model at 0x210000, bone 11's matrix at 0x220000.
			std::map<u32, u32> ram;
			const auto putw = [&ram](u32 a, u32 v) { ram[a] = v; };
			const auto putf = [&ram](u32 a, float f) {
				u32 v;
				std::memcpy(&v, &f, 4);
				ram[a] = v;
			};
			const Reader rd = [&ram](u32 a, u32 n, void* o) {
				for (u32 i = 0; i < n; i += 4)
				{
					const auto it = ram.find(a + i);
					if (it == ram.end())
						return false;
					std::memcpy(static_cast<u8*>(o) + i, &it->second, 4);
				}
				return true;
			};
			putw(0x200034, 0x210000);
			putf(0x200038, 100.0f);
			putf(0x20003C, 0.0f);
			putf(0x200040, 200.0f);
			putw(0x210814, 36u << 16);
			putw(0x210820 + 11 * 4, 0x220000);
			putf(0x220030, 101.0f);
			putf(0x220034, 128.0f);
			putf(0x220038, 205.0f);
			float c[3] = {};
			check(ChestFrom(rd, 0x200000, c) && near(c[1], 128.0f) && near(c[2], 205.0f), "chest: bone 11 read");
			putw(0x210814, 11u << 16);
			check(!ChestFrom(rd, 0x200000, c), "chest: bone 11 past the model's bone count refused");
			putw(0x210814, 36u << 16);
			putw(0x200034, 0x40210000); // a garbage pointer: the read through the reader fails, nothing raw
			check(!ChestFrom(rd, 0x200000, c), "chest: a model pointer that does not read refused");
			putw(0x200034, 0x210002);
			check(!ChestFrom(rd, 0x200000, c), "chest: an unaligned model pointer refused");
			putw(0x200034, 0x210000);
			putw(0x210820 + 11 * 4, 0x01FFFFF0);
			check(!ChestFrom(rd, 0x200000, c), "chest: a bone pointer in the last bytes of RAM refused");
			putw(0x210820 + 11 * 4, 0x220000);
			putf(0x220034, 900.0f);
			check(!ChestFrom(rd, 0x200000, c), "chest: a point over 3 m from the record refused");
			check(!ChestFrom(rd, 0, c), "chest: no record");
		}
		return fail;
	}
} // namespace VR::HeadInWall
