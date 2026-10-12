// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include <vector>

// First-person hands: a procedural low-poly glove holding the controller, or in the right hand the weapon the
// character has raised, shaped by the equipped item's kind (a pistol, a long gun, a melee weapon, something to throw
// or a spray can), drawn on the CPU from the head's position into a small image that the compositor shows as a quad
// layer facing the head (an impostor). No XR or Vulkan types here, so it stays self-contained.
namespace VR::HandModel
{
	static constexpr int kLeft = 0;
	static constexpr int kRight = 1;

	// Square RGBA8 impostor image, the same byte order and unpremultiplied alpha as the lever cards
	// (ControlQuads::PackRgba); background alpha 0.
	static constexpr u32 kImageSize = 256;
	static constexpr float kQuadSizeM = 0.30f;
	static constexpr float kQuadSizeGunM = 0.45f;
	// A long gun or a melee weapon reaches well past the hand, so its quad may grow to this (the image keeps its size,
	// so it is coarser then).
	static constexpr float kQuadSizeLongM = 1.30f;

	// What the right hand holds while the weapon is raised (HandState::gun), by the equipped item's kind (ItemData).
	enum class Held : u8
	{
		Pistol,  // a handgun or another one-handed gun (stun gun, nail gun, ampoule shooter); also the pistol shown from
		         // the stance alone while the weapon reader cannot tell what is equipped
		LongGun, // a two-handed gun: a longer barrel, a fore-end for the left hand and a stock back to the wrist
		Melee,   // a blade, pipe or pole along the pointing direction, as long as the item (knives ~25 cm, poles ~1 m)
		Thrown,  // a small object in the closed hand (a molotov by its neck, a stick, a piece of concrete)
		Spray,   // a spray can in the closed hand
	};

	struct HandState
	{
		bool valid = false;
		// In the XR base space. pos is the controller's grip position (inside the handle); quat (xyzw)
		// is the controller's pointing frame: -Z forward along the barrel, +Y up, +X right.
		float pos[3] = {0.0f, 0.0f, 0.0f};
		float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		float grip = 0.0f;
		float trigger = 0.0f;
		// Right hand only: hold the raised weapon (held says which) instead of the controller.
		bool gun = false;
		// With gun: what the hand holds, and the equipped item (it picks a long gun's or a melee weapon's own shape).
		Held held = Held::Pistol;
		u16 item_id = 0;
		// With gun, shot feedback on a pistol or a long gun: kick 0-1 pitches the hand and gun up (7 deg, a long gun
		// 4 deg) and back (1.5 cm, a long gun 2.5 cm) about the grip; flash draws a muzzle flash; slide 0-1 moves the
		// pistol's slide back (1.8 cm at 1); slide_locked holds it fully back (an empty pistol).
		float kick = 0.0f;
		bool flash = false;
		float slide = 0.0f;
		bool slide_locked = false;
		// Left hand only: closed round a long gun's fore-end (OffHandSupports) instead of holding the controller.
		bool support = false;
	};

	// What the hands show for the raised weapon. CameraDriver publishes it every vsync (SetWeaponHeld); the compositor
	// reads it.
	struct WeaponHeld
	{
		bool raised = false; // the right hand holds a weapon
		Held held = Held::Pistol;
		u16 item_id = 0;    // the equipped item (0 when the weapon reader cannot tell)
		bool known = false; // the weapon reader named the item; false for the pistol from the stance alone
		// The laser sight goes with it: a gun (ItemData::IsGun), an id the table lacks, or the pistol from the stance
		// alone; never a melee weapon, something to throw or a spray can.
		bool laser = false;
	};

	// The weapon reader's verdict to what the hands show. raised: the weapon is raised and the reader did not see bare
	// hands. item_known: the reader trusts the equipped record, whose item's kind (ItemData) picks the shape: a handgun or
	// another one-handed gun the pistol; a long gun, a melee weapon, something to throw or a spray can their own; the
	// laser sight only on a gun. An item that is no weapon leaves the hands bare, and an id the table lacks keeps the
	// pistol. Without item_known the pistol and its laser show from the stance alone, as before the reader knew more.
	WeaponHeld HeldFor(bool raised, bool item_known, u16 item_id);

	// The character's raised weapon (CameraDriver, every vsync; each weapon it shows is logged once, when it differs
	// from the last one logged). Any thread, one word, so a reader never sees half an update; nothing raised until
	// someone sets it.
	void SetWeaponHeld(const WeaponHeld& held);
	WeaponHeld GetWeaponHeld();
	// The right hand holds a weapon, any kind (GetWeaponHeld().raised).
	bool GunHeld();
	// "pistol", "long gun", "melee weapon", "thrown item" or "spray can", for the log.
	const char* HeldName(Held held);

	// The left hand closes round the long gun's fore-end within kSupportGripM of it, and opens again only past
	// kSupportReleaseM, so it does not flicker on the edge.
	static constexpr float kSupportGripM = 0.25f;
	static constexpr float kSupportReleaseM = 0.28f;

	// Whether the off (left) hand is drawn closed round the fore-end of the long gun the right hand holds: both hands
	// valid, gun_hand holding a long gun, and off_hand's grip position within kSupportGripM of the fore-end
	// (kSupportReleaseM while it already holds it, was). Pure: any thread.
	bool OffHandSupports(const HandState& gun_hand, const HandState& off_hand, bool was);

	// Where the laser sight starts for the gun the right hand holds (pos: the grip position; quat: its pointing frame, as
	// HandState takes them): out_origin gap_m past the gun's muzzle along the barrel, out_dir the barrel's direction
	// (-Z, unit). The pistol's muzzle is HudCards' kPistolMuzzle; a long gun's barrel runs on past it. False, with
	// nothing written, for a position that is not finite or an orientation that is not usable (a beam pointing the
	// wrong way is worse than none). Pure: any thread.
	bool LaserFrom(Held held, u16 item_id, const float pos[3], const float quat[4], float gap_m, float out_origin[3],
		float out_dir[3]);

	// Where a hand's impostor quad goes: on the plane through the middle of the hand (with a pistol, can or thrown item
	// in it), so the hand keeps its true depth in stereo, its +Z turned to the eye from the middle of the hand and all it
	// holds; "up" kept close to world up; centred on and just big enough for all of it, a long gun's or a melee weapon's
	// reach included, as seen from the eye (at least kQuadSizeM, kQuadSizeGunM with a weapon; at most twice that, or
	// kQuadSizeLongM for a long gun or a melee weapon).
	struct ImpostorQuad
	{
		float centre[3] = {0.0f, 0.0f, 0.0f};
		float right[3] = {1.0f, 0.0f, 0.0f};
		float up[3] = {0.0f, 1.0f, 0.0f};
		float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // xyzw, quad orientation
		float size_m = kQuadSizeM;
	};

	// False when the hand can't be drawn (invalid pose, or the eye is inside the hand, or the pistol, can or thrown
	// item in it). A long gun or a melee weapon reaching past the eye cannot all go on one quad: the quad then holds the
	// hand and as much of the weapon as fits (nearer than 3 cm to the eye is left out).
	// PlaceImpostor and RenderImpostor share scratch buffers: call them from one thread (the compositor's).
	bool PlaceImpostor(int hand, const HandState& h, const float eye[3], ImpostorQuad* out);

	// Renders the hand as seen from eye, in perspective, onto the quad plane (centre, unit right/up,
	// square side quad_size_m) into a kImageSize x kImageSize image.
	void RenderImpostor(int hand, const HandState& h, const float eye[3], const float quad_center[3],
		const float quad_right[3], const float quad_up[3], float quad_size_m, std::vector<u32>& out_rgba);

	// Test mode (PCSX2_VR_FAKE_HANDS): a hand held at a fixed offset from the head along its yaw,
	// pointing forward and slightly down, so the hands can be checked without controllers. With gun the right hand
	// holds FakeWeapon(); while that is a long gun the left hand is held at its fore-end (whatever gun says, so every
	// caller places the left hand alike).
	HandState FakeHandState(int hand, const float head_pos[3], const float head_quat[4], bool gun);

	// PCSX2_VR_FAKE_WEAPON=<item id> (decimal, read once): the weapon the test hands' gun is, as HeldFor(true, true, id)
	// gives it; the pistol (known false) without it.
	WeaponHeld FakeWeapon();

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
	// now seen from now_eye: ref invalid, the weapon held (or what it is), the left hand's grip on a fore-end or the
	// locked slide changed, a shot plays in either (kick, flash, slide), grip or trigger moved past
	// kRerasterArticulation, the view drifted past kRerasterDriftDeg or kRerasterDistanceRatio (or cannot be measured),
	// or the image is older than kRerasterMaxAgeMs.
	bool NeedsReraster(const HandState& ref, const float ref_eye[3], const float ref_centre[3], const HandState& now,
		const float now_eye[3], u64 age_ms);

	// Test aid (PCSX2_VR_FAKE_HANDS_SWEEP): sweeps a fake hand on the display clock t_ns (an XrTime), 2 s a cycle,
	// the left hand half a cycle behind the right: +-12 cm along x, +-5 cm along y at twice the rate, and +-0.35 rad
	// (20 deg) of yaw about world up applied before its own orientation. Grip, trigger and the pistol are untouched.
	void ApplyFakeSweep(int hand, s64 t_ns, HandState* h);

	// Checks the maths above, the weapon shapes (each draws a silhouette of its own that its quad holds), the left
	// hand's grip on a fore-end and the laser by kind; logs "(VR) Hands self-test: N checks passed", or each failure and
	// how many failed (PCSX2_VR_HANDS_SELFTEST; it draws with the scratch buffers, so the compositor's thread). True when
	// every check passed.
	bool SelfTest();
}
