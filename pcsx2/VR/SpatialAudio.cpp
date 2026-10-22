// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/SpatialAudio.h"

#include "Config.h"
#include "Memory.h"

#include "common/Console.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>

namespace VR::SpatialAudio
{
	namespace
	{
		constexpr u32 kBlock = 0xFF400u;
		constexpr u32 kMagic = 0x53504131u; // 'SPA1'
		// Which way view-space +x pans: +1 = right. Measured offline on File #1 (the game's own projection's x against
		// the view-space x for sounds in front, research note "Spatial audio").
		constexpr float kSign = 1.0f;

		bool IsFile1(u32 crc)
		{
			return crc == 0x32088394u || crc == 0x198EFDC1u || crc == 0x03C8F393u || crc == 0x32097C02u || crc == 0x03C90C05u;
		}

		bool s_written = false;
		u32 s_last_count = 0;
		u64 s_log_ms = 0;
		u32 s_agree = 0, s_disagree = 0;

		u64 NowMs()
		{
			return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}
		float F(u32 a) { return std::bit_cast<float>(static_cast<u32>(memRead32(a))); }
	} // namespace

	float PanX(float xv, float zv, float sign)
	{
		const float r = std::sqrt(xv * xv + zv * zv);
		const float s = (r < 1.0f) ? 0.0f : xv / r;
		return 322.5f + 320.0f * (s * sign);
	}

	int PanByte(float x)
	{
		const float c = std::clamp(x, 0.0f, 640.0f);
		return static_cast<int>(c / 5.0394f);
	}

	void Poll(u32 crc, bool live)
	{
		const bool want = live && EmuConfig.VR.SpatialAudio && IsFile1(crc) && !EmuConfig.DEV9.EthEnable && !EmuConfig.VR.OnlineSafe;
		if (!want)
		{
			// Cleared only while it reads ours (never a block some other build or game left there).
			if (s_written && memRead32(kBlock) == kMagic)
			{
				for (u32 i = 0; i < 0x28; i += 4)
					memWrite32(kBlock + i, 0);
			}
			s_written = false;
			return;
		}
		if (!s_written)
		{
			// The block's RAM must be free (zero) the first time, or hold our own magic (a state saved with it).
			const u32 m = memRead32(kBlock);
			if (m != 0 && m != kMagic)
			{
				static bool warned = false;
				if (!warned)
					Console.Warning("(VR) SpatialAudio: 0x%X is not free (0x%08X); the game's own pan stays.", kBlock, m);
				warned = true;
				return;
			}
			memWrite32(kBlock + 4, std::bit_cast<u32>(kSign));
			memWrite32(kBlock, kMagic);
			s_written = true;
			s_last_count = memRead32(kBlock + 0x1C);
			Console.WriteLn("(VR) SpatialAudio: on (sounds pan round the head; cave 0xFC000, block 0x%X).", kBlock);
		}
		const u32 count = memRead32(kBlock + 0x1C);
		const u64 now = NowMs();
		if (count != s_last_count)
		{
			// The game's own x against view-space x for a sound in front (w > 0, off centre): do they agree on the side?
			const float gx = F(kBlock + 0x10), gw = F(kBlock + 0x14), xv = F(kBlock + 0x20), zv = F(kBlock + 0x24);
			if (gw > 0.0f && zv < 0.0f && std::abs(gx - 320.0f) > 20.0f && std::abs(xv) > 10.0f)
				((gx > 320.0f) == (xv > 0.0f) ? s_agree : s_disagree)++;
		}
		if (count != s_last_count && now - s_log_ms > 5000)
		{
			s_log_ms = now;
			const float gx = F(kBlock + 0x10), gw = F(kBlock + 0x14), ox = F(kBlock + 0x18), xv = F(kBlock + 0x20),
						zv = F(kBlock + 0x24);
			Console.WriteLn("(VR) SpatialAudio: %u positioned sound updates; last: view (%.0f, %.0f) cm, azimuth %.0f deg, "
							"the game's x %.1f w %.2f (pan %d), ours x %.1f (pan %d); side agreement in front %u/%u.",
				count - s_last_count, static_cast<double>(xv), static_cast<double>(zv),
				static_cast<double>(std::atan2(xv, -zv) * 57.29578f), static_cast<double>(gx), static_cast<double>(gw),
				PanByte(gw < 0.0f ? 640.0f - std::clamp(gx, 0.0f, 640.0f) : gx), static_cast<double>(ox), PanByte(ox), s_agree,
				s_agree + s_disagree);
			s_last_count = count;
		}
	}

	int SelfTest()
	{
		int fail = 0;
		const auto check = [&fail](bool ok, const char* name) {
			if (!ok)
			{
				++fail;
				Console.Error("(VR) SpatialAudio self-test FAIL: %s", name);
			}
		};
		// Ahead (-z) and behind (+z) centred, right +x full right, left full left, 45 deg half way.
		check(PanByte(PanX(0.0f, -300.0f, 1.0f)) == 63 || PanByte(PanX(0.0f, -300.0f, 1.0f)) == 64, "ahead: centred");
		check(PanByte(PanX(0.0f, 300.0f, 1.0f)) == PanByte(PanX(0.0f, -300.0f, 1.0f)), "behind: centred too");
		check(PanByte(PanX(300.0f, 0.0f, 1.0f)) >= 126, "right: full right (126-127, the game's own float divide)");
		check(PanByte(PanX(-300.0f, 0.0f, 1.0f)) == 0, "left: full left (0)");
		check(PanByte(PanX(300.0f, 0.0f, -1.0f)) == 0, "sign -1 mirrors");
		const int p45 = PanByte(PanX(300.0f, -300.0f, 1.0f));
		check(p45 >= 107 && p45 <= 109, "45 deg right: 64 + 63 * 0.707");
		const int pb = PanByte(PanX(-300.0f, 300.0f, 1.0f));
		check(pb >= 18 && pb <= 20, "135 deg left (behind left): as far left as 45 deg left");
		check(PanByte(PanX(0.2f, 0.3f, 1.0f)) == PanByte(322.5f), "at the head: centred");
		check(PanByte(-50.0f) == 0 && PanByte(700.0f) == PanByte(640.0f), "pan byte clamps like the game");
		Console.WriteLn("(VR) SpatialAudio self-test: %d failure(s).", fail);
		return fail;
	}
} // namespace VR::SpatialAudio
