// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

class GSTexture;

// [VR] MenuPanel (research/2026-10-13-menu-panel.md, ROADMAP road step 1 stage 1): a game screen no longer pulls the
// player out of first person. While one is up (the profile's camera.lookAt.pauseWhen, u32 0x720AA0 on File #1),
// CameraDriver keeps writing the camera from the head (camera only: nothing is written to the character, the sticks
// belong to the menu), so the game keeps rendering the room around the player in stereo. The screen's own draws come
// after the room's in each frame; the GS splits them off:
//  - the colour target is snapshotted at the frame's first overlay draw (SplitTracker), so the snapshot is the room
//    alone; the present shows the snapshot on the first-person screen instead of the finished frame (GetOutput);
//  - the finished frame against the snapshot gives the screen's own pixels (a compute pass: what the overlay draws
//    changed, opaque; the rest clear), which the compositor shows on a flat panel in front of the player, where the
//    menu pointer aims.
// With the switch off nothing here runs: no snapshot, no pass, no panel; CameraDriver hands the camera back as before.
namespace VR::MenuPanel
{
	// --- CPU thread (CameraDriver), every vsync ----------------------------------------------------------------------
	// Whether first person is kept through a game screen this vsync: the switch on, first person armed and a
	// pauseWhen screen up (held through the guard flicker as menu mode is).
	void PublishScreenUp(bool up);
	// Any thread.
	bool ScreenUp();
	// Whether first person may keep the camera under a screen: the switch on and nothing refusing the panel. The compositor
	// refuses it when it cannot show one (PacerCopies on, the panel's swapchain failed); with no XR session at all (the
	// headset-free tests) nothing refuses. CPU thread.
	bool Usable();
	// XRCompositor: why it cannot show the panel (a static string), or null when it can.
	void SetCompositorRefusal(const char* why);

	// --- Which draws are the screen's (pure; GS thread) ----------------------------------------------------------------
	// One draw as GSRendererHW sees it before it is rendered.
	struct DrawNote
	{
		u32 fbp = 0;          // FRAME.Block
		u32 zbp = 0;          // ZBUF.Block
		bool z_test = false;  // ZTE on with a test other than ALWAYS (drawn against the room's depth)
		bool z_write = false; // ZMSK off
		bool tme = false;     // textured
		bool fst = false;     // UV (screen-space texel) coordinates: never a perspective draw
		bool q_const = false; // one Q for every vertex (no perspective across the draw)
		bool z_const = false; // one Z for every vertex
		bool sprite = false;  // sprite primitives
		bool covers = false;  // covers (nearly) the whole frame buffer
		bool blend = false;   // alpha blending on (ABE): drawn over what is there, not replacing it
	};

	// Perspective: textured with STQ coordinates whose Q varies, or untextured with a depth that varies.
	bool IsPerspective(const DrawNote& d);
	// A depth clear: a depth-writing draw over the whole buffer without the depth test.
	bool IsClear(const DrawNote& d);
	// The room: a depth-tested perspective draw.
	bool IsRoom(const DrawNote& d);
	// A 2D overlay draw: flat (UV coordinates, one Q for the draw, or untextured at one depth), whether or not it is
	// depth-tested, and not a clear.
	bool IsOverlay(const DrawNote& d);
	// A screen's backdrop: a blended overlay over the whole frame (File #1 tints the picture behind every screen with a
	// few full-frame sprites and a full-frame translucent fill). While the split is on these are left out of the frame:
	// the room needs no tint (it is shown in 3D around the panel), and on the panel they would cover every pixel.
	bool IsBackdrop(const DrawNote& d);

	// The split point per colour target. The game draws a frame's room (depth-tested perspective draws into a colour
	// target with its depth buffer), then, while a screen is up, the screen's draws over it into the same colour target.
	// The tracker asks for a snapshot of that colour target just before the screen's first draw: its depth clear or its
	// backdrop when there is one (File #1 starts every screen seen with one or the other), else the first flat draw after
	// the room. Everything after it in that frame is the screen's, its own 3D too (the take-it prompt's turning item);
	// only when a plain overlay asked for the snapshot and the room carries on is it dropped (2D in the room). The frame
	// ends when its target has been shown (NoteShown) and is drawn into again.
	enum class SplitAction : u8
	{
		None,
		Snapshot, // copy the colour target at `snapshot_fbp` now, before this draw
	};
	struct SplitTracker
	{
		static constexpr u32 kSlots = 2; // File #1 alternates two colour buffers (0x0 and 0xE00)
		struct Slot
		{
			u32 fbp = 0;
			bool used = false;
			u32 room_draws = 0;    // room draws in this frame before the snapshot
			bool snap = false;     // a snapshot of this frame's room is held for it
			bool strong = false;   // the screen's clear or backdrop has been seen since (it is a screen, not 2D in the room)
			u32 overlay_draws = 0; // draws since the snapshot (the screen's)
			bool shown = false;    // shown since its last draw: the next draw begins a new frame
			u64 age = 0;           // when it was last drawn into (for reuse of the slot)
		};
		Slot slot[kSlots];
		u64 clock = 0;
		u32 snapshot_fbp = 0;   // SplitAction::Snapshot: the colour target to copy
		int snapshot_slot = -1; // and the slot it goes in
		bool strong_pending = false;
		bool skip = false;      // the draw just noted is a screen's backdrop: leave it out (IsBackdrop)

		// Every draw while the switch is on (split_on false: nothing starts a snapshot or is left out).
		SplitAction NoteDraw(const DrawNote& d, bool split_on);
		// The snapshot NoteDraw asked for was taken (false: it could not be; that frame goes without a split).
		void SnapshotTaken(bool ok);
		// Every vsync: the colour target shown.
		void NoteShown(u32 fbp);
		// At the present: the slot holding the room of colour target fbp, with at least one of the screen's draws after
		// it; -1 when there is none (the frame shows as drawn).
		int SlotForPresent(u32 fbp) const;
		void Reset();
	};

	// --- GS thread -------------------------------------------------------------------------------------------------------
	// Whether the GS splits this frame: the switch on and a screen up, or within a few vsyncs of one (frames drawn
	// while it was up are still on their way to the present). Read once per draw.
	bool SplitOn();
	// GSRendererHW, every draw: the tracker (and the PCSX2_VR_MENUTRACE draw trace). Returns the action; on Snapshot the
	// caller looks the colour target up and calls TakeSnapshot with its texture.
	SplitAction NoteDraw(const DrawNote& d, u64 draw_n, const char* trace_extra);
	// Whether the draw NoteDraw just saw is to be left out (a screen's backdrop while the split is on), after any snapshot
	// it asked for has been taken.
	bool SkipDraw();
	// PCSX2_VR_MENUTRACE=<vsyncs> (offline only; read once): the draws of that many vsyncs are logged as "(VR) MTRACE"
	// lines each time a screen comes up or goes, and again 1.5 s after it came up. GSRendererHW passes its extra fields
	// only while this is true.
	bool Tracing();
	void TakeSnapshot(GSTexture* colour);
	// GSRendererHW::GetOutput, every vsync while the switch is on: the displayed colour target (it ends that frame).
	void NoteShown(u32 fbp);
	// GSRendererHW::GetOutput, as Merge looks up the displayed colour target: the room snapshot to show instead of
	// `colour` (same size, format and layers), or null to show the target as drawn.
	GSTexture* RoomFor(u32 fbp, GSTexture* colour);
	// GSRendererHW, right after Merge (in the GS command buffer, before the present's submit): the screen's own pixels
	// of this present (the displayed target `colour` against its room snapshot) into the panel image, for the
	// compositor. src_x/src_y/w/h: the displayed rectangle in the target (scaled texels). False (and the panel image
	// invalid for this present) when there is no split this present.
	// u0..v1: where that rectangle shows in the shown picture, as fractions of it.
	bool ExportPanel(u32 fbp, GSTexture* colour, u32 src_x, u32 src_y, u32 w, u32 h, float u0, float v0, float u1, float v1);
	// No panel this present (the reason logged once each time it changes).
	void NoPanel(const char* why);
	// Every vsync (GS thread), before Merge.
	void OnVSync();

	// The panel image of the latest present, for the compositor (GS thread): a VkImage (R8G8B8A8_UNORM, w x h, in
	// TRANSFER_SRC_OPTIMAL after the present's submit) or null; serial counts exports.
	struct PanelInfo
	{
		bool valid = false;
		void* image = nullptr; // VkImage
		u32 width = 0;
		u32 height = 0;
		u64 serial = 0;
		// The displayed rectangle the panel covers, as fractions of the shown picture (the menu pointer's u, v).
		float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
		u32 changed_px = 0; // pixels the screen's draws changed, from the last readback (0: not read)
	};
	const PanelInfo& Latest();

	// The GS renderer is going away: the snapshots, the panel image and the pass freed.
	void Destroy();

	// The pure parts: the overlay test and the tracker.
	bool SelfTest();
}
