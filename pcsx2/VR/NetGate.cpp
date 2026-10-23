// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/NetGate.h"

#include "Config.h"

#include "common/Console.h"

#include <cstdlib>
#include <cstring>

namespace VR::NetGate
{
	TestNet ParseTestNet(const char* env)
	{
		if (!env || env[0] == '\0')
			return TestNet::None;
		if (std::strcmp(env, "privatecoop") == 0)
			return TestNet::PrivateCoop;
		return TestNet::None;
	}

	bool OverrideActive(bool adapter_on, TestNet test)
	{
		return !adapter_on && test == TestNet::PrivateCoop;
	}

	bool OnlinePaths(bool adapter_on, TestNet test)
	{
		return adapter_on || OverrideActive(adapter_on, test);
	}

	TestNet Test()
	{
		static const TestNet s_test = []() {
			const char* env = std::getenv("PCSX2_VR_TEST_NET");
			const TestNet t = ParseTestNet(env);
			if (t == TestNet::PrivateCoop)
			{
				Console.Warning("(VR) NetGate: PCSX2_VR_TEST_NET=privatecoop: TEST OVERRIDE. With the network adapter off, the "
				                "VR layer takes its PRIVATE CO-OP code paths (as if online on the private server). The adapter "
				                "stays off, nothing is sent, the emulator's online locks are unchanged; with the adapter really "
				                "on this does nothing. For headset-free testing only.");
			}
			else if (env && env[0] != '\0')
			{
				Console.Warning("(VR) NetGate: PCSX2_VR_TEST_NET=%s is not a known value (only \"privatecoop\"): ignored.", env);
			}
			return t;
		}();
		return s_test;
	}

	bool IsOnline()
	{
		return OnlinePaths(EmuConfig.DEV9.EthEnable, Test());
	}

	bool TestOverrideActive()
	{
		return OverrideActive(EmuConfig.DEV9.EthEnable, Test());
	}

	bool StrictlyOffline()
	{
		return !IsOnline() && !EmuConfig.VR.OnlineSafe;
	}

	bool SelfTest()
	{
		int pass = 0, fail = 0;
		const auto check = [&](bool ok) { ok ? pass++ : fail++; };
		// Parsing: only the exact word selects anything.
		check(ParseTestNet(nullptr) == TestNet::None);
		check(ParseTestNet("") == TestNet::None);
		check(ParseTestNet("privatecoop") == TestNet::PrivateCoop);
		check(ParseTestNet("PrivateCoop") == TestNet::None);
		check(ParseTestNet("obsrv") == TestNet::None);
		check(ParseTestNet("online") == TestNet::None);
		check(ParseTestNet("1") == TestNet::None);
		check(ParseTestNet("privatecoop ") == TestNet::None);
		// Offline with no override: offline paths.
		check(!OnlinePaths(false, TestNet::None));
		check(!OverrideActive(false, TestNet::None));
		// Offline with the override: online paths, the override in effect.
		check(OnlinePaths(false, TestNet::PrivateCoop));
		check(OverrideActive(false, TestNet::PrivateCoop));
		// Really online: online paths whatever the override says, and the override never in effect (it cannot relax or
		// stand in for the real private-server check).
		check(OnlinePaths(true, TestNet::None));
		check(OnlinePaths(true, TestNet::PrivateCoop));
		check(!OverrideActive(true, TestNet::PrivateCoop));
		check(!OverrideActive(true, TestNet::None));
		Console.WriteLn("(VR) NetGate self-test: %d passed, %d failed", pass, fail);
		return fail == 0;
	}
} // namespace VR::NetGate
