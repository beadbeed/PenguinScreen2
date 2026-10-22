// SPDX-FileCopyrightText: 2026 PenguinScreen2 contributors
// SPDX-License-Identifier: GPL-3.0

#include "VR/VrBody.h"

#include "VR/CameraDriver.h"
#include "VR/VRInputState.h"
#include "VR/VRProfileDB.h"
#include "VR/VrPresence.h"
#include "Config.h"
#include "Memory.h"
#include "VMManager.h"
#include "common/Console.h"
#include "fmt/format.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace VR::VrBody
{
	namespace
	{
		constexpr double kPi = 3.14159265358979323846;
		constexpr double kDeg = kPi / 180.0;

		struct V
		{
			double x = 0.0, y = 0.0, z = 0.0;
		};
		V Ld(const double a[3]) { return {a[0], a[1], a[2]}; }
		void St(const V& v, double a[3])
		{
			a[0] = v.x;
			a[1] = v.y;
			a[2] = v.z;
		}
		V operator+(const V& a, const V& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
		V operator-(const V& a, const V& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
		V operator*(const V& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
		double Dot(const V& a, const V& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		V Cross(const V& a, const V& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
		double Len(const V& a) { return std::sqrt(Dot(a, a)); }
		V Norm(const V& a, const V& fallback = {0.0, 1.0, 0.0})
		{
			const double l = Len(a);
			return (l > 1e-12 && std::isfinite(l)) ? a * (1.0 / l) : fallback;
		}
		bool Finite(const V& a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
		// Any unit vector perpendicular to a (a unit).
		V AnyPerp(const V& a)
		{
			const V t = std::fabs(a.x) < 0.9 ? V{1.0, 0.0, 0.0} : V{0.0, 0.0, 1.0};
			return Norm(Cross(a, t));
		}
		V Row(const double r[3][3], int i) { return {r[i][0], r[i][1], r[i][2]}; }
		void SetRow(double r[3][3], int i, const V& v)
		{
			r[i][0] = v.x;
			r[i][1] = v.y;
			r[i][2] = v.z;
		}
		// p (row) * r
		V MulRow(const V& p, const double r[3][3])
		{
			return {p.x * r[0][0] + p.y * r[1][0] + p.z * r[2][0], p.x * r[0][1] + p.y * r[1][1] + p.z * r[2][1],
				p.x * r[0][2] + p.y * r[1][2] + p.z * r[2][2]};
		}
		// p (row) * r^T: a point's coordinates along r's axes
		V MulRowT(const V& p, const double r[3][3]) { return {Dot(p, Row(r, 0)), Dot(p, Row(r, 1)), Dot(p, Row(r, 2))}; }
		void MulRR(const double a[3][3], const double b[3][3], double out[3][3])
		{
			double t[3][3];
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
			std::memcpy(out, t, sizeof(t));
		}
		void Transpose(const double a[3][3], double out[3][3])
		{
			double t[3][3];
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					t[i][j] = a[j][i];
			std::memcpy(out, t, sizeof(t));
		}

		// Quaternions for rotations written as row frames: the frame r turns the row (1,0,0) into r's first axis, so its
		// column form is r^T. q = (w, x, y, z), the column-vector rotation.
		struct Q
		{
			double w = 1.0, x = 0.0, y = 0.0, z = 0.0;
		};
		Q QMul(const Q& a, const Q& b)
		{
			return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
				a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
		}
		Q QConj(const Q& a) { return {a.w, -a.x, -a.y, -a.z}; }
		Q QNorm(const Q& a)
		{
			const double l = std::sqrt(a.w * a.w + a.x * a.x + a.y * a.y + a.z * a.z);
			return (l > 1e-12) ? Q{a.w / l, a.x / l, a.y / l, a.z / l} : Q{};
		}
		Q QAxis(const V& axis, double ang)
		{
			const V a = Norm(axis, {1.0, 0.0, 0.0});
			const double s = std::sin(ang * 0.5);
			return {std::cos(ang * 0.5), a.x * s, a.y * s, a.z * s};
		}
		// From a row frame r (column matrix m = r^T).
		Q QFromRows(const double r[3][3])
		{
			double m[3][3];
			Transpose(r, m);
			const double tr = m[0][0] + m[1][1] + m[2][2];
			Q q;
			if (tr > 0.0)
			{
				const double s = std::sqrt(tr + 1.0) * 2.0;
				q = {0.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s};
			}
			else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
			{
				const double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0;
				q = {(m[2][1] - m[1][2]) / s, 0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s};
			}
			else if (m[1][1] > m[2][2])
			{
				const double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0;
				q = {(m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s};
			}
			else
			{
				const double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0;
				q = {(m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s};
			}
			return QNorm(q);
		}
		void RowsFromQ(const Q& qi, double r[3][3])
		{
			const Q q = QNorm(qi);
			const double m[3][3] = {
				{1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y - q.z * q.w), 2 * (q.x * q.z + q.y * q.w)},
				{2 * (q.x * q.y + q.z * q.w), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z - q.x * q.w)},
				{2 * (q.x * q.z - q.y * q.w), 2 * (q.y * q.z + q.x * q.w), 1 - 2 * (q.x * q.x + q.y * q.y)}};
			Transpose(m, r);
		}
		Q QSlerp(const Q& a, Q b, double t)
		{
			double d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
			if (d < 0.0)
			{
				b = {-b.w, -b.x, -b.y, -b.z};
				d = -d;
			}
			if (d > 0.9995)
				return QNorm({a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t});
			const double th = std::acos(std::clamp(d, -1.0, 1.0));
			const double sa = std::sin((1.0 - t) * th) / std::sin(th), sb = std::sin(t * th) / std::sin(th);
			return QNorm({a.w * sa + b.w * sb, a.x * sa + b.x * sb, a.y * sa + b.y * sb, a.z * sa + b.z * sb});
		}
		double QAngle(const Q& q) { return 2.0 * std::acos(std::clamp(std::fabs(q.w), 0.0, 1.0)); }

		// The rotation (row frame) taking the bone-frame pair (axis e1, a second direction e3 perpendicularised) onto the
		// target pair (g1, g3): rows e1, e2 = e3 x e1, e3 map to g1, g2 = g3 x g1, g3.
		void FrameFromPairs(const V& e1i, const V& e3i, const V& g1i, const V& g3i, double r[3][3])
		{
			const V e1 = Norm(e1i, {1.0, 0.0, 0.0});
			V e3 = e3i - e1 * Dot(e3i, e1);
			e3 = Norm(e3, AnyPerp(e1));
			const V e2 = Cross(e3, e1);
			const V g1 = Norm(g1i, {1.0, 0.0, 0.0});
			V g3 = g3i - g1 * Dot(g3i, g1);
			g3 = Norm(g3, AnyPerp(g1));
			const V g2 = Cross(g3, g1);
			double E[3][3], G[3][3], Et[3][3];
			SetRow(E, 0, e1);
			SetRow(E, 1, e2);
			SetRow(E, 2, e3);
			SetRow(G, 0, g1);
			SetRow(G, 1, g2);
			SetRow(G, 2, g3);
			Transpose(E, Et);
			MulRR(Et, G, r);
		}

		double Clamp(double v, double lo, double hi) { return std::min(std::max(v, lo), hi); }
	} // namespace

	Frame Compose(const Frame& c, const Frame& p)
	{
		Frame out;
		MulRR(c.r, p.r, out.r);
		St(MulRow(Ld(c.t), p.r) + Ld(p.t), out.t);
		return out;
	}

	Frame Inverse(const Frame& f)
	{
		Frame out;
		Transpose(f.r, out.r);
		St(MulRow(Ld(f.t), out.r) * -1.0, out.t);
		return out;
	}

	void ToFrame(const Frame& f, const double world[3], double local[3]) { St(MulRowT(Ld(world) - Ld(f.t), f.r), local); }
	void DirToFrame(const Frame& f, const double world[3], double local[3]) { St(MulRowT(Ld(world), f.r), local); }

	bool FromGame(const float m[16], Frame* out)
	{
		for (int i = 0; i < 16; i++)
			if (!std::isfinite(m[i]))
				return false;
		V r0{m[0], m[1], m[2]}, r1{m[4], m[5], m[6]}, r2{m[8], m[9], m[10]};
		const V t{m[12], m[13], m[14]};
		if (std::fabs(Len(r0) - 1.0) > 0.02 || std::fabs(Len(r1) - 1.0) > 0.02 || std::fabs(Len(r2) - 1.0) > 0.02 ||
			std::fabs(Dot(r0, r1)) > 0.02 || std::fabs(Dot(r1, r2)) > 0.02 || std::fabs(Dot(r0, r2)) > 0.02 ||
			Dot(Cross(r1, r2), r0) < 0.9 || std::fabs(t.x) > 1e6 || std::fabs(t.y) > 1e6 || std::fabs(t.z) > 1e6)
			return false;
		// Re-orthonormalised, keeping the second axis (the bones' up) and the third (forward).
		r1 = Norm(r1);
		r2 = Norm(r2 - r1 * Dot(r2, r1));
		r0 = Cross(r1, r2);
		SetRow(out->r, 0, r0);
		SetRow(out->r, 1, r1);
		SetRow(out->r, 2, r2);
		St(t, out->t);
		return true;
	}

	void ToGame(const Frame& f, float m[16])
	{
		for (int i = 0; i < 3; i++)
		{
			for (int j = 0; j < 3; j++)
				m[4 * i + j] = static_cast<float>(f.r[i][j]);
			m[4 * i + 3] = 0.0f;
		}
		for (int j = 0; j < 3; j++)
			m[12 + j] = static_cast<float>(f.t[j]);
		m[15] = 1.0f;
	}

	BoneMap DefaultMap()
	{
		BoneMap m;
		m.chest = 11;
		m.neck = 20;
		m.head = 21;
		m.arm[0] = {12, 13, 14, 15};
		m.arm[1] = {16, 17, 18, 19};
		return m;
	}

	bool CheckMap(const BoneMap& m, const int* parents, int n, std::string* why)
	{
		const auto fail = [why](const char* w) {
			if (why)
				*why = w;
			return false;
		};
		if (n <= 0 || n > kMaxBones)
			return fail("bone count out of range");
		const auto in = [n](int b) { return b >= 0 && b < n; };
		if (!in(m.chest) || !in(m.neck) || !in(m.head))
			return fail("chest, neck or head index past the model's bones");
		for (const ArmMap& a : m.arm)
			if (!in(a.clavicle) || !in(a.upper) || !in(a.fore) || !in(a.wrist))
				return fail("an arm index past the model's bones");
		if (parents[m.neck] != m.chest)
			return fail("the neck's parent is not the chest");
		if (parents[m.head] != m.neck)
			return fail("the head's parent is not the neck");
		for (const ArmMap& a : m.arm)
		{
			if (parents[a.clavicle] != m.chest)
				return fail("a clavicle's parent is not the chest");
			if (parents[a.upper] != a.clavicle || parents[a.fore] != a.upper || parents[a.wrist] != a.fore)
				return fail("an arm's chain is not clavicle -> upper -> fore -> wrist");
		}
		if (m.arm[0].clavicle == m.arm[1].clavicle)
			return fail("both arms are one");
		// The chest must not sit under anything this rewrites (the cave multiplies by its matrix).
		for (int b = m.chest, guard = 0; b >= 0 && guard < kMaxBones; b = parents[b], guard++)
			if (b == m.neck || b == m.arm[0].upper || b == m.arm[1].upper)
				return fail("the chest is under a rewritten bone");
		return true;
	}

	int Subtree(const int* parents, int n, int b, int* out, int max)
	{
		if (b < 0 || b >= n || max <= 0)
			return 0;
		int count = 0;
		out[count++] = b;
		// Breadth first over the list itself: a bone is added once its parent is in, so parents come first.
		for (int i = 0; i < count; i++)
			for (int c = 0; c < n && count < max; c++)
				if (parents[c] == out[i] && c != out[i])
				{
					bool seen = false;
					for (int k = 0; k < count; k++)
						seen |= (out[k] == c);
					if (!seen)
						out[count++] = c;
				}
		return count;
	}

	bool SolveTwoBone(const double s_[3], double l1, double l2, const double target_[3], const double pole_[3],
		double max_bend_deg, IkResult* out)
	{
		if (!(l1 > 1e-6) || !(l2 > 1e-6) || !std::isfinite(l1) || !std::isfinite(l2))
			return false;
		const V s = Ld(s_), target = Ld(target_), pole = Ld(pole_);
		if (!Finite(s) || !Finite(target) || !Finite(pole))
			return false;
		V d = target - s;
		const double dist = Len(d);
		const V pole_dir = Norm(pole, {0.0, -1.0, 0.0});
		// A target at the shoulder: reach along the pole's perpendicular, the elbow toward the pole.
		const V dhat = (dist > 1e-6) ? d * (1.0 / dist) : AnyPerp(pole_dir);
		const double inner = Clamp(180.0 - max_bend_deg, 1.0, 179.0) * kDeg;
		const double dmin = std::sqrt(std::max(0.0, l1 * l1 + l2 * l2 - 2.0 * l1 * l2 * std::cos(inner)));
		const double dmax = 0.999 * (l1 + l2);
		const double r = Clamp(dist, dmin, dmax);
		const double a = (l1 * l1 - l2 * l2 + r * r) / (2.0 * r);
		const double h = std::sqrt(std::max(0.0, l1 * l1 - a * a));
		V perp = pole_dir - dhat * Dot(pole_dir, dhat);
		perp = (Len(perp) > 1e-4) ? Norm(perp) : AnyPerp(dhat);
		St(s + dhat * a + perp * h, out->elbow);
		St(s + dhat * r, out->wrist);
		out->reach = r;
		out->clamped = std::fabs(r - dist) > 1e-9;
		return true;
	}

	void HeadAngles(const double fwd_[3], const double up_[3], double* yaw, double* pitch, double* roll)
	{
		const V f = Norm(Ld(fwd_), {0.0, 0.0, 1.0});
		*yaw = std::atan2(f.x, f.z);
		*pitch = std::atan2(-f.y, std::sqrt(f.x * f.x + f.z * f.z));
		const V up0{std::sin(*yaw) * std::sin(*pitch), std::cos(*pitch), std::cos(*yaw) * std::sin(*pitch)};
		V u = Ld(up_);
		u = Norm(u - f * Dot(u, f), up0);
		*roll = std::atan2(Dot(Cross(up0, u), f), Dot(up0, u));
	}

	void HeadFrame(double yaw, double pitch, double roll, double r[3][3])
	{
		const V f{std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
		const V up0{std::sin(yaw) * std::sin(pitch), std::cos(pitch), std::cos(yaw) * std::sin(pitch)};
		const V up = up0 * std::cos(roll) + Cross(f, up0) * std::sin(roll);
		SetRow(r, 0, Cross(up, f));
		SetRow(r, 1, up);
		SetRow(r, 2, f);
	}

	void BlendRot(const double a[3][3], const double b[3][3], double w, double out[3][3])
	{
		if (w <= 0.0)
		{
			std::memcpy(out, a, sizeof(double) * 9);
			return;
		}
		if (w >= 1.0)
		{
			std::memcpy(out, b, sizeof(double) * 9);
			return;
		}
		RowsFromQ(QSlerp(QFromRows(a), QFromRows(b), w), out);
	}

	double RotAngleDeg(const double a[3][3], const double b[3][3])
	{
		return QAngle(QMul(QConj(QFromRows(a)), QFromRows(b))) / kDeg;
	}

	void WantWeights(const BoneMap& m, const Frame* locals, const Targets& tg, const Tuning& tu, Weights* want)
	{
		want->head = tg.head ? 1.0 : 0.0;
		for (int a = 0; a < 2; a++)
		{
			want->arm[a] = 0.0;
			if (!tg.hand[a])
				continue;
			const ArmMap& am = m.arm[a];
			// The shoulder from the animated clavicle (relative to the chest).
			const Frame sh = Compose(locals[am.upper], locals[am.clavicle]);
			const double l = Len(Ld(locals[am.fore].t)) + Len(Ld(locals[am.wrist].t));
			const V tw = Ld(tg.hand_pos[a]) - Ld(tg.hand_fwd[a]) * tu.wrist_back_cm;
			const bool in_reach = Len(tw - Ld(sh.t)) <= tu.reach_slack * l;
			const bool in_front = tw.z >= -tu.behind_cm;
			want->arm[a] = (in_reach && in_front && Finite(tw)) ? 1.0 : 0.0;
		}
	}

	void EaseWeights(Weights* cur, const Weights& want, double dt, const Tuning& tu)
	{
		const double step = Clamp(dt, 0.0, 0.25) * tu.blend_per_s;
		const auto ease = [step](double& c, double w) { c = (w > c) ? std::min(w, c + step) : std::max(w, c - step); };
		ease(cur->head, want.head);
		ease(cur->arm[0], want.arm[0]);
		ease(cur->arm[1], want.arm[1]);
	}

	bool SolveBody(const BoneMap& m, const Frame* locals, const int* parents, int n, const Targets& tg, const Weights& w,
		const Tuning& tu, Pose* out)
	{
		*out = Pose{};
		if (!CheckMap(m, parents, n, nullptr))
			return false;
		// The animation's pose relative to the chest, for the chest's whole subtree.
		Frame anim[kMaxBones];
		Frame now[kMaxBones];
		int order[kMaxBones];
		const int count = Subtree(parents, n, m.chest, order, kMaxBones);
		anim[m.chest] = Frame{};
		for (int i = 1; i < count; i++)
			anim[order[i]] = Compose(locals[order[i]], anim[parents[order[i]]]);
		for (int i = 0; i < count; i++)
			now[order[i]] = anim[order[i]];

		// The head: its turn off the chest from the VR head, clamped, shared with the neck.
		if (w.head > 0.0 && tg.head)
		{
			double yaw, pitch, roll;
			HeadAngles(tg.head_fwd, tg.head_up, &yaw, &pitch, &roll);
			const double cy = Clamp(yaw, -tu.head_yaw_deg * kDeg, tu.head_yaw_deg * kDeg);
			const double cp = Clamp(pitch, -tu.head_pitch_up_deg * kDeg, tu.head_pitch_down_deg * kDeg);
			const double cr = Clamp(roll, -tu.head_roll_deg * kDeg, tu.head_roll_deg * kDeg);
			out->head_clamped = cy != yaw || cp != pitch || cr != roll;
			double rn[3][3], rh[3][3];
			HeadFrame(cy * tu.neck_share, cp * tu.neck_share, cr * tu.neck_share, rn);
			HeadFrame(cy, cp, cr, rh);
			BlendRot(anim[m.neck].r, rn, w.head, now[m.neck].r);
			// The head: its own turn, then placed on the (new) neck.
			double head_rel[3][3];
			BlendRot(anim[m.head].r, rh, w.head, head_rel);
			std::memcpy(now[m.head].r, head_rel, sizeof(head_rel));
			St(MulRow(Ld(locals[m.head].t), now[m.neck].r) + Ld(now[m.neck].t), now[m.head].t);
			// Every bone under the neck rebuilt from its local; the neck and head keep the turns set here.
			int sub[kMaxBones];
			const int k = Subtree(parents, n, m.neck, sub, kMaxBones);
			for (int i = 0; i < k; i++)
			{
				const int b = sub[i];
				if (b != m.neck && b != m.head)
					now[b] = Compose(locals[b], now[parents[b]]);
				if (out->count < kMaxOut)
				{
					out->bone[out->count] = b;
					out->rel[out->count] = now[b];
					out->count++;
				}
			}
		}

		for (int a = 0; a < 2; a++)
		{
			if (!(w.arm[a] > 0.0) || !tg.hand[a])
				continue;
			const ArmMap& am = m.arm[a];
			const double sigma = (locals[am.upper].t[0] >= 0.0) ? 1.0 : -1.0; // the side the arm sits on, in the model's X
			const V s = Ld(anim[am.upper].t);
			const V c1 = Ld(locals[am.fore].t), c2 = Ld(locals[am.wrist].t);
			const double l1 = Len(c1), l2 = Len(c2);
			const V hf = Norm(Ld(tg.hand_fwd[a]), {0.0, 0.0, 1.0});
			V tw = Ld(tg.hand_pos[a]) - hf * tu.wrist_back_cm;
			// Not into the torso (a box round the chest, chest frame): pushed out in front.
			if (std::fabs(tw.x) < 14.0 && tw.y > -45.0 && tw.y < 24.0 && tw.z > -12.0 && tw.z < 14.0)
				tw.z = 14.0;
			const V pole = V{0.0, -1.0, 0.0} + V{sigma, 0.0, 0.0} * 0.6 + V{0.0, 0.0, -1.0} * 0.4;
			double sv[3], tv[3], pv[3];
			St(s, sv);
			St(tw, tv);
			St(pole, pv);
			IkResult ik;
			if (!SolveTwoBone(sv, l1, l2, tv, pv, tu.max_elbow_bend_deg, &ik))
				continue;
			const V e = Ld(ik.elbow), wr = Ld(ik.wrist);
			out->wrist_err[a] = Len(wr - Ld(tg.hand_pos[a]) + hf * tu.wrist_back_cm);
			out->reach_clamped[a] = ik.clamped;
			const V u = Norm(e - s), f = Norm(wr - e, u);
			// The crease (each arm bone's +Z, perpendicularised) toward where the forearm bends; a straight arm: away
			// from the elbow's pole.
			V crease = f - u * Dot(f, u);
			if (Len(crease) < 1e-3)
			{
				crease = pole - u * Dot(pole, u);
				crease = Norm(crease, AnyPerp(u)) * -1.0;
			}
			double r_up[3][3], r_fore[3][3], r_hand[3][3];
			FrameFromPairs(c1, {0.0, 0.0, 1.0}, u, crease, r_up);
			// The forearm on the same hinge (the upper arm's second axis), its child along f.
			{
				const V hinge = Row(r_up, 1);
				const V e1 = Norm(c2, {sigma, 0.0, 0.0});
				V e3 = V{0.0, 0.0, 1.0} - e1 * e1.z;
				e3 = Norm(e3, AnyPerp(e1));
				const V e2 = Cross(e3, e1);
				// Rows e1, e2, e3 to f, hinge, f x hinge.
				const V g1 = f, g2 = Norm(hinge - f * Dot(hinge, f), AnyPerp(f)), g3 = Cross(g1, g2);
				double E[3][3], G[3][3], Et[3][3];
				SetRow(E, 0, e1);
				SetRow(E, 1, e2);
				SetRow(E, 2, e3);
				SetRow(G, 0, g1);
				SetRow(G, 1, g2);
				SetRow(G, 2, g3);
				Transpose(E, Et);
				MulRR(Et, G, r_fore);
			}
			// The hand from the controller: the fingers (the bone's +-X, the arm's side) along its forward, the thumb (+Z)
			// along its up.
			FrameFromPairs({sigma, 0.0, 0.0}, {0.0, 0.0, 1.0}, hf, Ld(tg.hand_up[a]), r_hand);
			// The wrist: the hand relative to the forearm (forearm local), split into a twist about the forearm's own axis
			// and a swing. The forearm takes a share of the twist; then the rest and the swing are clamped.
			{
				const V axis = Norm(c2, {sigma, 0.0, 0.0});
				const auto split = [&](const double fore[3][3], Q* swing, double* twist_ang) {
					double rel[3][3], fore_t[3][3];
					Transpose(fore, fore_t);
					MulRR(r_hand, fore_t, rel); // hand = rel * fore (rows)
					const Q q = QFromRows(rel);
					const double proj = q.x * axis.x + q.y * axis.y + q.z * axis.z;
					const Q twist = (std::fabs(q.w) + std::fabs(proj) > 1e-12) ? QNorm({q.w, axis.x * proj, axis.y * proj, axis.z * proj}) : Q{};
					*swing = QMul(q, QConj(twist));
					const double t = 2.0 * std::atan2(proj, q.w);
					*twist_ang = std::atan2(std::sin(t), std::cos(t));
				};
				Q swing;
				double tw_ang = 0.0;
				split(r_fore, &swing, &tw_ang);
				// The forearm turned about its own axis (in its local frame): rows = turn(local) * rows.
				double rf_local[3][3], r_fore2[3][3];
				RowsFromQ(QAxis(axis, tw_ang * tu.forearm_twist_share), rf_local);
				MulRR(rf_local, r_fore, r_fore2);
				std::memcpy(r_fore, r_fore2, sizeof(r_fore2));
				split(r_fore, &swing, &tw_ang);
				const double rest = Clamp(tw_ang, -tu.wrist_twist_deg * kDeg, tu.wrist_twist_deg * kDeg);
				V sw_axis{swing.x, swing.y, swing.z};
				if (swing.w < 0.0)
					sw_axis = sw_axis * -1.0;
				const double sw_ang = std::min(QAngle(swing), tu.wrist_swing_deg * kDeg);
				const Q rel2 = (Len(sw_axis) > 1e-9) ? QMul(QAxis(sw_axis, sw_ang), QAxis(axis, rest)) : QAxis(axis, rest);
				double rel2r[3][3];
				RowsFromQ(rel2, rel2r);
				MulRR(rel2r, r_fore, r_hand);
			}
			// Blended with the animation, then placed down the chain from the (animated) shoulder.
			const double wa = std::min(w.arm[a], 1.0);
			BlendRot(anim[am.upper].r, r_up, wa, now[am.upper].r);
			BlendRot(anim[am.fore].r, r_fore, wa, now[am.fore].r);
			BlendRot(anim[am.wrist].r, r_hand, wa, now[am.wrist].r);
			St(MulRow(c1, now[am.upper].r) + Ld(now[am.upper].t), now[am.fore].t);
			St(MulRow(c2, now[am.fore].r) + Ld(now[am.fore].t), now[am.wrist].t);
			for (int j = 0; j < 3; j++)
				out->wrist[a][j] = now[am.wrist].t[j];
			// Emit the upper arm's subtree: the three set above, every bone under the wrist rebuilt from its local.
			int sub[kMaxBones];
			const int k = Subtree(parents, n, am.upper, sub, kMaxBones);
			for (int i = 0; i < k; i++)
			{
				const int b = sub[i];
				if (b != am.upper && b != am.fore && b != am.wrist)
					now[b] = Compose(locals[b], now[parents[b]]);
				if (out->count < kMaxOut)
				{
					out->bone[out->count] = b;
					out->rel[out->count] = now[b];
					out->count++;
				}
			}
		}
		// The locals of the bones turned (the head's two, each driven arm's four): each frame against its parent's.
		const auto add_local = [&](int b) {
			if (out->lcount >= kMaxLocals)
				return;
			const int p = parents[b];
			out->lbone[out->lcount] = b;
			out->lrel[out->lcount] = (p == m.chest) ? now[b] : Compose(now[b], Inverse(now[p]));
			out->lcount++;
		};
		if (w.head > 0.0 && tg.head)
		{
			add_local(m.neck);
			add_local(m.head);
		}
		for (int a = 0; a < 2; a++)
			if (w.arm[a] > 0.0 && tg.hand[a])
			{
				add_local(m.arm[a].clavicle);
				add_local(m.arm[a].upper);
				add_local(m.arm[a].fore);
				add_local(m.arm[a].wrist);
			}
		return true;
	}
	bool FromGameScaled(const float m[16], Frame* out, double* scale)
	{
		for (int i = 0; i < 16; i++)
			if (!std::isfinite(m[i]))
				return false;
		const double l0 = std::sqrt(double(m[0]) * m[0] + double(m[1]) * m[1] + double(m[2]) * m[2]);
		const double l1 = std::sqrt(double(m[4]) * m[4] + double(m[5]) * m[5] + double(m[6]) * m[6]);
		const double l2 = std::sqrt(double(m[8]) * m[8] + double(m[9]) * m[9] + double(m[10]) * m[10]);
		const double s = (l0 + l1 + l2) / 3.0;
		if (!(s > 0.2 && s < 5.0) || std::fabs(l0 - s) > 0.02 * s || std::fabs(l1 - s) > 0.02 * s || std::fabs(l2 - s) > 0.02 * s)
			return false;
		float u[16];
		for (int i = 0; i < 12; i++)
			u[i] = static_cast<float>(m[i] / s);
		for (int i = 12; i < 16; i++)
			u[i] = m[i];
		u[3] = u[7] = u[11] = 0.0f;
		if (!FromGame(u, out))
			return false;
		*scale = s;
		return true;
	}

	void Mul44(const float a[16], const float b[16], float out[16])
	{
		float t[16];
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 4; j++)
				t[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j] + a[4 * i + 3] * b[12 + j];
		std::memcpy(out, t, sizeof(t));
	}

	bool UseLocals(bool offline, bool eth_enabled, bool drawtime)
	{
		return offline && !eth_enabled && !drawtime;
	}

	u8 SlotFlags(int si, bool locals, bool arms)
	{
		const u8 self = (si == 1) ? static_cast<u8>(kFlagSelf | kFlagCollapse) : static_cast<u8>(0);
		if (locals)
			return static_cast<u8>(kFlagLocals | self);
		return (si == 1) ? self : static_cast<u8>(arms ? kFlagAttach : 0);
	}

	void PackSlot(const BlockSlot& s, u32 w[16])
	{
		u8 b[64] = {};
		const auto put32 = [&b](int at, u32 v) {
			for (int k = 0; k < 4; k++)
				b[at + k] = static_cast<u8>(v >> (8 * k));
		};
		put32(0x00, s.record);
		put32(0x04, s.table);
		b[0x08] = s.count;
		b[0x09] = s.ref;
		b[0x0A] = s.flags;
		b[0x0B] = s.nloc;
		put32(0x0C, s.keep[0]);
		put32(0x10, s.keep[1]);
		for (int k = 0; k < 2; k++)
		{
			b[0x14 + 4 * k] = static_cast<u8>(s.attach_offset[k]);
			b[0x15 + 4 * k] = static_cast<u8>(s.attach_offset[k] >> 8);
			b[0x16 + 4 * k] = s.attach_bone[k];
		}
		for (int k = 0; k < kMaxOut; k++)
			b[0x1C + k] = s.bones[k];
		for (int k = 0; k < 16; k++)
			w[k] = static_cast<u32>(b[4 * k]) | (static_cast<u32>(b[4 * k + 1]) << 8) | (static_cast<u32>(b[4 * k + 2]) << 16) |
				   (static_cast<u32>(b[4 * k + 3]) << 24);
	}

	// --- The host ------------------------------------------------------------------------------------------------------
	namespace
	{
		struct SlotState
		{
			Weights w;
			int state = -1;
			u64 last_ms = 0;
			// Locals mode: what cave U was given for each bone, and the animation's own local last seen. A local in memory
			// that still equals ours (the animation did not run since: a frozen or paused channel) is not the
			// animation's: the last one seen stands in for it, so a pose never feeds back into itself.
			bool sent[kMaxBones] = {};
			float sent_m[kMaxBones][16] = {};
			bool seen[kMaxBones] = {};
			float seen_m[kMaxBones][16] = {};
			u32 record = 0;
		};
		SlotState s_slot[kSlots];
		bool s_written = false;
		u32 s_written_block = 0, s_written_tables = 0;
		int s_why = -1;
		u32 s_stamp = 0;
		u64 s_log_ms = 0;

		bool InRam(u32 a, u32 len) { return a >= 0x100000u && a + len <= Ps2MemSize::MainRam && a + len > a; }

		void ReadF16(u32 a, float m[16])
		{
			for (int k = 0; k < 16; k++)
				m[k] = std::bit_cast<float>(memRead32(a + 4u * static_cast<u32>(k)));
		}

		void ClearBlock(u32 block, u32 tables)
		{
			for (u32 off = 0; off < 0x100; off += 4)
				memWrite32(block + off, 0);
			for (u32 off = 0; off < 2 * ProfileDB::kBodyTableBytes; off += 4)
				memWrite32(tables + off, 0);
		}

		// The block and the tables are ours: zero, or our magic in the block (a table may hold our matrices).
		bool RegionOurs(u32 block, u32 tables)
		{
			if (memRead32(block) == kBlockMagic)
				return true;
			for (u32 off = 0; off < 0x100; off += 4)
				if (memRead32(block + off) != 0)
					return false;
			for (u32 off = 0; off < 2 * ProfileDB::kBodyTableBytes; off += 4)
				if (memRead32(tables + off) != 0)
					return false;
			return true;
		}

		// One character's skeleton, read from the game: the parent table, the chest subtree's locals and the chest's
		// world built from the record's matrix down the chain (the matrices in memory may hold last draw's overrides).
		struct Skeleton
		{
			int n = 0;
			int parents[kMaxBones] = {};
			u32 ptr[kMaxBones] = {};
			Frame locals[kMaxBones];
			Frame chest;        // the chest's world frame (unit axes)
			double scale = 1.0; // the character's
			float chest_m[16] = {};
		};

		bool ReadSkeleton(const ProfileDB::BodyParams& bp, const BoneMap& map, u32 rec, SlotState* st, Skeleton* sk,
			const char** why)
		{
			*why = nullptr;
			const u32 model = memRead32(rec + bp.bones);
			if (!InRam(model, bp.bone_table + 4u * kMaxBones))
			{
				*why = "the record has no model";
				return false;
			}
			const int n = memRead16(model + bp.bone_count);
			if (n <= 0 || n > kMaxBones)
			{
				*why = "the model's bone count is out of range";
				return false;
			}
			sk->n = n;
			for (int b = 0; b < n; b++)
			{
				const u32 p = memRead32(model + bp.bone_table + 4u * static_cast<u32>(b));
				if (!InRam(p, 0x190) || (p & 15u) != 0 || static_cast<s16>(memRead16(p + bp.bone_index)) != b)
				{
					*why = "a bone struct is not where its table says";
					return false;
				}
				sk->ptr[b] = p;
			}
			for (int b = 0; b < n; b++)
			{
				const u32 pp = memRead32(sk->ptr[b] + bp.bone_parent);
				sk->parents[b] = -1;
				for (int k = 0; k < n && pp != 0; k++)
					if (sk->ptr[k] == pp)
						sk->parents[b] = k;
				if (pp != 0 && sk->parents[b] < 0)
				{
					*why = "a bone's parent is not one of the model's bones";
					return false;
				}
			}
			if (!CheckMap(map, sk->parents, n, nullptr))
			{
				*why = "the bone map does not fit this model";
				return false;
			}
			// The chest's world: the record's matrix (+0x50), then every local from the root down to the chest.
			int chain[kMaxBones];
			int len = 0;
			for (int b = map.chest; b >= 0 && len < kMaxBones; b = sk->parents[b])
				chain[len++] = b;
			float w[16];
			ReadF16(rec + 0x50, w);
			for (int i = len - 1; i >= 0; i--)
			{
				float l[16];
				ReadF16(sk->ptr[chain[i]] + bp.local, l);
				Mul44(l, w, w);
			}
			std::memcpy(sk->chest_m, w, sizeof(w));
			if (!FromGameScaled(w, &sk->chest, &sk->scale))
			{
				*why = "the chest's world matrix is not a scaled rotation";
				return false;
			}
			int order[kMaxBones];
			const int count = Subtree(sk->parents, n, map.chest, order, kMaxBones);
			for (int i = 0; i < count; i++)
			{
				float l[16];
				const int b = order[i];
				ReadF16(sk->ptr[b] + bp.local, l);
				if (st && st->record == rec)
				{
					if (st->sent[b] && std::memcmp(l, st->sent_m[b], sizeof(l)) == 0)
					{
						if (st->seen[b])
							std::memcpy(l, st->seen_m[b], sizeof(l));
					}
					else
					{
						st->seen[b] = true;
						std::memcpy(st->seen_m[b], l, sizeof(l));
					}
				}
				if (!FromGame(l, &sk->locals[order[i]]))
				{
					*why = "a local matrix under the chest is not a rotation";
					return false;
				}
			}
			return true;
		}

		// A game point / direction into the chest's frame (game units at the model's own scale).
		void PointToChest(const Skeleton& sk, const float g[3], double out[3])
		{
			const double w[3] = {g[0], g[1], g[2]};
			ToFrame(sk.chest, w, out);
			for (int k = 0; k < 3; k++)
				out[k] /= sk.scale;
		}
		void DirToChest(const Skeleton& sk, const float g[3], double out[3])
		{
			const double w[3] = {g[0], g[1], g[2]};
			DirToFrame(sk.chest, w, out);
		}
		// And back to the game's world.
		void ChestToWorld(const Skeleton& sk, const double c[3], double w[3])
		{
			for (int k = 0; k < 3; k++)
				w[k] = (c[0] * sk.chest.r[0][k] + c[1] * sk.chest.r[1][k] + c[2] * sk.chest.r[2][k]) * sk.scale + sk.chest.t[k];
		}

		struct Input
		{
			bool head = false;
			Presence::GamePose head_pose;
			bool hand[2] = {false, false}; // per VR hand (0 left, 1 right)
			Presence::GamePose hand_pose[2];
			bool head_anchor = true; // the hands relative to the VR head, placed at the character's eyes
		};

		// The targets in the chest's frame. left_arm: which of the map's arms is the character's left.
		Targets MakeTargets(const ProfileDB::BodyParams& bp, const BoneMap& map, const Skeleton& sk, const Input& in, int left_arm)
		{
			Targets tg;
			if (in.head)
			{
				tg.head = true;
				DirToChest(sk, in.head_pose.fwd, tg.head_fwd);
				DirToChest(sk, in.head_pose.up, tg.head_up);
			}
			// The character's eyes, from its animated head (chest frame).
			const Frame head = Compose(sk.locals[map.head], sk.locals[map.neck]);
			double eye[3];
			for (int k = 0; k < 3; k++)
				eye[k] = bp.eye[0] * head.r[0][k] + bp.eye[1] * head.r[1][k] + bp.eye[2] * head.r[2][k] + head.t[k];
			double vr_head[3] = {};
			if (in.head)
				PointToChest(sk, in.head_pose.pos, vr_head);
			for (int h = 0; h < 2; h++)
			{
				if (!in.hand[h])
					continue;
				const int a = (h == 0) ? left_arm : 1 - left_arm;
				tg.hand[a] = true;
				PointToChest(sk, in.hand_pose[h].pos, tg.hand_pos[a]);
				if (in.head_anchor && in.head)
					for (int k = 0; k < 3; k++)
						tg.hand_pos[a][k] = tg.hand_pos[a][k] - vr_head[k] + eye[k];
				DirToChest(sk, in.hand_pose[h].fwd, tg.hand_fwd[a]);
				DirToChest(sk, in.hand_pose[h].up, tg.hand_up[a]);
			}
			return tg;
		}

		double Dist3(const double a[3], const double b[3])
		{
			return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
		}
	} // namespace

	void Shutdown()
	{
		if (s_written && VMManager::HasValidVM())
			ClearBlock(s_written_block, s_written_tables);
		s_written = false;
	}

	void Poll()
	{
		const Pcsx2Config::VROptions& vr = EmuConfig.VR;
		const ProfileDB::Profile* profile = nullptr;
		if (vr.VrBody && VMManager::GetState() == VMState::Running)
		{
			const std::string serial = VMManager::GetDiscSerial();
			if (!serial.empty())
				profile = ProfileDB::Lookup(serial, VMManager::GetDiscCRC());
		}
		const ProfileDB::BodyParams* bp = (profile && profile->body.has_value()) ? &profile->body.value() : nullptr;
		const auto set_why = [](int why) {
			static const char* const kWhy[] = {"live", "off", "this game's profile has no body block",
				"waiting: cave V is not in (its draw site does not read the cave's call)",
				"its free RAM (the host block or the tables) is not free: left alone"};
			if (why != s_why)
			{
				s_why = why;
				Console.WriteLn("(VR) Body: %s.", kWhy[why]);
			}
		};
		const auto clear = [&]() {
			if (s_written && VMManager::HasValidVM())
				ClearBlock(s_written_block, s_written_tables);
			s_written = false;
			for (SlotState& st : s_slot)
			{
				st.w = Weights{};
				st.record = 0;
			}
		};
		if (!vr.VrBody)
		{
			clear();
			if (s_why >= 0)
				set_why(1);
			return;
		}
		if (!bp)
		{
			clear();
			set_why(2);
			return;
		}
		if (s_written && (s_written_block != bp->block || s_written_tables != bp->tables))
			clear();
		if (memRead32(bp->hook) != bp->hook_on)
		{
			clear();
			set_why(3);
			return;
		}
		if (!s_written && !RegionOurs(bp->block, bp->tables))
		{
			set_why(4);
			return;
		}
		set_why(0);

		BoneMap map;
		map.chest = bp->chest;
		map.neck = bp->neck;
		map.head = bp->head;
		for (int a = 0; a < 2; a++)
			map.arm[a] = {bp->arms[a][0], bp->arms[a][1], bp->arms[a][2], bp->arms[a][3]};

		// Which arm is the character's left: the game's handedness through this console's own mapping (a VR point to
		// the left of the head, against up x forward in the game; File #1: the model's +X side is its left).
		int left_arm = 0;
		CameraDriver::Mapping mapping;
		const bool have_mapping = CameraDriver::GetMapping(&mapping);
		if (have_mapping)
		{
			const float o[3] = {0.0f, 1.6f, 0.0f}, l[3] = {-1.0f, 1.6f, 0.0f}, f[3] = {0.0f, 1.6f, -1.0f};
			float go[3], gl[3], gf[3];
			if (CameraDriver::GameFromVr(mapping, o, go) && CameraDriver::GameFromVr(mapping, l, gl) &&
				CameraDriver::GameFromVr(mapping, f, gf))
			{
				const double L[3] = {gl[0] - go[0], gl[1] - go[1], gl[2] - go[2]};
				const double F[3] = {gf[0] - go[0], gf[1] - go[1], gf[2] - go[2]};
				// up (0,1,0) x F = (F.z, 0, -F.x): the +X of a frame whose up is +Y and forward F.
				left_arm = (L[0] * F[2] - L[2] * F[0] >= 0.0) ? 0 : 1;
			}
		}

		static const bool s_log_env = [] {
			const char* e = std::getenv("PCSX2_VR_BODY_LOG");
			return e && e[0] == '1';
		}();
		static const bool s_loopback_env = [] {
			const char* e = std::getenv("PCSX2_VR_PRESENCE_LOOPBACK");
			return e && e[0] == '1';
		}();
		// The fallback: world matrices at the draw (cave V) instead of locals in the update (cave U).
		static const bool s_drawtime = [] {
			const char* e = std::getenv("PCSX2_VR_BODY_DRAWTIME");
			return e && e[0] == '1';
		}();
		// Cave U only strictly offline (night review C1): online every slot is posed at the draw (cave V) only.
		const bool use_locals = UseLocals(CameraDriver::NetworkOffline(), EmuConfig.DEV9.EthEnable, s_drawtime);
		const u64 now = CameraDriver::SteadyNowMs();
		const bool log_now = (s_log_env || s_loopback_env) && now - s_log_ms >= 2000;
		if (log_now)
			s_log_ms = now;

		const Tuning tu;
		BlockSlot slots[kSlots];
		static float tables[kSlots][kMaxOut][16];
		for (int si = 0; si < kSlots; si++)
		{
			SlotState& st = s_slot[si];
			const double dt = (st.last_ms != 0 && now > st.last_ms) ? (now - st.last_ms) / 1000.0 : 0.0;
			st.last_ms = now;
			Input in;
			u32 rec = 0;
			int state = 0;
			if (si == 0)
			{
				Presence::PartnerGame pg;
				if (!Presence::GetPartnerGame(&pg))
					state = 1;
				else
				{
					rec = pg.record;
					in.head = (pg.flags & Presence::kFlagHead) != 0;
					in.head_pose = pg.head;
					for (int h = 0; h < 2; h++)
					{
						in.hand[h] = (pg.flags & (h == 0 ? Presence::kFlagLeft : Presence::kFlagRight)) != 0;
						in.hand_pose[h] = pg.hand[h];
					}
					in.head_anchor = true;
				}
			}
			else
			{
				u32 base = 0;
				const VRInputSnapshot snap = GetInputSnapshot();
				if (!vr.VrBodySelf)
					state = 1;
				else if (!CameraDriver::NetworkOffline() || !have_mapping || !CameraDriver::LocalRecord(&base))
					state = 2;
				else if (snap.generation == 0 || !snap.actions_active)
					state = 3;
				else
				{
					rec = base;
					for (int h = 0; h < 2; h++)
					{
						const VRHandState& hs = snap.hands[h];
						if (!hs.grip_pose.valid)
							continue;
						const VRPose& orient = hs.aim_pose.valid ? hs.aim_pose : hs.grip_pose;
						in.hand[h] = Presence::PoseToGame(mapping, hs.grip_pose.position_xyz.data(), orient.orientation_xyzw.data(),
							&in.hand_pose[h]);
					}
					in.head_anchor = false; // your own hands where you see them
				}
			}
			Skeleton sk;
			const char* why = nullptr;
			Pose pose;
			Targets tg;
			bool solved = false;
			if (rec != 0 && state == 0)
			{
				if (st.record != rec)
				{
					std::memset(st.sent, 0, sizeof(st.sent));
					std::memset(st.seen, 0, sizeof(st.seen));
					st.record = rec;
				}
				if (!ReadSkeleton(*bp, map, rec, &st, &sk, &why))
					state = 4;
				else
				{
					tg = MakeTargets(*bp, map, sk, in, left_arm);
					Weights want;
					WantWeights(map, sk.locals, tg, tu, &want);
					if (si == 1)
						want.head = 0.0; // your own head is never drawn
					EaseWeights(&st.w, want, dt, tu);
					solved = SolveBody(map, sk.locals, sk.parents, sk.n, tg, st.w, tu, &pose) && pose.count > 0;
					if (!solved)
						state = 5;
				}
			}
			else
				st.w = Weights{};
			if (state != st.state)
			{
				st.state = state;
				static const char* const kState[] = {"posed", "nobody to pose", "not offline, no mapping or no local record",
					"no VR hands", "skeleton not usable", "nothing to pose (no tracked part in reach)"};
				Console.WriteLn("(VR) Body: %s %s%s%s (record %08X).", si == 0 ? "partner" : "self", kState[state], why ? ": " : "",
					why ? why : "", rec);
			}
			if (!solved)
				continue;
			BlockSlot& bs = slots[si];
			bs.record = rec;
			bs.table = bp->tables + static_cast<u32>(si) * ProfileDB::kBodyTableBytes;
			bs.count = 0;
			bs.ref = static_cast<u8>(map.chest);
			const bool arms = st.w.arm[0] > 0.0 || st.w.arm[1] > 0.0;
			bs.flags = SlotFlags(si, use_locals, arms);
			// The held items go back on the hands (the game's own FUN_006313A0 with the record's item slots, as its update
			// does: the item slot byte +0xC7C on the -X wrist, +0xC7D on the +X wrist).
			bs.attach_offset[0] = 0xC7C;
			bs.attach_bone[0] = static_cast<u8>(map.arm[1].wrist);
			bs.attach_offset[1] = 0xC7D;
			bs.attach_bone[1] = static_cast<u8>(map.arm[0].wrist);
			for (int k = 0; k < pose.count; k++)
			{
				if (!use_locals)
				{
					bs.bones[k] = static_cast<u8>(pose.bone[k]);
					ToGame(pose.rel[k], tables[si][k]);
				}
				// The bones the self draw keeps (the arms); every other one's axes are zeroed at the draw.
				if (pose.bone[k] < 32)
					bs.keep[0] |= 1u << pose.bone[k];
				else
					bs.keep[1] |= 1u << (pose.bone[k] - 32);
			}
			if (!use_locals)
				bs.count = static_cast<u8>(pose.count);
			else
			{
				// The locals, then the head's subtree as draw-time frames (over the game's own head look).
				int n = 0;
				for (int k = 0; k < pose.lcount && n < kMaxOut; k++, n++)
				{
					bs.bones[n] = static_cast<u8>(pose.lbone[k]);
					ToGame(pose.lrel[k], tables[si][n]);
					st.sent[pose.lbone[k]] = true;
					std::memcpy(st.sent_m[pose.lbone[k]], tables[si][n], sizeof(tables[si][n]));
				}
				bs.nloc = static_cast<u8>(n);
				int head_sub[kMaxBones];
				const int nh = Subtree(sk.parents, sk.n, map.head, head_sub, kMaxBones);
				for (int k = 0; k < pose.count && n < kMaxOut; k++)
				{
					bool in_head = false;
					for (int q = 0; q < nh; q++)
						in_head |= head_sub[q] == pose.bone[k];
					if (!in_head)
						continue;
					bs.bones[n] = static_cast<u8>(pose.bone[k]);
					ToGame(pose.rel[k], tables[si][n]);
					n++;
				}
				bs.count = static_cast<u8>(n);
			}
			if (log_now)
			{
				// The wrists: the solve's (chest frame) put in the world, against the VR targets; and what the cave wrote at
				// the last draw (the wrist bone's world matrix now in memory).
				std::string line;
				for (int a = 0; a < 2; a++)
				{
					if (!(st.w.arm[a] > 0.0))
					{
						line += fmt::format(" arm{} animation;", a);
						continue;
					}
					double wt[3], tt[3], tc[3];
					ChestToWorld(sk, pose.wrist[a], wt);
					for (int k = 0; k < 3; k++)
						tc[k] = tg.hand_pos[a][k] - tg.hand_fwd[a][k] * tu.wrist_back_cm;
					ChestToWorld(sk, tc, tt);
					const u32 wp = sk.ptr[map.arm[a].wrist];
					const double mem[3] = {std::bit_cast<float>(memRead32(wp + 0x30)), std::bit_cast<float>(memRead32(wp + 0x34)),
						std::bit_cast<float>(memRead32(wp + 0x38))};
					line += fmt::format(" arm{} (wrist bone {}) weight {:.2f}: target ({:.1f}, {:.1f}, {:.1f}), solved ({:.1f}, {:.1f}, "
										"{:.1f}) {:.1f} cm off{}, in memory ({:.1f}, {:.1f}, {:.1f}) {:.1f} cm off;",
						a, map.arm[a].wrist, st.w.arm[a], tt[0], tt[1], tt[2], wt[0], wt[1], wt[2], Dist3(wt, tt),
						pose.reach_clamped[a] ? " (out of reach)" : "", mem[0], mem[1], mem[2], Dist3(mem, tt));
				}
				if (st.w.head > 0.0)
				{
					const u32 hp = sk.ptr[map.head];
					const double fz[3] = {std::bit_cast<float>(memRead32(hp + 0x20)), std::bit_cast<float>(memRead32(hp + 0x24)),
						std::bit_cast<float>(memRead32(hp + 0x28))};
					double fl = std::sqrt(fz[0] * fz[0] + fz[1] * fz[1] + fz[2] * fz[2]);
					fl = fl > 1e-9 ? fl : 1.0;
					const float* hf = in.head_pose.fwd;
					const double dot = (fz[0] * hf[0] + fz[1] * hf[1] + fz[2] * hf[2]) / fl;
					line += fmt::format(" head forward in memory ({:.3f}, {:.3f}, {:.3f}) vs VR ({:.3f}, {:.3f}, {:.3f}): {:.1f} deg{};",
						fz[0] / fl, fz[1] / fl, fz[2] / fl, hf[0], hf[1], hf[2], std::acos(std::clamp(dot, -1.0, 1.0)) * 180.0 / kPi,
						pose.head_clamped ? " (clamped)" : "");
				}
				// The chest built down the chain against the chest's matrix in memory (the cave never writes it for a partner).
				const u32 cp = sk.ptr[map.chest];
				const double cm[3] = {std::bit_cast<float>(memRead32(cp + 0x30)), std::bit_cast<float>(memRead32(cp + 0x34)),
					std::bit_cast<float>(memRead32(cp + 0x38))};
				Console.WriteLn("(VR) Body: %s record %08X, %d bones, scale %.3f, left arm %d, chest chain vs memory %.2f:%s",
					si == 0 ? "partner" : "self", rec, pose.count, sk.scale, left_arm, Dist3(cm, sk.chest.t), line.c_str());
			}
		}

		// The tables first, then the slots, then the magic (all on the CPU thread between two EE slices: the cave sees one
		// whole block).
		bool any = false;
		for (int si = 0; si < kSlots; si++)
			any |= slots[si].record != 0;
		if (!any)
		{
			clear();
			return;
		}
		for (int si = 0; si < kSlots; si++)
		{
			for (int k = 0; k < slots[si].count; k++)
				for (int q = 0; q < 16; q++)
					memWrite32(slots[si].table + 64u * static_cast<u32>(k) + 4u * static_cast<u32>(q), std::bit_cast<u32>(tables[si][k][q]));
			u32 w[16];
			PackSlot(slots[si], w);
			// Words 0-13 only: +0x38 is cave U's (the stamp of the list it last applied).
			for (int q = 0; q < 14; q++)
				memWrite32(bp->block + kSlotOffset + kSlotBytes * static_cast<u32>(si) + 4u * static_cast<u32>(q), w[q]);
		}
		// The held items' attach: FUN_006313A0 (GAME overlay; the cave checks its first word before each call).
		const bool game_overlay = memRead32(0x540004) == 3 && memRead32(0x6313A0) == 0x27BDFF50u;
		memWrite32(bp->block + 0x08, game_overlay ? 0x6313A0u : 0u);
		memWrite32(bp->block + 0x0C, game_overlay ? 0x27BDFF50u : 0u);
		++s_stamp;
		memWrite32(bp->block + 0x04, s_stamp);
		memWrite32(bp->block, kBlockMagic);
		s_written = true;
		s_written_block = bp->block;
		s_written_tables = bp->tables;
	}
	// --- Self-test -----------------------------------------------------------------------------------------------------
	namespace
	{
		// File #1's player skeletons as the game has them (truevr/st04: Kevin's 36 bones, the woman's 43), parent tables
		// from each bone's +0xC8, the local offsets from +0x70.
		const int kKevinParents[36] = {-1, 0, 1, 2, 3, 4, 5, 3, 7, 8, 2, 10, 11, 12, 13, 14, 11, 16, 17, 18, 11, 20, 21, 22, 22,
			22, 22, 22, 19, 28, 28, 28, 15, 32, 32, 32};
		const int kWomanParents[43] = {-1, 0, 1, 2, 3, 4, 5, 3, 7, 8, 2, 10, 11, 12, 13, 14, 11, 16, 17, 18, 11, 20, 21, 22, 22,
			22, 22, 22, 19, 28, 28, 28, 15, 32, 32, 32, 21, 36, 3, 38, 39, 38, 41};
		const double kKevinOffsets[36][3] = {{0, 0, 0}, {0, 0, 0}, {-0.07, 105.54, 0.13}, {0, 0, 0}, {7.81, -16.0, 0}, {0, -39.0, 0.5},
			{0, -41.97, -1.5}, {-7.81, -16.0, 0}, {0, -39.0, 0.5}, {0, -41.97, -1.5}, {0, 1.0, 0}, {0, 16.0, -0.08}, {9.31, 16.75, -1.18},
			{8.28, 0, 0}, {26.06, 0, -1.08}, {25.13, 0, 0.8}, {-9.31, 16.75, -1.18}, {-8.28, 0, 0}, {-26.06, 0, -1.08}, {-25.13, 0, 0.8},
			{0, 20.65, -0.97}, {0, 7.43, 0}, {0, 0, 0}, {3.0, 8.0, 8.0}, {3.0, 6.2, 8.0}, {-3.0, 8.0, 8.0}, {-3.0, 6.2, 8.0}, {0, 3.0, 0.5},
			{0, 0, 0}, {-3.26, -2.58, 3.57}, {-9.43, 1.44, 2.96}, {-9.26, 1.31, -2.03}, {0, 0, 0}, {3.26, -2.58, 3.57}, {9.43, 1.44, 2.96},
			{9.26, 1.31, -2.03}};

		double D3(const V& a, const V& b) { return Len(a - b); }
	} // namespace

	bool SelfTest()
	{
		int checks = 0, failed = 0;
		const auto check = [&](bool ok, const char* name) {
			checks++;
			if (!ok)
			{
				failed++;
				Console.Error("(VR) Body self-test FAILED: %s", name);
			}
		};

		// --- Frames ---
		{
			Frame a;
			HeadFrame(0.4, 0.3, -0.2, a.r);
			a.t[0] = 5.0;
			a.t[1] = -2.0;
			a.t[2] = 7.0;
			const Frame id = Compose(a, Inverse(a));
			bool ok = true;
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					ok &= std::fabs(id.r[i][j] - (i == j ? 1.0 : 0.0)) < 1e-12;
			ok &= Len(Ld(id.t)) < 1e-12;
			check(ok, "a frame composed with its inverse is the identity");
			float m[16];
			ToGame(a, m);
			Frame b;
			check(FromGame(m, &b) && RotAngleDeg(a.r, b.r) < 1e-3 && D3(Ld(a.t), Ld(b.t)) < 1e-5, "ToGame / FromGame round trip");
			float sm[16];
			std::memcpy(sm, m, sizeof(m));
			for (int i = 0; i < 12; i++)
				sm[i] *= 1.05f;
			double sc = 0.0;
			check(!FromGame(sm, &b) && FromGameScaled(sm, &b, &sc) && std::fabs(sc - 1.05) < 1e-5 && RotAngleDeg(a.r, b.r) < 1e-3,
				"a matrix with the character's scale (1.05): refused as a rotation, split into rotation and scale");
			sm[0] = std::numeric_limits<float>::quiet_NaN();
			check(!FromGame(sm, &b) && !FromGameScaled(sm, &b, &sc), "a NaN refused");
			float sk[16];
			std::memcpy(sk, m, sizeof(m));
			sk[4] += 0.2f;
			check(!FromGame(sk, &b) && !FromGameScaled(sk, &b, &sc), "a skewed matrix refused");
			float p[16], q[16], r[16];
			Frame f2;
			HeadFrame(-0.7, 0.1, 0.05, f2.r);
			f2.t[1] = 3.0;
			ToGame(f2, p);
			Mul44(m, p, q);
			ToGame(Compose(a, f2), r);
			bool same = true;
			for (int i = 0; i < 16; i++)
				same &= std::fabs(q[i] - r[i]) < 1e-4f;
			check(same, "Mul44 is the game's order (child x parent), as Compose");
		}

		// --- Two-bone IK: reach, clamps, singularities ---
		{
			const double s[3] = {0, 0, 0}, pole[3] = {0, -1, 0};
			IkResult ik;
			const double t1[3] = {20, -30, 10};
			bool ok = SolveTwoBone(s, 26.0, 25.0, t1, pole, 150.0, &ik);
			check(ok && D3(Ld(ik.wrist), Ld(t1)) < 1e-9 && std::fabs(Len(Ld(ik.elbow)) - 26.0) < 1e-9 &&
					  std::fabs(D3(Ld(ik.wrist), Ld(ik.elbow)) - 25.0) < 1e-9 && !ik.clamped,
				"a reachable target: the wrist on it, both bones their length");
			{
				// The elbow toward the pole (its part off the reach line).
				const V d = Norm(Ld(t1));
				const V e = Ld(ik.elbow);
				const V off = e - d * Dot(e, d);
				const V pp = Ld(pole) - d * Dot(Ld(pole), d);
				check(Dot(off, pp) > 0.0, "the elbow goes toward the pole");
			}
			const double far[3] = {100, 0, 0};
			ok = SolveTwoBone(s, 26.0, 25.0, far, pole, 150.0, &ik);
			check(ok && ik.clamped && std::fabs(ik.wrist[0] - 0.999 * 51.0) < 1e-9 && std::fabs(ik.wrist[1]) < 1e-9,
				"out of reach: the arm straight (0.999 of its length) toward the target");
			const double nearT[3] = {1, 0, 0};
			ok = SolveTwoBone(s, 26.0, 25.0, nearT, pole, 150.0, &ik);
			{
				const V e = Ld(ik.elbow), w = Ld(ik.wrist);
				const double inner = std::acos(std::clamp(Dot(Norm(V{} - e), Norm(w - e)), -1.0, 1.0)) / kDeg;
				check(ok && ik.clamped && std::fabs(inner - 30.0) < 0.01, "too near: the elbow bent to its limit (150 deg), no further");
			}
			const double at[3] = {0, 0, 0};
			ok = SolveTwoBone(s, 26.0, 25.0, at, pole, 150.0, &ik);
			check(ok && Finite(Ld(ik.elbow)) && Finite(Ld(ik.wrist)) && std::fabs(Len(Ld(ik.elbow)) - 26.0) < 1e-9 &&
					  std::fabs(D3(Ld(ik.wrist), Ld(ik.elbow)) - 25.0) < 1e-9,
				"a target at the shoulder: finite, both bones their length");
			const double down[3] = {0, -40, 0};
			ok = SolveTwoBone(s, 26.0, 25.0, down, pole, 150.0, &ik);
			check(ok && Finite(Ld(ik.elbow)) && std::fabs(Len(Ld(ik.elbow)) - 26.0) < 1e-9 && D3(Ld(ik.wrist), Ld(down)) < 1e-9,
				"a pole along the reach: any perpendicular, still on the target");
			const double zero_pole[3] = {0, 0, 0};
			ok = SolveTwoBone(s, 26.0, 25.0, t1, zero_pole, 150.0, &ik);
			check(ok && D3(Ld(ik.wrist), Ld(t1)) < 1e-9, "no pole: still on the target");
			const double nan3[3] = {std::numeric_limits<double>::quiet_NaN(), 0, 0};
			check(!SolveTwoBone(s, 0.0, 25.0, t1, pole, 150.0, &ik) && !SolveTwoBone(s, 26.0, 25.0, nan3, pole, 150.0, &ik),
				"a zero bone or a NaN target refused");
		}

		// --- The head's angles ---
		{
			double r[3][3];
			HeadFrame(30 * kDeg, 20 * kDeg, 10 * kDeg, r);
			double y, p, rl;
			HeadAngles(r[2], r[1], &y, &p, &rl);
			check(std::fabs(y - 30 * kDeg) < 1e-9 && std::fabs(p - 20 * kDeg) < 1e-9 && std::fabs(rl - 10 * kDeg) < 1e-9,
				"head angles round trip (yaw 30, pitch 20 down, roll 10)");
			check(std::fabs(Dot(Cross(Row(r, 0), Row(r, 1)), Row(r, 2)) - 1.0) < 1e-9 && r[2][0] > 0.0 && r[2][1] < 0.0,
				"the head frame is a rotation; positive yaw turns toward +X, positive pitch looks down");
		}

		// --- The bone map ---
		{
			const BoneMap m = DefaultMap();
			std::string why;
			check(CheckMap(m, kKevinParents, 36, &why), "the map fits Kevin's 36 bones");
			check(CheckMap(m, kWomanParents, 43, &why), "and the woman's 43");
			int broken[36];
			std::memcpy(broken, kKevinParents, sizeof(broken));
			broken[15] = 13;
			check(!CheckMap(m, broken, 36, &why), "a wrist hung on the upper arm: refused");
			check(!CheckMap(m, kKevinParents, 18, &why), "a model too small for the map: refused");
			BoneMap swapped = m;
			swapped.arm[1] = swapped.arm[0];
			check(!CheckMap(swapped, kKevinParents, 36, &why), "both arms one: refused");
			int sub[kMaxBones];
			const int na = Subtree(kKevinParents, 36, 13, sub, kMaxBones);
			const int nn = Subtree(kKevinParents, 36, 20, sub, kMaxBones);
			const int nw = Subtree(kWomanParents, 43, 20, sub, kMaxBones);
			check(na == 7 && nn == 8 && nw == 10, "subtrees: an arm 7 bones (upper to fingers), Kevin's neck 8, the woman's 10 (hair)");
			const int nc = Subtree(kWomanParents, 43, 11, sub, kMaxBones);
			bool order_ok = nc > 0 && sub[0] == 11;
			for (int i = 1; i < nc; i++)
			{
				bool parent_before = false;
				for (int k = 0; k < i; k++)
					parent_before |= sub[k] == kWomanParents[sub[i]];
				order_ok &= parent_before;
			}
			check(order_ok && nc == 27, "the chest's subtree lists each bone after its parent (27 bones)");
		}

		// --- The whole solve, on Kevin's bind pose ---
		{
			const BoneMap m = DefaultMap();
			Frame locals[36];
			for (int b = 0; b < 36; b++)
				for (int k = 0; k < 3; k++)
					locals[b].t[k] = kKevinOffsets[b][k];
			const Tuning tu;
			// The shoulders relative to the chest: clavicle + upper.
			const V sh0 = Ld(kKevinOffsets[12]) + Ld(kKevinOffsets[13]);
			const V sh1 = Ld(kKevinOffsets[16]) + Ld(kKevinOffsets[17]);
			Targets tg;
			tg.hand[0] = tg.hand[1] = true;
			const V want0 = sh0 + V{8.0, -25.0, 30.0}, want1 = sh1 + V{-5.0, -10.0, 38.0};
			// The controllers point forward (+Z), up +Y; the grip is wrist_back_cm ahead of the wrist.
			St(want0 + V{0, 0, tu.wrist_back_cm}, tg.hand_pos[0]);
			St(want1 + V{0, 0, tu.wrist_back_cm}, tg.hand_pos[1]);
			Weights w;
			w.arm[0] = w.arm[1] = 1.0;
			Pose pose;
			bool ok = SolveBody(m, locals, kKevinParents, 36, tg, w, tu, &pose);
			check(ok && pose.count == 14 && D3(Ld(pose.wrist[0]), want0) < 1e-6 && D3(Ld(pose.wrist[1]), want1) < 1e-6,
				"both wrists on their targets (14 bones: two arms of 7)");
			// The hands: fingers along the controllers' forward, thumbs up.
			int i15 = -1, i19 = -1, i14 = -1, i32 = -1;
			for (int k = 0; k < pose.count; k++)
			{
				i15 = pose.bone[k] == 15 ? k : i15;
				i19 = pose.bone[k] == 19 ? k : i19;
				i14 = pose.bone[k] == 14 ? k : i14;
				i32 = pose.bone[k] == 32 ? k : i32;
			}
			check(i15 >= 0 && i19 >= 0 && Dot(Row(pose.rel[i15].r, 0), V{0, 0, 1}) > 0.97 && Dot(Row(pose.rel[i19].r, 0), V{0, 0, -1}) > 0.97,
				"the hands' fingers along the controllers (the +X hand's +X, the -X hand's -X)");
			check(i32 >= 0 && D3(Ld(pose.rel[i32].t), want0) < 1e-6, "the hand bone under the wrist follows it");
			// The elbow on the outside-down side and the bones' lengths kept.
			check(i14 >= 0 && std::fabs(D3(Ld(pose.rel[i14].t), sh0) - Len(Ld(kKevinOffsets[14]))) < 1e-6 && pose.rel[i14].t[1] < sh0.y,
				"the elbow below the shoulder, the upper arm its length");
			// The wrist's swing clamped: a controller pointing back along the forearm.
			Targets back = tg;
			St(V{0, 0, -1}, back.hand_fwd[0]);
			St(want0 + V{0, 0, -tu.wrist_back_cm}, back.hand_pos[0]);
			Pose pb;
			ok = SolveBody(m, locals, kKevinParents, 36, back, w, tu, &pb);
			int b14 = -1, b15 = -1;
			for (int k = 0; k < pb.count; k++)
			{
				b14 = pb.bone[k] == 14 ? k : b14;
				b15 = pb.bone[k] == 15 ? k : b15;
			}
			// The swing is how far the wrist turns the forearm's own axis (its child offset).
			const V c2 = Ld(kKevinOffsets[15]);
			const double bend = (b14 >= 0 && b15 >= 0) ?
									std::acos(std::clamp(Dot(Norm(MulRow(c2, pb.rel[b14].r)), Norm(MulRow(c2, pb.rel[b15].r))), -1.0, 1.0)) / kDeg :
									999.0;
			check(ok && bend <= tu.wrist_swing_deg + 0.5, "a hand bent back past the wrist's limit: clamped to 75 deg");
			// A target inside the chest: the wrist pushed out in front.
			Targets inside = tg;
			St(V{2.0, 0.0, 0.0} + V{0, 0, tu.wrist_back_cm}, inside.hand_pos[0]);
			Pose pin;
			ok = SolveBody(m, locals, kKevinParents, 36, inside, w, tu, &pin);
			check(ok && pin.wrist[0][2] > 12.0, "a hand inside the chest: the wrist stays in front of it");
			// Weights: one arm off (not emitted), the other half way (its upper arm turned half as far).
			Weights half;
			half.arm[0] = 0.5;
			Pose ph;
			ok = SolveBody(m, locals, kKevinParents, 36, tg, half, tu, &ph);
			int h13 = -1, f13 = -1;
			bool other = false;
			for (int k = 0; k < ph.count; k++)
			{
				h13 = ph.bone[k] == 13 ? k : h13;
				other |= ph.bone[k] == 17;
			}
			for (int k = 0; k < pose.count; k++)
				f13 = pose.bone[k] == 13 ? k : f13;
			const double full = f13 >= 0 ? RotAngleDeg(locals[13].r, pose.rel[f13].r) : 0.0;
			const double halfa = h13 >= 0 ? RotAngleDeg(locals[13].r, ph.rel[h13].r) : 0.0;
			check(ok && !other && ph.count == 7 && std::fabs(halfa - full * 0.5) < 0.05 && full > 10.0,
				"weight 0.5: the upper arm turned half way; weight 0: that arm left to the animation");
			// The head: 120 deg yaw clamped to 75, the neck 40% of it.
			Targets th;
			th.head = true;
			HeadFrame(120 * kDeg, 0.0, 0.0, locals[0].r); // scratch
			St(Row(locals[0].r, 2), th.head_fwd);
			St(Row(locals[0].r, 1), th.head_up);
			locals[0] = Frame{};
			for (int k = 0; k < 3; k++)
				locals[0].t[k] = kKevinOffsets[0][k];
			Weights wh;
			wh.head = 1.0;
			Pose ph2;
			ok = SolveBody(m, locals, kKevinParents, 36, th, wh, tu, &ph2);
			int n20 = -1, n21 = -1;
			for (int k = 0; k < ph2.count; k++)
			{
				n20 = ph2.bone[k] == 20 ? k : n20;
				n21 = ph2.bone[k] == 21 ? k : n21;
			}
			double y21 = 0, p21 = 0, r21 = 0, y20 = 0, p20 = 0, r20 = 0;
			if (n20 >= 0 && n21 >= 0)
			{
				HeadAngles(ph2.rel[n21].r[2], ph2.rel[n21].r[1], &y21, &p21, &r21);
				HeadAngles(ph2.rel[n20].r[2], ph2.rel[n20].r[1], &y20, &p20, &r20);
			}
			check(ok && ph2.count == 8 && ph2.head_clamped && std::fabs(y21 / kDeg - 75.0) < 1e-6 && std::fabs(y20 / kDeg - 30.0) < 1e-6,
				"the head turned 120 deg: clamped at 75, the neck 30 (40%), 8 bones (neck, head, face)");
			check(n21 >= 0 && D3(Ld(ph2.rel[n21].t), Ld(kKevinOffsets[20]) + Ld(kKevinOffsets[21])) < 0.05,
				"the head stays on its neck");
			Targets none;
			Pose pn;
			check(SolveBody(m, locals, kKevinParents, 36, none, w, tu, &pn) && pn.count == 0, "nothing tracked: nothing changed");
			// The weights' targets and easing.
			Weights want;
			Targets farT = tg;
			St(sh0 + V{0, 0, 200.0}, farT.hand_pos[0]);
			WantWeights(m, locals, farT, tu, &want);
			check(want.arm[0] == 0.0 && want.arm[1] == 1.0 && want.head == 0.0, "a hand 2 m away blends back; the other stays");
			Targets behind = tg;
			St(sh0 + V{0, 0, -40.0}, behind.hand_pos[0]);
			WantWeights(m, locals, behind, tu, &want);
			check(want.arm[0] == 0.0, "a hand behind the back blends back");
			Weights cur;
			Weights one;
			one.arm[0] = 1.0;
			EaseWeights(&cur, one, 0.125, tu);
			const double mid = cur.arm[0];
			EaseWeights(&cur, one, 0.2, tu);
			check(std::fabs(mid - 0.5) < 1e-9 && cur.arm[0] == 1.0, "the blend takes 0.25 s");
		}

		// --- The block ---
		{
			BlockSlot bs;
			bs.record = 0x477EB0;
			bs.table = 0xFD200;
			bs.count = 3;
			bs.ref = 11;
			bs.flags = kFlagAttach;
			bs.nloc = 2;
			bs.keep[0] = 0x80000001u;
			bs.keep[1] = 0x3u;
			bs.attach_offset[0] = 0xC7C;
			bs.attach_bone[0] = 19;
			bs.attach_offset[1] = 0xC7D;
			bs.attach_bone[1] = 15;
			bs.bones[0] = 13;
			bs.bones[1] = 14;
			bs.bones[2] = 15;
			u32 w[16];
			PackSlot(bs, w);
			check(w[0] == 0x477EB0 && w[1] == 0xFD200 && w[2] == (3u | (11u << 8) | (1u << 16) | (2u << 24)) && w[3] == 0x80000001u && w[4] == 3u &&
					  w[5] == (0xC7Cu | (19u << 16)) && w[6] == (0xC7Du | (15u << 16)) && w[7] == (13u | (14u << 8) | (15u << 16)),
				"the slot's layout (record, table, count/ref/flags, keep mask, attaches, bones)");
			check(kSlotOffset + kSlots * kSlotBytes <= 0x100 && 0x1C + kMaxOut <= 0x38 && kMaxOut * 64 <= static_cast<int>(ProfileDB::kBodyTableBytes) &&
					  kMaxLocals < kMaxOut,
				"the slots fit the block and the bones their table");
		}

		if (failed == 0)
			Console.WriteLn("(VR) Body self-test: %d checks passed.", checks);
		else
			Console.Error("(VR) Body self-test: %d of %d checks FAILED.", failed, checks);
		return failed == 0;
	}
} // namespace VR::VrBody
