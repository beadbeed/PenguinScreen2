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

	// The host block (body.block, 0x100 bytes) read by caves U and V: "VBD1", a stamp, the attach function and its first word, then two
	// slots of 0x40 from +0x10: u32 record, u32 table, u8 count, u8 ref (the chest), u8 flags, u8 nloc, u32 keep mask lo and
	// hi (flag kCollapse: the bones whose axes stay), two attaches (u16 record offset of the held item's slot byte, u8 the
	// hand bone, pad), u8 bones[kMaxOut]. Each table holds count 64-byte matrices (the bones' frames relative to the
	// chest, ToGame).
	static constexpr u32 kBlockMagic = 0x31444256u; // "VBD1"
	static constexpr u32 kSlotOffset = 0x10, kSlotBytes = 0x40;
	static constexpr int kSlots = 2; // 0 the partner, 1 the local character (VrBodySelf)
	static constexpr u8 kFlagAttach = 1, kFlagCollapse = 2, kFlagSelf = 4, kFlagLocals = 8;
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
	// The slot's flags: si 0 the partner, 1 the local character (kSelf | kCollapse); with locals kFlagLocals, else at the
	// draw (the partner's held items re-attached while an arm is posed). Pure.
	u8 SlotFlags(int si, bool locals, bool arms);

	// CPU thread, every vsync after VR presence's Poll: writes or clears the host block. Reads the bones; writes only the
	// block, its matrix tables (both in free RAM) and nothing else.
	void Poll();
	void Shutdown();

	// The math above (IK reach, clamps and singularities, the head clamp, the blend), the bone map against File #1's
	// parent tables, the cave's block layout. Logs "(VR) Body self-test: N checks passed" or each failure.
	bool SelfTest();
}
