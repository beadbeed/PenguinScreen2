// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/CameraDriver.h"
#include "VR/HandModel.h"
#include "VR/HeadPose.h"
#include "VR/PadLook.h"
#include "VR/VRManager.h"
#include "VR/SplitState.h"
#include "VR/VRInputState.h"
#include "VR/VRProfileDB.h"

#include "Config.h"
#include "MTGS.h"
#include "Memory.h"
#include "VMManager.h"

#include "common/Console.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <vector>

namespace VR::CameraDriver
{
	namespace
	{
		constexpr float PI_F = 3.14159265358979323846f;

		struct EulerAngles
		{
			float yaw = 0.0f;
			float pitch = 0.0f;
			float roll = 0.0f;
		};

		EulerAngles QuaternionToEulerYXZ(float x, float y, float z, float w)
		{
			const float n2 = x * x + y * y + z * z + w * w;
			if (!std::isfinite(n2) || n2 < 1e-12f)
				return EulerAngles{};
			const float inv = 1.0f / std::sqrt(n2);
			x *= inv;
			y *= inv;
			z *= inv;
			w *= inv;

			EulerAngles e;

			const float sinp = 2.0f * (w * x - y * z);
			if (std::abs(sinp) >= 1.0f)
				e.pitch = std::copysign(PI_F * 0.5f, sinp);
			else
				e.pitch = std::asin(sinp);

			e.yaw = std::atan2(2.0f * (w * y + x * z), 1.0f - 2.0f * (x * x + y * y));

			e.roll = std::atan2(2.0f * (w * z + x * y), 1.0f - 2.0f * (x * x + z * z));

			return e;
		}

		float SelectSource(ProfileDB::CameraSource src, const EulerAngles& e, const HeadPose::Snapshot& pose)
		{
			switch (src)
			{
				case ProfileDB::CameraSource::HeadYaw:   return e.yaw;
				case ProfileDB::CameraSource::HeadPitch: return e.pitch;
				case ProfileDB::CameraSource::HeadRoll:  return e.roll;
				case ProfileDB::CameraSource::HeadX:     return pose.position_x;
				case ProfileDB::CameraSource::HeadY:     return pose.position_y;
				case ProfileDB::CameraSource::HeadZ:     return pose.position_z;
				case ProfileDB::CameraSource::Constant:  return 0.0f;
				default:                                 return 0.0f;
			}
		}

		s32 SaturateRound(float v, double lo, double hi)
		{
			const double d = std::clamp(static_cast<double>(v), lo, hi);
			return static_cast<s32>(std::lround(d));
		}

		void EncodeAndWrite(u32 address, float value, ProfileDB::CameraEncoding enc)
		{
			if (!std::isfinite(value))
				return;

			switch (enc)
			{
				case ProfileDB::CameraEncoding::F32:
				{
					memWrite32(address, std::bit_cast<u32>(value));
					break;
				}
				case ProfileDB::CameraEncoding::S16_12:
				{
					const s32 raw = SaturateRound(value * 4096.0f,
						std::numeric_limits<s16>::min(), std::numeric_limits<s16>::max());
					memWrite16(address, static_cast<u16>(static_cast<s16>(raw)));
					break;
				}
				case ProfileDB::CameraEncoding::S32Angle:
				{
					const s32 raw = SaturateRound(value,
						std::numeric_limits<s32>::min(), std::numeric_limits<s32>::max());
					memWrite32(address, static_cast<u32>(raw));
					break;
				}
				default:
					break;
			}
		}

		std::optional<float> DecodeGuestValue(u32 address, ProfileDB::CameraEncoding enc)
		{
			switch (enc)
			{
				case ProfileDB::CameraEncoding::F32:
				{
					const float v = std::bit_cast<float>(static_cast<u32>(memRead32(address)));
					if (!std::isfinite(v))
						return std::nullopt;
					return v;
				}
				case ProfileDB::CameraEncoding::S16_12:
					return static_cast<float>(static_cast<s16>(static_cast<u16>(memRead16(address)))) * (1.0f / 4096.0f);
				case ProfileDB::CameraEncoding::S32Angle:
					return static_cast<float>(static_cast<s32>(memRead32(address)));
				default:
					return std::nullopt;
			}
		}

		float Wrap360(float v)
		{
			v = std::fmod(v, 360.0f);
			return (v < 0.0f) ? v + 360.0f : v;
		}

		constexpr u32 kMipsNop = 0x00000000u;
		constexpr u32 MipsLui(u32 rt, u32 imm16) { return 0x3C000000u | ((rt & 31u) << 16) | (imm16 & 0xFFFFu); }
		constexpr u32 MipsLwc1(u32 ft, u32 off16, u32 base) { return 0xC4000000u | ((base & 31u) << 21) | ((ft & 31u) << 16) | (off16 & 0xFFFFu); }
		constexpr u32 MipsAddS(u32 fd, u32 fs, u32 ft) { return 0x46000000u | ((ft & 31u) << 16) | ((fs & 31u) << 11) | ((fd & 31u) << 6); }
		constexpr u32 MipsJ(u32 target) { return 0x08000000u | ((target >> 2) & 0x03FFFFFFu); }
		constexpr u32 MipsJal(u32 target) { return 0x0C000000u | ((target >> 2) & 0x03FFFFFFu); }

		struct AssembledHook
		{
			u32 cave[5];
			u32 trampoline;
		};

		AssembledHook AssembleHook(const ProfileDB::CameraCodeHook& h)
		{
			const u32 hi = (h.scratch_address + 0x8000u) >> 16;
			const u32 lo = h.scratch_address & 0xFFFFu;
			AssembledHook a;
			a.cave[0] = MipsLui(h.addr_gpr, hi);
			a.cave[1] = MipsLwc1(h.scratch_fpr, lo, h.addr_gpr);
			a.cave[2] = MipsAddS(h.target_fpr, h.target_fpr, h.scratch_fpr);
			a.cave[3] = MipsJ(h.tail_jump_address);
			a.cave[4] = kMipsNop;
			a.trampoline = MipsJal(h.cave_address);
			return a;
		}

		struct Mat3
		{
			float m[9];
		};

		Mat3 Mat3Mul(const Mat3& a, const Mat3& b)
		{
			Mat3 o{};
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
				{
					float s = 0.0f;
					for (int k = 0; k < 3; k++)
						s += a.m[i * 3 + k] * b.m[k * 3 + j];
					o.m[i * 3 + j] = s;
				}
			return o;
		}

		Mat3 Mat3Transpose(const Mat3& a)
		{
			Mat3 o{};
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					o.m[i * 3 + j] = a.m[j * 3 + i];
			return o;
		}

		Mat3 Mat3FromEulerYXZ(float yaw, float pitch, float roll)
		{
			const float cy = std::cos(yaw), sy = std::sin(yaw);
			const float cx = std::cos(pitch), sx = std::sin(pitch);
			const float cz = std::cos(roll), sz = std::sin(roll);
			const Mat3 Ry{{cy, 0, sy, 0, 1, 0, -sy, 0, cy}};
			const Mat3 Rx{{1, 0, 0, 0, cx, -sx, 0, sx, cx}};
			const Mat3 Rz{{cz, -sz, 0, sz, cz, 0, 0, 0, 1}};
			return Mat3Mul(Mat3Mul(Ry, Rx), Rz);
		}

		std::optional<Mat3> ReadGuestMat3(u32 addr4x4)
		{
			Mat3 g{};
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 3; c++)
				{
					const float v = std::bit_cast<float>(static_cast<u32>(memRead32(addr4x4 + (r * 4 + c) * 4)));
					if (!std::isfinite(v))
						return std::nullopt;
					g.m[r * 3 + c] = v;
				}
			return g;
		}

		void WriteGuestMat3(u32 addr4x4, const Mat3& g)
		{
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 3; c++)
					memWrite32(addr4x4 + (r * 4 + c) * 4, std::bit_cast<u32>(g.m[r * 3 + c]));
		}

		void RunMatrixSelfTest()
		{
			const auto approx = [](float a, float b) { return std::abs(a - b) < 1e-4f; };
			const auto mat_eq = [&](const Mat3& A, const Mat3& B) {
				for (int i = 0; i < 9; i++)
					if (!approx(A.m[i], B.m[i]))
						return false;
				return true;
			};
			const Mat3 I{{1, 0, 0, 0, 1, 0, 0, 0, 1}};
			int pass = 0, fail = 0;
			const auto check = [&](bool ok, const char* name) {
				if (ok)
					pass++;
				else
				{
					fail++;
					Console.WriteLn("(VR) CameraDriver self-test FAIL: %s", name);
				}
			};

			const Mat3 R = Mat3FromEulerYXZ(0.3f, -0.2f, 0.1f);
			check(mat_eq(Mat3FromEulerYXZ(0, 0, 0), I), "euler(0,0,0) == identity");
			check(mat_eq(Mat3Mul(I, R), R) && mat_eq(Mat3Mul(R, I), R), "identity multiply");
			check(mat_eq(Mat3Mul(R, Mat3Transpose(R)), I), "R * transpose(R) == I (orthonormal)");
			check(mat_eq(Mat3Transpose(Mat3Transpose(R)), R), "transpose involution");
			const Mat3 Y90 = Mat3FromEulerYXZ(PI_F * 0.5f, 0, 0);
			check(approx(Y90.m[0], 0) && approx(Y90.m[2], 1) && approx(Y90.m[6], -1) && approx(Y90.m[8], 0),
				"yaw 90° matrix");
			const Mat3 h = Mat3FromEulerYXZ(0.2f, 0, 0);
			check(mat_eq(Mat3Mul(h, h), Mat3FromEulerYXZ(0.4f, 0, 0)), "yaw half+half == full");
			const Mat3 P90 = Mat3FromEulerYXZ(0, PI_F * 0.5f, 0);
			check(approx(P90.m[4], 0) && approx(P90.m[5], -1) && approx(P90.m[7], 1) && approx(P90.m[8], 0),
				"pitch 90° matrix");

			Console.WriteLn("(VR) CameraDriver matrix self-test: %d passed, %d failed.", pass, fail);
		}

		int CountAssemblerMismatches(bool log)
		{
			int fail = 0;
			const auto eq = [&](u32 got, u32 want, const char* name) {
				if (got != want)
				{
					fail++;
					if (log)
						Console.WriteLn("(VR) CameraDriver hook self-test FAIL: %s got 0x%08X want 0x%08X", name, got, want);
				}
			};

			eq(MipsAddS(13, 31, 13), 0x460DFB40u, "add.s f13,f31,f13 (community-verified)");
			eq(MipsJ(0x0036CC40u), 0x080DB310u, "j 0x0036CC40");
			eq(MipsJal(0x000F1100u), 0x0C03C440u, "jal 0x000F1100");
			eq(MipsLui(1, 0x000Fu), 0x3C01000Fu, "lui $1,0x000F");

			ProfileDB::CameraCodeHook hb;
			hb.scratch_address = 0x0010A800u;
			const AssembledHook ab = AssembleHook(hb);
			eq(ab.cave[0], MipsLui(1, 0x0011u), "hi carry: lui hi==0x0011");
			eq(ab.cave[1] & 0xFFFFu, 0xA800u, "lo half preserved 0xA800");

			ProfileDB::CameraCodeHook h;
			h.hook_address = 0x003456D0u;
			h.cave_address = 0x000F1100u;
			h.scratch_address = 0x000F1120u;
			h.tail_jump_address = 0x0036CC40u;
			h.target_fpr = 13;
			h.scratch_fpr = 1;
			h.addr_gpr = 1;
			const AssembledHook a = AssembleHook(h);
			eq(a.cave[0], 0x3C01000Fu, "GT4 cave[0] lui  $1,0x000F");
			eq(a.cave[1], 0xC4211120u, "GT4 cave[1] lwc1 $f1,0x1120($1)");
			eq(a.cave[2], 0x46016B40u, "GT4 cave[2] add.s f13,f13,f1");
			eq(a.cave[3], 0x080DB310u, "GT4 cave[3] j 0x0036CC40");
			eq(a.cave[4], 0x00000000u, "GT4 cave[4] nop");
			eq(a.trampoline, 0x0C03C440u, "GT4 trampoline jal 0x000F1100");
			return fail;
		}

		void RunHookSelfTest()
		{
			const int fail = CountAssemblerMismatches(true);
			Console.WriteLn("(VR) CameraDriver hook self-test: %d mismatch(es).", fail);
		}

		int CountMathMismatches(bool log)
		{
			int fail = 0;
			const auto check = [&](bool ok, const char* name) {
				if (!ok) { ++fail; if (log) Console.WriteLn("(VR) math self-test FAIL: %s", name); }
			};
			const auto approxEq = [](float a, float b) { return std::abs(a - b) < 1e-3f; };
			constexpr float H = 0.5235988f;
			const float sh = std::sin(H * 0.5f), ch = std::cos(H * 0.5f);

			const EulerAngles ident = QuaternionToEulerYXZ(0, 0, 0, 1);
			check(approxEq(ident.yaw, 0) && approxEq(ident.pitch, 0) && approxEq(ident.roll, 0), "identity quat -> 0 Euler");

			const EulerAngles ry = QuaternionToEulerYXZ(0, sh, 0, ch);
			check(approxEq(ry.yaw, H) && approxEq(ry.pitch, 0) && approxEq(ry.roll, 0), "+Y30 -> +yaw only");
			const EulerAngles rx = QuaternionToEulerYXZ(sh, 0, 0, ch);
			check(approxEq(rx.pitch, H) && approxEq(rx.yaw, 0) && approxEq(rx.roll, 0), "+X30 -> +pitch only");
			const EulerAngles rz = QuaternionToEulerYXZ(0, 0, sh, ch);
			check(approxEq(rz.roll, H) && approxEq(rz.yaw, 0) && approxEq(rz.pitch, 0), "+Z30 -> +roll only");
			check(approxEq(QuaternionToEulerYXZ(0, -sh, 0, ch).yaw, -H), "-Y30 -> -yaw (sign)");

			const EulerAngles zero = QuaternionToEulerYXZ(0, 0, 0, 0);
			check(approxEq(zero.yaw, 0) && approxEq(zero.pitch, 0) && approxEq(zero.roll, 0), "zero quat -> identity");
			check(approxEq(QuaternionToEulerYXZ(0, 2 * sh, 0, 2 * ch).yaw, H), "non-unit quat normalized");
			check(std::abs(QuaternionToEulerYXZ(0.7071f, 0, 0, 0.7071f).pitch) <= PI_F * 0.5f + 1e-3f, "pitch gimbal-clamped");

			check(approxEq(Wrap360(30.0f), 30.0f), "wrap 30");
			check(approxEq(Wrap360(390.0f), 30.0f), "wrap 390 -> 30");
			check(approxEq(Wrap360(-30.0f), 330.0f), "wrap -30 -> 330");
			check(approxEq(Wrap360(360.0f), 0.0f), "wrap 360 -> 0");

			const Mat3 R = Mat3FromEulerYXZ(0.3f, -0.2f, 0.1f);
			const Mat3 RRt = Mat3Mul(R, Mat3Transpose(R));
			bool ortho = true;
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c)
					ortho = ortho && approxEq(RRt.m[r * 3 + c], (r == c) ? 1.0f : 0.0f);
			check(ortho, "R * transpose(R) == I");

			return fail;
		}

		void MaybeRunSelfTest()
		{
			static const bool enabled = (std::getenv("PCSX2_VR_SELFTEST") != nullptr);
			static bool ran = false;
			if (!enabled || ran)
				return;
			ran = true;
			RunMatrixSelfTest();
			RunHookSelfTest();
		}

		u32 s_delta_crc = 0;
		std::vector<float> s_delta_prev;
		std::vector<bool> s_delta_has;

		struct AnchorState
		{
			float base = 0.0f;
			float last_written = 0.0f;
			float head_ref = 0.0f;
			float clamp_min = -std::numeric_limits<float>::infinity();
			float clamp_max = std::numeric_limits<float>::infinity();
			bool wrap360 = false;
			u32 addr = 0;
			ProfileDB::CameraEncoding encoding = ProfileDB::CameraEncoding::F32;
			bool anchored = false;
			bool has = false;
		};
		std::vector<AnchorState> s_anchor;

		void RestoreAnchoredScalar(AnchorState& a)
		{
			if (!a.has || !a.anchored)
			{
				a.has = false;
				return;
			}
			a.has = false;
			const std::optional<float> guest = DecodeGuestValue(a.addr, a.encoding);
			if (guest.has_value() && guest.value() == a.last_written)
			{
				float orig = a.base + a.head_ref;
				if (a.wrap360)
					orig = Wrap360(orig);
				orig = std::clamp(orig, a.clamp_min, a.clamp_max);
				EncodeAndWrite(a.addr, orig, a.encoding);
			}
			else
				DevCon.WriteLn("(VR) CameraDriver: anchored op at 0x%08X not restored — guest value is no longer ours (game wrote it).", a.addr);
		}

		struct MatBaseline
		{
			float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
			bool has = false;
		};
		u32 s_mat_crc = 0;
		std::vector<MatBaseline> s_mat_prev;

		struct MatAnchor
		{
			float base[9] = {};
			float written[9] = {};
			u32 addr = 0;
			bool has = false;
		};
		std::vector<MatAnchor> s_mat_anchor;

		void ResetDeltaState(bool keep_anchors = false)
		{
			std::fill(s_delta_has.begin(), s_delta_has.end(), false);
			for (MatBaseline& b : s_mat_prev)
				b.has = false;
			if (keep_anchors)
				return;
			for (MatAnchor& m : s_mat_anchor)
				m.has = false;
			for (AnchorState& a : s_anchor)
				a.has = false;
		}

		void EvaluateAndWrite(const ProfileDB::CameraWriteOp& op, size_t op_index, u32 address,
			const EulerAngles& e, const HeadPose::Snapshot& pose)
		{
			const float src = SelectSource(op.source, e, pose);
			float v;
			if (op.compose == ProfileDB::CameraCompose::Anchored)
			{
				if (op_index >= s_anchor.size())
					return;
				AnchorState& a = s_anchor[op_index];
				if (a.has && a.addr != address)
					RestoreAnchoredScalar(a);
				const std::optional<float> game = DecodeGuestValue(address, op.encoding);
				if (!game.has_value())
					return;
				const float head_term = src * op.axis_sign * op.scale;
				if (!a.has)
				{
					a.head_ref = op.anchor_head ? head_term : 0.0f;
					a.base = game.value() - a.head_ref;
					a.has = true;
				}
				else
				{
					float own = game.value() - a.last_written;
					if (op.wrap == ProfileDB::CameraWrap::Deg360)
						own = std::remainder(own, 360.0f);
					a.base += own;
				}
				v = a.base + head_term;
				if (op.wrap == ProfileDB::CameraWrap::Deg360)
				{
					v = Wrap360(v);
					a.base = std::remainder(a.base, 360.0f);
				}
				v = std::clamp(v, op.clamp_min, op.clamp_max);
				EncodeAndWrite(address, v, op.encoding);
				a.last_written = DecodeGuestValue(address, op.encoding).value_or(v);
				a.addr = address;
				a.encoding = op.encoding;
				a.clamp_min = op.clamp_min;
				a.clamp_max = op.clamp_max;
				a.wrap360 = (op.wrap == ProfileDB::CameraWrap::Deg360);
				a.anchored = true;
				return;
			}
			if (op.compose == ProfileDB::CameraCompose::Delta)
			{
				if (!s_delta_has[op_index])
				{
					s_delta_prev[op_index] = src;
					s_delta_has[op_index] = true;
					return;
				}
				float d = src - s_delta_prev[op_index];
				s_delta_prev[op_index] = src;
				if (op.source == ProfileDB::CameraSource::HeadYaw ||
					op.source == ProfileDB::CameraSource::HeadPitch ||
					op.source == ProfileDB::CameraSource::HeadRoll)
				{
					d = std::remainder(d, 2.0f * PI_F);
				}
				const std::optional<float> game = DecodeGuestValue(address, op.encoding);
				if (!game.has_value())
					return;
				v = game.value() + d * op.axis_sign * op.scale;
			}
			else
			{
				v = src * op.axis_sign * op.scale + op.bias;
			}
			if (op.wrap == ProfileDB::CameraWrap::Deg360)
				v = Wrap360(v);
			v = std::clamp(v, op.clamp_min, op.clamp_max);
			EncodeAndWrite(address, v, op.encoding);
		}

		void ApplyMatrixOp(const ProfileDB::CameraMatrixOp& op, size_t op_index, u32 address,
			u32 transpose_address, const EulerAngles& e)
		{
			if (op.anchored)
			{
				const std::optional<Mat3> guest = ReadGuestMat3(address);
				if (!guest.has_value())
					return;

				MatAnchor& an = s_mat_anchor[op_index];
				Mat3 base = guest.value();
				if (an.has)
				{
					bool is_ours = true;
					for (int i = 0; i < 9 && is_ours; i++)
						is_ours = std::fabs(guest.value().m[i] - an.written[i]) < 1e-5f;
					if (is_ours)
						std::memcpy(base.m, an.base, sizeof(base.m));
				}

				const Mat3 r_off = Mat3FromEulerYXZ(e.yaw * op.axis_sign_yaw,
					e.pitch * op.axis_sign_pitch, e.roll * op.axis_sign_roll);
				const Mat3 r_new = (op.order == ProfileDB::MatrixComposeOrder::Pre)
									   ? Mat3Mul(r_off, base)
									   : Mat3Mul(base, r_off);
				WriteGuestMat3(address, r_new);
				if (op.also_transpose)
					WriteGuestMat3(transpose_address, Mat3Transpose(r_new));
				std::memcpy(s_mat_anchor[op_index].base, base.m, sizeof(base.m));
				std::memcpy(s_mat_anchor[op_index].written, r_new.m, sizeof(r_new.m));
				s_mat_anchor[op_index].addr = address;
				s_mat_anchor[op_index].has = true;
				return;
			}

			MatBaseline& prev = s_mat_prev[op_index];
			if (!prev.has)
			{
				prev = MatBaseline{e.yaw, e.pitch, e.roll, true};
				return;
			}
			const float dyaw = std::remainder(e.yaw - prev.yaw, 2.0f * PI_F) * op.axis_sign_yaw;
			const float dpitch = std::remainder(e.pitch - prev.pitch, 2.0f * PI_F) * op.axis_sign_pitch;
			const float droll = std::remainder(e.roll - prev.roll, 2.0f * PI_F) * op.axis_sign_roll;
			prev = MatBaseline{e.yaw, e.pitch, e.roll, true};

			const std::optional<Mat3> game = ReadGuestMat3(address);
			if (!game.has_value())
				return;

			const Mat3 r_delta = Mat3FromEulerYXZ(dyaw, dpitch, droll);
			const Mat3 r_new = (op.order == ProfileDB::MatrixComposeOrder::Pre)
								   ? Mat3Mul(r_delta, game.value())
								   : Mat3Mul(game.value(), r_delta);
			WriteGuestMat3(address, r_new);
			if (op.also_transpose)
				WriteGuestMat3(transpose_address, Mat3Transpose(r_new));
		}

		struct ResolvedBase
		{
			u32 crc = 0;
			u32 base = 0;
			bool valid = false;
			u64 next_retry_vsync = 0;
		};
		ResolvedBase s_base;
		u64 s_vsync_counter = 0;
		u32 s_base_unresolved_vsyncs = 0;

		std::optional<u32> ScanForBase(const ProfileDB::CameraBase& spec)
		{
			if (!eeMem)
				return std::nullopt;

			const u32 ram_size = Ps2MemSize::MainRam;
			const u32 lo = std::min(spec.scan_start, ram_size);
			const u32 hi = std::min(spec.scan_end, ram_size);
			const size_t n = spec.pattern.size();
			if (n == 0 || lo + n >= hi)
				return std::nullopt;

			const u8* const ram = reinterpret_cast<const u8*>(eeMem->Main);
			const u8* const pat = spec.pattern.data();
			const u8* const msk = spec.mask.data();

			std::optional<u32> last_good;
			for (u32 at = lo; at + n <= hi; at++)
			{
				if (msk[0] && ram[at] != pat[0])
					continue;

				bool match = true;
				for (size_t i = 1; i < n; i++)
				{
					if (msk[i] && ram[at + i] != pat[i])
					{
						match = false;
						break;
					}
				}
				if (!match)
					continue;

				if (spec.has_validate)
				{
					const s64 vaddr = static_cast<s64>(at) + spec.validate_offset;
					if (vaddr < 0 || vaddr + 4 > static_cast<s64>(ram_size))
						continue;
					u32 dword;
					std::memcpy(&dword, ram + vaddr, 4);
					if (dword != spec.validate_equals)
						continue;
				}

				last_good = at;
			}

			if (!last_good.has_value())
				return std::nullopt;

			const s64 base = static_cast<s64>(last_good.value()) + spec.base_offset;
			if (base < 0 || base >= static_cast<s64>(ram_size))
				return std::nullopt;
			return static_cast<u32>(base);
		}

		std::optional<u32> WalkChain(u32 root, const std::vector<s32>& hops)
		{
			u32 deref = memRead32(root);
			if (deref == 0 || deref >= Ps2MemSize::MainRam || (deref & 3u) != 0)
				return std::nullopt;
			for (const s32 hop : hops)
			{
				const s64 slot = static_cast<s64>(deref) + hop;
				if (slot < 0 || slot + 4 > static_cast<s64>(Ps2MemSize::MainRam))
					return std::nullopt;
				deref = memRead32(static_cast<u32>(slot));
				if (deref == 0 || deref >= Ps2MemSize::MainRam || (deref & 3u) != 0)
					return std::nullopt;
			}
			return deref;
		}

		std::optional<u32> GetBase(const ProfileDB::CameraProfile& cam, u32 crc)
		{
			if (!cam.base.has_value())
				return std::nullopt;

			if (cam.base->is_indexed)
			{
				const ProfileDB::CameraBase& spec = cam.base.value();
				u32 idx = 0;
				switch (spec.index_width)
				{
					case 1: idx = memRead8(spec.index_addr); break;
					case 2: idx = memRead16(spec.index_addr); break;
					default: idx = memRead32(spec.index_addr); break;
				}
				const s64 resolved = static_cast<s64>(spec.indexed_base) + spec.array_offset +
									 static_cast<s64>(idx) * static_cast<s64>(spec.stride);
				if (resolved < 0 || resolved >= static_cast<s64>(Ps2MemSize::MainRam))
				{
					s_base.valid = false;
					return std::nullopt;
				}
				const u32 base = static_cast<u32>(resolved);
				if (!s_base.valid || s_base.crc != crc || s_base.base != base)
				{
					s_base = ResolvedBase{crc, base, true, 0};
					DevCon.WriteLn("(VR) CameraDriver: camera base resolved at 0x%08X (index %u).", base, idx);
				}
				return base;
			}

			if (cam.base->is_pointer)
			{
				const ProfileDB::CameraBase& spec = cam.base.value();
				const std::optional<u32> obj = WalkChain(spec.pointer_addr, spec.deref_offsets);
				if (!obj.has_value())
				{
					s_base.valid = false;
					return std::nullopt;
				}
				u32 deref = obj.value();
				if (spec.has_validate)
				{
					const s64 vaddr = static_cast<s64>(deref) + spec.validate_offset;
					if (vaddr < 0 || vaddr + 4 > static_cast<s64>(Ps2MemSize::MainRam) ||
						memRead32(static_cast<u32>(vaddr)) != spec.validate_equals)
					{
						s_base.valid = false;
						return std::nullopt;
					}
				}
				const s64 resolved = static_cast<s64>(deref) + spec.base_offset;
				if (resolved < 0 || resolved >= static_cast<s64>(Ps2MemSize::MainRam))
				{
					s_base.valid = false;
					return std::nullopt;
				}
				const u32 base = static_cast<u32>(resolved);
				if (!s_base.valid || s_base.crc != crc || s_base.base != base)
				{
					s_base = ResolvedBase{crc, base, true, 0};
					Console.WriteLn("(VR) CameraDriver: camera struct base resolved at 0x%08X (pointer deref, %zu extra hop(s)).",
						base, spec.deref_offsets.size());
				}
				return base;
			}

			if (s_base.valid && s_base.crc == crc)
			{
				if (cam.base->has_validate)
				{
					const s64 vaddr = static_cast<s64>(s_base.base) - cam.base->base_offset + cam.base->validate_offset;
					if (vaddr < 0 || memRead32(static_cast<u32>(vaddr)) != cam.base->validate_equals)
					{
						s_base.valid = false;
						s_base.next_retry_vsync = 0;
					}
				}
				if (s_base.valid)
					return s_base.base;
			}

			if (s_vsync_counter < s_base.next_retry_vsync)
				return std::nullopt;

			const std::optional<u32> found = ScanForBase(cam.base.value());
			if (found.has_value())
			{
				const bool was_valid = (s_base.valid && s_base.crc == crc && s_base.base == found.value());
				s_base = ResolvedBase{crc, found.value(), true, 0};
				if (!was_valid)
					Console.WriteLn("(VR) CameraDriver: camera struct base resolved at 0x%08X.", found.value());
				return s_base.base;
			}

			s_base = ResolvedBase{crc, 0, false, s_vsync_counter + 60};
			return std::nullopt;
		}

		bool GuardListPass(const std::vector<ProfileDB::CameraGuard>& guards)
		{
			for (const ProfileDB::CameraGuard& g : guards)
			{
				u32 address = g.ee_address;
				if (g.is_chain)
				{
					const std::optional<u32> obj = WalkChain(g.pointer_addr, g.deref_offsets);
					if (!obj.has_value())
						return false;
					const s64 a = static_cast<s64>(obj.value()) + g.offset;
					if (a < 0 || a + g.width > static_cast<s64>(Ps2MemSize::MainRam))
						return false;
					address = static_cast<u32>(a);
				}
				u32 v = 0;
				switch (g.width)
				{
					case 1: v = memRead8(address); break;
					case 2: v = memRead16(address); break;
					default: v = memRead32(address); break;
				}
				if ((v == g.equals) == g.not_equals)
					return false;
			}
			return true;
		}

		bool GuardsPass(const ProfileDB::CameraProfile& cam)
		{
			return GuardListPass(cam.guards);
		}

		bool s_silence_applied = false;
		u32 s_silence_crc = 0;
		std::vector<u8> s_silence_patched; // per entry: the word holds value_on because of us
		bool s_silence_repair = false;     // one-shot after a savestate load: undo patches saved in the state

		// Silence patches often sit in code the game reloads from disc (overlays), so they are checked
		// every armed vsync rather than written once: a word still holding the game's original value
		// is patched again, and a word holding neither value (other code is resident) is left alone.
		// An entry with a `when` guard is only touched while that guard passes. Only words we patched
		// are ever restored, except for a one-shot repair of guarded entries after a savestate load.
		void ApplySilence(const ProfileDB::CameraProfile& cam, u32 crc)
		{
			if (s_silence_crc != crc || s_silence_patched.size() != cam.silence.size())
				s_silence_patched.assign(cam.silence.size(), 0);
			size_t patched = 0;
			for (size_t i = 0; i < cam.silence.size(); i++)
			{
				const ProfileDB::CameraSilence& sil = cam.silence[i];
				if (!sil.when.empty() && !GuardListPass(sil.when))
				{
					s_silence_patched[i] = 0; // the code holding it is gone; a reload brings the original back
					continue;
				}
				if (sil.value_on == sil.value_off)
					continue;
				const u32 cur = static_cast<u32>(memRead32(sil.ee_address));
				if (cur == sil.value_off)
				{
					memWrite32(sil.ee_address, sil.value_on);
					s_silence_patched[i] = 1;
					patched++;
				}
				else if (cur == sil.value_on)
					s_silence_patched[i] = 1;
			}
			if (patched > 0)
				DevCon.WriteLn("(VR) CameraDriver: game camera writer silenced (%zu patch(es)).", patched);
			s_silence_applied = true;
			s_silence_crc = crc;
			s_silence_repair = false;
		}

		void RestoreSilence(const ProfileDB::CameraProfile& cam, u32 crc)
		{
			if (s_silence_crc != crc || s_silence_patched.size() != cam.silence.size())
				s_silence_patched.assign(cam.silence.size(), 0);
			size_t restored = 0;
			bool repair_pending = false;
			for (size_t i = 0; i < cam.silence.size(); i++)
			{
				const ProfileDB::CameraSilence& sil = cam.silence[i];
				const bool repair = s_silence_repair && !sil.when.empty();
				if (!s_silence_patched[i] && !repair)
					continue;
				if (!sil.when.empty() && !GuardListPass(sil.when))
				{
					if (repair)
						repair_pending = true; // check again once its code is resident
					s_silence_patched[i] = 0;
					continue;
				}
				if (sil.value_on != sil.value_off && static_cast<u32>(memRead32(sil.ee_address)) == sil.value_on)
				{
					memWrite32(sil.ee_address, sil.value_off);
					restored++;
				}
				s_silence_patched[i] = 0;
			}
			if (restored > 0)
				DevCon.WriteLn("(VR) CameraDriver: game camera writer restored (%zu patch(es)).", restored);
			s_silence_applied = false;
			s_silence_crc = crc;
			s_silence_repair = repair_pending;
		}

		bool s_hooks_installed = false;
		u32 s_hooks_crc = 0;
		std::vector<u32> s_hook_original;

		void ApplyCodeHooks(const ProfileDB::CameraProfile& cam, u32 crc)
		{
			if (s_hooks_installed && s_hooks_crc == crc)
			{
				bool intact = true;
				for (size_t i = 0; i < cam.code_hooks.size() && i < s_hook_original.size(); i++)
				{
					if (s_hook_original[i] == 0)
						continue;
					if (static_cast<u32>(memRead32(cam.code_hooks[i].hook_address)) != AssembleHook(cam.code_hooks[i]).trampoline)
					{
						intact = false;
						break;
					}
				}
				if (intact)
					return;
				s_hooks_installed = false;
			}
			s_hook_original.assign(cam.code_hooks.size(), 0);
			size_t installed = 0;
			for (size_t i = 0; i < cam.code_hooks.size(); i++)
			{
				const ProfileDB::CameraCodeHook& h = cam.code_hooks[i];
				if (!h.enabled)
					continue;
				const u32 orig = static_cast<u32>(memRead32(h.hook_address));
				if ((orig >> 26) != 0x03u)
				{
					DevCon.WriteLn("(VR) CameraDriver: code-hook site 0x%08X is not a JAL (0x%08X); skipping install.",
						h.hook_address, orig);
					continue;
				}
				const AssembledHook a = AssembleHook(h);
				for (int w = 0; w < 5; w++)
					memWrite32(h.cave_address + static_cast<u32>(w) * 4u, a.cave[w]);
				memWrite32(h.scratch_address, 0);
				memWrite32(h.hook_address, a.trampoline);
				s_hook_original[i] = orig;
				installed++;
			}
			if (installed > 0)
				DevCon.WriteLn("(VR) CameraDriver: %zu camera code-hook(s) installed.", installed);
			s_hooks_installed = true;
			s_hooks_crc = crc;
		}

		void RestoreCodeHooks(const ProfileDB::CameraProfile& cam, u32 crc)
		{
			if (!s_hooks_installed || s_hooks_crc != crc)
			{
				s_hooks_installed = false;
				return;
			}
			size_t restored = 0;
			for (size_t i = 0; i < cam.code_hooks.size() && i < s_hook_original.size(); i++)
			{
				if (s_hook_original[i] == 0)
					continue;
				memWrite32(cam.code_hooks[i].hook_address, s_hook_original[i]);
				s_hook_original[i] = 0;
				restored++;
			}
			if (restored > 0)
				DevCon.WriteLn("(VR) CameraDriver: %zu camera code-hook(s) restored.", restored);
			s_hooks_installed = false;
		}

		void WriteCodeHookScratch(const ProfileDB::CameraProfile& cam, const EulerAngles& e,
			const HeadPose::Snapshot& pose)
		{
			if (!s_hooks_installed)
				return;
			for (size_t i = 0; i < cam.code_hooks.size() && i < s_hook_original.size(); i++)
			{
				if (s_hook_original[i] == 0)
					continue;
				const ProfileDB::CameraCodeHook& h = cam.code_hooks[i];
				const float v = SelectSource(h.source, e, pose) * h.axis_sign * h.scale;
				EncodeAndWrite(h.scratch_address, v, ProfileDB::CameraEncoding::F32);
			}
		}

		PadLookState s_padlook{};

		bool s_armed_logged = false;
		u32 s_guard_fail_vsyncs = 0;

		std::atomic_bool s_recenter_requested{false};
		bool s_has_reference = false;
		u32 s_reference_crc = 0;
		float s_ref_x = 0.0f, s_ref_y = 0.0f, s_ref_z = 0.0f, s_ref_w = 1.0f;
		// Head position and yaw at the last recenter, for camera.lookAt head translation.
		float s_ref_px = 0.0f, s_ref_py = 0.0f, s_ref_pz = 0.0f, s_ref_yaw = 0.0f;
		bool s_ref_pos_valid = false;
		float s_last_dx = 0.0f, s_last_dy = 0.0f, s_last_dz = 0.0f; // held while position tracking is lost

		// camera.lookAt yaw anchor (game yaw, radians); retaken from the heading on arm and recenter.
		bool s_yaw_anchor_valid = false;
		float s_yaw_anchor = 0.0f;
		bool s_snap_ready = false;
		std::chrono::steady_clock::time_point s_turn_last{};

		float WrapPi(float a)
		{
			a = std::fmod(a + PI_F, 2.0f * PI_F);
			if (a < 0.0f)
				a += 2.0f * PI_F;
			return a - PI_F;
		}

		u16 ToBinaryAngle(float radians)
		{
			return static_cast<u16>(static_cast<s32>(std::lround(WrapPi(radians) * (65536.0f / (2.0f * PI_F)))) & 0xFFFF);
		}

		void QuatMultiply(float ax, float ay, float az, float aw, float bx, float by, float bz, float bw,
			float& ox, float& oy, float& oz, float& ow)
		{
			ox = aw * bx + ax * bw + ay * bz - az * by;
			oy = aw * by - ax * bz + ay * bw + az * bx;
			oz = aw * bz + ax * by - ay * bx + az * bw;
			ow = aw * bw - ax * bx - ay * by - az * bz;
		}

		void ApplyReference(float& x, float& y, float& z, float& w)
		{
			if (!s_has_reference)
				return;
			float ox, oy, oz, ow;
			QuatMultiply(-s_ref_x, -s_ref_y, -s_ref_z, s_ref_w, x, y, z, w, ox, oy, oz, ow);
			x = ox;
			y = oy;
			z = oz;
			w = ow;
		}

		// Seconds since the previous call, for smooth turning; 0 after a gap (pause, disarm, first call).
		float TurnDt()
		{
			const auto now = std::chrono::steady_clock::now();
			const float dt = std::chrono::duration<float>(now - s_turn_last).count();
			s_turn_last = now;
			return (dt > 0.0f && dt < 0.1f) ? dt : 0.0f;
		}

		u32 ReadSized(u32 address, u8 width)
		{
			switch (width)
			{
				case 1: return memRead8(address);
				case 2: return memRead16(address);
				default: return memRead32(address);
			}
		}

		void WriteSized(u32 address, u8 width, u32 value)
		{
			switch (width)
			{
				case 1: memWrite8(address, static_cast<u8>(value)); break;
				case 2: memWrite16(address, static_cast<u16>(value)); break;
				default: memWrite32(address, value); break;
			}
		}

		bool RecordChecksPass(const std::vector<ProfileDB::CameraLookAt::RecordCheck>& checks, u32 base)
		{
			for (const ProfileDB::CameraLookAt::RecordCheck& c : checks)
			{
				const s64 a = static_cast<s64>(base) + c.offset;
				if (a < 0 || a + c.width > static_cast<s64>(Ps2MemSize::MainRam) || (a % c.width) != 0)
					return false;
				if ((ReadSized(static_cast<u32>(a), c.width) == c.equals) == c.not_equals)
					return false;
			}
			return true;
		}

		// What camera.lookAt wrote this vsync, for matching against the game's view matrix later.
		struct LookAtWrite
		{
			float dir[3] = {0.0f, 0.0f, 1.0f};
			float eye[3] = {0.0f, 0.0f, 0.0f};
			float base_yaw = 0.0f; // game yaw the head yaw was added to (the anchor, or the heading)
			float yaw_sign = 1.0f;
		};

		// The base yaw camera.lookAt is using right now, for the compositor: a frame rendered at another
		// base yaw (stick turning since) is turned by the difference so artificial turns look as smooth as
		// head turns instead of stepping at the game's frame rate.
		std::atomic<float> s_base_yaw_now{0.0f};

		// camera.lookAt: put the game's eye/target camera at the character's head. Game yaw 0 faces
		// +Z, forward = (sin y, cos y) on the x/z floor plane, y up. Head yaw (OpenXR, CCW positive) is
		// added times yawSign (-1 for a game whose yaw grows clockwise), head pitch tilts the target,
		// and head translation since the last recenter moves the eye (forward/right/up, scaled to game
		// units). The base yaw is the character's heading, or with yawAnchor the camera's own yaw.
		bool ApplyLookAt(const ProfileDB::CameraLookAt& la, const std::optional<u32>& base, const EulerAngles& e,
			const HeadPose::Snapshot& pose, LookAtWrite* out)
		{
			if (!base.has_value() || (!la.when.empty() && !GuardListPass(la.when)))
				return false;
			const s64 pa = static_cast<s64>(base.value()) + la.position_offset;
			if (pa < 0 || pa + 12 > static_cast<s64>(Ps2MemSize::MainRam))
				return false;
			float p[3];
			for (int i = 0; i < 3; i++)
			{
				p[i] = std::bit_cast<float>(static_cast<u32>(memRead32(static_cast<u32>(pa) + i * 4)));
				if (!std::isfinite(p[i]))
					return false;
			}
			float heading = 0.0f;
			u32 heading_addr = 0;
			if (la.has_heading)
			{
				const s64 ha = static_cast<s64>(base.value()) + la.heading_offset;
				if (ha < 0 || ha + 2 > static_cast<s64>(Ps2MemSize::MainRam))
					return false;
				heading_addr = static_cast<u32>(ha);
				heading = static_cast<float>(static_cast<s16>(static_cast<u16>(memRead16(heading_addr)))) *
				          (2.0f * PI_F / 65536.0f);
			}

			float base_yaw = heading;
			VRInputSnapshot input;
			const bool smooth = la.smooth_turn_deg_s > 0.0f && !EmuConfig.VR.SnapTurn;
			const bool snap = !smooth && (la.snap_turn_deg > 0.0f || la.smooth_turn_deg_s > 0.0f);
			const float snap_deg = (la.snap_turn_deg > 0.0f) ? la.snap_turn_deg : 30.0f;
			const bool want_input = la.yaw_anchor && (((smooth || snap) && la.snap_stick_hand >= 0) || la.has_aim);
			if (want_input)
				input = GetInputSnapshot();
			const float turn_dt = TurnDt();
			if (la.yaw_anchor)
			{
				if (!s_yaw_anchor_valid)
				{
					// Each time first person takes over, the view looks the way the character faces,
					// whichever way the head happens to be turned.
					s_yaw_anchor = WrapPi(heading - la.yaw_sign * e.yaw);
					s_yaw_anchor_valid = true;
					s_snap_ready = false;
				}
				if (la.snap_stick_hand >= 0 && input.actions_active)
				{
					const float sx = input.hands[la.snap_stick_hand].thumbstick_x;
					// A right push turns the view right: -yawSign converts "clockwise" into game yaw.
					const float dir = (sx > 0.0f) ? 1.0f : -1.0f;
					if (smooth)
					{
						constexpr float kDeadzone = 0.2f;
						const float t = std::clamp((std::abs(sx) - kDeadzone) / (0.95f - kDeadzone), 0.0f, 1.0f);
						if (t > 0.0f)
						{
							const float rate = la.smooth_turn_deg_s * t * (0.5f + 0.5f * t);
							s_yaw_anchor = WrapPi(s_yaw_anchor - la.yaw_sign * dir * rate * turn_dt * (PI_F / 180.0f));
						}
						s_snap_ready = false;
					}
					else if (snap)
					{
						if (s_snap_ready && std::abs(sx) > 0.7f)
						{
							s_yaw_anchor = WrapPi(s_yaw_anchor - la.yaw_sign * dir * snap_deg * (PI_F / 180.0f));
							s_snap_ready = false;
						}
						else if (std::abs(sx) < 0.3f)
							s_snap_ready = true;
					}
				}
				base_yaw = s_yaw_anchor;
			}

			const float yaw = base_yaw + la.yaw_sign * e.yaw;
			const float pitch = std::clamp(la.pitch_sign * e.pitch, -1.45f, 1.45f);
			const float sy = std::sin(yaw), cy = std::cos(yaw);
			const float sp = std::sin(pitch), cp = std::cos(pitch);

			float eye[3] = {p[0] + la.eye_forward * sy, p[1] + la.eye_height, p[2] + la.eye_forward * cy};
			if (la.units_per_meter != 0.0f && s_ref_pos_valid)
			{
				if (pose.position_valid)
				{
					s_last_dx = pose.position_x - s_ref_px;
					s_last_dy = pose.position_y - s_ref_py;
					s_last_dz = pose.position_z - s_ref_pz;
				}
				const float dx = s_last_dx, dy = s_last_dy, dz = s_last_dz;
				// OpenXR: right +X, up +Y, forward -Z, rotated by the recenter yaw.
				const float rs = std::sin(s_ref_yaw), rc = std::cos(s_ref_yaw);
				const float fwd = -dx * rs - dz * rc;
				const float right = dx * rc - dz * rs;
				// Game: forward at base_yaw; "right" is a quarter turn clockwise, i.e. -yawSign * 90 deg.
				const float right_yaw = base_yaw - la.yaw_sign * (PI_F * 0.5f);
				const float u = la.units_per_meter;
				eye[0] += u * (fwd * std::sin(base_yaw) + right * std::sin(right_yaw));
				eye[1] += u * dy;
				eye[2] += u * (fwd * std::cos(base_yaw) + right * std::cos(right_yaw));
			}

			// Point-to-aim: while the stance says the weapon is raised, face where the aim hand points
			// (recenter-relative, around the camera anchor), so the camera itself does not turn.
			bool weapon_raised = false;
			if (la.has_aim && heading_addr != 0 && input.actions_active && la.aim_hand >= 0)
			{
				const s64 sa = static_cast<s64>(base.value()) + la.aim_stance_offset;
				if (sa >= 0 && sa + la.aim_stance_width <= static_cast<s64>(Ps2MemSize::MainRam))
				{
					u32 stance = 0;
					switch (la.aim_stance_width)
					{
						case 1: stance = memRead8(static_cast<u32>(sa)); break;
						case 2: stance = memRead16(static_cast<u32>(sa)); break;
						default: stance = memRead32(static_cast<u32>(sa)); break;
					}
					const VRPose& aim = input.hands[la.aim_hand].aim_pose;
					weapon_raised = (stance == la.aim_stance_equals);
					if (stance == la.aim_stance_equals && aim.valid)
					{
						const EulerAngles h = QuaternionToEulerYXZ(aim.orientation_xyzw[0], aim.orientation_xyzw[1],
							aim.orientation_xyzw[2], aim.orientation_xyzw[3]);
						memWrite16(heading_addr, ToBinaryAngle(base_yaw + la.yaw_sign * WrapPi(h.yaw - s_ref_yaw)));
						if (la.has_aim_pitch)
						{
							const s64 pa2 = static_cast<s64>(base.value()) + la.aim_pitch_offset;
							if (pa2 >= 0 && pa2 + 2 <= static_cast<s64>(Ps2MemSize::MainRam))
							{
								const float raw = std::clamp(la.aim_pitch_sign * h.pitch * (65536.0f / (2.0f * PI_F)),
									-la.aim_pitch_clamp, la.aim_pitch_clamp);
								memWrite16(static_cast<u32>(pa2), static_cast<u16>(static_cast<s16>(std::lround(raw))));
							}
						}
					}
				}
			}
			// The first-person hands show the pistol in the right hand while the weapon is raised.
			HandModel::SetGunHeld(weapon_raised);

			// Body follows view: standing idle with the stick centred, the character turns to face where the
			// camera looks, so picking up, checking and opening things work on what the player looks at.
			if (!la.body_follow.empty() && heading_addr != 0 && RecordChecksPass(la.body_follow, base.value()))
				memWrite16(heading_addr, ToBinaryAngle(yaw));

			const float target[3] = {eye[0] + la.distance * sy * cp, eye[1] + la.distance * sp,
				eye[2] + la.distance * cy * cp};
			for (int i = 0; i < 3; i++)
			{
				if (!std::isfinite(eye[i]) || !std::isfinite(target[i]) || std::abs(eye[i]) > 1e7f ||
					std::abs(target[i]) > 1e7f)
					return false;
			}

			for (u32 i = 0; i < 3; i++)
			{
				memWrite32(la.eye_address + i * 4, std::bit_cast<u32>(eye[i]));
				memWrite32(la.target_address + i * 4, std::bit_cast<u32>(target[i]));
			}
			if (la.roll_address != 0)
			{
				const s32 roll = static_cast<s32>(std::lround(la.roll_sign * e.roll * (65536.0f / (2.0f * PI_F)))) & 0xFFFF;
				memWrite32(la.roll_address, static_cast<u32>(roll));
			}
			s_base_yaw_now.store(base_yaw, std::memory_order_release);
			if (out)
			{
				out->base_yaw = base_yaw;
				out->yaw_sign = la.yaw_sign;
				out->dir[0] = sy * cp;
				out->dir[1] = sp;
				out->dir[2] = cy * cp;
				std::memcpy(out->eye, eye, sizeof(out->eye));
			}
			return true;
		}

		// camera.lookAt holds: saved on first write, put back (only if still ours) when first person ends.
		struct HoldState
		{
			bool saved = false;
			u32 original = 0;
		};
		std::vector<HoldState> s_hold_state;
		u32 s_hold_crc = 0;
		bool s_hold_repair = false; // one-shot after a savestate load: undo holds saved in the state

		void ApplyHolds(const ProfileDB::CameraLookAt& la, u32 crc)
		{
			if (s_hold_crc != crc || s_hold_state.size() != la.holds.size())
			{
				s_hold_state.assign(la.holds.size(), HoldState{});
				s_hold_crc = crc;
			}
			for (size_t i = 0; i < la.holds.size(); i++)
			{
				const ProfileDB::CameraLookAt::Hold& h = la.holds[i];
				if (!h.when.empty() && !GuardListPass(h.when))
					continue;
				HoldState& st = s_hold_state[i];
				const u32 cur = ReadSized(h.address, h.width);
				if (!st.saved)
				{
					st.original = (cur == h.value && h.has_restore) ? h.restore : cur;
					st.saved = true;
				}
				if (cur != h.value)
					WriteSized(h.address, h.width, h.value);
			}
		}

		void RestoreHolds(const ProfileDB::CameraLookAt* la, bool write)
		{
			if (la && write && s_hold_state.size() == la->holds.size())
			{
				for (size_t i = 0; i < la->holds.size(); i++)
				{
					const ProfileDB::CameraLookAt::Hold& h = la->holds[i];
					HoldState& st = s_hold_state[i];
					if (!st.saved || (!h.when.empty() && !GuardListPass(h.when)))
						continue;
					if (ReadSized(h.address, h.width) == h.value)
						WriteSized(h.address, h.width, st.original);
				}
			}
			for (HoldState& st : s_hold_state)
				st.saved = false;
		}

		// After loading a savestate made while first person was active, with first person now off: put
		// back the holds whose known original the profile gives, if the state carried our value.
		void RepairHolds(const ProfileDB::CameraLookAt& la)
		{
			for (const ProfileDB::CameraLookAt::Hold& h : la.holds)
			{
				if (!h.has_restore || (!h.when.empty() && !GuardListPass(h.when)))
					continue;
				if (ReadSized(h.address, h.width) == h.value)
					WriteSized(h.address, h.width, h.restore);
			}
		}

		std::atomic_bool s_lookat_active{false};
		std::atomic<u64> s_lookat_stamp_ms{0};
		bool s_fov_saved = false;
		u32 s_fov_saved_raw = 0;

		u64 NowMs()
		{
			return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		struct StampedPose
		{
			u64 ms = 0;
			HeadPose::Snapshot pose;
		};
		std::mutex s_pose_mutex;
		std::array<StampedPose, 64> s_pose_ring;
		size_t s_pose_next = 0;
		size_t s_pose_count = 0;

		void PushStampedPose(const HeadPose::Snapshot& pose)
		{
			std::lock_guard<std::mutex> lock(s_pose_mutex);
			s_pose_ring[s_pose_next] = StampedPose{NowMs(), pose};
			s_pose_next = (s_pose_next + 1) % s_pose_ring.size();
			s_pose_count = std::min(s_pose_count + 1, s_pose_ring.size());
		}

		// Render-pose sync. Each vsync records what camera.lookAt wrote; when the game's view matrix
		// changes (it built a new frame), the write it was built from is found by matching direction and
		// eye, and that write's head pose is queued for the frame. The pose of the frame on screen is sent
		// down the GS queue just before the vsync that presents it, so the compositor places each frame
		// exactly where it was rendered from: no time guess, no judder from 30 fps frames shown twice.
		struct WrittenPose
		{
			LookAtWrite w;
			HeadPose::Snapshot pose;
		};
		std::array<WrittenPose, 16> s_written{};
		size_t s_written_next = 0;
		size_t s_written_count = 0;
		struct FramePose
		{
			HeadPose::Snapshot pose;
			float base_yaw = 0.0f;
			float yaw_sign = 1.0f;
		};
		std::array<FramePose, 8> s_frame_poses{};
		std::array<u32, 8> s_frame_age{}; // how many writes back the match was (for the sync log)
		size_t s_frame_next = 0;
		size_t s_frame_count = 0;
		u64 s_frame_seq = 0;
		u64 s_frame_published_seq = 0;
		float s_view_prev[16] = {};
		bool s_view_prev_valid = false;
		bool s_render_published = false; // a valid render pose is on the GS side
		bool s_render_sync_logged = false;

		std::mutex s_gs_render_mutex;
		FramePose s_gs_render_pose;
		bool s_gs_render_valid = false;

		void PublishRenderPose(bool valid, const FramePose& pose)
		{
			s_render_published = valid;
			if (!MTGS::IsOpen())
				return;
			MTGS::RunOnGSThread([valid, pose]() {
				std::lock_guard<std::mutex> lock(s_gs_render_mutex);
				s_gs_render_valid = valid;
				s_gs_render_pose = pose;
			});
		}

		void ResetRenderSync()
		{
			s_written_count = 0;
			s_frame_count = 0;
			s_view_prev_valid = false;
			if (s_render_published)
				PublishRenderPose(false, FramePose{});
		}

		void PushWritten(const LookAtWrite& w, const HeadPose::Snapshot& pose)
		{
			s_written[s_written_next] = WrittenPose{w, pose};
			s_written_next = (s_written_next + 1) % s_written.size();
			s_written_count = std::min(s_written_count + 1, s_written.size());
		}

		// Before this vsync's write: if the view matrix changed since the last vsync, the game built a frame
		// from one of the earlier writes. Match it and queue that write's head pose for the frame.
		void TrackRenderPose(const ProfileDB::CameraLookAt& la)
		{
			if (la.view_matrix_address == 0)
				return;
			float m[16];
			for (u32 i = 0; i < 16; i++)
			{
				m[i] = std::bit_cast<float>(static_cast<u32>(memRead32(la.view_matrix_address + i * 4)));
				if (!std::isfinite(m[i]))
				{
					s_view_prev_valid = false;
					return;
				}
			}
			const bool changed = !s_view_prev_valid || std::memcmp(m, s_view_prev, sizeof(m)) != 0;
			std::memcpy(s_view_prev, m, sizeof(m));
			s_view_prev_valid = true;
			if (!changed || s_written_count == 0)
				return;

			// Row vectors: the camera looks down -column 2, and the eye is -(row 3) * R^T.
			const float look[3] = {-m[2], -m[6], -m[10]};
			float eye[3];
			for (int i = 0; i < 3; i++)
				eye[i] = -(m[12] * m[i * 4 + 0] + m[13] * m[i * 4 + 1] + m[14] * m[i * 4 + 2]);

			float best = std::numeric_limits<float>::max();
			const WrittenPose* hit = nullptr;
			u32 hit_age = 0;
			for (size_t i = 0; i < s_written_count; i++)
			{
				const WrittenPose& wp = s_written[(s_written_next + s_written.size() - 1 - i) % s_written.size()];
				const float dot = look[0] * wp.w.dir[0] + look[1] * wp.w.dir[1] + look[2] * wp.w.dir[2];
				const float dx = eye[0] - wp.w.eye[0], dy = eye[1] - wp.w.eye[1], dz = eye[2] - wp.w.eye[2];
				// Direction error (1 - cos) plus eye distance; 10 game units weigh about as much as 0.8 deg.
				const float err = (1.0f - dot) + 1.0e-6f * (dx * dx + dy * dy + dz * dz);
				if (err < best)
				{
					best = err;
					hit = &wp;
					hit_age = static_cast<u32>(i + 1);
				}
			}
			// Within about 3 deg and 30 units; anything else is a frame the game built from its own camera.
			if (!hit || !(best < 0.0025f))
				return;
			s_frame_poses[s_frame_next] = FramePose{hit->pose, hit->w.base_yaw, hit->w.yaw_sign};
			s_frame_age[s_frame_next] = hit_age;
			s_frame_next = (s_frame_next + 1) % s_frame_poses.size();
			s_frame_count = std::min(s_frame_count + 1, s_frame_poses.size());
			s_frame_seq++;
		}

		// Send the head pose of the frame on screen now, `syncFramesBack` matched frames behind the newest.
		void PublishMatchedPose(const ProfileDB::CameraLookAt& la)
		{
			if (la.view_matrix_address == 0 || s_frame_count <= la.sync_frames_back || s_frame_seq == s_frame_published_seq)
				return;
			const size_t idx = (s_frame_next + s_frame_poses.size() - 1 - la.sync_frames_back) % s_frame_poses.size();
			PublishRenderPose(true, s_frame_poses[idx]);
			s_frame_published_seq = s_frame_seq;
			static const bool s_synclog = (std::getenv("PCSX2_VR_SYNCLOG") != nullptr);
			if (!s_render_sync_logged)
			{
				Console.WriteLn("(VR) CameraDriver: first-person frames placed at their matched render pose (view matrix "
								"0x%08X, %u frame(s) back).", la.view_matrix_address, la.sync_frames_back);
				s_render_sync_logged = true;
			}
			if (s_synclog)
			{
				const size_t newest = (s_frame_next + s_frame_poses.size() - 1) % s_frame_poses.size();
				Console.WriteLn("(VR) sync: vsync %llu frame %llu matched the write %u vsync(s) back; showing frame -%u (match age %u).",
					static_cast<unsigned long long>(s_vsync_counter), static_cast<unsigned long long>(s_frame_seq),
					s_frame_age[newest], la.sync_frames_back, s_frame_age[idx]);
			}
		}

		void SetLookAtActive(bool active)
		{
			if (active)
				s_lookat_stamp_ms.store(NowMs(), std::memory_order_release);
			s_lookat_active.store(active, std::memory_order_release);
		}

		void RestoreFov(const ProfileDB::CameraProfile& cam)
		{
			if (!s_fov_saved)
				return;
			s_fov_saved = false;
			if (cam.fov.has_value() && cam.fov->encoding == ProfileDB::CameraEncoding::F32 &&
				static_cast<u32>(memRead32(cam.fov->ee_address)) == std::bit_cast<u32>(cam.fov->scale))
				memWrite32(cam.fov->ee_address, s_fov_saved_raw);
		}

		std::optional<HeadPose::Snapshot> MaybeFakePose()
		{
			static const char* env = std::getenv("PCSX2_VR_FAKE_HEADPOSE");
			if (!env)
				return std::nullopt;
			static const float amp_rad = static_cast<float>(std::atof(env)) * (PI_F / 180.0f);

			const float t = static_cast<float>(s_vsync_counter);
			const float yaw = amp_rad * std::sin(t * (2.0f * PI_F / 240.0f));
			const float pitch = 0.5f * amp_rad * std::sin(t * (2.0f * PI_F / 180.0f));

			const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f);
			const float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
			HeadPose::Snapshot p;
			p.orientation_w = cy * cp;
			p.orientation_x = cy * sp;
			p.orientation_y = sy * cp;
			p.orientation_z = -sy * sp;
			p.valid = true;
			return p;
		}
	}

	float PadLookDeflection(const ProfileDB::CameraPadLook& pl, float yaw_deg, PadLookState& st)
	{
		const float mag = std::abs(yaw_deg);
		const int dir = (yaw_deg > 0.0f) ? 1 : (yaw_deg < 0.0f) ? -1 : 0;

		const bool holding = (st.gate != 0) && (dir == st.gate);
		const bool engaged = (dir != 0) && (holding ? (mag >= pl.release_deg) : (mag > pl.engage_deg));

		float deflection = 0.0f;
		if (engaged)
		{
			const float t =
				std::clamp((mag - pl.engage_deg) / (pl.max_look_deg - pl.engage_deg), 0.0f, 1.0f);
			const float stick = pl.stick_floor + std::pow(t, pl.curve) * (1.0f - pl.stick_floor);
			deflection = std::copysign(stick, yaw_deg);
			st.gate = dir;
		}
		else
		{
			st.gate = 0;
		}

		if (pl.latch)
		{
			if (deflection >= 1.0f)
				st.latch = 1;
			else if (deflection <= -1.0f)
				st.latch = -1;
			else if (st.latch != 0)
			{
				const bool settled = (mag < pl.release_deg);
				const bool opposite = (mag > pl.engage_deg) && ((yaw_deg > 0.0f) != (st.latch > 0));
				if (settled || opposite)
					st.latch = 0;
				else
					deflection = static_cast<float>(st.latch);
			}
		}
		else
		{
			st.latch = 0;
		}
		return deflection;
	}

	void Apply()
	{
		s_vsync_counter++;
		MaybeRunSelfTest();

		const bool switched_on = EffectiveVREnabled(EmuConfig.VR.Enable) && EmuConfig.VR.HeadCamera &&
		                         !SplitState::Active();

		const VMState vm_state = VMManager::GetState();
		const bool vm_live = (vm_state == VMState::Running || vm_state == VMState::Paused);
		if (!vm_live)
		{
			SetLookAtActive(false);
			RestoreHolds(nullptr, false);
			s_written_count = 0;
			s_frame_count = 0;
			s_view_prev_valid = false;
			s_render_published = false;
			{
				std::lock_guard<std::mutex> lock(s_gs_render_mutex);
				s_gs_render_valid = false;
			}
			s_fov_saved = false;
			s_armed_logged = false;
			s_silence_applied = false;
			s_hooks_installed = false;
			s_base.valid = false;
			s_padlook = PadLookState{};
			ResetDeltaState();
			PadLook::Publish(0.0f);
			return;
		}

		ProfileDB::EnsureLoaded();
		const u32 crc = VMManager::GetDiscCRC();
		const ProfileDB::Profile* profile = ProfileDB::Lookup(VMManager::GetDiscSerial(), crc);
		if (!profile || !profile->camera.has_value())
		{
			SetLookAtActive(false);
			RestoreHolds(nullptr, false);
			ResetRenderSync();
			s_fov_saved = false;
			s_armed_logged = false;
			s_silence_applied = false;
			s_hooks_installed = false;
			s_base.valid = false;
			s_padlook = PadLookState{};
			if (vm_state == VMState::Running)
			{
				for (AnchorState& a : s_anchor)
					RestoreAnchoredScalar(a);
			}
			ResetDeltaState( vm_state != VMState::Running);
			PadLook::Publish(0.0f);
			return;
		}
		const ProfileDB::CameraProfile& cam = profile->camera.value();

		std::optional<HeadPose::Snapshot> fake = MaybeFakePose();
		const HeadPose::Snapshot pose = fake.has_value() ? fake.value() : HeadPose::Get();

		if (GuardsPass(cam))
			s_guard_fail_vsyncs = 0;
		else if (s_guard_fail_vsyncs < 0xFFFFFFFFu)
			s_guard_fail_vsyncs++;
		const bool guards_hold = (s_guard_fail_vsyncs == 0) ||
		                         (s_armed_logged && s_guard_fail_vsyncs <= cam.disarm_after_vsyncs);
		const bool armed = switched_on && (vm_state == VMState::Running) && pose.valid && guards_hold;

		if (armed != s_armed_logged)
		{
			// Each time first person takes over (after a door, cutscene or menu) the camera faces the
			// way the character faces.
			s_yaw_anchor_valid = false;
			if (armed)
				Console.WriteLn(Color_StrongGreen,
					"(VR) CameraDriver: ARMED (CRC %08X) — %zu write op(s), %zu matrix op(s)%s.",
					crc, cam.writes.size(), cam.matrix_writes.size(), cam.look_at.has_value() ? ", first-person lookAt" : "");
			else
			{
				const char* reason =
					!switched_on                   ? "VR/HeadCamera switch off" :
					(vm_state != VMState::Running) ? "VM not Running (paused)" :
					!pose.valid                    ? "no valid head pose published" :
													 "profile guard failed";
				Console.WriteLn(Color_StrongOrange, "(VR) CameraDriver: DISARMED (CRC %08X) — %s.", crc, reason);
			}
			s_armed_logged = armed;
		}

		if (!armed)
		{
			SetLookAtActive(false);
			// While the VM is paused nothing can change; holds keep their saved originals for the resume.
			if (vm_state == VMState::Running)
			{
				RestoreHolds(cam.look_at.has_value() ? &cam.look_at.value() : nullptr, true);
				if (s_hold_repair && cam.look_at.has_value())
					RepairHolds(cam.look_at.value());
				s_hold_repair = false;
			}
			ResetRenderSync();
			s_render_sync_logged = false;
			RestoreFov(cam);
			RestoreSilence(cam, crc);
			RestoreCodeHooks(cam, crc);
			if (vm_state == VMState::Running)
			{
				for (MatAnchor& an : s_mat_anchor)
				{
					if (!an.has)
						continue;
					const std::optional<Mat3> guest = ReadGuestMat3(an.addr);
					if (guest.has_value())
					{
						bool is_ours = true;
						for (int c = 0; c < 9 && is_ours; c++)
							is_ours = std::fabs(guest.value().m[c] - an.written[c]) < 1e-5f;
						if (is_ours)
						{
							Mat3 base{};
							std::memcpy(base.m, an.base, sizeof(base.m));
							WriteGuestMat3(an.addr, base);
						}
					}
					an.has = false;
				}
				for (AnchorState& a : s_anchor)
					RestoreAnchoredScalar(a);
			}
			s_padlook = PadLookState{};
			ResetDeltaState( vm_state != VMState::Running);
			PadLook::Publish(0.0f);
			return;
		}

		ApplySilence(cam, crc);
		ApplyCodeHooks(cam, crc);

		const std::optional<u32> base = GetBase(cam, crc);
		if (cam.base.has_value() && !base.has_value())
		{
			s_base_unresolved_vsyncs++;
			if (s_base_unresolved_vsyncs == 300 || (s_base_unresolved_vsyncs > 300 && (s_base_unresolved_vsyncs - 300) % 1800 == 0))
			{
				if (cam.base->is_pointer)
					Console.Warning("(VR) CameraDriver: camera base UNRESOLVED for %u s while armed (CRC %08X) — relative op(s) are sitting out; pointer root 0x%08X reads 0x%08X (a hop or the validate word failed if that is non-zero).",
						s_base_unresolved_vsyncs / 60, crc, cam.base->pointer_addr, memRead32(cam.base->pointer_addr));
				else
					Console.Warning("(VR) CameraDriver: camera base UNRESOLVED for %u s while armed (CRC %08X) — relative op(s) are sitting out (scan/indexed base: no match).",
						s_base_unresolved_vsyncs / 60, crc);
			}
		}
		else
			s_base_unresolved_vsyncs = 0;

		if (s_recenter_requested.exchange(false, std::memory_order_acq_rel) || !s_has_reference ||
			s_reference_crc != crc)
		{
			s_ref_x = pose.orientation_x;
			s_ref_y = pose.orientation_y;
			s_ref_z = pose.orientation_z;
			s_ref_w = pose.orientation_w;
			if (pose.position_valid)
			{
				s_ref_px = pose.position_x;
				s_ref_py = pose.position_y;
				s_ref_pz = pose.position_z;
				s_ref_pos_valid = true;
			}
			s_last_dx = s_last_dy = s_last_dz = 0.0f;
			s_ref_yaw = QuaternionToEulerYXZ(s_ref_x, s_ref_y, s_ref_z, s_ref_w).yaw;
			s_yaw_anchor_valid = false;
			s_has_reference = true;
			s_reference_crc = crc;
			ResetDeltaState( true);
			s_padlook.gate = 0;
			DevCon.WriteLn("(VR) CameraDriver: view recentered.");
		}

		if (s_delta_crc != crc || s_delta_prev.size() != cam.writes.size())
		{
			s_delta_crc = crc;
			s_delta_prev.assign(cam.writes.size(), 0.0f);
			s_delta_has.assign(cam.writes.size(), false);
			for (AnchorState& a : s_anchor)
				RestoreAnchoredScalar(a);
			s_anchor.assign(cam.writes.size(), AnchorState{});
		}
		if (s_mat_crc != crc || s_mat_prev.size() != cam.matrix_writes.size())
		{
			s_mat_crc = crc;
			s_mat_prev.assign(cam.matrix_writes.size(), MatBaseline{});
			s_mat_anchor.assign(cam.matrix_writes.size(), MatAnchor{});
		}

		float qx = pose.orientation_x, qy = pose.orientation_y, qz = pose.orientation_z, qw = pose.orientation_w;
		ApplyReference(qx, qy, qz, qw);

		const EulerAngles euler = QuaternionToEulerYXZ(qx, qy, qz, qw);

		for (size_t i = 0; i < cam.writes.size(); i++)
		{
			const ProfileDB::CameraWriteOp& op = cam.writes[i];
			u32 address = op.ee_address;
			if (op.relative)
			{
				if (!base.has_value())
				{
					s_delta_has[i] = false;
					if (i < s_anchor.size()) RestoreAnchoredScalar(s_anchor[i]);
					continue;
				}
				const s64 absolute = static_cast<s64>(address) + static_cast<s64>(base.value());
				if (absolute < 0 || absolute + 8 > static_cast<s64>(Ps2MemSize::MainRam))
				{
					s_delta_has[i] = false;
					if (i < s_anchor.size()) RestoreAnchoredScalar(s_anchor[i]);
					continue;
				}
				address = static_cast<u32>(absolute);
			}
			if (!op.when.empty() && !GuardListPass(op.when))
			{
				s_delta_has[i] = false;
				if (i < s_anchor.size())
					RestoreAnchoredScalar(s_anchor[i]);
				continue;
			}
			EvaluateAndWrite(op, i, address, euler, pose);
		}

		for (size_t i = 0; i < cam.matrix_writes.size(); i++)
		{
			const ProfileDB::CameraMatrixOp& op = cam.matrix_writes[i];
			u32 address = op.ee_address;
			u32 taddress = op.transpose_address;
			if (op.relative)
			{
				if (!base.has_value())
				{
					s_mat_prev[i].has = false;
					continue;
				}
				address += base.value();
				taddress += base.value();
			}
			if (!op.when.empty() && !GuardListPass(op.when))
			{
				s_mat_prev[i].has = false;
				if (s_mat_anchor[i].has)
				{
					const std::optional<Mat3> guest = ReadGuestMat3(address);
					if (guest.has_value())
					{
						bool is_ours = true;
						for (int c = 0; c < 9 && is_ours; c++)
							is_ours = std::fabs(guest.value().m[c] - s_mat_anchor[i].written[c]) < 1e-5f;
						if (is_ours)
						{
							Mat3 base{};
							std::memcpy(base.m, s_mat_anchor[i].base, sizeof(base.m));
							WriteGuestMat3(address, base);
						}
					}
					s_mat_anchor[i].has = false;
				}
				continue;
			}
			ApplyMatrixOp(op, i, address, taddress, euler);
		}

		// During the disarm grace (guards failing, still armed) nothing is written and the active flag
		// keeps its value: the game cannot move the camera while its writers are silenced anyway.
		const bool guards_now = (s_guard_fail_vsyncs == 0);
		if (!cam.look_at.has_value())
			SetLookAtActive(false);
		else if (guards_now)
		{
			const ProfileDB::CameraLookAt& la = cam.look_at.value();
			s_hold_repair = false;
			ApplyHolds(la, crc);
			if (!la.pause_when.empty() && GuardListPass(la.pause_when))
			{
				// A menu, the map or pause: the camera keeps its last view (its writers stay silenced) and the
				// world screen shows the menu in front of the player.
				ResetRenderSync();
				SetLookAtActive(false);
			}
			else
			{
				// lookAt recenters yaw only, so pitch and roll stay level with gravity whatever the head was
				// doing at the recenter.
				const EulerAngles head = QuaternionToEulerYXZ(pose.orientation_x, pose.orientation_y,
					pose.orientation_z, pose.orientation_w);
				const EulerAngles look{WrapPi(head.yaw - s_ref_yaw), head.pitch, head.roll};
				TrackRenderPose(la);
				LookAtWrite w;
				const bool wrote = ApplyLookAt(la, base, look, pose, &w);
				if (wrote)
				{
					PushStampedPose(pose);
					PushWritten(w, pose);
					PublishMatchedPose(la);
				}
				SetLookAtActive(wrote);
			}
		}
		else if (s_lookat_active.load(std::memory_order_relaxed))
			SetLookAtActive(true);

		WriteCodeHookScratch(cam, euler, pose);

		if (cam.fov.has_value() && guards_now)
		{
			if (!s_fov_saved)
			{
				s_fov_saved_raw = static_cast<u32>(memRead32(cam.fov->ee_address));
				s_fov_saved = true;
			}
			EncodeAndWrite(cam.fov->ee_address, cam.fov->scale, cam.fov->encoding);
		}

		if (cam.pad_look.has_value())
		{
			const float yaw_deg = euler.yaw * (180.0f / PI_F);
			PadLook::Publish(PadLookDeflection(cam.pad_look.value(), yaw_deg, s_padlook));
		}
		else
		{
			s_padlook = PadLookState{};
			PadLook::Publish(0.0f);
		}
	}

	void OnStateLoaded()
	{
		ResetDeltaState();
		s_silence_repair = true;
		s_fov_saved = false;
		// The loaded memory has its own values: holds save them afresh, and no frame matches old writes.
		RestoreHolds(nullptr, false);
		s_hold_repair = true;
		ResetRenderSync();
	}

	bool RenderPose(HeadPose::Snapshot* out, float* turn_yaw)
	{
		std::lock_guard<std::mutex> lock(s_gs_render_mutex);
		if (!s_gs_render_valid)
			return false;
		*out = s_gs_render_pose.pose;
		// The game camera's yaw is base + yawSign * head yaw. Showing a frame rendered at base B_r while
		// the base is now B_n means placing it yawSign * (B_r - B_n) further round in the room.
		if (turn_yaw)
			*turn_yaw = s_gs_render_pose.yaw_sign *
			            WrapPi(s_gs_render_pose.base_yaw - s_base_yaw_now.load(std::memory_order_acquire));
		return true;
	}

	u64 SteadyNowMs()
	{
		return NowMs();
	}

	bool FirstPersonPoseAt(u64 time_ms, HeadPose::Snapshot* out)
	{
		std::lock_guard<std::mutex> lock(s_pose_mutex);
		if (s_pose_count == 0)
			return false;
		// Walk back from the newest entry to the first one stamped at or before time_ms; if every
		// entry is newer, the oldest one is the best we have.
		const StampedPose* best = nullptr;
		for (size_t i = 0; i < s_pose_count; i++)
		{
			const StampedPose& e = s_pose_ring[(s_pose_next + s_pose_ring.size() - 1 - i) % s_pose_ring.size()];
			best = &e;
			if (e.ms <= time_ms)
				break;
		}
		const StampedPose& newest = s_pose_ring[(s_pose_next + s_pose_ring.size() - 1) % s_pose_ring.size()];
		if (!best || (time_ms > newest.ms && time_ms - newest.ms > 500))
			return false;
		*out = best->pose;
		return true;
	}

	bool LookAtActive()
	{
		// Lapses on its own when Apply stops running (pause, stall), so the screen goes world-locked.
		return s_lookat_active.load(std::memory_order_acquire) &&
		       NowMs() - s_lookat_stamp_ms.load(std::memory_order_acquire) < 150;
	}

	void RequestRecenter()
	{
		s_recenter_requested.store(true, std::memory_order_release);
	}

	bool SelfTestAssembler()
	{
		return CountAssemblerMismatches(false) == 0;
	}

	bool SelfTestMath()
	{
		return CountMathMismatches(false) == 0;
	}
}
