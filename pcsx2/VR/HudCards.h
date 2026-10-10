// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include <cstddef>
#include <string>
#include <vector>

// In-headset HUD cards: small CPU-rasterised RGBA images the compositor shows as quad layers. This file
// holds the card model (what each card says, and when), the rasteriser with its bitmap font, and the
// placement maths; no XR or Vulkan types, so it stays self-contained. XRCompositor owns the swapchains,
// uploads the pixels and submits the layers, and only while a VR session runs.
//
// Two cards: the toast, a short line of text view-locked below the line of sight (recenters, brightness,
// controllers paused), and the wrist card, on the back of the left wrist in first person, with the
// character's condition, an HP bar and the virus gauge read from the profile's hud.wrist block.
namespace VR::HudCards
{
	// Card slots, in the order the compositor submits them (the toast last, on top).
	static constexpr int kWrist = 0;
	static constexpr int kToast = 1;
	static constexpr int kCardCount = 2;

	// Toast image and quad: about 1.2 m ahead and 0.25 m below eye level, 0.5 m wide (about 24 deg, so
	// the image's 1024 pixels give the text 1.1 deg tall letters up to ~23 characters, smaller beyond).
	static constexpr u32 kToastWidth = 1024;
	static constexpr u32 kToastHeight = 160;
	static constexpr float kToastWidthM = 0.50f;
	static constexpr float kToastHeightM = kToastWidthM * static_cast<float>(kToastHeight) / static_cast<float>(kToastWidth);
	static constexpr float kToastDistanceM = 1.2f;
	static constexpr float kToastDropM = 0.25f;

	// Wrist card image and quad: about the size of a large watch face, held just off the glove.
	static constexpr u32 kWristWidth = 256;
	static constexpr u32 kWristHeight = 160;
	static constexpr float kWristWidthM = 0.09f;
	static constexpr float kWristHeightM = kWristWidthM * static_cast<float>(kWristHeight) / static_cast<float>(kWristWidth);
	// The wrist card fades in once its face points at the head within ~50 degrees, and out again past
	// ~57 (cosines; the gap keeps it from flickering on the edge).
	static constexpr float kWristShowCos = 0.643f;
	static constexpr float kWristHideCos = 0.545f;

	// Condition thresholds as fractions of max HP. The game shows Caution at 25% and Danger at 18%, so
	// Danger's line lies between them; Caution's upper edge is not verified yet.
	static constexpr float kDangerBelow = 0.20f;
	static constexpr float kCautionBelow = 0.60f;

	// Cards fade in and out over this long.
	static constexpr float kFadeSeconds = 0.15f;

	// Same byte order as the lever cards and hands (ControlQuads::PackRgba): R in the low byte,
	// unpremultiplied alpha in the high byte, sRGB-encoded colour.
	constexpr u32 Rgba(u32 r, u32 g, u32 b, u32 a = 255)
	{
		return (a << 24) | (b << 16) | (g << 8) | r;
	}

	// Shows text on the toast card for about `seconds` (fades included), replacing any toast on screen.
	// Any thread. Lower-case letters are drawn as capitals; characters outside the font are left blank.
	// Nothing is drawn without a running VR session (the toast just expires).
	void Toast(const std::string& text, float seconds = 2.0f);

	struct ToastView
	{
		std::string text;
		u32 serial = 0;       // changes with every Toast() call: the text needs rastering again
		float opacity = 0.0f; // fades included
	};

	// Compositor: the toast to show now; false when none is on screen. Any thread.
	bool CurrentToast(ToastView* out);

	// Compositor, each frame: with PCSX2_VR_HUD_TEST=1 a test toast every 3 s that cycles through
	// the whole font.
	void MaybeTestToast();

	// The wrist card's data, read each vsync from the local character's record.
	struct WristData
	{
		bool has_hp = false; // HP read and plausible (max > 0, hp <= max)
		u32 hp = 0;
		u32 hp_max = 0;
		bool has_virus = false;
		float virus_pct = 0.0f; // 0-100
		bool has_bleed = false;
		bool bleeding = false;
	};

	enum class Condition : u8
	{
		Unknown = 0, // no HP to judge by
		Fine,
		Caution,
		Danger,
	};

	Condition ConditionOf(const WristData& d);

	// CPU thread, every vsync right after CameraDriver::Apply: reads the record Apply resolved, the same
	// way GameFeedback does. Reads only, so the same online.
	void Poll();

	// Any thread: true while first person is armed with the record resolved and the profile has a
	// hud.wrist block; *out (may be null) gets the last vsync's reads.
	bool GetWrist(WristData* out);

	// Changes whenever the drawn wrist card would, so it is uploaded only then. no_data: the test-mode
	// card shown without a profile or first person ("NO DATA").
	u64 WristKey(const WristData& d, bool no_data);

	// Rasterise a card at full opacity into out (resized to the card's width x height).
	void RasterToast(std::vector<u32>& out, const std::string& text);
	void RasterWrist(std::vector<u32>& out, const WristData& d, bool no_data);

	// dst = src with every pixel's alpha scaled by opacity (0-1), for the fades.
	void ScaleAlpha(const u32* src, size_t count, float opacity, u32* dst);

	// The toast's pose in the head's VIEW space (position xyz, orientation xyzw): ahead and below,
	// tilted to face the eye.
	void ToastPose(float pos[3], float quat[4]);

	// The wrist card's pose in the XR base space for a left hand given the way HandModel takes it (pos:
	// the controller's grip position; quat: its pointing frame, -Z forward, +Y up, +X right): on the
	// back of the wrist, its face out of the back of the hand and its text running toward the fingers,
	// so it reads upright when the wrist is turned to the face. facing_cos is the cosine of the angle
	// between the card's face and the direction to the eye. face_eye (test mode) turns the card to face
	// the eye instead, still at the wrist.
	void PlaceWrist(const float hand_pos[3], const float hand_quat[4], const float eye[3], bool face_eye,
		float out_pos[3], float out_quat[4], float* facing_cos);

	// Condition thresholds, labels, keys, the font, the fades and the placement maths. Runs once (the
	// compositor's first HUD frame or the first Poll) when PCSX2_VR_HUD_SELFTEST is set.
	bool SelfTest();
}
