// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/EyeProjection.h"

#include "common/Console.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace VR::EyeProjection
{
	namespace
	{
		bool Finite(float v) { return std::isfinite(v); }
	} // namespace

	void CameraTangents(const GameCamera& cam, float tan[4])
	{
		const float ax = std::abs(cam.fx);
		const float ay = std::abs(cam.fy);
		tan[0] = (ax > 0.0f) ? -cam.half_w / ax : 0.0f;
		tan[1] = (ax > 0.0f) ? cam.half_w / ax : 0.0f;
		tan[2] = (ay > 0.0f) ? cam.half_h / ay : 0.0f;
		tan[3] = (ay > 0.0f) ? -cam.half_h / ay : 0.0f;
	}

	void PlaceEye(const GameCamera& cam, const float pos_m[3], float yaw_rad, float roll_rad, Eye* eye)
	{
		const float cr = std::cos(roll_rad), sr = std::sin(roll_rad);
		const float cy = std::cos(yaw_rad), sy = std::sin(yaw_rad);
		// Q = Rz(roll) * Ry(yaw): its columns are the eye's axes in the camera frame.
		float q[3][3];
		q[0][0] = cr * cy;  q[0][1] = -sr; q[0][2] = cr * sy;
		q[1][0] = sr * cy;  q[1][1] = cr;  q[1][2] = sr * sy;
		q[2][0] = -sy;      q[2][1] = 0.0f; q[2][2] = cy;
		// p_eye = Q^T (p - P), P in game units.
		for (int r = 0; r < 3; r++)
		{
			float t = 0.0f;
			for (int c = 0; c < 3; c++)
			{
				eye->m[r][c] = q[c][r];
				t += q[c][r] * pos_m[c] * cam.units_per_m;
			}
			eye->m[r][3] = -t;
		}
		for (int i = 0; i < 3; i++)
			eye->pos_m[i] = pos_m[i];
		const float hz = 0.5f * roll_rad, hy = 0.5f * yaw_rad;
		const float czh = std::cos(hz), szh = std::sin(hz), cyh = std::cos(hy), syh = std::sin(hy);
		eye->quat[0] = -szh * syh;
		eye->quat[1] = czh * syh;
		eye->quat[2] = szh * cyh;
		eye->quat[3] = czh * cyh;
	}

	bool Build(const GameCamera& cam, const EyeSpec& spec, Eye out[2])
	{
		if (!(std::abs(cam.fx) > 1e-3f) || !(std::abs(cam.fy) > 1e-3f) || !(cam.half_w > 0.0f) || !(cam.half_h > 0.0f) ||
			!(cam.units_per_m > 0.0f) || !Finite(spec.ipd_m) || !Finite(spec.cant_rad) || !Finite(spec.roll_rad))
			return false;
		const float cr = std::cos(spec.roll_rad);
		const float sr = std::sin(spec.roll_rad);
		for (int e = 0; e < 2; e++)
		{
			const float tl = spec.tan[e][0], tr = spec.tan[e][1], tu = spec.tan[e][2], td = spec.tan[e][3];
			if (!(tr - tl > 1e-4f) || !(tu - td > 1e-4f) || !Finite(tl) || !Finite(tr) || !Finite(tu) || !Finite(td))
				return false;
			Eye& o = out[e];
			const float side = (e == 0) ? -1.0f : 1.0f;
			// The eye's turn in the camera frame: the head's roll about z, then the view's outward yaw (OpenXR: a positive
			// yaw turns left, so the left view turns +cant and the right one -cant).
			const float yaw = -side * spec.cant_rad;
			// The eye's position: half the IPD along the rolled x axis.
			const float half = side * 0.5f * spec.ipd_m;
			const float p_m[3] = {cr * half, sr * half, 0.0f};
			PlaceEye(cam, p_m, yaw, spec.roll_rad, &o);
			// The eye's window [tl, tr] x [td, tu] spans the target: X from cx - half_w to cx + half_w, Y (down) from
			// cy - half_h at tu to cy + half_h at td.
			o.fx = 2.0f * cam.half_w / (tr - tl);
			o.cx = cam.cx - cam.half_w - tl * o.fx;
			o.fy = -2.0f * cam.half_h / (tu - td);
			o.cy = cam.cy - cam.half_h - tu * o.fy;
			o.fov_rad[0] = std::atan(tl);
			o.fov_rad[1] = std::atan(tr);
			o.fov_rad[2] = std::atan(tu);
			o.fov_rad[3] = std::atan(td);
		}
		return true;
	}

	bool MapVertex(const GameCamera& cam, const Eye& eye, float x, float y, float w, float* x2, float* y2, float* w2,
		float min_w)
	{
		const float p[3] = {(x - cam.cx) * w / cam.fx, (y - cam.cy) * w / cam.fy, -w};
		float pe[3];
		for (int r = 0; r < 3; r++)
			pe[r] = eye.m[r][0] * p[0] + eye.m[r][1] * p[1] + eye.m[r][2] * p[2] + eye.m[r][3];
		const float we = -pe[2];
		if (!(we >= min_w))
			return false;
		*x2 = eye.cx + eye.fx * pe[0] / we;
		*y2 = eye.cy + eye.fy * pe[1] / we;
		*w2 = we;
		return true;
	}

	unsigned short GameTextMapFixed(unsigned short v, float centre_px, float s)
	{
		const float m = GameTextMap(static_cast<float>(v), centre_px * 16.0f, s);
		if (!std::isfinite(m))
			return v;
		return static_cast<unsigned short>(std::clamp(std::lround(m), 0L, 65535L));
	}

	float GameTextDegreesBelow(float y, float cy, float half_h, float fraction, float tan_half)
	{
		if (!(half_h > 0.0f))
			return 0.0f;
		return std::atan((y - cy) / half_h * fraction * tan_half) * (180.0f / 3.14159265358979323846f);
	}

	bool SelfTest()
	{
		int checks = 0;
		std::string fails;
		const auto check = [&](bool ok, const char* what) {
			checks++;
			if (!ok)
				fails += std::string(fails.empty() ? "" : "; ") + what;
		};
		const auto near = [](float a, float b, float tol) { return std::abs(a - b) <= tol; };

		GameCamera cam; // File #1 in first person (the probe's log): fx 161.107, fy -187.958, centre 2048
		cam.fx = 161.107f;
		cam.fy = -187.958f;

		// 1. No IPD, no cant, the camera's own tangents: every vertex maps to itself.
		{
			EyeSpec s;
			s.ipd_m = 0.0f;
			CameraTangents(cam, s.tan[0]);
			CameraTangents(cam, s.tan[1]);
			Eye e[2];
			check(Build(cam, s, e), "identity build");
			float x2, y2, w2;
			bool ok = MapVertex(cam, e[1], 1900.0f, 2100.0f, 137.0f, &x2, &y2, &w2);
			check(ok && near(x2, 1900.0f, 1e-2f) && near(y2, 2100.0f, 1e-2f) && near(w2, 137.0f, 1e-3f), "identity map");
			check(near(e[0].fx, cam.fx, 1e-2f) && near(e[0].cy, cam.cy, 1e-2f) && near(e[0].fy, cam.fy, 1e-2f),
				"identity intrinsics");
		}
		// 2. A sideways eye: X' = X -/+ fx e / w, Y and w unchanged (the probe's map, checked against real renders).
		{
			EyeSpec s;
			s.ipd_m = 0.10f; // 5 cm each side
			CameraTangents(cam, s.tan[0]);
			CameraTangents(cam, s.tan[1]);
			Eye e[2];
			Build(cam, s, e);
			float x2, y2, w2;
			MapVertex(cam, e[1], 2000.0f, 1990.0f, 120.0f, &x2, &y2, &w2);
			check(near(x2, 2000.0f - cam.fx * 5.0f / 120.0f, 1e-2f) && near(y2, 1990.0f, 1e-2f) && near(w2, 120.0f, 1e-3f),
				"right eye shift");
			MapVertex(cam, e[0], 2000.0f, 1990.0f, 120.0f, &x2, &y2, &w2);
			check(near(x2, 2000.0f + cam.fx * 5.0f / 120.0f, 1e-2f), "left eye shift");
			check(near(e[1].pos_m[0], 0.05f, 1e-6f) && near(e[0].pos_m[0], -0.05f, 1e-6f), "eye positions");
		}
		// 3. Asymmetric tangents: the eye's left edge direction lands on the target's left edge, its up edge on the top.
		{
			EyeSpec s;
			s.ipd_m = 0.0f;
			const float t[4] = {-1.2f, 0.9f, 1.0f, -1.3f};
			for (int k = 0; k < 4; k++)
				s.tan[0][k] = s.tan[1][k] = t[k];
			Eye e[2];
			Build(cam, s, e);
			// A camera point at w = 200 in direction (tl, tu): X = cx + fx * tl, Y = cy + fy * tu in the game's projection.
			float x2, y2, w2;
			MapVertex(cam, e[0], cam.cx + cam.fx * -1.2f, cam.cy + cam.fy * 1.0f, 200.0f, &x2, &y2, &w2);
			check(near(x2, cam.cx - cam.half_w, 1e-2f) && near(y2, cam.cy - cam.half_h, 1e-2f), "asymmetric corner");
			MapVertex(cam, e[0], cam.cx + cam.fx * 0.9f, cam.cy + cam.fy * -1.3f, 50.0f, &x2, &y2, &w2);
			check(near(x2, cam.cx + cam.half_w, 1e-2f) && near(y2, cam.cy + cam.half_h, 1e-2f), "asymmetric far corner");
			check(near(e[0].fov_rad[0], std::atan(-1.2f), 1e-6f) && near(e[0].fov_rad[3], std::atan(-1.3f), 1e-6f),
				"fov angles");
		}
		// 4. Canted views: the point straight ahead of each turned eye lands on its window's centre.
		{
			EyeSpec s;
			s.ipd_m = 0.064f;
			s.cant_rad = 10.0f * 3.14159265f / 180.0f;
			for (int k = 0; k < 4; k++)
				s.tan[0][k] = s.tan[1][k] = (k == 0 || k == 3) ? -1.0f : 1.0f;
			Eye e[2];
			Build(cam, s, e);
			for (int i = 0; i < 2; i++)
			{
				// The eye's forward axis in the camera frame is -(third column of Q) = -(m row 2) since m = Q^T.
				const float d = 300.0f;
				const float px = e[i].pos_m[0] * 100.0f - e[i].m[2][0] * d;
				const float py = e[i].pos_m[1] * 100.0f - e[i].m[2][1] * d;
				const float pz = e[i].pos_m[2] * 100.0f - e[i].m[2][2] * d;
				const float w = -pz;
				float x2, y2, w2;
				MapVertex(cam, e[i], cam.cx + cam.fx * px / w, cam.cy + cam.fy * py / w, w, &x2, &y2, &w2);
				check(near(x2, cam.cx, 1e-2f) && near(y2, cam.cy, 1e-2f) && near(w2, d, 1e-2f), "canted centre");
			}
			// Outward: the left view looks left (forward x negative).
			check(-e[0].m[2][0] < 0.0f && -e[1].m[2][0] > 0.0f, "cant outward");
			// The quaternion turns -z the same way.
			const float* qq = e[0].quat;
			const float vx = 0.0f, vy = 0.0f, vz = -1.0f;
			// v' = v + 2w (q x v) + 2 q x (q x v)
			const float cx1 = qq[1] * vz - qq[2] * vy, cy1 = qq[2] * vx - qq[0] * vz, cz1 = qq[0] * vy - qq[1] * vx;
			const float cx2 = qq[1] * cz1 - qq[2] * cy1, cy2 = qq[2] * cx1 - qq[0] * cz1, cz2 = qq[0] * cy1 - qq[1] * cx1;
			const float rx = vx + 2.0f * (qq[3] * cx1 + cx2), ry = vy + 2.0f * (qq[3] * cy1 + cy2),
						rz = vz + 2.0f * (qq[3] * cz1 + cz2);
			check(near(rx, -e[0].m[2][0], 1e-4f) && near(ry, -e[0].m[2][1], 1e-4f) && near(rz, -e[0].m[2][2], 1e-4f),
				"quaternion matches");
		}
		// 5. Head roll (left ear down): the right eye is higher, and a far point gets no vertical disparity along the
		// rolled baseline beyond the roll itself.
		{
			EyeSpec s;
			s.ipd_m = 0.064f;
			s.roll_rad = 0.3f;
			CameraTangents(cam, s.tan[0]);
			CameraTangents(cam, s.tan[1]);
			Eye e[2];
			Build(cam, s, e);
			check(e[1].pos_m[1] > 0.0f && e[0].pos_m[1] < 0.0f, "roll raises the right eye");
		}
		// 6. Behind the eye: refused. Degenerate input: refused.
		{
			EyeSpec s;
			s.ipd_m = 0.064f;
			CameraTangents(cam, s.tan[0]);
			CameraTangents(cam, s.tan[1]);
			Eye e[2];
			Build(cam, s, e);
			float x2, y2, w2;
			check(!MapVertex(cam, e[0], 2048.0f, 2048.0f, 0.2f, &x2, &y2, &w2), "behind the eye");
			EyeSpec bad = s;
			bad.tan[1][0] = bad.tan[1][1];
			check(!Build(cam, bad, e), "degenerate fov");
			GameCamera none;
			check(!Build(none, s, e), "no camera");
		}

		// [VR] GameText: the 2D layer at kGameTextScale. On a 90-100 deg tall view (tan 45-50 deg), TrueStereo's plane
		// (0.9 of the view times the scale) and the first-person screen (the whole view times the scale) put File #1's
		// "X Obtained" (rows 385-396 of 448) and a speech subtitle (324-339) between 8 and 26 deg below the line of
		// sight, where they were 30-40 deg below at the game's size; the centre stays put, the vertices stay on screen.
		{
			const float s = kGameTextScale;
			check(s >= 0.45f && s <= 0.7f, "game text: the 2D layer at about half size");
			bool placed = true, was_low = true;
			for (float tan_half : {std::tan(45.0f * 3.14159265f / 180.0f), std::tan(50.0f * 3.14159265f / 180.0f)})
			{
				for (float fraction : {0.9f, 1.0f})
				{
					for (float y : {324.0f, 339.0f, 385.0f, 396.0f})
					{
						const float now = GameTextDegreesBelow(y, 224.0f, 224.0f, fraction * s, tan_half);
						placed = placed && now >= 8.0f && now <= 26.0f;
					}
					was_low = was_low && GameTextDegreesBelow(390.0f, 224.0f, 224.0f, fraction, tan_half) > 30.0f;
				}
			}
			check(placed, "game text: Obtained and a subtitle 8-26 deg below the line of sight");
			check(was_low, "game text: at the game's size Obtained sat more than 30 deg below");
			check(GameTextMap(2048.0f, 2048.0f, s) == 2048.0f && GameTextMap(2272.0f, 2048.0f, s) < 2272.0f &&
					  GameTextMap(1824.0f, 2048.0f, s) > 1824.0f,
				"game text: the centre stays, edges move inward");
			check(GameTextMapFixed(2048 * 16, 2048.0f, s) == 2048 * 16 &&
					  GameTextMapFixed((2048 + 166) * 16, 2048.0f, s) == static_cast<unsigned short>(std::lround((2048.0f + 166.0f * s) * 16.0f)) &&
					  GameTextMapFixed(65535, 2048.0f, s) < 65535 && GameTextMapFixed(0, 2048.0f, s) > 0,
				"game text: 12.4 vertices map toward the centre and stay in range");
			check(GameTextMapFixed(1234, std::nanf(""), s) == 1234, "game text: no camera centre, the vertex is left alone");
		}

		if (fails.empty())
			Console.WriteLn("(VR) Eye projection self-test: passed (%d checks).", checks);
		else
			Console.Error("(VR) Eye projection self-test: FAILED (%s) of %d checks.", fails.c_str(), checks);
		return fails.empty();
	}
} // namespace VR::EyeProjection
