// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/MeshRaster.h"

#include "common/Console.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace VR::MeshRaster
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;

		// Triangles are clipped to this many times the picture's half-size (in clip space), so the corners that reach the
		// fixed-point setup stay within a few million sub-sample units and the edge arithmetic cannot overflow.
		constexpr float kGuard = 8.0f;
		// Corners are snapped to 1 / 256 of a sample.
		constexpr s32 kSubBits = 8;
		constexpr s32 kSub = 1 << kSubBits;
		constexpr float kSubInv = 1.0f / static_cast<float>(kSub);

		// Where a vertex is outside the clip volume: the real planes (to throw out a triangle that is wholly outside one),
		// the guard planes round the picture (to cut a triangle that reaches far past it) and a vertex that is not finite.
		constexpr u32 kNear = 1u << 0;
		constexpr u32 kFar = 1u << 1;
		constexpr u32 kLeft = 1u << 2;
		constexpr u32 kRight = 1u << 3;
		constexpr u32 kBottom = 1u << 4;
		constexpr u32 kTop = 1u << 5;
		constexpr u32 kGuardLeft = 1u << 6;
		constexpr u32 kGuardRight = 1u << 7;
		constexpr u32 kGuardBottom = 1u << 8;
		constexpr u32 kGuardTop = 1u << 9;
		constexpr u32 kBad = 1u << 10;
		constexpr u32 kFrustumMask = kNear | kFar | kLeft | kRight | kBottom | kTop;
		constexpr u32 kClipMask = kNear | kFar | kGuardLeft | kGuardRight | kGuardBottom | kGuardTop;

		// A triangle clipped by six planes has at most nine corners.
		constexpr int kMaxPoly = 12;

		bool IsFinite(float f)
		{
			return (std::bit_cast<u32>(f) & 0x7F800000u) != 0x7F800000u;
		}

		bool AllFinite(const float* v, int n)
		{
			for (int i = 0; i < n; i++)
			{
				if (!IsFinite(v[i]))
					return false;
			}
			return true;
		}

		// floor() without the library call (no SSE4.1 to inline it); the argument is within int range or a texture
		// coordinate that SafeCoord has bounded.
		int FloorToInt(float f)
		{
			const int i = static_cast<int>(f);
			return (static_cast<float>(i) > f) ? i - 1 : i;
		}

		float Dot3(const float a[3], const float b[3])
		{
			return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
		}

		void Cross3(const float a[3], const float b[3], float out[3])
		{
			out[0] = a[1] * b[2] - a[2] * b[1];
			out[1] = a[2] * b[0] - a[0] * b[2];
			out[2] = a[0] * b[1] - a[1] * b[0];
		}

		// Scales v to unit length; false (v untouched) for a vector that is too short or not finite.
		bool Normalize3(float v[3])
		{
			const float l2 = Dot3(v, v);
			if (!(l2 > 1.0e-20f) || !IsFinite(l2))
				return false;
			const float inv = 1.0f / std::sqrt(l2);
			v[0] *= inv;
			v[1] *= inv;
			v[2] *= inv;
			return true;
		}

		// A 0-to-1 value as 0 to 256 (a NaN as 0): 256 so that a full factor multiplies a byte exactly (b * 256 >> 8).
		__fi u32 Q256(float f)
		{
			return static_cast<u32>(std::max(0.0f, std::min(f * 256.0f + 0.5f, 256.0f)));
		}

		// f held to 0 to 1 (a NaN as 0).
		float Unit(float f)
		{
			return (f > 0.0f) ? ((f < 1.0f) ? f : 1.0f) : 0.0f;
		}
	}

	// --- Matrices and cameras ------------------------------------------------------------------------------------------

	Mat4 Multiply(const Mat4& a, const Mat4& b)
	{
		Mat4 r;
		for (int col = 0; col < 4; col++)
		{
			for (int row = 0; row < 4; row++)
			{
				float s = 0.0f;
				for (int k = 0; k < 4; k++)
					s += a.m[k * 4 + row] * b.m[col * 4 + k];
				r.m[col * 4 + row] = s;
			}
		}
		return r;
	}

	Mat4 ModelMatrix(const float pos[3], const float quat_xyzw[4], const float scale[3])
	{
		float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
		if (quat_xyzw && AllFinite(quat_xyzw, 4))
		{
			const float l2 = quat_xyzw[0] * quat_xyzw[0] + quat_xyzw[1] * quat_xyzw[1] + quat_xyzw[2] * quat_xyzw[2] +
			                 quat_xyzw[3] * quat_xyzw[3];
			if (l2 > 1.0e-12f)
			{
				const float inv = 1.0f / std::sqrt(l2);
				x = quat_xyzw[0] * inv;
				y = quat_xyzw[1] * inv;
				z = quat_xyzw[2] * inv;
				w = quat_xyzw[3] * inv;
			}
		}
		const float rot[3][3] = {
			{1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - z * w), 2.0f * (x * z + y * w)},
			{2.0f * (x * y + z * w), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z - x * w)},
			{2.0f * (x * z - y * w), 2.0f * (y * z + x * w), 1.0f - 2.0f * (x * x + y * y)},
		};
		Mat4 r;
		for (int col = 0; col < 3; col++)
		{
			const float s = scale ? scale[col] : 1.0f;
			for (int row = 0; row < 3; row++)
				r.m[col * 4 + row] = rot[row][col] * s;
		}
		if (pos)
		{
			r.m[12] = pos[0];
			r.m[13] = pos[1];
			r.m[14] = pos[2];
		}
		return r;
	}

	Mat4 ViewFromAxes(const float eye[3], const float right[3], const float up[3], const float back[3])
	{
		const float* const rows[3] = {right, up, back};
		Mat4 v;
		for (int row = 0; row < 3; row++)
		{
			v.m[0 * 4 + row] = rows[row][0];
			v.m[1 * 4 + row] = rows[row][1];
			v.m[2 * 4 + row] = rows[row][2];
			v.m[3 * 4 + row] = -Dot3(rows[row], eye);
		}
		return v;
	}

	Mat4 LookAt(const float eye[3], const float target[3], const float up[3])
	{
		float fwd[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
		if (!Normalize3(fwd))
		{
			fwd[0] = 0.0f;
			fwd[1] = 0.0f;
			fwd[2] = -1.0f;
		}
		float right[3];
		Cross3(fwd, up, right);
		if (!Normalize3(right))
		{
			// Looking along up: any perpendicular will do.
			const float alt[3] = {1.0f, 0.0f, 0.0f};
			Cross3(fwd, alt, right);
			if (!Normalize3(right))
			{
				right[0] = 0.0f;
				right[1] = 0.0f;
				right[2] = 1.0f;
			}
		}
		float true_up[3];
		Cross3(right, fwd, true_up);
		const float back[3] = {-fwd[0], -fwd[1], -fwd[2]};
		return ViewFromAxes(eye, right, true_up, back);
	}

	Camera Camera::Perspective(const Mat4& view, float fov_y_rad, float aspect, float near_m, float far_m)
	{
		const float t = std::tan(0.5f * fov_y_rad);
		return OffAxis(view, -t * aspect, t * aspect, -t, t, near_m, far_m);
	}

	Camera Camera::OffAxis(const Mat4& view, float tan_left, float tan_right, float tan_down, float tan_up, float near_m,
		float far_m)
	{
		Camera c;
		c.view = view;
		c.orthographic = false;
		std::memset(c.proj.m, 0, sizeof(c.proj.m));
		const float w = tan_right - tan_left;
		const float h = tan_up - tan_down;
		const float range = far_m - near_m;
		c.proj.m[0] = 2.0f / w;
		c.proj.m[5] = 2.0f / h;
		c.proj.m[8] = (tan_right + tan_left) / w;
		c.proj.m[9] = (tan_up + tan_down) / h;
		c.proj.m[10] = -far_m / range;
		c.proj.m[11] = -1.0f;
		c.proj.m[14] = -far_m * near_m / range;
		return c;
	}

	Camera Camera::Orthographic(const Mat4& view, float left, float right, float bottom, float top, float near_m,
		float far_m)
	{
		Camera c;
		c.view = view;
		c.orthographic = true;
		std::memset(c.proj.m, 0, sizeof(c.proj.m));
		const float w = right - left;
		const float h = top - bottom;
		const float range = far_m - near_m;
		c.proj.m[0] = 2.0f / w;
		c.proj.m[5] = 2.0f / h;
		c.proj.m[10] = -1.0f / range;
		c.proj.m[12] = -(right + left) / w;
		c.proj.m[13] = -(top + bottom) / h;
		c.proj.m[14] = -near_m / range;
		c.proj.m[15] = 1.0f;
		return c;
	}

	bool Camera::Impostor(const float eye[3], const float quad_centre[3], const float quad_right[3],
		const float quad_up[3], float quad_size_m, float near_m, float far_m, Camera* out)
	{
		if (!out || !(quad_size_m > 0.0f) || !(near_m > 0.0f) || !(far_m > near_m) || !IsFinite(quad_size_m) ||
			!AllFinite(eye, 3) || !AllFinite(quad_centre, 3) || !AllFinite(quad_right, 3) || !AllFinite(quad_up, 3))
		{
			return false;
		}
		// The same frame HandModel::RenderImpostor builds: right normalised, the quad's normal from right x up, and up made
		// perpendicular again.
		float r[3] = {quad_right[0], quad_right[1], quad_right[2]};
		if (!Normalize3(r))
			return false;
		float n[3];
		Cross3(r, quad_up, n);
		if (!Normalize3(n))
			return false;
		float u[3];
		Cross3(n, r, u);

		const float ec[3] = {eye[0] - quad_centre[0], eye[1] - quad_centre[1], eye[2] - quad_centre[2]};
		const float de = Dot3(ec, n); // the eye's distance in front of the quad
		if (!(de >= 0.02f))
			return false;
		// The picture is the square of side quad_size_m on the plane at distance de, centred (ec_r, ec_u) away from where
		// the eye's axis meets it: an off-axis frustum, its edges in tangents at distance 1.
		const float ec_r = Dot3(ec, r);
		const float ec_u = Dot3(ec, u);
		const float half = 0.5f * quad_size_m;
		*out = OffAxis(ViewFromAxes(eye, r, u, n), (-half - ec_r) / de, (half - ec_r) / de, (-half - ec_u) / de,
			(half - ec_u) / de, near_m, far_m);
		return true;
	}

	bool ProjectPoint(const Camera& camera, u32 width, u32 height, const float world[3], float out_px[2], float* out_depth)
	{
		if (!world || !out_px || !AllFinite(world, 3))
			return false;
		const Mat4 vp = Multiply(camera.proj, camera.view);
		const float x = vp.m[0] * world[0] + vp.m[4] * world[1] + vp.m[8] * world[2] + vp.m[12];
		const float y = vp.m[1] * world[0] + vp.m[5] * world[1] + vp.m[9] * world[2] + vp.m[13];
		const float z = vp.m[2] * world[0] + vp.m[6] * world[1] + vp.m[10] * world[2] + vp.m[14];
		const float w = vp.m[3] * world[0] + vp.m[7] * world[1] + vp.m[11] * world[2] + vp.m[15];
		if (!(w > 0.0f) || !IsFinite(w))
			return false;
		const float inv = 1.0f / w;
		out_px[0] = (x * inv * 0.5f + 0.5f) * static_cast<float>(width);
		out_px[1] = (0.5f - y * inv * 0.5f) * static_cast<float>(height);
		if (out_depth)
			*out_depth = z * inv;
		return IsFinite(out_px[0]) && IsFinite(out_px[1]);
	}

	// --- Target --------------------------------------------------------------------------------------------------------

	void Target::Rect::Add(u32 ax0, u32 ay0, u32 ax1, u32 ay1)
	{
		if (ax1 <= ax0 || ay1 <= ay0)
			return;
		if (Empty())
		{
			x0 = ax0;
			y0 = ay0;
			x1 = ax1;
			y1 = ay1;
			return;
		}
		x0 = std::min(x0, ax0);
		y0 = std::min(y0, ay0);
		x1 = std::max(x1, ax1);
		y1 = std::max(y1, ay1);
	}

	Target::Target(u32 width, u32 height, u32 samples_per_axis, u32 vertex_capacity)
	{
		m_vertices.resize(vertex_capacity);
		Resize(width, height, samples_per_axis);
	}

	bool Target::Resize(u32 width, u32 height, u32 samples_per_axis)
	{
		m_dirty = Rect();
		m_clean = 0u;
		const size_t samples = static_cast<size_t>(width) * samples_per_axis * height * samples_per_axis;
		if (width == 0 || height == 0 || width > kMaxSize || height > kMaxSize ||
			(samples_per_axis != 1 && samples_per_axis != 2 && samples_per_axis != 4) || samples > kMaxSamples)
		{
			m_width = 0;
			m_height = 0;
			m_grid = 1;
			m_colour.clear();
			m_depth.clear();
			m_pixels.clear();
			return false;
		}
		m_width = width;
		m_height = height;
		m_grid = samples_per_axis;
		m_colour.assign(samples, 0u);
		m_depth.assign(samples, 1.0f);
		if (samples_per_axis > 1)
			m_pixels.assign(static_cast<size_t>(width) * height, 0u);
		else
			m_pixels.clear();
		return true;
	}

	void Target::Clear(u32 colour)
	{
		if (m_width == 0)
			return;
		const size_t sw = static_cast<size_t>(m_width) * m_grid;
		if (colour == m_clean)
		{
			// Everything outside the part the draws reached is already clear: redo only that part (and the pixels the last
			// Resolve may have made from it).
			if (!m_dirty.Empty())
			{
				for (u32 y = m_dirty.y0; y < m_dirty.y1; y++)
				{
					u32* colour_row = m_colour.data() + static_cast<size_t>(y) * sw;
					float* depth_row = m_depth.data() + static_cast<size_t>(y) * sw;
					std::fill(colour_row + m_dirty.x0, colour_row + m_dirty.x1, colour);
					std::fill(depth_row + m_dirty.x0, depth_row + m_dirty.x1, 1.0f);
				}
				if (m_grid > 1)
				{
					const u32 px0 = m_dirty.x0 / m_grid;
					const u32 px1 = std::min((m_dirty.x1 + m_grid - 1) / m_grid, m_width);
					const u32 py0 = m_dirty.y0 / m_grid;
					const u32 py1 = std::min((m_dirty.y1 + m_grid - 1) / m_grid, m_height);
					for (u32 y = py0; y < py1; y++)
					{
						u32* pixel_row = m_pixels.data() + static_cast<size_t>(y) * m_width;
						std::fill(pixel_row + px0, pixel_row + px1, colour);
					}
				}
			}
		}
		else
		{
			std::fill(m_colour.begin(), m_colour.end(), colour);
			std::fill(m_depth.begin(), m_depth.end(), 1.0f);
			if (m_grid > 1)
				std::fill(m_pixels.begin(), m_pixels.end(), colour);
			m_clean = colour;
		}
		m_dirty = Rect();
	}

	void Target::Resolve()
	{
		if (m_grid == 1 || m_width == 0 || m_dirty.Empty())
			return;
		const u32 n = m_grid;
		const u32 px0 = m_dirty.x0 / n;
		const u32 px1 = std::min((m_dirty.x1 + n - 1) / n, m_width);
		const u32 py0 = m_dirty.y0 / n;
		const u32 py1 = std::min((m_dirty.y1 + n - 1) / n, m_height);
		const size_t sw = static_cast<size_t>(m_width) * n;
		// A box filter over the n x n samples of a pixel, two channels to each half of a 32-bit word so that the sums of up to
		// sixteen bytes cannot spill into the next lane; rounded to nearest.
		const u32 shift = (n == 2) ? 2u : 4u;
		const u32 rounding = (n * n / 2) * 0x00010001u;
		for (u32 y = py0; y < py1; y++)
		{
			u32* out_row = m_pixels.data() + static_cast<size_t>(y) * m_width;
			for (u32 x = px0; x < px1; x++)
			{
				u32 rb = 0;
				u32 ag = 0;
				for (u32 sy = 0; sy < n; sy++)
				{
					const u32* row = m_colour.data() + (static_cast<size_t>(y) * n + sy) * sw + static_cast<size_t>(x) * n;
					for (u32 sx = 0; sx < n; sx++)
					{
						rb += row[sx] & 0x00FF00FFu;
						ag += (row[sx] >> 8) & 0x00FF00FFu;
					}
				}
				rb = ((rb + rounding) >> shift) & 0x00FF00FFu;
				ag = ((ag + rounding) >> shift) & 0x00FF00FFu;
				out_row[x] = rb | (ag << 8);
			}
		}
	}

	void Target::CopyUnpremultiplied(u32* out_rgba) const
	{
		if (!out_rgba || m_width == 0)
			return;
		const u32* src = Pixels();
		const size_t count = static_cast<size_t>(m_width) * m_height;
		for (size_t i = 0; i < count; i++)
		{
			const u32 p = src[i];
			const u32 a = p >> 24;
			if (a == 255u)
			{
				out_rgba[i] = p;
				continue;
			}
			if (a == 0u)
			{
				out_rgba[i] = 0u;
				continue;
			}
			const u32 r = std::min(255u, ((p & 0xFFu) * 255u + a / 2u) / a);
			const u32 g = std::min(255u, (((p >> 8) & 0xFFu) * 255u + a / 2u) / a);
			const u32 b = std::min(255u, (((p >> 16) & 0xFFu) * 255u + a / 2u) / a);
			out_rgba[i] = r | (g << 8) | (b << 16) | (a << 24);
		}
	}

	// --- Drawing -------------------------------------------------------------------------------------------------------

	namespace
	{
		// What a Draw works from: the mesh's arrays, the matrices and the light, set up once.
		struct DrawState
		{
			const u8* pos = nullptr;
			size_t pos_stride = 12;
			const u8* nrm = nullptr;
			size_t nrm_stride = 12;
			const u8* uv = nullptr;
			size_t uv_stride = 8;
			float model[12] = {}; // rows 0 to 2 of the model matrix, 4 columns
			float vp[16] = {};    // projection * view, column-major
			float nmat[9] = {};   // normals to world space (row-major), mirrored models included
			float eye[3] = {};
			float to_eye[3] = {}; // orthographic: the unit vector from a surface to the viewer
			float light[3] = {};  // world space, unit, towards the light
			bool ortho = false;
			bool lit = false;
			bool two_sided = false;
			bool linear = true;
			float ambient = 0.0f;
			float key = 0.0f;
			float headlight = 0.0f;
			float rim = 0.0f;
			int rim_power = 3;
		};

		// The light on a surface whose normal makes ndl with the light and ndv with the viewer: the factor on the colour.
		float LightFactor(const DrawState& ds, float ndl, float ndv)
		{
			const float l = Unit(ds.ambient + ds.key * std::max(ndl, 0.0f) + ds.headlight * Unit(ndv));
			// With the colours as sRGB bytes, a factor in linear light multiplies them as its approximate gamma curve.
			return ds.linear ? std::sqrt(l) : l;
		}

		float RimFactor(const DrawState& ds, float ndv)
		{
			const float edge = 1.0f - Unit(ndv);
			float r = edge;
			for (int i = 1; i < ds.rim_power; i++)
				r *= edge;
			r = Unit(ds.rim * r);
			return ds.linear ? std::sqrt(r) : r;
		}

		// A mesh vertex through the model, view and projection, with its light.
		void ShadeVertex(const DrawState& ds, u32 index, detail::ClipVertex& o)
		{
			const float* p = reinterpret_cast<const float*>(ds.pos + static_cast<size_t>(index) * ds.pos_stride);
			const float wx = ds.model[0] * p[0] + ds.model[1] * p[1] + ds.model[2] * p[2] + ds.model[3];
			const float wy = ds.model[4] * p[0] + ds.model[5] * p[1] + ds.model[6] * p[2] + ds.model[7];
			const float wz = ds.model[8] * p[0] + ds.model[9] * p[1] + ds.model[10] * p[2] + ds.model[11];
			o.x = ds.vp[0] * wx + ds.vp[4] * wy + ds.vp[8] * wz + ds.vp[12];
			o.y = ds.vp[1] * wx + ds.vp[5] * wy + ds.vp[9] * wz + ds.vp[13];
			o.z = ds.vp[2] * wx + ds.vp[6] * wy + ds.vp[10] * wz + ds.vp[14];
			o.w = ds.vp[3] * wx + ds.vp[7] * wy + ds.vp[11] * wz + ds.vp[15];

			if (!(IsFinite(o.x) && IsFinite(o.y) && IsFinite(o.z) && IsFinite(o.w)))
			{
				o.code = kBad;
			}
			else
			{
				const float gw = kGuard * o.w;
				o.code = (o.z < 0.0f ? kNear : 0u) | (o.z > o.w ? kFar : 0u) | (o.x < -o.w ? kLeft : 0u) |
				         (o.x > o.w ? kRight : 0u) | (o.y < -o.w ? kBottom : 0u) | (o.y > o.w ? kTop : 0u) |
				         (o.x < -gw ? kGuardLeft : 0u) | (o.x > gw ? kGuardRight : 0u) | (o.y < -gw ? kGuardBottom : 0u) |
				         (o.y > gw ? kGuardTop : 0u);
			}

			if (ds.uv)
			{
				const float* t = reinterpret_cast<const float*>(ds.uv + static_cast<size_t>(index) * ds.uv_stride);
				o.u = t[0];
				o.v = t[1];
			}
			else
			{
				o.u = 0.0f;
				o.v = 0.0f;
			}

			o.lit = 1.0f;
			o.rim = 0.0f;
			o.lit_b = 1.0f;
			o.rim_b = 0.0f;
			if (!ds.lit)
				return;
			const float* n = reinterpret_cast<const float*>(ds.nrm + static_cast<size_t>(index) * ds.nrm_stride);
			float nw[3] = {ds.nmat[0] * n[0] + ds.nmat[1] * n[1] + ds.nmat[2] * n[2],
				ds.nmat[3] * n[0] + ds.nmat[4] * n[1] + ds.nmat[5] * n[2],
				ds.nmat[6] * n[0] + ds.nmat[7] * n[1] + ds.nmat[8] * n[2]};
			if (!Normalize3(nw))
			{
				nw[0] = 0.0f;
				nw[1] = 0.0f;
				nw[2] = 1.0f;
			}
			float v[3] = {ds.to_eye[0], ds.to_eye[1], ds.to_eye[2]};
			if (!ds.ortho)
			{
				v[0] = ds.eye[0] - wx;
				v[1] = ds.eye[1] - wy;
				v[2] = ds.eye[2] - wz;
				if (!Normalize3(v))
				{
					v[0] = 0.0f;
					v[1] = 0.0f;
					v[2] = 1.0f;
				}
			}
			const float ndl = Dot3(nw, ds.light);
			const float ndv = Dot3(nw, v);
			o.lit = LightFactor(ds, ndl, ndv);
			if (ds.rim > 0.0f)
				o.rim = RimFactor(ds, ndv);
			if (ds.two_sided)
			{
				o.lit_b = LightFactor(ds, -ndl, -ndv);
				if (ds.rim > 0.0f)
					o.rim_b = RimFactor(ds, -ndv);
			}
		}

		// A corner while the triangle is clipped.
		struct PolyVert
		{
			float x, y, z, w;
			float u, v;
			float lit, rim;
		};

		float PlaneDistance(const PolyVert& p, int plane)
		{
			switch (plane)
			{
				case 0: return p.z;
				case 1: return p.w - p.z;
				case 2: return kGuard * p.w + p.x;
				case 3: return kGuard * p.w - p.x;
				case 4: return kGuard * p.w + p.y;
				default: return kGuard * p.w - p.y;
			}
		}

		// Clips a convex polygon against one plane (inside where the distance is not negative). A cut point is always
		// worked out from the inside corner toward the outside one, whichever way the edge is walked, so two triangles that
		// share an edge cut it at bit-identical points.
		int ClipAgainst(const PolyVert* in, int count, int plane, PolyVert* out)
		{
			int made = 0;
			for (int i = 0; i < count; i++)
			{
				const PolyVert& cur = in[i];
				const PolyVert& nxt = in[(i + 1 == count) ? 0 : i + 1];
				const float dc = PlaneDistance(cur, plane);
				const float dn = PlaneDistance(nxt, plane);
				const bool cur_in = dc >= 0.0f;
				const bool nxt_in = dn >= 0.0f;
				if (cur_in)
					out[made++] = cur;
				if (cur_in != nxt_in)
				{
					const PolyVert& inside = cur_in ? cur : nxt;
					const PolyVert& outside = cur_in ? nxt : cur;
					const float d_in = cur_in ? dc : dn;
					const float d_out = cur_in ? dn : dc;
					const float t = d_in / (d_in - d_out);
					PolyVert& cut = out[made++];
					cut.x = inside.x + (outside.x - inside.x) * t;
					cut.y = inside.y + (outside.y - inside.y) * t;
					cut.z = inside.z + (outside.z - inside.z) * t;
					cut.w = inside.w + (outside.w - inside.w) * t;
					cut.u = inside.u + (outside.u - inside.u) * t;
					cut.v = inside.v + (outside.v - inside.v) * t;
					cut.lit = inside.lit + (outside.lit - inside.lit) * t;
					cut.rim = inside.rim + (outside.rim - inside.rim) * t;
				}
			}
			return made;
		}

		// A corner in sample space: its position in 1/256 of a sample, and what is interpolated across the triangle: the
		// depth and the light as they are, the texture coordinates divided by w so that they come out perspective-correct.
		struct ScreenVert
		{
			s32 x, y;
			float z, iw, uw, vw, lit, rim;
		};

		// How the colour of a sample is made, set once per Draw.
		struct SampleState
		{
			const u32* texels = nullptr;
			s32 tex_w = 0;
			s32 tex_h = 0;
			float tex_wf = 0.0f;
			float tex_hf = 0.0f;
			float u_scale = 0.0f; // 256 texels' worth of u and v, for the power-of-two path
			float v_scale = 0.0f;
			s32 mask_u = 0;
			s32 mask_v = 0;
			bool pot = false; // both sides a power of two, so wrapping is a mask
			bool repeat_u = true;
			bool repeat_v = true;
			bool base_white = true;
			u32 base[3] = {256u, 256u, 256u}; // base colour, 0 to 256
			bool rim_on = false;
			u32 rim_colour[3] = {256u, 256u, 256u};
			s32 cutoff = 0; // texel alpha below this is not drawn; 0: off
		};

		struct Bounds
		{
			s32 x0 = INT_MAX;
			s32 y0 = INT_MAX;
			s32 x1 = -1;
			s32 y1 = -1;
		};

		struct FillContext
		{
			u32* colour = nullptr;
			float* depth = nullptr;
			s32 sw = 0;
			s32 sh = 0;
			SampleState ss;
			DrawStats* stats = nullptr;
			Bounds bounds;
		};

		// A texture coordinate that is safe to turn into an index: a NaN or a huge value becomes something in range.
		__fi float SafeCoord(float t)
		{
			return (t > -1.0e6f) ? ((t < 1.0e6f) ? t : 1.0e6f) : -1.0e6f;
		}

		// For a texture whose sides are not powers of two: u as 0 to 1, repeating or clamped.
		__fi float WrapRepeat(float t)
		{
			t = SafeCoord(t);
			return t - static_cast<float>(FloorToInt(t));
		}

		__fi float WrapClamp(float t)
		{
			return (t > 0.0f) ? ((t < 1.0f) ? t : 1.0f) : 0.0f;
		}

		// The four bytes of a texel in 16-bit lanes of a 64-bit word (R, B, G, A from the bottom), so that a weighted sum
		// of texels cannot spill from one channel into the next.
		__fi u64 Expand(u32 t)
		{
			return (static_cast<u64>(t & 0xFF00FF00u) << 24) | (t & 0x00FF00FFu);
		}

		// The bilinear blend of four texels, fx and fy (0 to 255) the fractions towards the right and lower ones: weights in
		// 256ths that add up to exactly 256.
		__fi u32 Blend4(u32 t00, u32 t10, u32 t01, u32 t11, u32 fx, u32 fy)
		{
			const u32 w11 = (fx * fy) >> 8;
			const u32 w10 = fx - w11;
			const u32 w01 = fy - w11;
			const u32 w00 = 256u - fx - fy + w11;
			const u64 sum = (Expand(t00) * w00 + Expand(t10) * w10 + Expand(t01) * w01 + Expand(t11) * w11 + 0x0080008000800080ull) >> 8;
			return static_cast<u32>(sum & 0xFFu) | static_cast<u32>(sum & 0xFF0000u) | static_cast<u32>((sum >> 24) & 0xFF00u) |
			       static_cast<u32>((sum >> 24) & 0xFF000000u);
		}

		// A texture coordinate in 1/256 texels (an s64, so that a wild coordinate cannot overflow it).
		__fi s64 TexelFixed(float t, float scale)
		{
			return static_cast<s64>(SafeCoord(t) * scale);
		}

		__fi u32 SampleNearest(const SampleState& s, float u, float v)
		{
			s32 x;
			s32 y;
			if (s.pot)
			{
				const s64 fu = TexelFixed(u, s.u_scale) >> kSubBits;
				const s64 fv = TexelFixed(v, s.v_scale) >> kSubBits;
				x = s.repeat_u ? static_cast<s32>(fu & s.mask_u) : static_cast<s32>(std::clamp<s64>(fu, 0, s.tex_w - 1));
				y = s.repeat_v ? static_cast<s32>(fv & s.mask_v) : static_cast<s32>(std::clamp<s64>(fv, 0, s.tex_h - 1));
			}
			else
			{
				const float fu = s.repeat_u ? WrapRepeat(u) : WrapClamp(u);
				const float fv = s.repeat_v ? WrapRepeat(v) : WrapClamp(v);
				x = std::clamp(static_cast<s32>(fu * s.tex_wf), 0, s.tex_w - 1);
				y = std::clamp(static_cast<s32>(fv * s.tex_hf), 0, s.tex_h - 1);
			}
			return s.texels[static_cast<size_t>(y) * s.tex_w + x];
		}

		__fi u32 SampleBilinear(const SampleState& s, float u, float v)
		{
			s32 x0;
			s32 x1;
			s32 y0;
			s32 y1;
			u32 fx;
			u32 fy;
			if (s.pot)
			{
				// Texel centres are half a texel in from the edges.
				const s64 fu = TexelFixed(u, s.u_scale) - kSub / 2;
				const s64 fv = TexelFixed(v, s.v_scale) - kSub / 2;
				const s64 xi = fu >> kSubBits;
				const s64 yi = fv >> kSubBits;
				fx = static_cast<u32>(fu & (kSub - 1));
				fy = static_cast<u32>(fv & (kSub - 1));
				if (s.repeat_u)
				{
					x0 = static_cast<s32>(xi & s.mask_u);
					x1 = static_cast<s32>((xi + 1) & s.mask_u);
				}
				else
				{
					x0 = static_cast<s32>(std::clamp<s64>(xi, 0, s.tex_w - 1));
					x1 = static_cast<s32>(std::clamp<s64>(xi + 1, 0, s.tex_w - 1));
				}
				if (s.repeat_v)
				{
					y0 = static_cast<s32>(yi & s.mask_v);
					y1 = static_cast<s32>((yi + 1) & s.mask_v);
				}
				else
				{
					y0 = static_cast<s32>(std::clamp<s64>(yi, 0, s.tex_h - 1));
					y1 = static_cast<s32>(std::clamp<s64>(yi + 1, 0, s.tex_h - 1));
				}
			}
			else
			{
				const float tu = (s.repeat_u ? WrapRepeat(u) : WrapClamp(u)) * s.tex_wf - 0.5f;
				const float tv = (s.repeat_v ? WrapRepeat(v) : WrapClamp(v)) * s.tex_hf - 0.5f;
				x0 = FloorToInt(tu);
				y0 = FloorToInt(tv);
				fx = std::min(static_cast<u32>((tu - static_cast<float>(x0)) * 256.0f), 255u);
				fy = std::min(static_cast<u32>((tv - static_cast<float>(y0)) * 256.0f), 255u);
				x1 = x0 + 1;
				y1 = y0 + 1;
				if (s.repeat_u)
				{
					x0 = (x0 < 0) ? x0 + s.tex_w : x0;
					x1 = (x1 >= s.tex_w) ? x1 - s.tex_w : x1;
				}
				if (s.repeat_v)
				{
					y0 = (y0 < 0) ? y0 + s.tex_h : y0;
					y1 = (y1 >= s.tex_h) ? y1 - s.tex_h : y1;
				}
				// Clamped, the ends clamp to the edge texel; either way never outside the texture.
				x0 = std::clamp(x0, 0, s.tex_w - 1);
				x1 = std::clamp(x1, 0, s.tex_w - 1);
				y0 = std::clamp(y0, 0, s.tex_h - 1);
				y1 = std::clamp(y1, 0, s.tex_h - 1);
			}
			const u32* row0 = s.texels + static_cast<size_t>(y0) * s.tex_w;
			const u32* row1 = s.texels + static_cast<size_t>(y1) * s.tex_w;
			return Blend4(row0[x0], row0[x1], row1[x0], row1[x1], fx, fy);
		}

		// The plane through a triangle's three values of one quantity, as its value at the first sample the triangle's
		// bounding box holds and its change a sample to the right and a sample down.
		struct Plane
		{
			float q0, dx, dy;
		};

		// Fills one triangle into the samples. kFilter: 0 no texture, 1 nearest, 2 bilinear.
		template<int kFilter>
		void FillTriangle(FillContext& fc, const ScreenVert& a_in, const ScreenVert& b_in, const ScreenVert& c_in)
		{
			const ScreenVert* pa = &a_in;
			const ScreenVert* pb = &b_in;
			const ScreenVert* pc = &c_in;
			// Edge functions are positive inside when the corners run this way round (in sample space, y down).
			s64 area2 = static_cast<s64>(pb->x - pa->x) * (pc->y - pa->y) - static_cast<s64>(pb->y - pa->y) * (pc->x - pa->x);
			if (area2 == 0)
				return;
			if (area2 < 0)
			{
				std::swap(pb, pc);
				area2 = -area2;
			}
			const ScreenVert& a = *pa;
			const ScreenVert& b = *pb;
			const ScreenVert& c = *pc;

			// The samples whose centres the bounding box can hold: the first at or after its left edge, the last at or
			// before its right (centres are at 128 + 256 n in sub-sample units).
			s32 x0 = (std::min({a.x, b.x, c.x}) + (kSub / 2 - 1)) >> kSubBits;
			s32 x1 = (std::max({a.x, b.x, c.x}) - kSub / 2) >> kSubBits;
			s32 y0 = (std::min({a.y, b.y, c.y}) + (kSub / 2 - 1)) >> kSubBits;
			s32 y1 = (std::max({a.y, b.y, c.y}) - kSub / 2) >> kSubBits;
			x0 = std::max(x0, 0);
			y0 = std::max(y0, 0);
			x1 = std::min(x1, fc.sw - 1);
			y1 = std::min(y1, fc.sh - 1);
			if (x0 > x1 || y0 > y1)
				return;

			// Edge functions a->b, b->c, c->a at the first sample, and how each changes a sample to the right and down. A
			// sample exactly on an edge counts only for an edge that is a top or left edge (one that goes up, or goes right
			// along the top): the rest start a hair outside.
			const s64 dx0 = b.x - a.x, dy0 = b.y - a.y;
			const s64 dx1 = c.x - b.x, dy1 = c.y - b.y;
			const s64 dx2 = a.x - c.x, dy2 = a.y - c.y;
			const s64 first_x = static_cast<s64>(x0) * kSub + kSub / 2;
			const s64 first_y = static_cast<s64>(y0) * kSub + kSub / 2;
			const auto bias = [](s64 dx, s64 dy) -> s64 { return (dy < 0 || (dy == 0 && dx > 0)) ? 0 : -1; };
			s64 row0 = dx0 * (first_y - a.y) - dy0 * (first_x - a.x) + bias(dx0, dy0);
			s64 row1 = dx1 * (first_y - b.y) - dy1 * (first_x - b.x) + bias(dx1, dy1);
			s64 row2 = dx2 * (first_y - c.y) - dy2 * (first_x - c.x) + bias(dx2, dy2);
			const s64 sx0 = -dy0 * kSub, sx1 = -dy1 * kSub, sx2 = -dy2 * kSub;
			const s64 sy0 = dx0 * kSub, sy1 = dx1 * kSub, sy2 = dx2 * kSub;

			// The attribute planes, from the snapped corners so that they agree with what is covered.
			const float ex1 = static_cast<float>(b.x - a.x) * kSubInv;
			const float ey1 = static_cast<float>(b.y - a.y) * kSubInv;
			const float ex2 = static_cast<float>(c.x - a.x) * kSubInv;
			const float ey2 = static_cast<float>(c.y - a.y) * kSubInv;
			const float area = ex1 * ey2 - ex2 * ey1;
			if (!(area > 0.0f))
				return;
			const float inv_area = 1.0f / area;
			const float ox = (static_cast<float>(x0) + 0.5f) - static_cast<float>(a.x) * kSubInv;
			const float oy = (static_cast<float>(y0) + 0.5f) - static_cast<float>(a.y) * kSubInv;
			const auto make_plane = [&](float qa, float qb, float qc) -> Plane {
				const float d1 = qb - qa;
				const float d2 = qc - qa;
				Plane p;
				p.dx = (d1 * ey2 - d2 * ey1) * inv_area;
				p.dy = (d2 * ex1 - d1 * ex2) * inv_area;
				p.q0 = qa + p.dx * ox + p.dy * oy;
				return p;
			};
			// A copy of the sampling state, so that the stores below cannot be taken to change it.
			const SampleState ss = fc.ss;
			const Plane pz = make_plane(a.z, b.z, c.z);
			const Plane plit = make_plane(a.lit, b.lit, c.lit);
			Plane prim = {};
			if (ss.rim_on)
				prim = make_plane(a.rim, b.rim, c.rim);
			Plane piw = {};
			Plane pu = {};
			Plane pv = {};
			if constexpr (kFilter != 0)
			{
				piw = make_plane(a.iw, b.iw, c.iw);
				pu = make_plane(a.uw, b.uw, c.uw);
				pv = make_plane(a.vw, b.vw, c.vw);
			}

			u64 covered = 0;
			u64 written = 0;
			for (s32 y = y0; y <= y1; y++)
			{
				const float fy = static_cast<float>(y - y0);
				const float z_row = pz.q0 + pz.dy * fy;
				const float lit_row = plit.q0 + plit.dy * fy;
				const float rim_row = prim.q0 + prim.dy * fy;
				float iw_row = 0.0f;
				float u_row = 0.0f;
				float v_row = 0.0f;
				if constexpr (kFilter != 0)
				{
					iw_row = piw.q0 + piw.dy * fy;
					u_row = pu.q0 + pu.dy * fy;
					v_row = pv.q0 + pv.dy * fy;
				}
				u32* crow = fc.colour + static_cast<size_t>(y) * fc.sw;
				float* drow = fc.depth + static_cast<size_t>(y) * fc.sw;
				s64 e0 = row0;
				s64 e1 = row1;
				s64 e2 = row2;
				float fx = 0.0f;
				bool seen = false;
				for (s32 x = x0; x <= x1; x++, fx += 1.0f, e0 += sx0, e1 += sx1, e2 += sx2)
				{
					if ((e0 | e1 | e2) < 0)
					{
						// The triangle is convex: past its far edge on this row there is nothing more.
						if (seen)
							break;
						continue;
					}
					seen = true;
					covered++;
					const float zf = z_row + pz.dx * fx;
					if (!(zf < drow[x]))
						continue;

					u32 texel = 0xFFFFFFFFu;
					if constexpr (kFilter != 0)
					{
						const float rw = 1.0f / (iw_row + piw.dx * fx);
						const float tu = (u_row + pu.dx * fx) * rw;
						const float tv = (v_row + pv.dx * fx) * rw;
						if constexpr (kFilter == 1)
							texel = SampleNearest(ss, tu, tv);
						else
							texel = SampleBilinear(ss, tu, tv);
						if (static_cast<s32>(texel >> 24) < ss.cutoff)
							continue;
					}

					// Texel * base * light, in 8.8 fixed point: a full factor leaves the byte as it was.
					const u32 lq = Q256(lit_row + plit.dx * fx);
					u32 r;
					u32 g;
					u32 bl;
					if constexpr (kFilter == 0)
					{
						if (ss.base_white)
						{
							r = (255u * lq) >> 8;
							g = r;
							bl = r;
						}
						else
						{
							r = (255u * ((lq * ss.base[0]) >> 8)) >> 8;
							g = (255u * ((lq * ss.base[1]) >> 8)) >> 8;
							bl = (255u * ((lq * ss.base[2]) >> 8)) >> 8;
						}
					}
					else
					{
						const u32 mr = ss.base_white ? lq : (lq * ss.base[0]) >> 8;
						const u32 mg = ss.base_white ? lq : (lq * ss.base[1]) >> 8;
						const u32 mb = ss.base_white ? lq : (lq * ss.base[2]) >> 8;
						r = ((texel & 0xFFu) * mr) >> 8;
						g = (((texel >> 8) & 0xFFu) * mg) >> 8;
						bl = (((texel >> 16) & 0xFFu) * mb) >> 8;
					}
					if (ss.rim_on)
					{
						const u32 rq = Q256(rim_row + prim.dx * fx);
						r = std::min(255u, r + ((rq * ss.rim_colour[0]) >> 8));
						g = std::min(255u, g + ((rq * ss.rim_colour[1]) >> 8));
						bl = std::min(255u, bl + ((rq * ss.rim_colour[2]) >> 8));
					}
					crow[x] = 0xFF000000u | (bl << 16) | (g << 8) | r;
					drow[x] = zf;
					written++;
				}
				row0 += sy0;
				row1 += sy1;
				row2 += sy2;
			}

			fc.stats->covered += covered;
			fc.stats->written += written;
			fc.stats->rasterised++;
			if (covered != 0)
			{
				fc.bounds.x0 = std::min(fc.bounds.x0, x0);
				fc.bounds.y0 = std::min(fc.bounds.y0, y0);
				fc.bounds.x1 = std::max(fc.bounds.x1, x1);
				fc.bounds.y1 = std::max(fc.bounds.y1, y1);
			}
		}

		using FillFn = void (*)(FillContext&, const ScreenVert&, const ScreenVert&, const ScreenVert&);
	}

	namespace detail
	{
		struct Rasteriser
		{
			static bool Run(Target& target, const Camera& camera, const Mesh& mesh, const Mat4& model,
				const Material& material, DrawStats& stats)
			{
				stats = DrawStats();
				if (target.Empty() || !mesh.positions || mesh.vertex_count == 0)
					return false;
				if (mesh.indices16 && mesh.indices32)
					return false;
				const bool indexed = mesh.indices16 || mesh.indices32;
				const u32 tri_count = (indexed ? mesh.index_count : mesh.vertex_count) / 3;
				stats.triangles = tri_count;
				if (tri_count == 0 || !AllFinite(model.m, 16) || !AllFinite(camera.view.m, 16) || !AllFinite(camera.proj.m, 16))
					return false;

				// The model's 3 x 3 and its cofactor matrix (the inverse transpose times the determinant), which turns a
				// normal; a mirroring model (negative determinant) swaps which way a triangle runs and flips the cofactor's
				// normals, both put right below.
				const float* mm = model.m;
				const float a00 = mm[0], a01 = mm[4], a02 = mm[8];
				const float a10 = mm[1], a11 = mm[5], a12 = mm[9];
				const float a20 = mm[2], a21 = mm[6], a22 = mm[10];
				const float det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
				if (!IsFinite(det) || std::fabs(det) < 1.0e-18f)
					return false;
				const float mirror = (det < 0.0f) ? -1.0f : 1.0f;

				DrawState ds;
				ds.pos = reinterpret_cast<const u8*>(mesh.positions);
				ds.pos_stride = mesh.position_stride ? mesh.position_stride : 3 * sizeof(float);
				ds.nrm = reinterpret_cast<const u8*>(mesh.normals);
				ds.nrm_stride = mesh.normal_stride ? mesh.normal_stride : 3 * sizeof(float);
				ds.uv = reinterpret_cast<const u8*>(mesh.uvs);
				ds.uv_stride = mesh.uv_stride ? mesh.uv_stride : 2 * sizeof(float);
				for (int row = 0; row < 3; row++)
				{
					for (int col = 0; col < 4; col++)
						ds.model[row * 4 + col] = mm[col * 4 + row];
				}
				const Mat4 vp = Multiply(camera.proj, camera.view);
				std::memcpy(ds.vp, vp.m, sizeof(ds.vp));
				const float cof[9] = {a11 * a22 - a12 * a21, a12 * a20 - a10 * a22, a10 * a21 - a11 * a20,
					a02 * a21 - a01 * a22, a00 * a22 - a02 * a20, a01 * a20 - a00 * a21,
					a01 * a12 - a02 * a11, a02 * a10 - a00 * a12, a00 * a11 - a01 * a10};
				for (int i = 0; i < 9; i++)
					ds.nmat[i] = cof[i] * mirror;

				// The camera's axes are the rows of the view's rotation, and its eye is where the view maps to the origin.
				const float* vm = camera.view.m;
				const float right[3] = {vm[0], vm[4], vm[8]};
				const float up[3] = {vm[1], vm[5], vm[9]};
				const float back[3] = {vm[2], vm[6], vm[10]};
				for (int i = 0; i < 3; i++)
					ds.eye[i] = -(vm[12] * right[i] + vm[13] * up[i] + vm[14] * back[i]);
				ds.ortho = camera.orthographic;
				ds.to_eye[0] = back[0];
				ds.to_eye[1] = back[1];
				ds.to_eye[2] = back[2];

				const Lighting& lt = material.lighting;
				ds.lit = lt.enabled && mesh.normals != nullptr;
				ds.two_sided = ds.lit && !material.cull_back;
				ds.linear = lt.linear_light;
				ds.ambient = IsFinite(lt.ambient) ? lt.ambient : 0.0f;
				ds.key = IsFinite(lt.key) ? lt.key : 0.0f;
				ds.headlight = IsFinite(lt.headlight) ? lt.headlight : 0.0f;
				ds.rim = IsFinite(lt.rim) ? lt.rim : 0.0f;
				ds.rim_power = std::clamp(lt.rim_power, 1, 8);
				float ld[3] = {lt.light_dir[0], lt.light_dir[1], lt.light_dir[2]};
				if (!AllFinite(ld, 3))
					ld[0] = ld[1] = ld[2] = 0.0f;
				if (lt.light_in_view_space)
				{
					// +X right, +Y up, +Z towards the viewer: the camera's own axes in world space.
					for (int i = 0; i < 3; i++)
						ds.light[i] = ld[0] * right[i] + ld[1] * up[i] + ld[2] * back[i];
				}
				else
				{
					ds.light[0] = ld[0];
					ds.light[1] = ld[1];
					ds.light[2] = ld[2];
				}
				if (!Normalize3(ds.light))
				{
					ds.light[0] = 0.0f;
					ds.light[1] = 1.0f;
					ds.light[2] = 0.0f;
				}

				FillContext fc;
				fc.colour = target.m_colour.data();
				fc.depth = target.m_depth.data();
				fc.sw = static_cast<s32>(target.SampleWidth());
				fc.sh = static_cast<s32>(target.SampleHeight());
				fc.stats = &stats;
				SampleState& ss = fc.ss;
				for (int i = 0; i < 3; i++)
				{
					ss.base[i] = Q256(material.base_colour[i]);
					ss.rim_colour[i] = Q256(lt.rim_colour[i]);
				}
				ss.base_white = (ss.base[0] == 256u && ss.base[1] == 256u && ss.base[2] == 256u);
				ss.rim_on = ds.lit && ds.rim > 0.0f;
				const float cutoff = std::clamp(IsFinite(material.alpha_cutoff) ? material.alpha_cutoff : 0.0f, 0.0f, 1.0f);
				ss.cutoff = static_cast<s32>(std::ceil(cutoff * 255.0f));
				int filter = 0;
				const Texture* tex = material.texture;
				if (tex && tex->texels && tex->width > 0 && tex->height > 0 && tex->width <= 65536 && tex->height <= 65536)
				{
					ss.texels = tex->texels;
					ss.tex_w = static_cast<s32>(tex->width);
					ss.tex_h = static_cast<s32>(tex->height);
					ss.tex_wf = static_cast<float>(tex->width);
					ss.tex_hf = static_cast<float>(tex->height);
					ss.u_scale = ss.tex_wf * static_cast<float>(kSub);
					ss.v_scale = ss.tex_hf * static_cast<float>(kSub);
					ss.mask_u = ss.tex_w - 1;
					ss.mask_v = ss.tex_h - 1;
					ss.pot = (tex->width & (tex->width - 1)) == 0 && (tex->height & (tex->height - 1)) == 0;
					ss.repeat_u = (tex->wrap_u == Texture::Wrap::Repeat);
					ss.repeat_v = (tex->wrap_v == Texture::Wrap::Repeat);
					filter = (tex->filter == Texture::Filter::Bilinear) ? 2 : 1;
				}
				const FillFn fill = (filter == 0) ? &FillTriangle<0> : ((filter == 1) ? &FillTriangle<1> : &FillTriangle<2>);

				// All the vertices go through the transform once and are shared by the triangles that use them, when they
				// fit the scratch; otherwise each triangle transforms its own three.
				const bool cached = mesh.vertex_count <= target.m_vertices.size();
				detail::ClipVertex* scratch = target.m_vertices.data();
				if (cached)
				{
					for (u32 i = 0; i < mesh.vertex_count; i++)
						ShadeVertex(ds, i, scratch[i]);
				}

				// Which way round counts as the front, as clip space sees it (the sign of a determinant that is the same
				// before and after the divide by w, so valid even for a triangle that crosses the eye's plane).
				const float front_sign = (mesh.front_ccw ? 1.0f : -1.0f) * mirror;
				const float fsw = static_cast<float>(fc.sw);
				const float fsh = static_cast<float>(fc.sh);

				for (u32 t = 0; t < tri_count; t++)
				{
					u32 idx[3];
					if (mesh.indices32)
					{
						idx[0] = mesh.indices32[t * 3];
						idx[1] = mesh.indices32[t * 3 + 1];
						idx[2] = mesh.indices32[t * 3 + 2];
					}
					else if (mesh.indices16)
					{
						idx[0] = mesh.indices16[t * 3];
						idx[1] = mesh.indices16[t * 3 + 1];
						idx[2] = mesh.indices16[t * 3 + 2];
					}
					else
					{
						idx[0] = t * 3;
						idx[1] = t * 3 + 1;
						idx[2] = t * 3 + 2;
					}
					if (idx[0] >= mesh.vertex_count || idx[1] >= mesh.vertex_count || idx[2] >= mesh.vertex_count)
					{
						stats.rejected++;
						continue;
					}

					detail::ClipVertex local[3];
					const detail::ClipVertex* v[3];
					for (int k = 0; k < 3; k++)
					{
						if (cached)
						{
							v[k] = &scratch[idx[k]];
						}
						else
						{
							ShadeVertex(ds, idx[k], local[k]);
							v[k] = &local[k];
						}
					}

					const u32 any = v[0]->code | v[1]->code | v[2]->code;
					if ((any & kBad) != 0 || (v[0]->code & v[1]->code & v[2]->code & kFrustumMask) != 0)
					{
						stats.rejected++;
						continue;
					}

					const float tri_det = v[0]->x * (v[1]->y * v[2]->w - v[2]->y * v[1]->w) -
					                      v[0]->y * (v[1]->x * v[2]->w - v[2]->x * v[1]->w) +
					                      v[0]->w * (v[1]->x * v[2]->y - v[2]->x * v[1]->y);
					if (!(std::fabs(tri_det) > 0.0f))
					{
						stats.rejected++;
						continue;
					}
					const bool facing_away = (tri_det * front_sign) < 0.0f;
					if (facing_away && material.cull_back)
					{
						stats.culled++;
						continue;
					}

					PolyVert work_a[kMaxPoly];
					PolyVert work_b[kMaxPoly];
					PolyVert* poly = work_a;
					for (int k = 0; k < 3; k++)
					{
						poly[k].x = v[k]->x;
						poly[k].y = v[k]->y;
						poly[k].z = v[k]->z;
						poly[k].w = v[k]->w;
						poly[k].u = v[k]->u;
						poly[k].v = v[k]->v;
						poly[k].lit = facing_away ? v[k]->lit_b : v[k]->lit;
						poly[k].rim = facing_away ? v[k]->rim_b : v[k]->rim;
					}
					int count = 3;
					if ((any & kClipMask) != 0)
					{
						const u32 planes = (any & (kNear | kFar)) | (((any >> 6) & 0xFu) << 2);
						PolyVert* other = work_b;
						for (int plane = 0; plane < 6 && count >= 3; plane++)
						{
							if ((planes & (1u << plane)) == 0)
								continue;
							count = ClipAgainst(poly, count, plane, other);
							std::swap(poly, other);
						}
						if (count < 3)
						{
							stats.rejected++;
							continue;
						}
						stats.clipped++;
					}

					ScreenVert sv[kMaxPoly];
					bool usable = true;
					for (int k = 0; k < count; k++)
					{
						const PolyVert& p = poly[k];
						if (!(p.w > 1.0e-12f))
						{
							usable = false;
							break;
						}
						const float iw = 1.0f / p.w;
						ScreenVert& s = sv[k];
						s.x = FloorToInt(((p.x * iw * 0.5f + 0.5f) * fsw) * static_cast<float>(kSub) + 0.5f);
						s.y = FloorToInt(((0.5f - p.y * iw * 0.5f) * fsh) * static_cast<float>(kSub) + 0.5f);
						s.z = p.z * iw;
						s.iw = iw;
						s.uw = p.u * iw;
						s.vw = p.v * iw;
						s.lit = p.lit;
						s.rim = p.rim;
					}
					if (!usable)
					{
						stats.rejected++;
						continue;
					}
					// A convex polygon fans from its first corner; the fan's inner edges are shared by exact arithmetic.
					for (int k = 1; k + 1 < count; k++)
						fill(fc, sv[0], sv[k], sv[k + 1]);
				}

				if (fc.bounds.x1 >= fc.bounds.x0 && fc.bounds.y1 >= fc.bounds.y0)
				{
					target.m_dirty.Add(static_cast<u32>(fc.bounds.x0), static_cast<u32>(fc.bounds.y0),
						static_cast<u32>(fc.bounds.x1) + 1u, static_cast<u32>(fc.bounds.y1) + 1u);
				}
				return true;
			}
		};
	}

	bool Draw(Target& target, const Camera& camera, const Mesh& mesh, const Mat4& model, const Material& material,
		DrawStats* stats)
	{
		DrawStats local;
		return detail::Rasteriser::Run(target, camera, mesh, model, material, stats ? *stats : local);
	}

	// --- Self-test -----------------------------------------------------------------------------------------------------

	namespace
	{
		struct Checker
		{
			int checks = 0;
			int failed = 0;

			void operator()(bool ok, const char* name)
			{
				checks++;
				if (!ok)
				{
					failed++;
					Console.Error("(VR) MeshRaster self-test FAIL: %s", name);
				}
			}
		};

		// A mesh the tests build vertex by vertex; every vertex has a normal and a UV.
		struct Geometry
		{
			std::vector<float> positions;
			std::vector<float> normals;
			std::vector<float> uvs;
			std::vector<u32> indices;

			u32 Add(float x, float y, float z, float u = 0.0f, float v = 0.0f, float nx = 0.0f, float ny = 0.0f,
				float nz = 1.0f)
			{
				positions.insert(positions.end(), {x, y, z});
				normals.insert(normals.end(), {nx, ny, nz});
				uvs.insert(uvs.end(), {u, v});
				return static_cast<u32>(positions.size() / 3 - 1);
			}

			void Tri(u32 a, u32 b, u32 c)
			{
				indices.push_back(a);
				indices.push_back(b);
				indices.push_back(c);
			}

			Mesh View() const
			{
				Mesh m;
				m.positions = positions.data();
				m.normals = normals.data();
				m.uvs = uvs.data();
				m.vertex_count = static_cast<u32>(positions.size() / 3);
				m.indices32 = indices.empty() ? nullptr : indices.data();
				m.index_count = static_cast<u32>(indices.size());
				return m;
			}
		};

		// Plain white, unlit, double-sided: the colour of a covered sample is exactly what the test sets.
		Material PlainMaterial()
		{
			Material m;
			m.lighting.enabled = false;
			m.cull_back = false;
			return m;
		}

		// A camera whose eye space is the target's pixels: x to the right, y up from the bottom edge, so a point at pixel
		// (px, py) is (px, height - py) at a depth in front of the camera.
		Camera PixelCamera(u32 width, u32 height)
		{
			return Camera::Orthographic(Mat4(), 0.0f, static_cast<float>(width), 0.0f, static_cast<float>(height), 0.1f, 100.0f);
		}

		u32 AddPx(Geometry& g, u32 height, float px, float py, float depth, float u = 0.0f, float v = 0.0f,
			float nx = 0.0f, float ny = 0.0f, float nz = 1.0f)
		{
			return g.Add(px, static_cast<float>(height) - py, -depth, u, v, nx, ny, nz);
		}

		// A rectangle [x0, x1) x [y0, y1) in pixels as two triangles, its UVs 0 to u_max across and 0 to 1 down.
		void AddPxQuad(Geometry& g, u32 height, float x0, float y0, float x1, float y1, float depth, float u_max = 1.0f,
			float nx = 0.0f, float ny = 0.0f, float nz = 1.0f)
		{
			const u32 tl = AddPx(g, height, x0, y0, depth, 0.0f, 0.0f, nx, ny, nz);
			const u32 bl = AddPx(g, height, x0, y1, depth, 0.0f, 1.0f, nx, ny, nz);
			const u32 br = AddPx(g, height, x1, y1, depth, u_max, 1.0f, nx, ny, nz);
			const u32 tr = AddPx(g, height, x1, y0, depth, u_max, 0.0f, nx, ny, nz);
			g.Tri(tl, bl, br);
			g.Tri(tl, br, tr);
		}

		// The samples of a target whose centres lie in [x0, x1) x [y0, y1) (pixels): what the top-left rule gives a
		// rectangle with its edges on those lines.
		u32 SamplesInRect(const Target& t, float x0, float y0, float x1, float y1)
		{
			const float grid = static_cast<float>(t.SamplesPerAxis());
			u32 n = 0;
			for (u32 sy = 0; sy < t.SampleHeight(); sy++)
			{
				for (u32 sx = 0; sx < t.SampleWidth(); sx++)
				{
					const float cx = (static_cast<float>(sx) + 0.5f) / grid;
					const float cy = (static_cast<float>(sy) + 0.5f) / grid;
					if (cx >= x0 && cx < x1 && cy >= y0 && cy < y1)
						n++;
				}
			}
			return n;
		}

		// Whether the samples drawn into (depth below 1) are exactly those in the rectangle.
		bool CoverageIsRect(const Target& t, float x0, float y0, float x1, float y1)
		{
			const float grid = static_cast<float>(t.SamplesPerAxis());
			for (u32 sy = 0; sy < t.SampleHeight(); sy++)
			{
				for (u32 sx = 0; sx < t.SampleWidth(); sx++)
				{
					const float cx = (static_cast<float>(sx) + 0.5f) / grid;
					const float cy = (static_cast<float>(sy) + 0.5f) / grid;
					const bool want = cx >= x0 && cx < x1 && cy >= y0 && cy < y1;
					const bool drawn = t.Depth()[static_cast<size_t>(sy) * t.SampleWidth() + sx] < 1.0f;
					if (want != drawn)
						return false;
				}
			}
			return true;
		}

		bool SamePixels(const Target& a, const Target& b)
		{
			return a.Width() == b.Width() && a.Height() == b.Height() &&
			       std::memcmp(a.Pixels(), b.Pixels(), sizeof(u32) * a.Width() * a.Height()) == 0;
		}

		void QuatMulXyzw(const float a[4], const float b[4], float out[4])
		{
			out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
			out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
			out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
			out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
		}

		void QuatAxis(float ax, float ay, float az, float rad, float out[4])
		{
			const float s = std::sin(0.5f * rad);
			out[0] = ax * s;
			out[1] = ay * s;
			out[2] = az * s;
			out[3] = std::cos(0.5f * rad);
		}

		void TransformPoint(const Mat4& m, const float p[3], float out[3])
		{
			for (int row = 0; row < 3; row++)
				out[row] = m.m[row] * p[0] + m.m[4 + row] * p[1] + m.m[8 + row] * p[2] + m.m[12 + row];
		}

		// The quad of the test scenes in model space: x and y from -1 to 1 on z = 0, UVs with v counting down from the top.
		void AddUnitQuad(Geometry& g)
		{
			const u32 bl = g.Add(-1.0f, -1.0f, 0.0f, 0.0f, 1.0f);
			const u32 br = g.Add(1.0f, -1.0f, 0.0f, 1.0f, 1.0f);
			const u32 tr = g.Add(1.0f, 1.0f, 0.0f, 1.0f, 0.0f);
			const u32 tl = g.Add(-1.0f, 1.0f, 0.0f, 0.0f, 0.0f);
			g.Tri(bl, br, tr);
			g.Tri(bl, tr, tl);
		}

		u32 ChannelR(u32 p) { return p & 0xFFu; }
		u32 ChannelG(u32 p) { return (p >> 8) & 0xFFu; }
		u32 ChannelB(u32 p) { return (p >> 16) & 0xFFu; }
		u32 ChannelA(u32 p) { return p >> 24; }
		bool Near(u32 a, u32 b, u32 tolerance) { return (a > b ? a - b : b - a) <= tolerance; }

		// Every sample of a filled rectangle, the quad's two triangles meeting in a diagonal that passes through sample
		// centres, a rectangle off the sample lines, and a lattice of shared corners on the half-sample grid (many exactly
		// on sample centres) that has to cover its outline once.
		void TestFillRule(Checker& check)
		{
			const u32 w = 64;
			const u32 h = 64;
			const Camera cam = PixelCamera(w, h);
			const Material mat = PlainMaterial();
			const u32 grids[3] = {1, 2, 4};
			for (const u32 grid : grids)
			{
				Target t(w, h, grid);
				DrawStats st;

				Geometry off;
				AddPxQuad(off, h, 10.3f, 20.6f, 40.7f, 60.2f, 5.0f);
				t.Clear();
				Draw(t, cam, off.View(), Mat4(), mat, &st);
				const u32 want_off = SamplesInRect(t, 10.3f, 20.6f, 40.7f, 60.2f);
				check(want_off > 0 && st.covered == want_off && st.written == want_off && CoverageIsRect(t, 10.3f, 20.6f, 40.7f, 60.2f),
					"a quad off the sample lines covers exactly its samples, once each (any grid)");

				Geometry on;
				AddPxQuad(on, h, 10.5f, 20.5f, 40.5f, 60.5f, 5.0f);
				t.Clear();
				Draw(t, cam, on.View(), Mat4(), mat, &st);
				const u32 want_on = SamplesInRect(t, 10.5f, 20.5f, 40.5f, 60.5f);
				check(want_on > 0 && st.covered == want_on && CoverageIsRect(t, 10.5f, 20.5f, 40.5f, 60.5f),
					"a quad with its edges through sample centres takes its left and top edges and not its right and bottom (top-left rule)");
				if (grid == 1)
					check(st.covered == 30u * 40u, "the quad from (10.5, 20.5) to (40.5, 60.5) is 30 x 40 pixels");

				// A 9 x 9 lattice, 5 pixels apart, its inner corners moved by up to 1.5 pixels in half-pixel steps, each cell
				// cut along alternating diagonals.
				Geometry lattice;
				u32 seed = 12345u;
				const auto jitter = [&seed]() {
					seed = seed * 1664525u + 1013904223u;
					return static_cast<float>(static_cast<int>((seed >> 16) % 7u) - 3) * 0.5f;
				};
				u32 id[9][9];
				for (int j = 0; j < 9; j++)
				{
					for (int i = 0; i < 9; i++)
					{
						const bool edge = (i == 0 || j == 0 || i == 8 || j == 8);
						const float jx = edge ? 0.0f : jitter();
						const float jy = edge ? 0.0f : jitter();
						id[j][i] = AddPx(lattice, h, 12.5f + 5.0f * static_cast<float>(i) + jx, 15.5f + 5.0f * static_cast<float>(j) + jy, 5.0f);
					}
				}
				for (int j = 0; j < 8; j++)
				{
					for (int i = 0; i < 8; i++)
					{
						if (((i + j) & 1) != 0)
						{
							lattice.Tri(id[j][i], id[j + 1][i], id[j + 1][i + 1]);
							lattice.Tri(id[j][i], id[j + 1][i + 1], id[j][i + 1]);
						}
						else
						{
							lattice.Tri(id[j][i], id[j + 1][i], id[j][i + 1]);
							lattice.Tri(id[j + 1][i], id[j + 1][i + 1], id[j][i + 1]);
						}
					}
				}
				t.Clear();
				Draw(t, cam, lattice.View(), Mat4(), mat, &st);
				const u32 want_lattice = SamplesInRect(t, 12.5f, 15.5f, 52.5f, 55.5f);
				check(st.triangles == 128u && st.covered == want_lattice && st.written == want_lattice &&
				      CoverageIsRect(t, 12.5f, 15.5f, 52.5f, 55.5f),
					"a lattice of 128 triangles sharing corners on sample centres covers its outline exactly once (no cracks, no double hits)");
			}
		}

		// A checkerboard quad in strong perspective shows, at each sampled pixel, the texel the ray through it reaches.
		void TestPerspectiveTexture(Checker& check)
		{
			const u32 w = 96;
			const u32 h = 96;
			u32 texels[64];
			const u32 red = PackRgba(220, 30, 30);
			const u32 blue = PackRgba(30, 40, 200);
			const u32 green = PackRgba(30, 220, 40);
			for (int y = 0; y < 8; y++)
			{
				for (int x = 0; x < 8; x++)
					texels[y * 8 + x] = (x == 0 && y == 0) ? green : (((x + y) & 1) != 0 ? red : blue);
			}
			Texture tex;
			tex.texels = texels;
			tex.width = 8;
			tex.height = 8;
			tex.filter = Texture::Filter::Nearest;
			tex.wrap_u = Texture::Wrap::Clamp;
			tex.wrap_v = Texture::Wrap::Clamp;
			Material mat = PlainMaterial();
			mat.lighting.enabled = false;
			mat.cull_back = true;
			mat.texture = &tex;

			Geometry g;
			AddUnitQuad(g);
			// The same quad through 16-bit indices.
			std::vector<u16> idx16;
			for (const u32 i : g.indices)
				idx16.push_back(static_cast<u16>(i));
			Mesh mesh = g.View();
			mesh.indices32 = nullptr;
			mesh.indices16 = idx16.data();

			float qy[4];
			float qx[4];
			float rot[4];
			QuatAxis(0.0f, 1.0f, 0.0f, 55.0f * kPi / 180.0f, qy);
			QuatAxis(1.0f, 0.0f, 0.0f, -20.0f * kPi / 180.0f, qx);
			QuatMulXyzw(qy, qx, rot);
			const float pos[3] = {0.2f, 0.1f, -3.2f};
			const Mat4 model = ModelMatrix(pos, rot);
			const float fov = 60.0f * kPi / 180.0f;
			const Camera cam = Camera::Perspective(Mat4(), fov, 1.0f, 0.1f, 50.0f);

			Target t(w, h, 1);
			DrawStats st;
			t.Clear();
			Draw(t, cam, mesh, model, mat, &st);
			check(st.culled == 0 && st.written > 1000u, "the tilted quad faces the camera, so back-face culling keeps it");

			// The ray through each pixel against the quad's plane, back in the quad's own coordinates.
			const float zero[3] = {0.0f, 0.0f, 0.0f};
			const float zaxis[3] = {0.0f, 0.0f, 1.0f};
			float origin[3];
			float normal[3];
			TransformPoint(model, zero, origin);
			float tip[3];
			TransformPoint(model, zaxis, tip);
			for (int i = 0; i < 3; i++)
				normal[i] = tip[i] - origin[i];
			const float tan_half = std::tan(0.5f * fov);
			int tested = 0;
			int wrong = 0;
			for (u32 py = 0; py < h; py++)
			{
				for (u32 px = 0; px < w; px++)
				{
					const float ndc_x = ((static_cast<float>(px) + 0.5f) / static_cast<float>(w)) * 2.0f - 1.0f;
					const float ndc_y = 1.0f - ((static_cast<float>(py) + 0.5f) / static_cast<float>(h)) * 2.0f;
					const float dir[3] = {ndc_x * tan_half, ndc_y * tan_half, -1.0f};
					const float denom = Dot3(dir, normal);
					if (std::fabs(denom) < 1.0e-4f)
						continue;
					const float s = Dot3(origin, normal) / denom;
					if (!(s > 0.0f))
						continue;
					const float hit[3] = {dir[0] * s - origin[0], dir[1] * s - origin[1], dir[2] * s - origin[2]};
					// Back into the model: undo the rotation (its columns are the model's axes, scaled by 1).
					const float lx = hit[0] * model.m[0] + hit[1] * model.m[1] + hit[2] * model.m[2];
					const float ly = hit[0] * model.m[4] + hit[1] * model.m[5] + hit[2] * model.m[6];
					if (std::fabs(lx) > 0.95f || std::fabs(ly) > 0.95f)
						continue;
					const float tu = (lx + 1.0f) * 0.5f * 8.0f;
					const float tv = (1.0f - ly) * 0.5f * 8.0f;
					const float fu = tu - std::floor(tu);
					const float fv = tv - std::floor(tv);
					if (fu < 0.15f || fu > 0.85f || fv < 0.15f || fv > 0.85f)
						continue;
					const int cx = static_cast<int>(tu);
					const int cy = static_cast<int>(tv);
					tested++;
					if (t.Pixel(px, py) != texels[cy * 8 + cx])
						wrong++;
				}
			}
			check(tested > 500, "the perspective texture test samples enough pixels");
			check(wrong == 0, "a checkerboard quad in strong perspective shows the texel the ray through each pixel reaches (perspective-correct UVs, v down from the top)");
		}

		void TestFiltering(Checker& check)
		{
			const u32 w = 48;
			const u32 h = 24;
			const Camera cam = PixelCamera(w, h);
			Target t(w, h, 1);

			// Bilinear between two texels, and nearest.
			const u32 ramp[2] = {PackRgba(0, 0, 0), PackRgba(255, 255, 255)};
			Texture tex;
			tex.texels = ramp;
			tex.width = 2;
			tex.height = 1;
			tex.wrap_u = Texture::Wrap::Clamp;
			tex.wrap_v = Texture::Wrap::Clamp;
			Material mat = PlainMaterial();
			mat.texture = &tex;
			Geometry g;
			AddPxQuad(g, h, 4.0f, 4.0f, 44.0f, 20.0f, 5.0f);
			tex.filter = Texture::Filter::Bilinear;
			t.Clear();
			Draw(t, cam, g.View(), Mat4(), mat);
			// Pixel x's centre is at u = (x + 0.5 - 4) / 40; texel centres are at u = 0.25 and 0.75.
			check(ChannelR(t.Pixel(13, 10)) <= 1u, "bilinear: before the first texel centre the texture clamps to it");
			check(Near(ChannelR(t.Pixel(23, 10)), 121u, 3u) && Near(ChannelR(t.Pixel(24, 10)), 134u, 3u),
				"bilinear: halfway between two texel centres the colour is halfway");
			check(ChannelR(t.Pixel(34, 10)) >= 254u, "bilinear: past the last texel centre the texture clamps to it");
			tex.filter = Texture::Filter::Nearest;
			t.Clear();
			Draw(t, cam, g.View(), Mat4(), mat);
			check(ChannelR(t.Pixel(23, 10)) == 0u && ChannelR(t.Pixel(24, 10)) == 255u, "nearest: the colour changes at the texel edge");

			// Repeat and clamp, u running 0 to 2 across the quad.
			const u32 pair[2] = {PackRgba(255, 0, 0), PackRgba(0, 0, 255)};
			tex.texels = pair;
			tex.filter = Texture::Filter::Nearest;
			Geometry two;
			AddPxQuad(two, h, 4.0f, 4.0f, 44.0f, 20.0f, 5.0f, 2.0f);
			tex.wrap_u = Texture::Wrap::Repeat;
			t.Clear();
			Draw(t, cam, two.View(), Mat4(), mat);
			check(t.Pixel(9, 10) == pair[0] && t.Pixel(19, 10) == pair[1] && t.Pixel(29, 10) == pair[0] && t.Pixel(39, 10) == pair[1],
				"repeat: the texture runs again past u = 1");
			tex.wrap_u = Texture::Wrap::Clamp;
			t.Clear();
			Draw(t, cam, two.View(), Mat4(), mat);
			check(t.Pixel(9, 10) == pair[0] && t.Pixel(19, 10) == pair[1] && t.Pixel(29, 10) == pair[1] && t.Pixel(39, 10) == pair[1],
				"clamp: the last texel runs on past u = 1");

			// Bilinear with repeat blends the last texel into the first across the seam.
			tex.texels = ramp;
			tex.filter = Texture::Filter::Bilinear;
			tex.wrap_u = Texture::Wrap::Repeat;
			t.Clear();
			Draw(t, cam, g.View(), Mat4(), mat);
			check(Near(ChannelR(t.Pixel(4, 10)), 121u, 3u), "bilinear, repeat: at u = 0 the last texel blends into the first");

			// Textures whose sides are not powers of two (the general path): three texels across.
			const u32 trio[3] = {PackRgba(255, 0, 0), PackRgba(0, 255, 0), PackRgba(0, 0, 255)};
			tex.texels = trio;
			tex.width = 3;
			tex.height = 1;
			tex.filter = Texture::Filter::Nearest;
			tex.wrap_u = Texture::Wrap::Repeat;
			Geometry thirds;
			AddPxQuad(thirds, h, 2.0f, 4.0f, 41.0f, 20.0f, 5.0f, 2.0f);
			t.Clear();
			Draw(t, cam, thirds.View(), Mat4(), mat);
			check(t.Pixel(5, 10) == trio[0] && t.Pixel(22, 10) == trio[0] && t.Pixel(30, 10) == trio[1] && t.Pixel(38, 10) == trio[2],
				"a texture three texels wide repeats (sides that are not powers of two)");
			tex.wrap_u = Texture::Wrap::Clamp;
			t.Clear();
			Draw(t, cam, thirds.View(), Mat4(), mat);
			check(t.Pixel(5, 10) == trio[0] && t.Pixel(22, 10) == trio[2] && t.Pixel(38, 10) == trio[2], "and clamps");
			const u32 peak[3] = {PackRgba(0, 0, 0), PackRgba(255, 255, 255), PackRgba(0, 0, 0)};
			tex.texels = peak;
			tex.filter = Texture::Filter::Bilinear;
			Geometry once;
			AddPxQuad(once, h, 2.0f, 4.0f, 41.0f, 20.0f, 5.0f);
			t.Clear();
			Draw(t, cam, once.View(), Mat4(), mat);
			check(ChannelR(t.Pixel(21, 10)) >= 250u && ChannelR(t.Pixel(8, 10)) <= 6u && Near(ChannelR(t.Pixel(14, 10)), 118u, 6u),
				"bilinear on a texture three texels wide: the middle texel's centre is solid and the ends fall away");
		}

		// Depth: two overlapping triangles in either order, and two that cross each other.
		void TestDepth(Checker& check)
		{
			const u32 w = 32;
			const u32 h = 32;
			const Camera cam = PixelCamera(w, h);
			Target t(w, h, 1);
			Material far_mat = PlainMaterial();
			far_mat.base_colour[0] = 0.0f;
			far_mat.base_colour[2] = 0.0f; // green
			Material near_mat = PlainMaterial();
			near_mat.base_colour[1] = 0.0f;
			near_mat.base_colour[2] = 0.0f; // red
			Geometry far_tri;
			far_tri.Tri(AddPx(far_tri, h, 2.0f, 2.0f, 8.0f), AddPx(far_tri, h, 2.0f, 26.0f, 8.0f), AddPx(far_tri, h, 26.0f, 2.0f, 8.0f));
			Geometry near_tri;
			near_tri.Tri(AddPx(near_tri, h, 6.0f, 6.0f, 3.0f), AddPx(near_tri, h, 6.0f, 30.0f, 3.0f), AddPx(near_tri, h, 30.0f, 6.0f, 3.0f));
			const u32 green = PackRgba(0, 255, 0);
			const u32 red = PackRgba(255, 0, 0);
			for (int order = 0; order < 2; order++)
			{
				t.Clear();
				if (order == 0)
				{
					Draw(t, cam, far_tri.View(), Mat4(), far_mat);
					Draw(t, cam, near_tri.View(), Mat4(), near_mat);
				}
				else
				{
					Draw(t, cam, near_tri.View(), Mat4(), near_mat);
					Draw(t, cam, far_tri.View(), Mat4(), far_mat);
				}
				check(t.Pixel(8, 8) == red && t.Pixel(3, 3) == green && t.Pixel(28, 6) == red && t.Pixel(20, 4) == green,
					order == 0 ? "depth: the nearer triangle wins where they overlap (far drawn first)"
							   : "depth: the nearer triangle wins where they overlap (far drawn last)");
			}

			// A flat quad at depth 5 and a slanted one from depth 3 at the left edge to 7 at the right: they cross at
			// x = 16, and the depth buffer holds the plane's own value.
			Geometry flat;
			AddPxQuad(flat, h, 0.0f, 0.0f, 32.0f, 32.0f, 5.0f);
			Geometry slant;
			{
				const u32 tl = AddPx(slant, h, 0.0f, 0.0f, 3.0f);
				const u32 bl = AddPx(slant, h, 0.0f, 32.0f, 3.0f);
				const u32 br = AddPx(slant, h, 32.0f, 32.0f, 7.0f);
				const u32 tr = AddPx(slant, h, 32.0f, 0.0f, 7.0f);
				slant.Tri(tl, bl, br);
				slant.Tri(tl, br, tr);
			}
			for (int order = 0; order < 2; order++)
			{
				t.Clear();
				if (order == 0)
				{
					Draw(t, cam, flat.View(), Mat4(), near_mat);
					Draw(t, cam, slant.View(), Mat4(), far_mat);
				}
				else
				{
					Draw(t, cam, slant.View(), Mat4(), far_mat);
					Draw(t, cam, flat.View(), Mat4(), near_mat);
				}
				check(t.Pixel(4, 10) == green && t.Pixel(15, 10) == green && t.Pixel(16, 10) == red && t.Pixel(28, 10) == red,
					"depth: two crossing planes meet on the line where their depths are equal, in either order");
			}
			// Ortho depth is linear: z = (distance - near) / (far - near).
			const float want = (3.0f + 4.0f * (4.5f / 32.0f) - 0.1f) / 99.9f;
			check(std::fabs(t.Depth()[10 * w + 4] - want) < 1.0e-4f, "depth: the buffer holds the interpolated plane's depth");
		}

		// A floor triangle with its apex behind the camera is cut by the near plane and covers the floor it should.
		void TestNearClip(Checker& check)
		{
			const u32 w = 64;
			const u32 h = 64;
			const Camera cam = Camera::OffAxis(Mat4(), -1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 100.0f);
			Material mat = PlainMaterial();
			Geometry g;
			g.Tri(g.Add(-5.0f, -1.0f, -10.0f), g.Add(5.0f, -1.0f, -10.0f), g.Add(0.0f, -1.0f, 5.0f));
			Target t(w, h, 1);
			DrawStats st;
			t.Clear();
			Draw(t, cam, g.View(), Mat4(), mat, &st);
			check(st.clipped == 1 && st.rejected == 0 && st.written > 100u, "a triangle across the camera's plane is clipped, not dropped");

			// The triangle in the floor plane's own (x, z) coordinates: A (-5, -10), B (5, -10), C (0, 5).
			const float ax = -5.0f, az = -10.0f, bx = 5.0f, bz = -10.0f, cx = 0.0f, cz = 5.0f;
			const float area = (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
			int tested = 0;
			int wrong = 0;
			for (u32 py = 0; py < h; py++)
			{
				for (u32 px = 0; px < w; px++)
				{
					const float ndc_x = ((static_cast<float>(px) + 0.5f) / static_cast<float>(w)) * 2.0f - 1.0f;
					const float ndc_y = 1.0f - ((static_cast<float>(py) + 0.5f) / static_cast<float>(h)) * 2.0f;
					const bool drawn = t.Pixel(px, py) != 0u;
					if (ndc_y > -0.02f)
					{
						// At and above the horizon the eye sees no floor at all.
						tested++;
						if (drawn)
							wrong++;
						continue;
					}
					const float dist = 1.0f / -ndc_y; // distance along -z at which the ray reaches y = -1
					const float fx = dist * ndc_x;
					const float fz = -dist;
					const float l0 = ((bx - fx) * (cz - fz) - (bz - fz) * (cx - fx)) / area;
					const float l1 = ((cx - fx) * (az - fz) - (cz - fz) * (ax - fx)) / area;
					const float l2 = 1.0f - l0 - l1;
					const float lo = std::min({l0, l1, l2});
					if (lo > 0.03f)
					{
						tested++;
						if (!drawn)
							wrong++;
					}
					else if (lo < -0.03f)
					{
						tested++;
						if (drawn)
							wrong++;
					}
				}
			}
			check(tested > 1500 && wrong == 0, "the clipped floor triangle covers exactly the floor the eye sees");
			bool depth_ok = true;
			for (size_t i = 0; i < static_cast<size_t>(w) * h; i++)
				depth_ok = depth_ok && t.Depth()[i] >= 0.0f && t.Depth()[i] <= 1.0f;
			check(depth_ok, "the clipped triangle's depths stay between the near and far planes");
		}

		void TestCulling(Checker& check)
		{
			const u32 w = 32;
			const u32 h = 32;
			const Camera cam = Camera::Perspective(Mat4(), 60.0f * kPi / 180.0f, 1.0f, 0.1f, 50.0f);
			Material mat = PlainMaterial();
			mat.cull_back = true;
			Geometry ccw; // apex, bottom left, bottom right: counter-clockwise from the camera
			ccw.Tri(ccw.Add(0.0f, 0.5f, -2.0f), ccw.Add(-0.5f, -0.5f, -2.0f), ccw.Add(0.5f, -0.5f, -2.0f));
			Geometry cw;
			cw.Tri(cw.Add(0.0f, 0.5f, -2.0f), cw.Add(0.5f, -0.5f, -2.0f), cw.Add(-0.5f, -0.5f, -2.0f));
			Target t(w, h, 1);
			DrawStats st;

			t.Clear();
			Draw(t, cam, ccw.View(), Mat4(), mat, &st);
			check(st.culled == 0 && st.written > 20u && t.Pixel(16, 16) != 0u, "culling: a counter-clockwise triangle is the front and stays");
			t.Clear();
			Draw(t, cam, cw.View(), Mat4(), mat, &st);
			check(st.culled == 1 && st.written == 0 && t.Pixel(16, 16) == 0u, "culling: a clockwise triangle is the back and goes");
			mat.cull_back = false;
			t.Clear();
			Draw(t, cam, cw.View(), Mat4(), mat, &st);
			check(st.culled == 0 && st.written > 20u && t.Pixel(16, 16) != 0u, "culling off: the back is drawn too");

			// A mesh that says clockwise is the front.
			mat.cull_back = true;
			Mesh flipped = cw.View();
			flipped.front_ccw = false;
			t.Clear();
			Draw(t, cam, flipped, Mat4(), mat, &st);
			check(st.culled == 0 && st.written > 20u, "culling: front_ccw = false makes clockwise the front");

			// A mirrored model (a left hand from the right hand's mesh) keeps the mesh's own front faces in front.
			const float mirror_scale[3] = {-1.0f, 1.0f, 1.0f};
			const float no_pos[3] = {0.0f, 0.0f, 0.0f};
			const float no_rot[4] = {0.0f, 0.0f, 0.0f, 1.0f};
			const Mat4 mirrored = ModelMatrix(no_pos, no_rot, mirror_scale);
			t.Clear();
			Draw(t, cam, ccw.View(), mirrored, mat, &st);
			check(st.culled == 0 && st.written > 20u, "culling: a mirroring model matrix keeps the mesh's front faces");
			t.Clear();
			Draw(t, cam, cw.View(), mirrored, mat, &st);
			check(st.culled == 1 && st.written == 0, "culling: and its back faces stay back faces");

			const float flat_scale[3] = {1.0f, 0.0f, 1.0f};
			check(!Draw(t, cam, ccw.View(), ModelMatrix(no_pos, no_rot, flat_scale), mat), "a model matrix that flattens the mesh draws nothing");
		}

		void TestAlpha(Checker& check)
		{
			const u32 w = 32;
			const u32 h = 32;
			const Camera cam = PixelCamera(w, h);
			const u32 red = PackRgba(255, 0, 0);
			const u32 blue = PackRgba(0, 0, 255);
			const u32 hole = PackRgba(0, 255, 0, 0);
			const u32 texels[4] = {red, hole, hole, blue};
			Texture tex;
			tex.texels = texels;
			tex.width = 2;
			tex.height = 2;
			tex.wrap_u = Texture::Wrap::Clamp;
			tex.wrap_v = Texture::Wrap::Clamp;
			Material mat = PlainMaterial();
			mat.texture = &tex;
			mat.alpha_cutoff = 0.5f;
			Geometry g;
			AddPxQuad(g, h, 4.0f, 4.0f, 28.0f, 28.0f, 5.0f);
			Target t(w, h, 1);
			DrawStats st;
			t.Clear();
			Draw(t, cam, g.View(), Mat4(), mat, &st);
			check(t.Pixel(8, 8) == red && t.Pixel(24, 24) == blue && t.Pixel(24, 8) == 0u && t.Pixel(8, 24) == 0u,
				"alpha test: transparent texels are not drawn and the opaque ones are");
			check(st.covered > st.written, "alpha test: the transparent samples are counted as covered, not written");

			// Nothing is written to the depth buffer where a texel is cut out, so what is behind shows through.
			Material behind = PlainMaterial();
			behind.base_colour[0] = 0.0f;
			behind.base_colour[1] = 0.0f; // blue
			Geometry back_quad;
			AddPxQuad(back_quad, h, 0.0f, 0.0f, 32.0f, 32.0f, 9.0f);
			Draw(t, cam, back_quad.View(), Mat4(), behind);
			check(t.Pixel(24, 8) == blue && t.Pixel(8, 8) == red, "alpha test: a cut-out lets the farther surface show and leaves its depth alone");

			mat.alpha_cutoff = 0.0f;
			t.Clear();
			Draw(t, cam, g.View(), Mat4(), mat);
			check(t.Pixel(24, 8) == PackRgba(0, 255, 0), "without the alpha test every texel is drawn, opaque");
		}

		void TestLighting(Checker& check)
		{
			const u32 w = 32;
			const u32 h = 32;
			const Camera cam = Camera::Perspective(Mat4(), 60.0f * kPi / 180.0f, 1.0f, 0.1f, 50.0f);
			Target t(w, h, 1);
			Material mat;
			mat.cull_back = true;
			mat.lighting.linear_light = false;
			mat.lighting.ambient = 0.25f;
			mat.lighting.key = 0.5f;
			mat.lighting.light_dir[0] = 0.0f;
			mat.lighting.light_dir[1] = 0.0f;
			mat.lighting.light_dir[2] = 1.0f;
			Geometry g; // facing the camera, normal towards it
			g.Tri(g.Add(-1.0f, -1.0f, -3.0f), g.Add(1.0f, -1.0f, -3.0f), g.Add(0.0f, 1.0f, -3.0f));

			t.Clear();
			Draw(t, cam, g.View(), Mat4(), mat);
			check(Near(ChannelR(t.Pixel(16, 16)), 191u, 1u) && ChannelR(t.Pixel(16, 16)) == ChannelB(t.Pixel(16, 16)),
				"lighting: facing the light, ambient + key = 0.75 of the colour");
			mat.lighting.light_dir[2] = -1.0f;
			t.Clear();
			Draw(t, cam, g.View(), Mat4(), mat);
			check(Near(ChannelR(t.Pixel(16, 16)), 63u, 1u), "lighting: facing away from the light, ambient only");
			mat.lighting.light_dir[2] = 1.0f;
			mat.base_colour[1] = 0.5f;
			mat.base_colour[2] = 0.0f;
			t.Clear();
			Draw(t, cam, g.View(), Mat4(), mat);
			check(Near(ChannelG(t.Pixel(16, 16)), 95u, 2u) && ChannelB(t.Pixel(16, 16)) == 0u && Near(ChannelR(t.Pixel(16, 16)), 191u, 1u),
				"lighting: the base colour tints each channel");
			mat.base_colour[1] = 1.0f;
			mat.base_colour[2] = 1.0f;
			mat.lighting.linear_light = true;
			t.Clear();
			Draw(t, cam, g.View(), Mat4(), mat);
			check(Near(ChannelR(t.Pixel(16, 16)), 221u, 2u), "lighting: in linear light the 0.75 factor is its square root on the bytes");
			mat.lighting.linear_light = false;

			// A back face drawn with culling off is lit from its own side.
			Geometry back;
			back.Tri(back.Add(-1.0f, -1.0f, -3.0f), back.Add(0.0f, 1.0f, -3.0f), back.Add(1.0f, -1.0f, -3.0f));
			mat.cull_back = false;
			t.Clear();
			Draw(t, cam, back.View(), Mat4(), mat);
			check(Near(ChannelR(t.Pixel(16, 16)), 63u, 1u), "lighting: a back face (culling off) has its normals turned, so it gets only the ambient light");
			mat.cull_back = true;

			// A light given in the camera's own frame follows the camera: looking down +z, the viewer's +z is world -z.
			const float origin[3] = {0.0f, 0.0f, 0.0f};
			const float ahead[3] = {0.0f, 0.0f, 3.0f};
			const float up[3] = {0.0f, 1.0f, 0.0f};
			const Camera turned = Camera::Perspective(LookAt(origin, ahead, up), 60.0f * kPi / 180.0f, 1.0f, 0.1f, 50.0f);
			Geometry behind; // the same triangle on the +z side, facing the camera at the origin
			behind.Tri(behind.Add(1.0f, -1.0f, 3.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f), behind.Add(-1.0f, -1.0f, 3.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f),
				behind.Add(0.0f, 1.0f, 3.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f));
			mat.lighting.light_in_view_space = true;
			t.Clear();
			Draw(t, turned, behind.View(), Mat4(), mat);
			check(Near(ChannelR(t.Pixel(16, 16)), 191u, 1u), "lighting: a light in view space (towards the viewer) lights what faces the viewer");
			mat.lighting.light_in_view_space = false;
			t.Clear();
			Draw(t, turned, behind.View(), Mat4(), mat);
			check(Near(ChannelR(t.Pixel(16, 16)), 63u, 1u), "lighting: the same light direction in world space points the other way");
			mat.lighting.light_in_view_space = false;

			// Rim and headlight, in an orthographic view whose view vector is the same everywhere.
			const Camera ortho = PixelCamera(w, h);
			Material rim_mat;
			rim_mat.cull_back = false;
			rim_mat.lighting.linear_light = false;
			rim_mat.lighting.ambient = 0.0f;
			rim_mat.lighting.key = 0.0f;
			rim_mat.lighting.rim = 1.0f;
			rim_mat.lighting.rim_power = 1;
			rim_mat.lighting.rim_colour[1] = 0.0f;
			rim_mat.lighting.rim_colour[2] = 0.0f;
			Geometry edge_on;
			AddPxQuad(edge_on, h, 4.0f, 4.0f, 28.0f, 28.0f, 5.0f, 1.0f, 1.0f, 0.0f, 0.0f);
			Geometry face_on;
			AddPxQuad(face_on, h, 4.0f, 4.0f, 28.0f, 28.0f, 5.0f, 1.0f, 0.0f, 0.0f, 1.0f);
			t.Clear();
			Draw(t, ortho, edge_on.View(), Mat4(), rim_mat);
			check(t.Pixel(16, 16) == PackRgba(255, 0, 0), "lighting: a surface edge-on to the viewer takes the full rim colour");
			t.Clear();
			Draw(t, ortho, face_on.View(), Mat4(), rim_mat);
			check(t.Pixel(16, 16) == PackRgba(0, 0, 0), "lighting: a surface facing the viewer takes none");
			rim_mat.lighting.rim = 0.0f;
			rim_mat.lighting.headlight = 0.5f;
			t.Clear();
			Draw(t, ortho, face_on.View(), Mat4(), rim_mat);
			check(Near(ChannelR(t.Pixel(16, 16)), 127u, 1u), "lighting: the headlight lights what faces the viewer");

			// A light along each axis lights the surface that faces it and not the one that faces away.
			Material axis_mat;
			axis_mat.cull_back = false;
			axis_mat.lighting.linear_light = false;
			axis_mat.lighting.ambient = 0.25f;
			axis_mat.lighting.key = 0.5f;
			bool axes_ok = true;
			for (int axis = 0; axis < 3; axis++)
			{
				float dir[3] = {0.0f, 0.0f, 0.0f};
				dir[axis] = 1.0f;
				axis_mat.lighting.light_dir[0] = dir[0];
				axis_mat.lighting.light_dir[1] = dir[1];
				axis_mat.lighting.light_dir[2] = dir[2];
				Geometry toward;
				AddPxQuad(toward, h, 4.0f, 4.0f, 28.0f, 28.0f, 5.0f, 1.0f, dir[0], dir[1], dir[2]);
				Geometry away;
				AddPxQuad(away, h, 4.0f, 4.0f, 28.0f, 28.0f, 5.0f, 1.0f, -dir[0], -dir[1], -dir[2]);
				t.Clear();
				Draw(t, ortho, toward.View(), Mat4(), axis_mat);
				axes_ok = axes_ok && Near(ChannelR(t.Pixel(16, 16)), 191u, 1u);
				t.Clear();
				Draw(t, ortho, away.View(), Mat4(), axis_mat);
				axes_ok = axes_ok && Near(ChannelR(t.Pixel(16, 16)), 63u, 1u);
			}
			check(axes_ok, "lighting: a light along x, y or z lights the surface facing it and leaves the one facing away to the ambient");

			// No normals: drawn unlit; lighting disabled: the same.
			Material plain;
			plain.cull_back = false;
			Mesh no_normals = g.View();
			no_normals.normals = nullptr;
			t.Clear();
			Draw(t, cam, no_normals, Mat4(), plain);
			check(t.Pixel(16, 16) == PackRgba(255, 255, 255), "a mesh without normals is drawn unlit");
		}

		// Smooth edges: a pixel half or a quarter covered resolves to that much alpha, and the unpremultiplied copy keeps
		// the colour.
		void TestSupersampling(Checker& check)
		{
			const u32 w = 16;
			const u32 h = 16;
			const Camera cam = PixelCamera(w, h);
			const Material mat = PlainMaterial();
			Geometry g;
			AddPxQuad(g, h, 4.0f, 4.0f, 9.5f, 12.0f, 5.0f);
			Target t2(w, h, 2);
			t2.Clear();
			Draw(t2, cam, g.View(), Mat4(), mat);
			t2.Resolve();
			const u32 half = t2.Pixel(9, 8);
			check(t2.Pixel(6, 8) == 0xFFFFFFFFu && t2.Pixel(11, 8) == 0u, "2 x 2: the inside is solid and the outside clear");
			check(Near(ChannelA(half), 128u, 1u) && Near(ChannelR(half), ChannelA(half), 1u) && Near(ChannelG(half), ChannelA(half), 1u),
				"2 x 2: a pixel with half its samples covered is half alpha, premultiplied");
			u32 straight[16 * 16];
			t2.CopyUnpremultiplied(straight);
			check(ChannelR(straight[8 * 16 + 9]) >= 254u && Near(ChannelA(straight[8 * 16 + 9]), 128u, 1u) && straight[8 * 16 + 6] == 0xFFFFFFFFu &&
			      straight[8 * 16 + 11] == 0u,
				"the unpremultiplied copy keeps the colour at an edge pixel");

			Geometry q;
			AddPxQuad(q, h, 4.0f, 4.0f, 9.25f, 12.0f, 5.0f);
			Target t4(w, h, 4);
			t4.Clear();
			Draw(t4, cam, q.View(), Mat4(), mat);
			t4.Resolve();
			check(Near(ChannelA(t4.Pixel(9, 8)), 64u, 1u), "4 x 4: a pixel with a quarter of its samples covered is a quarter alpha");

			// With one sample per pixel the same edge is all or nothing, and Resolve changes nothing.
			Target t1(w, h, 1);
			t1.Clear();
			Draw(t1, cam, g.View(), Mat4(), mat);
			t1.Resolve();
			check(t1.Pixel(8, 8) == 0xFFFFFFFFu && t1.Pixel(9, 8) == 0u,
				"1 x 1: a pixel whose centre is exactly on the right edge is not covered (top-left rule)");
		}

		// Clearing (full and partial), repeatability, and a mesh too big for the vertex scratch.
		void TestTargetState(Checker& check)
		{
			const u32 w = 64;
			const u32 h = 64;
			const Camera cam = PixelCamera(w, h);
			const Material mat = PlainMaterial();
			Geometry scene;
			AddPxQuad(scene, h, 10.3f, 12.6f, 30.7f, 40.2f, 5.0f);
			scene.Tri(AddPx(scene, h, 20.0f, 30.0f, 3.0f), AddPx(scene, h, 50.0f, 60.0f, 3.0f), AddPx(scene, h, 55.0f, 30.0f, 3.0f));
			Geometry big;
			AddPxQuad(big, h, 2.0f, 2.0f, 60.0f, 50.0f, 4.0f);

			Target fresh(w, h, 2);
			fresh.Clear();
			Draw(fresh, cam, scene.View(), Mat4(), mat);
			fresh.Resolve();

			Target used(w, h, 2);
			used.Clear();
			Draw(used, cam, big.View(), Mat4(), mat);
			used.Resolve();
			used.Clear();
			used.Resolve();
			bool empty = true;
			for (u32 i = 0; i < w * h; i++)
				empty = empty && used.Pixels()[i] == 0u;
			for (size_t i = 0; i < static_cast<size_t>(used.SampleWidth()) * used.SampleHeight(); i++)
				empty = empty && used.Depth()[i] == 1.0f;
			check(empty, "Clear erases what was drawn, pixels and depth (the part a draw reached)");
			Draw(used, cam, scene.View(), Mat4(), mat);
			used.Resolve();
			check(SamePixels(fresh, used), "a target cleared after another scene renders a scene as a fresh one does");

			used.Clear(PackRgba(16, 32, 48));
			used.Resolve();
			bool filled = true;
			for (u32 i = 0; i < w * h; i++)
				filled = filled && used.Pixels()[i] == PackRgba(16, 32, 48);
			check(filled, "Clear(colour) sets every pixel to the colour");
			used.Clear();
			Draw(used, cam, scene.View(), Mat4(), mat);
			used.Resolve();
			check(SamePixels(fresh, used), "clearing back to transparent after a coloured clear leaves nothing of it");

			Target again(w, h, 2);
			again.Clear();
			Draw(again, cam, scene.View(), Mat4(), mat);
			again.Resolve();
			check(SamePixels(fresh, again), "the same draw gives the same pixels twice");

			// More vertices than the scratch holds: transformed triangle by triangle, the same result.
			Target small(w, h, 2, 2);
			small.Clear();
			Draw(small, cam, scene.View(), Mat4(), mat);
			small.Resolve();
			check(SamePixels(fresh, small), "a mesh with more vertices than the scratch holds draws the same pixels");

			Target bad_size(0, 10, 1);
			Target bad_grid(8, 8, 3);
			Target usable(8, 8, 1);
			check(bad_size.Empty() && bad_grid.Empty() && !usable.Empty() && !usable.Resize(8, 8, 3) && usable.Empty(),
				"a target with a bad size or sample grid is empty");
			Target none;
			check(!Draw(none, cam, scene.View(), Mat4(), mat), "drawing into an empty target does nothing");
		}

		// Meshes that are wrong: not finite, an index past the end, nothing, far past the picture.
		void TestRobustness(Checker& check)
		{
			const u32 w = 32;
			const u32 h = 32;
			const Camera cam = PixelCamera(w, h);
			const Material mat = PlainMaterial();
			Target t(w, h, 2);
			DrawStats st;

			Geometry nan;
			const float quiet_nan = std::bit_cast<float>(0x7FC00000u);
			nan.Tri(AddPx(nan, h, 4.0f, 4.0f, 5.0f), AddPx(nan, h, 4.0f, 20.0f, 5.0f), nan.Add(quiet_nan, 10.0f, -5.0f));
			t.Clear();
			Draw(t, cam, nan.View(), Mat4(), mat, &st);
			check(st.rejected == 1 && st.written == 0, "a triangle with a vertex that is not finite is skipped");

			Geometry stray;
			stray.Tri(AddPx(stray, h, 4.0f, 4.0f, 5.0f), AddPx(stray, h, 4.0f, 20.0f, 5.0f), 99u);
			t.Clear();
			Draw(t, cam, stray.View(), Mat4(), mat, &st);
			check(st.rejected == 1 && st.written == 0, "a triangle with an index past the vertex count is skipped");

			Mesh empty;
			check(!Draw(t, cam, empty, Mat4(), mat), "a mesh with no vertices draws nothing");

			Geometry gone;
			gone.Tri(AddPx(gone, h, 100.0f, 4.0f, 5.0f), AddPx(gone, h, 100.0f, 20.0f, 5.0f), AddPx(gone, h, 120.0f, 4.0f, 5.0f));
			t.Clear();
			Draw(t, cam, gone.View(), Mat4(), mat, &st);
			check(st.rejected == 1 && st.covered == 0, "a triangle wholly outside the view is thrown out");

			// A triangle thousands of times the size of the picture, as a plain list of vertices (no indices): every sample.
			Geometry huge;
			AddPx(huge, h, -2.0e6f, -2.0e6f, 5.0f);
			AddPx(huge, h, -2.0e6f, 4.0e6f, 5.0f);
			AddPx(huge, h, 4.0e6f, -2.0e6f, 5.0f);
			Mesh huge_mesh = huge.View();
			huge_mesh.indices32 = nullptr;
			huge_mesh.index_count = 0;
			t.Clear();
			Draw(t, cam, huge_mesh, Mat4(), mat, &st);
			check(st.clipped == 1 && st.covered == static_cast<u64>(t.SampleWidth()) * t.SampleHeight(),
				"a triangle far past the picture (cut by the guard band) still covers every sample once");
		}

		// The camera against HandModel's impostor mapping, written out the way RenderImpostor does it, and ProjectPoint
		// against what Draw puts on the image.
		void TestCameras(Checker& check)
		{
			const float eye[3] = {0.12f, 1.31f, 0.35f};
			const float centre[3] = {-0.05f, 1.02f, -0.18f};
			const float right[3] = {0.93f, 0.05f, -0.21f}; // not quite unit, not quite perpendicular to up
			const float up[3] = {0.04f, 0.98f, 0.17f};
			const float size = 0.31f;
			Camera cam;
			check(Camera::Impostor(eye, centre, right, up, size, 0.03f, 20.0f, &cam), "the impostor camera is built for a quad in front of the eye");

			// HandModel::RenderImpostor's frame and projection.
			float r[3] = {right[0], right[1], right[2]};
			Normalize3(r);
			float n[3];
			Cross3(r, up, n);
			Normalize3(n);
			float u[3];
			Cross3(n, r, u);
			const float ec[3] = {eye[0] - centre[0], eye[1] - centre[1], eye[2] - centre[2]};
			const float de = Dot3(ec, n);
			const float ppm = 256.0f / size;
			const float pts[5][3] = {{-0.05f, 1.02f, -0.18f}, {0.0f, 1.0f, -0.3f}, {-0.12f, 1.1f, -0.25f}, {0.08f, 0.93f, -0.1f}, {-0.2f, 1.2f, -0.4f}};
			bool same = true;
			bool landed = true;
			for (const auto& p : pts)
			{
				const float d[3] = {p[0] - eye[0], p[1] - eye[1], p[2] - eye[2]};
				const float z = -Dot3(d, n);
				const float s = de / z;
				const float want_x = 128.0f + (Dot3(ec, r) + s * Dot3(d, r)) * ppm;
				const float want_y = 128.0f - (Dot3(ec, u) + s * Dot3(d, u)) * ppm;
				float px[2];
				same = same && ProjectPoint(cam, 256, 256, p, px) && std::fabs(px[0] - want_x) < 0.02f && std::fabs(px[1] - want_y) < 0.02f;
			}
			check(same, "the impostor camera projects a point to the pixel HandModel::RenderImpostor does");

			// A little triangle drawn round a point lands on the pixel ProjectPoint names.
			Target t(256, 256, 1);
			Material mat = PlainMaterial();
			const float p0[3] = {-0.1f, 1.08f, -0.27f};
			Geometry g;
			g.Tri(g.Add(p0[0] - 0.01f, p0[1] - 0.01f, p0[2]), g.Add(p0[0] + 0.01f, p0[1] - 0.01f, p0[2]), g.Add(p0[0], p0[1] + 0.01f, p0[2]));
			t.Clear();
			Draw(t, cam, g.View(), Mat4(), mat);
			float px[2];
			if (ProjectPoint(cam, 256, 256, p0, px))
			{
				const u32 ix = static_cast<u32>(px[0]);
				const u32 iy = static_cast<u32>(px[1]);
				landed = ix < 256 && iy < 256 && t.Pixel(ix, iy) != 0u;
			}
			else
			{
				landed = false;
			}
			check(landed, "a triangle round a world point is drawn on the pixel ProjectPoint gives it");

			const float behind_eye[3] = {eye[0] + 2.0f * n[0], eye[1] + 2.0f * n[1], eye[2] + 2.0f * n[2]};
			check(!ProjectPoint(cam, 256, 256, behind_eye, px), "a point behind the eye does not project");
			const float flat_eye[3] = {eye[0], eye[1], eye[2]};
			check(!Camera::Impostor(eye, flat_eye, right, up, size, 0.03f, 20.0f, &cam) && !Camera::Impostor(eye, centre, right, up, 0.0f, 0.03f, 20.0f, &cam),
				"no impostor camera for an eye on the quad's plane or a quad of no size");

			// LookAt, Perspective and the pixel mapping agree on which way is up and right.
			const float from[3] = {1.0f, 2.0f, 3.0f};
			const float to[3] = {1.0f, 2.0f, -1.0f};
			const float world_up[3] = {0.0f, 1.0f, 0.0f};
			const Camera look = Camera::Perspective(LookAt(from, to, world_up), 70.0f * kPi / 180.0f, 1.0f, 0.1f, 50.0f);
			const float ahead[3] = {1.0f, 2.0f, -2.0f};
			const float above_right[3] = {1.5f, 2.5f, -2.0f};
			float p_ahead[2];
			float p_ar[2];
			float depth_ahead = 0.0f;
			check(ProjectPoint(look, 100, 80, ahead, p_ahead, &depth_ahead) && std::fabs(p_ahead[0] - 50.0f) < 0.01f && std::fabs(p_ahead[1] - 40.0f) < 0.01f &&
			      depth_ahead > 0.0f && depth_ahead < 1.0f,
				"a point straight ahead of the camera lands in the middle of the image");
			check(ProjectPoint(look, 100, 80, above_right, p_ar) && p_ar[0] > 50.0f && p_ar[1] < 40.0f, "a point up and to the right lands up and to the right (y down in pixels)");

			// ModelMatrix: a quarter turn about +Y takes +X to -Z, then the scale and the translation.
			float quarter[4];
			QuatAxis(0.0f, 1.0f, 0.0f, 0.5f * kPi, quarter);
			const float place[3] = {1.0f, 2.0f, 3.0f};
			const float twice[3] = {2.0f, 2.0f, 2.0f};
			const Mat4 m = ModelMatrix(place, quarter, twice);
			const float unit_x[3] = {1.0f, 0.0f, 0.0f};
			float moved[3];
			TransformPoint(m, unit_x, moved);
			check(std::fabs(moved[0] - 1.0f) < 1.0e-5f && std::fabs(moved[1] - 2.0f) < 1.0e-5f && std::fabs(moved[2] - 1.0f) < 1.0e-5f,
				"ModelMatrix turns, scales and moves in that order");
		}

		// A sphere of about a thousand triangles with normals and UVs, drawn with everything on: a 64 x 64 bilinear
		// texture, lighting with a rim, back faces culled.
		struct Sphere
		{
			std::vector<float> positions;
			std::vector<float> uvs;
			std::vector<u16> indices;
		};

		Sphere MakeSphere(int stacks, int slices)
		{
			Sphere s;
			for (int i = 0; i <= stacks; i++)
			{
				const float theta = kPi * static_cast<float>(i) / static_cast<float>(stacks);
				for (int j = 0; j <= slices; j++)
				{
					const float phi = 2.0f * kPi * static_cast<float>(j) / static_cast<float>(slices);
					s.positions.push_back(std::sin(theta) * std::cos(phi));
					s.positions.push_back(std::cos(theta));
					s.positions.push_back(std::sin(theta) * std::sin(phi));
					s.uvs.push_back(static_cast<float>(j) / static_cast<float>(slices));
					s.uvs.push_back(static_cast<float>(i) / static_cast<float>(stacks));
				}
			}
			for (int i = 0; i < stacks; i++)
			{
				for (int j = 0; j < slices; j++)
				{
					const u16 v00 = static_cast<u16>(i * (slices + 1) + j);
					const u16 v01 = static_cast<u16>(v00 + 1);
					const u16 v10 = static_cast<u16>(v00 + slices + 1);
					const u16 v11 = static_cast<u16>(v10 + 1);
					s.indices.insert(s.indices.end(), {v00, v01, v10, v10, v01, v11});
				}
			}
			return s;
		}

		// A thousand triangles into 256 x 256: a hand's worth (the sphere filling a few thousand pixels, one flat colour, lit,
		// with a rim), a weapon's worth (the same with a nearest-filtered texture) and a big, heavy one (a quarter of the
		// image or more, bilinear texture, lit, with a rim), each at every sample grid. A Clear, a Draw and a Resolve a time,
		// on one core. Only logged: a loaded machine must not fail the test.
		void TestTiming(Checker& check)
		{
			const Sphere sphere = MakeSphere(21, 24);
			u32 texels[64 * 64];
			for (u32 y = 0; y < 64; y++)
			{
				for (u32 x = 0; x < 64; x++)
					texels[y * 64 + x] = (((x >> 3) + (y >> 3)) & 1u) != 0 ? PackRgba(200, 160, 120) : PackRgba(90, 70, 60);
			}
			Texture tex;
			tex.texels = texels;
			tex.width = 64;
			tex.height = 64;
			Mesh mesh;
			mesh.positions = sphere.positions.data();
			mesh.normals = sphere.positions.data(); // a unit sphere's normals are its positions
			mesh.uvs = sphere.uvs.data();
			mesh.vertex_count = static_cast<u32>(sphere.positions.size() / 3);
			mesh.indices16 = sphere.indices.data();
			mesh.index_count = static_cast<u32>(sphere.indices.size());
			const Camera cam = Camera::Perspective(Mat4(), 60.0f * kPi / 180.0f, 1.0f, 0.03f, 20.0f);

			struct Scene
			{
				const char* name;
				float radius;
				bool textured;
				Texture::Filter filter;
				float rim;
			};
			const Scene scenes[3] = {
				{"hand-like (flat colour, lit, rim)", 0.45f, false, Texture::Filter::Nearest, 0.4f},
				{"weapon-like (nearest texture, lit)", 0.45f, true, Texture::Filter::Nearest, 0.0f},
				{"heavy (bilinear texture, lit, rim)", 0.9f, true, Texture::Filter::Bilinear, 0.4f},
			};
			const u32 grids[3] = {1, 2, 4};
			const int runs[3] = {30, 15, 6};
			u32 sink = 0;
			bool drew = true;
			for (const Scene& scene : scenes)
			{
				Material mat;
				tex.filter = scene.filter;
				mat.texture = scene.textured ? &tex : nullptr;
				mat.lighting.light_in_view_space = true;
				mat.lighting.rim = scene.rim;
				mat.lighting.headlight = 0.2f;
				const float pos[3] = {0.0f, 0.0f, -2.4f};
				const float rot[4] = {0.0f, 0.0f, 0.0f, 1.0f};
				const float scale[3] = {scene.radius, scene.radius, scene.radius};
				const Mat4 model = ModelMatrix(pos, rot, scale);

				double ms[3] = {};
				DrawStats st;
				for (int k = 0; k < 3; k++)
				{
					Target t(256, 256, grids[k]);
					for (int i = 0; i < runs[k] + 2; i++)
					{
						const auto begin = std::chrono::steady_clock::now();
						t.Clear();
						Draw(t, cam, mesh, model, mat, &st);
						t.Resolve();
						const auto end = std::chrono::steady_clock::now();
						sink += t.Pixel(128, 128);
						// The first two runs warm the caches and the scratch.
						if (i >= 2)
							ms[k] += std::chrono::duration<double, std::milli>(end - begin).count() / static_cast<double>(runs[k]);
					}
					if (k == 0)
						drew = drew && st.triangles == 1008u && st.written > 1000u;
				}
				Console.WriteLn("(VR) MeshRaster timing: %u triangles into 256x256, %s, %u px drawn: %.3f ms at 1x1, %.3f ms at 2x2, %.3f ms at 4x4 "
								"(Clear + Draw + Resolve, one core)",
					static_cast<unsigned>(st.triangles), scene.name, static_cast<unsigned>(st.written / 16u), ms[0], ms[1], ms[2]);
			}
			check(sink != 0u && drew, "the timing scenes draw a thousand triangles");
		}
	}

	int SelfTest()
	{
		Checker check;
		TestFillRule(check);
		TestPerspectiveTexture(check);
		TestFiltering(check);
		TestDepth(check);
		TestNearClip(check);
		TestCulling(check);
		TestAlpha(check);
		TestLighting(check);
		TestSupersampling(check);
		TestTargetState(check);
		TestRobustness(check);
		TestCameras(check);
		TestTiming(check);
		if (check.failed == 0)
			Console.WriteLn("(VR) MeshRaster self-test: %d checks passed", check.checks);
		else
			Console.Error("(VR) MeshRaster self-test: %d of %d checks FAILED", check.failed, check.checks);
		return check.failed;
	}
}
