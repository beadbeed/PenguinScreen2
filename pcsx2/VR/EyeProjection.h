// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

// True per-eye rendering ([VR] TrueStereo; tools/outbreak-vr/research/2026-10-21-true-stereo.md). The game renders one
// camera; its VU1 programs emit each vertex as X = fx x / w + cx, Y = fy y / w + cy in GS pixels (w = -z, the camera
// looking down -z, game units), with Q = 1/w and a Z affine in 1/w. That is a projective map, so the vertex an eye would
// see is a fixed function of the emitted one: back to the camera point through the game's intrinsics, into the eye's frame
// (its offset and turn from the camera), and through the eye's own projection, which spans the eye's field-of-view
// tangents over the same target. Pure math: no state, no threads.
namespace VR::EyeProjection
{
	// The game camera as its screen matrix says (File #1: EE 0x2771A0, fx = m[0], fy = m[5], cx = -m[8], cy = -m[9]),
	// and the target it draws: half width and height in GS pixels around (cx, cy).
	struct GameCamera
	{
		float fx = 0.0f;
		float fy = 0.0f;
		float cx = 2048.0f;
		float cy = 2048.0f;
		float half_w = 256.0f;
		float half_h = 224.0f;
		float units_per_m = 100.0f;
	};

	// The headset's eyes, relative to the game camera (the head with its roll taken out: the game's camera is kept
	// roll-level): the IPD, how far each view is turned outward (canted displays), each eye's field of view as OpenXR
	// tangents (left and down negative), and the head's roll about the view axis (positive = left ear down).
	struct EyeSpec
	{
		float ipd_m = 0.064f;
		float cant_rad = 0.0f;
		float tan[2][4] = {}; // [eye][left, right, up, down]
		float roll_rad = 0.0f;
	};

	struct Eye
	{
		// Camera point (game units) to eye point (game units): p_eye = m * (p, 1), rows x, y, z.
		float m[3][4] = {};
		// The eye's projection onto the target, in GS pixels: X = fx x / w + cx, Y = fy y / w + cy.
		float fx = 0.0f;
		float fy = 0.0f;
		float cx = 0.0f;
		float cy = 0.0f;
		// The OpenXR view relative to the game camera: position in metres, orientation (x, y, z, w), and the field of
		// view in radians (left, right, up, down) the layer was drawn with.
		float pos_m[3] = {};
		float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		float fov_rad[4] = {};
	};

	// Both eyes (0 left, 1 right). False when the camera or a field of view is degenerate.
	bool Build(const GameCamera& cam, const EyeSpec& spec, Eye out[2]);

	// Puts one built eye at another pose in the camera frame (metres; turned by yaw about the camera's y, then roll
	// about its view axis), keeping its projection: Build's own placement, and headset-free checks against renders
	// from a moved camera (PCSX2_VR_TRUESTEREO_TEST=pose,...).
	void PlaceEye(const GameCamera& cam, const float pos_m[3], float yaw_rad, float roll_rad, Eye* eye);

	// The game camera's own tangents as an EyeSpec field of view (both eyes the same): what a symmetric test eye sees.
	void CameraTangents(const GameCamera& cam, float tan[4]);

	// One emitted vertex (GS pixels X, Y and its w in game units) through an eye: the twin of the vertex shader's
	// TrueStereo mode. False when the point is behind the eye (w' under min_w).
	bool MapVertex(const GameCamera& cam, const Eye& eye, float x, float y, float w, float* x2, float* y2, float* w2,
		float min_w = 0.5f);

	// [VR] GameText: in first person the game's own 2D layer (its text: speech subtitles, "X Obtained", examine and
	// system lines; File #1 has no persistent HUD) is drawn at kGameTextScale of its size about the view centre, so text
	// the game puts near the bottom edge (y 385-396 of 448 for "Obtained", 324-339 for a subtitle) lands 10-25 deg below
	// the line of sight instead of 30-40. TrueStereo: the flat plane spans that much less of the view; without it: the
	// flat draws' vertices are moved toward the centre of the game's picture. The game's own text, untouched otherwise.
	static constexpr float kGameTextScale = 0.5f;

	// One coordinate moved toward centre by scale s (any unit, the centre in the same unit).
	inline float GameTextMap(float v, float centre, float s) { return centre + s * (v - centre); }

	// A GS vertex coordinate (12.4 fixed, as in GSVertex XYZ) moved toward centre_px (GS pixels), rounded and clamped.
	unsigned short GameTextMapFixed(unsigned short v, float centre_px, float s);

	// Which draws GameText treats as the game's text (review 2026-10-22 M1: "FST and not filling the scissor" also
	// caught particles, the game's muzzle flash, strip effects, partial fades). Text is a textured sprite draw with UV
	// coordinates (FST) on the 2D layer: one Z, at the depth format's maximum (File #1's text: Z 0xFFFFFFFD of 32 bits,
	// depth-tested GEQUAL, so they always pass; a world sprite such as blood, sparks or the flash sits at its own depth
	// below that). Not sampling the frame or depth buffer it draws into (a feedback pass), not filling the scissor (a
	// fade). The texture page is not checked: the game's text uses more than one (logged as GameText scales/leaves).
	struct GameTextDraw
	{
		bool tme = false;            // textured
		bool fst = false;            // UV coordinates
		bool sprite = false;         // sprite primitives
		bool z_const = false;        // one Z for every vertex
		bool z_layer = false;        // that Z is within 15 of the depth format's maximum (the 2D layer)
		bool samples_target = false; // the texture is the frame or depth buffer being drawn
		bool fills = false;          // covers 95% or more of the scissor
	};
	bool IsGameTextDraw(const GameTextDraw& d);

	// Degrees below the line of sight of 2D-screen row y (screen centre cy, half-height half_h, in the same unit), on a 2D
	// layer drawn over `fraction` of a view whose half-height tangent is tan_half (TrueStereo: 0.9 * scale).
	float GameTextDegreesBelow(float y, float cy, float half_h, float fraction, float tan_half);

	// Unit tests (vr_selftests): logs "(VR) Eye projection self-test: ...".
	bool SelfTest();
} // namespace VR::EyeProjection
