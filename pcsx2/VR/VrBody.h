// SPDX-FileCopyrightText: 2026 PenguinScreen2 contributors
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

// VR body ([VR] VrBody, off by default; tools/outbreak-vr/research/2026-10-21-vr-body.md): a VR player is shown as the
// game's OWN character model, its own skeleton posed by the player's real head and hands. Nothing is drawn over it.
//
// - The skeleton (File #1, every player character checked: the same 36 core bones, a woman's model has 7 more for hair
//   and skirt): the chest 11; the neck 20 and head 21 (the face 22-27 under it); per side a clavicle (12 / 16), the
//   shoulder joint (13 / 17), the elbow (14 / 18), the wrist (15 / 19), a hand bone at the wrist (32 / 28) and three
//   finger bones under it. Each bone is a 0x190-byte struct reached through *(record + 0x34) + 0x820 + 4 * bone: +0x00
//   its world matrix, +0x40 its local one (relative to its parent; world = local x parent world, row vectors, row 3 the
//   joint), +0xC4 its index, +0xC8 its parent's struct.
// - The hook: the characters' body pass FUN_00345210 calls each character's draw (`jalr *(record + 0x18)`, at 0x345314).
//   By then the game has built this frame's world matrices from the animation; the draw itself turns them into the skin
//   matrices it sends to VU1 (FUN_001922B0: inverse bind x world). Cave V (0xFD000) is called there instead: for a record
//   named in the host block (0xFF300), it rewrites the world matrices of the listed bones as (a matrix the host gives,
//   relative to the chest) x (the chest's world matrix of this frame), with the game's own matrix multiply FUN_00190C50,
//   then calls the draw. Only matrices the game rebuilds from the animation every frame; never a record's game state.
// - The host (Poll, CPU thread, every vsync): for the partner (VrPresence's poses, in private co-op or the offline
//   loopback test) and, with VrBodySelf, the local character, it reads the bones' local matrices (the animation's) and
//   the chest's world matrix, poses the head and neck to the VR head (clamped), solves each arm with a two-bone IK so the
//   wrist reaches the VR hand (an elbow hint, the elbow's bend and the wrist's swing and twist clamped), turns the hand
//   to the controller, blends back to the animation when a hand is not tracked or out of reach, and writes the result.
//   The legs, hips and walking stay the game's own animation.
namespace VR::VrBody
{
	// A rigid frame as the game stores it, row vectors: r[0..2] the frame's axes and t its origin, all in the parent's
	// coordinates; a point p (row) in the frame is p * r + t in the parent's.
	struct Frame
	{
		double r[3][3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
		double t[3] = {0.0, 0.0, 0.0};
	};

	// child (in parent's coordinates) then parent (in its parent's): the child in the grandparent's. Pure.
	Frame Compose(const Frame& child, const Frame& parent);
	// The inverse of a rigid frame. Pure.
	Frame Inverse(const Frame& f);
	// A point and a direction into / out of a frame. Pure.
	void ToFrame(const Frame& f, const double world[3], double local[3]);
	void DirToFrame(const Frame& f, const double world[3], double local[3]);
	// From the game's 4 x 4 (16 floats, row major): false when not finite or the axes are not unit and perpendicular
	// within 2% (a scaled or broken matrix). The axes are re-orthonormalised. Pure.
	bool FromGame(const float m[16], Frame* out);
	void ToGame(const Frame& f, float m[16]);
	// A world matrix with the character's uniform scale (File #1 scales each character, e.g. Kevin 1.05): the rotation,
	// the scale (its axes' common length, within 2%) and the origin. Pure.
	bool FromGameScaled(const float m[16], Frame* out, double* scale);
	// a x b for plain 4 x 4 row-major matrices (the game's order: a's rows through b). Pure.
	void Mul44(const float a[16], const float b[16], float out[16]);

	// The bones this needs. arm[0] is the side whose bones sit at the model's +X (the clavicle's offset), arm[1] at -X.
	struct ArmMap
	{
		int clavicle = -1, upper = -1, fore = -1, wrist = -1;
	};
	struct BoneMap
	{
		int chest = -1, neck = -1, head = -1;
		ArmMap arm[2];
	};
	static constexpr int kMaxBones = 64;
	// File #1's player characters (every one seen: 36 or 43 bones).
	BoneMap DefaultMap();
	// The map against a model's parent table (parents[i] the index of bone i's parent, -1 for none): the neck's and both
	// clavicles' parent is the chest, the head's the neck, each arm's chain clavicle -> upper -> fore -> wrist, every
	// index below n. why says what failed. Pure.
	bool CheckMap(const BoneMap& m, const int* parents, int n, std::string* why);
	// Bone b and every bone under it, each after its parent; returns how many (at most max). Pure.
	int Subtree(const int* parents, int n, int b, int* out, int max);

	// Two-bone IK in one frame: from the shoulder s, an upper bone of length l1 and a lower of l2, the wrist toward
	// target. The reach is clamped to what the chain can do: at most 0.999 (l1 + l2) and at least the reach with the
	// elbow bent to its limit (max_bend_deg from straight). The elbow goes toward the pole direction (its part
	// perpendicular to the reach; a pole along the reach falls back to any perpendicular). Pure.
	struct IkResult
	{
		double elbow[3] = {};
		double wrist[3] = {};
		double reach = 0.0;   // |wrist - s| after the clamp
		bool clamped = false; // the target was out of the chain's range
	};
	bool SolveTwoBone(const double s[3], double l1, double l2, const double target[3], const double pole[3],
		double max_bend_deg, IkResult* out);

	// The head's turn relative to the chest (chest frame: +Y up, +Z forward): yaw about +Y (toward +X positive), pitch
	// (down positive), roll about the forward. Angles from a forward and an up; a frame (rows X, Y, Z: Z the forward, Y
	// the up) from angles. Pure.
	void HeadAngles(const double fwd[3], const double up[3], double* yaw, double* pitch, double* roll);
	void HeadFrame(double yaw, double pitch, double roll, double r[3][3]);

	// Rotations: a slerp between two (w 0 gives a, 1 gives b), and the angle between them in degrees. Pure.
	void BlendRot(const double a[3][3], const double b[3][3], double w, double out[3][3]);
	double RotAngleDeg(const double a[3][3], const double b[3][3]);

	struct Tuning
	{
		double head_yaw_deg = 75.0, head_pitch_down_deg = 55.0, head_pitch_up_deg = 45.0, head_roll_deg = 35.0;
		double neck_share = 0.4;      // of each head angle, the rest on the head
		double max_elbow_bend_deg = 150.0;
		double wrist_swing_deg = 75.0; // the hand's bend off the forearm
		double wrist_twist_deg = 90.0; // and its twist about it
		double forearm_twist_share = 0.5;
		double wrist_back_cm = 6.0;    // the wrist joint behind the controller's grip, along its forward
		double reach_slack = 1.3;      // a target past this many arm lengths from the shoulder blends the arm back
		double behind_cm = 25.0;       // and one further behind the chest than this
		double blend_per_s = 4.0;      // weight change a second (0.25 s in or out)
	};

	// One character's VR targets, in its chest's frame (game units).
	struct Targets
	{
		bool head = false;
		double head_fwd[3] = {0.0, 0.0, 1.0};
		double head_up[3] = {0.0, 1.0, 0.0};
		bool hand[2] = {false, false}; // per arm (arm[0] / arm[1] of the map), not per VR hand
		double hand_pos[2][3] = {};    // the controller's grip
		double hand_fwd[2][3] = {{0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}};
		double hand_up[2][3] = {{0.0, 1.0, 0.0}, {0.0, 1.0, 0.0}};
	};
	struct Weights
	{
		double head = 0.0;
		double arm[2] = {0.0, 0.0};
	};

	// What the solve gives: every changed bone's frame relative to the chest (the cave makes each one's world matrix
	// frame x chest world), the arms' wrist positions and how they did.
	static constexpr int kMaxOut = 24;
	static constexpr int kMaxLocals = 12; // neck, head, and per arm clavicle, upper, fore, wrist
	struct Pose
	{
		int count = 0;
		int bone[kMaxOut] = {};
		Frame rel[kMaxOut];
		double wrist[2][3] = {};     // chest frame, after the blend
		double wrist_err[2] = {};    // |the IK wrist - its target| before the blend (game units)
		bool reach_clamped[2] = {};
		bool head_clamped = false;
		// The same pose as LOCAL matrices (each relative to its parent) of the few bones it turns: the neck and head, and
		// per arm the clavicle (its animated local, so the arm hangs off the chest rigidly), upper, fore and wrist. Cave U
		// writes these in the character's update, so the game's own world matrices, held items and hit spheres follow.
		int lcount = 0;
		int lbone[kMaxLocals] = {};
		Frame lrel[kMaxLocals];
		// SolveAnchored only: which entries are drawn bones (the rest have zero axes: their vertices vanish at one point),
		// and per arm the shoulder the IK started from (after any slide toward the hand) and the elbow (chest frame).
		bool keep[kMaxOut] = {};
		bool drawn[2] = {};
		double shoulder[2][3] = {};
		double elbow[2][3] = {};
	};

	// The solve for one character. locals[i]: bone i's local frame (the game's +0x40, the animation's); parents as
	// CheckMap. Bones of a part whose weight is 0 are left out (the game's own matrices stay). Pure.
	bool SolveBody(const BoneMap& m, const Frame* locals, const int* parents, int n, const Targets& tg, const Weights& w,
		const Tuning& tu, Pose* out);
	// The weights' targets: a part counts only while tracked, and an arm only while its target is within reach_slack arm
	// lengths of the shoulder and not further than behind_cm behind the chest. Pure.
	void WantWeights(const BoneMap& m, const Frame* locals, const Targets& tg, const Tuning& tu, Weights* want);
	// cur eased toward want at blend_per_s for dt seconds. Pure.
	void EaseWeights(Weights* cur, const Weights& want, double dt, const Tuning& tu);

	// --- Your own arms anchored to the headset ([VR] VrBodySelfStyle Forearms / Arms; research/2026-10-27-body-anchor.md) ---
	//
	// Measured offline (2026-10-27): the recentre puts the eye 155 cm over the character whatever the seated height, but the
	// game's own skeleton has its head bone 6 cm IN FRONT of that eye and level with it, and the shoulders only 12-14 cm under
	// it (one of them ahead of the eye in the gun stance): the upper arms' sleeves come up to 5-8 cm under the eye, so you
	// look out of Kevin's neck. The anchored styles leave the game's skeleton alone and build your shoulders from your head
	// (a neck model), turn the body between your head and your hands, solve each arm from there to the controller as the
	// eye sees it and draw only the game's own forearms and hands (Forearms) or the whole arms (Arms). Draw-only: cave V at
	// the draw, after the hit tests; the game's arms, its held items and its hurt spheres stay its animation's.
	enum class SelfStyle : int
	{
		Forearms = 0,
		Arms = 1,
		GameShoulders = 2,
	};
	struct AnchorTuning
	{
		double pivot_down_m = 0.075, pivot_back_m = 0.08;      // the neck's pivot from the eye, in the head's frame
		double shoulder_down_m = 0.14, shoulder_back_m = 0.01; // the shoulders' midpoint from the pivot, the body upright
		double hands_share = 0.7;        // the body's yaw: the head's, this share of the way to the tracked hands' midpoint
		double max_body_yaw_deg = 60.0;  // never further than this from the head's (the neck turns the rest)
		double yaw_rate_per_s = 8.0;     // how fast it follows (1 / its time constant)
		double protract_m = 0.08;        // a shoulder slides this far toward a hand out of reach; past that the arm stops short
		double seat_m = 0.45, seated_eye_m = 1.20; // the calibration: an eye height over the floor against the nominal seated one
		double scale_min = 0.85, scale_max = 1.15;
	};
	// The body's scale from the eye's height over the floor at the recentre: (eye - seat) / (seated_eye - seat), clamped;
	// 1 when that height is unknown or no seated eye height (not finite, under 0.9 m or over 1.5 m). Pure.
	double CalibrateScale(double eye_over_floor_m, const AnchorTuning& tu);
	struct AnchorBody
	{
		double pivot[3] = {};
		double fwd[3] = {0.0, 0.0, -1.0}; // the body's forward, level
		double right[3] = {1.0, 0.0, 0.0};
		double shoulder[2][3] = {};       // [0] the left shoulder joint, [1] the right
		double yaw = 0.0;                 // the body's yaw (radians, counter-clockwise from -Z)
		bool hands_used = false;          // the hands turned the body
	};
	// VR space (OpenXR: +X right, +Y up, -Z forward; metres). The eye's pose (its position, forward and up), the hands
	// tracked and their grips, the calibrated scale, the shoulders' half width (the model's own), dt since the last call.
	// The body's yaw goes hands_share of the way from the head's to the hands' midpoint (seen from the pivot, when that is
	// 15 cm out and not behind), eased at yaw_rate_per_s, at most max_body_yaw_deg off the head's; yaw_state carries it
	// between calls (NaN: none yet). Pure.
	bool AnchorShoulders(const double head_pos[3], const double head_fwd[3], const double head_up[3], const bool hand[2],
		const double hand_pos[2][3], double scale, double half_width_m, double dt, const AnchorTuning& tu, double* yaw_state,
		AnchorBody* out);
	// The bones cave V writes for your own anchored arms, in order: the drawn ones (per arm: the upper arm with Arms, the
	// forearm, the wrist and every bone under it), then bones given zero axes at one point so their vertices vanish there:
	// with Forearms each upper arm at its elbow; the clavicles at the chest's joint (Forearms) or at their shoulder (Arms);
	// the chest itself and the neck's subtree (head, face, hair) at the chest's joint. At most kMaxOut (the head's last
	// bones are left out first: each then falls on its own joint, every bone round it on one point); every other bone the
	// cave's collapse takes to its own joint (the waist, the hips, the legs). The cave's reference bone must not be in it
	// (the host uses the root). Pure.
	enum class HideKind : u8
	{
		Drawn = 0,
		AtChest = 1,
		AtElbow = 2,
		AtShoulder = 3,
	};
	struct HidePlan
	{
		int count = 0;
		int bone[kMaxOut] = {};
		HideKind kind[kMaxOut] = {};
		int arm[kMaxOut] = {}; // the arm (map arm 0 / 1) a bone belongs to, -1 for none
	};
	bool SelfHidePlan(const BoneMap& m, const int* parents, int n, SelfStyle style, HidePlan* out);
	// One character's anchored targets in one frame of its skeleton (the host uses the root bone's, which has none of the
	// chest's animated sway; game units at the model's scale): the hands as Targets (the grips as the eye sees them), the
	// anchored shoulder joints per arm, the body's forward and up, the chest's joint.
	struct AnchorTargets
	{
		Targets tg;
		double shoulder[2][3] = {};
		double body_fwd[3] = {0.0, 0.0, 1.0};
		double body_up[3] = {0.0, 1.0, 0.0};
		double chest[3] = {}; // the chest's joint in this frame: where the hidden bones' vertices go
	};
	// Per tracked arm: the shoulder slid toward the wrist's target by up to protract (game units) when it is out of reach,
	// then the two-bone IK (the reach clamped: the arm never stretches) and the hand on the controller, as SolveBody; the
	// entries in SelfHidePlan's order, the untracked arm's bones all hidden at the chest. False with nothing drawn. Pure.
	bool SolveAnchored(const BoneMap& m, const Frame* locals, const int* parents, int n, const AnchorTargets& at,
		const Tuning& tu, SelfStyle style, double protract, Pose* out);

	// The host block (body.block, 0x100 bytes) read by caves U and V: "VBD1", a stamp, the attach function and its first word, then two
	// slots of 0x40 from +0x10: u32 record, u32 table, u8 count, u8 ref (the chest), u8 flags, u8 nloc, u32 keep mask lo and
	// hi (flag kCollapse: the bones whose axes stay), two attaches (u16 record offset of the held item's slot byte, u8 the
	// hand bone, pad), u8 bones[kMaxOut]. Each table holds count 64-byte matrices (the bones' frames relative to the
	// chest, ToGame).
	static constexpr u32 kBlockMagic = 0x31444256u; // "VBD1"
	static constexpr u32 kSlotOffset = 0x10, kSlotBytes = 0x40;
	static constexpr int kSlots = 2; // 0 the partner, 1 the local character (VrBodySelf)
	static constexpr u8 kFlagAttach = 1, kFlagCollapse = 2, kFlagSelf = 4, kFlagLocals = 8;
	// Slot flag 0x10 (NativeHands; cave W at 0xFD378 reads it, slot 1 only): the game draws this record's held weapon in the
	// hand cave V posed. Cave W is the profile's site at 0x560C74 (the held-object pass FUN_00560B60): for the focus
	// character it takes the old skip unless the block's slot 1 names the record and has this flag.
	static constexpr u8 kFlagWeapon = 0x10;
	// Flag kLocals (the normal way): the slot's first nloc entries are LOCAL matrices that cave U writes in the tick's
	// world build (FUN_00346810, before the world matrices, the held items' attach and the hit tests; the animation
	// writes every local again after it, so the host always reads the animation's own), and it stamps the slot's word
	// +0x38 with the block's stamp. The entries after them are frames relative to the chest that cave V turns into world
	// matrices at the draw: the head's subtree, since the game's own head-look layer (FUN_0062F790, in the players' tick
	// after the world build) rewrites bone 21 and its subtree. Without the flag (the fallback, PCSX2_VR_BODY_DRAWTIME=1)
	// every entry is a draw-time frame.
	struct BlockSlot
	{
		u32 record = 0;
		u32 table = 0;
		u8 count = 0;
		u8 ref = 0;
		u8 flags = 0;
		u8 nloc = 0; // kLocals: how many of the entries are locals (the rest are draw-time frames)
		u32 keep[2] = {0, 0};
		u16 attach_offset[2] = {0, 0};
		u8 attach_bone[2] = {0, 0};
		u8 bones[kMaxOut] = {};
	};
	// Into the slot's 16 little-endian words. Pure.
	void PackSlot(const BlockSlot& s, u32 words[16]);

	// Whether a slot may use cave U (kFlagLocals: the bones' LOCAL matrices written in the tick's world build, before the
	// world matrices, the held items and the hit tests; night review C1). Only strictly offline (no network adapter, the
	// camera driver's net mode Offline) and not in the draw-time fallback. Online (private co-op, or anywhere) never, for no
	// slot, the local one included: there the hurt spheres and the held weapon of every character stay the game's own and
	// hits stay one-to-one with what each console simulates; the partner is posed at the draw only (cave V, render-only,
	// after the hit tests). The profile closes cave U's site online too (whenSetting VrBodyLocals, never co-op live). Pure.
	bool UseLocals(bool offline, bool eth_enabled, bool drawtime);
	// Whether the host may zero the block and its tables (night review M3): the disc CRC now is the one it was written
	// under and the block's first word is still our magic. Pure.
	bool MayClear(u32 crc_now, u32 crc_written, u32 block_word0);
	// The slot's flags: si 0 the partner, 1 the local character (kSelf | kCollapse); with locals kFlagLocals, else at the
	// draw (the partner's held items re-attached while an arm is posed). Pure.
	// native_weapon (NativeHands, the local character's draw-only slot): the held items go back on the posed hands at the draw
	// (cave V's attach, flag 1) and cave W lets the focus character's held object be drawn (kFlagWeapon).
	u8 SlotFlags(int si, bool locals, bool arms, bool native_weapon = false);

	// --- NativeHands: one set of hands (research/2026-10-30-native-hands.md) ---
	//
	// The host says per VR hand, each vsync, whether the game is drawing that hand (your own anchored arms: the slot was
	// written, cave V and the focus character's draw site are in; the weapon too through cave W). The compositor then asks
	// NativeHandsUpdate every present with its own gate (first person armed, no menu up): a hand is decided "native" only
	// after the raw state has held for kNativeHoldMs, and goes back to the overlay hand THE SAME FRAME the raw state drops,
	// so there is never a frame with no hand and a pose that flaps never flickers between the two. Pure step below.
	static constexpr u64 kNativeHoldMs = 200;
	struct NativeHold
	{
		bool on[2] = {false, false};
		u64 since_ms[2] = {0, 0}; // when the raw state last turned live (0: not live)
	};
	// One hand's step. Returns whether the hand is native now. Pure.
	bool NativeHoldStep(NativeHold* h, int hand, bool raw, u64 now_ms, u64 hold_ms);
	// CPU thread (Poll): the raw state per VR hand (0 left, 1 right) as of steady time now_ms; weapon: the right hand's
	// weapon is drawn by the game too (cave W in, the attach function known).
	void NativeHandsPublish(const bool hand[2], bool weapon, u64 now_ms);
	// Compositor (any thread, each present): the raw state if it was published within kNativeFreshMs and gate_ok, held;
	// returns whether `hand` is native now and logs the changes ("(VR) NativeHands: ..."). `fresh_ms` is a test hook.
	static constexpr u64 kNativeFreshMs = 150;
	bool NativeHandsUpdate(u64 now_ms, bool gate_ok, bool out_hand[2], bool* out_weapon);
	// The last decision for a hand (any thread; no update). PhysicalMelee asks it to leave its hit-stop off.
	bool NativeHandsLive(int hand);
	// The shift from GameFromVr to where the eye sees a VR point in the game: eye_forward along the view's level forward (game
	// units), published by Poll with the anchored solve and read by the gun ray so the shot starts at the drawn muzzle. False
	// when it is not fresh (over kNativeFreshMs old).
	bool NativeSeenShift(float out_g[3]);
	// The weapon as the game draws it in the posed hand (the resolved grip pose: the solved, clamped wrist plus the game's own
	// attach entry), in the game's world (the eye's shift included: where it is drawn) and, for the laser, in VR space.
	// Published by Poll, one vsync old at most; the gun ray, the melee shape and the laser read it, so the shot leaves the
	// barrel you see and the hit is where the weapon is drawn. False unless the right hand is native and the pose fresh.
	struct NativeWeapon
	{
		double origin[3] = {};  // the model's origin
		double axes[3][3] = {}; // the model's axes (unit rows)
		double muzzle[3] = {};  // the type's muzzle point
		double barrel[3] = {};  // the barrel: the model's -X
		bool vr_valid = false;
		double vr_muzzle[3] = {}; // the same muzzle and barrel in VR space (metres), for the laser
		double vr_barrel[3] = {};
	};
	bool NativeWeaponNow(NativeWeapon* out);
	// The attach maths for the self-tests and the note: where the game puts a held weapon's model origin, its barrel and
	// its muzzle, for a wrist frame and a weapon-type entry. wrist: the hand bone's frame (axes the game's rows: row 0 the
	// bone's +X, fingers along -X for the -X wrist, row 2 +Z the thumb), grip in the bone's frame times scale, euler the
	// entry's turn (radians, X then Y then Z as the game's row vectors), muzzle in the model's space. Pure (game units).
	struct AttachedWeapon
	{
		double origin[3] = {};  // the model's origin in the world frame wrist is given in
		double axes[3][3] = {}; // the model's X, Y, Z axes there (rows)
		double muzzle[3] = {};  // the muzzle point
		double barrel[3] = {};  // the barrel direction: the model's -X
	};
	AttachedWeapon AttachWeapon(const Frame& wrist, const double grip[3], double scale, const double euler[3], const double muzzle[3]);

	// CPU thread, every vsync after VR presence's Poll: writes or clears the host block. Reads the bones; writes only the
	// block, its matrix tables (both in free RAM) and nothing else.
	void Poll();
	// VM shutdown, reset and ELF change: forgets the block without writing guest memory (night review M3). Clears are only
	// ever written while the disc CRC is the one the block was written under and the block reads our magic.
	void Shutdown();
	// CPU thread, a state was loaded: the next Poll zeroes a block reading our magic (the state's, night review m3).
	void OnStateLoaded();

	// The math above (IK reach, clamps and singularities, the head clamp, the blend), the bone map against File #1's
	// parent tables, the cave's block layout. Logs "(VR) Body self-test: N checks passed" or each failure.
	bool SelfTest();
}
