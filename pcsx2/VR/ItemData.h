// SPDX-FileCopyrightText: 2026 PenguinScreen2 contributors
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Types.h"

// Resident Evil Outbreak item ids (both Files share them) to a short English name and a kind, for the VR hands, the
// belt pouches and the wrist card. The ids and names come from Outbreak-Research/Documentation
// (docs/game data/items.md, MIT License, Copyright (c) 2022 Outbreak-Research); the kinds are ours, from the same
// tables (weapon ids 0-32, melee 100-119, ammo 200-263, recovery 300-318, character items 400-456, key items 10000+).
namespace VR::ItemData
{
	enum class Kind : u8
	{
		Unknown,     // not in the table
		Handgun,     // one-handed gun: handguns, magnums, revolvers, Kevin's 45 Auto, Mark's handgun
		LongGun,     // two-handed gun: submachine gun, shotguns, rifles, capsule shooters, launchers
		Melee,       // knives, pipes, poles, axe, hammer, crutch, wrench
		Thrown,      // throwable stick, molotov, concrete piece
		Spray,       // pesticide / flame spray
		OtherWeapon, // stun gun, nail gun, ampoule shooter
		Ammo,        // magazines, loaders, rounds
		Recovery,    // herbs, sprays, medicine
		Personal,    // character items that are not weapons (coin, medical set, tool box, picks, ...)
		Key,         // scenario key items and papers
		Other,
	};

	struct Entry
	{
		u16 id;
		Kind kind;
		const char* name; // at most 24 characters, ASCII (may contain an apostrophe or '&')
	};

	// nullptr for an id not in the table.
	const Entry* Find(u16 id);
	// "" for an id not in the table.
	const char* Name(u16 id);
	Kind KindOf(u16 id);
	// Handgun, LongGun, Melee, Thrown, Spray or OtherWeapon.
	bool IsWeapon(Kind kind);
	// A gun, the kind the laser sight belongs to (Handgun, LongGun, OtherWeapon).
	bool IsGun(Kind kind);
	const char* KindName(Kind kind);
	// The entries, sorted by id ascending (self-tests).
	u32 Count();
	const Entry& At(u32 index);
} // namespace VR::ItemData
