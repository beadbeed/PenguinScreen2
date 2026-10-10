// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include "VR/SpatialControls.h"

#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace VR::ProfileDB
{
	enum class Tier
	{
		Screen,
		Stereo,
		Immersive,
	};

	enum class UvDrawPolicy
	{
		Screen,
		World,
	};

	enum class StereoMap : u32
	{
		Linear = 0,
		Bands = 1,
		Log = 2,
	};

	struct StereoBand
	{
		float conv = 0.0f;
		float sep = 0.0f;
	};

	struct StereoLogParams
	{
		float w0 = 1.0f;
		float w1 = 1.0f;
		float dfar = 0.0f;
	};

	struct StereoResolvedMap
	{
		StereoMap map = StereoMap::Linear;
		u32 band_count = 1;
		float split_q[3] = {};
		float conv[4] = {};
		float sep[4] = {};
		float bias[4] = {};
		float log_w0 = 0.0f;
		float log_w1 = 0.0f;
		float log_dfar = 0.0f;
	};

	u32 SelectBand(const StereoResolvedMap& map, float q);

	float EvalBand(const StereoResolvedMap& map, u32 band, float q);

	float EvalDisparity(const StereoResolvedMap& map, float separation, float convergence, float q);

	struct StereoSceneRule
	{
		u32 ee_address = 0;
		u32 equals = 0;
		u8 width = 4;
		std::optional<float> separation;
		std::optional<float> convergence;

		std::optional<StereoResolvedMap> map_override;

		std::string label;
	};

	struct CollimateRule
	{
		s8 prim = -1;
		s8 tme = 1;
		s8 abe = -1;
		s32 min_w = 0;
		s32 max_w = 0;
		s32 min_h = 0;
		s32 max_h = 0;
		float rx0 = 0.0f, ry0 = 0.0f, rx1 = 0.0f, ry1 = 0.0f;
		float tu0 = 0.0f, tv0 = 0.0f, tu1 = 0.0f, tv1 = 0.0f;
		std::string label;
	};

	static constexpr u32 kMaxCollimateRules = 4;

	struct HudCollimate
	{
		float disparity = 0.0f;
		std::vector<CollimateRule> rules;
	};

	struct StereoParams
	{
		float separation = 0.0f;
		float convergence = 0.0f;
		UvDrawPolicy uv_draws = UvDrawPolicy::Screen;

		bool pin_uniform_q = false;

		bool z_driven_depth = false;

		std::vector<StereoSceneRule> scenes;

		// stereo.firstPerson: used instead of the base/scene values while camera.lookAt drives the camera
		// (the head-locked first-person screen needs different separation/convergence than the world one).
		std::optional<float> fp_separation;
		std::optional<float> fp_convergence;

		std::optional<HudCollimate> hud_collimate;

		StereoMap map = StereoMap::Linear;
		std::vector<float> splits;
		std::vector<StereoBand> bands;
		std::optional<StereoLogParams> log_params;

		StereoResolvedMap resolved;
	};

	enum class CameraEncoding
	{
		F32,
		S16_12,
		S32Angle,
	};

	enum class CameraSource
	{
		Constant,
		HeadYaw,
		HeadPitch,
		HeadRoll,
		HeadX,
		HeadY,
		HeadZ,
	};

	enum class CameraCompose
	{
		Absolute,
		Delta,
		Anchored,
	};

	enum class CameraWrap
	{
		None,
		Deg360,
	};

	struct CameraGuard
	{
		u32 ee_address = 0;
		u32 equals = 0;
		u8 width = 4;
		bool not_equals = false;
		bool is_chain = false;
		u32 pointer_addr = 0;
		std::vector<s32> deref_offsets;
		s32 offset = 0;
	};

	struct CameraWriteOp
	{
		u32 ee_address = 0;
		bool relative = false;
		CameraEncoding encoding = CameraEncoding::F32;
		CameraSource source = CameraSource::Constant;
		CameraCompose compose = CameraCompose::Absolute;
		CameraWrap wrap = CameraWrap::None;
		float scale = 1.0f;
		float bias = 0.0f;
		float clamp_min = -std::numeric_limits<float>::infinity();
		float clamp_max = std::numeric_limits<float>::infinity();
		float axis_sign = 1.0f;
		bool anchor_head = true;
		std::vector<CameraGuard> when;
	};

	enum class MatrixComposeOrder
	{
		Pre,
		Post,
	};

	struct CameraMatrixOp
	{
		u32 ee_address = 0;
		bool relative = false;
		bool anchored = false;
		MatrixComposeOrder order = MatrixComposeOrder::Pre;
		bool also_transpose = false;
		u32 transpose_address = 0;
		float axis_sign_yaw = 1.0f;
		float axis_sign_pitch = 1.0f;
		float axis_sign_roll = 1.0f;
		std::vector<CameraGuard> when;
	};

	struct CameraFov
	{
		u32 ee_address = 0;
		CameraEncoding encoding = CameraEncoding::F32;
		float scale = 1.0f;
	};

	struct CameraBase
	{
		std::vector<u8> pattern;
		std::vector<u8> mask;
		u32 scan_start = 0x00100000;
		u32 scan_end = 0x02000000;
		bool is_pointer = false;
		u32 pointer_addr = 0;
		std::vector<s32> deref_offsets;
		bool is_indexed = false;
		u32 indexed_base = 0;
		s64 array_offset = 0;
		u32 index_addr = 0;
		u8 index_width = 1;
		u32 stride = 0;
		s64 base_offset = 0;
		bool has_validate = false;
		s64 validate_offset = 0;
		u32 validate_equals = 0;
	};

	struct CameraSilence
	{
		u32 ee_address = 0;
		u32 value_on = 0;
		u32 value_off = 0;
		// Optional: only patch/restore while these pass (e.g. the overlay holding the code is resident).
		std::vector<CameraGuard> when;
	};

	struct CameraCodeHook
	{
		bool enabled = false;
		u32 hook_address = 0;
		u32 cave_address = 0;
		u32 scratch_address = 0;
		u32 tail_jump_address = 0;
		CameraSource source = CameraSource::HeadYaw;
		float scale = 1.0f;
		float axis_sign = 1.0f;
		u8 target_fpr = 13;
		u8 scratch_fpr = 1;
		u8 addr_gpr = 1;
	};

	struct CameraPadLook
	{
		float max_look_deg = 90.0f;
		float engage_deg = 12.0f;
		float curve = 1.0f;
		bool latch = false;

		float release_deg = 4.0f;

		float stick_floor = 0.0f;
	};

	// First-person look-at camera for games whose renderer builds its view from an eye and a
	// target vector (e.g. Outbreak). Each vsync: eye = character position (read at base +
	// position_offset) + eye height + head translation; target = eye + distance * forward, where
	// forward's yaw is the character's heading plus head yaw and its pitch is head pitch.
	struct CameraLookAt
	{
		u32 eye_address = 0;
		u32 target_address = 0;
		s64 position_offset = 0;
		bool has_heading = false;
		s64 heading_offset = 0;
		float eye_height = 160.0f;
		float eye_forward = 0.0f;
		float distance = 100.0f;
		float units_per_meter = 0.0f;
		float yaw_sign = 1.0f;
		float pitch_sign = 1.0f;
		u32 roll_address = 0;
		float roll_sign = 1.0f;
		std::vector<CameraGuard> when;

		// Camera yaw anchor: the camera keeps its own yaw (taken from the heading whenever the camera
		// arms or recenters, then turned by snap turns) instead of following the heading every vsync.
		// Needed when the game's movement is camera-relative: following the heading feeds back, the
		// character turns toward where the head looks and the camera turns further.
		bool yaw_anchor = false;
		float snap_turn_deg = 0.0f;
		s8 snap_stick_hand = 1; // VRInputSnapshot hand whose thumbstick X snap-turns; -1 = none

		// Point-to-aim (needs yaw_anchor): while the stance value at base + aim_stance_offset equals
		// aim_stance_equals, the heading is written so the character faces where the aim hand points.
		bool has_aim = false;
		s64 aim_stance_offset = 0;
		u32 aim_stance_equals = 0;
		u8 aim_stance_width = 1;
		s8 aim_hand = 1;
		bool has_aim_pitch = false;
		s64 aim_pitch_offset = 0; // s16 binary angle (0x10000 = 360 deg)
		float aim_pitch_sign = 1.0f;
		float aim_pitch_clamp = 8192.0f; // s16 units

		// Smooth turning on the turn stick, degrees per second at full deflection (needs yaw_anchor).
		// Used unless the VR SnapTurn setting is on or this is 0; then snap_turn_deg applies.
		float smooth_turn_deg_s = 0.0f;

		// While these all pass (a menu, the map, pause), the camera holds its last view, the head no
		// longer steers it and first person counts as inactive, so the world screen is shown.
		std::vector<CameraGuard> pause_when;

		// A value at base + offset; all checks of a list must hold.
		struct RecordCheck
		{
			s64 offset = 0;
			u32 equals = 0;
			u8 width = 1;
			bool not_equals = false;
		};
		// Body follows view: while every check holds (standing idle, stick centred), the heading is
		// written to the camera yaw, so picking up and checking things faces where the player looks.
		std::vector<RecordCheck> body_follow;

		// Values held while first person is active (e.g. a camera-focus byte so the near-camera cull
		// hides the local body) and put back when it ends, if the game has not changed them since.
		struct Hold
		{
			u32 address = 0;
			u8 width = 4;
			u32 value = 0;
			// The value to put back when the one found already equals ours (a savestate made while held).
			bool has_restore = false;
			u32 restore = 0;
			// Only held (and put back) while these pass, e.g. the overlay holding the data is resident.
			std::vector<CameraGuard> when;
		};
		std::vector<Hold> holds;

		// The game's view matrix (4x4 f32, row vectors: look = -column 2, eye = -row 3 * R^T). Matching
		// it against recent writes tells which head pose each displayed frame was rendered from.
		u32 view_matrix_address = 0;
		// Which matched frame is on screen at a vsync: the newest matched view matrix minus this many
		// (1 = the game builds its next view right after a flip, while the last frame is displayed).
		u32 sync_frames_back = 1;
	};

	struct CameraProfile
	{
		std::vector<CameraWriteOp> writes;
		std::vector<CameraMatrixOp> matrix_writes;
		std::optional<CameraFov> fov;
		std::optional<CameraBase> base;
		std::vector<CameraGuard> guards;
		std::vector<CameraSilence> silence;
		std::vector<CameraCodeHook> code_hooks;
		std::optional<CameraPadLook> pad_look;
		std::optional<CameraLookAt> look_at;
		// Stay armed until the guards have failed this many vsyncs in a row, so a guard byte the game
		// flips for a frame inside its own update doesn't hand the camera back for a frame.
		u32 disarm_after_vsyncs = 0;
	};

	struct SpatialControlSpec
	{
		std::string device;
		SpatialControls::DeviceKind kind = SpatialControls::DeviceKind::Count;
		std::string id;
		struct
		{
			float side = 0.0f;
			float height = 0.0f;
			float forward = 0.0f;
			float yaw_deg = 0.0f;
		} placement;
		float travel = 0.12f;
		float sweep_deg = 60.0f;
		float arm_length = 0.20f;
		bool upright = false;
		float grab_radius = 0.05f;
		float grab_length = 0.12f;
		SpatialControls::GrabMode grab_mode = SpatialControls::GrabMode::Toggle;
		float break_away = 0.25f;
		SpatialControls::ReleaseMode release = SpatialControls::ReleaseMode::Latch;
		float spring_rate = 4.0f;
		bool one_way = false;
		int detented = 0;
		SpatialControls::PowerMode power = SpatialControls::PowerMode::Sum;
		float engage = 0.0f;
		SpatialControls::TwinMode mode = SpatialControls::TwinMode::Composed;
		float lever_sign = 1.0f;
		float steer_full_lock = 1.0f;
		float steer_deadband = 0.05f;
		float steer_curve = 1.0f;
		float steer_sign = 1.0f;
		float output_floor = 0.0f;
		float lock_to_lock_deg = 900.0f;
		float rim_radius = 0.18f;
		int gears = 6;
		bool has_reverse = true;
		bool has_neutral = true;
		float travel_x = 0.10f;
		float travel_y = 0.10f;
		bool twist = false;
		float twist_range_deg = 60.0f;
		bool rifle = false;
		bool aim_left = false;
		float zone_radius = 0.15f;
		SpatialControls::ZoneHand zone_hand = SpatialControls::ZoneHand::Either;
		bool zone_require_grip = true;
		std::string preset;
		std::vector<CameraGuard> when;
		struct Bind
		{
			std::string control;
			std::string target;
		};
		std::vector<Bind> bind;
		u32 pad_port = 0;
		int usb_port = -1;
	};

	struct SplitParams
	{
		enum class Layout : u8
		{
			Horizontal,
			Vertical,
		};
		enum class Mode : u8
		{
			Focus,
			Duo,
			Mirror,
			Local,
		};
		struct Probe
		{
			u32 ee_address = 0;
			u32 threshold = 0;
			bool at_least = false;
			u8 width = 4;
		};
		struct Rect
		{
			float x = 0.0f, y = 0.0f, w = 1.0f, h = 1.0f;
		};

		Layout layout = Layout::Horizontal;
		u8 views = 2;
		u8 local_view = 0;
		u8 local_pad_port = 0;
		Rect rects[2];
		bool rects_explicit = false;
		std::optional<Probe> active;
		Mode mode = Mode::Focus;
		float side_scale = 0.4f;
		float side_angle_deg = 35.0f;
		bool stereo_on = true;
	};

	struct Profile
	{
		std::string serial;
		std::string name;
		std::vector<u32> crcs;
		Tier tier = Tier::Screen;
		std::optional<StereoParams> stereo;
		std::optional<float> screen_distance;
		std::optional<float> screen_height;
		std::optional<float> screen_arc_deg;
		std::optional<bool> screen_follow_head;
		// screen.firstPerson: head-locked screen while camera.lookAt drives the camera (sized to the game
		// camera's FOV); the keys above apply the rest of the time (menus, item screen, cutscenes).
		std::optional<float> fp_screen_distance;
		std::optional<float> fp_screen_height;
		std::optional<float> fp_screen_arc_deg;
		std::optional<float> fp_screen_pose_lag_ms;
		std::optional<CameraProfile> camera;
		std::optional<SplitParams> split;
		std::vector<SpatialControlSpec> controls;
		std::string notes;
	};

	void EnsureLoaded();

	void ReloadIfChanged();

	struct LoadIssue
	{
		std::string file;
		std::string message;
	};

	const std::vector<LoadIssue>& ValidateAtLaunch();

	enum class StereoRail
	{
		Divergence,
		FixationGap,
	};

	struct StereoRailFinding
	{
		StereoRail rail = StereoRail::Divergence;
		std::string serial;
		std::string site;
		float arcmin = 0.0f;
		bool from_map = false;
		std::string message;
	};

	const std::vector<StereoRailFinding>& StereoRailFindings();

	struct ProfileAdvisory
	{
		std::string serial;
		std::string site;
		std::string message;
	};

	const std::vector<ProfileAdvisory>& ProfileAdvisories();

	struct Summary
	{
		std::string serial;
		std::string name;
		bool has_stereo = false;
		bool has_camera = false;
		float separation = 0.0f;
		float convergence = 0.0f;
	};

	std::vector<Summary> ListProfiles();

	const Profile* Lookup(const std::string_view serial, u32 crc);

	void Reset();

	bool SelfTestMultibandResolve();
}
