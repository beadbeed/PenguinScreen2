// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/ItemData.h"

#include "common/Pcsx2Defs.h"

#include <string>
#include <vector>

// Resident Evil Outbreak's own weapon models, for the VR hands: read from the player's own disc image while the game
// runs and decoded in memory, never written anywhere. Every weapon a scenario can hand out is in that scenario's
// OBJSxx.NBD (NETBIO00.DAT -> rXXX.afs -> OBJSxx.NBD); each item is one root of the NBD's hierarchy, whose group id
// is the item id VR::ItemData uses. tools/outbreak-vr/research/2026-10-12-weapon-models-research.md has the layout,
// and tools/outbreak-vr/gamemodels_check.py decodes the same disc the same way to compare.
//
// Format knowledge from Fothsid's libthanatos (https://github.com/Fothsid/libthanatos) and OBTool
// (https://github.com/Fothsid/OBTool), both GPL-3.0; see GameModels.cpp.
namespace VR::GameModels
{
	// The strips index position, normal and UV with one index, so they come per vertex.
	struct Vertex
	{
		// Centimetres in the item's model space, Y up. Items lie as on the floor: a pistol on its side in the XZ plane,
		// barrel toward -X, top of the slide toward +Z, thickness along Y; its origin is near the trigger. Melee items
		// lie along X, centred on the origin.
		float pos[3];
		float normal[3]; // as stored (unit length on both discs)
		// Across the part's texture, v counting down from the top row. Mostly 0..1, but on File #2 the Shotgun E, Hunting
		// Rifle and Handgun HP run u up to 2: sample with wrap (the GS's repeat), never clamp.
		float uv[2];
	};

	// A decoded texture page: rows top to bottom, each texel ControlQuads::PackRgba's order (r | g << 8 | b << 16 |
	// a << 24), alpha on the 0-255 scale.
	struct Texture
	{
		u16 width = 0;  // powers of two, at most 1024
		u16 height = 0;
		std::vector<u32> rgba;
	};

	// One rigid piece of a weapon in one texture page. Parts with the same slot move together: a part whose triangles
	// use two pages comes as two parts with that slot.
	struct Part
	{
		// The skin weight slot: pistols 0 frame, 1 magazine, 2 slide; Magnum Revolver 0 frame, 1 hammer, 2 cylinder;
		// shotguns 0 body, 1 pump; most melee weapons have only slot 0.
		u8 slot = 0;
		// The game's joint id for the slot (the mesh's JointRefList), -1 without one. It indexes a table in the game,
		// not the hierarchy below, so only the slot order means anything here.
		s32 joint = -1;
		// The slot's node in the hierarchy (the item root's slot-th child), relative to the item root: where a moving
		// part turns or slides from (the revolver's hammer pin, the cylinder's crane). The vertices are already in the
		// item's model space (bind pose), so a part draws in place with no transform. False: no node for the slot,
		// and these stay identity.
		bool has_node = false;
		float pivot[3] = {0.0f, 0.0f, 0.0f};    // translation, cm
		float rotation[3] = {0.0f, 0.0f, 0.0f}; // Euler angles X, Y, Z in radians, as the node stores them
		float scale[3] = {1.0f, 1.0f, 1.0f};
		const Texture* texture = nullptr; // nullptr: the material has no texture (none on the discs checked)
		std::vector<Vertex> vertices;
		// A triangle list, three indices into vertices per triangle, strips unrolled. Each triangle is turned so that
		// (b - a) x (c - a) points the way its vertex normals do (the strips' alternating winding agrees with the normals
		// for all but a handful of slivers).
		std::vector<u16> indices;
	};

	struct Model
	{
		u16 item_id = 0;
		ItemData::Kind kind = ItemData::Kind::Unknown;
		char source[16] = {}; // the OBJS file it came from, e.g. "OBJS01.NBD"
		float bounds_min[3] = {0.0f, 0.0f, 0.0f}; // cm, over every part
		float bounds_max[3] = {0.0f, 0.0f, 0.0f};
		u32 triangles = 0;        // over every part
		u32 strip_triangles = 0;  // what the strips spell before degenerate ones are dropped
		u32 degenerate = 0;       // dropped: a repeated index or no area
		u32 flipped = 0;          // turned to agree with the vertex normals
		std::vector<Part> parts;
	};

	// Starts reading the running game's weapon models, once per disc: for SLPM-65428 / SLPM-65692 only, on a worker
	// thread of its own, from the disc image the VM booted through a reader of its own (never the emulated CDVD's).
	// The VR session calls it when it starts (a session that starts before a game boots waits for the VM to name its
	// disc); any thread, returns at once, and again for the same disc does nothing. Logs one summary line when done
	// ("(VR) Game models: ..."). Only the start reads the disc: a game booted later while the same session runs keeps
	// what was loaded for the first.
	void StartLoad();

	// The weapon's model, or nullptr until the load finished, for another game, or when the disc has no mesh for the
	// item (6 S&W model 500, 16-18 the red/green/white Capsule Guns and 26 the Extinguish grenade launcher are on
	// neither File). Any thread. A pointer stays valid until the emulator exits, even if another disc is loaded later.
	const Model* Find(u16 item_id);

	enum class State : u8
	{
		Idle,        // nothing started yet
		Loading,     // the worker is reading the disc
		Loaded,      // Find answers (the summary line says how many)
		NotOutbreak, // the running game is not Outbreak File #1 or #2, or not booted from an image
		Failed,      // the disc could not be read or holds none of the files
	};

	// The load's state and, once it ended, the summary line it logged (for logs; any thread).
	State LoadState(std::string* summary = nullptr);
	const char* StateName(State state);

	// PCSX2_VR_MODELS_SELFTEST: runs after the load, on its worker. Decodes a small NBD built in code (strip winding,
	// the CLUT order, 4- and 8-bit textures, pivots), then the same NBD cut short at every length and with every byte
	// flipped (nothing may crash or break an invariant), then checks the disc's library: the counts
	// tools/outbreak-vr/gamemodels_check.py prints for File #1 and #2, triangle indices in range, finite UVs,
	// power-of-two textures, and the Handgun / Kevin's 45 Auto / Shotgun triangle counts against the research
	// (259 / 299 / 250). Logs "(VR) Game models self-test: N checks passed", or each failure and how many failed.
	bool SelfTest();
} // namespace VR::GameModels
