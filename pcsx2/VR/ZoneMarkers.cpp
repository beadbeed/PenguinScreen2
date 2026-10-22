// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/ZoneMarkers.h"
#include "VR/CameraDriver.h"
#include "VR/GameFeedback.h"
#include "VR/HandModel.h"
#include "VR/ItemData.h"

#include "common/Console.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <mutex>

namespace VR::ZoneMarkers
{
	namespace
	{
		using SpatialControls::Quat;
		using SpatialControls::Vec3;

		constexpr float kPi = 3.14159265358979323846f;
		constexpr float kDegToRad = kPi / 180.0f;
		constexpr int kSize = static_cast<int>(kImageSize);
		constexpr size_t kPixels = static_cast<size_t>(kImageSize) * kImageSize;

		// PlaceMarks: every mark at least kNearM in front of the eye along the view; the quad kFitMargin bigger than the
		// marks seen from the eye (their lines and soft edges included), and between kMinQuadM and kMaxQuadM.
		constexpr float kNearM = 0.05f;
		constexpr float kFitMargin = 1.12f;
		constexpr float kMinQuadM = 0.04f;
		constexpr float kMaxQuadM = 1.2f;
		// Lines are kLineM wide at the mark, and kMinLinePx to kMaxLinePx in the image.
		constexpr float kLineM = 0.0025f;
		constexpr float kMinLinePx = 1.6f;
		constexpr float kMaxLinePx = 4.0f;
		// The bag reaches this far round its middle, in its half heights (the corners of its body).
		constexpr float kBagReach = 1.45f;
		// A pouch's ring is drawn at this much of its ball's outline. Seen from the eyes the shipped rig's rows lie almost
		// along the line of sight, so full-size outlines would pile into one another; at half size each pouch keeps a ring
		// of its own round its point (and a hand in the ball lights it).
		constexpr float kRingFrac = 0.5f;
		// Dim: a pale khaki at kRingAlpha (a ring) or kBagAlpha (the bag, smaller, so a little stronger). Lit: a warmer
		// white at kLitAlpha, with a fill of kFillAlpha inside.
		constexpr u32 kDimR = 214, kDimG = 205, kDimB = 178;
		constexpr u32 kLitR = 255, kLitG = 236, kLitB = 190;
		constexpr float kRingAlpha = 0.30f;
		constexpr float kBagAlpha = 0.40f;
		constexpr float kLitAlpha = 0.85f;
		constexpr float kFillAlpha = 0.10f;

		struct Published
		{
			ZoneView view;
			u64 ms = 0; // CameraDriver::SteadyNowMs at the publish; 0 never
		};
		std::mutex s_zone_mutex;
		Published s_zone[static_cast<int>(Zone::Count)];

		Vec3 Cross(const Vec3& a, const Vec3& b)
		{
			return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
		}

		Vec3 Normalize(const Vec3& a)
		{
			const float l = SpatialControls::Length(a);
			return (l > 1.0e-8f) ? a * (1.0f / l) : Vec3{0.0f, 0.0f, 0.0f};
		}

		Vec3 Load(const float v[3])
		{
			return Vec3{v[0], v[1], v[2]};
		}

		void Store(const Vec3& a, float out[3])
		{
			out[0] = a.x;
			out[1] = a.y;
			out[2] = a.z;
		}

		bool Finite(const float* v, int n)
		{
			for (int i = 0; i < n; i++)
			{
				if (!std::isfinite(v[i]))
					return false;
			}
			return true;
		}

		bool Finite(const Vec3& v)
		{
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
		}

		// Unit quaternion from q (xyzw); identity when it is unusable.
		Quat UnitQuat(const float q[4])
		{
			const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
			if (!(l > 1.0e-6f) || !std::isfinite(l))
				return Quat{};
			return Quat{q[0] / l, q[1] / l, q[2] / l, q[3] / l};
		}

		// Rotation matrix with columns r, u, n -> quaternion (xyzw), as HandModel's quads are built.
		void QuatFromBasis(const Vec3& r, const Vec3& u, const Vec3& n, float out[4])
		{
			const float m00 = r.x, m01 = u.x, m02 = n.x;
			const float m10 = r.y, m11 = u.y, m12 = n.y;
			const float m20 = r.z, m21 = u.z, m22 = n.z;
			const float trace = m00 + m11 + m22;
			float x, y, z, w;
			if (trace > 0.0f)
			{
				const float sq = std::sqrt(trace + 1.0f) * 2.0f;
				w = 0.25f * sq;
				x = (m21 - m12) / sq;
				y = (m02 - m20) / sq;
				z = (m10 - m01) / sq;
			}
			else if (m00 > m11 && m00 > m22)
			{
				const float sq = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
				w = (m21 - m12) / sq;
				x = 0.25f * sq;
				y = (m01 + m10) / sq;
				z = (m02 + m20) / sq;
			}
			else if (m11 > m22)
			{
				const float sq = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
				w = (m02 - m20) / sq;
				x = (m01 + m10) / sq;
				y = 0.25f * sq;
				z = (m12 + m21) / sq;
			}
			else
			{
				const float sq = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
				w = (m10 - m01) / sq;
				x = (m02 + m20) / sq;
				y = (m12 + m21) / sq;
				z = 0.25f * sq;
			}
			const float l = std::sqrt(x * x + y * y + z * z + w * w);
			out[0] = x / l;
			out[1] = y / l;
			out[2] = z / l;
			out[3] = w / l;
		}

		// A point seen from the eye on a quad's plane: the central projection, in metres from the quad's centre along its
		// right and up (and the scale s that maps a size at the point onto the plane). With the eye at e and the plane's
		// normal n toward it: the point's depth along the view z, the eye's height over the plane de, s = de / z.
		struct OnPlane
		{
			float x = 0.0f;
			float y = 0.0f;
			float s = 0.0f;
			float dist = 0.0f; // from the eye to the point
		};

		bool ProjectOnPlane(const Vec3& e, const Vec3& c, const Vec3& r, const Vec3& u, const Vec3& n, const Vec3& p,
			OnPlane* out)
		{
			const Vec3 ec = e - c;
			const float de = SpatialControls::Dot(ec, n);
			const Vec3 d = p - e;
			const float z = -SpatialControls::Dot(d, n);
			if (!(de > 0.02f) || !(z > kNearM))
				return false;
			out->s = de / z;
			out->x = SpatialControls::Dot(ec, r) + out->s * SpatialControls::Dot(d, r);
			out->y = SpatialControls::Dot(ec, u) + out->s * SpatialControls::Dot(d, u);
			out->dist = SpatialControls::Length(d);
			return std::isfinite(out->x) && std::isfinite(out->y) && std::isfinite(out->s);
		}

		// How far a mark reaches round its middle on the plane: a ring kRingFrac of its ball's outline (the angular radius
		// asin(r / dist), on the plane), the bag kBagReach of its half height.
		float ReachOnPlane(const Mark& m, const OnPlane& p)
		{
			if (m.shape == Shape::Bag)
				return kBagReach * m.radius * p.s;
			if (m.shape == Shape::Box)
				return m.radius * p.s; // a corner: a point, with a little room for the lines that meet there
			const float sq = p.dist * p.dist - m.radius * m.radius;
			if (!(sq > 1.0e-8f))
				return -1.0f; // the eye inside the ball
			return kRingFrac * m.radius * p.s * p.dist / std::sqrt(sq);
		}

		float Clamp01(float v)
		{
			return (v > 0.0f) ? std::min(v, 1.0f) : 0.0f; // also catches NaN
		}

		// Coverage of a line w pixels wide at signed distance d from its middle.
		float LineCover(float d, float w)
		{
			return Clamp01(0.5f * w + 0.5f - std::fabs(d));
		}

		// Signed distance to a rounded box centred at (cx, cy) with half extents (hx, hy) and corner radius cr.
		float BoxDistance(float px, float py, float cx, float cy, float hx, float hy, float cr)
		{
			const float qx = std::fabs(px - cx) - (hx - cr);
			const float qy = std::fabs(py - cy) - (hy - cr);
			const float ox = std::max(qx, 0.0f);
			const float oy = std::max(qy, 0.0f);
			return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - cr;
		}

		// Distance to the segment a-b.
		float SegmentDistance(float px, float py, float ax, float ay, float bx, float by)
		{
			const float vx = bx - ax, vy = by - ay;
			const float wx = px - ax, wy = py - ay;
			const float vv = vx * vx + vy * vy;
			const float t = (vv > 1.0e-12f) ? std::clamp((wx * vx + wy * vy) / vv, 0.0f, 1.0f) : 0.0f;
			const float dx = wx - t * vx, dy = wy - t * vy;
			return std::sqrt(dx * dx + dy * dy);
		}

		// How much of the pixel a mark covers as a line (line) and as a fill (fill), at (px, py) from its middle in
		// pixels, y up; reach_px is its projected radius (a ring) or half height (the bag), w the line width.
		void Cover(const Mark& m, float px, float py, float reach_px, float w, float* line, float* fill)
		{
			if (m.shape == Shape::Ring)
			{
				const float d = std::sqrt(px * px + py * py);
				*line = LineCover(d - reach_px, w);
				*fill = Clamp01(reach_px - d + 0.5f);
				return;
			}
			// The bag, in its half heights R: a body with rounded corners, a flap closing in a V over its top, a buckle where
			// the V meets, and a loop over the body.
			const float R = reach_px;
			const float body = BoxDistance(px, py, 0.0f, -0.25f * R, 1.0f * R, 0.75f * R, 0.28f * R);
			float l = LineCover(body, w);
			l = std::max(l, LineCover(SegmentDistance(px, py, -1.0f * R, 0.2f * R, 0.0f, -0.18f * R), w));
			l = std::max(l, LineCover(SegmentDistance(px, py, 0.0f, -0.18f * R, 1.0f * R, 0.2f * R), w));
			const float buckle = BoxDistance(px, py, 0.0f, -0.3f * R, 0.14f * R, 0.12f * R, 0.03f * R);
			l = std::max(l, Clamp01(0.5f - buckle));
			if (py >= 0.5f * R)
			{
				const float lx = px, ly = py - 0.5f * R;
				l = std::max(l, LineCover(std::sqrt(lx * lx + ly * ly) - 0.36f * R, w));
			}
			*line = l;
			*fill = Clamp01(0.5f - body);
		}

		u32 Pack(u32 r, u32 g, u32 b, float a)
		{
			const u32 a8 = static_cast<u32>(std::lround(Clamp01(a) * 255.0f));
			return (a8 << 24) | (b << 16) | (g << 8) | r;
		}

		// The quad's frame from its quaternion's basis vectors (Quad keeps right and up; n = right x up).
		void QuadFrame(const Quad& q, Vec3* c, Vec3* r, Vec3* u, Vec3* n)
		{
			*c = Load(q.centre);
			*r = Normalize(Load(q.right));
			*n = Normalize(Cross(*r, Load(q.up)));
			*u = Cross(*n, *r);
		}
	}

	Zone ZoneOf(std::string_view control_id)
	{
		const auto is = [control_id](std::string_view name) {
			if (control_id.size() != name.size())
				return false;
			for (size_t i = 0; i < name.size(); i++)
			{
				char ch = control_id[i];
				if (ch >= 'A' && ch <= 'Z')
					ch = static_cast<char>(ch - 'A' + 'a');
				if (ch != name[i])
					return false;
			}
			return true;
		};
		if (is("holster"))
			return Zone::Holster;
		if (is("belt"))
			return Zone::Belt;
		if (is("backpack"))
			return Zone::Backpack;
		return Zone::Count;
	}

	bool LongGunEquipped()
	{
		if (GameFeedback::WeaponInHand() != GameFeedback::Hold::Weapon)
			return false;
		const GameFeedback::WeaponView wv = GameFeedback::GetWeapon();
		return wv.valid && ItemData::KindOf(wv.item_id) == ItemData::Kind::LongGun;
	}

	bool RouteToLongGun(bool pressed, bool long_gun, PressRoute& st)
	{
		if (!pressed)
		{
			st = PressRoute{};
			return false;
		}
		if (!st.pressing)
		{
			st.pressing = true;
			st.long_gun = long_gun;
		}
		return st.long_gun;
	}

	void PublishZone(Zone zone, const ZoneView& view)
	{
		const int i = static_cast<int>(zone);
		if (i < 0 || i >= static_cast<int>(Zone::Count))
			return;
		const u64 now_ms = CameraDriver::SteadyNowMs();
		std::lock_guard<std::mutex> lock(s_zone_mutex);
		s_zone[i].view = view;
		s_zone[i].ms = (now_ms != 0) ? now_ms : 1;
	}

	bool GetZone(Zone zone, ZoneView* out)
	{
		const int i = static_cast<int>(zone);
		if (i < 0 || i >= static_cast<int>(Zone::Count))
			return false;
		Published p;
		{
			std::lock_guard<std::mutex> lock(s_zone_mutex);
			p = s_zone[i];
		}
		const u64 now_ms = CameraDriver::SteadyNowMs();
		if (p.ms == 0 || (now_ms > p.ms && now_ms - p.ms > kStaleMs) || !p.view.armed)
			return false;
		if (out)
			*out = p.view;
		return true;
	}

	bool ZoneCentre(const VRInputSnapshot& snap, const SpatialControls::ZoneParams& p, SpatialControls::ZoneState& yaw_state,
		Vec3* centre, float* yaw)
	{
		SpatialControls::Anchor head;
		if (!SpatialControls::HeadAnchor(snap, yaw_state, &head))
			return false;
		// As StepZone places it: the offset's own yaw is not used.
		const SpatialControls::Placement at{p.offset.side, p.offset.height, p.offset.forward, 0.0f};
		const Vec3 c = SpatialControls::PlaceControl(head, at).pivot;
		if (!Finite(c) || !std::isfinite(head.yaw))
			return false;
		if (centre)
			*centre = c;
		if (yaw)
			*yaw = head.yaw;
		return true;
	}

	bool ClearOfView(const float eye[3], const float head_quat[4], const float point[3])
	{
		if (!Finite(eye, 3) || !Finite(head_quat, 4) || !Finite(point, 3))
			return false;
		const Vec3 fwd = UnitQuat(head_quat).Rotate(Vec3{0.0f, 0.0f, -1.0f});
		// Looking down at the body, where the markers are: anything goes.
		const float down_deg = std::asin(std::clamp(-fwd.y, -1.0f, 1.0f)) / kDegToRad;
		if (down_deg > kLookDownDeg)
			return true;
		const Vec3 to = Load(point) - Load(eye);
		const float len = SpatialControls::Length(to);
		if (!(len > 1.0e-4f))
			return false;
		return SpatialControls::Dot(to, fwd) / len < std::cos(kClearDeg * kDegToRad);
	}

	std::array<bool, kSlots> Plan(u32 free_layers, const std::array<bool, kSlots>& want)
	{
		std::array<bool, kSlots> go{};
		// In order of what matters most, so the last ones go first when layers are short.
		static constexpr int kOrder[kSlots] = {kHolster, kPartner, kBelt, kPouches, kTarget};
		for (const int slot : kOrder)
		{
			if (!want[static_cast<size_t>(slot)] || free_layers == 0)
				continue;
			go[static_cast<size_t>(slot)] = true;
			free_layers--;
		}
		return go;
	}

	bool PlaceMarks(const Mark* marks, int count, const float eye[3], Quad* out)
	{
		if (!marks || !out || count <= 0 || count > kMaxMarks || !Finite(eye, 3))
			return false;
		Vec3 mid{0.0f, 0.0f, 0.0f};
		for (int i = 0; i < count; i++)
		{
			if (!Finite(marks[i].centre, 3) || !std::isfinite(marks[i].radius) || !(marks[i].radius > 0.0f))
				return false;
			mid = mid + Load(marks[i].centre);
		}
		mid = mid * (1.0f / static_cast<float>(count));
		const Vec3 e = Load(eye);
		const Vec3 to_eye = e - mid;
		const float dist = SpatialControls::Length(to_eye);
		if (!(dist > 1.0e-3f) || !std::isfinite(dist))
			return false;
		// Facing the eye, "up" as near world up as it goes (-Z looking straight down on it).
		const Vec3 n = to_eye * (1.0f / dist);
		Vec3 up_ref{0.0f, 1.0f, 0.0f};
		if (std::fabs(SpatialControls::Dot(up_ref, n)) > 0.97f)
			up_ref = Vec3{0.0f, 0.0f, -1.0f};
		const Vec3 u = Normalize(up_ref - n * SpatialControls::Dot(up_ref, n));
		const Vec3 r = Cross(u, n);

		float x0 = std::numeric_limits<float>::max();
		float y0 = std::numeric_limits<float>::max();
		float x1 = -std::numeric_limits<float>::max();
		float y1 = -std::numeric_limits<float>::max();
		for (int i = 0; i < count; i++)
		{
			OnPlane p;
			if (!ProjectOnPlane(e, mid, r, u, n, Load(marks[i].centre), &p))
				return false;
			const float reach = ReachOnPlane(marks[i], p);
			if (!(reach > 0.0f) || !std::isfinite(reach))
				return false;
			const float rr = reach + 2.0f * kLineM * p.s;
			x0 = std::min(x0, p.x - rr);
			x1 = std::max(x1, p.x + rr);
			y0 = std::min(y0, p.y - rr);
			y1 = std::max(y1, p.y + rr);
		}
		const float size = std::max(x1 - x0, y1 - y0) * kFitMargin;
		if (!std::isfinite(size))
			return false;
		const Vec3 c = mid + r * (0.5f * (x0 + x1)) + u * (0.5f * (y0 + y1));
		Store(c, out->centre);
		Store(r, out->right);
		Store(u, out->up);
		QuatFromBasis(r, u, n, out->quat);
		out->size_m = std::clamp(size, kMinQuadM, kMaxQuadM);
		return true;
	}

	namespace
	{
		// Hand targeting's box: the eight Box marks (the floor quad's corners, then the same four at the top) joined by the
		// twelve edges of a box, each a line kLineM wide at its corner (kMinLinePx to kMaxLinePx in the image) in the dim
		// colour at kRingAlpha, or in the lit colour, brighter, as the marks' glow says (1 near, 2 inside). Nothing is
		// drawn when a corner cannot be projected (one nearer the eye than 5 cm along the view): a half box would mislead.
		void RasterBox(const Mark* marks, int count, const float eye[3], const Quad& quad, std::vector<u32>& out_rgba)
		{
			if (count != 8)
				return;
			Vec3 c, r, u, n;
			QuadFrame(quad, &c, &r, &u, &n);
			const Vec3 e = Load(eye);
			const float px_per_m = static_cast<float>(kImageSize) / quad.size_m;
			const float half = 0.5f * static_cast<float>(kImageSize);
			float xs[8];
			float ys[8];
			float ws[8];
			for (int i = 0; i < 8; i++)
			{
				OnPlane p;
				if (!Finite(marks[i].centre, 3) || !ProjectOnPlane(e, c, r, u, n, Load(marks[i].centre), &p))
					return;
				xs[i] = half + p.x * px_per_m;
				ys[i] = half - p.y * px_per_m;
				ws[i] = std::clamp(kLineM * p.s * px_per_m, kMinLinePx, kMaxLinePx);
			}
			// A corner far off the image (a plane nearly edge-on) is kept for the line's direction, but the pixel bounds are
			// clamped before the conversion to int (review a8); NaN never reaches here (ProjectOnPlane, Finite).
			const auto to_px = [](float f) { return static_cast<int>(std::clamp(f, -1.0e6f, 1.0e6f)); };
			static constexpr float kBoxAlpha[3] = {kRingAlpha, 0.62f, 0.95f};
			const int glow = std::clamp<int>(marks[0].glow, 0, 2);
			const float line_alpha = kBoxAlpha[glow];
			const u32 cr = (glow > 0) ? kLitR : kDimR;
			const u32 cg = (glow > 0) ? kLitG : kDimG;
			const u32 cb = (glow > 0) ? kLitB : kDimB;
			static constexpr int kEdges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5},
				{2, 6}, {3, 7}};
			for (const auto& edge : kEdges)
			{
				const int a = edge[0];
				const int b = edge[1];
				const float w = 0.5f * (ws[a] + ws[b]);
				const float reach = w + 1.0f;
				const int bx0 = std::max(0, to_px(std::floor(std::min(xs[a], xs[b]) - reach)));
				const int bx1 = std::min(kSize - 1, to_px(std::ceil(std::max(xs[a], xs[b]) + reach)));
				const int by0 = std::max(0, to_px(std::floor(std::min(ys[a], ys[b]) - reach)));
				const int by1 = std::min(kSize - 1, to_px(std::ceil(std::max(ys[a], ys[b]) + reach)));
				for (int py = by0; py <= by1; py++)
				{
					u32* row = out_rgba.data() + static_cast<size_t>(py) * kImageSize;
					for (int px = bx0; px <= bx1; px++)
					{
						const float d = SegmentDistance(static_cast<float>(px) + 0.5f, static_cast<float>(py) + 0.5f, xs[a], ys[a], xs[b], ys[b]);
						const float alpha = LineCover(d, w) * line_alpha;
						// Where lines overlap, the stronger one shows.
						if (alpha > static_cast<float>(row[px] >> 24) / 255.0f + 1.0e-3f)
							row[px] = Pack(cr, cg, cb, alpha);
					}
				}
			}
		}
	}

	void RasterMarks(const Mark* marks, int count, const float eye[3], const Quad& quad, std::vector<u32>& out_rgba)
	{
		out_rgba.assign(kPixels, 0u);
		if (!marks || count <= 0 || !Finite(eye, 3) || !Finite(quad.centre, 3) || !Finite(quad.right, 3) ||
			!Finite(quad.up, 3) || !(quad.size_m > 0.0f))
		{
			return;
		}
		if (marks[0].shape == Shape::Box)
		{
			RasterBox(marks, count, eye, quad, out_rgba);
			return;
		}
		Vec3 c, r, u, n;
		QuadFrame(quad, &c, &r, &u, &n);
		const Vec3 e = Load(eye);
		const float px_per_m = static_cast<float>(kImageSize) / quad.size_m;
		const float half = 0.5f * static_cast<float>(kImageSize);
		for (int i = 0; i < count && i < kMaxMarks; i++)
		{
			const Mark& m = marks[i];
			if (!Finite(m.centre, 3) || !(m.radius > 0.0f))
				continue;
			OnPlane p;
			if (!ProjectOnPlane(e, c, r, u, n, Load(m.centre), &p))
				continue;
			const float reach_m = ReachOnPlane(m, p);
			if (!(reach_m > 0.0f))
				continue;
			// Ring: its reach on the plane is the radius drawn; the bag: its half height.
			const float shape_px = ((m.shape == Shape::Bag) ? (m.radius * p.s) : reach_m) * px_per_m;
			const float w = std::clamp(kLineM * p.s * px_per_m, kMinLinePx, kMaxLinePx);
			const float cx = half + p.x * px_per_m;
			const float cy = half - p.y * px_per_m;
			const float extent = reach_m * px_per_m + w + 1.0f;
			const int bx0 = std::max(0, static_cast<int>(std::floor(cx - extent)));
			const int bx1 = std::min(kSize - 1, static_cast<int>(std::ceil(cx + extent)));
			const int by0 = std::max(0, static_cast<int>(std::floor(cy - extent)));
			const int by1 = std::min(kSize - 1, static_cast<int>(std::ceil(cy + extent)));
			const float line_alpha = m.lit ? kLitAlpha : ((m.shape == Shape::Bag) ? kBagAlpha : kRingAlpha);
			const u32 cr = m.lit ? kLitR : kDimR;
			const u32 cg = m.lit ? kLitG : kDimG;
			const u32 cb = m.lit ? kLitB : kDimB;
			for (int y = by0; y <= by1; y++)
			{
				u32* row = out_rgba.data() + static_cast<size_t>(y) * kImageSize;
				for (int x = bx0; x <= bx1; x++)
				{
					float line = 0.0f;
					float fill = 0.0f;
					Cover(m, static_cast<float>(x) + 0.5f - cx, cy - (static_cast<float>(y) + 0.5f), shape_px, w, &line, &fill);
					const float a = std::max(line * line_alpha, m.lit ? fill * kFillAlpha : 0.0f);
					// Where marks overlap, the stronger one shows.
					if (a > static_cast<float>(row[x] >> 24) / 255.0f + 1.0e-3f)
						row[x] = Pack(cr, cg, cb, a);
				}
			}
		}
	}

	Drawn Describe(const float frame_pos[3], const float frame_quat[4], const float eye[3], const Quad& quad, u64 key,
		const Mark* marks, int count)
	{
		Drawn d;
		d.valid = true;
		for (int k = 0; k < 3; k++)
		{
			d.frame_pos[k] = frame_pos[k];
			d.eye[k] = eye[k];
		}
		for (int k = 0; k < 4; k++)
			d.frame_quat[k] = frame_quat[k];
		d.quad = quad;
		d.key = key;
		d.count = std::clamp(count, 0, kMaxMarks);
		const Quat inv = UnitQuat(frame_quat).Conjugate();
		for (int i = 0; i < d.count; i++)
			Store(inv.Rotate(Load(marks[i].centre) - Load(frame_pos)), d.local[i]);
		return d;
	}

	bool NeedsRedraw(const Drawn& d, const float frame_pos[3], const float frame_quat[4], const float eye[3], u64 key,
		const Mark* marks, int count)
	{
		if (!d.valid || d.key != key || d.count != std::clamp(count, 0, kMaxMarks))
			return true;
		const Quat inv = UnitQuat(frame_quat).Conjugate();
		for (int i = 0; i < d.count; i++)
		{
			const Vec3 local = inv.Rotate(Load(marks[i].centre) - Load(frame_pos));
			if (!(SpatialControls::Length(local - Load(d.local[i])) <= kMarkMoveM))
				return true;
		}
		const HandModel::ViewDrift drift =
			HandModel::MeasureViewDrift(d.frame_pos, d.frame_quat, d.eye, d.quad.centre, frame_pos, frame_quat, eye);
		return !drift.valid || drift.angle_deg > HandModel::kRerasterDriftDeg ||
		       std::fabs(drift.distance_ratio - 1.0f) > HandModel::kRerasterDistanceRatio;
	}

	void PlaceKept(const Drawn& d, const float frame_pos[3], const float frame_quat[4], float out_centre[3],
		float out_quat[4])
	{
		HandModel::RePoseRigid(d.frame_pos, d.frame_quat, frame_pos, frame_quat, d.quad.centre, d.quad.quat, out_centre,
			out_quat);
	}

	bool SelfTest()
	{
		int checks = 0;
		int failed = 0;
		const auto check = [&checks, &failed](bool ok, const char* name) {
			checks++;
			if (!ok)
			{
				failed++;
				Console.Error("(VR) Zone markers self-test FAIL: %s", name);
			}
		};
		const auto near_v = [](const Vec3& a, const Vec3& b, float tol) { return SpatialControls::Length(a - b) <= tol; };
		// The head: eyes at (0.1, 1.2, -0.3), facing yaw, tipped down by pitch_deg.
		const auto head_snap = [](float yaw, float pitch_deg) {
			VRInputSnapshot snap;
			snap.generation = 1;
			snap.actions_active = true;
			const Quat q = Quat::FromYaw(yaw) * Quat{std::sin(-0.5f * pitch_deg * kDegToRad), 0.0f, 0.0f,
				std::cos(-0.5f * pitch_deg * kDegToRad)};
			snap.head_pose.orientation_xyzw = {q.x, q.y, q.z, q.w};
			snap.head_pose.position_xyz = {0.1f, 1.2f, -0.3f};
			snap.head_pose.valid = true;
			return snap;
		};
		const Vec3 eyes{0.1f, 1.2f, -0.3f};

		check(ZoneOf("holster") == Zone::Holster && ZoneOf("Belt") == Zone::Belt && ZoneOf("backpack") == Zone::Backpack &&
				  ZoneOf("") == Zone::Count && ZoneOf("holsters") == Zone::Count,
			"the holster, belt and backpack Zone controls are found by id");

		// --- Where a press goes -------------------------------------------------------------------------------------
		{
			PressRoute route;
			const bool first = RouteToLongGun(true, true, route);
			const bool held = RouteToLongGun(true, false, route); // the weapon changed mid-squeeze
			const bool released = RouteToLongGun(false, false, route);
			const bool next = RouteToLongGun(true, false, route);
			const bool later = RouteToLongGun(true, true, route); // a long gun equipped mid-squeeze
			check(first && held && !released, "a press begun with a long gun goes out on PressedLongGun until it ends");
			check(!next && !later, "a press begun with anything else stays on Pressed, even if a long gun comes mid-squeeze");
		}

		// --- The marker is where StepZone tests the hand ------------------------------------------------------------
		// The shipped Outbreak holster (both Files): 0.22 right, 0.50 down, 0.12 ahead of the eyes, a 20 cm ball, the right
		// hand.
		SpatialControls::ZoneParams holster;
		holster.offset = SpatialControls::Placement{0.22f, -0.50f, 0.12f, 0.0f};
		holster.radius = 0.20f;
		holster.hand = SpatialControls::ZoneHand::Right;
		const float yaw0 = 0.4f;
		{
			const VRInputSnapshot snap = head_snap(yaw0, 30.0f);
			SpatialControls::ZoneState keep;
			Vec3 c;
			float yaw = 0.0f;
			const bool placed = ZoneCentre(snap, holster, keep, &c, &yaw);
			check(placed && std::fabs(yaw - yaw0) < 1.0e-4f, "a zone marker is placed from a head looking down, with its yaw");
			// The same zone stepped as VRInputSource steps it: HeadAnchor, then StepZone with the right hand.
			const auto inside_at = [&](const Vec3& p) {
				SpatialControls::ZoneState st;
				SpatialControls::Anchor a;
				if (!SpatialControls::HeadAnchor(snap, st, &a))
					return false;
				SpatialControls::Hands hands;
				hands.hand[1].valid = true;
				hands.hand[1].position = p;
				SpatialControls::StepZone(a, holster, hands, st);
				return st.inside[1];
			};
			const Vec3 out_dir = Normalize(Vec3{0.3f, -0.4f, 0.5f});
			check(placed && inside_at(c), "a right hand at the holster marker's middle is in the holster zone");
			check(placed && inside_at(c + out_dir * 0.19f), "a right hand 19 cm from the marker is still in the zone");
			check(placed && !inside_at(c + out_dir * 0.21f), "a right hand 21 cm from the marker (past the 20 cm radius) is not");

			// Looking ahead instead puts it in the same place; turning the head turns it round the eyes.
			const VRInputSnapshot level = head_snap(yaw0, 0.0f);
			SpatialControls::ZoneState keep_level;
			Vec3 c_level;
			check(ZoneCentre(level, holster, keep_level, &c_level, nullptr) && near_v(c, c_level, 1.0e-4f),
				"looking down at the holster does not move its marker (the zones turn with yaw only)");
			const VRInputSnapshot turned = head_snap(yaw0 + 0.5f, 30.0f);
			SpatialControls::ZoneState keep_turned;
			Vec3 c_turned;
			const Vec3 want = eyes + Quat::FromYaw(0.5f).Rotate(c - eyes);
			check(ZoneCentre(turned, holster, keep_turned, &c_turned, nullptr) && near_v(c_turned, want, 1.0e-4f),
				"turning the head 0.5 rad turns the holster marker round the eyes with it");
			VRInputSnapshot no_head = snap;
			no_head.head_pose.valid = false;
			SpatialControls::ZoneState keep_none;
			check(!ZoneCentre(no_head, holster, keep_none, &c, nullptr), "no head pose: no zone marker");
		}

		// --- Out of the middle of the view --------------------------------------------------------------------------
		{
			const float level[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // looking along -Z
			const float e[3] = {0.0f, 1.2f, 0.0f};
			const float ahead[3] = {0.0f, 1.2f, -1.0f};
			const float off10[3] = {std::sin(10.0f * kDegToRad), 1.2f, -std::cos(10.0f * kDegToRad)};
			const float off30[3] = {std::sin(30.0f * kDegToRad), 1.2f, -std::cos(30.0f * kDegToRad)};
			const float hip[3] = {0.22f, 0.70f, -0.12f};
			check(!ClearOfView(e, level, ahead) && !ClearOfView(e, level, off10),
				"looking ahead, nothing is drawn within 20 deg of the line of sight");
			check(ClearOfView(e, level, off30) && ClearOfView(e, level, hip), "looking ahead, the hip and 30 deg off are drawn");
			const float half = -0.5f * 40.0f * kDegToRad; // 40 deg down: about +X by -40 deg
			const float down[4] = {std::sin(half), 0.0f, 0.0f, std::cos(half)};
			const float gaze[3] = {0.0f, 1.2f - std::sin(40.0f * kDegToRad), -std::cos(40.0f * kDegToRad)};
			check(ClearOfView(e, down, gaze), "looking 40 deg down at the body, a marker in the middle of the view is drawn");
		}

		// --- The layer budget ---------------------------------------------------------------------------------------
		{
			const std::array<bool, kSlots> all{{true, true, true}};
			const auto is = [](const std::array<bool, kSlots>& a, bool h, bool b, bool p) {
				return a[kHolster] == h && a[kBelt] == b && a[kPouches] == p;
			};
			check(is(Plan(3, all), true, true, true) && is(Plan(9, all), true, true, true), "room for all: all three go");
			check(is(Plan(2, all), true, true, false), "two layers free: the pouches' rings go first");
			check(is(Plan(1, all), true, false, false), "one layer free: the holster stays, the markers go");
			check(is(Plan(0, all), false, false, false), "no layer free: nothing");
			check(is(Plan(1, std::array<bool, kSlots>{{false, true, true}}), false, true, false),
				"no holster wanted: one layer goes to the belt's bag");
		}

		// --- The holster's pose and image ---------------------------------------------------------------------------
		{
			const float centre[3] = {0.3f, 0.7f, -0.4f};
			float pos[3];
			float quat[4];
			const bool posed = HandModel::HolsterPose(HandModel::Held::Pistol, centre, 0.0f, pos, quat);
			const Quat q = UnitQuat(quat);
			const Vec3 barrel = q.Rotate(Vec3{0.0f, 0.0f, -1.0f});
			const Vec3 top = q.Rotate(Vec3{0.0f, 1.0f, 0.0f});
			const Vec3 side = q.Rotate(Vec3{1.0f, 0.0f, 0.0f});
			// The grip runs from the slide down the handle, whose top leans 15 deg forward in the hand.
			const Vec3 grip = q.Rotate(Vec3{0.0f, -std::cos(15.0f * kDegToRad), std::sin(15.0f * kDegToRad)});
			check(posed && near_v(Load(pos), Load(centre), 1.0e-6f), "the holstered weapon's grip position is the zone's middle");
			check(posed && barrel.y < -0.9f && barrel.z > 0.2f, "holstered, the muzzle points down and a little back");
			check(posed && grip.y > 0.3f, "holstered, the grip stands up toward the hand");
			check(posed && top.z < -0.9f && side.x > 0.99f, "holstered, the gun's top faces the front and its right side out");
			float quat_t[4];
			float pos_t[3];
			HandModel::HolsterPose(HandModel::Held::Pistol, centre, 0.7f, pos_t, quat_t);
			check(near_v(UnitQuat(quat_t).Rotate(Vec3{0.0f, 0.0f, -1.0f}), Quat::FromYaw(0.7f).Rotate(barrel), 1.0e-4f),
				"the holster turns with the body's yaw");
			float quat_s[4];
			HandModel::HolsterPose(HandModel::Held::Spray, centre, 0.0f, pos_t, quat_s);
			const Vec3 can_axis =
				UnitQuat(quat_s).Rotate(Vec3{0.0f, std::cos(15.0f * kDegToRad), -std::sin(15.0f * kDegToRad)});
			check(can_axis.y > 0.999f, "a spray can (or something to throw) stands upright on the belt");
			const float bad[3] = {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
			check(!HandModel::HolsterPose(HandModel::Held::Pistol, bad, 0.0f, pos_t, quat_t), "a centre that is not finite: no pose");

			// Seen from eyes 0.5 m above and to the left, as from the head: placed, drawn, no glove, brighter when lit.
			const float eye[3] = {0.08f, 1.2f, -0.28f};
			HandModel::HandState hs;
			hs.valid = true;
			hs.gun = true;
			hs.holstered = true;
			hs.held = HandModel::Held::Pistol;
			hs.item_id = 400;
			for (int k = 0; k < 3; k++)
				hs.pos[k] = pos[k];
			for (int k = 0; k < 4; k++)
				hs.quat[k] = quat[k];
			HandModel::ImpostorQuad iq;
			const bool placed = HandModel::PlaceImpostor(HandModel::kRight, hs, eye, &iq);
			check(placed && iq.size_m >= HandModel::kQuadSizeHolsterM - 1.0e-4f &&
					  iq.size_m <= 2.0f * HandModel::kQuadSizeHolsterM + 1.0e-4f,
				"a holstered pistol's quad is placed, between 0.25 and 0.5 m");
			std::vector<u32> px;
			// The image's covered pixels into *covered, and the sum of their colour bytes.
			const auto sum = [&px](u32* covered) {
				double total = 0.0;
				u32 n = 0;
				for (const u32 v : px)
				{
					if ((v >> 24) == 0)
						continue;
					n++;
					total += static_cast<double>((v & 0xFFu) + ((v >> 8) & 0xFFu) + ((v >> 16) & 0xFFu));
				}
				*covered = n;
				return total;
			};
			u32 opaque = 0;
			u32 opaque_lit = 0;
			double dim = 0.0;
			double bright = 0.0;
			if (placed)
			{
				HandModel::RenderImpostor(HandModel::kRight, hs, eye, iq.centre, iq.right, iq.up, iq.size_m, px);
				dim = sum(&opaque);
				hs.lit = true;
				HandModel::RenderImpostor(HandModel::kRight, hs, eye, iq.centre, iq.right, iq.up, iq.size_m, px);
				bright = sum(&opaque_lit);
				hs.lit = false;
			}
			check(opaque > 400 && opaque == opaque_lit, "the holstered pistol draws a silhouette, the same lit or not");
			// 1.45 times the light is about 1.17 times the sRGB bytes on these dark materials.
			check(bright > dim * 1.08, "lit (the right hand in reach), the holstered pistol is brighter");
			// The same pose held in the hand draws the glove round it: more of the image is covered.
			HandModel::HandState in_hand = hs;
			in_hand.holstered = false;
			HandModel::ImpostorQuad iq_hand;
			u32 opaque_hand = 0;
			if (HandModel::PlaceImpostor(HandModel::kRight, in_hand, eye, &iq_hand))
			{
				// Drawn on the holster's quad, so the two are measured alike.
				HandModel::RenderImpostor(HandModel::kRight, in_hand, eye, iq.centre, iq.right, iq.up, iq.size_m, px);
				sum(&opaque_hand);
			}
			check(opaque_hand > opaque, "holstered, the weapon is drawn without the glove");
			check(HandModel::NeedsReraster(hs, eye, iq.centre, in_hand, eye, 0),
				"a weapon going from the holster to the hand is drawn again");

			// A long pole hangs at the hip too, its quad up to the long size.
			{
				HandModel::HandState pole = hs;
				pole.held = HandModel::Held::Melee;
				pole.item_id = 106;
				HandModel::HolsterPose(pole.held, centre, 0.0f, pole.pos, pole.quat);
				HandModel::ImpostorQuad pq;
				u32 n = 0;
				if (HandModel::PlaceImpostor(HandModel::kRight, pole, eye, &pq) && pq.size_m <= HandModel::kQuadSizeLongM + 1.0e-4f)
				{
					HandModel::RenderImpostor(HandModel::kRight, pole, eye, pq.centre, pq.right, pq.up, pq.size_m, px);
					sum(&n);
				}
				check(n > 200, "a holstered long pole is placed and drawn");
			}

			// A long gun slung over the right shoulder: the barrel up behind it, leaning out and back, turning with the yaw.
			float sling_pos[3];
			float sling_quat[4];
			const bool slung = HandModel::SlingPose(centre, 0.0f, sling_pos, sling_quat);
			const Vec3 sling_barrel = UnitQuat(sling_quat).Rotate(Vec3{0.0f, 0.0f, -1.0f});
			const Vec3 sling_side = UnitQuat(sling_quat).Rotate(Vec3{1.0f, 0.0f, 0.0f});
			check(slung && near_v(Load(sling_pos), Load(centre), 1.0e-6f) && sling_barrel.y > 0.9f && sling_barrel.x > 0.1f &&
					  sling_barrel.z > 0.2f && sling_side.x > 0.9f,
				"slung, a long gun's barrel stands up behind the shoulder, leaning out, its right side out");
			float sling_quat_t[4];
			HandModel::SlingPose(centre, 0.7f, pos_t, sling_quat_t);
			check(near_v(UnitQuat(sling_quat_t).Rotate(Vec3{0.0f, 0.0f, -1.0f}), Quat::FromYaw(0.7f).Rotate(sling_barrel), 1.0e-4f),
				"the sling turns with the body's yaw");
			check(!HandModel::SlingPose(bad, 0.0f, pos_t, sling_quat_t), "a sling centre that is not finite: no pose");
			// Seen from the eyes 18 cm away (the shipped backpack zone: 0.15 right, 0.05 down, 0.08 behind them), with its
			// barrel passing beside the head: the shotgun is still placed and drawn (its grip is what the eye stays out of).
			{
				const float back[3] = {eye[0] + 0.15f, eye[1] - 0.05f, eye[2] + 0.08f};
				HandModel::HandState gun = hs;
				gun.held = HandModel::Held::LongGun;
				gun.item_id = 11;
				HandModel::SlingPose(back, 0.0f, gun.pos, gun.quat);
				HandModel::ImpostorQuad gq;
				u32 n = 0;
				if (HandModel::PlaceImpostor(HandModel::kRight, gun, eye, &gq) && gq.size_m <= HandModel::kQuadSizeLongM + 1.0e-4f)
				{
					HandModel::RenderImpostor(HandModel::kRight, gun, eye, gq.centre, gq.right, gq.up, gq.size_m, px);
					sum(&n);
				}
				check(n > 100, "a shotgun slung over the shoulder, beside the head, is placed and drawn");
			}
		}

		// --- The pouches' rings and the belt's bag ------------------------------------------------------------------
		{
			// The shipped File #1 rig, as the seat places it for eyes at `eyes` facing yaw 0 (side, height, -forward).
			const float rig[5][3] = {{-0.10f, -0.20f, -0.14f}, {0.10f, -0.20f, -0.14f}, {-0.07f, -0.29f, -0.24f},
				{0.07f, -0.29f, -0.24f}, {0.0f, -0.40f, -0.28f}};
			Mark marks[5];
			for (int i = 0; i < 5; i++)
			{
				for (int k = 0; k < 3; k++)
					marks[i].centre[k] = (k == 0 ? eyes.x : k == 1 ? eyes.y : eyes.z) + rig[i][k];
				marks[i].radius = 0.065f;
				marks[i].shape = Shape::Ring;
			}
			const float eye[3] = {eyes.x, eyes.y, eyes.z};
			Quad q;
			const bool placed = PlaceMarks(marks, 5, eye, &q);
			check(placed && q.size_m > 0.1f && q.size_m <= kMaxQuadM, "the five pouch rings are placed on one quad");
			std::vector<u32> px;
			bool edge_clear = true;
			u32 drawn = 0;
			if (placed)
			{
				RasterMarks(marks, 5, eye, q, px);
				for (int i = 0; i < kSize; i++)
				{
					const u32 edge = px[static_cast<size_t>(i)] | px[static_cast<size_t>(kSize - 1) * kImageSize + i] |
					                 px[static_cast<size_t>(i) * kImageSize] |
					                 px[static_cast<size_t>(i) * kImageSize + kSize - 1];
					edge_clear = edge_clear && (edge >> 24) == 0;
				}
				for (const u32 v : px)
					drawn += ((v >> 24) != 0) ? 1u : 0u;
			}
			check(placed && edge_clear && drawn > 500, "every ring lies inside its quad, none touching the edge");

			// Where ring 1 is seen, its outline: drawn on its edge, clear in its middle; lit, brighter, with a faint fill.
			const auto alpha_at = [&](const Mark& m, float frac, bool* ok) {
				Vec3 c, r, u, n;
				QuadFrame(q, &c, &r, &u, &n);
				OnPlane p;
				*ok = ProjectOnPlane(Load(eye), c, r, u, n, Load(m.centre), &p);
				const float ppm = static_cast<float>(kImageSize) / q.size_m;
				const float reach = ReachOnPlane(m, p);
				// Toward the image's right: the neighbours are farther off along the up/down and to the left.
				const int x = static_cast<int>(std::floor(0.5f * kImageSize + (p.x + frac * reach) * ppm));
				const int y = static_cast<int>(std::floor(0.5f * kImageSize - p.y * ppm));
				*ok = *ok && x >= 0 && x < kSize && y >= 0 && y < kSize;
				return *ok ? (px[static_cast<size_t>(y) * kImageSize + x] >> 24) : 0u;
			};
			bool ok_edge = false, ok_mid = false;
			const u32 edge_dim = alpha_at(marks[1], 1.0f, &ok_edge);
			const u32 mid_dim = alpha_at(marks[1], 0.0f, &ok_mid);
			check(ok_edge && ok_mid && edge_dim > 40 && edge_dim < 120 && mid_dim == 0,
				"an unlit ring is a faint outline, clear in its middle");
			marks[1].lit = true;
			RasterMarks(marks, 5, eye, q, px);
			const u32 edge_lit = alpha_at(marks[1], 1.0f, &ok_edge);
			const u32 mid_lit = alpha_at(marks[1], 0.0f, &ok_mid);
			check(ok_edge && ok_mid && edge_lit > 2 * edge_dim && mid_lit > 0 && mid_lit < 60,
				"a lit ring (a hand in that pouch) is brighter, with a faint fill");
			marks[1].lit = false;

			Mark bag;
			bag.centre[0] = eyes.x - 0.22f;
			bag.centre[1] = eyes.y - 0.50f;
			bag.centre[2] = eyes.z - 0.12f;
			bag.radius = 0.5f * kBagHeightM;
			bag.shape = Shape::Bag;
			Quad bq;
			u32 bag_max = 0;
			u32 bag_px = 0;
			if (PlaceMarks(&bag, 1, eye, &bq))
			{
				RasterMarks(&bag, 1, eye, bq, px);
				for (const u32 v : px)
				{
					bag_max = std::max(bag_max, v >> 24);
					bag_px += ((v >> 24) != 0) ? 1u : 0u;
				}
			}
			check(bag_px > 300 && bag_max > 80 && bag_max < 130, "the belt's bag is drawn small and dim");
			bag.lit = true;
			u32 bag_lit_max = 0;
			RasterMarks(&bag, 1, eye, bq, px);
			for (const u32 v : px)
				bag_lit_max = std::max(bag_lit_max, v >> 24);
			check(bag_lit_max > 200, "a lit bag (the left hand at the belt) is bright");
			check(!PlaceMarks(&bag, 0, eye, &bq) && !PlaceMarks(&bag, 1, bag.centre, &bq), "no marks, or the eye at one: no quad");

			// --- Drawn again only when needed ---------------------------------------------------------------------
			const float frame_pos[3] = {eyes.x, eyes.y - 0.3f, eyes.z - 0.2f};
			const float frame_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
			const Drawn d = Describe(frame_pos, frame_quat, eye, q, 0x1Full, marks, 5);
			check(!NeedsRedraw(d, frame_pos, frame_quat, eye, 0x1Full, marks, 5), "nothing changed: the image is kept");
			check(NeedsRedraw(d, frame_pos, frame_quat, eye, 0x1Full | (1ull << 33), marks, 5) &&
					  NeedsRedraw(Drawn{}, frame_pos, frame_quat, eye, 0x1Full, marks, 5),
				"a pouch lit (or nothing drawn yet): drawn again");
			// Everything (the eye too) moved 10 cm: kept, and the kept image moves with its frame.
			const float t[3] = {0.1f, -0.05f, 0.07f};
			Mark moved[5];
			for (int i = 0; i < 5; i++)
			{
				moved[i] = marks[i];
				for (int k = 0; k < 3; k++)
					moved[i].centre[k] += t[k];
			}
			const float frame_moved[3] = {frame_pos[0] + t[0], frame_pos[1] + t[1], frame_pos[2] + t[2]};
			const float eye_moved[3] = {eye[0] + t[0], eye[1] + t[1], eye[2] + t[2]};
			check(!NeedsRedraw(d, frame_moved, frame_quat, eye_moved, 0x1Full, moved, 5),
				"the rig and the eye moved together: the image is kept");
			float kept_c[3];
			float kept_q[4];
			PlaceKept(d, frame_moved, frame_quat, kept_c, kept_q);
			check(near_v(Load(kept_c), Load(q.centre) + Load(t), 1.0e-5f), "a kept image moves with its frame");
			const float eye_side[3] = {eye[0] + 0.05f, eye[1], eye[2]};
			check(NeedsRedraw(d, frame_pos, frame_quat, eye_side, 0x1Full, marks, 5),
				"the eye 5 cm to the side (several degrees of view): drawn again");
			for (int i = 0; i < 5; i++)
				moved[i] = marks[i];
			moved[2].centre[0] += 0.01f;
			check(NeedsRedraw(d, frame_pos, frame_quat, eye, 0x1Full, moved, 5), "one pouch moved 1 cm: drawn again");
		}

		// --- Hand targeting's box (the fourth slot) -----------------------------------------------------------------
		{
			const std::array<bool, kSlots> with_box{{true, true, true, true}};
			check(Plan(4, with_box)[kTarget] && !Plan(3, with_box)[kTarget] && Plan(3, with_box)[kPouches] && !Plan(0, with_box)[kTarget],
				"the hand-targeting box is the first thing dropped when layers are short");
			check(Plan(1, std::array<bool, kSlots>{{false, false, false, true}})[kTarget], "with nothing else wanted it takes a free layer");
			// VR presence's partner (the fifth slot): after the holster, before the bag, the rings and the box.
			const std::array<bool, kSlots> every{{true, true, true, true, true}};
			const std::array<bool, kSlots> two = Plan(2, every);
			check(two[kHolster] && two[kPartner] && !two[kBelt] && !two[kPouches] && !two[kTarget],
				"two layers free with everything wanted: the holster and the partner");
			check(Plan(5, every)[kPartner] && Plan(5, every)[kTarget] && !Plan(4, every)[kTarget] && Plan(4, every)[kPartner],
				"room for all five: all go; one short: the box goes, the partner stays");
			check(Plan(1, std::array<bool, kSlots>{{false, false, false, false, true}})[kPartner] &&
					  !Plan(0, std::array<bool, kSlots>{{false, false, false, false, true}})[kPartner],
				"the partner alone takes a free layer, and none without one");

			// A box 0.5 m wide and 0.4 m tall, 0.6 to 1.1 m ahead of a head at the origin (+Y up, forward -Z).
			const float beye[3] = {0.0f, 1.2f, 0.0f};
			const float bx[4] = {-0.25f, 0.25f, 0.25f, -0.25f};
			const float bz[4] = {-0.6f, -0.6f, -1.1f, -1.1f};
			Mark box[8];
			for (int i = 0; i < 4; i++)
			{
				box[i].centre[0] = bx[i];
				box[i].centre[1] = 0.9f;
				box[i].centre[2] = bz[i];
				box[4 + i].centre[0] = bx[i];
				box[4 + i].centre[1] = 1.3f;
				box[4 + i].centre[2] = bz[i];
			}
			for (Mark& m : box)
			{
				m.radius = kBoxCornerM;
				m.shape = Shape::Box;
			}
			Quad box_quad;
			std::vector<u32> box_pixels;
			const bool placed = PlaceMarks(box, 8, beye, &box_quad);
			check(placed, "a hand-targeting box in front of the eye gets a quad");
			u32 alpha_max[3] = {0, 0, 0};
			u32 dim_pixels = 0;
			for (int g = 0; g < 3 && placed; g++)
			{
				for (Mark& m : box)
					m.glow = static_cast<u8>(g);
				RasterMarks(box, 8, beye, box_quad, box_pixels);
				for (const u32 v : box_pixels)
				{
					alpha_max[g] = std::max(alpha_max[g], v >> 24);
					if (g == 0 && (v >> 24) != 0)
						dim_pixels++;
				}
			}
			check(placed && dim_pixels > 500 && alpha_max[0] > 60 && alpha_max[0] < 100,
				"a far box is twelve faint lines");
			check(placed && alpha_max[1] > alpha_max[0] && alpha_max[2] > alpha_max[1] && alpha_max[2] > 200,
				"a box brightens as the hand comes near, and is brightest with the hand inside");
			// A corner behind the eye (or nearer than 5 cm) draws no box at all, not half of one.
			Mark behind[8];
			for (int i = 0; i < 8; i++)
			{
				behind[i] = box[i];
			}
			behind[3].centre[2] = 0.5f;
			Quad behind_quad;
			check(!PlaceMarks(behind, 8, beye, &behind_quad), "a box with a corner behind the eye gets no quad");
			if (placed)
			{
				RasterMarks(behind, 8, beye, box_quad, box_pixels);
				u32 any = 0;
				for (const u32 v : box_pixels)
					any |= v;
				check(any == 0, "and rasterised on another box's quad it draws nothing");
				std::vector<u32> seven;
				RasterMarks(box, 7, beye, box_quad, seven);
				any = 0;
				for (const u32 v : seven)
					any |= v;
				check(any == 0, "seven corners are not a box: nothing is drawn");
			}
		}

		if (failed == 0)
			Console.WriteLn("(VR) Zone markers self-test: %d checks passed.", checks);
		else
			Console.Error("(VR) Zone markers self-test: %d of %d checks FAILED.", failed, checks);
		return failed == 0;
	}
}
