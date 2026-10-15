// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/HeadPose.h"

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

	// --- Calm menus ([VR] MenuPanelRoom, MenuPanelRoomBrightness; research note, "Calm menus") ------------------------
	// Alex's first session with the panel (2026-10-14): "the inventory is within my eyes, but I can still see the
	// background moving around". The room behind the panel is the game's 30 fps view, about 100 ms behind the head, so
	// with the panel crisp in front it swims. Three ways to show it:
	//  - Freeze: the camera holds the pose it was last written from as the screen came up. The game keeps drawing the
	//    room from there (it still runs: a zombie still moves), every frame is placed at that render pose, so the room
	//    stays put in the world as the head turns, dimmed, with black past its edges (the head-locked edge fill is off
	//    while it holds). As the screen goes the camera is written from the head again; the compositor places each frame
	//    where it was rendered from, so the world does not jump, only the picture's edge catches up with the head.
	//  - Dim: the camera follows the head as before, the room dimmed.
	//  - Live: as first played.
	// No layer, no swapchain: the dim goes through the presentation lift's own shader pass (VR::PresentationLift).
	enum class RoomMode : u8
	{
		Freeze = 0, // the values of Pcsx2Config::VROptions::MenuPanelRoomMode
		Dim = 1,
		Live = 2,
	};

	// CPU thread (CameraDriver), every vsync: the setting, for the GS thread, and whether the camera is held for a screen
	// this vsync (the compositor turns the edge fill off then).
	void PublishRoom(RoomMode mode, float brightness, bool frozen);
	// Any thread.
	bool RoomFrozen();

	// What the room behind the panel looks like at the dim's full effect (pure).
	struct RoomLook
	{
		float brightness = 1.0f; // of the picture as shown in play
		float saturation = 1.0f;
	};
	constexpr float kRoomSaturation = 0.8f; // a little greyer too: calmer, and the panel's colours stand out
	constexpr float kRoomDimInS = 0.25f;    // the dim comes in over this
	constexpr float kRoomDimOutS = 0.2f;    // and goes over this
	RoomLook RoomLookFor(RoomMode mode, float brightness);
	// The dim's amount (0 none, 1 full) eased toward target (0 or 1) over dt seconds.
	float EaseRoomDim(float amount, float target, float dt);
	// The look at an amount of the dim (1: full, 0: as in play).
	RoomLook RoomLookAt(const RoomLook& full, float amount);
	// The shader multiplies the colour by its brightness before its gamma (out = (c * b) ^ (1 / gamma)): to make the
	// picture `dim` times as bright after the gamma, b is multiplied by dim ^ gamma.
	float DimBeforeGamma(float dim, float gamma);
	// GS thread, once per present after GetOutput (VR::PresentationLift): the room's look this present, eased in while
	// the shown frame is a split room (the screen on the panel, not in the picture) and the mode is not Live, out after.
	// False when it is as in play.
	bool RoomLookNow(RoomLook* out);

	// The held camera pose (pure; CameraDriver, every vsync camera.lookAt drives the camera).
	struct RoomFreeze
	{
		bool frozen = false;
		HeadPose::Snapshot pose; // the camera pose held: the last one written before the screen came up
		HeadPose::Snapshot held; // what Step returns: pose's orientation and position, the live head's stamps
		// screen: a game screen up with first person kept (camera only); freeze: RoomMode::Freeze; last_written: the pose
		// the camera was last written from (null: none yet); live: the head now. Returns the pose to write the camera from
		// (no prediction: it is not moving), or null for the head as usual. began / ended: the hold started / ended now.
		const HeadPose::Snapshot* Step(bool screen, bool freeze, const HeadPose::Snapshot* last_written,
			const HeadPose::Snapshot& live, bool* began, bool* ended);
		void Reset() { *this = RoomFreeze{}; }
	};

	// --- The panel's place (pure; XRCompositor) -------------------------------------------------------------------------
	// 1.0 m ahead of the eyes, 0.12 m below them, 0.80 m tall (about 44 deg; 1.07 m wide at 4:3), tilted to face the eyes,
	// placed along the yaw the head looks as the screen comes up (its pitch left out: the item screen opens with a look
	// down at the belt), and held there in the world while the head looks around it. Only a turn of more than
	// kPanelReplaceDeg away from it brings it round to the new yaw, gliding (kPanelGlideS) rather than jumping.
	constexpr float kPanelDistanceM = 1.0f;
	constexpr float kPanelHeightM = 0.80f;
	constexpr float kPanelDropM = 0.12f;
	constexpr float kPanelReplaceDeg = 60.0f;
	constexpr float kPanelGlideS = 0.15f; // time constant
	// The yaw the head looks along (OpenXR: forward -Z, yaw counter-clockwise from above), from its orientation x, y, z, w;
	// `fallback` when it looks straight up or down.
	float PanelYawOf(const float q[4], float fallback);
	// The panel's pose for eyes at `eye` and a yaw: position and orientation (x, y, z, w).
	void PlacePanel(const float eye[3], float yaw, float pos[3], float quat[4]);
	struct PanelPlacer
	{
		bool placed = false;
		bool gliding = false;
		float yaw = 0.0f;      // where it stands now
		float eye[3] = {};     // the eyes it stands in front of
		float target_yaw = 0.0f;
		float target_eye[3] = {};
		// As the screen comes up: in front of the head.
		void Show(const float eye_now[3], float head_yaw);
		// Every present while it is up: true when the head has turned far enough away that it set off for the head's yaw
		// now (logged by the caller).
		bool Update(const float eye_now[3], float head_yaw, float dt);
		void Hide() { placed = false; gliding = false; }
	};

	// The pure parts of the calm menus: the look, the ease, the gamma, the hold, the placement and the re-place.
	bool FeelSelfTest();
}
