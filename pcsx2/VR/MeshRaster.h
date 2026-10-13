// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include <cstddef>
#include <vector>

// A small CPU rasteriser for textured, lit triangle meshes: the game's weapon models (a few hundred triangles, one RGBA8
// texture) and the CC0 hand mesh (about a thousand, untextured, with vertex normals), drawn into the 256 x 256 impostor
// images HandModel hands to the compositor. Standalone: no XR, Vulkan or emulator types, one thread at a time per Target,
// no allocation while drawing, and the same input always gives the same pixels.
//
// USE
//
//   Target target(256, 256, 2);                                   // once: 256 x 256, 2 x 2 samples per pixel
//   Camera camera;
//   Camera::Impostor(eye, quad_centre, quad_right, quad_up, quad_size_m, 0.03f, 100.0f, &camera); // per image
//   target.Clear();                                                // transparent black, depth 1
//   Draw(target, camera, hand_mesh, hand_model, hand_material);    // per mesh (a weapon's parts one by one)
//   Draw(target, camera, gun_mesh, gun_model, gun_material);
//   target.Resolve();                                              // averages the samples down to pixels
//   target.CopyUnpremultiplied(out_rgba);                          // the impostor's image, as the compositor takes it
//
// CONVENTIONS
//
// - Right-handed, +Y up, a camera looks down its own -Z (OpenXR's base space); distances are metres. Matrices are
//   column-major (element row r, column c is m[c * 4 + r]) and apply to column vectors. Quaternions are xyzw.
// - A target's pixel (x, y) has x to the right and y downwards, row 0 on top, and its centre at (x + 0.5, y + 0.5), as
//   HandModel's impostor images. A pixel is a little-endian u32: R in the low byte, then G, B, and A in the top byte
//   (the byte order of ControlQuads::PackRgba).
// - Depth is the projection's z / w in 0 (the near plane) to 1 (the far plane); a sample is drawn when it is nearer than
//   what is there (less than, so the first of two equal depths stays). Clear sets depth to 1.
// - A texture's texel (x, y) is texels[y * width + x] and covers u in [x / width, (x + 1) / width) and v in
//   [y / height, (y + 1) / height): v counts from the top row, like glTF and the game's own UVs.
// - A triangle is front-facing when its vertices run counter-clockwise as seen from the front (Mesh::front_ccw, the
//   OpenGL default). A model matrix that mirrors (negative determinant, a left hand drawn from the right hand's mesh) is
//   allowed: it keeps the mesh's front faces in front, and normals are turned with it.
//
// CAMERA
//
// A Camera is a view matrix (world to eye, a rigid transform) and a projection (eye to clip). Build one with
//   Camera::Perspective(view, fov_y_rad, aspect, near, far)         a symmetric frustum,
//   Camera::OffAxis(view, tan_left, tan_right, tan_down, tan_up, near, far)
//                                                                   a frustum the eye need not look at the middle of:
//                                                                   the tangents of the angles from the view axis to
//                                                                   the four edges of the picture (an OpenXR XrFovf),
//   Camera::Orthographic(view, left, right, bottom, top, near, far) a box in eye space,
//   Camera::Impostor(eye, quad_centre, quad_right, quad_up, quad_size_m, near, far, &out)
//                                                                   HandModel's impostor view: the eye is the centre of
//                                                                   projection and the picture is the square of side
//                                                                   quad_size_m on the quad's plane, centred on
//                                                                   quad_centre, with the quad's up up. Pixels land
//                                                                   exactly where HandModel::RenderImpostor puts them.
// and a view with LookAt(eye, target, up) or ViewFromAxes(eye, right, up, back). ProjectPoint gives the pixel (and depth)
// a world point lands on, for placing a muzzle flash or a laser's start on the image.
//
// MESH, MATERIAL, DRAW
//
// Mesh: vertex arrays (positions xyz, normals xyz, optional UVs uv; each with an optional byte stride, a multiple of 4,
// 0 meaning tightly packed) and a triangle list by u16 or u32 indices (neither: the vertices are a plain triangle list).
// The mesh is borrowed, never copied: it must stay alive for the Draw. A triangle with an index past the vertex count, or
// a vertex that is not finite, is skipped.
//
// Material:
//   texture       an optional RGBA8 Texture (texel A = 255 is opaque): nearest or bilinear, and repeat or clamp for u and
//                 v. Bilinear filters straight (not premultiplied) texels, so when a texture is cut out with the alpha
//                 test, give its transparent texels the colour of their neighbours (dilate), or the cut edge goes dark.
//                 No mipmaps: shrinking a big texture a lot is where supersampling earns its keep.
//   base_colour   multiplies the texture (or is the colour without one), each channel 0 to 1.
//   lighting      one directional key light plus ambient, Lambert, an optional headlight (the part facing the viewer is
//                 lit) and an optional rim (the part turning away from the viewer glows). Worked out per vertex and
//                 interpolated across the triangle (Gouraud), so it needs the mesh's normals; without them the mesh is
//                 drawn unlit. The total light is clamped to 0 to 1: a surface never gets brighter than its colour.
//                 light_in_view_space gives the direction relative to the camera (x right, y up, z towards the viewer),
//                 which keeps the shading steady as the head moves, as HandModel's impostors do. linear_light counts the
//                 light in linear light while the colours stay sRGB bytes (a factor is applied as its square root, the
//                 usual gamma approximation), as HandModel shades; off, the factors multiply the bytes as they are (the
//                 PS2 game's own look).
//   alpha_cutoff  above 0, a texel (a filtered texel with bilinear) whose alpha is below it is not drawn: no colour and
//                 no depth, so what is behind shows. What is drawn is opaque.
//   cull_back     drop the faces turned away. Off, they are drawn too, lit from their own side (the normals turned).
//
// Draw(target, camera, mesh, model, material, &stats) draws every triangle of the mesh transformed by model and returns
// false when nothing could be drawn (a missing target or arrays, or a model matrix that is flat or not finite). The
// optional DrawStats counts what became of the triangles and samples, which the self-test and a profile read.
//
// TARGET
//
// Target holds the colour (premultiplied RGBA8) and float depth buffers, allocated once and kept. samples_per_axis is 1,
// 2 or 4: an ordered grid of that many by that many samples per pixel (4, or 16 samples), each shaded on its own and
// averaged down by Resolve for smooth edges and cut-outs. Pixels() is the final image: with 1 it is the buffer drawn
// into; with 2 or 4 it is only up to date after Resolve (call it once, after the last Draw). Clear and Resolve only
// touch the part of the buffers the draws since the last Clear reached, so a small hand in a big image costs little.
// CopyUnpremultiplied gives the image with straight alpha, the form the impostor quads use.
//
// HOW IT FILLS
//
// Triangles are clipped against the near and far planes and a wide guard band round the picture in homogeneous clip
// space (cut points always worked out from the inside vertex out, so two triangles sharing an edge cut it at the same
// point), their corners are snapped to 1/256 of a sample, and each sample is covered when it is inside all three edges,
// worked out in exact integer arithmetic. A sample exactly on an edge belongs to the triangle that has it as its top or
// left edge (the Direct3D top-left rule), so a mesh's shared edges leave neither cracks nor double hits. Depth, light
// and UVs are interpolated in screen space from the plane through the triangle's corners (the UVs divided by w, so they
// are perspective-correct). Only a triangle that reaches beyond the guard band (eight times the picture's size) can be
// cut off its neighbour's edge by a hair.
//
// SPEED
//
// The cost is mostly in the samples shaded: the area drawn, times 1, 4 or 16 for the sample grid. On a 2.5 GHz cloud
// core a sphere of 1008 triangles filling 5.6 thousand pixels of a 256 x 256 image takes (Clear + Draw + Resolve) 0.10 ms
// at 1 x 1, 0.3 ms at 2 x 2 and 0.9 ms at 4 x 4 as a flat colour, lit, with a rim (a hand's worth), and 0.11, 0.37 and
// 1.2 ms with a nearest-filtered texture (a weapon's worth); filling 25 thousand pixels with a bilinear texture it takes
// 0.7, 2.7 and 11 ms. Textures with power-of-two sides sample fastest. SelfTest logs the same for the machine it runs on.
namespace VR::MeshRaster
{
	// Column-major 4 x 4 matrix, identity by default.
	struct Mat4
	{
		float m[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
	};

	// a * b: b applies first.
	Mat4 Multiply(const Mat4& a, const Mat4& b);

	// Translation * rotation * scale for a mesh part: pos, a unit quaternion xyzw (identity when unusable) and a scale on
	// each axis (null: 1). A negative scale mirrors.
	Mat4 ModelMatrix(const float pos[3], const float quat_xyzw[4], const float scale[3] = nullptr);

	// A view matrix with the eye at eye, the camera's right, up and back (+Z, away from where it looks) axes in world
	// space (unit and perpendicular), and one that looks from eye at target with up the way to keep upright.
	Mat4 ViewFromAxes(const float eye[3], const float right[3], const float up[3], const float back[3]);
	Mat4 LookAt(const float eye[3], const float target[3], const float up[3]);

	struct Camera
	{
		Mat4 view; // world to eye; a rigid transform (lighting reads the eye position and axes off it)
		Mat4 proj; // eye to clip, depth 0 at the near plane to 1 at the far
		bool orthographic = false; // set by Orthographic; picks the view vector the lighting uses

		static Camera Perspective(const Mat4& view, float fov_y_rad, float aspect, float near_m, float far_m);
		static Camera OffAxis(const Mat4& view, float tan_left, float tan_right, float tan_down, float tan_up, float near_m,
			float far_m);
		static Camera Orthographic(const Mat4& view, float left, float right, float bottom, float top, float near_m,
			float far_m);

		// HandModel's impostor view (see CAMERA above). False, with *out untouched, for input that is not finite, a size
		// that is not positive, or an eye less than 2 cm in front of the quad's plane (RenderImpostor draws nothing then
		// either).
		static bool Impostor(const float eye[3], const float quad_centre[3], const float quad_right[3],
			const float quad_up[3], float quad_size_m, float near_m, float far_m, Camera* out);
	};

	// The pixel a world point lands on in a width x height target (in pixels, fractional, the same mapping Draw uses) and
	// its depth. False for a point at or behind the eye's plane (w <= 0) or that is not finite; the pixel can still be off
	// the image, and the depth outside 0 to 1 when the point is nearer than the near plane or past the far.
	bool ProjectPoint(const Camera& camera, u32 width, u32 height, const float world[3], float out_px[2],
		float* out_depth = nullptr);

	struct Texture
	{
		enum class Filter : u8
		{
			Nearest,
			Bilinear,
		};
		enum class Wrap : u8
		{
			Repeat,
			Clamp,
		};

		const u32* texels = nullptr; // width * height RGBA8 (R in the low byte), row 0 on top; borrowed
		u32 width = 0;
		u32 height = 0;
		Filter filter = Filter::Nearest;
		Wrap wrap_u = Wrap::Repeat;
		Wrap wrap_v = Wrap::Repeat;
	};

	struct Mesh
	{
		const float* positions = nullptr; // 3 floats a vertex
		const float* normals = nullptr;   // 3 floats a vertex; null: drawn unlit
		const float* uvs = nullptr;       // 2 floats a vertex; null: u = v = 0 (fine without a texture)
		u32 position_stride = 0;          // bytes from one vertex to the next in each array; 0: tightly packed
		u32 normal_stride = 0;
		u32 uv_stride = 0;
		u32 vertex_count = 0;
		// The triangles: index_count / 3 of them. At most one of the two index arrays; with neither, vertex_count / 3
		// triangles of consecutive vertices.
		const u16* indices16 = nullptr;
		const u32* indices32 = nullptr;
		u32 index_count = 0;
		bool front_ccw = true; // front faces run counter-clockwise (false: clockwise)
	};

	struct Lighting
	{
		bool enabled = true;
		float light_dir[3] = {0.35f, 0.8f, 0.5f}; // towards the light (it need not be a unit vector)
		bool light_in_view_space = false;         // light_dir relative to the camera: x right, y up, z towards the viewer
		float ambient = 0.3f;
		float key = 0.7f;       // Lambert strength of the key light
		float headlight = 0.0f; // light on what faces the viewer, whatever the key light does
		float rim = 0.0f;       // strength of the glow on what turns away from the viewer; 0 is off
		int rim_power = 3;      // how narrow the rim is (1 to 8): (1 - facing)^rim_power
		float rim_colour[3] = {1.0f, 1.0f, 1.0f};
		bool linear_light = true;
	};

	struct Material
	{
		const Texture* texture = nullptr;
		float base_colour[3] = {1.0f, 1.0f, 1.0f};
		Lighting lighting;
		float alpha_cutoff = 0.0f; // 0: off
		bool cull_back = true;
	};

	// What became of one Draw's triangles and samples.
	struct DrawStats
	{
		u32 triangles = 0;  // in the mesh
		u32 culled = 0;     // back-facing, with cull_back
		u32 rejected = 0;   // outside the view, flat, not finite, or with an index that is out of range
		u32 clipped = 0;    // cut by a clip plane (the near plane, mostly)
		u32 rasterised = 0; // triangles filled (a clipped one may be several)
		u64 covered = 0;    // samples inside a triangle, before depth and alpha
		u64 written = 0;    // samples that passed both and were drawn
	};

	namespace detail
	{
		struct Rasteriser;

		// A vertex after the model, view and projection, and its light.
		struct ClipVertex
		{
			float x, y, z, w;
			float u, v;
			float lit, rim;     // the light on its front side
			float lit_b, rim_b; // and on its back (only for a double-sided material)
			u32 code;           // which clip planes it is outside
		};
	}

	class Target
	{
	public:
		static constexpr u32 kMaxSize = 4096;
		// Samples (pixels times the grid's square), so that a target stays within 128 MB.
		static constexpr size_t kMaxSamples = static_cast<size_t>(1) << 24;
		static constexpr u32 kDefaultVertexCapacity = 4096;

		Target() = default;
		// A width x height image at samples_per_axis (1, 2 or 4) samples across and down a pixel, cleared to transparent
		// black. A size that is not usable (see Resize) leaves the target empty.
		Target(u32 width, u32 height, u32 samples_per_axis = 1, u32 vertex_capacity = kDefaultVertexCapacity);

		// Reallocates only when the buffers have to grow. False, and the target left empty, for a side of 0 or past
		// kMaxSize, a samples_per_axis other than 1, 2 or 4, or more than kMaxSamples samples. Clears to transparent black.
		bool Resize(u32 width, u32 height, u32 samples_per_axis);

		u32 Width() const { return m_width; }
		u32 Height() const { return m_height; }
		u32 SamplesPerAxis() const { return m_grid; }
		bool Empty() const { return m_width == 0; }

		// Sets every pixel to colour (premultiplied RGBA8; transparent black by default) and the depth to 1.
		void Clear(u32 colour = 0u);

		// Averages the samples to the pixels (a no-op with one sample). Call it after the last Draw and before reading
		// Pixels().
		void Resolve();

		// The image: Width() * Height() premultiplied RGBA8 pixels, row 0 on top.
		const u32* Pixels() const { return (m_grid == 1) ? m_colour.data() : m_pixels.data(); }
		u32 Pixel(u32 x, u32 y) const { return Pixels()[static_cast<size_t>(y) * m_width + x]; }
		// Writes the image with straight alpha (colour divided by alpha), Width() * Height() pixels, as the lever cards and
		// the hand impostors are.
		void CopyUnpremultiplied(u32* out_rgba) const;

		// The depth of every sample: SampleWidth() * SampleHeight() floats.
		const float* Depth() const { return m_depth.data(); }
		u32 SampleWidth() const { return m_width * m_grid; }
		u32 SampleHeight() const { return m_height * m_grid; }

	private:
		friend struct detail::Rasteriser;

		struct Rect
		{
			u32 x0 = 0, y0 = 0, x1 = 0, y1 = 0; // in samples, the end exclusive
			bool Empty() const { return x1 <= x0 || y1 <= y0; }
			void Add(u32 ax0, u32 ay0, u32 ax1, u32 ay1);
		};

		u32 m_width = 0;
		u32 m_height = 0;
		u32 m_grid = 1;
		std::vector<u32> m_colour;  // one per sample; the pixels themselves when the grid is 1
		std::vector<float> m_depth; // one per sample
		std::vector<u32> m_pixels;  // the resolved image, only with a grid above 1
		Rect m_dirty;               // what the draws since the last Clear reached
		u32 m_clean = 0;            // the colour everything outside m_dirty holds
		std::vector<detail::ClipVertex> m_vertices; // the vertex scratch
	};

	bool Draw(Target& target, const Camera& camera, const Mesh& mesh, const Mat4& model, const Material& material,
		DrawStats* stats = nullptr);

	// A pixel from its bytes, as the target stores it.
	constexpr u32 PackRgba(u32 r, u32 g, u32 b, u32 a = 255)
	{
		return r | (g << 8) | (b << 16) | (a << 24);
	}

	// Checks the fill rule (no cracks, no double hits, at every sample grid), perspective-correct texturing, depth,
	// clipping, culling, the alpha test, filtering and wrapping, the lighting, supersampling, the camera against
	// HandModel's projection, clearing and determinism, and logs a timing line for a mesh of a thousand triangles into
	// 256 x 256 ("(VR) MeshRaster self-test: N checks passed", or each failure and how many failed). Run by the
	// compositor under PCSX2_VR_HANDS_SELFTEST or PCSX2_VR_SELFTEST. Returns the number of checks that failed (0: all
	// passed).
	int SelfTest();
}
