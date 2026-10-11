// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include <vector>

// First-person hands: a procedural low-poly glove (and a pistol when a weapon is raised), drawn on
// the CPU from the head's position into a small image that the compositor shows as a quad layer
// facing the head (an impostor). No XR or Vulkan types here, so it stays self-contained.
namespace VR::HandModel
{
	static constexpr int kLeft = 0;
	static constexpr int kRight = 1;

	// Square RGBA8 impostor image, the same byte order and unpremultiplied alpha as the lever cards
	// (ControlQuads::PackRgba); background alpha 0.
	static constexpr u32 kImageSize = 256;
	static constexpr float kQuadSizeM = 0.30f;
	static constexpr float kQuadSizeGunM = 0.45f;

	struct HandState
	{
		bool valid = false;
		// In the XR base space. pos is the controller's grip position (inside the handle); quat (xyzw)
		// is the controller's pointing frame: -Z forward along the barrel, +Y up, +X right.
		float pos[3] = {0.0f, 0.0f, 0.0f};
		float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		float grip = 0.0f;
		float trigger = 0.0f;
		// Right hand only: hold a pistol instead of the controller.
		bool gun = false;
		// With gun, shot feedback: kick 0-1 pitches the hand and gun up (7 deg) and back (1.5 cm) about the
		// grip; flash draws a muzzle flash; slide 0-1 moves the slide back (1.8 cm at 1); slide_locked holds it
		// fully back (an empty gun).
		float kick = 0.0f;
		bool flash = false;
		float slide = 0.0f;
		bool slide_locked = false;
	};

	// The player's character has a weapon raised (the right hand then holds the pistol).
	// Any thread; false until someone sets it.
	void SetGunHeld(bool held);
	bool GunHeld();

	// Where a hand's impostor quad goes: centred on the hand (or hand + gun), its +Z facing the eye,
	// "up" kept close to world up, big enough for the hand seen from the eye.
	struct ImpostorQuad
	{
		float centre[3] = {0.0f, 0.0f, 0.0f};
		float right[3] = {1.0f, 0.0f, 0.0f};
		float up[3] = {0.0f, 1.0f, 0.0f};
		float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // xyzw, quad orientation
		float size_m = kQuadSizeM;
	};

	// False when the hand can't be drawn (invalid pose, or the eye is inside the hand).
	// PlaceImpostor and RenderImpostor share scratch buffers: call them from one thread (the compositor's).
	bool PlaceImpostor(int hand, const HandState& h, const float eye[3], ImpostorQuad* out);

	// Renders the hand as seen from eye, in perspective, onto the quad plane (centre, unit right/up,
	// square side quad_size_m) into a kImageSize x kImageSize image.
	void RenderImpostor(int hand, const HandState& h, const float eye[3], const float quad_center[3],
		const float quad_right[3], const float quad_up[3], float quad_size_m, std::vector<u32>& out_rgba);

	// Test mode (PCSX2_VR_FAKE_HANDS): a hand held at a fixed offset from the head along its yaw,
	// pointing forward and slightly down, so the hands can be checked without controllers.
	HandState FakeHandState(int hand, const float head_pos[3], const float head_quat[4], bool gun);

	// Placement at the headset's display rate: the compositor's VR pacer moves a hand's image (and the laser and
	// the wrist card) with the controller every display frame, and the GS thread draws a hand again only when the
	// image no longer fits the view. Everything below is pure (no scratch state): any thread.

	// Moves a layer rigidly with its hand. The hand went from (ref_pos, ref_quat) to (now_pos, now_quat), so a
	// layer placed at (in_centre, in_quat) for the first pose goes to out = now * ref^-1 * in: out_quat =
	// (now * conj(ref)) * in_quat, out_centre = now_pos + (now * conj(ref)) rotating (in_centre - ref_pos).
	// Quaternions are xyzw, normalised here (identity when unusable); non-finite input copies in to out.
	void RePoseRigid(const float ref_pos[3], const float ref_quat[4], const float now_pos[3], const float now_quat[4],
		const float in_centre[3], const float in_quat[4], float out_centre[3], float out_quat[4]);

	// How far the view of a hand has moved from the one its image was drawn for, at ref_centre (a rigid point on
	// the hand: the image's centre): the angle between the directions to the eye in the hand's own frame, then and
	// now, and the eye's distance now over then. Not valid when either distance is under 5 cm or anything is not
	// finite.
	struct ViewDrift
	{
		bool valid = false;
		float angle_deg = 0.0f;
		float distance_ratio = 1.0f;
	};
	ViewDrift MeasureViewDrift(const float ref_pos[3], const float ref_quat[4], const float ref_eye[3],
		const float ref_centre[3], const float now_pos[3], const float now_quat[4], const float now_eye[3]);

	// A kept image is drawn again past this view drift, this change in the eye's distance (a ratio), this change in
	// grip or trigger, or this age.
	static constexpr float kRerasterDriftDeg = 2.0f;
	static constexpr float kRerasterDistanceRatio = 0.03f;
	static constexpr float kRerasterArticulation = 0.03f;
	static constexpr u64 kRerasterMaxAgeMs = 500;

	// Whether a hand image drawn for ref, seen from ref_eye and centred at ref_centre, has to be drawn again to show
	// now seen from now_eye: ref invalid, the pistol or the locked slide changed, a shot plays in either (kick, flash,
	// slide), grip or trigger moved past kRerasterArticulation, the view drifted past kRerasterDriftDeg or
	// kRerasterDistanceRatio (or cannot be measured), or the image is older than kRerasterMaxAgeMs.
	bool NeedsReraster(const HandState& ref, const float ref_eye[3], const float ref_centre[3], const HandState& now,
		const float now_eye[3], u64 age_ms);

	// Test aid (PCSX2_VR_FAKE_HANDS_SWEEP): sweeps a fake hand on the display clock t_ns (an XrTime), 2 s a cycle,
	// the left hand half a cycle behind the right: +-12 cm along x, +-5 cm along y at twice the rate, and +-0.35 rad
	// (20 deg) of yaw about world up applied before its own orientation. Grip, trigger and the pistol are untouched.
	void ApplyFakeSweep(int hand, s64 t_ns, HandState* h);

	// Checks the maths above; logs "(VR) Hands self-test: N checks passed", or each failure and how many failed
	// (PCSX2_VR_HANDS_SELFTEST). True when every check passed.
	bool SelfTest();
}
