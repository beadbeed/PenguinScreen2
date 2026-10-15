// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include <openxr/openxr.h>

class GSTexture;

// The displayed frame's depth, exported beside its colour (research/2026-10-13-depth-export.md), the foundation of
// depth-based reprojection (ROADMAP road step 5). Two users:
//  - [VR] DepthLayer: the first-person frame goes to the runtime as a projection layer at its render pose with an
//    XR_KHR_composition_layer_depth depth image, so the runtime's own reprojection can use it (XRCompositor);
//  - PCSX2_VR_DEPTHPROBE: a headset-free debug path that reads the exported depth back and logs it in metres at
//    chosen pixels (README).
// With neither on, nothing here runs: no copy, no buffer.
//
// What the depth is. The GS depth target holds z * 2^-32 (tfx.glsl: gl_Position.z = z * exp2(-32), the depth target's
// format is D32_SFLOAT(_S8_UINT)), the same normalised z the Z-driven depth fit (GSRendererHW, VRZQDepthFit) reads.
// That fit finds the game's texture Q, which is 1/w for the view depth w in game units, affine in that z:
//     q = beta * (z_norm - a0)        (R2 = 1.000 on File #1)
// so the view depth in metres is exactly
//     depth_m = 1 / (units_per_meter * beta * (z_norm - a0))
// with camera.lookAt.unitsPerMeter (100 game units a metre on File #1), the same scale the stereo's convergence uses
// (StereoAuto: conv = d * unitsPerMeter). 1/depth is affine in the stored value, which is exactly how an OpenXR
// depth image is read (1/distance affine in the value between minDepth at nearZ and maxDepth at farZ), so the raw
// values go to the runtime unconverted, described by the range RangeForRawDepth works out: no conversion pass, and
// no rounding beyond the depth target's own float.
namespace VR::DepthExport
{
	// The Z-driven depth fit as GSRendererHW last accepted it: q = beta * (z_norm - a0). Not valid until a frame has been
	// fitted (or while the fit is refused).
	struct ZFit
	{
		bool valid = false;
		float a0 = 0.0f;
		float beta = 0.0f;
	};

	// The default game units a metre (camera.lookAt.unitsPerMeter's own default, StereoAuto::kDefaultUnitsPerMeter).
	inline constexpr float kDefaultUnitsPerMeter = 100.0f;
	// The distances RangeForRawDepth's range spans: anything farther (the cleared background, the sky) reads as the far
	// end, anything nearer as the near end.
	inline constexpr double kRangeFarM = 500.0;
	inline constexpr double kRangeNearM = 0.05;

	// The view depth in metres along the render camera's forward axis of a stored depth value; 0 when it is not a depth
	// in front of the camera (z_norm at or beyond a0, the depth of infinity, or no usable fit or scale).
	double LinearDepthM(const ZFit& fit, float units_per_meter, double z_norm);

	// The stored depth value of a view depth in metres (LinearDepthM's inverse); NaN without a usable fit.
	double DepthValueFor(const ZFit& fit, float units_per_meter, double metres);

	// How the exported raw depth is described to the runtime (XrCompositionLayerDepthInfoKHR): the values span
	// [min_depth, max_depth], near_z is the distance at min_depth and far_z the one at max_depth. For File #1 (beta > 0:
	// a larger value is nearer) near_z > far_z, the reversed order the extension allows.
	struct XrDepthRange
	{
		bool valid = false;
		float min_depth = 0.0f;
		float max_depth = 1.0f;
		float near_z = 0.0f;
		float far_z = 0.0f;
	};
	XrDepthRange RangeForRawDepth(const ZFit& fit, float units_per_meter, double far_m = kRangeFarM, double near_m = kRangeNearM);

	// How an OpenXR runtime reads a depth value with that range: the projection's depth, 1/distance affine in the value
	// from 1/near_z at min_depth to 1/far_z at max_depth (values outside clamped to the ends). 0 without a valid range.
	double XrDistanceAt(const XrDepthRange& range, double value);

	// The view a projection layer needs to show exactly what a quad layer shows one eye: the quad (centre pose, width x
	// height metres) seen from the eye at head + eye_offset (world metres). The view gets the quad's orientation (the
	// first-person screen faces the head roll-level) and the eye's position, and the fov whose tangents meet the quad's
	// edges as seen from there, so each image pixel lands where the quad put it for that eye. False when the quad is not
	// in front of the eye.
	bool QuadViewForEye(const XrPosef& quad, float width, float height, const XrVector3f& head, const XrVector3f& eye_offset,
		XrPosef* view_pose, XrFovf* view_fov);

	// An eye's offset from the head centre in world metres: head * (x, 0, 0), x = -IPD/2 for the left eye, +IPD/2 for the
	// right, with the head's full orientation (its roll included, as the runtime places the eyes that see the quad).
	XrVector3f EyeOffset(const XrQuaternionf& head, float x);

	// Where an export maps into the shown colour image, and what it holds.
	struct ExportInfo
	{
		bool valid = false;
		u32 width = 0;       // the colour image's size (the depth is laid out at it, row by row, one float a texel)
		u32 height = 0;
		u32 layers = 1;      // 2 in layered stereo: each eye its own depth layer
		ZFit fit;            // the fit the depth was exported with
		float units_per_meter = kDefaultUnitsPerMeter;
		u64 serial = 0;      // counts exports
		u32 src_bp = 0;      // the depth target's base block (for the log)
		u32 colour_bp = 0;   // the displayed colour target's base block
		bool snapshot = false; // taken when the frame's depth was finished (DepthTracker), not at the present
	};

	// --- Which depth belongs to the displayed frame (pure; GS thread) ---------------------------------------------------
	// The game draws a frame's 3D into a colour target with a depth target, then the next frame clears or redraws that
	// depth, possibly before the first frame is presented. DepthTracker watches the draws: while depth-tested draws go
	// into one (colour, depth) pair that frame's depth is live; the first draw that would change that depth for another
	// frame (a depth-tested or depth-writing draw with another colour target, or a draw whose colour target is the depth
	// buffer itself, a clear) ends it, and the caller snapshots the depth right then, before that draw, labelled with the
	// colour target. At the present the displayed frame takes the depth whose label is its own colour target (or one
	// that feeds it): the live depth when that frame is still the latest, else the snapshot.
	struct DrawNote
	{
		u32 fbp = 0;          // FRAME.Block
		u32 zbp = 0;          // ZBUF.Block
		bool z_test = false;  // ZTE on with a test other than ALWAYS (a 3D draw against the depth)
		bool z_write = false; // ZMSK off
	};
	enum class TrackAction : u8
	{
		None,
		Snapshot, // copy the depth target at zbp now, before this draw, labelled snapshot_label
	};
	struct DepthTracker
	{
		bool live = false;     // a run of depth-tested draws into (live_fbp, live_zbp) is the latest depth
		u32 live_fbp = 0;
		u32 live_zbp = 0;
		u32 live_draws = 0;
		bool snap_valid = false; // a snapshot holds the depth of colour target snap_fbp
		u32 snap_fbp = 0;
		u32 snap_zbp = 0;
		u32 snapshot_label = 0; // TrackAction::Snapshot: the colour target the snapshot belongs to

		// Every draw, before it is rendered.
		TrackAction NoteDraw(const DrawNote& d);
		// The snapshot NoteDraw asked for is taken (or could not be: false drops it).
		void SnapshotTaken(bool ok);
	};
	enum class PresentSource : u8
	{
		None,
		Live,     // the depth target still holds this frame's depth
		Snapshot, // the snapshot taken when it was finished
	};
	// The depth for a displayed colour target displayed_fbp. feeder_fbp: the colour target its 3D was drawn into, when the
	// game draws into one target and copies it to the displayed one (0: none known).
	PresentSource SourceForPresent(const DepthTracker& t, u32 displayed_fbp, u32 feeder_fbp);

	// --- The export (GS thread) ------------------------------------------------------------------------------------------
	// Whether anything wants the depth: the depth layer up and running (SetCompositorWants) or the probe.
	bool Wanted();
	// XRCompositor: the depth layer is up (a session with the extension and its swapchains), or no longer.
	void SetCompositorWants(bool wants);
	// PCSX2_VR_DEPTHPROBE (offline only; read once): the debug readback after the present (README).
	bool ProbeEnabled();

	// GSRendererHW, every draw while Wanted(): the tracker, and the snapshot it asks for (depth: the texture of the depth
	// target the finished frame used; null or a failed copy drops the snapshot).
	DepthTracker& Tracker();
	bool TakeSnapshot(GSTexture* depth);

	// [VR] MenuPanel's room (research/2026-10-13-menu-panel.md): while a game screen is up, the screen clears the depth
	// again before its own draws, so the tracker's snapshot of that frame holds the screen's depth, not the room's. The
	// room the headset shows then is MenuPanel's colour snapshot, taken just before the screen's first draw; this is
	// the depth at that same point, one per MenuPanel slot (SplitTracker::kSlots), labelled with the colour target.
	// GSRendererHW takes it right after MenuPanel's snapshot (drops it when there is no depth to take), and a present
	// that shows that slot's room exports it (ExportRequest::room_slot). Under MenuPanelRoom = Freeze the room is drawn
	// every frame from the held camera, so each frame's depth goes with its own picture and render pose.
	inline constexpr u32 kRoomSlots = 2;
	bool TakeRoomSnapshot(u32 slot, u32 fbp, u32 zbp, GSTexture* depth);
	void DropRoomSnapshot(u32 slot);
	// The room depth of `slot` was taken for colour target fbp (on this device).
	bool HasRoomSnapshot(u32 slot, u32 fbp);
	// Its depth target's base block (for the log), 0 when none.
	u32 RoomSnapshotZbp(u32 slot);

	// GSRendererHW at each present, right after Merge (before the present's submit, so VR::EndOfFrame's copies find it on
	// the GPU): this frame's depth copied into the export buffer in the GS's command buffer, laid out as the colour image
	// `current` is: depth texel (src_x + x, src_y + y) at colour pixel
	// (dst_x + x, dst_y + y), each layer its own width x height plane of floats.
	struct ExportRequest
	{
		bool use_snapshot = false;   // the tracker's snapshot, else `depth`
		int room_slot = -1;          // >= 0: MenuPanel's room depth of that slot (TakeRoomSnapshot), over both of those
		GSTexture* depth = nullptr;  // the live depth target
		GSTexture* colour = nullptr; // the shown colour image (only the probe's dump reads it)
		u32 src_x = 0, src_y = 0;    // the displayed rectangle's origin in the depth texture (scaled texels)
		u32 dst_x = 0, dst_y = 0;    // where it lands in the colour image
		u32 copy_w = 0, copy_h = 0;  // its size
		u32 width = 0, height = 0;   // the colour image's size
		u32 layers = 1;              // the colour image's layers
		u32 colour_bp = 0;
		u32 src_bp = 0;
		ZFit fit;
		float units_per_meter = kDefaultUnitsPerMeter;
	};
	bool Export(const ExportRequest& req);
	// No depth for this present: Latest() goes invalid, and the reason is logged once each time it changes.
	void NoExport(const char* why);

	// The latest export (GS thread): valid only for the present that made it.
	const ExportInfo& Latest();
	// The export buffer holding Latest() (a VkBuffer; layer l at l * width * height floats), for the depth layer's copy.
	void* LatestBuffer();

	// GS thread, after VR::EndOfFrame: the probe's readback and log, when it is on and due.
	void ProbeAfterPresent();

	// The GS renderer is going away: the snapshot and the buffer freed.
	void Destroy();

	// The pure parts: the conversion, the runtime's reading of the range, the quad-to-view geometry and the tracker.
	bool SelfTest();
}
