// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/HudCards.h"
#include "VR/CameraDriver.h"
#include "VR/VRProfileDB.h"

#include "Config.h"
#include "Memory.h"
#include "VMManager.h"

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

		// Comfort blink: opaque black; the fade is all in the upload's alpha scaling.
		constexpr u32 kBlinkBlack = Rgba(0, 0, 0, 255);
		constexpr u64 kBlinkTotalMs = static_cast<u64>(kBlinkRiseMs) + kBlinkHoldMs + kBlinkFallMs;

		// Wrist card: a dark panel edged in the condition colour, the condition label, the HP bar and the
		// virus line.
		constexpr u32 kWristFill = Rgba(0x10, 0x12, 0x18, 0xd8);
		constexpr u32 kWristText = Rgba(0xe4, 0xe8, 0xee);
		constexpr u32 kBarTrack = Rgba(0x2a, 0x2e, 0x38);
		constexpr u32 kColourFine = Rgba(0x5c, 0xd0, 0x6a);
		constexpr u32 kColourCaution = Rgba(0xf2, 0xc8, 0x3a);
		constexpr u32 kColourDanger = Rgba(0xec, 0x48, 0x40);
		constexpr u32 kColourUnknown = Rgba(0x8a, 0x90, 0x9c);
		constexpr int kWristRadius = 18;
		constexpr int kWristBorder = 3;
		constexpr int kWristPadPx = 14;
		constexpr int kLabelY = 36;
		constexpr int kBarX0 = 18;
		constexpr int kBarX1 = static_cast<int>(kWristWidth) - 19;
		constexpr int kBarY0 = 66;
		constexpr int kBarY1 = 87;
		constexpr int kBarWidthPx = kBarX1 - kBarX0 + 1;
		constexpr int kVirusY = 122;
		// Anything past this is not a character id, so it can't index the virus table.
		constexpr u32 kMaxCharacterId = 32;
		// A virus maximum outside 1..this is not one (a stale overlay or a wrong table): no gauge.
		constexpr u32 kMaxVirusMax = 100000000u;

		// Where the card sits on a left hand, in its pointing frame (metres): just off the back of the
		// glove's cuff (HandModel's wrist runs about 6-9 cm behind the grip, its back 4.7 cm to the left).
		constexpr float kWristOffset[3] = {-0.057f, -0.004f, 0.090f};
		// The card's axes in the pointing frame: text right = -Z (toward the fingers), text up = -Y, face
		// = -X (out of the back of the hand); a half turn about (1, 0, -1) / sqrt(2). With the wrist turned
		// to the face (palm down, forearm across the body) that reads upright, facing the eyes.
		constexpr float kWristLocalQuat[4] = {0.70710678f, 0.0f, -0.70710678f, 0.0f};

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
		V3 Sub(const V3& a, const V3& b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; }
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

		V3 Load(const float v[3]) { return V3{v[0], v[1], v[2]}; }
		void Store(const V3& a, float out[3])
		{
			out[0] = a.x;
			out[1] = a.y;
			out[2] = a.z;
		}

		// Rotates v by the unit quaternion q (xyzw).
		V3 Rotate(const float q[4], const V3& v)
		{
			const V3 u{q[0], q[1], q[2]};
			const V3 t = Mul(Cross(u, v), 2.0f);
			return Add(Add(v, Mul(t, q[3])), Cross(u, t));
		}

		// out = a * b (b applied first), xyzw.
		void QuatMul(const float a[4], const float b[4], float out[4])
		{
			out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
			out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
			out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
			out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
		}

		// Unit quaternion, identity when q is not usable.
		void NormalizeQuat(const float q[4], float out[4])
		{
			const float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
			if (!std::isfinite(n2) || n2 < 1.0e-12f)
			{
				out[0] = out[1] = out[2] = 0.0f;
				out[3] = 1.0f;
				return;
			}
			const float inv = 1.0f / std::sqrt(n2);
			for (int i = 0; i < 4; i++)
				out[i] = q[i] * inv;
		}

		// The rotation whose columns are the orthonormal axes x, y, z, as a quaternion (xyzw).
		void QuatFromBasis(const V3& x, const V3& y, const V3& z, float q[4])
		{
			const float m00 = x.x, m01 = y.x, m02 = z.x;
			const float m10 = x.y, m11 = y.y, m12 = z.y;
			const float m20 = x.z, m21 = y.z, m22 = z.z;
			const float tr = m00 + m11 + m22;
			if (tr > 0.0f)
			{
				const float sc = std::sqrt(tr + 1.0f) * 2.0f;
				q[3] = 0.25f * sc;
				q[0] = (m21 - m12) / sc;
				q[1] = (m02 - m20) / sc;
				q[2] = (m10 - m01) / sc;
			}
			else if (m00 > m11 && m00 > m22)
			{
				const float sc = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
				q[3] = (m21 - m12) / sc;
				q[0] = 0.25f * sc;
				q[1] = (m01 + m10) / sc;
				q[2] = (m02 + m20) / sc;
			}
			else if (m11 > m22)
			{
				const float sc = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
				q[3] = (m02 - m20) / sc;
				q[0] = (m01 + m10) / sc;
				q[1] = 0.25f * sc;
				q[2] = (m12 + m21) / sc;
			}
			else
			{
				const float sc = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
				q[3] = (m10 - m01) / sc;
				q[0] = (m02 + m20) / sc;
				q[1] = (m12 + m21) / sc;
				q[2] = 0.25f * sc;
			}
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

		// Blink opacity at now_ms for a blink started at start_ms (0: none yet): dark over the rise, black
		// through the hold, clear again over the fall.
		float BlinkOpacityAt(u64 start_ms, u64 now_ms)
		{
			if (start_ms == 0 || now_ms < start_ms)
				return 0.0f;
			const u64 t = now_ms - start_ms;
			if (t < kBlinkRiseMs)
				return static_cast<float>(t) / static_cast<float>(kBlinkRiseMs);
			if (t < static_cast<u64>(kBlinkRiseMs) + kBlinkHoldMs)
				return 1.0f;
			if (t < kBlinkTotalMs)
				return static_cast<float>(kBlinkTotalMs - t) / static_cast<float>(kBlinkFallMs);
			return 0.0f;
		}

		// The start that carries a blink on when it is triggered again at now_ms: kept while it is still going
		// dark, otherwise moved onto the rising edge at its current opacity (black now if it was holding), so
		// it never lightens and the hold runs in full again once it is black.
		u64 BlinkRestartAt(u64 start_ms, u64 now_ms)
		{
			const float o = BlinkOpacityAt(start_ms, now_ms);
			if (o <= 0.0f)
				return now_ms;
			if (now_ms - start_ms < kBlinkRiseMs)
				return start_ms;
			const u64 back = static_cast<u64>(std::lround(o * static_cast<float>(kBlinkRiseMs)));
			return (now_ms > back) ? (now_ms - back) : 1;
		}

		std::mutex s_blink_mutex;
		u64 s_blink_start_ms = 0;
		// The ComfortBlink setting, published by Poll (CPU thread) for every thread; on until the first
		// vsync, as the setting's default is.
		std::atomic<bool> s_blink_enabled{true};

		// The wrist card's reads, published by Poll (CPU thread) for the compositor.
		std::mutex s_wrist_mutex;
		bool s_wrist_active = false;
		WristData s_wrist;
		bool s_wrist_logged = false; // CPU thread

		bool ReadAt(u64 address, u8 width, u32* out)
		{
			if (address + width > Ps2MemSize::MainRam)
				return false;
			const u32 a = static_cast<u32>(address);
			switch (width)
			{
				case 1: *out = memRead8(a); break;
				case 2: *out = memRead16(a); break;
				default: *out = memRead32(a); break;
			}
			return true;
		}

		// Reads only. Each part is shown only when its values look like what they claim to be.
		// The last virus value read while the guards held, shown through a disarm grace instead of reading a
		// table that may already be gone (CPU thread only).
		bool s_virus_last_has = false;
		float s_virus_last_pct = 0.0f;

		void ReadWrist(const ProfileDB::HudWristParams& w, u32 record, bool guards_now, WristData* d)
		{
			const u64 rec = record;
			if (w.has_hp)
			{
				u32 hp = 0;
				u32 hp_max = 0;
				if (ReadAt(rec + w.hp_offset, w.hp_width, &hp) && ReadAt(rec + w.hp_max_offset, w.hp_width, &hp_max) &&
					hp_max > 0 && hp <= hp_max)
				{
					d->has_hp = true;
					d->hp = hp;
					d->hp_max = hp_max;
				}
			}
			if (w.has_virus && !guards_now)
			{
				d->has_virus = s_virus_last_has;
				d->virus_pct = s_virus_last_pct;
			}
			else if (w.has_virus)
			{
				// The per-character table is overlay data, resident only with the GAME overlay: read only while
				// the camera guards (which check exactly that) hold this vsync.
				u32 counter = 0;
				u32 id = 0;
				u32 vmax = 0;
				if (ReadAt(rec + w.virus_offset, 4, &counter) && ReadAt(rec + w.virus_char_offset, 1, &id) &&
					id < kMaxCharacterId && ReadAt(static_cast<u64>(w.virus_max_table) + static_cast<u64>(id) * 4u, 4, &vmax) &&
					vmax > 0 && vmax <= kMaxVirusMax && static_cast<u64>(counter) <= static_cast<u64>(vmax) * 2u)
				{
					d->has_virus = true;
					d->virus_pct = static_cast<float>(std::min(100.0, 100.0 * static_cast<double>(counter) / static_cast<double>(vmax)));
				}
				s_virus_last_has = d->has_virus;
				s_virus_last_pct = d->virus_pct;
			}
			if (w.has_bleed)
			{
				u32 v = 0;
				if (ReadAt(rec + w.bleed_offset, w.bleed_width, &v))
				{
					d->has_bleed = true;
					d->bleeding = (v != 0);
				}
			}
		}

		std::string WristLabel(const WristData& d, bool no_data)
		{
			if (no_data)
				return "NO DATA";
			const Condition c = ConditionOf(d);
			const bool bleed = d.has_bleed && d.bleeding;
			if (c == Condition::Danger)
				return bleed ? "DANGER BLEED" : "DANGER";
			if (bleed)
				return "BLEED";
			if (c == Condition::Caution)
				return "CAUTION";
			if (c == Condition::Fine)
				return "FINE";
			return "--";
		}

		u32 WristColour(const WristData& d, bool no_data)
		{
			if (no_data)
				return kColourUnknown;
			const Condition c = ConditionOf(d);
			if (c == Condition::Danger || (d.has_bleed && d.bleeding))
				return kColourDanger;
			if (c == Condition::Caution)
				return kColourCaution;
			if (c == Condition::Fine)
				return kColourFine;
			return kColourUnknown;
		}

		// HP bar fill in pixels; a sliver stays visible above 0 HP.
		int BarFillPx(const WristData& d)
		{
			if (!d.has_hp || d.hp_max == 0)
				return 0;
			const float f = std::clamp(static_cast<float>(d.hp) / static_cast<float>(d.hp_max), 0.0f, 1.0f);
			const int px = static_cast<int>(std::lround(f * static_cast<float>(kBarWidthPx)));
			return (d.hp > 0) ? std::max(px, 1) : px;
		}

		// The virus gauge in tenths of a percent as drawn, -1 without one.
		int VirusTenths(const WristData& d)
		{
			if (!d.has_virus || !std::isfinite(d.virus_pct))
				return -1;
			return std::clamp(static_cast<int>(std::lround(d.virus_pct * 10.0f)), 0, 1000);
		}

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

	bool Blink(const char* reason)
	{
		if (!s_blink_enabled.load(std::memory_order_relaxed))
			return false;
		const u64 now = NowMs();
		{
			std::lock_guard<std::mutex> lock(s_blink_mutex);
			s_blink_start_ms = BlinkRestartAt(s_blink_start_ms, now);
		}
		Console.WriteLn("(VR) HUD blink (%s).", reason ? reason : "blink");
		return true;
	}

	bool CurrentBlink(float* opacity)
	{
		const u64 now = NowMs();
		float o = 0.0f;
		{
			std::lock_guard<std::mutex> lock(s_blink_mutex);
			o = BlinkOpacityAt(s_blink_start_ms, now);
		}
		if (opacity)
			*opacity = o;
		return o > 0.0f;
	}

	Condition ConditionOf(const WristData& d)
	{
		if (!d.has_hp || d.hp_max == 0)
			return Condition::Unknown;
		const float f = static_cast<float>(d.hp) / static_cast<float>(d.hp_max);
		if (f < kDangerBelow)
			return Condition::Danger;
		if (f < kCautionBelow)
			return Condition::Caution;
		return Condition::Fine;
	}

	void Poll()
	{
		MaybeRunSelfTest();

		// Read live (the setting is not part of the VR config comparison), and only here: the compositor's
		// GS thread must not touch EmuConfig.
		s_blink_enabled.store(EmuConfig.VR.ComfortBlink, std::memory_order_relaxed);

		// LocalRecord is true only while first person is armed with the record resolved this vsync (flat play
		// never gets past it); guards_now says the camera guards held too, so the GAME overlay's virus table is
		// resident (during a disarm grace the last value read is shown instead).
		WristData d;
		bool active = false;
		u32 record = 0;
		bool guards_now = false;
		if (!CameraDriver::LocalRecord(&record, &guards_now))
			s_virus_last_has = false;
		else
		{
			const std::string serial = VMManager::GetDiscSerial();
			const ProfileDB::Profile* profile = serial.empty() ? nullptr : ProfileDB::Lookup(serial, VMManager::GetDiscCRC());
			if (profile && profile->hud.has_value() && profile->hud->wrist.has_value())
			{
				active = true;
				ReadWrist(profile->hud->wrist.value(), record, guards_now, &d);
			}
		}
		if (active != s_wrist_logged)
		{
			s_wrist_logged = active;
			if (active)
				Console.WriteLn("(VR) HUD: wrist card live: HP %u/%u%s, virus %.1f%%%s, bleeding %s.", d.hp, d.hp_max,
					d.has_hp ? "" : " (not read)", static_cast<double>(d.virus_pct), d.has_virus ? "" : " (not read)",
					d.has_bleed ? (d.bleeding ? "yes" : "no") : "not read");
		}
		std::lock_guard<std::mutex> lock(s_wrist_mutex);
		s_wrist_active = active;
		s_wrist = d;
	}

	bool GetWrist(WristData* out)
	{
		std::lock_guard<std::mutex> lock(s_wrist_mutex);
		if (out)
			*out = s_wrist;
		return s_wrist_active;
	}

	u64 WristKey(const WristData& d, bool no_data)
	{
		if (no_data)
			return 1ull << 40;
		u64 k = static_cast<u64>(ConditionOf(d));
		k |= static_cast<u64>(d.has_hp ? 1u : 0u) << 3;
		k |= static_cast<u64>((d.has_bleed && d.bleeding) ? 1u : 0u) << 4;
		k |= static_cast<u64>(BarFillPx(d)) << 8;
		k |= static_cast<u64>(VirusTenths(d) + 1) << 20;
		return k;
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

	void MaybeTestBlink()
	{
		static const bool s_test = [] {
			const char* v = std::getenv("PCSX2_VR_BLINK_TEST");
			return v && v[0] != '\0' && std::strcmp(v, "0") != 0;
		}();
		if (!s_test)
			return;
		// Compositor thread only.
		static u64 s_next_ms = 0;
		const u64 now = NowMs();
		if (now < s_next_ms)
			return;
		s_next_ms = now + 2000;
		Blink("PCSX2_VR_BLINK_TEST");
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

	void RasterWrist(std::vector<u32>& out, const WristData& d, bool no_data)
	{
		constexpr int w = static_cast<int>(kWristWidth);
		constexpr int h = static_cast<int>(kWristHeight);
		out.assign(static_cast<size_t>(kWristWidth) * kWristHeight, kTransparent);
		Canvas c{out, w, h};
		const u32 colour = WristColour(d, no_data);
		c.Panel(1, 1, w - 2, h - 2, kWristRadius, kWristFill, (colour & 0x00FFFFFFu) | (0xe0u << 24), kWristBorder);

		const std::string label = WristLabel(d, no_data);
		c.Text(w / 2, kLabelY, label, FitScale(label.size(), w - 2 * kWristPadPx, 4), colour);

		// HP bar: a dark track filled in the condition colour.
		c.Rect(kBarX0, kBarY0, kBarX1, kBarY1, kBarTrack);
		const int fill = no_data ? 0 : BarFillPx(d);
		if (fill > 0)
			c.Rect(kBarX0, kBarY0, kBarX0 + fill - 1, kBarY1, colour);

		// Tenths drawn from the same rounding WristKey uses, so the key and the picture always agree.
		const int tenths = no_data ? -1 : VirusTenths(d);
		char virus[32];
		if (tenths >= 0)
			std::snprintf(virus, sizeof(virus), "VIRUS %d.%d%%", tenths / 10, tenths % 10);
		else
			std::snprintf(virus, sizeof(virus), "%s", "VIRUS --");
		const std::string virus_line(virus);
		c.Text(w / 2, kVirusY, virus_line, FitScale(virus_line.size(), w - 2 * kWristPadPx, 3), kWristText);
	}

	void RasterBlink(std::vector<u32>& out)
	{
		out.assign(static_cast<size_t>(kBlinkImageSize) * kBlinkImageSize, kBlinkBlack);
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

	void BlinkPose(float pos[3], float quat[4])
	{
		pos[0] = 0.0f;
		pos[1] = 0.0f;
		pos[2] = -kBlinkDistanceM;
		quat[0] = 0.0f;
		quat[1] = 0.0f;
		quat[2] = 0.0f;
		quat[3] = 1.0f;
	}

	void PlaceWrist(const float hand_pos[3], const float hand_quat[4], const float eye[3], bool face_eye,
		float out_pos[3], float out_quat[4], float* facing_cos)
	{
		float q[4];
		NormalizeQuat(hand_quat, q);
		const V3 centre = Add(Load(hand_pos), Rotate(q, V3{kWristOffset[0], kWristOffset[1], kWristOffset[2]}));
		const V3 to_eye = Normalize(Sub(Load(eye), centre));
		if (face_eye)
		{
			// Turned to the eye with its text as upright as the world allows.
			const V3 z = (Dot(to_eye, to_eye) > 0.5f) ? to_eye : V3{0.0f, 0.0f, 1.0f};
			V3 x = Cross(V3{0.0f, 1.0f, 0.0f}, z);
			x = (Dot(x, x) > 1.0e-6f) ? Normalize(x) : V3{1.0f, 0.0f, 0.0f};
			const V3 y = Cross(z, x);
			QuatFromBasis(x, y, z, out_quat);
		}
		else
		{
			QuatMul(q, kWristLocalQuat, out_quat);
		}
		Store(centre, out_pos);
		if (facing_cos)
			*facing_cos = Dot(Rotate(out_quat, V3{0.0f, 0.0f, 1.0f}), to_eye);
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

		// Blink: black after 40 ms, held to 100 ms, clear at 180 ms; a new trigger never lightens it.
		check(BlinkOpacityAt(1000, 1000) == 0.0f, "blink starts clear");
		check(std::abs(BlinkOpacityAt(1000, 1020) - 0.5f) < 0.01f, "blink half dark after 20 ms");
		check(BlinkOpacityAt(1000, 1040) == 1.0f && BlinkOpacityAt(1000, 1099) == 1.0f, "blink black from 40 to 100 ms");
		check(std::abs(BlinkOpacityAt(1000, 1140) - 0.5f) < 0.01f, "blink half clear 40 ms into the fall");
		check(BlinkOpacityAt(1000, 1180) == 0.0f && BlinkOpacityAt(0, 5000) == 0.0f,
			"blink over after 180 ms, and none before the first");
		check(BlinkRestartAt(1000, 1020) == 1000u, "a trigger while going dark keeps going dark");
		check(BlinkRestartAt(1000, 1070) == 1030u && BlinkOpacityAt(1030, 1129) == 1.0f,
			"a trigger while black holds black again in full");
		const u64 again = BlinkRestartAt(1000, 1140);
		check(std::abs(BlinkOpacityAt(again, 1140) - 0.5f) < 0.03f && BlinkOpacityAt(again, 1141) >= BlinkOpacityAt(again, 1140),
			"a trigger while clearing goes dark again from where it was");
		check(BlinkRestartAt(1000, 1300) == 1300u, "a trigger after a blink starts a new one");
		std::vector<u32> bimg;
		RasterBlink(bimg);
		bool black = (bimg.size() == static_cast<size_t>(kBlinkImageSize) * kBlinkImageSize);
		for (const u32 p : bimg)
			black = black && (p == kBlinkBlack);
		check(black, "the blink image is opaque black");
		float bpos[3];
		float bquat[4];
		BlinkPose(bpos, bquat);
		const float blink_half_deg = std::atan((0.5f * kBlinkSizeM) / kBlinkDistanceM) * (180.0f / 3.14159265f);
		check(bpos[2] < 0.0f && bquat[3] == 1.0f && blink_half_deg > 75.0f, "the blink covers over 150 degrees ahead");

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

		// Condition: Danger below 20% of max HP, Caution below 60%, Fine from there; no HP, no condition.
		const auto cond = [](u32 hp, u32 hp_max) {
			WristData d;
			d.has_hp = true;
			d.hp = hp;
			d.hp_max = hp_max;
			return ConditionOf(d);
		};
		check(cond(100, 100) == Condition::Fine && cond(60, 100) == Condition::Fine, "Fine from 60% of max HP");
		check(cond(59, 100) == Condition::Caution && cond(20, 100) == Condition::Caution, "Caution from 20% to below 60%");
		check(cond(19, 100) == Condition::Danger && cond(0, 100) == Condition::Danger, "Danger below 20%");
		check(ConditionOf(WristData{}) == Condition::Unknown, "no HP read: no condition");

		// Labels and colours: bleeding shows over Fine and Caution, and next to Danger.
		WristData wd;
		wd.has_hp = true;
		wd.hp = 90;
		wd.hp_max = 100;
		wd.has_bleed = true;
		check(WristLabel(wd, false) == "FINE" && WristColour(wd, false) == kColourFine, "Fine is green");
		wd.bleeding = true;
		check(WristLabel(wd, false) == "BLEED" && WristColour(wd, false) == kColourDanger, "bleeding is a red BLEED");
		wd.hp = 10;
		check(WristLabel(wd, false) == "DANGER BLEED", "Danger and bleeding say both");
		wd.bleeding = false;
		wd.hp = 40;
		check(WristLabel(wd, false) == "CAUTION" && WristColour(wd, false) == kColourCaution, "Caution is yellow");
		check(WristLabel(wd, true) == "NO DATA", "the test card without data");

		// Keys: a change the card shows changes the key, one it can't show doesn't.
		WristData ka;
		ka.has_hp = true;
		ka.hp = 1000;
		ka.hp_max = 1000;
		ka.has_virus = true;
		ka.virus_pct = 12.34f;
		WristData kb = ka;
		kb.hp = 999;
		check(WristKey(ka, false) == WristKey(kb, false), "an HP change under one bar pixel keeps the key");
		kb.hp = 500;
		check(WristKey(ka, false) != WristKey(kb, false), "an HP change on the bar changes the key");
		kb = ka;
		kb.virus_pct = 12.36f;
		check(WristKey(ka, false) != WristKey(kb, false), "a virus tenth changes the key");
		check(WristKey(ka, true) != WristKey(ka, false), "the NO DATA card has its own key");
		WristData sliver = ka;
		sliver.hp = 1;
		check(BarFillPx(sliver) == 1, "a sliver of bar stays above 0 HP");

		// Wrist placement: the face points out of the back of the left hand (-X of its pointing frame), so
		// a head on that side sees it and one on the palm side doesn't; face_eye always faces the head.
		const float hand_pos[3] = {0.0f, 1.0f, 0.0f};
		const float hand_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		float wp[3];
		float wq[4];
		float facing = 0.0f;
		const float eye_back[3] = {-0.5f, 1.0f - 0.004f, 0.09f};
		PlaceWrist(hand_pos, hand_quat, eye_back, false, wp, wq, &facing);
		check(facing > 0.99f, "the wrist card faces out of the back of the hand");
		const V3 text_right = Rotate(wq, V3{1.0f, 0.0f, 0.0f});
		check(text_right.z < -0.99f, "the wrist card's text runs toward the fingers");
		const float eye_palm[3] = {0.5f, 1.0f - 0.004f, 0.09f};
		PlaceWrist(hand_pos, hand_quat, eye_palm, false, wp, wq, &facing);
		check(facing < -0.99f, "the wrist card is hidden from the palm side");
		PlaceWrist(hand_pos, hand_quat, eye_palm, true, wp, wq, &facing);
		check(facing > 0.999f, "test mode turns the wrist card to the eye");
		const V3 up = Rotate(wq, V3{0.0f, 1.0f, 0.0f});
		check(up.y > 0.99f, "test mode keeps the wrist card's text upright");

		// The wrist card rasters at its size with the label in its colour.
		std::vector<u32> wimg;
		RasterWrist(wimg, ka, false);
		check(wimg.size() == static_cast<size_t>(kWristWidth) * kWristHeight, "wrist image size");
		size_t green_px = 0;
		for (const u32 p : wimg)
			green_px += (p == kColourFine) ? 1u : 0u;
		check(green_px > 0, "the wrist card draws Fine in green");

		Console.WriteLn(fail == 0 ? Color_StrongGreen : Color_StrongRed, "(VR) HUD self-test: %d failure(s).", fail);
		return fail == 0;
	}
}
