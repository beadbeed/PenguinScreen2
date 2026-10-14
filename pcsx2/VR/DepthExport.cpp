// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/DepthExport.h"
#include "VR/CameraDriver.h"
#include "VR/StereoState.h"

#include "Config.h"
#include "VMManager.h"

#ifdef ENABLE_VULKAN
#include "GS/GS.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/GSTextureVK.h"
#endif

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace VR::DepthExport
{
	namespace
	{
		bool FitUsable(const ZFit& fit, float units_per_meter)
		{
			return fit.valid && std::isfinite(fit.a0) && std::isfinite(fit.beta) && fit.beta != 0.0f &&
			       std::isfinite(units_per_meter) && units_per_meter > 0.0f;
		}

		// q * v for a unit quaternion q (x, y, z, w).
		XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v)
		{
			// v + 2 w (u x v) + 2 u x (u x v), u = (x, y, z)
			const float ux = q.x, uy = q.y, uz = q.z, w = q.w;
			const float tx = 2.0f * (uy * v.z - uz * v.y);
			const float ty = 2.0f * (uz * v.x - ux * v.z);
			const float tz = 2.0f * (ux * v.y - uy * v.x);
			return {v.x + w * tx + (uy * tz - uz * ty), v.y + w * ty + (uz * tx - ux * tz), v.z + w * tz + (ux * ty - uy * tx)};
		}

		XrVector3f RotateInverse(const XrQuaternionf& q, const XrVector3f& v)
		{
			return Rotate(XrQuaternionf{-q.x, -q.y, -q.z, q.w}, v);
		}
	}

	double LinearDepthM(const ZFit& fit, float units_per_meter, double z_norm)
	{
		if (!FitUsable(fit, units_per_meter) || !std::isfinite(z_norm))
			return 0.0;
		// q = 1/w (w the view depth in game units) = beta * (z - a0).
		const double q = static_cast<double>(fit.beta) * (z_norm - static_cast<double>(fit.a0));
		if (!(q > 0.0))
			return 0.0;
		return 1.0 / (static_cast<double>(units_per_meter) * q);
	}

	double DepthValueFor(const ZFit& fit, float units_per_meter, double metres)
	{
		if (!FitUsable(fit, units_per_meter) || !(metres > 0.0))
			return std::numeric_limits<double>::quiet_NaN();
		return static_cast<double>(fit.a0) +
		       1.0 / (static_cast<double>(fit.beta) * static_cast<double>(units_per_meter) * metres);
	}

	XrDepthRange RangeForRawDepth(const ZFit& fit, float units_per_meter, double far_m, double near_m)
	{
		XrDepthRange r;
		if (!FitUsable(fit, units_per_meter) || !(far_m > near_m) || !(near_m > 0.0))
			return r;
		// The values at the far and the near distance; the range is their span within [0, 1], and each end's distance is
		// read back from its value (exact for the ends that were clamped to 0 or 1 too).
		const double v_far = std::clamp(DepthValueFor(fit, units_per_meter, far_m), 0.0, 1.0);
		const double v_near = std::clamp(DepthValueFor(fit, units_per_meter, near_m), 0.0, 1.0);
		const double lo = std::min(v_far, v_near);
		const double hi = std::max(v_far, v_near);
		const double d_lo = LinearDepthM(fit, units_per_meter, lo);
		const double d_hi = LinearDepthM(fit, units_per_meter, hi);
		// Both ends must be depths in front of the camera, and the float range must not collapse.
		if (!(d_lo > 0.0) || !(d_hi > 0.0) || !(static_cast<float>(hi) > static_cast<float>(lo)))
			return r;
		r.min_depth = static_cast<float>(lo);
		r.max_depth = static_cast<float>(hi);
		// The distances of the values as stored (floats), so the runtime's line through the ends is the fit's.
		r.near_z = static_cast<float>(LinearDepthM(fit, units_per_meter, r.min_depth));
		r.far_z = static_cast<float>(LinearDepthM(fit, units_per_meter, r.max_depth));
		r.valid = (r.near_z > 0.0f) && (r.far_z > 0.0f) && std::isfinite(r.near_z) && std::isfinite(r.far_z);
		return r;
	}

	double XrDistanceAt(const XrDepthRange& range, double value)
	{
		if (!range.valid || !(range.max_depth > range.min_depth))
			return 0.0;
		const double t = std::clamp((value - static_cast<double>(range.min_depth)) /
		                                (static_cast<double>(range.max_depth) - static_cast<double>(range.min_depth)),
			0.0, 1.0);
		const double inv_n = 1.0 / static_cast<double>(range.near_z);
		const double inv_f = 1.0 / static_cast<double>(range.far_z);
		const double inv = inv_n + t * (inv_f - inv_n);
		return (inv > 0.0) ? (1.0 / inv) : 0.0;
	}

	XrVector3f EyeOffset(const XrQuaternionf& head, float x)
	{
		return Rotate(head, XrVector3f{x, 0.0f, 0.0f});
	}

	bool QuadViewForEye(const XrPosef& quad, float width, float height, const XrVector3f& head, const XrVector3f& eye_offset,
		XrPosef* view_pose, XrFovf* view_fov)
	{
		const XrQuaternionf& r = quad.orientation;
		const XrVector3f eye = {head.x + eye_offset.x, head.y + eye_offset.y, head.z + eye_offset.z};
		// The quad's centre seen from the eye, in the quad's own frame (it faces +Z, so it lies ahead along -Z).
		const XrVector3f c = RotateInverse(r, XrVector3f{quad.position.x - eye.x, quad.position.y - eye.y, quad.position.z - eye.z});
		const float dist = -c.z;
		if (!(dist > 1.0e-4f) || !(width > 0.0f) || !(height > 0.0f))
			return false;
		const float cx = c.x;
		const float cy = c.y;
		view_pose->orientation = r;
		view_pose->position = eye;
		view_fov->angleLeft = std::atan((cx - 0.5f * width) / dist);
		view_fov->angleRight = std::atan((cx + 0.5f * width) / dist);
		view_fov->angleUp = std::atan((cy + 0.5f * height) / dist);
		view_fov->angleDown = std::atan((cy - 0.5f * height) / dist);
		return true;
	}

	TrackAction DepthTracker::NoteDraw(const DrawNote& d)
	{
		snapshot_label = 0;
		const bool uses_depth = d.z_test || d.z_write;
		// A draw into the depth buffer's own memory as its colour target: the game clearing (or reusing) it.
		const bool writes_depth_memory = live && (d.fbp == live_zbp);
		if (live && (writes_depth_memory || (uses_depth && d.zbp == live_zbp && d.fbp != live_fbp)))
		{
			// The live frame's depth is finished: snapshot it before this draw changes it.
			snapshot_label = live_fbp;
			live = false;
			return TrackAction::Snapshot;
		}
		if (d.z_test && d.fbp != d.zbp)
		{
			if (!live || d.fbp != live_fbp || d.zbp != live_zbp)
				live_draws = 0;
			live = true;
			live_fbp = d.fbp;
			live_zbp = d.zbp;
			live_draws++;
		}
		return TrackAction::None;
	}

	void DepthTracker::SnapshotTaken(bool ok)
	{
		snap_valid = ok;
		if (ok)
		{
			snap_fbp = snapshot_label;
			snap_zbp = live_zbp;
		}
		snapshot_label = 0;
	}

	PresentSource SourceForPresent(const DepthTracker& t, u32 displayed_fbp, u32 feeder_fbp)
	{
		const auto ours = [&](u32 fbp) { return fbp == displayed_fbp || (feeder_fbp != 0 && fbp == feeder_fbp); };
		// The latest depth first: a frame still being the newest drawn has its depth live in the target.
		if (t.live && t.live_draws > 0 && ours(t.live_fbp))
			return PresentSource::Live;
		if (t.snap_valid && ours(t.snap_fbp))
			return PresentSource::Snapshot;
		return PresentSource::None;
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
				Console.Error("(VR) Depth export self-test FAIL: %s", name);
			}
		};
		const auto rel_near = [](double a, double b, double tol) { return std::abs(a - b) <= tol * std::max(std::abs(b), 1e-30); };

		// --- The conversion --------------------------------------------------------------------------------------------
		// File #1's fit as GSRendererHW logs it ("slope 5.960436e-08, A -335.2": q = slope * (z - A) over the integer z;
		// a0 = A / 2^32, beta = slope * 2^32) and its 100 units a metre.
		ZFit fit;
		fit.valid = true;
		fit.a0 = static_cast<float>(-335.2 / 4294967296.0);
		fit.beta = static_cast<float>(5.960436e-08 * 4294967296.0);
		const float upm = 100.0f;
		{
			// By hand: the integer z 20953 (a J's Bar draw, q 0.00127 in the trace) is q = 5.960436e-8 * (20953 + 335.2)
			// = 0.00126887, w = 788.10 units = 7.881 m.
			const double z = 20953.0 / 4294967296.0;
			check(rel_near(LinearDepthM(fit, upm, z), 7.8810, 1e-4), "z 20953 reads 7.881 m, by hand");
			check(rel_near(LinearDepthM(fit, upm, static_cast<double>(fit.a0) + 1.0 / (static_cast<double>(fit.beta) * 100.0 * 1.05)),
					  1.05, 1e-9),
				"the value a0 + 1 / (beta * 100 * 1.05) reads 1.05 m");
			bool round_trip = true;
			for (double m : {0.05, 0.3, 0.5, 1.0, 1.04, 2.0, 7.5, 40.0, 499.0})
				round_trip = round_trip && rel_near(LinearDepthM(fit, upm, DepthValueFor(fit, upm, m)), m, 1e-9);
			check(round_trip, "LinearDepthM undoes DepthValueFor from 5 cm to 499 m");
			check(LinearDepthM(fit, upm, fit.a0) == 0.0 && LinearDepthM(fit, upm, fit.a0 - 1e-9) == 0.0,
				"the value of infinity (a0), and beyond it, is no depth");
			check(rel_near(LinearDepthM(fit, upm, 0.0), 500.2, 1e-3), "the cleared depth (0) reads the game's far plane, 500 m");
			// q is 1/w in game units: twice the units a metre halves the metres.
			check(rel_near(LinearDepthM(fit, 200.0f, z), 7.8810 / 2.0, 1e-4), "the metres scale with unitsPerMeter");
			// The fit's own form: w (units) = 1 / q.
			const double q = static_cast<double>(fit.beta) * (z - static_cast<double>(fit.a0));
			check(rel_near(LinearDepthM(fit, upm, z) * upm, 1.0 / q, 1e-12), "metres x units a metre = 1 / q");
			ZFit none;
			check(LinearDepthM(none, upm, z) == 0.0 && std::isnan(DepthValueFor(none, upm, 1.0)), "no fit, no depth");
			check(LinearDepthM(fit, 0.0f, z) == 0.0, "no scale, no depth");
		}

		// --- The range the raw depth is described with -----------------------------------------------------------------
		{
			const XrDepthRange r = RangeForRawDepth(fit, upm);
			check(r.valid && r.min_depth < r.max_depth && r.min_depth >= 0.0f && r.max_depth <= 1.0f, "a valid range in [0, 1]");
			check(r.near_z > r.far_z, "beta > 0: nearZ (at minDepth) is the far end, the reversed order");
			check(rel_near(r.near_z, kRangeFarM, 1e-3) && rel_near(r.far_z, kRangeNearM, 1e-3), "it spans 500 m to 5 cm");
			// Exact: the runtime's reading of any stored value is the fit's depth.
			bool exact = true;
			double worst = 0.0;
			for (double m : {0.06, 0.3, 0.5, 0.75, 1.0, 1.04, 1.5, 3.0, 10.0, 60.0, 400.0})
			{
				const float stored = static_cast<float>(DepthValueFor(fit, upm, m)); // as the depth target holds it
				const double ours = LinearDepthM(fit, upm, stored);
				const double theirs = XrDistanceAt(r, stored);
				worst = std::max(worst, std::abs(theirs - ours) / ours);
				exact = exact && rel_near(theirs, ours, 1e-5);
			}
			check(exact, "the runtime's reading of a stored value is LinearDepthM's, 6 cm to 400 m (within 1e-5)");
			check(XrDistanceAt(r, r.min_depth) == static_cast<double>(r.near_z) || rel_near(XrDistanceAt(r, r.min_depth), r.near_z, 1e-12),
				"minDepth reads nearZ");
			check(rel_near(XrDistanceAt(r, 0.0), r.near_z, 1e-9), "the cleared background (0) reads the far end, 500 m");
			// A fit whose z falls as things come nearer (beta < 0): the order flips and it is still exact.
			ZFit neg;
			neg.valid = true;
			neg.a0 = 2.0e-3f;
			neg.beta = -1.0e3f;
			const XrDepthRange rn = RangeForRawDepth(neg, upm);
			const float s1 = static_cast<float>(DepthValueFor(neg, upm, 1.04));
			check(rn.valid && rn.near_z < rn.far_z && rel_near(XrDistanceAt(rn, s1), LinearDepthM(neg, upm, s1), 1e-5),
				"a falling z: nearZ < farZ, still exact");
			check(!RangeForRawDepth(ZFit{}, upm).valid && XrDistanceAt(XrDepthRange{}, 0.5) == 0.0, "no fit, no range");
			if (!exact)
				Console.Error("(VR) Depth export self-test: worst relative error %.3g.", worst);
		}

		// --- The projection view that shows what the quad shows ----------------------------------------------------------
		{
			// A quad 1.5 m ahead of a head at (0.2, 1.6, -0.4), turned 30 deg left and tipped 10 deg down, 3.2 x 2.4 m.
			const float yaw = 0.5236f, pitch = -0.1745f;
			const float sy = std::sin(0.5f * yaw), cy = std::cos(0.5f * yaw), sp = std::sin(0.5f * pitch), cp = std::cos(0.5f * pitch);
			XrPosef quad;
			quad.orientation = {cy * sp, sy * cp, -sy * sp, cy * cp}; // yaw then pitch, as the first-person placement builds it
			const XrVector3f head = {0.2f, 1.6f, -0.4f};
			const XrVector3f fwd = Rotate(quad.orientation, XrVector3f{0.0f, 0.0f, -1.0f});
			const float d = 1.5f;
			quad.position = {head.x + d * fwd.x, head.y + d * fwd.y, head.z + d * fwd.z};
			const float w = 3.2f, h = 2.4f;
			bool same = true;
			// The eyes of a head turned like the quad, and of one rolled 30 deg and 180 deg about its forward axis (the
			// eyes then sit above and below, or swapped: SteamVR's null headset is rolled 180 deg).
			const XrVector3f roll_axis = fwd;
			const auto rolled = [&](float deg) {
				const float a = 0.5f * deg * 3.14159265f / 180.0f;
				const XrQuaternionf rq = {roll_axis.x * std::sin(a), roll_axis.y * std::sin(a), roll_axis.z * std::sin(a), std::cos(a)};
				// rq * quad.orientation
				const XrQuaternionf& b = quad.orientation;
				return XrQuaternionf{rq.w * b.x + rq.x * b.w + rq.y * b.z - rq.z * b.y, rq.w * b.y - rq.x * b.z + rq.y * b.w + rq.z * b.x,
					rq.w * b.z + rq.x * b.y - rq.y * b.x + rq.z * b.w, rq.w * b.w - rq.x * b.x - rq.y * b.y - rq.z * b.z};
			};
			const XrVector3f eyes[] = {EyeOffset(quad.orientation, -0.032f), XrVector3f{0.0f, 0.0f, 0.0f},
				EyeOffset(quad.orientation, 0.032f), EyeOffset(rolled(30.0f), -0.032f), EyeOffset(rolled(180.0f), -0.032f)};
			for (const XrVector3f& eye_off : eyes)
			{
				XrPosef vp;
				XrFovf fov;
				if (!QuadViewForEye(quad, w, h, head, eye_off, &vp, &fov))
				{
					same = false;
					continue;
				}
				const float tl = std::tan(fov.angleLeft), tr = std::tan(fov.angleRight);
				const float tu = std::tan(fov.angleUp), td = std::tan(fov.angleDown);
				for (float u : {0.0f, 0.25f, 0.5f, 0.9f, 1.0f})
				{
					for (float v : {0.0f, 0.4f, 1.0f})
					{
						// The quad's point at image (u, v) (v down), and the direction to it from the view.
						const XrVector3f local = {(u - 0.5f) * w, (0.5f - v) * h, 0.0f};
						const XrVector3f off = Rotate(quad.orientation, local);
						const XrVector3f p = {quad.position.x + off.x - vp.position.x, quad.position.y + off.y - vp.position.y,
							quad.position.z + off.z - vp.position.z};
						const XrVector3f pv = RotateInverse(vp.orientation, p);
						// A projection view maps image u linearly in tangent space from angleLeft to angleRight, v from up to down.
						const float tx = pv.x / -pv.z, ty = pv.y / -pv.z;
						same = same && std::abs(tx - (tl + u * (tr - tl))) < 1e-4f && std::abs(ty - (tu + v * (td - tu))) < 1e-4f;
					}
				}
			}
			check(same, "each image pixel lands where the quad put it: left, centre and right eye, a 30 and a 180 deg roll");
			XrPosef vp;
			XrFovf fov;
			const XrVector3f left = EyeOffset(quad.orientation, -0.032f);
			QuadViewForEye(quad, w, h, head, left, &vp, &fov);
			check(fov.angleRight > -fov.angleLeft, "the left eye sees more of the quad to its right");
			check(std::abs(vp.position.x - (head.x + left.x)) < 1e-6f && std::abs(vp.position.y - (head.y + left.y)) < 1e-6f &&
					  std::abs(vp.position.z - (head.z + left.z)) < 1e-6f,
				"the view sits at the eye");
			check(std::abs(fov.angleUp - std::atan(1.2f / 1.5f)) < 1e-5f && std::abs(fov.angleDown + std::atan(1.2f / 1.5f)) < 1e-5f,
				"the vertical edges at atan(h/2 / d)");
			XrFovf upside;
			QuadViewForEye(quad, w, h, head, EyeOffset(rolled(180.0f), -0.032f), &vp, &upside);
			check(std::abs(upside.angleLeft + fov.angleRight) < 1e-5f && std::abs(upside.angleRight + fov.angleLeft) < 1e-5f,
				"rolled 180 deg, the left eye sits where the right one did (the null headset's case)");
			XrPosef behind = quad;
			behind.position = {head.x - d * fwd.x, head.y - d * fwd.y, head.z - d * fwd.z};
			check(!QuadViewForEye(behind, w, h, head, XrVector3f{0.0f, 0.0f, 0.0f}, &vp, &fov), "a quad behind the head has no view");
		}

		// --- Which depth the displayed frame takes ---------------------------------------------------------------------
		{
			// Double buffering, one depth buffer: frame A into 0x0 with depth 0x2300, then frame B into 0x1180 with it.
			DepthTracker t;
			const DrawNote a3d{0x0000, 0x2300, true, true};
			const DrawNote hud{0x0000, 0x2300, false, false};
			const DrawNote clear{0x2300, 0x2300, false, false}; // a sprite into the depth buffer's memory
			const DrawNote b3d{0x1180, 0x2300, true, true};
			bool ok = t.NoteDraw(a3d) == TrackAction::None && t.NoteDraw(a3d) == TrackAction::None &&
			          t.NoteDraw(hud) == TrackAction::None;
			check(ok && SourceForPresent(t, 0x0000, 0) == PresentSource::Live, "A presented while it is the latest: its depth is live");
			check(SourceForPresent(t, 0x1180, 0) == PresentSource::None, "B before it was drawn: no depth");
			const TrackAction c = t.NoteDraw(clear);
			check(c == TrackAction::Snapshot && t.snapshot_label == 0x0000, "the clear of the depth buffer snapshots A first");
			t.SnapshotTaken(true);
			check(SourceForPresent(t, 0x0000, 0) == PresentSource::Snapshot, "A presented after the clear: the snapshot");
			ok = t.NoteDraw(b3d) == TrackAction::None;
			check(ok && SourceForPresent(t, 0x0000, 0) == PresentSource::Snapshot && SourceForPresent(t, 0x1180, 0) == PresentSource::Live,
				"B drawing: A still has its snapshot, B is live");
			// No clear: B's first depth-tested draw ends A's run directly.
			DepthTracker u;
			u.NoteDraw(a3d);
			check(u.NoteDraw(b3d) == TrackAction::Snapshot && u.snapshot_label == 0x0000, "a draw for another frame snapshots A");
			u.SnapshotTaken(true);
			u.NoteDraw(b3d);
			check(SourceForPresent(u, 0x0000, 0) == PresentSource::Snapshot, "A then takes its snapshot");
			u.SnapshotTaken(false);
			check(SourceForPresent(u, 0x0000, 0) == PresentSource::None, "a snapshot that failed is not used");
			// Drawn into 0x3000 and copied to the displayed 0x0: the feeder's depth.
			DepthTracker f;
			f.NoteDraw(DrawNote{0x3000, 0x2300, true, true});
			check(SourceForPresent(f, 0x0000, 0x3000) == PresentSource::Live && SourceForPresent(f, 0x0000, 0) == PresentSource::None,
				"a frame copied from the target its 3D went into takes that target's depth");
			// 2D only (a menu, no depth test): nothing.
			DepthTracker m;
			m.NoteDraw(hud);
			check(SourceForPresent(m, 0x0000, 0) == PresentSource::None, "no depth-tested draw, no depth");
		}

		if (failed == 0)
			Console.WriteLn("(VR) Depth export self-test: %d checks passed.", checks);
		else
			Console.Error("(VR) Depth export self-test: %d of %d checks FAILED.", failed, checks);
		return failed == 0;
	}

	// =================================================================================================================
	// The export (GS thread)
	// =================================================================================================================
	namespace
	{
		std::atomic_bool s_compositor_wants{false};
		DepthTracker s_tracker;
		ExportInfo s_latest;
		u64 s_serial = 0;
		const char* s_noexport_logged = nullptr;
		bool s_first_logged = false;

		struct ProbeConfig
		{
			bool enabled = false;
			float every_s = 2.0f;
			bool dump = false;
			u32 dump_skip = 0;  // readbacks not dumped first
			u32 max_dumps = 2;  // then this many dumped
			// Points in the render camera's frame, metres (x right, y up, -z ahead), whose depth is read.
			std::vector<XrVector3f> points;
		};

		const ProbeConfig& Probe()
		{
			static const ProbeConfig cfg = [] {
				ProbeConfig c;
				const char* env = std::getenv("PCSX2_VR_DEPTHPROBE");
				if (!env)
					return c;
				c.enabled = true;
				const float every = static_cast<float>(std::atof(env));
				if (every >= 0.2f && every <= 60.0f)
					c.every_s = every;
				if (const char* dump = std::getenv("PCSX2_VR_DEPTHPROBE_DUMP"))
				{
					// "" or "1": the first two readbacks; "skip,count": count of them after the first skip.
					c.dump = true;
					unsigned skip = 0, count = 0;
					if (std::sscanf(dump, "%u,%u", &skip, &count) == 2 && count > 0 && count <= 20)
					{
						c.dump_skip = skip;
						c.max_dumps = count;
					}
				}
				if (const char* at = std::getenv("PCSX2_VR_DEPTHPROBE_AT"))
				{
					// "x,y,z;x,y,z": metres from the render camera.
					const std::string list(at);
					size_t pos = 0;
					while (pos < list.size())
					{
						const size_t end = std::min(list.find(';', pos), list.size());
						float x = 0.0f, y = 0.0f, z = 0.0f;
						if (std::sscanf(list.substr(pos, end - pos).c_str(), "%f,%f,%f", &x, &y, &z) == 3 && z < 0.0f)
							c.points.push_back({x, y, z});
						pos = end + 1;
					}
				}
				Console.WriteLn("(VR) Depth probe: on (PCSX2_VR_DEPTHPROBE, offline only): every %.1f s the exported depth is "
								"read back and logged in metres at a grid and %zu chosen point(s)%s.",
					c.every_s, c.points.size(), c.dump ? "; some are dumped to depthprobe/ (PCSX2_VR_DEPTHPROBE_DUMP)" : "");
				return c;
			}();
			return cfg;
		}

		u64 NowMs()
		{
			return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		// PFM (portable float map, metres, rows bottom to top) of one layer, for a look at the whole depth offline.
		void DumpLayer(const std::string& path, const float* metres, u32 w, u32 h)
		{
			std::FILE* f = FileSystem::OpenCFile(path.c_str(), "wb");
			if (!f)
				return;
			std::fprintf(f, "Pf\n%u %u\n-1.0\n", w, h);
			for (u32 y = 0; y < h; y++)
				std::fwrite(metres + static_cast<size_t>(h - 1 - y) * w, sizeof(float), w, f);
			std::fclose(f);
		}

#ifdef ENABLE_VULKAN
		struct VkBuf
		{
			GSDeviceVK* device = nullptr;
			VmaAllocator allocator = VK_NULL_HANDLE;
			VkBuffer buffer = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE;
			void* mapped = nullptr;
			VkDeviceSize size = 0;
		};
		// The export buffer (device-local; the depth layer's copy reads it) and the probe's readback (host-visible).
		VkBuf s_export;
		VkBuf s_readback;
		// The snapshot: a depth texture like the depth target, holding the finished frame's depth.
		GSTexture* s_snap = nullptr;
		GSDeviceVK* s_snap_device = nullptr;

		// The probe's pending readback: the export it copies, and the GS fence it waits for (never waited on).
		bool s_probe_pending = false;
		u64 s_probe_fence = 0;
		ExportInfo s_probe_info;
		u64 s_probe_next_ms = 0;
		u32 s_probe_dumps = 0;
		u32 s_probe_reads = 0; // readbacks logged
		// The shown colour image's layer 0 went into the readback after the depth (a dump due), and whether it is BGRA.
		bool s_probe_colour = false;
		bool s_probe_colour_bgra = false;
		bool s_probe_online_logged = false;
		bool s_buffer_failed_logged = false;

		void Free(VkBuf& b)
		{
			GSDeviceVK* const dev = GSDeviceVK::GetInstance();
			if (b.buffer != VK_NULL_HANDLE && dev && dev == b.device && dev->GetAllocator() == b.allocator)
				dev->DeferBufferDestruction(b.buffer, b.allocation);
			b = VkBuf{};
		}

		bool EnsureBuffer(GSDeviceVK* dev, VkBuf& b, VkDeviceSize size, bool host)
		{
			// A restarted renderer is a new device: the old buffer died with its allocator.
			if (b.buffer != VK_NULL_HANDLE && (b.device != dev || b.allocator != dev->GetAllocator()))
				b = VkBuf{};
			if (b.buffer != VK_NULL_HANDLE && b.size == size)
				return true;
			Free(b);
			VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
			bci.size = size;
			bci.usage = host ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
			bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo aci = {};
			if (host)
			{
				aci.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
				aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
				aci.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
			}
			else
			{
				aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
			}
			VmaAllocationInfo ai = {};
			VkBuffer buffer = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE;
			const VkResult res = vmaCreateBuffer(dev->GetAllocator(), &bci, &aci, &buffer, &allocation, &ai);
			if (res != VK_SUCCESS || (host && !ai.pMappedData))
			{
				if (res == VK_SUCCESS)
					dev->DeferBufferDestruction(buffer, allocation);
				if (!s_buffer_failed_logged)
				{
					s_buffer_failed_logged = true;
					Console.Warning("(VR) Depth export: could not create a %llu-byte buffer (VkResult %d); no depth.",
						static_cast<unsigned long long>(size), static_cast<int>(res));
				}
				return false;
			}
			b.device = dev;
			b.allocator = dev->GetAllocator();
			b.buffer = buffer;
			b.allocation = allocation;
			b.mapped = ai.pMappedData;
			b.size = size;
			return true;
		}

		void BufferBarrier(VkCommandBuffer cmd, VkBuffer buffer, VkAccessFlags src_access, VkAccessFlags dst_access,
			VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
		{
			VkBufferMemoryBarrier b = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
			b.srcAccessMask = src_access;
			b.dstAccessMask = dst_access;
			b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.buffer = buffer;
			b.offset = 0;
			b.size = VK_WHOLE_SIZE;
			vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 1, &b, 0, nullptr);
		}

		GSDeviceVK* VulkanDevice()
		{
			if (!g_gs_device || g_gs_device->GetRenderAPI() != RenderAPI::Vulkan)
				return nullptr;
			return GSDeviceVK::GetInstance();
		}
#endif
	}

	bool ProbeEnabled()
	{
		return Probe().enabled;
	}

	bool Wanted()
	{
		return s_compositor_wants.load(std::memory_order_acquire) || Probe().enabled;
	}

	void SetCompositorWants(bool wants)
	{
		s_compositor_wants.store(wants, std::memory_order_release);
	}

	DepthTracker& Tracker()
	{
		return s_tracker;
	}

	const ExportInfo& Latest()
	{
		return s_latest;
	}

	void NoExport(const char* why)
	{
		s_latest.valid = false;
		if (why && why != s_noexport_logged)
		{
			s_noexport_logged = why;
			Console.WriteLn("(VR) Depth export: no depth for the shown frame: %s.", why);
		}
	}

#ifdef ENABLE_VULKAN
	bool TakeSnapshot(GSTexture* depth)
	{
		GSDeviceVK* const dev = VulkanDevice();
		if (!dev || !depth || !depth->IsDepthStencil())
			return false;
		if (s_snap && (s_snap_device != dev || s_snap->GetSize() != depth->GetSize() ||
						  s_snap->GetArrayLayers() != depth->GetArrayLayers() || s_snap->GetFormat() != depth->GetFormat()))
		{
			if (s_snap_device == dev)
				g_gs_device->Recycle(s_snap);
			s_snap = nullptr;
		}
		if (!s_snap)
		{
			s_snap = g_gs_device->CreateDepthStencil(depth->GetWidth(), depth->GetHeight(), false, false, depth->GetArrayLayers());
			s_snap_device = dev;
			if (!s_snap)
				return false;
		}
		// The GS's own copy: recorded into the GS command buffer in draw order, with the depth target's pending clear and
		// its layout handled, so the snapshot is the depth as the finished frame left it, before the draw that changes it.
		g_gs_device->CopyRect(depth, s_snap, GSVector4i(0, 0, depth->GetWidth(), depth->GetHeight()), 0, 0);
		return true;
	}

	void* LatestBuffer()
	{
		return s_latest.valid ? reinterpret_cast<void*>(s_export.buffer) : nullptr;
	}

	bool Export(const ExportRequest& req)
	{
		s_latest.valid = false;
		GSDeviceVK* const dev = VulkanDevice();
		GSTexture* const src_tex = req.use_snapshot ? ((s_snap_device == dev) ? s_snap : nullptr) : req.depth;
		if (!dev || !src_tex || !src_tex->IsDepthStencil())
		{
			NoExport("no depth texture");
			return false;
		}
		if (req.width == 0 || req.height == 0 || req.copy_w == 0 || req.copy_h == 0 || req.dst_x + req.copy_w > req.width ||
			req.dst_y + req.copy_h > req.height || req.src_x + req.copy_w > static_cast<u32>(src_tex->GetWidth()) ||
			req.src_y + req.copy_h > static_cast<u32>(src_tex->GetHeight()))
		{
			NoExport("the shown frame does not map onto its depth target");
			return false;
		}
		const u32 layers = std::min<u32>(std::max<u32>(req.layers, 1u), 2u);
		const VkDeviceSize plane = static_cast<VkDeviceSize>(req.width) * req.height * 4u;
		const bool fresh = (s_export.buffer == VK_NULL_HANDLE || s_export.device != dev || s_export.size != plane * layers);
		if (!EnsureBuffer(dev, s_export, plane * layers, false))
		{
			NoExport("no export buffer");
			return false;
		}

		GSTextureVK* const src = static_cast<GSTextureVK*>(src_tex);
		if (dev->InRenderPass())
			dev->EndRenderPass();
		if (src->GetState() == GSTexture::State::Cleared)
			src->CommitClear();
		const VkCommandBuffer cmd = dev->GetCurrentCommandBuffer();
		if (fresh)
		{
			// Texels outside the shown rectangle read 0 (the cleared depth: the far end of the range).
			vkCmdFillBuffer(cmd, s_export.buffer, 0, VK_WHOLE_SIZE, 0u);
		}
		// After the depth layer's last read of the buffer (a copy in the compositor's submit, earlier on the queue).
		BufferBarrier(cmd, s_export.buffer, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		src->TransitionToLayout(cmd, GSTextureVK::Layout::TransferSrc);
		VkBufferImageCopy regions[2] = {};
		const u32 src_layers = std::max<u32>(static_cast<u32>(src->GetArrayLayers()), 1u);
		for (u32 l = 0; l < layers; l++)
		{
			VkBufferImageCopy& r = regions[l];
			r.bufferOffset = plane * l + (static_cast<VkDeviceSize>(req.dst_y) * req.width + req.dst_x) * 4u;
			r.bufferRowLength = req.width;
			r.bufferImageHeight = req.height;
			r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			r.imageSubresource.mipLevel = 0;
			// A single-layer depth gives both eyes its one layer.
			r.imageSubresource.baseArrayLayer = src->GetBaseArrayLayer() + std::min(l, src_layers - 1);
			r.imageSubresource.layerCount = 1;
			r.imageOffset = {static_cast<s32>(req.src_x), static_cast<s32>(req.src_y), 0};
			r.imageExtent = {req.copy_w, req.copy_h, 1};
		}
		vkCmdCopyImageToBuffer(cmd, src->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_export.buffer, layers, regions);
		BufferBarrier(cmd, s_export.buffer, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		src->SetUseFenceCounter(dev->GetCurrentFenceCounter());

		s_latest.valid = true;
		s_latest.width = req.width;
		s_latest.height = req.height;
		s_latest.layers = layers;
		s_latest.fit = req.fit;
		s_latest.units_per_meter = req.units_per_meter;
		s_latest.serial = ++s_serial;
		s_latest.src_bp = req.src_bp;
		s_latest.colour_bp = req.colour_bp;
		s_latest.snapshot = req.use_snapshot;
		s_noexport_logged = nullptr;
		if (!s_first_logged)
		{
			s_first_logged = true;
			const XrDepthRange r = RangeForRawDepth(req.fit, req.units_per_meter);
			Console.WriteLn("(VR) Depth export: first frame: the shown colour target 0x%x takes %s of depth target 0x%x "
							"(%dx%d, %u layer(s), z/2^32 floats), %ux%u from (%u, %u) at (%u, %u) of a %ux%u image; fit a0 %.9g "
							"beta %.6g, %.0f units/m, so depth = 1 / (%.0f * beta * (z - a0)) m; the runtime's range %.9g..%.9g "
							"reads %.3f..%.4f m.",
				req.colour_bp, req.use_snapshot ? "the snapshot taken as its frame was finished" : "the live depth", req.src_bp,
				src->GetWidth(), src->GetHeight(), src_layers, req.copy_w, req.copy_h, req.src_x, req.src_y, req.dst_x, req.dst_y,
				req.width, req.height, req.fit.a0, req.fit.beta, req.units_per_meter, req.units_per_meter, r.min_depth, r.max_depth,
				r.near_z, r.far_z);
		}

		// The probe: a copy of this export into its readback buffer, read once the GPU is past it (never waited on).
		const ProbeConfig& pcfg = Probe();
		if (pcfg.enabled && !s_probe_pending)
		{
			const u64 now_ms = NowMs();
			if (VMManager::IsOnlineLocked())
			{
				if (!s_probe_online_logged)
				{
					s_probe_online_logged = true;
					Console.Warning("(VR) Depth probe: refused, the network adapter is on (offline test aid).");
				}
			}
			else if (now_ms >= s_probe_next_ms && EnsureBuffer(dev, s_readback, plane * (layers + 1), true))
			{
				s_probe_next_ms = now_ms + static_cast<u64>(pcfg.every_s * 1000.0f);
				const VkBufferCopy bc = {0, 0, plane * layers};
				vkCmdCopyBuffer(cmd, s_export.buffer, s_readback.buffer, 1, &bc);
				// A dump also takes the shown colour (layer 0, RGBA8) after the depth, to lay the two over each other.
				GSTextureVK* const colour = static_cast<GSTextureVK*>(req.colour);
				s_probe_colour = false;
				if (pcfg.dump && s_probe_reads >= pcfg.dump_skip && s_probe_dumps < pcfg.max_dumps && colour && colour->GetWidth() == static_cast<int>(req.width) &&
					colour->GetHeight() == static_cast<int>(req.height) &&
					(colour->GetVkFormat() == VK_FORMAT_R8G8B8A8_UNORM || colour->GetVkFormat() == VK_FORMAT_B8G8R8A8_UNORM))
				{
					if (colour->GetState() == GSTexture::State::Cleared)
						colour->CommitClear();
					colour->TransitionToLayout(cmd, GSTextureVK::Layout::TransferSrc);
					VkBufferImageCopy r = {};
					r.bufferOffset = plane * layers;
					r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, colour->GetBaseArrayLayer(), 1};
					r.imageExtent = {req.width, req.height, 1};
					vkCmdCopyImageToBuffer(cmd, colour->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_readback.buffer, 1, &r);
					colour->SetUseFenceCounter(dev->GetCurrentFenceCounter());
					s_probe_colour = true;
					s_probe_colour_bgra = (colour->GetVkFormat() == VK_FORMAT_B8G8R8A8_UNORM);
				}
				BufferBarrier(cmd, s_readback.buffer, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
					VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT);
				s_probe_pending = true;
				s_probe_fence = dev->GetCurrentFenceCounter();
				s_probe_info = s_latest;
			}
		}
		return true;
	}

	void ProbeAfterPresent()
	{
		if (!s_probe_pending)
			return;
		GSDeviceVK* const dev = VulkanDevice();
		if (!dev || s_readback.buffer == VK_NULL_HANDLE || s_readback.device != dev)
		{
			s_probe_pending = false;
			return;
		}
		if (dev->GetCompletedFenceCounter() < s_probe_fence)
			return;
		s_probe_pending = false;
		const ExportInfo& info = s_probe_info;
		const size_t plane = static_cast<size_t>(info.width) * info.height;
		if (plane * (info.layers + 1) * 4u > s_readback.size)
			return;
		vmaInvalidateAllocation(dev->GetAllocator(), s_readback.allocation, 0, VK_WHOLE_SIZE);
		const float* const raw = static_cast<const float*>(s_readback.mapped);
		const auto metres_at = [&](u32 layer, int x, int y) -> double {
			if (x < 0 || y < 0 || x >= static_cast<int>(info.width) || y >= static_cast<int>(info.height))
				return -1.0;
			return LinearDepthM(info.fit, info.units_per_meter, raw[plane * layer + static_cast<size_t>(y) * info.width + x]);
		};

		// The render camera the shown frame was drawn with: its vertical FOV, and the 4:3 (16:9) picture.
		// The shown frame's own FOV when it was matched to its render pose, else the one camera.fov writes now (the same
		// while it holds still, as in a scripted test).
		float fov_deg = 0.0f;
		const char* fov_from = "the shown frame's";
		if (!CameraDriver::RenderFov(&fov_deg))
		{
			const CameraDriver::FovState fs = CameraDriver::GetFovState();
			fov_deg = fs.sizes_screen ? fs.fov_deg : 0.0f;
			fov_from = "camera.fov's now";
		}
		const bool have_fov = fov_deg > 1.0f && fov_deg < 179.0f;
		const float aspect = (GSConfig.AspectRatio == AspectRatioType::R16_9) ? (16.0f / 9.0f) : (4.0f / 3.0f);
		const float tan_v = std::tan(0.5f * fov_deg * 3.14159265f / 180.0f);
		const StereoState::Params st = StereoState::Get();
		const unsigned long long serial = static_cast<unsigned long long>(info.serial);

		Console.WriteLn("(VR) Depth probe #%llu: %ux%u, %u layer(s), from %s of depth target 0x%x (colour 0x%x); render FOV "
						"%.2f deg (%s), aspect %.4f; stereo sep %.5f conv %.1f floor %.3f; fit a0 %.9g beta %.6g, %.0f units/m.",
			serial, info.width, info.height, info.layers, info.snapshot ? "the snapshot" : "the live depth", info.src_bp,
			info.colour_bp, fov_deg, have_fov ? fov_from : "unknown", aspect, st.separation, st.convergence,
			st.near_floor, info.fit.a0, info.fit.beta, info.units_per_meter);
		// A 5 x 3 grid (metres along the view axis; 0 = no depth), layer 0.
		for (int gy = 1; gy <= 3; gy++)
		{
			std::string row;
			for (int gx = 1; gx <= 5; gx++)
				row += fmt::format(" {:7.3f}", metres_at(0, static_cast<int>(info.width * gx / 6), static_cast<int>(info.height * gy / 4)));
			Console.WriteLn("(VR) Depth probe #%llu: grid row %d/4 (x 1/6..5/6, layer 0):%s", serial, gy, row.c_str());
		}
		// The chosen points: where each lands in each layer (the centre view's pixel shifted by that layer's disparity for its
		// depth, as tfx.glsl shifts it, sign * sep * max(floor, 1 - conv / w)), and the depth read there.
		if (have_fov)
		{
			for (const XrVector3f& p : Probe().points)
			{
				const float ahead = -p.z;
				const float tx = p.x / ahead, ty = p.y / ahead;
				const double expect_dist = std::sqrt(static_cast<double>(p.x) * p.x + static_cast<double>(p.y) * p.y +
													 static_cast<double>(p.z) * p.z);
				const float ndc_x = tx / (aspect * tan_v);
				const float ndc_y = ty / tan_v;
				const float w_units = ahead * info.units_per_meter;
				const float disp = st.separation * std::max(st.near_floor, 1.0f - st.convergence / w_units);
				for (u32 l = 0; l < info.layers; l++)
				{
					const float sign = (info.layers > 1) ? ((l == 0) ? -1.0f : 1.0f) : 0.0f;
					const int px = static_cast<int>((0.5f + 0.5f * (ndc_x + sign * disp)) * static_cast<float>(info.width));
					const int py = static_cast<int>((0.5f - 0.5f * ndc_y) * static_cast<float>(info.height));
					// A 9 x 9 window around it: the nearest, the median and the farthest.
					std::vector<double> win;
					for (int dy = -4; dy <= 4; dy++)
					{
						for (int dx = -4; dx <= 4; dx++)
						{
							const double m = metres_at(l, px + dx, py + dy);
							if (m > 0.0)
								win.push_back(m);
						}
					}
					std::sort(win.begin(), win.end());
					const double at = metres_at(l, px, py);
					Console.WriteLn("(VR) Depth probe #%llu: point (%.3f, %.3f, %.3f) m, expected %.3f m ahead (%.3f m away): "
									"layer %u pixel (%d, %d): %.4f m ahead (%.4f m along its ray); 9x9 around it %.4f / %.4f / "
									"%.4f m.",
						serial, p.x, p.y, p.z, static_cast<double>(ahead), expect_dist, l, px, py, at,
						at * std::sqrt(1.0 + static_cast<double>(tx) * tx + static_cast<double>(ty) * ty),
						win.empty() ? 0.0 : win.front(), win.empty() ? 0.0 : win[win.size() / 2], win.empty() ? 0.0 : win.back());
				}
			}
		}
		const bool dump_due = Probe().dump && s_probe_reads >= Probe().dump_skip && s_probe_dumps < Probe().max_dumps;
		s_probe_reads++;
		if (dump_due)
		{
			s_probe_dumps++;
			const std::string dir = Path::Combine(EmuFolders::DataRoot, "depthprobe");
			FileSystem::EnsureDirectoryExists(dir.c_str(), true);
			std::vector<float> metres(plane);
			for (u32 l = 0; l < info.layers; l++)
			{
				for (size_t i = 0; i < plane; i++)
					metres[i] = static_cast<float>(LinearDepthM(info.fit, info.units_per_meter, raw[plane * l + i]));
				const std::string path = Path::Combine(dir, fmt::format("depth_{:06d}_L{}.pfm", info.serial, l));
				DumpLayer(path, metres.data(), info.width, info.height);
				Console.WriteLn("(VR) Depth probe #%llu: dumped %s (metres, PFM).", serial, path.c_str());
			}
			if (s_probe_colour)
			{
				// The shown colour, layer 0, as a binary PPM beside it (the same present).
				const u8* px = static_cast<const u8*>(s_readback.mapped) + plane * 4u * info.layers;
				const std::string path = Path::Combine(dir, fmt::format("colour_{:06d}_L0.ppm", info.serial));
				if (std::FILE* f = FileSystem::OpenCFile(path.c_str(), "wb"))
				{
					std::fprintf(f, "P6\n%u %u\n255\n", info.width, info.height);
					std::vector<u8> row(static_cast<size_t>(info.width) * 3u);
					for (u32 y = 0; y < info.height; y++)
					{
						const u8* in = px + static_cast<size_t>(y) * info.width * 4u;
						for (u32 x = 0; x < info.width; x++)
						{
							row[x * 3 + 0] = in[x * 4 + (s_probe_colour_bgra ? 2 : 0)];
							row[x * 3 + 1] = in[x * 4 + 1];
							row[x * 3 + 2] = in[x * 4 + (s_probe_colour_bgra ? 0 : 2)];
						}
						std::fwrite(row.data(), 1, row.size(), f);
					}
					std::fclose(f);
					Console.WriteLn("(VR) Depth probe #%llu: dumped %s (the shown colour, the same present).", serial, path.c_str());
				}
			}
		}
	}

	void Destroy()
	{
		Free(s_export);
		Free(s_readback);
		if (s_snap && s_snap_device == GSDeviceVK::GetInstance() && g_gs_device)
			g_gs_device->Recycle(s_snap);
		s_snap = nullptr;
		s_snap_device = nullptr;
		s_probe_pending = false;
		s_latest = ExportInfo{};
		s_tracker = DepthTracker{};
	}
#else
	bool TakeSnapshot(GSTexture*) { return false; }
	void* LatestBuffer() { return nullptr; }
	bool Export(const ExportRequest&)
	{
		NoExport("the depth export is Vulkan only");
		return false;
	}
	void ProbeAfterPresent() {}
	void Destroy() {}
#endif
}
