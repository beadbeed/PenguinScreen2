// SPDX-FileCopyrightText: 2026 PenguinScreen2 contributors
// SPDX-License-Identifier: GPL-3.0

#include "VR/ItemReach.h"

#include "VR/CameraDriver.h"
#include "VR/HeadPose.h"
#include "VR/HudCards.h"
#include "VR/ItemData.h"
#include "VR/VRInput.h"
#include "VR/VRProfileDB.h"

#include "Config.h"
#include "Memory.h"
#include "MemoryTypes.h"
#include "VMManager.h"

#include "common/Console.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>

namespace VR::ItemReach
{
	namespace
	{
		constexpr double kPi = 3.14159265358979323846;

		// The cue. A light tick in the right hand (the A button, which presses Circle, is under that thumb) and a toast for
		// a moment: it comes up once per approach and is meant to be glanced at, not read.
		constexpr int kRightHand = 1;
		constexpr float kTickAmp = 0.25f;
		constexpr float kTickSeconds = 0.020f;
		constexpr float kToastSeconds = 1.4f;
		// The button the toast names. The HUD font has no brackets, so parentheses.
		constexpr const char* kToastSuffix = "  (A)";

		// Once per approach: after an item has been announced it is not announced again while it stays in reach, and it is
		// announced again only after it has been out of reach for kRearmMs (the search flickers at a quad's edge, and a
		// step back and forth is one approach). Two different items one after the other are two cues, but never closer
		// than kMinCueGapMs, so a pair of items side by side cannot make the toast stutter.
		constexpr u64 kRearmMs = 500;
		constexpr u64 kMinCueGapMs = 600;

		// One search in so many vsyncs: 30 times a second at 60 Hz, a step of a few centimetres at a walk.
		constexpr u32 kEvalEveryVsyncs = 2;

		// The most nodes the walk will ever be asked for, whatever a profile says.
		constexpr u32 kHardNodeCap = 4096;

		// --- Reading the game ---

		bool ReadGuest(u32 address, u8 width, u32* value)
		{
			if (static_cast<u64>(address) + width > static_cast<u64>(Ps2MemSize::MainRam) ||
				(address & (static_cast<u32>(width) - 1u)) != 0)
				return false;
			switch (width)
			{
				case 1: *value = memRead8(address); break;
				case 2: *value = memRead16(address); break;
				default: *value = memRead32(address); break;
			}
			return true;
		}

		// Every read goes through this: a 64-bit address (a node pointer plus an offset cannot wrap into a valid one), checked
		// against main RAM, then given to the reader.
		struct Peek
		{
			MemReader reader;

			bool operator()(u64 address, u8 width, u32* value) const
			{
				return address + width <= static_cast<u64>(Ps2MemSize::MainRam) && reader(static_cast<u32>(address), width, value);
			}

			bool F32(u64 address, double* out) const
			{
				u32 bits = 0;
				if (!(*this)(address, 4, &bits))
					return false;
				float f = 0.0f;
				std::memcpy(&f, &bits, sizeof(f));
				*out = static_cast<double>(f);
				return true;
			}

			bool S32(u64 address, double* out) const
			{
				u32 bits = 0;
				if (!(*this)(address, 4, &bits))
					return false;
				*out = static_cast<double>(static_cast<s32>(bits));
				return true;
			}
		};

		// --- The game's own tests, as measured ---

		struct Quad
		{
			double x[4];
			double z[4];
		};

		// Even-odd, so the corners may wind either way.
		bool PointInQuad(double px, double pz, const Quad& q)
		{
			bool inside = false;
			for (int i = 0; i < 4; i++)
			{
				const int j = (i + 1) & 3;
				const double x1 = q.x[i];
				const double z1 = q.z[i];
				const double x2 = q.x[j];
				const double z2 = q.z[j];
				if ((z1 > pz) != (z2 > pz) && px < (x2 - x1) * (pz - z1) / (z2 - z1) + x1)
					inside = !inside;
			}
			return inside;
		}

		// The squared distance from the point to the segment (a zero-length one is a point).
		double SegmentDistanceSq(double px, double pz, double ax, double az, double bx, double bz)
		{
			const double dx = bx - ax;
			const double dz = bz - az;
			const double len2 = dx * dx + dz * dz;
			double t = 0.0;
			if (len2 > 0.0)
				t = std::clamp(((px - ax) * dx + (pz - az) * dz) / len2, 0.0, 1.0);
			const double cx = ax + t * dx - px;
			const double cz = az + t * dz - pz;
			return cx * cx + cz * cz;
		}

		// The circle (centre, radius) overlaps the quad: its centre inside, or within radius of an edge (compared squared, so
		// a centre exactly radius away counts and nothing depends on a square root).
		bool CircleOverlapsQuad(double px, double pz, double radius, const Quad& q)
		{
			if (PointInQuad(px, pz, q))
				return true;
			const double r2 = radius * radius;
			for (int i = 0; i < 4; i++)
			{
				const int j = (i + 1) & 3;
				if (SegmentDistanceSq(px, pz, q.x[i], q.z[i], q.x[j], q.z[j]) <= r2)
					return true;
			}
			return false;
		}

		// The character's height is inside the node's, or the node starts up to `above` over it (an item on a counter).
		bool HeightsOverlap(double y, double y_min, double y_max, double above)
		{
			return (y_min <= y && y <= y_max) || (y < y_min && y_min <= y + above);
		}

		// How many bytes of a node the walk touches, so one range check covers them all.
		u64 NodeBytes(const ProfileDB::ReachParams& p)
		{
			const u64 ends[] = {
				static_cast<u64>(p.next_offset) + 4,
				static_cast<u64>(p.quad_offset) + 32,
				static_cast<u64>(p.y_min_offset) + 4,
				static_cast<u64>(p.y_max_offset) + 4,
				static_cast<u64>(p.type_offset) + 1,
				static_cast<u64>(p.mode_offset) + 1,
				static_cast<u64>(p.priority_offset) + 1,
				static_cast<u64>(p.enabled_offset) + 1,
				static_cast<u64>(p.busy_offset) + 1,
				static_cast<u64>(p.item_offset) + 4,
			};
			return *std::max_element(std::begin(ends), std::end(ends));
		}
	}

	Reach Evaluate(const ProfileDB::ReachParams& p, u32 record, MemReader reader)
	{
		Reach out;
		const Peek peek{reader ? reader : &ReadGuest};

		// The probe circle: ahead of the character along its heading by `ahead` plus its own reach float, at its height.
		const u64 rec = record;
		u32 heading_bits = 0;
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
		double reach = 0.0;
		if (!peek.F32(rec + p.position_offset, &x) || !peek.F32(rec + p.position_offset + 4, &y) ||
			!peek.F32(rec + p.position_offset + 8, &z) || !peek(rec + p.heading_offset, 2, &heading_bits) ||
			!peek.F32(rec + p.reach_offset, &reach) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
			!std::isfinite(reach) || std::fabs(reach) > 1000.0)
		{
			out.fault = Fault::Probe;
			return out;
		}
		const double heading = static_cast<double>(static_cast<s16>(static_cast<u16>(heading_bits))) * 2.0 * kPi / 65536.0;
		const double ahead = static_cast<double>(p.ahead) + reach;
		const double px = x + std::sin(heading) * ahead;
		const double pz = z + std::cos(heading) * ahead;
		const double radius = static_cast<double>(p.radius);
		const double above = static_cast<double>(p.above);

		u32 node = 0;
		if (!peek(p.list_head, 4, &node))
		{
			out.fault = Fault::ListHead;
			return out;
		}

		const u64 node_bytes = NodeBytes(p);
		const u32 cap = std::clamp<u32>(p.max_nodes, 1u, kHardNodeCap);
		// Brent's cycle check: `mark` is a node seen a power of two of steps ago; meeting it again is a loop. The cap below
		// is the backstop either way.
		u32 mark = 0;
		u32 since = 0;
		u32 limit = 1;
		u32 best_priority = 0;
		while (node != 0)
		{
			if (out.walked >= cap)
			{
				out.fault = Fault::NodeCap;
				break;
			}
			if ((node & 3u) != 0 || static_cast<u64>(node) + node_bytes > static_cast<u64>(Ps2MemSize::MainRam))
			{
				out.fault = Fault::NodePointer;
				break;
			}
			if (node == mark)
			{
				out.fault = Fault::Cycle;
				break;
			}
			if (++since == limit)
			{
				mark = node;
				limit <<= 1;
				since = 0;
			}
			out.walked++;

			u32 next = 0;
			u32 type = 0;
			u32 mode = 0;
			u32 enabled = 0;
			u32 busy = 0;
			u32 priority = 0;
			if (!peek(static_cast<u64>(node) + p.next_offset, 4, &next) || !peek(static_cast<u64>(node) + p.type_offset, 1, &type) ||
				!peek(static_cast<u64>(node) + p.mode_offset, 1, &mode) ||
				!peek(static_cast<u64>(node) + p.enabled_offset, 1, &enabled) ||
				!peek(static_cast<u64>(node) + p.busy_offset, 1, &busy) ||
				!peek(static_cast<u64>(node) + p.priority_offset, 1, &priority))
			{
				out.fault = Fault::NodePointer;
				break;
			}

			// A floor item the game's search would look at: the right kind, switched on, not in use. Only a node that could
			// beat the best so far is looked at any further (the lowest priority number wins, the first in the list on a tie).
			const bool candidate = type == p.accept_type && mode == p.accept_mode && enabled != 0 && busy == 0;
			if (candidate && (!out.found || priority < best_priority))
			{
				double y_min = 0.0;
				double y_max = 0.0;
				Quad q{};
				bool have = peek.S32(static_cast<u64>(node) + p.y_min_offset, &y_min) &&
				            peek.S32(static_cast<u64>(node) + p.y_max_offset, &y_max);
				for (int i = 0; i < 4 && have; i++)
				{
					have = peek.S32(static_cast<u64>(node) + p.quad_offset + static_cast<u64>(i) * 8, &q.x[i]) &&
					       peek.S32(static_cast<u64>(node) + p.quad_offset + static_cast<u64>(i) * 8 + 4, &q.z[i]);
				}
				if (have && HeightsOverlap(y, y_min, y_max, above) && CircleOverlapsQuad(px, pz, radius, q))
				{
					// The item behind the node. A record outside main RAM names nothing, and the node is not a candidate.
					u32 index = 0;
					u32 id = 0;
					if (peek(static_cast<u64>(node) + p.item_offset, 4, &index) &&
						peek(static_cast<u64>(p.item_table) + static_cast<u64>(index) * p.item_stride + p.item_id_offset, 2, &id))
					{
						out.found = true;
						out.index = index;
						out.item_id = static_cast<u16>(id);
						out.node = node;
						best_priority = priority;
					}
				}
			}
			node = next;
		}
		return out;
	}

	namespace
	{
		// The list walk Evaluate does (the same head, node range check, loop check and cap), calling visit(node) for each node
		// it reaches; visit returns false when something it needed did not read (the walk stops there, as Evaluate's does).
		// *walked counts the nodes visited. Returns why it stopped early, or Fault::None at the end of the list.
		template <class Visit>
		Fault WalkTriggerList(const Peek& peek, const ProfileDB::ReachParams& p, u32* walked, Visit&& visit)
		{
			u32 node = 0;
			if (!peek(p.list_head, 4, &node))
				return Fault::ListHead;
			const u64 node_bytes = NodeBytes(p);
			const u32 cap = std::clamp<u32>(p.max_nodes, 1u, kHardNodeCap);
			u32 mark = 0;
			u32 since = 0;
			u32 limit = 1;
			while (node != 0)
			{
				if (*walked >= cap)
					return Fault::NodeCap;
				if ((node & 3u) != 0 || static_cast<u64>(node) + node_bytes > static_cast<u64>(Ps2MemSize::MainRam))
					return Fault::NodePointer;
				if (node == mark)
					return Fault::Cycle;
				if (++since == limit)
				{
					mark = node;
					limit <<= 1;
					since = 0;
				}
				(*walked)++;
				u32 next = 0;
				if (!peek(static_cast<u64>(node) + p.next_offset, 4, &next) || !visit(node))
					return Fault::NodePointer;
				node = next;
			}
			return Fault::None;
		}
	}

	HandTarget EvaluateHandTarget(const ProfileDB::ReachParams& p, u32 record, const double hand_game[3], MemReader reader,
		const double* only_centre, double only_tol_cm)
	{
		HandTarget out;
		if (!hand_game || !std::isfinite(hand_game[0]) || !std::isfinite(hand_game[1]) || !std::isfinite(hand_game[2]))
			return out;
		for (int k = 0; k < 3; k++)
			out.hand[k] = hand_game[k];
		const Peek peek{reader ? reader : &ReadGuest};
		const double hx = hand_game[0];
		const double hy = hand_game[1];
		const double hz = hand_game[2];

		double best_d2 = 0.0;
		out.fault = WalkTriggerList(peek, p, &out.walked, [&](u32 node) {
			// A floor item the game's search would look at (Evaluate's filters), then only the Handgun's.
			u32 type = 0;
			u32 mode = 0;
			u32 enabled = 0;
			u32 busy = 0;
			if (!peek(static_cast<u64>(node) + p.type_offset, 1, &type) || !peek(static_cast<u64>(node) + p.mode_offset, 1, &mode) ||
				!peek(static_cast<u64>(node) + p.enabled_offset, 1, &enabled) ||
				!peek(static_cast<u64>(node) + p.busy_offset, 1, &busy))
				return false;
			if (!(type == p.accept_type && mode == p.accept_mode && enabled != 0 && busy == 0))
				return true;
			u32 index = 0;
			u32 id = 0;
			// A record that does not read names nothing: not a candidate, as in Evaluate.
			if (!peek(static_cast<u64>(node) + p.item_offset, 4, &index) ||
				!peek(static_cast<u64>(p.item_table) + static_cast<u64>(index) * p.item_stride + p.item_id_offset, 2, &id))
				return true;
			if (id != p.hand_item)
				return true;
			double y_min = 0.0;
			double y_max = 0.0;
			Quad q{};
			bool have = peek.S32(static_cast<u64>(node) + p.y_min_offset, &y_min) &&
			            peek.S32(static_cast<u64>(node) + p.y_max_offset, &y_max);
			for (int i = 0; i < 4 && have; i++)
			{
				have = peek.S32(static_cast<u64>(node) + p.quad_offset + static_cast<u64>(i) * 8, &q.x[i]) &&
				       peek.S32(static_cast<u64>(node) + p.quad_offset + static_cast<u64>(i) * 8 + 4, &q.z[i]);
			}
			if (!have)
				return true;
			if (only_centre)
			{
				const double cx = 0.25 * (q.x[0] + q.x[1] + q.x[2] + q.x[3]) - only_centre[0];
				const double cy = 0.5 * (y_min + y_max) - only_centre[1];
				const double cz = 0.25 * (q.z[0] + q.z[1] + q.z[2] + q.z[3]) - only_centre[2];
				if (!(cx * cx + cy * cy + cz * cz <= only_tol_cm * only_tol_cm))
					return true;
			}

			// How far the hand is from the volume: on the floor plane from the quad (0 inside it), in height from the range.
			const bool in_quad = PointInQuad(hx, hz, q);
			double horizontal = 0.0;
			if (!in_quad)
			{
				double edge_sq = std::numeric_limits<double>::max();
				for (int i = 0; i < 4; i++)
				{
					const int j = (i + 1) & 3;
					edge_sq = std::min(edge_sq, SegmentDistanceSq(hx, hz, q.x[i], q.z[i], q.x[j], q.z[j]));
				}
				horizontal = std::sqrt(edge_sq);
			}
			const double vertical = (hy < y_min) ? (y_min - hy) : (hy > y_max) ? (hy - y_max) : 0.0;
			const double d2 = horizontal * horizontal + vertical * vertical;
			// The nearest to the hand wins, the first in the list on a tie.
			if (out.found && !(d2 < best_d2))
				return true;
			out.found = true;
			best_d2 = d2;
			out.node = node;
			out.index = index;
			out.item_id = static_cast<u16>(id);
			for (int i = 0; i < 4; i++)
			{
				out.quad_x[i] = q.x[i];
				out.quad_z[i] = q.z[i];
			}
			out.y_min = y_min;
			out.y_max = y_max;
			out.horizontal_cm = horizontal;
			out.vertical_cm = vertical;
			// On an edge the distance can come out a rounding error above 0: that is still the quad's edge, inside.
			out.inside = (in_quad || horizontal < 1.0e-9) && vertical == 0.0;
			return true;
		});

		if (!out.found)
			return out;
		out.centre[0] = 0.25 * (out.quad_x[0] + out.quad_x[1] + out.quad_x[2] + out.quad_x[3]);
		out.centre[1] = 0.5 * (out.y_min + out.y_max);
		out.centre[2] = 0.25 * (out.quad_z[0] + out.quad_z[1] + out.quad_z[2] + out.quad_z[3]);
		// What the game's own press would pick from where the character stands: the same node or another.
		const Reach forward = Evaluate(p, record, reader);
		out.forward_agrees = forward.found && forward.node == out.node;
		return out;
	}

	namespace
	{
		// --- Once per approach ---

		struct Tracker
		{
			// The previous search, for `changed`.
			bool last_found = false;
			u32 last_index = 0;
			u16 last_id = 0;
			// The item last announced, until it has been away for kRearmMs.
			bool cued = false;
			u32 cued_index = 0;
			u16 cued_id = 0;
			bool away = false;
			u64 away_ms = 0;
			bool ever_cued = false;
			u64 cue_ms = 0;
		};

		struct Update
		{
			bool in_reach = false;
			u32 index = 0;
			u16 item_id = 0;
			bool changed = false;  // what is in reach differs from the previous search (entry, switch or leave)
			bool announce = false; // a new approach: cue it
		};

		Update Step(Tracker& t, const Reach& r, u64 now_ms)
		{
			Update u;
			u.in_reach = r.found;
			u.index = r.found ? r.index : 0;
			u.item_id = r.found ? r.item_id : 0;
			u.changed = (r.found != t.last_found) || (r.found && (r.index != t.last_index || r.item_id != t.last_id));
			t.last_found = r.found;
			t.last_index = u.index;
			t.last_id = u.item_id;

			if (r.found)
			{
				t.away = false;
				const bool new_item = !t.cued || t.cued_index != r.index || t.cued_id != r.item_id;
				// A cue held back by the minimum gap is not lost: the item is still new on the next search.
				if (new_item && (!t.ever_cued || now_ms - t.cue_ms >= kMinCueGapMs))
				{
					u.announce = true;
					t.cued = true;
					t.cued_index = r.index;
					t.cued_id = r.item_id;
					t.ever_cued = true;
					t.cue_ms = now_ms;
				}
			}
			else if (t.cued)
			{
				if (!t.away)
				{
					t.away = true;
					t.away_ms = now_ms;
				}
				else if (now_ms - t.away_ms >= kRearmMs)
				{
					t.cued = false;
					t.away = false;
				}
			}
			return u;
		}

		// --- The cue ---

		// The toast for an item, false for an id ItemData does not know (nothing is cued for those).
		bool ToastFor(u16 item_id, std::string* text)
		{
			const ItemData::Entry* e = ItemData::Find(item_id);
			if (!e)
				return false;
			*text = std::string(e->name) + kToastSuffix;
			return true;
		}

		void Announce(const Update& u, float strength)
		{
			std::string text;
			if (!ToastFor(u.item_id, &text))
			{
				Console.WriteLn("(VR) Pickup cue: item %u (record %u) in reach; ItemData has no name for it, so no cue.",
					static_cast<u32>(u.item_id), u.index);
				return;
			}
			Console.WriteLn("(VR) Pickup cue: %s (item %u) in reach", ItemData::Name(u.item_id), static_cast<u32>(u.item_id));
			const float amp = std::clamp(kTickAmp * (std::isfinite(strength) ? strength : 0.0f), 0.0f, 1.0f);
			if (amp > 0.0f)
				XRInput::QueuePulse(kRightHand, amp, kTickSeconds);
			HudCards::Toast(text, kToastSeconds);
		}

		const char* FaultText(Fault f)
		{
			switch (f)
			{
				case Fault::Probe: return "the character's position, heading or reach did not read as numbers";
				case Fault::ListHead: return "the list head address could not be read";
				case Fault::NodePointer:
					return "a trigger node's address is outside main RAM, misaligned or unreadable, so the walk stopped there";
				case Fault::NodeCap: return "the walk reached reach.maxNodes nodes with the list still going";
				case Fault::Cycle: return "the trigger list loops back on itself, so the walk stopped";
				default: return "";
			}
		}

		// --- State (the CPU thread's, except the published view) ---

		Tracker s_tracker;
		bool s_active = false;
		u32 s_vsync = 0;
		u32 s_fault_seen = 0; // one bit per Fault kind said this arm

		std::mutex s_view_mutex;
		View s_view;

		void Publish(bool in_reach, u32 index, u16 item_id)
		{
			std::lock_guard<std::mutex> lock(s_view_mutex);
			if (s_view.in_reach != in_reach || (in_reach && (s_view.index != index || s_view.item_id != item_id)))
				s_view.serial++;
			s_view.in_reach = in_reach;
			s_view.index = in_reach ? index : 0;
			s_view.item_id = in_reach ? item_id : 0;
		}

		// --- Hand targeting (the CPU thread's, except the published mark) ---

		// The one trigger and its distances come from the profile's reach.hand (File #1: the Handgun on J's Bar counter). A target
		// exists while that volume's middle is within hand_range of the hand (hand_keep once it has one, so the edge of the range
		// does not flicker it); farther, the volume is in another room or part of the level and drawing it would only confuse.
		// hand_near is the marker's "brighter within".
		constexpr u32 kHandEvalEveryVsyncs = 2;
		constexpr u64 kHandLogMs = 1000;
		constexpr u64 kMarkStaleMs = 250;

		struct HandState
		{
			bool logged_arm = false; // the activation lines were said since first person last armed
			bool has_target = false;
			u32 node = 0;
			u16 item_id = 0;
			float centre_game[3] = {}; // the target's volume centre as found, game cm, and in the headset's space, metres
			float centre_vr[3] = {};   // (said again when it is lost)
			bool inside = false;     // the last evaluation's verdict, for the one tick per entry
			bool published = false;  // a marker is out there (so a vsync with nothing to do touches no lock)
			u32 vsync = 0;
			u64 next_log_ms = 0;
		};
		HandState s_hand;

		std::mutex s_mark_mutex;
		TargetMark s_mark;
		bool s_mark_valid = false;
		u64 s_mark_ms = 0;

		void PublishMark(const TargetMark* mark)
		{
			s_hand.published = (mark != nullptr);
			std::lock_guard<std::mutex> lock(s_mark_mutex);
			s_mark_valid = (mark != nullptr);
			if (mark)
			{
				s_mark = *mark;
				const u64 now = CameraDriver::SteadyNowMs();
				s_mark_ms = (now != 0) ? now : 1;
			}
		}

		// No target any more (or not evaluating at all): the marker goes, and a target that was there is said lost.
		void HandStop(bool say)
		{
			if (!s_hand.has_target && !s_hand.published)
				return;
			if (s_hand.has_target && say)
				Console.WriteLn("(VR) Hand targeting: target lost: node 0x%08X, item %u (%s), volume centre game (%.1f, %.1f, %.1f) cm, "
								"VR (%.3f, %.3f, %.3f) m",
					s_hand.node, static_cast<u32>(s_hand.item_id), ItemData::Name(s_hand.item_id),
					static_cast<double>(s_hand.centre_game[0]), static_cast<double>(s_hand.centre_game[1]),
					static_cast<double>(s_hand.centre_game[2]), static_cast<double>(s_hand.centre_vr[0]),
					static_cast<double>(s_hand.centre_vr[1]), static_cast<double>(s_hand.centre_vr[2]));
			s_hand.has_target = false;
			s_hand.inside = false;
			s_hand.next_log_ms = 0;
			PublishMark(nullptr);
		}

		std::atomic<bool> s_selftest_ran{false};

		void MaybeRunSelfTest()
		{
			static const bool enabled = (std::getenv("PCSX2_VR_SELFTEST") != nullptr);
			if (!enabled || s_selftest_ran.exchange(true, std::memory_order_acq_rel))
				return;
			SelfTest();
		}
	}

	void Poll(const ProfileDB::Profile* profile, const Live& live, u64 now_ms, float haptic_strength)
	{
		MaybeRunSelfTest();

		const ProfileDB::ReachParams* params =
			(profile && profile->reach.has_value()) ? &profile->reach.value() : nullptr;
		if (!params || !EmuConfig.VR.PickupCue || !live.armed || !VMManager::HasValidVM())
		{
			// Off, or first person is not armed (a cutscene, a grab, the title screen): the next approach cues afresh.
			if (s_active)
			{
				s_active = false;
				s_tracker = Tracker{};
				s_vsync = 0;
				Publish(false, 0, 0);
			}
			return;
		}
		if (!s_active)
		{
			s_active = true;
			s_fault_seen = 0;
			Console.WriteLn("(VR) Pickup cue: on (reach model from the profile, list head 0x%X)", params->list_head);
		}

		// The camera guards' flicker (the room-camera byte flips about 12 times a second): the same scene, so the search sits
		// this vsync out and everything stays as it was.
		if (!live.guards_now)
			return;
		// Where Circle answers something else (a screen's prompt, firing) there is nothing to cue, and the tracker keeps what
		// it knew, so a prompt declined with Cross, or a weapon lowered beside the same item, is not announced again.
		if (live.screen_up || live.weapon_raised)
		{
			Publish(false, 0, 0);
			return;
		}
		if ((s_vsync++ % kEvalEveryVsyncs) != 0)
			return;

		const Reach r = Evaluate(*params, live.record, nullptr);
		if (r.fault != Fault::None)
		{
			const u32 bit = 1u << static_cast<u32>(r.fault);
			if ((s_fault_seen & bit) == 0)
			{
				s_fault_seen |= bit;
				Console.Warning("(VR) Pickup cue: %s (list head 0x%X, record 0x%X, %u node(s) walked); said once per arm.",
					FaultText(r.fault), params->list_head, live.record, r.walked);
			}
		}
		const Update u = Step(s_tracker, r, now_ms);
		Publish(u.in_reach, u.index, u.item_id);
		if (u.announce)
			Announce(u, haptic_strength);
	}

	void PollHandTarget(const ProfileDB::Profile* profile, const Live& live, u64 now_ms, float haptic_strength)
	{
		const ProfileDB::ReachParams* params =
			(profile && profile->reach.has_value() && profile->reach->has_hand) ? &profile->reach.value() : nullptr;
		if (!params || !EmuConfig.VR.HandTargeting || !live.hand_game || !live.net_offline || !live.armed ||
			!VMManager::HasValidVM())
		{
			// Off, or not first person offline in File #1: nothing is shown, and the next arm says its lines afresh.
			HandStop(true);
			s_hand.logged_arm = false;
			return;
		}
		// The camera guards' flicker: the same scene, so this vsync is sat out and everything stays as it was.
		if (!live.guards_now)
			return;
		if (live.screen_up)
		{
			HandStop(true);
			return;
		}
		// The mapping exists only while the camera is being driven (CameraDriver::GetMapping), the hand only while it is tracked.
		CameraDriver::Mapping map;
		const VRInputSnapshot snap = GetInputSnapshot();
		const VRPose& grip = snap.hands[VRInputSnapshot::RIGHT].grip_pose;
		if (!CameraDriver::GetMapping(&map) || snap.generation == 0 || !snap.actions_active || !grip.valid)
		{
			HandStop(true);
			return;
		}

		if (!s_hand.logged_arm)
		{
			// Once each time first person arms: what this is, and whether the mapping agrees with the code that places the eye.
			const HeadPose::Snapshot head = HeadPose::Get();
			const float head_vr[3] = {head.position_x, head.position_y, head.position_z};
			double eye_err_mm = 0.0;
			double round_trip_mm = 0.0;
			if (head.valid && head.position_valid &&
				CameraDriver::MappingCheck(head_vr, grip.position_xyz.data(), &eye_err_mm, &round_trip_mm))
			{
				s_hand.logged_arm = true;
				Console.WriteLn("(VR) Hand targeting: on (offline File #1, right hand, read-only): the item %u trigger centred within %.0f cm "
								"of (%.1f, %.1f, %.1f), range %.0f/%.0f cm, brighter within %.0f cm (profile reach.hand)",
					static_cast<u32>(params->hand_item), static_cast<double>(params->hand_tolerance),
					static_cast<double>(params->hand_centre[0]), static_cast<double>(params->hand_centre[1]),
					static_cast<double>(params->hand_centre[2]), static_cast<double>(params->hand_range),
					static_cast<double>(params->hand_keep), static_cast<double>(params->hand_near));
				Console.WriteLn("(VR) Hand targeting: mapping check: eye->head error %.3f mm, round trip %.3f mm", eye_err_mm,
					round_trip_mm);
			}
		}

		if ((s_hand.vsync++ % kHandEvalEveryVsyncs) != 0)
			return;

		float hand_game[3];
		if (!CameraDriver::GameFromVr(map, grip.position_xyz.data(), hand_game))
		{
			HandStop(true);
			return;
		}
		const double hand[3] = {hand_game[0], hand_game[1], hand_game[2]};
		const double only_centre[3] = {params->hand_centre[0], params->hand_centre[1], params->hand_centre[2]};
		const HandTarget t = EvaluateHandTarget(*params, live.record, hand, nullptr, only_centre, params->hand_tolerance);
		const double to_centre = std::sqrt((t.hand[0] - t.centre[0]) * (t.hand[0] - t.centre[0]) +
		                                   (t.hand[1] - t.centre[1]) * (t.hand[1] - t.centre[1]) +
		                                   (t.hand[2] - t.centre[2]) * (t.hand[2] - t.centre[2]));
		if (!t.found || to_centre > (s_hand.has_target ? params->hand_keep : params->hand_range))
		{
			HandStop(true);
			return;
		}

		// The volume centre and the box's corners in the headset's space.
		const float centre_game[3] = {static_cast<float>(t.centre[0]), static_cast<float>(t.centre[1]),
			static_cast<float>(t.centre[2])};
		float centre_vr[3] = {};
		TargetMark mark;
		bool mapped = CameraDriver::VrFromGame(map, centre_game, centre_vr);
		for (int i = 0; i < 4 && mapped; i++)
		{
			const float low[3] = {static_cast<float>(t.quad_x[i]), static_cast<float>(t.y_min), static_cast<float>(t.quad_z[i])};
			const float high[3] = {static_cast<float>(t.quad_x[i]), static_cast<float>(t.y_max), static_cast<float>(t.quad_z[i])};
			mapped = CameraDriver::VrFromGame(map, low, mark.corner[i]) && CameraDriver::VrFromGame(map, high, mark.corner[4 + i]);
		}
		if (!mapped)
		{
			HandStop(true);
			return;
		}

		// A different node is another target: the old one is lost first.
		if (s_hand.has_target && s_hand.node != t.node)
			HandStop(true);
		if (!s_hand.has_target)
		{
			s_hand.has_target = true;
			s_hand.node = t.node;
			s_hand.item_id = t.item_id;
			for (int k = 0; k < 3; k++)
			{
				s_hand.centre_game[k] = centre_game[k];
				s_hand.centre_vr[k] = centre_vr[k];
			}
			s_hand.inside = false;
			s_hand.next_log_ms = 0;
			Console.WriteLn("(VR) Hand targeting: target found: node 0x%08X, item %u (%s), volume centre game (%.1f, %.1f, %.1f) cm, "
							"VR (%.3f, %.3f, %.3f) m",
				t.node, static_cast<u32>(t.item_id), ItemData::Name(t.item_id), t.centre[0], t.centre[1], t.centre[2],
				static_cast<double>(centre_vr[0]), static_cast<double>(centre_vr[1]), static_cast<double>(centre_vr[2]));
		}

		// The marker: dim while the hand is far, brighter within hand_near of the volume, brightest inside it.
		const double volume_cm = std::sqrt(t.horizontal_cm * t.horizontal_cm + t.vertical_cm * t.vertical_cm);
		mark.node = t.node;
		mark.glow = t.inside ? 2 : (volume_cm <= params->hand_near ? 1 : 0);
		PublishMark(&mark);

		// One light tick when the hand enters the volume, not again until it has left.
		if (t.inside && !s_hand.inside)
		{
			const float amp = std::clamp(kTickAmp * (std::isfinite(haptic_strength) ? haptic_strength : 0.0f), 0.0f, 1.0f);
			if (amp > 0.0f)
				XRInput::QueuePulse(kRightHand, amp, kTickSeconds);
		}
		s_hand.inside = t.inside;

		if (now_ms < s_hand.next_log_ms)
			return;
		s_hand.next_log_ms = now_ms + kHandLogMs;
		Console.WriteLn("(VR) Hand targeting: hand VR (%.3f, %.3f, %.3f) m, game (%.1f, %.1f, %.1f) cm; volume %.1f cm horizontal, "
						"%.1f cm vertical, %.1f cm to its centre; inside %s; forward search agrees %s",
			static_cast<double>(grip.position_xyz[0]), static_cast<double>(grip.position_xyz[1]),
			static_cast<double>(grip.position_xyz[2]), t.hand[0], t.hand[1], t.hand[2], t.horizontal_cm, t.vertical_cm, to_centre,
			t.inside ? "yes" : "no", t.forward_agrees ? "yes" : "no");

		// The projection check: is the volume's middle where the game's own camera sees it, as the map says it is? (What
		// agreement does and does not prove is on CameraDriver::ProjectionCheck.)
		CameraDriver::ProjectionResult pr;
		if (CameraDriver::ProjectionCheck(centre_game, &pr))
		{
			Console.WriteLn("(VR) Hand targeting: projection check: game view yaw %.2f pitch %.2f deg, headset yaw %.2f pitch %.2f deg, "
							"difference %.2f deg",
				static_cast<double>(pr.game_yaw_deg), static_cast<double>(pr.game_pitch_deg), static_cast<double>(pr.head_yaw_deg),
				static_cast<double>(pr.head_pitch_deg), static_cast<double>(pr.diff_deg));
		}
		else
		{
			Console.WriteLn("(VR) Hand targeting: projection check: not available (no matched frame this vsync, the camera turned since that frame, or the view's yaw sign is not 1)");
		}
	}

	bool GetTargetMark(TargetMark* out)
	{
		std::lock_guard<std::mutex> lock(s_mark_mutex);
		if (!s_mark_valid)
			return false;
		const u64 now = CameraDriver::SteadyNowMs();
		if (now > s_mark_ms && now - s_mark_ms > kMarkStaleMs)
			return false;
		if (out)
			*out = s_mark;
		return true;
	}

	void OnStateLoaded()
	{
		s_tracker = Tracker{};
		s_vsync = 0;
		Publish(false, 0, 0);
		// A loaded state is another scene (and another character position): hand targeting starts over quietly.
		s_hand.has_target = false;
		s_hand.inside = false;
		s_hand.next_log_ms = 0;
		PublishMark(nullptr);
	}

	View Current()
	{
		std::lock_guard<std::mutex> lock(s_view_mutex);
		return s_view;
	}
}

// The self-test: scripted RAM, so none of it needs a running game.
namespace VR::ItemReach
{
	namespace
	{
		// A window of scripted guest RAM, little-endian like the EE's, inside main RAM so the range check passes and anything
		// else in main RAM (beyond the window) reads as unreadable.
		constexpr u32 kTestBase = 0x00200000;
		constexpr u32 kTestRecord = kTestBase + 0x000;
		constexpr u32 kTestHead = kTestBase + 0x400;
		constexpr u32 kTestItems = kTestBase + 0x600;
		constexpr u32 kTestNodes = kTestBase + 0x1000;
		constexpr u32 kTestNodeStride = 0x50;
		constexpr size_t kTestSize = 0x2000;
		std::array<u8, kTestSize> s_test_ram{};

		bool TestRead(u32 address, u8 width, u32* value)
		{
			if (address < kTestBase || static_cast<u64>(address) + width > static_cast<u64>(kTestBase) + kTestSize ||
				(address & (static_cast<u32>(width) - 1u)) != 0)
				return false;
			u32 v = 0;
			for (u32 i = 0; i < width; i++)
				v |= static_cast<u32>(s_test_ram[static_cast<size_t>(address - kTestBase + i)]) << (8u * i);
			*value = v;
			return true;
		}

		void TestPut(u32 address, u8 width, u32 value)
		{
			for (u32 i = 0; i < width; i++)
			{
				const u64 at = static_cast<u64>(address) + i;
				if (at >= kTestBase && at < static_cast<u64>(kTestBase) + kTestSize)
					s_test_ram[static_cast<size_t>(at - kTestBase)] = static_cast<u8>(value >> (8u * i));
			}
		}

		void TestPutF32(u32 address, float value)
		{
			u32 bits = 0;
			std::memcpy(&bits, &value, sizeof(bits));
			TestPut(address, 4, bits);
		}

		// One trigger node as the game lays it out (File #1's offsets, which ReachParams defaults to).
		struct NodeSpec
		{
			s32 q[8] = {};      // four (x, z) corners
			s32 y_min = 100;
			s32 y_max = 200;
			u8 type = 0x80;
			u8 mode = 0;
			u8 priority = 0;
			u8 enabled = 1;
			u8 busy = 0;
			u32 item = 1;       // item record index
			int next = -1;      // the next node's slot, -1 ends the list
			u32 next_raw = 0;   // used instead of a slot when non-zero
		};

		u32 NodeAddress(int slot)
		{
			return kTestNodes + static_cast<u32>(slot) * kTestNodeStride;
		}

		void Rect(NodeSpec& n, s32 x0, s32 z0, s32 x1, s32 z1)
		{
			const s32 c[8] = {x0, z0, x1, z0, x1, z1, x0, z1};
			std::copy(std::begin(c), std::end(c), std::begin(n.q));
		}

		void PutNode(int slot, const NodeSpec& n)
		{
			const u32 a = NodeAddress(slot);
			for (int i = 0; i < 8; i++)
				TestPut(a + static_cast<u32>(i) * 4, 4, static_cast<u32>(n.q[i]));
			TestPut(a + 0x20, 4, static_cast<u32>(n.y_min));
			TestPut(a + 0x24, 4, static_cast<u32>(n.y_max));
			TestPut(a + 0x28, 4, n.next_raw != 0 ? n.next_raw : (n.next >= 0 ? NodeAddress(n.next) : 0u));
			TestPut(a + 0x30, 1, n.type);
			TestPut(a + 0x31, 1, n.mode);
			TestPut(a + 0x32, 1, n.priority);
			TestPut(a + 0x3C, 1, n.enabled);
			TestPut(a + 0x3D, 1, n.busy);
			TestPut(a + 0x40, 4, n.item);
		}

		void PutCharacter(float x, float y, float z, int heading, float reach)
		{
			TestPutF32(kTestRecord + 0x38, x);
			TestPutF32(kTestRecord + 0x3C, y);
			TestPutF32(kTestRecord + 0x40, z);
			TestPut(kTestRecord + 0x92, 2, static_cast<u16>(static_cast<s16>(heading)));
			TestPutF32(kTestRecord + 0x1B4, reach);
		}

		void PutItem(u32 index, u16 id)
		{
			TestPut(kTestItems + index * 60 + 0x06, 2, id);
		}
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

		s_test_ram.fill(0);
		ProfileDB::ReachParams p;
		p.list_head = kTestHead;
		p.item_table = kTestItems;
		PutItem(1, 300); // Green Herb
		PutItem(2, 0);   // Handgun
		PutItem(3, 301);
		PutItem(4, 302);
		PutItem(5, 303);
		PutItem(6, 304);

		const auto search = [&]() { return Evaluate(p, kTestRecord, TestRead); };
		const auto at = [&](float x, float y, float z, int heading = 0, float reach = 0.0f) {
			PutCharacter(x, y, z, heading, reach);
			return search();
		};
		// A search that found item record `index` (id from the table above), and nothing else wrong.
		const auto got = [](const Reach& r, u32 index, u16 id) { return r.found && r.index == index && r.item_id == id; };

		// --- The bathroom Green Herb (File #1, measured against the game's own press, 2026-10-12): trigger x 9581-9622,
		// z 13052-13093, the sink at y 137, Kevin at x 9602 facing +z (heading 0). z 13000 and 12965 took it, 12950 and
		// 12935 did not. (The node's own heights are not in the research, so these are scripted around 137.) ---
		NodeSpec herb;
		Rect(herb, 9581, 13052, 9622, 13093);
		herb.item = 1;
		PutNode(0, herb);
		TestPut(kTestHead, 4, NodeAddress(0));
		check(got(at(9602, 137, 13000), 1, 300), "herb: z 13000 is in reach (the probe is 6 from the quad)");
		check(got(at(9602, 137, 12965), 1, 300), "herb: z 12965 is in reach (41 from the quad)");
		check(!at(9602, 137, 12950).found, "herb: z 12950 is not (56 from the quad)");
		check(!at(9602, 137, 12935).found, "herb: z 12935 is not (71 from the quad)");
		check(got(at(9602, 137, 12956), 1, 300), "herb: exactly 50 from an edge counts");
		check(!at(9602, 137, 12955).found, "herb: 51 from an edge does not");
		check(!at(9541, 137, 12966).found, "herb: 57 from the corner, though within 50 either way, is a circle not a box: out");
		check(got(at(9546, 137, 12971), 1, 300), "herb: 49.5 from the corner is in");
		check(got(at(9602, 137, 13014), 1, 300), "herb: the probe inside the quad is in");
		check(at(9602, 137, 12965).node == NodeAddress(0) && at(9602, 137, 12965).walked == 1 &&
				  at(9602, 137, 12965).fault == Fault::None,
			"herb: the winning node and the walk are reported");

		// --- Heights: the character's y inside [y min, y max], or the node starting up to 200 over it. ---
		herb.y_min = 100;
		herb.y_max = 200;
		PutNode(0, herb);
		check(got(at(9602, 150, 13014), 1, 300), "height: between y min and y max");
		check(got(at(9602, 100, 13014), 1, 300), "height: exactly y min");
		check(got(at(9602, 200, 13014), 1, 300), "height: exactly y max");
		check(!at(9602, 200.5f, 13014).found, "height: over y max (and not under y min) is out");
		check(got(at(9602, 99, 13014), 1, 300), "height: 1 under y min (the node starts 101 over the character)");
		check(got(at(9602, -100, 13014), 1, 300), "height: the node starting exactly 200 over the character counts");
		check(!at(9602, -100.5f, 13014).found, "height: 200.5 over the character does not");
		check(!at(9602, -1000, 13014).found, "height: far under is out");

		// --- Which nodes count at all. ---
		const auto variant = [&](const char* name, const NodeSpec& n, bool want) {
			PutNode(0, n);
			check(at(9602, 137, 13014).found == want, name);
		};
		NodeSpec n = herb;
		variant("filter: the plain node counts", n, true);
		n = herb;
		n.type = 0x81;
		variant("filter: another trigger type (0x81) does not", n, false);
		n = herb;
		n.type = 0x05;
		variant("filter: an examine spot (type 0x05) does not", n, false);
		n = herb;
		n.mode = 1;
		variant("filter: another search mode does not", n, false);
		n = herb;
		n.enabled = 0;
		variant("filter: a disabled node does not", n, false);
		n = herb;
		n.enabled = 2;
		variant("filter: any non-zero enabled byte counts", n, true);
		n = herb;
		n.busy = 1;
		variant("filter: a busy node does not", n, false);
		n = herb;
		n.busy = 0xFF;
		variant("filter: any non-zero busy byte does not", n, false);
		PutNode(0, herb);

		// --- Several nodes: the lowest priority number wins, the first in the list on a tie. ---
		{
			NodeSpec a = herb;
			NodeSpec b = herb;
			a.priority = 5;
			a.item = 1;
			a.next = 1;
			b.priority = 2;
			b.item = 2;
			PutNode(0, a);
			PutNode(1, b);
			check(got(at(9602, 137, 13014), 2, 0), "priority: 2 beats 5 (the second node in the list wins)");
			a.priority = 3;
			b.priority = 3;
			PutNode(0, a);
			PutNode(1, b);
			check(got(at(9602, 137, 13014), 1, 300), "priority: a tie goes to the first node in the list");
			b.priority = 0;
			b.enabled = 0;
			PutNode(1, b);
			check(got(at(9602, 137, 13014), 1, 300), "priority: a better node that does not count does not win");
			b.enabled = 1;
			Rect(b, 0, 0, 10, 10);
			PutNode(1, b);
			check(got(at(9602, 137, 13014), 1, 300), "priority: a better node out of reach does not win");
			PutNode(0, herb);
		}

		// --- Headings (s16, 0x10000 = 360 degrees, 0 faces +z, 0x4000 faces +x): four quads 46 units from the character, one
		// each way; exactly the one it faces is found. ---
		{
			NodeSpec north = herb;
			NodeSpec east = herb;
			NodeSpec south = herb;
			NodeSpec west = herb;
			Rect(north, 4995, 5041, 5005, 5051);
			Rect(east, 5041, 4995, 5051, 5005);
			Rect(south, 4995, 4949, 5005, 4959);
			Rect(west, 4949, 4995, 4959, 5005);
			north.item = 1;
			east.item = 3;
			south.item = 4;
			west.item = 5;
			north.next = 1;
			east.next = 2;
			south.next = 3;
			PutNode(0, north);
			PutNode(1, east);
			PutNode(2, south);
			PutNode(3, west);
			check(got(at(5000, 137, 5000, 0), 1, 300), "heading 0 faces +z");
			check(got(at(5000, 137, 5000, 0x4000), 3, 301), "heading 0x4000 faces +x");
			check(got(at(5000, 137, 5000, -32768), 4, 302), "heading -0x8000 (0x8000) faces -z");
			check(got(at(5000, 137, 5000, -16384), 5, 303), "heading -0x4000 (0xC000) faces -x");

			// The character's own reach float lengthens the probe: a small quad 146 ahead is found only with reach 100.
			NodeSpec far_quad = herb;
			Rect(far_quad, 4995, 5141, 5005, 5151);
			far_quad.item = 6;
			west.next = 4;
			PutNode(3, west);
			PutNode(4, far_quad);
			check(got(at(5000, 137, 5000, 0, 0.0f), 1, 300), "reach 0: the probe is 46 ahead, the near quad");
			check(got(at(5000, 137, 5000, 0, 100.0f), 6, 304), "reach 100: the probe is 146 ahead, the far quad");
		}

		// --- Quad shapes: the inside test and the edge test on their own, winding, a point, negative corners. ---
		{
			NodeSpec big = herb;
			Rect(big, 0, 0, 1000, 1000);
			PutNode(0, big);
			check(got(at(500, 137, 454), 1, 300), "shape: a probe far inside a big quad is in (the inside test)");
			check(!at(500, 137, 1100).found, "shape: 146 outside a big quad is out");

			// A diamond (x 0-2000, z 0-2000), wound both ways: its top vertex (1000, 2000).
			NodeSpec diamond = herb;
			const s32 d[8] = {1000, 0, 2000, 1000, 1000, 2000, 0, 1000};
			std::copy(std::begin(d), std::end(d), std::begin(diamond.q));
			PutNode(0, diamond);
			check(got(at(1000, 137, 954), 1, 300), "shape: a diamond, the probe at its middle");
			check(got(at(1000, 137, 2004), 1, 300), "shape: a diamond, the probe exactly 50 over its top vertex");
			check(!at(1000, 137, 2014).found, "shape: a diamond, the probe 60 over its top vertex");
			const s32 rev[8] = {0, 1000, 1000, 2000, 2000, 1000, 1000, 0};
			std::copy(std::begin(rev), std::end(rev), std::begin(diamond.q));
			PutNode(0, diamond);
			check(got(at(1000, 137, 954), 1, 300) && got(at(1000, 137, 2004), 1, 300) && !at(1000, 137, 2014).found,
				"shape: the same diamond wound the other way gives the same answers");

			// All four corners one point: a plain circle test.
			NodeSpec dot = herb;
			const s32 pt[8] = {500, 500, 500, 500, 500, 500, 500, 500};
			std::copy(std::begin(pt), std::end(pt), std::begin(dot.q));
			PutNode(0, dot);
			check(got(at(500, 137, 494), 1, 300), "shape: a quad collapsed to a point, 40 from it is in");
			check(got(at(500, 137, 504), 1, 300), "shape: a quad collapsed to a point, exactly 50 from it is in");
			check(!at(500, 137, 514).found, "shape: a quad collapsed to a point, 60 from it is out");

			// Negative corners (s32) and the extremes.
			NodeSpec neg = herb;
			Rect(neg, -1000, -1000, -900, -900);
			PutNode(0, neg);
			check(got(at(-950, 137, -996), 1, 300), "shape: a quad at negative coordinates");
			NodeSpec huge = herb;
			Rect(huge, -2147483647 - 1, -2147483647 - 1, 2147483647, 2147483647);
			PutNode(0, huge);
			check(got(at(0, 137, 0), 1, 300), "shape: a quad spanning the whole s32 range");
			PutNode(0, herb);
		}

		// --- What does not read: the walk says so and finds nothing it cannot trust. ---
		{
			PutCharacter(9602, 137, 13014, 0, 0.0f);
			TestPutF32(kTestRecord + 0x38, std::nanf(""));
			Reach r = search();
			check(!r.found && r.fault == Fault::Probe, "probe: a NaN position finds nothing");
			PutCharacter(9602, 137, 13014, 0, 1.0e9f);
			r = search();
			check(!r.found && r.fault == Fault::Probe, "probe: an absurd reach finds nothing");
			PutCharacter(9602, 137, 13014, 0, 0.0f);
			r = Evaluate(p, 0x01FFFFF0, TestRead);
			check(!r.found && r.fault == Fault::Probe, "probe: a record at the end of main RAM does not read");
			r = Evaluate(p, kTestRecord, TestRead);
			check(got(r, 1, 300) && r.fault == Fault::None, "probe: and the same search reads fine");

			ProfileDB::ReachParams bad_head = p;
			bad_head.list_head = kTestBase - 4;
			check(Evaluate(bad_head, kTestRecord, TestRead).fault == Fault::ListHead, "list head: unreadable is a fault");
			bad_head.list_head = 0x01FFFFFE;
			check(Evaluate(bad_head, kTestRecord, TestRead).fault == Fault::ListHead, "list head: past main RAM is a fault");
			TestPut(kTestHead, 4, 0);
			r = search();
			check(!r.found && r.fault == Fault::None && r.walked == 0, "list head: 0 is an empty list, not a fault");
			TestPut(kTestHead, 4, 0xFFFFFFF0u);
			r = search();
			check(!r.found && r.fault == Fault::NodePointer && r.walked == 0, "list head: a first node outside main RAM is a fault");
			TestPut(kTestHead, 4, NodeAddress(0) + 2);
			check(search().fault == Fault::NodePointer, "list head: a misaligned first node is a fault");
			TestPut(kTestHead, 4, NodeAddress(0));
		}

		// --- The walk: a loop, a cap, a bad next pointer. ---
		{
			NodeSpec self = herb;
			self.next = 0;
			PutNode(0, self);
			Reach r = at(9602, 137, 13014);
			check(got(r, 1, 300) && r.fault == Fault::Cycle && r.walked == 1, "walk: a node linking to itself is met once, then stops");

			NodeSpec far_node = herb;
			Rect(far_node, 0, 0, 10, 10);
			far_node.next = 1;
			NodeSpec hit = herb;
			hit.next = 0;
			PutNode(0, far_node);
			PutNode(1, hit);
			r = search();
			check(got(r, 1, 300) && r.fault == Fault::Cycle && r.walked == 2, "walk: a two-node loop is found after two nodes");

			// A ring of five that never matches: stopped by the loop check, far inside the cap.
			for (int i = 0; i < 5; i++)
			{
				NodeSpec ring = far_node;
				ring.next = (i + 1) % 5;
				PutNode(i, ring);
			}
			r = search();
			check(!r.found && r.fault == Fault::Cycle && r.walked <= 16, "walk: a ring of five is stopped by the loop check");

			// A chain of twelve nodes: the match is the 6th, then the 10th. A cap of 8 sees the first and not the second (and says the
			// list went on); a cap of 12 walks it all.
			const auto chain = [&](int match_slot) {
				for (int i = 0; i < 12; i++)
				{
					NodeSpec link = (i == match_slot) ? herb : far_node;
					link.next = (i + 1 < 12) ? i + 1 : -1;
					PutNode(i, link);
				}
			};
			PutCharacter(9602, 137, 13014, 0, 0.0f);
			ProfileDB::ReachParams capped = p;
			capped.max_nodes = 8;
			chain(5);
			r = Evaluate(capped, kTestRecord, TestRead);
			check(got(r, 1, 300) && r.walked == 8 && r.fault == Fault::NodeCap, "walk: a match inside the cap is found, the cap is reported");
			chain(9);
			r = Evaluate(capped, kTestRecord, TestRead);
			check(!r.found && r.walked == 8 && r.fault == Fault::NodeCap, "walk: a match past the cap is not found");
			capped.max_nodes = 12;
			r = Evaluate(capped, kTestRecord, TestRead);
			check(got(r, 1, 300) && r.walked == 12 && r.fault == Fault::None, "walk: a cap of exactly the list's length walks it all, no fault");

			// A bad next pointer ends the walk after the nodes before it; those still count.
			NodeSpec bad = herb;
			bad.next_raw = 0x02000000;
			PutNode(0, bad);
			r = search();
			check(got(r, 1, 300) && r.walked == 1 && r.fault == Fault::NodePointer, "walk: a next pointer past main RAM stops the walk, the node before it counts");
			bad.next_raw = NodeAddress(1) + 2;
			PutNode(0, bad);
			check(search().fault == Fault::NodePointer, "walk: a misaligned next pointer stops it");
			bad.next_raw = 0x00300000;
			PutNode(0, bad);
			r = search();
			check(got(r, 1, 300) && r.fault == Fault::NodePointer, "walk: a next pointer in main RAM but not readable stops it");
			PutNode(0, herb);
		}

		// --- The item record behind a node must be readable. ---
		{
			NodeSpec lost = herb;
			lost.priority = 1;
			lost.item = 0x10000000;
			lost.next = 1;
			NodeSpec fine = herb;
			fine.priority = 5;
			fine.item = 2;
			PutNode(0, lost);
			PutNode(1, fine);
			check(got(at(9602, 137, 13014), 2, 0), "item: a better node whose record is outside main RAM is passed over");
			lost.item = 0x2000;
			PutNode(0, lost);
			check(got(search(), 2, 0), "item: so is one whose record is in main RAM but not readable");
			PutNode(1, herb);
			PutNode(0, herb);
			TestPut(kTestHead, 4, NodeAddress(0));
			lost.next = -1;
			PutNode(0, lost);
			check(!search().found && search().fault == Fault::None, "item: with nothing else, nothing is in reach");
			PutNode(0, herb);
		}

		// --- Hand targeting: the Handgun's volume against a hand point (EvaluateHandTarget). ---
		{
			const auto hand_at = [&](double x, double y, double z) {
				const double hand[3] = {x, y, z};
				return EvaluateHandTarget(p, kTestRecord, hand, TestRead);
			};
			// The character stands where the herb test had it (z 13000, facing +z): the game's own search reaches the node.
			PutCharacter(9602, 137, 13000, 0, 0.0f);
			TestPut(kTestHead, 4, NodeAddress(0));
			NodeSpec gun = herb; // x 9581-9622, z 13052-13093, y 100-200, priority 0
			gun.item = 2;        // the Handgun (id 0) in the table above
			PutNode(0, herb);
			check(!hand_at(9600, 150, 13060).found, "hand target: a list with only a Green Herb has no Handgun target");
			PutNode(0, gun);
			HandTarget ht = hand_at(9600, 150, 13060);
			check(ht.found && ht.node == NodeAddress(0) && ht.item_id == 0 && ht.index == 2 && ht.fault == Fault::None && ht.walked == 1,
				"hand target: the Handgun's node is found, with its record and the walk");
			check(ht.inside && ht.horizontal_cm == 0.0 && ht.vertical_cm == 0.0, "hand target: in the quad and the range is inside");
			check(std::fabs(ht.centre[0] - 9601.5) < 1.0e-9 && std::fabs(ht.centre[1] - 150.0) < 1.0e-9 &&
					  std::fabs(ht.centre[2] - 13072.5) < 1.0e-9,
				"hand target: the volume's middle is the corners' mean and the range's middle");
			check(ht.y_min == 100.0 && ht.y_max == 200.0 && ht.quad_x[0] == 9581.0 && ht.quad_z[2] == 13093.0,
				"hand target: the quad and the range as the node holds them");
			ht = hand_at(9650, 150, 13060);
			check(ht.found && !ht.inside && std::fabs(ht.horizontal_cm - 28.0) < 1.0e-9 && ht.vertical_cm == 0.0,
				"hand target: 28 cm outside an edge is 28 cm horizontal, not inside");
			ht = hand_at(9632, 150, 13102);
			check(!ht.inside && std::fabs(ht.horizontal_cm - std::sqrt(181.0)) < 1.0e-9, "hand target: past a corner it is the distance to the corner");
			ht = hand_at(9600, 210, 13060);
			check(!ht.inside && ht.horizontal_cm == 0.0 && std::fabs(ht.vertical_cm - 10.0) < 1.0e-9, "hand target: 10 cm over the range");
			ht = hand_at(9600, 90, 13060);
			check(!ht.inside && std::fabs(ht.vertical_cm - 10.0) < 1.0e-9, "hand target: 10 cm under the range");
			check(hand_at(9600, 100, 13060).inside && hand_at(9600, 200, 13060).inside, "hand target: the range's ends are inside (no margin)");
			check(hand_at(9622, 150, 13060).inside, "hand target: a point on the quad's edge is inside");
			check(!hand_at(9622.5, 150, 13060).inside, "hand target: half a centimetre past the edge is not");
			const double nan_hand[3] = {std::nan(""), 150.0, 13060.0};
			check(!EvaluateHandTarget(p, kTestRecord, nan_hand, TestRead).found, "hand target: a hand that is not a number finds nothing");

			// Only item id 0: a Green Herb node on the hand does not count, and the Handgun's farther node does.
			{
				NodeSpec near_herb = herb;
				near_herb.next = 1;
				PutNode(0, near_herb);
				PutNode(1, gun);
				ht = hand_at(9600, 150, 13060);
				check(ht.found && ht.node == NodeAddress(1) && ht.item_id == 0, "hand target: only Handgun nodes count, the herb's is passed over");
				// Two Handgun nodes: the nearer to the hand.
				NodeSpec far_gun = gun;
				Rect(far_gun, 0, 0, 10, 10);
				far_gun.next = 1;
				PutNode(0, far_gun);
				ht = hand_at(9600, 150, 13060);
				check(ht.found && ht.node == NodeAddress(1), "hand target: of two Handgun nodes the nearer one");
				ht = hand_at(5, 150, 5);
				check(ht.found && ht.node == NodeAddress(0) && ht.inside, "hand target: and the other when the hand is there");
			}

			// The node filters are the pickup cue's.
			const auto gun_variant = [&](const char* name, const NodeSpec& spec, bool want) {
				PutNode(0, spec);
				check(hand_at(9600, 150, 13060).found == want, name);
			};
			NodeSpec vspec = gun;
			gun_variant("hand target: the plain Handgun node counts", vspec, true);
			vspec.type = 0x81;
			gun_variant("hand target: another trigger type does not", vspec, false);
			vspec = gun;
			vspec.mode = 1;
			gun_variant("hand target: another search mode does not", vspec, false);
			vspec = gun;
			vspec.enabled = 0;
			gun_variant("hand target: a disabled node does not", vspec, false);
			vspec = gun;
			vspec.busy = 1;
			gun_variant("hand target: a busy node does not", vspec, false);
			vspec = gun;
			vspec.item = 0x10000000;
			gun_variant("hand target: a node whose record does not read does not", vspec, false);

			// Does the game's own forward search pick the same node? Both nodes cover the probe; the lower priority wins, the
			// first in the list on a tie.
			NodeSpec gun_first = gun;
			gun_first.next = 1;
			NodeSpec herb_second = herb;
			PutNode(0, gun_first);
			PutNode(1, herb_second);
			ht = hand_at(9600, 150, 13060);
			check(ht.found && ht.node == NodeAddress(0) && ht.forward_agrees, "hand target: the game's search picks the same node (a tie, the first in the list)");
			gun_first.priority = 3;
			PutNode(0, gun_first);
			ht = hand_at(9600, 150, 13060);
			check(ht.found && ht.node == NodeAddress(0) && !ht.forward_agrees,
				"hand target: another node wins the game's search (a lower priority number): does not agree");
			PutNode(0, herb);
		}

		// --- Once per approach. ---
		{
			Tracker t;
			Reach x;
			x.found = true;
			x.index = 1;
			x.item_id = 300;
			Reach y = x;
			y.index = 2;
			y.item_id = 0;
			Reach z = x; // the same id, another record
			z.index = 3;
			const Reach none{};
			Update u = Step(t, x, 1000);
			check(u.announce && u.changed && u.in_reach && u.item_id == 300 && u.index == 1, "once: the first sight is announced");
			u = Step(t, x, 1033);
			check(!u.announce && !u.changed, "once: staying in reach is not announced again, and is no change");
			u = Step(t, none, 1066);
			check(!u.announce && u.changed && !u.in_reach, "once: a leave is a change, not a cue");
			u = Step(t, x, 1100);
			check(!u.announce && u.changed, "once: back after 0.03 s is no new approach");
			Step(t, none, 2000);
			Step(t, none, 2200);
			Step(t, none, 2400);
			u = Step(t, x, 2450);
			check(!u.announce, "once: back after 0.45 s out is still the same approach");
			Step(t, none, 3000);
			Step(t, none, 3300);
			Step(t, none, 3499);
			u = Step(t, none, 3500);
			check(!u.announce, "once: leaving is never a cue");
			u = Step(t, x, 3600);
			check(u.announce, "once: back after 0.5 s out is a new approach");
			u = Step(t, y, 4300);
			check(u.announce && u.item_id == 0 && u.changed, "once: sliding to another item is announced");
			u = Step(t, z, 5000);
			check(u.announce && u.item_id == 300 && u.index == 3, "once: the same id in another record is another item");
			u = Step(t, x, 6000);
			check(u.announce, "once: and back to the first one");
			u = Step(t, y, 6100);
			check(!u.announce && u.changed, "once: a switch inside 0.6 s of the last cue is held back (still a change)");
			u = Step(t, y, 6300);
			check(!u.announce, "once: and still held back");
			u = Step(t, y, 6650);
			check(u.announce && u.item_id == 0, "once: and announced once the gap has passed");
			u = Step(t, y, 6700);
			check(!u.announce, "once: and not again");
			Step(t, none, 7000);
			u = Step(t, y, 7100);
			check(!u.announce, "once: out 0.1 s and back is the same approach");
			Step(t, none, 7200);
			Step(t, none, 7650);
			u = Step(t, y, 7700);
			check(!u.announce, "once: the out time restarts after each return (0.45 s again)");
			Tracker fresh;
			u = Step(fresh, y, 7800);
			check(u.announce, "once: a fresh tracker (a state load, a new arm) announces what is in reach");
		}

		// --- The cue itself. ---
		{
			std::string text;
			check(ToastFor(400, &text) && text == "Kevin's 45 Auto  (A)", "toast: \"Kevin's 45 Auto  (A)\"");
			check(ToastFor(0, &text) && text == "Handgun  (A)", "toast: \"Handgun  (A)\"");
			check(ToastFor(300, &text) && text == "Green Herb  (A)", "toast: \"Green Herb  (A)\"");
			check(!ToastFor(12345, &text), "toast: an id ItemData does not know has no cue");
			bool fits = ItemData::Count() > 0;
			for (u32 i = 0; i < ItemData::Count(); i++)
			{
				std::string t;
				fits = fits && ToastFor(ItemData::At(i).id, &t) && t.size() <= 48;
			}
			check(fits, "toast: every item name plus the button fits the toast's 48 characters");
			check(kTickAmp <= 0.35f && kTickSeconds <= 0.03f && kToastSeconds <= 2.0f, "cue: a light tick and a short toast");
		}

		// --- The shipped File #1 block. ---
		{
			const ProfileDB::Profile* file1 = ProfileDB::Lookup("SLPM-65428", 0);
			check(file1 && file1->reach.has_value(), "profile: File #1 has a reach block");
			if (file1 && file1->reach.has_value())
			{
				const ProfileDB::ReachParams& r = file1->reach.value();
				check(r.list_head == 0x23BD90 && r.next_offset == 0x28 && r.quad_offset == 0x00 && r.y_min_offset == 0x20 &&
						  r.y_max_offset == 0x24 && r.type_offset == 0x30 && r.mode_offset == 0x31 && r.priority_offset == 0x32 &&
						  r.enabled_offset == 0x3C && r.busy_offset == 0x3D && r.item_offset == 0x40,
					"profile: File #1's list head 0x23BD90 and node offsets");
				check(r.accept_type == 0x80 && r.accept_mode == 0 && r.item_table == 0x3967D0 && r.item_stride == 60 &&
						  r.item_id_offset == 0x06,
					"profile: File #1 accepts type 0x80 mode 0 and names items from 0x3967D0");
				check(r.position_offset == 0x38 && r.heading_offset == 0x92 && r.reach_offset == 0x1B4 && r.ahead == 46.0f &&
						  r.radius == 50.0f && r.above == 200.0f && r.max_nodes >= 64,
					"profile: File #1's probe (+0x38, +0x92, +0x1B4, 46 ahead, radius 50, 200 above)");
				check(r.has_hand && r.hand_item == 0 && r.hand_centre[0] == 8493.5f && r.hand_centre[1] == 140.0f &&
						  r.hand_centre[2] == 12922.5f && r.hand_tolerance == 10.0f && r.hand_range == 300.0f &&
						  r.hand_keep == 320.0f && r.hand_near == 30.0f,
					"profile: File #1's reach.hand is the J's Bar Handgun (item 0 at (8493.5, 140, 12922.5), 10 cm, 300/320, 30)");
			}
		}

		if (failures == 0)
			Console.WriteLn(Color_StrongGreen, "(VR) Pickup cue self-test: passed (%d checks: the herb's measured reach, heights, "
											   "node filters, priority, headings, quad shapes, unreadable data, the walk's loop and "
											   "cap, hand targeting's Handgun volume, once per approach, the toast, the shipped block).", checks);
		else
			Console.WriteLn(Color_StrongRed, "(VR) Pickup cue self-test FAIL: %s (%d of %d checks failed).", first, failures, checks);
		return failures == 0;
	}
}
