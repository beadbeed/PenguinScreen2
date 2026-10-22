// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/HeadPose.h"

namespace VR::ProfileDB
{
	struct CameraPadLook;
}

namespace VR::CameraDriver
{
	struct PadLookState
	{
		int gate = 0;
		int latch = 0;
	};

	float PadLookDeflection(const ProfileDB::CameraPadLook& pl, float yaw_deg, PadLookState& st);

	void Apply();

	// True while camera.lookAt is driving the game camera (any thread).
	bool LookAtActive();

	// Comfort vignette: how much the view is moving without the head right now, 0-1. The larger of a smooth stick
	// turn's rate against lookAt.smoothTurnDegPerSec and walking (the move stick's push, or the character's own
	// speed against a brisk walk), eased in over about 0.15 s and out over 0.3 s. Snap turns count nothing (the
	// comfort blink covers them). Published by Apply (CPU thread) only while the VR ComfortVignette setting is on
	// and camera.lookAt drives the camera; 0 at once otherwise, after a savestate load, and when Apply stops
	// running (pause, stall). Any thread.
	float ComfortMotion();

	// The head pose camera.lookAt drove the game camera with at (or just before) time_ms, on the
	// SteadyNowMs() clock, so the compositor can show each frame where it was rendered from instead
	// of gluing an old frame to the face. False when nothing recent is recorded. Any thread.
	bool FirstPersonPoseAt(u64 time_ms, HeadPose::Snapshot* out);
	u64 SteadyNowMs();

	// GS thread: the head pose the frame being presented was rendered from, matched through the game's
	// view matrix and sent down the GS queue in step with the frames. False when not known (no
	// viewMatrix in the profile, first person not active, or no frame matched yet). turn_yaw (radians,
	// OpenXR yaw, may be null) is how much further round the frame belongs because the view has been
	// turned by the stick (or the character's heading) since it was rendered; about 0 through a smooth
	// turn that lookAt.predict renders ahead. Each call also measures the prediction horizon.
	bool RenderPose(HeadPose::Snapshot* out, float* turn_yaw);

	// GS thread, alongside RenderPose: the vertical FOV (degrees) the frame being presented was rendered with, from the
	// same matched write. False when not known (no matched frame, or camera.fov does not size the first-person screen).
	bool RenderFov(float* fov_deg);

	// First-person field of view (VR setting FirstPersonFov). camera.fov is taken as the game camera's vertical FOV in
	// degrees when the profile's flat screen.firstPerson is sized to it (ScreenMatchesFov); the first-person screen then
	// follows the FOV each frame was rendered with. Auto fits the FOV to the headset on each first-person arm
	// (AutoFirstPersonFovDeg), easing it in from camera.fov over kFovEaseVsyncs; it changes while armed only once, when
	// the headset's eyes first arrive. Profile writes camera.fov unchanged. The maths below is pure.
	inline constexpr float kMaxAutoFovDeg = 130.0f;
	inline constexpr float kDefaultFovMarginDeg = 6.0f;
	inline constexpr u32 kFovEaseVsyncs = 30;

	// The vertical FOV that covers both eyes plus margin_deg. fov_deg[eye][L, R, U, D] in degrees and cant_deg (how far
	// the views are turned outward) as XRCompositor::EyeInfo has them (left and down negative). The game's projection is
	// symmetric about its axis, so it takes twice the further of the eyes' furthest up and down reach (each plus half the
	// margin), widened if a picture `aspect` times as wide as tall would not reach their furthest left or right (plus half
	// the margin) either; that side reach is measured from the head's forward, so views turned outward add their cant to
	// it (max(-L, R) + max(cant_deg, 0)). Clamped to [profile_fov_deg, max(profile_fov_deg, kMaxAutoFovDeg)];
	// profile_fov_deg when the eyes are not usable.
	float AutoFirstPersonFovDeg(const float fov_deg[2][4], float cant_deg, float aspect, float margin_deg, float profile_fov_deg);

	// The height of a flat screen distance_m away that spans fov_deg vertically: 2 * distance * tan(fov / 2). 0 for a
	// FOV outside 1-179 deg or a distance not above 0.
	float ScreenHeightForFov(float distance_m, float fov_deg);

	// True when a flat screen distance_m away and height_m tall spans fov_deg (within 2%): the profile's first-person
	// screen is sized to camera.fov read as a vertical FOV in degrees, so that FOV may be refitted and the screen follows.
	bool ScreenMatchesFov(float distance_m, float height_m, float fov_deg);

	// from + (to - from) * smoothstep(step / kFovEaseVsyncs).
	float EaseFov(float from, float to, u32 step);

	// What camera.fov writes, for the compositor (the first-person screen's size, its log) and auto stereo. Any thread.
	struct FovState
	{
		bool sizes_screen = false; // camera.fov is a vertical FOV in degrees that sizes the first-person screen
		bool auto_fit = false;     // the target is fitted to the headset (FirstPersonFov = Auto, eyes known)
		float fov_deg = 0.0f;      // written on the latest armed vsync (0: none yet)
		float target_deg = 0.0f;   // what this arm eases to
		float profile_deg = 0.0f;  // camera.fov's own value (the floor)
		float margin_deg = 0.0f;   // FirstPersonFovMarginDeg as applied
		float aspect = 0.0f;       // the first-person screen's aspect the fit used
		float reach_up_deg = 0.0f, reach_down_deg = 0.0f, reach_side_deg = 0.0f; // the eyes' furthest reach (auto_fit)
		const char* source = "";   // why: "auto", or the reason the profile's value is used
	};
	FovState GetFovState();

	// The measured head-pose-to-present horizon in seconds (clamped to 0-0.12, 0 until measured): how far
	// ahead lookAt.predict leads the head. Any thread.
	float HeadPredictionHorizon();

	// How fast RenderPose's turn_yaw is changing right now, in radians per second of OpenXR yaw: a smooth stick turn
	// in progress (0 without one, or while first person is not active). The VR pacer turns the first-person screen
	// on by it toward each display time between GS frames. Any thread.
	float ScreenTurnRate();

	// PCSX2_VR_FAKE_HEADPOSE: the fake head's yaw (radians, OpenXR, CCW positive) at time_ms on the
	// SteadyNowMs() clock, so the compositor can compare where a frame is placed with where the fake head is
	// when it is shown. False (yaw untouched) when the fake head is off. Any thread.
	bool FakeHeadYawAt(u64 time_ms, float* yaw);

	// Any thread. The recenter happens on the next armed vsync, which then shows a "Recentered" toast
	// unless toast is false (for a caller that announces it itself, like the runtime-recenter follow).
	void RequestRecenter(bool toast = true);

	// CPU thread, after Apply: true while first person is armed (guards holding, head pose valid) and this
	// vsync resolved camera.base; *base (may be null) is that record, the local character's in Outbreak.
	// guards_now (optional): this vsync's camera guards held too, not just the disarm grace after they failed
	// (overlay data such as the GAME overlay's tables is only resident then).
	bool LocalRecord(u32* base, bool* guards_now = nullptr);

	// The local slot (an indexed camera.base's index byte, File #1 u8 0x48BF7D), debounced. Offline the game itself
	// sets that byte to 1, then 2, while it runs the AI partners (0x588C30) and puts 0 back, so a vsync can land on a
	// partner's slot (research/2026-10-13-near-cull.md; nightly review M1). A new value is taken only after it reads the
	// same on `need` consecutive samples (one per running vsync); a read that differs keeps the last stable value.
	// Pure.
	struct StableSlot
	{
		bool known = false;  // a value has held for `need` samples since the last Reset
		u32 stable = 0;      // the slot in use while known
		u32 candidate = 0;   // the latest read and how many samples in a row it has held
		u32 run = 0;
		u32 ignored = 0;     // samples that read something other than `stable` while known (saturates)
		// One sample; true while known (*out, may be null, the stable slot).
		bool Update(u32 raw, u32 need, u32* out = nullptr);
		void Reset() { *this = StableSlot{}; }
	};
	inline constexpr u32 kSlotStableVsyncs = 8;

	// CPU thread, after Apply: the debounced local slot (StableSlot), the one camera.base, LocalRecord and every
	// owner check of the local player's items must use. False until it has held kSlotStableVsyncs running vsyncs (and
	// after a savestate load, a game change or no VM, until it holds again). Never read 0x48BF7D directly.
	bool LocalSlot(u32* slot);

	// Pure: StableSlot's rules (a partner's flicker never becomes the slot; a real change does, after the hold).
	bool SelfTestStableSlot();

	// --- Code caves (camera.caves; nightly review M2, b2, d2) ---
	// A cave's range against our words: ram[k] is what the word at from + 4k holds, ours[k] our word there (0 in a gap
	// between entries and in a NOP slot). Clear: every word 0. Ours: every word exactly ours. Foreign: anything else
	// (a word that is neither, or part ours and part zero); *first_bad (may be null) then names the first word that is
	// neither 0 nor ours, or the first word that differs from ours for a part-written cave.
	enum class CaveRegion
	{
		Clear,
		Ours,
		Foreign,
	};
	CaveRegion CheckCaveRegion(const u32* ram, const u32* ours, size_t n, size_t* first_bad = nullptr);

	// A memory view for the cave install (the guest's in the emulator, an array in the self-test).
	struct CaveMemory
	{
		u32 (*read)(void* ctx, u32 address) = nullptr;
		void (*write)(void* ctx, u32 address, u32 value) = nullptr;
		void* ctx = nullptr;
	};
	// All or nothing: true when the cave at `from` holds exactly `ours` afterwards. A Clear range gets every word written
	// and read back; a read-back mismatch zeroes what was written and fails. A Foreign range is not written at all.
	bool InstallCave(const CaveMemory& mem, u32 from, const u32* ours, size_t n, CaveRegion* seen = nullptr);
	// Zero the words that hold exactly our word (a foreign word is left as it is). Returns how many were zeroed.
	size_t ZeroCave(const CaveMemory& mem, u32 from, const u32* ours, size_t n);
	// A cave's body may be zeroed only once its sites have been back to stock for kCaveDrainFrames of the game's frame
	// counter (and at least kCaveDrainMinVsyncs vsyncs; kCaveDrainNoCounterVsyncs without a counter), and the EE's pc
	// and $ra are outside [from, to): a game function the cave called may still return into it.
	inline constexpr u32 kCaveDrainFrames = 3;
	inline constexpr u32 kCaveDrainMinVsyncs = 3;
	inline constexpr u32 kCaveDrainNoCounterVsyncs = 8;
	bool CaveDrainDone(bool has_counter, u32 counter_at_start, u32 counter_now, u32 vsyncs, u32 pc, u32 ra, u32 from, u32 to);

	// Pure: the cave rules above over an array (a foreign word refuses the whole install, a read-back failure rolls back,
	// the drain waits for the counter and for pc / $ra to leave).
	bool SelfTestCaves();

	// CPU thread, after Apply: true while the game runs, the camera guards pass (or failed for no longer than
	// the profile's disarm grace) and this vsync resolved camera.base, whether or not first person is armed. Valid while grabbed, down or dead too (it ignores
	// releaseWhen, the head pose and the network mode). For reads only (GameFeedback's hurt and weapon cues, and the
	// grab breakout's grabbed checks); resolved only for profiles with a feedback or breakout block.
	bool LocalRecordAny(u32* base);

	// CPU thread: true while the network mode Apply last resolved is Offline (adapter off and the game not in network
	// play); false in private co-op and when restricted. Hand targeting runs only offline.
	bool NetworkOffline();

	// CPU thread: true while the network mode Apply last resolved is private co-op (the adapter on, our private server,
	// [VR] PrivateCoopFirstPerson on). [VR] PhysicalMeleeOnline acts only then.
	bool PrivateCoop();

	// --- The headset's space against the game's (hand targeting; reads only, nothing here writes the game) ---
	//
	// What camera.lookAt does to place the eye, as numbers a hand position can be mapped with. ApplyLookAt publishes one
	// on each vsync it writes the camera (first person armed, no menu): the character's position, the eye height and
	// forward, the TRUE base yaw (the character's heading or the camera's own yaw anchor; not the stick-predicted
	// cam_base the frame is written with), the recenter's head position and yaw, and the scale. OpenXR space: right +X,
	// up +Y, forward -Z, metres, yaw counter-clockwise about +Y. Game space: centimetres, Y up, heading 0 faces +Z,
	// (sin y, cos y) on the floor plane, y grows with yaw_sign (1: counter-clockwise like the headset's).
	//
	//   d      = vr - ref_pos                                         (metres from the recenter's head)
	//   fwd    = -dx sin(ref_yaw) - dz cos(ref_yaw)                    (the recenter yaw taken out: forward / right)
	//   right  =  dx cos(ref_yaw) - dz sin(ref_yaw)
	//   game.x = char.x + u (fwd sin b + right sin(b - yaw_sign pi/2))   b = base_yaw, u = units_per_meter
	//   game.z = char.z + u (fwd cos b + right cos(b - yaw_sign pi/2))
	//   game.y = char.y + eye_height + u dy
	//
	// which is exactly the eye ApplyLookAt writes (LookAtEye), less eye_forward: the game puts its eye eye_forward units
	// ahead of that along the view yaw (sin yaw_cam, cos yaw_cam), so a head at VR point H has its game EYE at
	// GameFromVr(H) + eye_forward * (sin yaw_cam, 0, cos yaw_cam), and VrFromGame(eye) is H moved eye_forward / u metres
	// along the head's horizontal forward (File #1: 10 cm), not H. yaw_cam is cam_base plus yaw_sign times the head's yaw
	// since the recenter. With yaw_sign -1 the map is a reflection (the game's yaw grows the other way); the inverse still
	// holds. The map follows the character: it changes as the character walks and as the base yaw turns, so a game point
	// is only as old as the latest publish (GetMapping is false once that is over 250 ms old, which is also every vsync
	// the camera is not driven: disarmed, a cutscene, a menu up without [VR] MenuPanel; with it the camera keeps being
	// driven, camera only, under a game screen).
	struct Mapping
	{
		bool valid = false;            // numbers usable: finite, units_per_meter non-zero, a recenter reference taken
		float character[3] = {};       // the character's position (camera.lookAt.position), game units
		float eye_height = 0.0f;       // game units over character[1] the recentered head's eye sits
		float eye_forward = 0.0f;      // game units ahead of the head along the view yaw (not part of the map)
		float base_yaw = 0.0f;         // radians, the true base yaw
		float ref_pos[3] = {};         // the head position at the recenter, metres
		float ref_yaw = 0.0f;          // the head's yaw at the recenter, radians
		float units_per_meter = 0.0f;  // game units per metre
		float yaw_sign = 1.0f;
		float pitch_sign = 1.0f;       // camera.lookAt.pitchSign (ProjectionCheck)
		u64 stamp_ms = 0;              // SteadyNowMs when published
	};

	// The latest published mapping, false when there is none or it is over 250 ms old. Any thread.
	bool GetMapping(Mapping* out);

	// Pure: false for an unusable mapping or a point that is not finite. The two are exact inverses (in double, so the
	// float outputs round trip within a few hundredths of a millimetre at File #1's coordinates).
	bool GameFromVr(const Mapping& m, const float vr[3], float game[3]);
	bool VrFromGame(const Mapping& m, const float game[3], float vr[3]);
	// The same with the latest published mapping (GetMapping), false without one.
	bool GameFromVr(const float vr[3], float game[3]);
	bool VrFromGame(const float game[3], float vr[3]);

	// CPU thread, hand targeting's activation line: the mapping's self-consistency on live numbers. eye_err_mm: for the
	// head position head_vr, the eye ApplyLookAt would write (with eye_forward taken as 0) mapped back with VrFromGame
	// against head_vr; round_trip_mm: the worst of GameFromVr then VrFromGame over the head, extra_vr (may be null) and
	// two fixed offsets from the head. False without a mapping.
	bool MappingCheck(const float head_vr[3], const float extra_vr[3], double* eye_err_mm, double* round_trip_mm);

	// What hand targeting's projection check compares for one game point (see ProjectionCheck).
	struct ProjectionResult
	{
		float game_yaw_deg = 0.0f, game_pitch_deg = 0.0f; // where the game's own view matrix puts the point
		float head_yaw_deg = 0.0f, head_pitch_deg = 0.0f; // where the head the frame was rendered from sees VrFromGame(point)
		float diff_deg = 0.0f;                            // the angle between the two directions
	};

	// CPU thread, after Apply: for a game point (centimetres) the direction from the game's eye, in the frame of its view
	// (yaw to the right and pitch up, in degrees from the view's forward), taken from the game's own view matrix (the 4x4
	// at camera.lookAt.viewMatrix, row vectors, the camera looking down -z, as TrackRenderPose reads it) against the
	// direction from the head to the same point mapped into the headset's space (VrFromGame), in the frame of the head pose
	// that view was rendered from (the newest matched frame), with the head's roll dropped as the camera drops it and the
	// eye moved eye_forward ahead as the game moves it.
	//
	// What agreement (a difference of a degree or less, steady as the head turns and moves) means: the map's position,
	// yaw, handedness and scale put a world point where the game's own camera sees it, so a hand position mapped with
	// GameFromVr is where that hand is in the picture the game rendered. What it does NOT prove: that the trigger volume
	// matches the pistol you can see (the volumes are 2-5 times the objects and their middles are not the models'
	// middles: this only checks the point it is given); anything about display latency (the frame is shown some tens of
	// milliseconds after the head pose it was rendered from, and the compositor places it by that pose; the hand pose
	// sampled for the poll is a different instant again); or the eyes (each eye is half the IPD to a side of the head the
	// check uses). Only meaningful while the character stands still and the view does not turn: the matrix is the newest
	// matched frame's, up to a few vsyncs old, mapped with the mapping of this vsync. False without a mapping, a view
	// matrix read this vsync that matched a write, or a head pose with a position.
	bool ProjectionCheck(const float game_point[3], ProjectionResult* out);

	void OnStateLoaded();

	// Why the camera is not armed (the DISARMED log line), for telemetry.
	enum class DisarmReason : u8
	{
		Armed = 0,
		NoVM = 1,
		NoProfile = 2,   // no profile, or it has no camera section
		SwitchedOff = 3, // VR, the head-tracked camera setting, or split-screen
		NotRunning = 4,  // paused
		NoHeadPose = 5,
		GuardFailed = 6,
		OnlineRestricted = 7, // online, and not in private co-op (obsrv, an unknown server, or the setting is off)
	};

	// What Apply() saw on the last vsync, for the PINE test runner (MsgVRTelemetry) and later the
	// flight recorder. Any thread.
	struct Telemetry
	{
		u64 vsync = 0;
		bool armed = false;
		DisarmReason disarm_reason = DisarmReason::NoVM;
		u32 guard_fail_vsyncs = 0;
		bool look_at_active = false;
		bool pause_when_hit = false; // lookAt.pauseWhen passed (a menu is open)
		bool weapon_raised = false;  // lookAt.aim stance
		bool aim_moving = false;     // walk-and-shoot owns the move stick
		bool yaw_anchor_valid = false;
		float yaw_anchor = 0.0f;   // game yaw, radians
		float base_yaw_now = 0.0f; // game yaw minus yawSign * the recenter yaw, radians (true, not predicted)
		u32 match_age = 0;         // how many writes back the newest matched frame was (0: none)
		u32 frames_matched = 0;
		bool online_safe = false;
		bool test_head = false;     // the head pose came from PINE test input
		float prediction_ms = 0.0f; // lookAt.predict horizon a smooth turn is rendered ahead by (0: off or not measured)
	};

	Telemetry GetTelemetry();

	bool SelfTestAssembler();

	bool SelfTestMath();

	// [VR] DoorVeil (with SeamlessDoors): the room change around a door is shown black. Started by a room load
	// (camera.roomLoad) that begins while first person is armed; held through the load and until first person is back
	// and the frame on screen is one drawn from the head (a matched render pose) for kShownVsyncs; let go if first person
	// does not come back within kNoReturnVsyncs of the load's end, and after kMaxVsyncs in all. Pure: Apply feeds it.
	struct DoorVeil
	{
		enum class Phase : u8
		{
			Idle,
			Loading, // the room load runs
			Waiting, // the load is over; waiting for first person's own frames on screen
		};
		struct In
		{
			bool enabled = false;      // the switches (SeamlessDoors, DoorVeil, VR + HeadCamera) and the VM running
			bool fp_before = false;    // first person was armed on the previous vsync (its disarm grace included)
			bool loading = false;      // camera.roomLoad holds
			bool armed = false;        // first person armed this vsync
			bool ours_shown = false;   // the frame on screen is one drawn from the head (a matched render pose)
			// The game's newest view matrix was built from first person's write (its frame is on its way to the screen).
			// With the head held still the matched pose is never published (no new view to match), so this alone also
			// lets go, after kNewestVsyncs (the frames in flight, about 3 at 30 fps, have reached the screen by then).
			bool newest_ours = false;
		};
		static constexpr u32 kShownVsyncs = 2;
		static constexpr u32 kNewestVsyncs = 8;
		static constexpr u32 kNoReturnVsyncs = 90;
		static constexpr u32 kMaxVsyncs = 1800;
		Phase phase = Phase::Idle;
		u32 total = 0;   // vsyncs since the veil began
		u32 waited = 0;  // vsyncs since the load ended
		u32 shown = 0;   // vsyncs in a row first person's own frames have been on screen
		u32 newest = 0;  // vsyncs in a row the newest view matrix has been first person's
		bool gave_up = false; // the last veil ended on a timeout
		// This vsync: whether the view is held black.
		bool Step(const In& in);
	};

	// [VR] GrabFacePull: the camera's yaw anchor eased toward target (radians, game yaw) over dt seconds, at most
	// kGrabPullMaxDegS and kGrabPullGain times the error (so it slows into the target), and nothing within
	// kGrabPullDoneDeg. *rate_frac: the turn's share of the top speed (for the comfort vignette).
	static constexpr float kGrabPullMaxDegS = 20.0f;
	static constexpr float kGrabPullGain = 1.5f;
	static constexpr float kGrabPullDoneDeg = 3.0f;
	float GrabPullStep(float anchor, float target, float dt, float* rate_frac = nullptr);

	// [VR] GrabFirstPerson: whether lookAt.releaseWhen hands the camera back this vsync. released: an entry holds;
	// grabbed: the profile's breakout.grabbed checks all hold (grabbed and still in normal play, so never down or dead).
	constexpr bool LookAtReleasedFor(bool released, bool grabbed, bool grab_first_person)
	{
		return released && !(grab_first_person && grabbed);
	}

	bool SelfTestGrabDoor();
}
