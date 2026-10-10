// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/HandModel.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <utility>

namespace VR::HandModel
{
	namespace
	{
		std::atomic<bool> s_gun_held{false};

		constexpr float kPi = 3.14159265358979323846f;
		constexpr float kDegToRad = kPi / 180.0f;
		constexpr int kSize = static_cast<int>(kImageSize);
		constexpr size_t kPixels = static_cast<size_t>(kImageSize) * kImageSize;

		// The hand is modelled in centimetres in the pointing frame, right hand. The controller handle
		// (and the pistol grip) leans its top this far forward of +Y, like a pistol grip angle.
		constexpr float kHandleTiltDeg = 15.0f;
		// The palm runs back from the knuckles this far below square to the handle, so after the
		// handle tilt the wrist comes out level instead of cocked up.
		constexpr float kPalmDropDeg = 15.0f;
		constexpr float kFingerHalfT = 0.9f; // fingers ~1.8 cm thick
		// Triangles closer to the eye than this are dropped rather than clipped.
		constexpr float kNearM = 0.03f;

		struct V3
		{
			float x, y, z;
		};

		V3 Add(const V3& a, const V3& b) { return V3{a.x + b.x, a.y + b.y, a.z + b.z}; }
		V3 Sub(const V3& a, const V3& b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; }
		V3 Mul(const V3& a, float s) { return V3{a.x * s, a.y * s, a.z * s}; }
		float Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		V3 Cross(const V3& a, const V3& b)
		{
			return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
		}
		float Length(const V3& a) { return std::sqrt(Dot(a, a)); }
		V3 Normalize(const V3& a)
		{
			const float l = Length(a);
			return (l > 1.0e-8f) ? Mul(a, 1.0f / l) : V3{0.0f, 0.0f, 0.0f};
		}
		V3 Load(const float v[3]) { return V3{v[0], v[1], v[2]}; }
		void Store(const V3& a, float out[3])
		{
			out[0] = a.x;
			out[1] = a.y;
			out[2] = a.z;
		}

		// Rotates v by the unit quaternion q (xyzw).
		V3 Rotate(const float q[4], const V3& v)
		{
			const V3 u = V3{q[0], q[1], q[2]};
			const V3 t = Mul(Cross(u, v), 2.0f);
			return Add(Add(v, Mul(t, q[3])), Cross(u, t));
		}

		// Hamilton product a * b of xyzw quaternions (b applied first).
		void QuatMul(const float a[4], const float b[4], float out[4])
		{
			const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
			const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
			const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
			const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
			out[0] = x;
			out[1] = y;
			out[2] = z;
			out[3] = w;
		}

		// Rotates v about the unit axis k by angle (radians).
		V3 RotateAxis(const V3& v, const V3& k, float angle)
		{
			const float c = std::cos(angle);
			const float s = std::sin(angle);
			return Add(Add(Mul(v, c), Mul(Cross(k, v), s)), Mul(k, Dot(k, v) * (1.0f - c)));
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

		// Unit quaternion from h.quat; identity when it is unusable.
		void NormalizedQuat(const HandState& h, float out[4])
		{
			const float l = std::sqrt(h.quat[0] * h.quat[0] + h.quat[1] * h.quat[1] + h.quat[2] * h.quat[2] +
									  h.quat[3] * h.quat[3]);
			if (!(l > 1.0e-6f) || !std::isfinite(l))
			{
				out[0] = out[1] = out[2] = 0.0f;
				out[3] = 1.0f;
				return;
			}
			for (int i = 0; i < 4; i++)
				out[i] = h.quat[i] / l;
		}

		enum Mat : u8
		{
			kMatGlove,
			kMatKnuckle,
			kMatCuff,
			kMatHandle,
			kMatGunSlide,
			kMatGunFrame,
			kMatCount
		};

		struct Material
		{
			float r, g, b; // linear albedo
			float spec;
			float rim;
		};

		float SrgbToLinear(u32 c8)
		{
			const float c = static_cast<float>(c8) / 255.0f;
			return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
		}

		Material MakeMaterial(u32 rgb, float spec, float rim)
		{
			Material m;
			m.r = SrgbToLinear((rgb >> 16) & 0xFFu);
			m.g = SrgbToLinear((rgb >> 8) & 0xFFu);
			m.b = SrgbToLinear(rgb & 0xFFu);
			m.spec = spec;
			m.rim = rim;
			return m;
		}

		// Lighting is done in linear light and written as sRGB bytes, which is what the sRGB
		// swapchain the compositor picks expects.
		struct Tables
		{
			Material mat[kMatCount];
			u8 to_srgb[1025];

			Tables()
			{
				// Highlights stay faint: with flat faces a strong one flashes the whole face.
				mat[kMatGlove] = MakeMaterial(0x4a5a48u, 0.03f, 0.45f);
				mat[kMatKnuckle] = MakeMaterial(0x6a7d64u, 0.05f, 0.45f);
				mat[kMatCuff] = MakeMaterial(0x3c4a3eu, 0.03f, 0.40f);
				mat[kMatHandle] = MakeMaterial(0x26282cu, 0.10f, 0.35f);
				mat[kMatGunSlide] = MakeMaterial(0x4b4f55u, 0.15f, 0.40f);
				mat[kMatGunFrame] = MakeMaterial(0x2c2e32u, 0.08f, 0.35f);
				for (int i = 0; i <= 1024; i++)
				{
					const float l = static_cast<float>(i) / 1024.0f;
					const float sv = (l <= 0.0031308f) ? (l * 12.92f) : (1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f);
					to_srgb[i] = static_cast<u8>(std::clamp(sv * 255.0f + 0.5f, 0.0f, 255.0f));
				}
			}
		};

		const Tables& GetTables()
		{
			static const Tables s_tables;
			return s_tables;
		}

		struct Tri
		{
			V3 p[3];
			V3 n; // outward face normal
			u8 mat;
		};

		// Quad a-b-c-d (a loop) as two triangles. The normal is taken from the diagonals and turned
		// to point away from inside, so the builders never have to care about winding.
		void AddQuad(std::vector<Tri>& out, const V3& a, const V3& b, const V3& c, const V3& d, const V3& inside, u8 mat)
		{
			V3 n = Normalize(Cross(Sub(c, a), Sub(d, b)));
			const V3 centre = Mul(Add(Add(a, b), Add(c, d)), 0.25f);
			if (Dot(n, Sub(centre, inside)) < 0.0f)
				n = Mul(n, -1.0f);
			Tri t;
			t.n = n;
			t.mat = mat;
			t.p[0] = a;
			t.p[1] = b;
			t.p[2] = c;
			out.push_back(t);
			t.p[1] = c;
			t.p[2] = d;
			out.push_back(t);
		}

		// Six-sided solid: c[0..3] loop around one end, c[4..7] are the matching corners of the other.
		void AddHex(std::vector<Tri>& out, const V3* c, u8 mat)
		{
			V3 inside = V3{0.0f, 0.0f, 0.0f};
			for (int i = 0; i < 8; i++)
				inside = Add(inside, c[i]);
			inside = Mul(inside, 0.125f);
			AddQuad(out, c[0], c[1], c[2], c[3], inside, mat);
			AddQuad(out, c[4], c[5], c[6], c[7], inside, mat);
			for (int k = 0; k < 4; k++)
			{
				const int k1 = (k + 1) & 3;
				AddQuad(out, c[k], c[k1], c[4 + k1], c[4 + k], inside, mat);
			}
		}

		// Box (parallelepiped) from a centre and three half-extent vectors.
		void AddBox(std::vector<Tri>& out, const V3& centre, const V3& hx, const V3& hy, const V3& hz, u8 mat)
		{
			V3 c[8];
			for (int e = 0; e < 2; e++)
			{
				const V3 o = Add(centre, Mul(hz, (e == 0) ? -1.0f : 1.0f));
				c[e * 4 + 0] = Sub(Sub(o, hx), hy);
				c[e * 4 + 1] = Sub(Add(o, hx), hy);
				c[e * 4 + 2] = Add(Add(o, hx), hy);
				c[e * 4 + 3] = Add(Sub(o, hx), hy);
			}
			AddHex(out, c, mat);
		}

		void AddBoxAA(std::vector<Tri>& out, float cx, float cy, float cz, float hx, float hy, float hz, u8 mat)
		{
			AddBox(out, V3{cx, cy, cz}, V3{hx, 0.0f, 0.0f}, V3{0.0f, hy, 0.0f}, V3{0.0f, 0.0f, hz}, mat);
		}

		// Tapered box from base along dir for len: half width hw across, half thickness ht along top
		// (the back of a finger). The far end is scaled by taper.
		void AddSegment(std::vector<Tri>& out, const V3& base, const V3& dir_in, const V3& top_in, float len, float hw,
			float ht, float taper, u8 mat)
		{
			const V3 dir = Normalize(dir_in);
			const V3 top = Normalize(Sub(top_in, Mul(dir, Dot(top_in, dir))));
			const V3 side = Cross(top, dir);
			const V3 tip = Add(base, Mul(dir, len));
			V3 c[8];
			for (int e = 0; e < 2; e++)
			{
				const V3 o = (e == 0) ? base : tip;
				const float sc = (e == 0) ? 1.0f : taper;
				for (int k = 0; k < 4; k++)
				{
					const float sx = (k == 1 || k == 2) ? hw : -hw;
					const float sy = (k >= 2) ? ht : -ht;
					c[e * 4 + k] = Add(o, Add(Mul(side, sx * sc), Mul(top, sy * sc)));
				}
			}
			AddHex(out, c, mat);
		}

		struct FingerDef
		{
			V3 knuckle;
			float len[3];
			float hw;
		};

		// Index (top, nearest the thumb) to little. The knuckles sit on the right of the handle, just in
		// front of it, so the curled fingers wrap round it toward -X.
		constexpr FingerDef kFingers[4] = {
			{{2.8f, 3.0f, -2.2f}, {4.3f, 2.6f, 2.1f}, 0.95f},
			{{2.9f, 1.0f, -2.5f}, {4.7f, 2.9f, 2.2f}, 1.0f},
			{{2.8f, -1.0f, -2.3f}, {4.4f, 2.8f, 2.1f}, 0.95f},
			{{2.6f, -2.8f, -1.8f}, {3.5f, 2.2f, 1.9f}, 0.85f},
		};

		// Thumb from its base on the palm: resting on top of the controller, or from the web of the
		// hand along the left of the pistol frame.
		constexpr float kThumbLen[3] = {3.6f, 3.0f, 2.5f};
		constexpr V3 kThumbRestBase = {1.9f, 3.4f, 1.6f};
		constexpr V3 kThumbRest[3] = {{-0.47f, 0.55f, -0.69f}, {-0.35f, 0.10f, -0.93f}, {-0.15f, -0.12f, -0.98f}};
		constexpr V3 kThumbGunBase = {1.6f, 4.6f, 2.2f};
		constexpr V3 kThumbGun[3] = {{-0.80f, 0.05f, -0.60f}, {-0.25f, -0.05f, -0.97f}, {-0.10f, -0.10f, -0.99f}};

		// Right hand in the handle frame (handle along +Y through the origin, -Z forward, back of the
		// hand toward +X); BuildWorld tilts it into the pointing frame.
		void BuildHand(std::vector<Tri>& out, float grip, float trigger, bool gun)
		{
			const V3 up = V3{0.0f, 1.0f, 0.0f};
			grip = std::isfinite(grip) ? std::clamp(grip, 0.0f, 1.0f) : 0.0f;
			trigger = std::isfinite(trigger) ? std::clamp(trigger, 0.0f, 1.0f) : 0.0f;

			// Joint bends in degrees (knuckle, middle, tip). Middle/ring/little stay wrapped round the
			// handle even when the grip isn't squeezed. The index rests round the trigger and curls in
			// as it is pulled; on the pistol it stays inside the guard.
			const float curl = 0.8f + 0.2f * grip;
			const float rest_deg[3] = {78.0f * curl, 95.0f * curl, 62.0f * curl};
			float index_deg[3];
			if (gun)
			{
				index_deg[0] = 30.0f + 10.0f * trigger;
				index_deg[1] = 95.0f + 15.0f * trigger;
				index_deg[2] = 35.0f + 15.0f * trigger;
			}
			else
			{
				index_deg[0] = 25.0f + 20.0f * trigger;
				index_deg[1] = 80.0f + 20.0f * trigger;
				index_deg[2] = 35.0f + 20.0f * trigger;
			}
			for (int f = 0; f < 4; f++)
			{
				const FingerDef& fd = kFingers[f];
				const float* bend_deg = (f == 0) ? index_deg : rest_deg;
				V3 joint = fd.knuckle;
				V3 dir = V3{0.0f, 0.0f, -1.0f};
				V3 top = V3{1.0f, 0.0f, 0.0f};
				for (int sgm = 0; sgm < 3; sgm++)
				{
					// Curling turns the finger about the handle axis, from -Z toward -X (the palm side).
					const float bend = bend_deg[sgm] * kDegToRad;
					dir = RotateAxis(dir, up, bend);
					top = RotateAxis(top, up, bend);
					// Start a little behind the joint so the outside of the bend shows no gap.
					const float overlap = kFingerHalfT * 0.7f;
					AddSegment(out, Sub(joint, Mul(dir, overlap)), dir, top, fd.len[sgm] + overlap, fd.hw, kFingerHalfT,
						(sgm == 2) ? 0.78f : 0.94f, kMatGlove);
					if (sgm == 0)
					{
						// Lighter knuckle pad on the back of the glove.
						const V3 pad = Add(joint, Mul(top, kFingerHalfT + 0.15f));
						AddSegment(out, Sub(pad, Mul(dir, 0.8f)), dir, top, 1.6f, fd.hw * 0.85f, 0.35f, 0.9f, kMatKnuckle);
					}
					joint = Add(joint, Mul(dir, fd.len[sgm]));
				}
			}

			// Palm: ~9 wide (along the knuckle line), 3 thick, ~9 long, skewed so it runs back and down
			// from the knuckles; then a cuff that narrows a little at the wrist end.
			const float drop = kPalmDropDeg * kDegToRad;
			const V3 back = V3{0.0f, -std::sin(drop), std::cos(drop)};
			const V3 knuckle_line = V3{2.8f, 0.1f, -1.9f};
			constexpr float kPalmLen = 8.6f;
			AddBox(out, Add(knuckle_line, Mul(back, kPalmLen * 0.5f)), V3{1.5f, 0.0f, 0.0f}, V3{0.0f, 4.4f, 0.0f},
				Mul(back, kPalmLen * 0.5f), kMatGlove);
			AddSegment(out, Add(knuckle_line, Mul(back, kPalmLen - 0.6f)), back, V3{1.0f, 0.0f, 0.0f}, 3.0f, 3.8f, 1.9f,
				0.92f, kMatCuff);

			const V3* thumb = gun ? kThumbGun : kThumbRest;
			const V3 thumb_top = V3{0.55f, 1.0f, 0.25f};
			V3 tj = gun ? kThumbGunBase : kThumbRestBase;
			for (int sgm = 0; sgm < 3; sgm++)
			{
				const V3 d = Normalize(thumb[sgm]);
				const float overlap = (sgm == 0) ? 0.0f : 0.7f;
				AddSegment(out, Sub(tj, Mul(d, overlap)), d, thumb_top, kThumbLen[sgm] + overlap, (sgm == 0) ? 1.25f : 1.05f,
					(sgm == 0) ? 1.15f : 0.98f, (sgm == 2) ? 0.8f : 0.95f, kMatGlove);
				tj = Add(tj, Mul(d, kThumbLen[sgm]));
			}

			if (!gun)
			{
				// A hint of the controller: the handle in the fist and the head the thumb rests on.
				AddBoxAA(out, 0.0f, -0.6f, 0.0f, 1.3f, 5.0f, 1.6f, kMatHandle);
				AddBoxAA(out, -0.3f, 4.5f, -1.0f, 1.9f, 0.6f, 2.9f, kMatHandle);
			}
			else
			{
				// Pistol grip in the fist (the rest of the pistol is in BuildGunTop).
				AddBoxAA(out, 0.0f, -0.6f, 0.0f, 1.35f, 5.2f, 2.0f, kMatGunFrame);
			}
		}

		// The rest of the pistol, in the pointing frame itself so the barrel runs along -Z.
		void BuildGunTop(std::vector<Tri>& out)
		{
			AddBoxAA(out, 0.0f, 6.4f, -5.0f, 1.5f, 1.75f, 9.0f, kMatGunSlide); // slide, 3 x 3.5 x 18
			AddBoxAA(out, 0.0f, 4.2f, -7.6f, 1.25f, 0.6f, 5.2f, kMatGunFrame); // dust cover under it
			AddBoxAA(out, 0.0f, 0.4f, -5.6f, 0.45f, 0.3f, 2.6f, kMatGunFrame); // trigger guard, bottom
			AddBoxAA(out, 0.0f, 2.2f, -8.0f, 0.45f, 1.8f, 0.3f, kMatGunFrame); // trigger guard, front
			AddBoxAA(out, 0.0f, 2.0f, -4.6f, 0.35f, 1.0f, 0.35f, kMatGunSlide); // trigger
			AddBoxAA(out, 0.0f, 6.7f, -14.06f, 0.45f, 0.45f, 0.08f, kMatGunFrame); // muzzle
			AddBoxAA(out, 0.0f, 8.35f, -13.0f, 0.22f, 0.2f, 0.35f, kMatGunFrame); // front sight
			AddBoxAA(out, 0.0f, 8.35f, 3.0f, 0.75f, 0.2f, 0.3f, kMatGunFrame); // rear sight
		}

		std::vector<Tri> s_mesh;

		// Builds the hand into s_mesh and moves it into the base space (metres). Optionally returns
		// the bounds in the (mirrored, tilted) local frame, in centimetres.
		void BuildWorld(int hand, const HandState& h, V3* local_min, V3* local_max)
		{
			const bool gun = h.gun && hand == kRight;
			s_mesh.clear();
			s_mesh.reserve(512);
			BuildHand(s_mesh, h.grip, h.trigger, gun);
			const size_t tilted = s_mesh.size();
			if (gun)
				BuildGunTop(s_mesh);

			const float ta = -kHandleTiltDeg * kDegToRad;
			const float tc = std::cos(ta);
			const float ts = std::sin(ta);
			const float mx = (hand == kLeft) ? -1.0f : 1.0f;
			float q[4];
			NormalizedQuat(h, q);
			const V3 pos = Load(h.pos);
			V3 lmin = V3{1.0e9f, 1.0e9f, 1.0e9f};
			V3 lmax = V3{-1.0e9f, -1.0e9f, -1.0e9f};

			for (size_t i = 0; i < s_mesh.size(); i++)
			{
				Tri& t = s_mesh[i];
				const bool tilt = (i < tilted);
				// Handle frame -> pointing frame (top of the handle forward), mirrored for the left hand.
				const auto local = [&](const V3& v) {
					V3 r = v;
					if (tilt)
						r = V3{v.x, v.y * tc - v.z * ts, v.y * ts + v.z * tc};
					r.x *= mx;
					return r;
				};
				for (int k = 0; k < 3; k++)
				{
					const V3 l = local(t.p[k]);
					lmin = V3{std::min(lmin.x, l.x), std::min(lmin.y, l.y), std::min(lmin.z, l.z)};
					lmax = V3{std::max(lmax.x, l.x), std::max(lmax.y, l.y), std::max(lmax.z, l.z)};
					t.p[k] = Add(pos, Rotate(q, Mul(l, 0.01f)));
				}
				t.n = Rotate(q, local(t.n));
			}
			if (local_min)
				*local_min = lmin;
			if (local_max)
				*local_max = lmax;
		}

		// Rotation matrix with columns r, u, n -> quaternion (xyzw).
		void QuatFromBasis(const V3& r, const V3& u, const V3& n, float out[4])
		{
			const float m00 = r.x, m01 = u.x, m02 = n.x;
			const float m10 = r.y, m11 = u.y, m12 = n.y;
			const float m20 = r.z, m21 = u.z, m22 = n.z;
			const float trace = m00 + m11 + m22;
			float x, y, z, w;
			if (trace > 0.0f)
			{
				const float s = std::sqrt(trace + 1.0f) * 2.0f;
				w = 0.25f * s;
				x = (m21 - m12) / s;
				y = (m02 - m20) / s;
				z = (m10 - m01) / s;
			}
			else if (m00 > m11 && m00 > m22)
			{
				const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
				w = (m21 - m12) / s;
				x = 0.25f * s;
				y = (m01 + m10) / s;
				z = (m02 + m20) / s;
			}
			else if (m11 > m22)
			{
				const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
				w = (m02 - m20) / s;
				x = (m01 + m10) / s;
				y = 0.25f * s;
				z = (m12 + m21) / s;
			}
			else
			{
				const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
				w = (m10 - m01) / s;
				x = (m02 + m20) / s;
				y = (m12 + m21) / s;
				z = 0.25f * s;
			}
			const float l = std::sqrt(x * x + y * y + z * z + w * w);
			out[0] = x / l;
			out[1] = y / l;
			out[2] = z / l;
			out[3] = w / l;
		}

		struct SV
		{
			float x, y, iz; // pixel position, 1 / view depth
		};

		struct Bounds
		{
			int x0, y0, x1, y1;
		};

		u32 PackLinear(const Tables& tab, float r, float g, float b)
		{
			const auto q = [&tab](float v) -> u32 {
				const float c = (v > 0.0f) ? std::min(v, 1.0f) : 0.0f; // also catches NaN
				return tab.to_srgb[static_cast<int>(c * 1024.0f + 0.5f)];
			};
			return (0xFFu << 24) | (q(b) << 16) | (q(g) << 8) | q(r);
		}

		// Flat shading per face: ambient (a little brighter from above), Lambert from a light above
		// and to the right of the viewer, a headlight, a rim on faces turning away, and a small highlight.
		u32 Shade(const Tables& tab, const Tri& t, const V3& to_eye, const V3& light, const V3& up)
		{
			const Material& m = tab.mat[t.mat];
			const V3 v = Normalize(to_eye);
			const float ndl = std::max(Dot(t.n, light), 0.0f);
			const float ndv = std::clamp(Dot(t.n, v), 0.0f, 1.0f);
			const float hemi = 0.5f + 0.5f * Dot(t.n, up);
			// The headlight term keeps faces turned to the viewer readable against a dark game scene.
			const float k = 0.22f + 0.12f * hemi + 0.70f * ndl + 0.22f * ndv;
			const float edge = 1.0f - ndv;
			const float rim = m.rim * edge * edge * edge;
			const float nh = std::max(Dot(t.n, Normalize(Add(light, v))), 0.0f);
			const float nh2 = nh * nh;
			const float nh4 = nh2 * nh2;
			const float nh8 = nh4 * nh4;
			const float nh16 = nh8 * nh8;
			const float spec = m.spec * nh16 * nh16; // pow 32
			return PackLinear(tab, m.r * k + rim * 0.50f + spec, m.g * k + rim * 0.62f + spec, m.b * k + rim * 0.56f + spec);
		}

		// Fills one screen-space triangle with a depth test (larger 1/z is nearer).
		void RasterTri(const SV& a, const SV& b_in, const SV& c_in, u32 colour, float* depth, u32* px, Bounds& bounds)
		{
			const SV* b = &b_in;
			const SV* c = &c_in;
			float area = (b->x - a.x) * (c->y - a.y) - (b->y - a.y) * (c->x - a.x);
			if (area < 0.0f)
			{
				// Faces were culled with their normals; just make the edge functions positive inside.
				std::swap(b, c);
				area = -area;
			}
			if (area < 1.0e-6f)
				return;

			const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b->x, c->x}))));
			const int x1 = std::min(kSize - 1, static_cast<int>(std::ceil(std::max({a.x, b->x, c->x}))));
			const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b->y, c->y}))));
			const int y1 = std::min(kSize - 1, static_cast<int>(std::ceil(std::max({a.y, b->y, c->y}))));
			if (x0 > x1 || y0 > y1)
				return;

			// Edge function opposite each vertex (its barycentric weight times area), stepped per pixel.
			const float sa_x = -(c->y - b->y), sa_y = c->x - b->x;
			const float sb_x = -(a.y - c->y), sb_y = a.x - c->x;
			const float sc_x = -(b->y - a.y), sc_y = b->x - a.x;
			const float px0 = static_cast<float>(x0) + 0.5f;
			const float py0 = static_cast<float>(y0) + 0.5f;
			float ra = (c->x - b->x) * (py0 - b->y) - (c->y - b->y) * (px0 - b->x);
			float rb = (a.x - c->x) * (py0 - c->y) - (a.y - c->y) * (px0 - c->x);
			float rc = (b->x - a.x) * (py0 - a.y) - (b->y - a.y) * (px0 - a.x);
			const float inv_area = 1.0f / area;
			const float za = a.iz * inv_area;
			const float zb = b->iz * inv_area;
			const float zc = c->iz * inv_area;
			bool any = false;
			for (int y = y0; y <= y1; y++)
			{
				float ea = ra, eb = rb, ec = rc;
				float* drow = depth + static_cast<size_t>(y) * kImageSize;
				u32* prow = px + static_cast<size_t>(y) * kImageSize;
				for (int x = x0; x <= x1; x++)
				{
					if (ea >= 0.0f && eb >= 0.0f && ec >= 0.0f)
					{
						const float iz = ea * za + eb * zb + ec * zc;
						if (iz > drow[x])
						{
							drow[x] = iz;
							prow[x] = colour;
							any = true;
						}
					}
					ea += sa_x;
					eb += sb_x;
					ec += sc_x;
				}
				ra += sa_y;
				rb += sb_y;
				rc += sc_y;
			}
			if (any)
			{
				bounds.x0 = std::min(bounds.x0, x0);
				bounds.y0 = std::min(bounds.y0, y0);
				bounds.x1 = std::max(bounds.x1, x1);
				bounds.y1 = std::max(bounds.y1, y1);
			}
		}

		std::vector<float> s_depth;
		std::vector<u8> s_mask;

		// Cheap edge anti-aliasing: alpha is a 3x3 tent filter of the coverage, and the pixels just
		// outside the silhouette take their covered neighbours' colour (alpha is unpremultiplied).
		void SoftenEdges(u32* px, const Bounds& bounds)
		{
			constexpr int kStride = kSize + 2;
			s_mask.assign(static_cast<size_t>(kStride) * kStride, 0);
			for (int y = bounds.y0; y <= bounds.y1; y++)
			{
				const float* drow = s_depth.data() + static_cast<size_t>(y) * kImageSize;
				u8* mrow = s_mask.data() + static_cast<size_t>(y + 1) * kStride + 1;
				for (int x = bounds.x0; x <= bounds.x1; x++)
					mrow[x] = static_cast<u8>((drow[x] > 0.0f) ? 1 : 0);
			}

			const int x0 = std::max(bounds.x0 - 1, 0);
			const int x1 = std::min(bounds.x1 + 1, kSize - 1);
			const int y0 = std::max(bounds.y0 - 1, 0);
			const int y1 = std::min(bounds.y1 + 1, kSize - 1);
			for (int y = y0; y <= y1; y++)
			{
				const u8* m = s_mask.data() + static_cast<size_t>(y + 1) * kStride + 1;
				u32* prow = px + static_cast<size_t>(y) * kImageSize;
				for (int x = x0; x <= x1; x++)
				{
					const u8* mc = m + x;
					const int w = 4 * mc[0] + 2 * (mc[-1] + mc[1] + mc[-kStride] + mc[kStride]) +
								  (mc[-kStride - 1] + mc[-kStride + 1] + mc[kStride - 1] + mc[kStride + 1]);
					if (w == 0 || w == 16)
						continue;
					const u32 alpha = static_cast<u32>(w) * 255u / 16u;
					if (mc[0])
					{
						prow[x] = (prow[x] & 0x00FFFFFFu) | (alpha << 24);
						continue;
					}
					// Outside: average the covered neighbours' colour.
					u32 r = 0, g = 0, b = 0, n = 0;
					for (int dy = -1; dy <= 1; dy++)
					{
						for (int dx = -1; dx <= 1; dx++)
						{
							const int nx = x + dx;
							const int ny = y + dy;
							if (nx < 0 || ny < 0 || nx >= kSize || ny >= kSize || !mc[dy * kStride + dx])
								continue;
							const u32 c = px[static_cast<size_t>(ny) * kImageSize + nx];
							r += c & 0xFFu;
							g += (c >> 8) & 0xFFu;
							b += (c >> 16) & 0xFFu;
							n++;
						}
					}
					if (n == 0)
						continue;
					prow[x] = (alpha << 24) | ((b / n) << 16) | ((g / n) << 8) | (r / n);
				}
			}
		}
	}

	void SetGunHeld(bool held)
	{
		s_gun_held.store(held, std::memory_order_relaxed);
	}

	bool GunHeld()
	{
		return s_gun_held.load(std::memory_order_relaxed);
	}

	bool PlaceImpostor(int hand, const HandState& h, const float eye[3], ImpostorQuad* out)
	{
		if (!out || !h.valid || !Finite(h.pos, 3) || !Finite(h.quat, 4) || !Finite(eye, 3))
			return false;

		V3 lmin, lmax;
		BuildWorld(hand, h, &lmin, &lmax);
		float q[4];
		NormalizedQuat(h, q);
		const V3 centre = Add(Load(h.pos), Rotate(q, Mul(Mul(Add(lmin, lmax), 0.5f), 0.01f)));
		const float radius = 0.5f * 0.01f * Length(Sub(lmax, lmin));

		const V3 to_eye = Sub(Load(eye), centre);
		const float dist = Length(to_eye);
		if (!(dist > radius * 1.1f) || dist < 0.05f)
			return false;
		const V3 n = Mul(to_eye, 1.0f / dist);
		// Any in-plane spin is invisible (the image is rendered for this exact quad), so world up is
		// only a stable choice; it just needs a fallback when looking straight down on the hand.
		V3 up_ref = V3{0.0f, 1.0f, 0.0f};
		if (std::fabs(Dot(up_ref, n)) > 0.97f)
			up_ref = V3{0.0f, 0.0f, -1.0f};
		const V3 up = Normalize(Sub(up_ref, Mul(n, Dot(up_ref, n))));
		const V3 right = Cross(up, n);

		// Bounding sphere seen from the eye, so a hand held close to the face isn't cut off.
		const bool gun = h.gun && hand == kRight;
		const float base = gun ? kQuadSizeGunM : kQuadSizeM;
		const float needed = 2.04f * dist * radius / std::sqrt(dist * dist - radius * radius);
		out->size_m = std::clamp(needed, base, base * 2.0f);
		Store(centre, out->centre);
		Store(right, out->right);
		Store(up, out->up);
		QuatFromBasis(right, up, n, out->quat);
		return true;
	}

	void RenderImpostor(int hand, const HandState& h, const float eye[3], const float quad_center[3],
		const float quad_right[3], const float quad_up[3], float quad_size_m, std::vector<u32>& out_rgba)
	{
		out_rgba.assign(kPixels, 0u);
		if (!h.valid || !(quad_size_m > 0.0f) || !Finite(h.pos, 3) || !Finite(h.quat, 4) || !Finite(eye, 3) ||
			!Finite(quad_center, 3) || !Finite(quad_right, 3) || !Finite(quad_up, 3))
		{
			return;
		}

		const V3 e = Load(eye);
		const V3 c = Load(quad_center);
		const V3 r = Normalize(Load(quad_right));
		const V3 n = Normalize(Cross(r, Load(quad_up)));
		const V3 u = Cross(n, r);
		const V3 ec = Sub(e, c);
		const float de = Dot(ec, n); // eye's distance in front of the quad
		if (de < 0.02f)
			return;
		const float ec_r = Dot(ec, r);
		const float ec_u = Dot(ec, u);
		const float px_per_m = static_cast<float>(kImageSize) / quad_size_m;
		const float half = 0.5f * static_cast<float>(kImageSize);

		BuildWorld(hand, h, nullptr, nullptr);
		s_depth.assign(kPixels, 0.0f);
		const Tables& tab = GetTables();
		const V3 light = Normalize(Add(Add(Mul(r, 0.35f), Mul(u, 0.8f)), Mul(n, 0.5f)));
		Bounds bounds = {kSize, kSize, -1, -1};

		for (const Tri& t : s_mesh)
		{
			const V3 centroid = Mul(Add(Add(t.p[0], t.p[1]), t.p[2]), 1.0f / 3.0f);
			const V3 to_eye = Sub(e, centroid);
			if (Dot(t.n, to_eye) <= 0.0f)
				continue;

			// Central projection from the eye onto the quad plane.
			SV sv[3];
			bool ok = true;
			for (int k = 0; k < 3; k++)
			{
				const V3 d = Sub(t.p[k], e);
				const float z = -Dot(d, n);
				if (!(z > kNearM))
				{
					ok = false;
					break;
				}
				const float s = de / z;
				sv[k].x = half + (ec_r + s * Dot(d, r)) * px_per_m;
				sv[k].y = half - (ec_u + s * Dot(d, u)) * px_per_m;
				sv[k].iz = 1.0f / z;
				if (!std::isfinite(sv[k].x) || !std::isfinite(sv[k].y))
				{
					ok = false;
					break;
				}
			}
			if (!ok)
				continue;
			RasterTri(sv[0], sv[1], sv[2], Shade(tab, t, to_eye, light, u), s_depth.data(), out_rgba.data(), bounds);
		}

		if (bounds.x1 >= bounds.x0 && bounds.y1 >= bounds.y0)
			SoftenEdges(out_rgba.data(), bounds);
	}

	HandState FakeHandState(int hand, const float head_pos[3], const float head_quat[4], bool gun)
	{
		HandState h;
		if (!Finite(head_pos, 3) || !Finite(head_quat, 4))
			return h;

		// Head yaw only, the same way the compositor levels the screen.
		const float qx = head_quat[0], qy = head_quat[1], qz = head_quat[2], qw = head_quat[3];
		float fx = -2.0f * (qx * qz + qw * qy);
		float fz = -(1.0f - 2.0f * (qx * qx + qy * qy));
		const float fl = std::sqrt(fx * fx + fz * fz);
		if (fl > 1.0e-4f)
		{
			fx /= fl;
			fz /= fl;
		}
		else
		{
			fx = 0.0f;
			fz = -1.0f;
		}
		const float side = (hand == kRight) ? 1.0f : -1.0f;
		const V3 fwd = V3{fx, 0.0f, fz};
		const V3 rgt = V3{-fz, 0.0f, fx};
		const V3 p = Add(Add(Load(head_pos), Mul(rgt, 0.22f * side)), Add(V3{0.0f, -0.30f, 0.0f}, Mul(fwd, 0.35f)));
		Store(p, h.pos);

		// Pointing along the head's yaw, turned in a little toward the middle, 15 degrees down, and
		// rolled in 20 degrees the way controllers are usually held (so the back of the hand shows).
		const float yaw = std::atan2(-fx, -fz) + side * 6.0f * kDegToRad;
		const float pitch = -15.0f * kDegToRad;
		const float roll = side * 20.0f * kDegToRad;
		const float q_yaw[4] = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
		const float q_pitch[4] = {std::sin(pitch * 0.5f), 0.0f, 0.0f, std::cos(pitch * 0.5f)};
		const float q_roll[4] = {0.0f, 0.0f, std::sin(roll * 0.5f), std::cos(roll * 0.5f)};
		float q_yp[4];
		QuatMul(q_yaw, q_pitch, q_yp);
		QuatMul(q_yp, q_roll, h.quat);
		h.grip = 0.5f;
		h.trigger = gun ? 0.0f : 0.15f;
		h.gun = gun && hand == kRight;
		h.valid = true;
		return h;
	}
}
