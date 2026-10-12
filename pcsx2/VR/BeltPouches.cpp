// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/BeltPouches.h"
#include "VR/CameraDriver.h"
#include "VR/GameFeedback.h"
#include "VR/HudCards.h"
#include "VR/MenuPointer.h"
#include "VR/SplitState.h"
#include "VR/VRInput.h"
#include "VR/VRProfileDB.h"
#include "VR/XRCompositor.h"

#include "Input/VRInputSource.h"
#include "Memory.h"
#include "MemoryTypes.h"
#include "VMManager.h"

#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace VR::BeltPouches
{
	namespace
	{
		// The grip, as the Zone controls take it: squeezed past kGripOn, open again below kGripOff.
		constexpr float kGripOn = 0.6f;
		constexpr float kGripOff = 0.4f;
		// Reaching an empty pouch: a short sharp tap, unlike the zones' entry tick (kHapticZoneEnter 0.25 / 20 ms), so a
		// hand can tell a pouch from a hip zone. One holding an item: two firmer beats 90 ms apart.
		constexpr SpatialControls::HapticCue kTickEmpty{0.35f, 0.012f};
		constexpr SpatialControls::HapticCue kTickItem{0.55f, 0.025f};
		constexpr float kDoubleTickGapMs = 90.0f;
		// A squeeze that did something: the zones' grab tick. One that did nothing: a dull buzz.
		constexpr SpatialControls::HapticCue kTickSqueeze{0.5f, 0.03f};
		constexpr SpatialControls::HapticCue kTickRefused{0.15f, 0.12f};
		// An open press is held this long (a 30 fps game sees it); for this long after one, a squeeze only asks for its
		// cell (the screen is still opening), so a second squeeze never presses the menu button again and closes it.
		constexpr float kOpenPressMs = 150.0f;
		constexpr float kOpenSettleMs = 1500.0f;
		// Where a claimed hand is put for the Zone controls: out of every zone's reach.
		constexpr float kFarAway = 1.0e4f;
		// How long the toast naming what a hand reached for stays up (fades included).
		constexpr float kNameToastSeconds = 1.5f;

		// CPU thread only (the input poll).
		State s_state;
		bool s_claimed[2] = {false, false};
		float s_open_hold_ms = 0.0f;
		std::string s_open_id;
		SpatialControls::ZoneState s_head_yaw; // HeadAnchor's yaw, kept while looking straight down
		bool s_announced = false;
		u32 s_announced_crc = 0;
		// Per pouch, the item byte last logged as naming a record that failed the item checks (0: none), so a stale index
		// is logged once, not on every reach.
		std::array<u8, kMaxPouches> s_name_failed{};
		std::atomic<bool> s_selftest_ran{false};

		std::mutex s_telemetry_mutex;
		Telemetry s_telemetry;

		bool DefaultRead(u32 address, u8 width, u32* value)
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

		bool Read(GuestReader reader, u32 address, u8 width, u32* value)
		{
			return (reader ? reader : DefaultRead)(address, width, value);
		}

		// One guard, checked the way VRInputSource::EvaluateGuards checks each of a list.
		bool GuardPasses(const ProfileDB::CameraGuard& g, GuestReader reader)
		{
			u32 address = g.ee_address;
			if (g.is_chain)
			{
				u32 deref = 0;
				if (!Read(reader, g.pointer_addr, 4, &deref) || deref == 0 || deref >= Ps2MemSize::MainRam || (deref & 3u) != 0)
					return false;
				for (const s32 hop : g.deref_offsets)
				{
					const s64 slot = static_cast<s64>(deref) + hop;
					if (slot < 0 || slot + 4 > static_cast<s64>(Ps2MemSize::MainRam))
						return false;
					if (!Read(reader, static_cast<u32>(slot), 4, &deref) || deref == 0 || deref >= Ps2MemSize::MainRam ||
						(deref & 3u) != 0)
						return false;
				}
				const s64 a = static_cast<s64>(deref) + g.offset;
				if (a < 0 || a + g.width > static_cast<s64>(Ps2MemSize::MainRam))
					return false;
				address = static_cast<u32>(a);
			}
			u32 v = 0;
			if (!Read(reader, address, g.width, &v))
				return false;
			return (v == g.equals) != g.not_equals;
		}

		// suspendWhen: any one passing suspends.
		bool AnyGuardPasses(const std::vector<ProfileDB::CameraGuard>& guards, GuestReader reader)
		{
			for (const ProfileDB::CameraGuard& g : guards)
			{
				if (GuardPasses(g, reader))
					return true;
			}
			return false;
		}

		bool HandAllowed(SpatialControls::ZoneHand rule, int hand)
		{
			return rule == SpatialControls::ZoneHand::Either || (rule == SpatialControls::ZoneHand::Left && hand == 0) ||
			       (rule == SpatialControls::ZoneHand::Right && hand == 1);
		}

		bool Finite(const SpatialControls::Vec3& v)
		{
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
		}

		const char* HandName(int hand)
		{
			return (hand == 0) ? "left" : "right";
		}

		const char* ItemName(s8 item)
		{
			return (item == 1) ? "holds an item" : (item == 0) ? "empty" : "contents not read";
		}

		void Pulse(int hand, const SpatialControls::HapticCue& cue)
		{
			XRInput::QueuePulse(hand, cue.amplitude, cue.seconds);
		}

		void MaybeRunSelfTest()
		{
			static const bool enabled = (std::getenv("PCSX2_VR_SELFTEST") != nullptr);
			if (!enabled || s_selftest_ran.exchange(true, std::memory_order_acq_rel))
				return;
			SelfTest();
		}
	}

	BodyFrame SeatFrame(const SpatialControls::Vec3& head_pos, const SpatialControls::Quat& head_q, float yaw)
	{
		BodyFrame f;
		const float n2 = head_q.x * head_q.x + head_q.y * head_q.y + head_q.z * head_q.z + head_q.w * head_q.w;
		if (!std::isfinite(n2) || n2 < 1.0e-6f || !Finite(head_pos) || !std::isfinite(yaw))
			return f;
		const float inv = 1.0f / std::sqrt(n2);
		const SpatialControls::Quat q{head_q.x * inv, head_q.y * inv, head_q.z * inv, head_q.w * inv};
		// OpenXR: +Y up, +Z back. The neck is below and behind the eyes in the head's own frame, so wherever the head
		// turns or tips, this is the same point on the body.
		const SpatialControls::Vec3 neck_in_head{0.0f, -kNeckDown, kNeckBack};
		const SpatialControls::Vec3 neck = head_pos + q.Rotate(neck_in_head);
		// The upright head facing yaw that the profile's placements are measured from, back up from that neck.
		f.origin = neck - SpatialControls::Quat::FromYaw(yaw).Rotate(neck_in_head);
		f.yaw = yaw;
		f.valid = Finite(f.origin);
		return f;
	}

	BodyFrame HeadFrame(const SpatialControls::Vec3& head_pos, float yaw)
	{
		BodyFrame f;
		if (!Finite(head_pos) || !std::isfinite(yaw))
			return f;
		f.origin = head_pos;
		f.yaw = yaw;
		f.valid = true;
		return f;
	}

	SpatialControls::Vec3 PouchCentre(const BodyFrame& f, const ProfileDB::Pouch& pouch)
	{
		SpatialControls::Anchor a;
		a.position = f.origin;
		a.yaw = f.yaw;
		const SpatialControls::Placement at{pouch.side, pouch.height, pouch.forward, 0.0f};
		return SpatialControls::PlaceControl(a, at).pivot;
	}

	int PouchUnder(const ProfileDB::PouchParams& p, const BodyFrame& f, int hand, const SpatialControls::Vec3& pos, int prev)
	{
		if (!f.valid || !Finite(pos) || !HandAllowed(p.hand, hand))
			return -1;
		const int count = std::min(static_cast<int>(p.list.size()), kMaxPouches);
		if (prev >= 0 && prev < count)
		{
			const ProfileDB::Pouch& kept = p.list[static_cast<size_t>(prev)];
			if (SpatialControls::Length(pos - PouchCentre(f, kept)) <= kept.radius + kExitMargin)
				return prev;
		}
		int best = -1;
		float best_d = 0.0f;
		for (int i = 0; i < count; i++)
		{
			const ProfileDB::Pouch& pouch = p.list[static_cast<size_t>(i)];
			const float d = SpatialControls::Length(pos - PouchCentre(f, pouch));
			if (d <= pouch.radius && (best < 0 || d < best_d))
			{
				best = i;
				best_d = d;
			}
		}
		return best;
	}

	StepOut Step(const ProfileDB::PouchParams& p, const StepIn& in, State& st)
	{
		StepOut out;
		st.since_open_ms = std::min(st.since_open_ms + in.dt_ms, 1.0e9f);
		const int count = std::min(static_cast<int>(p.list.size()), kMaxPouches);
		for (int h = 0; h < 2; h++)
		{
			HandState& hs = st.hand[h];
			const HandIn& hi = in.hands[h];
			if (!in.active || !hi.valid || count == 0)
			{
				hs = HandState{};
				continue;
			}

			// The grip, with the zones' hysteresis. A hand's first poll only records it, so a grip already closed as the
			// pouches come on (first person arming again, the hand coming back into tracking) is no squeeze.
			const bool was_squeezed = hs.squeezed;
			const bool squeezed = was_squeezed ? (hi.squeeze > kGripOff) : (hi.squeeze >= kGripOn);
			const bool squeeze_edge = hs.primed && squeezed && !was_squeezed;
			hs.primed = true;
			hs.squeezed = squeezed;
			if (!squeezed)
				hs.owned = false;

			// The second beat of a double tick.
			if (hs.second_tick_ms >= 0.0f)
			{
				hs.second_tick_ms -= in.dt_ms;
				if (hs.second_tick_ms <= 0.0f)
				{
					hs.second_tick_ms = -1.0f;
					out.tick[h] = Tick::Firm;
				}
			}

			const int prev = hs.pouch;
			// Within reach of a zone that takes this hand, the zone has it: the hip gestures never become pouch squeezes.
			const int now = hi.in_zone ? -1 : PouchUnder(p, in.frame, h, hi.position, prev);
			hs.pouch = now;
			out.hover[h] = now;
			out.claimed[h] = (now >= 0);
			hs.since_left_ms = std::min(hs.since_left_ms + in.dt_ms, 1.0e9f);
			if (prev >= 0 && now != prev)
			{
				hs.left_pouch = prev;
				hs.since_left_ms = 0.0f;
			}
			if (now >= 0 && now != prev)
			{
				out.entered[h] = true;
				// Reaching in with an open hand ticks; a closed one passing through (holding a zone's button) does not.
				if (!squeezed)
				{
					if (in.items[static_cast<size_t>(now)] == 1)
					{
						out.reach[h] = Tick::Firm;
						hs.second_tick_ms = kDoubleTickGapMs;
						// And names it, once per reach: not on a quick return to the pouch just left (a hand on its edge),
						// and not over a menu.
						out.name[h] = !in.menu && !(now == hs.left_pouch && hs.since_left_ms < kNameReentryMs);
					}
					else
					{
						out.reach[h] = Tick::Light;
					}
					out.tick[h] = out.reach[h];
				}
			}

			// A squeeze begun in a pouch (a grip closed on the way in does nothing).
			if (squeeze_edge && now >= 0)
			{
				hs.owned = true;
				hs.second_tick_ms = -1.0f; // the squeeze's own tick, not a late beat
				const ProfileDB::Pouch& pouch = p.list[static_cast<size_t>(now)];
				const bool steerable = in.pointer && pouch.cell >= 0 && pouch.cell < in.cells;
				Action action = Action::Refused;
				const char* why = nullptr;
				if (in.suspend)
				{
					why = "an item's command submenu is open";
				}
				else if (in.screen_up)
				{
					if (steerable)
						action = Action::Request;
					else
						why = in.pointer ? "its cell is not one the menu pointer maps" : "point-to-select is off";
				}
				else if (st.since_open_ms < kOpenSettleMs)
				{
					// The item screen is still opening from an earlier squeeze: this one only changes the cell.
					action = steerable ? Action::Request : Action::None;
				}
				else if (!in.menu && in.first_person)
				{
					if (in.can_open)
						action = steerable ? Action::OpenAndRequest : Action::Open;
					else
						why = "nothing in the profile opens the item screen";
				}
				else
				{
					why = in.menu ? "another menu is up" : "first person is not driving the camera";
				}
				if (action == Action::Open || action == Action::OpenAndRequest)
				{
					out.open_press = true;
					st.since_open_ms = 0.0f;
				}
				out.action[h] = action;
				out.action_pouch[h] = now;
				out.refused_why[h] = why;
				out.tick[h] = (action == Action::Refused) ? Tick::Refused : Tick::Grab;
			}
			out.owned[h] = hs.owned || now >= 0;
		}
		return out;
	}

	ZoneBalls PlaceZones(const ProfileDB::Profile& profile, const SpatialControls::Anchor& head)
	{
		ZoneBalls zones;
		const float margin = SpatialControls::ZoneParams{}.exit_margin;
		for (const ProfileDB::SpatialControlSpec& c : profile.controls)
		{
			if (c.kind != SpatialControls::DeviceKind::Zone || zones.count >= kMaxZoneBalls)
				continue;
			// As StepZone places it: the placement's yaw is not used.
			const SpatialControls::Placement at{c.placement.side, c.placement.height, c.placement.forward, 0.0f};
			ZoneBall& b = zones.ball[static_cast<size_t>(zones.count)];
			b.centre = SpatialControls::PlaceControl(head, at).pivot;
			b.reach = c.zone_radius + margin;
			b.hand = c.zone_hand;
			zones.count++;
		}
		return zones;
	}

	bool InOwnZone(const ZoneBalls& zones, int hand, const SpatialControls::Vec3& pos)
	{
		if (!Finite(pos))
			return false;
		for (int i = 0; i < zones.count && i < kMaxZoneBalls; i++)
		{
			const ZoneBall& b = zones.ball[static_cast<size_t>(i)];
			if (HandAllowed(b.hand, hand) && SpatialControls::Length(pos - b.centre) <= b.reach)
				return true;
		}
		return false;
	}

	SpatialControls::Hands HandsOutsidePouches(const SpatialControls::Hands& hands)
	{
		SpatialControls::Hands out = hands;
		for (int h = 0; h < 2; h++)
		{
			if (s_claimed[h])
				out.hand[h].position = {kFarAway, kFarAway, kFarAway};
		}
		return out;
	}

	bool OpenPressFor(std::string_view zone_id)
	{
		return s_open_hold_ms > 0.0f && !zone_id.empty() && zone_id == s_open_id;
	}

	Telemetry GetTelemetry()
	{
		std::lock_guard<std::mutex> lock(s_telemetry_mutex);
		return s_telemetry;
	}

	void Poll(const VRInputSnapshot& snap, GuestReader reader, float dt)
	{
		MaybeRunSelfTest();

		const float dt_ms = std::clamp(std::isfinite(dt) ? dt : 0.0f, 0.0f, 0.25f) * 1000.0f;
		s_open_hold_ms = std::max(0.0f, s_open_hold_ms - dt_ms);

		// Nothing is looked up or read unless first person is armed with the local record resolved (CameraDriver: never
		// online outside private co-op, so never on obsrv) and the VM runs: the pouches cost nothing otherwise.
		const bool live = snap.generation != 0 && snap.actions_active;
		const bool vm_running = VMManager::GetState() == VMState::Running;
		u32 record = 0;
		const ProfileDB::Profile* profile = nullptr;
		if (live && vm_running && !SplitState::Active() && CameraDriver::LocalRecord(&record) && VMManager::HasValidVM())
		{
			const std::string serial = VMManager::GetDiscSerial();
			profile = serial.empty() ? nullptr : ProfileDB::Lookup(serial, VMManager::GetDiscCRC());
			if (profile && (!profile->pouches.has_value() || profile->pouches->list.empty()))
				profile = nullptr;
		}
		const ProfileDB::PouchParams* params = profile ? &profile->pouches.value() : nullptr;
		const int count = params ? std::min(static_cast<int>(params->list.size()), kMaxPouches) : 0;

		StepIn in;
		in.dt_ms = dt_ms;
		in.items.fill(-1);
		std::array<u8, kMaxPouches> item_bytes{};
		SpatialControls::Anchor head;
		if (params)
		{
			// The frame: on the seat (the neck, the recentered forward), or as the zones (the head and its yaw).
			if (SpatialControls::HeadAnchor(snap, s_head_yaw, &head))
			{
				if (params->anchor == ProfileDB::PouchAnchor::Head)
				{
					in.frame = HeadFrame(head.position, head.yaw);
				}
				else
				{
					// The forward the player last recentered to (the screen anchor, re-placed at each recenter); the
					// head's yaw until the compositor has placed one (no headset runtime: the PINE test head).
					const XRCompositor::ScreenAnchor sa = XRCompositor::GetScreenAnchor();
					const float yaw = (sa.generation != 0 && std::isfinite(sa.yaw)) ? sa.yaw : head.yaw;
					const VRPose& hp = snap.head_pose;
					const SpatialControls::Quat q{hp.orientation_xyzw[0], hp.orientation_xyzw[1], hp.orientation_xyzw[2],
						hp.orientation_xyzw[3]};
					in.frame = SeatFrame(head.position, q, yaw);
				}
			}
			in.active = in.frame.valid;
		}
		if (in.active)
		{
			const SpatialControls::Hands hands = SpatialControls::HandsFromSnapshot(snap);
			// The Zone controls where they are this poll (they turn with the head; the seat's pouches do not).
			const ZoneBalls zones = PlaceZones(*profile, head);
			for (int h = 0; h < 2; h++)
			{
				in.hands[h].valid = hands.hand[h].valid;
				in.hands[h].position = hands.hand[h].position;
				in.hands[h].squeeze = hands.hand[h].squeeze;
				in.hands[h].in_zone = InOwnZone(zones, h, hands.hand[h].position);
			}
			const std::optional<ProfileDB::MenuPointerParams>& mp = profile->menu_pointer;
			// The item screen up as the menu pointer last decided it (its gate takes liveWhen as well as when), not
			// re-evaluated here: the cursor struct's when stays set after the screen closes, and the map and pause pass
			// it too, so a squeeze there would leave a request that steers. One poll late, which a squeeze never notices.
			in.screen_up = mp.has_value() && MenuPointer::ItemScreenLive();
			in.menu = SpatialControls::MenuMode();
			in.first_person = CameraDriver::LookAtActive();
			// Only with the item screen up: a submenu flag left set behind a closed screen must not refuse every squeeze.
			in.suspend = in.screen_up && !params->suspend_when.empty() && AnyGuardPasses(params->suspend_when, reader);
			in.pointer = mp.has_value() && HudCards::MenuPointerEnabled();
			in.cells = mp.has_value() ? static_cast<int>(mp->cells.size()) : 0;
			in.can_open = !params->open.empty();
			// The local character's item bytes (0 = an empty cell). Reads only.
			for (int i = 0; i < count; i++)
			{
				const ProfileDB::Pouch& pouch = params->list[static_cast<size_t>(i)];
				u32 byte = 0;
				if (pouch.has_item && Read(reader, record + pouch.item_offset, 1, &byte))
				{
					in.items[static_cast<size_t>(i)] = static_cast<s8>((byte != 0) ? 1 : 0);
					item_bytes[static_cast<size_t>(i)] = static_cast<u8>(byte);
				}
			}

			// Once per game: what the pouches will do.
			const u32 crc = VMManager::GetDiscCRC();
			if (!s_announced || s_announced_crc != crc)
			{
				s_announced = true;
				s_announced_crc = crc;
				std::string does;
				if (!params->open.empty())
					does = "opens the item screen with VR-" + params->open +
					       (mp.has_value() ? " and steers its cursor to the pouch's cell" : " (no menuPointer: no steering)");
				else if (mp.has_value())
					does = "steers the item screen's cursor to the pouch's cell while the screen is up";
				else
					does = "does nothing (no open control and no menuPointer)";
				const bool names = GameFeedback::ItemTableOf(*profile).valid;
				Console.WriteLn("(VR) Pouches: %d pouch(es) on the %s; reaching into one ticks (twice for an item%s), a squeeze %s.",
					count, params->anchor == ProfileDB::PouchAnchor::Head ? "head (as the zones)" : "seat (neck, recentered forward)",
					names ? ", naming it" : "", does.c_str());
			}
		}

		static const ProfileDB::PouchParams s_no_params{};
		const StepOut out = Step(params ? *params : s_no_params, in, s_state);

		// The Zone controls stop seeing a hand in a pouch; the menu pointer stops counting a pouch hand's grip.
		for (int h = 0; h < 2; h++)
			s_claimed[h] = out.claimed[h];
		MenuPointer::SetPouchGrips(out.owned[0], out.owned[1]);
		if (!in.active)
			s_open_hold_ms = 0.0f;

		// Name what an open hand reached for (Step decides when: once per reach into a pouch holding an item). The pouch's
		// item byte is a record index into the item table feedback.weapon names, and the record must pass the weapon
		// reader's checks (present, naming itself, the local player's) before its name is shown. Reads only.
		if (params && (out.name[0] || out.name[1]))
		{
			const GameFeedback::ItemTable item_table = GameFeedback::ItemTableOf(*profile);
			for (int h = 0; h < 2; h++)
			{
				const int k = out.hover[h];
				if (!out.name[h] || k < 0 || k >= count || !item_table.valid)
					continue;
				const u8 cell_byte = item_bytes[static_cast<size_t>(k)];
				const GameFeedback::ItemRecord rec =
					GameFeedback::ReadItemRecord(item_table, cell_byte, reader ? reader : DefaultRead);
				if (rec.valid)
				{
					HudCards::Toast(GameFeedback::ItemLabel(rec.item_id, rec.count), kNameToastSeconds);
				}
				else if (s_name_failed[static_cast<size_t>(k)] != cell_byte)
				{
					s_name_failed[static_cast<size_t>(k)] = cell_byte;
					Console.WriteLn("(VR) Pouches: pouch %d's item byte names record %u, which fails the item checks (present "
									"%u, own index %u, owner %u, local slot %d); its name is not shown.", k,
						static_cast<unsigned>(cell_byte), rec.present, rec.own_index, rec.owner,
						rec.slot_known ? static_cast<int>(rec.slot) : -1);
				}
			}
		}

		u32 enters = 0;
		u32 item_ticks = 0;
		u32 squeezes = 0;
		u32 requests = 0;
		u32 refused = 0;
		u32 opens_sent = 0;
		for (int h = 0; h < 2; h++)
		{
			switch (out.tick[h])
			{
				case Tick::Light: Pulse(h, kTickEmpty); break;
				case Tick::Firm: Pulse(h, kTickItem); break;
				case Tick::Grab: Pulse(h, kTickSqueeze); break;
				case Tick::Refused: Pulse(h, kTickRefused); break;
				default: break;
			}
			enters += (out.reach[h] != Tick::None) ? 1u : 0u;
			item_ticks += (out.reach[h] == Tick::Firm) ? 1u : 0u;

			const int k = out.action_pouch[h];
			if (k < 0 || !params || k >= count)
				continue;
			squeezes++;
			const ProfileDB::Pouch& pouch = params->list[static_cast<size_t>(k)];
			const s8 item = in.items[static_cast<size_t>(k)];
			switch (out.action[h])
			{
				case Action::Request:
					MenuPointer::RequestCell(pouch.cell, h);
					requests++;
					Console.WriteLn("(VR) Pouches: pouch %d (cell %d, %s) squeezed with the %s hand; the item cursor goes "
									"there.", k, pouch.cell, ItemName(item), HandName(h));
					break;
				case Action::OpenAndRequest:
					MenuPointer::RequestCell(pouch.cell, h);
					requests++;
					Console.WriteLn("(VR) Pouches: pouch %d (cell %d, %s) squeezed with the %s hand; opening the item screen "
									"with VR-%s, then the cursor goes to the cell.", k, pouch.cell, ItemName(item), HandName(h),
						params->open.c_str());
					break;
				case Action::Open:
					Console.WriteLn("(VR) Pouches: pouch %d (cell %d, %s) squeezed with the %s hand; opening the item screen "
									"with VR-%s (point-to-select cannot steer to that cell).", k, pouch.cell, ItemName(item),
						HandName(h), params->open.c_str());
					break;
				case Action::None:
					Console.WriteLn("(VR) Pouches: pouch %d squeezed with the %s hand while the item screen opens.", k, HandName(h));
					break;
				case Action::Refused:
				default:
					refused++;
					Console.WriteLn("(VR) Pouches: pouch %d squeezed with the %s hand; nothing done (%s).", k, HandName(h),
						out.refused_why[h] ? out.refused_why[h] : "refused");
					break;
			}
		}
		if (out.open_press && params)
		{
			s_open_id = params->open;
			s_open_hold_ms = kOpenPressMs;
			opens_sent++;
		}

		std::lock_guard<std::mutex> lock(s_telemetry_mutex);
		Telemetry& t = s_telemetry;
		t.active = in.active;
		t.screen_up = in.screen_up;
		t.menu_mode = in.menu;
		t.head_anchor = params && params->anchor == ProfileDB::PouchAnchor::Head;
		t.open_pressing = s_open_hold_ms > 0.0f;
		t.suspended = in.suspend;
		t.count = static_cast<u8>(in.active ? count : 0);
		for (int h = 0; h < 2; h++)
		{
			t.hover[static_cast<size_t>(h)] = static_cast<s8>(out.hover[h]);
			t.owned[static_cast<size_t>(h)] = out.owned[h];
		}
		for (int i = 0; i < kMaxPouches; i++)
		{
			Telemetry::PouchView& v = t.pouches[static_cast<size_t>(i)];
			v = Telemetry::PouchView{};
			if (!in.active || i >= count)
				continue;
			const ProfileDB::Pouch& pouch = params->list[static_cast<size_t>(i)];
			const SpatialControls::Vec3 c = PouchCentre(in.frame, pouch);
			v.centre[0] = c.x;
			v.centre[1] = c.y;
			v.centre[2] = c.z;
			v.radius = pouch.radius;
			v.cell = static_cast<s8>(pouch.cell);
			v.item = in.items[static_cast<size_t>(i)];
			v.item_byte = item_bytes[static_cast<size_t>(i)];
		}
		t.enters += enters;
		t.item_ticks += item_ticks;
		t.squeezes += squeezes;
		t.opens += opens_sent;
		t.requests += requests;
		t.refused += refused;
	}

	bool SelfTest()
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
		using SpatialControls::Quat;
		using SpatialControls::Vec3;
		const auto dist = [](const Vec3& a, const Vec3& b) { return SpatialControls::Length(a - b); };

		// The shipped File #1 layout (bin/resources/vr-profiles/SLPM-65428.yaml): a 2 x 2 chest rig laid out like the
		// item grid, and the equipped box in front of the belt buckle (the profile itself is checked further down).
		ProfileDB::PouchParams rig;
		rig.open = "belt";
		const auto add = [&rig](int cell, float side, float height, float forward, bool item, u32 offset) {
			ProfileDB::Pouch pouch;
			pouch.cell = cell;
			pouch.side = side;
			pouch.height = height;
			pouch.forward = forward;
			pouch.has_item = item;
			pouch.item_offset = offset;
			rig.list.push_back(pouch);
		};
		add(0, -0.10f, -0.20f, 0.14f, true, 0xC84);
		add(1, 0.10f, -0.20f, 0.14f, true, 0xC85);
		add(2, -0.07f, -0.29f, 0.24f, true, 0xC86);
		add(3, 0.07f, -0.29f, 0.24f, true, 0xC87);
		add(4, 0.0f, -0.40f, 0.28f, false, 0);

		// --- Frames -------------------------------------------------------------------------------------------------
		const Vec3 eyes{0.1f, 1.2f, -0.3f};
		const float rest_yaw = 0.3f;
		const Quat upright = Quat::FromYaw(rest_yaw);
		const BodyFrame seat_up = SeatFrame(eyes, upright, rest_yaw);
		const BodyFrame head_up = HeadFrame(eyes, rest_yaw);
		check(seat_up.valid && head_up.valid && dist(PouchCentre(seat_up, rig.list[3]), PouchCentre(head_up, rig.list[3])) < 1.0e-4f,
			"upright and facing the recentered forward, the seat places a pouch where the head anchor does");
		{
			// The pouch is where the profile says, turned with the yaw: zone-style placement from the head.
			const Vec3 c = PouchCentre(head_up, rig.list[1]);
			const Vec3 want = eyes + Quat::FromYaw(rest_yaw).Rotate(Vec3{0.10f, -0.20f, -0.14f});
			check(dist(c, want) < 1.0e-4f, "a pouch sits at its side, height and forward from the head, turned with the yaw");
		}
		const Vec3 neck_in_head{0.0f, -kNeckDown, kNeckBack};
		const Vec3 neck = eyes + upright.Rotate(neck_in_head);
		{
			// Looking 45 deg down swings the eyes round the neck (forward and down): the seat keeps every pouch still,
			// while a head-anchored pouch drops away from the hand reaching for it.
			const float half = -0.5f * 0.7853982f;
			const Quat down = upright * Quat{std::sin(half), 0.0f, 0.0f, std::cos(half)};
			const Vec3 eyes_down = neck - down.Rotate(neck_in_head);
			check(dist(eyes_down, eyes) > 0.05f, "looking down moves the eyes several centimetres");
			const BodyFrame seat_down = SeatFrame(eyes_down, down, rest_yaw);
			bool still = seat_down.valid;
			for (const ProfileDB::Pouch& pouch : rig.list)
				still = still && dist(PouchCentre(seat_down, pouch), PouchCentre(seat_up, pouch)) < 1.0e-4f;
			check(still, "looking down at the pouches moves none of them on the seat");
			check(dist(PouchCentre(HeadFrame(eyes_down, rest_yaw), rig.list[2]), PouchCentre(head_up, rig.list[2])) > 0.05f,
				"while on the head anchor a pouch moves more than 5 cm");
		}
		{
			// Turning the head 40 deg to glance at a pouch: the recentered forward stays, so the pouches stay.
			const Quat turned = Quat::FromYaw(rest_yaw + 0.6981317f);
			const Vec3 eyes_turned = neck - turned.Rotate(neck_in_head);
			const BodyFrame seat_turned = SeatFrame(eyes_turned, turned, rest_yaw);
			check(seat_turned.valid && dist(PouchCentre(seat_turned, rig.list[0]), PouchCentre(seat_up, rig.list[0])) < 1.0e-4f,
				"turning the head moves no pouch on the seat");
			check(dist(PouchCentre(HeadFrame(eyes_turned, rest_yaw + 0.6981317f), rig.list[0]), PouchCentre(head_up, rig.list[0])) > 0.08f,
				"while on the head anchor the pouch swings away");
		}
		check(!SeatFrame(eyes, Quat{0.0f, 0.0f, 0.0f, 0.0f}, rest_yaw).valid &&
				  !HeadFrame(Vec3{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}, 0.0f).valid,
			"no frame from a bad head pose");

		// --- Hit tests ----------------------------------------------------------------------------------------------
		// From the origin facing -Z: a pouch's centre is (side, height, -forward).
		const BodyFrame plain = HeadFrame(Vec3{0.0f, 0.0f, 0.0f}, 0.0f);
		const auto at = [&](int i, float dx, float dy, float dz) {
			const Vec3 c = PouchCentre(plain, rig.list[static_cast<size_t>(i)]);
			return Vec3{c.x + dx, c.y + dy, c.z + dz};
		};
		check(dist(at(3, 0.0f, 0.0f, 0.0f), Vec3{0.07f, -0.29f, -0.24f}) < 1.0e-5f, "pouch 3 sits at (0.07, -0.29, -0.24)");
		check(PouchUnder(rig, plain, 1, at(3, 0.0f, 0.0f, 0.0f), -1) == 3, "a hand at a pouch's centre is in it");
		check(PouchUnder(rig, plain, 1, at(3, 0.06f, 0.0f, 0.0f), -1) == 3 && PouchUnder(rig, plain, 1, at(3, 0.07f, 0.0f, 0.0f), -1) == -1,
			"a pouch is entered only inside its 6.5 cm radius");
		check(PouchUnder(rig, plain, 1, at(3, 0.08f, 0.0f, 0.0f), 3) == 3 && PouchUnder(rig, plain, 1, at(3, 0.09f, 0.0f, 0.0f), 3) == -1,
			"and kept out to 2 cm past it");
		check(PouchUnder(rig, plain, 0, at(0, 0.0f, 0.03f, 0.0f), -1) == 0 && PouchUnder(rig, plain, 0, at(0, 0.0f, -0.07f, 0.0f), -1) == -1,
			"between two pouches a hand is in neither");
		{
			ProfileDB::PouchParams two = rig;
			two.list[1].side = -0.02f; // overlapping pouch 0, 8 cm away
			check(PouchUnder(two, plain, 0, at(0, 0.05f, 0.0f, 0.0f), -1) == 1 && PouchUnder(two, plain, 0, at(0, 0.03f, 0.0f, 0.0f), -1) == 0,
				"in two overlapping pouches a hand belongs to the nearer one");
			two.hand = SpatialControls::ZoneHand::Left;
			check(PouchUnder(two, plain, 1, at(0, 0.0f, 0.0f, 0.0f), -1) == -1 && PouchUnder(two, plain, 0, at(0, 0.0f, 0.0f, 0.0f), -1) == 0,
				"hand: left leaves the right hand out");
		}
		{
			bool apart = true;
			for (size_t i = 0; i < rig.list.size(); i++)
			{
				for (size_t j = i + 1; j < rig.list.size(); j++)
					apart = apart && dist(at(static_cast<int>(i), 0, 0, 0), at(static_cast<int>(j), 0, 0, 0)) >=
					                     rig.list[i].radius + rig.list[j].radius;
			}
			check(apart, "no two of the rig's pouches overlap");
		}

		// --- The zones come first ----------------------------------------------------------------------------------
		// A Step input with both hands valid and open, away from everything, for the zone checks.
		const auto base_for_zones = [](const ProfileDB::PouchParams& pp, const BodyFrame& frame) {
			StepIn z;
			z.active = true;
			z.frame = frame;
			z.first_person = true;
			z.pointer = true;
			z.cells = 5;
			z.can_open = !pp.open.empty();
			z.items.fill(-1);
			for (HandIn& hand_in : z.hands)
			{
				hand_in.valid = true;
				hand_in.position = Vec3{0.5f, 0.3f, -0.6f};
			}
			return z;
		};
		// A holster-like zone (right hand, the hip holster's ball with its exit margin) reaching into pouch 3's ball: a
		// right hand there is the holster's, a left hand there is the pouch's.
		{
			ZoneBalls zones;
			zones.count = 1;
			zones.ball[0].centre = Vec3{0.22f, -0.50f, -0.12f};
			zones.ball[0].reach = 0.23f;
			zones.ball[0].hand = SpatialControls::ZoneHand::Right;
			const Vec3 c3 = at(3, 0.0f, 0.0f, 0.0f);
			const Vec3 to_holster = zones.ball[0].centre - c3;
			const Vec3 edge = c3 + to_holster * (0.06f / SpatialControls::Length(to_holster));
			check(!InOwnZone(zones, 1, c3) && InOwnZone(zones, 1, edge) && !InOwnZone(zones, 0, edge) &&
					  PouchUnder(rig, plain, 1, edge, -1) == 3,
				"pouch 3's ball reaches into the holster's: its centre is clear, its edge toward the holster is not");
			State st;
			StepIn z_in = base_for_zones(rig, plain);
			z_in.hands[1].position = edge;
			z_in.hands[1].in_zone = InOwnZone(zones, 1, edge);
			z_in.hands[0].position = edge;
			z_in.hands[0].in_zone = InOwnZone(zones, 0, edge);
			StepOut o = Step(rig, z_in, st);
			check(o.hover[1] < 0 && !o.claimed[1] && o.tick[1] == Tick::None && o.hover[0] == 3 && o.claimed[0],
				"there the right hand is the holster's (no pouch, no tick) and the left hand is the pouch's");
			z_in.hands[1].squeeze = 1.0f;
			o = Step(rig, z_in, st);
			check(o.action_pouch[1] < 0 && !o.open_press && !o.owned[1], "and a right squeeze there is the holster's alone");
			// The right hand moving to the pouch's centre (out of the holster's reach) is in the pouch again.
			z_in.hands[1].squeeze = 0.0f;
			z_in.hands[1].position = c3;
			z_in.hands[1].in_zone = InOwnZone(zones, 1, c3);
			o = Step(rig, z_in, st);
			check(o.hover[1] == 3 && o.claimed[1], "at the pouch's centre the right hand is in it");
			// Kept in the pouch by its exit margin, a hand reaching back into the holster's reach is the holster's.
			z_in.hands[1].position = edge;
			z_in.hands[1].in_zone = InOwnZone(zones, 1, edge);
			o = Step(rig, z_in, st);
			check(o.hover[1] < 0 && !o.claimed[1], "a hand in the pouch is let go as it comes within the holster's reach");
		}
		check(kTickEmpty.amplitude != SpatialControls::kHapticZoneEnter.amplitude &&
				  kTickEmpty.seconds != SpatialControls::kHapticZoneEnter.seconds,
			"an empty pouch's tick is not the zones' entry tick");

		// The shipped profiles' pouches against their own Zone controls, as the profiles load: the real spheres (each zone's
		// radius plus the zones' exit margin, the farthest a hand can be and still be inside it), head upright, where the
		// seat and head anchors agree. Every pouch keeps a core of kMinCore round its point that no zone taking the same
		// hand reaches, so either hand can use it; where a pouch's ball (with its exit margin) does reach into such a zone,
		// a hand there is the zone's, facing ahead and with the head turned 40 deg either way (the zones turn with it).
		{
			constexpr float kMinCore = 0.05f;
			static constexpr const char* kShipped[] = {"SLPM-65428", "SLPM-65692"};
			const float yaws[] = {0.0f, 0.6981317f, -0.6981317f};
			for (const char* serial : kShipped)
			{
				const ProfileDB::Profile* shipped = ProfileDB::Lookup(serial, 0);
				const bool has = shipped && shipped->pouches.has_value() && !shipped->pouches->list.empty();
				check(has, "the shipped profiles load with their pouches");
				if (!has)
					continue;
				const ProfileDB::PouchParams& pp = shipped->pouches.value();
				// Every pouch reads its box's item byte (the buckle's: the special item's), and the profile names the
				// item table those bytes index, so each can be named.
				bool every_item_read = true;
				for (const ProfileDB::Pouch& pouch : pp.list)
					every_item_read = every_item_read && pouch.has_item;
				check(every_item_read && GameFeedback::ItemTableOf(*shipped).valid,
					"every shipped pouch reads its item byte, and the profile names the item table it indexes");
				const SpatialControls::Anchor ahead{};
				const ZoneBalls ahead_zones = PlaceZones(*shipped, ahead);
				check(ahead_zones.count >= 3, "the shipped profiles' holster, belt and backpack zones are placed");
				bool core = true;
				bool reachable = true;
				bool zone_wins = true;
				bool pouches_apart = true;
				for (size_t i = 0; i < pp.list.size(); i++)
				{
					const ProfileDB::Pouch& pouch = pp.list[i];
					const Vec3 c = PouchCentre(plain, pouch);
					for (size_t j = i + 1; j < pp.list.size(); j++)
						pouches_apart = pouches_apart && dist(c, PouchCentre(plain, pp.list[j])) >= pouch.radius + pp.list[j].radius;
					for (int hh = 0; hh < 2; hh++)
					{
						if (!HandAllowed(pp.hand, hh))
							continue;
						reachable = reachable && !InOwnZone(ahead_zones, hh, c) &&
						            PouchUnder(pp, plain, hh, c, -1) == static_cast<int>(i);
						for (int z = 0; z < ahead_zones.count; z++)
						{
							const ZoneBall& b = ahead_zones.ball[static_cast<size_t>(z)];
							if (HandAllowed(b.hand, hh))
								core = core && SpatialControls::Length(b.centre - c) >= b.reach + kMinCore;
						}
						for (const float yaw : yaws)
						{
							SpatialControls::Anchor turned{};
							turned.yaw = yaw;
							const ZoneBalls zones = PlaceZones(*shipped, turned);
							for (int z = 0; z < zones.count; z++)
							{
								const ZoneBall& b = zones.ball[static_cast<size_t>(z)];
								const Vec3 to_zone = b.centre - c;
								const float d = SpatialControls::Length(to_zone);
								const float keep = pouch.radius + kExitMargin;
								if (!HandAllowed(b.hand, hh) || d < 1.0e-4f || d - b.reach >= keep)
									continue;
								// The point toward the zone inside both balls, with the hand kept in the pouch (prev) there.
								const float t = 0.5f * (std::max(d - b.reach, 0.0f) + keep);
								const Vec3 pt = c + to_zone * (t / d);
								State zs;
								zs.hand[static_cast<size_t>(hh)].pouch = static_cast<int>(i);
								zs.hand[static_cast<size_t>(hh)].primed = true;
								StepIn zi = base_for_zones(pp, plain);
								zi.hands[static_cast<size_t>(hh)].position = pt;
								zi.hands[static_cast<size_t>(hh)].in_zone = InOwnZone(zones, hh, pt);
								zi.hands[static_cast<size_t>(1 - hh)].valid = false;
								const StepOut zo = Step(pp, zi, zs);
								zone_wins = zone_wins && zi.hands[static_cast<size_t>(hh)].in_zone && zo.hover[static_cast<size_t>(hh)] < 0 &&
								            !zo.claimed[static_cast<size_t>(hh)];
							}
						}
					}
				}
				check(pouches_apart, "no two of a shipped profile's pouches overlap");
				check(reachable, "every shipped pouch's centre is in that pouch for either hand, no zone reaching it");
				check(core, "every shipped pouch keeps 5 cm round its point clear of every zone that takes the same hand");
				check(zone_wins, "where a shipped pouch reaches into a zone taking the same hand, a hand there is the zone's, "
								 "facing ahead or turned 40 deg");
			}
		}

		// --- Squeezes and ticks -------------------------------------------------------------------------------------
		const Vec3 away{0.5f, 0.3f, -0.6f};
		StepIn base;
		base.active = true;
		base.frame = plain;
		base.first_person = true;
		base.pointer = true;
		base.cells = 5;
		base.can_open = true;
		base.items.fill(-1);
		base.items[0] = 1;
		base.items[1] = 0;
		base.items[2] = 0;
		base.items[3] = 1;
		base.hands[0].valid = true;
		base.hands[1].valid = true;
		base.hands[0].position = away;
		base.hands[1].position = away;
		const auto place = [&](StepIn& s_in, int hand, int pouch, float squeeze) {
			s_in.hands[static_cast<size_t>(hand)].position = (pouch >= 0) ? at(pouch, 0.0f, 0.0f, 0.0f) : away;
			s_in.hands[static_cast<size_t>(hand)].squeeze = squeeze;
		};
		{
			State st;
			StepIn s_in = base;
			StepOut o = Step(rig, s_in, st);
			check(o.hover[0] < 0 && o.tick[0] == Tick::None && !o.open_press, "hands away from the pouches: nothing");
			place(s_in, 0, 1, 0.0f);
			o = Step(rig, s_in, st);
			check(o.entered[0] && o.hover[0] == 1 && o.tick[0] == Tick::Light && o.claimed[0] && o.owned[0],
				"an open hand reaching an empty pouch: one light tick, and the zones no longer see it");
			o = Step(rig, s_in, st);
			check(!o.entered[0] && o.tick[0] == Tick::None, "staying in it ticks no more");
			place(s_in, 0, -1, 0.0f);
			o = Step(rig, s_in, st);
			check(o.hover[0] < 0 && !o.claimed[0] && !o.owned[0], "out again: released");
			place(s_in, 0, 0, 0.0f);
			o = Step(rig, s_in, st);
			check(o.tick[0] == Tick::Firm && o.reach[0] == Tick::Firm, "a pouch whose cell holds an item: a firm tick");
			int beats = 0;
			int polls = 0;
			for (int i = 0; i < 12; i++)
			{
				o = Step(rig, s_in, st);
				if (o.tick[0] == Tick::Firm && beats == 0)
					polls = i + 1;
				beats += (o.tick[0] == Tick::Firm) ? 1 : 0;
			}
			check(beats == 1 && polls >= 5 && polls <= 6, "and a second one about 90 ms later");

			// Squeezed there with the item screen closed: open it, then ask for the cell.
			place(s_in, 0, 0, 1.0f);
			o = Step(rig, s_in, st);
			check(o.action[0] == Action::OpenAndRequest && o.action_pouch[0] == 0 && o.open_press && o.tick[0] == Tick::Grab,
				"a squeeze with the item screen closed opens it and asks for the pouch's cell");
			place(s_in, 0, -1, 1.0f);
			o = Step(rig, s_in, st);
			check(o.owned[0] && !o.claimed[0] && o.action[0] == Action::None && !o.open_press,
				"the closed grip leaving the pouch stays the pouch's, and does nothing more");
			place(s_in, 0, 3, 1.0f);
			o = Step(rig, s_in, st);
			check(o.entered[0] && o.tick[0] == Tick::None && o.action_pouch[0] < 0, "a closed grip passing into a pouch does nothing");
			place(s_in, 0, -1, 0.0f);
			o = Step(rig, s_in, st);
			check(!o.owned[0], "the grip opening lets it go");

			// Within 1.5 s of the open press the screen is still opening: a squeeze only asks for its cell.
			place(s_in, 1, 3, 0.0f);
			Step(rig, s_in, st);
			place(s_in, 1, 3, 1.0f);
			o = Step(rig, s_in, st);
			check(o.action[1] == Action::Request && !o.open_press, "a squeeze while the screen opens asks for its cell, no second open");
			place(s_in, 1, -1, 0.0f);
			for (int i = 0; i < 100; i++)
				Step(rig, s_in, st);

			// The screen up: a squeeze re-targets the cursor.
			s_in.screen_up = true;
			s_in.menu = true;
			s_in.first_person = false;
			place(s_in, 1, 2, 0.0f);
			Step(rig, s_in, st);
			place(s_in, 1, 2, 1.0f);
			o = Step(rig, s_in, st);
			check(o.action[1] == Action::Request && o.action_pouch[1] == 2 && !o.open_press && o.tick[1] == Tick::Grab,
				"with the item screen up a squeeze asks for the cell and presses nothing");
			place(s_in, 1, 2, 0.0f);
			Step(rig, s_in, st);

			// A command submenu open: refused.
			s_in.suspend = true;
			place(s_in, 1, 2, 1.0f);
			o = Step(rig, s_in, st);
			check(o.action[1] == Action::Refused && o.tick[1] == Tick::Refused && o.refused_why[1] != nullptr,
				"with a command submenu open a squeeze is refused");
			s_in.suspend = false;
			place(s_in, 1, 2, 0.0f);
			Step(rig, s_in, st);

			// Point-to-select off: nothing to steer with.
			s_in.pointer = false;
			place(s_in, 1, 2, 1.0f);
			o = Step(rig, s_in, st);
			check(o.action[1] == Action::Refused, "with point-to-select off a squeeze on the open screen is refused");
			s_in.pointer = true;
			place(s_in, 1, 2, 0.0f);
			Step(rig, s_in, st);

			// The map or pause up (a menu, not the item screen): refused, nothing pressed.
			s_in.screen_up = false;
			place(s_in, 1, 2, 1.0f);
			o = Step(rig, s_in, st);
			check(o.action[1] == Action::Refused && !o.open_press, "with another menu up a squeeze is refused and opens nothing");
			place(s_in, 1, 2, 0.0f);
			Step(rig, s_in, st);
		}
		{
			// Names: an open hand reaching a pouch whose cell holds an item names it, once per reach; an empty pouch, a
			// closed grip, a quick return to the pouch just left (a hand on its edge) and a menu name nothing.
			State st;
			StepIn s_in = base;
			Step(rig, s_in, st);
			place(s_in, 0, 0, 0.0f);
			StepOut o = Step(rig, s_in, st);
			check(o.name[0] && o.reach[0] == Tick::Firm, "an open hand reaching a pouch with an item names it");
			o = Step(rig, s_in, st);
			check(!o.name[0], "staying in it names it no more");
			place(s_in, 0, -1, 0.0f);
			Step(rig, s_in, st);
			place(s_in, 0, 0, 0.0f);
			o = Step(rig, s_in, st);
			check(o.entered[0] && o.reach[0] == Tick::Firm && !o.name[0], "back in the pouch it left a poll ago: a tick, no name");
			place(s_in, 0, -1, 0.0f);
			for (int i = 0; i < 31; i++) // 31 polls of 16.7 ms: past kNameReentryMs
				Step(rig, s_in, st);
			place(s_in, 0, 0, 0.0f);
			o = Step(rig, s_in, st);
			check(o.name[0], "back in it after 0.5 s: named again");
			place(s_in, 0, 3, 0.0f);
			o = Step(rig, s_in, st);
			check(o.hover[0] == 3 && o.name[0], "straight on into another pouch with an item: named");
			place(s_in, 0, 1, 0.0f);
			o = Step(rig, s_in, st);
			check(o.hover[0] == 1 && o.reach[0] == Tick::Light && !o.name[0], "an empty pouch: the light tick, no name");
			place(s_in, 1, -1, 1.0f);
			Step(rig, s_in, st);
			place(s_in, 1, 0, 1.0f);
			o = Step(rig, s_in, st);
			check(o.entered[1] && o.reach[1] == Tick::None && !o.name[1], "a closed grip passing into a pouch names nothing");
			State menu_st;
			StepIn m_in = base;
			m_in.menu = true;
			m_in.screen_up = true;
			Step(rig, m_in, menu_st);
			place(m_in, 0, 3, 0.0f);
			o = Step(rig, m_in, menu_st);
			check(o.reach[0] == Tick::Firm && !o.name[0], "with a menu up the tick still comes, the name does not");
		}
		{
			// Closed, but the cell is not one point-to-select maps (or nothing opens the screen).
			State st;
			StepIn s_in = base;
			s_in.cells = 4;
			place(s_in, 0, 4, 0.0f);
			Step(rig, s_in, st);
			place(s_in, 0, 4, 1.0f);
			StepOut o = Step(rig, s_in, st);
			check(o.action[0] == Action::Open && o.open_press, "a pouch whose cell is not mapped still opens the item screen");
			State st2;
			StepIn shut = base;
			shut.can_open = false;
			place(shut, 0, 2, 0.0f);
			Step(rig, shut, st2);
			place(shut, 0, 2, 1.0f);
			o = Step(rig, shut, st2);
			check(o.action[0] == Action::Refused && !o.open_press, "with nothing to open the item screen a squeeze is refused");
		}
		{
			// A hand that appears (or first person that arms) with the grip already closed in a pouch: no squeeze.
			State st;
			StepIn s_in = base;
			place(s_in, 0, 1, 1.0f);
			StepOut o = Step(rig, s_in, st);
			check(o.action_pouch[0] < 0 && !o.open_press && o.hover[0] == 1, "a grip already closed as the hand appears is no squeeze");
			// Both hands squeezing pouches in one poll with the screen closed: one open press.
			State both;
			StepIn b_in = base;
			place(b_in, 0, 0, 0.0f);
			place(b_in, 1, 3, 0.0f);
			Step(rig, b_in, both);
			place(b_in, 0, 0, 1.0f);
			place(b_in, 1, 3, 1.0f);
			o = Step(rig, b_in, both);
			check(o.action[0] == Action::OpenAndRequest && o.action[1] == Action::Request && o.open_press,
				"two squeezes in one poll press the menu button once");
			// The pouches going inactive let every hand go at once.
			b_in.active = false;
			o = Step(rig, b_in, both);
			check(o.hover[0] < 0 && o.hover[1] < 0 && !o.owned[0] && !o.owned[1] && !o.claimed[0] && !o.claimed[1],
				"inactive, no hand is in a pouch or owned by one");
		}

		// The item reader the names come from (it logs its own line, naming its first failed check).
		check(GameFeedback::ItemSelfTest(), "the item names self-test passes");

		if (failures == 0)
			Console.WriteLn(Color_StrongGreen, "(VR) BeltPouches self-test: passed (%d checks: seat and head frames, looking "
											   "down, glancing, hit tests, zones first, the shipped layouts against their zones, ticks, "
											   "names, squeezes, opening).", checks);
		else
			Console.WriteLn(Color_StrongRed, "(VR) BeltPouches self-test FAIL: %s (%d of %d checks failed).", first, failures, checks);
		return failures == 0;
	}
}
