// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/DepthExport.h"

#include "common/Pcsx2Defs.h"

#include <openxr/openxr.h>

#ifdef ENABLE_VULKAN
#include "GS/Renderers/Vulkan/VKLoader.h"
#endif

// [VR] DepthWarp: our own translation reprojection of the first-person frame, per eye and per display frame, in the VR
// pacer (research/2026-10-13-depth-export.md, "Our own warp"). The game renders one view per frame at 30 fps; the
// compositor places each eye's image on a world-locked quad at the pose it was rendered from, so the runtime corrects
// rotation but a lean moves the picture, not the world. With the switch on, every display frame each eye's image is
// redrawn into its swapchain as seen from where that eye is now: a grid mesh over the image is displaced by the frame's
// exported depth (DepthExport) and drawn from the eye's current position onto the same quad, so near things move against
// far ones by the true parallax. The quad stays where it was (rotation is still corrected by the runtime as before).
//
// The geometry, in the quad's own frame (origin at its centre, +X right, +Y up, +Z out of its face towards the viewer;
// image u to the right, v down): the image pixel (u, v) lies on the quad at Q = ((u - 0.5) W, (0.5 - v) H, 0). The frame
// was seen from the source eye E (E.z > 0, the eye's distance to the quad plane), and the exported depth d is the view
// depth along the render camera's forward axis, which is the quad's -Z (QuadViewForEye: the views share that axis). So
// the world point is X = E + (Q - E) d / E.z, and from the current eye F it lies on the quad at
// Q' = F + (X - F) F.z / (F.z - X.z). For a sideways move D (F = E + D, D.z = 0) that is Q' = Q + D (1 - E.z / d): a
// point at the quad's distance stays, a far one moves with the head, a near one against it.
namespace VR::DepthWarp
{
	// The warp's view of one eye: the quad's size and the source and current eyes, all in the quad's frame (metres).
	struct Geometry
	{
		float width = 0.0f;
		float height = 0.0f;
		XrVector3f src_eye = {0.0f, 0.0f, 0.0f};
		XrVector3f dst_eye = {0.0f, 0.0f, 0.0f};
	};

	// The depth clamp the warp reads the exported depth with: nearer reads as kNearM, farther (the cleared background,
	// 500.2 m) as kFarM, which moves with the head as the far plane should.
	inline constexpr float kNearM = 0.05f;
	inline constexpr float kFarM = 500.0f;
	// The mesh: kGridCells x kGridCells cells over the image, extended by kGridMargin (in uv) past each edge, where the
	// edge texels are stretched (no black border when the eye moves); each vertex takes the nearest depth within
	// kFootprintTexels of it (foreground edges stay whole, the background stretches into what it uncovers).
	inline constexpr u32 kGridCells = 128;
	inline constexpr float kGridMargin = 0.06f;
	inline constexpr float kFootprintTexels = 6.0f;

	// p (world) in the quad's frame.
	XrVector3f ToQuadLocal(const XrPosef& quad, const XrVector3f& p);

	// The world point (quad frame) image pixel (u, v) at view depth depth_m shows, seen from g.src_eye. False when the
	// eye is not in front of the quad or the depth is not positive.
	bool Unproject(const Geometry& g, float u, float v, float depth_m, XrVector3f* x);

	// Where x (quad frame) lies on the quad's image seen from g.dst_eye, and its view depth from there. False when it is
	// not in front of that eye.
	bool Project(const Geometry& g, const XrVector3f& x, float* u, float* v, float* depth_m);

	// Unproject from the source eye, then Project from the current one (what the mesh vertex shader does).
	bool Reproject(const Geometry& g, float u, float v, float depth_m, float* u2, float* v2, float* depth2_m);

	// The sideways shift on the quad (metres, along the move) of a point at view depth depth_m for a sideways move of
	// move_m by an eye quad_distance_m from the quad: move (1 - quad_distance / depth).
	double SidewaysShiftM(double move_m, double quad_distance_m, double depth_m);

	// One grid vertex as the shader computes it, on the CPU (the self-tests' reference): gx, gy in 0..cells, the depth
	// plane (raw z/2^32 floats, width x height, row by row) read through the fit, the nearest within the footprint.
	// Returns false (and the undisplaced uv) when the geometry does not allow a warp.
	bool GridVertex(const Geometry& g, const DepthExport::ZFit& fit, float units_per_meter, const float* plane, u32 width,
		u32 height, u32 gx, u32 gy, u32 cells, float* u2, float* v2);

	// The pure parts: the frame, the projection round trip, the sideways parallax at known depths, the conversion the
	// shader reads the depth with, and the grid displacement on a synthetic depth plane.
	bool SelfTest();

#ifdef ENABLE_VULKAN
	// The warp's push constants (the vertex shader's block, std430 vec4s).
	struct PushConstants
	{
		float src_eye[4]; // xyz; w: 1 = warp, 0 = draw the image as it is
		float dst_eye[4]; // xyz; w: grid cells per side
		float quad[4];    // width, height (m), a0, beta * units per metre
		float clampm[4];  // near m, far m, grid margin (uv), footprint (texels)
		u32 img[4];       // width, height, the plane's offset in floats, 0
	};
	PushConstants MakePushConstants(const Geometry& g, bool warp, const DepthExport::ZFit& fit, float units_per_meter,
		u32 width, u32 height, u32 plane_offset_floats);

	// The pass's Vulkan objects: a render pass into a colour image of the swapchain's format with a D16 depth attachment,
	// the mesh pipeline, its layout (push descriptors: the source colour, the depth plane), the linear clamp sampler.
	struct Pipeline
	{
		VkRenderPass render_pass = VK_NULL_HANDLE;
		VkDescriptorSetLayout ds_layout = VK_NULL_HANDLE;
		VkPipelineLayout layout = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkSampler sampler = VK_NULL_HANDLE;
		VkFormat colour_format = VK_FORMAT_UNDEFINED;
		bool valid() const { return pipeline != VK_NULL_HANDLE; }
	};
	inline constexpr VkFormat kDepthFormat = VK_FORMAT_D16_UNORM;
	// GS thread (it compiles through the GS's shader cache). False (out cleared) on any failure.
	bool CreatePipeline(VkDevice device, VkFormat colour_format, Pipeline* out);
	void DestroyPipeline(VkDevice device, Pipeline* p);
	inline constexpr u32 VertexCount() { return kGridCells * kGridCells * 6; }
#endif
}
