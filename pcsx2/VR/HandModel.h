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
}
