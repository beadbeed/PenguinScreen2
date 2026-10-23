// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/CutsceneView.h"

#include "common/Console.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace VR::CutsceneView
{
	namespace
	{
		// v' = q v q^-1 (q = x, y, z, w)
		void Rotate(const float q[4], const float v[3], float out[3])
		{
			const float t1x = q[1] * v[2] - q[2] * v[1], t1y = q[2] * v[0] - q[0] * v[2], t1z = q[0] * v[1] - q[1] * v[0];
			const float t2x = q[1] * t1z - q[2] * t1y, t2y = q[2] * t1x - q[0] * t1z, t2z = q[0] * t1y - q[1] * t1x;
			out[0] = v[0] + 2.0f * (q[3] * t1x + t2x);
			out[1] = v[1] + 2.0f * (q[3] * t1y + t2y);
			out[2] = v[2] + 2.0f * (q[3] * t1z + t2z);
		}
		bool Finite3(const float v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
	} // namespace

	void SoftClamp(const float v[3], float r, float out[3])
	{
		const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (!Finite3(v) || !(r > 0.0f))
		{
			out[0] = out[1] = out[2] = 0.0f;
			return;
		}
		const float k = (len > 1e-6f) ? (r * std::tanh(len / r)) / len : 1.0f;
		for (int i = 0; i < 3; i++)
			out[i] = v[i] * k;
	}

	void EyePositions(const float head_pos[3], const float head_quat[4], float ipd_m, float max_offset_m, float out[2][3])
	{
		float c[3];
		SoftClamp(head_pos, max_offset_m, c);
		float q[4] = {head_quat[0], head_quat[1], head_quat[2], head_quat[3]};
		const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
		if (!(n > 1e-6f) || !std::isfinite(n))
		{
			q[0] = q[1] = q[2] = 0.0f;
			q[3] = 1.0f;
		}
		else
		{
			for (float& x : q)
				x /= n;
		}
		const float ipd = (std::isfinite(ipd_m) && ipd_m > 0.0f && ipd_m < 0.2f) ? ipd_m : 0.064f;
		for (int e = 0; e < 2; e++)
		{
			const float half[3] = {(e == 0 ? -0.5f : 0.5f) * ipd, 0.0f, 0.0f};
			float o[3];
			Rotate(q, half, o);
			for (int i = 0; i < 3; i++)
				out[e][i] = c[i] + o[i];
		}
	}

	bool WindowTangents(const EyeProjection::GameCamera& cam, const float eye_m[3], float window_m, float tan[4])
	{
		float t0[4];
		EyeProjection::CameraTangents(cam, t0);
		const float d = window_m + eye_m[2]; // the eye's distance to the window plane (+z is behind the camera)
		if (!(window_m > 0.0f) || !Finite3(eye_m) || !(d > 0.05f * window_m) || !(t0[1] > t0[0]) || !(t0[2] > t0[3]))
			return false;
		tan[0] = (window_m * t0[0] - eye_m[0]) / d;
		tan[1] = (window_m * t0[1] - eye_m[0]) / d;
		tan[2] = (window_m * t0[2] - eye_m[1]) / d;
		tan[3] = (window_m * t0[3] - eye_m[1]) / d;
		return true;
	}

	bool BuildEyes(const EyeProjection::GameCamera& cam, const float eye_pos[2][3], float window_m, EyeProjection::Eye out[2])
	{
		EyeProjection::EyeSpec spec;
		spec.ipd_m = 0.0f;
		for (int e = 0; e < 2; e++)
		{
			if (!WindowTangents(cam, eye_pos[e], window_m, spec.tan[e]))
				return false;
		}
		if (!EyeProjection::Build(cam, spec, out))
			return false;
		for (int e = 0; e < 2; e++)
			EyeProjection::PlaceEye(cam, eye_pos[e], 0.0f, 0.0f, &out[e]);
		return true;
	}

	void HeadInAnchor(const float head_pos[3], const float head_quat[4], const float anchor_pos[3], float anchor_yaw,
		float out_pos[3], float out_quat[4])
	{
		const float s = std::sin(0.5f * anchor_yaw), c = std::cos(0.5f * anchor_yaw);
		const float inv[4] = {0.0f, -s, 0.0f, c}; // q(yaw)^-1
		const float d[3] = {head_pos[0] - anchor_pos[0], head_pos[1] - anchor_pos[1], head_pos[2] - anchor_pos[2]};
		Rotate(inv, d, out_pos);
		// inv * head
		const float* h = head_quat;
		out_quat[0] = inv[3] * h[0] + inv[0] * h[3] + inv[1] * h[2] - inv[2] * h[1];
		out_quat[1] = inv[3] * h[1] - inv[0] * h[2] + inv[1] * h[3] + inv[2] * h[0];
		out_quat[2] = inv[3] * h[2] + inv[0] * h[1] - inv[1] * h[0] + inv[2] * h[3];
		out_quat[3] = inv[3] * h[3] - inv[0] * h[0] - inv[1] * h[1] - inv[2] * h[2];
	}

	GateStep StepGate(WindowGate& g, bool want, bool black, bool can_blink, bool force, u64 now_ms, u32 rise_ms)
	{
		if (want == g.shown)
		{
			g.due_ms = 0;
			return GateStep::Hold;
		}
		if (force || black || !can_blink)
		{
			Flip(g, want);
			return GateStep::Flipped;
		}
		if (g.due_ms == 0)
		{
			g.due_ms = std::max<u64>(1, now_ms + rise_ms);
			return GateStep::StartBlink;
		}
		if (now_ms >= g.due_ms)
		{
			Flip(g, want);
			return GateStep::Flipped;
		}
		return GateStep::Hold;
	}

	void Flip(WindowGate& g, bool want)
	{
		g.shown = want;
		g.due_ms = 0;
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

		// A cutscene camera at File #1's default cutscene FOV class (fx 527.5 at FOV 40; the 512x448 target).
		EyeProjection::GameCamera cam;
		cam.fx = 527.5f;
		cam.fy = -615.0f;
		const float id[4] = {0.0f, 0.0f, 0.0f, 1.0f};

		// 1. The head where it began: the eyes half the IPD either side, level; their windows are the camera's, shifted.
		{
			const float zero[3] = {0.0f, 0.0f, 0.0f};
			float p[2][3];
			EyePositions(zero, id, 0.064f, kMaxOffsetM, p);
			check(near(p[0][0], -0.032f, 1e-6f) && near(p[1][0], 0.032f, 1e-6f) && near(p[0][1], 0.0f, 1e-6f) &&
					  near(p[1][2], 0.0f, 1e-6f),
				"eyes at rest");
			float t0[4], tl[4];
			EyeProjection::CameraTangents(cam, t0);
			check(WindowTangents(cam, zero, kWindowM, tl) && near(tl[0], t0[0], 1e-6f) && near(tl[3], t0[3], 1e-6f),
				"the camera's own window");
			check(WindowTangents(cam, p[1], kWindowM, tl) && near(tl[1] - tl[0], t0[1] - t0[0], 1e-5f) && tl[0] < t0[0],
				"the right eye's window: same width, shifted left");
		}
		// 2. A point on the window plane lands on the same pixel for every eye (the window has no disparity); a far point
		// moves with the eye (parallax), a near one against it.
		{
			const float pos[2][3] = {{-0.032f, 0.0f, 0.0f}, {0.11f, -0.05f, 0.08f}};
			EyeProjection::Eye e[2];
			check(BuildEyes(cam, pos, kWindowM, e), "build");
			const float w_plane = kWindowM * cam.units_per_m;
			const float X = 2048.0f + 90.0f, Y = 2048.0f - 60.0f;
			for (int i = 0; i < 2; i++)
			{
				float x2, y2, w2;
				const bool ok = EyeProjection::MapVertex(cam, e[i], X, Y, w_plane, &x2, &y2, &w2);
				check(ok && near(x2, X, 0.02f) && near(y2, Y, 0.02f), "window plane: no disparity");
			}
			// The window's corners are the target's corners for each eye.
			float x2, y2, w2;
			EyeProjection::MapVertex(cam, e[1], 2048.0f - 256.0f, 2048.0f - 224.0f, w_plane, &x2, &y2, &w2);
			check(near(x2, 2048.0f - 256.0f, 0.05f) && near(y2, 2048.0f - 224.0f, 0.05f), "window corner");
			// Eyes level, 6.4 cm apart. A point straight ahead 3 m away crosses the window at x_e (1 - D / z) for an eye at
			// x_e: left of centre for the left eye, right for the right one (uncrossed), fx ipd (1 - D / z) / D pixels apart
			// (D and z in metres here, the window's pixels being fx per unit tangent); a point nearer than the window crosses.
			const float pl[2][3] = {{-0.032f, 0.0f, 0.0f}, {0.032f, 0.0f, 0.0f}};
			BuildEyes(cam, pl, kWindowM, e);
			float xl, xr, yl, yr, wl, wr;
			EyeProjection::MapVertex(cam, e[0], 2048.0f, 2048.0f, 300.0f, &xl, &yl, &wl);
			EyeProjection::MapVertex(cam, e[1], 2048.0f, 2048.0f, 300.0f, &xr, &yr, &wr);
			check(xr > xl && near(yl, yr, 1e-3f), "far point: uncrossed disparity, no vertical");
			check(near(xr - xl, cam.fx * 0.064f * (1.0f - kWindowM / 3.0f) / kWindowM, 0.05f), "far point: disparity size");
			EyeProjection::MapVertex(cam, e[0], 2048.0f, 2048.0f, 50.0f, &xl, &yl, &wl);
			EyeProjection::MapVertex(cam, e[1], 2048.0f, 2048.0f, 50.0f, &xr, &yr, &wr);
			check(xl > xr, "near point: crossed disparity (out of the window)");
		}
		// 3. The soft clamp: one-to-one for small moves, never past the radius.
		{
			const float small[3] = {0.01f, 0.0f, 0.0f};
			const float big[3] = {0.0f, 3.0f, 4.0f};
			float o[3];
			SoftClamp(small, kMaxOffsetM, o);
			check(near(o[0], 0.01f, 1e-4f), "soft clamp: small moves kept");
			SoftClamp(big, kMaxOffsetM, o);
			const float len = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
			check(len <= kMaxOffsetM + 1e-5f && len > 0.99f * kMaxOffsetM && near(o[1] / o[2], 0.75f, 1e-4f),
				"soft clamp: bounded, same direction");
			const float bad[3] = {NAN, 0.0f, 0.0f};
			SoftClamp(bad, kMaxOffsetM, o);
			check(o[0] == 0.0f && o[1] == 0.0f && o[2] == 0.0f, "soft clamp: not finite -> centre");
		}
		// 4. A head turned 90 degrees left: the eyes lie along the view axis, both still in front of the window.
		{
			const float zero[3] = {0.0f, 0.0f, 0.0f};
			const float q[4] = {0.0f, std::sin(0.25f * 3.14159265f), 0.0f, std::cos(0.25f * 3.14159265f)};
			float p[2][3];
			EyePositions(zero, q, 0.064f, kMaxOffsetM, p);
			check(near(p[0][0], 0.0f, 1e-5f) && near(p[0][2], 0.032f, 1e-5f) && near(p[1][2], -0.032f, 1e-5f),
				"turned head: eyes along the view axis");
			EyeProjection::Eye e[2];
			check(BuildEyes(cam, p, kWindowM, e), "turned head: builds");
		}
		// 5. The anchor frame: a head 20 cm to the anchor's right, the anchor turned 90 degrees left (facing -x).
		{
			const float yaw = 0.5f * 3.14159265f;
			const float anchor[3] = {1.0f, 1.2f, -2.0f};
			// Facing -x, its right is -z... right of a yaw-left-90 frame: R(yaw) (1,0,0) = (cos, 0, -sin) = (0, 0, -1).
			const float head[3] = {1.0f, 1.2f, -2.2f};
			const float hq[4] = {0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw)};
			float p[3], q[4];
			HeadInAnchor(head, hq, anchor, yaw, p, q);
			check(near(p[0], 0.2f, 1e-5f) && near(p[1], 0.0f, 1e-5f) && near(p[2], 0.0f, 1e-5f), "anchor: offset");
			check(near(std::abs(q[3]), 1.0f, 1e-5f), "anchor: facing it is no turn");
		}
		// 6. Degenerate: an eye at the window plane, no camera.
		{
			const float at[2][3] = {{0.0f, 0.0f, -kWindowM}, {0.0f, 0.0f, 0.0f}};
			EyeProjection::Eye e[2];
			check(!BuildEyes(cam, at, kWindowM, e), "eye at the window: refused");
			EyeProjection::GameCamera none;
			const float pl[2][3] = {{-0.032f, 0.0f, 0.0f}, {0.032f, 0.0f, 0.0f}};
			check(!BuildEyes(none, pl, kWindowM, e), "no camera: refused");
		}

		// 7. The window's edges under a blink (evening review m8).
		{
			WindowGate g;
			check(StepGate(g, false, false, true, false, 1000, 40) == GateStep::Hold && !g.shown, "gate: nothing wanted");
			check(StepGate(g, true, false, true, false, 1000, 40) == GateStep::StartBlink && !g.shown && g.due_ms == 1040,
				"gate: up: a blink first, the world screen until it has risen");
			check(StepGate(g, true, false, true, false, 1020, 40) == GateStep::Hold && !g.shown, "gate: up: still rising");
			check(StepGate(g, true, false, true, false, 1040, 40) == GateStep::Flipped && g.shown && g.due_ms == 0,
				"gate: up once the blink is black");
			check(StepGate(g, true, false, true, false, 1100, 40) == GateStep::Hold && g.shown, "gate: held up");
			check(StepGate(g, false, false, true, false, 2000, 40) == GateStep::StartBlink && g.shown, "gate: down: a blink first");
			check(StepGate(g, true, false, true, false, 2010, 40) == GateStep::Hold && g.shown && g.due_ms == 0,
				"gate: wanted back before the flip: nothing changes");
			check(StepGate(g, false, true, true, false, 2100, 40) == GateStep::Flipped && !g.shown,
				"gate: the view already black (the mode switch's blink): at once");
			check(StepGate(g, true, false, false, false, 2200, 40) == GateStep::Flipped && g.shown, "gate: no blink can be drawn: at once");
			check(StepGate(g, false, false, true, true, 2300, 40) == GateStep::Flipped && !g.shown, "gate: forced (first person): at once");
			Flip(g, true);
			check(g.shown && g.due_ms == 0, "gate: a blink that could not start flips at once");
		}

		if (fails.empty())
			Console.WriteLn("(VR) Cutscene view self-test: %d checks passed.", checks);
		else
			Console.Error("(VR) Cutscene view self-test: FAILED: %s (%d checks).", fails.c_str(), checks);
		return fails.empty();
	}
} // namespace VR::CutsceneView
