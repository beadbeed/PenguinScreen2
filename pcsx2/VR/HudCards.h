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
// The toast is a short line of text view-locked below the line of sight (recenters, brightness,
// controllers paused).
namespace VR::HudCards
{
	// Card slots, in the order the compositor submits them (the toast last, on top).
	static constexpr int kToast = 0;
	static constexpr int kCardCount = 1;

	// Toast image and quad: about 1.2 m ahead and 0.25 m below eye level, 0.5 m wide (about 24 deg, so
	// the image's 1024 pixels give the text 1.1 deg tall letters up to ~23 characters, smaller beyond).
	static constexpr u32 kToastWidth = 1024;
	static constexpr u32 kToastHeight = 160;
	static constexpr float kToastWidthM = 0.50f;
	static constexpr float kToastHeightM = kToastWidthM * static_cast<float>(kToastHeight) / static_cast<float>(kToastWidth);
	static constexpr float kToastDistanceM = 1.2f;
	static constexpr float kToastDropM = 0.25f;

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

	// Rasterise a card at full opacity into out (resized to the card's width x height).
	void RasterToast(std::vector<u32>& out, const std::string& text);

	// dst = src with every pixel's alpha scaled by opacity (0-1), for the fades.
	void ScaleAlpha(const u32* src, size_t count, float opacity, u32* dst);

	// The toast's pose in the head's VIEW space (position xyz, orientation xyzw): ahead and below,
	// tilted to face the eye.
	void ToastPose(float pos[3], float quat[4]);

	// The font, the fades and the placement maths. Runs once from the compositor's first HUD frame
	// when PCSX2_VR_HUD_SELFTEST is set.
	bool SelfTest();
}
