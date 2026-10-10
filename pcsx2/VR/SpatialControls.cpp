// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/SpatialControls.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>

namespace VR::SpatialControls
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;
		constexpr float kDegToRad = kPi / 180.0f;

		float Clamp01(float v)
		{
			return std::clamp(v, 0.0f, 1.0f);
		}

		float Sign(float v)
		{
			return (v < 0.0f) ? -1.0f : 1.0f;
		}

		float Finite(float v)
		{
			return std::isfinite(v) ? v : 0.0f;
		}

		float Unwrap(float angle, float previous)
		{
			float d = angle - std::fmod(previous, 2.0f * kPi);
			while (d > kPi)
				d -= 2.0f * kPi;
			while (d < -kPi)
				d += 2.0f * kPi;
			return previous + d;
		}

		float SnapToDetent(float v, const Rail& rail)
		{
			if (rail.detents < 2 || !(rail.hi > rail.lo))
				return v;
			const float step = (rail.hi - rail.lo) / static_cast<float>(rail.detents - 1);
			const float k = std::round((v - rail.lo) / step);
			return std::clamp(rail.lo + k * step, rail.lo, rail.hi);
		}

		float RimAngle(const Vec3& local)
		{
			return std::atan2(local.x, local.y);
		}

		bool GrabRequested(const GrabParams& g, const HandInput& h, const Reach& r)
		{
			if (!h.valid || !r.in_reach)
				return false;
			return (g.grab_mode == GrabMode::Toggle) ? r.grip_edge : (h.squeeze > g.grab_on);
		}
		bool ReleaseRequested(const GrabParams& g, const HandInput& h, const Reach& r, bool* broke)
		{
			*broke = false;
			if (!h.valid)
				return true;
			if (g.break_away > 0.0f && Finite(r.distance) > g.break_away)
			{
				*broke = true;
				return true;
			}
			return (g.grab_mode == GrabMode::Toggle) ? r.grip_edge : (h.squeeze < g.grab_off);
		}
	}

	Vec3 operator+(const Vec3& a, const Vec3& b)
	{
		return {a.x + b.x, a.y + b.y, a.z + b.z};
	}
	Vec3 operator-(const Vec3& a, const Vec3& b)
	{
		return {a.x - b.x, a.y - b.y, a.z - b.z};
	}
	Vec3 operator*(const Vec3& a, float s)
	{
		return {a.x * s, a.y * s, a.z * s};
	}
	float Dot(const Vec3& a, const Vec3& b)
	{
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}
	float Length(const Vec3& a)
	{
		return std::sqrt(Dot(a, a));
	}

	Vec3 Quat::Rotate(const Vec3& v) const
	{
		const Vec3 q{x, y, z};
		const Vec3 t{2.0f * (q.y * v.z - q.z * v.y), 2.0f * (q.z * v.x - q.x * v.z), 2.0f * (q.x * v.y - q.y * v.x)};
		const Vec3 qxt{q.y * t.z - q.z * t.y, q.z * t.x - q.x * t.z, q.x * t.y - q.y * t.x};
		return {v.x + w * t.x + qxt.x, v.y + w * t.y + qxt.y, v.z + w * t.z + qxt.z};
	}

	Quat Quat::FromYaw(float radians)
	{
		return {0.0f, std::sin(radians * 0.5f), 0.0f, std::cos(radians * 0.5f)};
	}

	Quat Quat::Conjugate() const
	{
		return {-x, -y, -z, w};
	}

	Quat Quat::operator*(const Quat& o) const
	{
		return {
			w * o.x + x * o.w + y * o.z - z * o.y,
			w * o.y - x * o.z + y * o.w + z * o.x,
			w * o.z + x * o.y - y * o.x + z * o.w,
			w * o.w - x * o.x - y * o.y - z * o.z,
		};
	}

	Frame PlaceControl(const Anchor& anchor, const Placement& placement)
	{
		const Quat yaw = Quat::FromYaw(anchor.yaw);
		Frame f;
		f.pivot = anchor.position + yaw.Rotate({placement.side, placement.height, -placement.forward});
		f.orientation = yaw * Quat::FromYaw(placement.yaw_deg * kDegToRad);
		return f;
	}

	Hands HandsFromSnapshot(const VRInputSnapshot& snapshot)
	{
		Hands h;
		for (int i = 0; i < 2; ++i)
		{
			const VRHandState& src = snapshot.hands[i];
			HandInput& dst = h.hand[i];
			dst.valid = snapshot.generation != 0 && snapshot.actions_active && src.grip_pose.valid;
			dst.position = {src.grip_pose.position_xyz[0], src.grip_pose.position_xyz[1], src.grip_pose.position_xyz[2]};
			dst.orientation = {src.grip_pose.orientation_xyzw[0], src.grip_pose.orientation_xyzw[1],
				src.grip_pose.orientation_xyzw[2], src.grip_pose.orientation_xyzw[3]};
			dst.squeeze = Clamp01(Finite(src.grip));
			dst.trigger = Clamp01(Finite(src.trigger));
		}
		return h;
	}

	float ApplyIdleDetent(float v, float idle_detent)
	{
		return (std::fabs(v) < idle_detent) ? 0.0f : v;
	}

	float ApplyOutputFloor(float v, float deadband, float floor)
	{
		v = Finite(v);
		deadband = std::clamp(Finite(deadband), 0.0f, 0.999f);
		floor = std::clamp(Finite(floor), 0.0f, 1.0f);
		const float mag = std::fabs(v);
		if (mag <= deadband)
			return 0.0f;
		const float t = Clamp01((mag - deadband) / (1.0f - deadband));
		return Sign(v) * (floor + (1.0f - floor) * t);
	}

	float GripDistance(const Vec3& hand, const Vec3& grip_centre, const Vec3& grip_axis, float grip_length)
	{
		const float half = std::max(Finite(grip_length), 0.0f) * 0.5f;
		const float axis_len = Length(grip_axis);
		const Vec3 axis = (axis_len > 1.0e-6f) ? grip_axis * (1.0f / axis_len) : Vec3{};
		const Vec3 d = hand - grip_centre;
		const float along = std::clamp(Dot(d, axis), -half, half);
		return Length(d - axis * along);
	}

	void MarkGripEdges(const GrabParams& grab, const Hands& hands, GripEdges& st, std::array<Reach, 2>& reach)
	{
		for (int i = 0; i < 2; ++i)
		{
			const float squeeze = Clamp01(Finite(hands.hand[i].squeeze));
			const bool was = st.on[i];
			const bool now = was ? !(squeeze < grab.grab_off) : (squeeze > grab.grab_on);
			st.on[i] = now;
			reach[i].grip_edge = now && !was;
		}
	}

	void Step1D(const GrabParams& grab, const Rail& rail, const Hands& hands, const std::array<Reach, 2>& reach,
		float dt, Axis1D& st)
	{
		const float lo = std::min(rail.lo, rail.hi);
		const float hi = std::max(rail.lo, rail.hi);
		dt = std::clamp(Finite(dt), 0.0f, 0.25f);
		st.broke_away = false;

		if (st.Held())
		{
			const HandInput& h = hands.hand[st.hand];
			bool broke = false;
			if (ReleaseRequested(grab, h, reach[st.hand], &broke))
			{
				const bool by_press = !broke && h.valid && grab.grab_mode == GrabMode::Toggle;
				st.hand = -1;
				st.broke_away = broke;
				if (grab.release == ReleaseMode::Detent)
					st.value = SnapToDetent(st.value, rail);
				if (by_press)
					return;
			}
			else
			{
				const float raw = Finite(reach[st.hand].raw);
				const float unclamped = raw - st.offset;
				const float clamped = std::clamp(unclamped, lo, hi);
				if (clamped != unclamped)
					st.offset = raw - clamped;
				st.value = clamped;
				return;
			}
		}

		int best = -1;
		for (int i = 0; i < 2; ++i)
		{
			const HandInput& h = hands.hand[i];
			if (!GrabRequested(grab, h, reach[i]))
				continue;
			if (best < 0 || h.squeeze > hands.hand[best].squeeze)
				best = i;
		}
		if (best >= 0)
		{
			st.hand = best;
			st.offset = Finite(reach[best].raw) - st.value;
			return;
		}

		if (grab.release == ReleaseMode::Spring && st.value != 0.0f)
		{
			const float rest = std::clamp(0.0f, lo, hi);
			const float step = grab.spring_rate * dt;
			if (std::fabs(st.value - rest) <= step)
				st.value = rest;
			else
				st.value += (st.value > rest) ? -step : step;
		}
	}

	Vec3 SlideKnobPosition(const Frame& frame, const SlideParams& p, float value)
	{
		return frame.pivot + frame.orientation.Rotate(p.rail_dir) * (value * p.travel);
	}

	void StepSlide(const Frame& frame, const SlideParams& p, const GrabParams& grab, const Hands& hands, float dt,
		SlideState& st)
	{
		const Vec3 rail = frame.orientation.Rotate(p.rail_dir);
		const float travel = (p.travel > 1.0e-4f) ? p.travel : 1.0e-4f;
		const Vec3 knob = SlideKnobPosition(frame, p, st.axis.value);
		const Vec3 grip_axis = frame.orientation.Rotate(p.grip_dir);

		std::array<Reach, 2> reach{};
		MarkGripEdges(grab, hands, st.grip, reach);
		for (int i = 0; i < 2; ++i)
		{
			const HandInput& h = hands.hand[i];
			if (!h.valid)
				continue;
			reach[i].raw = Dot(h.position - frame.pivot, rail) / travel;
			reach[i].distance = GripDistance(h.position, knob, grip_axis, grab.grab_length);
			reach[i].in_reach = reach[i].distance <= grab.grab_radius;
		}

		Rail r;
		r.lo = p.one_way ? 0.0f : -1.0f;
		r.hi = 1.0f;
		r.detents = p.detents;
		Step1D(grab, r, hands, reach, dt, st.axis);
	}

	void StepRotary(const Frame& frame, const RotaryParams& p, const GrabParams& grab, const Hands& hands, float dt,
		RotaryState& st)
	{
		const float half_lock = std::max(p.lock_to_lock_deg * 0.5f, 1.0f) * kDegToRad;

		std::array<Reach, 2> reach{};
		std::array<Vec3, 2> local{};
		MarkGripEdges(grab, hands, st.grip, reach);
		st.second_broke_away = false;
		for (int i = 0; i < 2; ++i)
		{
			const HandInput& h = hands.hand[i];
			if (!h.valid)
			{
				st.cont_valid[i] = false;
				continue;
			}
			local[i] = frame.ToLocal(h.position);
			const float radial = std::sqrt(local[i].x * local[i].x + local[i].y * local[i].y);
			if (p.grip == RotaryGrip::Tip)
			{
				const float th = std::clamp(st.axis.value, -1.0f, 1.0f) * half_lock;
				const Vec3 tip{std::sin(th) * p.rim_radius, std::cos(th) * p.rim_radius, 0.0f};
				const Vec3 axis = p.tip_upright ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{std::sin(th), std::cos(th), 0.0f};
				reach[i].distance = GripDistance(local[i], tip, axis, grab.grab_length);
				reach[i].in_reach = reach[i].distance <= grab.grab_radius;
			}
			else
			{
				const float off_rim = radial - p.rim_radius;
				reach[i].distance = std::sqrt(off_rim * off_rim + local[i].z * local[i].z);
				reach[i].in_reach = std::fabs(off_rim) <= grab.grab_radius &&
									std::fabs(local[i].z) <= grab.grab_radius && radial > 1.0e-3f;
			}
			const float a = RimAngle(local[i]);
			st.cont_angle[i] = st.cont_valid[i] ? Unwrap(a, st.cont_angle[i]) : a;
			st.cont_valid[i] = true;
			reach[i].raw = st.cont_angle[i] / half_lock;
		}

		if (p.two_hand && st.axis.Held())
		{
			const int primary = st.axis.hand;
			const int other = 1 - primary;
			const HandInput& ho = hands.hand[other];
			bool other_broke = false;
			if (st.second_hand < 0 && GrabRequested(grab, ho, reach[other]))
			{
				const Vec3 d = local[other] - local[primary];
				const float a = std::atan2(d.x, d.y);
				st.pair_angle = st.pair_valid ? Unwrap(a, st.pair_angle) : a;
				st.pair_valid = true;
				st.second_hand = other;
				st.pair_offset = st.pair_angle / half_lock - st.axis.value;
			}
			else if (st.second_hand >= 0 && ReleaseRequested(grab, ho, reach[other], &other_broke))
			{
				st.second_hand = -1;
				st.pair_valid = false;
				st.second_broke_away = other_broke;
				st.axis.offset = reach[primary].raw - st.axis.value;
			}

			if (st.second_hand >= 0)
			{
				const HandInput& hp = hands.hand[primary];
				bool primary_broke = false;
				if (ReleaseRequested(grab, hp, reach[primary], &primary_broke))
				{
					st.axis.hand = st.second_hand;
					st.second_hand = -1;
					st.pair_valid = false;
					st.axis.broke_away = primary_broke;
					st.axis.offset = reach[st.axis.hand].raw - st.axis.value;
					return;
				}
				else
				{
					const Vec3 d = local[other] - local[primary];
					st.pair_angle = Unwrap(std::atan2(d.x, d.y), st.pair_angle);
					const float raw = st.pair_angle / half_lock;
					const float unclamped = raw - st.pair_offset;
					const float clamped = std::clamp(unclamped, -1.0f, 1.0f);
					if (clamped != unclamped)
						st.pair_offset = raw - clamped;
					st.axis.value = clamped;
					return;
				}
			}
		}
		else
		{
			st.second_hand = -1;
			st.pair_valid = false;
		}

		Rail r;
		r.lo = p.one_way ? 0.0f : -1.0f;
		r.hi = 1.0f;
		r.detents = p.detents;
		Step1D(grab, r, hands, reach, dt, st.axis);
	}

	namespace
	{
		float LeverHalfSweep(const LeverParams& p)
		{
			return std::max(p.sweep_deg * 0.5f, 1.0f) * kDegToRad;
		}
		float LeverArmLength(const LeverParams& p)
		{
			return std::max(Finite(p.arm_length), 1.0e-3f);
		}
		Vec3 LeverTipLocal(const LeverParams& p, float th)
		{
			return {std::sin(th) * LeverArmLength(p), std::cos(th) * LeverArmLength(p), 0.0f};
		}
		Vec3 LeverAxisLocal(const LeverParams& p, float th)
		{
			return p.upright ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{std::sin(th), std::cos(th), 0.0f};
		}
	}

	Frame LeverArmFrame(const Frame& control, const LeverParams& p)
	{
		Frame f;
		f.pivot = control.pivot - control.Up() * LeverArmLength(p);
		f.orientation = control.orientation * Quat::FromYaw(kPi * 0.5f);
		return f;
	}

	Vec3 LeverGripPosition(const Frame& control, const LeverParams& p, float value)
	{
		const float th = std::clamp(Finite(value), -1.0f, 1.0f) * LeverHalfSweep(p);
		return LeverArmFrame(control, p).ToWorld(LeverTipLocal(p, th));
	}

	Vec3 LeverGripAxis(const Frame& control, const LeverParams& p, float value)
	{
		const float th = std::clamp(Finite(value), -1.0f, 1.0f) * LeverHalfSweep(p);
		return LeverArmFrame(control, p).orientation.Rotate(LeverAxisLocal(p, th));
	}

	void StepLever(const Frame& control, const LeverParams& p, const GrabParams& grab, const Hands& hands, float dt,
		LeverState& st)
	{
		RotaryParams rp;
		rp.lock_to_lock_deg = std::max(p.sweep_deg, 2.0f);
		rp.rim_radius = LeverArmLength(p);
		rp.grip = RotaryGrip::Tip;
		rp.tip_upright = p.upright;
		rp.two_hand = false;
		rp.one_way = p.one_way;
		rp.detents = p.detents;
		StepRotary(LeverArmFrame(control, p), rp, grab, hands, dt, st);
	}

	Vec3 GimbalKnobPosition(const Frame& frame, const GimbalParams& p, const GimbalState& st)
	{
		return frame.pivot + frame.Right() * (st.x.value * p.travel_x) + frame.Up() * (st.y.value * p.travel_y);
	}

	void StepGimbal(const Frame& frame, const GimbalParams& p, const GrabParams& grab, const Hands& hands, float dt,
		GimbalState& st)
	{
		const Vec3 right = frame.Right();
		const Vec3 up = frame.Up();
		const float tx = (p.travel_x > 1.0e-4f) ? p.travel_x : 1.0e-4f;
		const float ty = (p.travel_y > 1.0e-4f) ? p.travel_y : 1.0e-4f;
		const Vec3 knob = GimbalKnobPosition(frame, p, st);

		std::array<Reach, 2> rx{}, ry{};
		MarkGripEdges(grab, hands, st.grip, rx);
		for (int i = 0; i < 2; ++i)
		{
			const HandInput& h = hands.hand[i];
			ry[i].grip_edge = rx[i].grip_edge;
			if (!h.valid)
				continue;
			const Vec3 d = h.position - frame.pivot;
			rx[i].raw = Dot(d, right) / tx;
			ry[i].raw = Dot(d, up) / ty;
			rx[i].distance = ry[i].distance = GripDistance(h.position, knob, up, grab.grab_length);
			rx[i].in_reach = ry[i].in_reach = rx[i].distance <= grab.grab_radius;
		}

		const int was = st.x.hand;
		Rail r;
		Step1D(grab, r, hands, rx, dt, st.x);
		Step1D(grab, r, hands, ry, dt, st.y);
		st.hand = st.x.hand;

		if (p.twist)
		{
			const float range = std::max(p.twist_range_deg * 0.5f, 1.0f) * kDegToRad;
			if (st.hand >= 0)
			{
				const HandInput& h = hands.hand[st.hand];
				if (was < 0)
				{
					st.grab_orientation = h.orientation;
					st.cont_twist = 0.0f;
					st.cont_twist_valid = true;
					st.twist.offset = 0.0f - st.twist.value;
				}
				const Quat rel = h.orientation * st.grab_orientation.Conjugate();
				const Vec3 swung = frame.orientation.Conjugate().Rotate(rel.Rotate(right));
				const float a = std::atan2(-swung.z, swung.x);
				st.cont_twist = st.cont_twist_valid ? Unwrap(a, st.cont_twist) : a;
				st.cont_twist_valid = true;
				const float raw = st.cont_twist / range;
				const float unclamped = raw - st.twist.offset;
				const float clamped = std::clamp(unclamped, -1.0f, 1.0f);
				if (clamped != unclamped)
					st.twist.offset = raw - clamped;
				st.twist.value = clamped;
				st.twist.hand = st.hand;
			}
			else
			{
				st.twist.hand = -1;
				st.cont_twist_valid = false;
				std::array<Reach, 2> none{};
				Step1D(grab, r, hands, none, dt, st.twist);
			}
		}
	}

	namespace
	{
		void SelectorArcSpan(const SelectorParams& p, float* lo, float* step_angle)
		{
			const float half = LeverHalfSweep(p.arc);
			*lo = p.arc.one_way ? 0.0f : -half;
			*step_angle = (half - *lo) / static_cast<float>(std::max(p.positions, 2) - 1);
		}
	}

	Vec3 SelectorKnobPosition(const Frame& frame, const SelectorParams& p, int index)
	{
		const int n = std::max(p.positions, 2);
		if (p.on_arc)
		{
			float lo = 0.0f, step_angle = 1.0f;
			SelectorArcSpan(p, &lo, &step_angle);
			const float th = lo + static_cast<float>(std::clamp(index, 0, n - 1)) * step_angle;
			return LeverArmFrame(frame, p.arc).ToWorld(LeverTipLocal(p.arc, th));
		}
		const float step = p.travel / static_cast<float>(n - 1);
		return frame.pivot + frame.orientation.Rotate(p.rail_dir) * (static_cast<float>(index) * step);
	}

	void StepSelector(const Frame& frame, const SelectorParams& p, const GrabParams& grab, const Hands& hands,
		float dt, SelectorState& st)
	{
		(void)dt;
		st.shifted_up = st.shifted_down = false;
		st.broke_away = false;
		const int n = std::max(p.positions, 2);
		st.index = std::clamp(st.index, 0, n - 1);
		const Vec3 knob = SelectorKnobPosition(frame, p, st.index);

		const float step = std::max(p.travel, 1.0e-4f) / static_cast<float>(n - 1);
		const Vec3 rail = frame.orientation.Rotate(p.rail_dir);
		const Frame arm = p.on_arc ? LeverArmFrame(frame, p.arc) : Frame{};
		float arc_lo = 0.0f, arc_step = 1.0f;
		if (p.on_arc)
			SelectorArcSpan(p, &arc_lo, &arc_step);
		auto path = [&](int i) {
			if (p.on_arc)
				return (RimAngle(arm.ToLocal(hands.hand[i].position)) - arc_lo) / arc_step;
			return Dot(hands.hand[i].position - frame.pivot, rail) / step;
		};
		const Vec3 grip_axis = p.on_arc ?
								   arm.orientation.Rotate(LeverAxisLocal(p.arc, arc_lo + static_cast<float>(st.index) * arc_step)) :
								   frame.orientation.Rotate(p.grip_dir);

		std::array<Reach, 2> reach{};
		MarkGripEdges(grab, hands, st.grip, reach);
		for (int i = 0; i < 2; ++i)
		{
			if (!hands.hand[i].valid)
				continue;
			reach[i].distance = GripDistance(hands.hand[i].position, knob, grip_axis, grab.grab_length);
			reach[i].in_reach = reach[i].distance <= grab.grab_radius;
		}

		if (st.hand >= 0)
		{
			const HandInput& h = hands.hand[st.hand];
			bool broke = false;
			if (ReleaseRequested(grab, h, reach[st.hand], &broke))
			{
				st.hand = -1;
				st.broke_away = broke;
				return;
			}
			const float q = path(st.hand) - st.offset;
			const float hyst = std::clamp(p.hysteresis, 0.0f, 0.49f);
			if (q > static_cast<float>(st.index) + 0.5f + hyst && st.index < n - 1)
			{
				st.index++;
				st.shifted_up = true;
			}
			else if (q < static_cast<float>(st.index) - 0.5f - hyst && st.index > 0)
			{
				st.index--;
				st.shifted_down = true;
			}
			return;
		}

		int best = -1;
		for (int i = 0; i < 2; ++i)
		{
			const HandInput& h = hands.hand[i];
			if (!GrabRequested(grab, h, reach[i]))
				continue;
			if (best < 0 || h.squeeze > hands.hand[best].squeeze)
				best = i;
		}
		if (best >= 0)
		{
			st.hand = best;
			st.offset = path(best) - static_cast<float>(st.index);
		}
	}

	bool StepResetChord(bool left_click, bool right_click, float dt, float hold_s, ChordState& st)
	{
		if (!(left_click && right_click))
		{
			st.held_s = 0.0f;
			st.fired = false;
			return false;
		}
		st.held_s += std::clamp(Finite(dt), 0.0f, 0.25f);
		if (st.fired || st.held_s < std::max(Finite(hold_s), 0.0f))
			return false;
		st.fired = true;
		return true;
	}

	namespace
	{
		// Indexed by ControlId: same order as the enum.
		constexpr const char* kControlNames[] = {
			"LeftLever", "RightLever", "Power", "Steer", "GrabbedLeft", "GrabbedRight",
			"Throttle", "Grabbed", "Notch1", "Notch2", "Notch3", "Notch4", "Notch5", "Notch6", "Notch7", "Notch8",
			"Steering",
			"Gear1", "Gear2", "Gear3", "Gear4", "Gear5", "Gear6", "GearR", "GearN", "ShiftUp", "ShiftDown",
			"X", "Y", "Twist", "Trigger",
			"PointerX", "PointerY", "OnScreen", "A", "B",
			"ButtonA", "ButtonB", "ButtonX", "ButtonY", "Menu", "LeftStickClick", "RightStickClick",
			"LeftTrigger", "RightTrigger", "LeftGrip", "RightGrip",
			"LeftStickX", "LeftStickY", "RightStickX", "RightStickY",
			"Pressed",
			"LeftTriggerPress", "RightTriggerPress", "LeftGripPress", "RightGripPress",
			"DpadUp", "DpadDown", "DpadLeft", "DpadRight", "LeftBumper", "RightBumper", "View",
			"RightStickUp", "RightStickDown",
		};
		static_assert(std::size(kControlNames) == kControlCount);

		constexpr ControlDef kThrottleControls[] = {
			{ControlId::Throttle, "Throttle", ControlType::Axis},
			{ControlId::Grabbed, "Grabbed", ControlType::Button},
			{ControlId::Notch1, "Notch1", ControlType::Button},
			{ControlId::Notch2, "Notch2", ControlType::Button},
			{ControlId::Notch3, "Notch3", ControlType::Button},
			{ControlId::Notch4, "Notch4", ControlType::Button},
			{ControlId::Notch5, "Notch5", ControlType::Button},
			{ControlId::Notch6, "Notch6", ControlType::Button},
			{ControlId::Notch7, "Notch7", ControlType::Button},
			{ControlId::Notch8, "Notch8", ControlType::Button},
		};
		constexpr ControlDef kTwinThrottlesControls[] = {
			{ControlId::LeftLever, "LeftLever", ControlType::Axis},
			{ControlId::RightLever, "RightLever", ControlType::Axis},
			{ControlId::Power, "Power", ControlType::Axis},
			{ControlId::Steer, "Steer", ControlType::Axis},
			{ControlId::GrabbedLeft, "GrabbedLeft", ControlType::Button},
			{ControlId::GrabbedRight, "GrabbedRight", ControlType::Button},
		};
		constexpr ControlDef kWheelControls[] = {
			{ControlId::Steering, "Steering", ControlType::Axis},
			{ControlId::GrabbedLeft, "GrabbedLeft", ControlType::Button},
			{ControlId::GrabbedRight, "GrabbedRight", ControlType::Button},
		};
		constexpr ControlDef kShifterControls[] = {
			{ControlId::Gear1, "Gear1", ControlType::Button},
			{ControlId::Gear2, "Gear2", ControlType::Button},
			{ControlId::Gear3, "Gear3", ControlType::Button},
			{ControlId::Gear4, "Gear4", ControlType::Button},
			{ControlId::Gear5, "Gear5", ControlType::Button},
			{ControlId::Gear6, "Gear6", ControlType::Button},
			{ControlId::GearR, "GearR", ControlType::Button},
			{ControlId::GearN, "GearN", ControlType::Button},
			{ControlId::ShiftUp, "ShiftUp", ControlType::Button},
			{ControlId::ShiftDown, "ShiftDown", ControlType::Button},
			{ControlId::Grabbed, "Grabbed", ControlType::Button},
		};
		constexpr ControlDef kStickControls[] = {
			{ControlId::X, "X", ControlType::Axis},
			{ControlId::Y, "Y", ControlType::Axis},
			{ControlId::Twist, "Twist", ControlType::Axis},
			{ControlId::Trigger, "Trigger", ControlType::UnitAxis},
			{ControlId::Grabbed, "Grabbed", ControlType::Button},
		};
		constexpr ControlDef kLightGunControls[] = {
			{ControlId::PointerX, "PointerX", ControlType::Axis},
			{ControlId::PointerY, "PointerY", ControlType::Axis},
			{ControlId::OnScreen, "OnScreen", ControlType::Button},
			{ControlId::Trigger, "Trigger", ControlType::UnitAxis},
			{ControlId::A, "A", ControlType::Button},
			{ControlId::B, "B", ControlType::Button},
			{ControlId::Grabbed, "Grabbed", ControlType::Button},
		};

		constexpr ControlDef kGamepadControls[] = {
			{ControlId::ButtonA, "ButtonA", ControlType::Button},
			{ControlId::ButtonB, "ButtonB", ControlType::Button},
			{ControlId::ButtonX, "ButtonX", ControlType::Button},
			{ControlId::ButtonY, "ButtonY", ControlType::Button},
			{ControlId::Menu, "Menu", ControlType::Button},
			{ControlId::LeftStickClick, "LeftStickClick", ControlType::Button},
			{ControlId::RightStickClick, "RightStickClick", ControlType::Button},
			{ControlId::LeftTrigger, "LeftTrigger", ControlType::UnitAxis},
			{ControlId::RightTrigger, "RightTrigger", ControlType::UnitAxis},
			{ControlId::LeftGrip, "LeftGrip", ControlType::UnitAxis},
			{ControlId::RightGrip, "RightGrip", ControlType::UnitAxis},
			{ControlId::LeftStickX, "LeftStickX", ControlType::Axis},
			{ControlId::LeftStickY, "LeftStickY", ControlType::Axis},
			{ControlId::RightStickX, "RightStickX", ControlType::Axis},
			{ControlId::RightStickY, "RightStickY", ControlType::Axis},
			{ControlId::LeftTriggerPress, "LeftTriggerPress", ControlType::Button},
			{ControlId::RightTriggerPress, "RightTriggerPress", ControlType::Button},
			{ControlId::LeftGripPress, "LeftGripPress", ControlType::Button},
			{ControlId::RightGripPress, "RightGripPress", ControlType::Button},
			{ControlId::DpadUp, "DpadUp", ControlType::Button},
			{ControlId::DpadDown, "DpadDown", ControlType::Button},
			{ControlId::DpadLeft, "DpadLeft", ControlType::Button},
			{ControlId::DpadRight, "DpadRight", ControlType::Button},
			{ControlId::LeftBumper, "LeftBumper", ControlType::Button},
			{ControlId::RightBumper, "RightBumper", ControlType::Button},
			{ControlId::View, "View", ControlType::Button},
			{ControlId::RightStickUp, "RightStickUp", ControlType::Button},
			{ControlId::RightStickDown, "RightStickDown", ControlType::Button},
		};
		constexpr ControlDef kZoneControls[] = {
			{ControlId::Pressed, "Pressed", ControlType::Button},
		};

		constexpr DeviceDef kCatalogue[] = {
			{DeviceKind::Throttle, "Throttle", kThrottleControls},
			{DeviceKind::TwinThrottles, "TwinThrottles", kTwinThrottlesControls},
			{DeviceKind::Wheel, "Wheel", kWheelControls},
			{DeviceKind::Shifter, "Shifter", kShifterControls},
			{DeviceKind::Stick, "Stick", kStickControls},
			{DeviceKind::LightGun, "LightGun", kLightGunControls},
			{DeviceKind::Gamepad, "Gamepad", kGamepadControls},
			{DeviceKind::Zone, "Zone", kZoneControls},
		};
		static_assert(std::size(kCatalogue) == static_cast<size_t>(DeviceKind::Count));

		float& At(ControlValues& v, ControlId id)
		{
			return v[static_cast<u32>(id)];
		}
	}

	std::span<const DeviceDef> Catalogue()
	{
		return kCatalogue;
	}

	ControlType ControlTypeOf(ControlId id)
	{
		for (const DeviceDef& d : kCatalogue)
		{
			for (const ControlDef& c : d.controls)
			{
				if (c.id == id)
					return c.type;
			}
		}
		return ControlType::Axis;
	}

	const DeviceDef* FindDevice(std::string_view name)
	{
		for (const DeviceDef& d : kCatalogue)
		{
			if (name == d.name)
				return &d;
		}
		return nullptr;
	}

	const DeviceDef* FindDevice(DeviceKind kind)
	{
		const u32 i = static_cast<u32>(kind);
		return (i < std::size(kCatalogue)) ? &kCatalogue[i] : nullptr;
	}

	const ControlDef* FindControl(const DeviceDef& device, std::string_view name)
	{
		for (const ControlDef& c : device.controls)
		{
			if (name == c.name)
				return &c;
		}
		return nullptr;
	}

	const ControlDef* FindControl(const DeviceDef& device, ControlId id)
	{
		for (const ControlDef& c : device.controls)
		{
			if (c.id == id)
				return &c;
		}
		return nullptr;
	}

	const char* ControlName(ControlId id)
	{
		const u32 i = static_cast<u32>(id);
		return (i < kControlCount) ? kControlNames[i] : "";
	}

	std::optional<ControlId> ParseControlId(std::string_view name)
	{
		for (u32 i = 0; i < kControlCount; ++i)
		{
			if (name == kControlNames[i])
				return static_cast<ControlId>(i);
		}
		return std::nullopt;
	}

	const char* DeviceName(DeviceKind kind)
	{
		const DeviceDef* d = FindDevice(kind);
		return d ? d->name : "";
	}

	Frame LeverFrame(const Frame& control, const TwinThrottlesParams& p, int side)
	{
		Frame f = control;
		const float half = p.lever_spacing * 0.5f;
		f.pivot = control.pivot + control.Right() * (side == 0 ? -half : half);
		return f;
	}

	void StepTwinThrottles(const Frame& frame, const TwinThrottlesParams& p, const Hands& hands, float dt,
		TwinThrottlesState& st)
	{
		Hands for_left = hands;
		if (st.right.axis.Held())
			for_left.hand[st.right.axis.hand].valid = false;
		StepLever(LeverFrame(frame, p, 0), p.lever, p.grab, for_left, dt, st.left);

		Hands for_right = hands;
		if (st.left.axis.Held())
			for_right.hand[st.left.axis.hand].valid = false;
		StepLever(LeverFrame(frame, p, 1), p.lever, p.grab, for_right, dt, st.right);

		const float left = ApplyIdleDetent(st.left.axis.value, p.grab.idle_detent);
		const float right = ApplyIdleDetent(st.right.axis.value, p.grab.idle_detent);
		st.engaged = TwinThrottlesEngage(TwinThrottlesPower(left, right, p), p, st.engaged);
	}

	bool TwinThrottlesEngage(float power, const TwinThrottlesParams& p, bool engaged)
	{
		if (!(p.engage > 0.0f))
			return false;
		const float on = p.engage;
		const float off = std::max(p.engage - std::max(p.engage_hysteresis, 0.0f), 0.0f);
		if (engaged)
			return !(power < off);
		return power >= on;
	}

	float TwinThrottlesPower(float left, float right, const TwinThrottlesParams& p)
	{
		const float power = (p.power == PowerMode::Max) ? std::max(left, right) : (left + right) * 0.5f;
		return std::clamp(power, 0.0f, 1.0f);
	}

	float TwinThrottlesSteer(float left, float right, const TwinThrottlesParams& p)
	{
		const float full = (std::fabs(p.steer_full_lock) > 1.0e-4f) ? p.steer_full_lock : 1.0f;
		float s = std::clamp((left - right) / full, -1.0f, 1.0f);
		const float mag = std::fabs(s);
		const float deadband = std::clamp(p.steer_deadband, 0.0f, 0.999f);
		if (mag <= deadband)
			return 0.0f;
		float t = (mag - deadband) / (1.0f - deadband);
		const float curve = std::clamp(p.steer_curve, 0.2f, 5.0f);
		t = std::pow(Clamp01(t), curve);
		const float signed_t = Sign(s) * t * ((p.steer_sign < 0.0f) ? -1.0f : 1.0f);
		return ApplyOutputFloor(signed_t, 0.0f, p.output_floor);
	}

	float TwinThrottlesLeverOutput(float lever, const TwinThrottlesParams& p)
	{
		const float s = std::clamp(Finite(lever), -1.0f, 1.0f);
		const float mag = std::fabs(s);
		const float deadband = std::clamp(p.steer_deadband, 0.0f, 0.999f);
		if (mag <= deadband)
			return 0.0f;
		float t = (mag - deadband) / (1.0f - deadband);
		const float curve = std::clamp(p.steer_curve, 0.2f, 5.0f);
		t = std::pow(Clamp01(t), curve);
		const float signed_t = Sign(s) * t * ((p.lever_sign < 0.0f) ? -1.0f : 1.0f);
		return ApplyOutputFloor(signed_t, 0.0f, p.output_floor);
	}

	ControlValues ComposeTwinThrottles(const TwinThrottlesParams& p, const TwinThrottlesState& st)
	{
		ControlValues v{};
		const float left = ApplyIdleDetent(st.left.axis.value, p.grab.idle_detent);
		const float right = ApplyIdleDetent(st.right.axis.value, p.grab.idle_detent);
		const bool direct = (p.mode == TwinMode::Direct);
		At(v, ControlId::LeftLever) = direct ? TwinThrottlesLeverOutput(left, p) : left;
		At(v, ControlId::RightLever) = direct ? TwinThrottlesLeverOutput(right, p) : right;
		At(v, ControlId::Power) = (p.engage > 0.0f) ? (st.engaged ? 1.0f : 0.0f) : TwinThrottlesPower(left, right, p);
		At(v, ControlId::Steer) = TwinThrottlesSteer(left, right, p);
		At(v, ControlId::GrabbedLeft) = st.left.axis.Held() ? 1.0f : 0.0f;
		At(v, ControlId::GrabbedRight) = st.right.axis.Held() ? 1.0f : 0.0f;
		return v;
	}

	void StepThrottle(const Frame& frame, const ThrottleParams& p, const Hands& hands, float dt, ThrottleState& st)
	{
		if (p.detented >= 2)
		{
			SelectorParams sp;
			sp.positions = std::min(p.detented, kMaxNotches);
			sp.on_arc = true;
			sp.arc = p.lever;
			StepSelector(frame, sp, p.grab, hands, dt, st.notch);
			const float t = static_cast<float>(st.notch.index) / static_cast<float>(sp.positions - 1);
			st.lever.axis.value = p.lever.one_way ? t : (t * 2.0f - 1.0f);
			st.lever.axis.hand = st.notch.hand;
			st.lever.axis.broke_away = st.notch.broke_away;
			return;
		}
		StepLever(frame, p.lever, p.grab, hands, dt, st.lever);
	}

	ControlValues ComposeThrottle(const ThrottleParams& p, const ThrottleState& st)
	{
		ControlValues v{};
		At(v, ControlId::Throttle) = ApplyIdleDetent(st.lever.axis.value, p.grab.idle_detent);
		At(v, ControlId::Grabbed) = st.lever.axis.Held() ? 1.0f : 0.0f;
		if (p.detented >= 2)
		{
			const int n = std::min(p.detented, kMaxNotches);
			for (int i = 0; i < n; ++i)
				v[static_cast<u32>(ControlId::Notch1) + i] = (st.notch.index == i) ? 1.0f : 0.0f;
		}
		return v;
	}

	void StepWheel(const Frame& frame, const WheelParams& p, const Hands& hands, float dt, WheelState& st)
	{
		StepRotary(frame, p.rim, p.grab, hands, dt, st.rotary);
	}

	ControlValues ComposeWheel(const WheelParams& p, const WheelState& st)
	{
		ControlValues v{};
		At(v, ControlId::Steering) = ApplyIdleDetent(st.rotary.axis.value, p.grab.idle_detent);
		const int h1 = st.rotary.axis.hand, h2 = st.rotary.second_hand;
		At(v, ControlId::GrabbedLeft) = (h1 == VRInputSnapshot::LEFT || h2 == VRInputSnapshot::LEFT) ? 1.0f : 0.0f;
		At(v, ControlId::GrabbedRight) = (h1 == VRInputSnapshot::RIGHT || h2 == VRInputSnapshot::RIGHT) ? 1.0f : 0.0f;
		return v;
	}

	int ShifterPositions(const ShifterParams& p)
	{
		return std::clamp(p.gears, 1, 6) + (p.has_reverse ? 1 : 0) + (p.has_neutral ? 1 : 0);
	}

	void StepShifter(const Frame& frame, const ShifterParams& p, const Hands& hands, float dt, ShifterState& st)
	{
		SelectorParams sp = p.gate;
		sp.positions = ShifterPositions(p);
		StepSelector(frame, sp, p.grab, hands, dt, st.sel);
	}

	ControlValues ComposeShifter(const ShifterParams& p, const ShifterState& st)
	{
		ControlValues v{};
		int i = 0;
		if (p.has_reverse)
			At(v, ControlId::GearR) = (st.sel.index == i++) ? 1.0f : 0.0f;
		if (p.has_neutral)
			At(v, ControlId::GearN) = (st.sel.index == i++) ? 1.0f : 0.0f;
		const int gears = std::clamp(p.gears, 1, 6);
		for (int g = 0; g < gears; ++g)
			v[static_cast<u32>(ControlId::Gear1) + g] = (st.sel.index == i + g) ? 1.0f : 0.0f;
		At(v, ControlId::ShiftUp) = st.sel.shifted_up ? 1.0f : 0.0f;
		At(v, ControlId::ShiftDown) = st.sel.shifted_down ? 1.0f : 0.0f;
		At(v, ControlId::Grabbed) = (st.sel.hand >= 0) ? 1.0f : 0.0f;
		return v;
	}

	void StepStick(const Frame& frame, const StickParams& p, const Hands& hands, float dt, StickState& st)
	{
		StepGimbal(frame, p.gimbal, p.grab, hands, dt, st.gimbal);
	}

	ControlValues ComposeStick(const StickParams& p, const StickState& st, const Hands& hands)
	{
		ControlValues v{};
		At(v, ControlId::X) = ApplyIdleDetent(st.gimbal.x.value, p.grab.idle_detent);
		At(v, ControlId::Y) = ApplyIdleDetent(st.gimbal.y.value, p.grab.idle_detent);
		At(v, ControlId::Twist) = p.gimbal.twist ? ApplyIdleDetent(st.gimbal.twist.value, p.grab.idle_detent) : 0.0f;
		const int h = st.gimbal.hand;
		At(v, ControlId::Trigger) = (h >= 0 && hands.hand[h].valid) ? hands.hand[h].trigger : 0.0f;
		At(v, ControlId::Grabbed) = (h >= 0) ? 1.0f : 0.0f;
		return v;
	}

	ControlValues ComposeLightGun(const LightGunParams& p, const Hands& hands)
	{
		ControlValues v{};
		const int h = (p.aim_hand == VRInputSnapshot::LEFT) ? VRInputSnapshot::LEFT : VRInputSnapshot::RIGHT;
		const HandInput& hand = hands.hand[h];
		At(v, ControlId::Trigger) = (hand.valid && hand.trigger > 0.5f) ? 1.0f : 0.0f;
		At(v, ControlId::A) = (hand.valid && hand.trigger > 0.5f) ? 0.0f : 0.0f;
		At(v, ControlId::Grabbed) = hand.valid ? 1.0f : 0.0f;
		return v;
	}

	namespace
	{
		std::atomic_int s_suppressed_move_hand{-1};

		// Radial deadzone with the travel above it rescaled to the full range: a stick resting slightly off
		// centre reads exactly 0, so it isn't re-sent every poll over another pad bound to the same stick.
		void GamepadStick(float x, float y, float& out_x, float& out_y)
		{
			constexpr float kDeadzone = 0.1f;
			x = std::clamp(Finite(x), -1.0f, 1.0f);
			y = std::clamp(Finite(y), -1.0f, 1.0f);
			const float mag = std::sqrt(x * x + y * y);
			if (!(mag > kDeadzone))
			{
				out_x = 0.0f;
				out_y = 0.0f;
				return;
			}
			const float scale = std::min((mag - kDeadzone) / (1.0f - kDeadzone), 1.0f) / mag;
			out_x = std::clamp(x * scale, -1.0f, 1.0f);
			out_y = std::clamp(y * scale, -1.0f, 1.0f);
		}

		std::atomic_bool s_sprint_blocked{false};

		constexpr float kSprintOn = 0.5f; // stick magnitude (after the deadzone) the click must come with
		constexpr float kSprintHold = 0.25f; // the latch holds while the stick is pushed past this
		constexpr float kSprintCentredS = 0.15f; // and drops once it has been centred this long
		constexpr float kFlickOn = 0.75f;
		constexpr float kFlickOff = 0.4f;
		constexpr float kFlickMinPressS = 0.12f; // a 30 fps game polls every 33 ms: hold a quick flick long enough

		// One flick button: presses on `press`, lets go on `released` once held kFlickMinPressS.
		void StepFlick(bool press, bool released, float dt, bool& on, float& held_s)
		{
			if (on)
			{
				held_s += dt;
				if (released && held_s >= kFlickMinPressS)
					on = false;
			}
			else if (press)
			{
				on = true;
				held_s = 0.0f;
			}
		}

		// ComposeGamepad with the cross-thread inputs passed in, so the self-test is deterministic.
		ControlValues ComposeGamepadWith(const VRInputSnapshot& snapshot, const GamepadParams& p, GamepadState& st,
			float dt, int suppressed, bool sprint_blocked)
		{
			ControlValues v{};
			if (snapshot.generation == 0 || !snapshot.actions_active)
			{
				st.Reset();
				return v;
			}
			dt = std::clamp(Finite(dt), 0.0f, 0.25f);
			const VRHandState& l = snapshot.hands[VRInputSnapshot::LEFT];
			const VRHandState& r = snapshot.hands[VRInputSnapshot::RIGHT];
			const auto button = [](bool b) { return b ? 1.0f : 0.0f; };
			const auto axis = [](float f) { return std::clamp(Finite(f), -1.0f, 1.0f); };
			At(v, ControlId::ButtonA) = button(r.a);
			At(v, ControlId::ButtonB) = button(r.b);
			// X, Y and Menu are on the left Touch controller but the right Steam Frame one.
			At(v, ControlId::ButtonX) = button(l.x || r.x);
			At(v, ControlId::ButtonY) = button(l.y || r.y);
			At(v, ControlId::Menu) = button(l.menu || r.menu);
			At(v, ControlId::LeftStickClick) = button(l.thumbstick_click);
			At(v, ControlId::RightStickClick) = button(r.thumbstick_click);
			At(v, ControlId::LeftTrigger) = Clamp01(Finite(l.trigger));
			At(v, ControlId::RightTrigger) = Clamp01(Finite(r.trigger));
			At(v, ControlId::LeftGrip) = Clamp01(Finite(l.grip));
			At(v, ControlId::RightGrip) = Clamp01(Finite(r.grip));
			float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
			if (suppressed != VRInputSnapshot::LEFT)
				GamepadStick(l.thumbstick_x, l.thumbstick_y, lx, ly);
			if (suppressed != VRInputSnapshot::RIGHT)
				GamepadStick(r.thumbstick_x, r.thumbstick_y, rx, ry);
			At(v, ControlId::LeftStickX) = lx;
			At(v, ControlId::LeftStickY) = ly;
			At(v, ControlId::RightStickX) = rx;
			At(v, ControlId::RightStickY) = ry;
			static constexpr float kPressAt = 0.5f;
			At(v, ControlId::LeftTriggerPress) = button(At(v, ControlId::LeftTrigger) >= kPressAt);
			At(v, ControlId::RightTriggerPress) = button(At(v, ControlId::RightTrigger) >= kPressAt);
			At(v, ControlId::LeftGripPress) = button(At(v, ControlId::LeftGrip) >= kPressAt);
			At(v, ControlId::RightGripPress) = button(At(v, ControlId::RightGrip) >= kPressAt);
			At(v, ControlId::DpadUp) = button(l.dpad_up || r.dpad_up);
			At(v, ControlId::DpadDown) = button(l.dpad_down || r.dpad_down);
			At(v, ControlId::DpadLeft) = button(l.dpad_left || r.dpad_left);
			At(v, ControlId::DpadRight) = button(l.dpad_right || r.dpad_right);
			At(v, ControlId::LeftBumper) = button(l.bumper);
			At(v, ControlId::RightBumper) = button(r.bumper);
			At(v, ControlId::View) = button(l.view || r.view);

			// Ad-lib flicks from the raw stick (the deadzone rescale would move the thresholds); a stick the
			// camera driver is walking with flicks nothing. Up and down can't both be pressed.
			float fx = 0.0f, fy = 0.0f;
			if (suppressed != VRInputSnapshot::RIGHT)
			{
				fx = axis(r.thumbstick_x);
				fy = axis(r.thumbstick_y);
			}
			const bool vertical = std::fabs(fy) > 2.0f * std::fabs(fx);
			StepFlick(vertical && fy > kFlickOn && !st.flick_down, fy < kFlickOff, dt, st.flick_up, st.flick_up_s);
			StepFlick(vertical && -fy > kFlickOn && !st.flick_up, -fy < kFlickOff, dt, st.flick_down, st.flick_down_s);
			At(v, ControlId::RightStickUp) = button(st.flick_up);
			At(v, ControlId::RightStickDown) = button(st.flick_down);

			// Sprint latch, on the stick as the game gets it (deadzone and suppression applied): walk-and-shoot's
			// stick reads 0, so it neither latches nor holds the latch.
			if (static_cast<u32>(p.sprint_latch) < kControlCount)
			{
				const bool click = l.thumbstick_click;
				const float mag = std::sqrt(lx * lx + ly * ly);
				if (sprint_blocked)
				{
					st.sprint = false;
				}
				else if (!st.sprint)
				{
					// A fresh click only: a click held through a block, or from before the push, doesn't latch.
					if (click && !st.click_prev && mag > kSprintOn)
					{
						st.sprint = true;
						st.centred_s = 0.0f;
					}
				}
				else if (mag > kSprintHold)
				{
					st.centred_s = 0.0f;
				}
				else
				{
					st.centred_s += dt;
					if (st.centred_s >= kSprintCentredS)
						st.sprint = false;
				}
				st.click_prev = click;
				if (st.sprint)
				{
					// ORed into the latch control: one binding (ButtonB: Cross) carries both, so a tap of B can't
					// release the run the way two bindings on one PS2 button would (the latest change wins there).
					At(v, ControlId::LeftStickClick) = 0.0f;
					At(v, p.sprint_latch) = 1.0f;
				}
			}
			return v;
		}
	}

	void SetMoveStickSuppressed(int hand)
	{
		s_suppressed_move_hand.store(hand, std::memory_order_release);
	}

	void SetSprintBlocked(bool blocked)
	{
		s_sprint_blocked.store(blocked, std::memory_order_release);
	}

	ControlValues ComposeGamepad(const VRInputSnapshot& snapshot, const GamepadParams& p, GamepadState& st, float dt)
	{
		return ComposeGamepadWith(snapshot, p, st, dt, s_suppressed_move_hand.load(std::memory_order_acquire),
			s_sprint_blocked.load(std::memory_order_acquire));
	}

	bool SelfTestGamepad(const char** failed)
	{
		const char* first = nullptr;
		const auto check = [&first](bool ok, const char* name) {
			if (!ok && !first)
				first = name;
		};
		const auto on = [](const ControlValues& v, ControlId id) { return v[static_cast<u32>(id)] > 0.5f; };
		VRInputSnapshot s;
		s.generation = 1;
		s.actions_active = true;
		VRHandState& l = s.hands[VRInputSnapshot::LEFT];
		VRHandState& r = s.hands[VRInputSnapshot::RIGHT];
		// `polls` polls at 60 Hz; the last one's values.
		const auto run = [&s](const GamepadParams& p, GamepadState& st, int polls, bool blocked, int suppressed) {
			ControlValues v{};
			for (int i = 0; i < polls; ++i)
				v = ComposeGamepadWith(s, p, st, 1.0f / 60.0f, suppressed, blocked);
			return v;
		};
		constexpr int kNone = -1;

		// Without sprintLatch the click and B pass straight through, as before the latch existed.
		{
			const GamepadParams off;
			GamepadState st;
			l.thumbstick_y = 1.0f;
			l.thumbstick_click = true;
			ControlValues v = run(off, st, 3, false, kNone);
			check(on(v, ControlId::LeftStickClick) && !on(v, ControlId::ButtonB), "no sprintLatch: the click passes, B untouched");
			check(std::fabs(v[static_cast<u32>(ControlId::LeftStickY)] - 1.0f) < 1.0e-4f, "no sprintLatch: the stick passes");
			l.thumbstick_click = false;
			v = run(off, st, 3, false, kNone);
			check(!on(v, ControlId::LeftStickClick) && !on(v, ControlId::ButtonB) && !st.sprint, "no sprintLatch: nothing held after the click");
			r.b = true;
			v = run(off, st, 1, false, kNone);
			check(on(v, ControlId::ButtonB), "no sprintLatch: B passes");
			r.b = false;
			v = run(off, st, 1, false, kNone);
			check(!on(v, ControlId::ButtonB), "no sprintLatch: B lets go");
		}

		// sprintLatch: ButtonB.
		{
			GamepadParams p;
			p.sprint_latch = ControlId::ButtonB;
			GamepadState st;
			l = VRHandState{};
			r = VRHandState{};
			l.thumbstick_y = 1.0f;
			ControlValues v = run(p, st, 2, false, kNone);
			check(!on(v, ControlId::ButtonB), "latch: pushing alone does not run");
			l.thumbstick_click = true;
			v = run(p, st, 1, false, kNone);
			check(on(v, ControlId::ButtonB) && !on(v, ControlId::LeftStickClick), "latch: a click while pushed holds B, the click is not passed");
			l.thumbstick_click = false;
			v = run(p, st, 60, false, kNone);
			check(on(v, ControlId::ButtonB), "latch: stays on while pushed");
			r.b = true;
			run(p, st, 3, false, kNone);
			r.b = false;
			v = run(p, st, 3, false, kNone);
			check(on(v, ControlId::ButtonB), "latch: a tap of B does not release it");
			l.thumbstick_y = 0.4f; // 0.33 after the deadzone: past the 0.25 hold
			v = run(p, st, 30, false, kNone);
			check(on(v, ControlId::ButtonB), "latch: holds with the stick past a quarter");
			l.thumbstick_y = 0.0f;
			v = run(p, st, 6, false, kNone);
			check(on(v, ControlId::ButtonB), "latch: survives 0.1 s centred");
			v = run(p, st, 6, false, kNone);
			check(!on(v, ControlId::ButtonB) && !st.sprint, "latch: drops after 0.15 s centred");

			l.thumbstick_click = true; // stick centred
			v = run(p, st, 2, false, kNone);
			check(!on(v, ControlId::ButtonB) && on(v, ControlId::LeftStickClick), "latch: a click with the stick centred does not latch");
			l.thumbstick_y = 1.0f; // still clicked, now pushed: no fresh click
			v = run(p, st, 2, false, kNone);
			check(!on(v, ControlId::ButtonB), "latch: a click held from before the push does not latch");
			l.thumbstick_click = false;
			run(p, st, 1, false, kNone);

			l.thumbstick_click = true;
			run(p, st, 1, false, kNone);
			l.thumbstick_click = false;
			v = run(p, st, 1, true, kNone);
			check(!on(v, ControlId::ButtonB) && !st.sprint, "latch: drops at once when blocked");
			v = run(p, st, 3, false, kNone);
			check(!on(v, ControlId::ButtonB), "latch: unblocking does not latch again");
			l.thumbstick_click = true;
			v = run(p, st, 1, true, kNone);
			check(!on(v, ControlId::ButtonB), "latch: cannot latch while blocked");
			l.thumbstick_click = false;
			run(p, st, 1, false, kNone);

			GamepadState walking;
			run(p, walking, 1, false, VRInputSnapshot::LEFT);
			l.thumbstick_click = true;
			v = run(p, walking, 1, false, VRInputSnapshot::LEFT);
			check(!on(v, ControlId::ButtonB), "latch: a stick the camera driver walks with does not latch");
			l.thumbstick_click = false;
		}

		// Ad-lib flicks.
		{
			const GamepadParams off;
			GamepadState st;
			l = VRHandState{};
			r = VRHandState{};
			r.thumbstick_y = 1.0f;
			ControlValues v = run(off, st, 1, false, kNone);
			check(on(v, ControlId::RightStickUp) && !on(v, ControlId::RightStickDown), "flick: straight up presses RightStickUp");
			r.thumbstick_y = 0.0f;
			v = run(off, st, 3, false, kNone);
			check(on(v, ControlId::RightStickUp), "flick: a quick flick is held for the minimum press");
			v = run(off, st, 9, false, kNone);
			check(!on(v, ControlId::RightStickUp), "flick: lets go after 0.12 s");
			r.thumbstick_x = 0.3f;
			r.thumbstick_y = -0.9f;
			v = run(off, st, 1, false, kNone);
			check(on(v, ControlId::RightStickDown) && !on(v, ControlId::RightStickUp), "flick: down within 2:1 presses RightStickDown");
			r = VRHandState{};
			v = run(off, st, 12, false, kNone);
			check(!on(v, ControlId::RightStickDown), "flick: down lets go");
			r.thumbstick_x = 0.7f;
			r.thumbstick_y = 0.7f;
			v = run(off, st, 3, false, kNone);
			check(!on(v, ControlId::RightStickUp) && !on(v, ControlId::RightStickDown), "flick: a diagonal turns, no ad-lib");
			r.thumbstick_x = 0.5f;
			r.thumbstick_y = 0.9f;
			v = run(off, st, 3, false, kNone);
			check(!on(v, ControlId::RightStickUp), "flick: needs twice as far up as sideways");
			r.thumbstick_x = 0.0f;
			r.thumbstick_y = 0.7f;
			v = run(off, st, 3, false, kNone);
			check(!on(v, ControlId::RightStickUp), "flick: needs 0.75");
			r = VRHandState{};
			run(off, st, 1, false, kNone);
			r.thumbstick_y = 1.0f;
			v = run(off, st, 1, false, VRInputSnapshot::RIGHT);
			check(!on(v, ControlId::RightStickUp), "flick: not while the right stick is suppressed");
			v = run(off, st, 1, false, kNone);
			check(on(v, ControlId::RightStickUp), "flick: presses once the stick is no longer suppressed");
		}

		if (failed)
			*failed = first;
		return first == nullptr;
	}

	bool HeadAnchor(const VRInputSnapshot& snapshot, ZoneState& st, Anchor* out)
	{
		const VRPose& head = snapshot.head_pose;
		if (snapshot.generation == 0 || !head.valid)
			return false;
		const float qx = head.orientation_xyzw[0], qy = head.orientation_xyzw[1];
		const float qz = head.orientation_xyzw[2], qw = head.orientation_xyzw[3];
		// The head's forward (-Z) flattened onto the floor, as XRCompositor anchors the screen.
		const float fx = -2.0f * (qw * qy + qz * qx);
		const float fz = -(1.0f - 2.0f * (qx * qx + qy * qy));
		if ((fx * fx + fz * fz) > 1.0e-4f)
		{
			st.head_yaw = std::atan2(-fx, -fz);
			st.have_yaw = true;
		}
		if (!st.have_yaw)
			return false;
		out->position = {head.position_xyz[0], head.position_xyz[1], head.position_xyz[2]};
		out->yaw = st.head_yaw;
		return true;
	}

	ZoneEvents StepZone(const Anchor& head, const ZoneParams& p, const Hands& hands, ZoneState& st)
	{
		ZoneEvents ev;
		const Placement at{p.offset.side, p.offset.height, p.offset.forward, 0.0f};
		const Vec3 centre = PlaceControl(head, at).pivot;
		for (int h = 0; h < 2; ++h)
		{
			const bool allowed = p.hand == ZoneHand::Either || (p.hand == ZoneHand::Left && h == VRInputSnapshot::LEFT) ||
								 (p.hand == ZoneHand::Right && h == VRInputSnapshot::RIGHT);
			const HandInput& hand = hands.hand[h];
			if (!allowed || !hand.valid)
			{
				st.inside[h] = false;
				st.squeezed[h] = false;
				st.pressing[h] = false;
				continue;
			}

			const float dist = Length(hand.position - centre);
			const bool inside = dist <= (st.inside[h] ? p.radius + p.exit_margin : p.radius);
			const bool squeezed = st.squeezed[h] ? hand.squeeze > p.grip_off : hand.squeeze >= p.grip_on;

			bool pressing;
			if (p.require_grip)
				pressing = squeezed && (st.pressing[h] || (inside && !st.squeezed[h]));
			else
				pressing = inside;

			ev.entered[h] = inside && !st.inside[h];
			ev.pressed[h] = pressing && !st.pressing[h];
			st.inside[h] = inside;
			st.squeezed[h] = squeezed;
			st.pressing[h] = pressing;
		}
		return ev;
	}

	ControlValues ComposeZone(const ZoneState& st)
	{
		ControlValues v{};
		At(v, ControlId::Pressed) = (st.pressing[0] || st.pressing[1]) ? 1.0f : 0.0f;
		return v;
	}
}
