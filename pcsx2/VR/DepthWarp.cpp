// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/DepthWarp.h"

#ifdef ENABLE_VULKAN
#include "GS/Renderers/Vulkan/VKBuilders.h"
#include "GS/Renderers/Vulkan/VKShaderCache.h"
#endif

#include "common/Console.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace VR::DepthWarp
{
	namespace
	{
		XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v)
		{
			const float ux = q.x, uy = q.y, uz = q.z, w = q.w;
			const float tx = 2.0f * (uy * v.z - uz * v.y);
			const float ty = 2.0f * (uz * v.x - ux * v.z);
			const float tz = 2.0f * (ux * v.y - uy * v.x);
			return {v.x + w * tx + (uy * tz - uz * ty), v.y + w * ty + (uz * tx - ux * tz), v.z + w * tz + (ux * ty - uy * tx)};
		}

		// The shader's reading of a raw depth value: 1 / (beta * upm * (z - a0)), clamped; the far end when it is not a
		// depth in front of the camera (the cleared z = 0 is 500.2 m on File #1, inside the clamp anyway).
		float ShaderMetres(const DepthExport::ZFit& fit, float units_per_meter, float z)
		{
			const float q = fit.beta * units_per_meter * (z - fit.a0);
			return (q > 0.0f) ? std::clamp(1.0f / q, kNearM, kFarM) : kFarM;
		}
	}

	XrVector3f ToQuadLocal(const XrPosef& quad, const XrVector3f& p)
	{
		const XrQuaternionf& r = quad.orientation;
		return Rotate(XrQuaternionf{-r.x, -r.y, -r.z, r.w},
			XrVector3f{p.x - quad.position.x, p.y - quad.position.y, p.z - quad.position.z});
	}

	bool Unproject(const Geometry& g, float u, float v, float depth_m, XrVector3f* x)
	{
		const XrVector3f& e = g.src_eye;
		if (!(e.z > 1.0e-3f) || !(depth_m > 0.0f) || !(g.width > 0.0f) || !(g.height > 0.0f))
			return false;
		const float qx = (u - 0.5f) * g.width;
		const float qy = (0.5f - v) * g.height;
		const float s = depth_m / e.z;
		*x = {e.x + (qx - e.x) * s, e.y + (qy - e.y) * s, e.z - e.z * s};
		return true;
	}

	bool Project(const Geometry& g, const XrVector3f& x, float* u, float* v, float* depth_m)
	{
		const XrVector3f& f = g.dst_eye;
		const float d = f.z - x.z;
		if (!(f.z > 1.0e-3f) || !(d > 1.0e-4f) || !(g.width > 0.0f) || !(g.height > 0.0f))
			return false;
		const float t = f.z / d;
		const float qx = f.x + (x.x - f.x) * t;
		const float qy = f.y + (x.y - f.y) * t;
		*u = qx / g.width + 0.5f;
		*v = 0.5f - qy / g.height;
		*depth_m = d;
		return true;
	}

	bool Reproject(const Geometry& g, float u, float v, float depth_m, float* u2, float* v2, float* depth2_m)
	{
		XrVector3f x;
		return Unproject(g, u, v, depth_m, &x) && Project(g, x, u2, v2, depth2_m);
	}

	double SidewaysShiftM(double move_m, double quad_distance_m, double depth_m)
	{
		return move_m * (1.0 - quad_distance_m / depth_m);
	}

	bool GridVertex(const Geometry& g, const DepthExport::ZFit& fit, float units_per_meter, const float* plane, u32 width,
		u32 height, u32 gx, u32 gy, u32 cells, float* u2, float* v2)
	{
		// As the vertex shader: the grid spans [-margin, 1 + margin], the colour is read at the uv clamped to the image.
		const float m = kGridMargin;
		const float u = -m + static_cast<float>(gx) * ((1.0f + 2.0f * m) / static_cast<float>(cells));
		const float v = -m + static_cast<float>(gy) * ((1.0f + 2.0f * m) / static_cast<float>(cells));
		*u2 = u;
		*v2 = v;
		if (!plane || width == 0 || height == 0 || !(g.src_eye.z > 1.0e-3f) || !(g.dst_eye.z > 1.0e-3f))
			return false;
		const float uc = std::clamp(u, 0.0f, 1.0f);
		const float vc = std::clamp(v, 0.0f, 1.0f);
		float d = kFarM;
		for (int j = -1; j <= 1; j++)
		{
			for (int i = -1; i <= 1; i++)
			{
				const float su = uc + static_cast<float>(i) * kFootprintTexels / static_cast<float>(width);
				const float sv = vc + static_cast<float>(j) * kFootprintTexels / static_cast<float>(height);
				const int px = std::clamp(static_cast<int>(su * static_cast<float>(width)), 0, static_cast<int>(width) - 1);
				const int py = std::clamp(static_cast<int>(sv * static_cast<float>(height)), 0, static_cast<int>(height) - 1);
				d = std::min(d, ShaderMetres(fit, units_per_meter, plane[static_cast<size_t>(py) * width + px]));
			}
		}
		// The shader's own steps (the near clamp on the new depth included).
		const XrVector3f& e = g.src_eye;
		const XrVector3f& f = g.dst_eye;
		const float qx = (u - 0.5f) * g.width, qy = (0.5f - v) * g.height;
		const float s = d / e.z;
		const XrVector3f x = {e.x + (qx - e.x) * s, e.y + (qy - e.y) * s, e.z - e.z * s};
		const float dd = std::max(f.z - x.z, kNearM);
		const float t = f.z / dd;
		*u2 = (f.x + (x.x - f.x) * t) / g.width + 0.5f;
		*v2 = 0.5f - (f.y + (x.y - f.y) * t) / g.height;
		return true;
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
				Console.Error("(VR) Depth warp self-test FAIL: %s", name);
			}
		};
		const auto near_abs = [](double a, double b, double tol) { return std::abs(a - b) <= tol; };

		// The first-person quad offline: 1.5 m ahead, 4.77 x 3.58 m (100 deg vertical), the image 1536 x 1344.
		const float W = 4.77f, H = 3.58f, D = 1.5f;
		const u32 IW = 1536, IH = 1344;

		// --- The quad frame ----------------------------------------------------------------------------------------------
		{
			// A quad turned 30 deg about Y and 10 deg about X, somewhere in the room: its own axes come back as the unit axes.
			const float a = 30.0f * 3.14159265f / 180.0f, b = 10.0f * 3.14159265f / 180.0f;
			const XrQuaternionf qy = {0.0f, std::sin(a / 2), 0.0f, std::cos(a / 2)};
			const XrQuaternionf qx = {std::sin(b / 2), 0.0f, 0.0f, std::cos(b / 2)};
			const XrQuaternionf q = {qy.w * qx.x + qy.x * qx.w + qy.y * qx.z - qy.z * qx.y,
				qy.w * qx.y - qy.x * qx.z + qy.y * qx.w + qy.z * qx.x, qy.w * qx.z + qy.x * qx.y - qy.y * qx.x + qy.z * qx.w,
				qy.w * qx.w - qy.x * qx.x - qy.y * qx.y - qy.z * qx.z};
			const XrPosef quad = {q, {0.3f, 1.6f, -1.2f}};
			const XrVector3f off = Rotate(q, XrVector3f{1.0f, 2.0f, 3.0f});
			const XrVector3f l = ToQuadLocal(quad, XrVector3f{0.3f + off.x, 1.6f + off.y, -1.2f + off.z});
			check(near_abs(l.x, 1.0, 1e-5) && near_abs(l.y, 2.0, 1e-5) && near_abs(l.z, 3.0, 1e-5),
				"a point at the quad's own (1, 2, 3) reads (1, 2, 3) in its frame");
		}

		// --- The round trip: the eye not moved, every pixel stays at every depth -----------------------------------------
		{
			Geometry g;
			g.width = W;
			g.height = H;
			g.src_eye = g.dst_eye = {0.032f, 0.0f, D};
			bool ok = true;
			for (float u : {0.0f, 0.13f, 0.5f, 0.91f, 1.0f})
				for (float v : {0.0f, 0.27f, 0.5f, 1.0f})
					for (float d : {0.06f, 1.05f, 1.5f, 7.88f, 500.0f})
					{
						float u2, v2, d2;
						ok = ok && Reproject(g, u, v, d, &u2, &v2, &d2) && near_abs(u2, u, 2e-6) && near_abs(v2, v, 2e-6) &&
						     std::abs(d2 - d) <= 1e-5f * d;
					}
			check(ok, "an unmoved eye: every pixel and depth comes back unchanged (round trip)");
		}

		// --- Sideways: 10 cm to the right, the parallax at known depths --------------------------------------------------
		{
			Geometry g;
			g.width = W;
			g.height = H;
			g.src_eye = {-0.032f, 0.0f, D};
			g.dst_eye = {-0.032f + 0.10f, 0.0f, D};
			const double px_per_m = IW / static_cast<double>(W);
			float u2, v2, d2;
			// The counter top at 1.045 m (slot 7): it moves against the head, 0.1 (1 - 1.5 / 1.045) = -4.35 cm = -14.0 px.
			Reproject(g, 0.5f, 0.6f, 1.045f, &u2, &v2, &d2);
			check(near_abs((u2 - 0.5) * W, SidewaysShiftM(0.10, D, 1.045), 1e-5) && near_abs(v2, 0.6, 1e-6),
				"the counter at 1.045 m shifts 0.1 (1 - 1.5 / 1.045) m against a 10 cm lean, not up or down");
			check(near_abs((u2 - 0.5) * W * px_per_m, -14.02, 0.05), "that is -14.0 px on the 1536 px image");
			// At the quad's own distance: no shift.
			Reproject(g, 0.3f, 0.4f, D, &u2, &v2, &d2);
			check(near_abs(u2, 0.3, 1e-6) && near_abs(v2, 0.4, 1e-6), "a point at the quad's distance (1.5 m) stays");
			// A wall at 4 m: with the head, 0.1 (1 - 1.5 / 4) = 6.25 cm = 20.1 px; so the counter moves 34.2 px against it.
			Reproject(g, 0.2f, 0.4f, 4.0f, &u2, &v2, &d2);
			check(near_abs((u2 - 0.2) * W, 0.0625, 1e-5), "a wall at 4 m shifts 6.25 cm with the head");
			// The far plane (the cleared depth, 500 m): with the head by all but 0.3%: a fixed direction.
			Reproject(g, 0.8f, 0.1f, 500.0f, &u2, &v2, &d2);
			check(near_abs((u2 - 0.8) * W, 0.10 * (1.0 - D / 500.0), 1e-5), "the far plane moves with the head (no parallax)");
			check(near_abs(d2, 500.0, 1e-3), "a sideways move keeps the view depth");
		}

		// --- Any move, against the direct projection: the warped pixel is where the world point is from the new eye -----
		{
			Geometry g;
			g.width = W;
			g.height = H;
			g.src_eye = {0.02f, -0.01f, D};      // an eye rolled a little (y off the axis)
			g.dst_eye = {0.07f, 0.04f, D - 0.12f}; // leaned 12 cm forward, 5 cm right and up
			bool ok = true;
			for (const XrVector3f& x : {XrVector3f{0.4f, -0.35f, D - 1.05f}, XrVector3f{-2.0f, 1.0f, D - 6.0f},
					 XrVector3f{0.0f, 0.0f, D - 0.6f}, XrVector3f{30.0f, -8.0f, D - 120.0f}})
			{
				// Where the source eye saw it (u, v) and how deep (its view depth along -Z).
				Geometry src = g;
				src.dst_eye = g.src_eye;
				float u, v, d;
				Project(src, x, &u, &v, &d);
				float u2, v2, d2, ue, ve, de;
				Project(g, x, &ue, &ve, &de);
				ok = ok && Reproject(g, u, v, d, &u2, &v2, &d2) && near_abs(u2, ue, 1e-5) && near_abs(v2, ve, 1e-5) &&
				     std::abs(d2 - de) <= 1e-4f * de;
			}
			check(ok, "a lean in any direction: each pixel lands where its world point projects from the new eye");
			// The shift depends on the move only, not on where the source eye sits beside the axis (so each eye's image
			// moves alike, and a head roll between the eyes does not matter).
			Geometry a = g, b = g;
			a.src_eye = {-0.032f, 0.0f, D};
			a.dst_eye = {-0.032f + 0.1f, 0.0f, D};
			b.src_eye = {0.032f, 0.01f, D};
			b.dst_eye = {0.032f + 0.1f, 0.01f, D};
			float ua, va, da, ub, vb, db;
			Reproject(a, 0.37f, 0.52f, 2.2f, &ua, &va, &da);
			Reproject(b, 0.37f, 0.52f, 2.2f, &ub, &vb, &db);
			check(near_abs(ua, ub, 1e-6) && near_abs(va, vb, 1e-6), "a sideways move shifts the left and right eye's pixel alike");
			// Behind the new eye: refused.
			Geometry c = g;
			c.dst_eye = {0.0f, 0.0f, 0.1f};
			float uc, vc, dc;
			XrVector3f behind;
			Unproject(c, 0.5f, 0.5f, 0.0f, &behind);
			check(!Reproject(c, 0.5f, 0.5f, -1.0f, &uc, &vc, &dc), "no depth: no warp");
		}

		// --- The conversion the shader reads the depth with ---------------------------------------------------------------
		DepthExport::ZFit fit;
		fit.valid = true;
		fit.a0 = static_cast<float>(-335.2 / 4294967296.0);
		fit.beta = static_cast<float>(5.960436e-08 * 4294967296.0);
		const float upm = 100.0f;
		{
			bool ok = true;
			for (double m : {0.06, 0.5, 1.045, 1.5, 7.881, 120.0, 400.0})
			{
				const float z = static_cast<float>(DepthExport::DepthValueFor(fit, upm, m));
				ok = ok && std::abs(ShaderMetres(fit, upm, z) - m) <= 2e-4 * m &&
				     std::abs(ShaderMetres(fit, upm, z) - DepthExport::LinearDepthM(fit, upm, z)) <= 2e-4 * m;
			}
			check(ok, "the shader's float reading of a stored depth is its metres (0.06-400 m, 0.02%), as LinearDepthM");
			check(ShaderMetres(fit, upm, 0.0f) >= 499.0f, "the cleared depth (z = 0) reads as the far plane");
			check(ShaderMetres(fit, upm, 1.0f) <= kNearM + 1e-6f && ShaderMetres(fit, upm, -1.0f) == kFarM,
				"the reading is clamped (nearer than 5 cm, beyond infinity)");
		}

		// --- The grid on a synthetic depth plane: a counter at 1.045 m in a room 4 m deep ---------------------------------
		{
			std::vector<float> plane(static_cast<size_t>(IW) * IH, static_cast<float>(DepthExport::DepthValueFor(fit, upm, 4.0)));
			const float z_counter = static_cast<float>(DepthExport::DepthValueFor(fit, upm, 1.045));
			// The counter: the image's middle third across, its lower 40 % down.
			for (u32 y = IH * 6 / 10; y < IH; y++)
				for (u32 x = IW / 3; x < IW * 2 / 3; x++)
					plane[static_cast<size_t>(y) * IW + x] = z_counter;
			// The top 10 % is the cleared background.
			for (u32 y = 0; y < IH / 10; y++)
				for (u32 x = 0; x < IW; x++)
					plane[static_cast<size_t>(y) * IW + x] = 0.0f;
			Geometry g;
			g.width = W;
			g.height = H;
			g.src_eye = {0.032f, 0.0f, D};
			g.dst_eye = {0.132f, 0.0f, D};
			const u32 n = kGridCells;
			const float step = (1.0f + 2.0f * kGridMargin) / static_cast<float>(n);
			const auto grid_u = [&](u32 gx) { return -kGridMargin + static_cast<float>(gx) * step; };
			// A vertex well inside the counter, one in the wall, one in the background, one just outside the counter's
			// edge (within the footprint: it takes the counter's depth), one beyond the image's edge.
			const auto index_of = [&](float uv) { return static_cast<u32>(std::lround((uv + kGridMargin) / step)); };
			const u32 gx_counter = index_of(0.5f), gy_counter = index_of(0.8f);
			const u32 gx_wall = index_of(0.15f), gy_wall = index_of(0.4f);
			const u32 gy_sky = index_of(0.03f);
			float u2, v2;
			const double px = IW / static_cast<double>(W);
			bool ok = GridVertex(g, fit, upm, plane.data(), IW, IH, gx_counter, gy_counter, n, &u2, &v2);
			check(ok && near_abs((u2 - grid_u(gx_counter)) * W * px, SidewaysShiftM(0.1, D, 1.045) * px, 0.05),
				"grid: a vertex on the counter shifts -14.0 px");
			ok = GridVertex(g, fit, upm, plane.data(), IW, IH, gx_wall, gy_wall, n, &u2, &v2);
			check(ok && near_abs((u2 - grid_u(gx_wall)) * W * px, SidewaysShiftM(0.1, D, 4.0) * px, 0.05),
				"grid: a vertex on the wall at 4 m shifts +20.1 px");
			ok = GridVertex(g, fit, upm, plane.data(), IW, IH, gx_wall, gy_sky, n, &u2, &v2);
			check(ok && near_abs((u2 - grid_u(gx_wall)) * W * px, SidewaysShiftM(0.1, D, 500.0) * px, 0.05),
				"grid: a vertex on the cleared background shifts with the head (+32.1 px)");
			// The counter's left edge is at u = 1/3; the last vertex left of it within 6 texels takes the counter's depth.
			u32 gx_edge = 0;
			for (u32 gx = 0; gx <= n; gx++)
			{
				const float u = grid_u(gx);
				if (u < 1.0f / 3.0f && (1.0f / 3.0f - u) * IW < kFootprintTexels - 0.5f)
					gx_edge = gx;
			}
			if (gx_edge != 0)
			{
				ok = GridVertex(g, fit, upm, plane.data(), IW, IH, gx_edge, gy_counter, n, &u2, &v2);
				check(ok && near_abs((u2 - grid_u(gx_edge)) * W * px, SidewaysShiftM(0.1, D, 1.045) * px, 0.05),
					"grid: a vertex beside the counter's edge takes its (nearer) depth");
			}
			ok = GridVertex(g, fit, upm, plane.data(), IW, IH, 0, gy_wall, n, &u2, &v2);
			check(ok && grid_u(0) < 0.0f && near_abs((u2 - grid_u(0)) * W * px, SidewaysShiftM(0.1, D, 4.0) * px, 0.05),
				"grid: the margin vertex past the image's edge moves with the edge's depth");
			// The grid's edges reach past the image both ways, wider than the largest shift (32 px), so no border opens.
			check(kGridMargin * IW > 1.5 * 0.1 * px, "the margin covers a 15 cm lean's far-plane shift");
			// No usable geometry: the vertex is not displaced.
			Geometry bad = g;
			bad.src_eye.z = 0.0f;
			ok = GridVertex(bad, fit, upm, plane.data(), IW, IH, gx_wall, gy_wall, n, &u2, &v2);
			check(!ok && u2 == grid_u(gx_wall), "no source eye in front of the quad: the vertex stays");
			// An unmoved eye: the grid is the identity.
			Geometry still = g;
			still.dst_eye = still.src_eye;
			ok = GridVertex(still, fit, upm, plane.data(), IW, IH, gx_counter, gy_counter, n, &u2, &v2);
			check(ok && near_abs(u2, grid_u(gx_counter), 1e-6) && near_abs(v2, grid_u(gy_counter), 1e-6),
				"an unmoved eye: the grid is the identity");
		}

		Console.WriteLn("(VR) Depth warp self-test: %d/%d checks passed.", checks - failed, checks);
		return failed == 0;
	}

#ifdef ENABLE_VULKAN
	PushConstants MakePushConstants(const Geometry& g, bool warp, const DepthExport::ZFit& fit, float units_per_meter,
		u32 width, u32 height, u32 plane_offset_floats)
	{
		PushConstants pc = {};
		pc.src_eye[0] = g.src_eye.x;
		pc.src_eye[1] = g.src_eye.y;
		pc.src_eye[2] = g.src_eye.z;
		pc.src_eye[3] = warp ? 1.0f : 0.0f;
		pc.dst_eye[0] = g.dst_eye.x;
		pc.dst_eye[1] = g.dst_eye.y;
		pc.dst_eye[2] = g.dst_eye.z;
		pc.dst_eye[3] = static_cast<float>(kGridCells);
		pc.quad[0] = g.width;
		pc.quad[1] = g.height;
		pc.quad[2] = fit.a0;
		pc.quad[3] = fit.beta * units_per_meter;
		pc.clampm[0] = kNearM;
		pc.clampm[1] = kFarM;
		pc.clampm[2] = kGridMargin;
		pc.clampm[3] = kFootprintTexels;
		pc.img[0] = width;
		pc.img[1] = height;
		pc.img[2] = plane_offset_floats;
		pc.img[3] = 0;
		return pc;
	}

	namespace
	{
		// The mesh: GridVertex's steps, per vertex (no vertex buffer: the vertex index names the cell and the corner).
		constexpr const char* kVertexShader = R"(#version 450 core
layout(push_constant) uniform PC
{
	vec4 src_eye;
	vec4 dst_eye;
	vec4 quad;
	vec4 clampm;
	uvec4 img;
} pc;
layout(std430, set = 0, binding = 1) readonly buffer DepthPlane { float z[]; } dp;
layout(location = 0) out vec2 v_uv;

float MetresAt(vec2 uv)
{
	ivec2 size = ivec2(pc.img.xy);
	ivec2 p = clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1);
	float zn = dp.z[pc.img.z + uint(p.y) * pc.img.x + uint(p.x)];
	float q = pc.quad.w * (zn - pc.quad.z);
	return (q > 0.0) ? clamp(1.0 / q, pc.clampm.x, pc.clampm.y) : pc.clampm.y;
}

void main()
{
	const ivec2 corner[6] = ivec2[6](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 0), ivec2(1, 1), ivec2(0, 1));
	int n = int(pc.dst_eye.w);
	int cell = gl_VertexIndex / 6;
	ivec2 g = ivec2(cell % n, cell / n) + corner[gl_VertexIndex % 6];
	float m = pc.clampm.z;
	vec2 uv = vec2(-m) + vec2(g) * ((1.0 + 2.0 * m) / float(n));
	vec2 uvc = clamp(uv, vec2(0.0), vec2(1.0));
	v_uv = uvc;
	vec2 uv2 = uv;
	float depth_out = 0.5;
	if (pc.src_eye.w > 0.5)
	{
		vec2 fp = vec2(pc.clampm.w) / vec2(pc.img.xy);
		float d = pc.clampm.y;
		for (int j = -1; j <= 1; j++)
			for (int i = -1; i <= 1; i++)
				d = min(d, MetresAt(uvc + vec2(float(i), float(j)) * fp));
		vec3 E = pc.src_eye.xyz;
		vec3 F = pc.dst_eye.xyz;
		vec3 Q = vec3((uv.x - 0.5) * pc.quad.x, (0.5 - uv.y) * pc.quad.y, 0.0);
		vec3 X = E + (Q - E) * (d / E.z);
		float dd = max(F.z - X.z, pc.clampm.x);
		vec3 Q2 = F + (X - F) * (F.z / dd);
		uv2 = vec2(Q2.x / pc.quad.x + 0.5, 0.5 - Q2.y / pc.quad.y);
		depth_out = clamp(pc.clampm.x / dd, 0.0, 1.0);
	}
	gl_Position = vec4(uv2 * 2.0 - 1.0, depth_out, 1.0);
}
)";

		constexpr const char* kFragmentShader = R"(#version 450 core
layout(set = 0, binding = 0) uniform sampler2D src_tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
void main()
{
	o_col = texture(src_tex, v_uv);
}
)";
	}

	bool CreatePipeline(VkDevice device, VkFormat colour_format, Pipeline* out)
	{
		*out = Pipeline{};
		if (!g_vulkan_shader_cache || device == VK_NULL_HANDLE)
			return false;
		Pipeline p;
		p.colour_format = colour_format;

		Vulkan::RenderPassBuilder rpb;
		rpb.AddAttachment(colour_format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		rpb.AddAttachment(kDepthFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		const u32 sub = rpb.AddSubpass();
		rpb.AddSubpassColorAttachment(sub, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		rpb.AddSubpassDepthAttachment(sub, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		p.render_pass = rpb.Create(device);

		Vulkan::DescriptorSetLayoutBuilder dslb;
		dslb.SetPushFlag();
		dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
		dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT);
		p.ds_layout = (p.render_pass != VK_NULL_HANDLE) ? dslb.Create(device) : VK_NULL_HANDLE;
		if (p.ds_layout != VK_NULL_HANDLE)
		{
			Vulkan::PipelineLayoutBuilder plb;
			plb.AddPushConstants(VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants));
			plb.AddDescriptorSet(p.ds_layout);
			p.layout = plb.Create(device);
		}

		VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		sci.magFilter = VK_FILTER_LINEAR;
		sci.minFilter = VK_FILTER_LINEAR;
		sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sci.maxLod = 0.0f;
		if (vkCreateSampler(device, &sci, nullptr, &p.sampler) != VK_SUCCESS)
			p.sampler = VK_NULL_HANDLE;

		const VkShaderModule vs = (p.layout != VK_NULL_HANDLE) ? g_vulkan_shader_cache->GetVertexShader(kVertexShader) : VK_NULL_HANDLE;
		const VkShaderModule fs = (vs != VK_NULL_HANDLE) ? g_vulkan_shader_cache->GetFragmentShader(kFragmentShader) : VK_NULL_HANDLE;
		if (vs != VK_NULL_HANDLE && fs != VK_NULL_HANDLE && p.sampler != VK_NULL_HANDLE)
		{
			Vulkan::GraphicsPipelineBuilder gpb;
			gpb.SetPipelineLayout(p.layout);
			gpb.SetRenderPass(p.render_pass, 0);
			gpb.SetVertexShader(vs);
			gpb.SetFragmentShader(fs);
			gpb.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
			gpb.SetNoCullRasterizationState();
			gpb.SetDepthState(true, true, VK_COMPARE_OP_GREATER_OR_EQUAL);
			gpb.SetNoStencilState();
			gpb.SetNoBlendingState();
			gpb.SetDynamicViewportAndScissorState();
			p.pipeline = gpb.Create(device, g_vulkan_shader_cache->GetPipelineCache(true), false);
		}
		if (vs != VK_NULL_HANDLE)
			vkDestroyShaderModule(device, vs, nullptr);
		if (fs != VK_NULL_HANDLE)
			vkDestroyShaderModule(device, fs, nullptr);
		if (p.pipeline == VK_NULL_HANDLE)
		{
			DestroyPipeline(device, &p);
			return false;
		}
		*out = p;
		return true;
	}

	void DestroyPipeline(VkDevice device, Pipeline* p)
	{
		if (device != VK_NULL_HANDLE)
		{
			if (p->pipeline != VK_NULL_HANDLE)
				vkDestroyPipeline(device, p->pipeline, nullptr);
			if (p->layout != VK_NULL_HANDLE)
				vkDestroyPipelineLayout(device, p->layout, nullptr);
			if (p->ds_layout != VK_NULL_HANDLE)
				vkDestroyDescriptorSetLayout(device, p->ds_layout, nullptr);
			if (p->render_pass != VK_NULL_HANDLE)
				vkDestroyRenderPass(device, p->render_pass, nullptr);
			if (p->sampler != VK_NULL_HANDLE)
				vkDestroySampler(device, p->sampler, nullptr);
		}
		*p = Pipeline{};
	}
#endif
}
