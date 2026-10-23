// SPDX-FileCopyrightText: 2026 PenguinScreen2 contributors
// SPDX-License-Identifier: GPL-3.0

#include "VR/HandPickup.h"
#include "VR/HeadInWall.h"

#include "VR/CameraDriver.h"
#include "VR/ItemData.h"
#include "VR/ItemReach.h"
#include "VR/SpatialControls.h"
#include "VR/VRInput.h"
#include "VR/VRProfileDB.h"
#include "VR/ZoneMarkers.h"

#include "Config.h"
#include "Memory.h"
#include "MemoryTypes.h"
#include "VMManager.h"

#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace VR::HandPickup
{
	const char* PhaseName(Phase p)
	{
		switch (p)
		{
			case Phase::Idle: return "idle";
			case Phase::Armed: return "armed";
			case Phase::Reaching: return "reaching";
			case Phase::Prompt: return "prompt";
			case Phase::Steering: return "steering";
			case Phase::Confirming: return "confirming";
		}
		return "?";
	}

	int DpadTowardFree(int from, const u8 cells[4], u32 cell_count)
	{
		const int n = static_cast<int>(std::min<u32>(cell_count, 4));
		if (from < 0 || from >= n)
			return -1;
		int best = -1;
		int best_d = 0;
		for (int k = 0; k < n; k++)
		{
			if (cells[k] != 0)
				continue;
			const int d = std::abs((k & 1) - (from & 1)) + std::abs((k >> 1) - (from >> 1));
			if (best < 0 || d < best_d)
			{
				best = k;
				best_d = d;
			}
		}
		if (best < 0 || best == from)
			return -1;
		// Columns first (0 <-> 1, 2 <-> 3), then rows (0 <-> 2, 1 <-> 3): never right of column 1, never down from row 1.
		if ((best & 1) != (from & 1))
			return (from & 1) == 0 ? 3 : 2;
		return (from >> 1) == 0 ? 1 : 0;
	}

	namespace
	{
		bool AnyFree(const In& in)
		{
			for (u32 k = 0; k < std::min<u32>(in.cell_count, 4); k++)
			{
				if (in.cells[k] == 0)
					return true;
			}
			return false;
		}

		void ToIdle(State& st)
		{
			st.phase = Phase::Idle;
			st.hand = -1;
			st.held = false;
			st.checked = false;
			st.steers = 0;
			st.steer_from = -1;
			st.steer_to = -1;
			st.steer_moved = false;
			st.cursor_seen = false;
		}

		bool CursorMoved(const In& in, const State& st)
		{
			return st.cursor_seen &&
			       (in.cursor_grid != st.cursor_grid || in.cursor_node != st.cursor_node || in.cursor_index != st.cursor_index);
		}

		void SeeCursor(const In& in, State& st)
		{
			st.cursor_seen = true;
			st.cursor_grid = in.cursor_grid;
			st.cursor_node = in.cursor_node;
			st.cursor_index = in.cursor_index;
			st.cursor_since = in.now_ms;
		}

		// The cell a D-pad press takes the cursor to in the 2 x 2 grid (0 1 / 2 3), -1 off the grid.
		int SteerTarget(int from, int dpad)
		{
			if (from < 0 || from > 3)
				return -1;
			switch (dpad)
			{
				case 0: return (from >> 1) == 1 ? from - 2 : -1;
				case 1: return (from >> 1) == 0 ? from + 2 : -1;
				case 2: return (from & 1) == 1 ? from - 1 : -1;
				case 3: return (from & 1) == 0 ? from + 1 : -1;
				default: return -1;
			}
		}
	}

	const char* PressFault(PressKind kind, const PressView& v, const PressExpect& e)
	{
		switch (kind)
		{
			case PressKind::None:
				return nullptr;
			case PressKind::Arm:
				if (v.screen_flag || v.overlay_up || v.menu_mode)
					return "a game screen rose under the arming Circle";
				return nullptr;
			case PressKind::Steer:
				if (!v.take_prompt)
					return "the take-it prompt went away under the D-pad press";
				if (v.player_input)
					return "the player pressed something under the D-pad press";
				return nullptr;
			case PressKind::Confirm:
				if (!v.take_prompt)
					return "the take-it prompt went away under the confirm";
				if (v.player_input)
					return "the player pressed something under the confirm";
				if (v.grid != 1 || v.node != e.node || v.index != e.index)
					return "the prompt's cursor moved under the confirm";
				if (v.cell != 0)
					return "the cell under the cursor is no longer free";
				return nullptr;
		}
		return nullptr;
	}

	Out Step(const Tuning& t, const In& in, State& st)
	{
		Out o;
		// The squeeze latch, both hands, every vsync: an edge needs the grip open first.
		bool edge[2] = {};
		for (int h = 0; h < 2; h++)
		{
			const float g = std::isfinite(in.grip[h]) ? in.grip[h] : 0.0f;
			if (!in.enabled)
			{
				// Off (or not first person): a grip already closed when it comes back must open first.
				st.released[h] = false;
				continue;
			}
			if (g < kGripOff)
				st.released[h] = true;
			else if (g >= kGripOn && st.released[h])
			{
				st.released[h] = false;
				edge[h] = true;
			}
		}

		// Night review M1: when a screen flag was last up and the player's own Circle last down (kept through ToIdle).
		if (in.screen_up || in.screen_flag)
		{
			st.screen_seen = true;
			st.screen_ms = in.now_ms;
		}
		if (in.player_circle)
		{
			st.own_circle_seen = true;
			st.own_circle_at = in.now_ms;
		}

		if (!in.enabled)
		{
			if (st.phase != Phase::Idle || in.block_armed)
			{
				o.clear = true;
				if (st.phase != Phase::Idle)
					o.event = "stopped (off, not first person, not offline or the cave is out): nothing more is pressed";
			}
			ToIdle(st);
			return o;
		}

		// The arming hand's grip, held all the way from its squeeze.
		if (st.phase != Phase::Idle && st.hand >= 0 && !(std::isfinite(in.grip[st.hand]) && in.grip[st.hand] >= kGripOff))
			st.held = false;
		const u64 since = in.now_ms - st.t0;

		switch (st.phase)
		{
			case Phase::Idle:
			{
				if (in.block_armed)
				{
					// Not ours to keep (a loaded state's, or one left by a stop): cleared.
					o.clear = true;
					return o;
				}
				for (int h : {1, 0})
				{
					if (!edge[h] || !in.target_highlighted || in.target_hand != h)
						continue;
					if (in.screen_up)
						o.event = "squeeze with a game screen up: nothing armed";
					else if (in.screen_flag)
						o.event = "squeeze while a game screen is opening (the screen flag is up): nothing armed";
					else if (st.screen_seen && in.now_ms - st.screen_ms < t.screen_clear_ms)
						o.event = "squeeze just after a game screen (the flags not clear long enough): nothing armed";
					else if (in.busy)
						o.event = "squeeze while the character is busy (a pickup, an examine or another action under way): nothing armed";
					else if (st.own_circle_seen && in.now_ms - st.own_circle_at < t.own_circle_ms)
						o.event = "squeeze just after your own Circle (a pickup of yours may be under way): nothing armed";
					else if (in.weapon_raised)
						o.event = "squeeze with the weapon raised (Circle fires): nothing armed";
					else if (in.hand_in_zone[h])
						o.event = "squeeze inside a body zone (it is the zone's): nothing armed";
					else if (!in.guards_now)
						o.event = "squeeze during a camera cut: nothing armed";
					else
					{
						st.phase = Phase::Armed;
						st.hand = h;
						st.held = true;
						st.node = in.target_node;
						st.index = in.target_index;
						st.item = in.target_item;
						st.t0 = in.now_ms;
						o.arm = true;
						o.press_circle = true;
						o.event = "squeeze on the item: armed, Circle pressed";
					}
					break;
				}
				return o;
			}

			case Phase::Armed:
				if (in.press_dropped)
				{
					o.clear = true;
					o.stop_press = true;
					o.event = "the arming Circle was dropped (a game screen rose under it); cleared";
					ToIdle(st);
				}
				else if (in.screen_flag || in.screen_up)
				{
					o.clear = true;
					o.stop_press = true;
					o.event = "a game screen came up before the search; cleared, the Circle let go";
					ToIdle(st);
				}
				else if (!in.block_armed)
				{
					st.phase = Phase::Reaching;
					st.t0 = in.now_ms;
					o.event = "the game searched at the hand (the cave took the word)";
				}
				else if (since >= t.arm_timeout_ms)
				{
					o.clear = true;
					o.event = "timeout: the press did not reach the game's pickup search; cleared";
					ToIdle(st);
				}
				else if (!in.target_highlighted || in.target_node != st.node)
				{
					o.clear = true;
					o.event = "the hand left the item before the search; cleared";
					ToIdle(st);
				}
				return o;

			case Phase::Reaching:
				if (in.take_prompt)
				{
					st.phase = Phase::Prompt;
					st.t0 = in.now_ms;
					st.prompt_t0 = in.now_ms;
					st.checked = false;
					st.steers = 0;
					SeeCursor(in, st);
				}
				else if (in.screen_up)
				{
					o.event = "another screen came up after the search (not a take-it prompt): nothing confirmed";
					ToIdle(st);
				}
				else if (since >= t.reach_timeout_ms)
				{
					o.event = "no take-it prompt came after the search: nothing confirmed";
					ToIdle(st);
				}
				return o;

			case Phase::Prompt:
				if (!in.take_prompt)
				{
					o.event = in.screen_up ? "the screen is no longer a take-it prompt: nothing confirmed" :
					                         "the prompt closed before the build confirmed it (answered by the player)";
					ToIdle(st);
					return o;
				}
				// Every vsync, up to the press: the prompt still shows this item and a free cell is still there.
				{
					if (in.prompt_record != st.index || in.prompt_id != st.item || in.result != st.index)
					{
						o.event = "the take-it prompt is not for the item under the hand (another item, a partner's hand-over): "
						          "nothing confirmed";
						ToIdle(st);
						return o;
					}
					if (!AnyFree(in))
					{
						o.event = "inventory full: the game's swap is the player's, nothing confirmed";
						ToIdle(st);
						return o;
					}
					st.checked = true;
				}
				if (in.press_dropped)
				{
					o.stop_press = true;
					o.event = "a press of ours was dropped on the prompt: nothing more is pressed";
					ToIdle(st);
					return o;
				}
				// Night review M2: the cursor struct moving is never ours here (the game placing it at the prompt's start
				// aside): the prompt is the player's.
				if (CursorMoved(in, st))
				{
					if (in.now_ms - st.prompt_t0 >= t.cursor_place_ms)
					{
						o.stop_press = true;
						o.event = "the prompt's cursor moved (not by a press of ours): nothing confirmed";
						ToIdle(st);
						return o;
					}
					SeeCursor(in, st);
				}
				if (in.player_buttons)
				{
					o.event = "the player pressed a button on the prompt: it is theirs";
					ToIdle(st);
					return o;
				}
				if (!st.held)
				{
					o.event = "the grip was let go before the prompt: the prompt is the player's";
					ToIdle(st);
					return o;
				}
				if (since < t.settle_ms)
					return o;
				if (!in.cursor_in_grid || in.cursor_cell < 0 || in.cursor_cell >= static_cast<int>(std::min<u32>(in.cell_count, 4)))
				{
					o.event = "the prompt's cursor is not on one of your own cells: nothing confirmed";
					ToIdle(st);
					return o;
				}
				if (in.cells[in.cursor_cell] == 0)
				{
					if (in.now_ms - st.cursor_since < t.cursor_still_ms)
						return o; // the cursor still this long first
					st.phase = Phase::Confirming;
					st.t0 = in.now_ms;
					o.press_circle = true;
					o.confirm = true;
					o.event = "take-it prompt for the item, grip held, cursor on a free cell: confirmed (Circle)";
					return o;
				}
				if (st.steers >= t.max_steer)
				{
					o.event = "the cursor did not reach a free cell: nothing confirmed";
					ToIdle(st);
					return o;
				}
				o.press_dpad = DpadTowardFree(in.cursor_cell, in.cells, in.cell_count);
				if (o.press_dpad < 0)
				{
					o.event = "no way to a free cell from the cursor: nothing confirmed";
					ToIdle(st);
					return o;
				}
				st.phase = Phase::Steering;
				st.t0 = in.now_ms;
				st.steer_from = in.cursor_cell;
				st.steer_to = SteerTarget(in.cursor_cell, o.press_dpad);
				st.steer_moved = false;
				st.steers++;
				o.event = "the prompt's cursor is on a full cell: one D-pad press toward a free one";
				return o;

			case Phase::Steering:
				if (!in.take_prompt)
				{
					o.event = "the prompt closed while steering: nothing confirmed";
					ToIdle(st);
				}
				else if (in.press_dropped)
				{
					o.stop_press = true;
					o.event = "the D-pad press was dropped: nothing confirmed";
					ToIdle(st);
				}
				else if (in.player_buttons)
				{
					o.stop_press = true;
					o.event = "the player pressed a button on the prompt: it is theirs";
					ToIdle(st);
				}
				else if (!st.held)
				{
					o.stop_press = true;
					o.event = "the grip was let go: the prompt is the player's";
					ToIdle(st);
				}
				else
				{
					// Night review M2: one move, to the cell the press sends it to; any other move is not ours.
					if (CursorMoved(in, st))
					{
						if (st.steer_moved || in.cursor_cell != st.steer_to)
						{
							o.stop_press = true;
							o.event = "the prompt's cursor went where the D-pad press did not send it: nothing confirmed";
							ToIdle(st);
							return o;
						}
						st.steer_moved = true;
						SeeCursor(in, st);
					}
					if (st.steer_moved && since >= t.press_ms)
					{
						// Back to the prompt's checks with the settle already served (a short one for the cursor's draw).
						st.phase = Phase::Prompt;
						st.t0 = in.now_ms - (t.settle_ms > t.press_ms ? t.settle_ms - t.press_ms : 0);
					}
					else if (since >= t.steer_wait_ms)
					{
						o.event = "the cursor did not answer the D-pad: nothing confirmed";
						ToIdle(st);
					}
				}
				return o;

			case Phase::Confirming:
				if (in.press_dropped)
				{
					o.stop_press = true;
					o.event = "the confirm was dropped (the cursor, the prompt or the cell changed, or the player pressed): "
					          "nothing more is pressed";
					ToIdle(st);
					return o;
				}
				if (since < t.press_ms && in.take_prompt && (CursorMoved(in, st) || in.player_buttons))
				{
					o.stop_press = true;
					o.event = "the cursor moved or the player pressed under the confirm: the Circle let go";
					ToIdle(st);
					return o;
				}
				if (!in.screen_up)
				{
					for (u32 k = 0; k < std::min<u32>(in.cell_count, 4); k++)
					{
						if (in.cells[k] != 0 && in.cells[k] == st.index)
						{
							o.taken_cell = static_cast<int>(k);
							o.event = "taken: the item is in your cells";
							ToIdle(st);
							return o;
						}
					}
					if (since >= t.close_timeout_ms + t.cell_wait_ms)
					{
						o.event = "the prompt closed but the item is not in your cells";
						ToIdle(st);
					}
				}
				else if (since >= t.close_timeout_ms)
				{
					o.event = "the prompt did not close after the confirm: nothing more is pressed";
					ToIdle(st);
				}
				return o;
		}
		return o;
	}

	// --- The emulator side (CPU thread) ---
	namespace
	{
		constexpr u32 kBlockWords = 8;
		constexpr u32 kTagHigh = 0x4850u; // +0x0C's high half: the block is ours

		State s_state;
		bool s_load_pending = false;    // a state was loaded: the block's armed word goes on the next poll
		bool s_block_ok = false;        // the block was checked (zero, or ours) this enable
		bool s_block_refused = false;   // ... and found to be someone else's (said once)
		bool s_enabled_said = false;
		const char* s_wait = nullptr;
		u32 s_seq = 0;
		u32 s_record = 0;               // the record the block was armed for (logs)
		std::atomic<u64> s_circle_until{0};
		std::atomic<u64> s_dpad_until{0};
		std::atomic<int> s_dpad_dir{-1};
		// Night review M1 / M2: what the press out now is (re-checked at every input poll), whether the pad is the
		// prompt's alone (Prompt, Steering), and a press CurrentPress dropped since the last vsync.
		std::atomic<int> s_press_kind{static_cast<int>(PressKind::None)};
		std::atomic<bool> s_exclusive{false};
		std::atomic<bool> s_dropped{false};
		std::atomic<const char*> s_drop_why{nullptr};
		// The guest addresses CurrentPress re-reads (written on the CPU thread before the press starts; CurrentPress runs
		// on the CPU thread's input poll too).
		struct PressGuard
		{
			bool valid = false;
			u32 overlay = 0, which = 0, sub = 0, grid = 0, node = 0, index = 0;
			bool has_screen_flag = false;
			u32 screen_flag = 0;
			u32 cell = 0; // the record's cell the confirm was checked on
			PressExpect expect;
		};
		PressGuard s_guard;

		// The player's own input on either controller: any button, the trigger, a D-pad direction or a stick pushed past
		// half (in menu mode the right stick drives the D-pad; the left stick moves menu cursors too).
		bool PlayerInput(const VRInputSnapshot& snap)
		{
			if (snap.generation == 0 || !snap.actions_active)
				return false;
			bool any = false;
			for (const VRHandState& hs : snap.hands)
			{
				any = any || hs.a || hs.b || hs.x || hs.y || hs.menu || hs.view || hs.bumper || hs.thumbstick_click ||
				      hs.dpad_up || hs.dpad_down || hs.dpad_left || hs.dpad_right || hs.trigger >= 0.5f ||
				      std::fabs(hs.thumbstick_x) >= 0.5f || std::fabs(hs.thumbstick_y) >= 0.5f;
			}
			return any;
		}

		// The player's own Circle (the VR Gamepad: ButtonA and RightTriggerPress).
		bool PlayerCircle(const VRInputSnapshot& snap)
		{
			if (snap.generation == 0 || !snap.actions_active)
				return false;
			const VRHandState& r = snap.hands[VRInputSnapshot::RIGHT];
			return r.a || r.trigger >= 0.5f;
		}

		bool Read(u32 address, u8 width, u32* v)
		{
			if (static_cast<u64>(address) + width > static_cast<u64>(Ps2MemSize::MainRam) || (address & (width - 1u)) != 0)
				return false;
			*v = width == 1 ? memRead8(address) : width == 2 ? memRead16(address) : memRead32(address);
			return true;
		}

		u32 Read32(u32 a)
		{
			u32 v = 0;
			return Read(a, 4, &v) ? v : 0;
		}

		void Wait(const char* why)
		{
			if (why == s_wait)
				return;
			s_wait = why;
			if (why)
				Console.WriteLn("(VR) Hand pickup: not armed: %s", why);
		}

		// The block is ours to write: every word zero, or our tag in +0x0C.
		bool BlockOurs(const ProfileDB::ReachParams& p)
		{
			bool zero = true;
			for (u32 k = 0; k < kBlockWords; k++)
				zero = zero && Read32(p.pickup_block + 4 * k) == 0;
			return zero || (Read32(p.pickup_block + 0x0C) >> 16) == kTagHigh;
		}

		void ClearWord(const ProfileDB::ReachParams& p)
		{
			if (Read32(p.pickup_block) != 0 && (Read32(p.pickup_block) == p.pickup_magic || BlockOurs(p)))
				memWrite32(p.pickup_block, 0);
		}

		void ArmBlock(const ProfileDB::ReachParams& p, u32 record, const float point[3])
		{
			u32 counter = 0;
			Read(p.pickup_counter, 1, &counter);
			memWrite32(p.pickup_block, 0);
			memWrite32(p.pickup_block + 0x04, counter & 0xFFu);
			memWrite32(p.pickup_block + 0x08, record);
			s_seq = (s_seq + 1) & 0xFFFFu;
			memWrite32(p.pickup_block + 0x0C, (kTagHigh << 16) | s_seq);
			for (int k = 0; k < 3; k++)
			{
				u32 bits = 0;
				std::memcpy(&bits, &point[k], 4);
				memWrite32(p.pickup_block + 0x10 + 4u * static_cast<u32>(k), bits);
			}
			memWrite32(p.pickup_block + 0x1C, 0);
			// The armed word last, so the cave never sees it with the rest half written.
			memWrite32(p.pickup_block, p.pickup_magic);
		}

		void StartPress(bool circle, int dpad, u64 now, u64 ms, PressKind kind)
		{
			s_press_kind.store(static_cast<int>(kind), std::memory_order_release);
			if (circle)
				s_circle_until.store(now + ms, std::memory_order_release);
			if (dpad >= 0 && dpad < 4)
			{
				s_dpad_dir.store(dpad, std::memory_order_release);
				s_dpad_until.store(now + ms, std::memory_order_release);
			}
		}

		void StopPresses()
		{
			s_press_kind.store(static_cast<int>(PressKind::None), std::memory_order_release);
			s_circle_until.store(0, std::memory_order_release);
			s_dpad_until.store(0, std::memory_order_release);
			s_dpad_dir.store(-1, std::memory_order_release);
		}
	}

	void Poll(const ProfileDB::Profile* profile, const ItemReach::Live& live)
	{
		const ProfileDB::ReachParams* p =
			(profile && profile->reach.has_value() && profile->reach->has_pickup) ? &profile->reach.value() : nullptr;
		if (!p || !VMManager::HasValidVM())
		{
			if (s_state.phase != Phase::Idle)
				Console.WriteLn("(VR) Hand pickup: stopped (no profile block or no game)");
			s_state = State{};
			s_block_ok = false;
			StopPresses();
			s_exclusive.store(false, std::memory_order_release);
			s_dropped.store(false, std::memory_order_release);
			return;
		}
		const u64 now = CameraDriver::SteadyNowMs();

		// A loaded state may hold an armed word of its own: gone before anything else.
		if (s_load_pending)
		{
			s_load_pending = false;
			if (Read32(p->pickup_block) == p->pickup_magic && BlockOurs(*p))
			{
				memWrite32(p->pickup_block, 0);
				Console.WriteLn("(VR) Hand pickup: a loaded state's armed word cleared");
			}
		}

		const bool cave_in = Read32(p->pickup_site) == p->pickup_site_on && Read32(p->pickup_cave) == p->pickup_cave_first;
		const bool want = EmuConfig.VR.HandPickup && live.armed && live.hand_game && live.net_offline;
		if (want && !s_block_ok && !s_block_refused)
		{
			if (BlockOurs(*p))
				s_block_ok = true;
			else
			{
				s_block_refused = true;
				Console.Warning("(VR) Hand pickup: the block at 0x%X holds someone else's words: HandPickup stays off until the "
								"next game", p->pickup_block);
			}
		}
		if (!want)
		{
			s_block_ok = false;
			s_block_refused = false;
		}

		In in;
		in.now_ms = now;
		in.enabled = want && cave_in && s_block_ok;
		if (!EmuConfig.VR.HandPickup)
			Wait(nullptr);
		else if (!live.armed)
			Wait("first person is not armed");
		else if (!live.hand_game)
			Wait("not File #1");
		else if (!live.net_offline)
			Wait("not offline");
		else if (!cave_in)
			Wait("the cave at the Circle search is not in (yet)");
		else if (!s_block_ok)
			Wait("the host block is not ours");
		else
			Wait(nullptr);
		if (in.enabled && !s_enabled_said)
		{
			s_enabled_said = true;
			Console.WriteLn("(VR) Hand pickup: on (offline File #1): squeeze on a highlighted item to take it; the cave at 0x%X "
							"is in, block 0x%X", p->pickup_site, p->pickup_block);
		}
		else if (!in.enabled)
			s_enabled_said = false;

		in.guards_now = live.guards_now;
		u32 overlay = 0, which = 0, sub = 0, grid = 0, node = 0, index = 0;
		Read(p->pickup_overlay, 4, &overlay);
		Read(p->pickup_which, 1, &which);
		Read(p->pickup_sub, 1, &sub);
		in.screen_up = overlay == 1 || live.screen_up;
		in.take_prompt = overlay == 1 && which == 1 && sub == 2;
		// Night review M1: melee.screenFlag (0x48BF7E) rises 0.55-0.65 s before the overlay on the item screen and the map.
		const bool has_screen_flag = profile->melee.has_value() && profile->melee->screen_flag.has_value();
		if (has_screen_flag)
		{
			u32 v = 0;
			in.screen_flag = !Read(profile->melee->screen_flag.value(), 1, &v) || v != 0;
		}
		in.weapon_raised = live.weapon_raised;
		const VRInputSnapshot snap = GetInputSnapshot();
		const bool hands_live = snap.generation != 0 && snap.actions_active;
		for (int h = 0; h < 2; h++)
			in.grip[h] = hands_live ? snap.hands[h].grip : 0.0f;
		in.player_buttons = PlayerInput(snap);
		in.player_circle = PlayerCircle(snap);
		in.press_dropped = s_dropped.exchange(false, std::memory_order_acq_rel);
		if (in.press_dropped)
		{
			const char* why = s_drop_why.load(std::memory_order_acquire);
			Console.WriteLn("(VR) Hand pickup: a press of ours was dropped as it went out: %s", why ? why : "?");
		}
		for (ZoneMarkers::Zone z : {ZoneMarkers::Zone::Holster, ZoneMarkers::Zone::Belt, ZoneMarkers::Zone::Backpack})
		{
			ZoneMarkers::ZoneView v;
			if (ZoneMarkers::GetZone(z, &v))
			{
				in.hand_in_zone[0] = in.hand_in_zone[0] || v.inside[0];
				in.hand_in_zone[1] = in.hand_in_zone[1] || v.inside[1];
			}
		}
		const ItemReach::PickupTarget tg = ItemReach::CurrentPickup();
		in.target_highlighted = tg.valid && tg.highlighted && tg.point_ok;
		// Night review m10: not through a wall. The line from the character's head point to the search point must not
		// cross a wall that blocks the player (HeadInWall's copy of the player collision; no check without a fresh copy).
		{
			static bool s_wall_said = false;
			bool known = false;
			const bool walled = in.target_highlighted && HeadInWall::HeadToPointBlocked(VMManager::GetDiscCRC(), tg.point, &known);
			if (walled)
				in.target_highlighted = false;
			if (walled && !s_wall_said)
				Console.WriteLn("(VR) Hand pickup: item %u is behind a wall from the character's head: not taken by hand.",
					static_cast<unsigned>(tg.item_id));
			s_wall_said = walled;
		}
		in.target_hand = tg.hand;
		in.target_node = tg.node;
		in.target_index = tg.index;
		in.target_item = tg.item_id;
		in.block_armed = Read32(p->pickup_block) == p->pickup_magic;
		const u32 rec = live.record;
		// Night review M1: the action byte (feedback.weapon.action, +0x54F): 0 idle, 1 walk, 3 run; anything else (9 the
		// pickup reach, an examine, a reload) is an action under way.
		if (live.armed && rec != 0 && profile->feedback.has_value() && profile->feedback->weapon_action_offset != 0)
		{
			u32 action = 0;
			in.busy = !Read(rec + profile->feedback->weapon_action_offset, 1, &action) || !(action == 0 || action == 1 || action == 3);
		}
		if (live.armed && rec != 0)
		{
			Read(p->pickup_prompt + 1, 1, &in.prompt_record);
			Read(p->pickup_prompt + 2, 2, &in.prompt_id);
			Read(rec + p->pickup_result, 1, &in.result);
			in.cell_count = std::min<u32>(p->pickup_cell_count, 4);
			for (u32 k = 0; k < in.cell_count; k++)
			{
				u32 c = 0;
				Read(rec + p->pickup_cells + k, 1, &c);
				in.cells[k] = static_cast<u8>(c);
			}
		}
		if (in.take_prompt)
		{
			Read(p->pickup_grid, 1, &grid);
			Read(p->pickup_cursor_node, 4, &node);
			Read(p->pickup_cursor_index, 2, &index);
			in.cursor_in_grid = grid == 1;
			in.cursor_grid = grid;
			in.cursor_node = node;
			in.cursor_index = index;
			for (u32 k = 0; k < in.cell_count; k++)
			{
				if (node == p->pickup_cell_nodes[k] && index == k)
					in.cursor_cell = static_cast<int>(k);
			}
		}

		static const Tuning s_tuning{};
		const Phase before = s_state.phase;
		const Out o = Step(s_tuning, in, s_state);

		if (o.clear)
			ClearWord(*p);
		if (o.arm)
		{
			s_record = rec;
			ArmBlock(*p, rec, tg.point);
		}
		if (o.stop_press)
			StopPresses();
		if (o.press_circle || o.press_dpad >= 0)
		{
			// What CurrentPress re-reads at every poll while the press is out.
			PressGuard g;
			g.valid = true;
			g.overlay = p->pickup_overlay;
			g.which = p->pickup_which;
			g.sub = p->pickup_sub;
			g.grid = p->pickup_grid;
			g.node = p->pickup_cursor_node;
			g.index = p->pickup_cursor_index;
			g.has_screen_flag = has_screen_flag;
			g.screen_flag = has_screen_flag ? profile->melee->screen_flag.value() : 0;
			g.cell = (o.confirm && in.cursor_cell >= 0) ? rec + p->pickup_cells + static_cast<u32>(in.cursor_cell) : 0;
			g.expect.node = in.cursor_node;
			g.expect.index = in.cursor_index;
			s_guard = g;
			const PressKind kind = o.confirm ? PressKind::Confirm : (o.press_dpad >= 0 ? PressKind::Steer : PressKind::Arm);
			StartPress(o.press_circle, o.press_dpad, now, s_tuning.press_ms, kind);
		}
		// Night review M2: while the prompt is checked or steered, the pad is the prompt's (the menu pointer's D-pad and
		// every other synthetic value read 0); the confirm press holds it too (CurrentPress).
		s_exclusive.store(s_state.phase == Phase::Prompt || s_state.phase == Phase::Steering, std::memory_order_release);
		if (s_state.phase == Phase::Idle && before != Phase::Idle && !o.press_circle)
			s_dpad_until.store(0, std::memory_order_release);
		if (o.event)
		{
			if (o.arm)
				Console.WriteLn("(VR) Hand pickup: %s: %s hand, item %u (%s, record %u, node 0x%08X), search point (%.1f, %.1f, %.1f), "
								"record 0x%X, block 0x%X request %u; the character's own Circle would take it: %s",
					o.event, s_state.hand == 1 ? "right" : "left", static_cast<u32>(s_state.item), ItemData::Name(s_state.item),
					s_state.index, s_state.node, static_cast<double>(tg.point[0]), static_cast<double>(tg.point[1]),
					static_cast<double>(tg.point[2]), rec, p->pickup_block, s_seq, tg.forward_agrees ? "yes" : "no");
			else if (o.taken_cell >= 0)
				Console.WriteLn("(VR) Hand pickup: %s (cell %d holds record %u, %s)", o.event, o.taken_cell, s_state.index,
					ItemData::Name(s_state.item));
			else
				Console.WriteLn("(VR) Hand pickup: %s [%s -> %s; prompt %s (record %u, item %u), result %u, cells %u %u %u %u, cursor %s%d]",
					o.event, PhaseName(before), PhaseName(s_state.phase), in.take_prompt ? "up" : "no", in.prompt_record, in.prompt_id, in.result,
					in.cells[0], in.cells[1], in.cells[2], in.cells[3], in.cursor_in_grid ? "grid " : "off-grid ", in.cursor_cell);
		}
		else if (before == Phase::Reaching && s_state.phase == Phase::Prompt)
		{
			Console.WriteLn("(VR) Hand pickup: take-it prompt up: record %u item %u, search result %u (wanted %u, item %u), cells %u %u %u %u, cursor %s%d, "
							"grip %s",
				in.prompt_record, in.prompt_id, in.result, s_state.index, static_cast<u32>(s_state.item), in.cells[0], in.cells[1], in.cells[2], in.cells[3],
				in.cursor_in_grid ? "grid " : "off-grid ", in.cursor_cell, s_state.held ? "held" : "let go");
		}
	}

	void OnStateLoaded()
	{
		if (s_state.phase != Phase::Idle)
			Console.WriteLn("(VR) Hand pickup: a state was loaded: started over");
		s_state = State{};
		s_load_pending = true;
		s_block_ok = false;
		StopPresses();
		s_exclusive.store(false, std::memory_order_release);
		s_dropped.store(false, std::memory_order_release);
	}

	Press CurrentPress()
	{
		Press pr;
		const u64 now = CameraDriver::SteadyNowMs();
		pr.circle = now < s_circle_until.load(std::memory_order_acquire);
		if (now < s_dpad_until.load(std::memory_order_acquire))
			pr.dpad = s_dpad_dir.load(std::memory_order_acquire);
		const PressKind kind = static_cast<PressKind>(s_press_kind.load(std::memory_order_acquire));
		if ((pr.circle || pr.dpad >= 0) && kind != PressKind::None)
		{
			// Re-checked at the instant it goes out (night review M1 / M2): a fault drops it, and the next vsync ends the take.
			const char* why = "no game";
			if (s_guard.valid && VMManager::HasValidVM())
			{
				PressView v;
				u32 overlay = 0, which = 0, sub = 0, flag = 0, cell = 0;
				Read(s_guard.overlay, 4, &overlay);
				Read(s_guard.which, 1, &which);
				Read(s_guard.sub, 1, &sub);
				Read(s_guard.grid, 1, &v.grid);
				Read(s_guard.node, 4, &v.node);
				Read(s_guard.index, 2, &v.index);
				v.screen_flag = s_guard.has_screen_flag && (!Read(s_guard.screen_flag, 1, &flag) || flag != 0);
				v.overlay_up = overlay == 1;
				v.menu_mode = SpatialControls::MenuMode();
				v.take_prompt = overlay == 1 && which == 1 && sub == 2;
				v.cell = 0xFF;
				if (s_guard.cell != 0 && Read(s_guard.cell, 1, &cell))
					v.cell = static_cast<u8>(cell);
				else if (kind != PressKind::Confirm)
					v.cell = 0;
				v.player_input = kind != PressKind::Arm && PlayerInput(GetInputSnapshot());
				why = PressFault(kind, v, s_guard.expect);
			}
			if (why)
			{
				StopPresses();
				s_drop_why.store(why, std::memory_order_release);
				s_dropped.store(true, std::memory_order_release);
				pr.circle = false;
				pr.dpad = -1;
			}
			else
				pr.exclusive = kind != PressKind::Arm;
		}
		pr.exclusive = pr.exclusive || s_exclusive.load(std::memory_order_acquire);
		return pr;
	}
}

// The self-test: the state machine on scripted vsyncs.
namespace VR::HandPickup
{
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

		const Tuning t{};
		// A ready scene: first person offline, the right hand highlighting record 7 (node 0x1234), cells 0 and 2 full.
		const auto ready = []() {
			In in;
			in.now_ms = 1000;
			in.enabled = true;
			in.guards_now = true;
			in.target_highlighted = true;
			in.target_hand = 1;
			in.target_node = 0x1234;
			in.target_index = 7;
			in.target_item = 300;
			in.cells[0] = 3;
			in.cells[2] = 5;
			return in;
		};
		// Steps the same input forward by dt ms, n times; the last Out.
		const auto run = [&](In& in, State& st, u64 dt, int n) {
			Out o;
			for (int k = 0; k < n; k++)
			{
				in.now_ms += dt;
				const Out x = Step(t, in, st);
				if (x.arm || x.clear || x.press_circle || x.press_dpad >= 0 || x.event || x.taken_cell >= 0)
					o = x;
			}
			return o;
		};

		// --- The squeeze latch: release first, a threshold, one edge per squeeze. ---
		{
			State st;
			In in = ready();
			in.grip[1] = 1.0f;
			Out o = Step(t, in, st);
			check(!o.arm && st.phase == Phase::Idle, "latch: a grip already closed at the start arms nothing (release first)");
			in.grip[1] = 0.2f;
			Step(t, in, st);
			in.grip[1] = 0.6f;
			o = Step(t, in, st);
			check(!o.arm, "latch: 0.6 is below the squeeze threshold");
			in.grip[1] = 0.8f;
			o = Step(t, in, st);
			check(o.arm && o.press_circle && st.phase == Phase::Armed && st.index == 7 && st.hand == 1,
				"latch: open then past 0.75 arms the word and presses Circle once");
			o = Step(t, in, st);
			check(!o.arm && !o.press_circle, "latch: holding it presses nothing more");
		}
		// --- What a squeeze is refused for. ---
		{
			const auto squeeze = [&](In in) {
				State st;
				in.grip[1] = 0.0f;
				Step(t, in, st);
				in.grip[1] = 1.0f;
				in.now_ms += 16;
				return Step(t, in, st);
			};
			In in = ready();
			check(squeeze(in).arm, "refuse: the ready scene arms");
			in = ready();
			in.screen_up = true;
			check(!squeeze(in).arm, "refuse: not with a game screen up");
			in = ready();
			in.weapon_raised = true;
			check(!squeeze(in).arm, "refuse: not with the weapon raised (Circle fires)");
			in = ready();
			in.hand_in_zone[1] = true;
			check(!squeeze(in).arm, "refuse: not inside a body zone");
			in = ready();
			in.target_highlighted = false;
			check(!squeeze(in).arm, "refuse: not without a highlighted item");
			in = ready();
			in.target_hand = 0;
			check(!squeeze(in).arm, "refuse: the right grip does not take the left hand's item");
			in = ready();
			in.guards_now = false;
			check(!squeeze(in).arm, "refuse: not during a camera cut");
			in = ready();
			in.enabled = false;
			check(!squeeze(in).arm, "refuse: not while off (offline File #1, the cave in)");
			// The left hand's own item, its own grip.
			in = ready();
			in.target_hand = 0;
			State st;
			Step(t, in, st);
			in.grip[0] = 1.0f;
			in.now_ms += 16;
			const Out o = Step(t, in, st);
			check(o.arm && st.hand == 0, "left: the left grip takes the left hand's item");
		}

		// Arms the ready scene (right hand) and returns the state in Armed.
		const auto armed = [&](In& in, State& st) {
			st = State{};
			in = ready();
			Step(t, in, st);
			in.grip[1] = 1.0f;
			in.now_ms += 16;
			Step(t, in, st);
			in.block_armed = true;
		};
		// --- The armed word: the cave takes it, or the host clears it. ---
		{
			State st;
			In in;
			armed(in, st);
			Out o = run(in, st, 16, 18); // 288 ms
			check(st.phase == Phase::Armed && !o.clear, "armed: waits up to 300 ms");
			o = run(in, st, 16, 2);
			check(st.phase == Phase::Idle && o.clear, "armed: cleared on the 300 ms timeout");
			armed(in, st);
			in.screen_up = true;
			o = run(in, st, 16, 1);
			check(st.phase == Phase::Idle && o.clear, "armed: cleared when a screen comes up first");
			armed(in, st);
			in.target_highlighted = false;
			o = run(in, st, 16, 1);
			check(st.phase == Phase::Idle && o.clear, "armed: cleared when the hand leaves the item");
			armed(in, st);
			in.target_node = 0x9999;
			o = run(in, st, 16, 1);
			check(st.phase == Phase::Idle && o.clear, "armed: cleared when the hand moves to another item");
			armed(in, st);
			in.enabled = false;
			o = run(in, st, 16, 1);
			check(st.phase == Phase::Idle && o.clear, "armed: cleared when it is switched off (or goes online, or a state loads)");
			armed(in, st);
			in.grip[1] = 0.0f; // let go at once: the search still runs, but nothing will be confirmed
			in.block_armed = false;
			o = run(in, st, 16, 1);
			check(st.phase == Phase::Reaching && !o.clear, "armed: the cave taking the word moves on to the reach");
			State idle;
			In stale = ready();
			stale.block_armed = true;
			o = Step(t, stale, idle);
			check(o.clear && idle.phase == Phase::Idle, "idle: an armed word that is not ours to keep (a loaded state's) is cleared");
		}

		// The prompt's cursor struct on cell k of your grid (its node, index and the grid byte as the game has them).
		const auto cursor = [](In& in, int k) {
			in.cursor_in_grid = true;
			in.cursor_cell = k;
			in.cursor_grid = 1;
			in.cursor_node = 0x720C10u + 0x50u * static_cast<u32>(k);
			in.cursor_index = static_cast<u32>(k);
		};
		// To the prompt: armed, the cave takes it, the reach, the prompt up with this item's result.
		const auto prompt = [&](In& in, State& st, int cell = 1) {
			armed(in, st);
			in.block_armed = false;
			run(in, st, 16, 1);  // Reaching
			run(in, st, 16, 60); // the reach animation, ~1 s
			in.screen_up = true;
			in.take_prompt = true;
			in.prompt_record = 7;
			in.prompt_id = 300;
			in.result = 7;
			cursor(in, cell);    // 1: free
			run(in, st, 16, 1);  // Prompt
		};
		// --- The prompt: the confirm and every refusal. ---
		{
			State st;
			In in;
			prompt(in, st);
			check(st.phase == Phase::Prompt, "prompt: the take-it prompt after the reach");
			Out o = run(in, st, 16, 17); // 272 ms of settle
			check(st.phase == Phase::Prompt && !o.press_circle, "prompt: nothing pressed in the first 300 ms");
			o = run(in, st, 16, 2);
			check(st.phase == Phase::Confirming && o.press_circle, "prompt: grip held, the cursor on a free cell: Circle");
			in.screen_up = false;
			in.take_prompt = false;
			o = run(in, st, 16, 3);
			check(st.phase == Phase::Confirming && o.taken_cell < 0, "confirming: waits for the item to show in a cell");
			in.cells[1] = 7;
			o = run(in, st, 16, 1);
			check(st.phase == Phase::Idle && o.taken_cell == 1, "confirming: taken into cell 1");

			prompt(in, st);
			in.result = 9;
			o = run(in, st, 16, 1);
			check(st.phase == Phase::Idle && !o.press_circle && o.event, "prompt: another item's prompt is not confirmed");
			prompt(in, st);
			in.prompt_record = 9;
			o = run(in, st, 16, 1);
			check(st.phase == Phase::Idle && !o.press_circle, "prompt: a prompt showing another record (a partner's hand-over) is not confirmed");
			prompt(in, st);
			in.prompt_id = 301;
			o = run(in, st, 16, 1);
			check(st.phase == Phase::Idle && !o.press_circle, "prompt: a prompt showing another item id is not confirmed");
			prompt(in, st);
			in.cells[1] = 4;
			in.cells[3] = 6;
			o = run(in, st, 16, 30);
			check(st.phase == Phase::Idle && !o.press_circle && o.press_dpad < 0, "prompt: inventory full (the game's swap): nothing");
			prompt(in, st);
			in.grip[1] = 0.1f;
			o = run(in, st, 16, 30);
			check(st.phase == Phase::Idle && !o.press_circle, "prompt: the grip let go: the prompt is the player's");
			armed(in, st);
			in.block_armed = false;
			run(in, st, 16, 1);
			in.grip[1] = 0.0f; // let go during the reach
			run(in, st, 16, 30);
			in.grip[1] = 1.0f; // and squeezed again: not the same deliberate hold
			run(in, st, 16, 30);
			in.screen_up = in.take_prompt = true;
			in.prompt_record = 7;
			in.prompt_id = 300;
			in.result = 7;
			cursor(in, 1);
			o = run(in, st, 16, 40);
			check(!o.press_circle && st.phase == Phase::Idle, "prompt: a grip let go during the reach and closed again is not held");
			prompt(in, st);
			in.player_buttons = true;
			o = run(in, st, 16, 30);
			check(st.phase == Phase::Idle && !o.press_circle, "prompt: the player's own button: theirs");
			prompt(in, st);
			in.take_prompt = false; // the item screen, the map, pause: overlay up, not a take-it prompt
			o = run(in, st, 16, 30);
			check(st.phase == Phase::Idle && !o.press_circle, "prompt: another screen is never confirmed");
			prompt(in, st);
			in.cursor_in_grid = false;
			in.cursor_cell = -1;
			o = run(in, st, 16, 30);
			check(st.phase == Phase::Idle && !o.press_circle && o.press_dpad < 0, "prompt: a cursor off your own cells: nothing");
			// Reaching: another screen, or no prompt at all.
			armed(in, st);
			in.block_armed = false;
			run(in, st, 16, 1);
			in.screen_up = true;
			o = run(in, st, 16, 1);
			check(st.phase == Phase::Idle && !o.press_circle, "reaching: a screen that is not a take-it prompt: nothing");
			armed(in, st);
			in.block_armed = false;
			run(in, st, 16, 1);
			o = run(in, st, 16, 200);
			check(st.phase == Phase::Idle && !o.press_circle, "reaching: no prompt in 3 s: nothing");
		}
		// --- Steering: a full cell under the cursor, a free one elsewhere. ---
		{
			State st;
			In in;
			prompt(in, st, 0); // full
			Out o = run(in, st, 16, 19);
			check(st.phase == Phase::Steering && o.press_dpad == 3 && !o.press_circle, "steer: from full cell 0 right to free cell 1");
			cursor(in, 1);
			o = run(in, st, 16, 7);
			check(st.phase == Phase::Prompt, "steer: the cursor moved: back to the prompt's checks");
			o = run(in, st, 16, 8);
			check(st.phase == Phase::Confirming && o.press_circle, "steer: then confirmed on the free cell");
			prompt(in, st, 0);
			run(in, st, 16, 19);
			o = run(in, st, 16, 40); // the cursor never moves
			check(st.phase == Phase::Idle && !o.press_circle, "steer: a cursor that does not answer: nothing confirmed");
			u8 c[4] = {3, 4, 5, 0};
			check(DpadTowardFree(0, c, 4) == 3 && DpadTowardFree(1, c, 4) == 1 && DpadTowardFree(2, c, 4) == 3 &&
					  DpadTowardFree(3, c, 4) == -1,
				"route: toward cell 3 (right, down, right; already there)");
			u8 c2[4] = {0, 4, 5, 6};
			check(DpadTowardFree(1, c2, 4) == 2 && DpadTowardFree(2, c2, 4) == 0 && DpadTowardFree(3, c2, 4) == 2,
				"route: toward cell 0 (left, up, left first)");
			u8 c3[4] = {1, 2, 3, 4};
			check(DpadTowardFree(0, c3, 4) == -1 && DpadTowardFree(-1, c2, 4) == -1 && DpadTowardFree(4, c2, 4) == -1,
				"route: none with no free cell or no cell");
		}
		// --- Night review M1: the arming Circle never reaches a screen that is opening, nor a pickup under way. ---
		{
			const auto squeeze_at = [&](In in, State& st) {
				in.grip[1] = 0.0f;
				Step(t, in, st);
				in.grip[1] = 1.0f;
				in.now_ms += 16;
				return Step(t, in, st);
			};
			State st;
			In in = ready();
			in.screen_flag = true;
			check(!squeeze_at(in, st).arm, "M1: not while the screen flag (0x48BF7E) is up, before the overlay");
			st = State{};
			in = ready();
			in.screen_up = true;
			Step(t, in, st); // a screen up, then gone
			in.screen_up = false;
			in.now_ms += 100;
			check(!squeeze_at(in, st).arm, "M1: not within 200 ms of a screen");
			in.now_ms += 200;
			check(squeeze_at(in, st).arm, "M1: arms once the screens have been clear 200 ms");
			st = State{};
			in = ready();
			in.screen_flag = true;
			Step(t, in, st);
			in.screen_flag = false;
			in.now_ms += 150;
			check(!squeeze_at(in, st).arm, "M1: the screen flag counts for the 200 ms too");
			st = State{};
			in = ready();
			in.busy = true;
			check(!squeeze_at(in, st).arm, "M1: not while the character is busy (the reach of a pickup under way)");
			st = State{};
			in = ready();
			in.player_circle = true;
			Step(t, in, st);
			in.player_circle = false;
			in.now_ms += 1000;
			check(!squeeze_at(in, st).arm, "M1: not within 1.5 s of your own Circle");
			in.now_ms += 600;
			check(squeeze_at(in, st).arm, "M1: arms 1.5 s after your own Circle");
			// Armed: the screen flag rising drops the Circle and clears the word.
			In a;
			armed(a, st);
			a.screen_flag = true;
			Out o = run(a, st, 16, 1);
			check(st.phase == Phase::Idle && o.clear && o.stop_press, "M1: armed, the screen flag rising: cleared, the Circle let go");
			armed(a, st);
			a.press_dropped = true;
			o = run(a, st, 16, 1);
			check(st.phase == Phase::Idle && o.clear && o.stop_press, "M1: armed, the arming press dropped as it went out: cleared");
			// The press as it goes out.
			PressView v;
			const PressExpect e;
			check(PressFault(PressKind::Arm, v, e) == nullptr, "M1: the arming Circle goes out with every screen flag clear");
			v.screen_flag = true;
			check(PressFault(PressKind::Arm, v, e) != nullptr, "M1: the arming Circle is dropped once the screen flag rises");
			v = PressView{};
			v.overlay_up = true;
			check(PressFault(PressKind::Arm, v, e) != nullptr, "M1: ... once the overlay rises");
			v = PressView{};
			v.menu_mode = true;
			check(PressFault(PressKind::Arm, v, e) != nullptr, "M1: ... once menu mode (pauseWhen) is on");
		}
		// --- Night review M2: the confirm lands only on the checked free cell; any cursor move or player input aborts. ---
		{
			State st;
			In in;
			prompt(in, st);
			run(in, st, 16, 8); // 128 ms: past the cursor's placing
			cursor(in, 3);      // free too, but not moved by us
			Out o = run(in, st, 16, 30);
			check(st.phase == Phase::Idle && !o.press_circle, "M2: the cursor moved on the prompt (not by us): nothing confirmed");
			prompt(in, st);
			cursor(in, 3); // the game placing its cursor in the prompt's first 100 ms
			o = run(in, st, 16, 30);
			check(st.phase == Phase::Confirming && o.press_circle && o.confirm, "M2: a cursor placed in the first 100 ms is the game's");
			prompt(in, st);
			run(in, st, 16, 18); // 288 ms
			cursor(in, 3);       // moved just before the press
			o = run(in, st, 16, 3);
			check(st.phase == Phase::Idle && !o.press_circle, "M2: a move just before the press: nothing confirmed");
			prompt(in, st);
			o = run(in, st, 16, 19);
			check(st.phase == Phase::Confirming && o.press_circle, "M2: the confirm (setup)");
			cursor(in, 3);
			o = run(in, st, 16, 1);
			check(st.phase == Phase::Idle && o.stop_press, "M2: the cursor moving under the confirm press lets Circle go");
			prompt(in, st);
			run(in, st, 16, 19);
			in.player_buttons = true;
			o = run(in, st, 16, 1);
			check(st.phase == Phase::Idle && o.stop_press, "M2: the player's input under the confirm press lets Circle go");
			prompt(in, st);
			run(in, st, 16, 19);
			in.press_dropped = true;
			o = run(in, st, 16, 1);
			check(st.phase == Phase::Idle && o.stop_press, "M2: a confirm dropped as it went out ends the take");
			prompt(in, st);
			in.press_dropped = true;
			o = run(in, st, 16, 1);
			check(st.phase == Phase::Idle && !o.press_circle, "M2: a press dropped on the prompt ends the take");
			// Steering: one move, to the cell the press sends the cursor to.
			prompt(in, st, 0);
			run(in, st, 16, 19);
			check(st.phase == Phase::Steering && st.steer_to == 1, "M2: steering right from cell 0 must reach cell 1");
			cursor(in, 2); // somewhere else
			o = run(in, st, 16, 1);
			check(st.phase == Phase::Idle && o.stop_press && !o.press_circle, "M2: the cursor going elsewhere while steering: nothing");
			prompt(in, st, 0);
			run(in, st, 16, 19);
			cursor(in, 1);
			run(in, st, 16, 2);
			cursor(in, 3); // and on again before the checks
			o = run(in, st, 16, 20);
			check(st.phase == Phase::Idle && !o.press_circle, "M2: a second move while steering: nothing confirmed");
			prompt(in, st, 0);
			run(in, st, 16, 19);
			in.player_buttons = true;
			o = run(in, st, 16, 1);
			check(st.phase == Phase::Idle && o.stop_press, "M2: the player's input while steering lets the D-pad go");
			// The press as it goes out.
			PressView v;
			v.take_prompt = true;
			v.grid = 1;
			v.node = 0x720C60;
			v.index = 1;
			PressExpect e;
			e.node = 0x720C60;
			e.index = 1;
			check(PressFault(PressKind::Confirm, v, e) == nullptr, "M2: the confirm goes out on the checked free cell");
			PressView w = v;
			w.node = 0x720CB0;
			check(PressFault(PressKind::Confirm, w, e) != nullptr, "M2: dropped: the cursor's node changed");
			w = v;
			w.index = 2;
			check(PressFault(PressKind::Confirm, w, e) != nullptr, "M2: dropped: the cursor's index changed");
			w = v;
			w.grid = 0;
			check(PressFault(PressKind::Confirm, w, e) != nullptr, "M2: dropped: the cursor left your grid (the partner's panel)");
			w = v;
			w.cell = 9;
			check(PressFault(PressKind::Confirm, w, e) != nullptr, "M2: dropped: the cell filled (the swap)");
			w = v;
			w.player_input = true;
			check(PressFault(PressKind::Confirm, w, e) != nullptr, "M2: dropped: the player's input");
			w = v;
			w.take_prompt = false;
			check(PressFault(PressKind::Confirm, w, e) != nullptr, "M2: dropped: the prompt went");
			w = v;
			w.player_input = true;
			check(PressFault(PressKind::Steer, w, e) != nullptr && PressFault(PressKind::Steer, v, e) == nullptr,
				"M2: the D-pad press is dropped on the player's input only");
		}

		// --- The shipped File #1 block. ---
		{
			const ProfileDB::Profile* f1 = ProfileDB::Lookup("SLPM-65428", 0);
			const ProfileDB::ReachParams* r = (f1 && f1->reach.has_value()) ? &f1->reach.value() : nullptr;
			check(r && r->has_pickup, "profile: File #1 has reach.pickup");
			if (r && r->has_pickup)
			{
				check(r->pickup_block == 0xFF200 && r->pickup_magic == 0x48504B31 && r->pickup_counter == 0x48BF93 &&
						  r->pickup_site == 0x61DB94 && r->pickup_site_on == 0x0C03FB00 && r->pickup_cave == 0xFEC00 &&
						  r->pickup_cave_first == 0x3C0B0010,
					"profile: block 0xFF200 'HPK1', the counter, site 0x61DB94 -> jal 0xFEC00, the cave's first word");
				check(r->pickup_prompt == 0x722890 && r->pickup_result == 0xC7A && r->pickup_cells == 0xC84 && r->pickup_cell_count == 4 &&
						  r->pickup_overlay == 0x720AA0 && r->pickup_which == 0x720BE4 && r->pickup_sub == 0x720BE2 &&
						  r->pickup_grid == 0x723F94 && r->pickup_cursor_node == 0x720B84 && r->pickup_cursor_index == 0x720B8C &&
						  r->pickup_cell_nodes[0] == 0x720C10 && r->pickup_cell_nodes[3] == 0x720DD8,
					"profile: the record's result and cells, the prompt's flags and cursor");
				check(r->pickup_near > 0.0f && r->pickup_near <= 10.0f && r->pickup_show >= r->pickup_near,
					"profile: highlighted within a few cm, the box shown farther");
				check(r->pickup_max_reach == 85.0f, "profile: arm's reach 85 cm");
			}
		}

		if (failures == 0)
			Console.WriteLn(Color_StrongGreen, "(VR) Hand pickup self-test: passed (%d checks: the squeeze latch, refusals, the armed "
											   "word's timeout and clears, the prompt's checks and confirm, full inventory, steering, "
											   "the shipped block).", checks);
		else
			Console.WriteLn(Color_StrongRed, "(VR) Hand pickup self-test FAIL: %s (%d of %d checks failed).", first, failures, checks);
		return failures == 0;
	}
}
