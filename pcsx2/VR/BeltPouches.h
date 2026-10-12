// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include "VR/SpatialControls.h"
#include "VR/VRInputState.h"

#include <array>
#include <string_view>

namespace VR::ProfileDB
{
	struct Pouch;
	struct PouchParams;
	struct Profile;
}

// Belt pouches in VR (profile pouches:), Dungeons of Eternity style: spots on the seated player's body, each tied to one
// of the game's item cells. While first person is armed (so never on obsrv, where it never arms), a hand reaching into a
// pouch ticks: lightly, or firmer and twice while that cell holds an item (read from the local character's record;
// nothing is written). Squeezing the grip there opens the item screen if it is closed, through the profile's open Zone
// control exactly as a squeeze in that zone would (the character stops first, then the menu button is pressed), and asks
// the menu pointer to steer the game's cursor to the pouch's cell (MenuPointer::RequestCell: the same closed-loop D-pad
// presses, refused while a command submenu is open, never a confirm). With the item screen already up a squeeze just
// re-targets the cursor. The player confirms with the trigger as usual.
//
// An open hand reaching into a pouch whose cell holds an item also shows a short toast naming it: the item byte is a
// record index into the item table the profile's feedback.weapon names, read and checked as GameFeedback's weapon
// reader checks the equipped record (GameFeedback::ReadItemRecord), and named from ItemData with its count
// ("Kevin's 45 Auto 7", "45 Auto Magazine x7", "Green Herb"). Once per reach: not on a quick return to the pouch just
// left (kNameReentryMs), not for an empty pouch, and not while a menu is up (the item screen names its items itself).
namespace VR::BeltPouches
{
	using GuestReader = bool (*)(u32 address, u8 width, u32* value);

	static constexpr int kMaxPouches = 8;
	// The neck pivot the seat anchor hangs the pouches from: below and behind the eyes, in the head's own frame (the
	// usual neck model's numbers). Turning or tipping the head swings the eyes round it, not the body.
	static constexpr float kNeckDown = 0.08f;
	static constexpr float kNeckBack = 0.075f;
	// A hand in a pouch stays in it until this much past its radius.
	static constexpr float kExitMargin = 0.02f;
	// A hand back in the pouch it left less than this long ago (jitter on the edge) ticks again but names nothing.
	static constexpr float kNameReentryMs = 500.0f;
	// The profile's Zone controls PlaceZones looks at (Outbreak has three).
	static constexpr int kMaxZoneBalls = 16;

	// CPU thread, every input poll (VRInputSource::PollEvents): before MenuPointer::Poll, so a squeeze's request and the
	// pouch hands' grips reach it this poll, and before the controls compose, so an open press goes out this poll.
	// reader reads guest memory (null: the default reader); dt is the time since the last poll in seconds.
	void Poll(const VRInputSnapshot& snap, GuestReader reader, float dt);

	// The hands as a Zone control sees them this poll: a hand inside a pouch is moved out of every zone's reach. A hand
	// never counts as inside a pouch while it is within reach of a Zone control that takes that hand (InOwnZone), so this
	// only ever hides a hand from zones that could not have seen it there, and the hip gestures work exactly as without
	// pouches. A press a zone already holds carries on (a zone holds its button until the grip opens, wherever the hand
	// goes), and a grip closed in a pouch presses no zone the hand moves into. Unchanged while the pouches are not running.
	// CPU thread, after Poll.
	SpatialControls::Hands HandsOutsidePouches(const SpatialControls::Hands& hands);

	// True while a pouch squeezed with the item screen closed is opening it through the Zone control with this id (the
	// profile's pouches.open): one 0.15 s press, which the zone takes as a squeeze of its own (a stopFirst zone stops the
	// character, then presses). CPU thread, after Poll.
	bool OpenPressFor(std::string_view zone_id);

	// What the last Poll saw, for PINE (MsgVRMenuPointer op 2) and the tests. Any thread.
	struct Telemetry
	{
		bool active = false;     // first person armed with the local record, and the profile has pouches
		bool screen_up = false;  // the menu pointer's menu (the item screen) is up
		bool menu_mode = false;  // a menu is up (item screen, map, pause)
		bool head_anchor = false; // placed as the zones (anchor: head) rather than on the seat
		bool open_pressing = false;
		bool suspended = false; // the item screen is up and a pouches.suspendWhen guard passes
		std::array<s8, 2> hover{{-1, -1}}; // the pouch each hand is in, -1 none
		std::array<bool, 2> owned{};       // grips belonging to the pouches (not a menu pointer override)
		u8 count = 0;
		struct PouchView
		{
			float centre[3] = {0.0f, 0.0f, 0.0f}; // in the XR base space, where the hands are
			float radius = 0.0f;
			s8 cell = -1;
			s8 item = -1; // 1 the cell holds an item, 0 it is empty, -1 not known
			u8 item_byte = 0;
		};
		std::array<PouchView, kMaxPouches> pouches{};
		// Counts since the emulator started.
		u32 enters = 0;     // hands reaching into a pouch with the grip open (each a tick)
		u32 item_ticks = 0; // of which the firm double tick of a held item
		u32 squeezes = 0;
		u32 opens = 0;    // open presses sent through the open zone
		u32 requests = 0; // cell requests sent to the menu pointer
		u32 refused = 0;  // squeezes that did nothing (a submenu, another menu, nothing to open with)
	};
	Telemetry GetTelemetry();

	// --- The parts below are pure, for the self-test. ---

	// Where a pouch's head-relative placement is measured from, and the way the pouches face.
	struct BodyFrame
	{
		bool valid = false;
		SpatialControls::Vec3 origin;
		float yaw = 0.0f; // radians, OpenXR yaw (0 faces -Z, positive turns left), as SpatialControls::HeadAnchor
	};

	// anchor: seat. The neck pivot under the head (kNeckDown below and kNeckBack behind the eyes, in the head's own frame)
	// stays put as the head turns or looks down; the pouches hang from it as the profile places them from an upright head,
	// facing yaw (the forward the player last recentered to).
	BodyFrame SeatFrame(const SpatialControls::Vec3& head_pos, const SpatialControls::Quat& head_q, float yaw);

	// anchor: head. As the Zone controls: from the head's position, facing its yaw.
	BodyFrame HeadFrame(const SpatialControls::Vec3& head_pos, float yaw);

	// A pouch's centre in the XR base space.
	SpatialControls::Vec3 PouchCentre(const BodyFrame& f, const ProfileDB::Pouch& pouch);

	// The pouch (an index into p.list) a hand at pos is in: kept while within its radius plus kExitMargin, else the
	// nearest one whose radius it is inside; -1 none, or when the profile's hand rule leaves this hand (0 left, 1 right)
	// out.
	int PouchUnder(const ProfileDB::PouchParams& p, const BodyFrame& f, int hand, const SpatialControls::Vec3& pos, int prev);

	// One Zone control as a ball: where a hand may be and still be inside it (its radius plus its exit margin).
	struct ZoneBall
	{
		SpatialControls::Vec3 centre;
		float reach = 0.0f;
		SpatialControls::ZoneHand hand = SpatialControls::ZoneHand::Either;
	};
	struct ZoneBalls
	{
		std::array<ZoneBall, kMaxZoneBalls> ball{};
		int count = 0;
	};

	// The profile's Zone controls placed from head the way VRInputSource steps them (from the head's position, turned
	// with its yaw), each as the ball of its radius plus the zones' exit margin.
	ZoneBalls PlaceZones(const ProfileDB::Profile& profile, const SpatialControls::Anchor& head);

	// A hand (0 left, 1 right) at pos is within reach of a Zone control that takes that hand: the zone may have it, so no
	// pouch does. The zones turn with the head and the seat-anchored pouches do not, so this is decided every poll.
	bool InOwnZone(const ZoneBalls& zones, int hand, const SpatialControls::Vec3& pos);

	struct HandIn
	{
		bool valid = false;
		SpatialControls::Vec3 position; // the grip pose
		float squeeze = 0.0f;
		bool in_zone = false; // InOwnZone: the zone's, never a pouch's
	};

	struct StepIn
	{
		bool active = false; // first person armed with the local record, the VM running, the profile has pouches
		BodyFrame frame;
		std::array<HandIn, 2> hands{};
		bool screen_up = false;    // the menu pointer's menu (the item screen) is up
		bool menu = false;         // menu mode: a menu is up (item screen, map, pause)
		bool first_person = false; // first person drives the camera (no menu up)
		bool suspend = false;      // the item screen is up and a pouches.suspendWhen guard passes (a command submenu)
		bool pointer = false;      // point-to-select can take a request (the profile has a menuPointer, the setting on)
		int cells = 0;             // menuPointer.cells
		bool can_open = false;     // pouches.open names one of the profile's Zone controls
		std::array<s8, kMaxPouches> items{}; // per pouch: 1 the cell holds an item, 0 empty, -1 not known
		float dt_ms = 1000.0f / 60.0f;
	};

	enum class Action : u8
	{
		None,           // a squeeze while the screen is still opening, for a cell that cannot be steered to
		Request,        // the item screen is up (or opening): steer to the cell
		OpenAndRequest, // closed: open it, then steer
		Open,           // closed: open it (the cell cannot be steered to)
		Refused,        // a submenu, another menu, or nothing to open the screen with
	};

	enum class Tick : u8
	{
		None,
		Light,   // reached an empty pouch (or one that cannot tell)
		Firm,    // one of the double tick of a pouch holding an item
		Grab,    // a squeeze that did something
		Refused, // a squeeze that did nothing
	};

	struct StepOut
	{
		std::array<int, 2> hover{{-1, -1}};
		std::array<bool, 2> entered{};
		std::array<Tick, 2> reach{};   // reaching into a pouch with the grip open: Light, or Firm for a held item
		std::array<bool, 2> owned{};   // grip belongs to the pouches: in one, or still closed after a squeeze in one
		std::array<bool, 2> claimed{}; // in a pouch: the Zone controls do not see the hand
		std::array<Tick, 2> tick{};    // the pulse to play this poll (a reach, a double tick's second beat, a squeeze)
		std::array<bool, 2> name{};    // a reach into a pouch holding an item: show the item's name (once per reach)
		std::array<Action, 2> action{};
		std::array<int, 2> action_pouch{{-1, -1}};
		std::array<const char*, 2> refused_why{};
		bool open_press = false; // start an open press this poll
	};

	struct HandState
	{
		int pouch = -1;
		bool primed = false;   // squeezed below tracks this hand (a grip closed when it appears is no squeeze)
		bool squeezed = false; // with hysteresis
		bool owned = false;    // squeezed in a pouch and not opened since
		float second_tick_ms = -1.0f; // the double tick's second beat is due in this long; below 0 none
		int left_pouch = -1;          // the pouch this hand last left
		float since_left_ms = 1.0e9f; // how long ago it left it
	};

	struct State
	{
		std::array<HandState, 2> hand{};
		float since_open_ms = 1.0e9f; // since an open press was sent
		void Reset()
		{
			hand = {};
			since_open_ms = 1.0e9f;
		}
	};

	StepOut Step(const ProfileDB::PouchParams& p, const StepIn& in, State& st);

	// The frames, hit tests, ticks, names and squeeze decisions on scripted hands, and the shipped Outbreak layout against
	// the hip zones; also runs GameFeedback::ItemSelfTest. Runs once from Poll when PCSX2_VR_SELFTEST is set; logs "(VR)
	// BeltPouches self-test: passed (...)" or "... FAIL: <first check>".
	bool SelfTest();
}
