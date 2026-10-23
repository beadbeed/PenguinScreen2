// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include "VR/VRInputState.h"

#include <array>
#include <string>
#include <vector>

// [VR] PhysicalMelee: swinging a melee weapon with your own arm (tools/outbreak-vr/research/2026-10-14-melee.md).
//
// What the game does (File #1): with the weapon raised, Circle starts the character's swing (sub-action 3); the swing's
// motion spawns an attack object at one frame and ends it at a later one, and while it lives the game tests a capsule (a
// pipe, a pole) or a sphere (a knife) on the weapon's own world matrix against the enemies, stages the damage and plays
// the enemy's hit reaction. What this does:
//   - a deliberate swing of the right controller (the speed of a point up its handle, over a threshold along a stroke
//     that goes somewhere, one stroke one swing) presses the game's own attack on the player's own pad: Circle, with R1
//     held first if the weapon is down (the game swings only from the raised stance);
//   - every input poll it writes where the VR weapon is, in the game's space, into a block in free RAM; a code cave the
//     profile installs (camera.silence entries with whenSetting: PhysicalMelee) makes the local player's attack object
//     follow that block instead of the canned swing, so the game's own hit test runs where your hand holds the weapon;
//   - a short tick when the game's swing starts, a firmer buzz when an enemy's HP drops during it.
// Never while a game screen is up, while grabbed or hurt. Offline in full. Online only with the separate [VR]
// PhysicalMeleeOnline, only in private co-op (tools/outbreak-vr/research/2026-10-14-melee-coop.md): there every console
// runs every character's swing from the state partners send, and the console that runs a room's enemies (its master)
// decides their HP and reactions and sends them to the others. So online the swing presses the game's own attack (the
// partners see the game's own swing) and the hit follows the VR weapon only while this console is the room's master;
// otherwise the code is the game's own, exactly. Only the player's own pad, that block and the caves are ever written;
// never another character's record.
namespace VR::PhysicalMelee
{
	// --- The swing detector: pure, so the self-test can run it on scripted hands. ---

	struct DetectorParams
	{
		// The point whose speed is measured: this far up the controller's handle (the pointing frame's +Y, where the game
		// holds a shaft through the fist), metres. Its speed includes the wrist's turn, as a weapon's head does.
		float tip_m = 0.40f;
		// A stroke starts when that point moves faster than speed_on (m/s, smoothed) and ends once it is slower than
		// speed_off for end_s.
		float speed_on = 2.2f;
		float speed_off = 0.8f;
		float end_s = 0.08f;
		// It counts as a swing (one request) once, within the stroke, the point has travelled travel_m and its straight-line
		// displacement since the stroke began is at least straightness times that path: a waggle or a shake (breaking free of
		// a grab) goes back and forth and never counts.
		float travel_m = 0.20f;
		float straightness = 0.6f;
		// A stroke longer than this without counting is not a swing (a slow sweep); it has to end before another can start.
		float stroke_max_s = 0.6f;
		// The velocity is smoothed over about this long (an exponential, seconds).
		float smooth_s = 0.025f;
		// A jump of the point by more than this in one poll is a tracking glitch, not a swing: the stroke is dropped.
		float jump_m = 0.8f;
	};

	struct DetectorIn
	{
		float dt = 0.0f;           // seconds since the last Step; clamped to 0-0.25
		bool valid = false;        // the hand's pose is tracked this poll
		float tip[3] = {};         // the measured point, metres, in the XR base space
		bool armed = false;        // a swing may count now (a melee weapon, first person, no screen, not grabbed)
	};

	struct DetectorState
	{
		bool have_prev = false;
		float prev[3] = {};
		float vel[3] = {};          // smoothed, m/s
		bool in_stroke = false;
		bool counted = false;       // this stroke already made its request
		float stroke_s = 0.0f;
		float slow_s = 0.0f;
		float path_m = 0.0f;
		float start[3] = {};
		void Reset() { *this = DetectorState{}; }
	};

	struct DetectorOut
	{
		bool swing = false;         // true for exactly one Step per counted stroke
		bool in_stroke = false;
		float speed = 0.0f;         // smoothed speed this poll, m/s
		float path_m = 0.0f;        // path of the stroke so far
	};

	// One poll. Never fails: NaNs, missing poses and nonsense params are tolerated (params clamped in a copy). While not
	// armed nothing counts, but strokes are still tracked, so a stroke begun before arming never counts half-way.
	DetectorOut Detect(const DetectorIn& in, const DetectorParams& params, DetectorState& st);

	// --- The attack driver: a swing request to the game's own attack input. Pure. ---

	struct DriverParams
	{
		// Circle is held this long (the game reads it at 30-60 Hz), and at most press_max_s waiting for the swing to show.
		float press_s = 0.10f;
		float press_max_s = 0.30f;
		// With the weapon down, R1 is held for at most raise_max_s waiting for the aim stance; given up after.
		float raise_max_s = 0.9f;
		// The game's swing is followed for at most swing_max_s (a pipe's whole swing is 1.43 s, a brush's 1.87 s).
		float swing_max_s = 2.5f;
		// An enemy's HP dropping up to contact_s after the swing ended still counts as this swing's contact.
		float contact_s = 0.25f;
		// After a swing, a new request is taken only cooldown_s later.
		float cooldown_s = 0.25f;
	};

	enum class Phase : u8
	{
		Idle,
		Raising,   // R1 held, waiting for the aim stance (the weapon was down)
		Pressing,  // Circle held, waiting for the game's swing
		Swinging,  // the game's swing runs (R1 still held if we raised the weapon)
		Cooldown,  // the swing is over; contact still counts for contact_s, new requests wait for cooldown_s
	};
	const char* PhaseName(Phase phase);

	enum class Result : u8
	{
		None,
		Swung,        // the game's swing ran (with or without contact)
		NotRaised,    // R1 was held and the aim stance never came (another pad holds something, or the game refused)
		NoSwing,      // Circle was pressed and the game never swung (the weapon broke, a prompt, or it was busy)
		Screen,       // a game screen opened
		Lost,         // first person, the weapon or the stance went away (hurt, grabbed)
	};
	const char* ResultName(Result result);

	struct DriverIn
	{
		float dt = 0.0f;
		bool request = false;   // Detect's swing this poll; taken in Idle only
		bool usable = false;    // first person, a melee weapon, the setting on, Presses(mode), stance normal or raised
		bool screen = false;    // a game screen is up
		bool raised = false;    // stance raised (the game's attack works from here)
		bool aim_idle = false;  // ... and its sub-action is aim idle (Circle starts a swing now)
		bool swinging = false;  // the game's melee swing (sub-action attack) runs
		bool hp_drop = false;   // an enemy's HP dropped this poll
	};

	struct DriverOut
	{
		Phase phase = Phase::Idle;
		bool hold_attack = false;  // the pad's attack button (Circle) reads pressed
		bool hold_raise = false;   // the pad's raise button (R1) reads pressed
		bool started = false;      // a request was taken this poll
		bool live = false;         // the game's swing started this poll (the cue)
		bool contact = false;      // this swing's first contact, this poll (the buzz)
		bool finished = false;     // an attempt ended this poll: result, seconds
		Result result = Result::None;
		float latency_s = 0.0f;    // with live: from the request to the game's swing
	};

	struct DriverState
	{
		Phase phase = Phase::Idle;
		float phase_s = 0.0f;
		float since_request_s = 0.0f;
		bool we_raise = false;     // R1 is ours for this attempt
		bool saw_swing = false;
		bool contact = false;
		void Reset() { *this = DriverState{}; }
	};

	DriverOut Step(const DriverIn& in, const DriverParams& params, DriverState& st);

	// --- The weapon's pose in the game's space. Pure. ---

	// The game's object matrix (4 x 4 row vectors, rows the model's x, y, z axes as unit vectors in game space, then the
	// position and 1) from the model's frame in the XR base space (HandModel::ModelFrame) and a point map vr -> game
	// (CameraDriver::GameFromVr with the latest mapping): each axis is mapped as the difference of two mapped points a
	// centimetre apart, so the map's yaw, scale and handedness carry over exactly. False when the map fails or an axis
	// comes out degenerate.
	using PointMap = bool (*)(const void* ctx, const float vr[3], float game[3]);
	bool GameMatrix(const float origin_vr[3], const float axes_vr[3][3], PointMap map, const void* ctx, float out[16]);

	// The detector on scripted strokes (an overhead swing, a side swing, a stab, slow sweeps, a shake, tracking loss and
	// glitches, a stroke begun before arming), the driver on a scripted game (a raised swing, a lowered one, a swing that
	// never starts, a screen, contact inside and after the swing, the cooldown), and GameMatrix on a turned, offset map.
	// Returns the name of every failed check; empty is a pass.
	std::vector<std::string> SelfTest();

	// --- Contact mode ([VR] PhysicalMeleeMode = Contact; tools/outbreak-vr/research/2026-10-14-vr-melee.md). Pure. ---
	//
	// The VR weapon's own hit shape touching an enemy's hurt spheres makes the game apply a hit through its own code
	// (cave D calls the attack spawn FUN_005521B0 for the local player, with no attack action, no button pressed); the
	// strength from the tip's speed scales the game's own critical multiplier (cave E), so a hard swing raises the game's
	// own reaction class (a flinch becomes a stagger, a stagger a knockdown).

	// Tuning, per weapon type (the profile's melee.contact, its weapons list overriding the defaults).
	struct ContactParams
	{
		float speed = 1.8f;      // m/s at the weapon's far hit point: a touch counts at or above this
		float rearm = 0.9f;      // m/s: an enemy that was struck is armed again once the tip has been slower than this...
		float rearm_s = 0.10f;   // ... for this long,
		float leave_s = 0.15f;   // ... or once the weapon has been off that enemy this long
		float firm = 3.0f;       // m/s: from here the strength is 1 (the game's own hit)
		float heavy = 5.5f;      // m/s: from here the strength is strong
		float light = 0.6f;      // the strength at speed (it rises to 1 at firm): less damage, the weapon's own class
		float strong = 1.5f;     // the strength from heavy: more damage, and above 1 the game raises the class by one
		float lead_cm = 8.0f;    // game units added to the summed radii, so the strike is asked for a frame before the touch
		float cooldown_s = 0.12f; // after a strike the next one waits at least this long
		int window_frames = 6;   // the strike's attack object lives this many game frames (the node dies at its first hit)
		float hold_s = 0.035f;   // after the first touch the strike waits up to this long while the tip is still speeding
		                         // up and still touching, so its strength is the swing's, not the edge of the reach's
	};
	ContactParams SanitizedContact(const ContactParams& in);

	// The strength for a tip speed (m/s): 0 below p.speed (no strike), light rising to 1 by p.firm, 1 to p.heavy, then
	// p.strong. Never NaN.
	float Strength(float speed, const ContactParams& p);

	// The game's hit shape for a weapon, in its model's frame (game units): a capsule from p to p + d * length, or a sphere
	// at p; radius r (FUN_005AAE60 / FUN_005AA9B0, shapes at 0x6EBFA0).
	struct HitShape
	{
		bool valid = false;
		bool capsule = false;
		float p[3] = {};
		float d[3] = {};
		float length = 0.0f;
		float radius = 0.0f;
	};
	// The shape's two ends (a sphere's are both p) placed by a game matrix (4 x 4 row vectors, rows unit axes, row 3 the
	// position), in game units. The far one (from the model's origin, the grip) is b.
	void PlaceShape(const HitShape& s, const float m[16], float a[3], float b[3]);

	// The distance from c to the segment a-b, squared.
	float SegmentPointDistSq(const float a[3], const float b[3], const float c[3]);

	// One enemy's hurt spheres this poll (game units).
	struct EnemyVolume
	{
		int id = -1;
		int count = 0;
		float c[8][3] = {};
		float r[8] = {};
	};

	// Whether the weapon's shape (ends a, b, radius r; a sphere has a == b) touches one of the enemy's spheres within
	// lead; the touched sphere in *sphere. Swept: also the shape at the previous poll (pa, pb, when have_prev) and halfway,
	// so a fast swing cannot pass through between two polls.
	bool Touches(const float a[3], const float b[3], float r, bool have_prev, const float pa[3], const float pb[3],
		const EnemyVolume& e, float lead, int* sphere = nullptr);

	struct StrikeState
	{
		struct Enemy
		{
			bool struck = false;   // struck this swing: not again until re-armed
			float off_s = 0.0f;    // since the weapon last touched it
		};
		std::array<Enemy, 64> enemies{};
		float slow_s = 0.0f;       // the tip slower than rearm this long
		float since_strike_s = 10.0f;
		int pending = -1;          // touched at speed, the strike held while the tip still speeds up
		float pending_s = 0.0f;
		float pending_peak = 0.0f; // the fastest the tip went while held
		float last_speed = 0.0f;
		void Reset() { *this = StrikeState{}; }
	};

	struct StrikeIn
	{
		float dt = 0.0f;
		bool usable = false;        // contact may strike now (a melee weapon, first person, no screen, not hurt or grabbed)
		float speed = 0.0f;         // the tip, m/s
		bool live = false;          // the game still has the last strike's attack object (the block's frames left)
		const int* touching = nullptr; // the enemy ids the shape touches this poll
		int touching_count = 0;
	};

	struct StrikeOut
	{
		int enemy = -1;             // strike this enemy now (-1: none)
		float strength = 0.0f;
	};

	// One poll: re-arms enemies (slow tip or off them), then strikes the first armed touched enemy if the tip is fast
	// enough, nothing is live and the cooldown has passed: at once, or up to hold_s later while the tip is still speeding
	// up and the weapon still touches it (the strength from the fastest it went). One strike per swing per enemy.
	StrikeOut StrikeStep(const StrikeIn& in, const ContactParams& p, StrikeState& st);

	// Strength, the geometry and the strike logic on scripted swings. Returns the failed checks.
	std::vector<std::string> SelfTestContact();

	// --- Punches and pistol-whips ([VR] PhysicalMeleeFists; research/2026-10-14-vr-melee.md section 12). Pure. ---
	//
	// With contact strictly offline, bare hands (no item equipped) or a gun: the right hand's own strike through cave D and
	// cave H, with the knife's attack (id 33: a sphere r 30 at (-10, 0, 0) in the attack object's frame) at a fist-sized
	// point on the hand, or on the gun's butt; the strength scaled to a light hit. A melee weapon keeps its own shape.
	enum class FistKind : u8
	{
		None,
		Fist, // bare hands: the knuckles
		Whip, // a gun in the hand: its butt
	};
	// What the right hand strikes with: None unless the setting, the profile's melee.fist and contact strictly offline
	// (contact_offline) all hold; a melee weapon is never a fist; bare hands a fist, a gun a whip, anything else none.
	FistKind ChooseFist(bool setting, bool has_fist, bool contact_offline, bool bare, bool gun, bool melee);
	// The attack object's frame for a strike at point (metres, in the hand's pointing frame: x right, y up, z back) of
	// the hand at pos / quat (the XR base space): the hand's axes, and an origin put so that the knife sphere's centre
	// (-10, 0, 0) cm lands on the point (origin = point + 0.10 m along the hand's x). False for a non-finite pose.
	bool FistFrame(const float pos[3], const float quat[4], const float point[3], float origin[3], float axes[3][3]);
	// The strike's strength: the contact strength (light rising to 1 by firm, never above) times scale, 0 below speed.
	float FistStrength(float speed, const ContactParams& p, float scale);
	// ChooseFist, FistFrame and FistStrength on scripted cases. Returns the failed checks.
	std::vector<std::string> SelfTestFists();

	// --- Hit jolt ([VR] MeleeHitJolt, a prototype; research/2026-10-14-vr-melee.md sections 8 and 9). Pure. ---
	//
	// A contact hit pushes the struck body part the way the swing went: a few bones near the hit get a rotation that
	// decays to nothing over a few game frames, through the game's own per-bone apply (FUN_0055C450: the bone's local
	// matrix, about its parent's axes, so its children follow), from cave G in the zombie's update.

	// The bones a hit on one hurt sphere moves: the sphere's bone (hit_bone), then up to four bones, each with its parent
	// (for the parent's frame) and a share of the angle.
	struct JoltChain
	{
		u32 hit_bone = 0;
		int count = 0;
		u8 bones[4] = {};
		u8 parents[4] = {};
		float weights[4] = {};
		u8 below = 0; // bit i: bones[i] moves the body below its joint (the legs), not above it
		u8 loose = 0; // bit i: bones[i] follows loosely (the spring's loose stiffness, damping and clamp: the arms)
		u8 hard = 0;  // bit i: bones[i] moves only on a hard hit (spring.hard m/s and up): one more bone down the chain
	};
	// The spring-damper jolt (melee.jolt.spring; without it the linear decay above): each struck bone has an angle and an
	// angular velocity (rotation vectors in its parent's frame, degrees and degrees a second); a hit adds impulse x speed x
	// the bone's weight (deg/s) along the swing's turn, and each enemy update (1/30 s) integrates
	// angle'' = -stiffness * angle - damping * angle', slightly underdamped so it overshoots once and settles; the angle is
	// clamped to maxDeg (looseMaxDeg for loose bones) and the outward velocity taken off there. Hits add up.
	struct SpringParams
	{
		float stiffness = 200.0f;      // 1/s^2 (omega0 about 14.1 rad/s)
		float damping = 12.7f;         // 1/s (damping ratio about 0.45: one overshoot of about a fifth, settled by ~0.5 s)
		float loose_stiffness = 120.0f; // the arms: slower and less damped, they swing
		float loose_damping = 7.0f;
		float impulse = 120.0f;        // deg/s of angular velocity per m/s of the weapon's far hit point (x the bone's weight)
		float max_speed = 8.0f;        // m/s: faster swings count as this
		float max_deg = 30.0f;         // the clamp, per bone
		float loose_max_deg = 40.0f;
		float hard = 4.0f;             // m/s: from here the chain's hard bones move too
		float settle_deg = 0.75f;      // settled (the entry goes) when the angle and ...
		float settle_rate = 15.0f;     // ... the angular velocity (deg/s) are both under these
		int max_frames = 45;           // updates at most (1.5 s), whatever the spring does
	};
	struct SpringBone
	{
		float angle[3] = {};
		float rate[3] = {};
		bool loose = false;
	};
	// Adds an impulse of dps degrees a second about the (parent-frame, any length) axis.
	void SpringKick(SpringBone& b, const float axis[3], float dps);
	// One enemy update (dt seconds, integrated in four sub-steps), then the clamp.
	void SpringStep(SpringBone& b, const SpringParams& p, float dt = 1.0f / 30.0f);
	bool SpringSettled(const SpringBone& b, const SpringParams& p);
	// The kick for a swing at speed m/s on a bone of weight w (deg/s; 0 when not finite).
	float SpringImpulse(float speed, float w, const SpringParams& p);
	// A rotation vector (degrees, parent frame) as the three angles FUN_0055C450 applies (Rz * Ry * Rx, 65536 a turn).
	void JoltEuler(const float rotvec_deg[3], s16 out[3]);
	// The integrator on scripted hits (one overshoot, the settle time, the clamps, accumulation). Returns the failed checks.
	std::vector<std::string> SelfTestSpring();
	struct JoltParams
	{
		float slow = 1.8f;    // m/s at the weapon's far hit point: min_deg and min_frames here and below
		float fast = 6.0f;    // m/s: max_deg and max_frames here and above (linear between)
		float min_deg = 8.0f;
		float max_deg = 24.0f;
		int min_frames = 5;   // updates of the enemy (30 a second) from the full angle back to nothing (linear)
		int max_frames = 10;
		std::vector<JoltChain> chains;
		bool spring_on = false; // melee.jolt.spring present: the spring-damper instead of the linear decay
		SpringParams spring;
	};
	struct JoltSize
	{
		float deg = 0.0f;
		int frames = 0;
	};
	// The angle and the time for a tip speed (m/s). Never NaN; frames at least 1.
	JoltSize JoltMagnitude(float speed, const JoltParams& p);
	// The chain for the struck sphere's bone, null when none.
	const JoltChain* JoltChainFor(const JoltParams& p, u32 hit_bone);
	// One bone's rotation: about the axis (pivot -> contact) x velocity, with the lever's vertical part turned to the side
	// of the joint the bone moves (above it, or below it for the legs: below), so the part it moves goes with the swing,
	// moved into the parent's frame (parent_world: its 4 x 4 row-vector world matrix; only the 3 x 3 is used, inverted, so
	// its scale does not matter), deg degrees, as the three angles FUN_0055C450 applies (65536 a turn, about the parent's
	// x, then y, then z: exactly that turn, Rz * Ry * Rx, not the small-angle split). A lever shorter than 8 game units, or one along the velocity, uses straight up instead; false when the
	// velocity is too slow (under 1 game unit a second), the parent's matrix is singular or nothing is left.
	bool JoltAngles(const float contact[3], const float pivot[3], const float vel[3], const float parent_world[16], float deg,
		s16 out[3], bool below = false);
	// The same turn's unit axis in the parent's frame (what JoltAngles turns about); false as JoltAngles.
	bool JoltAxis(const float contact[3], const float pivot[3], const float vel[3], const float parent_world[16], float out[3],
		bool below = false);
	// One entry of cave G's block (24 bytes at block + 0x10 + 24 * k): the enemy, the bone, the frames left, whether it
	// still waits for the hit (armed at the strike: cave G holds it until the enemy's own hit counter +0x13F is set, the
	// update the hit lands in, and clears it), the angles now and what is taken off them after each frame (angle / frames,
	// toward zero).
	struct JoltEntry
	{
		u32 enemy = 0;
		u8 bone = 0;
		u8 frames = 0;
		u8 wait = 0;
		s16 angle[3] = {};
		s16 rate[3] = {};
	};
	JoltEntry MakeJoltEntry(u32 enemy, u8 bone, const s16 angle[3], int frames, bool wait = false);
	// The entry as the cave reads it: six little-endian words.
	void PackJoltEntry(const JoltEntry& e, u32 words[6]);
	constexpr int kJoltEntries = 4;
	constexpr u32 kJoltBlockBytes = 0x10 + 24u * kJoltEntries;

	// The magnitude, the chains, the axes and the entries on scripted hits. Returns the failed checks.
	std::vector<std::string> SelfTestJolt();

	// --- The glue (VRInputSource::PollEvents). ---

	using GuestReader = bool (*)(u32 address, u8 width, u32* value);

	struct Frame
	{
		int pad_port = -1;           // the pad the presses go to (the profile's VR Gamepad); -1: none this poll
		bool hold_attack = false;    // the Gamepad's controls bound to attack_button read 1
		bool hold_raise = false;     // the Gamepad's controls bound to R1 read 1
		std::string attack_button;   // melee.attackButton ("Circle")
	};

	// CPU thread, every input poll, after ReloadDriver::Poll and before the controls compose. reader reads guest memory
	// (null: main RAM); dt in seconds. Writes the override block (main RAM, while Overrides(mode)), plays the cues, logs.
	Frame Poll(const VRInputSnapshot& snap, GuestReader reader, float dt);

	// CPU thread: a loaded state has its own enemies; the HP baseline starts again.
	void OnStateLoaded();

	// --- Where it may act (pure, on the settings and what the game says). ---

	enum class Mode : u8
	{
		Off,
		Offline,    // strictly offline (no network adapter, not online-safe): presses, the block and the caves
		CoopPress,  // private co-op with PhysicalMeleeOnline: the presses and the cues only; the game's own swing and hit
		CoopFull,   // ... and this console runs the room's enemies: the block and the caves too
	};
	const char* ModeName(Mode mode);

	struct ModeIn
	{
		bool setting = false;          // [VR] PhysicalMelee
		bool online_setting = false;   // [VR] PhysicalMeleeOnline
		bool strictly_offline = false; // no network adapter and [VR] OnlineSafe off
		bool private_coop = false;     // CameraDriver's network mode is private co-op
		bool online_safe = false;      // [VR] OnlineSafe
		bool has_online = false;       // the profile's melee.online (and melee.override)
		bool room_master = false;      // this console runs the enemies of the room the local player is in
		bool free_clear = false;       // the caves' free RAM read zero before they went in online
	};
	Mode Decide(const ModeIn& in);
	inline bool Presses(Mode mode) { return mode != Mode::Off; }
	// How long the game's screens (MenuMode and melee.screenFlag) must have read clear before a press may start.
	inline constexpr float kScreenClearS = 0.2f;
	inline bool Overrides(Mode mode) { return mode == Mode::Offline || mode == Mode::CoopFull; }
	// Contact mode online (CoopFull, the only case its hits apply online): cave F shows partners the start of a swing for
	// this many game frames after each strike (block +0x30), then sends the swing's end; this console never runs it.
	// Offline nothing is shown (0), unless test is set (PCSX2_VR_TEST_PARTNER_SWING=1: offline the sender returns at
	// once, so only the cave's own steps run; for the headset-free checks).
	constexpr u32 kPartnerSwingFrames = 30;
	inline u32 PartnerSwingFrames(Mode mode, bool test) { return (mode == Mode::CoopFull || (test && mode == Mode::Offline)) ? kPartnerSwingFrames : 0u; }

	// Online: who runs the room's enemies, as the game decides an enemy's death (FUN_0060EF30 through 0x1AF440 and
	// NETAQ.BIN's per-room table): only while the network overlay's tag word reads tag, the u16 at room is below 128, and
	// then masters[room * 4] is the slot. -1 when any of that fails (no console is taken as the master).
	struct OnlineParams
	{
		u32 room = 0;
		u32 masters = 0;
		u32 tag_addr = 0;
		u32 tag = 0;
	};
	int RoomMasterSlot(GuestReader reader, const OnlineParams& params, u32* room_out = nullptr);
	// The words from lo up to hi all read zero (lo and hi word aligned); *busy_addr (optional) the first that does not.
	bool RegionZero(GuestReader reader, u32 lo, u32 hi, u32* busy_addr = nullptr);
	// A block in free RAM is ours to write (night review m4, late review m3): its magic word at magic_addr already reads
	// magic, or every word from lo up to hi reads zero.
	bool RegionFreeOrOurs(GuestReader reader, u32 lo, u32 hi, u32 magic_addr, u32 magic);
	// After a state load (evening review m4): whether a block this session armed may still count as armed, so its next
	// write skips RegionFreeOrOurs: only when the loaded RAM reads our magic at it (a state saved while it was armed).
	// Anything else (zero, or someone else's words) is not armed, and the next write checks the region first. False for a
	// zero address or magic.
	bool ArmedAfterLoad(GuestReader reader, u32 magic_addr, u32 magic);
	// A GuestReader over main RAM (CPU thread; false without a VM or past main RAM).
	bool ReadMainRam(u32 address, u8 width, u32* value);

	// CPU thread: CameraDriver's whenSetting gate asks, in private co-op, whether the caves may be in: true only while the
	// last input poll decided CoopFull, and that poll ran within the last 250 ms (a stale decision counts as false).
	bool CoopOverrideLive();

	// Decide, RoomMasterSlot and RegionZero on scripted inputs and a scripted memory. Returns the failed checks.
	std::vector<std::string> SelfTestOnline();
}
