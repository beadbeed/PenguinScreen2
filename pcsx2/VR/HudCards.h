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
// Six cards: the toast, a short line of text view-locked below the line of sight (recenters, brightness,
// controllers paused, the item a hand reaches for in a belt pouch); the wrist card, on the back of the left
// wrist in first person, with the character's condition, an HP bar and the virus gauge read from the profile's
// hud.wrist block, and the equipped weapon's name and rounds (feedback.weapon); the
// comfort blink, a black quad over the whole view for snap turns and screen changes; the laser sight,
// a thin red beam from the pistol's muzzle along the aim while the weapon is raised in first person (in a
// menu the same slot carries the menu pointer's beam instead); the comfort vignette, a view-locked black
// ring that closes in on the edges of the view while first person turns or walks the view without the head;
// and the menu pointer's dot, lying on the screen where the controller points while a menu is up.
namespace VR::HudCards
{
	// Card slots. The compositor submits them in slot order, but PlaceCardLayer (below) puts three of them in under
	// the hands, which are nearer the eye: the laser (or the menu beam), over it the menu pointer's dot, and over both
	// the toast. The rest go over everything: the wrist card, then the vignette over the hands and the wrist card, the
	// blink last. So the farther layer is always drawn first: screen, beam, dot, toast (0.65 m), hands and wrist card
	// (0.3-0.5 m), vignette ring (0.5 m, out at the edges of the view, clear where the toast is), blink.
	static constexpr int kLaser = 0;
	static constexpr int kWrist = 1;
	static constexpr int kVignette = 2;
	static constexpr int kToast = 3;
	static constexpr int kBlink = 4;
	static constexpr int kPointer = 5;
	static constexpr int kCardCount = 6;

	// Puts card's layer into layers[0, layer_count) as the compositor submits them, with room for one more (the caller
	// checks the capacity). under_from is where the layers that go over the HUD's under-hands cards begin (the hands,
	// from BuildHandLayers); with nothing there it equals layer_count. The laser (or menu beam) and the menu pointer's
	// dot go in at under_from and move it past themselves, so the dot lands over the beam. The toast goes in at
	// under_from too but leaves it where it is, so a dot or beam placed after it still goes in under the toast: the
	// toast (0.65 m) is nearer than the screen the dot lies on and most of the beam, and farther than the hands, which
	// then hide it where they pass in front of it, as a panel behind them would be hidden, instead of the toast drawing
	// over a nearer hand (the item-name toast shows while a hand reaches into a pouch). Every other card goes on top.
	template <typename Layer>
	void PlaceCardLayer(int card, Layer* layers, u32& layer_count, u32& under_from, Layer layer)
	{
		const bool under_hands = (card == kLaser || card == kPointer || card == kToast);
		const bool moves_mark = (card == kLaser || card == kPointer);
		if (under_hands && under_from < layer_count)
		{
			for (u32 k = layer_count; k > under_from; k--)
				layers[k] = layers[k - 1];
			layers[under_from] = layer;
			layer_count++;
			if (moves_mark)
				under_from++;
		}
		else
		{
			// On top, either because the card goes over everything or because nothing is over the mark yet. A beam or
			// dot appended moves the mark past itself; an appended toast leaves it under itself (still the old
			// layer_count), so a later beam or dot goes in under the toast.
			layers[layer_count++] = layer;
			if (moves_mark)
				under_from = layer_count;
		}
	}

	// Toast image and quad: about 24 deg wide and 12 deg below the line of sight (so the image's 1024 pixels give
	// the text 1.1 deg tall letters up to ~23 characters, smaller beyond). It sits kToastDistanceM ahead, nearer
	// than any shipped profile's first-person near field brings the world (stereo.firstPerson nearest, kNearFieldMinM
	// below; GSRendererHW), so no counter or wall seen through it comes out in front of it in stereo. kToastDesignM is the
	// distance its size was first drawn for (0.5 m wide, 0.25 m down at 1.2 m): width and drop scale with the
	// distance, so the angles stay those.
	static constexpr u32 kToastWidth = 1024;
	static constexpr u32 kToastHeight = 160;
	static constexpr float kToastDistanceM = 0.65f;
	static constexpr float kToastDesignM = 1.2f;
	static constexpr float kToastWidthM = 0.50f * kToastDistanceM / kToastDesignM;
	static constexpr float kToastHeightM = kToastWidthM * static_cast<float>(kToastHeight) / static_cast<float>(kToastWidth);
	static constexpr float kToastDropM = 0.25f * kToastDistanceM / kToastDesignM;
	// The nearest any shipped profile's near field has put world geometry (stereo.firstPerson nearest: 0.75 m in
	// wave 4, 1.0 m now). The toast must stay nearer than this. NearDepthMetres can still be set down to 0.45 m by
	// hand, past the toast; that is the player's choice.
	static constexpr float kNearFieldMinM = 0.75f;
	static_assert(kToastDistanceM < kNearFieldMinM, "the toast must be nearer than the near field can bring the world");

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

	// Comfort blink: black over kBlinkRiseMs, held for kBlinkHoldMs, clear again over kBlinkFallMs (0.18 s in
	// all). It hides the jumps prediction can't: a snap turn shows the old frame turned aside with a black
	// band until the new view's frames arrive, about 85-100 ms later (inside the hold), and the screen
	// changing between the first-person and the world screen (made while black, see XRCompositor). Quick to
	// go dark so little of the jump shows, slower to clear.
	static constexpr u32 kBlinkRiseMs = 40;
	static constexpr u32 kBlinkHoldMs = 60;
	static constexpr u32 kBlinkFallMs = 80;
	// A plain black image, stretched by the quad layer. Not the smallest possible: a runtime turning down
	// a tiny swapchain would take every card down with it.
	static constexpr u32 kBlinkImageSize = 16;
	// View-locked just in front of the eyes, 4 m square at 0.3 m: about 163 degrees across each way, past
	// any headset's field of view.
	static constexpr float kBlinkDistanceM = 0.3f;
	static constexpr float kBlinkSizeM = 4.0f;

	// Laser sight: one quad 4 mm wide and 6 m long from just ahead of the impostor pistol's muzzle along the
	// aim, turned about its own length to face the head. No depth, so it fades out toward its far end
	// instead of stopping on what it points at. The image runs across the beam (soft red edges round a
	// hotter core) and along it (the fade, top row at the far end); 16 pixels across, the same floor as the
	// blink's image.
	static constexpr u32 kLaserImageWidth = 16;
	static constexpr u32 kLaserImageHeight = 256;
	static constexpr float kLaserWidthM = 0.004f;
	static constexpr float kLaserLengthM = 6.0f;
	// The beam starts this far ahead of the muzzle's face.
	static constexpr float kLaserMuzzleGapM = 0.02f;

	// Comfort vignette: view-locked like the blink, kVignetteDistanceM ahead and kVignetteSizeM square (about
	// 161 degrees across, past any headset's field of view). Its image is clear out to kVignetteClearFrac of the
	// vignette's radius and ramps (smoothstep) to opaque black at that radius, which lies about 58 degrees off
	// the line of sight (tangent kVignetteRadiusTan), inside a headset's view so the ramp is seen; past it, out
	// to the quad's edges, it stays opaque, so a wide headset never sees round the quad. Shown at the VR
	// VignetteStrength setting times CameraDriver::ComfortMotion(), so the ring only closes in while the view
	// moves without the head.
	static constexpr u32 kVignetteImageSize = 256;
	static constexpr float kVignetteDistanceM = 0.5f;
	static constexpr float kVignetteSizeM = 6.0f;
	static constexpr float kVignetteRadiusTan = 1.6f;
	static constexpr float kVignetteClearFrac = 0.45f;
	static_assert(kVignetteRadiusTan * kVignetteDistanceM < 0.5f * kVignetteSizeM,
		"the vignette's ring must fit on its quad");

	// Menu pointer (MenuPointer setting, while a menu is up on the world screen or the menu tablet): a dot lying on the
	// screen where the pointing hand's aim meets it, kPointerDeg across as seen from the eye (diameter 2 d tan(0.3 deg)
	// at distance d, at least kPointerMinM), kPointerLiftM off the screen's face; and a beam kBeamWidthM wide from
	// kBeamStartM ahead of the hand's aim pose to the dot. The beam rides the laser's slot and swapchain (16 x 256), so
	// it shows only while the laser is not placed. The dot looks plain off a cell, and warm with a ring on one.
	static constexpr u32 kPointerImageSize = 64;
	static constexpr float kPointerDeg = 0.6f;
	static constexpr float kPointerMinM = 0.006f;
	static constexpr float kPointerLiftM = 0.002f;
	static constexpr float kBeamWidthM = 0.003f;
	static constexpr float kBeamStartM = 0.02f;

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

	// Starts a comfort blink; any thread. Returns false, and does nothing, while the VR ComfortBlink
	// setting is off (as published by Poll, so the GS thread never reads EmuConfig). A blink already
	// running carries on from its current darkness and holds black again from now, so overlapping
	// triggers never lighten the view mid-blink. reason is for the log. Drawn only while a VR session runs.
	bool Blink(const char* reason = "blink");

	// Compositor: the blink's opacity now (0-1); false when no blink is on screen. Any thread.
	bool CurrentBlink(float* opacity);

	// Compositor, each frame: with PCSX2_VR_BLINK_TEST=1 a blink every 2 s.
	void MaybeTestBlink();

	// The VR LaserSight setting as published by Poll (on until the first vsync, as its default is), so the
	// GS thread never reads EmuConfig. Any thread.
	bool LaserSightEnabled();

	// The VR MenuPointer setting (the beam, the dot, the hands in menus and point-to-select) as published by Poll (on
	// until the first vsync, as its default is). Any thread.
	bool MenuPointerEnabled();

	// The VR MenuTablet setting (menus on a small tilted screen in front of the chest) as published by Poll (off until
	// the first vsync, as its default is). Any thread.
	bool MenuTabletEnabled();

	// The comfort vignette's strength as published by Poll: the VR VignetteStrength setting (clamped to its
	// range) while ComfortVignette is on, else 0 (off until the first vsync, as its default is), so the GS
	// thread never reads EmuConfig. Any thread.
	float VignetteStrength();

	// Compositor, each frame: with PCSX2_VR_VIGNETTE_TEST=1, true and *motion (may be null) going 0 -> 1 -> 0
	// every 2 s in place of CameraDriver::ComfortMotion(), at any time; false when the test is off.
	bool TestVignetteMotion(float* motion);

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
		// Rounds in the equipped gun (GameFeedback, feedback.weapon), drawn under the HP bar: "AMMO n", or the weapon's
		// name and rounds (below).
		bool has_ammo = false;
		u16 ammo = 0;
		// With has_ammo: the equipped weapon's item id. When ItemData knows it, its short name takes the place of AMMO
		// on that line ("45 AUTO 7"; a weapon without rounds, such as a knife, shows its name alone).
		bool has_item = false;
		u16 item_id = 0;
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
	// way GameFeedback does. Reads only, so the same online. Also publishes the ComfortBlink, LaserSight,
	// MenuPointer, MenuTablet and comfort vignette (ComfortVignette, VignetteStrength) settings.
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
	void RasterBlink(std::vector<u32>& out);
	void RasterLaser(std::vector<u32>& out);
	void RasterVignette(std::vector<u32>& out);
	// The menu pointer's dot (kPointerImageSize square): a soft disc, white with a dark rim off a cell, warm white with
	// a thin outer ring on one.
	void RasterPointer(std::vector<u32>& out, bool on_cell);
	// The menu pointer's beam, the laser's size: a cool white-blue core with soft edges, fading in over the first tenth
	// from the hand (the bottom row) and even from there to the screen (the top row).
	void RasterBeam(std::vector<u32>& out);

	// dst = src with every pixel's alpha scaled by opacity (0-1), for the fades.
	void ScaleAlpha(const u32* src, size_t count, float opacity, u32* dst);

	// The toast's pose in the head's VIEW space (position xyz, orientation xyzw): ahead and below,
	// tilted to face the eye.
	void ToastPose(float pos[3], float quat[4]);

	// The blink's pose in the VIEW space: straight ahead at kBlinkDistanceM, square to the line of sight.
	void BlinkPose(float pos[3], float quat[4]);

	// The vignette's pose in the VIEW space: straight ahead at kVignetteDistanceM, square to the line of sight,
	// its ring centred on it.
	void VignettePose(float pos[3], float quat[4]);

	// The wrist card's pose in the XR base space for a left hand given the way HandModel takes it (pos:
	// the controller's grip position; quat: its pointing frame, -Z forward, +Y up, +X right): on the
	// back of the wrist, its face out of the back of the hand and its text running toward the fingers,
	// so it reads upright when the wrist is turned to the face. facing_cos is the cosine of the angle
	// between the card's face and the direction to the eye. face_eye (test mode) turns the card to face
	// the eye instead, still at the wrist.
	void PlaceWrist(const float hand_pos[3], const float hand_quat[4], const float eye[3], bool face_eye,
		float out_pos[3], float out_quat[4], float* facing_cos);

	// The laser's quad in the XR base space for a right hand given the way HandModel takes it (pos: the
	// controller's grip position; quat: its pointing frame, -Z along the pistol's barrel): from
	// kLaserMuzzleGapM ahead of the impostor pistol's muzzle along -Z for kLaserLengthM. out_pos is the
	// beam's middle; the quad's +Y runs along the beam (so its image's top row is the far end) and its +Z is
	// turned about the beam toward the eye. False, and the frame goes without the beam, when the eye is on
	// the beam's line (no side of the quad faces it) or a pose is not usable.
	bool PlaceLaser(const float hand_pos[3], const float hand_quat[4], const float eye[3], float out_pos[3],
		float out_quat[4]);

	// A beam quad in the XR base space from origin along dir (need not be unit length) for length metres: out_pos is its
	// middle, its +Y runs along the beam (the image's top row at the far end) and its +Z is turned about the beam toward
	// the eye. PlaceLaser is this from just past the pistol's muzzle. False, and the frame goes without the beam, when
	// the eye is on the beam's line, the length is not positive or a value is not usable.
	bool PlaceBeam(const float origin[3], const float dir[3], float length, const float eye[3], float out_pos[3],
		float out_quat[4]);

	// The menu pointer's dot for a hit on the screen quad whose orientation is screen_quat (+Z its face normal): lying
	// flat on the screen kPointerLiftM off its face (out_quat is the screen's own), out_size across (metres) so it spans
	// kPointerDeg from the eye, at least kPointerMinM.
	void PlacePointer(const float hit[3], const float screen_quat[4], const float eye[3], float out_pos[3],
		float out_quat[4], float* out_size);

	// Condition thresholds, labels, keys, the font (every item name included), the wrist card's weapon line, the fades,
	// the blink, the laser, the vignette, the menu pointer's dot and beam, the placement maths (the toast nearer than
	// the near field at its old angles) and the layer order (PlaceCardLayer). Runs once (the
	// compositor's first HUD frame or the first Poll) when PCSX2_VR_HUD_SELFTEST is set.
	bool SelfTest();
}
