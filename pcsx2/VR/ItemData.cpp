// SPDX-FileCopyrightText: 2026 PenguinScreen2 contributors
// SPDX-License-Identifier: GPL-3.0

#include "VR/ItemData.h"

#include <algorithm>
#include <iterator>

// Generated from Outbreak-Research/Documentation, docs/game data/items.md (MIT License, Copyright (c) 2022
// Outbreak-Research), item id table: names shortened to 24 characters, scenario notes dropped.

// The item table below is derived from Outbreak-Research/Documentation under this license:
//
// MIT License
//
// Copyright (c) 2022 Outbreak-Research
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
namespace VR::ItemData
{
	namespace
	{
		constexpr Entry kItems[] = {
			{0, Kind::Handgun, "Handgun"},
			{1, Kind::Handgun, "Handgun SG"},
			{2, Kind::Handgun, "Handgun GL"},
			{3, Kind::Handgun, "Magnum Handgun"},
			{4, Kind::Handgun, "Magnum Revolver"},
			{5, Kind::Handgun, "Handgun HP"},
			{6, Kind::Handgun, "S&W model 500"},
			{7, Kind::Handgun, "Revolver"},
			{8, Kind::Handgun, "Burst Handgun"},
			{10, Kind::LongGun, "Submachine Gun"},
			{11, Kind::LongGun, "Shotgun"},
			{12, Kind::LongGun, "Shotgun E"},
			{13, Kind::LongGun, "Hunting Rifle"},
			{14, Kind::LongGun, "Assault Rifle"},
			{15, Kind::LongGun, "Capsule Gun(BLUE)"},
			{16, Kind::LongGun, "Capsule Gun(RED)"},
			{17, Kind::LongGun, "Capsule Gun(GREEN)"},
			{18, Kind::LongGun, "Capsule Gun(WHITE)"},
			{19, Kind::Other, "Mine Detector"},
			{20, Kind::LongGun, "Rocket Launcher"},
			{23, Kind::LongGun, "Grenade L. (BURST)"},
			{24, Kind::LongGun, "Grenade L. (FLAME)"},
			{25, Kind::LongGun, "Grenade L. (ACID)"},
			{26, Kind::LongGun, "Grenade L. (Extinguish)"},
			{27, Kind::LongGun, "Grenade L. (BOW GAS)"},
			{28, Kind::Spray, "Pesticide Spray"},
			{29, Kind::Spray, "Flame Spray"},
			{30, Kind::OtherWeapon, "Stun Gun"},
			{31, Kind::OtherWeapon, "Nail Gun"},
			{32, Kind::OtherWeapon, "Ampoule Shooter"},
			{100, Kind::Melee, "Survival Knife"},
			{101, Kind::Melee, "Folding Knife"},
			{102, Kind::Melee, "Butcher Knife"},
			{103, Kind::Melee, "Iron Pipe"},
			{104, Kind::Melee, "Curved Iron Pipe"},
			{105, Kind::Melee, "Bent Iron Pipe"},
			{106, Kind::Melee, "Long Pole"},
			{107, Kind::Melee, "Square Timber"},
			{108, Kind::Other, "Bomb Switch"},
			{109, Kind::Melee, "Axe"},
			{110, Kind::Melee, "Scrub Brush"},
			{111, Kind::Melee, "Wooden Pole"},
			{112, Kind::Thrown, "Throwable Stick"},
			{113, Kind::Melee, "Spear"},
			{114, Kind::Thrown, "Molotov Cocktail"},
			{115, Kind::Melee, "Hammer"},
			{116, Kind::Melee, "Crutch"},
			{117, Kind::Melee, "Stun Rod"},
			{118, Kind::Thrown, "Concrete Piece"},
			{119, Kind::Melee, "Broken Crutch"},
			{155, Kind::Other, "Empty Bottle"},
			{156, Kind::Other, "Red Chemical Bottle"},
			{157, Kind::Other, "Green Chemical Bottle"},
			{158, Kind::Other, "Yellow Chemical Bottle"},
			{159, Kind::Other, "Gray Chemical Bottle"},
			{160, Kind::Other, "Time Bomb(before ignitio"},
			{161, Kind::Other, "Time Bomb(after ignition"},
			{200, Kind::Ammo, "Handgun Magazine"},
			{201, Kind::Ammo, "Handgun SG Magazine"},
			{202, Kind::Ammo, "Handgun GL Magazine"},
			{203, Kind::Ammo, "Magnum Clip"},
			{204, Kind::Ammo, "Magnum Revolver S.Loader"},
			{205, Kind::Ammo, "Revolver S.Loader"},
			{206, Kind::Ammo, "Sub Machine Gun Clip"},
			{207, Kind::Ammo, "Assault Rifle Clip"},
			{209, Kind::Ammo, "Burst Hg Magazine"},
			{210, Kind::Ammo, "45 Auto Magazine"},
			{211, Kind::Ammo, "Handgun HP Magazine"},
			{250, Kind::Ammo, "Hand Gun Rounds 9mm"},
			{251, Kind::Ammo, "Magnum Hg rounds"},
			{252, Kind::Ammo, "Magnum Revolver Rounds"},
			{253, Kind::Ammo, "Revolver Rounds"},
			{254, Kind::Ammo, "Shotgun Rounds"},
			{255, Kind::Ammo, "Rifle Rounds"},
			{256, Kind::Ammo, "Rocket Rounds"},
			{257, Kind::Ammo, "Burst Rounds"},
			{258, Kind::Ammo, "Flame Rounds"},
			{259, Kind::Ammo, "Acid Rounds"},
			{260, Kind::Ammo, "Fire Extinguisher Rounds"},
			{261, Kind::Ammo, "BOW Gas Rounds"},
			{262, Kind::Ammo, "45 Auto Rounds"},
			{263, Kind::Ammo, "High Power Revolver Roun"},
			{300, Kind::Recovery, "Green Herb"},
			{301, Kind::Recovery, "Blue Herb"},
			{302, Kind::Recovery, "Red Herb"},
			{303, Kind::Recovery, "Mixed Herb (G+G)"},
			{304, Kind::Recovery, "Mixed Herb (G+G+G)"},
			{305, Kind::Recovery, "Mixed Herb (G+R)"},
			{306, Kind::Recovery, "Mixed Herb (G+B)"},
			{307, Kind::Recovery, "Mixed Herb (G+G+B)"},
			{308, Kind::Recovery, "Mixed Herb (G+R+B)"},
			{309, Kind::Recovery, "First Aid Spray"},
			{310, Kind::Recovery, "Recovery Medicine"},
			{311, Kind::Recovery, "Hemostat"},
			{312, Kind::Recovery, "Daylight"},
			{313, Kind::Recovery, "Mixed Herb(R+B)"},
			{314, Kind::Recovery, "Antidote"},
			{315, Kind::Recovery, "Recovery Medicine (L)"},
			{316, Kind::Recovery, "Anti Virus"},
			{317, Kind::Recovery, "Anti Virus (L)"},
			{318, Kind::Recovery, "Recovery Medicine Base"},
			{400, Kind::Handgun, "Kevin's 45 Auto"},
			{401, Kind::Handgun, "Mark's Handgun"},
			{402, Kind::Personal, "Coin holder(Jim's)"},
			{403, Kind::Personal, "Jim's Coin"},
			{404, Kind::Personal, "Lock Pick"},
			{405, Kind::Personal, "Medical Set"},
			{406, Kind::Personal, "Tool Box"},
			{407, Kind::Melee, "Folding Knife(David's)"},
			{408, Kind::Melee, "Monkey Wrench"},
			{409, Kind::Personal, "Vinyl Tape"},
			{410, Kind::Personal, "Junk Parts"},
			{411, Kind::Personal, "Picking Tool"},
			{412, Kind::Personal, "Knapsack"},
			{413, Kind::Personal, "Herb Case"},
			{414, Kind::Personal, "I-Shaped Pick"},
			{415, Kind::Personal, "S-Shaped Pick"},
			{416, Kind::Personal, "W-Shaped Pick"},
			{417, Kind::Personal, "P-Shaped Pick"},
			{418, Kind::Personal, "Bandage"},
			{419, Kind::Personal, "Lucky Coin"},
			{420, Kind::Personal, "Charm"},
			{450, Kind::Personal, "Lighter"},
			{451, Kind::Personal, "Alcohol Bottle"},
			{452, Kind::Personal, "Bottle + Newspaper"},
			{453, Kind::Personal, "Broken Handgun"},
			{454, Kind::Personal, "Broken Shotgun"},
			{455, Kind::Personal, "Battery"},
			{456, Kind::Personal, "Broken Handgun SG"},
			{10100, Kind::Key, "Staff Room Key"},
			{10101, Kind::Key, "Key with a Red Tag"},
			{10102, Kind::Key, "Key with a Blue Tag"},
			{10103, Kind::Key, "Forklift Key"},
			{10105, Kind::Key, "Storage Key"},
			{10106, Kind::Key, "Detonator Handle"},
			{10107, Kind::Key, "Detonator Main Unit"},
			{10108, Kind::Key, "Detonator"},
			{10150, Kind::Key, "Newspaper"},
			{10206, Kind::Key, "Charlie's ID tag"},
			{10207, Kind::Key, "Len's ID tag"},
			{10208, Kind::Key, "Gold Key"},
			{10209, Kind::Key, "Silver Key"},
			{10210, Kind::Key, "Security Room Card Key"},
			{10211, Kind::Key, "Red Jewel"},
			{10600, Kind::Key, "Examination Room Key"},
			{10601, Kind::Key, "ID Card Lv1"},
			{10602, Kind::Key, "ID Card Lv2"},
			{10603, Kind::Key, "MO Disk"},
			{10604, Kind::Key, "MO Disk(Code A)"},
			{10605, Kind::Key, "MO Disk(Code G)"},
			{10609, Kind::Key, "Valve Handle (6 Sided)"},
			{10610, Kind::Key, "Valve Handle (4 Sided)"},
			{10611, Kind::Key, "Crowbar"},
			{10612, Kind::Key, "Model Grenade L."},
			{10650, Kind::Key, "Newspaper 1"},
			{10651, Kind::Key, "Newspaper 2"},
			{10652, Kind::Key, "Newspaper 3"},
			{11000, Kind::Key, "Employee Area Key"},
			{11001, Kind::Key, "B2F Key"},
			{11002, Kind::Key, "Ventilation Tower Key"},
			{11003, Kind::Key, "Valve Handle"},
			{11004, Kind::Key, "Repair Tape"},
			{11005, Kind::Key, "Rubber Sheet"},
			{11006, Kind::Key, "Founder's Emblem(Werner/"},
			{11007, Kind::Key, "Founder's Emblem(Oral/Bl"},
			{11008, Kind::Key, "Model Train Wheel"},
			{11009, Kind::Key, "Blood Pack(already used)"},
			{11010, Kind::Key, "Blood Pack"},
			{11050, Kind::Key, "Newspaper 1"},
			{11051, Kind::Key, "Newspaper 2"},
			{11052, Kind::Key, "Newspaper 3"},
			{11500, Kind::Key, "Joker Key"},
			{11501, Kind::Key, "Onyx Plate"},
			{11502, Kind::Key, "Sapphire Plate"},
			{11503, Kind::Key, "Ruby Plate"},
			{11504, Kind::Key, "Emerald Plate"},
			{11505, Kind::Key, "Amethyst Plate"},
			{11506, Kind::Key, "Padlock Key"},
			{11507, Kind::Key, "Ace Key"},
			{11508, Kind::Key, "Gas Canister"},
			{11509, Kind::Key, "Plywood Board"},
			{11510, Kind::Key, "Film A"},
			{11511, Kind::Key, "Used Gas Canister"},
			{11512, Kind::Key, "Unicorn Medal"},
			{11513, Kind::Key, "Film B"},
			{11514, Kind::Key, "Film C"},
			{11515, Kind::Key, "Film D"},
			{11550, Kind::Key, "Secret File"},
			{12600, Kind::Key, "Auxiliary Building Key"},
			{12601, Kind::Key, "Administrator's Office K"},
			{12602, Kind::Key, "Rusty Key"},
			{12603, Kind::Key, "Syringe (empty)"},
			{12604, Kind::Key, "Syringe (solvent)"},
			{12605, Kind::Key, "Pendant"},
			{12800, Kind::Key, "Blood pack(already used)"},
			{12801, Kind::Key, "Blood pack"},
			{12805, Kind::Key, "Level 1 Card Key"},
			{12806, Kind::Key, "Level 2 Card Key"},
			{12807, Kind::Key, "Chain Key"},
			{12850, Kind::Key, "Male Nurse's Diary"},
			{12851, Kind::Key, "Researcher's Diary"},
			{12852, Kind::Key, "Research Request"},
			{13500, Kind::Key, "UMB No.3"},
			{13501, Kind::Key, "VP-017"},
			{13502, Kind::Key, "V-JOLT"},
			{13503, Kind::Key, "Wrench"},
			{13504, Kind::Key, "Frozen Wrench"},
			{13505, Kind::Key, "Lab Cardkey"},
			{13506, Kind::Key, "Turntable Key"},
			{13507, Kind::Key, "Valve Handle"},
			{13508, Kind::Key, "Hand Burner"},
			{13550, Kind::Key, "no data"},
			{14000, Kind::Key, "Bolt Cutters"},
			{14001, Kind::Key, "Elephant Key"},
			{14002, Kind::Key, "Alligator Key"},
			{14003, Kind::Key, "Lion Key"},
			{14004, Kind::Key, "Office Key"},
			{14005, Kind::Key, "Mr. Racoon Medal"},
			{14006, Kind::Key, "Lion Emblem (RED)"},
			{14007, Kind::Key, "Lion Emblem (BLUE)"},
			{14008, Kind::Key, "Blank Tape"},
			{14009, Kind::Key, "Parade BGM Tape"},
			{14100, Kind::Key, "Brass Spectacles"},
			{14101, Kind::Key, "Card Key"},
			{14102, Kind::Key, "V-Poison"},
			{14103, Kind::Key, "P-Base"},
			{14104, Kind::Key, "Reagent Case"},
			{14105, Kind::Key, "Sealed Reagent Case"},
			{14106, Kind::Key, "P-Base(sealed)"},
			{14107, Kind::Key, "T-Blood"},
			{14108, Kind::Key, "Red Jewel"},
			{14109, Kind::Key, "Blue Jewel"},
			{14110, Kind::Key, "P-Base(depleted)"},
			{14111, Kind::Key, "Key with a Red Tag"},
		};
	} // namespace

	const Entry* Find(u16 id)
	{
		const Entry* end = kItems + std::size(kItems);
		const Entry* it = std::lower_bound(kItems, end, id, [](const Entry& e, u16 v) { return e.id < v; });
		return (it != end && it->id == id) ? it : nullptr;
	}

	const char* Name(u16 id)
	{
		const Entry* e = Find(id);
		return e ? e->name : "";
	}

	Kind KindOf(u16 id)
	{
		const Entry* e = Find(id);
		return e ? e->kind : Kind::Unknown;
	}

	bool IsWeapon(Kind kind)
	{
		return kind == Kind::Handgun || kind == Kind::LongGun || kind == Kind::Melee || kind == Kind::Thrown ||
		       kind == Kind::Spray || kind == Kind::OtherWeapon;
	}

	bool IsGun(Kind kind)
	{
		return kind == Kind::Handgun || kind == Kind::LongGun || kind == Kind::OtherWeapon;
	}

	const char* KindName(Kind kind)
	{
		switch (kind)
		{
			case Kind::Handgun: return "handgun";
			case Kind::LongGun: return "long gun";
			case Kind::Melee: return "melee";
			case Kind::Thrown: return "thrown";
			case Kind::Spray: return "spray";
			case Kind::OtherWeapon: return "other weapon";
			case Kind::Ammo: return "ammo";
			case Kind::Recovery: return "recovery";
			case Kind::Personal: return "personal";
			case Kind::Key: return "key item";
			case Kind::Other: return "other";
			default: return "unknown";
		}
	}

	u32 Count()
	{
		return static_cast<u32>(std::size(kItems));
	}

	const Entry& At(u32 index)
	{
		return kItems[std::min<u32>(index, Count() - 1)];
	}
} // namespace VR::ItemData
