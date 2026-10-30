// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/VrGrab.h"
#include "VR/GrabBreakout.h"
#include "VR/CameraDriver.h"
#include "VR/VRInput.h"
#include "VR/GameFeedback.h"
#include "VR/ItemData.h"
#include "VR/VRProfileDB.h"

#include "Config.h"
#include "Memory.h"
#include "VMManager.h"
#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <mutex>

namespace VR::VrGrab
{
	namespace
	{
		constexpr float kPi = 3.14159265358979f;
		// A grip position unchanged this long has stopped (scripted input is re-sent at 20-90 Hz).
		constexpr float kStillS = 0.1f;

		// File #1 (SLPM-65428) layout, research/2026-10-27-vr-grab.md.
		constexpr u32 kEnemyTable = 0x3FFCA0, kEnemyStride = 0xD90, kEnemyCount = 64;
		constexpr u32 kZombieUpdate = 0x664290;          // enemy +0x14: the zombie's update
		constexpr u32 kEState = 0x08, kESub = 0x09, kETarget = 0x24, kEPos = 0x38, kEUpdate = 0x14;
		constexpr u32 kESphereShapes = 0x1D8, kESphereCentres = 0x1DC, kESphereCount = 0x1E0;
		constexpr u8 kLungeState = 2, kLungeSub = 6;     // the grab lunge (motion 413)
		constexpr u32 kRStance = 0x08, kRSub = 0x09, kRPhase = 0x0A, kRPos = 0x38, kRHp = 0x544;
		constexpr u32 kREquipped = 0xC7C;                // the equipped item's record index (feedback.weapon.equipped)
		constexpr u32 kRGrabber = 0xB24;                 // the zombie holding this record
		constexpr u32 kRMeter = 0xCDD;                   // the escape meter (s8: bit 7 the counter flag, low 7 the count)
		constexpr u8 kGrabbed = 5;

		float Finite(float v, float d = 0.0f) { return std::isfinite(v) ? v : d; }
		float Dot(const std::array<float, 3>& a, const std::array<float, 3>& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
		std::array<float, 3> Sub(const std::array<float, 3>& a, const std::array<float, 3>& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
		float Len(const std::array<float, 3>& a) { return std::sqrt(Dot(a, a)); }

		// Rotates v by the unit quaternion q (x, y, z, w).
		std::array<float, 3> Rotate(const std::array<float, 4>& q, const std::array<float, 3>& v)
		{
			const float x = q[0], y = q[1], z = q[2], w = q[3];
			const std::array<float, 3> u = {x, y, z};
			const std::array<float, 3> t = {2.0f * (y * v[2] - z * v[1]), 2.0f * (z * v[0] - x * v[2]), 2.0f * (x * v[1] - y * v[0])};
			return {v[0] + w * t[0] + (u[1] * t[2] - u[2] * t[1]), v[1] + w * t[1] + (u[2] * t[0] - u[0] * t[2]),
				v[2] + w * t[2] + (u[0] * t[1] - u[1] * t[0])};
		}

		// The hand's velocity: the runtime's when it reports one, else the grip position's change since it last changed.
		void Track(const VRHandState& hs, bool live, float dt, HandTrack& h)
		{
			h.cooldown_s = std::max(0.0f, h.cooldown_s - dt);
			if (!live || !hs.grip_pose.valid)
			{
				const float cd = h.cooldown_s;
				h = HandTrack{};
				h.cooldown_s = cd;
				return;
			}
			const std::array<float, 3> pos = {Finite(hs.grip_pose.position_xyz[0]), Finite(hs.grip_pose.position_xyz[1]),
				Finite(hs.grip_pose.position_xyz[2])};
			const std::array<float, 3> rv = {Finite(hs.grip_linear_velocity[0]), Finite(hs.grip_linear_velocity[1]),
				Finite(hs.grip_linear_velocity[2])};
			const bool moved = !h.have_pos || pos != h.pos;
			h.since_move_s += dt;
			if (Dot(rv, rv) > 1.0e-8f)
				h.vel = rv;
			else if (h.have_pos && moved && h.since_move_s > 1.0e-4f)
			{
				const float inv = 1.0f / h.since_move_s;
				h.vel = {(pos[0] - h.pos[0]) * inv, (pos[1] - h.pos[1]) * inv, (pos[2] - h.pos[2]) * inv};
			}
			else if (!h.have_pos || h.since_move_s >= kStillS)
				h.vel = {0.0f, 0.0f, 0.0f};
			if (moved)
			{
				h.pos = pos;
				h.have_pos = true;
				h.since_move_s = 0.0f;
			}
		}

		// The ray from o along unit d against a sphere (c, r): the distance along the ray to the closest approach and how
		// far the ray passes from the centre; false when the sphere is behind.
		bool RayPass(const std::array<float, 3>& o, const std::array<float, 3>& d, const std::array<float, 3>& c, float* along,
			float* miss)
		{
			const std::array<float, 3> oc = Sub(c, o);
			const float t = Dot(oc, d);
			if (t <= 0.0f)
				return false;
			const std::array<float, 3> p = {o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t};
			*along = t;
			*miss = Len(Sub(c, p));
			return true;
		}

		// Shared between the CPU thread (Vsync publishes the target, takes the points) and the input thread.
		std::mutex s_target_mutex;
		Target s_target;
		u64 s_target_ms = 0;
		std::atomic<int> s_pending_points{0};
		std::atomic<bool> s_pending_decisive{false};
		std::atomic<u8> s_pending_kind{0};
		// The game's own shots (ShotWatch, CPU thread, every vsync) for the input thread's candidate shots.
		std::atomic<bool> s_shot_gun{false};
		std::atomic<u32> s_shot_serial{0};

		// CPU thread only.
		ShotWatch s_shot_watch;
		GameFeedback::ItemTable s_item_table;
		u32 s_item_table_crc = 0;
		std::bitset<kEnemyCount> s_lunging;
		bool s_was_grabbed = false;
		u64 s_grab_start_ms = 0;
		u8 s_meter_at_start = 0;
		u8 s_last_phase = 0;   // the game's way out of this grab (10, 20, 30), 0 before it
		u8 s_meter_at_way = 0; // the meter's count then
		int s_actions = 0;
		int s_meter_writes = 0;
		bool s_logged_on = false;
		bool s_located_logged = false;

		u64 NowMs()
		{
			return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		float ReadF(u32 a) { return std::bit_cast<float>(static_cast<u32>(memRead32(a))); }
		bool InRam(u32 a, u32 size) { return a >= 0x100000u && a + size <= Ps2MemSize::MainRam; }
	}

	const char* KindName(Kind k)
	{
		switch (k)
		{
			case Kind::Shove: return "shove";
			case Kind::HardShove: return "hard shove";
			case Kind::Shot: return "shot";
			case Kind::HeadShot: return "headshot";
			default: return "none";
		}
	}

	bool Decisive(Kind k) { return k == Kind::HardShove || k == Kind::HeadShot; }

	bool ShotWatch::Step(u32 eq_now, bool is_gun, u16 ammo_now)
	{
		const bool same = baseline && eq == eq_now && is_gun;
		const bool fired = same && ammo_now < ammo && static_cast<u32>(ammo - ammo_now) <= kMaxShotStep;
		if (fired)
			serial++;
		baseline = eq_now != 0;
		eq = eq_now;
		ammo = ammo_now;
		gun = is_gun && eq_now != 0;
		return fired;
	}

	Event Detect(const VRInputSnapshot& snapshot, const Target& target, const ShotEvidence& shot, const Params& p, bool grabbed,
		float dt, State& st)
	{
		Event ev;
		dt = std::clamp(Finite(dt), 0.0f, 0.25f);
		const bool live = snapshot.generation != 0 && snapshot.actions_active;
		const float cos_toward = std::cos(std::clamp(p.toward_deg, 0.0f, 90.0f) * kPi / 180.0f);
		for (int i = 0; i < 2; ++i)
		{
			const VRHandState& hs = snapshot.hands[i];
			HandTrack& h = st.hand[i];
			Track(hs, live, dt, h);
			const float trig = live ? Finite(hs.trigger) : 0.0f;
			const bool trig_edge = h.trigger_armed && trig >= p.trigger_on;
			if (trig < p.trigger_off)
				h.trigger_armed = true;
			else if (trig >= p.trigger_on)
				h.trigger_armed = false;
			// The game's own shot since the last poll; a candidate waits shot_window_s for it.
			const bool fired = shot.serial != h.seen_serial;
			h.seen_serial = shot.serial;
			if (h.cand)
			{
				h.cand_age_s += dt;
				if (h.cand_age_s > p.shot_window_s || !grabbed)
					h.cand = false;
			}
			if (!grabbed || !target.valid || target.count <= 0 || !h.have_pos || h.cooldown_s > 0.0f || ev.kind != Kind::None)
				continue;

			// A shove: into a sphere, toward its centre, fast.
			const float speed = Len(h.vel);
			if (speed >= p.shove_speed)
			{
				for (int s = 0; s < std::min(target.count, kMaxSpheres); ++s)
				{
					const std::array<float, 3> to = Sub(target.c[s], h.pos);
					const float d = Len(to);
					const float surface = d - target.r[s];
					if (!(surface <= p.reach_m) || d < 1.0e-4f)
						continue;
					if (Dot(h.vel, to) < cos_toward * speed * d)
						continue;
					ev.kind = speed >= p.hard_speed ? Kind::HardShove : Kind::Shove;
					ev.hand = i;
					ev.sphere = s;
					ev.speed = speed;
					ev.dist = std::max(surface, 0.0f);
					break;
				}
			}
			// A shot: the trigger with the aim ray through a sphere at point-blank range (the head first) is a candidate,
			// only with a gun equipped and this hand the one that fires it; it counts when the game fires within
			// shot_window_s (a round gone from the gun). The head sphere on the ray at the press makes it a headshot.
			if (ev.kind == Kind::None && trig_edge && hs.aim_pose.valid && shot.gun && shot.hand == i)
			{
				const std::array<float, 3> o = {Finite(hs.aim_pose.position_xyz[0]), Finite(hs.aim_pose.position_xyz[1]),
					Finite(hs.aim_pose.position_xyz[2])};
				const std::array<float, 3> d = Rotate(hs.aim_pose.orientation_xyzw, {0.0f, 0.0f, -1.0f});
				const float dl = Len(d);
				bool found = false;
				if (dl > 1.0e-4f)
				{
					const std::array<float, 3> du = {d[0] / dl, d[1] / dl, d[2] / dl};
					for (int pass = 0; pass < 2 && !found; ++pass)
					{
						for (int s = 0; s < std::min(target.count, kMaxSpheres); ++s)
						{
							if (target.head[s] != (pass == 0))
								continue;
							float along = 0.0f, miss = 0.0f;
							if (!RayPass(o, du, target.c[s], &along, &miss) || along > p.shot_range_m ||
								miss > target.r[s] + p.shot_margin_m)
								continue;
							h.cand = true;
							h.cand_age_s = 0.0f;
							h.cand_sphere = s;
							h.cand_head = target.head[s];
							h.cand_dist = along;
							found = true;
							break;
						}
					}
				}
			}
			if (ev.kind == Kind::None && h.cand && fired && shot.gun && shot.hand == i && h.cand_age_s <= p.shot_window_s)
			{
				ev.kind = h.cand_head ? Kind::HeadShot : Kind::Shot;
				ev.hand = i;
				ev.sphere = h.cand_sphere;
				ev.dist = h.cand_dist;
				h.cand = false;
			}
			if (ev.kind != Kind::None)
				h.cooldown_s = std::max(p.cooldown_s, 0.0f);
		}
		return ev;
	}

	u8 MeterAfter(u8 meter, int points, bool decisive)
	{
		const u8 flag = meter & 0x80u;
		const int count = meter & 0x7F;
		const int after = decisive ? 0 : std::clamp(count - std::max(points, 0), 0, count);
		return static_cast<u8>(flag | static_cast<u8>(after));
	}

	bool MeterGate(u8 stance, u8 sub, u8 phase, s16 hp)
	{
		return stance == kGrabbed && (sub == 1 || sub == 2) && phase >= 4 && phase <= 6 && hp >= 2;
	}

	bool LungeEdge(const EnemyView& e, const Params& p, bool& lunging)
	{
		const bool now = e.present && e.zombie && e.state == kLungeState && e.sub == kLungeSub && e.targets_local &&
		                 std::isfinite(e.dist) && e.dist <= p.cue_within;
		const bool edge = now && !lunging;
		lunging = now;
		return edge;
	}

	void Vsync(bool enabled, bool offline, u32 base, bool grabbed)
	{
		const Params p{};
		if (!enabled || base == 0 || !InRam(base, 0x10E0))
		{
			s_lunging.reset();
			s_was_grabbed = false;
			s_shot_watch = ShotWatch{};
			s_shot_gun.store(false);
			s_pending_points.store(0);
			s_pending_decisive.store(false);
			std::lock_guard<std::mutex> lock(s_target_mutex);
			s_target = Target{};
			return;
		}
		if (!s_logged_on)
		{
			s_logged_on = true;
			Console.WriteLn("(VR) VrGrab: on: a buzz when a zombie lunges to grab you; while grabbed, shove its head or "
			                "chest, strike its head or shoot it point-blank to break free (presses on your own pad; %s).",
				offline ? "offline, the game's own escape meter too" : "online, the presses only");
		}

		// The game's own shots: the equipped gun's round count in the local record (reads only). A melee weapon, a throwable
		// or empty hands are not guns and never shoot.
		{
			const u32 crc = VMManager::GetDiscCRC();
			if (s_item_table_crc != crc)
			{
				s_item_table_crc = crc;
				const std::string serial = VMManager::GetDiscSerial();
				const ProfileDB::Profile* prof = serial.empty() ? nullptr : ProfileDB::Lookup(serial, crc);
				s_item_table = prof ? GameFeedback::ItemTableOf(*prof) : GameFeedback::ItemTable{};
				s_item_table.slot_from_camera = true;
			}
			const u32 eq = memRead8(base + kREquipped);
			GameFeedback::ItemRecord rec;
			if (eq != 0)
				rec = GameFeedback::ReadItemRecord(s_item_table, eq, nullptr);
			const bool gun = rec.valid && ItemData::IsGun(ItemData::KindOf(rec.item_id));
			if (s_shot_watch.Step(rec.valid ? eq : 0u, gun, rec.count))
				Console.WriteLn("(VR) VrGrab: the game fired (item %u, rounds now %u).", static_cast<unsigned>(rec.item_id),
					static_cast<unsigned>(rec.count));
			s_shot_gun.store(s_shot_watch.gun);
			s_shot_serial.store(s_shot_watch.serial);
		}

		// The telegraph (reads only).
		const float kx = ReadF(base + kRPos), kz = ReadF(base + kRPos + 8);
		for (u32 i = 0; i < kEnemyCount; ++i)
		{
			const u32 e = kEnemyTable + i * kEnemyStride;
			EnemyView v;
			v.present = memRead8(e) != 0;
			if (v.present)
			{
				v.zombie = static_cast<u32>(memRead32(e + kEUpdate)) == kZombieUpdate;
				v.state = memRead8(e + kEState);
				v.sub = memRead8(e + kESub);
				v.targets_local = static_cast<u32>(memRead32(e + kETarget)) == base;
				v.dist = std::hypot(ReadF(e + kEPos) - kx, ReadF(e + kEPos + 8) - kz);
			}
			bool lunging = s_lunging[i];
			if (LungeEdge(v, p, lunging))
			{
				const float a = std::clamp(p.cue_amp * std::clamp(EmuConfig.VR.HapticStrength, 0.0f, 1.0f), 0.0f, 1.0f);
				if (a > 0.0f)
				{
					XRInput::QueuePulse(VRInputSnapshot::LEFT, a, p.cue_s);
					XRInput::QueuePulse(VRInputSnapshot::RIGHT, a, p.cue_s);
				}
				Console.WriteLn("(VR) VrGrab: enemy %u lunges to grab you (%.0f units away): the cue on both hands.", i,
					static_cast<double>(v.dist));
			}
			s_lunging[i] = lunging;
		}

		// The grab: the grabbing zombie's spheres into the XR space for the input thread.
		const u8 stance = memRead8(base + kRStance), sub = memRead8(base + kRSub), phase = memRead8(base + kRPhase);
		const s16 hp = static_cast<s16>(static_cast<u16>(memRead16(base + kRHp)));
		const bool held = grabbed && stance == kGrabbed;
		Target t;
		const u32 z = held ? static_cast<u32>(memRead32(base + kRGrabber)) : 0u;
		if (z >= kEnemyTable && z < kEnemyTable + kEnemyCount * kEnemyStride && (z - kEnemyTable) % kEnemyStride == 0 &&
			memRead8(z) != 0)
		{
			CameraDriver::Mapping m;
			const u32 n = std::min<u32>(memRead8(z + kESphereCount), kMaxSpheres);
			const u32 shapes = static_cast<u32>(memRead32(z + kESphereShapes));
			const u32 centres = static_cast<u32>(memRead32(z + kESphereCentres));
			if (n > 0 && InRam(shapes, 20 * n) && InRam(centres, 12 * n) && CameraDriver::GetMapping(&m) && m.valid &&
				m.units_per_meter > 0.0f)
			{
				int top = -1;
				float top_y = -1.0e30f;
				for (u32 k = 0; k < n; ++k)
				{
					const float r = ReadF(shapes + 20 * k + 0x0C);
					const float g[3] = {ReadF(centres + 12 * k), ReadF(centres + 12 * k + 4), ReadF(centres + 12 * k + 8)};
					float vr[3];
					if (!(r > 0.0f) || !CameraDriver::VrFromGame(m, g, vr))
						break;
					const int s = t.count++;
					t.c[s] = {vr[0], vr[1], vr[2]};
					t.r[s] = r / m.units_per_meter;
					if (g[1] > top_y)
					{
						top_y = g[1];
						top = s;
					}
				}
				// The highest sphere is the head; with three or more, the lowest (the hips) is no shove target.
				if (t.count > 0)
				{
					t.head[top] = true;
					if (t.count >= 3)
					{
						int low = 0;
						for (int s = 1; s < t.count; ++s)
							low = (t.c[s][1] < t.c[low][1]) ? s : low;
						if (low != top)
						{
							for (int s = low; s + 1 < t.count; ++s)
							{
								t.c[s] = t.c[s + 1];
								t.r[s] = t.r[s + 1];
								t.head[s] = t.head[s + 1];
							}
							t.count--;
						}
					}
					t.valid = true;
				}
			}
		}
		{
			std::lock_guard<std::mutex> lock(s_target_mutex);
			s_target = t;
			s_target_ms = NowMs();
		}
		if (held && t.valid && !s_located_logged)
		{
			s_located_logged = true;
			for (int s = 0; s < t.count; ++s)
				Console.WriteLn("(VR) VrGrab: the grabbing zombie's %s at VR (%.3f, %.3f, %.3f) m, r %.2f m.", t.head[s] ? "head" : "body",
					static_cast<double>(t.c[s][0]), static_cast<double>(t.c[s][1]), static_cast<double>(t.c[s][2]),
					static_cast<double>(t.r[s]));
		}
		if (!held)
			s_located_logged = false;

		if (held && !s_was_grabbed)
		{
			s_was_grabbed = true;
			s_grab_start_ms = NowMs();
			s_meter_at_start = memRead8(base + kRMeter);
			s_last_phase = 0;
			s_actions = 0;
			s_meter_writes = 0;
			s_pending_points.store(0);
			s_pending_decisive.store(false);
			Console.WriteLn("(VR) VrGrab: grabbed (sub-state %u, the zombie %s); shove it, strike its head or shoot it "
			                "point-blank to break free.", sub, t.valid ? "located" : "not located");
		}
		else if (!held && s_was_grabbed)
		{
			s_was_grabbed = false;
			const char* how = s_last_phase == 10 ? (s_meter_at_way == 0 ? "the game's own escape, the shove-off (phase 10)" :
			                                                               "the zombie let go after its bites (code 99, phase 10)") :
			                  s_last_phase == 20 ? "the game's knife counter (phase 20)" :
			                  s_last_phase == 30 ? "down or dead" : "released by the game (a hit on the zombie, or the end)";
			Console.WriteLn("(VR) VrGrab: free after %.2f s (%s; %d VR action(s), %d meter write(s)).",
				static_cast<double>(NowMs() - s_grab_start_ms) / 1000.0, how, s_actions, s_meter_writes);
		}
		if (held)
		{
			// The game's own way out, as its grab handler takes it: phase 10/11 the shove-off (the meter empty: the
			// escape; otherwise the zombie let go after its bites, code 99), 20/21 the knife counter, 30 and up down.
			const u8 way = (phase == 10 || phase == 11) ? 10 : (phase == 20 || phase == 21) ? 20 : (phase >= 30 && phase != 255) ? 30 : 0;
			if (way != 0 && s_last_phase == 0)
			{
				s_last_phase = way;
				s_meter_at_way = memRead8(base + kRMeter) & 0x7Fu;
				Console.WriteLn("(VR) VrGrab: the game takes its way out, phase %u, %.2f s into the grab (escape meter %u).", phase,
					static_cast<double>(NowMs() - s_grab_start_ms) / 1000.0, s_meter_at_way);
			}
		}

		// The escape actions the input thread counted: offline only, onto the game's own meter in our own record.
		const int points = s_pending_points.exchange(0);
		const bool decisive = s_pending_decisive.exchange(false);
		const Kind kind = static_cast<Kind>(s_pending_kind.exchange(0));
		if (points > 0 || decisive)
		{
			s_actions++;
			if (offline && held && MeterGate(stance, sub, phase, hp))
			{
				const u8 before = memRead8(base + kRMeter);
				const u8 after = MeterAfter(before, points, decisive);
				if (after != before)
				{
					memWrite8(base + kRMeter, after);
					s_meter_writes++;
				}
				Console.WriteLn("(VR) VrGrab: %s: the escape meter %u -> %u%s.", KindName(kind), before & 0x7Fu, after & 0x7Fu,
					(after & 0x7Fu) == 0 ? " (empty: the game's own escape follows)" : "");
			}
			else
			{
				Console.WriteLn("(VR) VrGrab: %s: presses only (%s).", KindName(kind),
					!offline ? "online" : !held ? "not held" : "the game is not counting the meter now");
			}
		}
	}

	Event InputPoll(const VRInputSnapshot& snapshot, bool grabbed, float dt, State& st, float* burst_s)
	{
		const Params p{};
		Target t;
		{
			std::lock_guard<std::mutex> lock(s_target_mutex);
			const u64 now = NowMs();
			if (s_target_ms != 0 && (now <= s_target_ms || now - s_target_ms <= 250))
				t = s_target;
		}
		ShotEvidence shot;
		shot.gun = s_shot_gun.load();
		shot.hand = VRInputSnapshot::RIGHT; // the game's Circle is the right trigger
		shot.serial = s_shot_serial.load();
		const Event ev = Detect(snapshot, t, shot, p, grabbed, dt, st);
		if (ev.kind == Kind::None)
			return ev;
		if (burst_s)
			*burst_s = std::max(*burst_s, p.burst_s);
		const int pts = (ev.kind == Kind::Shot) ? p.shot_points : p.shove_points;
		s_pending_points.fetch_add(Decisive(ev.kind) ? 0 : pts);
		if (Decisive(ev.kind))
			s_pending_decisive.store(true);
		s_pending_kind.store(static_cast<u8>(ev.kind));
		Console.WriteLn("(VR) VrGrab: %s with the %s hand (sphere %d%s, %.2f m/s, %.2f m).", KindName(ev.kind),
			ev.hand == VRInputSnapshot::LEFT ? "left" : "right", ev.sphere, t.head[std::max(ev.sphere, 0)] ? ", the head" : "",
			static_cast<double>(ev.speed), static_cast<double>(ev.dist));
		return ev;
	}

	bool SelfTest()
	{
		bool ok = true;
		const auto check = [&ok](bool c, const char* what) {
			if (!c)
			{
				Console.Error("(VR) VrGrab self-test: %s", what);
				ok = false;
			}
		};
		const Params p{};

		// The meter: bit 7 kept, never below 0, never raised, decisive empties.
		check(MeterAfter(30, 10, false) == 20, "30 less 10 is 20");
		check(MeterAfter(5, 10, false) == 0, "never below 0");
		check(MeterAfter(0x80 | 30, 10, false) == (0x80 | 20), "bit 7 (the knife counter) is kept");
		check(MeterAfter(30, 0, true) == 0 && MeterAfter(0x80 | 7, 0, true) == 0x80, "decisive empties the count only");
		check(MeterAfter(30, -5, false) == 30, "negative points never raise it");
		check(MeterGate(5, 1, 6, 100) && MeterGate(5, 2, 4, 2) && !MeterGate(5, 0, 6, 100) && !MeterGate(5, 1, 3, 100) &&
				  !MeterGate(5, 1, 10, 100) && !MeterGate(5, 1, 6, 1) && !MeterGate(1, 1, 6, 100),
			"the gate: stance 5, sub-state 1 or 2, phases 4-6, HP 2 and up");

		// The telegraph: one cue per lunge at us, within range.
		{
			bool lunging = false;
			EnemyView e;
			e.present = e.zombie = e.targets_local = true;
			e.state = 1;
			e.sub = 31;
			e.dist = 40.0f;
			check(!LungeEdge(e, p, lunging), "no cue while it stalks");
			e.state = 2;
			e.sub = 9;
			check(!LungeEdge(e, p, lunging), "no cue for its swipe (sub-state 9)");
			e.sub = 6;
			check(LungeEdge(e, p, lunging), "a cue when the grab lunge starts");
			check(!LungeEdge(e, p, lunging), "once per lunge");
			e.state = 3;
			check(!LungeEdge(e, p, lunging) && !lunging, "the lunge ends (hit: hurt state)");
			e.state = 2;
			e.targets_local = false;
			check(!LungeEdge(e, p, lunging), "no cue for a lunge at a partner");
			e.targets_local = true;
			e.dist = 900.0f;
			check(!LungeEdge(e, p, lunging), "no cue from across the room");
			e.dist = 40.0f;
			e.zombie = false;
			check(!LungeEdge(e, p, lunging), "zombies only");
		}

		// The escape detector on scripted hands. The zombie's head 40 cm ahead of the head, its chest below.
		Target t;
		t.valid = true;
		t.count = 2;
		t.c[0] = {0.0f, 1.2f, -0.40f};
		t.r[0] = 0.12f;
		t.head[0] = true;
		t.c[1] = {0.0f, 0.85f, -0.40f};
		t.r[1] = 0.18f;
		const auto snap_at = [](const std::array<float, 3>& pos, const std::array<float, 3>& vel, float trigger,
								 const std::array<float, 4>& q) {
			VRInputSnapshot s;
			s.generation = 1;
			s.actions_active = true;
			VRHandState& h = s.hands[VRInputSnapshot::RIGHT];
			h.grip_pose.valid = h.aim_pose.valid = true;
			h.grip_pose.position_xyz = pos;
			h.aim_pose.position_xyz = pos;
			h.aim_pose.orientation_xyzw = q;
			h.grip_linear_velocity = vel;
			h.trigger = trigger;
			return s;
		};
		const std::array<float, 4> fwd = {0.0f, 0.0f, 0.0f, 1.0f}; // aim along -Z
		const std::array<float, 4> back = {0.0f, 1.0f, 0.0f, 0.0f}; // aim along +Z
		const float dt = 1.0f / 90.0f;
		ShotEvidence sh; // a gun in the right hand, no shot yet
		sh.gun = true;
		sh.hand = VRInputSnapshot::RIGHT;
		{
			State st;
			Event e = Detect(snap_at({0.0f, 1.15f, -0.15f}, {0.0f, 0.0f, -1.8f}, 0.0f, fwd), t, sh, p, true, dt, st);
			check(e.kind == Kind::Shove && e.sphere == 0 && e.hand == VRInputSnapshot::RIGHT, "a thrust into the head is a shove");
			e = Detect(snap_at({0.0f, 1.15f, -0.15f}, {0.0f, 0.0f, -1.8f}, 0.0f, fwd), t, sh, p, true, dt, st);
			check(e.kind == Kind::None, "one per hand per cooldown");
		}
		{
			State st;
			const Event e = Detect(snap_at({0.0f, 0.85f, -0.10f}, {0.0f, 0.0f, -3.0f}, 0.0f, fwd), t, sh, p, true, dt, st);
			check(e.kind == Kind::HardShove && e.sphere == 1, "a hard thrust into the chest is a hard shove");
		}
		{
			State st;
			const Event e = Detect(snap_at({0.0f, 1.15f, -0.15f}, {0.0f, 0.0f, 1.8f}, 0.0f, fwd), t, sh, p, true, dt, st);
			check(e.kind == Kind::None, "pulling the hand back is no shove");
		}
		{
			State st;
			const Event e = Detect(snap_at({0.0f, 1.15f, -0.15f}, {0.0f, 0.0f, -0.8f}, 0.0f, fwd), t, sh, p, true, dt, st);
			check(e.kind == Kind::None, "a slow push is no shove");
		}
		{
			State st;
			const Event e = Detect(snap_at({0.6f, 1.15f, 0.30f}, {0.0f, 0.0f, -1.8f}, 0.0f, fwd), t, sh, p, true, dt, st);
			check(e.kind == Kind::None, "a thrust out of reach is no shove");
		}
		{
			State st;
			const Event e = Detect(snap_at({0.0f, 1.15f, -0.15f}, {0.0f, 0.0f, -1.8f}, 0.0f, fwd), t, sh, p, false, dt, st);
			check(e.kind == Kind::None, "nothing counts without the grab");
			Target none;
			const Event e2 = Detect(snap_at({0.0f, 1.15f, -0.15f}, {0.0f, 0.0f, -1.8f}, 0.0f, fwd), none, sh, p, true, dt, st);
			check(e2.kind == Kind::None, "nothing counts without a located zombie");
		}
		{
			// The shot: the trigger's edge is a candidate; the game's own shot (the gun's round count) makes it a shot.
			State st;
			ShotEvidence fired = sh;
			fired.serial = 1;
			Event e = Detect(snap_at({0.0f, 1.2f, 0.20f}, {0.0f, 0.0f, 0.0f}, 0.0f, fwd), t, sh, p, true, dt, st);
			check(e.kind == Kind::None, "the trigger released: nothing");
			e = Detect(snap_at({0.0f, 1.2f, 0.20f}, {0.0f, 0.0f, 0.0f}, 1.0f, fwd), t, sh, p, true, dt, st);
			check(e.kind == Kind::None, "the trigger with no shot from the game: nothing (an empty clip, a lowered gun)");
			e = Detect(snap_at({0.0f, 1.2f, 0.20f}, {0.0f, 0.0f, 0.0f}, 1.0f, fwd), t, fired, p, true, dt, st);
			check(e.kind == Kind::HeadShot && e.sphere == 0 && e.hand == VRInputSnapshot::RIGHT,
				"the game's shot a poll after the press, the ray through the head: a headshot");
			for (int k = 0; k < 60; ++k)
				e = Detect(snap_at({0.0f, 1.2f, 0.20f}, {0.0f, 0.0f, 0.0f}, 1.0f, fwd), t, fired, p, true, dt, st);
			check(e.kind == Kind::None, "holding the trigger shoots once");
		}
		{
			// The shot in the same poll as the press counts; no shot at all, however long the press, never does.
			State st;
			ShotEvidence fired = sh;
			fired.serial = 1;
			Detect(snap_at({0.0f, 1.2f, 0.20f}, {}, 0.0f, fwd), t, sh, p, true, dt, st);
			const Event e = Detect(snap_at({0.0f, 1.2f, 0.20f}, {}, 1.0f, fwd), t, fired, p, true, dt, st);
			check(e.kind == Kind::HeadShot, "the shot in the press's own poll counts");
			State st2;
			Event e2;
			Detect(snap_at({0.0f, 1.2f, 0.20f}, {}, 0.0f, fwd), t, sh, p, true, dt, st2);
			for (int k = 0; k < 120; ++k)
				e2 = Detect(snap_at({0.0f, 1.2f, 0.20f}, {}, 1.0f, fwd), t, sh, p, true, dt, st2);
			check(e2.kind == Kind::None, "no shot evidence: no event at all");
		}
		{
			// A melee weapon or empty hands (no gun): never a shot, even with a counter change.
			State st;
			ShotEvidence melee = sh;
			melee.gun = false;
			melee.serial = 1;
			Detect(snap_at({0.0f, 1.2f, 0.20f}, {}, 0.0f, fwd), t, melee, p, true, dt, st);
			const Event e = Detect(snap_at({0.0f, 1.2f, 0.20f}, {}, 1.0f, fwd), t, melee, p, true, dt, st);
			check(e.kind == Kind::None, "a melee weapon or empty hands: no shot candidate");
			ShotEvidence left = sh;
			left.hand = VRInputSnapshot::LEFT;
			left.serial = 1;
			State st2;
			Detect(snap_at({0.0f, 1.2f, 0.20f}, {}, 0.0f, fwd), t, left, p, true, dt, st2);
			const Event e2 = Detect(snap_at({0.0f, 1.2f, 0.20f}, {}, 1.0f, fwd), t, left, p, true, dt, st2);
			check(e2.kind == Kind::None, "the game's shot is the other hand's: no event for this hand's trigger");
		}
		{
			// The shot lands inside the window (200 ms on) and outside it (300 ms on).
			ShotEvidence fired = sh;
			fired.serial = 1;
			for (int late = 0; late < 2; ++late)
			{
				State st;
				Detect(snap_at({0.0f, 1.2f, 0.20f}, {}, 0.0f, fwd), t, sh, p, true, dt, st);
				Event e = Detect(snap_at({0.0f, 1.2f, 0.20f}, {}, 1.0f, fwd), t, sh, p, true, dt, st);
				const int polls = late ? 27 : 18; // 300 ms / 200 ms at 90 Hz
				for (int k = 0; k < polls; ++k)
					e = Detect(snap_at({0.0f, 1.2f, 0.20f}, {}, 1.0f, fwd), t, sh, p, true, dt, st);
				e = Detect(snap_at({0.0f, 1.2f, 0.20f}, {}, 1.0f, fwd), t, fired, p, true, dt, st);
				check(late ? e.kind == Kind::None : e.kind == Kind::HeadShot,
					late ? "the shot 300 ms after the press: no event" : "the shot 200 ms after the press: a headshot");
			}
		}
		{
			State st;
			ShotEvidence fired = sh;
			fired.serial = 1;
			Detect(snap_at({0.0f, 0.85f, 0.20f}, {}, 0.0f, fwd), t, sh, p, true, dt, st);
			Detect(snap_at({0.0f, 0.85f, 0.20f}, {}, 1.0f, fwd), t, sh, p, true, dt, st);
			const Event e = Detect(snap_at({0.0f, 0.85f, 0.20f}, {}, 1.0f, fwd), t, fired, p, true, dt, st);
			check(e.kind == Kind::Shot && e.sphere == 1, "the shot through the chest is a shot (progress, not decisive)");
		}
		{
			// The game's shot with the ray off the zombie: no candidate, no event; ShotWatch's rules.
			State st;
			ShotEvidence fired = sh;
			fired.serial = 1;
			Detect(snap_at({0.0f, 1.2f, 0.20f}, {}, 0.0f, back), t, sh, p, true, dt, st);
			Detect(snap_at({0.0f, 1.2f, 0.20f}, {}, 1.0f, back), t, sh, p, true, dt, st);
			const Event e = Detect(snap_at({0.0f, 1.2f, 0.20f}, {}, 1.0f, back), t, fired, p, true, dt, st);
			check(e.kind == Kind::None, "a shot with the ray off the zombie: no event");
			ShotWatch w;
			check(!w.Step(3, true, 7) && !w.Step(3, true, 7) && w.Step(3, true, 6) && w.serial == 1 && w.gun,
				"watch: a round gone from the equipped gun is a shot");
			check(!w.Step(3, true, 7) && !w.Step(3, true, 7), "watch: a reload (the count rose) is no shot");
			check(!w.Step(4, true, 3) && !w.Step(4, true, 3) && w.Step(4, true, 2) && w.serial == 2,
				"watch: a swap to another record is a new baseline, then its next round is a shot");
			check(!w.Step(5, false, 9) && !w.Step(5, false, 8) && !w.gun, "watch: a melee weapon never fires");
			check(!w.Step(0, false, 0) && !w.gun && !w.Step(3, true, 5) && !w.Step(3, true, 5), "watch: nothing equipped, then a gun, baseline only");
			check(!w.Step(3, true, 5) && w.Step(3, true, 4) && !w.Step(3, true, 0) , "watch: a drop of more than kMaxShotStep rounds is no shot");
		}
		{
			State st;
			Detect(snap_at({0.0f, 1.2f, 0.20f}, {}, 0.0f, back), t, sh, p, true, dt, st);
			const Event e = Detect(snap_at({0.0f, 1.2f, 0.20f}, {}, 1.0f, back), t, sh, p, true, dt, st);
			check(e.kind == Kind::None, "aimed away: no shot");
		}
		{
			State st;
			Detect(snap_at({0.0f, 1.2f, 2.5f}, {}, 0.0f, fwd), t, sh, p, true, dt, st);
			const Event e = Detect(snap_at({0.0f, 1.2f, 2.5f}, {}, 1.0f, fwd), t, sh, p, true, dt, st);
			check(e.kind == Kind::None, "not point-blank: no shot");
		}
		{
			// Velocity from positions when the runtime gives none (scripted input).
			State st;
			Detect(snap_at({0.0f, 1.15f, -0.12f}, {}, 0.0f, fwd), t, sh, p, true, dt, st);
			const Event e = Detect(snap_at({0.0f, 1.15f, -0.12f - 2.0f * dt}, {}, 0.0f, fwd), t, sh, p, true, dt, st);
			check(e.kind == Kind::Shove, "a shove measured from positions");
		}
		check(Decisive(Kind::HardShove) && Decisive(Kind::HeadShot) && !Decisive(Kind::Shove) && !Decisive(Kind::Shot),
			"hard shoves and headshots are decisive");
		{
			// The breakout runs its presses for a counted action's burst without a shake, only while grabbed.
			VRInputSnapshot s;
			s.generation = 1;
			s.actions_active = true;
			s.hands[VRInputSnapshot::RIGHT].grip_pose.valid = true;
			GrabBreakout::State bs;
			const GrabBreakout::Params bp{};
			GrabBreakout::Output o = GrabBreakout::Step(s, bp, true, dt, bs);
			check(!o.active, "still hands, no burst: no presses");
			bs.burst_s = p.burst_s;
			o = GrabBreakout::Step(s, bp, true, dt, bs);
			check(o.active && o.circle, "a burst runs the presses (Circle first)");
			int active = 0;
			for (int k = 0; k < 90; ++k)
				active += GrabBreakout::Step(s, bp, true, dt, bs).active ? 1 : 0;
			check(active >= 40 && active <= 46 && bs.burst_s == 0.0f, "for burst_s, then stops");
			bs.burst_s = p.burst_s;
			check(!GrabBreakout::Step(s, bp, false, dt, bs).active && bs.burst_s == 0.0f, "not grabbed: the burst is dropped");
		}
		return ok;
	}
}
