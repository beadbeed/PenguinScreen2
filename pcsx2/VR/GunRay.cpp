// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/GunRay.h"
#include "VR/CameraDriver.h"
#include "VR/GameFeedback.h"
#include "VR/HandModel.h"
#include "VR/ItemData.h"
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
#include <cmath>
#include <cstring>

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

	std::vector<std::string> SelfTest()
	{
		std::vector<std::string> fail;
		const auto check = [&](bool ok, const char* name) {
			if (!ok)
				fail.emplace_back(name);
		};
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
		return fail;
	}

	// -----------------------------------------------------------------------------------------------------------------
	// The glue.
	// -----------------------------------------------------------------------------------------------------------------
	namespace
	{
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

	void OnStateLoaded()
	{
		// A block saved armed is ours to clear (or rewrite) at the next poll; the count starts again.
		s_armed = s_addr != 0;
		s_stale = true;
		s_watch_s = 0.0f;
	}

	void Poll(const VRInputSnapshot& snap, GuestReader reader, float dt)
	{
		dt = std::isfinite(dt) ? std::clamp(dt, 0.0f, 0.25f) : 0.0f;
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
		const bool offline = !EmuConfig.DEV9.EthEnable && !EmuConfig.VR.OnlineSafe;
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
		if (mp && fb && setting && offline && slot_known && record_steady)
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
					gun = ItemData::IsGun(ItemData::KindOf(item_id));
				}
			}
		}

		bool wrote = false;
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
			if (ray && CameraDriver::GetMapping(&map) && RayToGame(from, dir, MapPoint, &map, og, dg) &&
				ShotFrame(og, dg, back, m, origin) && Read(reader, mp->counter, 1, &counter))
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

				// A shot cave I sent along the ray: logged, and the enemies' HP watched for 0.3 s.
				u32 shots = 0;
				if (Read(reader, b + kOffShots, 4, &shots) && shots != s_last_shots)
				{
					s_last_shots = shots;
					std::memcpy(s_shot_origin, origin, sizeof(origin));
					std::memcpy(s_shot_dir, dg, sizeof(dg));
					Console.WriteLn("(VR) Gun ray: shot %u along the VR ray from (%.0f, %.0f, %.0f) toward (%.3f, %.3f, %.3f) "
									"(back %.0f).", shots, static_cast<double>(origin[0]), static_cast<double>(origin[1]),
						static_cast<double>(origin[2]), static_cast<double>(dg[0]), static_cast<double>(dg[1]),
						static_cast<double>(dg[2]), static_cast<double>(back));
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
	}
}
