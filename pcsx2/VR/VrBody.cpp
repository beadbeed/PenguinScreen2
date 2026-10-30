// SPDX-FileCopyrightText: 2026 PenguinScreen2 contributors
// SPDX-License-Identifier: GPL-3.0

#include "VR/VrBody.h"
#include "VR/NetGate.h"

#include "VR/CameraDriver.h"
#include "VR/GunRay.h"
#include "VR/GameFeedback.h"
#include "VR/HandModel.h"
#include "VR/SpatialControls.h"
#include "VR/VRInputState.h"
#include "VR/VRProfileDB.h"
#include "VR/VrPresence.h"
#include "Config.h"
#include "Memory.h"
#include "VMManager.h"
#include "common/Console.h"
#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
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

		// One arm's chain toward a wrist target (one frame throughout): the two-bone IK from the shoulder s (c1 the forearm's
		// offset in the upper arm, c2 the wrist's in the forearm, sigma the model's X side), the upper arm's crease toward the
		// bend, the forearm on the same hinge, the hand from the controller (hf its forward, hu its up) with the wrist's twist
		// shared with the forearm and the rest and the swing clamped. SolveBody's arm, and SolveAnchored's.
		struct ArmChain
		{
			double r_up[3][3] = {}, r_fore[3][3] = {}, r_hand[3][3] = {};
			V elbow, wrist;
			bool clamped = false;
		};
		bool SolveArmChain(const V& s, const V& c1, const V& c2, double sigma, const V& tw, const V& pole, const V& hf,
			const V& hu, const Tuning& tu, ArmChain* out)
		{
			const double l1 = Len(c1), l2 = Len(c2);
			double sv[3], tv[3], pv[3];
			St(s, sv);
			St(tw, tv);
			St(pole, pv);
			IkResult ik;
			if (!SolveTwoBone(sv, l1, l2, tv, pv, tu.max_elbow_bend_deg, &ik))
				return false;
			const V e = Ld(ik.elbow), wr = Ld(ik.wrist);
			out->elbow = e;
			out->wrist = wr;
			out->clamped = ik.clamped;
			const V u = Norm(e - s), f = Norm(wr - e, u);
			// The crease (each arm bone's +Z, perpendicularised) toward where the forearm bends; a straight arm: away
			// from the elbow's pole.
			V crease = f - u * Dot(f, u);
			if (Len(crease) < 1e-3)
			{
				crease = pole - u * Dot(pole, u);
				crease = Norm(crease, AnyPerp(u)) * -1.0;
			}
			double(&r_up)[3][3] = out->r_up;
			double(&r_fore)[3][3] = out->r_fore;
			double(&r_hand)[3][3] = out->r_hand;
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
			FrameFromPairs({sigma, 0.0, 0.0}, {0.0, 0.0, 1.0}, hf, hu, r_hand);
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
			return true;
		}
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
			const V hf = Norm(Ld(tg.hand_fwd[a]), {0.0, 0.0, 1.0});
			V tw = Ld(tg.hand_pos[a]) - hf * tu.wrist_back_cm;
			// Not into the torso (a box round the chest, chest frame): pushed out in front.
			if (std::fabs(tw.x) < 14.0 && tw.y > -45.0 && tw.y < 24.0 && tw.z > -12.0 && tw.z < 14.0)
				tw.z = 14.0;
			const V pole = V{0.0, -1.0, 0.0} + V{sigma, 0.0, 0.0} * 0.6 + V{0.0, 0.0, -1.0} * 0.4;
			ArmChain ac;
			if (!SolveArmChain(s, c1, c2, sigma, tw, pole, hf, Ld(tg.hand_up[a]), tu, &ac))
				continue;
			out->wrist_err[a] = Len(ac.wrist - Ld(tg.hand_pos[a]) + hf * tu.wrist_back_cm);
			out->reach_clamped[a] = ac.clamped;
			double r_up[3][3], r_fore[3][3], r_hand[3][3];
			std::memcpy(r_up, ac.r_up, sizeof(r_up));
			std::memcpy(r_fore, ac.r_fore, sizeof(r_fore));
			std::memcpy(r_hand, ac.r_hand, sizeof(r_hand));
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
	double CalibrateScale(double eye_over_floor_m, const AnchorTuning& tu)
	{
		if (!std::isfinite(eye_over_floor_m) || eye_over_floor_m < 0.9 || eye_over_floor_m > 1.5 || !(tu.seated_eye_m > tu.seat_m + 0.1))
			return 1.0;
		return Clamp((eye_over_floor_m - tu.seat_m) / (tu.seated_eye_m - tu.seat_m), tu.scale_min, tu.scale_max);
	}

	bool AnchorShoulders(const double head_pos[3], const double head_fwd[3], const double head_up[3], const bool hand[2],
		const double hand_pos[2][3], double scale, double half_width_m, double dt, const AnchorTuning& tu, double* yaw_state,
		AnchorBody* out)
	{
		const V hp = Ld(head_pos);
		const V f = Norm(Ld(head_fwd), {0.0, 0.0, -1.0}), u0 = Ld(head_up);
		const V u = Norm(u0 - f * Dot(u0, f), AnyPerp(f));
		if (!Finite(hp) || !Finite(f) || !Finite(u) || !std::isfinite(scale) || !(scale > 0.0) || !std::isfinite(half_width_m))
			return false;
		const double k = scale;
		// The head's yaw: its forward on the floor; looking far up or down, its up tells where the face points.
		V fh{f.x, 0.0, f.z};
		if (Len(fh) < 0.3)
			fh = fh + V{u.x, 0.0, u.z} * (f.y < 0.0 ? 1.0 : -1.0);
		fh = Norm(fh, {0.0, 0.0, -1.0});
		const double head_yaw = std::atan2(-fh.x, -fh.z);
		// The neck's pivot under and behind the eye, in the head's frame: it stays put as the head turns and nods.
		const V pivot = hp - u * (tu.pivot_down_m * k) - f * (tu.pivot_back_m * k);
		// The body's yaw: the head's, turned toward the tracked hands' midpoint when that is ahead of the pivot.
		double target = head_yaw;
		bool used = false;
		{
			V mid{};
			int nh = 0;
			for (int h = 0; h < 2; h++)
				if (hand[h] && Finite(Ld(hand_pos[h])))
				{
					mid = mid + Ld(hand_pos[h]);
					nh++;
				}
			if (nh > 0)
			{
				const V d = mid * (1.0 / nh) - pivot;
				const V dh{d.x, 0.0, d.z};
				if (Len(dh) > 0.15 && Dot(Norm(dh), fh) > -0.3)
				{
					const double hy = std::atan2(-dh.x, -dh.z);
					target = head_yaw + tu.hands_share * std::remainder(hy - head_yaw, 2.0 * kPi);
					used = true;
				}
			}
		}
		const double lim = tu.max_body_yaw_deg * kDeg;
		double yaw = target;
		if (yaw_state && std::isfinite(*yaw_state))
		{
			const double a = 1.0 - std::exp(-std::max(0.0, tu.yaw_rate_per_s) * Clamp(dt, 0.0, 0.5));
			yaw = *yaw_state + a * std::remainder(target - *yaw_state, 2.0 * kPi);
		}
		// Never further from the head than the neck allows.
		yaw = std::remainder(head_yaw + Clamp(std::remainder(yaw - head_yaw, 2.0 * kPi), -lim, lim), 2.0 * kPi);
		if (yaw_state)
			*yaw_state = yaw;
		const V bf{-std::sin(yaw), 0.0, -std::cos(yaw)}, br{std::cos(yaw), 0.0, -std::sin(yaw)};
		// The shoulders: their midpoint under the pivot (the body upright), a little behind it, the model's own half width
		// each side.
		const V mid = pivot + V{0.0, -tu.shoulder_down_m * k, 0.0} - bf * (tu.shoulder_back_m * k);
		St(pivot, out->pivot);
		St(bf, out->fwd);
		St(br, out->right);
		St(mid - br * half_width_m, out->shoulder[0]);
		St(mid + br * half_width_m, out->shoulder[1]);
		out->yaw = yaw;
		out->hands_used = used;
		return true;
	}

	bool SelfHidePlan(const BoneMap& m, const int* parents, int n, SelfStyle style, HidePlan* out)
	{
		*out = HidePlan{};
		if (style == SelfStyle::GameShoulders || !CheckMap(m, parents, n, nullptr))
			return false;
		const auto add = [out](int b, HideKind k, int arm) {
			if (out->count >= kMaxOut)
				return;
			for (int i = 0; i < out->count; i++)
				if (out->bone[i] == b)
					return;
			out->bone[out->count] = b;
			out->kind[out->count] = k;
			out->arm[out->count] = arm;
			out->count++;
		};
		// The drawn bones first: per arm the upper arm (Arms) or the forearm (Forearms) and everything under it.
		for (int a = 0; a < 2; a++)
		{
			int sub[kMaxBones];
			const int top = (style == SelfStyle::Arms) ? m.arm[a].upper : m.arm[a].fore;
			const int k = Subtree(parents, n, top, sub, kMaxBones);
			for (int i = 0; i < k; i++)
				add(sub[i], HideKind::Drawn, a);
		}
		// Then the hidden ones next to them: the upper arms (Forearms), the clavicles, the head's subtree.
		if (style == SelfStyle::Forearms)
			for (int a = 0; a < 2; a++)
				add(m.arm[a].upper, HideKind::AtElbow, a);
		for (int a = 0; a < 2; a++)
			add(m.arm[a].clavicle, style == SelfStyle::Arms ? HideKind::AtShoulder : HideKind::AtChest, a);
		add(m.chest, HideKind::AtChest, -1);
		int sub[kMaxBones];
		const int k = Subtree(parents, n, m.neck, sub, kMaxBones);
		for (int i = 0; i < k; i++)
			add(sub[i], HideKind::AtChest, -1);
		return out->count > 0;
	}

	bool SolveAnchored(const BoneMap& m, const Frame* locals, const int* parents, int n, const AnchorTargets& at,
		const Tuning& tu, SelfStyle style, double protract, Pose* out)
	{
		*out = Pose{};
		HidePlan plan;
		if (!SelfHidePlan(m, parents, n, style, &plan))
			return false;
		const V up = Norm(Ld(at.body_up), {0.0, 1.0, 0.0});
		const V fwd = Norm(Ld(at.body_fwd) - up * Dot(Ld(at.body_fwd), up), AnyPerp(up));
		const V left = Cross(up, fwd); // the model's +X side (up x forward)
		Frame now[kMaxBones];
		V elbow[2], shoulder[2];
		for (int a = 0; a < 2; a++)
		{
			if (!at.tg.hand[a])
				continue;
			const ArmMap& am = m.arm[a];
			const double sigma = (locals[am.upper].t[0] >= 0.0) ? 1.0 : -1.0;
			const V c1 = Ld(locals[am.fore].t), c2 = Ld(locals[am.wrist].t);
			const V hf = Norm(Ld(at.tg.hand_fwd[a]), {0.0, 0.0, 1.0});
			const V tw = Ld(at.tg.hand_pos[a]) - hf * tu.wrist_back_cm;
			V s = Ld(at.shoulder[a]);
			if (!Finite(s) || !Finite(tw))
				continue;
			// Out of reach: the shoulder slides toward the hand (a real one reaches forward too), then the arm stops short.
			const double reach = 0.999 * (Len(c1) + Len(c2)), d = Len(tw - s);
			if (d > reach && std::isfinite(protract) && protract > 0.0)
				s = s + Norm(tw - s) * std::min(d - reach, protract);
			const V pole = up * -1.0 + left * (sigma * 0.6) - fwd * 0.4;
			ArmChain ac;
			if (!SolveArmChain(s, c1, c2, sigma, tw, pole, hf, Ld(at.tg.hand_up[a]), tu, &ac))
				continue;
			std::memcpy(now[am.upper].r, ac.r_up, sizeof(ac.r_up));
			St(s, now[am.upper].t);
			std::memcpy(now[am.fore].r, ac.r_fore, sizeof(ac.r_fore));
			St(MulRow(c1, now[am.upper].r) + s, now[am.fore].t);
			std::memcpy(now[am.wrist].r, ac.r_hand, sizeof(ac.r_hand));
			St(MulRow(c2, now[am.fore].r) + Ld(now[am.fore].t), now[am.wrist].t);
			int sub[kMaxBones];
			const int k = Subtree(parents, n, am.wrist, sub, kMaxBones);
			for (int i = 1; i < k; i++)
				now[sub[i]] = Compose(locals[sub[i]], now[parents[sub[i]]]);
			out->drawn[a] = true;
			elbow[a] = Ld(now[am.fore].t);
			shoulder[a] = s;
			St(elbow[a], out->elbow[a]);
			St(s, out->shoulder[a]);
			St(Ld(now[am.wrist].t), out->wrist[a]);
			out->wrist_err[a] = Len(Ld(now[am.wrist].t) - tw);
			out->reach_clamped[a] = ac.clamped;
		}
		if (!out->drawn[0] && !out->drawn[1])
			return false;
		const auto hidden_at = [](const V& p) {
			Frame f;
			std::memset(f.r, 0, sizeof(f.r));
			St(p, f.t);
			return f;
		};
		for (int i = 0; i < plan.count; i++)
		{
			const int b = plan.bone[i], a = plan.arm[i];
			const bool arm_drawn = a >= 0 && out->drawn[a];
			Frame f = hidden_at(Ld(at.chest)); // the chest's own joint
			bool keep = false;
			switch (plan.kind[i])
			{
				case HideKind::Drawn:
					if (arm_drawn)
					{
						f = now[b];
						keep = true;
					}
					break;
				case HideKind::AtElbow:
					if (arm_drawn)
						f = hidden_at(elbow[a]);
					break;
				case HideKind::AtShoulder:
					if (arm_drawn)
						f = hidden_at(shoulder[a]);
					break;
				case HideKind::AtChest:
					break;
			}
			out->bone[out->count] = b;
			out->rel[out->count] = f;
			out->keep[out->count] = keep;
			out->count++;
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

	bool MayClear(u32 crc_now, u32 crc_written, u32 block_word0)
	{
		return crc_now == crc_written && block_word0 == kBlockMagic;
	}

	bool UseLocals(bool offline, bool eth_enabled, bool drawtime)
	{
		return offline && !eth_enabled && !drawtime;
	}

	u8 SlotFlags(int si, bool locals, bool arms, bool native_weapon)
	{
		const u8 self = (si == 1) ? static_cast<u8>(kFlagSelf | kFlagCollapse) : static_cast<u8>(0);
		if (locals)
			return static_cast<u8>(kFlagLocals | self);
		if (si == 1 && native_weapon)
			return static_cast<u8>(self | kFlagAttach | kFlagWeapon); // the held items on the posed hands, and drawn (cave W)
		return (si == 1) ? self : static_cast<u8>(arms ? kFlagAttach : 0);
	}

	// --- NativeHands ---------------------------------------------------------------------------------------------------
	namespace
	{
		constexpr double kSelfWristSwingDeg = 90.0; // your own anchored hand's wrist (the weapon follows it)
	} // namespace

	bool NativeHoldStep(NativeHold* h, int hand, bool raw, u64 now_ms, u64 hold_ms)
	{
		if (!h || hand < 0 || hand > 1)
			return false;
		if (!raw)
		{
			// The pose dropped out: the overlay hand is back this very frame.
			h->on[hand] = false;
			h->since_ms[hand] = 0;
			return false;
		}
		if (h->since_ms[hand] == 0)
			h->since_ms[hand] = now_ms ? now_ms : 1;
		if (!h->on[hand] && now_ms >= h->since_ms[hand] && now_ms - h->since_ms[hand] >= hold_ms)
			h->on[hand] = true;
		return h->on[hand];
	}

	namespace
	{
		std::mutex s_native_mutex;
		bool s_native_raw[2] = {false, false};
		bool s_native_raw_weapon = false;
		u64 s_native_raw_ms = 0;
		NativeHold s_native_hold;
		bool s_native_weapon_decided = false;
		std::atomic<bool> s_native_live[2] = {false, false};
	} // namespace

	void NativeHandsPublish(const bool hand[2], bool weapon, u64 now_ms)
	{
		std::lock_guard<std::mutex> lock(s_native_mutex);
		s_native_raw[0] = hand[0];
		s_native_raw[1] = hand[1];
		s_native_raw_weapon = weapon;
		s_native_raw_ms = now_ms;
	}

	bool NativeHandsUpdate(u64 now_ms, bool gate_ok, bool out_hand[2], bool* out_weapon)
	{
		std::lock_guard<std::mutex> lock(s_native_mutex);
		const bool fresh = s_native_raw_ms != 0 && now_ms >= s_native_raw_ms && now_ms - s_native_raw_ms <= kNativeFreshMs;
		bool any = false;
		for (int h = 0; h < 2; h++)
		{
			const bool was = s_native_hold.on[h];
			const bool now = NativeHoldStep(&s_native_hold, h, fresh && gate_ok && s_native_raw[h], now_ms, kNativeHoldMs);
			if (now != was)
				Console.WriteLn("(VR) NativeHands: the %s hand is %s.", h == 0 ? "left" : "right",
					now ? "the game's own (the overlay hand is no longer submitted)" : "the overlay hand again (the native pose dropped out)");
			s_native_live[h].store(now, std::memory_order_relaxed);
			if (out_hand)
				out_hand[h] = now;
			any |= now;
		}
		s_native_weapon_decided = s_native_hold.on[1] && s_native_raw_weapon;
		if (out_weapon)
			*out_weapon = s_native_weapon_decided;
		return any;
	}

	namespace
	{
		float s_seen_shift[3] = {};
		u64 s_seen_shift_ms = 0;
	} // namespace

	namespace
	{
		NativeWeapon s_native_weapon;
		u64 s_native_weapon_ms = 0;
	} // namespace

	bool NativeWeaponNow(NativeWeapon* out)
	{
		std::lock_guard<std::mutex> lock(s_native_mutex);
		const u64 now = CameraDriver::SteadyNowMs();
		if (s_native_weapon_ms == 0 || now < s_native_weapon_ms || now - s_native_weapon_ms > kNativeFreshMs || !s_native_live[1].load())
			return false;
		if (out)
			*out = s_native_weapon;
		return true;
	}

	bool NativeSeenShift(float out_g[3])
	{
		std::lock_guard<std::mutex> lock(s_native_mutex);
		const u64 now = CameraDriver::SteadyNowMs();
		if (s_seen_shift_ms == 0 || now < s_seen_shift_ms || now - s_seen_shift_ms > kNativeFreshMs)
			return false;
		for (int k = 0; k < 3; k++)
			out_g[k] = s_seen_shift[k];
		return true;
	}

	bool NativeHandsLive(int hand)
	{
		return hand >= 0 && hand <= 1 && s_native_live[hand].load(std::memory_order_relaxed);
	}

	AttachedWeapon AttachWeapon(const Frame& wrist, const double grip[3], double scale, const double euler[3], const double muzzle[3])
	{
		// The game's row-vector rotations (FUN_0018FAD0 / FB70 / FBD0 style): X, then Y, then Z.
		const auto rx = [](double a, double r[3][3]) {
			const double c = std::cos(a), s = std::sin(a);
			const double m[3][3] = {{1, 0, 0}, {0, c, s}, {0, -s, c}};
			std::memcpy(r, m, sizeof(m));
		};
		const auto ry = [](double a, double r[3][3]) {
			const double c = std::cos(a), s = std::sin(a);
			const double m[3][3] = {{c, 0, -s}, {0, 1, 0}, {s, 0, c}};
			std::memcpy(r, m, sizeof(m));
		};
		const auto rz = [](double a, double r[3][3]) {
			const double c = std::cos(a), s = std::sin(a);
			const double m[3][3] = {{c, s, 0}, {-s, c, 0}, {0, 0, 1}};
			std::memcpy(r, m, sizeof(m));
		};
		double X[3][3], Y[3][3], Z[3][3], A[3][3], W[3][3];
		rx(euler[0], X);
		ry(euler[1], Y);
		rz(euler[2], Z);
		MulRR(X, Y, A);
		MulRR(A, Z, A);
		MulRR(A, wrist.r, W); // the model's axes in the world frame
		AttachedWeapon out;
		const V g = Ld(grip) * scale;
		const V o = MulRow(g, wrist.r) + Ld(wrist.t);
		St(o, out.origin);
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				out.axes[i][j] = W[i][j];
		const V m = MulRow(MulRow(Ld(muzzle), A) + g, wrist.r) + Ld(wrist.t);
		St(m, out.muzzle);
		St(Row(W, 0) * -1.0, out.barrel);
		return out;
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
			double anchor_yaw = std::numeric_limits<double>::quiet_NaN(); // the anchored body's yaw (VrBodySelfStyle)
		};
		SlotState s_slot[kSlots];
		bool s_written = false;
		u32 s_written_block = 0, s_written_tables = 0;
		u32 s_written_crc = 0;        // the disc CRC the block was written under (night review M3)
		bool s_state_loaded = false;  // a state was loaded: a block reading our magic is the state's (night review m3)
		int s_why = -1;
		u32 s_stamp = 0;
		u64 s_log_ms = 0;

		bool InRam(u32 a, u32 len) { return a >= 0x100000u && a + len <= Ps2MemSize::MainRam && a + len > a; }

		void ReadF16(u32 a, float m[16])
		{
			for (int k = 0; k < 16; k++)
				m[k] = std::bit_cast<float>(memRead32(a + 4u * static_cast<u32>(k)));
		}

		// Zeroes the block and its tables, only while they are this game's and ours (night review M3): a VM, the disc CRC
		// they were written under, and our magic still in the block. Never in another game, the BIOS or an ELF loader.
		void ClearBlock(u32 block, u32 tables, u32 crc)
		{
			if (!VMManager::HasValidVM() || !MayClear(VMManager::GetDiscCRC(), crc, memRead32(block)))
				return;
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
			int root = -1;      // the chain's root bone and its world frame (the anchored arms' reference)
			Frame root_w;
			double root_scale = 1.0;
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
				if (i == len - 1)
				{
					sk->root = chain[i];
					if (!FromGameScaled(w, &sk->root_w, &sk->root_scale))
						sk->root = -1;
				}
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

		// Your own anchored arms (VrBodySelfStyle Forearms / Arms): the seated calibration, latched at VR start and on every
		// recentre (the camera mapping's reference changing): the eye's height over the floor then gives the body's scale.
		struct Calibration
		{
			bool have = false;
			float ref[3] = {};
			float ref_yaw = 0.0f;
			double eye_floor = std::numeric_limits<double>::quiet_NaN();
			double scale = 1.0;
		};
		Calibration s_cal;

		// What the log prints of the anchored solve (game coordinates).
		struct AnchorLog
		{
			double eye[3] = {};
			double view_fwd[3] = {0.0, 0.0, 1.0}; // level
			double body_off_deg = 0.0;            // the body's yaw off the head's
			bool hands_used = false;
		};

		// The VR point p as the eye sees it, in the game: GameFromVr, then eye_forward along the view's yaw (the game's eye is
		// that far ahead of GameFromVr(head), so everything the eye sees is drawn that much further on; CameraDriver.h).
		bool SeenFromVr(const CameraDriver::Mapping& m, const double shift[3], const double p[3], double g[3])
		{
			const float pf[3] = {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
			float gf[3];
			if (!CameraDriver::GameFromVr(m, pf, gf))
				return false;
			for (int k = 0; k < 3; k++)
				g[k] = gf[k] + shift[k];
			return true;
		}

		bool MakeAnchorTargets(const CameraDriver::Mapping& m, const Skeleton& sk, const BoneMap& map, const VRInputSnapshot& snap,
			int left_arm, double dt, double* yaw_state, AnchorTargets* at, AnchorLog* lg)
		{
			if (!snap.head_pose.valid || !(std::fabs(m.units_per_meter) > 1e-3f))
				return false;
			// The calibration, at VR start and on each recentre.
			const bool moved = !s_cal.have || std::fabs(s_cal.ref[0] - m.ref_pos[0]) > 1e-3f || std::fabs(s_cal.ref[1] - m.ref_pos[1]) > 1e-3f ||
							   std::fabs(s_cal.ref[2] - m.ref_pos[2]) > 1e-3f || std::fabs(s_cal.ref_yaw - m.ref_yaw) > 1e-3f;
			const AnchorTuning atu;
			if (moved)
			{
				s_cal.have = true;
				std::memcpy(s_cal.ref, m.ref_pos, sizeof(s_cal.ref));
				s_cal.ref_yaw = m.ref_yaw;
				s_cal.eye_floor = snap.head_floor_y;
				s_cal.scale = CalibrateScale(s_cal.eye_floor, atu);
				*yaw_state = std::numeric_limits<double>::quiet_NaN();
				if (std::isfinite(s_cal.eye_floor))
					Console.WriteLn("(VR) Body: self calibrated at the recentre: eye %.2f m over the floor, body scale %.2f%s.", s_cal.eye_floor,
						s_cal.scale, s_cal.scale == 1.0 && (s_cal.eye_floor < 0.9 || s_cal.eye_floor > 1.5) ? " (not a seated eye height)" : "");
				else
					Console.WriteLn("(VR) Body: self calibrated at the recentre: the floor is unknown, body scale 1.00.");
			}
			const auto& hq = snap.head_pose.orientation_xyzw;
			const double qx = hq[0], qy = hq[1], qz = hq[2], qw = hq[3];
			const auto rot = [&](double vx, double vy, double vz, double o[3]) {
				// v + 2 q x (q x v + w v)
				const double tx = qy * vz - qz * vy + qw * vx, ty = qz * vx - qx * vz + qw * vy, tz = qx * vy - qy * vx + qw * vz;
				o[0] = vx + 2.0 * (qy * tz - qz * ty);
				o[1] = vy + 2.0 * (qz * tx - qx * tz);
				o[2] = vz + 2.0 * (qx * ty - qy * tx);
			};
			double hp[3], hf[3], hu[3];
			for (int k = 0; k < 3; k++)
				hp[k] = snap.head_pose.position_xyz[k];
			rot(0.0, 0.0, -1.0, hf);
			rot(0.0, 1.0, 0.0, hu);
			bool hand[2] = {};
			double hand_vr[2][3] = {};
			for (int h = 0; h < 2; h++)
			{
				const VRHandState& hs = snap.hands[h];
				hand[h] = hs.grip_pose.valid;
				for (int k = 0; k < 3; k++)
					hand_vr[h][k] = hs.grip_pose.position_xyz[k];
			}
			// The shoulders' half width: the model's own (clavicle and shoulder joint offsets), in metres.
			const double half_units = 0.5 * (std::fabs(sk.locals[map.arm[0].clavicle].t[0] + sk.locals[map.arm[0].upper].t[0]) +
												std::fabs(sk.locals[map.arm[1].clavicle].t[0] + sk.locals[map.arm[1].upper].t[0]));
			const double half_m = half_units * sk.scale / std::fabs(m.units_per_meter);
			AnchorBody body;
			if (!AnchorShoulders(hp, hf, hu, hand, hand_vr, s_cal.scale, half_m, dt, atu, yaw_state, &body))
				return false;
			// The eye's shift (eye_forward along the view's level forward, in the game).
			double shift[3] = {};
			double g0[3], g1[3];
			{
				double fl[3] = {hf[0], 0.0, hf[2]};
				const double l = std::sqrt(fl[0] * fl[0] + fl[2] * fl[2]);
				if (l < 1e-3)
				{
					fl[0] = body.fwd[0];
					fl[2] = body.fwd[2];
				}
				else
				{
					fl[0] /= l;
					fl[2] /= l;
				}
				const double p1[3] = {hp[0] + fl[0], hp[1], hp[2] + fl[2]};
				if (!SeenFromVr(m, shift, hp, g0) || !SeenFromVr(m, shift, p1, g1))
					return false;
				double d[3] = {g1[0] - g0[0], 0.0, g1[2] - g0[2]};
				const double dl = std::sqrt(d[0] * d[0] + d[2] * d[2]);
				if (!(dl > 1e-6))
					return false;
				for (int k = 0; k < 3; k++)
				{
					lg->view_fwd[k] = d[k] / dl;
					shift[k] = lg->view_fwd[k] * m.eye_forward;
					lg->eye[k] = g0[k] + shift[k];
				}
			}
			lg->hands_used = body.hands_used;
			{
				double hy = std::atan2(-hf[0], -hf[2]);
				lg->body_off_deg = std::remainder(body.yaw - hy, 2.0 * kPi) / kDeg;
			}
			// Everything into the chest's frame.
			for (int side = 0; side < 2; side++)
			{
				const int a = (side == 0) ? left_arm : 1 - left_arm;
				double g[3];
				if (!SeenFromVr(m, shift, body.shoulder[side], g))
					return false;
				const float gf[3] = {static_cast<float>(g[0]), static_cast<float>(g[1]), static_cast<float>(g[2])};
				PointToChest(sk, gf, at->shoulder[a]);
				if (!hand[side])
					continue;
				const VRHandState& hs = snap.hands[side];
				const VRPose& orient = hs.aim_pose.valid ? hs.aim_pose : hs.grip_pose;
				Presence::GamePose gp;
				if (!Presence::PoseToGame(m, hs.grip_pose.position_xyz.data(), orient.orientation_xyzw.data(), &gp))
					continue;
				for (int k = 0; k < 3; k++)
					gp.pos[k] = static_cast<float>(gp.pos[k] + shift[k]);
				at->tg.hand[a] = true;
				PointToChest(sk, gp.pos, at->tg.hand_pos[a]);
				DirToChest(sk, gp.fwd, at->tg.hand_fwd[a]);
				DirToChest(sk, gp.up, at->tg.hand_up[a]);
			}
			{
				const double p1[3] = {body.pivot[0] + body.fwd[0], body.pivot[1], body.pivot[2] + body.fwd[2]};
				double a0[3], a1[3];
				if (!SeenFromVr(m, shift, body.pivot, a0) || !SeenFromVr(m, shift, p1, a1))
					return false;
				const float bf[3] = {static_cast<float>(a1[0] - a0[0]), static_cast<float>(a1[1] - a0[1]), static_cast<float>(a1[2] - a0[2])};
				const float bu[3] = {0.0f, 1.0f, 0.0f};
				DirToChest(sk, bf, at->body_fwd);
				DirToChest(sk, bu, at->body_up);
			}
			return at->tg.hand[0] || at->tg.hand[1];
		}

		// A game point against the eye, in the view's level frame: cm up, forward, right.
		void FromEye(const AnchorLog& lg, const double w[3], double o[3])
		{
			const double d[3] = {w[0] - lg.eye[0], w[1] - lg.eye[1], w[2] - lg.eye[2]};
			const double* f = lg.view_fwd;
			const double right[3] = {-f[2], 0.0, f[0]}; // up x forward is the model's left
			o[0] = d[1];
			o[1] = d[0] * f[0] + d[2] * f[2];
			o[2] = d[0] * right[0] + d[2] * right[2];
		}

		// The distance from p to the segment a-b.
		double SegDist(const double p[3], const double a[3], const double b[3])
		{
			double ab[3], ap[3];
			for (int k = 0; k < 3; k++)
			{
				ab[k] = b[k] - a[k];
				ap[k] = p[k] - a[k];
			}
			const double l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
			const double t = l2 > 1e-12 ? std::clamp((ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l2, 0.0, 1.0) : 0.0;
			double q[3];
			for (int k = 0; k < 3; k++)
				q[k] = a[k] + ab[k] * t - p[k];
			return std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
		}
	} // namespace

	void Shutdown()
	{
		// VM shutdown, reset or ELF change (night review M3): forget the block, never write (the RAM is going, or is the
		// next program's).
		s_written = false;
		s_written_block = s_written_tables = s_written_crc = 0;
		s_state_loaded = false;
		for (SlotState& st : s_slot)
			st = SlotState{};
		s_cal = Calibration{};
	}

	void OnStateLoaded()
	{
		s_state_loaded = true;
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
			// NativeHands: nothing is drawn by the game any more, the overlay hands are back at once.
			const bool none[2] = {false, false};
			NativeHandsPublish(none, false, CameraDriver::SteadyNowMs());
			NativeHandsUpdate(CameraDriver::SteadyNowMs(), false, nullptr, nullptr);
			if (s_written)
				ClearBlock(s_written_block, s_written_tables, s_written_crc);
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
		if (s_written && (s_written_block != bp->block || s_written_tables != bp->tables || s_written_crc != VMManager::GetDiscCRC()))
			clear();
		// A loaded state's block (night review m3): its slots name records posed in another session; zeroed while it reads
		// our magic, so the caves stop applying its stale matrices, then posed afresh.
		if (s_state_loaded)
		{
			s_state_loaded = false;
			if (memRead32(bp->block) == kBlockMagic)
			{
				s_written = true;
				s_written_block = bp->block;
				s_written_tables = bp->tables;
				s_written_crc = VMManager::GetDiscCRC();
				clear();
				Console.WriteLn("(VR) Body: a loaded state's block cleared.");
			}
		}
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
		const bool use_locals = UseLocals(CameraDriver::NetworkOffline(), NetGate::IsOnline(), s_drawtime);
		// Your own arms (VrBodySelfStyle): anchored to the headset and drawn at the draw only (Forearms, Arms), or from the
		// game's shoulders as the first version (GameShoulders).
		const SelfStyle style = static_cast<SelfStyle>(static_cast<int>(vr.VrBodySelfStyle));
		const bool anchored = style == SelfStyle::Forearms || style == SelfStyle::Arms;
		const u64 now = CameraDriver::SteadyNowMs();
		const bool log_now = (s_log_env || s_loopback_env) && now - s_log_ms >= 2000;
		if (log_now)
			s_log_ms = now;

		const Tuning tu;
		BlockSlot slots[kSlots];
		static float tables[kSlots][kMaxOut][16];
		// NativeHands: per VR hand, whether the game draws the hand this vsync (set below for the local anchored slot).
		const bool game_overlay_now = memRead32(0x540004) == 3 && memRead32(0x6313A0) == 0x27BDFF50u;
		const bool native_sites = vr.NativeHands && anchored && game_overlay_now && bp->self_site != 0 && bp->weapon_site != 0 &&
		                          memRead32(bp->self_site) == bp->self_site_on && memRead32(bp->weapon_site) == bp->weapon_site_on;
		bool native_hand[2] = {false, false};
		bool native_weapon_raw = false;
		for (int si = 0; si < kSlots; si++)
		{
			SlotState& st = s_slot[si];
			const double dt = (st.last_ms != 0 && now > st.last_ms) ? (now - st.last_ms) / 1000.0 : 0.0;
			st.last_ms = now;
			Input in;
			u32 rec = 0;
			int state = 0;
			VRInputSnapshot self_snap;
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
				self_snap = snap;
				if (!vr.VrBodySelf)
					state = 1;
				else if (!CameraDriver::NetworkOffline() || !have_mapping || !CameraDriver::LocalRecord(&base))
					state = 2;
				else if (snap.generation == 0 || !snap.actions_active)
					state = 3;
				else if (anchored && !snap.head_pose.valid)
					state = 6;
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
			const bool self_anchored = si == 1 && anchored;
			AnchorLog alg;
			Skeleton skr; // the anchored arms' frame: the skeleton with the root's world as its reference
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
				else if (self_anchored)
				{
					// Everything in the root bone's frame (the character's place and heading, none of the chest's sway): the
					// cave multiplies by the root's world of the frame it draws.
					AnchorTargets at;
					skr = sk;
					skr.chest = sk.root_w;
					skr.scale = sk.root_scale;
					if (sk.root < 0 || sk.root == map.chest)
						state = 4;
					else if (!MakeAnchorTargets(mapping, skr, map, self_snap, left_arm, dt, &st.anchor_yaw, &at, &alg))
						state = 6;
					else
					{
						{
							// The eye's shift (NativeSeenShift): eye_forward along the view's level forward.
							std::lock_guard<std::mutex> lock(s_native_mutex);
							for (int k = 0; k < 3; k++)
								s_seen_shift[k] = static_cast<float>(alg.view_fwd[k] * mapping.eye_forward);
							s_seen_shift_ms = CameraDriver::SteadyNowMs();
						}
						const AnchorTuning atu;
						const float cj[3] = {static_cast<float>(sk.chest.t[0]), static_cast<float>(sk.chest.t[1]), static_cast<float>(sk.chest.t[2])};
						PointToChest(skr, cj, at.chest);
						tg = at.tg;
						const double protract = atu.protract_m * std::fabs(mapping.units_per_meter) / sk.scale;
						// NativeHands: the weapon is the game's and follows this hand, so the wrist may flex further than the partner's
						// (a gun held at the face needs about 81 deg; at 75 the drawn gun was 5.7 deg and 2.5 cm off the controller).
						Tuning tu_self = tu;
						tu_self.wrist_swing_deg = std::max(tu.wrist_swing_deg, kSelfWristSwingDeg);
						solved = SolveAnchored(map, sk.locals, sk.parents, sk.n, at, tu_self, style, protract, &pose) && pose.count > 0;
						if (!solved)
							state = 5;
						st.w = Weights{};
						for (int a = 0; a < 2; a++)
							st.w.arm[a] = pose.drawn[a] ? 1.0 : 0.0;
					}
				}
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
					"no VR hands", "skeleton not usable", "nothing to pose (no tracked part in reach)",
					"no VR head (the anchored arms start from it)"};
				Console.WriteLn("(VR) Body: %s %s%s%s (record %08X).", si == 0 ? "partner" : "self", kState[state], why ? ": " : "",
					why ? why : "", rec);
			}
			if (!solved)
				continue;
			BlockSlot& bs = slots[si];
			bs.record = rec;
			bs.table = bp->tables + static_cast<u32>(si) * ProfileDB::kBodyTableBytes;
			bs.count = 0;
			bs.ref = static_cast<u8>(self_anchored ? sk.root : map.chest);
			const bool arms = st.w.arm[0] > 0.0 || st.w.arm[1] > 0.0;
			// The anchored arms are draw-only, offline or not: cave V's world entries; never cave U's locals, so the game's own
			// arms, held items and hit spheres stay its animation's.
			// NativeHands: the right VR hand is the model's arm 1 (File #1: left_arm 0), where the game holds the weapon (bone 19).
			const bool native = self_anchored && native_sites && left_arm == 0 && pose.drawn[1];
			bs.flags = self_anchored ? SlotFlags(si, false, arms, native) : SlotFlags(si, use_locals, arms);
			if (self_anchored && native_sites)
			{
				// The draw-time attach (flag 1) only for a hand the game draws: the item's matrix follows the posed wrist.
				native_weapon_raw = native;
				for (int vh = 0; vh < 2; vh++)
					native_hand[vh] = pose.drawn[vh == 0 ? left_arm : 1 - left_arm];
			}
			// The held items go back on the hands (the game's own FUN_006313A0 with the record's item slots, as its update
			// does: the item slot byte +0xC7C on the -X wrist, +0xC7D on the +X wrist).
			bs.attach_offset[0] = 0xC7C;
			bs.attach_bone[0] = static_cast<u8>(map.arm[1].wrist);
			bs.attach_offset[1] = 0xC7D;
			bs.attach_bone[1] = static_cast<u8>(map.arm[0].wrist);
			if (self_anchored && !native)
			{
				// Draw-only without NativeHands: the game's held items stay its animation's (no attach).
				bs.attach_offset[0] = 0;
				bs.attach_offset[1] = 0;
			}
			else if (self_anchored)
			{
				if (!pose.drawn[1])
					bs.attach_offset[0] = 0;
				if (!pose.drawn[0])
					bs.attach_offset[1] = 0;
			}
			if (self_anchored)
			{
				for (int k = 0; k < pose.count; k++)
				{
					bs.bones[k] = static_cast<u8>(pose.bone[k]);
					ToGame(pose.rel[k], tables[si][k]);
					if (pose.keep[k])
						bs.keep[pose.bone[k] / 32] |= 1u << (pose.bone[k] % 32);
				}
				bs.count = static_cast<u8>(pose.count);
			}
			for (int k = 0; k < pose.count && !self_anchored; k++)
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
			if (!use_locals && !self_anchored)
				bs.count = static_cast<u8>(pose.count);
			else if (!self_anchored)
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
			// NativeHands: the weapon as the game will draw it this vsync (the solved wrist, the game's attach entry), published for the
			// gameplay side (the gun ray, the melee shape) and the laser.
			if (self_anchored && native)
			{
				bool published = false;
				int wk = -1;
				for (int k = 0; k < pose.count; k++)
					if (pose.bone[k] == map.arm[1].wrist)
						wk = k;
				const u32 item = memRead8(rec + 0xC7C);
				const u32 type = memRead16(rec + 0xCA8);
				if (wk >= 0 && item != 0 && type < 0x100)
				{
					const u32 ent = 0x6E5940u + 0x44u * type;
					const u32 var = memRead8(rec + 0xB99) & 1u;
					double grd[3], mzd[3], eu[3];
					for (int k = 0; k < 3; k++)
					{
						mzd[k] = std::bit_cast<float>(memRead32(ent + 0x38 + 4 * k));
						grd[k] = std::bit_cast<float>(memRead32(ent + 0x10 + 12 * var + 4 * k));
						eu[k] = static_cast<s16>(memRead16(ent + 0x28 + 8 * var + 2 * k)) * (2.0 * kPi / 65536.0);
					}
					Frame wf;
					MulRR(pose.rel[wk].r, skr.chest.r, wf.r);
					ChestToWorld(skr, pose.rel[wk].t, wf.t);
					const AttachedWeapon aw = AttachWeapon(wf, grd, sk.scale, eu, mzd);
					NativeWeapon nw;
					for (int k = 0; k < 3; k++)
					{
						nw.origin[k] = aw.origin[k];
						nw.muzzle[k] = aw.muzzle[k];
						nw.barrel[k] = aw.barrel[k];
						for (int j = 0; j < 3; j++)
							nw.axes[k][j] = aw.axes[k][j];
					}
					published = Finite(Ld(nw.origin)) && Finite(Ld(nw.muzzle)) && Finite(Ld(nw.barrel));
					// VR space for the laser: the drawn point is GameFromVr(v) + shift, so v = VrFromGame(g - shift).
					float sh[3];
					if (published && NativeSeenShift(sh))
					{
						const auto to_vr = [&](const double g[3], float v[3]) {
							const float gf[3] = {static_cast<float>(g[0] - sh[0]), static_cast<float>(g[1] - sh[1]), static_cast<float>(g[2] - sh[2])};
							return CameraDriver::VrFromGame(mapping, gf, v);
						};
						float v0[3], v1[3];
						double ahead[3];
						for (int k = 0; k < 3; k++)
							ahead[k] = aw.muzzle[k] + aw.barrel[k] * 100.0;
						if (to_vr(aw.muzzle, v0) && to_vr(ahead, v1))
						{
							const V d = Norm(V{static_cast<double>(v1[0] - v0[0]), static_cast<double>(v1[1] - v0[1]), static_cast<double>(v1[2] - v0[2])});
							for (int k = 0; k < 3; k++)
								nw.vr_muzzle[k] = v0[k];
							St(d, nw.vr_barrel);
							nw.vr_valid = true;
						}
					}
				if (published)
					{
						std::lock_guard<std::mutex> lock(s_native_mutex);
						s_native_weapon = nw;
						s_native_weapon_ms = CameraDriver::SteadyNowMs();
					}
				}
				if (!published)
				{
					std::lock_guard<std::mutex> lock(s_native_mutex);
					s_native_weapon_ms = 0;
				}
			}
			// NativeHands (PCSX2_VR_NATIVE_LOG=1, every 0.3 s): where the game DREW the held weapon (its object's matrix, read at
			// this vsync, after the draw cave V attached it to the posed wrist) against the controller, the attach maths and the
			// gun ray's muzzle (the gameplay side).
			static const bool s_nat_env = [] {
				const char* e = std::getenv("PCSX2_VR_NATIVE_LOG");
				return e && e[0] == '1';
			}();
			static u64 s_nat_log_ms = 0;
			if (s_nat_env && self_anchored && native && now - s_nat_log_ms >= 300)
			{
				s_nat_log_ms = now;
				const u32 item = memRead8(rec + 0xC7C);
				const u32 obj = item ? memRead32(0x3967EC + 60u * item) : 0u;
				const u32 type = memRead16(rec + 0xCA8);
				const u32 ent = 0x6E5940u + 0x44u * type;
				if (obj && InRam(obj, 0x210) && type < 0x100 && sk.ptr[map.arm[1].wrist] != 0)
				{
					float M[16], W[16], mz[3], gr[3];
					ReadF16(obj + 0x1C0, M);
					ReadF16(sk.ptr[map.arm[1].wrist], W);
					const u32 var = memRead8(rec + 0xB99) & 1u;
					for (int k = 0; k < 3; k++)
					{
						mz[k] = std::bit_cast<float>(memRead32(ent + 0x38 + 4 * k));
						gr[k] = std::bit_cast<float>(memRead32(ent + 0x10 + 12 * var + 4 * k));
					}
					double eu[3];
					for (int k = 0; k < 3; k++)
						eu[k] = static_cast<s16>(memRead16(ent + 0x28 + 8 * var + 2 * k)) * (2.0 * kPi / 65536.0);
					const double grd[3] = {gr[0], gr[1], gr[2]}, mzd[3] = {mz[0], mz[1], mz[2]};
					Frame wf;
					double wsc = 1.0;
					const bool wok = FromGameScaled(W, &wf, &wsc);
					const AttachedWeapon aw = AttachWeapon(wf, grd, sk.scale, eu, mzd);
					// What the game drew.
					const V org{M[12], M[13], M[14]};
					const V mrow0{M[0], M[1], M[2]}, mrow1{M[4], M[5], M[6]}, mrow2{M[8], M[9], M[10]};
					const V drawn_muzzle = org + mrow0 * mz[0] + mrow1 * mz[1] + mrow2 * mz[2];
					const V barrel = Norm(mrow0 * -1.0);
					// The controller (right VR hand = arm 1), in the world.
					double gp[3], p1[3], p2[3];
					ChestToWorld(skr, tg.hand_pos[1], gp);
					double fw[3], up[3];
					for (int k = 0; k < 3; k++)
					{
						fw[k] = tg.hand_pos[1][k] + tg.hand_fwd[1][k] * 10.0;
						up[k] = tg.hand_pos[1][k] + tg.hand_up[1][k] * 10.0;
					}
					ChestToWorld(skr, fw, p1);
					ChestToWorld(skr, up, p2);
					const V ctl_fwd = Norm(Ld(p1) - Ld(gp));
					const auto ang = [](const V& a, const V& b) { return std::acos(Clamp(Dot(Norm(a), Norm(b)), -1.0, 1.0)) / kDeg; };
					const double d_grip = Len(org - Ld(gp));
					const double d_exp = wok ? Len(org - Ld(aw.origin)) : -1.0;
					double a_exp = -1.0;
					if (wok)
					{
						a_exp = 0.0;
						for (int k = 0; k < 3; k++)
							a_exp = std::max(a_exp, ang(Row(aw.axes, k), k == 0 ? mrow0 : (k == 1 ? mrow1 : mrow2)));
					}
					// The controller's own derivation (HandModel's native placement: the posed wrist 6 cm behind the grip, the game's
					// attach), mapped into the game as the eye sees it, against what the game drew: the gameplay side's reading.
					double d_ctl = -1.0, a_ctl = -1.0, d_ctl_mz = -1.0;
					{
						const GameFeedback::WeaponView wv = GameFeedback::GetWeapon();
						const VRHandState& rh = self_snap.hands[VRInputSnapshot::RIGHT];
						float sh[3];
						static u64 s_why_ms = 0;
						if (now - s_why_ms > 5000 && !(wv.valid && wv.attach.valid && rh.grip_pose.valid))
						{
							s_why_ms = now;
							Console.WriteLn("(VR) NativeHands: no controller comparison (weapon view %d, attach %d, right grip %d).", wv.valid, wv.attach.valid,
								rh.grip_pose.valid);
						}
						if (wv.valid && wv.attach.valid && rh.grip_pose.valid && NativeSeenShift(sh))
						{
							const VRPose& orient = rh.aim_pose.valid ? rh.aim_pose : rh.grip_pose;
							float ov[3], ax[3][3];
							const HandModel::WeaponHeld wh = HandModel::GetWeaponHeld();
							if (HandModel::ModelFrame(wv.attach, wh.known ? wh.held : HandModel::Held::Pistol, rh.grip_pose.position_xyz.data(),
									orient.orientation_xyzw.data(), ov, ax, true))
							{
								float g0[3], g1[3];
								bool ok = CameraDriver::GameFromVr(mapping, ov, g0);
								V axg[3];
								for (int k = 0; ok && k < 3; k++)
								{
									const float pv[3] = {ov[0] + ax[k][0] * 0.01f, ov[1] + ax[k][1] * 0.01f, ov[2] + ax[k][2] * 0.01f};
									ok = CameraDriver::GameFromVr(mapping, pv, g1);
									axg[k] = Norm(V{g1[0] - g0[0], g1[1] - g0[1], g1[2] - g0[2]});
								}
								if (ok)
								{
									const V eo = V{g0[0] + sh[0], g0[1] + sh[1], g0[2] + sh[2]};
									d_ctl = Len(org - eo);
									a_ctl = std::max({ang(axg[0], mrow0), ang(axg[1], mrow1), ang(axg[2], mrow2)});
									const V em = eo + axg[0] * mz[0] + axg[1] * mz[1] + axg[2] * mz[2];
									d_ctl_mz = Len(drawn_muzzle - em);
								}
							}
						}
					}
					float ro[3] = {}, rd[3] = {};
					const bool ray = GunRay::LastShotRay(ro, rd, 600);
					const V rov{ro[0], ro[1], ro[2]}, rdv{rd[0], rd[1], rd[2]};
					Console.WriteLn("(VR) NativeHands weapon: item %u type %u object %08X; drawn origin (%.1f, %.1f, %.1f), %.1f cm from the controller "
									"grip; vs the game's own attach maths on the posed wrist %.2f cm / %.2f deg; barrel %.2f deg off the controller's "
									"forward; drawn muzzle (%.1f, %.1f, %.1f); vs the controller's own placement: origin %.2f cm, axes %.2f deg, "
									"muzzle %.2f cm; gun ray (shot start) %s %.1f cm from the drawn muzzle, its direction "
									"%.2f deg off the barrel.",
						item, type, obj, org.x, org.y, org.z, d_grip, d_exp, a_exp, ang(barrel, ctl_fwd), drawn_muzzle.x, drawn_muzzle.y,
						drawn_muzzle.z, d_ctl, a_ctl, d_ctl_mz, ray ? "" : "(none)", ray ? Len(rov - drawn_muzzle) : -1.0,
						ray ? ang(rdv, barrel) : -1.0);
				}
			}
			if (log_now && self_anchored)
			{
				// Per arm: the wrist's target (the controller's grip, moved to the wrist, as the eye sees it), the solve and the
				// wrist bone's world in memory (what cave V wrote at the last draw); then where the shoulders, elbows and the
				// drawn bones are against the eye.
				std::string line;
				double nearest = 1e9, top = 1e9;
				for (int a = 0; a < 2; a++)
				{
					if (!pose.drawn[a])
					{
						line += fmt::format(" arm{} hidden;", a);
						continue;
					}
					double wt[3], tt[3], tc[3], sw[3], ew[3];
					ChestToWorld(skr, pose.wrist[a], wt);
					for (int k = 0; k < 3; k++)
						tc[k] = tg.hand_pos[a][k] - tg.hand_fwd[a][k] * tu.wrist_back_cm;
					ChestToWorld(skr, tc, tt);
					ChestToWorld(skr, pose.shoulder[a], sw);
					ChestToWorld(skr, pose.elbow[a], ew);
					const u32 wp = sk.ptr[map.arm[a].wrist], ep = sk.ptr[map.arm[a].fore];
					const double mem[3] = {std::bit_cast<float>(memRead32(wp + 0x30)), std::bit_cast<float>(memRead32(wp + 0x34)),
						std::bit_cast<float>(memRead32(wp + 0x38))};
					const double emem[3] = {std::bit_cast<float>(memRead32(ep + 0x30)), std::bit_cast<float>(memRead32(ep + 0x34)),
						std::bit_cast<float>(memRead32(ep + 0x38))};
					line += fmt::format(" arm{} (wrist bone {}) weight 1.00: target ({:.1f}, {:.1f}, {:.1f}), solved ({:.1f}, {:.1f}, "
										"{:.1f}) {:.1f} cm off{}, in memory ({:.1f}, {:.1f}, {:.1f}) {:.1f} cm off;",
						a, map.arm[a].wrist, tt[0], tt[1], tt[2], wt[0], wt[1], wt[2], Dist3(wt, tt), pose.reach_clamped[a] ? " (out of reach)" : "",
						mem[0], mem[1], mem[2], Dist3(mem, tt));
					double so[3], eo[3], wo[3];
					FromEye(alg, sw, so);
					FromEye(alg, ew, eo);
					FromEye(alg, wt, wo);
					const double lf = Dist3(ew, wt) / skr.scale, lu = Dist3(sw, ew) / skr.scale;
					line += fmt::format(" [anchor arm{}: shoulder ({:+.1f} up, {:+.1f} fwd, {:+.1f} right) elbow ({:+.1f}, {:+.1f}, {:+.1f}) in memory "
										"{:.1f} cm off, wrist ({:+.1f}, {:+.1f}, {:+.1f}); forearm {:.2f} upper arm {:.2f} model units]",
						a, so[0], so[1], so[2], eo[0], eo[1], eo[2], Dist3(emem, ew), wo[0], wo[1], wo[2], lf, lu);
					// The drawn bones: Forearms the forearm (elbow to wrist), Arms the upper arm too.
					nearest = std::min(nearest, SegDist(alg.eye, ew, wt));
					if (style == SelfStyle::Arms)
					{
						nearest = std::min(nearest, SegDist(alg.eye, sw, ew));
						top = std::min(top, alg.eye[1] - sw[1]);
					}
				}
				const std::string shoulders = top < 1e8 ? fmt::format("{:.1f} cm under the eye", top) : std::string("none (Forearms)");
				Console.WriteLn("(VR) Body: self record %08X, %d bones, scale %.3f, left arm %d, style %s, body scale %.2f, body yaw %+.1f deg off the "
								"head%s:%s [anchor drawn bones: nearest %.1f cm from the eye; drawn shoulders %s]",
					rec, pose.count, sk.scale, left_arm, style == SelfStyle::Arms ? "Arms" : "Forearms", s_cal.scale, alg.body_off_deg,
					alg.hands_used ? " (toward the hands)" : "", line.c_str(), nearest, shoulders.c_str());
			}
			else if (log_now)
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
		const bool game_overlay = game_overlay_now;
		memWrite32(bp->block + 0x08, game_overlay ? 0x6313A0u : 0u);
		memWrite32(bp->block + 0x0C, game_overlay ? 0x27BDFF50u : 0u);
		++s_stamp;
		memWrite32(bp->block + 0x04, s_stamp);
		memWrite32(bp->block, kBlockMagic);
		s_written = true;
		s_written_block = bp->block;
		s_written_tables = bp->tables;
		s_written_crc = VMManager::GetDiscCRC();
		NativeHandsPublish(native_hand, native_weapon_raw, CameraDriver::SteadyNowMs());
		// The decision is made here too (the compositor asks again every present with the same gate; the state is shared), so
		// the gameplay side (the gun ray, the melee shape, the hit-stop) and a headset-free run see it without a compositor.
		NativeHandsUpdate(CameraDriver::SteadyNowMs(), CameraDriver::LookAtActive() && !SpatialControls::MenuMode(), nullptr, nullptr);
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
		double SegDistV(const V& p, const V& a, const V& b)
		{
			const V ab = b - a;
			const double l2 = Dot(ab, ab);
			const double t = l2 > 1e-12 ? Clamp(Dot(p - a, ab) / l2, 0.0, 1.0) : 0.0;
			return Len(p - (a + ab * t));
		}
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


		// --- Your own arms anchored to the headset (VrBodySelfStyle) ---
		{
			const AnchorTuning atu;
			// The calibration: a seated eye over the floor scales the neck model; an unknown or a head-level origin does not.
			check(std::fabs(CalibrateScale(1.2, atu) - 1.0) < 1e-12 && std::fabs(CalibrateScale(1.1, atu) - 0.65 / 0.75) < 1e-12 &&
					  std::fabs(CalibrateScale(1.3, atu) - 0.85 / 0.75) < 1e-12,
				"calibration: eye 1.1 / 1.2 / 1.3 m over the floor give a body scale 0.87 / 1 / 1.13");
			check(CalibrateScale(std::numeric_limits<double>::quiet_NaN(), atu) == 1.0 && CalibrateScale(0.02, atu) == 1.0 &&
					  CalibrateScale(1.75, atu) == 1.0 && CalibrateScale(0.95, atu) == atu.scale_min,
				"calibration: no floor, a head-level origin or a standing eye leave 1; a low seat clamps to the minimum");

			// The neck model: the shoulders under and behind the eye, level, the model's half width each side.
			const double hw = (9.31 + 8.28) * 1.05 / 100.0;
			const double eye[3] = {0.0, 1.2, 0.0}, fwd[3] = {0.0, 0.0, -1.0}, up[3] = {0.0, 1.0, 0.0};
			const bool none[2] = {false, false};
			const double nowhere[2][3] = {};
			AnchorBody b;
			double yaw = std::numeric_limits<double>::quiet_NaN();
			bool ok = AnchorShoulders(eye, fwd, up, none, nowhere, 1.0, hw, 0.0, atu, &yaw, &b);
			const double drop = atu.pivot_down_m + atu.shoulder_down_m, back = atu.pivot_back_m + atu.shoulder_back_m;
			check(ok && std::fabs(b.shoulder[0][1] - (1.2 - drop)) < 1e-12 && std::fabs(b.shoulder[1][1] - (1.2 - drop)) < 1e-12 &&
					  std::fabs(b.shoulder[0][2] - back) < 1e-12 && std::fabs(b.shoulder[0][0] + hw) < 1e-12 && std::fabs(b.shoulder[1][0] - hw) < 1e-12 &&
					  drop > 0.2 && back > 0.08,
				"the shoulders 21.5 cm under and 9 cm behind the eye, the model's half width each side (left at -X)");
			// A nod or a turn about the neck's pivot (the head rotating about it) leaves the pivot and the shoulders put.
			{
				const V pivot = Ld(b.pivot);
				bool same = true;
				for (const double pitch : {-50.0, 30.0})
					for (const double yaw_deg : {0.0, 35.0})
					{
						// The head's frame: yaw about +Y then pitch about its right; the eye = pivot + R (0, down, -back) undone.
						const double cy = std::cos(yaw_deg * kDeg), sy = std::sin(yaw_deg * kDeg), cp = std::cos(pitch * kDeg),
									 sp = std::sin(pitch * kDeg);
						const V f{-sy * cp, sp, -cy * cp};
						const V r{cy, 0.0, -sy};
						const V u = Cross(r, f);
						const V e = pivot + u * atu.pivot_down_m + f * atu.pivot_back_m;
						double ev[3], fv[3], uv[3];
						St(e, ev);
						St(f, fv);
						St(u, uv);
						AnchorBody bn;
						double yn = std::numeric_limits<double>::quiet_NaN();
						same &= AnchorShoulders(ev, fv, uv, none, nowhere, 1.0, hw, 0.0, atu, &yn, &bn) && D3(Ld(bn.pivot), pivot) < 1e-9 &&
								std::fabs(bn.shoulder[0][1] - b.shoulder[0][1]) < 1e-9 && std::fabs(yn - yaw_deg * kDeg) < 1e-9;
					}
				check(same, "the neck model: nodding 50 down or 30 up and turning 35 keep the pivot and the shoulders' height");
			}
			// The body turns between the head and the hands (70% of the way), at most 60 deg off the head, and eases.
			{
				const bool both[2] = {true, true};
				const double ahead_left[2][3] = {{-0.5, 0.8, -0.2}, {-0.4, 0.8, -0.3}}; // the midpoint 45 deg left of ahead
				AnchorBody bl;
				double y1 = std::numeric_limits<double>::quiet_NaN();
				AnchorShoulders(eye, fwd, up, both, ahead_left, 1.0, hw, 0.0, atu, &y1, &bl);
				const double mx = -0.45 - bl.pivot[0], mz = -0.25 - bl.pivot[2];
				const double want = atu.hands_share * std::atan2(-mx, -mz);
				check(bl.hands_used && std::fabs(y1 - want) < 1e-9 && y1 > 0.0, "hands ahead to the left turn the body 70% of the way there");
				// The head turned 90 left, the hands ahead: the body stays between, within 60 deg of the head.
				const double fl[3] = {-1.0, 0.0, 0.0};
				const double ahead[2][3] = {{-0.2, 0.8, -0.45}, {0.2, 0.8, -0.45}};
				double y2 = std::numeric_limits<double>::quiet_NaN();
				AnchorShoulders(eye, fl, up, both, ahead, 1.0, hw, 0.0, atu, &y2, &bl);
				check(y2 > 10.0 * kDeg && y2 < 60.0 * kDeg && 90.0 * kDeg - y2 <= atu.max_body_yaw_deg * kDeg + 1e-9,
					"the head 90 left with the hands ahead: the body between them, within 60 deg of the head");
				// Easing: from 0 toward the hands' yaw, a step of 1 / 60 s goes 1 - exp(-8 / 60) of the way.
				double y3 = 0.0;
				AnchorShoulders(eye, fwd, up, both, ahead_left, 1.0, hw, 1.0 / 60.0, atu, &y3, &bl);
				check(std::fabs(y3 - want * (1.0 - std::exp(-atu.yaw_rate_per_s / 60.0))) < 1e-9, "the body's yaw eases toward its target");
				const double nan3[3] = {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0};
				check(!AnchorShoulders(nan3, fwd, up, both, ahead, 1.0, hw, 0.0, atu, &y3, &bl), "a NaN head refused");
			}

			// The hide plan: what is drawn and where everything else vanishes, on Kevin's 36 bones and the woman's 43.
			const BoneMap m = DefaultMap();
			HidePlan hp;
			bool okp = SelfHidePlan(m, kKevinParents, 36, SelfStyle::Forearms, &hp);
			const auto kinds = [&hp](HideKind k) {
				u64 mask = 0;
				for (int i = 0; i < hp.count; i++)
					if (hp.kind[i] == k)
						mask |= u64{1} << hp.bone[i];
				return mask;
			};
			const auto bits = [](std::initializer_list<int> l) {
				u64 mask = 0;
				for (int b : l)
					mask |= u64{1} << b;
				return mask;
			};
			check(okp && hp.count == 24 && kinds(HideKind::Drawn) == bits({14, 15, 32, 33, 34, 35, 18, 19, 28, 29, 30, 31}) &&
					  kinds(HideKind::AtElbow) == bits({13, 17}) && kinds(HideKind::AtChest) == bits({11, 12, 16, 20, 21, 22, 23, 24, 25, 26}) &&
					  kinds(HideKind::AtShoulder) == 0,
				"Forearms on Kevin: the forearms and hands drawn, the upper arms at their elbows, the chest, the clavicles and the head at the chest (24; the jaw 27 on its own joint)");
			okp = SelfHidePlan(m, kKevinParents, 36, SelfStyle::Arms, &hp);
			check(okp && hp.count == 24 && kinds(HideKind::Drawn) == bits({13, 14, 15, 32, 33, 34, 35, 17, 18, 19, 28, 29, 30, 31}) &&
					  kinds(HideKind::AtShoulder) == bits({12, 16}) && kinds(HideKind::AtChest) == bits({11, 20, 21, 22, 23, 24, 25, 26}) &&
					  kinds(HideKind::AtElbow) == 0,
				"Arms on Kevin: the whole arms drawn, the clavicles at their shoulders, the chest and the head at the chest (24)");
			okp = SelfHidePlan(m, kWomanParents, 43, SelfStyle::Forearms, &hp);
			check(okp && hp.count == kMaxOut && (kinds(HideKind::Drawn) & bits({14, 18, 15, 19})) == bits({14, 18, 15, 19}) &&
					  kinds(HideKind::AtElbow) == bits({13, 17}) && (kinds(HideKind::AtChest) & bits({12, 16, 20, 21})) == bits({12, 16, 20, 21}),
				"the woman's 43 bones: the table full, only the head's last bones left to the cave's own collapse");
			check(!SelfHidePlan(m, kKevinParents, 36, SelfStyle::GameShoulders, &hp) && !SelfHidePlan(m, kKevinParents, 18, SelfStyle::Forearms, &hp),
				"no plan for GameShoulders or a model the map does not fit");

			// The whole anchored solve, end to end from the headset, on Kevin's bind pose: the model's frame is VR space turned
			// half round (+X left, +Z forward) in game units at Kevin's scale. Six seated poses at three eye heights.
			Frame locals[36];
			for (int bn = 0; bn < 36; bn++)
				for (int k = 0; k < 3; k++)
					locals[bn].t[k] = kKevinOffsets[bn][k];
			const double units = 100.0 / 1.05;
			const auto to_model = [units](const double v[3], double o[3]) {
				o[0] = -v[0] * units;
				o[1] = v[1] * units;
				o[2] = -v[2] * units;
			};
			const auto dir_model = [](const double v[3], double o[3]) {
				o[0] = -v[0];
				o[1] = v[1];
				o[2] = -v[2];
			};
			const double poses[6][2][3] = {{{-0.20, -0.32, -0.38}, {0.22, -0.30, -0.42}}, {{-0.18, -0.50, -0.25}, {0.18, -0.50, -0.25}},
				{{-0.18, -0.20, -0.58}, {0.18, -0.20, -0.58}}, {{-0.22, -0.45, -0.30}, {0.25, 0.10, -0.30}},
				{{-0.55, -0.28, -0.10}, {0.55, -0.28, -0.10}}, {{-0.20, -0.32, -0.38}, {0.22, -0.30, -0.42}}};
			const Tuning tu;
			double worst_err = 0.0, worst_len = 0.0, min_under = 1e9, min_near = 1e9;
			bool all = true;
			for (const double h : {1.1, 1.2, 1.3})
				for (const SelfStyle style : {SelfStyle::Forearms, SelfStyle::Arms})
					for (int pi = 0; pi < 6; pi++)
					{
						const double k = CalibrateScale(h, atu);
						const double e[3] = {0.0, h, 0.0};
						double hand_vr[2][3];
						for (int s2 = 0; s2 < 2; s2++)
							for (int q = 0; q < 3; q++)
								hand_vr[s2][q] = poses[pi][s2][q] + e[q];
						const bool both[2] = {true, true};
						AnchorBody ab;
						double yy = std::numeric_limits<double>::quiet_NaN();
						all &= AnchorShoulders(e, fwd, up, both, hand_vr, k, hw, 0.0, atu, &yy, &ab);
						AnchorTargets at;
						for (int a = 0; a < 2; a++)
						{
							// Arm 0 (the model's +X) is the character's left: the VR left hand.
							at.tg.hand[a] = true;
							to_model(hand_vr[a], at.tg.hand_pos[a]);
							const double cf[3] = {0.0, 0.0, -1.0}, cu[3] = {0.0, 1.0, 0.0};
							dir_model(cf, at.tg.hand_fwd[a]);
							dir_model(cu, at.tg.hand_up[a]);
							to_model(ab.shoulder[a], at.shoulder[a]);
						}
						dir_model(ab.fwd, at.body_fwd);
						Pose pz;
						all &= SolveAnchored(m, locals, kKevinParents, 36, at, tu, style, atu.protract_m * units, &pz) && pz.drawn[0] && pz.drawn[1];
						double em[3];
						to_model(e, em);
						for (int a = 0; a < 2; a++)
						{
							worst_err = std::max(worst_err, pz.wrist_err[a] / units * 100.0); // cm
							const ArmMap& am = m.arm[a];
							worst_len = std::max(worst_len, std::fabs(D3(Ld(pz.elbow[a]), Ld(pz.wrist[a])) - Len(Ld(locals[am.wrist].t))));
							worst_len = std::max(worst_len, std::fabs(D3(Ld(pz.shoulder[a]), Ld(pz.elbow[a])) - Len(Ld(locals[am.fore].t))));
							// The drawn bones' highest point and nearest approach to the eye (cm), less a 6 cm sleeve.
							// The drawn shoulder (Arms): its sleeve's top under the eye. A raised hand may go over the eye, as yours does.
							if (style == SelfStyle::Arms)
								min_under = std::min(min_under, (em[1] - pz.shoulder[a][1]) / units * 100.0 - 6.0);
							double near_e = SegDistV(Ld(em), Ld(pz.elbow[a]), Ld(pz.wrist[a]));
							if (style == SelfStyle::Arms)
								near_e = std::min(near_e, SegDistV(Ld(em), Ld(pz.shoulder[a]), Ld(pz.elbow[a])));
							min_near = std::min(min_near, near_e / units * 100.0 - 6.0);
						}
						// Hidden bones have zero axes; only drawn ones are kept.
						for (int i = 0; i < pz.count; i++)
						{
							double ax = 0.0;
							for (int r = 0; r < 3; r++)
								for (int c = 0; c < 3; c++)
									ax += std::fabs(pz.rel[i].r[r][c]);
							all &= pz.keep[i] ? std::fabs(ax - 0.0) > 1.0 : ax == 0.0;
						}
					}
			check(all && worst_err < 0.5, "seated eye 1.1-1.3 m, six hand poses, both styles: every wrist on its controller (within 0.5 cm)");
			check(all && worst_len < 1e-6, "the arms never stretch: the forearm and the upper arm keep their lengths");
			check(all && min_under > 5.0 && min_near > 8.0,
				"the eye never inside or above the drawn shoulders (their sleeve's top over 5 cm under it), every drawn bone's sleeve over 8 cm from it");
			// Out of reach: the shoulder slides at most protract, then the arm stops short, straight, still its length.
			{
				AnchorTargets at;
				at.tg.hand[0] = true;
				const V sh = Ld(kKevinOffsets[12]) + Ld(kKevinOffsets[13]);
				St(sh, at.shoulder[0]);
				St(sh + V{0.0, 0.0, 51.0 + 4.0 + tu.wrist_back_cm}, at.tg.hand_pos[0]); // 4 units past the reach
				Pose po;
				const double protract = 8.0;
				bool ok2 = SolveAnchored(m, locals, kKevinParents, 36, at, tu, SelfStyle::Forearms, protract, &po);
				check(ok2 && po.drawn[0] && !po.drawn[1] && po.wrist_err[0] < 1e-6 &&
						  std::fabs(D3(Ld(po.shoulder[0]), sh) - (55.0 - 0.999 * (Len(Ld(kKevinOffsets[14])) + Len(Ld(kKevinOffsets[15]))))) < 1e-6,
					"a hand just out of reach: the shoulder slides exactly the shortfall toward it, the wrist on target");
				St(sh + V{0.0, 0.0, 51.0 + 20.0 + tu.wrist_back_cm}, at.tg.hand_pos[0]);
				ok2 = SolveAnchored(m, locals, kKevinParents, 36, at, tu, SelfStyle::Forearms, protract, &po);
				check(ok2 && po.reach_clamped[0] && std::fabs(D3(Ld(po.shoulder[0]), sh) - protract) < 1e-9 && po.wrist_err[0] > 11.0 &&
						  std::fabs(D3(Ld(po.elbow[0]), Ld(po.wrist[0])) - Len(Ld(kKevinOffsets[15]))) < 1e-9,
					"20 units out: the shoulder slides its 8, the arm straight and short, the forearm its length");
				// The untracked arm: every one of its bones hidden at the chest, nothing of it kept.
				bool hidden = true;
				for (int i = 0; i < po.count; i++)
					if (po.bone[i] == 17 || po.bone[i] == 18 || po.bone[i] == 19 || po.bone[i] == 28)
						hidden &= !po.keep[i] && Len(Ld(po.rel[i].t)) == 0.0;
				check(hidden, "an untracked hand: its whole arm hidden at the chest's joint");
				Targets nobody;
				AnchorTargets an;
				an.tg = nobody;
				check(!SolveAnchored(m, locals, kKevinParents, 36, an, tu, SelfStyle::Forearms, protract, &po) &&
						  !SolveAnchored(m, locals, kKevinParents, 36, at, tu, SelfStyle::GameShoulders, protract, &po),
					"no hand tracked, or GameShoulders: nothing for the anchored draw");
			}
			// Forearms: an upper arm hidden at its elbow (the forearm's joint), the clavicles at the chest's own joint.
			{
				AnchorTargets at;
				at.tg.hand[0] = at.tg.hand[1] = true;
				const V sh0 = Ld(kKevinOffsets[12]) + Ld(kKevinOffsets[13]), sh1 = Ld(kKevinOffsets[16]) + Ld(kKevinOffsets[17]);
				St(sh0, at.shoulder[0]);
				St(sh1, at.shoulder[1]);
				St(sh0 + V{8.0, -25.0, 30.0 + tu.wrist_back_cm}, at.tg.hand_pos[0]);
				St(sh1 + V{-5.0, -10.0, 38.0 + tu.wrist_back_cm}, at.tg.hand_pos[1]);
				Pose pf;
				bool ok3 = SolveAnchored(m, locals, kKevinParents, 36, at, tu, SelfStyle::Forearms, 8.0, &pf);
				int i13 = -1, i14 = -1, i12 = -1;
				for (int i = 0; i < pf.count; i++)
				{
					i13 = pf.bone[i] == 13 ? i : i13;
					i14 = pf.bone[i] == 14 ? i : i14;
					i12 = pf.bone[i] == 12 ? i : i12;
				}
				check(ok3 && i13 >= 0 && i14 >= 0 && i12 >= 0 && !pf.keep[i13] && pf.keep[i14] && D3(Ld(pf.rel[i13].t), Ld(pf.rel[i14].t)) < 1e-12 &&
						  Len(Ld(pf.rel[i12].t)) == 0.0 && D3(Ld(pf.rel[i14].t), Ld(pf.elbow[0])) < 1e-12,
					"Forearms: the upper arm's vertices vanish at the elbow, the clavicle's at the chest's joint");
				// The block for it: every entry a draw-time frame, the keep mask the drawn bones.
				check(SlotFlags(1, false, true) == (kFlagSelf | kFlagCollapse), "the anchored slot: self and collapse, never cave U's locals");
			}
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

		// --- NativeHands: the decision, its hold and the attach maths ---
		{
			// The step: native only after the raw state has held 200 ms; a drop is the overlay hand AT ONCE; a flap never gets there.
			NativeHold h;
			const bool a = NativeHoldStep(&h, 1, true, 1000, kNativeHoldMs);
			const bool b = NativeHoldStep(&h, 1, true, 1100, kNativeHoldMs);
			const bool c = NativeHoldStep(&h, 1, true, 1199, kNativeHoldMs);
			const bool d = NativeHoldStep(&h, 1, true, 1200, kNativeHoldMs);
			check(!a && !b && !c && d, "native hands: taken over after the raw state has held 200 ms (not at 199)");
			check(NativeHoldStep(&h, 1, true, 5000, kNativeHoldMs), "native hands: stays native while the pose lives");
			check(!NativeHoldStep(&h, 1, false, 5010, kNativeHoldMs) && !h.on[1], "native hands: a dropped pose is the overlay hand that same frame");
			check(!NativeHoldStep(&h, 1, true, 5020, kNativeHoldMs) && !NativeHoldStep(&h, 1, true, 5219, kNativeHoldMs) &&
					  NativeHoldStep(&h, 1, true, 5220, kNativeHoldMs),
				"native hands: back again only after another 200 ms of staying live");
			NativeHold flap;
			bool ever = false;
			for (u64 t = 0; t < 4000; t += 100) // live 100 ms, down 100 ms, ...: never 200 ms in a row
				ever |= NativeHoldStep(&flap, 0, (t / 100) % 2 == 0, 10000 + t, kNativeHoldMs);
			check(!ever, "native hands: a pose that flaps every 100 ms never takes over (no flicker)");
			check(!NativeHoldStep(nullptr, 0, true, 1, 200) && !NativeHoldStep(&h, 2, true, 1, 200) && !NativeHoldStep(&h, -1, true, 1, 200),
				"native hands: a bad hand or no state is never native");
			// The per-hand states are independent: the left hand's live run does not start the right's clock.
			NativeHold two;
			NativeHoldStep(&two, 0, true, 100, 200);
			check(!NativeHoldStep(&two, 1, true, 250, 200) && NativeHoldStep(&two, 0, true, 300, 200) && !two.on[1],
				"native hands: each hand's hold is its own");
			// Update: the raw published state, its age and the gate.
			const u64 t0 = 1000000;
			const bool live[2] = {true, true}, none[2] = {false, false};
			bool out[2] = {};
			bool wpn = false;
			NativeHandsPublish(none, false, t0);
			NativeHandsUpdate(t0, true, out, &wpn); // a clean start
			NativeHandsPublish(live, true, t0 + 10);
			NativeHandsUpdate(t0 + 10, true, out, &wpn);
			check(!out[0] && !out[1] && !wpn && !NativeHandsLive(1), "native hands update: fresh, but not yet held");
			NativeHandsPublish(live, true, t0 + 100);
			NativeHandsUpdate(t0 + 100, true, out, &wpn);
			NativeHandsPublish(live, true, t0 + 200);
			NativeHandsUpdate(t0 + 200, true, out, &wpn);
			NativeHandsPublish(live, true, t0 + 210);
			NativeHandsUpdate(t0 + 210, true, out, &wpn);
			check(out[0] && out[1] && wpn && NativeHandsLive(0) && NativeHandsLive(1), "native hands update: both hands and the weapon native after 0.2 s");
			NativeHandsUpdate(t0 + 211, false, out, &wpn);
			check(!out[0] && !out[1] && !wpn && !NativeHandsLive(1), "native hands update: a menu (the gate) or a cutscene gives the overlay hands back that frame");
			NativeHandsPublish(live, true, t0 + 300);
			NativeHandsUpdate(t0 + 300, true, out, &wpn);
			NativeHandsUpdate(t0 + 520, true, out, &wpn); // the host's last word is 220 ms old: stale
			check(!out[0] && !out[1], "native hands update: a host that stopped publishing (over 150 ms) is no native hand");
			const bool right_only[2] = {false, true};
			NativeHandsPublish(right_only, false, t0 + 600);
			NativeHandsUpdate(t0 + 600, true, out, &wpn);
			NativeHandsPublish(right_only, false, t0 + 810);
			NativeHandsUpdate(t0 + 810, true, out, &wpn);
			check(!out[0] && out[1] && !wpn, "native hands update: only the right hand tracked, and no weapon the game draws");
			NativeHandsPublish(none, false, t0 + 900);
			NativeHandsUpdate(t0 + 900, true, out, &wpn);
			check(!out[0] && !out[1] && !NativeHandsLive(0) && !NativeHandsLive(1) && !NativeHandsLive(2), "native hands update: nothing live at the end");
			// The slot's flags.
			check(SlotFlags(1, false, true, true) == (kFlagSelf | kFlagCollapse | kFlagAttach | kFlagWeapon) &&
					  SlotFlags(1, false, true, false) == (kFlagSelf | kFlagCollapse) &&
					  SlotFlags(1, true, true, true) == (kFlagLocals | kFlagSelf | kFlagCollapse) && SlotFlags(0, false, true, true) == kFlagAttach &&
					  SlotFlags(0, false, false, true) == 0,
				"native hands: the weapon flag only on the local draw-only slot (never with locals, never for the partner)");
			check((kFlagWeapon & (kFlagAttach | kFlagCollapse | kFlagSelf | kFlagLocals)) == 0, "the weapon flag is a free bit of the slot's flags byte");
		}
		{
			// The attach maths, against File #1's own table (read live, 2026-10-30): the 45 Auto (type 7: grip (-13.45, -1.60, 2.13), no
			// turn, muzzle (-9.5, 0, 5.09)), the shotgun (type 12: turn (-0.3, 10.6, -2.6) deg, a barrel 10.9 deg off the bone) and the
			// spray (type 27, turn (101.8, -23.6, -77.7) deg), on a wrist frame turned and moved: the model's origin is (grip x scale)
			// through the hand, the barrel the model's -X.
			Frame wrist;
			HeadFrame(0.7, -0.4, 0.2, wrist.r);
			wrist.t[0] = 120.0;
			wrist.t[1] = 80.0;
			wrist.t[2] = -40.0;
			const double g45[3] = {-13.45, -1.60, 2.13}, none3[3] = {0, 0, 0}, mz45[3] = {-9.50, 0.0, 5.09};
			const AttachedWeapon w45 = AttachWeapon(wrist, g45, 1.05, none3, mz45);
			const V want_o = MulRow(Ld(g45) * 1.05, wrist.r) + Ld(wrist.t);
			check(D3(Ld(w45.origin), want_o) < 1e-9 && D3(Ld(w45.barrel), Row(wrist.r, 0) * -1.0) < 1e-9 &&
					  D3(Ld(w45.muzzle), MulRow(Ld(mz45) + Ld(g45) * 1.05, wrist.r) + Ld(wrist.t)) < 1e-9,
				"attach maths: the 45's origin is (grip x 1.05) through the hand, its barrel the bone's -X, its muzzle ahead of it");
			const double e12[3] = {-0.3 * kDeg, 10.6 * kDeg, -2.6 * kDeg}, g12[3] = {-39.04, -1.19, 7.68}, mz12[3] = {-40.13, 0.0, 6.34};
			const AttachedWeapon w12 = AttachWeapon(wrist, g12, 1.05, e12, mz12);
			const double turn12 = std::acos(Clamp(Dot(Ld(w12.barrel), Row(wrist.r, 0) * -1.0), -1.0, 1.0)) / kDeg;
			check(std::fabs(turn12 - 10.9) < 0.05, "attach maths: the shotgun's barrel is 10.9 deg off the bone (the 2026-10-14 table)");
			// The axes stay a rotation: unit, perpendicular, right-handed.
			const double e27[3] = {101.8 * kDeg, -23.6 * kDeg, -77.7 * kDeg}, g27[3] = {-7.91, -2.92, -2.17}, mz27[3] = {0.0, 10.1, 5.7};
			const AttachedWeapon w27 = AttachWeapon(wrist, g27, 1.05, e27, mz27);
			bool rot = true;
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					rot &= std::fabs(Dot(Row(w27.axes, i), Row(w27.axes, j)) - (i == j ? 1.0 : 0.0)) < 1e-9;
			rot &= std::fabs(Dot(Cross(Row(w27.axes, 0), Row(w27.axes, 1)), Row(w27.axes, 2)) - 1.0) < 1e-9;
			check(rot, "attach maths: the spray's axes (101.8, -23.6, -77.7) are a rotation");
			// The same through HandModel's native placement (the laser's and the gameplay's reading), with a pointing frame that is the
			// identity: bone X -> +Z, Y -> +X, Z -> +Y (the hand model's own layout), the wrist 6.3 cm behind the grip point. The two
			// implementations (this one and HandModel::PlaceModel) must give the same origin, axes and muzzle.
			Frame bone;
			const double rows[3][3] = {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}};
			std::memcpy(bone.r, rows, sizeof(rows));
			bone.t[2] = HandModel::kNativeWristBackCm;
			struct Case
			{
				const double *g, *eu, *mz;
				HandModel::Held held;
			};
			const Case cases[] = {{g45, none3, mz45, HandModel::Held::Pistol}, {g12, e12, mz12, HandModel::Held::LongGun},
				{g27, e27, mz27, HandModel::Held::Melee}};
			bool same = true, muz = true;
			double worst_o = 0.0, worst_a = 0.0, worst_m = 0.0, worst_b = 0.0;
			for (const Case& cs : cases)
			{
				HandModel::GameAttach at;
				at.valid = true;
				for (int k = 0; k < 3; k++)
				{
					at.grip_cm[k] = static_cast<float>(cs.g[k]);
					at.rot_rad[k] = static_cast<float>(cs.eu[k]);
					at.muzzle_cm[k] = static_cast<float>(cs.mz[k]);
				}
				at.has_muzzle = true;
				const float pos[3] = {0, 0, 0}, quat[4] = {0, 0, 0, 1};
				float org[3], ax[3][3];
				const AttachedWeapon aw = AttachWeapon(bone, cs.g, 1.05, cs.eu, cs.mz);
				same &= HandModel::ModelFrame(at, cs.held, pos, quat, org, ax, true);
				const double o_cm[3] = {org[0] * 100.0, org[1] * 100.0, org[2] * 100.0};
				worst_o = std::max(worst_o, D3(Ld(o_cm), Ld(aw.origin)));
				for (int k = 0; k < 3; k++)
					worst_a = std::max(worst_a, std::acos(Clamp(Dot(V{ax[k][0], ax[k][1], ax[k][2]}, Row(aw.axes, k)), -1.0, 1.0)) / kDeg);
				if (cs.held != HandModel::Held::Melee)
				{
					HandModel::HandState hs;
					hs.valid = true;
					hs.gun = true;
					hs.held = cs.held;
					hs.native = true;
					hs.attach = at;
					float from[3], dir[3];
					muz &= HandModel::ModelLaserFrom(hs, 0.0f, from, dir);
					const double m_cm[3] = {from[0] * 100.0, from[1] * 100.0, from[2] * 100.0};
					worst_m = std::max(worst_m, D3(Ld(m_cm), Ld(aw.muzzle)));
					worst_b = std::max(worst_b, std::acos(Clamp(Dot(V{dir[0], dir[1], dir[2]}, Ld(aw.barrel)), -1.0, 1.0)) / kDeg);
				}
			}
			char what[256];
			std::snprintf(what, sizeof(what), "attach maths: VrBody's and HandModel's native placements agree (45, shotgun, spray: origin %.4f cm, axes %.3f deg, "
											  "muzzle %.4f cm, barrel %.3f deg)", worst_o, worst_a, worst_m, worst_b);
			check(same && muz && worst_o < 0.01 && worst_a < 0.05 && worst_m < 0.01 && worst_b < 0.05, what);
			// What the overlay hand's own anchor did: the 45's origin against the native one, in the pointing frame.
			HandModel::GameAttach a45;
			a45.valid = true;
			for (int k = 0; k < 3; k++)
			{
				a45.grip_cm[k] = static_cast<float>(g45[k]);
				a45.muzzle_cm[k] = static_cast<float>(mz45[k]);
			}
			a45.has_muzzle = true;
			float ov[3], oa[3][3], nv[3], na[3][3];
			const float pos0[3] = {0, 0, 0}, quat0[4] = {0, 0, 0, 1};
			if (HandModel::ModelFrame(a45, HandModel::Held::Pistol, pos0, quat0, ov, oa, false) &&
				HandModel::ModelFrame(a45, HandModel::Held::Pistol, pos0, quat0, nv, na, true))
			{
				const double dd = 100.0 * std::sqrt((ov[0] - nv[0]) * (ov[0] - nv[0]) + (ov[1] - nv[1]) * (ov[1] - nv[1]) + (ov[2] - nv[2]) * (ov[2] - nv[2]));
				Console.WriteLn("(VR) Body self-test: the 45's origin sits %.2f cm apart in the overlay hand's placement (the hand model's wrist anchor) and the "
								"native one (the posed wrist 6.3 cm behind the grip point).", dd);
				check(dd > 0.0 && dd < 30.0, "native hands: the overlay and native placements of the 45 differ, by less than a hand's length");
			}
		}

		if (failed == 0)
			Console.WriteLn("(VR) Body self-test: %d checks passed.", checks);
		else
			Console.Error("(VR) Body self-test: %d of %d checks FAILED.", failed, checks);
		return failed == 0;
	}
} // namespace VR::VrBody
