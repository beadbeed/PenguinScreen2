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
		// A user override (any of these) stops a steer: the right stick as far as the menu stick engages (and on down to
		// its release while engaged, where SpatialControls' menu stick keeps repeating), the left stick (the game's own
		// cursor stick) half way, a grip squeezed.
		constexpr float kOverrideRightStick = 0.6f;
		constexpr float kStickRelease = 0.35f;
		constexpr float kOverrideLeftStick = 0.5f;
		constexpr float kOverrideGrip = 0.6f;
		// After an override, re-arming waits kOverrideSettleMs from its last poll and then until the dot has rested
		// kRestMs on one hover (kOverrideSettleMaxMs at most): the controller springs back from the thumb's push
		// meanwhile, and the dot crossing cells then is not the player pointing.
		constexpr float kOverrideSettleMs = 250.0f;
		constexpr float kOverrideSettleMaxMs = 1000.0f;
		// A hover held this long is the dot resting there, not brushing it: after an override or a submenu, only the dot
		// resting on another cell arms again.
		constexpr float kRestMs = 100.0f;
		// The pull: the trigger past kPullStart has begun to move. From then until it reaches the confirm (kPullMaxMs at
		// most: longer, it is a finger resting on it) nothing is steered on its own, and the confirm it brings goes by the
		// hover just before it began, so the dip a pull gives the controller cannot redirect it. A, or a pull too quick to
		// see begin, goes by the hover kPullLookbackMs before. The cursor's cell (or a steer's target) wins while the dot
		// was within its rect grown by kPullGrow of its size on each side.
		constexpr float kPullStart = 0.1f;
		constexpr float kPullMaxMs = 500.0f;
		constexpr float kPullLookbackMs = 120.0f;
		constexpr float kPullGrow = 0.25f;
		// pull_cell's answer when there is no telling which cell a confirm was for: the dot came onto a cell other than the
		// cursor's without resting on any, from off the cells or en route, and no pull was seen begin on it. The confirm is
		// swallowed (one tick) rather than passed on the cursor's cell or sent to a cell the dot only crossed.
		constexpr int kPullUnsure = -2;
		// Steering stays suspended this many polls after a suspendWhen guard stops passing.
		constexpr u32 kSuspendTailPolls = 6;
		// Event lines logged per opening of the menu; the rest are counted and reported as it closes.
		constexpr u32 kLogLinesPerOpen = 24;
		// A belt pouch's request point-to-select has not taken by then lapses: long enough for the stop, the press and
		// the item screen opening after a squeeze with the screen closed, short enough not to steer long after.
		constexpr float kRequestLapseMs = 2500.0f;
		// A refused request: a dull buzz in the hand that squeezed the pouch, unlike any landing tick.
		constexpr SpatialControls::HapticCue kRequestRefusedTick{0.15f, 0.12f};

		std::atomic<int> s_hovered{-1};
		// The profile's menu (the item screen) was up and live at the last Poll, menu mode and the VM running: what
		// ItemScreenLive() returns.
		std::atomic<bool> s_item_screen_live{false};
		// RequestCell / SetPouchGrips (BeltPouches::Poll, just before Poll on the same thread).
		std::atomic<int> s_request_cell{-1};
		std::atomic<int> s_request_hand{-1};
		std::atomic<bool> s_pouch_grip_left{false};
		std::atomic<bool> s_pouch_grip_right{false};

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
		// liveWhen (LiveGate): a liveWhen guard has passed since the when guards last rose. While it is held the when
		// guards are read every running poll, so it goes the moment the menu closes, whatever else stopped meanwhile.
		bool s_live_seen = false;
		// A stretch of the when guards passing with a liveWhen (for its one log line as it ends), and whether a liveWhen
		// guard passed in it.
		bool s_when_stretch = false;
		bool s_when_stretch_live = false;
		int s_request_hand_now = -1;   // the hand of the latest pouch request
		int s_request_steer_hand = -1; // a steer a pouch started ticks in this hand until it ends
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
			st.unanswered = 0;
			st.unresponsive = false;
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
			st.hover_count = 0;
			st.hover_next = 0;
			st.settling = false;
			st.settle_ms = 0.0f;
			st.override_cells = 0;
			st.pull_from_valid = false;
		}

		// A negative inset grows the rect.
		bool InRect(const float r[4], float u, float v, float inset)
		{
			const float iu = inset * (r[2] - r[0]);
			const float iv = inset * (r[3] - r[1]);
			return u >= r[0] + iu && u <= r[2] - iu && v >= r[1] + iv && v <= r[3] - iv;
		}

		// The hover ring (State::hover): this poll's sample goes in once the hover is known.
		void PushHover(State& st, const HoverSample& h)
		{
			st.hover[st.hover_next] = h;
			st.hover_next = (st.hover_next + 1) % kHoverHistory;
			st.hover_count = std::min(st.hover_count + 1, kHoverHistory);
		}

		// The k-th newest sample (0 the newest); false past the oldest.
		bool HoverAgo(const State& st, u32 k, HoverSample* out)
		{
			if (k >= st.hover_count)
				return false;
			*out = st.hover[(st.hover_next + kHoverHistory - 1 - k) % kHoverHistory];
			return true;
		}

		// The newest sample at least ms older than the newest, and how many samples back it is (*k_out); false when the
		// ring does not reach that far back (the menu has only just opened).
		bool HoverBefore(const State& st, float ms, HoverSample* out, u32* k_out)
		{
			float age = 0.0f;
			HoverSample h;
			for (u32 k = 0; HoverAgo(st, k, &h); k++)
			{
				if (age >= ms)
				{
					*out = h;
					*k_out = k;
					return true;
				}
				age += h.dt_ms;
			}
			return false;
		}

		// The samples newer than the one k_from back, oldest first: the first cell the dot had rested on kRestMs by then
		// (-1 for none), and (*unbroken) whether the hover stayed on cell all the way.
		int RestedSince(const State& st, u32 k_from, int cell, bool* unbroken)
		{
			int rested = -1;
			*unbroken = cell >= 0;
			HoverSample h;
			for (u32 k = std::min(k_from, st.hover_count); k > 0; k--)
			{
				if (!HoverAgo(st, k - 1, &h))
					continue;
				if (h.under != cell)
					*unbroken = false;
				if (rested < 0 && h.under >= 0 && h.held_ms >= kRestMs)
					rested = h.under;
			}
			return rested;
		}

		// The cell the dot is on, else the one it was last on within ms; -1 for none.
		int RecentCell(const State& st, float ms)
		{
			if (st.under >= 0)
				return st.under;
			float age = 0.0f;
			HoverSample h;
			for (u32 k = 0; age <= ms && HoverAgo(st, k, &h); k++)
			{
				if (h.under >= 0)
					return h.under;
				age += h.dt_ms;
			}
			return -1;
		}

		u64 CellBit(int cell)
		{
			return (cell >= 0 && cell < 64) ? (u64{1} << cell) : u64{0};
		}

		// The samples within ms of the newest, newest first: the cell the dot last rested on kRestMs (-1 for none), and
		// (*cells) every cell it was on, a bit per cell.
		int RestedBefore(const State& st, float ms, u64* cells)
		{
			int rested = -1;
			*cells = 0;
			float age = 0.0f;
			HoverSample h;
			for (u32 k = 0; age <= ms && HoverAgo(st, k, &h); k++)
			{
				*cells |= CellBit(h.under);
				if (rested < 0 && h.under >= 0 && h.held_ms >= kRestMs)
					rested = h.under;
				age += h.dt_ms;
			}
			return rested;
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
			p.dwell_ms = 150.0f;
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

	bool ItemScreenLive()
	{
		return s_item_screen_live.load(std::memory_order_acquire);
	}

	void RequestCell(int cell_index, int hand)
	{
		// The hand first: Poll takes the cell, then reads the hand that goes with it.
		s_request_hand.store((hand == 0 || hand == 1) ? hand : -1, std::memory_order_release);
		s_request_cell.store(cell_index >= 0 ? cell_index : -1, std::memory_order_release);
	}

	void SetPouchGrips(bool left, bool right)
	{
		s_pouch_grip_left.store(left, std::memory_order_release);
		s_pouch_grip_right.store(right, std::memory_order_release);
	}

	float OverrideGrip(float left, float right, bool left_pouch, bool right_pouch)
	{
		const float l = (left_pouch || !std::isfinite(left)) ? 0.0f : left;
		const float r = (right_pouch || !std::isfinite(right)) ? 0.0f : right;
		return std::max(l, r);
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

	bool LiveGate(bool when_up, bool live_now, bool* seen)
	{
		if (!when_up)
		{
			*seen = false;
			return false;
		}
		if (live_now)
			*seen = true;
		return *seen;
	}

	StepOut Step(const ProfileDB::MenuPointerParams& p, const StepIn& in, State& st)
	{
		StepOut out;
		// The controls coming back from a gap (the SteamVR dashboard, a stalled snapshot), where everything read released:
		// a confirm still held is the one from before the gap, not a fresh pull. It makes no edge and stays held back until
		// let go, so neither the pointer nor the game takes it for a new press (VRInputSource's release-first latch marks
		// the same hold on the same poll and lets it go on the same release, so the next pull is free for both).
		if (in.actions_live && !st.actions_were_live && in.confirm)
		{
			st.swallow = true;
			st.confirm_prev = true;
		}
		st.actions_were_live = in.actions_live;
		// Not const: a confirm already held as the gate opens is no edge (below). A hold the gap check above caught makes
		// no edge here, so it is swallowed once, not twice.
		bool confirm_edge = in.confirm && !st.confirm_prev;
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
		// The right stick as the menu stick: engaged from kOverrideRightStick and held down to kStickRelease, where
		// SpatialControls' menu stick keeps pressing, so every press it makes counts as the player's.
		st.stick_engaged = in.right_stick >= (st.stick_engaged ? kStickRelease : kOverrideRightStick);
		// The pull: the trigger past kPullStart. Where the dot was just before it began (last poll's hover: this poll's dot
		// may already be dipping) is what the confirm it brings goes by.
		const float trigger = in.confirm ? 1.0f : (std::isfinite(in.trigger) ? std::clamp(in.trigger, 0.0f, 1.0f) : 0.0f);
		const bool pull_began = trigger >= kPullStart && !st.pull;
		if (trigger < kPullStart)
		{
			st.pull = false;
		}
		else if (pull_began)
		{
			st.pull = true;
			st.pull_ms = 0.0f;
			st.pull_polls = 0;
			st.pull_from_valid = HoverAgo(st, 0, &st.pull_from);
		}
		else
		{
			st.pull_ms += in.dt_ms;
			st.pull_polls++;
		}

		// A pouch's request waiting at the start of this poll (it held the confirm controls back last poll if the gate
		// was up).
		const bool request_was_waiting = st.request >= 0;

		// A belt pouch's request (RequestCell): the latest replaces an earlier one, and one point-to-select has not taken
		// within kRequestLapseMs lapses, menu up or not.
		if (in.request >= 0)
		{
			if (in.request < static_cast<int>(p.cells.size()))
			{
				st.request = in.request;
				st.request_ms = 0.0f;
			}
			else
			{
				out.request_refused = "no such cell";
				out.request_refused_cell = in.request;
			}
		}
		else if (st.request >= 0)
		{
			st.request_ms += in.dt_ms;
			if (st.request_ms > kRequestLapseMs)
			{
				out.request_lapsed = st.request;
				st.request = -1;
			}
		}

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
				// A pouch's request still waiting goes with the menu it was waiting in; one made with the menu closed
				// (st.open false) waits on for it to open.
				if (st.request >= 0)
				{
					out.request_refused = "the menu closed";
					out.request_refused_cell = st.request;
					st.request = -1;
				}
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
			// A confirm that reads pulled on the poll the gate opens was held through a gap before it that the
			// actions_live check at the top did not see (the controls read released meanwhile, so it looks fresh), or
			// pulled in the same poll the menu came up. Either way the player did not pull it for this menu: it is held,
			// not an edge, and held back until let go (the confirm hold keeps it from the game; after a real gap
			// VRInputSource's release-first latch does too, and both let go on the same release), so it neither
			// confirms the cell the cursor is on nor steers to the dot's cell to confirm there. A hold the top check
			// already caught makes no edge, so it never comes here.
			if (confirm_edge)
			{
				confirm_edge = false;
				st.swallow = true;
			}
		}
		else
		{
			st.open_ms += in.dt_ms;
		}

		// Landing: the cursor reached another cell, whatever moved it (a steer, the D-pad, a stick). One the pointer did
		// not make (it was not steering, or the cell is not the one its press leads to) is a move of the player's own, or
		// the game's: the left stick under the override, the menu stick, a layout other than the profile's.
		bool foreign = false;
		if (current >= 0 && current != st.last_cell)
		{
			out.landed = true;
			const bool planned = st.target >= 0 && st.phase != kIdle && st.dir >= 0 && st.dir < 4 && st.step_from >= 0 &&
			                     st.step_from < static_cast<int>(p.cells.size()) &&
			                     current == p.cells[static_cast<size_t>(st.step_from)].next[st.dir];
			foreign = !planned;
		}
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
		HoverSample sample;
		sample.under = static_cast<s8>(st.under);
		sample.hit = in.hit;
		sample.u = in.u;
		sample.v = in.v;
		sample.dt_ms = in.dt_ms;
		sample.held_ms = st.under_ms;
		PushHover(st, sample);
		// The cell under the pointer as the menu opened is ignored until the hover changes, so the cursor never jumps
		// on its own as a menu opens.
		if (opened)
			st.ignore = st.under;
		else if (st.ignore != -2 && st.under != st.ignore)
			st.ignore = -2;
		// The settle after an override (State::settling). The cell the dot comes to rest on becomes the one to hold off
		// from if the dot was on it while the player worked the control: the controller pushed off the cell it started
		// on by the thumb is not the player pointing anew. A cell the dot reaches only afterwards is.
		if (st.settling)
		{
			st.settle_ms += in.dt_ms;
			if ((st.settle_ms >= kOverrideSettleMs && st.under_ms >= kRestMs) || st.settle_ms >= kOverrideSettleMaxMs)
			{
				st.settling = false;
				if (st.under >= 0 && (st.override_cells & CellBit(st.under)) != 0)
					st.disarm_under = st.under;
			}
		}
		// Any change of hover arms again, except after an override or a submenu: there only the dot resting on a cell
		// other than the one it was on does, once an override has settled.
		if (st.disarmed && !st.settling && st.under != st.disarm_under &&
			(!st.disarm_strict || (st.under >= 0 && st.under_ms >= kRestMs)))
		{
			st.disarmed = false;
			st.disarm_strict = false;
		}

		// The player taking over stops any steer: another control (the menu stick all the while it is engaged), or the
		// cursor moved by something other than the pointer. The pointer then holds off from the cell the dot was resting
		// on as the override began, never one it brushes meanwhile, until the dot rests on another. The thumb's push, a
		// B press or a squeeze moves the controller before the control crosses its threshold, so the dot may already be
		// on a neighbour as the override engages: the cell it last rested on within kOverrideSettleMs is the one held
		// (else the one it is on, or was last on), and every cell it was on in that time counts as one the override
		// pushed it to.
		const bool took_over = in.dpad || st.stick_engaged || in.left_stick >= kOverrideLeftStick || back_edge || menu_edge ||
		                       in.grip >= kOverrideGrip;
		const bool override_now = took_over || foreign;
		if (override_now)
		{
			int hold_cell = st.disarm_under;
			if (!st.settling)
			{
				const int rested_cell = RestedBefore(st, kOverrideSettleMs, &st.override_cells);
				hold_cell = (rested_cell >= 0) ? rested_cell : RecentCell(st, kOverrideSettleMs);
			}
			abort_steer(took_over ? "you took over" : "the cursor moved off the planned path");
			st.disarmed = true;
			st.disarm_strict = true;
			st.disarm_under = hold_cell;
			st.override_cells |= CellBit(hold_cell) | CellBit(st.under);
			st.settling = true;
			st.settle_ms = 0.0f;
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
		// The pouches' suspendWhen (an item's command submenu is open) holds every steer, the pointer's as well as a
		// pouch's: the D-pad would work the submenu.
		suspended = suspended || in.pouch_suspend;

		// Suspended, nothing is pressed from this very poll: a press under way is let go and the steer ends. A confirm
		// it was holding for the arrival never reaches the game (held back only while still held, so it can never turn
		// into a confirm in a submenu the player did not make), and from here on the trigger passes straight through:
		// nothing is armed, so a fresh pull is the player's own confirm on whatever the game has up.
		const bool can_steer = !st.selfcheck_failed && !st.unresponsive && !suspended;
		if (!can_steer)
		{
			const char* why = "a confirm just reached the game";
			if (st.selfcheck_failed)
				why = "the cursor struct did not match";
			else if (st.unresponsive)
				why = "the cursor does not answer";
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

		// A belt pouch's request: steered like a pointed-at cell once the menu has been up armDelayMs (the game may not
		// take presses while the screen opens), unless steering is suspended or the self-check failed, or then the player
		// is taking over (a stick still pushed from walking may let go within the delay) or the cursor is off the cells,
		// which refuse it. It never confirms. The pointer then waits for its hover to change, so it does not pull the
		// cursor straight back.
		if (st.request >= 0)
		{
			const int to = st.request;
			const char* refuse = can_steer           ? nullptr :
			                     st.selfcheck_failed ? "the cursor struct did not match" :
			                     st.unresponsive     ? "the cursor does not answer" :
			                     (submenu || in.pouch_suspend || p.suspend_when.empty()) ? "a submenu is open" :
			                                                                             "a confirm just reached the game";
			const bool settled = st.open_ms >= p.arm_delay_ms;
			if (!refuse && settled)
				refuse = override_now ? "you took over" : (current < 0) ? "the cursor is not on the mapped cells" : nullptr;
			if (refuse)
			{
				st.request = -1;
				out.request_refused = refuse;
				out.request_refused_cell = to;
			}
			else if (settled)
			{
				st.request = -1;
				out.request_started = to;
				out.request_from = current;
				// A confirm waiting for the old target was meant for that cell: it never reaches the game.
				if (st.pending)
				{
					st.pending = false;
					out.confirm_swallowed = true;
					if (in.confirm)
						st.swallow = true;
				}
				if (to == current)
				{
					abort_steer("a pouch asked for the cell it is on");
					// A trigger held through the wait was held back: letting it through now would be a fresh press.
					if (in.confirm && !confirm_edge)
						st.swallow = true;
				}
				else if (st.target >= 0)
					retarget(to);
				else
					start_steer(to);
				st.disarmed = true;
				st.disarm_under = st.under;
			}
		}

		const bool armed = can_steer && !st.disarmed;
		out.hovered = armed ? st.under : -1;

		// The cell a confirm is for, judged from before the pull, so the dip a trigger pull gives the controller never
		// redirects it: the hover just before the trigger began to move, when that was seen (a pull begun on an earlier
		// poll, within kPullMaxMs), else the hover kPullLookbackMs before (A, or a pull too quick to see begin). ref (the
		// cursor's cell, or a steer's target) wins while the dot was then within its rect grown by kPullGrow, and without a
		// pull seen also while the dot still is. Otherwise it goes by rest, not just time: that hover's cell if the dot
		// had rested on it kRestMs, or has stayed on it since; else the first cell the dot rested on after it (a pull
		// begun as the hand still moved, off the cells or across a cell on the way); else, with the pull seen, the cell it
		// began on (a fast click). -1: no cell (the confirm stays with the cursor's own); kPullUnsure: the dot is on another
		// cell than ref with nothing to tell which one the player meant.
		const auto pull_cell = [&](int ref) {
			const bool seen = st.pull_from_valid && !pull_began && st.pull_ms <= kPullMaxMs;
			HoverSample from;
			u32 from_k = 0;
			if (seen)
			{
				from = st.pull_from;
				from_k = st.pull_polls + 1;
			}
			else if (!HoverBefore(st, kPullLookbackMs, &from, &from_k))
			{
				return -1;
			}
			const bool ref_ok = ref >= 0 && ref < static_cast<int>(p.cells.size());
			if (ref_ok && from.hit && InRect(p.cells[static_cast<size_t>(ref)].rect, from.u, from.v, -kPullGrow))
				return ref;
			if (ref_ok && !seen && in.hit && InRect(p.cells[static_cast<size_t>(ref)].rect, in.u, in.v, -kPullGrow))
				return ref;
			const int from_cell = static_cast<int>(from.under);
			bool unbroken = false;
			const int rested = RestedSince(st, from_k, from_cell, &unbroken);
			if (from_cell >= 0 && (from.held_ms >= kRestMs || unbroken))
				return from_cell;
			if (rested >= 0)
				return rested;
			if (seen && from_cell >= 0)
				return from_cell;
			return (st.under >= 0 && st.under != ref) ? kPullUnsure : -1;
		};
		// A confirm with no telling which cell it was for: it never reaches the game, held back until let go.
		const auto swallow_unsure = [&]() {
			st.swallow = true;
			out.confirm_swallowed = true;
			out.confirm_unsure = true;
		};

		if (confirm_edge)
		{
			if (st.target >= 0)
			{
				// Pulled while steering: it waits for the arrival; pulled with the dot on another cell before the pull, it
				// goes there instead.
				const int pulled_for = pull_cell(st.target);
				if (armed && pulled_for == kPullUnsure)
				{
					swallow_unsure();
				}
				else
				{
					if (armed && pulled_for >= 0 && pulled_for != st.target && current >= 0)
						retarget(pulled_for);
					st.pending = true;
				}
			}
			else if (st.request >= 0)
			{
				// A pouch's request still waiting (the arm delay): the cursor has not started for the pouch's cell, so the
				// cell it is on is not the one the player asked for. Like a confirm pulled before the cursor has arrived
				// it never reaches the game; the player pulls again once the cursor lands.
				st.swallow = true;
				out.confirm_swallowed = true;
			}
			else
			{
				const int pulled_on = pull_cell(current);
				if (armed && pulled_on == kPullUnsure)
				{
					swallow_unsure();
				}
				else if (armed && pulled_on >= 0 && current >= 0 && pulled_on != current)
				{
					// Pulled with the dot on another cell: straight there, no dwell, and the confirm waits for the arrival.
					start_steer(pulled_on);
					st.pending = true;
				}
				else
				{
					// It reaches the game now: on the cursor's own cell, or (suspended) on whatever the game has up, such
					// as the highlighted entry of a submenu. With suspendWhen, steering then waits kAfterConfirmMs for a
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
		}

		// Pointing at a cell: once it has held dwellMs (and the menu has been open armDelayMs), the cursor goes there. A
		// steer whose confirm is waiting keeps the cell it was pulled on, and nothing starts or turns while a pull is
		// under way (its dip must not steer either), nor on the poll its confirm arrives: that confirm was decided above
		// (passed on the cursor's cell, sent to the cell the pull began on, or swallowed), and a press now would go out
		// with it, the dip's cell taking the confirm the player made on another.
		const bool pulling = st.pull && !in.confirm && st.pull_ms <= kPullMaxMs;
		const bool passive = armed && !pulling && !confirm_edge && st.ignore == -2 && st.open_ms >= p.arm_delay_ms &&
		                     st.under >= 0 && st.under_ms >= p.dwell_ms && current >= 0;
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
				{
					abort_steer("the cursor did not move");
					// kMaxUnansweredPresses of these in one opening: no more presses until the menu opens again.
					st.unanswered++;
					if (st.unanswered >= kMaxUnansweredPresses && !st.unresponsive)
					{
						st.unresponsive = true;
						out.unresponsive_now = true;
					}
				}
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
		// A pouch's request held the confirm controls back while it waited (as a steer in flight does); a trigger still
		// held as it ends (taken, refused or lapsed) stays held back until let go, or the hold dropping would be a fresh
		// press on whatever cell the cursor is on. A fresh pull this poll was decided above (pending, or swallowed).
		if ((request_was_waiting || st.request >= 0) && in.confirm && !confirm_edge)
			st.swallow = true;
		out.confirm_hold = out.in_flight || st.swallow || (st.request >= 0);
		// As it stands after this poll (a confirm passed now has latched, or started its wait, already).
		out.suspended = (p.suspend_when.empty() ? st.latched : (suspended || st.after_confirm_ms > 0.0f)) || in.pouch_suspend;
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
		// A belt pouch's request made this poll (BeltPouches::Poll runs just before, on this thread).
		const int request = s_request_cell.exchange(-1, std::memory_order_acq_rel);
		if (request >= 0)
			s_request_hand_now = s_request_hand.load(std::memory_order_acquire);
		const ProfileDB::MenuPointerParams* params = nullptr;
		const std::vector<ProfileDB::CameraGuard>* pouch_guards = nullptr;
		bool screen_up = false;
		// Also while a pouch's request waits (the screen still opening): it needs the cells and the screen's guards.
		bool when_up = false;
		bool evaluated = false;
		if ((gate_conditions || s_state.latched || (s_live_seen && vm_running) || s_state.request >= 0 || request >= 0) &&
			VMManager::HasValidVM())
		{
			const std::string serial = VMManager::GetDiscSerial();
			const ProfileDB::Profile* profile = serial.empty() ? nullptr : ProfileDB::Lookup(serial, VMManager::GetDiscCRC());
			if (profile && profile->menu_pointer.has_value())
			{
				params = &profile->menu_pointer.value();
				evaluated = true;
				when_up = VRInputSource::EvaluateGuards(params->when, reader);
				// liveWhen: the when guards also pass on other screens (the map or pause after an item-screen visit), so
				// the menu counts as up only once one of its guards has passed since they rose.
				if (params->live_when.empty())
					screen_up = when_up;
				else
					screen_up = LiveGate(when_up, when_up && AnyGuardPasses(params->live_when, reader), &s_live_seen);
			}
			if (profile && profile->pouches.has_value() && !profile->pouches->suspend_when.empty())
				pouch_guards = &profile->pouches->suspend_when;
		}
		// Not looked at on a running poll (out of menu mode, no profile): the menu may have changed unseen, so the proof
		// that it is the menu proper goes. With the VM paused nothing can have changed.
		if (!evaluated && vm_running)
			s_live_seen = false;
		// One line as a stretch of the when guards passing ends without a liveWhen guard ever passing in it (the map or
		// pause), so a liveWhen that never matches the menu proper shows in the log.
		if (evaluated && when_up && !params->live_when.empty())
		{
			s_when_stretch = true;
			s_when_stretch_live = s_when_stretch_live || screen_up;
		}
		else if (s_when_stretch && (evaluated || vm_running))
		{
			if (!s_when_stretch_live)
				Console.WriteLn("(VR) MenuPointer: a menu was up without the profile's liveWhen passing (the map or pause, "
								"not the item screen); point-to-select stayed off.");
			s_when_stretch = false;
			s_when_stretch_live = false;
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

		// The pointer's hit: GetInputSnapshot's, which is where the compositor draws the dot (the right hand's aim ray; the
		// left's only standing in for a right controller that is not tracked, or for a PINE test hand), so the left
		// controller, which carries the Steam Frame's D-pad, never takes the pointer from the right one (hand right, or
		// either). A profile naming the left hand casts the left ray instead whenever that hit is the right's.
		VRScreenHit hit = snap.screen_hit;
		if (in.gate && params->hand == VRInputSnapshot::LEFT && hit.valid && !hit.test && hit.hand != params->hand)
		{
			hit = RayHitScreen(GetScreenTransform(), snap.hands[params->hand].aim_pose);
			hit.hand = params->hand;
		}
		in.hit = hit.valid;
		in.u = hit.u;
		in.v = hit.v;
		in.confirm = r.trigger >= 0.5f || r.a;
		// The pull's onset: the analog trigger, A counting as a full pull at once.
		in.trigger = r.a ? 1.0f : r.trigger;
		in.actions_live = live && snap.actions_active;
		// B lets a latch go because it is the game's cancel; with the VM paused the game never sees it.
		in.back = r.b && vm_running;
		in.menu = l.menu || r.menu;
		in.dpad = l.dpad_up || l.dpad_down || l.dpad_left || l.dpad_right || r.dpad_up || r.dpad_down || r.dpad_left ||
		          r.dpad_right;
		in.right_stick = std::sqrt(r.thumbstick_x * r.thumbstick_x + r.thumbstick_y * r.thumbstick_y);
		in.left_stick = std::sqrt(l.thumbstick_x * l.thumbstick_x + l.thumbstick_y * l.thumbstick_y);
		// A grip squeezed in a belt pouch is that pouch's request, not the player taking over.
		in.grip = OverrideGrip(l.grip, r.grip, s_pouch_grip_left.load(std::memory_order_acquire),
			s_pouch_grip_right.load(std::memory_order_acquire));
		in.suspend = in.gate && !params->suspend_when.empty() && AnyGuardPasses(params->suspend_when, reader);
		in.dt_ms = std::clamp(std::isfinite(dt) ? dt : 0.0f, 0.0f, 0.25f) * 1000.0f;
		in.request = request;
		in.pouch_suspend = in.gate && pouch_guards != nullptr && AnyGuardPasses(*pouch_guards, reader);

		static const ProfileDB::MenuPointerParams s_no_params{};
		const StepOut out = Step(params ? *params : s_no_params, in, s_state);

		SpatialControls::SetPointerDpad(out.dir);
		SpatialControls::SetPointerConfirmHold(out.confirm_hold);
		s_hovered.store(out.hovered, std::memory_order_release);

		// Ticks: in the hand of the pouch that started the steer while it runs, else in the pointing hand while the
		// pointer is on the screen, else the right.
		if (out.request_started >= 0)
			s_request_steer_hand = s_request_hand_now;
		const int tick_hand = (s_request_steer_hand == 0 || s_request_steer_hand == 1) ? s_request_steer_hand :
		                      (hit.valid && hit.hand >= 0 && hit.hand < 2)          ? hit.hand :
		                                                                              VRInputSnapshot::RIGHT;
		if (out.landed)
			XRInput::QueuePulse(tick_hand, SpatialControls::kHapticZoneEnter.amplitude, SpatialControls::kHapticZoneEnter.seconds);
		if (out.confirm_swallowed)
			XRInput::QueuePulse(tick_hand, SpatialControls::kHapticRelease.amplitude, SpatialControls::kHapticRelease.seconds);
		if (!out.in_flight)
			s_request_steer_hand = -1;
		// A pouch's request refused: a dull buzz in the hand that squeezed it.
		if (out.request_refused && (s_request_hand_now == 0 || s_request_hand_now == 1))
			XRInput::QueuePulse(s_request_hand_now, kRequestRefusedTick.amplitude, kRequestRefusedTick.seconds);

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
		s_item_screen_live.store(screen_open, std::memory_order_release);

		if (out.selfcheck_failed_now && !out.opened && LogLine())
		{
			Console.Warning("(VR) MenuPointer: the cursor struct no longer matches its cell (node %06X index %u x %u y %u); "
							"point-to-select off until the screen opens again.",
				node, index, x, y);
		}
		if (out.unresponsive_now && LogLine())
		{
			Console.Warning("(VR) MenuPointer: the cursor did not answer %u presses (node %06X index %u x %u y %u); "
							"point-to-select off until the screen opens again.",
				kMaxUnansweredPresses, node, index, x, y);
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
			{
				if (out.confirm_unsure)
					Console.WriteLn("(VR) MenuPointer: confirm swallowed (the dot had not rested on the cell it is on).");
				else
					Console.WriteLn("(VR) MenuPointer: confirm swallowed (the cursor had not arrived).");
			}
		}
		// Pouch requests are the player's own squeezes, a few a minute at most: always logged.
		if (out.request_started >= 0)
		{
			if (out.request_started == out.request_from)
				Console.WriteLn("(VR) MenuPointer: a pouch asked for cell %d; the cursor is already there.", out.request_started);
			else
				Console.WriteLn("(VR) MenuPointer: a pouch asked for cell %d; steering there from cell %d.", out.request_started,
					out.request_from);
		}
		if (out.request_refused)
			Console.WriteLn("(VR) MenuPointer: a pouch's request for cell %d refused (%s).", out.request_refused_cell,
				out.request_refused);
		if (out.request_lapsed >= 0)
			Console.WriteLn("(VR) MenuPointer: a pouch's request for cell %d lapsed (point-to-select did not take it within "
							"%.1f s).", out.request_lapsed, static_cast<double>(kRequestLapseMs) / 1000.0);

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
		t.request = static_cast<s8>(s_state.request);
		t.requests += (request >= 0) ? 1u : 0u;
		t.requests_started += (out.request_started >= 0) ? 1u : 0u;
		t.requests_refused += out.request_refused ? 1u : 0u;
		t.requests_lapsed += (out.request_lapsed >= 0) ? 1u : 0u;
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

		// The pointer's hit as GetInputSnapshot takes it (AimScreenHit), which the shipped profiles' hand: right keeps: the
		// right hand's ray; the left's only standing in for a right controller that is not tracked, or for a PINE test hand.
		{
			const VRPose left_on = AimAlong(V3{0.0f, 1.5f, 0.0f}, fwd);
			const VRPose right_on = AimAlong(V3{0.1f, 1.5f, 0.0f}, fwd);
			const VRPose right_away = AimAlong(V3{0.2f, 1.5f, 0.0f}, V3{0.0f, 0.0f, 1.0f});
			const VRPose untracked{};
			h = AimScreenHit(world, left_on, right_away, false);
			check(!h.valid, "the left controller's ray never takes the pointer from a tracked right one pointing away");
			h = AimScreenHit(world, left_on, right_on, false);
			check(h.valid && h.hand == VRInputSnapshot::RIGHT && std::fabs(h.u - (0.5f + 0.1f / (2.0f * 0.867f))) < 1.0e-3f,
				"the right hand's ray points");
			h = AimScreenHit(world, left_on, untracked, false);
			check(h.valid && h.hand == VRInputSnapshot::LEFT && std::fabs(h.u - 0.5f) < 1.0e-3f,
				"with the right controller not tracked, the left's ray stands in");
			h = AimScreenHit(world, left_on, right_away, true);
			check(h.valid && h.hand == VRInputSnapshot::LEFT, "a PINE test hand on the left points");
			h = AimScreenHit(world, left_on, right_on, true);
			check(h.valid && h.hand == VRInputSnapshot::RIGHT, "the right hand's ray comes first even then");
			check(!AimScreenHit(invalid, left_on, untracked, true).valid, "no screen, no hit for either hand");
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

		// --- liveWhen: the menu proper -----------------------------------------------------------------------------
		// File #1: the item screen opens in its own grid (liveWhen 0x723F94 = 1) and its cursor struct still reads 1 after
		// it closes; the menu flag is also up for the map and pause, where liveWhen reads 0.
		{
			bool live_seen = false;
			check(!LiveGate(false, false, &live_seen) && !live_seen, "no menu up: not live");
			check(LiveGate(true, true, &live_seen) && live_seen, "the item screen opens in its grid: live");
			check(LiveGate(true, false, &live_seen), "the cursor moving to the equipped box (liveWhen 0) stays live");
			check(!LiveGate(false, false, &live_seen) && !live_seen, "the item screen closing clears it");
			bool map_live = false;
			for (int i = 0; i < 60; i++)
				map_live = map_live || LiveGate(true, false, &live_seen);
			check(!map_live && !live_seen, "the map or pause after an item-screen visit (stale cursor struct) is never live");
			check(!LiveGate(false, false, &live_seen) && LiveGate(true, true, &live_seen), "the item screen again: live again");
		}

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
			// Points at (u, v) of the game picture.
			void Aim(float u, float v)
			{
				in.hit = true;
				in.u = u;
				in.v = v;
			}
			// A trigger pull seen beginning (the trigger part way for a poll), then crossing half: the confirm's edge.
			void Pull()
			{
				in.trigger = 0.3f;
				Poll();
				in.trigger = 1.0f;
				in.confirm = true;
				Poll();
			}
			void LetGo()
			{
				in.trigger = 0.0f;
				in.confirm = false;
				Poll();
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

		// A game that does not move where planned (Right from cell 0 really goes down to cell 2): the landing off the
		// planned path is a move the pointer did not make, so the steer stops at once and nothing more is pressed.
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
			check(s.aborts == 1 && same(s.abort_reason, "the cursor moved off the planned path") && s.game.presses == 1 &&
					  s.game.cell == 2 && s.arrivals == 0,
				"a cursor that lands off the planned path stops the steer after that press");
		}

		// A cell more presses away than maxPresses: stopped there.
		{
			ProfileDB::MenuPointerParams few = p;
			few.max_presses = 1;
			Sim s;
			s.p = &few;
			s.Open();
			s.ResetCounts();
			s.Point(3);
			s.Run(60);
			check(s.aborts == 1 && same(s.abort_reason, "too many presses") && s.game.presses == 1 && s.game.cell == 2 &&
					  s.arrivals == 0,
				"a cell more than maxPresses presses away stops the steer after maxPresses");
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
			// A second unanswered press in the same opening turns point-to-select off until the menu opens again (a stale
			// cursor struct under the map or pause would otherwise send a press on every change of hover).
			s.Point(3);
			s.Run(120);
			check(s.aborts == 2 && s.game.presses == 2 && !s.out.unresponsive_now && s.st.unresponsive,
				"a second unanswered press turns steering off for the opening");
			s.Point(2);
			s.Run(60);
			s.Point(1);
			s.Run(60);
			check(s.game.presses == 2 && s.out.hovered < 0 && s.out.dir < 0, "after it, no change of hover presses anything");
			s.in.gate = false;
			s.Poll();
			s.game.ignore = false;
			s.ResetCounts();
			s.Open();
			s.Point(3);
			s.Run(60);
			check(!s.st.unresponsive && s.arrivals == 1 && s.game.cell == 3, "the menu opening again steers again");
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

		// Fast click: a pull begun with the dot on another cell (before the dwell has brought the cursor there) goes there
		// at once, the confirm held back until the arrival.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.ResetCounts();
			s.Point(3);
			s.Run(2);
			s.Pull();
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
			s.LetGo();
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
			check(s.arrivals == 0 && s.game.presses == 0 && s.game.cell == 3,
				"and the pointer leaves it there: a move it did not make is the player's");
			s.Point(2);
			s.Run(60);
			check(s.arrivals == 1 && s.game.cell == 2, "the dot resting on another cell steers again");
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

		// A trigger held through an input gap (the SteamVR dashboard, a stale snapshot: the controls read released
		// meanwhile) is no fresh pull on the poll the gate opens again: it neither steers to the dot's cell to confirm there
		// nor confirms the cursor's own cell, and stays held back until let go.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.ResetCounts();
			s.in.gate = false;
			s.in.screen_up = true;
			s.Run(5);
			s.Point(3);
			s.in.gate = true;
			s.in.confirm = true;
			s.Poll();
			check(s.out.opened && s.passed == 0 && !s.out.in_flight && s.out.confirm_hold && s.st.swallow,
				"a trigger held through a gap, the dot on another cell: no steer, and held back as the gate opens");
			s.Run(40);
			check(s.passed == 0 && s.arrivals == 0 && s.game.presses == 0 && s.game.cell == 0 && s.out.confirm_hold &&
					  s.phantom == 0,
				"and while it stays held nothing is pressed or confirmed");
			s.in.confirm = false;
			s.Poll();
			check(!s.out.confirm_hold && s.passed == 0, "until it is let go");
			s.in.confirm = true;
			s.Run(40);
			s.in.confirm = false;
			s.Poll();
			check(s.passed == 1 && s.game.cell == 3 && s.phantom == 0, "then a fresh pull is the player's own");

			s.Point(-1);
			s.in.back = true; // Cross lets that confirm's latch go
			s.Poll();
			s.in.back = false;
			s.Poll();
			s.ResetCounts();
			s.in.gate = false;
			s.Run(5);
			s.in.gate = true;
			s.in.confirm = true;
			s.Poll();
			s.Run(10);
			check(s.passed == 0 && s.out.confirm_hold && s.phantom == 0 && !s.st.latched,
				"held through a gap on the cursor's own cell: nothing confirmed");
			s.in.confirm = false;
			s.Poll();
			check(!s.out.confirm_hold && s.passed == 0 && s.phantom == 0, "and nothing when it is let go");

			s.in.confirm = true;
			s.Poll();
			s.in.confirm = false;
			s.Poll();
			check(s.passed == 1 && s.st.latched, "a confirm on the cursor's own cell latches");
			s.ResetCounts();
			s.in.gate = false;
			s.Run(5);
			s.in.gate = true;
			s.in.confirm = true;
			s.Run(10);
			check(s.st.latched && s.passed == 0 && s.out.confirm_hold && s.phantom == 0,
				"held through a gap with the latch kept (the menu stayed up): nothing reaches what the game has up");
			s.in.confirm = false;
			s.Poll();
			check(!s.out.confirm_hold && s.passed == 0, "and it is free once let go");
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

		// --- The player's own moves, the pull and gaps ---------------------------------------------------------------
		// The player moves the cursor with the right stick while the dot rests on cell 0. The thumb's push jiggles the
		// controller: the dot leaves the cells and brushes cell 2 during the push and again after the stick is let go,
		// then comes back to cell 0. Nothing is pressed (the pointer never undoes the player's move), and a pull then
		// confirms the cell the stick chose.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.Point(0);
			s.Run(20);
			s.ResetCounts();
			s.in.right_stick = 1.0f;
			s.Point(-1);
			s.Run(2);
			s.Point(2);
			s.Run(2);
			s.game.cell = 1; // the menu stick's presses land
			s.Point(0);
			s.Run(3);
			s.game.cell = 3;
			s.Run(3);
			s.in.right_stick = 0.0f;
			s.Point(2);
			s.Run(2);
			s.Point(-1);
			s.Run(2);
			s.Point(0);
			s.Run(60);
			check(s.game.presses == 0 && s.game.cell == 3 && s.out.target < 0,
				"a stick move stands: the dot jiggling off the cells, onto another and back presses nothing");
			s.Pull();
			check(s.passed == 1 && s.out.confirm_cell == 3 && s.game.presses == 0, "and a pull confirms the cell the stick chose");
			s.LetGo();
		}

		// The menu stick eased back into its repeat band (under the 0.6 that engages it, over the 0.35 that lets it go) is
		// still the player working it: the dot moving to another cell meanwhile steers nothing, nor once the stick is let
		// go with the dot resting where the thumb left it.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.Point(0);
			s.Run(20);
			s.ResetCounts();
			s.in.right_stick = 1.0f;
			s.Run(3);
			s.in.right_stick = 0.5f;
			s.Point(1);
			s.Run(30);
			check(s.game.presses == 0, "the menu stick in its repeat band is still the player's: nothing is pressed");
			s.in.right_stick = 0.0f;
			s.Run(60);
			check(s.game.presses == 0 && s.game.cell == 0, "nor once it is let go with the dot where the thumb left it");
		}

		// The stick's push leaves the dot on cell 2 and it rests there: that is the controller, not the player pointing.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.Point(0);
			s.Run(20);
			s.ResetCounts();
			s.in.right_stick = 1.0f;
			s.Run(2);
			s.Point(2);
			s.Run(3);
			s.game.cell = 1;
			s.Run(3);
			s.in.right_stick = 0.0f;
			s.Run(60);
			check(s.game.presses == 0 && s.game.cell == 1, "the dot left on a cell the stick's push moved it to presses nothing");
		}

		// The thumb's push moves the controller before the stick crosses the override: the dot is already on cell 2 with
		// the stick at 0.4, then the stick engages (the cursor lands on cell 1) and is let go. Cell 0, where the dot was
		// resting, is the cell held, and cell 2 one the push moved it to: nothing is pressed, whether the dot springs back
		// to cell 0 or stays on cell 2.
		for (int back = 0; back < 2; back++)
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.Point(0);
			s.Run(20);
			s.ResetCounts();
			s.in.right_stick = 0.4f;
			s.Point(2);
			s.Run(2);
			s.in.right_stick = 1.0f;
			s.game.cell = 1;
			s.Run(3);
			s.in.right_stick = 0.0f;
			s.Point(back ? 0 : 2);
			s.Run(60);
			check(s.game.presses == 0 && s.game.cell == 1 && s.arrivals == 0,
				back ? "a stick push that moved the dot before it engaged stands, the dot back where it rested" :
				       "a stick push that moved the dot before it engaged stands, the dot left where the push moved it");
		}

		// A cell the dot reaches only after the stick is let go is the player pointing: it steers once the override has
		// settled.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.Point(0);
			s.Run(20);
			s.ResetCounts();
			s.in.right_stick = 1.0f;
			s.Run(3);
			s.game.cell = 1;
			s.Run(3);
			s.in.right_stick = 0.0f;
			s.Point(3);
			s.Run(60);
			check(s.arrivals == 1 && s.game.cell == 3, "a cell the dot reaches after the stick is let go steers once it settles");
		}

		// A cursor move the pointer did not make (the left stick under its override, the game itself) is the player's:
		// with the dot resting on cell 1, where the pointer brought the cursor, the cursor moves to cell 3 on its own.
		// Nothing is pressed, not even after the dot leaves the cells and comes back to cell 1; the dot resting on another
		// cell steers again.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.Point(1);
			s.Run(40);
			check(s.game.cell == 1, "the pointer brings the cursor to cell 1");
			s.ResetCounts();
			s.game.cell = 3;
			s.Run(60);
			check(s.game.presses == 0 && s.game.cell == 3, "a cursor move the pointer did not make stands");
			s.Point(-1);
			s.Run(10);
			s.Point(1);
			s.Run(60);
			check(s.game.presses == 0 && s.game.cell == 3, "nor does the dot leaving the cells and coming back undo it");
			s.Point(2);
			s.Run(60);
			check(s.arrivals == 1 && s.game.cell == 2, "the dot resting on another cell steers again");
		}

		// A pull's dip: the dot low in cell 0, where the cursor is; the trigger begins to move and the dot dips well into
		// cell 2 before it crosses half. The confirm goes to cell 0 at once, and nothing is pressed.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.Aim(0.851f, 0.29f);
			s.Run(30);
			s.ResetCounts();
			s.in.trigger = 0.2f;
			s.Poll();
			s.Aim(0.851f, 0.33f);
			s.in.trigger = 0.35f;
			s.Poll();
			s.Aim(0.851f, 0.37f);
			s.in.trigger = 0.45f;
			s.Poll();
			s.in.trigger = 0.8f;
			s.in.confirm = true;
			s.Poll();
			check(s.passed == 1 && s.out.confirm_cell == 0 && s.out.target < 0 && s.game.presses == 0,
				"a pull's dip onto cell 2 confirms cell 0, where the dot was as the pull began");
			s.LetGo();
		}

		// With A (no pull to see begin) the dot 120 ms before decides: A pressed as the dot dips onto cell 2 confirms cell 0.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.Aim(0.851f, 0.29f);
			s.Run(30);
			s.ResetCounts();
			s.Aim(0.851f, 0.37f);
			s.in.confirm = true;
			s.Poll();
			check(s.passed == 1 && s.out.confirm_cell == 0 && s.out.target < 0 && s.game.presses == 0,
				"A pressed as the dot dips onto cell 2 confirms cell 0");
			s.LetGo();
		}

		// A fast click with a dip: the pull begins with the dot low in cell 1 (the cursor still on cell 0) and dips onto
		// cell 3 before the trigger crosses half. The cursor goes to cell 1, where the dot was, and the confirm passes there.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.ResetCounts();
			s.Aim(0.9385f, 0.29f);
			s.Run(2);
			s.in.trigger = 0.3f;
			s.Poll();
			s.Aim(0.9385f, 0.37f);
			s.in.trigger = 1.0f;
			s.in.confirm = true;
			s.Poll();
			check(s.out.target == 1 && s.out.confirm_hold, "a fast click that dips onto cell 3 steers to cell 1, where it began");
			int guard = 0;
			while (s.arrivals == 0 && guard++ < 30)
				s.Poll();
			check(s.arrivals == 1 && s.passed == 1 && s.game.cell == 1, "and its confirm passes there");
			s.LetGo();
		}

		// A slow pull whose dip rests on cell 2 longer than dwellMs before the trigger crosses half: the confirm goes to
		// cell 0, where the dot rested as the pull began, and no press goes out with it, on that poll or after; with
		// suspendWhen or without.
		for (int with_suspend = 0; with_suspend < 2; with_suspend++)
		{
			ProfileDB::MenuPointerParams sp = p;
			if (with_suspend)
				sp.suspend_when.push_back(ProfileDB::CameraGuard{});
			Sim s;
			s.p = &sp;
			s.Open();
			s.Aim(0.851f, 0.29f);
			s.Run(30);
			s.ResetCounts();
			s.in.trigger = 0.2f;
			s.Poll();
			s.Aim(0.851f, 0.37f);
			s.in.trigger = 0.35f;
			s.Run(12);
			s.in.trigger = 0.8f;
			s.in.confirm = true;
			s.Poll();
			check(s.passed == 1 && s.out.confirm_cell == 0 && s.out.dir < 0 && !s.out.confirm_hold && s.out.target < 0,
				"a slow pull whose dip rests on cell 2 confirms cell 0, and no press goes out with it");
			s.Run(10);
			s.LetGo();
			check(s.passed == 1 && s.phantom == 0 && s.game.presses == 0 && s.game.cell == 0,
				"nor after: the game sees the one confirm and no D-pad");
		}

		// A pull begun as the hand still moves goes by where the dot comes to rest, not where it was as the pull began:
		// begun with the dot off the cells, the dot then resting on cell 3, the cursor goes to cell 3 and the confirm
		// passes there, never on the cursor's cell 0; begun as the dot crossed cell 1 on its way to rest on cell 3, the
		// same.
		for (int via = 0; via < 2; via++)
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.ResetCounts();
			if (via)
			{
				s.Point(1);
				s.Poll();
			}
			s.in.trigger = 0.3f;
			if (via)
				s.Point(3);
			s.Poll();
			s.Point(3);
			s.in.trigger = 0.4f;
			s.Run(13);
			s.in.trigger = 1.0f;
			s.in.confirm = true;
			s.Poll();
			check(s.passed == 0 && s.out.target == 3 && s.out.confirm_hold,
				via ? "a pull begun as the dot crossed cell 1, resting on cell 3 by the confirm, steers to cell 3" :
				      "a pull begun with the dot off the cells, resting on cell 3 by the confirm, steers to cell 3");
			int guard = 0;
			while (s.arrivals == 0 && guard++ < 30)
				s.Poll();
			check(s.arrivals == 1 && s.passed == 1 && s.game.cell == 3 && s.phantom == 0, "and its confirm passes on cell 3");
			s.LetGo();
		}

		// A pressed as the dot comes onto cell 3 from off the cells, resting nowhere: there is no telling which cell it
		// was for, so it is swallowed (held back until let go), not passed on the cursor's cell 0.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.ResetCounts();
			s.Point(3);
			s.Run(3);
			s.in.confirm = true;
			s.Poll();
			check(s.passed == 0 && s.swallowed == 1 && s.out.confirm_hold && s.out.target < 0 && s.out.dir < 0,
				"A pressed as the dot comes onto cell 3 without resting is swallowed, not passed on cell 0");
			s.Run(5);
			check(s.passed == 0 && s.phantom == 0 && s.out.confirm_hold, "and stays held back while it is held");
			s.LetGo();
		}

		// No steer starts while a pull is under way (its dip must not steer either): the dot goes to cell 3 with the
		// trigger part way, and nothing is pressed until it is let go; then the cursor follows.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.Point(0);
			s.Run(20);
			s.ResetCounts();
			s.in.trigger = 0.3f;
			s.Poll();
			s.Point(3);
			s.Run(20);
			check(s.game.presses == 0 && s.out.target < 0, "no steer starts while a pull is under way");
			s.in.trigger = 0.0f;
			s.Run(40);
			check(s.arrivals == 1 && s.game.cell == 3 && s.passed == 0, "let go, the cursor follows the dot");
		}

		// A finger resting on the trigger is no pull, even jittering about where a pull begins: after 0.5 s the cursor
		// follows the dot again.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.Point(0);
			s.Run(20);
			s.ResetCounts();
			s.Point(3);
			for (int i = 0; i < 90; i++)
			{
				s.in.trigger = (i % 2 == 0) ? 0.12f : 0.08f;
				s.Poll();
			}
			check(s.arrivals == 1 && s.game.cell == 3 && s.passed == 0,
				"a finger resting on the trigger, jittering where a pull begins, stops steering for 0.5 s only");
			s.in.trigger = 0.0f;
			s.Poll();
		}

		// A trigger held through a gap in the controls (the SteamVR dashboard, a stalled snapshot: everything reads
		// released, and the gate drops) is not a fresh pull when they come back: nothing reaches the game, and it stays held
		// back until let go.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.Point(3);
			s.Run(40);
			check(s.game.cell == 3, "the cursor on cell 3 before the gap");
			s.ResetCounts();
			s.in.actions_live = false;
			s.in.gate = false;
			s.in.screen_up = true;
			s.Run(10);
			s.in.actions_live = true;
			s.in.gate = true;
			s.in.trigger = 1.0f;
			s.in.confirm = true;
			s.Poll();
			check(s.passed == 0 && s.out.confirm_hold && s.out.target < 0, "a trigger held through a gap passes nothing as it ends");
			s.Run(20);
			check(s.passed == 0 && s.out.confirm_hold && s.phantom == 0, "and stays held back while it is held");
			s.LetGo();
			check(!s.out.confirm_hold, "let go, the trigger is the player's again");
			s.Run(30);
			s.Pull();
			check(s.passed == 1 && s.out.confirm_cell == 3, "and the next pull confirms as usual");
			s.LetGo();
		}

		// The two gap checks together (the controls coming back, StepIn::actions_live; the gate opening with a confirm
		// edge): a hold both could see is swallowed once and let go by one release, so the next real pull is never
		// swallowed again, whether the gate opens on the same poll as the controls or later.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.Point(3);
			s.Run(40);
			s.ResetCounts();
			s.in.actions_live = false;
			s.in.gate = false;
			s.in.screen_up = true;
			s.Run(10);
			s.in.actions_live = true;
			s.in.gate = true;
			s.in.trigger = 1.0f;
			s.in.confirm = true;
			s.Poll();
			check(s.out.opened && s.out.confirm_hold && s.st.swallow && s.passed == 0 && s.swallowed == 0,
				"the controls and the gate back on one poll, the trigger held: one hold, no swallowed confirm counted");
			s.Run(5);
			s.LetGo();
			check(!s.out.confirm_hold && !s.st.swallow, "one release lets it go");
			s.Run(30);
			s.Pull();
			check(s.passed == 1 && !s.out.confirm_hold && s.phantom == 0, "and the next pull passes, not swallowed again");
			s.LetGo();

			Sim t;
			t.p = &p;
			t.Open();
			t.ResetCounts();
			t.in.actions_live = false;
			t.in.gate = false;
			t.in.screen_up = true;
			t.Run(5);
			t.in.actions_live = true;
			t.in.confirm = true;
			t.Run(3);
			t.in.gate = true;
			t.Run(10);
			check(t.passed == 0 && t.out.confirm_hold && t.phantom == 0,
				"the controls back before the gate, the trigger held from the gap: still held back as the gate opens");
			t.LetGo();
			t.Run(30);
			t.Pull();
			check(t.passed == 1 && t.phantom == 0, "and a pull after letting go passes");
			t.LetGo();
		}

		// --- Belt pouch requests (RequestCell) ----------------------------------------------------------------------
		// What the requests did over some polls.
		struct RequestLog
		{
			int started = -1;
			const char* refused = nullptr;
			int lapsed = -1;
		};
		const auto note = [](const StepOut& o, RequestLog& rl) {
			if (o.request_started >= 0)
				rl.started = o.request_started;
			if (o.request_refused)
				rl.refused = o.request_refused;
			if (o.request_lapsed >= 0)
				rl.lapsed = o.request_lapsed;
		};
		// One poll with a request made in it.
		const auto ask = [&note](Sim& s, int cell, RequestLog& rl) {
			s.in.request = cell;
			s.Poll();
			s.in.request = -1;
			note(s.out, rl);
		};
		const auto run_noting = [&note](Sim& s, int n, RequestLog& rl) {
			for (int i = 0; i < n; i++)
			{
				s.Poll();
				note(s.out, rl);
			}
		};

		check(OverrideGrip(0.9f, 0.2f, true, false) == 0.2f && OverrideGrip(0.9f, 0.2f, false, false) == 0.9f &&
				  OverrideGrip(0.9f, 0.8f, true, true) == 0.0f,
			"a grip in a belt pouch is no override; the other hand's still is");

		// Made with the menu closed: kept until it opens, steered once the arm delay has passed, nothing confirmed.
		{
			Sim s;
			s.p = &p;
			s.game.layout = &p;
			RequestLog rl;
			ask(s, 3, rl);
			run_noting(s, 30, rl);
			check(s.st.request == 3 && s.game.presses == 0 && rl.started < 0 && !rl.refused,
				"a request made with the menu closed waits for it");
			s.in.gate = true;
			run_noting(s, 10, rl);
			check(s.game.presses == 0 && s.st.request == 3, "and for the arm delay once it is up");
			int guard = 0;
			while (s.arrivals == 0 && guard++ < 60)
			{
				s.Poll();
				note(s.out, rl);
			}
			check(rl.started == 3 && s.arrivals == 1 && s.game.cell == 3 && s.passed == 0 && s.st.request < 0,
				"then the cursor is steered to the pouch's cell, and nothing is confirmed");
			check(s.game.max_held <= 2 && s.game.min_gap >= 2, "with the pointer's own press timing");
		}

		// A trigger pulled while a request waits out the arm delay (the screen just appeared): the cursor is still on the
		// cell the screen opened on, so it never reaches the game, and the cursor still goes to the pouch's cell.
		{
			Sim s;
			s.p = &p;
			s.game.layout = &p;
			RequestLog rl;
			ask(s, 3, rl);
			run_noting(s, 4, rl);
			s.in.gate = true;
			run_noting(s, 5, rl);
			const bool held_waiting = s.out.confirm_hold;
			s.in.confirm = true;
			s.Poll();
			note(s.out, rl);
			check(held_waiting && s.passed == 0 && s.out.confirm_hold && s.st.request == 3,
				"a trigger pulled while a request waits for the arm delay does not reach the game");
			run_noting(s, 3, rl);
			s.in.confirm = false;
			int guard = 0;
			while (s.arrivals == 0 && guard++ < 60)
			{
				s.Poll();
				note(s.out, rl);
			}
			check(rl.started == 3 && s.arrivals == 1 && s.game.cell == 3 && s.passed == 0 && s.swallowed >= 1,
				"and the cursor is still steered to the pouch's cell, nothing confirmed");
			s.in.confirm = true;
			s.Poll();
			s.in.confirm = false;
			s.Poll();
			check(s.passed == 1 && s.out.current == 3, "a pull once it has landed confirms the pouch's cell");
		}

		// A trigger held through the wait (pulled before the screen came up) is not let through as the request ends.
		{
			Sim s;
			s.p = &p;
			s.game.layout = &p;
			RequestLog rl;
			s.in.confirm = true;
			ask(s, 0, rl);
			s.in.gate = true;
			run_noting(s, 40, rl);
			check(rl.started == 0 && s.passed == 0 && s.out.confirm_hold,
				"a trigger held while a request for the cursor's own cell waits stays held back after it");
			s.in.confirm = false;
			s.Poll();
			check(!s.out.confirm_hold && s.passed == 0, "until it is let go");
		}

		// Made with the screen up: it takes the cursor from the pointed-at cell, and the pointer waits for its hover to
		// change before it steers again.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.Point(1);
			s.Run(40);
			check(s.game.cell == 1, "the pointer brings the cursor to cell 1 first");
			s.ResetCounts();
			RequestLog rl;
			ask(s, 3, rl);
			run_noting(s, 60, rl);
			check(rl.started == 3 && s.arrivals == 1 && s.game.cell == 3 && s.passed == 0,
				"a request steers the cursor to its cell, away from the pointed-at one");
			check(s.out.hovered < 0 && s.out.dir < 0, "and the pointer still on cell 1 does not pull it back");
			s.Point(2);
			s.Run(60);
			check(s.game.cell == 2, "until it moves to another cell");
		}

		// Latched after a confirm (a command submenu may be up): refused, nothing pressed.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.ResetCounts();
			s.in.confirm = true;
			s.Poll();
			s.in.confirm = false;
			check(s.passed == 1 && s.st.latched, "a confirm on the cursor's cell latches");
			RequestLog rl;
			ask(s, 3, rl);
			run_noting(s, 40, rl);
			check(same(rl.refused, "a submenu is open") && s.game.presses == 0 && s.st.request < 0,
				"latched, a request is refused and nothing is pressed");
		}

		// The pouches' submenu guard: a request is refused, and a steer under way stops the poll it starts passing.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.ResetCounts();
			s.in.pouch_suspend = true;
			RequestLog rl;
			ask(s, 3, rl);
			run_noting(s, 40, rl);
			check(same(rl.refused, "a submenu is open") && s.game.presses == 0 && s.out.suspended,
				"with the pouches' submenu guard passing, a request is refused and nothing is pressed");
			s.in.pouch_suspend = false;
			s.Run(5);
			RequestLog rl_box;
			ask(s, 4, rl_box);
			int guard = 0;
			while (s.game.presses == 0 && guard++ < 30)
				s.Poll();
			check(rl_box.started == 4 && s.out.in_flight, "a request steers once the guard clears");
			s.in.pouch_suspend = true;
			s.Poll();
			check(s.out.dir < 0 && !s.out.in_flight && same(s.out.aborted, "a submenu is open"),
				"the guard passing mid-steer stops the steer at once");
			const int pressed_before = s.game.presses;
			s.Run(30);
			check(s.game.presses == pressed_before, "and nothing is pressed while it passes");
		}

		// Waiting in the arm delay when the menu closes: it goes with the menu.
		{
			Sim s;
			s.p = &p;
			s.game.layout = &p;
			s.in.gate = true;
			s.Run(3);
			RequestLog rl;
			ask(s, 3, rl);
			s.in.gate = false;
			run_noting(s, 3, rl);
			s.in.gate = true;
			run_noting(s, 60, rl);
			check(same(rl.refused, "the menu closed") && rl.started < 0 && s.game.presses == 0,
				"a request still waiting when the menu closes goes with it");
		}

		// Never taken: it lapses after 2.5 s and steers nothing when the menu opens later.
		{
			Sim s;
			s.p = &p;
			s.game.layout = &p;
			RequestLog rl;
			ask(s, 2, rl);
			run_noting(s, 160, rl);
			check(rl.lapsed == 2 && s.st.request < 0, "a request the menu never takes lapses after 2.5 s");
			s.in.gate = true;
			s.Run(60);
			check(s.game.presses == 0, "and steers nothing when the menu opens later");
		}

		// The cell the cursor is already on: nothing pressed.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.ResetCounts();
			RequestLog rl;
			ask(s, 0, rl);
			run_noting(s, 30, rl);
			check(rl.started == 0 && !rl.refused && s.game.presses == 0, "a request for the cell the cursor is on presses nothing");
		}

		// A trigger already held when a request starts is swallowed, not turned into a confirm on arrival.
		{
			Sim s;
			s.p = &p;
			s.game.layout = &p;
			s.in.confirm = true;
			s.Poll();
			s.Open();
			s.ResetCounts();
			RequestLog rl;
			ask(s, 3, rl);
			int guard = 0;
			while (s.arrivals == 0 && guard++ < 60)
				s.Poll();
			check(s.arrivals == 1 && s.passed == 0 && s.out.confirm_hold,
				"a trigger held when a request starts is held back, not confirmed");
			s.in.confirm = false;
			s.Poll();
			check(!s.out.confirm_hold, "and let go once released");
		}

		// A confirm waiting for the pointer's target was meant for that cell: a request elsewhere swallows it.
		{
			Sim s;
			s.p = &p;
			s.Open();
			s.ResetCounts();
			s.Point(4);
			s.Run(2);
			s.Pull();
			check(s.out.target == 4 && s.st.pending, "a fast click on the equipped box waits for the cursor");
			RequestLog rl;
			ask(s, 1, rl);
			check(rl.started == 1 && !s.st.pending && s.out.target == 1, "a request mid-flight retargets the steer");
			int guard = 0;
			while (s.arrivals == 0 && guard++ < 60)
				s.Poll();
			check(s.arrivals == 1 && s.game.cell == 1 && s.passed == 0 && s.swallowed == 1,
				"and the confirm meant for the equipped box never reaches the game");
			s.Point(-1);
			s.LetGo();
		}

		// A stick still pushed from walking as the screen opens keeps the request if let go within the arm delay; a
		// request made while the player works a stick is refused.
		{
			Sim s;
			s.p = &p;
			s.game.layout = &p;
			RequestLog rl;
			ask(s, 3, rl);
			s.in.left_stick = 1.0f;
			s.in.gate = true;
			run_noting(s, 8, rl);
			s.in.left_stick = 0.0f;
			int guard = 0;
			while (s.arrivals == 0 && guard++ < 60)
			{
				s.Poll();
				note(s.out, rl);
			}
			check(rl.started == 3 && s.arrivals == 1 && s.game.cell == 3, "a stick let go within the arm delay keeps the request");

			Sim t;
			t.p = &p;
			t.Open();
			t.ResetCounts();
			RequestLog rl_stick;
			t.in.left_stick = 1.0f;
			ask(t, 3, rl_stick);
			t.in.left_stick = 0.0f;
			run_noting(t, 30, rl_stick);
			check(same(rl_stick.refused, "you took over") && t.game.presses == 0, "a request made while a stick is pushed is refused");
		}

		// Out of range: refused at once.
		{
			Sim s;
			s.p = &p;
			s.Open();
			RequestLog rl;
			ask(s, 9, rl);
			check(same(rl.refused, "no such cell") && s.st.request < 0, "a request for a cell the profile does not have is refused");
		}

		// Pouch requests under suspendWhen (the submenu safety): refused while a submenu's flag is up and through its
		// tail, and in the wait after a confirm; once clear, a request steers (the strict re-arm after a submenu is the
		// pointer's, not a squeeze's), and nothing is ever confirmed for the player.
		{
			ProfileDB::MenuPointerParams sp = p;
			sp.suspend_when.push_back(ProfileDB::CameraGuard{});
			Sim s;
			s.p = &sp;
			s.Open();
			s.ResetCounts();
			s.in.suspend = true;
			s.Run(3);
			RequestLog rl_sub;
			ask(s, 3, rl_sub);
			run_noting(s, 30, rl_sub);
			check(same(rl_sub.refused, "a submenu is open") && rl_sub.started < 0 && s.game.presses == 0 && s.st.request < 0,
				"suspendWhen: a pouch's request with a submenu open is refused and presses nothing");
			s.in.suspend = false;
			s.Poll();
			RequestLog rl_tail;
			ask(s, 3, rl_tail);
			check(same(rl_tail.refused, "a submenu is open") && s.game.presses == 0, "suspendWhen: and in the submenu's tail");
			s.Run(20);
			s.in.confirm = true;
			s.Poll();
			s.in.confirm = false;
			RequestLog rl_wait;
			ask(s, 3, rl_wait);
			check(s.passed == 1 && same(rl_wait.refused, "a confirm just reached the game") && s.game.presses == 0,
				"suspendWhen: a pouch's request in the wait after a confirm is refused");
			s.Run(30);
			RequestLog rl_clear;
			ask(s, 3, rl_clear);
			int guard = 0;
			while (s.arrivals == 0 && guard++ < 60)
				s.Poll();
			check(rl_clear.started == 3 && s.arrivals == 1 && s.game.cell == 3 && s.passed == 1 && s.phantom == 0,
				"suspendWhen: once clear a pouch's request steers, and nothing is confirmed for the player");
		}

		if (failures == 0)
			Console.WriteLn(Color_StrongGreen, "(VR) MenuPointer self-test: passed (%d checks: ray/quad, tablet, hands, hover, "
											   "plan, online rule, liveWhen, steering, overrides, aborts, confirm, latch, suspendWhen, "
											   "submenus, the player's own moves, the pull's dip, gaps, pouch requests).", checks);
		else
			Console.WriteLn(Color_StrongRed, "(VR) MenuPointer self-test FAIL: %s (%d of %d checks failed).", first, failures, checks);
		return failures == 0;
	}
}
