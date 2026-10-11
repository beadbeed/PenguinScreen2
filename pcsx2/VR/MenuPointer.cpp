// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/MenuPointer.h"
#include "VR/HudCards.h"
#include "VR/SpatialControls.h"
#include "VR/SplitState.h"
#include "VR/VRInput.h"
#include "VR/VRProfileDB.h"

#include "Input/VRInputSource.h"
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
#include <mutex>
#include <string>
#include <vector>

namespace VR::MenuPointer
{
	namespace
	{
		constexpr int kIdle = 0;
		constexpr int kPressing = 1;
		constexpr int kReleased = 2;
		// A user override (any of these) stops a steer: the right stick as far as the menu stick engages, the left stick
		// (the game's own cursor stick) half way, a grip squeezed.
		constexpr float kOverrideRightStick = 0.6f;
		constexpr float kOverrideLeftStick = 0.5f;
		constexpr float kOverrideGrip = 0.6f;
		// Steering stays suspended this many polls after a suspendWhen guard stops passing.
		constexpr u32 kSuspendTailPolls = 6;
		// Event lines logged per opening of the menu; the rest are counted and reported as it closes.
		constexpr u32 kLogLinesPerOpen = 24;

		std::atomic<int> s_hovered{-1};

		std::mutex s_telemetry_mutex;
		Telemetry s_telemetry;

		// CPU thread only.
		State s_state;
		bool s_screen_open = false; // the profile's menu was up last poll (whether or not point-to-select ran)
		bool s_submenu_open = false; // a suspendWhen guard held steering last poll (StepOut::submenu), for its log lines
		u32 s_log_budget = 0;
		u32 s_log_dropped = 0;
		u32 s_open_steers = 0;
		u32 s_open_confirms = 0;
		std::atomic<bool> s_selftest_ran{false};

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

		// suspendWhen: any one passing suspends steering.
		bool AnyGuardPasses(const std::vector<ProfileDB::CameraGuard>& guards, GuestReader reader)
		{
			for (const ProfileDB::CameraGuard& g : guards)
			{
				if (GuardPasses(g, reader))
					return true;
			}
			return false;
		}

		// Everything about one opening of the menu, back to closed. The swallow, the button edges and the wait after a
		// confirm carry on.
		void ResetOpen(State& st)
		{
			st.open = false;
			st.open_ms = 0.0f;
			st.selfcheck_failed = false;
			st.last_cell = -1;
			st.under = -1;
			st.under_ms = 0.0f;
			st.ignore = -2;
			st.disarmed = false;
			st.disarm_under = -1;
			st.disarm_strict = false;
			st.latched = false;
			st.latch_cell = -1;
			st.suspend_polls = 0;
			st.target = -1;
			st.phase = kIdle;
			st.dir = -1;
			st.phase_polls = 0;
			st.step_from = -1;
			st.presses = 0;
			st.steer_from = -1;
			st.steer_ms = 0.0f;
			st.pending = false;
		}

		bool InRect(const float r[4], float u, float v, float inset)
		{
			const float iu = inset * (r[2] - r[0]);
			const float iv = inset * (r[3] - r[1]);
			return u >= r[0] + iu && u <= r[2] - iu && v >= r[1] + iv && v <= r[3] - iv;
		}

		// The cell the cursor struct says the cursor is on: node, index, x and y must all match one. *mismatch is set
		// when no cell matches but one has that node (the self-check fails); any other node (a partner's panel, another
		// tab, a submenu) is simply no cell.
		int MatchCell(const ProfileDB::MenuPointerParams& p, u32 node, u32 index, u32 x, u32 y, bool* mismatch)
		{
			*mismatch = false;
			for (size_t i = 0; i < p.cells.size(); i++)
			{
				const ProfileDB::MenuPointerCell& c = p.cells[i];
				if (c.node == node && c.index == index && c.x == x && c.y == y)
					return static_cast<int>(i);
			}
			for (const ProfileDB::MenuPointerCell& c : p.cells)
				*mismatch = *mismatch || (c.node == node);
			return -1;
		}

		bool LogLine()
		{
			if (s_log_budget == 0)
			{
				s_log_dropped++;
				return false;
			}
			s_log_budget--;
			return true;
		}

		void MaybeRunSelfTest()
		{
			static const bool enabled = (std::getenv("PCSX2_VR_SELFTEST") != nullptr);
			if (!enabled || s_selftest_ran.exchange(true, std::memory_order_acq_rel))
				return;
			SelfTest();
		}

		// Self-test maths: small vectors and quaternions (xyzw).
		struct V3
		{
			float x, y, z;
		};
		V3 Sub(const V3& a, const V3& b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; }
		V3 Add(const V3& a, const V3& b) { return V3{a.x + b.x, a.y + b.y, a.z + b.z}; }
		V3 Mul(const V3& a, float s) { return V3{a.x * s, a.y * s, a.z * s}; }
		float Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		V3 Cross(const V3& a, const V3& b) { return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
		V3 Normalize(const V3& a)
		{
			const float l = std::sqrt(Dot(a, a));
			return (l > 1.0e-8f) ? Mul(a, 1.0f / l) : V3{0.0f, 0.0f, 0.0f};
		}
		V3 Rotate(const std::array<float, 4>& q, const V3& v)
		{
			const V3 u{q[0], q[1], q[2]};
			const V3 t = Mul(Cross(u, v), 2.0f);
			return Add(Add(v, Mul(t, q[3])), Cross(u, t));
		}
		// The rotation whose columns are the orthonormal axes x, y, z.
		std::array<float, 4> QuatFromBasis(const V3& x, const V3& y, const V3& z)
		{
			const float m00 = x.x, m01 = y.x, m02 = z.x;
			const float m10 = x.y, m11 = y.y, m12 = z.y;
			const float m20 = x.z, m21 = y.z, m22 = z.z;
			const float tr = m00 + m11 + m22;
			std::array<float, 4> q{};
			if (tr > 0.0f)
			{
				const float sc = std::sqrt(tr + 1.0f) * 2.0f;
				q = {(m21 - m12) / sc, (m02 - m20) / sc, (m10 - m01) / sc, 0.25f * sc};
			}
			else if (m00 > m11 && m00 > m22)
			{
				const float sc = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
				q = {0.25f * sc, (m01 + m10) / sc, (m02 + m20) / sc, (m21 - m12) / sc};
			}
			else if (m11 > m22)
			{
				const float sc = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
				q = {(m01 + m10) / sc, 0.25f * sc, (m12 + m21) / sc, (m02 - m20) / sc};
			}
			else
			{
				const float sc = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
				q = {(m02 + m20) / sc, (m12 + m21) / sc, 0.25f * sc, (m10 - m01) / sc};
			}
			return q;
		}
		// An aim pose at `from` whose -Z points along `dir`.
		VRPose AimAlong(const V3& from, const V3& dir)
		{
			const V3 z = Normalize(Mul(dir, -1.0f));
			V3 x = Cross(V3{0.0f, 1.0f, 0.0f}, z);
			x = (Dot(x, x) > 1.0e-6f) ? Normalize(x) : V3{1.0f, 0.0f, 0.0f};
			const V3 y = Cross(z, x);
			VRPose pose;
			pose.orientation_xyzw = QuatFromBasis(x, y, z);
			pose.position_xyz = {from.x, from.y, from.z};
			pose.valid = true;
			return pose;
		}

		// File #1's item screen as the shipped profile maps it (bin/resources/vr-profiles/SLPM-65428.yaml).
		ProfileDB::MenuPointerParams ItemScreenParams()
		{
			ProfileDB::MenuPointerParams p;
			p.node_addr = 0x720B84;
			p.index_addr = 0x720B8C;
			p.x_addr = 0x720B8E;
			p.y_addr = 0x720B90;
			p.coop = true;
			const auto cell = [&p](u32 node, u16 index, u16 x, u16 y, float u0, float v0, float u1, float v1, s8 up, s8 down,
								  s8 left, s8 right) {
				ProfileDB::MenuPointerCell c;
				c.node = node;
				c.index = index;
				c.x = x;
				c.y = y;
				c.rect[0] = u0;
				c.rect[1] = v0;
				c.rect[2] = u1;
				c.rect[3] = v1;
				c.next[0] = up;
				c.next[1] = down;
				c.next[2] = left;
				c.next[3] = right;
				p.cells.push_back(c);
			};
			cell(0x720C10, 0, 498, 108, 0.807f, 0.184f, 0.895f, 0.307f, -1, 2, -1, 1);
			cell(0x720CA8, 1, 550, 108, 0.895f, 0.184f, 0.982f, 0.307f, -1, 3, 0, -1);
			cell(0x720D40, 2, 498, 160, 0.807f, 0.307f, 0.895f, 0.430f, 0, 4, -1, 3);
			cell(0x720DD8, 3, 550, 160, 0.895f, 0.307f, 0.982f, 0.430f, 1, 4, 2, -1);
			cell(0x722928, 0, 550, 216, 0.888f, 0.456f, 0.979f, 0.581f, 2, -1, -1, -1);
			return p;
		}
	}

	int HoveredCell()
	{
		return s_hovered.load(std::memory_order_acquire);
	}

	Telemetry GetTelemetry()
	{
		Telemetry t;
		{
			std::lock_guard<std::mutex> lock(s_telemetry_mutex);
			t = s_telemetry;
		}
		const VRScreenTransform screen = GetScreenTransform();
		t.transform_valid = screen.valid;
		t.tablet = screen.valid && screen.tablet;
		for (int i = 0; i < 3; i++)
			t.centre[i] = screen.center.position_xyz[i];
		for (int i = 0; i < 4; i++)
			t.quat[i] = screen.center.orientation_xyzw[i];
		t.half_width = screen.half_width;
		t.half_height = screen.half_height;
		return t;
	}

	int HoverCell(const ProfileDB::MenuPointerParams& p, float u, float v, int prev)
	{
		if (!std::isfinite(u) || !std::isfinite(v))
			return -1;
		const int count = static_cast<int>(p.cells.size());
		if (prev >= 0 && prev < count && InRect(p.cells[static_cast<size_t>(prev)].rect, u, v, 0.0f))
			return prev;
		for (int i = 0; i < count; i++)
		{
			if (InRect(p.cells[static_cast<size_t>(i)].rect, u, v, p.enter_inset))
				return i;
		}
		return -1;
	}

	int PlanStep(const ProfileDB::MenuPointerParams& p, int from, int to)
	{
		const int count = static_cast<int>(p.cells.size());
		if (from < 0 || to < 0 || from >= count || to >= count || from == to || count > 64)
			return -1;
		// Breadth first from `from`, each cell remembering the first press of the way it was reached.
		int first[64];
		bool seen[64] = {};
		int queue[64];
		int head = 0;
		int tail = 0;
		seen[from] = true;
		first[from] = -1;
		queue[tail++] = from;
		while (head < tail)
		{
			const int cur = queue[head++];
			for (int d = 0; d < 4; d++)
			{
				const int nb = p.cells[static_cast<size_t>(cur)].next[d];
				if (nb < 0 || nb >= count || seen[nb])
					continue;
				const int way = (cur == from) ? d : first[cur];
				if (nb == to)
					return way;
				seen[nb] = true;
				first[nb] = way;
				queue[tail++] = nb;
			}
		}
		return -1;
	}

	bool SteeringAllowed(bool online_locked, bool menu_mode, bool coop)
	{
		return !online_locked || (coop && menu_mode);
	}

	StepOut Step(const ProfileDB::MenuPointerParams& p, const StepIn& in, State& st)
	{
		StepOut out;
		const bool confirm_edge = in.confirm && !st.confirm_prev;
		st.confirm_prev = in.confirm;
		const bool back_edge = in.back && !st.back_prev;
		st.back_prev = in.back;
		const bool menu_edge = in.menu && !st.menu_prev;
		st.menu_prev = in.menu;
		// A swallowed confirm stays held back until the controls are let go, whatever else happens meanwhile.
		if (st.swallow && !in.confirm)
			st.swallow = false;
		// The wait after a confirm (suspendWhen) counts down every poll, the gate up or down.
		st.after_confirm_ms = std::max(0.0f, st.after_confirm_ms - in.dt_ms);

		// Stops a steer. A confirm waiting for it never reaches the game: it is swallowed (held back until let go if it
		// is still held). The pointer then waits for the hover to change before it steers again, so nothing retries.
		const auto abort_steer = [&](const char* why) {
			if (st.target < 0)
				return;
			out.aborted = why;
			out.aborted_target = st.target;
			out.aborted_presses = st.presses;
			if (st.pending)
			{
				st.pending = false;
				out.confirm_swallowed = true;
				if (in.confirm)
					st.swallow = true;
			}
			st.target = -1;
			st.phase = kIdle;
			st.dir = -1;
			st.phase_polls = 0;
			st.presses = 0;
			st.disarmed = true;
			st.disarm_under = st.under;
		};

		if (!in.gate)
		{
			// Nothing is pressed from the poll the gate drops, and everything about this opening resets, except the latch
			// while the menu is still up: a submenu opened by the confirm may still be up behind the gate (the dashboard, a
			// pause, the setting off), and the D-pad must not work it when the gate comes back. Cross, the cursor on
			// another cell or the menu closing lets it go, as with the gate up.
			if (st.open)
			{
				abort_steer("the menu closed");
				out.closed = true;
			}
			bool keep_latch = st.latched && in.screen_up && !back_edge;
			if (keep_latch && in.cursor_ok)
			{
				bool mismatch = false;
				const int cell = MatchCell(p, in.node, in.index, in.x, in.y, &mismatch);
				if (cell >= 0 && cell != st.latch_cell)
					keep_latch = false;
			}
			const int latch_cell = st.latch_cell;
			ResetOpen(st);
			if (keep_latch)
			{
				st.latched = true;
				st.latch_cell = latch_cell;
			}
			// The wait after a confirm outlives a gate drop only while the menu is still up; a confirm reaching the game
			// meanwhile (with the gate down nothing but a swallow holds the trigger, and a fresh pull is never swallowed)
			// starts it as well.
			if (!in.screen_up)
				st.after_confirm_ms = 0.0f;
			else if (confirm_edge && !p.suspend_when.empty())
				st.after_confirm_ms = kAfterConfirmMs;
			out.suspended = st.latched;
			out.confirm_hold = st.swallow;
			return out;
		}

		// The cursor's cell. A node that is a cell's with the rest different fails the self-check, and point-to-select
		// stops until the menu opens again.
		bool mismatch = false;
		const int current = in.cursor_ok ? MatchCell(p, in.node, in.index, in.x, in.y, &mismatch) : -1;
		if (mismatch && !st.selfcheck_failed)
		{
			st.selfcheck_failed = true;
			out.selfcheck_failed_now = true;
		}
		out.current = current;

		bool opened = false;
		if (!st.open)
		{
			// A latch kept through a gate drop (the menu stayed up) carries into this opening; a closed menu cleared it.
			const bool latched = st.latched;
			const int latch_cell = st.latch_cell;
			ResetOpen(st);
			st.latched = latched;
			st.latch_cell = latch_cell;
			st.open = true;
			opened = true;
			out.opened = true;
			// No landing tick for the cell the menu opens on.
			st.last_cell = current;
		}
		else
		{
			st.open_ms += in.dt_ms;
		}

		// Landing: the cursor reached another cell, whatever moved it (a steer, the D-pad, a stick).
		if (current >= 0 && current != st.last_cell)
			out.landed = true;
		st.last_cell = current;

		// Hover, with the cell's hysteresis; how long it has stayed.
		const int under = in.hit ? HoverCell(p, in.u, in.v, st.under) : -1;
		if (under != st.under)
		{
			st.under = under;
			st.under_ms = 0.0f;
		}
		else
		{
			st.under_ms += in.dt_ms;
		}
		// The cell under the pointer as the menu opened is ignored until the hover changes, so the cursor never jumps
		// on its own as a menu opens.
		if (opened)
			st.ignore = st.under;
		else if (st.ignore != -2 && st.under != st.ignore)
			st.ignore = -2;
		// Any change of hover arms again, except after a submenu: there only the hover reaching another cell does.
		if (st.disarmed && st.under != st.disarm_under && (!st.disarm_strict || st.under >= 0))
		{
			st.disarmed = false;
			st.disarm_strict = false;
		}

		// The player taking over with another control stops any steer, and the pointer waits for the hover to change.
		const bool override_now = in.dpad || in.right_stick >= kOverrideRightStick || in.left_stick >= kOverrideLeftStick ||
		                          back_edge || menu_edge || in.grip >= kOverrideGrip;
		if (override_now)
		{
			abort_steer("you took over");
			st.disarmed = true;
			st.disarm_under = st.under;
		}

		// After a confirm reached the game (with no suspendWhen to say when a submenu is up), steering waits until Cross,
		// the cursor moving to another cell or the menu closing: the D-pad might be working a submenu by then.
		if (st.latched && (back_edge || (current >= 0 && current != st.latch_cell)))
			st.latched = false;
		// With suspendWhen, steering waits while one of its guards passes (a submenu is open) and kSuspendTailPolls
		// after, and for the wait after a confirm (a submenu that confirm opens may not have its flag up yet).
		bool suspended = false;
		bool submenu = false;
		if (!p.suspend_when.empty())
		{
			if (in.suspend)
			{
				st.suspend_polls = kSuspendTailPolls;
				submenu = true;
			}
			else if (st.suspend_polls > 0)
			{
				st.suspend_polls--;
				submenu = true;
			}
			suspended = submenu || st.after_confirm_ms > 0.0f;
		}
		else
		{
			suspended = st.latched;
		}

		// Suspended, nothing is pressed from this very poll: a press under way is let go and the steer ends. A confirm
		// it was holding for the arrival never reaches the game (held back only while still held, so it can never turn
		// into a confirm in a submenu the player did not make), and from here on the trigger passes straight through:
		// nothing is armed, so a fresh pull is the player's own confirm on whatever the game has up.
		const bool can_steer = !st.selfcheck_failed && !suspended;
		if (!can_steer)
		{
			const char* why = "a confirm just reached the game";
			if (st.selfcheck_failed)
				why = "the cursor struct did not match";
			else if (submenu || p.suspend_when.empty())
				why = "a submenu is open";
			abort_steer(why);
		}
		if (st.target >= 0 && current < 0)
			abort_steer("the cursor left the cells");
		// A submenu: once it has closed, steering stays off until the dot reaches a cell other than the one it is on as
		// the submenu (and its tail) ends, so closing it never sends the cursor to wherever the dot was left meanwhile.
		if (submenu)
		{
			st.disarmed = true;
			st.disarm_strict = true;
			st.disarm_under = st.under;
		}
		const bool armed = can_steer && !st.disarmed;
		out.hovered = armed ? st.under : -1;

		const auto start_steer = [&](int to) {
			st.target = to;
			st.phase = kIdle;
			st.dir = -1;
			st.phase_polls = 0;
			st.presses = 0;
			st.steer_from = current;
			st.steer_ms = 0.0f;
			// The confirm controls already down with no fresh press: held back until let go, so a held trigger never
			// turns into a confirm when the cursor arrives.
			if (in.confirm && !confirm_edge)
				st.swallow = true;
		};
		// A new target mid-steer: the press under way finishes, and the next one is planned for it.
		const auto retarget = [&](int to) {
			st.target = to;
			st.presses = 0;
		};

		if (confirm_edge)
		{
			if (st.target >= 0)
			{
				// Pulled while steering: it waits for the arrival; pulled on another cell, it goes there instead.
				if (armed && st.under >= 0 && st.under != st.target && current >= 0)
					retarget(st.under);
				st.pending = true;
			}
			else if (armed && st.under >= 0 && current >= 0 && st.under != current)
			{
				// Pulled on another cell: straight there, no dwell, and the confirm waits for the arrival.
				start_steer(st.under);
				st.pending = true;
			}
			else
			{
				// It reaches the game now: on the cursor's own cell, or (suspended) on whatever the game has up, such as
				// the highlighted entry of a submenu. With suspendWhen, steering then waits kAfterConfirmMs for a
				// submenu this confirm opens to show its flag.
				out.confirm_passed = true;
				out.confirm_cell = current;
				if (p.suspend_when.empty())
				{
					st.latched = true;
					st.latch_cell = current;
					out.latched = true;
				}
				else
				{
					st.after_confirm_ms = kAfterConfirmMs;
				}
			}
		}

		// Pointing at a cell: once it has held dwellMs (and the menu has been open armDelayMs), the cursor goes there. A
		// steer whose confirm is waiting keeps the cell it was pulled on.
		const bool passive = armed && st.ignore == -2 && st.open_ms >= p.arm_delay_ms && st.under >= 0 &&
		                     st.under_ms >= p.dwell_ms && current >= 0;
		if (passive)
		{
			if (st.target < 0 && st.under != current)
				start_steer(st.under);
			else if (st.target >= 0 && !st.pending && st.under != st.target)
				retarget(st.under);
		}

		// A confirm let go before the cursor arrived never reaches the game.
		if (st.pending && !in.confirm)
		{
			st.pending = false;
			out.confirm_swallowed = true;
		}

		const auto arrive = [&]() {
			out.arrived = true;
			out.arrived_from = st.steer_from;
			out.arrived_ms = st.steer_ms;
			out.arrived_presses = st.presses;
			if (st.pending)
			{
				// The held confirm now reaches the game, on the right cell.
				st.pending = false;
				out.confirm_passed = true;
				out.confirm_cell = current;
				if (p.suspend_when.empty())
				{
					st.latched = true;
					st.latch_cell = current;
					out.latched = true;
				}
				else
				{
					st.after_confirm_ms = kAfterConfirmMs;
				}
			}
			st.target = -1;
			st.phase = kIdle;
			st.dir = -1;
			st.phase_polls = 0;
			st.presses = 0;
		};

		// Steering, closed loop: press the planned direction pressVsyncs polls, release at least gapVsyncs (a 30 fps
		// game could merge two presses across a shorter gap), wait for the cursor to move (at most settleVsyncs), then
		// plan again from wherever it is.
		if (st.target >= 0)
		{
			st.steer_ms += in.dt_ms;
			if (st.phase == kPressing)
			{
				if (st.phase_polls < p.press_vsyncs)
				{
					out.dir = st.dir;
					st.phase_polls++;
				}
				else
				{
					st.phase = kReleased;
					st.phase_polls = 0;
				}
			}
			if (st.phase == kReleased)
			{
				if (current == st.target)
					arrive();
				else if (current != st.step_from)
				{
					if (st.phase_polls >= p.gap_vsyncs)
						st.phase = kIdle;
					else
						st.phase_polls++;
				}
				else if (st.phase_polls >= p.settle_vsyncs)
					abort_steer("the cursor did not move");
				else
					st.phase_polls++;
			}
			if (st.target >= 0 && st.phase == kIdle)
			{
				if (current == st.target)
				{
					arrive();
				}
				else
				{
					const int dir = PlanStep(p, current, st.target);
					if (dir < 0)
						abort_steer("no way there");
					else if (st.presses >= p.max_presses)
						abort_steer("too many presses");
					else
					{
						st.phase = kPressing;
						st.dir = dir;
						st.phase_polls = 1;
						st.step_from = current;
						st.presses++;
						out.dir = dir;
						out.pressed = true;
					}
				}
			}
		}

		out.target = st.target;
		out.presses = st.presses;
		out.in_flight = st.target >= 0;
		out.confirm_hold = out.in_flight || st.swallow;
		// As it stands after this poll (a confirm passed now has latched, or started its wait, already).
		out.suspended = p.suspend_when.empty() ? st.latched : (suspended || st.after_confirm_ms > 0.0f);
		out.submenu = submenu;
		out.selfcheck_failed = st.selfcheck_failed;
		return out;
	}

	void Poll(const VRInputSnapshot& snap, GuestReader reader, float dt)
	{
		MaybeRunSelfTest();

		const bool live = snap.generation != 0;
		const bool menu_mode = SpatialControls::MenuMode();
		const VRHandState& l = snap.hands[VRInputSnapshot::LEFT];
		const VRHandState& r = snap.hands[VRInputSnapshot::RIGHT];

		// Nothing is looked up or read out of menu mode, or with the VM paused (an idle poll), so normal play costs nothing;
		// the one exception is a latch held after a confirm, which needs to know whether the menu is still up whatever
		// else has stopped (the dashboard, a pause, the setting off, menu mode), so it outlives a submenu only as long
		// as the menu.
		const bool vm_running = VMManager::GetState() == VMState::Running;
		const bool gate_conditions = live && snap.actions_active && menu_mode && !SplitState::Active() && vm_running;
		const ProfileDB::MenuPointerParams* params = nullptr;
		bool screen_up = false;
		if ((gate_conditions || s_state.latched) && VMManager::HasValidVM())
		{
			const std::string serial = VMManager::GetDiscSerial();
			const ProfileDB::Profile* profile = serial.empty() ? nullptr : ProfileDB::Lookup(serial, VMManager::GetDiscCRC());
			if (profile && profile->menu_pointer.has_value())
			{
				params = &profile->menu_pointer.value();
				screen_up = VRInputSource::EvaluateGuards(params->when, reader);
			}
		}
		const bool screen_open = gate_conditions && screen_up;

		StepIn in;
		in.screen_up = screen_up;
		u32 node = 0;
		u32 index = 0;
		u32 x = 0;
		u32 y = 0;
		if (screen_up)
		{
			in.cursor_ok = Read(reader, params->node_addr, 4, &node) && Read(reader, params->index_addr, 2, &index) &&
			               Read(reader, params->x_addr, 2, &x) && Read(reader, params->y_addr, 2, &y);
		}
		const bool enabled = HudCards::MenuPointerEnabled();
		const bool online = VMManager::IsOnlineLocked();
		const bool allowed = screen_open && SteeringAllowed(online, menu_mode, params->coop);
		in.gate = screen_open && enabled && allowed;
		in.node = node;
		in.index = static_cast<u16>(index);
		in.x = static_cast<u16>(x);
		in.y = static_cast<u16>(y);

		// The pointer's hit, from the profile's hand (either: the right first, as GetInputSnapshot takes it).
		VRScreenHit hit = snap.screen_hit;
		if (in.gate && params->hand >= 0 && hit.valid && !hit.test && hit.hand != params->hand)
		{
			hit = RayHitScreen(GetScreenTransform(), snap.hands[params->hand].aim_pose);
			hit.hand = params->hand;
		}
		in.hit = hit.valid;
		in.u = hit.u;
		in.v = hit.v;
		in.confirm = r.trigger >= 0.5f || r.a;
		// B lets a latch go because it is the game's cancel; with the VM paused the game never sees it.
		in.back = r.b && vm_running;
		in.menu = l.menu || r.menu;
		in.dpad = l.dpad_up || l.dpad_down || l.dpad_left || l.dpad_right || r.dpad_up || r.dpad_down || r.dpad_left ||
		          r.dpad_right;
		in.right_stick = std::sqrt(r.thumbstick_x * r.thumbstick_x + r.thumbstick_y * r.thumbstick_y);
		in.left_stick = std::sqrt(l.thumbstick_x * l.thumbstick_x + l.thumbstick_y * l.thumbstick_y);
		in.grip = std::max(l.grip, r.grip);
		in.suspend = in.gate && !params->suspend_when.empty() && AnyGuardPasses(params->suspend_when, reader);
		in.dt_ms = std::clamp(std::isfinite(dt) ? dt : 0.0f, 0.0f, 0.25f) * 1000.0f;

		static const ProfileDB::MenuPointerParams s_no_params{};
		const StepOut out = Step(params ? *params : s_no_params, in, s_state);

		SpatialControls::SetPointerDpad(out.dir);
		SpatialControls::SetPointerConfirmHold(out.confirm_hold);
		s_hovered.store(out.hovered, std::memory_order_release);

		// Ticks: in the pointing hand while the pointer is on the screen, else the right.
		const int tick_hand = (hit.valid && hit.hand >= 0 && hit.hand < 2) ? hit.hand : VRInputSnapshot::RIGHT;
		if (out.landed)
			XRInput::QueuePulse(tick_hand, SpatialControls::kHapticZoneEnter.amplitude, SpatialControls::kHapticZoneEnter.seconds);
		if (out.confirm_swallowed)
			XRInput::QueuePulse(tick_hand, SpatialControls::kHapticRelease.amplitude, SpatialControls::kHapticRelease.seconds);

		// Logs: one line as the menu opens, the events (rate-limited per opening), a summary as it closes.
		if (screen_open && !s_screen_open)
		{
			s_log_budget = kLogLinesPerOpen;
			s_log_dropped = 0;
			s_open_steers = 0;
			s_open_confirms = 0;
			bool mismatch = false;
			const int cell = in.cursor_ok ? MatchCell(*params, node, index, x, y, &mismatch) : -1;
			const char* state = !enabled ? "off (setting off)" :
			                    !allowed ? "off (coop off online)" :
			                    mismatch ? "off (self-check)" :
			                    !in.cursor_ok ? "off (cursor not readable)" :
			                    online ? "on (online, private co-op)" : "on";
			Console.WriteLn("(VR) MenuPointer: item screen open, cursor at cell %d (node %06X index %u x %u y %u); "
							"point-to-select %s.",
				cell, node, index, x, y, state);
		}
		else if (!screen_open && s_screen_open)
		{
			if (s_open_steers > 0 || s_open_confirms > 0 || s_log_dropped > 0)
			{
				Console.WriteLn("(VR) MenuPointer: item screen closed (%u steer(s), %u confirm(s); %u line(s) not logged).",
					s_open_steers, s_open_confirms, s_log_dropped);
			}
		}
		s_screen_open = screen_open;

		if (out.selfcheck_failed_now && !out.opened && LogLine())
		{
			Console.Warning("(VR) MenuPointer: the cursor struct no longer matches its cell (node %06X index %u x %u y %u); "
							"point-to-select off until the screen opens again.",
				node, index, x, y);
		}
		if (out.arrived)
		{
			s_open_steers++;
			if (LogLine())
			{
				Console.WriteLn("(VR) MenuPointer: cell %d -> %d in %u press(es), %u ms.", out.arrived_from, out.current,
					out.arrived_presses, static_cast<u32>(std::lround(out.arrived_ms)));
			}
		}
		if (out.aborted)
		{
			s_open_steers++;
			if (LogLine())
			{
				Console.WriteLn("(VR) MenuPointer: steering to cell %d aborted (%s) after %u press(es).", out.aborted_target,
					out.aborted, out.aborted_presses);
			}
		}
		if (out.confirm_passed)
		{
			s_open_confirms++;
			if (LogLine())
			{
				if (out.latched)
					Console.WriteLn("(VR) MenuPointer: confirm passed on cell %d; steering paused until Cross, a cursor move or "
									"the screen closes.",
						out.confirm_cell);
				else if (out.submenu)
					Console.WriteLn("(VR) MenuPointer: confirm passed into the submenu (the player's own pull; nothing steered).");
				else if (params && !params->suspend_when.empty())
					Console.WriteLn("(VR) MenuPointer: confirm passed on cell %d; steering waits %.0f ms for a submenu's flag.",
						out.confirm_cell, static_cast<double>(kAfterConfirmMs));
				else
					Console.WriteLn("(VR) MenuPointer: confirm passed on cell %d.", out.confirm_cell);
			}
		}
		// A submenu (suspendWhen) holding steering, as it opens and as it ends (after its tail).
		if (out.submenu != s_submenu_open)
		{
			s_submenu_open = out.submenu;
			if (screen_open && LogLine())
			{
				if (out.submenu)
					Console.WriteLn("(VR) MenuPointer: submenu open; nothing is pressed and the trigger passes straight through.");
				else
					Console.WriteLn("(VR) MenuPointer: submenu closed; steering waits for the dot to reach another cell.");
			}
		}
		if (out.confirm_swallowed)
		{
			s_open_confirms++;
			if (LogLine())
				Console.WriteLn("(VR) MenuPointer: confirm swallowed (the cursor had not arrived).");
		}

		std::lock_guard<std::mutex> lock(s_telemetry_mutex);
		Telemetry& t = s_telemetry;
		t.hit_valid = hit.valid;
		t.hit_right = hit.valid && hit.hand == VRInputSnapshot::RIGHT;
		t.test_hit = hit.valid && hit.test;
		t.u = hit.valid ? hit.u : 0.0f;
		t.v = hit.valid ? hit.v : 0.0f;
		t.hit_distance = hit.valid ? hit.distance : 0.0f;
		t.menu_mode = menu_mode;
		t.gate = in.gate;
		t.in_flight = out.in_flight;
		t.confirm_held = out.confirm_hold;
		t.suspended = out.suspended;
		t.selfcheck_failed = out.selfcheck_failed;
		t.refused_online = screen_open && !allowed;
		t.hovered = static_cast<s8>(out.hovered);
		t.current = static_cast<s8>(out.current);
		t.target = static_cast<s8>(out.target);
		t.presses_target = static_cast<u8>(std::min<u32>(out.presses, 255));
		t.dir = static_cast<s8>(out.dir);
		t.node_raw = node;
		t.presses_total += out.pressed ? 1u : 0u;
		t.arrivals += out.arrived ? 1u : 0u;
		t.aborts += out.aborted ? 1u : 0u;
		t.ticks += out.landed ? 1u : 0u;
		t.confirms_passed += out.confirm_passed ? 1u : 0u;
		t.confirms_swallowed += out.confirm_swallowed ? 1u : 0u;
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

		// --- Ray and quad --------------------------------------------------------------------------------------------
		VRScreenTransform world;
		world.valid = true;
		world.center.valid = true;
		world.center.position_xyz = {0.0f, 1.5f, -2.5f};
		world.center.orientation_xyzw = {0.0f, 0.0f, 0.0f, 1.0f};
		world.half_width = 0.867f;
		world.half_height = 0.65f;
		const V3 fwd{0.0f, 0.0f, -1.0f};
		VRScreenHit h = RayHitScreen(world, AimAlong(V3{0.0f, 1.5f, 0.0f}, fwd));
		check(h.valid && std::fabs(h.u - 0.5f) < 1.0e-4f && std::fabs(h.v - 0.5f) < 1.0e-4f &&
				  std::fabs(h.distance - 2.5f) < 1.0e-3f && std::fabs(h.point[2] + 2.5f) < 1.0e-3f,
			"the screen's centre is u 0.5, v 0.5, 2.5 m out");
		h = RayHitScreen(world, AimAlong(V3{0.999f * 0.867f, 1.5f + 0.999f * 0.65f, 0.0f}, fwd));
		check(h.valid && h.u > 0.999f && h.v < 0.001f, "the top right corner is u 1, v 0");
		h = RayHitScreen(world, AimAlong(V3{-0.999f * 0.867f, 1.5f - 0.999f * 0.65f, 0.0f}, fwd));
		check(h.valid && h.u < 0.001f && h.v > 0.999f, "the bottom left corner is u 0, v 1");
		check(!RayHitScreen(world, AimAlong(V3{1.001f * 0.867f, 1.5f, 0.0f}, fwd)).valid, "just off the edge misses");
		check(!RayHitScreen(world, AimAlong(V3{0.0f, 1.5f, -5.0f}, V3{0.0f, 0.0f, 1.0f})).valid, "a ray from behind misses");
		check(!RayHitScreen(world, AimAlong(V3{0.0f, 1.5f, 0.0f}, V3{0.0f, 0.0f, 1.0f})).valid, "a ray pointing away misses");
		check(!RayHitScreen(world, AimAlong(V3{0.0f, 1.5f, -1.0f}, V3{1.0f, 0.0f, 0.0f})).valid, "a parallel ray misses");
		check(!RayHitScreen(world, AimAlong(V3{0.0f, 1.5f, -2.47f}, fwd)).valid, "a hit nearer than 5 cm misses");
		VRScreenTransform invalid = world;
		invalid.valid = false;
		check(!RayHitScreen(invalid, AimAlong(V3{0.0f, 1.5f, 0.0f}, fwd)).valid, "no screen, no hit");

		// The menu tablet as XRCompositor places it: anchored at the head, turned 30 deg, 0.7 m ahead, 0.3 m below,
		// tilted back to face the eyes.
		{
			const float yaw = 0.5235988f;
			const float d = 0.7f;
			const float drop = 0.3f;
			const float tilt = std::atan2(drop, d);
			const V3 head{0.2f, 1.2f, 0.1f};
			const float sx = std::sin(-0.5f * tilt), cx = std::cos(-0.5f * tilt);
			const float sy = std::sin(0.5f * yaw), cy = std::cos(0.5f * yaw);
			VRScreenTransform tablet;
			tablet.valid = true;
			tablet.tablet = true;
			tablet.center.valid = true;
			tablet.center.orientation_xyzw = {cy * sx, sy * cx, -sy * sx, cy * cx};
			tablet.center.position_xyz = {head.x - d * std::sin(yaw), head.y - drop, head.z - d * std::cos(yaw)};
			tablet.half_height = 0.18f;
			tablet.half_width = 0.24f;
			const V3 c{tablet.center.position_xyz[0], tablet.center.position_xyz[1], tablet.center.position_xyz[2]};
			const V3 normal = Rotate(tablet.center.orientation_xyzw, V3{0.0f, 0.0f, 1.0f});
			check(Dot(normal, Normalize(Sub(head, c))) > 0.9994f, "the tablet faces the eyes");
			const V3 target = Add(c, Rotate(tablet.center.orientation_xyzw, V3{0.1f, -0.05f, 0.0f}));
			h = RayHitScreen(tablet, AimAlong(head, Sub(target, head)));
			check(h.valid && std::fabs(h.u - (0.5f + 0.1f / 0.48f)) < 1.0e-3f && std::fabs(h.v - (0.5f + 0.05f / 0.36f)) < 1.0e-3f,
				"a tilted tablet hit gives the expected u, v");
		}

		// --- Hover and plan -----------------------------------------------------------------------------------------
		const ProfileDB::MenuPointerParams p = ItemScreenParams();
		check(HoverCell(p, 0.9385f, 0.2455f, -1) == 1, "the middle of cell 1 hovers cell 1");
		check(HoverCell(p, 0.9335f, 0.5185f, -1) == 4, "the middle of the special box hovers it");
		check(HoverCell(p, 0.894f, 0.25f, -1) == -1, "the edge between two cells hovers neither until well inside");
		check(HoverCell(p, 0.894f, 0.25f, 0) == 0, "a hovered cell stays hovered out to its full rect");
		check(HoverCell(p, 0.897f, 0.25f, 0) == -1 && HoverCell(p, 0.91f, 0.25f, 0) == 1,
			"past its rect the hover leaves, and enters the next inside its inset");
		check(HoverCell(p, 0.5f, 0.5f, 3) == -1, "off the cells, nothing");

		const auto presses = [&p](int from, int to) {
			int cur = from;
			int n = 0;
			while (cur != to && n < 10)
			{
				const int d = PlanStep(p, cur, to);
				if (d < 0)
					return -1;
				cur = p.cells[static_cast<size_t>(cur)].next[d];
				n++;
			}
			return (cur == to) ? n : -1;
		};
		check(presses(0, 3) == 2, "cell 0 to 3 takes two presses");
		check(presses(1, 4) == 2, "cell 1 to the special box takes two presses");
		check(presses(4, 1) == 3, "the special box to cell 1 takes three presses");
		check(PlanStep(p, 2, 2) == -1 && PlanStep(p, 0, 9) == -1, "no press to where it is, or to no cell");
		{
			ProfileDB::MenuPointerParams cut = p;
			cut.cells[2].next[1] = -1;
			cut.cells[3].next[1] = -1;
			check(PlanStep(cut, 0, 4) == -1, "an unreachable cell has no way there");
		}

		// --- The online rule ----------------------------------------------------------------------------------------
		check(SteeringAllowed(false, false, false) && SteeringAllowed(false, true, false), "offline: always");
		check(SteeringAllowed(true, true, true), "online in menu mode with coop: true");
		check(!SteeringAllowed(true, true, false) && !SteeringAllowed(true, false, true) && !SteeringAllowed(true, false, false),
			"online otherwise: never");

		// --- Steering against a scripted game -----------------------------------------------------------------------
		// The game takes a press held at least 2 polls after a gap of at least 2, and moves the cursor 2 polls later along
		// its own neighbours (which a test may make differ from the profile's).
		struct FakeGame
		{
			const ProfileDB::MenuPointerParams* layout = nullptr;
			int cell = 0;
			bool unknown = false; // the cursor on a node no cell has (a partner's panel)
			int x_off = 0;        // x read off by this much (a different layout)
			bool ignore = false;  // presses do nothing
			int held_dir = -1;
			int held_polls = 0;
			int released_polls = 99;
			int gap_before = 99;
			bool taken = false;
			int pending_dir = -1;
			int pending_in = 0;
			int presses = 0;
			int max_held = 0;
			int min_gap = 99;
			// A command submenu: with submenu_on_confirm a confirm reaching the game opens it at once, but its flag (what
			// suspendWhen reads) is seen only flag_delay polls later. While it is open a press the game takes moves its
			// highlight (counted), not the cursor, and the next confirm closes it, flag and all, at once.
			bool submenu_on_confirm = false;
			int flag_delay = 0;
			int flag_in = 0;
			bool submenu = false;
			bool flag = false;
			int submenu_presses = 0;

			void Confirmed()
			{
				if (submenu)
				{
					submenu = false;
					flag = false;
					flag_in = 0;
				}
				else if (submenu_on_confirm)
				{
					submenu = true;
					flag_in = flag_delay;
					flag = (flag_delay <= 0);
				}
			}
			void Advance()
			{
				if (flag_in > 0 && --flag_in == 0 && submenu)
					flag = true;
				if (pending_in > 0 && --pending_in == 0 && pending_dir >= 0)
				{
					const int to = layout->cells[static_cast<size_t>(cell)].next[pending_dir];
					if (to >= 0)
						cell = to;
					pending_dir = -1;
				}
			}
			void Fill(StepIn& in) const
			{
				in.cursor_ok = true;
				if (unknown)
				{
					in.node = 0x721330;
					in.index = 0;
					in.x = 498;
					in.y = 108;
					return;
				}
				const ProfileDB::MenuPointerCell& c = layout->cells[static_cast<size_t>(cell)];
				in.node = c.node;
				in.index = c.index;
				in.x = static_cast<u16>(c.x + x_off);
				in.y = c.y;
			}
			void See(int dir)
			{
				if (dir < 0)
				{
					held_dir = -1;
					held_polls = 0;
					released_polls++;
					return;
				}
				if (dir != held_dir)
				{
					gap_before = released_polls;
					min_gap = std::min(min_gap, released_polls);
					held_dir = dir;
					held_polls = 0;
					taken = false;
					presses++;
				}
				held_polls++;
				released_polls = 0;
				max_held = std::max(max_held, held_polls);
				if (!taken && !ignore && held_polls >= 2 && gap_before >= 2)
				{
					taken = true;
					if (submenu)
					{
						submenu_presses++;
					}
					else
					{
						pending_dir = dir;
						pending_in = 2;
					}
				}
			}
		};
		struct Sim
		{
			const ProfileDB::MenuPointerParams* p = nullptr;
			FakeGame game;
			State st;
			StepIn in;
			StepOut out;
			int polls = 0;
			int arrivals = 0;
			int arrive_poll = -1;
			int aborts = 0;
			const char* abort_reason = nullptr;
			int passed = 0;
			int swallowed = 0;
			int landed = 0;
			bool hold_dropped_in_flight = false;
			bool auto_suspend = false;  // in.suspend follows the game's submenu flag
			bool trigger_prev = false;  // the confirm controls as the game got them last poll
			int phantom = 0;            // confirms the game saw that the pointer never passed

			void Poll()
			{
				game.Advance();
				game.Fill(in);
				if (auto_suspend)
					in.suspend = game.flag;
				out = Step(*p, in, st);
				game.See(out.dir);
				// The confirm controls as the game gets them (the trigger, unless held back): a rising edge the pointer
				// did not pass would be a confirm the player never made at that moment.
				const bool trigger = in.confirm && !out.confirm_hold;
				if (trigger && !trigger_prev && !out.confirm_passed)
					phantom++;
				trigger_prev = trigger;
				if (out.confirm_passed)
					game.Confirmed();
				polls++;
				if (out.arrived)
				{
					arrivals++;
					if (arrive_poll < 0)
						arrive_poll = polls;
				}
				if (out.aborted)
				{
					aborts++;
					abort_reason = out.aborted;
				}
				passed += out.confirm_passed ? 1 : 0;
				swallowed += out.confirm_swallowed ? 1 : 0;
				landed += out.landed ? 1 : 0;
				if (out.in_flight && !out.confirm_hold)
					hold_dropped_in_flight = true;
			}
			void Run(int n)
			{
				for (int i = 0; i < n; i++)
					Poll();
			}
			// Points at the middle of a cell, or off the screen (-1).
			void Point(int cell)
			{
				in.hit = cell >= 0;
				if (cell >= 0)
				{
					const float* r = p->cells[static_cast<size_t>(cell)].rect;
					in.u = 0.5f * (r[0] + r[2]);
					in.v = 0.5f * (r[1] + r[3]);
				}
			}
			// The menu opens with the pointer off the screen and the arm delay passes.
			void Open()
			{
				game.layout = p;
				in.gate = true;
				Point(-1);
				Run(25);
			}
			void ResetCounts()
			{
				arrivals = 0;
				arrive_poll = -1;
				aborts = 0;
				abort_reason = nullptr;
				passed = 0;
				swallowed = 0;
				landed = 0;
				polls = 0;
				game.presses = 0;
			}
		};
		const auto same = [](const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; };

		// Arrival: two presses to cell 3, each held at most 2 polls with gaps of at least 2, a landing per cell; then
		// one to the special box and three back to cell 1.
		{
			Sim s;
			s.p = &p;
			s.Open();
			check(s.out.current == 0 && s.game.presses == 0 && !s.out.in_flight, "the menu opens at rest on cell 0");
			s.ResetCounts();
			s.Point(3);
			s.Run(40);
			check(s.arrivals == 1 && s.game.cell == 3 && s.arrive_poll > 0 && s.arrive_poll <= 20,
				"pointing at cell 3 brings the cursor there within 20 polls");
			check(s.game.presses == 2 && s.landed == 2, "cell 0 to 3: two presses and a landing tick per cell");
			check(s.game.max_held <= 2 && s.game.min_gap >= 2, "presses are held at most 2 polls with gaps of at least 2");
			check(s.out.dir < 0 && !s.out.in_flight && !s.out.confirm_hold && s.out.hovered == 3,
				"after arriving nothing is pressed or held, and cell 3 is hovered");
			s.ResetCounts();
			s.Point(4);
			s.Run(40);
			check(s.arrivals == 1 && s.game.cell == 4 && s.game.presses == 1, "cell 3 to the special box: one press");
			s.ResetCounts();
			s.Point(1);
			s.Run(60);
			check(s.arrivals == 1 && s.game.cell == 1 && s.game.presses == 3, "the special box to cell 1: three presses");
			check(s.game.max_held <= 2 && s.game.min_gap >= 2, "every press held at most 2 polls, every gap at least 2");
		}

		// The gate dropping mid-press releases in that poll, and nothing is pressed after.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.Point(3);
			int guard = 0;
			while (s.out.dir < 0 && guard++ < 30)
				s.Poll();
			check(s.out.dir >= 0, "a steer starts");
			s.in.gate = false;
			s.Poll();
			check(s.out.dir < 0 && !s.out.confirm_hold && s.out.aborted != nullptr, "the gate dropping releases in the same poll");
			const int before = s.game.presses;
			s.Run(30);
			check(s.game.presses == before && s.out.dir < 0, "nothing is pressed while the gate is down");
		}

		// The player taking over aborts the steer; the pointer waits for the hover to change, then steers again.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.ResetCounts();
			s.Point(4);
			int guard = 0;
			while (s.out.dir < 0 && guard++ < 30)
				s.Poll();
			s.in.right_stick = 1.0f;
			s.Poll();
			check(s.aborts == 1 && same(s.abort_reason, "you took over") && s.out.dir < 0, "the right stick takes over at once");
			s.in.right_stick = 0.0f;
			const int before = s.game.presses;
			s.Run(60);
			check(s.game.presses == before && s.aborts == 1, "no presses after an override while the hover stays");
			s.ResetCounts();
			s.Point(1);
			s.Run(60);
			check(s.arrivals == 1 && s.game.cell == 1, "a new hover steers again");
		}

		// A game that never lands where planned: stopped after maxPresses.
		{
			ProfileDB::MenuPointerParams wrong = p;
			wrong.cells[0].next[3] = 2; // Right from cell 0 really goes down to cell 2
			Sim s;
			s.p = &p;
			s.Open();
			s.game.layout = &wrong;
			s.ResetCounts();
			s.Point(1);
			s.Run(150);
			check(s.aborts == 1 && same(s.abort_reason, "too many presses") && s.game.presses == p.max_presses && s.arrivals == 0,
				"a cursor that never gets there stops after maxPresses presses");
		}

		// A game that ignores the presses: stopped after settleVsyncs, and not retried while the hover stays.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.game.ignore = true;
			s.ResetCounts();
			s.Point(1);
			s.Run(120);
			check(s.aborts == 1 && same(s.abort_reason, "the cursor did not move") && s.game.presses == 1,
				"a cursor that does not move stops the steer after one press");
		}

		// The cursor leaving the cells (a partner's panel) stops the steer.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.ResetCounts();
			s.Point(4);
			int guard = 0;
			while (s.game.cell == 0 && guard++ < 30)
				s.Poll();
			s.game.unknown = true;
			s.Poll();
			check(s.aborts == 1 && same(s.abort_reason, "the cursor left the cells") && s.out.dir < 0 && s.out.current < 0,
				"an unknown node stops the steer");
			const int before = s.game.presses;
			s.Run(30);
			check(s.game.presses == before, "and nothing is pressed after");
		}

		// The cursor struct disagreeing with a cell (x off by one): at rest the whole opening.
		{
			Sim s;
			s.p = &p;
			s.game.x_off = 1;
			s.Open();
			s.ResetCounts();
			s.Point(3);
			s.Run(60);
			check(s.game.presses == 0 && s.out.selfcheck_failed && s.out.hovered < 0, "a failed self-check stays at rest");
		}

		// The cell under the pointer as the menu opens is ignored until the hover changes.
		{
			Sim s;
			s.p = &p;
			s.game.layout = &p;
			s.Point(3);
			s.in.gate = true;
			s.Run(90);
			check(s.game.presses == 0 && s.game.cell == 0, "the hover the menu opens with moves nothing");
			s.Point(1);
			s.Run(40);
			check(s.arrivals == 1 && s.game.cell == 1, "a changed hover steers");
		}

		// Fast click: the trigger pulled on another cell goes there at once, the confirm held back until the arrival.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.ResetCounts();
			s.Point(3);
			s.in.confirm = true;
			s.Poll();
			check(s.out.target == 3 && s.out.confirm_hold && s.out.dir >= 0, "a fast click sets the target and holds the confirm");
			bool early = false;
			int guard = 0;
			while (s.arrivals == 0 && guard++ < 30)
			{
				s.Poll();
				early = early || (s.arrivals == 0 && s.passed != 0);
			}
			check(s.arrivals == 1 && s.passed == 1 && !early && !s.out.confirm_hold && s.game.cell == 3 &&
					  !s.hold_dropped_in_flight,
				"the held confirm passes at the arrival, on cell 3");
			s.in.confirm = false;
			s.Poll();
		}

		// A press during the flight passes at the arrival; a tap let go before it is swallowed (one tick).
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.ResetCounts();
			s.Point(3);
			int guard = 0;
			while (!s.out.in_flight && guard++ < 30)
				s.Poll();
			s.in.confirm = true;
			s.Poll();
			check(s.out.confirm_hold && s.passed == 0, "a press during the flight is held back");
			guard = 0;
			while (s.arrivals == 0 && guard++ < 30)
				s.Poll();
			check(s.arrivals == 1 && s.passed == 1 && !s.out.confirm_hold, "and passes at the arrival");
			s.in.confirm = false;
			s.Point(-1);
			s.Poll();

			Sim t;
			t.p = &p;
			t.Open();
			t.ResetCounts();
			t.Point(4);
			guard = 0;
			while (!t.out.in_flight && guard++ < 30)
				t.Poll();
			t.in.confirm = true;
			t.Poll();
			t.in.confirm = false;
			t.Poll();
			check(t.swallowed == 1 && t.out.confirm_hold, "a tap let go before the arrival is swallowed");
			guard = 0;
			while (t.arrivals == 0 && guard++ < 40)
				t.Poll();
			check(t.arrivals == 1 && t.passed == 0 && t.swallowed == 1, "and nothing passes at the arrival");
		}

		// A trigger already held when a steer starts is swallowed until it is let go.
		{
			Sim s;
			s.p = &p;
			s.game.layout = &p;
			s.in.confirm = true;
			s.Poll(); // held with the menu closed: no fresh press once it opens
			s.Open();
			check(s.passed == 0, "a trigger held into the menu passes nothing new");
			s.ResetCounts();
			s.Point(3);
			int guard = 0;
			while (s.arrivals == 0 && guard++ < 40)
				s.Poll();
			check(s.arrivals == 1 && s.passed == 0 && s.out.confirm_hold, "a held trigger is swallowed at the steer's start");
			s.in.confirm = false;
			s.Poll();
			check(!s.out.confirm_hold, "and let go once released");
		}

		// The latch (no suspendWhen): a confirm that reaches the game pauses steering until Cross, a cursor move or close.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.ResetCounts();
			s.in.confirm = true;
			s.Poll();
			s.in.confirm = false;
			check(s.passed == 1 && s.out.suspended, "a confirm on the cursor's own cell passes and latches");
			s.Point(3);
			s.Run(40);
			check(s.game.presses == 0, "latched: pointing at another cell presses nothing");
			s.in.back = true;
			s.Poll();
			s.in.back = false;
			check(!s.out.suspended, "Cross lets the latch go");
			s.Point(1);
			s.Run(40);
			check(s.arrivals == 1 && s.game.cell == 1, "and steering resumes on the next hover");
			s.ResetCounts();
			s.in.confirm = true;
			s.Poll();
			s.in.confirm = false;
			check(s.out.suspended, "a confirm on the hovered cell latches again");
			s.game.cell = 3; // the cursor moved by itself
			s.Poll();
			check(!s.out.suspended, "the cursor reaching another cell lets the latch go");
			s.Run(40);
			check(s.arrivals == 1 && s.game.cell == 1, "and the pointer brings it back");
			s.in.confirm = true;
			s.Poll();
			s.in.confirm = false;
			s.in.gate = false;
			s.Poll();
			s.in.gate = true;
			s.Poll();
			check(!s.out.suspended && !s.st.latched, "closing the menu lets the latch go");
		}

		// The latch lives as long as the menu, not the gate: the dashboard, a pause or the setting off with the item
		// screen (and maybe a submenu) still up keeps it, so the D-pad never works the submenu when the gate comes back.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.ResetCounts();
			s.in.confirm = true;
			s.Poll();
			s.in.confirm = false;
			check(s.passed == 1 && s.st.latched, "a confirm latches before the gate drops");
			s.in.gate = false;
			s.in.screen_up = true;
			s.Run(5);
			check(s.st.latched && s.out.dir < 0 && s.game.presses == 0, "a gate drop with the menu still up keeps the latch");
			s.in.gate = true;
			s.Run(25); // past the arm delay, the pointer off the screen
			s.Point(3);
			s.Run(40);
			s.Point(1);
			s.Run(40);
			check(s.game.presses == 0 && s.st.latched && s.out.suspended,
				"back through the gate, pointing at other cells presses nothing while latched");
			s.in.back = true;
			s.Poll();
			s.in.back = false;
			check(!s.st.latched, "Cross lets the kept latch go");
			s.Point(3);
			s.Run(60);
			check(s.arrivals == 1 && s.game.cell == 3, "and steering resumes on the next hover");

			s.Point(-1);
			s.Run(5);
			s.in.confirm = true;
			s.Poll();
			s.in.confirm = false;
			check(s.st.latched, "a confirm latches again");
			s.in.gate = false;
			s.Run(3);
			s.game.cell = 2; // the cursor moved while the gate was down
			s.Poll();
			check(!s.st.latched, "the cursor reaching another cell behind the gate lets the latch go");
			s.in.screen_up = false;
			s.in.gate = true;
			s.Poll();
			s.in.confirm = true;
			s.Poll();
			s.in.confirm = false;
			s.in.gate = false;
			s.Poll();
			check(!s.st.latched, "the menu closing behind the gate lets the latch go");
		}

		// suspendWhen: steering waits while it passes and 6 polls after, then until the dot reaches another cell (the
		// cell it was left on, or leaving the cells and coming back, moves nothing); a confirm does not latch, it only
		// holds steering for kAfterConfirmMs.
		{
			ProfileDB::MenuPointerParams sp = p;
			sp.suspend_when.push_back(ProfileDB::CameraGuard{});
			Sim s;
			s.p = &sp;
			s.Open();
			s.ResetCounts();
			s.in.suspend = true;
			s.Point(3);
			s.Run(40);
			check(s.game.presses == 0 && s.out.suspended && s.out.submenu, "suspendWhen suspends steering");
			s.in.suspend = false;
			s.Run(5);
			check(s.game.presses == 0 && s.out.suspended, "for 6 polls after it clears");
			s.Run(60);
			check(s.game.presses == 0 && !s.out.suspended && !s.out.submenu && s.out.hovered < 0,
				"after a submenu, the cell the dot was left on moves nothing");
			s.Point(-1);
			s.Run(10);
			s.Point(3);
			s.Run(60);
			check(s.game.presses == 0, "nor does the dot leaving the cells and coming back to it");
			s.Point(1);
			s.Run(60);
			check(s.arrivals == 1 && s.game.cell == 1, "the dot reaching another cell steers again");
			s.Point(3);
			s.Run(60);
			check(s.arrivals == 2 && s.game.cell == 3, "and from then on as usual");
			s.in.confirm = true;
			s.Poll();
			s.in.confirm = false;
			s.Poll();
			check(s.passed == 1 && s.out.suspended && !s.st.latched, "with suspendWhen a confirm does not latch: it holds steering");
			s.Run(30);
			check(!s.out.suspended, "for kAfterConfirmMs only");
		}

		// suspendWhen: the poll a submenu's flag rises, a press under way is let go and the steer ends; nothing is
		// pressed while it is up, wherever the dot goes.
		{
			ProfileDB::MenuPointerParams sp = p;
			sp.suspend_when.push_back(ProfileDB::CameraGuard{});
			Sim s;
			s.p = &sp;
			s.Open();
			s.ResetCounts();
			s.Point(3);
			int guard = 0;
			while (s.out.dir < 0 && guard++ < 30)
				s.Poll();
			check(s.out.dir >= 0, "suspendWhen: a steer starts");
			s.in.suspend = true;
			s.Poll();
			check(s.out.dir < 0 && !s.out.in_flight && s.out.suspended && same(s.abort_reason, "a submenu is open"),
				"the press under way is let go the poll a submenu's flag rises");
			const int before = s.game.presses;
			s.Point(1);
			s.Run(40);
			s.Point(4);
			s.Run(40);
			check(s.game.presses == before && s.out.dir < 0 && s.out.hovered < 0,
				"nothing is pressed while a submenu is up, wherever the dot goes");
		}

		// suspendWhen: a confirm waiting for the cursor when a submenu opens never reaches the game. It stays held back
		// only while it is still held (so it can never turn into a submenu confirm the player did not make); once let
		// go the trigger is the player's, and a fresh pull passes straight through to the submenu.
		{
			ProfileDB::MenuPointerParams sp = p;
			sp.suspend_when.push_back(ProfileDB::CameraGuard{});
			Sim s;
			s.p = &sp;
			s.Open();
			s.ResetCounts();
			s.Point(4);
			int guard = 0;
			while (!s.out.in_flight && guard++ < 30)
				s.Poll();
			s.in.confirm = true;
			s.Poll();
			check(s.out.in_flight && s.out.confirm_hold && s.passed == 0, "a pull during the flight is held back");
			s.in.suspend = true;
			s.Poll();
			check(s.swallowed == 1 && s.passed == 0 && s.out.dir < 0 && !s.out.in_flight, "a submenu opening drops it");
			s.Run(10);
			check(s.passed == 0 && s.out.confirm_hold, "held back while it is still held");
			s.in.confirm = false;
			s.Poll();
			check(!s.out.confirm_hold, "and the trigger is the player's once let go");
			s.Point(1);
			s.Run(5);
			s.in.confirm = true;
			s.Poll();
			check(s.passed == 1 && !s.out.confirm_hold && s.out.target < 0 && s.out.dir < 0,
				"a fresh pull passes straight through to the submenu, wherever the dot is");
			s.in.confirm = false;
			s.Poll();
			check(s.phantom == 0, "the game never sees a confirm the pointer did not pass");
		}

		// The race: the game opens a command submenu as a confirm reaches it, but its flag is seen only 8 polls (133 ms)
		// later, and the dot goes straight on to another cell meanwhile. No press may go out in between (it would move
		// the submenu's highlight, and the next pull confirm the wrong entry), and closing the submenu with the player's
		// own pull moves nothing either.
		{
			ProfileDB::MenuPointerParams sp = p;
			sp.suspend_when.push_back(ProfileDB::CameraGuard{});
			Sim s;
			s.p = &sp;
			s.auto_suspend = true;
			s.game.submenu_on_confirm = true;
			s.game.flag_delay = 8;
			s.Open();
			s.ResetCounts();
			s.Point(0);
			s.Run(10);
			s.in.confirm = true;
			s.Poll();
			s.in.confirm = false;
			check(s.passed == 1 && s.game.submenu && s.out.suspended,
				"a confirm on the cursor's own cell opens its submenu and holds steering");
			s.Point(3);
			s.Run(60);
			check(s.game.flag && s.game.submenu_presses == 0 && s.game.presses == 0 && s.out.dir < 0,
				"nothing is pressed between a confirm and its submenu's flag, nor after");
			s.in.confirm = true;
			s.Poll();
			s.in.confirm = false;
			check(s.passed == 2 && !s.game.submenu, "the player's own pull confirms the submenu entry");
			s.Run(60);
			check(s.game.presses == 0 && s.game.cell == 0 && s.phantom == 0,
				"closing it moves nothing, and every confirm the game saw was one the pointer passed");
			s.Point(1);
			s.Run(60);
			check(s.arrivals == 1 && s.game.cell == 1, "after the submenu, the dot reaching another cell steers again");
		}

		// A confirm that opens no submenu: steering waits kAfterConfirmMs, then goes on to the cell the dot moved to.
		{
			ProfileDB::MenuPointerParams sp = p;
			sp.suspend_when.push_back(ProfileDB::CameraGuard{});
			Sim s;
			s.p = &sp;
			s.auto_suspend = true;
			s.Open();
			s.ResetCounts();
			s.Point(0);
			s.Run(10);
			s.in.confirm = true;
			s.Poll();
			s.in.confirm = false;
			s.Point(3);
			s.Run(15);
			check(s.passed == 1 && s.game.presses == 0 && s.out.suspended, "for 0.4 s after a confirm nothing is pressed");
			s.Run(40);
			check(s.arrivals == 1 && s.game.cell == 3 && !s.out.suspended, "with no submenu, steering then goes on");
		}

		// A gate drop (the dashboard, a pause) with a submenu up: back through the gate it is still suspended, and once
		// the player's own pull has closed it the cursor stays put until the dot reaches another cell.
		{
			ProfileDB::MenuPointerParams sp = p;
			sp.suspend_when.push_back(ProfileDB::CameraGuard{});
			Sim s;
			s.p = &sp;
			s.auto_suspend = true;
			s.game.submenu_on_confirm = true;
			s.game.flag_delay = 2;
			s.Open();
			s.ResetCounts();
			s.Point(0);
			s.Run(10);
			s.in.confirm = true;
			s.Poll();
			s.in.confirm = false;
			s.Run(10);
			check(s.game.flag && s.out.submenu, "the submenu opens");
			s.in.gate = false;
			s.in.screen_up = true;
			s.Run(10);
			s.Point(3);
			s.in.gate = true;
			s.Run(60);
			check(s.game.presses == 0 && s.game.submenu_presses == 0 && s.out.submenu,
				"back through the gate with the submenu up, nothing is pressed");
			s.in.confirm = true;
			s.Poll();
			s.in.confirm = false;
			s.Run(60);
			check(!s.game.submenu && s.game.presses == 0 && s.game.cell == 0, "once it has closed the cursor stays put");
			s.Point(1);
			s.Run(60);
			check(s.arrivals == 1 && s.game.cell == 1, "until the dot reaches another cell");
		}

		if (failures == 0)
			Console.WriteLn(Color_StrongGreen, "(VR) MenuPointer self-test: passed (%d checks: ray/quad, tablet, hover, plan, "
											   "online rule, steering, overrides, aborts, confirm, latch, suspendWhen, submenus).", checks);
		else
			Console.WriteLn(Color_StrongRed, "(VR) MenuPointer self-test FAIL: %s (%d of %d checks failed).", first, failures, checks);
		return failures == 0;
	}
}
