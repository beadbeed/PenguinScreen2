// SPDX-FileCopyrightText: 2026 PenguinScreen2 contributors
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include "VR/CameraDriver.h"
#include "VR/ZoneMarkers.h"

#include <string>
#include <string_view>
#include <vector>

// VR presence ([VR] VrPresence, off by default; tools/outbreak-vr/research/2026-10-21-vr-presence.md): in private co-op,
// two PenguinScreen2 consoles send each other their players' VR head and hand poses over a small UDP side channel of
// their own (never the game's connection), and each draws the partner's VR head and hands beside the partner's game
// character. The game's own netcode, memory and traffic are untouched: everything here only reads game memory.
//
// - The channel: one fixed 156-byte packet about 30 times a second each way (under 5 KB/s): the sender's slot, the room
//   it is in, its character's position, and the head and both hands as a position and two directions (forward, up) in
//   the GAME's coordinates (centimetres; CameraDriver::GameFromVr), so the receiver places them in its own world with its
//   own mapping (VrFromGame). A shared word's hash (VrPresenceKey), a random session id per sender start and a sequence
//   number drop foreign, corrupt, old and reordered packets. The socket lives on its own thread; the CPU thread (Poll)
//   and the compositor's GS thread only swap small structs under a mutex.
// - The peer is VrPresencePeer, host:port, and must be a private IPv4 address (a LAN 10/8, 172.16/12, 192.168/16,
//   Tailscale 100.64/10, or this PC 127/8; a name is resolved and its address checked the same way). Packets from any
//   other address are dropped. The public internet (obsrv included) is never sent to.
// - Nothing is sent, and the socket is not even open, unless first person is armed with its record resolved and the
//   network mode is private co-op, or, for the offline loopback test, PCSX2_VR_PRESENCE_LOOPBACK=1 with the game offline
//   and the peer on 127/8.
// - The partner whose slot the packets carry, in the same room as this player (the game's own room numbers), is placed
//   on this console's copy of that character: the poses are moved by where it stands, so they sit on it even when the
//   game's copy lags. Nothing is drawn over it: VR body ([VR] VrBody, VR/VrBody.h) poses that character's own skeleton
//   from these poses (GetPartnerGame). The first version's placeholder (a head shell and teal hands in an impostor) is
//   gone; GetPartnerView is always false, so the compositor's partner slot stays empty.
namespace VR::Presence
{
	static constexpr u32 kMagic = 0x31505256u; // "VRP1"
	static constexpr u8 kVersion = 1;
	static constexpr size_t kPacketSize = 156;
	static constexpr u32 kSendIntervalMs = 33;
	// A partner whose newest packet is older than this is not drawn.
	static constexpr u64 kStaleMs = 250;
	// Packets of a new sender session (a restarted console) are taken only after the old one has been quiet this long.
	static constexpr u64 kSessionSwitchMs = 1000;
	static constexpr int kDefaultPort = 47800;
	// A part further than this from its own character (game units, the sender's) is dropped as nonsense.
	static constexpr float kMaxReachCm = 300.0f;

	// A head or a hand in the game's coordinates: a position (game units) and two unit directions, forward (OpenXR -Z of
	// the pose) and up (+Y).
	struct GamePose
	{
		float pos[3] = {0.0f, 0.0f, 0.0f};
		float fwd[3] = {0.0f, 0.0f, 1.0f};
		float up[3] = {0.0f, 1.0f, 0.0f};
	};

	static constexpr u8 kFlagHead = 1;
	static constexpr u8 kFlagLeft = 2;
	static constexpr u8 kFlagRight = 4;

	struct Packet
	{
		u8 slot = 0;  // the sender's local slot (its character's record id)
		u8 flags = 0; // kFlagHead | kFlagLeft | kFlagRight: which parts are valid
		u16 room = 0; // the room the sender's character is in (the game's u16)
		u32 key = 0;  // KeyHash(VrPresenceKey)
		u32 session = 0;
		u32 seq = 0;
		u32 send_ms = 0; // the sender's clock (for the log only)
		float character[3] = {0.0f, 0.0f, 0.0f}; // the sender's character position, game units
		GamePose head;
		GamePose hand[2]; // HandModel::kLeft / kRight
		u8 grip[2] = {0, 0};    // 0-255
		u8 trigger[2] = {0, 0}; // 0-255
	};

	// Little-endian, fixed size, an FNV-1a checksum over the rest. Encode: false for a buffer under kPacketSize. Decode:
	// false for a wrong size, magic, version or checksum, a slot past 3, an unknown flag, anything not finite, a position
	// past 1e6 or a valid part whose directions are not unit and roughly perpendicular. Pure.
	bool Encode(const Packet& p, u8* out, size_t size);
	bool Decode(const u8* data, size_t size, Packet* out);

	// FNV-1a of the text. Pure.
	u32 KeyHash(std::string_view text);
	// The key is a word (not empty or blanks only: that hash is a fixed public value; night review m5). Pure.
	bool KeyUsable(std::string_view text);

	// host order (a.b.c.d is a << 24 | ...). 10/8, 172.16/12, 192.168/16, 100.64/10 and 127/8 only. Pure.
	bool IsPrivateIPv4(u32 addr);
	bool IsLoopbackIPv4(u32 addr);
	// "a.b.c.d" (four decimal parts 0-255, nothing else) into host order. Pure.
	bool ParseIPv4(std::string_view text, u32* addr);
	// "host:port": a host of letters, digits, '.', '-' and '_' (an IPv4 address or a name) and a port 1-65535. Pure.
	bool ParsePeer(std::string_view text, std::string* host, u16* port);

	// The stale and reorder filter: the first packet, a later sequence number of the same session (wrap-aware), or a new
	// session once the old one has been quiet kSessionSwitchMs. Updates st when accepted. Pure.
	struct SeqState
	{
		bool have = false;
		u32 session = 0;
		u32 seq = 0;
		u64 last_ms = 0;
	};
	bool Accept(SeqState& st, u32 session, u32 seq, u64 now_ms);

	// A pose in the headset's space (OpenXR: metres, -Z forward, quat xyzw) into the game's, and back moved by shift (game
	// units) first. Both through the mapping's own points, so a mirroring map (yaw_sign -1) needs nothing special. False
	// for an unusable mapping or input. Pure.
	bool PoseToGame(const CameraDriver::Mapping& m, const float pos[3], const float quat[4], GamePose* out);
	bool PoseFromGame(const CameraDriver::Mapping& m, const GamePose& in, const float shift[3], float pos[3], float quat[4]);

	// The partner in this headset's space (the log only now; the compositor no longer gets one).
	struct PartnerView
	{
		bool valid = false;
		u8 slot = 0;
		u8 flags = 0; // which parts are valid (kFlag*)
		float head_pos[3] = {};
		float head_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		float hand_pos[2][3] = {};
		float hand_quat[2][4] = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
		float grip[2] = {0.0f, 0.0f};
		u32 seq = 0;
		u64 stamp_ms = 0; // CameraDriver::SteadyNowMs when Poll published it
	};

	// Always false now (the placeholder drawing is gone; kept so the compositor's old partner slot links and stays empty).
	bool GetPartnerView(PartnerView* out);

	// The partner as VR body poses it: its record, and its VR head and hands in the GAME's coordinates moved onto this
	// console's copy of its character (record position minus the packet's character position added), within kStaleMs.
	struct PartnerGame
	{
		bool valid = false;
		u32 record = 0; // the partner's character record (camera.base's, the packet's slot, used, this room)
		u8 slot = 0;
		u8 flags = 0; // kFlag*
		GamePose head;
		GamePose hand[2];
		float grip[2] = {0.0f, 0.0f};
		float trigger[2] = {0.0f, 0.0f};
		u32 seq = 0;
		u64 stamp_ms = 0;
	};
	// Any thread.
	bool GetPartnerGame(PartnerGame* out);

	// The setting is on and names a peer (the compositor makes the partner's swapchain only then). Any thread.
	bool Wanted();

	// CPU thread, every vsync after CameraDriver::Apply: decides whether the channel is live, hands the local poses to the
	// socket thread and places the newest partner packet. Reads game memory only.
	void Poll();

	// Stops the socket thread (it is started again by Poll when wanted).
	void Shutdown();

	// Inert (false, an empty image, 0): the placeholder drawing is gone; kept only so the compositor's old slot links.
	bool PlacePartner(const PartnerView& v, const float eye[3], ZoneMarkers::Quad* out);
	void RasterPartner(const PartnerView& v, const float eye[3], const ZoneMarkers::Quad& quad, std::vector<u32>& out_rgba);
	int PartnerMarks(const PartnerView& v, ZoneMarkers::Mark* marks);

	// Encode and decode, the private-address filter and the peer parser, the stale and reorder filter, the pose mapping
	// round trip (with a mirroring map and a shift). Logs "(VR) Presence self-test: N checks
	// passed" or each failure. True when every check passed.
	bool SelfTest();
}
