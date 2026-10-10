// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/GameFeedback.h"
#include "VR/CameraDriver.h"
#include "VR/SpatialControls.h"
#include "VR/VRInput.h"
#include "VR/VRProfileDB.h"
#include "VR/XRSession.h"

#include "Config.h"
#include "Memory.h"
#include "VMManager.h"

#include "common/Console.h"

#include <algorithm>
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

		// The cues use the camera's guards and its record: no cues on the title screen, in cutscenes or
		// with first person off. Each time they stop, the next reads are a fresh baseline, not a hit.
		u32 record = 0;
		if (!fb || (!fb->has_hurt && !fb->has_fire) || VMManager::GetState() != VMState::Running ||
			!CameraDriver::LocalRecord(&record))
		{
			s_cues = CueState{};
			return;
		}
		// Another record (a different character slot) is a new baseline too.
		if (s_cues.record_valid && s_cues.record != record)
			s_cues = CueState{};
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
		if (fb->has_fire)
		{
			s.has_counter = ReadAt(fb->fire_address, fb->fire_width, &s.counter);
			s.counter_mask = WidthMask(fb->fire_width);
			s.fire_hand = (fb->fire_hand >= 0) ? fb->fire_hand : AimHand(*profile);
		}
		Step(s, s_cues);
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
