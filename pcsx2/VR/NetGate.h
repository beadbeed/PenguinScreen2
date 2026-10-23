// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

// Where the VR layer's online code paths run: one place every "is the network adapter on" question goes through, instead
// of each module reading EmuConfig.DEV9.EthEnable itself.
//
// The test override (PCSX2_VR_TEST_NET=privatecoop, read once at the first call, logged loudly): with the network adapter
// OFF, the VR layer takes its online code paths as if the adapter were on and the login host were our private server (so
// with [VR] PrivateCoopFirstPerson on the network mode resolves to private co-op, and without it to restricted, exactly as
// online). It only selects code paths, so they can be checked offline with savestates and the scripted tools:
// - it never turns the network adapter on, and never sends anything anywhere (VR presence keeps to a 127/8 peer under it);
// - with the adapter really on it does nothing at all: the real online rules decide, unchanged;
// - the emulator's online locks (no pause, speed change, state load or save: VMManager::IsOnlineLocked) are not touched by
//   it; PCSX2_TEST_ONLINE_LOCKS=1 forces those on for testing, as before;
// - PINE's test commands and the headset-free tools still see the adapter off, as it really is.
namespace VR::NetGate
{
	enum class TestNet : u8
	{
		None,        // no override (unset, empty, or any other value: an unknown value is refused and logged)
		PrivateCoop, // "privatecoop": the private co-op code paths
	};

	// Pure: what an environment value asks for (nullptr = unset).
	TestNet ParseTestNet(const char* env);

	// Pure: whether the online code paths run, for the real adapter state and the override.
	bool OnlinePaths(bool adapter_on, TestNet test);
	// Pure: whether the override is in effect (set, and the adapter really off).
	bool OverrideActive(bool adapter_on, TestNet test);

	// The override this process was started with (PCSX2_VR_TEST_NET, read once; logs a warning the first time it is set).
	TestNet Test();

	// True when the VR layer should take its online code paths: the network adapter on, or the test override with it off.
	bool IsOnline();
	// True while the test override is in effect (set, adapter really off): the private-server check is taken as passed.
	bool TestOverrideActive();
	// Neither IsOnline() nor [VR] OnlineSafe: "strictly offline".
	bool StrictlyOffline();

	// Pure checks of the rules above; logs "(VR) NetGate self-test: N passed, M failed".
	bool SelfTest();
} // namespace VR::NetGate
