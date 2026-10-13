// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include "VR/SpatialControls.h"
#include "VR/VRInputState.h"

#include <array>
#include <string_view>
#include <vector>

// Body-zone markers in first person (VR setting ZoneMarkers): something to see where the body gestures work, so a hand
// is not reaching for nothing. The equipped weapon hangs in the right-hip holster zone while it is lowered (HandModel
// draws it as the hand holds it, without the glove), or, a long gun, slung over the right shoulder in the backpack zone
// (whose squeeze then raises it: SpatialControls' PressedLongGun); a small dim bag marks the left-hip belt zone, and
// faint rings mark the belt pouches. Each brightens while its hand is in reach. They are placed exactly where the tests that decide a
// squeeze place them: a zone from the input snapshot's head the way StepZone places it (SpatialControls::HeadAnchor, then
// PlaceControl), the pouches where the last pouch poll put them (BeltPouches::GetTelemetry). XRCompositor draws them; this
// file holds what the input poll publishes and the pure parts: where they go, which fit in the frame's layers, the bag's
// and the rings' image, and when an image has to be drawn again. No XR or Vulkan types.
namespace VR::ZoneMarkers
{
	// One quad layer, and one image, each. kHolster is the holstered weapon, at the hip or slung over the shoulder.
	static constexpr int kHolster = 0;
	static constexpr int kBelt = 1;
	static constexpr int kPouches = 2;
	// Hand targeting's volume (ItemReach::TargetMark, VR setting HandTargeting): a wireframe box, eight corner marks. It has
	// a slot of its own and is the first thing dropped when layers are short; nothing is drawn in it unless that setting
	// has published a target.
	static constexpr int kTarget = 3;
	static constexpr int kSlots = 4;

	// The images: square RGBA8, the hands' byte order (sRGB-encoded colour, unpremultiplied alpha), background alpha 0.
	static constexpr u32 kImageSize = 256;

	// The Zone controls that get a marker, by their id in the profile: the right-hip holster (readies the weapon), the
	// left-hip belt (opens the item screen) and the over-the-shoulder backpack, which carries a long gun (and otherwise
	// shows nothing: it is behind the ear, out of sight).
	enum class Zone : u8
	{
		Holster,
		Belt,
		Backpack,
		Count,
	};

	// "holster", "belt" or "backpack" (ASCII case ignored), else Count.
	Zone ZoneOf(std::string_view control_id);

	// The local character's equipped weapon is a long gun: feedback.weapon trusts the equipped record (GameFeedback's
	// Weapon) and ItemData calls its item a LongGun. It decides both where the holstered weapon is drawn (the shoulder
	// rather than the hip) and where a press of a zone binding PressedLongGun goes, so the two always agree. Any thread.
	bool LongGunEquipped();

	// Where a Zone control's press goes (VRInputSource, every poll): to PressedLongGun while long_gun says so (the
	// profile binds it and LongGunEquipped), else Pressed. Decided as the press begins and kept until it ends, so a
	// weapon change mid-squeeze never moves a held press to the other button.
	struct PressRoute
	{
		bool pressing = false;
		bool long_gun = false;
	};
	// pressed: the zone's own press this poll. True when it goes out on PressedLongGun. Pure.
	bool RouteToLongGun(bool pressed, bool long_gun, PressRoute& st);

	// One Zone control as the input poll stepped it.
	struct ZoneView
	{
		bool armed = false; // its control is armed this poll (its when guards hold, the session live)
		SpatialControls::ZoneParams params; // its offset from the head, radius, exit margin and hand
		std::array<bool, 2> inside{};       // StepZone's verdict this poll: the hand is in the zone's reach
	};

	// CPU thread, every input poll (VRInputSource::PollEvents), for each Zone control with a role.
	void PublishZone(Zone zone, const ZoneView& view);

	// A published zone older than this is gone (the poll stopped, or the profile no longer has it).
	static constexpr u64 kStaleMs = 250;

	// The last PublishZone for zone, if it came within kStaleMs and the zone is armed; false otherwise. Any thread.
	bool GetZone(Zone zone, ZoneView* out);

	// Where a zone's middle is now, from the snapshot's head exactly as StepZone places it: HeadAnchor (whose yaw_state
	// keeps the head's yaw while it looks straight up or down, as each Zone control's own state does), then PlaceControl
	// with the zone's offset and no yaw of its own. *yaw (may be null) gets the head yaw it turned with. False without a
	// usable head pose. Pure.
	bool ZoneCentre(const VRInputSnapshot& snap, const SpatialControls::ZoneParams& p, SpatialControls::ZoneState& yaw_state,
		SpatialControls::Vec3* centre, float* yaw);

	// The belt zone's bag is this tall, at the zone's middle.
	static constexpr float kBagHeightM = 0.06f;

	// A marker stays out of the middle of the view while the head looks ahead: not drawn within kClearDeg of the line of
	// sight unless the head is tipped down more than kLookDownDeg, looking at the body where the markers are meant to be
	// seen. Seated and looking ahead, the hips and the pouches are far below kClearDeg, so this only keeps a profile's odd
	// zone (or a recenter gone wrong) from putting one over the game.
	static constexpr float kClearDeg = 20.0f;
	static constexpr float kLookDownDeg = 25.0f;

	// Whether a marker at point may be drawn for the head at eye with orientation head_quat (xyzw): see kClearDeg. False
	// for anything not finite. Pure.
	bool ClearOfView(const float eye[3], const float head_quat[4], const float point[3]);

	// Which of the wanted layers go in the free_layers left: the holster first, then the belt's bag, then the pouches'
	// rings, then hand targeting's box (so the box goes first when layers are short, then the markers, then the holster).
	// Pure.
	std::array<bool, kSlots> Plan(u32 free_layers, const std::array<bool, kSlots>& want);

	enum class Shape : u8
	{
		Ring, // a pouch: a ring round its point, half its ball's outline as seen from the eye
		Bag,  // the belt zone: a small pouch with a flap, buckle and loop, upright
		Box,  // hand targeting: one corner of a wireframe box; eight marks make the box (see below)
	};

	struct Mark
	{
		float centre[3] = {0.0f, 0.0f, 0.0f}; // in the XR base space
		float radius = 0.0f; // metres: a ring's ball; the bag's half height; a Box corner's size (just above 0)
		Shape shape = Shape::Ring;
		bool lit = false; // a hand is in reach: drawn brighter
		// Box only: 0 dim (the hand is far), 1 brighter (near), 2 brightest (inside). A Box mark set is exactly eight
		// corners, the floor quad's four in order and then the same four at the top, and is drawn as its twelve edges.
		u8 glow = 0;
	};
	static constexpr int kMaxMarks = 8;
	// A Box corner's radius: only what keeps PlaceMarks' quad clear of the lines at the box's corners.
	static constexpr float kBoxCornerM = 0.004f;

	// A marker image's quad: facing the eye on the plane through the marks' middle (so in stereo they sit at about their
	// depth), "up" as near world up as it goes, square and just big enough for every mark as seen from the eye.
	struct Quad
	{
		float centre[3] = {0.0f, 0.0f, 0.0f};
		float right[3] = {1.0f, 0.0f, 0.0f};
		float up[3] = {0.0f, 1.0f, 0.0f};
		float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // xyzw, the quad's orientation (+Z toward the eye)
		float size_m = 0.0f;
	};

	// False with no marks, a mark nearer the eye than 5 cm along the view or not finite. Pure.
	bool PlaceMarks(const Mark* marks, int count, const float eye[3], Quad* out);

	// Draws the marks as seen from eye onto the quad into a kImageSize square image: dim and thin, brighter and with a
	// faint fill while lit. Pure (its own output only).
	void RasterMarks(const Mark* marks, int count, const float eye[3], const Quad& quad, std::vector<u32>& out_rgba);

	// What an image was drawn for. It rides a frame: a zone's middle turned with the head's yaw (the holster's own pose
	// for the weapon), the pouches' middle not turned. key says what it shows (the weapon, which marks, which are lit);
	// local holds the marks in that frame, so a frame that only moved keeps the image.
	struct Drawn
	{
		bool valid = false;
		float frame_pos[3] = {0.0f, 0.0f, 0.0f};
		float frame_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		float eye[3] = {0.0f, 0.0f, 0.0f};
		Quad quad;
		u64 key = 0;
		int count = 0;
		float local[kMaxMarks][3] = {};
	};

	// The lit bits of a key live here; a kept image whose key differs only in them may still be shown until the new one
	// is drawn (a frame late is fine), one showing something else may not.
	static constexpr u64 kKeyLitMask = 0xFFFFull << 32;

	// A mark that moved this far in its frame since the image was drawn is drawn again.
	static constexpr float kMarkMoveM = 0.004f;

	// d with marks (count of them) in the frame (frame_pos, frame_quat), seen from eye, showing key. Pure.
	Drawn Describe(const float frame_pos[3], const float frame_quat[4], const float eye[3], const Quad& quad, u64 key,
		const Mark* marks, int count);

	// Whether an image drawn as d has to be drawn again to show key with these marks in the frame now, seen from eye:
	// nothing drawn yet, key or the number of marks changed, a mark moved kMarkMoveM in the frame, or the view of the
	// image drifted past HandModel's kRerasterDriftDeg or kRerasterDistanceRatio (or cannot be measured). Pure.
	bool NeedsRedraw(const Drawn& d, const float frame_pos[3], const float frame_quat[4], const float eye[3], u64 key,
		const Mark* marks, int count);

	// Where a kept image goes now: its quad moved rigidly with its frame from where it was drawn
	// (HandModel::RePoseRigid). Pure.
	void PlaceKept(const Drawn& d, const float frame_pos[3], const float frame_quat[4], float out_centre[3],
		float out_quat[4]);

	// The placement against StepZone (a hand at the marker's middle is in the zone, one just past its radius is not, and
	// looking down moves neither), the holster's pose and image (grip up toward the hand, muzzle down and back, turning
	// with the head, brighter while lit), the budget, the view rule, the rings and the bag, and the redraw rules. Logs
	// "(VR) Zone markers self-test: N checks passed", or each failure. It draws the holster with HandModel's scratch
	// buffers, so the compositor's thread (PCSX2_VR_HANDS_SELFTEST, at VR start). True when every check passed.
	bool SelfTest();
}
