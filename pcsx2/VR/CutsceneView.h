// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/EyeProjection.h"

#include "common/Pcsx2Defs.h"

// [VR] CutsceneStereo (tools/outbreak-vr/research/2026-10-22-cutscene-vr.md). The game's real-time cutscenes (File #1:
// scenario status 0x48BF60 == 4) are drawn by the engine from its own camera (eye, target, FOV and roll keyed by the
// cutscene player 0x5BD690 through 0x590400 / 0x5908A0, rebuilt into the view 0x3062E0 and the screen matrix 0x2771A0 by
// 0x345740 every frame). In VR that picture is shown through a world-locked window: the director's camera sits where
// the head was as the cutscene began (level, facing the same way), the window is the camera's image plane kWindowM ahead
// at exactly the camera's field of view (one-to-one: the scene keeps its real size), and each eye's layer is the scene
// through that window from where the eye really is (an off-axis view, mapped per eye as TrueStereo does). Leaning gives
// a little look-around through the window; the head's offset is softly clamped to kMaxOffsetM. The game's camera moves
// only inside the window; the room around it holds still, so the director's pans, dollies and cuts are not felt as
// motion. Pure math: no state, no threads.
namespace VR::CutsceneView
{
	// The window's distance from the cutscene camera (metres): the plane the game's 2D draws (letterbox, subtitles,
	// fades) sit on and where the frame's edges have no disparity. Things nearer than this come out of the window.
	static constexpr float kWindowM = 1.0f;
	// How far the head may lean from where it was as the cutscene began before the view stops following (metres; a soft
	// radial clamp: r' = R tanh(r / R), so small moves are one-to-one and the view eases to a stop near R).
	static constexpr float kMaxOffsetM = 0.15f;

	// A soft radial clamp of v (metres) to radius r.
	void SoftClamp(const float v[3], float r, float out[3]);

	// Both eyes' positions in the cutscene camera's frame (x right, y up, -z ahead; metres) from the head's pose relative
	// to the anchor (the head as the cutscene began, level: same frame): the head's offset softly clamped, each eye half
	// the IPD along the head's own (turned and rolled) x axis.
	void EyePositions(const float head_pos[3], const float head_quat[4], float ipd_m, float max_offset_m,
		float out[2][3]);

	// One eye's field of view (tangents left, right, up, down; left and down negative) through the window: the camera's
	// own window at window_m, seen from eye_m. False when the eye is at or past the window plane.
	bool WindowTangents(const EyeProjection::GameCamera& cam, const float eye_m[3], float window_m, float tan[4]);

	// Both eyes as EyeProjection eyes: not turned (the window faces the camera), placed at eye_pos, each with its own
	// window tangents, so the eye's layer spans the window exactly. False on a degenerate camera or eye.
	bool BuildEyes(const EyeProjection::GameCamera& cam, const float eye_pos[2][3], float window_m,
		EyeProjection::Eye out[2]);

	// The head relative to the anchor (anchor position and yaw about +y in the space the head is in, OpenXR conventions):
	// out_pos = R(-yaw) (head - anchor), out_quat = q(yaw)^-1 q_head.
	void HeadInAnchor(const float head_pos[3], const float head_quat[4], const float anchor_pos[3], float anchor_yaw,
		float out_pos[3], float out_quat[4]);

	// The window's edges as screen changes (evening review m8): the window is not a screen mode, so the mode switch's blink
	// does not cover it. It goes up or down only where it is not seen: at once while the view is black already (the mode
	// switch's blink, a door's veil) or when forced (the screen it is shown on is gone: first person, the tablet, tracking
	// lost), or when no blink can be drawn (the HUD off); otherwise a blink of its own starts and the flip waits for its
	// rise (rise_ms). Wanted back before that: nothing changes.
	struct WindowGate
	{
		bool shown = false;
		u64 due_ms = 0; // the flip waits for its blink until then (0: no flip waiting)
	};
	enum class GateStep
	{
		Hold,       // as it is (or still waiting for the blink)
		Flipped,    // shown changed now
		StartBlink, // start a blink now (due_ms set); if it cannot start, Flip at once
	};
	GateStep StepGate(WindowGate& g, bool want, bool black, bool can_blink, bool force, u64 now_ms, u32 rise_ms);
	void Flip(WindowGate& g, bool want);

	// Unit tests (vr_selftests): logs "(VR) Cutscene view self-test: ...".
	bool SelfTest();
} // namespace VR::CutsceneView
