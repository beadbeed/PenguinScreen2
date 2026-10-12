// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

namespace VR::ProfileDB
{
	struct Profile;
}

// Controller haptics for profiles with a feedback: block. Shots, hits and low health are read from game
// memory (reads only, so the same online) and felt in the VR controllers; the game's own DualShock rumble
// is forwarded too. Nothing here runs without a live VR session.
namespace VR::GameFeedback
{
	// CPU thread, every vsync right after CameraDriver::Apply (it reads the record Apply resolved).
	void Poll();

	// CPU thread, from InputManager::SetPadVibrationIntensity with the pad's motor levels (0-1). Acts only
	// on a change, only for the pad port of the profile's VR Gamepad and only while forwarding is on.
	void OnPadRumble(u32 pad, float large_motor, float small_motor);

	// From InputManager::PauseVibration: stops forwarded rumble; the next level the game sends restarts it.
	void PauseRumble();

	// CPU thread: a loaded state has its own HP and counters, so the next reads are a new baseline.
	void OnStateLoaded();

	// What the local character holds, from feedback.weapon: Unknown without that key, with no record (title
	// screen, cutscenes) or when the equipped item record fails its checks (CameraDriver then keeps showing
	// the pistol from the stance alone). Any thread; updated by Poll.
	enum class Hold : u8
	{
		Unknown,
		BareHands,
		Weapon,
	};
	Hold WeaponInHand();

	// The equipped weapon for the headset (recoil, slide, muzzle flash, the wrist card's rounds). valid only
	// while WeaponInHand() is Weapon. The times are on the CameraDriver::SteadyNowMs() clock (0: never);
	// shot_serial counts every shot cue. Any thread.
	struct WeaponView
	{
		bool valid = false;
		u8 equipped = 0;
		u16 item_id = 0;
		u16 ammo = 0;
		u32 shot_serial = 0;
		u64 shot_ms = 0;
		u64 dry_ms = 0;
		u64 reload_ms = 0;
	};
	WeaponView GetWeapon();

	// Feeds synthetic HP, shot-counter and weapon sequences through the cue rules and checks the queued pulses.
	// Runs once from Poll when PCSX2_VR_FEEDBACK_SELFTEST is set.
	bool SelfTest();

	// --- Item records: naming what a belt pouch (or anything else) holds. Reads only. ---

	// Reads guest memory: the same signature as BeltPouches::GuestReader. Null reads main RAM (nothing without a VM).
	using ItemMemReader = bool (*)(u32 address, u8 width, u32* value);

	// Where the local character's item records are: feedback.weapon's item table (its stride and the item id, count and
	// owner offsets in a record) and the local slot byte (camera.base's index address). valid only with feedback.weapon
	// and an indexed camera.base.
	struct ItemTable
	{
		bool valid = false;
		u32 table = 0;
		u32 stride = 0;
		u32 id_offset = 0;
		u32 count_offset = 0;
		u32 owner_offset = 0;
		u32 slot_address = 0;
	};
	ItemTable ItemTableOf(const ProfileDB::Profile& profile);

	// One record of the item table, checked the way the weapon reader checks the equipped one: present (+0x00 == 1),
	// naming its own index (+0x02) and owned by the local slot + 1. valid is false for index 0 (an empty cell), for a
	// record outside main RAM and for one that fails a check (another player's, a stale index).
	struct ItemRecord
	{
		bool valid = false;
		u32 index = 0;
		u16 item_id = 0;
		u16 count = 0; // rounds in a gun, how many of an ammo or recovery item
		// What the checks saw, for a log line when they fail.
		u32 present = 0;
		u32 own_index = 0;
		u32 owner = 0;
		bool slot_known = false;
		u32 slot = 0;
	};
	ItemRecord ReadItemRecord(const ItemTable& table, u32 index, ItemMemReader reader);

	// The item's name (ItemData) with its count where it has one: a gun's or a spray's rounds ("Kevin's 45 Auto 7"), the
	// count of ammo ("45 Auto Magazine x7") and of a recovery item holding more than one ("Green Herb x2"), the name
	// alone for anything else ("Green Herb", "Survival Knife"); "Item <id>" for an id ItemData does not know.
	std::string ItemLabel(u16 item_id, u16 count);

	// ItemData lookups, ItemLabel and ReadItemRecord's checks against a scripted item table; logs "(VR) Item names
	// self-test: passed (N checks)" or "... FAIL: <first check>". Run from the BeltPouches self-test (PCSX2_VR_SELFTEST).
	bool ItemSelfTest();
}
