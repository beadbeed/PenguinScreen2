// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/GameFeedback.h"
#include "VR/CameraDriver.h"
#include "VR/HandModel.h"
#include "VR/ItemData.h"
#include "VR/SpatialControls.h"
#include "VR/VRInput.h"
#include "VR/VRProfileDB.h"
#include "VR/XRSession.h"

#include "Config.h"
#include "Memory.h"
#include "VMManager.h"

#include "common/Console.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace VR::GameFeedback
{
	namespace
	{
		constexpr int kLeft = 0;
		constexpr int kRight = 1;

		// Hit: 0.4 + 0.6 * (drop / max HP), 120-250 ms, both hands. Always stronger than a heartbeat, so a
		// pending beat never stands in for a bite.
		constexpr float kHurtBase = 0.4f;
		constexpr float kHurtGain = 0.6f;
		constexpr float kHurtMinMs = 120.0f;
		constexpr float kHurtMaxMs = 250.0f;

		// Shot: a sharp kick in the aim hand per counter step; a bigger jump is a scenario reset.
		constexpr float kShotAmp = 1.0f;
		constexpr float kShotSeconds = 0.040f;
		constexpr u32 kShotMaxStep = 5;

		// Heartbeat (left hand): two short beats 150 ms apart, every 1.0 s, every 0.8 s below 10% HP.
		constexpr float kBeatLubAmp = 0.35f;
		constexpr float kBeatDubAmp = 0.25f;
		constexpr float kBeatSeconds = 0.025f;
		constexpr u64 kBeatGapMs = 150;
		constexpr u64 kBeatPeriodMs = 1000;
		constexpr u64 kBeatCriticalPeriodMs = 800;
		constexpr float kCriticalFraction = 0.10f;

		constexpr u64 kLogIntervalMs = 200;
		constexpr u64 kBadHpLogIntervalMs = 10000;

		const char* HandName(int hand)
		{
			return (hand == kLeft) ? "left" : "right";
		}

		struct LogLimit
		{
			u64 last_ms = 0;
			u32 skipped = 0;
			bool any = false;
		};

		// One line per cue kind per interval; the next line that gets out says how many did not.
		bool TakeLog(LogLimit& l, u64 now_ms, u64 interval_ms, u32* skipped)
		{
			if (l.any && now_ms - l.last_ms < interval_ms)
			{
				l.skipped++;
				return false;
			}
			*skipped = l.skipped;
			l.skipped = 0;
			l.last_ms = now_ms;
			l.any = true;
			return true;
		}

		// " (N more not logged)" when the limiter held lines back, else empty.
		struct MoreSuffix
		{
			char text[40] = {};
			explicit MoreSuffix(u32 skipped)
			{
				if (skipped > 0)
					std::snprintf(text, sizeof(text), " (%u more not logged)", skipped);
			}
		};

		LogLimit s_log_hurt;
		LogLimit s_log_shot;
		LogLimit s_log_beat;
		LogLimit s_log_bad_hp;
		LogLimit s_log_rumble;
		LogLimit s_log_reload;
		LogLimit s_log_dry;

		// What one vsync read. The self-test builds these by hand.
		struct Sample
		{
			bool has_hp = false;
			u32 hp = 0;
			u32 hp_max = 0;
			float danger_below = 0.0f; // fraction of max HP; 0 = no heartbeat
			bool has_counter = false;
			u32 counter = 0;
			u32 counter_mask = 0xFFFFu;
			int fire_hand = kRight;
			float strength = 1.0f;
			u64 now_ms = 0;
			bool log = true;
		};

		struct CueState
		{
			bool record_valid = false;
			u32 record = 0;
			bool hp_valid = false;
			u32 hp = 0;
			bool counter_valid = false;
			u32 counter = 0;
			bool beating = false;
			u64 next_beat_ms = 0;
			bool dub_pending = false;
			u64 dub_ms = 0;
		};

		CueState s_cues;

		// Weapon (feedback.weapon): the equipped item record's rounds. A drop of 1-2 with the weapon raised is a
		// shot, any rise a reload; a bigger drop (or a drop while lowered, or during the reload action) is not.
		constexpr u32 kWeaponMaxShotStep = 2;
		constexpr float kReloadTickAmp = 0.3f;
		constexpr float kReloadTickSeconds = 0.015f;
		constexpr u64 kReloadTickGapMs = 60;
		constexpr float kDryAmp = 0.2f;
		constexpr float kDrySeconds = 0.012f;
		constexpr float kTriggerPressed = 0.5f;

		// The shot kick by the equipped item's kind (ItemData): a handgun, another gun or an id the table lacks as above
		// (kShotAmp, kShotSeconds); a long gun harder and twice as long, and in the other hand too while it holds the
		// fore-end (HandModel::OffHandSupports); a throw or a spray only a light cue; a melee weapon none (a swing drops
		// no rounds, and a count it keeps is not ammo).
		constexpr float kLongGunShotAmp = 1.4f; // past 1, so it stays stronger than a handgun's at a lower Haptic Strength
		constexpr float kLongGunShotSeconds = 0.080f;
		constexpr float kLongGunSupportAmp = 0.6f;
		constexpr float kThrowAmp = 0.35f;
		constexpr float kThrowSeconds = 0.025f;
		constexpr float kSprayAmp = 0.25f;
		constexpr float kSpraySeconds = 0.060f;

		struct ShotKick
		{
			float amp = 0.0f; // 0: no shot at all
			float seconds = 0.0f;
			float support_amp = 0.0f; // the other hand, while it holds a long gun's fore-end
		};

		ShotKick ShotKickFor(ItemData::Kind kind)
		{
			ShotKick k;
			switch (kind)
			{
				case ItemData::Kind::LongGun:
					k.amp = kLongGunShotAmp;
					k.seconds = kLongGunShotSeconds;
					k.support_amp = kLongGunSupportAmp;
					break;
				case ItemData::Kind::Melee:
					break;
				case ItemData::Kind::Thrown:
					k.amp = kThrowAmp;
					k.seconds = kThrowSeconds;
					break;
				case ItemData::Kind::Spray:
					k.amp = kSprayAmp;
					k.seconds = kSpraySeconds;
					break;
				default:
					k.amp = kShotAmp;
					k.seconds = kShotSeconds;
					break;
			}
			return k;
		}

		// A gun (or an id the table lacks) clicks when pulled empty; a melee weapon, something to throw or a spray can
		// does not.
		bool DryFires(ItemData::Kind kind)
		{
			return kind != ItemData::Kind::Melee && kind != ItemData::Kind::Thrown && kind != ItemData::Kind::Spray;
		}

		// What one vsync read of the weapon. The self-test builds these by hand.
		struct WeaponSample
		{
			bool readable = false; // the equipped byte could be read
			u32 eq = 0;            // equipped item record index, 0 = bare hands
			bool rec_in_ram = false;
			u32 present = 0; // the record's +0x00
			u32 index = 0;   // its +0x02 (its own index)
			u32 owner = 0;   // its owner byte (player slot + 1)
			bool slot_known = false;
			u32 slot = 0; // the local player's slot (camera.base index byte)
			u16 item_id = 0;
			u16 ammo = 0;
			bool raised = false;    // stance == raised
			bool reloading = false; // action == reload
			bool trigger = false;   // right trigger past kTriggerPressed
			bool a = false;         // right A
			bool support = false;   // the left hand holds the long gun's fore-end, as the hands draw it
			int aim_hand = kRight;
			float strength = 1.0f;
			u64 now_ms = 0;
			bool log = true;
		};

		struct WeaponState
		{
			bool baseline = false;
			u32 eq = 0;
			u16 ammo = 0;
			bool trigger_down = true; // held until seen released, so a held trigger is never an edge
			bool a_down = true;
			bool tick_pending = false; // the reload's second tick
			u64 tick_ms = 0;
			Hold hold = Hold::Unknown;
			WeaponView view;
			std::array<bool, 256> bad_logged{}; // "failed checks" said once per equipped index
		};

		WeaponState s_weapon;

		// Published for the GS thread (HandModel, HudCards) and CameraDriver.
		std::atomic<u8> s_hold{static_cast<u8>(Hold::Unknown)};
		std::mutex s_weapon_mutex;
		WeaponView s_weapon_view;

		void PublishWeapon(const WeaponState& st)
		{
			s_hold.store(static_cast<u8>(st.hold), std::memory_order_release);
			std::lock_guard<std::mutex> lock(s_weapon_mutex);
			s_weapon_view = st.view;
		}

		// A new baseline: the next good read sets the rounds without a cue. The cue times and shot serial carry on.
		void ResetWeaponBaseline(WeaponState& st)
		{
			st.baseline = false;
			st.eq = 0;
			st.ammo = 0;
			st.tick_pending = false;
			st.hold = Hold::Unknown;
			st.view.valid = false;
			st.view.equipped = 0;
			st.view.item_id = 0;
			st.view.ammo = 0;
		}

		Hold ClassifyWeapon(const WeaponSample& w)
		{
			if (!w.readable)
				return Hold::Unknown;
			if (w.eq == 0)
				return Hold::BareHands;
			if (w.rec_in_ram && w.present == 1 && w.index == w.eq && w.slot_known && w.owner == w.slot + 1)
				return Hold::Weapon;
			return Hold::Unknown;
		}

		// Game rumble forwarding. The port is published by Poll; the rest is guarded by the mutex because
		// InputManager can stop vibration from outside the vsync.
		std::atomic<int> s_rumble_port{-1};
		std::atomic<float> s_strength{1.0f};
		std::mutex s_rumble_mutex;
		float s_rumble_large = 0.0f;
		float s_rumble_small = 0.0f;
		bool s_rumble_buzzing = false;

		void Pulse(int hand, float amplitude, float seconds, float strength)
		{
			const float a = std::clamp(amplitude * strength, 0.0f, 1.0f);
			if (a > 0.0f)
				XRInput::QueuePulse(hand, a, seconds);
		}

		void Step(const Sample& s, CueState& st)
		{
			// HP. A value above max (or no max) is not HP: it neither cues nor serves as the baseline, so a
			// bad read can't turn into a hit when good reads come back.
			bool hurt = false;
			u32 hp_before = 0;
			float frac = 0.0f;
			if (s.has_hp && s.hp_max > 0 && s.hp <= s.hp_max)
			{
				if (st.hp_valid && s.hp < st.hp)
				{
					hurt = true;
					hp_before = st.hp;
					frac = std::min(1.0f, static_cast<float>(st.hp - s.hp) / static_cast<float>(s.hp_max));
				}
				st.hp = s.hp;
				st.hp_valid = true;
			}
			else
			{
				u32 skipped = 0;
				if (s.has_hp && s.log && TakeLog(s_log_bad_hp, s.now_ms, kBadHpLogIntervalMs, &skipped))
					Console.Warning("(VR) Feedback: HP %u with max %u does not look like HP; the hurt and heartbeat cues "
									"sit out (check feedback.hurt in the profile).", s.hp, s.hp_max);
				st.hp_valid = false;
			}

			// Heartbeat, queued before the shot and the hit so a stronger cue of the same vsync is the one kept.
			const bool danger = st.hp_valid && s.danger_below > 0.0f && st.hp > 0 &&
			                    static_cast<float>(st.hp) < s.danger_below * static_cast<float>(s.hp_max);
			if (danger)
			{
				if (!st.beating)
				{
					st.beating = true;
					st.next_beat_ms = s.now_ms;
					st.dub_pending = false;
				}
				if (st.dub_pending && s.now_ms >= st.dub_ms)
				{
					Pulse(kLeft, kBeatDubAmp, kBeatSeconds, s.strength);
					st.dub_pending = false;
				}
				if (s.now_ms >= st.next_beat_ms)
				{
					const bool critical = static_cast<float>(st.hp) < kCriticalFraction * static_cast<float>(s.hp_max);
					const u64 period = critical ? kBeatCriticalPeriodMs : kBeatPeriodMs;
					Pulse(kLeft, kBeatLubAmp, kBeatSeconds, s.strength);
					st.dub_pending = true;
					st.dub_ms = s.now_ms + kBeatGapMs;
					// From now, not from the missed slot, so a stall can't queue a burst of beats.
					st.next_beat_ms = s.now_ms + period;
					u32 skipped = 0;
					if (s.log && TakeLog(s_log_beat, s.now_ms, kLogIntervalMs, &skipped))
						Console.WriteLn("(VR) Feedback: heartbeat, HP %u of %u (%.0f%%), left hand every %.1f s%s.",
							st.hp, s.hp_max, 100.0 * static_cast<double>(st.hp) / static_cast<double>(s.hp_max),
							static_cast<double>(period) / 1000.0, MoreSuffix(skipped).text);
				}
			}
			else
			{
				st.beating = false;
				st.dub_pending = false;
			}

			// Shots: the counter's step, modulo its width so a wrap still counts.
			if (s.has_counter)
			{
				if (st.counter_valid)
				{
					const u32 step = (s.counter - st.counter) & s.counter_mask;
					if (step >= 1 && step <= kShotMaxStep)
					{
						Pulse(s.fire_hand, kShotAmp, kShotSeconds, s.strength);
						u32 skipped = 0;
						if (s.log && TakeLog(s_log_shot, s.now_ms, kLogIntervalMs, &skipped))
							Console.WriteLn("(VR) Feedback: shot, counter %u -> %u, %s hand %.2f for %.0f ms%s.",
								st.counter, s.counter, HandName(s.fire_hand), static_cast<double>(kShotAmp),
								static_cast<double>(kShotSeconds) * 1000.0, MoreSuffix(skipped).text);
					}
				}
				st.counter = s.counter;
				st.counter_valid = true;
			}
			else
			{
				st.counter_valid = false;
			}

			if (hurt)
			{
				const float amp = kHurtBase + kHurtGain * frac;
				const float ms = kHurtMinMs + (kHurtMaxMs - kHurtMinMs) * frac;
				Pulse(kLeft, amp, ms / 1000.0f, s.strength);
				Pulse(kRight, amp, ms / 1000.0f, s.strength);
				u32 skipped = 0;
				if (s.log && TakeLog(s_log_hurt, s.now_ms, kLogIntervalMs, &skipped))
					Console.WriteLn("(VR) Feedback: hurt, HP %u -> %u of %u, both hands %.2f for %.0f ms%s.",
						hp_before, s.hp, s.hp_max, static_cast<double>(amp), static_cast<double>(ms),
						MoreSuffix(skipped).text);
			}
		}

		void WeaponStep(const WeaponSample& w, WeaponState& st)
		{
			const bool trigger_edge = (w.trigger && !st.trigger_down) || (w.a && !st.a_down);
			st.trigger_down = w.trigger;
			st.a_down = w.a;

			if (st.tick_pending && w.now_ms >= st.tick_ms)
			{
				Pulse(kLeft, kReloadTickAmp, kReloadTickSeconds, w.strength);
				Pulse(kRight, kReloadTickAmp, kReloadTickSeconds, w.strength);
				st.tick_pending = false;
			}

			const Hold hold = ClassifyWeapon(w);
			if (hold == Hold::Unknown && w.readable && w.eq != 0 && w.eq < st.bad_logged.size() && !st.bad_logged[w.eq])
			{
				st.bad_logged[w.eq] = true;
				if (w.log)
					Console.Warning("(VR) Feedback: equipped record %u failed checks (present %u, index %u, owner %u, slot %u)",
						w.eq, w.present, w.index, w.owner, w.slot);
			}
			if (hold != Hold::Weapon)
			{
				ResetWeaponBaseline(st);
				st.hold = hold;
				return;
			}

			// Another equipped record is a new baseline, no cue. The item's kind sizes the kick (none for melee).
			const bool same = st.baseline && st.eq == w.eq;
			const ItemData::Kind kind = ItemData::KindOf(w.item_id);
			if (same && w.ammo < st.ammo)
			{
				const u32 drop = static_cast<u32>(st.ammo) - static_cast<u32>(w.ammo);
				const ShotKick kick = ShotKickFor(kind);
				if (drop <= kWeaponMaxShotStep && w.raised && !w.reloading && kick.amp > 0.0f)
				{
					st.view.shot_serial++;
					st.view.shot_ms = w.now_ms;
					Pulse(w.aim_hand, kick.amp, kick.seconds, w.strength);
					// A long gun's kick reaches the left hand too while it holds the fore-end.
					const bool supported = w.support && kick.support_amp > 0.0f && w.aim_hand != kLeft;
					if (supported)
						Pulse(kLeft, kick.support_amp, kick.seconds, w.strength);
					u32 skipped = 0;
					if (w.log && TakeLog(s_log_shot, w.now_ms, kLogIntervalMs, &skipped))
						Console.WriteLn("(VR) Feedback: shot (%s), ammo %u -> %u, %s hand %.2f for %.0f ms%s%s.",
							ItemData::KindName(kind), static_cast<u32>(st.ammo), static_cast<u32>(w.ammo), HandName(w.aim_hand),
							static_cast<double>(std::min(kick.amp, 1.0f)), static_cast<double>(kick.seconds) * 1000.0,
							supported ? ", the left hand on the fore-end too" : "", MoreSuffix(skipped).text);
				}
			}
			else if (same && w.ammo > st.ammo)
			{
				st.view.reload_ms = w.now_ms;
				Pulse(kLeft, kReloadTickAmp, kReloadTickSeconds, w.strength);
				Pulse(kRight, kReloadTickAmp, kReloadTickSeconds, w.strength);
				st.tick_pending = true;
				st.tick_ms = w.now_ms + kReloadTickGapMs;
				u32 skipped = 0;
				if (w.log && TakeLog(s_log_reload, w.now_ms, kLogIntervalMs, &skipped))
					Console.WriteLn("(VR) Feedback: reload, ammo %u -> %u, two ticks in both hands%s.",
						static_cast<u32>(st.ammo), static_cast<u32>(w.ammo), MoreSuffix(skipped).text);
			}

			// Dry fire: the trigger (or A) pulled on an empty gun held up (a melee weapon, a throw or a spray never clicks).
			if (w.raised && w.ammo == 0 && trigger_edge && DryFires(kind))
			{
				st.view.dry_ms = w.now_ms;
				Pulse(w.aim_hand, kDryAmp, kDrySeconds, w.strength);
				u32 skipped = 0;
				if (w.log && TakeLog(s_log_dry, w.now_ms, kLogIntervalMs, &skipped))
					Console.WriteLn("(VR) Feedback: dry fire, empty gun, %s hand %.2f for %.0f ms%s.", HandName(w.aim_hand),
						static_cast<double>(kDryAmp), static_cast<double>(kDrySeconds) * 1000.0, MoreSuffix(skipped).text);
			}

			st.baseline = true;
			st.eq = w.eq;
			st.ammo = w.ammo;
			st.hold = Hold::Weapon;
			st.view.valid = true;
			st.view.equipped = static_cast<u8>(w.eq);
			st.view.item_id = w.item_id;
			st.view.ammo = w.ammo;
		}

		bool ReadAt(u64 address, u8 width, u32* out)
		{
			if (address + width > Ps2MemSize::MainRam)
				return false;
			const u32 a = static_cast<u32>(address);
			switch (width)
			{
				case 1: *out = memRead8(a); break;
				case 2: *out = memRead16(a); break;
				default: *out = memRead32(a); break;
			}
			return true;
		}

		u32 WidthMask(u8 width)
		{
			return (width >= 4) ? 0xFFFFFFFFu : ((1u << (width * 8u)) - 1u);
		}

		int AimHand(const ProfileDB::Profile& p)
		{
			if (p.camera.has_value() && p.camera->look_at.has_value() && p.camera->look_at->has_aim &&
				(p.camera->look_at->aim_hand == kLeft || p.camera->look_at->aim_hand == kRight))
				return p.camera->look_at->aim_hand;
			return kRight;
		}

		// Whether the left hand holds the fore-end of the long gun item_id in the right, as the hands draw them
		// (HandModel::OffHandSupports from the grip positions and the right hand's aim orientation; one read, so the
		// take-hold distance).
		bool LeftOnForeEnd(const VRInputSnapshot& snap, u16 item_id)
		{
			if (ItemData::KindOf(item_id) != ItemData::Kind::LongGun)
				return false;
			const VRHandState& gun_src = snap.hands[VRInputSnapshot::RIGHT];
			const VRHandState& off_src = snap.hands[VRInputSnapshot::LEFT];
			if (!gun_src.grip_pose.valid || !off_src.grip_pose.valid)
				return false;
			const VRPose& orient = gun_src.aim_pose.valid ? gun_src.aim_pose : gun_src.grip_pose;
			HandModel::HandState gun_hand;
			gun_hand.valid = true;
			gun_hand.gun = true;
			gun_hand.held = HandModel::Held::LongGun;
			gun_hand.item_id = item_id;
			HandModel::HandState off_hand;
			off_hand.valid = true;
			for (int k = 0; k < 3; k++)
			{
				gun_hand.pos[k] = gun_src.grip_pose.position_xyz[k];
				off_hand.pos[k] = off_src.grip_pose.position_xyz[k];
			}
			for (int k = 0; k < 4; k++)
				gun_hand.quat[k] = orient.orientation_xyzw[k];
			return HandModel::OffHandSupports(gun_hand, off_hand, false);
		}

		float HapticStrength()
		{
			const float v = EmuConfig.VR.HapticStrength;
			return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 1.0f;
		}

		// With s_rumble_mutex held.
		void StopRumbleLocked()
		{
			if (s_rumble_buzzing)
			{
				XRInput::QueueHaptic(kLeft, 0.0f);
				XRInput::QueueHaptic(kRight, 0.0f);
				s_rumble_buzzing = false;
			}
			s_rumble_large = 0.0f;
			s_rumble_small = 0.0f;
		}

		// Forwarding needs the profile to ask for it, the GameRumble setting and a VR Gamepad on a pad port.
		// It doesn't use bindings, so a DualSense bound to the same port keeps its own rumble.
		void UpdateRumbleTarget(const ProfileDB::Profile* profile, const ProfileDB::FeedbackParams* fb)
		{
			int port = -1;
			if (profile && fb && fb->pad_rumble && EmuConfig.VR.GameRumble)
			{
				for (const ProfileDB::SpatialControlSpec& c : profile->controls)
				{
					if (c.kind == SpatialControls::DeviceKind::Gamepad && c.usb_port < 0)
					{
						port = static_cast<int>(c.pad_port);
						break;
					}
				}
			}
			const int prev = s_rumble_port.exchange(port, std::memory_order_acq_rel);
			if (prev == port)
				return;
			std::lock_guard<std::mutex> lock(s_rumble_mutex);
			StopRumbleLocked();
			if (port >= 0)
				Console.WriteLn("(VR) Feedback: the game's rumble on pad %d goes to the VR controllers.", port + 1);
			else
				Console.WriteLn("(VR) Feedback: game rumble forwarding stopped.");
		}

		void MaybeRunSelfTest()
		{
			static const bool enabled = (std::getenv("PCSX2_VR_FEEDBACK_SELFTEST") != nullptr);
			static bool ran = false;
			if (!enabled || ran)
				return;
			ran = true;
			if (XRInput::IsInitialized())
			{
				Console.Warning("(VR) Feedback self-test skipped: VR controller input is running and would take the test "
								"pulses (run it with no headset session).");
				return;
			}
			SelfTest();
		}
	}

	void Poll()
	{
		MaybeRunSelfTest();

		// Haptics need a running session; without one (flat play) no profile is looked up and nothing is read.
		const bool session = XRSession::IsSessionRunning();
		const ProfileDB::Profile* profile = nullptr;
		if (session)
		{
			const std::string serial = VMManager::GetDiscSerial();
			if (!serial.empty())
				profile = ProfileDB::Lookup(serial, VMManager::GetDiscCRC());
		}
		const ProfileDB::FeedbackParams* fb =
			(profile && profile->feedback.has_value()) ? &profile->feedback.value() : nullptr;

		const float strength = HapticStrength();
		s_strength.store(strength, std::memory_order_relaxed);
		UpdateRumbleTarget(profile, fb);

		// The cues use the camera's guards and its record: no cues on the title screen or in cutscenes. They
		// keep going while grabbed, down or dead (LocalRecordAny ignores releaseWhen), so a bite still pulses.
		// Each time they stop, the next reads are a fresh baseline, not a hit.
		u32 record = 0;
		if (!fb || (!fb->has_hurt && !fb->has_fire && !fb->has_weapon) || VMManager::GetState() != VMState::Running ||
			!CameraDriver::LocalRecordAny(&record))
		{
			s_cues = CueState{};
			ResetWeaponBaseline(s_weapon);
			s_weapon.trigger_down = true;
			s_weapon.a_down = true;
			PublishWeapon(s_weapon);
			return;
		}
		// Another record (a different character slot) is a new baseline too.
		if (s_cues.record_valid && s_cues.record != record)
		{
			s_cues = CueState{};
			ResetWeaponBaseline(s_weapon);
		}
		s_cues.record_valid = true;
		s_cues.record = record;

		Sample s;
		s.now_ms = CameraDriver::SteadyNowMs();
		s.strength = strength;
		if (fb->has_hurt)
		{
			s.has_hp = ReadAt(static_cast<u64>(record) + fb->hurt_offset, fb->hurt_width, &s.hp) &&
			           ReadAt(static_cast<u64>(record) + fb->hurt_max_offset, fb->hurt_width, &s.hp_max);
			s.danger_below = fb->has_danger ? fb->danger_below : 0.0f;
		}
		// With feedback.weapon the weapon reader owns the shot kick; the counter would kick twice.
		if (fb->has_fire && !fb->has_weapon)
		{
			s.has_counter = ReadAt(fb->fire_address, fb->fire_width, &s.counter);
			s.counter_mask = WidthMask(fb->fire_width);
			s.fire_hand = (fb->fire_hand >= 0) ? fb->fire_hand : AimHand(*profile);
		}
		Step(s, s_cues);

		// Weapon: reads only, the local record and its own equipped item record behind the owner check.
		if (fb->has_weapon)
		{
			WeaponSample w;
			w.now_ms = s.now_ms;
			w.strength = strength;
			w.aim_hand = AimHand(*profile);
			u32 eq = 0;
			w.readable = ReadAt(static_cast<u64>(record) + fb->weapon_equipped_offset, 1, &eq);
			w.eq = eq;
			if (profile->camera.has_value() && profile->camera->base.has_value() && profile->camera->base->is_indexed)
				w.slot_known = ReadAt(profile->camera->base->index_addr, 1, &w.slot);
			const u64 rec = static_cast<u64>(fb->weapon_table) + static_cast<u64>(eq) * static_cast<u64>(fb->weapon_stride);
			w.rec_in_ram = w.readable && eq != 0 && rec + fb->weapon_stride <= static_cast<u64>(Ps2MemSize::MainRam);
			if (w.rec_in_ram)
			{
				u32 v = 0;
				ReadAt(rec, 1, &w.present);
				ReadAt(rec + 2, 1, &w.index);
				ReadAt(rec + fb->weapon_owner_offset, 1, &w.owner);
				if (ReadAt(rec + fb->weapon_id_offset, 2, &v))
					w.item_id = static_cast<u16>(v);
				if (ReadAt(rec + fb->weapon_count_offset, 2, &v))
					w.ammo = static_cast<u16>(v);
			}
			u32 stance = 0;
			w.raised = ReadAt(static_cast<u64>(record) + fb->weapon_stance_offset, 1, &stance) &&
			           stance == fb->weapon_stance_raised;
			if (fb->has_weapon_reload)
			{
				u32 action = 0;
				w.reloading = ReadAt(static_cast<u64>(record) + fb->weapon_action_offset, 1, &action) &&
				              action == fb->weapon_reload_action;
			}
			const VRInputSnapshot snap = GetInputSnapshot();
			if (snap.actions_active)
			{
				w.trigger = snap.hands[VRInputSnapshot::RIGHT].trigger > kTriggerPressed;
				w.a = snap.hands[VRInputSnapshot::RIGHT].a;
				w.support = LeftOnForeEnd(snap, w.item_id);
			}
			WeaponStep(w, s_weapon);
		}
		else
		{
			ResetWeaponBaseline(s_weapon);
		}
		PublishWeapon(s_weapon);
	}

	Hold WeaponInHand()
	{
		return static_cast<Hold>(s_hold.load(std::memory_order_acquire));
	}

	WeaponView GetWeapon()
	{
		std::lock_guard<std::mutex> lock(s_weapon_mutex);
		return s_weapon_view;
	}

	void OnPadRumble(u32 pad, float large_motor, float small_motor)
	{
		const int port = s_rumble_port.load(std::memory_order_acquire);
		if (port < 0 || pad != static_cast<u32>(port))
			return;
		const float large = std::isfinite(large_motor) ? std::clamp(large_motor, 0.0f, 1.0f) : 0.0f;
		const float small_level = std::isfinite(small_motor) ? std::clamp(small_motor, 0.0f, 1.0f) : 0.0f;

		std::lock_guard<std::mutex> lock(s_rumble_mutex);
		if (large == s_rumble_large && small_level == s_rumble_small)
			return;
		s_rumble_large = large;
		s_rumble_small = small_level;

		// The large motor is felt in both hands, the small one in the right, where it sits in a DualShock.
		const float strength = s_strength.load(std::memory_order_relaxed);
		const float left = large * strength;
		const float right = std::max(large, small_level) * strength;
		if (left > 0.0f || right > 0.0f || s_rumble_buzzing)
		{
			XRInput::QueueHaptic(kLeft, left);
			XRInput::QueueHaptic(kRight, right);
			s_rumble_buzzing = (left > 0.0f || right > 0.0f);
		}
		u32 skipped = 0;
		if (TakeLog(s_log_rumble, CameraDriver::SteadyNowMs(), kLogIntervalMs, &skipped))
			Console.WriteLn("(VR) Feedback: game rumble on pad %u, large %.2f small %.2f -> left %.2f right %.2f%s.",
				pad + 1, static_cast<double>(large), static_cast<double>(small_level), static_cast<double>(left),
				static_cast<double>(right), MoreSuffix(skipped).text);
	}

	void PauseRumble()
	{
		std::lock_guard<std::mutex> lock(s_rumble_mutex);
		StopRumbleLocked();
	}

	void OnStateLoaded()
	{
		s_cues = CueState{};
		ResetWeaponBaseline(s_weapon);
		s_weapon.trigger_down = true;
		s_weapon.a_down = true;
		PublishWeapon(s_weapon);
	}

	bool SelfTest()
	{
		int fail = 0;
		const auto check = [&fail](bool ok, const char* name) {
			if (!ok)
			{
				++fail;
				Console.Error("(VR) Feedback self-test FAIL: %s", name);
			}
		};
		const auto approx = [](float a, float b) { return std::abs(a - b) < 0.002f; };
		float amp = 0.0f, secs = 0.0f;
		const auto take = [&amp, &secs](int hand) { return XRInput::TakePendingPulseForTest(hand, &amp, &secs); };
		const auto none = [&take]() {
			const bool l = take(kLeft);
			const bool r = take(kRight);
			return !l && !r;
		};

		none(); // drop anything already queued

		// Hit: the first read is a baseline; a drop pulses both hands at 0.4 + 0.6 d/max for 120-250 ms.
		CueState st;
		Sample s;
		s.log = false;
		s.has_hp = true;
		s.hp_max = 100;
		s.hp = 100;
		s.now_ms = 1000;
		Step(s, st);
		check(none(), "first HP read is a baseline");
		s.hp = 80;
		s.now_ms += 17;
		Step(s, st);
		check(take(kLeft) && approx(amp, 0.52f) && approx(secs, 0.146f), "HP -20 of 100: left 0.52 for 146 ms");
		check(take(kRight) && approx(amp, 0.52f) && approx(secs, 0.146f), "HP -20 of 100: right 0.52 for 146 ms");
		s.hp = 90;
		s.now_ms += 17;
		Step(s, st);
		check(none(), "healing does not pulse");
		s.hp = 0;
		s.now_ms += 17;
		Step(s, st);
		check(take(kLeft) && approx(amp, 0.94f) && approx(secs, 0.237f), "HP -90 of 100: left 0.94 for 237 ms");
		check(take(kRight) && approx(amp, 0.94f), "HP -90 of 100: right 0.94");
		s.hp = 150;
		s.now_ms += 17;
		Step(s, st);
		check(none(), "HP above max does not pulse");
		s.hp = 50;
		s.now_ms += 17;
		Step(s, st);
		check(none(), "HP above max breaks the baseline");
		s.strength = 0.5f;
		s.hp = 30;
		s.now_ms += 17;
		Step(s, st);
		check(take(kLeft) && approx(amp, 0.26f), "HapticStrength 0.5 halves the hit");
		none();
		s.strength = 1.0f;

		// Heartbeat: below danger * max, two left-hand beats 150 ms apart every 1.0 s (0.8 s below 10%).
		st = CueState{};
		s.danger_below = 0.25f;
		s.hp = 20;
		const u64 t0 = 5000;
		s.now_ms = t0;
		Step(s, st);
		check(take(kLeft) && approx(amp, kBeatLubAmp) && approx(secs, kBeatSeconds), "danger: first beat at once, left hand");
		check(!take(kRight), "danger: nothing in the right hand");
		s.now_ms = t0 + 100;
		Step(s, st);
		check(none(), "danger: nothing between the two beats");
		s.now_ms = t0 + 167;
		Step(s, st);
		check(take(kLeft) && approx(amp, kBeatDubAmp), "danger: second beat 150 ms later");
		s.now_ms = t0 + 983;
		Step(s, st);
		check(none(), "danger: quiet until the next second");
		s.now_ms = t0 + 1001;
		Step(s, st);
		check(take(kLeft) && approx(amp, kBeatLubAmp), "danger: next beat after 1.0 s");
		s.now_ms = t0 + 1160;
		Step(s, st);
		check(take(kLeft) && approx(amp, kBeatDubAmp), "danger: its second beat");
		// A fresh baseline at 5 HP: dropping there would be a hit, which outweighs the beat.
		st = CueState{};
		s.hp = 5;
		s.now_ms = t0 + 3000;
		Step(s, st);
		check(take(kLeft) && approx(amp, kBeatLubAmp), "critical: first beat at once");
		s.now_ms = t0 + 3160;
		Step(s, st);
		check(take(kLeft) && approx(amp, kBeatDubAmp), "critical: second beat");
		s.now_ms = t0 + 3790;
		Step(s, st);
		check(none(), "critical: quiet before 0.8 s");
		s.now_ms = t0 + 3801;
		Step(s, st);
		check(take(kLeft) && approx(amp, kBeatLubAmp), "critical: next beat after 0.8 s");
		s.hp = 60;
		s.now_ms = t0 + 3960;
		Step(s, st);
		check(none(), "out of danger: the heartbeat stops and its second beat is dropped");
		st = CueState{};
		s.hp = 0;
		s.now_ms = t0 + 5000;
		Step(s, st);
		check(none(), "no heartbeat at 0 HP");

		// Shots: the counter rising by 1-5 kicks the aim hand at 1.0 for 40 ms; other changes are ignored.
		st = CueState{};
		s = Sample{};
		s.log = false;
		s.has_counter = true;
		s.counter_mask = 0xFFFFu;
		s.fire_hand = kRight;
		s.counter = 10;
		s.now_ms = 9000;
		Step(s, st);
		check(none(), "first counter read is a baseline");
		s.counter = 11;
		Step(s, st);
		check(take(kRight) && approx(amp, 1.0f) && approx(secs, 0.040f), "shot: aim hand 1.0 for 40 ms");
		check(!take(kLeft), "shot: nothing in the other hand");
		Step(s, st);
		check(none(), "no shot while the counter holds");
		s.counter = 30;
		Step(s, st);
		check(none(), "a jump of 19 is a reset, not shots");
		s.counter = 0xFFFE;
		Step(s, st);
		check(none(), "a jump backwards is a reset");
		s.counter = 0x0001;
		Step(s, st);
		check(take(kRight), "the counter wrapping by 3 is a shot");
		s.counter = 0x0000;
		Step(s, st);
		check(none(), "a counter going down is not a shot");
		s.fire_hand = kLeft;
		s.counter = 2;
		Step(s, st);
		check(take(kLeft), "fire.hand left kicks the left hand");
		check(!take(kRight), "fire.hand left leaves the right hand alone");

		// Weapon: record 87 owned by slot 0 (owner 1), 7 rounds, raised.
		{
			WeaponState ws;
			WeaponSample w;
			w.log = false;
			w.readable = true;
			w.eq = 87;
			w.rec_in_ram = true;
			w.present = 1;
			w.index = 87;
			w.owner = 1;
			w.slot_known = true;
			w.slot = 0;
			w.item_id = 0x57;
			w.ammo = 7;
			w.raised = true;
			w.aim_hand = kRight;
			w.now_ms = 20000;
			WeaponStep(w, ws);
			check(none(), "weapon: first read is a baseline");
			check(ws.hold == Hold::Weapon && ws.view.valid && ws.view.ammo == 7, "weapon: a good record is Weapon with 7 rounds");
			w.ammo = 6;
			w.now_ms += 17;
			WeaponStep(w, ws);
			check(take(kRight) && approx(amp, kShotAmp) && approx(secs, kShotSeconds), "weapon: 7 -> 6 raised kicks the aim hand");
			check(!take(kLeft), "weapon: the shot leaves the other hand alone");
			check(ws.view.shot_serial == 1 && ws.view.shot_ms == w.now_ms, "weapon: the shot is counted and timed");
			w.now_ms += 17;
			WeaponStep(w, ws);
			check(none(), "weapon: no kick while the rounds hold");

			// Another record with fewer rounds: a new baseline, no kick.
			w.eq = 88;
			w.index = 88;
			w.ammo = 5;
			w.now_ms += 17;
			WeaponStep(w, ws);
			check(none(), "weapon: an equipped change does not kick");
			check(ws.view.shot_serial == 1, "weapon: an equipped change is not a shot");
			w.eq = 87;
			w.index = 87;
			w.ammo = 6;
			w.now_ms += 17;
			WeaponStep(w, ws);
			check(none(), "weapon: changing back is a baseline too");

			// Reload 6 -> 7: a tick in both hands now and another 60 ms later.
			w.ammo = 7;
			w.now_ms += 17;
			const u64 reload_at = w.now_ms;
			WeaponStep(w, ws);
			check(take(kLeft) && approx(amp, kReloadTickAmp) && approx(secs, kReloadTickSeconds), "reload: left tick");
			check(take(kRight) && approx(amp, kReloadTickAmp), "reload: right tick");
			check(ws.view.reload_ms == reload_at, "reload: timed");
			w.now_ms = reload_at + 30;
			WeaponStep(w, ws);
			check(none(), "reload: nothing between the ticks");
			w.now_ms = reload_at + 61;
			WeaponStep(w, ws);
			check(take(kLeft) && take(kRight), "reload: the second tick 60 ms later");
			w.now_ms += 17;
			WeaponStep(w, ws);
			check(none(), "reload: only two ticks");

			// Lowered: a drop is not a shot. Nor is a drop during the reload action, or one of 3.
			w.raised = false;
			w.ammo = 6;
			w.now_ms += 17;
			WeaponStep(w, ws);
			check(none(), "weapon: a drop at stance 1 is ignored");
			w.raised = true;
			w.reloading = true;
			w.ammo = 5;
			w.now_ms += 17;
			WeaponStep(w, ws);
			check(none(), "weapon: a drop during the reload action is ignored");
			w.reloading = false;
			w.ammo = 2;
			w.now_ms += 17;
			WeaponStep(w, ws);
			check(none(), "weapon: a drop of 3 is not a shot");

			// Empty: a trigger edge clicks; holding it does not repeat; A counts too.
			w.ammo = 0;
			w.now_ms += 17;
			WeaponStep(w, ws);
			none();
			w.trigger = true;
			w.now_ms += 17;
			WeaponStep(w, ws);
			check(take(kRight) && approx(amp, kDryAmp) && approx(secs, kDrySeconds), "dry fire: trigger edge on an empty gun");
			check(ws.view.dry_ms == w.now_ms, "dry fire: timed");
			w.now_ms += 17;
			WeaponStep(w, ws);
			check(none(), "dry fire: a held trigger does not repeat");
			w.trigger = false;
			w.a = true;
			w.now_ms += 17;
			WeaponStep(w, ws);
			check(take(kRight) && approx(amp, kDryAmp), "dry fire: right A edge");
			w.a = false;
			w.ammo = 3;
			w.now_ms += 17;
			WeaponStep(w, ws);
			none(); // the reload ticks
			w.trigger = true;
			w.now_ms += 17;
			WeaponStep(w, ws);
			check(none(), "dry fire: not with rounds left");
			w.trigger = false;
			w.now_ms += 200;
			WeaponStep(w, ws);
			none();

			// Identity checks: a record of another player, or not naming itself, is Unknown and silent.
			WeaponSample bad = w;
			bad.owner = 2;
			bad.ammo = 2;
			bad.now_ms += 17;
			WeaponStep(bad, ws);
			check(ws.hold == Hold::Unknown && !ws.view.valid, "weapon: wrong owner is Unknown");
			check(none(), "weapon: wrong owner does not kick");
			bad = w;
			bad.index = 86;
			check(ClassifyWeapon(bad) == Hold::Unknown, "weapon: wrong index is Unknown");
			bad = w;
			bad.present = 0;
			check(ClassifyWeapon(bad) == Hold::Unknown, "weapon: an absent record is Unknown");
			bad = w;
			bad.slot_known = false;
			check(ClassifyWeapon(bad) == Hold::Unknown, "weapon: no local slot is Unknown");
			bad = w;
			bad.eq = 0;
			check(ClassifyWeapon(bad) == Hold::BareHands, "weapon: nothing equipped is BareHands");
			bad = w;
			bad.readable = false;
			check(ClassifyWeapon(bad) == Hold::Unknown, "weapon: an unreadable record is Unknown");
			w.ammo = 2;
			w.now_ms += 34;
			WeaponStep(w, ws);
			check(none() && ws.hold == Hold::Weapon, "weapon: back to a good record is a baseline");
		}

		// Kinds (ItemData): a long gun kicks harder and longer (and in the left hand too while it holds the fore-end), a
		// melee weapon not at all and it never clicks empty, a throw and a spray lightly.
		{
			WeaponState ws;
			WeaponSample w;
			w.log = false;
			w.readable = true;
			w.eq = 90;
			w.rec_in_ram = true;
			w.present = 1;
			w.index = 90;
			w.owner = 1;
			w.slot_known = true;
			w.slot = 0;
			w.item_id = 11; // Shotgun
			w.ammo = 7;
			w.raised = true;
			w.aim_hand = kRight;
			w.now_ms = 40000;
			WeaponStep(w, ws);
			check(none(), "long gun: first read is a baseline");
			w.ammo = 6;
			w.now_ms += 17;
			WeaponStep(w, ws);
			check(take(kRight) && approx(amp, 1.0f) && approx(secs, kLongGunShotSeconds),
				"long gun: a full kick twice as long in the aim hand");
			check(!take(kLeft), "long gun: nothing in the left hand while it is off the fore-end");
			w.support = true;
			w.ammo = 5;
			w.now_ms += 17;
			WeaponStep(w, ws);
			check(take(kRight) && approx(secs, kLongGunShotSeconds), "long gun, left hand on the fore-end: the aim hand kicks");
			check(take(kLeft) && approx(amp, kLongGunSupportAmp) && approx(secs, kLongGunShotSeconds),
				"long gun, left hand on the fore-end: and the left hand with it");
			w.support = false;
			w.strength = 0.5f;
			w.ammo = 4;
			w.now_ms += 17;
			WeaponStep(w, ws);
			check(take(kRight) && approx(amp, 0.7f), "long gun at Haptic Strength 0.5: 0.7, more than a handgun's 0.5");
			none();
			w.strength = 1.0f;

			// Melee (an iron pipe): a drop in its count is no shot, and empty it never clicks.
			w.eq = 91;
			w.index = 91;
			w.item_id = 103;
			w.ammo = 3;
			w.now_ms += 17;
			WeaponStep(w, ws);
			check(none(), "melee: another record is a baseline");
			const u32 shots_before = ws.view.shot_serial;
			w.ammo = 2;
			w.now_ms += 17;
			WeaponStep(w, ws);
			check(none() && ws.view.shot_serial == shots_before, "melee: a count drop is no shot and no kick");
			w.ammo = 0;
			w.now_ms += 17;
			WeaponStep(w, ws);
			none();
			w.trigger = true;
			w.now_ms += 17;
			WeaponStep(w, ws);
			check(none() && ws.view.dry_ms != w.now_ms, "melee: no dry fire");
			w.trigger = false;
			w.now_ms += 17;
			WeaponStep(w, ws);
			none();

			// Something to throw and a spray can: a light cue each.
			w.eq = 92;
			w.index = 92;
			w.item_id = 114; // Molotov Cocktail
			w.ammo = 2;
			w.now_ms += 17;
			WeaponStep(w, ws);
			none();
			w.ammo = 1;
			w.now_ms += 17;
			WeaponStep(w, ws);
			check(take(kRight) && approx(amp, kThrowAmp) && approx(secs, kThrowSeconds), "thrown: a light cue");
			w.eq = 93;
			w.index = 93;
			w.item_id = 28; // Pesticide Spray
			w.ammo = 3;
			w.now_ms += 17;
			WeaponStep(w, ws);
			none();
			w.ammo = 2;
			w.now_ms += 17;
			WeaponStep(w, ws);
			check(take(kRight) && approx(amp, kSprayAmp) && approx(secs, kSpraySeconds), "spray: a light cue");
			check(ShotKickFor(ItemData::Kind::Handgun).amp == kShotAmp && ShotKickFor(ItemData::Kind::Unknown).amp == kShotAmp &&
					ShotKickFor(ItemData::Kind::Melee).amp == 0.0f,
				"a handgun and an id the table lacks keep the pistol's kick; melee has none");
			none();
		}

		// Stronger wins: a weaker pulse queued after a stronger one must not replace it (a beat vs a bite).
		XRInput::QueuePulse(kRight, 0.3f, 0.05f);
		XRInput::QueuePulse(kRight, 0.8f, 0.05f);
		XRInput::QueuePulse(kRight, 0.2f, 0.05f);
		check(take(kRight) && approx(amp, 0.8f), "stronger pulse wins (0.3, 0.8, 0.2 -> 0.8)");
		none();

		Console.WriteLn(fail == 0 ? Color_StrongGreen : Color_StrongRed, "(VR) Feedback self-test: %d failure(s).", fail);
		return fail == 0;
	}
}

// Item records: the item table feedback.weapon names, read and checked the way the weapon reader checks the equipped
// record, and named from ItemData (the belt pouches' toasts). Reads only.
namespace VR::GameFeedback
{
	namespace
	{
		bool ReadItemMemory(u32 address, u8 width, u32* value)
		{
			if (!VMManager::HasValidVM() || static_cast<u64>(address) + width > Ps2MemSize::MainRam)
				return false;
			switch (width)
			{
				case 1: *value = memRead8(address); break;
				case 2: *value = memRead16(address); break;
				default: *value = memRead32(address); break;
			}
			return true;
		}

		// The weapon reader's identity rule (ClassifyWeapon's Weapon case), for any record index.
		bool ItemRecordPasses(u32 index, u32 present, u32 own_index, u32 owner, bool slot_known, u32 slot)
		{
			return index != 0 && present == 1 && own_index == index && slot_known && owner == slot + 1;
		}

		// ItemSelfTest's guest memory: a small window of scripted RAM at kItemTestBase, little-endian like the EE's.
		constexpr u32 kItemTestBase = 0x00100000;
		std::array<u8, 0x300> s_item_test_ram{};

		bool ItemTestRead(u32 address, u8 width, u32* value)
		{
			if (address < kItemTestBase ||
				static_cast<u64>(address) + width > static_cast<u64>(kItemTestBase) + s_item_test_ram.size())
				return false;
			u32 v = 0;
			for (u32 i = 0; i < width; i++)
				v |= static_cast<u32>(s_item_test_ram[static_cast<size_t>(address - kItemTestBase + i)]) << (8u * i);
			*value = v;
			return true;
		}

		void ItemTestPut(u32 address, u8 width, u32 value)
		{
			for (u32 i = 0; i < width; i++)
			{
				const u64 at = static_cast<u64>(address) + i;
				if (at >= kItemTestBase && at < static_cast<u64>(kItemTestBase) + s_item_test_ram.size())
					s_item_test_ram[static_cast<size_t>(at - kItemTestBase)] = static_cast<u8>(value >> (8u * i));
			}
		}
	}

	ItemTable ItemTableOf(const ProfileDB::Profile& profile)
	{
		ItemTable t;
		if (!profile.feedback.has_value() || !profile.feedback->has_weapon || !profile.camera.has_value() ||
			!profile.camera->base.has_value() || !profile.camera->base->is_indexed)
			return t;
		const ProfileDB::FeedbackParams& fb = profile.feedback.value();
		t.table = fb.weapon_table;
		t.stride = fb.weapon_stride;
		t.id_offset = fb.weapon_id_offset;
		t.count_offset = fb.weapon_count_offset;
		t.owner_offset = fb.weapon_owner_offset;
		// Read as one byte, as the weapon reader reads it.
		t.slot_address = profile.camera->base->index_addr;
		t.valid = (t.stride != 0);
		return t;
	}

	ItemRecord ReadItemRecord(const ItemTable& table, u32 index, ItemMemReader reader)
	{
		ItemRecord r;
		r.index = index;
		if (!table.valid || index == 0 || table.stride == 0)
			return r;
		const u64 rec = static_cast<u64>(table.table) + static_cast<u64>(index) * static_cast<u64>(table.stride);
		if (rec + table.stride > static_cast<u64>(Ps2MemSize::MainRam))
			return r;
		const ItemMemReader peek = reader ? reader : ReadItemMemory;
		const u32 at = static_cast<u32>(rec);
		u32 id = 0;
		u32 count = 0;
		r.slot_known = peek(table.slot_address, 1, &r.slot);
		if (!peek(at, 1, &r.present) || !peek(at + 2, 1, &r.own_index) || !peek(at + table.owner_offset, 1, &r.owner) ||
			!peek(at + table.id_offset, 2, &id) || !peek(at + table.count_offset, 2, &count))
			return r;
		r.item_id = static_cast<u16>(id);
		r.count = static_cast<u16>(count);
		r.valid = ItemRecordPasses(index, r.present, r.own_index, r.owner, r.slot_known, r.slot);
		return r;
	}

	std::string ItemLabel(u16 item_id, u16 count)
	{
		const ItemData::Entry* e = ItemData::Find(item_id);
		const unsigned n = count;
		char text[48];
		if (!e)
			std::snprintf(text, sizeof(text), "Item %u", static_cast<unsigned>(item_id));
		else if (ItemData::IsGun(e->kind) || e->kind == ItemData::Kind::Spray)
			std::snprintf(text, sizeof(text), "%s %u", e->name, n);
		else if (e->kind == ItemData::Kind::Ammo || (e->kind == ItemData::Kind::Recovery && n > 1))
			std::snprintf(text, sizeof(text), "%s x%u", e->name, n);
		else
			std::snprintf(text, sizeof(text), "%s", e->name);
		return std::string(text);
	}

	bool ItemSelfTest()
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

		// ItemData: a few ids from the table, and nothing for an id it does not have.
		const auto named = [](u16 id, const char* want, ItemData::Kind kind) {
			const ItemData::Entry* e = ItemData::Find(id);
			return e && e->id == id && e->kind == kind && std::string(e->name) == want && std::string(ItemData::Name(id)) == want &&
			       ItemData::KindOf(id) == kind;
		};
		check(named(0, "Handgun", ItemData::Kind::Handgun), "item 0 is the Handgun");
		check(named(210, "45 Auto Magazine", ItemData::Kind::Ammo), "item 210 is the 45 Auto Magazine (ammo)");
		check(named(400, "Kevin's 45 Auto", ItemData::Kind::Handgun), "item 400 is Kevin's 45 Auto (a handgun)");
		check(named(300, "Green Herb", ItemData::Kind::Recovery), "item 300 is the Green Herb (recovery)");
		check(ItemData::Find(9) == nullptr && std::string(ItemData::Name(9)).empty() && ItemData::Find(12345) == nullptr &&
				  std::string(ItemData::Name(65535)).empty() && ItemData::KindOf(9) == ItemData::Kind::Unknown,
			"an id the table does not have has no entry and the name \"\"");
		bool sorted = ItemData::Count() > 0;
		for (u32 i = 1; i < ItemData::Count(); i++)
			sorted = sorted && ItemData::At(i - 1).id < ItemData::At(i).id;
		check(sorted, "the item table is sorted by id, so Find's binary search sees every entry");

		// Labels: rounds after a gun or a spray, x and the count after ammo (and recovery past one), else the name alone.
		check(ItemLabel(400, 7) == "Kevin's 45 Auto 7", "a gun shows its rounds (Kevin's 45 Auto 7)");
		check(ItemLabel(0, 0) == "Handgun 0", "an empty gun shows 0 rounds");
		check(ItemLabel(210, 7) == "45 Auto Magazine x7", "ammo shows its count (45 Auto Magazine x7)");
		check(ItemLabel(300, 1) == "Green Herb" && ItemLabel(300, 0) == "Green Herb", "one herb shows its name alone");
		check(ItemLabel(303, 2) == "Mixed Herb (G+G) x2", "a recovery item holding more than one shows the count");
		check(ItemLabel(100, 1) == "Survival Knife", "a knife shows its name alone");
		check(ItemLabel(29, 30) == "Flame Spray 30", "a spray weapon shows its count");
		check(ItemLabel(10102, 1) == "Key with a Blue Tag", "a key item shows its name alone");
		check(ItemLabel(12345, 3) == "Item 12345", "an id the table does not have shows \"Item <id>\"");

		// Records: the table at +0x10 of the scripted RAM (60-byte records laid out as Outbreak's), the local slot byte
		// at +0x00. Slot 0, so owner 1 is ours.
		s_item_test_ram.fill(0);
		ItemTable tbl;
		tbl.valid = true;
		tbl.table = kItemTestBase + 0x10;
		tbl.stride = 60;
		tbl.id_offset = 0x06;
		tbl.count_offset = 0x24;
		tbl.owner_offset = 0x28;
		tbl.slot_address = kItemTestBase;
		const auto put_record = [&tbl](u32 index, u32 present, u32 own_index, u32 id, u32 count, u32 owner) {
			const u32 base = tbl.table + index * tbl.stride;
			ItemTestPut(base, 1, present);
			ItemTestPut(base + 2, 1, own_index);
			ItemTestPut(base + tbl.id_offset, 2, id);
			ItemTestPut(base + tbl.count_offset, 2, count);
			ItemTestPut(base + tbl.owner_offset, 1, owner);
		};
		ItemTestPut(tbl.slot_address, 1, 0);
		put_record(3, 1, 3, 400, 7, 1);    // ours
		put_record(5, 1, 5, 210, 7, 2);    // slot 1's
		put_record(6, 0, 6, 300, 1, 1);    // not present
		put_record(7, 1, 8, 300, 1, 1);    // names another index
		put_record(9, 1, 9, 12345, 1, 1);  // an id ItemData does not have
		const ItemRecord ours = ReadItemRecord(tbl, 3, ItemTestRead);
		check(ours.valid && ours.index == 3 && ours.item_id == 400 && ours.count == 7 &&
				  ItemLabel(ours.item_id, ours.count) == "Kevin's 45 Auto 7",
			"a record that is present, names itself and is ours is read: Kevin's 45 Auto 7");
		check(!ReadItemRecord(tbl, 5, ItemTestRead).valid, "another player's record (owner = slot 1 + 1) is not named");
		ItemTestPut(tbl.slot_address, 1, 1);
		check(ReadItemRecord(tbl, 5, ItemTestRead).valid && !ReadItemRecord(tbl, 3, ItemTestRead).valid,
			"as slot 1, slot 1's record is ours and slot 0's is not");
		ItemTestPut(tbl.slot_address, 1, 0);
		check(!ReadItemRecord(tbl, 6, ItemTestRead).valid, "a record that is not present is not named");
		check(!ReadItemRecord(tbl, 7, ItemTestRead).valid, "a record naming another index is not named");
		check(!ReadItemRecord(tbl, 0, ItemTestRead).valid, "index 0 (an empty cell) names nothing");
		const ItemRecord unknown = ReadItemRecord(tbl, 9, ItemTestRead);
		check(unknown.valid && ItemLabel(unknown.item_id, unknown.count) == "Item 12345",
			"a good record with an id the table does not have shows \"Item <id>\"");
		check(!ReadItemRecord(tbl, 40, ItemTestRead).valid, "a record the reader cannot read names nothing");
		ItemTable no_slot = tbl;
		no_slot.slot_address = kItemTestBase - 1;
		check(!ReadItemRecord(no_slot, 3, ItemTestRead).valid, "without the local slot byte nothing is named");
		ItemTable past_ram = tbl;
		past_ram.table = Ps2MemSize::MainRam - 30;
		check(!ReadItemRecord(past_ram, 1, ItemTestRead).valid, "a record past the end of main RAM is not read");
		ItemTable no_table = tbl;
		no_table.valid = false;
		check(!ReadItemRecord(no_table, 3, ItemTestRead).valid, "no item table (no feedback.weapon), no names");

		// The table from a profile: feedback.weapon and an indexed camera.base.
		ProfileDB::Profile prof;
		check(!ItemTableOf(prof).valid, "a profile without feedback.weapon has no item table");
		prof.feedback.emplace();
		prof.feedback->has_weapon = true;
		prof.feedback->weapon_table = 0x3967D0;
		prof.camera.emplace();
		prof.camera->base.emplace();
		check(!ItemTableOf(prof).valid, "nor one whose camera.base is not indexed (no local slot byte)");
		prof.camera->base->is_indexed = true;
		prof.camera->base->index_addr = 0x48BF7D;
		const ItemTable from_profile = ItemTableOf(prof);
		check(from_profile.valid && from_profile.table == 0x3967D0 && from_profile.stride == 60 && from_profile.id_offset == 0x06 &&
				  from_profile.count_offset == 0x24 && from_profile.owner_offset == 0x28 && from_profile.slot_address == 0x48BF7D,
			"feedback.weapon's table and record fields and camera.base's slot byte make the item table");

		// The shipped Outbreak profiles: File #1's table 0x3967D0 and slot byte 0x48BF7D, File #2's 0x397B40 and 0x4912BD.
		const ProfileDB::Profile* file1 = ProfileDB::Lookup("SLPM-65428", 0);
		const ProfileDB::Profile* file2 = ProfileDB::Lookup("SLPM-65692", 0);
		const ItemTable table1 = file1 ? ItemTableOf(*file1) : ItemTable{};
		const ItemTable table2 = file2 ? ItemTableOf(*file2) : ItemTable{};
		check(table1.valid && table1.table == 0x3967D0 && table1.slot_address == 0x48BF7D && table1.stride == 60,
			"File #1's profile names its item table (0x3967D0) and slot byte (0x48BF7D)");
		check(table2.valid && table2.table == 0x397B40 && table2.slot_address == 0x4912BD && table2.stride == 60,
			"File #2's profile names its item table (0x397B40) and slot byte (0x4912BD)");

		if (failures == 0)
			Console.WriteLn(Color_StrongGreen, "(VR) Item names self-test: passed (%d checks: ItemData lookups, labels, record "
											   "checks, the shipped item tables).", checks);
		else
			Console.WriteLn(Color_StrongRed, "(VR) Item names self-test FAIL: %s (%d of %d checks failed).", first, failures, checks);
		return failures == 0;
	}
}
