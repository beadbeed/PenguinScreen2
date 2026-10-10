// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/VRInput.h"
#include "VR/XRSession.h"

#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace VR
{
	namespace
	{
		constexpr const char* HAND_PATH[2] = {"/user/hand/left", "/user/hand/right"};

		constexpr const char* TOUCH_PROFILE = "/interaction_profiles/oculus/touch_controller";
		constexpr const char* SIMPLE_PROFILE = "/interaction_profiles/khr/simple_controller";
		constexpr const char* FRAME_PROFILE = "/interaction_profiles/valve/frame_controller";

		std::mutex s_snapshot_mutex;
		VRInputSnapshot s_snapshot;

		// A publisher that stops (a failed sync, a dead session, a stalled frame loop) must not leave
		// the last held trigger or stick active while emulation keeps running. Normal publishing is
		// once per presented frame, so this is far longer than any healthy gap.
		constexpr std::uint64_t kSnapshotMaxAgeMs = 500;

		std::uint64_t NowMs()
		{
			return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		void PublishSnapshot(VRInputSnapshot snapshot)
		{
			std::lock_guard lock{s_snapshot_mutex};
			snapshot.generation = s_snapshot.generation + 1;
			snapshot.published_ms = NowMs();
			s_snapshot = snapshot;
		}

		// Neutral and inactive: every consumer releases its controls. Only when something was
		// active, so a session that never had input doesn't appear to come alive.
		void PublishNeutralIfActive()
		{
			bool was_active;
			{
				std::lock_guard lock{s_snapshot_mutex};
				was_active = s_snapshot.generation != 0 && s_snapshot.actions_active;
			}
			if (was_active)
				PublishSnapshot(VRInputSnapshot{});
		}

		void ResetSnapshot()
		{
			std::lock_guard lock{s_snapshot_mutex};
			s_snapshot = VRInputSnapshot{};
		}

		VRInput* s_publisher = nullptr;

		std::atomic<int> s_haptic_request[2] = {-1, -1};
		int s_haptic_applied[2] = {0, 0};

		std::atomic<int> s_pulse_request[2] = {-1, -1};

		int PackPulse(float amplitude, float seconds)
		{
			if (!std::isfinite(amplitude))
				amplitude = 0.f;
			if (!std::isfinite(seconds))
				seconds = 0.f;
			const int permille = static_cast<int>(std::lround(std::clamp(amplitude, 0.f, 1.f) * 1000.f));
			const int ms = static_cast<int>(std::lround(std::clamp(seconds, 0.f, 10.f) * 1000.f));
			return permille | (ms << 16);
		}

		bool CheckXR(XrResult res, const char* what)
		{
			if (XR_SUCCEEDED(res))
				return true;

			char buf[XR_MAX_RESULT_STRING_SIZE] = "?";
			if (XRSession::HasInstance())
				xrResultToString(XRSession::GetInstance(), res, buf);
			Console.Error("(VR) %s failed: %s (%d)", what, buf, static_cast<int>(res));
			return false;
		}

		void CopyPose(const XrPosef& in, VRPose* out)
		{
			out->orientation_xyzw = {in.orientation.x, in.orientation.y, in.orientation.z, in.orientation.w};
			out->position_xyz = {in.position.x, in.position.y, in.position.z};
		}

		bool ReadBool(XrSession session, XrAction action, XrPath subaction_path)
		{
			if (action == XR_NULL_HANDLE)
				return false;

			XrActionStateGetInfo get_info = {XR_TYPE_ACTION_STATE_GET_INFO};
			get_info.action = action;
			get_info.subactionPath = subaction_path;

			XrActionStateBoolean state = {XR_TYPE_ACTION_STATE_BOOLEAN};
			if (XR_FAILED(xrGetActionStateBoolean(session, &get_info, &state)))
				return false;

			return state.isActive == XR_TRUE && state.currentState == XR_TRUE;
		}

		float ReadFloat(XrSession session, XrAction action, XrPath subaction_path)
		{
			if (action == XR_NULL_HANDLE)
				return 0.f;

			XrActionStateGetInfo get_info = {XR_TYPE_ACTION_STATE_GET_INFO};
			get_info.action = action;
			get_info.subactionPath = subaction_path;

			XrActionStateFloat state = {XR_TYPE_ACTION_STATE_FLOAT};
			if (XR_FAILED(xrGetActionStateFloat(session, &get_info, &state)) || state.isActive != XR_TRUE)
				return 0.f;

			return std::isfinite(state.currentState) ? state.currentState : 0.f;
		}

		XrVector2f ReadVector2(XrSession session, XrAction action, XrPath subaction_path)
		{
			if (action == XR_NULL_HANDLE)
				return {0.f, 0.f};

			XrActionStateGetInfo get_info = {XR_TYPE_ACTION_STATE_GET_INFO};
			get_info.action = action;
			get_info.subactionPath = subaction_path;

			XrActionStateVector2f state = {XR_TYPE_ACTION_STATE_VECTOR2F};
			if (XR_FAILED(xrGetActionStateVector2f(session, &get_info, &state)) || state.isActive != XR_TRUE)
				return {0.f, 0.f};

			if (!std::isfinite(state.currentState.x) || !std::isfinite(state.currentState.y))
				return {0.f, 0.f};

			return state.currentState;
		}
	}

	VRInputSnapshot GetInputSnapshot()
	{
		std::lock_guard lock{s_snapshot_mutex};
		VRInputSnapshot snap = s_snapshot;
		if (snap.generation != 0 && snap.actions_active && NowMs() - snap.published_ms > kSnapshotMaxAgeMs)
		{
			// Producer went quiet: report neutral, keep the generation so "live" is unchanged.
			const std::uint64_t generation = snap.generation;
			snap = VRInputSnapshot{};
			snap.generation = generation;
			snap.published_ms = s_snapshot.published_ms;
		}
		return snap;
	}

	VRScreenTransform GetScreenTransform()
	{
		return VRScreenTransform{};
	}

	VRInput::VRInput() = default;

	VRInput::~VRInput()
	{
		Shutdown();
	}

	bool VRInput::CreateAction(XrInstance instance, XrActionType type, const char* name,
		const char* localized_name, XrAction* out)
	{
		XrActionCreateInfo create_info = {XR_TYPE_ACTION_CREATE_INFO};
		create_info.actionType = type;
		std::strncpy(create_info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
		std::strncpy(create_info.localizedActionName, localized_name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
		create_info.countSubactionPaths = static_cast<uint32_t>(m_hand_paths.size());
		create_info.subactionPaths = m_hand_paths.data();

		const XrResult res = xrCreateAction(m_action_set, &create_info, out);
		if (XR_FAILED(res))
		{
			char what[64];
			std::snprintf(what, sizeof(what), "xrCreateAction(%s)", name);
			return CheckXR(res, what);
		}
		return true;
	}

	bool VRInput::CreateActionSetAndActions(XrInstance instance)
	{
		XrActionSetCreateInfo set_info = {XR_TYPE_ACTION_SET_CREATE_INFO};
		std::strncpy(set_info.actionSetName, "pcsx2_vr", XR_MAX_ACTION_SET_NAME_SIZE - 1);
		std::strncpy(set_info.localizedActionSetName, "PCSX2 VR", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
		set_info.priority = 0;
		if (!CheckXR(xrCreateActionSet(instance, &set_info, &m_action_set), "xrCreateActionSet"))
		{
			m_action_set = XR_NULL_HANDLE;
			return false;
		}

		for (int hand = 0; hand < HAND_COUNT; ++hand)
		{
			if (!CheckXR(xrStringToPath(instance, HAND_PATH[hand], &m_hand_paths[hand]), "xrStringToPath(hand)"))
				return false;
		}

		// clang-format off
		return CreateAction(instance, XR_ACTION_TYPE_POSE_INPUT,       "aim_pose",         "Aim Pose",         &m_aim_pose) &&
		       CreateAction(instance, XR_ACTION_TYPE_POSE_INPUT,       "grip_pose",        "Grip Pose",        &m_grip_pose) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_a",         "A Button",         &m_button_a) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_b",         "B Button",         &m_button_b) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_x",         "X Button",         &m_button_x) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_y",         "Y Button",         &m_button_y) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_menu",      "Menu Button",      &m_button_menu) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_view",      "View Button",      &m_button_view) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "thumbstick_click", "Thumbstick Click", &m_thumbstick_click) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "dpad_up",          "D-pad Up",         &m_dpad_up) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "dpad_down",        "D-pad Down",       &m_dpad_down) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "dpad_left",        "D-pad Left",       &m_dpad_left) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "dpad_right",       "D-pad Right",      &m_dpad_right) &&
		       CreateAction(instance, XR_ACTION_TYPE_BOOLEAN_INPUT,    "bumper",           "Bumper",           &m_bumper) &&
		       CreateAction(instance, XR_ACTION_TYPE_FLOAT_INPUT,      "trigger",          "Trigger",          &m_trigger) &&
		       CreateAction(instance, XR_ACTION_TYPE_FLOAT_INPUT,      "grip",             "Grip",             &m_grip) &&
		       CreateAction(instance, XR_ACTION_TYPE_VECTOR2F_INPUT,   "thumbstick",       "Thumbstick",       &m_thumbstick) &&
		       CreateAction(instance, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic",           "Haptic Feedback",  &m_haptic);
		// clang-format on
	}

	bool VRInput::SuggestBindings(XrInstance instance)
	{
		struct Suggestion
		{
			XrAction action;
			const char* path;
		};

		const auto suggest = [&](const char* profile, const std::vector<Suggestion>& suggestions) {
			std::vector<XrActionSuggestedBinding> bindings;
			bindings.reserve(suggestions.size());
			for (const Suggestion& suggestion : suggestions)
			{
				XrPath path = XR_NULL_PATH;
				if (XR_FAILED(xrStringToPath(instance, suggestion.path, &path)))
				{
					Console.Warning("(VR) xrStringToPath failed for %s", suggestion.path);
					continue;
				}
				bindings.push_back({suggestion.action, path});
			}

			XrPath profile_path = XR_NULL_PATH;
			if (!CheckXR(xrStringToPath(instance, profile, &profile_path), "xrStringToPath(profile)"))
				return false;

			XrInteractionProfileSuggestedBinding suggested = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
			suggested.interactionProfile = profile_path;
			suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
			suggested.suggestedBindings = bindings.data();

			if (XR_FAILED(xrSuggestInteractionProfileBindings(instance, &suggested)))
			{
				Console.Warning("(VR) Runtime rejected the suggested bindings for %s", profile);
				return false;
			}

			Console.WriteLn("(VR) Suggested %u controller bindings for %s", static_cast<unsigned>(bindings.size()), profile);
			return true;
		};

		const bool touch_ok = suggest(TOUCH_PROFILE, {
			{m_aim_pose,         "/user/hand/left/input/aim/pose"},
			{m_aim_pose,         "/user/hand/right/input/aim/pose"},
			{m_grip_pose,        "/user/hand/left/input/grip/pose"},
			{m_grip_pose,        "/user/hand/right/input/grip/pose"},
			{m_button_a,         "/user/hand/right/input/a/click"},
			{m_button_b,         "/user/hand/right/input/b/click"},
			{m_button_x,         "/user/hand/left/input/x/click"},
			{m_button_y,         "/user/hand/left/input/y/click"},
			{m_button_menu,      "/user/hand/left/input/menu/click"},
			{m_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
			{m_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
			{m_trigger,          "/user/hand/left/input/trigger/value"},
			{m_trigger,          "/user/hand/right/input/trigger/value"},
			{m_grip,             "/user/hand/left/input/squeeze/value"},
			{m_grip,             "/user/hand/right/input/squeeze/value"},
			{m_thumbstick,       "/user/hand/left/input/thumbstick"},
			{m_thumbstick,       "/user/hand/right/input/thumbstick"},
			{m_haptic,           "/user/hand/left/output/haptic"},
			{m_haptic,           "/user/hand/right/output/haptic"},
		});

		const bool simple_ok = suggest(SIMPLE_PROFILE, {
			{m_aim_pose,    "/user/hand/left/input/aim/pose"},
			{m_aim_pose,    "/user/hand/right/input/aim/pose"},
			{m_grip_pose,   "/user/hand/left/input/grip/pose"},
			{m_grip_pose,   "/user/hand/right/input/grip/pose"},
			{m_trigger,     "/user/hand/left/input/select/click"},
			{m_trigger,     "/user/hand/right/input/select/click"},
			{m_button_a,    "/user/hand/left/input/select/click"},
			{m_button_a,    "/user/hand/right/input/select/click"},
			{m_button_menu, "/user/hand/left/input/menu/click"},
			{m_button_menu, "/user/hand/right/input/menu/click"},
			{m_haptic,      "/user/hand/left/output/haptic"},
			{m_haptic,      "/user/hand/right/output/haptic"},
		});

		// Valve Steam Frame (SteamVR) is gamepad-shaped: A/B/X/Y and Menu are on the right
		// controller, the D-pad and View on the left. Without its own bindings SteamVR remaps the
		// Touch ones onto it and X/Y come out as B. A runtime rejects a whole suggestion over one
		// path it doesn't support, so if the full set fails, retry without the D-pad, bumpers and
		// View. The Frame is optional: only Touch and Simple decide whether input works at all.
		const std::vector<Suggestion> frame_minimal = {
			{m_aim_pose,         "/user/hand/left/input/aim/pose"},
			{m_aim_pose,         "/user/hand/right/input/aim/pose"},
			{m_grip_pose,        "/user/hand/left/input/grip/pose"},
			{m_grip_pose,        "/user/hand/right/input/grip/pose"},
			{m_button_a,         "/user/hand/right/input/a/click"},
			{m_button_b,         "/user/hand/right/input/b/click"},
			{m_button_x,         "/user/hand/right/input/x/click"},
			{m_button_y,         "/user/hand/right/input/y/click"},
			{m_button_menu,      "/user/hand/right/input/menu/click"},
			{m_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
			{m_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
			{m_trigger,          "/user/hand/left/input/trigger/value"},
			{m_trigger,          "/user/hand/right/input/trigger/value"},
			{m_grip,             "/user/hand/left/input/squeeze/value"},
			{m_grip,             "/user/hand/right/input/squeeze/value"},
			{m_thumbstick,       "/user/hand/left/input/thumbstick"},
			{m_thumbstick,       "/user/hand/right/input/thumbstick"},
			{m_haptic,           "/user/hand/left/output/haptic"},
			{m_haptic,           "/user/hand/right/output/haptic"},
		};
		const std::vector<Suggestion> frame_extra = {
			{m_button_view, "/user/hand/left/input/view/click"},
			{m_dpad_up,     "/user/hand/left/input/dpad_up/click"},
			{m_dpad_down,   "/user/hand/left/input/dpad_down/click"},
			{m_dpad_left,   "/user/hand/left/input/dpad_left/click"},
			{m_dpad_right,  "/user/hand/left/input/dpad_right/click"},
			{m_bumper,      "/user/hand/left/input/bumper/click"},
			{m_bumper,      "/user/hand/right/input/bumper/click"},
		};
		std::vector<Suggestion> frame_full = frame_minimal;
		frame_full.insert(frame_full.end(), frame_extra.begin(), frame_extra.end());

		// The profile only exists with XR_VALVE_frame_controller_interaction (SteamVR); other runtimes skip it.
		if (XRSession::HasFrameControllerInteraction())
		{
			const bool frame_full_ok = suggest(FRAME_PROFILE, frame_full);
			const bool frame_minimal_ok = !frame_full_ok && suggest(FRAME_PROFILE, frame_minimal);
			if (frame_full_ok)
				Console.WriteLn("(VR) Steam Frame controller: full bindings accepted (D-pad, bumpers and View included).");
			else if (frame_minimal_ok)
				Console.WriteLn("(VR) Steam Frame controller: minimal bindings accepted (no D-pad, bumpers or View).");
			else
				Console.Warning("(VR) Steam Frame controller bindings not accepted (only matters with Steam Frame controllers).");
		}

		if (!touch_ok && !simple_ok)
		{
			Console.Error("(VR) No interaction profile bindings were accepted; controllers will produce nothing.");
			return false;
		}

		return true;
	}

	bool VRInput::AttachAndCreateSpaces(XrInstance instance, XrSession session)
	{
		XrSessionActionSetsAttachInfo attach_info = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
		attach_info.countActionSets = 1;
		attach_info.actionSets = &m_action_set;
		if (!CheckXR(xrAttachSessionActionSets(session, &attach_info), "xrAttachSessionActionSets"))
			return false;

		for (int hand = 0; hand < HAND_COUNT; ++hand)
		{
			XrActionSpaceCreateInfo space_info = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
			space_info.subactionPath = m_hand_paths[hand];
			space_info.poseInActionSpace.orientation.w = 1.0f;

			space_info.action = m_aim_pose;
			if (!CheckXR(xrCreateActionSpace(session, &space_info, &m_aim_spaces[hand]), "xrCreateActionSpace(aim)"))
				return false;

			space_info.action = m_grip_pose;
			if (!CheckXR(xrCreateActionSpace(session, &space_info, &m_grip_spaces[hand]), "xrCreateActionSpace(grip)"))
				return false;
		}

		XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
		rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
		rsci.poseInReferenceSpace.orientation.w = 1.0f;
		if (XR_FAILED(xrCreateReferenceSpace(session, &rsci, &m_view_space)))
		{
			m_view_space = XR_NULL_HANDLE;
			Console.Warning("(VR) VIEW space for the input snapshot failed; head_pose stays invalid.");
		}

		return true;
	}

	bool VRInput::Initialize()
	{
		if (m_action_set != XR_NULL_HANDLE)
		{
			Console.Error("(VR) VRInput::Initialize called twice.");
			return false;
		}
		if (!XRSession::HasInstance() || !XRSession::HasSession())
		{
			Console.Error("(VR) VRInput::Initialize needs a live instance and session.");
			return false;
		}

		const XrInstance instance = XRSession::GetInstance();
		if (!CreateActionSetAndActions(instance) || !SuggestBindings(instance) ||
			!AttachAndCreateSpaces(instance, XRSession::GetSession()))
		{
			Shutdown();
			return false;
		}

		m_attached = true;
		m_locate_warned = false;
		m_last_actions_active = -1;
		s_publisher = this;
		ResetSnapshot();
		Console.WriteLn("(VR) Controller action set attached (spatial controls can read the hands).");
		return true;
	}

	void VRInput::Shutdown()
	{
		const bool session_alive = m_attached && XRSession::HasSession();
		const bool instance_alive = m_attached && XRSession::HasInstance();

		for (int hand = 0; hand < HAND_COUNT; ++hand)
		{
			if (m_aim_spaces[hand] != XR_NULL_HANDLE)
			{
				if (session_alive)
					xrDestroySpace(m_aim_spaces[hand]);
				m_aim_spaces[hand] = XR_NULL_HANDLE;
			}
			if (m_grip_spaces[hand] != XR_NULL_HANDLE)
			{
				if (session_alive)
					xrDestroySpace(m_grip_spaces[hand]);
				m_grip_spaces[hand] = XR_NULL_HANDLE;
			}
		}
		if (m_view_space != XR_NULL_HANDLE)
		{
			if (session_alive)
				xrDestroySpace(m_view_space);
			m_view_space = XR_NULL_HANDLE;
		}

		for (XrAction* action : {&m_aim_pose, &m_grip_pose, &m_button_a, &m_button_b, &m_button_x, &m_button_y,
				 &m_button_menu, &m_button_view, &m_thumbstick_click, &m_dpad_up, &m_dpad_down, &m_dpad_left,
				 &m_dpad_right, &m_bumper, &m_trigger, &m_grip, &m_thumbstick, &m_haptic})
		{
			if (*action != XR_NULL_HANDLE)
			{
				if (instance_alive)
					xrDestroyAction(*action);
				*action = XR_NULL_HANDLE;
			}
		}

		if (m_action_set != XR_NULL_HANDLE)
		{
			if (instance_alive)
				xrDestroyActionSet(m_action_set);
			m_action_set = XR_NULL_HANDLE;
			Console.WriteLn("(VR) Controller action set destroyed.");
		}

		m_hand_paths = {};
		m_attached = false;
		if (s_publisher == this)
		{
			s_publisher = nullptr;
			ResetSnapshot();
		}
	}

	bool VRInput::ReadHand(XrSession session, int hand, XrTime time, VRHandState* out)
	{
		const XrPath hand_path = m_hand_paths[hand];

		out->a = ReadBool(session, m_button_a, hand_path);
		out->b = ReadBool(session, m_button_b, hand_path);
		out->x = ReadBool(session, m_button_x, hand_path);
		out->y = ReadBool(session, m_button_y, hand_path);
		out->menu = ReadBool(session, m_button_menu, hand_path);
		out->thumbstick_click = ReadBool(session, m_thumbstick_click, hand_path);
		out->dpad_up = ReadBool(session, m_dpad_up, hand_path);
		out->dpad_down = ReadBool(session, m_dpad_down, hand_path);
		out->dpad_left = ReadBool(session, m_dpad_left, hand_path);
		out->dpad_right = ReadBool(session, m_dpad_right, hand_path);
		out->bumper = ReadBool(session, m_bumper, hand_path);
		out->view = ReadBool(session, m_button_view, hand_path);
		out->trigger = ReadFloat(session, m_trigger, hand_path);
		out->grip = ReadFloat(session, m_grip, hand_path);

		const XrVector2f thumbstick = ReadVector2(session, m_thumbstick, hand_path);
		out->thumbstick_x = thumbstick.x;
		out->thumbstick_y = thumbstick.y;

		if (time == 0)
			return false;

		const XrSpace base_space = XRSession::GetSpace();
		constexpr XrSpaceLocationFlags POSE_VALID =
			XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;

		bool ok = true;

		XrSpaceLocation aim_location = {XR_TYPE_SPACE_LOCATION};
		if (XR_SUCCEEDED(xrLocateSpace(m_aim_spaces[hand], base_space, time, &aim_location)))
		{
			if ((aim_location.locationFlags & POSE_VALID) == POSE_VALID)
			{
				CopyPose(aim_location.pose, &out->aim_pose);
				out->aim_pose.valid = true;
			}
		}
		else
		{
			ok = false;
		}

		XrSpaceVelocity grip_velocity = {XR_TYPE_SPACE_VELOCITY};
		XrSpaceLocation grip_location = {XR_TYPE_SPACE_LOCATION, &grip_velocity};
		if (XR_SUCCEEDED(xrLocateSpace(m_grip_spaces[hand], base_space, time, &grip_location)))
		{
			if ((grip_location.locationFlags & POSE_VALID) == POSE_VALID)
			{
				CopyPose(grip_location.pose, &out->grip_pose);
				out->grip_pose.valid = true;
			}
			if ((grip_velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0)
			{
				out->grip_linear_velocity = {grip_velocity.linearVelocity.x, grip_velocity.linearVelocity.y,
					grip_velocity.linearVelocity.z};
			}
		}
		else
		{
			ok = false;
		}

		return ok;
	}

	bool VRInput::Update(XrTime display_time)
	{
		if (m_action_set == XR_NULL_HANDLE || !m_attached)
			return false;

		const XrSession session = XRSession::GetSession();
		if (session == XR_NULL_HANDLE || !XRSession::IsSessionRunning())
		{
			PublishNeutralIfActive();
			return false;
		}

		XrActiveActionSet active_set = {m_action_set, XR_NULL_PATH};
		XrActionsSyncInfo sync_info = {XR_TYPE_ACTIONS_SYNC_INFO};
		sync_info.countActiveActionSets = 1;
		sync_info.activeActionSets = &active_set;
		const XrResult sync_result = xrSyncActions(session, &sync_info);
		if (!CheckXR(sync_result, "xrSyncActions"))
		{
			PublishNeutralIfActive();
			return false;
		}
		const bool actions_active = sync_result == XR_SUCCESS;
		if (m_last_actions_active != static_cast<int>(actions_active))
		{
			m_last_actions_active = static_cast<int>(actions_active);
			Console.WriteLn("(VR) Controller actions %s (session %sfocused).",
				actions_active ? "active" : "inactive", actions_active ? "" : "not ");
		}

		const XrTime time = display_time;

		VRInputSnapshot snapshot;
		snapshot.actions_active = actions_active;
		bool located = true;
		for (int hand = 0; hand < HAND_COUNT; ++hand)
			located = ReadHand(session, hand, time, &snapshot.hands[hand]) && located;

		if (time != 0 && m_view_space != XR_NULL_HANDLE)
		{
			XrSpaceLocation head_location = {XR_TYPE_SPACE_LOCATION};
			if (XR_SUCCEEDED(xrLocateSpace(m_view_space, XRSession::GetSpace(), time, &head_location)))
			{
				constexpr XrSpaceLocationFlags POSE_VALID =
					XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
				if ((head_location.locationFlags & POSE_VALID) == POSE_VALID)
				{
					CopyPose(head_location.pose, &snapshot.head_pose);
					snapshot.head_pose.valid = true;
				}
			}
			else
			{
				located = false;
			}
		}

		if (!located && !m_locate_warned)
		{
			m_locate_warned = true;
			if (time == 0)
			{
				Console.Warning("(VR) Input update without a display time; poses are published invalid, "
								"buttons/axes stay live. Logged once.");
			}
			else
			{
				Console.Warning("(VR) xrLocateSpace failed (time %lld); poses will be published invalid "
								"until it recovers. Logged once.",
					static_cast<long long>(time));
			}
		}

		PublishSnapshot(snapshot);
		return true;
	}

	void VRInput::TriggerHaptic(int hand, float duration_seconds, float frequency_hz, float amplitude)
	{
		if (m_haptic == XR_NULL_HANDLE || !m_attached || !XRSession::IsSessionRunning())
			return;
		if (hand < 0 || hand >= HAND_COUNT)
			return;

		XrHapticVibration vibration = {XR_TYPE_HAPTIC_VIBRATION};
		vibration.duration = duration_seconds > 0.f ?
								 static_cast<XrDuration>(duration_seconds * 1'000'000'000.0) :
								 XR_MIN_HAPTIC_DURATION;
		vibration.frequency = frequency_hz > 0.f ? frequency_hz : XR_FREQUENCY_UNSPECIFIED;
		vibration.amplitude = std::clamp(amplitude, 0.f, 1.f);

		XrHapticActionInfo action_info = {XR_TYPE_HAPTIC_ACTION_INFO};
		action_info.action = m_haptic;
		action_info.subactionPath = m_hand_paths[hand];

		xrApplyHapticFeedback(XRSession::GetSession(), &action_info,
			reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
	}

	void VRInput::StopHaptic(int hand)
	{
		if (m_haptic == XR_NULL_HANDLE || !m_attached || !XRSession::IsSessionRunning())
			return;
		if (hand < 0 || hand >= HAND_COUNT)
			return;

		XrHapticActionInfo action_info = {XR_TYPE_HAPTIC_ACTION_INFO};
		action_info.action = m_haptic;
		action_info.subactionPath = m_hand_paths[hand];

		xrStopHapticFeedback(XRSession::GetSession(), &action_info);
	}

	namespace XRInput
	{
		namespace
		{
			VRInput s_input;
			constexpr float kHapticPulseSeconds = 0.05f;
		}

		bool Initialize()
		{
			for (std::atomic<int>& r : s_haptic_request)
				r.store(-1, std::memory_order_relaxed);
			for (std::atomic<int>& r : s_pulse_request)
				r.store(-1, std::memory_order_relaxed);
			s_haptic_applied[0] = s_haptic_applied[1] = 0;
			return s_input.Initialize();
		}

		void Update(XrTime display_time)
		{
			if (!s_input.IsInitialized())
				return;
			s_input.Update(display_time);

			for (int hand = 0; hand < 2; ++hand)
			{
				const int requested = s_haptic_request[hand].exchange(-1, std::memory_order_acq_rel);
				if (requested >= 0)
					s_haptic_applied[hand] = requested;
				if (s_haptic_applied[hand] > 0)
					s_input.TriggerHaptic(hand, kHapticPulseSeconds, 0.f, static_cast<float>(s_haptic_applied[hand]) / 1000.f);
				else if (requested == 0)
					s_input.StopHaptic(hand);

				const int pulse = s_pulse_request[hand].exchange(-1, std::memory_order_acq_rel);
				if (pulse >= 0)
					s_input.TriggerHaptic(hand, static_cast<float>(pulse >> 16) / 1000.f, 0.f, static_cast<float>(pulse & 0xFFFF) / 1000.f);
			}
		}

		void Shutdown()
		{
			s_input.Shutdown();
		}

		bool IsInitialized()
		{
			return s_input.IsInitialized();
		}

		void QueueHaptic(int hand, float amplitude)
		{
			if (hand < 0 || hand >= 2)
				return;
			if (!std::isfinite(amplitude))
				amplitude = 0.f;
			const int permille = static_cast<int>(std::lround(std::clamp(amplitude, 0.f, 1.f) * 1000.f));
			s_haptic_request[hand].store(permille, std::memory_order_release);
		}

		void QueuePulse(int hand, float amplitude, float seconds)
		{
			if (hand < 0 || hand >= 2)
				return;
			s_pulse_request[hand].store(PackPulse(amplitude, seconds), std::memory_order_release);
		}

		bool TakePendingPulseForTest(int hand, float* amplitude, float* seconds)
		{
			if (hand < 0 || hand >= 2)
				return false;
			const int pulse = s_pulse_request[hand].exchange(-1, std::memory_order_acq_rel);
			if (pulse < 0)
				return false;
			if (amplitude)
				*amplitude = static_cast<float>(pulse & 0xFFFF) / 1000.f;
			if (seconds)
				*seconds = static_cast<float>(pulse >> 16) / 1000.f;
			return true;
		}

		void PublishSnapshotForTest(const VRInputSnapshot& snapshot)
		{
			PublishSnapshot(snapshot);
		}

		void ResetSnapshotForTest()
		{
			ResetSnapshot();
		}
	}
}
