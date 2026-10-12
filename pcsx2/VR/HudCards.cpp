// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/HudCards.h"
#include "VR/CameraDriver.h"
#include "VR/GameFeedback.h"
#include "VR/ItemData.h"
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
		// With a weapon in hand the rounds go under the bar and the virus line moves down and shrinks.
		constexpr int kAmmoY = 106;
		constexpr int kVirusWithAmmoY = 136;
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

		// Laser sight: red, paler toward a hot core, at most this opaque (just past the muzzle, on the centre
		// line); the alpha falls off across the beam and along it.
		constexpr float kLaserPeakAlpha = 230.0f;
		// Fraction of the length (about 3 cm) over which the beam fades in from its start, so it doesn't begin
		// on a hard edge; and how steeply it fades out along the rest (alpha ~ (1 - s)^k at fraction s of the
		// length: about 40% at 3 m, 10% at 5 m).
		constexpr float kLaserStartFade = 0.005f;
		constexpr float kLaserFadePower = 1.3f;
		// Half-width fraction (from the centre line) of the paler core.
		constexpr float kLaserCoreHalf = 0.4f;
		// The impostor pistol's muzzle in the right hand's pointing frame, metres: the front face of HandModel's
		// muzzle box (BuildGunTop), on the barrel's axis 6.7 cm above the grip position and 14.14 cm ahead of it.
		constexpr float kPistolMuzzle[3] = {0.0f, 0.067f, -0.1414f};
		// Nearer the beam's line than this, the eye sees the quad edge-on.
		constexpr float kLaserMinOffAxisM = 0.001f;

		// Menu pointer's dot: radii in image pixels from the centre of the kPointerImageSize image (32 px). Off a cell a
		// white core with a dark rim (readable on the item screen's white highlight and its dark panels alike); on a cell
		// a warm white core inside a thin amber ring.
		constexpr float kDotOuterPx = 29.5f;
		constexpr float kDotCorePx = 19.0f;
		constexpr float kDotRingInPx = 23.5f;
		constexpr float kDotRingOutPx = 27.5f;
		// Menu pointer's beam: at most this opaque on its centre line, and fading in over this fraction of its length
		// from the hand.
		constexpr float kBeamPeakAlpha = 170.0f;
		constexpr float kBeamStartFade = 0.10f;
		constexpr float kBeamCoreHalf = 0.4f;

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
			// For item names ("Kevin's 45 Auto", "S&W model 500").
			{'\'', {" ##  ", "  #  ", " #   ", "     ", "     ", "     ", "     "}},
			{'&', {" ##  ", "#  # ", "# #  ", " #   ", "# # #", "#  # ", " ## #"}},
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
		// The LaserSight setting, published the same way.
		std::atomic<bool> s_laser_enabled{true};
		// The MenuPointer and MenuTablet settings, published the same way (on and off until the first vsync, as their
		// defaults are).
		std::atomic<bool> s_menu_pointer_enabled{true};
		std::atomic<bool> s_menu_tablet_enabled{false};
		// The comfort vignette's strength (0 while ComfortVignette is off), published the same way; off until the
		// first vsync, as the setting's default is.
		std::atomic<float> s_vignette_strength{0.0f};

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

		// The longest ammo line that still draws at scale 2 inside the wrist card's padding: 19 characters.
		constexpr size_t kWristLineChars =
			static_cast<size_t>((static_cast<int>(kWristWidth) - 2 * kWristPadPx + 2) / (2 * kAdvance));

		// A weapon's name for the wrist card, in capitals and without the owner Outbreak's character weapons carry:
		// "Kevin's 45 Auto" -> "45 AUTO", "Folding Knife(David's)" -> "FOLDING KNIFE".
		std::string WristWeaponName(const char* name)
		{
			const std::string full = name ? name : "";
			std::string s = full;
			// A leading "<Name>'s ".
			const size_t space = s.find(' ');
			if (space != std::string::npos && space >= 2 && s.compare(space - 2, 2, "'s") == 0 && space + 1 < s.size())
				s.erase(0, space + 1);
			// A trailing "(<Name>'s)".
			const size_t open = s.rfind('(');
			if (open != std::string::npos && s.size() >= 3 && s.compare(s.size() - 3, 3, "'s)") == 0)
			{
				s.erase(open);
				while (!s.empty() && s.back() == ' ')
					s.pop_back();
			}
			if (s.empty())
				s = full;
			for (char& c : s)
			{
				if (c >= 'a' && c <= 'z')
					c = static_cast<char>(c - 'a' + 'A');
			}
			return s;
		}

		// The wrist card's ammo line: the equipped weapon's name and its rounds ("45 AUTO 7"), the name alone for a
		// weapon without rounds (a knife, a pipe, a molotov), or "AMMO 7" without an id ItemData knows. A name too long
		// for kWristLineChars is cut, closing a bracket the cut leaves open.
		std::string WristAmmoLine(const WristData& d)
		{
			char rounds[16];
			std::snprintf(rounds, sizeof(rounds), "%u", static_cast<unsigned>(d.ammo));
			const ItemData::Entry* e = d.has_item ? ItemData::Find(d.item_id) : nullptr;
			std::string name = e ? WristWeaponName(e->name) : std::string();
			if (!e || name.empty())
				return std::string("AMMO ") + rounds;
			const bool with_rounds = ItemData::IsGun(e->kind) || e->kind == ItemData::Kind::Spray;
			const size_t suffix = with_rounds ? (1 + std::strlen(rounds)) : 0;
			const size_t room = (kWristLineChars > suffix) ? (kWristLineChars - suffix) : 0;
			if (name.size() > room)
			{
				name.resize(room);
				const size_t open = name.rfind('(');
				if (open != std::string::npos && name.find(')', open) == std::string::npos)
				{
					// Cut inside a bracket: make room to close it, unless nothing inside it would be left.
					name.pop_back();
					while (!name.empty() && name.back() == ' ')
						name.pop_back();
					if (name.size() > open + 1)
						name.push_back(')');
					else
						name.resize(std::min(name.size(), open));
				}
				while (!name.empty() && name.back() == ' ')
					name.pop_back();
			}
			if (name.empty())
				return std::string("AMMO ") + rounds;
			return with_rounds ? (name + " " + rounds) : name;
		}

		// Which ItemData entry the ammo line names, as its index + 1; 0 for the plain AMMO line (no line, no id or one the
		// table does not have), which depends on the rounds alone.
		u32 WristItemSlot(const WristData& d)
		{
			if (!d.has_ammo || !d.has_item || ItemData::Count() == 0)
				return 0;
			const ItemData::Entry* e = ItemData::Find(d.item_id);
			return e ? static_cast<u32>(e - &ItemData::At(0)) + 1u : 0u;
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
		s_laser_enabled.store(EmuConfig.VR.LaserSight, std::memory_order_relaxed);
		s_menu_pointer_enabled.store(EmuConfig.VR.MenuPointer, std::memory_order_relaxed);
		s_menu_tablet_enabled.store(EmuConfig.VR.MenuTablet, std::memory_order_relaxed);
		// Clamped here too, for a value that did not come through LoadSave.
		float vignette = 0.0f;
		if (EmuConfig.VR.ComfortVignette)
		{
			const float v = EmuConfig.VR.VignetteStrength;
			vignette = std::isfinite(v) ? std::clamp(v, Pcsx2Config::VROptions::MIN_VIGNETTE_STRENGTH,
			                                  Pcsx2Config::VROptions::MAX_VIGNETTE_STRENGTH) :
			                              Pcsx2Config::VROptions::DEFAULT_VIGNETTE_STRENGTH;
		}
		s_vignette_strength.store(vignette, std::memory_order_relaxed);

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
				// The rounds in the equipped gun (feedback.weapon), when GameFeedback trusts the record, and its item id
				// for the weapon's name.
				if (GameFeedback::WeaponInHand() == GameFeedback::Hold::Weapon)
				{
					const GameFeedback::WeaponView wv = GameFeedback::GetWeapon();
					if (wv.valid)
					{
						d.has_ammo = true;
						d.ammo = wv.ammo;
						d.has_item = true;
						d.item_id = wv.item_id;
					}
				}
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
		k |= static_cast<u64>(d.has_ammo ? 1u : 0u) << 5;
		if (d.has_ammo)
			k |= static_cast<u64>(d.ammo) << 44;
		// The weapon the ammo line names (ItemData has fewer than 1024 entries).
		k |= static_cast<u64>(WristItemSlot(d) & 0x3FFu) << 30;
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
			". , % : - / + ! ? ( ) ' &",
			"Kevin's 45 Auto 7, S&W",
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

	bool LaserSightEnabled()
	{
		return s_laser_enabled.load(std::memory_order_relaxed);
	}

	bool MenuPointerEnabled()
	{
		return s_menu_pointer_enabled.load(std::memory_order_relaxed);
	}

	bool MenuTabletEnabled()
	{
		return s_menu_tablet_enabled.load(std::memory_order_relaxed);
	}

	float VignetteStrength()
	{
		return s_vignette_strength.load(std::memory_order_relaxed);
	}

	bool TestVignetteMotion(float* motion)
	{
		static const bool s_test = [] {
			const char* v = std::getenv("PCSX2_VR_VIGNETTE_TEST");
			return v && v[0] != '\0' && std::strcmp(v, "0") != 0;
		}();
		if (!s_test)
			return false;
		// Up over a second, down over the next.
		const u64 phase = NowMs() % 2000u;
		const u64 rise = (phase < 1000u) ? phase : (2000u - phase);
		if (motion)
			*motion = static_cast<float>(rise) / 1000.0f;
		return true;
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
		const bool ammo = !no_data && d.has_ammo;
		if (ammo)
		{
			// The weapon's name and rounds ("45 AUTO 7"), or "AMMO 7"; at scale 2 or larger (WristAmmoLine cuts to fit).
			const std::string ammo_line = WristAmmoLine(d);
			c.Text(w / 2, kAmmoY, ammo_line, FitScale(ammo_line.size(), w - 2 * kWristPadPx, 3), kWristText);
		}
		c.Text(w / 2, ammo ? kVirusWithAmmoY : kVirusY, virus_line,
			FitScale(virus_line.size(), w - 2 * kWristPadPx, ammo ? 2 : 3), kWristText);
	}

	void RasterBlink(std::vector<u32>& out)
	{
		out.assign(static_cast<size_t>(kBlinkImageSize) * kBlinkImageSize, kBlinkBlack);
	}

	void RasterLaser(std::vector<u32>& out)
	{
		constexpr int w = static_cast<int>(kLaserImageWidth);
		constexpr int h = static_cast<int>(kLaserImageHeight);
		out.assign(static_cast<size_t>(kLaserImageWidth) * kLaserImageHeight, kTransparent);
		for (int y = 0; y < h; ++y)
		{
			// Row 0 is the quad's top, the beam's far end: s runs from 0 at the muzzle end to 1 there.
			const float s = (static_cast<float>(h - 1 - y) + 0.5f) / static_cast<float>(h);
			const float along = std::min(1.0f, s / kLaserStartFade) * std::pow(1.0f - s, kLaserFadePower);
			for (int x = 0; x < w; ++x)
			{
				// t: 0 on the centre line, 1 at the edges.
				const float t = std::fabs((static_cast<float>(x) + 0.5f) / static_cast<float>(w) - 0.5f) * 2.0f;
				const float across = (1.0f - t * t) * (1.0f - t * t);
				const float core = std::clamp(1.0f - t / kLaserCoreHalf, 0.0f, 1.0f);
				const float alpha = std::clamp(kLaserPeakAlpha * across * along, 0.0f, 255.0f);
				const u32 a = static_cast<u32>(std::lround(alpha));
				const u32 g = static_cast<u32>(std::lround(30.0f + 110.0f * core));
				const u32 b = static_cast<u32>(std::lround(20.0f + 90.0f * core));
				out[static_cast<size_t>(y) * kLaserImageWidth + static_cast<size_t>(x)] = Rgba(255, g, b, a);
			}
		}
	}

	void RasterVignette(std::vector<u32>& out)
	{
		constexpr int n = static_cast<int>(kVignetteImageSize);
		out.assign(static_cast<size_t>(kVignetteImageSize) * kVignetteImageSize, kBlinkBlack);
		// The ring's radius in pixels: where tangent kVignetteRadiusTan off the line of sight lands on the quad.
		constexpr float half = 0.5f * static_cast<float>(n);
		constexpr float radius_px = (kVignetteRadiusTan * kVignetteDistanceM) / (0.5f * kVignetteSizeM) * half;
		for (int y = 0; y < n; ++y)
		{
			const float dy = (static_cast<float>(y) + 0.5f) - half;
			for (int x = 0; x < n; ++x)
			{
				const float dx = (static_cast<float>(x) + 0.5f) - half;
				// r: 0 at the centre, 1 on the ring; clear to kVignetteClearFrac, opaque from 1 out to the corners.
				const float r = std::sqrt(dx * dx + dy * dy) / radius_px;
				const float t = std::clamp((r - kVignetteClearFrac) / (1.0f - kVignetteClearFrac), 0.0f, 1.0f);
				const float alpha = 255.0f * t * t * (3.0f - 2.0f * t);
				out[static_cast<size_t>(y) * kVignetteImageSize + static_cast<size_t>(x)] =
					Rgba(0, 0, 0, static_cast<u32>(std::lround(alpha)));
			}
		}
	}

	void RasterPointer(std::vector<u32>& out, bool on_cell)
	{
		constexpr int n = static_cast<int>(kPointerImageSize);
		out.assign(static_cast<size_t>(kPointerImageSize) * kPointerImageSize, kTransparent);
		constexpr float half = 0.5f * static_cast<float>(n);
		// Coverage of the ring r0..r1 at radius r, anti-aliased over a pixel at each edge.
		const auto band = [](float r, float r0, float r1) {
			return std::clamp(r - r0 + 0.5f, 0.0f, 1.0f) * std::clamp(r1 - r + 0.5f, 0.0f, 1.0f);
		};
		struct Layer
		{
			float r, g, b, a;
		};
		for (int y = 0; y < n; ++y)
		{
			const float dy = (static_cast<float>(y) + 0.5f) - half;
			for (int x = 0; x < n; ++x)
			{
				const float dx = (static_cast<float>(x) + 0.5f) - half;
				const float r = std::sqrt(dx * dx + dy * dy);
				const float core = std::clamp(kDotCorePx + 0.5f - r, 0.0f, 1.0f);
				Layer layers[3] = {};
				int count = 0;
				if (!on_cell)
				{
					// A white core inside a dark rim out to the edge.
					layers[count++] = {255.0f, 255.0f, 255.0f, 255.0f * core};
					layers[count++] = {24.0f, 26.0f, 32.0f, 215.0f * band(r, kDotCorePx, kDotOuterPx)};
				}
				else
				{
					// A warm white core, a faint gap, then a thin amber ring.
					layers[count++] = {255.0f, 238.0f, 205.0f, 255.0f * core};
					layers[count++] = {255.0f, 238.0f, 205.0f, 60.0f * band(r, kDotCorePx, kDotRingInPx)};
					layers[count++] = {255.0f, 186.0f, 84.0f, 235.0f * band(r, kDotRingInPx, kDotRingOutPx)};
				}
				float sum_a = 0.0f, cr = 0.0f, cg = 0.0f, cb = 0.0f;
				for (int i = 0; i < count; ++i)
				{
					sum_a += layers[i].a;
					cr += layers[i].r * layers[i].a;
					cg += layers[i].g * layers[i].a;
					cb += layers[i].b * layers[i].a;
				}
				if (sum_a <= 0.0f)
					continue;
				const auto channel = [sum_a](float c) {
					return static_cast<u32>(std::clamp(std::lround(c / sum_a), 0L, 255L));
				};
				const u32 a = static_cast<u32>(std::lround(std::min(sum_a, 255.0f)));
				out[static_cast<size_t>(y) * kPointerImageSize + static_cast<size_t>(x)] =
					Rgba(channel(cr), channel(cg), channel(cb), a);
			}
		}
	}

	void RasterBeam(std::vector<u32>& out)
	{
		constexpr int w = static_cast<int>(kLaserImageWidth);
		constexpr int h = static_cast<int>(kLaserImageHeight);
		out.assign(static_cast<size_t>(kLaserImageWidth) * kLaserImageHeight, kTransparent);
		for (int y = 0; y < h; ++y)
		{
			// Row 0 is the quad's top, the beam's far end on the screen: s runs from 0 at the hand to 1 there.
			const float s = (static_cast<float>(h - 1 - y) + 0.5f) / static_cast<float>(h);
			const float along = std::min(1.0f, s / kBeamStartFade);
			for (int x = 0; x < w; ++x)
			{
				// t: 0 on the centre line, 1 at the edges.
				const float t = std::fabs((static_cast<float>(x) + 0.5f) / static_cast<float>(w) - 0.5f) * 2.0f;
				const float across = (1.0f - t * t) * (1.0f - t * t);
				const float core = std::clamp(1.0f - t / kBeamCoreHalf, 0.0f, 1.0f);
				const u32 a = static_cast<u32>(std::lround(std::clamp(kBeamPeakAlpha * across * along, 0.0f, 255.0f)));
				const u32 r = static_cast<u32>(std::lround(150.0f + 105.0f * core));
				const u32 g = static_cast<u32>(std::lround(200.0f + 55.0f * core));
				out[static_cast<size_t>(y) * kLaserImageWidth + static_cast<size_t>(x)] = Rgba(r, g, 255, a);
			}
		}
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

	void VignettePose(float pos[3], float quat[4])
	{
		pos[0] = 0.0f;
		pos[1] = 0.0f;
		pos[2] = -kVignetteDistanceM;
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

	bool PlaceLaser(const float hand_pos[3], const float hand_quat[4], const float eye[3], float out_pos[3],
		float out_quat[4])
	{
		for (int i = 0; i < 3; i++)
		{
			if (!std::isfinite(hand_pos[i]) || !std::isfinite(eye[i]))
				return false;
		}
		// A beam pointing the wrong way is worse than none: no identity fallback for an unusable orientation.
		const float n2 = hand_quat[0] * hand_quat[0] + hand_quat[1] * hand_quat[1] + hand_quat[2] * hand_quat[2] +
		                 hand_quat[3] * hand_quat[3];
		if (!std::isfinite(n2) || n2 < 1.0e-6f)
			return false;
		float q[4];
		NormalizeQuat(hand_quat, q);
		const V3 dir = Rotate(q, V3{0.0f, 0.0f, -1.0f});
		const V3 muzzle = Add(Load(hand_pos), Rotate(q, V3{kPistolMuzzle[0], kPistolMuzzle[1], kPistolMuzzle[2]}));
		float start[3];
		float along[3];
		Store(Add(muzzle, Mul(dir, kLaserMuzzleGapM)), start);
		Store(dir, along);
		return PlaceBeam(start, along, kLaserLengthM, eye, out_pos, out_quat);
	}

	bool PlaceBeam(const float origin[3], const float dir[3], float length, const float eye[3], float out_pos[3],
		float out_quat[4])
	{
		for (int i = 0; i < 3; i++)
		{
			if (!std::isfinite(origin[i]) || !std::isfinite(dir[i]) || !std::isfinite(eye[i]))
				return false;
		}
		if (!std::isfinite(length) || !(length > 0.0f))
			return false;
		const V3 d = Normalize(Load(dir));
		if (Dot(d, d) < 0.5f)
			return false;
		const V3 centre = Add(Load(origin), Mul(d, 0.5f * length));
		// The eye's offset from the beam's line, the same from every point along it: turning the quad's face
		// toward it squares the whole beam to the eye as far as one flat quad can be.
		const V3 to_eye = Sub(Load(eye), centre);
		const V3 off_axis = Sub(to_eye, Mul(d, Dot(to_eye, d)));
		const float off_len = std::sqrt(Dot(off_axis, off_axis));
		if (!(off_len > kLaserMinOffAxisM))
			return false;
		const V3 z = Mul(off_axis, 1.0f / off_len);
		const V3 x = Cross(d, z);
		QuatFromBasis(x, d, z, out_quat);
		Store(centre, out_pos);
		return true;
	}

	void PlacePointer(const float hit[3], const float screen_quat[4], const float eye[3], float out_pos[3],
		float out_quat[4], float* out_size)
	{
		float q[4];
		NormalizeQuat(screen_quat, q);
		const V3 normal = Rotate(q, V3{0.0f, 0.0f, 1.0f});
		const V3 pos = Add(Load(hit), Mul(normal, kPointerLiftM));
		Store(pos, out_pos);
		for (int i = 0; i < 4; i++)
			out_quat[i] = q[i];
		if (out_size)
		{
			const V3 to_eye = Sub(Load(eye), pos);
			const float d = std::sqrt(Dot(to_eye, to_eye));
			const float size = 2.0f * d * std::tan(0.5f * kPointerDeg * (3.14159265f / 180.0f));
			*out_size = (std::isfinite(size) && size > kPointerMinM) ? size : kPointerMinM;
		}
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
		for (const char* p = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,%:-/+!?()'&"; *p; ++p)
			all = all && (FindGlyph(*p) != nullptr);
		check(all, "the font covers A-Z, 0-9, space and . , % : - / + ! ? ( ) ' &");
		check(FindGlyph('q') == FindGlyph('Q') && FindGlyph('q') != nullptr, "lower case draws as capitals");
		check(FindGlyph('~') == nullptr, "characters outside the font are left blank");
		// The apostrophe and the ampersand of item names ("Kevin's 45 Auto", "S&W model 500"): a mark at the top of the
		// cell, and a figure the cell's full height. No character has two glyphs.
		const Glyph* apostrophe = FindGlyph('\'');
		bool apostrophe_high = apostrophe != nullptr && std::strchr(apostrophe->rows[0], '#') != nullptr;
		for (int row = 3; apostrophe && row < kGlyphH; ++row)
			apostrophe_high = apostrophe_high && std::strchr(apostrophe->rows[row], '#') == nullptr;
		check(apostrophe_high, "the apostrophe is a mark at the top of the cell");
		const Glyph* ampersand = FindGlyph('&');
		bool ampersand_tall = ampersand != nullptr;
		for (int row = 0; ampersand && row < kGlyphH; ++row)
			ampersand_tall = ampersand_tall && std::strchr(ampersand->rows[row], '#') != nullptr;
		check(ampersand_tall, "the ampersand fills the cell's height");
		bool glyphs_unique = true;
		for (size_t gi = 0; gi < std::size(kFont); gi++)
		{
			for (size_t gj = gi + 1; gj < std::size(kFont); gj++)
				glyphs_unique = glyphs_unique && kFont[gi].ch != kFont[gj].ch;
		}
		check(glyphs_unique, "no character has two glyphs");
		// Every name ItemData can hand a pouch's toast or the wrist card draws in full.
		bool item_names_drawn = ItemData::Count() > 0;
		for (u32 ii = 0; ii < ItemData::Count(); ii++)
		{
			for (const char* ch = ItemData::At(ii).name; ch && *ch; ++ch)
				item_names_drawn = item_names_drawn && FindGlyph(*ch) != nullptr;
		}
		check(item_names_drawn, "every item name (ItemData) draws with the font");

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
		// It sits nearer than the near field brings the world, so nothing seen through it comes out in front of it in
		// stereo, and farther than the view-locked vignette drawn over it.
		check(-pos[2] == kToastDistanceM && kToastDistanceM < kNearFieldMinM && kToastDistanceM > kVignetteDistanceM,
			"toast nearer than the near field, farther than the vignette");
		// Moved in from 1.2 m with the same angles: about 24 deg wide, 12 deg below the line of sight.
		check(std::abs(kToastWidthM / kToastDistanceM - 0.50f / 1.2f) < 1.0e-4f &&
				  std::abs(-pos[1] / -pos[2] - 0.25f / 1.2f) < 1.0e-4f,
			"toast keeps its angular size and drop");
		// Its corners lie in the vignette's clear middle, so the ring drawn over it never darkens its text.
		const float toast_corner_tan = std::sqrt(std::pow(0.5f * kToastWidthM / kToastDistanceM, 2.0f) +
			std::pow((kToastDropM + 0.5f * kToastHeightM) / kToastDistanceM, 2.0f));
		check(toast_corner_tan < kVignetteRadiusTan * kVignetteClearFrac, "toast lies in the vignette's clear middle");

		// Layer order (PlaceCardLayer), with the cards placed in slot order as the compositor does. With hands: the
		// screen, then beam, dot and toast under the hands, the wrist card, vignette and blink over them.
		{
			constexpr int kScreenLayer = 100;
			constexpr int kHandLayer = 200;
			int order_layers[16] = {kScreenLayer, kHandLayer, kHandLayer + 1};
			u32 order_count = 3;
			u32 order_under = 1;
			for (int card = 0; card < kCardCount; card++)
				PlaceCardLayer(card, order_layers, order_count, order_under, card);
			const int with_hands[] = {kScreenLayer, kLaser, kPointer, kToast, kHandLayer, kHandLayer + 1, kWrist, kVignette, kBlink};
			bool hands_ok = (order_count == std::size(with_hands));
			for (u32 li = 0; hands_ok && li < order_count; li++)
				hands_ok = order_layers[li] == with_hands[li];
			check(hands_ok, "layers: screen, beam, dot, toast, hands, wrist, vignette, blink");

			// Without hands the toast still goes under the wrist card and the vignette, and the dot under the toast.
			int bare_layers[16] = {kScreenLayer};
			u32 bare_count = 1;
			u32 bare_under = 1;
			for (int card = 0; card < kCardCount; card++)
				PlaceCardLayer(card, bare_layers, bare_count, bare_under, card);
			const int no_hands[] = {kScreenLayer, kLaser, kPointer, kToast, kWrist, kVignette, kBlink};
			bool bare_ok = (bare_count == std::size(no_hands));
			for (u32 li = 0; bare_ok && li < bare_count; li++)
				bare_ok = bare_layers[li] == no_hands[li];
			check(bare_ok, "layers without hands: screen, beam, dot, toast, wrist, vignette, blink");

			// The toast alone over the hands' mark stays under them.
			int lone_layers[8] = {kScreenLayer, kHandLayer};
			u32 lone_count = 2;
			u32 lone_under = 1;
			PlaceCardLayer(kToast, lone_layers, lone_count, lone_under, kToast);
			check(lone_count == 3 && lone_layers[1] == kToast && lone_layers[2] == kHandLayer && lone_under == 1,
				"the toast goes in under the hands");
		}

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
		kb = ka;
		kb.has_ammo = true;
		kb.ammo = 7;
		check(WristKey(ka, false) != WristKey(kb, false), "a weapon's rounds appearing changes the key");
		WristData kc = kb;
		kc.ammo = 6;
		check(WristKey(kb, false) != WristKey(kc, false), "a shot (7 -> 6 rounds) changes the key");
		kc.ammo = 7;
		check(WristKey(kb, false) == WristKey(kc, false), "the same rounds keep the key");
		kc.has_ammo = false;
		kc.ammo = 0;
		check(WristKey(ka, false) == WristKey(kc, false), "no weapon is the card without the AMMO line");

		// The ammo line names the equipped weapon (ItemData): in capitals, without its owner, with its rounds, cut to
		// draw at scale 2 or larger; AMMO and the rounds without an id the table knows. The key follows the weapon.
		WristData wl = kb;
		check(WristAmmoLine(wl) == "AMMO 7", "a weapon without an item id shows AMMO and its rounds");
		wl.has_item = true;
		wl.item_id = 400;
		check(WristAmmoLine(wl) == "45 AUTO 7", "Kevin's 45 Auto shows as 45 AUTO 7");
		wl.item_id = 0;
		wl.ammo = 15;
		check(WristAmmoLine(wl) == "HANDGUN 15", "the Handgun shows as HANDGUN 15");
		wl.item_id = 6;
		wl.ammo = 5;
		check(WristAmmoLine(wl) == "S&W MODEL 500 5", "the S&W model 500 keeps its ampersand");
		wl.item_id = 407;
		check(WristAmmoLine(wl) == "FOLDING KNIFE", "a knife shows its name alone, without its owner");
		wl.item_id = 26;
		wl.ammo = 6;
		const std::string wl_cut = WristAmmoLine(wl);
		check(wl_cut == "GRENADE L. (EXTI) 6" &&
				  FitScale(wl_cut.size(), static_cast<int>(kWristWidth) - 2 * kWristPadPx, 3) == 2,
			"a long name is cut to draw at scale 2, its bracket closed");
		wl.item_id = 27;
		wl.ammo = 1;
		check(WristAmmoLine(wl) == "GRENADE L. (BOW) 1", "a cut leaves no space before the closing bracket");
		wl.item_id = 9999;
		check(WristAmmoLine(wl) == "AMMO 1", "an id the table does not have shows AMMO and its rounds");
		bool wl_fits = true;
		for (u32 iw = 0; iw < ItemData::Count(); iw++)
		{
			WristData wl_any = kb;
			wl_any.has_item = true;
			wl_any.item_id = ItemData::At(iw).id;
			wl_any.ammo = 65535;
			wl_fits = wl_fits && WristAmmoLine(wl_any).size() <= kWristLineChars;
		}
		check(wl_fits && kWristLineChars == 19u, "every item's ammo line fits 19 characters, even with 65535 rounds");
		WristData wk_handgun = kb;
		wk_handgun.has_item = true;
		wk_handgun.item_id = 0;
		WristData wk_auto = wk_handgun;
		wk_auto.item_id = 400;
		check(WristKey(kb, false) != WristKey(wk_handgun, false) && WristKey(wk_handgun, false) != WristKey(wk_auto, false),
			"naming the weapon, or another weapon with the same rounds, changes the key");
		WristData wk_odd1 = wk_handgun;
		wk_odd1.item_id = 9998;
		WristData wk_odd2 = wk_handgun;
		wk_odd2.item_id = 9999;
		check(WristKey(wk_odd1, false) == WristKey(wk_odd2, false) && WristKey(wk_odd1, false) == WristKey(kb, false),
			"ids the table does not have draw the same AMMO line, so they keep the key");
		WristData wk_none = wk_auto;
		wk_none.has_ammo = false;
		check(WristKey(wk_none, false) == WristKey(ka, false), "no weapon equipped: no line, whatever id is left over");

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
		std::vector<u32> wimg_plain;
		std::vector<u32> wimg_named;
		RasterWrist(wimg_plain, kb, false);
		RasterWrist(wimg_named, wk_auto, false);
		check(wimg_plain.size() == wimg_named.size() && wimg_plain != wimg_named, "the weapon's name is drawn on the wrist card");

		// Laser: bright just past the muzzle (the image's bottom rows) and gone at the far end (its top row),
		// soft across, red.
		std::vector<u32> limg;
		RasterLaser(limg);
		check(limg.size() == static_cast<size_t>(kLaserImageWidth) * kLaserImageHeight, "laser image size");
		const auto laser_px = [&limg](u32 x, u32 y) {
			const size_t i = static_cast<size_t>(y) * kLaserImageWidth + x;
			return (i < limg.size()) ? limg[i] : 0u;
		};
		constexpr u32 kLaserMidX = kLaserImageWidth / 2;
		constexpr u32 kLaserNearY = kLaserImageHeight - 8;
		const u32 near_a = laser_px(kLaserMidX, kLaserNearY) >> 24;
		const u32 mid_a = laser_px(kLaserMidX, kLaserImageHeight / 2) >> 24;
		const u32 far_a = laser_px(kLaserMidX, 0) >> 24;
		check(near_a > 200u && mid_a < near_a && far_a < mid_a && far_a < 8u, "the laser fades out toward its far end");
		check((laser_px(0, kLaserNearY) >> 24) < near_a / 4u &&
				  (laser_px(kLaserImageWidth - 1, kLaserNearY) >> 24) < near_a / 4u,
			"the laser's edges are soft");
		check((laser_px(0, kLaserNearY) & 0xFFu) == 255u && (laser_px(kLaserMidX, kLaserNearY) & 0xFFu) == 255u,
			"the laser is red");

		// Laser placement: along the aim from just past the pistol's muzzle, turned about the beam to face the
		// eye; looking straight down the beam, or with no usable aim, the frame goes without it.
		float lp[3];
		float lq[4];
		const float eye_above[3] = {0.0f, 1.3f, 0.3f};
		bool laser_placed = PlaceLaser(hand_pos, hand_quat, eye_above, lp, lq);
		V3 beam_dir = Rotate(lq, V3{0.0f, 1.0f, 0.0f});
		V3 beam_face = Rotate(lq, V3{0.0f, 0.0f, 1.0f});
		const float beam_mid_z = kPistolMuzzle[2] - kLaserMuzzleGapM - 0.5f * kLaserLengthM;
		check(laser_placed && beam_dir.z < -0.999f, "the laser quad's +Y runs along the aim");
		check(laser_placed && std::abs(lp[0]) < 1.0e-4f && std::abs(lp[1] - (1.0f + kPistolMuzzle[1])) < 1.0e-4f &&
				  std::abs(lp[2] - beam_mid_z) < 1.0e-3f,
			"the laser runs on from just past the pistol's muzzle");
		check(laser_placed && beam_face.y > 0.999f, "the laser quad turns about the beam to face the eye");
		// Aim turned a quarter left (along -X) and the eye off to its right: still along the aim, facing the eye.
		const float yaw_quat[4] = {0.0f, 0.70710678f, 0.0f, 0.70710678f};
		const float eye_side[3] = {-1.0f, 1.0f + kPistolMuzzle[1], -1.0f};
		laser_placed = PlaceLaser(hand_pos, yaw_quat, eye_side, lp, lq);
		beam_dir = Rotate(lq, V3{0.0f, 1.0f, 0.0f});
		beam_face = Rotate(lq, V3{0.0f, 0.0f, 1.0f});
		check(laser_placed && beam_dir.x < -0.999f && beam_face.z < -0.999f, "a turned laser still faces the eye");
		const float eye_on_axis[3] = {0.0f, 1.0f + kPistolMuzzle[1], 0.5f};
		check(!PlaceLaser(hand_pos, hand_quat, eye_on_axis, lp, lq), "looking straight down the beam skips the frame");
		const float bad_quat[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		check(!PlaceLaser(hand_pos, bad_quat, eye_above, lp, lq), "no beam from an unusable aim");

		// Vignette: clear in the middle and out to 45% of its ring's radius, opaque black from the ring out to the
		// corners, darkening steadily between; straight ahead, covering over 150 degrees.
		std::vector<u32> vimg;
		RasterVignette(vimg);
		check(vimg.size() == static_cast<size_t>(kVignetteImageSize) * kVignetteImageSize, "vignette image size");
		const auto vignette_a = [&vimg](u32 x, u32 y) {
			const size_t i = static_cast<size_t>(y) * kVignetteImageSize + x;
			return (i < vimg.size()) ? (vimg[i] >> 24) : 0u;
		};
		constexpr u32 kVignetteMid = kVignetteImageSize / 2;
		constexpr float kVignetteRingPx = (kVignetteRadiusTan * kVignetteDistanceM) / (0.5f * kVignetteSizeM) *
		                                  (0.5f * static_cast<float>(kVignetteImageSize));
		const u32 vignette_clear_x = kVignetteMid + static_cast<u32>(kVignetteRingPx * kVignetteClearFrac) - 1u;
		const u32 vignette_ring_x = kVignetteMid + static_cast<u32>(kVignetteRingPx) + 1u;
		check(vignette_a(kVignetteMid, kVignetteMid) == 0u && vignette_a(vignette_clear_x, kVignetteMid) == 0u,
			"the vignette is clear in the middle");
		check(vignette_a(vignette_ring_x, kVignetteMid) == 255u && vignette_a(0, 0) == 255u &&
				  vignette_a(kVignetteImageSize - 1, kVignetteImageSize - 1) == 255u,
			"the vignette is opaque from its ring out to the corners");
		bool vignette_ramps = vignette_a(vignette_clear_x + 8u, kVignetteMid) > 0u &&
		                      vignette_a(vignette_clear_x + 8u, kVignetteMid) < 255u;
		for (u32 x = kVignetteMid; x + 1 < kVignetteImageSize; x++)
			vignette_ramps = vignette_ramps && vignette_a(x + 1, kVignetteMid) >= vignette_a(x, kVignetteMid);
		check(vignette_ramps, "the vignette darkens steadily outward");
		check(!vimg.empty() && (vimg[0] & 0x00FFFFFFu) == 0u, "the vignette is black");
		float vpos[3];
		float vquat[4];
		VignettePose(vpos, vquat);
		const float vignette_half_deg = std::atan((0.5f * kVignetteSizeM) / kVignetteDistanceM) * (180.0f / 3.14159265f);
		check(vpos[0] == 0.0f && vpos[1] == 0.0f && vpos[2] < 0.0f && vquat[3] == 1.0f && vignette_half_deg > 75.0f,
			"the vignette covers over 150 degrees straight ahead");

		// Menu pointer's dot: its size, more opaque in the middle than at its edge with clear corners, warm with a ring
		// on a cell.
		std::vector<u32> dot_off;
		std::vector<u32> dot_on;
		RasterPointer(dot_off, false);
		RasterPointer(dot_on, true);
		const size_t dot_px_count = static_cast<size_t>(kPointerImageSize) * kPointerImageSize;
		check(dot_off.size() == dot_px_count && dot_on.size() == dot_px_count, "pointer dot image size");
		const auto dot_px = [](const std::vector<u32>& img, u32 x, u32 y) {
			const size_t i = static_cast<size_t>(y) * kPointerImageSize + x;
			return (i < img.size()) ? img[i] : 0u;
		};
		constexpr u32 kDotMid = kPointerImageSize / 2;
		check((dot_px(dot_off, kDotMid, kDotMid) >> 24) > (dot_px(dot_off, 0, kDotMid) >> 24) &&
				  (dot_px(dot_on, kDotMid, kDotMid) >> 24) > (dot_px(dot_on, 0, kDotMid) >> 24),
			"the pointer dot is more opaque in the middle than at its edge");
		check((dot_px(dot_off, 0, 0) >> 24) == 0u && (dot_px(dot_on, 0, 0) >> 24) == 0u, "the pointer dot's corners are clear");
		check(((dot_px(dot_on, kDotMid, kDotMid) >> 16) & 0xFFu) < ((dot_px(dot_off, kDotMid, kDotMid) >> 16) & 0xFFu),
			"the pointer dot is warmer on a cell");
		check((dot_px(dot_on, kDotMid + 25, kDotMid) >> 24) > 150u && (dot_px(dot_off, kDotMid + 25, kDotMid) & 0xFFu) < 64u,
			"the pointer dot has a ring on a cell and a dark rim off one");

		// Menu pointer's beam: the laser's size, more opaque on its centre line, fading in from the hand and even from
		// there to the screen.
		std::vector<u32> beam_img;
		RasterBeam(beam_img);
		check(beam_img.size() == static_cast<size_t>(kLaserImageWidth) * kLaserImageHeight, "menu beam image size (the laser's)");
		const auto beam_px = [&beam_img](u32 x, u32 y) {
			const size_t i = static_cast<size_t>(y) * kLaserImageWidth + x;
			return (i < beam_img.size()) ? beam_img[i] : 0u;
		};
		const u32 beam_mid_a = beam_px(kLaserMidX, kLaserImageHeight / 2) >> 24;
		check(beam_mid_a > 100u && beam_mid_a > (beam_px(0, kLaserImageHeight / 2) >> 24) * 4u,
			"the menu beam is more opaque on its centre line than at its edges");
		check((beam_px(kLaserMidX, kLaserImageHeight - 1) >> 24) < beam_mid_a / 4u &&
				  (beam_px(kLaserMidX, 0) >> 24) == beam_mid_a,
			"the menu beam fades in from the hand and stays even to the screen");

		// Beam placement: +Y along the beam, +Z turned about it to the eye, centred half its length out; none
		// looking straight down it or with no length.
		const float mb_origin[3] = {0.0f, 1.0f, 0.0f};
		const float mb_dir[3] = {0.0f, 0.0f, -2.0f}; // not unit length on purpose
		const float mb_eye[3] = {0.0f, 1.3f, 0.3f};
		float bmp[3];
		float bmq[4];
		const bool mb_ok = PlaceBeam(mb_origin, mb_dir, 2.0f, mb_eye, bmp, bmq);
		const V3 mb_y = Rotate(bmq, V3{0.0f, 1.0f, 0.0f});
		const V3 mb_z = Rotate(bmq, V3{0.0f, 0.0f, 1.0f});
		check(mb_ok && mb_y.z < -0.999f, "the menu beam quad's +Y runs along its direction");
		check(mb_ok && mb_z.y > 0.999f, "the menu beam quad turns about its length to face the eye");
		check(mb_ok && std::abs(bmp[0]) < 1.0e-4f && std::abs(bmp[1] - 1.0f) < 1.0e-4f && std::abs(bmp[2] + 1.0f) < 1.0e-4f,
			"the menu beam's centre is half its length out");
		const float mb_eye_on_axis[3] = {0.0f, 1.0f, 0.5f};
		check(!PlaceBeam(mb_origin, mb_dir, 2.0f, mb_eye_on_axis, bmp, bmq) &&
				  !PlaceBeam(mb_origin, mb_dir, 0.0f, mb_eye, bmp, bmq),
			"no menu beam looking straight down it or with no length");

		// Dot placement: on the screen's plane just off its face, with the screen's orientation, about 0.6 deg across
		// from the eye and never under 6 mm.
		const float dot_screen_q[4] = {0.0f, 0.38268343f, 0.0f, 0.92387953f}; // turned 45 deg about +Y
		const float dot_hit[3] = {0.5f, 1.2f, -2.0f};
		const V3 dot_normal = Rotate(dot_screen_q, V3{0.0f, 0.0f, 1.0f});
		const auto eye_out = [&](float metres, float out[3]) { Store(Add(Load(dot_hit), Mul(dot_normal, metres)), out); };
		float eye_near[3];
		float eye_far[3];
		float eye_close[3];
		eye_out(1.0f, eye_near);
		eye_out(3.0f, eye_far);
		eye_out(0.2f, eye_close);
		float dpos[3];
		float dquat[4];
		float size_far = 0.0f;
		float size_close = 0.0f;
		float size_near = 0.0f;
		PlacePointer(dot_hit, dot_screen_q, eye_far, dpos, dquat, &size_far);
		PlacePointer(dot_hit, dot_screen_q, eye_close, dpos, dquat, &size_close);
		PlacePointer(dot_hit, dot_screen_q, eye_near, dpos, dquat, &size_near);
		const V3 dot_lift = Sub(Load(dpos), Load(dot_hit));
		const V3 dot_sideways = Sub(dot_lift, Mul(dot_normal, Dot(dot_lift, dot_normal)));
		check(std::abs(Dot(dot_lift, dot_normal) - kPointerLiftM) < 1.0e-5f && Dot(dot_sideways, dot_sideways) < 1.0e-10f,
			"the pointer dot lies on the screen, just off its face");
		bool same_quat = true;
		for (int i = 0; i < 4; i++)
			same_quat = same_quat && std::abs(dquat[i] - dot_screen_q[i]) < 1.0e-5f;
		check(same_quat, "the pointer dot has the screen's orientation");
		const float dot_expect = 2.0f * (1.0f - kPointerLiftM) * std::tan(0.5f * kPointerDeg * (3.14159265f / 180.0f));
		check(size_far > size_near && std::abs(size_near - dot_expect) < 1.0e-4f,
			"the pointer dot grows with distance, about 0.6 deg across");
		check(size_close == kPointerMinM, "the pointer dot is never smaller than 6 mm");

		Console.WriteLn(fail == 0 ? Color_StrongGreen : Color_StrongRed, "(VR) HUD self-test: %d failure(s).", fail);
		return fail == 0;
	}
}
