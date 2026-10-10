// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/HudCards.h"

#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace VR::HudCards
{
	namespace
	{
		constexpr float kFadeMsF = kFadeSeconds * 1000.0f;
		constexpr u64 kFadeMs = static_cast<u64>(kFadeMsF + 0.5f);
		// Longer text is cut; at this length the toast still fits it at a readable size.
		constexpr size_t kMaxToastChars = 48;

		constexpr u32 kTransparent = Rgba(0, 0, 0, 0);
		constexpr u32 kToastFill = Rgba(0x12, 0x14, 0x1a, 0xd0);
		constexpr u32 kToastEdge = Rgba(0x56, 0x5c, 0x6a, 0xe8);
		constexpr u32 kToastText = Rgba(0xee, 0xf0, 0xf4);
		constexpr int kToastRadius = 40;
		constexpr int kToastBorder = 3;
		constexpr int kToastPadPx = 40;
		// 7 * 7 = 49 px letters, about 1.1 deg at the toast's size and distance.
		constexpr int kToastMaxScale = 7;

		u64 NowMs()
		{
			return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		struct V3
		{
			float x, y, z;
		};

		V3 Add(const V3& a, const V3& b) { return V3{a.x + b.x, a.y + b.y, a.z + b.z}; }
		V3 Mul(const V3& a, float s) { return V3{a.x * s, a.y * s, a.z * s}; }
		float Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		V3 Cross(const V3& a, const V3& b)
		{
			return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
		}
		V3 Normalize(const V3& a)
		{
			const float l = std::sqrt(Dot(a, a));
			return (l > 1.0e-8f) ? Mul(a, 1.0f / l) : V3{0.0f, 0.0f, 0.0f};
		}

		// Rotates v by the unit quaternion q (xyzw).
		V3 Rotate(const float q[4], const V3& v)
		{
			const V3 u{q[0], q[1], q[2]};
			const V3 t = Mul(Cross(u, v), 2.0f);
			return Add(Add(v, Mul(t, q[3])), Cross(u, t));
		}

		// 5 x 7 bitmap font, one string per row ('#' = lit). Lower case is drawn with the capitals.
		struct Glyph
		{
			char ch;
			const char* rows[7];
		};
		constexpr Glyph kFont[] = {
			{'A', {" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}},
			{'B', {"#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "}},
			{'C', {" ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### "}},
			{'D', {"#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "}},
			{'E', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"}},
			{'F', {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "}},
			{'G', {" ### ", "#   #", "#    ", "# ###", "#   #", "#   #", " ####"}},
			{'H', {"#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}},
			{'I', {" ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}},
			{'J', {"  ###", "   # ", "   # ", "   # ", "   # ", "#  # ", " ##  "}},
			{'K', {"#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"}},
			{'L', {"#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"}},
			{'M', {"#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #"}},
			{'N', {"#   #", "#   #", "##  #", "# # #", "#  ##", "#   #", "#   #"}},
			{'O', {" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}},
			{'P', {"#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "}},
			{'Q', {" ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"}},
			{'R', {"#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"}},
			{'S', {" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "}},
			{'T', {"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "}},
			{'U', {"#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}},
			{'V', {"#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "}},
			{'W', {"#   #", "#   #", "#   #", "# # #", "# # #", "# # #", " # # "}},
			{'X', {"#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"}},
			{'Y', {"#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "}},
			{'Z', {"#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"}},
			{'0', {" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "}},
			{'1', {"  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}},
			{'2', {" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"}},
			{'3', {"#####", "   # ", "  #  ", "   # ", "    #", "#   #", " ### "}},
			{'4', {"   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "}},
			{'5', {"#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "}},
			{'6', {"  ## ", " #   ", "#    ", "#### ", "#   #", "#   #", " ### "}},
			{'7', {"#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "}},
			{'8', {" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "}},
			{'9', {" ### ", "#   #", "#   #", " ####", "    #", "   # ", " ##  "}},
			{' ', {"     ", "     ", "     ", "     ", "     ", "     ", "     "}},
			{'.', {"     ", "     ", "     ", "     ", "     ", " ##  ", " ##  "}},
			{',', {"     ", "     ", "     ", "     ", " ##  ", "  #  ", " #   "}},
			{'%', {"##   ", "##  #", "   # ", "  #  ", " #   ", "#  ##", "   ##"}},
			{':', {"     ", " ##  ", " ##  ", "     ", " ##  ", " ##  ", "     "}},
			{'-', {"     ", "     ", "     ", "#####", "     ", "     ", "     "}},
			{'/', {"     ", "    #", "   # ", "  #  ", " #   ", "#    ", "     "}},
			{'+', {"     ", "  #  ", "  #  ", "#####", "  #  ", "  #  ", "     "}},
			{'!', {"  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "     ", "  #  "}},
			{'?', {" ### ", "#   #", "    #", "   # ", "  #  ", "     ", "  #  "}},
			{'(', {"   # ", "  #  ", " #   ", " #   ", " #   ", "  #  ", "   # "}},
			{')', {" #   ", "  #  ", "   # ", "   # ", "   # ", "  #  ", " #   "}},
		};
		constexpr int kGlyphW = 5;
		constexpr int kGlyphH = 7;
		constexpr int kAdvance = kGlyphW + 1; // one blank column between letters

		const Glyph* FindGlyph(char ch)
		{
			if (ch >= 'a' && ch <= 'z')
				ch = static_cast<char>(ch - 'a' + 'A');
			for (const Glyph& g : kFont)
			{
				if (g.ch == ch)
					return &g;
			}
			return nullptr;
		}

		int TextWidth(size_t chars, int scale)
		{
			return (chars == 0) ? 0 : static_cast<int>(chars) * kAdvance * scale - scale;
		}

		// The largest whole-pixel scale (max_scale down to 1) at which `chars` letters fit max_width.
		int FitScale(size_t chars, int max_width, int max_scale)
		{
			for (int sc = max_scale; sc > 1; sc--)
			{
				if (TextWidth(chars, sc) <= max_width)
					return sc;
			}
			return 1;
		}

		struct Canvas
		{
			std::vector<u32>& px;
			int w;
			int h;

			void Put(int x, int y, u32 c)
			{
				if (x < 0 || y < 0 || x >= w || y >= h)
					return;
				px[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = c;
			}

			// Inclusive corners.
			void Rect(int x0, int y0, int x1, int y1, u32 c)
			{
				for (int y = std::max(y0, 0); y <= std::min(y1, h - 1); ++y)
				{
					for (int x = std::max(x0, 0); x <= std::min(x1, w - 1); ++x)
						px[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = c;
				}
			}

			// Rounded panel, edge colour in the outer `border` pixels, with an anti-aliased outline (the
			// colours carry their own alpha; coverage scales it).
			void Panel(int x0, int y0, int x1, int y1, int radius, u32 fill, u32 edge, int border)
			{
				const float r = static_cast<float>(radius);
				for (int y = y0; y <= y1; ++y)
				{
					for (int x = x0; x <= x1; ++x)
					{
						const float cx = std::clamp(static_cast<float>(x), static_cast<float>(x0 + radius), static_cast<float>(x1 - radius));
						const float cy = std::clamp(static_cast<float>(y), static_cast<float>(y0 + radius), static_cast<float>(y1 - radius));
						const float dx = static_cast<float>(x) - cx;
						const float dy = static_cast<float>(y) - cy;
						const float d = std::sqrt(dx * dx + dy * dy);
						const float cover = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
						if (cover <= 0.0f)
							continue;
						const bool on_edge = (d > r - static_cast<float>(border) + 0.5f) || x < x0 + border ||
						                     x > x1 - border || y < y0 + border || y > y1 - border;
						const u32 c = on_edge ? edge : fill;
						const u32 a = static_cast<u32>(std::lround(static_cast<float>(c >> 24) * cover));
						Put(x, y, (c & 0x00FFFFFFu) | (a << 24));
					}
				}
			}

			// Text centred on (cx, cy), each font pixel scale x scale.
			void Text(int cx, int cy, const std::string& text, int scale, u32 colour)
			{
				int x = cx - TextWidth(text.size(), scale) / 2;
				const int y0 = cy - (kGlyphH * scale) / 2;
				for (const char ch : text)
				{
					const Glyph* g = FindGlyph(ch);
					if (g)
					{
						for (int row = 0; row < kGlyphH; ++row)
						{
							for (int col = 0; col < kGlyphW; ++col)
							{
								if (g->rows[row][col] == '#')
									Rect(x + col * scale, y0 + row * scale, x + col * scale + scale - 1, y0 + row * scale + scale - 1, colour);
							}
						}
					}
					x += kAdvance * scale;
				}
			}
		};

		// Toast opacity at now_ms for a toast shown from start_ms to end_ms: fades in, holds, fades out.
		float ToastOpacityAt(u64 start_ms, u64 end_ms, u64 now_ms)
		{
			if (end_ms <= start_ms || now_ms < start_ms || now_ms >= end_ms)
				return 0.0f;
			const float in = static_cast<float>(now_ms - start_ms) / kFadeMsF;
			const float out = static_cast<float>(end_ms - now_ms) / kFadeMsF;
			return std::clamp(std::min(in, out), 0.0f, 1.0f);
		}

		struct ToastState
		{
			std::string text;
			u64 start_ms = 0;
			u64 end_ms = 0;
			u32 serial = 0;
		};
		std::mutex s_toast_mutex;
		ToastState s_toast;

		std::atomic<bool> s_selftest_ran{false};

		void MaybeRunSelfTest()
		{
			static const bool enabled = (std::getenv("PCSX2_VR_HUD_SELFTEST") != nullptr);
			if (!enabled || s_selftest_ran.exchange(true, std::memory_order_acq_rel))
				return;
			SelfTest();
		}
	}

	void Toast(const std::string& text, float seconds)
	{
		if (!std::isfinite(seconds))
			seconds = 2.0f;
		seconds = std::clamp(seconds, 0.5f, 10.0f);
		const std::string shown = text.substr(0, kMaxToastChars);
		const u64 now = NowMs();
		{
			std::lock_guard<std::mutex> lock(s_toast_mutex);
			// A toast replacing one still on screen starts at full opacity instead of blinking out and in.
			const bool showing = ToastOpacityAt(s_toast.start_ms, s_toast.end_ms, now) > 0.0f;
			s_toast.text = shown;
			s_toast.start_ms = (showing && now > kFadeMs) ? (now - kFadeMs) : now;
			s_toast.end_ms = now + static_cast<u64>(seconds * 1000.0f);
			s_toast.serial++;
		}
		Console.WriteLn("(VR) HUD toast: \"%s\" (%.1f s).", shown.c_str(), static_cast<double>(seconds));
	}

	bool CurrentToast(ToastView* out)
	{
		const u64 now = NowMs();
		std::lock_guard<std::mutex> lock(s_toast_mutex);
		const float opacity = ToastOpacityAt(s_toast.start_ms, s_toast.end_ms, now);
		if (opacity <= 0.0f)
			return false;
		if (out)
		{
			out->text = s_toast.text;
			out->serial = s_toast.serial;
			out->opacity = opacity;
		}
		return true;
	}

	void MaybeTestToast()
	{
		MaybeRunSelfTest();

		static const bool s_test = [] {
			const char* v = std::getenv("PCSX2_VR_HUD_TEST");
			return v && v[0] != '\0' && std::strcmp(v, "0") != 0;
		}();
		if (!s_test)
			return;
		// Compositor thread only.
		static u64 s_next_ms = 0;
		static u32 s_count = 0;
		const u64 now = NowMs();
		if (now < s_next_ms)
			return;
		s_next_ms = now + 3000;
		static const char* const kLines[] = {
			"HUD test: toast card",
			"ABCDEFGHIJKLM 01234",
			"NOPQRSTUVWXYZ 56789",
			". , % : - / + ! ? ( )",
			"Recentered (SteamVR)",
		};
		constexpr u32 kLineCount = static_cast<u32>(sizeof(kLines) / sizeof(kLines[0]));
		Toast(kLines[s_count % kLineCount], 2.0f);
		s_count++;
	}

	void RasterToast(std::vector<u32>& out, const std::string& text)
	{
		constexpr int w = static_cast<int>(kToastWidth);
		constexpr int h = static_cast<int>(kToastHeight);
		out.assign(static_cast<size_t>(kToastWidth) * kToastHeight, kTransparent);
		Canvas c{out, w, h};
		c.Panel(1, 1, w - 2, h - 2, kToastRadius, kToastFill, kToastEdge, kToastBorder);
		const int scale = FitScale(text.size(), w - 2 * kToastPadPx, kToastMaxScale);
		c.Text(w / 2, h / 2, text, scale, kToastText);
	}

	void ScaleAlpha(const u32* src, size_t count, float opacity, u32* dst)
	{
		const float o = std::isfinite(opacity) ? std::clamp(opacity, 0.0f, 1.0f) : 0.0f;
		const u32 k = static_cast<u32>(std::lround(o * 255.0f));
		for (size_t i = 0; i < count; i++)
		{
			const u32 p = src[i];
			const u32 a = ((p >> 24) * k + 127u) / 255u;
			dst[i] = (p & 0x00FFFFFFu) | (a << 24);
		}
	}

	void ToastPose(float pos[3], float quat[4])
	{
		pos[0] = 0.0f;
		pos[1] = -kToastDropM;
		pos[2] = -kToastDistanceM;
		// Tilted about X so its face (+Z) points back up at the eye rather than over its head.
		const float tilt = -std::atan2(kToastDropM, kToastDistanceM);
		quat[0] = std::sin(tilt * 0.5f);
		quat[1] = 0.0f;
		quat[2] = 0.0f;
		quat[3] = std::cos(tilt * 0.5f);
	}

	bool SelfTest()
	{
		int fail = 0;
		const auto check = [&fail](bool ok, const char* name) {
			if (!ok)
			{
				++fail;
				Console.Error("(VR) HUD self-test FAIL: %s", name);
			}
		};

		// Font: every glyph is 5 x 7 and the whole required set is there.
		bool rows_ok = true;
		for (const Glyph& g : kFont)
		{
			for (const char* row : g.rows)
				rows_ok = rows_ok && row && std::strlen(row) == static_cast<size_t>(kGlyphW);
		}
		check(rows_ok, "every glyph row is 5 pixels wide");
		bool all = true;
		for (const char* p = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,%:-/+!?()"; *p; ++p)
			all = all && (FindGlyph(*p) != nullptr);
		check(all, "the font covers A-Z, 0-9, space and . , % : - / + ! ? ( )");
		check(FindGlyph('q') == FindGlyph('Q') && FindGlyph('q') != nullptr, "lower case draws as capitals");
		check(FindGlyph('~') == nullptr, "characters outside the font are left blank");

		// Fades: 0.15 s in, 0.15 s out, opaque in between.
		check(ToastOpacityAt(1000, 3000, 1000) == 0.0f, "toast starts transparent");
		check(std::abs(ToastOpacityAt(1000, 3000, 1075) - 0.5f) < 0.01f, "toast half faded in after 75 ms");
		check(ToastOpacityAt(1000, 3000, 2000) == 1.0f, "toast opaque mid-way");
		check(std::abs(ToastOpacityAt(1000, 3000, 2925) - 0.5f) < 0.01f, "toast half faded out 75 ms before the end");
		check(ToastOpacityAt(1000, 3000, 3000) == 0.0f, "toast gone at the end");

		// Alpha scaling keeps the colour.
		const u32 px = Rgba(10, 20, 30, 200);
		u32 half = 0;
		ScaleAlpha(&px, 1, 0.5f, &half);
		check((half & 0x00FFFFFFu) == (px & 0x00FFFFFFu) && (half >> 24) == 100u, "alpha scaling keeps the colour");

		// The toast faces the eye from below the line of sight.
		float pos[3];
		float q[4];
		ToastPose(pos, q);
		const V3 face = Rotate(q, V3{0.0f, 0.0f, 1.0f});
		const V3 to_eye = Normalize(V3{-pos[0], -pos[1], -pos[2]});
		check(Dot(face, to_eye) > 0.999f, "toast faces the eye");

		// Rastering: the right size, opaque text on a translucent panel, clear corners.
		std::vector<u32> img;
		RasterToast(img, "Recentered (SteamVR)");
		check(img.size() == static_cast<size_t>(kToastWidth) * kToastHeight, "toast image size");
		size_t text_px = 0;
		for (const u32 p : img)
			text_px += (p == kToastText) ? 1u : 0u;
		check(text_px > 0, "toast text is drawn");
		check(!img.empty() && (img[0] >> 24) == 0u, "toast corner is clear");
		check(img.size() > static_cast<size_t>(kToastWidth) * 8u + 8u &&
				  (img[static_cast<size_t>(kToastWidth) * 8u + 8u] >> 24) == 0u,
			"toast rounded corner is clear");
		check(FitScale(10, static_cast<int>(kToastWidth) - 2 * kToastPadPx, kToastMaxScale) == kToastMaxScale &&
				  FitScale(kMaxToastChars, static_cast<int>(kToastWidth) - 2 * kToastPadPx, kToastMaxScale) >= 3,
			"toast text scales down to fit");

		Console.WriteLn(fail == 0 ? Color_StrongGreen : Color_StrongRed, "(VR) HUD self-test: %d failure(s).", fail);
		return fail == 0;
	}
}
