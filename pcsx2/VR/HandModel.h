// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include <vector>

namespace VR::GameModels
{
	struct Model;
}

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

	// How the game itself attaches a weapon's model to the character's right hand: one entry of its weapon-type table
	// (the profile's feedback.weapon.hold; File #1: 0x6E5944, 0x44 bytes an entry), read by GameFeedback. The game draws
	// the held weapon with the matrix (vertex * R + grip) * hand, in its row-vector convention: vertex is a point of the
	// model (game units, 1 cm), R = Rx(rot[0]) * Ry(rot[1]) * Rz(rot[2]) (X applied first), grip a point in the hand bone's
	// own frame and hand the bone's matrix (node 19/28 of the character's skeleton; its scale, Kevin's 1.05, applies to
	// grip, not to the weapon). The bone's frame, in the bind pose: -X from the wrist toward the fingers, -Y the palm,
	// +Z the thumb's side.
	struct GameAttach
	{
		bool valid = false;
		u16 type = 0; // the game's weapon type (the table index), 0 = none
		float grip_cm[3] = {0.0f, 0.0f, 0.0f};
		float rot_rad[3] = {0.0f, 0.0f, 0.0f};
		bool has_muzzle = false;
		float muzzle_cm[3] = {0.0f, 0.0f, 0.0f}; // a gun's muzzle in the model's own space
	};

	// Decodes one weapon-type table entry (bytes as in guest memory, little-endian) into out: false, with out left
	// invalid, when the entry is short, its byte +2 does not name type, type is 0, or a value is not finite or is
	// further than a metre (grip) or two (muzzle) out. Pure.
	bool DecodeGameAttach(const u8* entry, size_t size, u16 type, u32 grip_offset, u32 rotation_offset, u32 muzzle_offset,
		GameAttach* out);

	// [VR] WeaponSceneLighting: the room's light on the hand and weapon models, from the shown game frame.
	//
	// The compositor reads the frame back (a small grid of averaged sRGB colours, every few presents) and hands the
	// grid to SampleSceneLight, which gives the colour and brightness of the part of the picture the hands are usually
	// seen against (the lower middle) and the side the light comes from (where the frame is brighter, left against right
	// and top against bottom). SmoothSceneLight eases one estimate towards the next over about kSceneLightTauS, so a
	// muzzle flash or a cut does not make the models flicker. SetSceneLight publishes the eased light; the mesh drawing
	// then lights from it, with no rim, its brightness following the room's (never above the fixed look's) and its
	// colour tinted by the room's. Without a published light (or with the setting off) the fixed look stays.
	struct SceneLight
	{
		bool valid = false;
		float colour[3] = {1.0f, 1.0f, 1.0f}; // the room's tint, each channel 0 to 1, the brightest 1
		float level = 1.0f;                   // the model's brightness against the fixed look, kSceneLevelMin to 1
		float dir[3] = {0.35f, 0.8f, 0.5f};   // towards the light, in view space (x right, y up, z to the viewer), unit
	};
	static constexpr float kSceneLightTauS = 0.5f;
	static constexpr float kSceneLevelMin = 0.3f;
	// A room whose sampled region averages this sRGB luminance (0 to 1) lights the models fully (level 1).
	static constexpr float kSceneFullLuma = 0.42f;

	struct HandState
	{
		bool valid = false;
		// [VR] GameWeaponModels: drawn with the hand model (HandMesh) and, in the right hand, the game's own weapon model
		// (GameModels) placed by attach; false, or a weapon whose model or attach is missing, is today's procedural look.
		bool mesh = false;
		// [VR] WeaponSceneLighting: the room's light for the mesh look (invalid: the fixed look). A change past
		// SceneLightChanged redraws a kept image.
		SceneLight scene;
		GameAttach attach;
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
		// Right hand only, with gun: the weapon alone, hanging in the hip holster (ZoneMarkers), no glove. pos and quat
		// are then the pointing frame the weapon has in its holster (HolsterPose), pos where the hand closes on it; grip,
		// trigger and the shot feedback are not used. lit draws it brighter (the right hand is in the holster's reach).
		bool holstered = false;
		bool lit = false;
		// [VR] NativeHands: the game itself draws this hand and its weapon (VrBody's posed wrist, the game's own attach), so the
		// weapon's frame, muzzle and barrel for the laser and the gameplay are the game's: the wrist a fixed
		// kNativeWristBackCm behind the grip point along the pointing frame's forward, the model as the game attaches it, no
		// turn onto -Z (the barrel is where the game draws it). Never drawn by MeshRaster in this state.
		bool native = false;
	};
	// The posed wrist behind the controller's grip point (VrBody's Tuning::wrist_back_cm 6, in the character's units; Kevin's
	// scale 1.05 as the grip's), cm along the pointing frame's +Z.
	static constexpr float kNativeWristBackCm = 6.0f * 1.05f;

	// A holstered weapon's quad is at least this big, and twice that at most (a long gun's or a melee weapon's up to
	// kQuadSizeLongM, as in the hand).
	static constexpr float kQuadSizeHolsterM = 0.25f;
	// The holster carries a gun or a melee weapon muzzle (or blade) down, tipped this far back from straight down, so its
	// grip stands up and back toward the hand reaching down for it, as a hip holster with a slight cant does.
	static constexpr float kHolsterCantDeg = 15.0f;
	// A lit holstered weapon is drawn this much brighter.
	static constexpr float kHolsterLitGain = 1.45f;

	// Where a weapon hangs in the hip holster whose middle is centre, the body facing yaw (radians, OpenXR yaw: 0 faces
	// -Z, positive turns left, as SpatialControls::HeadAnchor gives it): out_pos is centre (the grip position, where the
	// hand closes) and out_quat the pointing frame (as HandState takes it). A gun or a melee weapon hangs muzzle or blade
	// down, kHolsterCantDeg back from straight down, its top to the front and its right side out; something to throw or a
	// spray can stands upright on the belt (its handle axis vertical). False, with nothing written, for a centre or yaw
	// that is not finite. Pure: any thread.
	// model: the weapon is drawn with its game model ([VR] GameWeaponModels, DrawsMesh); a melee weapon then hangs with
	// its shaft down (the game holds a shaft through the fist, along the hand's +Y, not along -Z as the placeholder),
	// kHolsterCantDeg back from straight down, its hold point at centre. Guns, cans and things to throw are posed alike
	// either way.
	bool HolsterPose(Held held, const float centre[3], float yaw, float out_pos[3], float out_quat[4], bool model = false);

	// A long gun slung over the right shoulder leans its barrel this far back from straight up and this far out, away
	// from the head.
	static constexpr float kSlingBackDeg = 15.0f;
	static constexpr float kSlingOutDeg = 10.0f;

	// Where a long gun hangs slung over the shoulder whose reach is centred at centre, the body facing yaw (as
	// HolsterPose): out_pos is centre (the grip position, where the hand reaching back closes), the barrel up behind the
	// shoulder (kSlingBackDeg back, kSlingOutDeg out), the stock down the back, the gun's right side out. False, with
	// nothing written, for a centre or yaw that is not finite. Pure: any thread.
	bool SlingPose(const float centre[3], float yaw, float out_pos[3], float out_quat[4]);

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

	// LaserFrom for the game model the right hand draws with [VR] GameWeaponModels: from gap_m past that model's muzzle
	// (its weapon-type entry's muzzle, placed as the hand draws it), along -Z. False, with nothing written, when the hand
	// would draw the placeholder for it (no model, no attach, or no muzzle in its entry): use LaserFrom then. With
	// gun_hand.native the beam goes along the barrel as the game draws it (the model's -X), from where the game draws the
	// muzzle, and it needs no model (a native weapon is the game's, not ours).
	bool ModelLaserFrom(const HandState& gun_hand, float gap_m, float out_origin[3], float out_dir[3]);

	// Where the game model of the weapon a right hand at pos / quat (the grip position and pointing frame, as HandState
	// takes them) holds sits, exactly as the hand draws it with [VR] GameWeaponModels (PlaceModel: the game's attach on
	// the hand model): out_origin the model's origin in metres, out_axes[k] the model's axis k (x, y, z) as a unit vector,
	// both in the XR base space. A model point m (centimetres) is then out_origin + sum_k m[k] * 0.01 * out_axes[k]
	// ([VR] PhysicalMelee maps this into the game to put the game's own hit test on the VR weapon). False, with nothing
	// written, for an invalid attach or a pose that is not finite or usable. Pure: any thread.
	// native: [VR] NativeHands' placement (HandState::native): the game's own, from the posed wrist.
	bool ModelFrame(const GameAttach& attach, Held held, const float pos[3], const float quat[4], float out_origin[3],
		float out_axes[3][3], bool native = false);

	// Whether this hand image is drawn with the hand model and the game's weapon model ([VR] GameWeaponModels): h.mesh,
	// and for a weapon in the right hand its model decoded and its attach known. A weapon that falls back to the
	// placeholder is logged once per item. Any thread.
	bool DrawsMesh(int hand, const HandState& h);

	// Test aid: where the weapon models come from (GameModels::Find by default; a self-test passes its own). Not
	// thread-safe: set it before any drawing and put the default back (nullptr) after.
	using ModelLookup = const GameModels::Model* (*)(u16 item_id);
	void SetModelLookupForTest(ModelLookup lookup);

	// What the mesh drawing costs: hand images drawn with MeshRaster since the last call, their total and longest time
	// in microseconds. Any thread.
	struct MeshDrawStats
	{
		u32 images = 0;
		u64 total_us = 0;
		u32 max_us = 0;
	};
	MeshDrawStats TakeMeshDrawStats();

	// From a grid_w x grid_h grid of sRGB pixels (RGBA8, R in the low byte, row 0 on top). Invalid for an empty grid.
	SceneLight SampleSceneLight(const u32* grid, u32 grid_w, u32 grid_h);
	// prev eased towards target over dt_s seconds (an invalid prev jumps to target; an invalid target keeps prev).
	SceneLight SmoothSceneLight(const SceneLight& prev, const SceneLight& target, float dt_s);
	// The light the mesh drawing uses (any thread); an invalid one restores the fixed look. GetSceneLight is what was
	// set; how far a drawn image's light is from it decides a redraw (SceneLightChanged).
	void SetSceneLight(const SceneLight& light);
	SceneLight GetSceneLight();
	bool SceneLightChanged(const SceneLight& a, const SceneLight& b);

	// PlaceImpostor's fit for the mesh look: a box's 8 corners as seen from the eye (x right, y up, z the distance in
	// front of the eye along the quad's normal), projected from the eye onto the plane de in front of it. The box's 12
	// edges are cut where they come nearer than near_z, so a pole reaching past the eye still gives the outline of what
	// lies in front (not the old fallback round the hand, which cut the far end). out_rect = x0, y0, x1, y1 on the
	// plane; false when no part of the box is near_z or more in front. Pure.
	bool ProjectBoxOnPlane(const float corners[8][3], float de, float near_z, float out_rect[4]);
	// The largest quad a long gun or a melee weapon drawn with its game model may get (the image keeps 256 pixels, so
	// the bigger quad is coarser, only while the weapon needs it).
	static constexpr float kQuadSizeModelLongMaxM = 3.2f;

	// PCSX2_VR_DUMP_HANDS=<folder> (the CPU thread, once in game): draws each weapon of items (with its attach) as the right
	// hand holds it and as it hangs in the holster, from a fixed eye, into PNG files there, and logs what each took. For
	// checking the placement offline; the images show the game's own models, so they stay on this PC (never commit or
	// share them). Returns how many images were written.
	struct DumpItem
	{
		u16 item_id = 0;
		GameAttach attach;
	};
	int DumpImages(const char* folder, const DumpItem* items, int count);

	// Where a hand's impostor quad goes: on the plane through the middle of the hand (with a pistol, can or thrown item
	// in it), so the hand keeps its true depth in stereo, its +Z turned to the eye from the middle of the hand and all it
	// holds; "up" kept close to world up; centred on and just big enough for all of it, a long gun's or a melee weapon's
	// reach included, as seen from the eye (at least kQuadSizeM, kQuadSizeGunM with a weapon, kQuadSizeHolsterM for a
	// holstered one; at most twice that, or kQuadSizeLongM for a long gun or a melee weapon).
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
	// now seen from now_eye: ref invalid, the weapon held (or what it is), the left hand's grip on a fore-end, the
	// locked slide, holstered or lit changed, a shot plays in either (kick, flash, slide), grip or trigger moved past
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
