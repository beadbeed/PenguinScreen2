// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/HandModel.h"
#include "VR/GameModels.h"
#include "VR/HandMesh.h"
#include "VR/ItemData.h"
#include "VR/MeshRaster.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Image.h"
#include "common/Path.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace VR::HandModel
{
	namespace
	{
		// The raised weapon (WeaponHeld, PackHeld), and the last one logged (all ones: none yet).
		std::atomic<u32> s_weapon_held{0};
		std::atomic<u32> s_weapon_logged{0xFFFFFFFFu};

		// WeaponHeld in one word: bit 0 raised, 1 known, 2 laser, bits 8-15 held, 16-31 the item id.
		u32 PackHeld(const WeaponHeld& w)
		{
			return (w.raised ? 1u : 0u) | (w.known ? 2u : 0u) | (w.laser ? 4u : 0u) | (static_cast<u32>(w.held) << 8) |
			       (static_cast<u32>(w.item_id) << 16);
		}

		WeaponHeld UnpackHeld(u32 v)
		{
			WeaponHeld w;
			w.raised = (v & 1u) != 0;
			w.known = (v & 2u) != 0;
			w.laser = (v & 4u) != 0;
			const u32 held = (v >> 8) & 0xFFu;
			w.held = (held <= static_cast<u32>(Held::Spray)) ? static_cast<Held>(held) : Held::Pistol;
			w.item_id = static_cast<u16>(v >> 16);
			return w;
		}

		constexpr float kPi = 3.14159265358979323846f;
		constexpr float kDegToRad = kPi / 180.0f;
		constexpr int kSize = static_cast<int>(kImageSize);
		constexpr size_t kPixels = static_cast<size_t>(kImageSize) * kImageSize;

		// The hand is modelled in centimetres in the pointing frame, right hand. The controller handle
		// (and the pistol grip) leans its top this far forward of +Y, like a pistol grip angle.
		constexpr float kHandleTiltDeg = 15.0f;
		// The palm runs back from the knuckles this far below square to the handle, so after the
		// handle tilt the wrist comes out level instead of cocked up.
		constexpr float kPalmDropDeg = 15.0f;
		// Shot feedback (HandState::kick, slide): at full kick the hand and gun pitch up this far about the grip
		// and come back this far toward the wrist; the slide travels this far back when it cycles or locks.
		constexpr float kRecoilDeg = 7.0f;
		constexpr float kRecoilBackCm = 1.5f;
		constexpr float kSlideTravelCm = 1.8f;
		// A long gun, heavier, kicks less far up and further back.
		constexpr float kLongRecoilDeg = 4.0f;
		constexpr float kLongRecoilBackCm = 2.5f;
		constexpr float kFingerHalfT = 0.9f; // fingers ~1.8 cm thick
		// Triangles closer to the eye than this are dropped rather than clipped.
		constexpr float kNearM = 0.03f;
		// PlaceImpostor fits the quad to the hand's bounding box seen from the eye when every corner of it is at least
		// this far in front of the eye, with this margin (the edge softening's border included); otherwise it centres on
		// the hand and makes room for this much more than the hand.
		constexpr float kFitNearM = 0.05f;
		constexpr float kFitMargin = 1.06f;
		constexpr float kFallbackGrow = 1.5f;

		// The guns' barrels lie along one line in the pointing frame, 6.7 cm over the grip position: the pistol's muzzle
		// face is 14.14 cm ahead (HudCards' kPistolMuzzle), and a long gun's barrel goes on from the front of its
		// receiver to its own muzzle (LongGunDef).
		constexpr float kBoreYCm = 6.7f;
		constexpr float kPistolMuzzleZCm = -14.14f;
		constexpr float kReceiverFrontCm = -14.0f;
		// The height of a long gun's fore-end (where the left hand closes) over the grip position.
		constexpr float kForeEndYCm = 4.5f;

		// A long gun's look, by item: a pump shotgun (wooden fore-end and stock, a magazine tube under the barrel), a
		// hunting rifle (wood, a scope), an assault rifle or submachine gun (black, a magazine) or a launcher (a fat
		// barrel, a fore-grip). Its muzzle face and the middle of its fore-end lie this far along the pointing
		// direction (-Z) from the grip position.
		enum class LongStyle : u8
		{
			Shotgun,
			Rifle,
			Assault,
			Launcher,
		};

		struct LongGunDef
		{
			u16 id;
			LongStyle style;
			float muzzle_cm;
			float fore_end_cm;
		};

		constexpr LongGunDef kLongGuns[] = {
			{10, LongStyle::Assault, -36.0f, -19.0f},  // Submachine Gun
			{11, LongStyle::Shotgun, -52.0f, -25.0f},  // Shotgun
			{12, LongStyle::Shotgun, -52.0f, -25.0f},  // Shotgun E
			{13, LongStyle::Rifle, -58.0f, -25.0f},    // Hunting Rifle
			{14, LongStyle::Assault, -46.0f, -22.0f},  // Assault Rifle
			{15, LongStyle::Launcher, -40.0f, -22.0f}, // Capsule Gun (blue, red, green, white)
			{16, LongStyle::Launcher, -40.0f, -22.0f},
			{17, LongStyle::Launcher, -40.0f, -22.0f},
			{18, LongStyle::Launcher, -40.0f, -22.0f},
			{20, LongStyle::Launcher, -55.0f, -24.0f}, // Rocket Launcher
			{23, LongStyle::Launcher, -44.0f, -22.0f}, // Grenade Launcher (burst, flame, acid, extinguish, BOW gas)
			{24, LongStyle::Launcher, -44.0f, -22.0f},
			{25, LongStyle::Launcher, -44.0f, -22.0f},
			{26, LongStyle::Launcher, -44.0f, -22.0f},
			{27, LongStyle::Launcher, -44.0f, -22.0f},
		};
		// Any other long gun.
		constexpr LongGunDef kLongGunDefault = {0, LongStyle::Shotgun, -52.0f, -25.0f};

		const LongGunDef& LongGunFor(u16 item_id)
		{
			for (const LongGunDef& d : kLongGuns)
			{
				if (d.id == item_id)
					return d;
			}
			return kLongGunDefault;
		}

		// A melee weapon's look and length from its butt to its tip, by item (Outbreak-Research's ids, ItemData): knives
		// about 25 cm, pipes and the wrench about 45, the axe, hammer and stun rod about 50 with a head (the scrub brush
		// 40), poles, the square timber, the spear and the crutches about a metre. Each lies along the pointing
		// direction, its butt just behind the middle of the fist.
		enum class MeleeStyle : u8
		{
			Blade,
			BroadBlade,
			Pipe,
			BentPipe,
			Wrench,
			Pole,
			Timber,
			Spear,
			Axe,
			Hammer,
			Crutch,
			Rod,
			Brush,
		};

		struct MeleeDef
		{
			u16 id;
			MeleeStyle style;
			float length_cm;
		};

		constexpr MeleeDef kMelee[] = {
			{100, MeleeStyle::Blade, 25.0f},      // Survival Knife
			{101, MeleeStyle::Blade, 22.0f},      // Folding Knife
			{102, MeleeStyle::BroadBlade, 28.0f}, // Butcher Knife
			{103, MeleeStyle::Pipe, 45.0f},       // Iron Pipe
			{104, MeleeStyle::BentPipe, 45.0f},   // Curved Iron Pipe
			{105, MeleeStyle::BentPipe, 45.0f},   // Bent Iron Pipe
			{106, MeleeStyle::Pole, 100.0f},      // Long Pole
			{107, MeleeStyle::Timber, 100.0f},    // Square Timber
			{109, MeleeStyle::Axe, 50.0f},        // Axe
			{110, MeleeStyle::Brush, 40.0f},      // Scrub Brush
			{111, MeleeStyle::Pole, 100.0f},      // Wooden Pole
			{113, MeleeStyle::Spear, 100.0f},     // Spear
			{115, MeleeStyle::Hammer, 50.0f},     // Hammer
			{116, MeleeStyle::Crutch, 100.0f},    // Crutch
			{117, MeleeStyle::Rod, 50.0f},        // Stun Rod
			{119, MeleeStyle::Crutch, 100.0f},    // Broken Crutch
			{407, MeleeStyle::Blade, 22.0f},      // Folding Knife (David's)
			{408, MeleeStyle::Wrench, 45.0f},     // Monkey Wrench
		};
		// Any other melee weapon.
		constexpr MeleeDef kMeleeDefault = {0, MeleeStyle::Pipe, 45.0f};

		const MeleeDef& MeleeFor(u16 item_id)
		{
			for (const MeleeDef& d : kMelee)
			{
				if (d.id == item_id)
					return d;
			}
			return kMeleeDefault;
		}

		// A melee weapon's axis runs this high over the grip position (through the middle of the fist), and its butt
		// this far behind it.
		constexpr float kMeleeAxisYCm = 0.6f;
		constexpr float kMeleeButtCm = 3.5f;
		// A curved or bent pipe turns up this far, over its last kBendCm.
		constexpr float kBendDeg = 35.0f;
		constexpr float kBendCm = 14.0f;

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
		float Length(const V3& a) { return std::sqrt(Dot(a, a)); }
		V3 Normalize(const V3& a)
		{
			const float l = Length(a);
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
			const V3 u = V3{q[0], q[1], q[2]};
			const V3 t = Mul(Cross(u, v), 2.0f);
			return Add(Add(v, Mul(t, q[3])), Cross(u, t));
		}

		// Hamilton product a * b of xyzw quaternions (b applied first).
		void QuatMul(const float a[4], const float b[4], float out[4])
		{
			const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
			const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
			const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
			const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
			out[0] = x;
			out[1] = y;
			out[2] = z;
			out[3] = w;
		}

		// Rotates v about the unit axis k by angle (radians).
		V3 RotateAxis(const V3& v, const V3& k, float angle)
		{
			const float c = std::cos(angle);
			const float s = std::sin(angle);
			return Add(Add(Mul(v, c), Mul(Cross(k, v), s)), Mul(k, Dot(k, v) * (1.0f - c)));
		}

		bool Finite(const float* v, int n)
		{
			for (int i = 0; i < n; i++)
			{
				if (!std::isfinite(v[i]))
					return false;
			}
			return true;
		}

		// Unit quaternion from h.quat; identity when it is unusable.
		void NormalizedQuat(const HandState& h, float out[4])
		{
			const float l = std::sqrt(h.quat[0] * h.quat[0] + h.quat[1] * h.quat[1] + h.quat[2] * h.quat[2] +
									  h.quat[3] * h.quat[3]);
			if (!(l > 1.0e-6f) || !std::isfinite(l))
			{
				out[0] = out[1] = out[2] = 0.0f;
				out[3] = 1.0f;
				return;
			}
			for (int i = 0; i < 4; i++)
				out[i] = h.quat[i] / l;
		}

		enum Mat : u8
		{
			kMatGlove,
			kMatKnuckle,
			kMatCuff,
			kMatHandle,
			kMatGunSlide,
			kMatGunFrame,
			kMatFlash, // muzzle flash: unlit, constant warm white
			kMatWood,  // stocks, fore-ends, poles, handles
			kMatSteel, // blades, pipes, heads
			kMatGlass, // the molotov's bottle
			kMatCloth, // the molotov's rag, the brush's bristles
			kMatCan,   // the spray can
			kMatStone, // the concrete piece
			kMatCount
		};

		struct Material
		{
			float r, g, b; // linear albedo
			float spec;
			float rim;
		};

		float SrgbToLinear(u32 c8)
		{
			const float c = static_cast<float>(c8) / 255.0f;
			return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
		}

		Material MakeMaterial(u32 rgb, float spec, float rim)
		{
			Material m;
			m.r = SrgbToLinear((rgb >> 16) & 0xFFu);
			m.g = SrgbToLinear((rgb >> 8) & 0xFFu);
			m.b = SrgbToLinear(rgb & 0xFFu);
			m.spec = spec;
			m.rim = rim;
			return m;
		}

		// Lighting is done in linear light and written as sRGB bytes, which is what the sRGB
		// swapchain the compositor picks expects.
		struct Tables
		{
			Material mat[kMatCount];
			u8 to_srgb[1025];

			Tables()
			{
				// Highlights stay faint: with flat faces a strong one flashes the whole face.
				mat[kMatGlove] = MakeMaterial(0x4a5a48u, 0.03f, 0.45f);
				mat[kMatKnuckle] = MakeMaterial(0x6a7d64u, 0.05f, 0.45f);
				mat[kMatCuff] = MakeMaterial(0x3c4a3eu, 0.03f, 0.40f);
				mat[kMatHandle] = MakeMaterial(0x26282cu, 0.10f, 0.35f);
				mat[kMatGunSlide] = MakeMaterial(0x4b4f55u, 0.15f, 0.40f);
				mat[kMatGunFrame] = MakeMaterial(0x2c2e32u, 0.08f, 0.35f);
				mat[kMatFlash] = MakeMaterial(0xffe8b0u, 0.0f, 0.0f);
				mat[kMatWood] = MakeMaterial(0x6e4a2cu, 0.05f, 0.35f);
				mat[kMatSteel] = MakeMaterial(0x8d939au, 0.25f, 0.45f);
				mat[kMatGlass] = MakeMaterial(0x3f6e45u, 0.30f, 0.50f);
				mat[kMatCloth] = MakeMaterial(0xb5a17eu, 0.03f, 0.35f);
				mat[kMatCan] = MakeMaterial(0xa8322au, 0.15f, 0.40f);
				mat[kMatStone] = MakeMaterial(0x86837cu, 0.02f, 0.35f);
				for (int i = 0; i <= 1024; i++)
				{
					const float l = static_cast<float>(i) / 1024.0f;
					const float sv = (l <= 0.0031308f) ? (l * 12.92f) : (1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f);
					to_srgb[i] = static_cast<u8>(std::clamp(sv * 255.0f + 0.5f, 0.0f, 255.0f));
				}
			}
		};

		const Tables& GetTables()
		{
			static const Tables s_tables;
			return s_tables;
		}

		struct Tri
		{
			V3 p[3];
			V3 n; // outward face normal
			u8 mat;
		};

		// Quad a-b-c-d (a loop) as two triangles. The normal is taken from the diagonals and turned
		// to point away from inside, so the builders never have to care about winding.
		void AddQuad(std::vector<Tri>& out, const V3& a, const V3& b, const V3& c, const V3& d, const V3& inside, u8 mat)
		{
			V3 n = Normalize(Cross(Sub(c, a), Sub(d, b)));
			const V3 centre = Mul(Add(Add(a, b), Add(c, d)), 0.25f);
			if (Dot(n, Sub(centre, inside)) < 0.0f)
				n = Mul(n, -1.0f);
			Tri t;
			t.n = n;
			t.mat = mat;
			t.p[0] = a;
			t.p[1] = b;
			t.p[2] = c;
			out.push_back(t);
			t.p[1] = c;
			t.p[2] = d;
			out.push_back(t);
		}

		// Six-sided solid: c[0..3] loop around one end, c[4..7] are the matching corners of the other.
		void AddHex(std::vector<Tri>& out, const V3* c, u8 mat)
		{
			V3 inside = V3{0.0f, 0.0f, 0.0f};
			for (int i = 0; i < 8; i++)
				inside = Add(inside, c[i]);
			inside = Mul(inside, 0.125f);
			AddQuad(out, c[0], c[1], c[2], c[3], inside, mat);
			AddQuad(out, c[4], c[5], c[6], c[7], inside, mat);
			for (int k = 0; k < 4; k++)
			{
				const int k1 = (k + 1) & 3;
				AddQuad(out, c[k], c[k1], c[4 + k1], c[4 + k], inside, mat);
			}
		}

		// Box (parallelepiped) from a centre and three half-extent vectors.
		void AddBox(std::vector<Tri>& out, const V3& centre, const V3& hx, const V3& hy, const V3& hz, u8 mat)
		{
			V3 c[8];
			for (int e = 0; e < 2; e++)
			{
				const V3 o = Add(centre, Mul(hz, (e == 0) ? -1.0f : 1.0f));
				c[e * 4 + 0] = Sub(Sub(o, hx), hy);
				c[e * 4 + 1] = Sub(Add(o, hx), hy);
				c[e * 4 + 2] = Add(Add(o, hx), hy);
				c[e * 4 + 3] = Add(Sub(o, hx), hy);
			}
			AddHex(out, c, mat);
		}

		void AddBoxAA(std::vector<Tri>& out, float cx, float cy, float cz, float hx, float hy, float hz, u8 mat)
		{
			AddBox(out, V3{cx, cy, cz}, V3{hx, 0.0f, 0.0f}, V3{0.0f, hy, 0.0f}, V3{0.0f, 0.0f, hz}, mat);
		}

		// Tapered box from base along dir for len: half width hw across, half thickness ht along top
		// (the back of a finger). The far end is scaled by taper.
		void AddSegment(std::vector<Tri>& out, const V3& base, const V3& dir_in, const V3& top_in, float len, float hw,
			float ht, float taper, u8 mat)
		{
			const V3 dir = Normalize(dir_in);
			const V3 top = Normalize(Sub(top_in, Mul(dir, Dot(top_in, dir))));
			const V3 side = Cross(top, dir);
			const V3 tip = Add(base, Mul(dir, len));
			V3 c[8];
			for (int e = 0; e < 2; e++)
			{
				const V3 o = (e == 0) ? base : tip;
				const float sc = (e == 0) ? 1.0f : taper;
				for (int k = 0; k < 4; k++)
				{
					const float sx = (k == 1 || k == 2) ? hw : -hw;
					const float sy = (k >= 2) ? ht : -ht;
					c[e * 4 + k] = Add(o, Add(Mul(side, sx * sc), Mul(top, sy * sc)));
				}
			}
			AddHex(out, c, mat);
		}

		struct FingerDef
		{
			V3 knuckle;
			float len[3];
			float hw;
		};

		// Index (top, nearest the thumb) to little. The knuckles sit on the right of the handle, just in
		// front of it, so the curled fingers wrap round it toward -X.
		constexpr FingerDef kFingers[4] = {
			{{2.8f, 3.0f, -2.2f}, {4.3f, 2.6f, 2.1f}, 0.95f},
			{{2.9f, 1.0f, -2.5f}, {4.7f, 2.9f, 2.2f}, 1.0f},
			{{2.8f, -1.0f, -2.3f}, {4.4f, 2.8f, 2.1f}, 0.95f},
			{{2.6f, -2.8f, -1.8f}, {3.5f, 2.2f, 1.9f}, 0.85f},
		};

		// Thumb from its base on the palm: resting on top of the controller, from the web of the hand
		// along the left of the pistol frame, or wrapped over the closed fingers of a fist.
		constexpr float kThumbLen[3] = {3.6f, 3.0f, 2.5f};
		constexpr V3 kThumbRestBase = {1.9f, 3.4f, 1.6f};
		constexpr V3 kThumbRest[3] = {{-0.47f, 0.55f, -0.69f}, {-0.35f, 0.10f, -0.93f}, {-0.15f, -0.12f, -0.98f}};
		constexpr V3 kThumbGunBase = {1.6f, 4.6f, 2.2f};
		constexpr V3 kThumbGun[3] = {{-0.80f, 0.05f, -0.60f}, {-0.25f, -0.05f, -0.97f}, {-0.10f, -0.10f, -0.99f}};
		constexpr V3 kThumbFistBase = {1.6f, 3.6f, 1.6f};
		constexpr V3 kThumbFist[3] = {{-0.75f, -0.05f, -0.66f}, {-0.55f, -0.25f, -0.80f}, {-0.05f, -0.45f, -0.89f}};

		// How the hand closes: round the controller (the index on its trigger, the thumb resting on top, the
		// controller's handle and head in the fist), round a gun's grip (the index inside the trigger guard, the
		// thumb along the frame, the grip in the fist), or into a fist round whatever the weapon builders put
		// there (a melee weapon's shaft, a can, something to throw, a long gun's fore-end): every finger closed
		// and the thumb over them, grip and trigger aside.
		enum class Pose : u8
		{
			Controller,
			Gun,
			Fist,
		};

		// A gun's grip, in the handle frame: in the fist (BuildHand's Gun pose), or alone in the holster.
		void AddGunGrip(std::vector<Tri>& out)
		{
			AddBoxAA(out, 0.0f, -0.6f, 0.0f, 1.35f, 5.2f, 2.0f, kMatGunFrame);
		}

		// Right hand in the handle frame (handle along +Y through the origin, -Z forward, back of the
		// hand toward +X); BuildWorld tilts it into the pointing frame.
		void BuildHand(std::vector<Tri>& out, float grip, float trigger, Pose pose)
		{
			const V3 up = V3{0.0f, 1.0f, 0.0f};
			grip = std::isfinite(grip) ? std::clamp(grip, 0.0f, 1.0f) : 0.0f;
			trigger = std::isfinite(trigger) ? std::clamp(trigger, 0.0f, 1.0f) : 0.0f;

			// Joint bends in degrees (knuckle, middle, tip). Middle/ring/little stay wrapped round the
			// handle even when the grip isn't squeezed. The index rests round the trigger and curls in
			// as it is pulled; on the pistol it stays inside the guard. A fist closes all four.
			const float curl = (pose == Pose::Fist) ? 1.0f : (0.8f + 0.2f * grip);
			const float rest_deg[3] = {78.0f * curl, 95.0f * curl, 62.0f * curl};
			float index_deg[3];
			if (pose == Pose::Gun)
			{
				index_deg[0] = 30.0f + 10.0f * trigger;
				index_deg[1] = 95.0f + 15.0f * trigger;
				index_deg[2] = 35.0f + 15.0f * trigger;
			}
			else if (pose == Pose::Fist)
			{
				for (int k = 0; k < 3; k++)
					index_deg[k] = rest_deg[k];
			}
			else
			{
				index_deg[0] = 25.0f + 20.0f * trigger;
				index_deg[1] = 80.0f + 20.0f * trigger;
				index_deg[2] = 35.0f + 20.0f * trigger;
			}
			for (int f = 0; f < 4; f++)
			{
				const FingerDef& fd = kFingers[f];
				const float* bend_deg = (f == 0) ? index_deg : rest_deg;
				V3 joint = fd.knuckle;
				V3 dir = V3{0.0f, 0.0f, -1.0f};
				V3 top = V3{1.0f, 0.0f, 0.0f};
				for (int sgm = 0; sgm < 3; sgm++)
				{
					// Curling turns the finger about the handle axis, from -Z toward -X (the palm side).
					const float bend = bend_deg[sgm] * kDegToRad;
					dir = RotateAxis(dir, up, bend);
					top = RotateAxis(top, up, bend);
					// Start a little behind the joint so the outside of the bend shows no gap.
					const float overlap = kFingerHalfT * 0.7f;
					AddSegment(out, Sub(joint, Mul(dir, overlap)), dir, top, fd.len[sgm] + overlap, fd.hw, kFingerHalfT,
						(sgm == 2) ? 0.78f : 0.94f, kMatGlove);
					if (sgm == 0)
					{
						// Lighter knuckle pad on the back of the glove.
						const V3 pad = Add(joint, Mul(top, kFingerHalfT + 0.15f));
						AddSegment(out, Sub(pad, Mul(dir, 0.8f)), dir, top, 1.6f, fd.hw * 0.85f, 0.35f, 0.9f, kMatKnuckle);
					}
					joint = Add(joint, Mul(dir, fd.len[sgm]));
				}
			}

			// Palm: ~9 wide (along the knuckle line), 3 thick, ~9 long, skewed so it runs back and down
			// from the knuckles; then a cuff that narrows a little at the wrist end.
			const float drop = kPalmDropDeg * kDegToRad;
			const V3 back = V3{0.0f, -std::sin(drop), std::cos(drop)};
			const V3 knuckle_line = V3{2.8f, 0.1f, -1.9f};
			constexpr float kPalmLen = 8.6f;
			AddBox(out, Add(knuckle_line, Mul(back, kPalmLen * 0.5f)), V3{1.5f, 0.0f, 0.0f}, V3{0.0f, 4.4f, 0.0f},
				Mul(back, kPalmLen * 0.5f), kMatGlove);
			AddSegment(out, Add(knuckle_line, Mul(back, kPalmLen - 0.6f)), back, V3{1.0f, 0.0f, 0.0f}, 3.0f, 3.8f, 1.9f,
				0.92f, kMatCuff);

			const V3* thumb = (pose == Pose::Gun) ? kThumbGun : (pose == Pose::Fist) ? kThumbFist : kThumbRest;
			const V3 thumb_top = V3{0.55f, 1.0f, 0.25f};
			V3 tj = (pose == Pose::Gun) ? kThumbGunBase : (pose == Pose::Fist) ? kThumbFistBase : kThumbRestBase;
			for (int sgm = 0; sgm < 3; sgm++)
			{
				const V3 d = Normalize(thumb[sgm]);
				const float overlap = (sgm == 0) ? 0.0f : 0.7f;
				AddSegment(out, Sub(tj, Mul(d, overlap)), d, thumb_top, kThumbLen[sgm] + overlap, (sgm == 0) ? 1.25f : 1.05f,
					(sgm == 0) ? 1.15f : 0.98f, (sgm == 2) ? 0.8f : 0.95f, kMatGlove);
				tj = Add(tj, Mul(d, kThumbLen[sgm]));
			}

			if (pose == Pose::Controller)
			{
				// A hint of the controller: the handle in the fist and the head the thumb rests on.
				AddBoxAA(out, 0.0f, -0.6f, 0.0f, 1.3f, 5.0f, 1.6f, kMatHandle);
				AddBoxAA(out, -0.3f, 4.5f, -1.0f, 1.9f, 0.6f, 2.9f, kMatHandle);
			}
			else if (pose == Pose::Gun)
			{
				// The gun's grip in the fist (the rest of a pistol is in BuildGunTop, of a long gun in BuildLongGun).
				AddGunGrip(out);
			}
		}

		// The rest of the pistol, in the pointing frame itself so the barrel runs along -Z. slide_cm moves the
		// slide and its sights back (+Z) as it cycles or locks open; flash adds a muzzle flash ahead of it.
		void BuildGunTop(std::vector<Tri>& out, float slide_cm, bool flash)
		{
			const float s = slide_cm;
			AddBoxAA(out, 0.0f, 6.4f, -5.0f + s, 1.5f, 1.75f, 9.0f, kMatGunSlide); // slide, 3 x 3.5 x 18
			AddBoxAA(out, 0.0f, 4.2f, -7.6f, 1.25f, 0.6f, 5.2f, kMatGunFrame); // dust cover under it
			AddBoxAA(out, 0.0f, 0.4f, -5.6f, 0.45f, 0.3f, 2.6f, kMatGunFrame); // trigger guard, bottom
			AddBoxAA(out, 0.0f, 2.2f, -8.0f, 0.45f, 1.8f, 0.3f, kMatGunFrame); // trigger guard, front
			AddBoxAA(out, 0.0f, 2.0f, -4.6f, 0.35f, 1.0f, 0.35f, kMatGunSlide); // trigger
			AddBoxAA(out, 0.0f, 6.7f, -14.06f, 0.45f, 0.45f, 0.08f, kMatGunFrame); // muzzle
			AddBoxAA(out, 0.0f, 8.35f, -13.0f + s, 0.22f, 0.2f, 0.35f, kMatGunFrame); // front sight (on the slide)
			AddBoxAA(out, 0.0f, 8.35f, 3.0f + s, 0.75f, 0.2f, 0.3f, kMatGunFrame); // rear sight
			if (flash)
			{
				// A small four-point star just past the muzzle: two thin crossed bars, 3 x 0.4 x 0.4.
				AddBoxAA(out, 0.0f, 6.7f, -14.5f, 1.5f, 0.2f, 0.2f, kMatFlash);
				AddBoxAA(out, 0.0f, 6.7f, -14.5f, 0.2f, 1.5f, 0.2f, kMatFlash);
			}
		}

		// A long gun round the grip in the fist (BuildHand's Gun pose), in the pointing frame: the receiver over the
		// hand, the trigger and its guard, a short stock back to about the wrist, the barrel along the pistol's line
		// to the muzzle, and the style's fore-end (where the left hand closes), tube, scope or magazine; flash adds a
		// muzzle flash past it.
		void BuildLongGun(std::vector<Tri>& out, u16 item_id, bool flash)
		{
			const LongGunDef& g = LongGunFor(item_id);
			const bool launcher = (g.style == LongStyle::Launcher);
			const u8 furniture = (g.style == LongStyle::Shotgun || g.style == LongStyle::Rifle) ? kMatWood : kMatGunFrame;
			AddBoxAA(out, 0.0f, 6.0f, -5.0f, 1.7f, 2.0f, 9.0f, kMatGunFrame); // receiver, 3.4 x 4 x 18
			AddBoxAA(out, 0.0f, 0.4f, -5.6f, 0.45f, 0.3f, 2.6f, kMatGunFrame); // trigger guard, bottom
			AddBoxAA(out, 0.0f, 2.2f, -8.0f, 0.45f, 1.8f, 0.3f, kMatGunFrame); // trigger guard, front
			AddBoxAA(out, 0.0f, 2.0f, -4.6f, 0.35f, 1.0f, 0.35f, kMatGunSlide); // trigger
			// The stock: from the back of the receiver down a little to about the wrist, broadening toward the butt.
			AddSegment(out, V3{0.0f, 5.8f, 3.0f}, V3{0.0f, -0.22f, 1.0f}, V3{0.0f, 1.0f, 0.0f}, 9.0f, 1.3f, 1.8f, 1.15f,
				furniture);
			const float r = launcher ? 2.0f : (g.style == LongStyle::Rifle) ? 0.6f : 0.75f;
			const float barrel = kReceiverFrontCm - g.muzzle_cm;
			AddBoxAA(out, 0.0f, kBoreYCm, kReceiverFrontCm - 0.5f * barrel, r, r, 0.5f * barrel,
				launcher ? kMatGunFrame : kMatGunSlide);
			switch (g.style)
			{
				case LongStyle::Shotgun:
					// A magazine tube under the barrel, and the pump round it.
					AddBoxAA(out, 0.0f, 4.6f, 0.5f * (kReceiverFrontCm + g.muzzle_cm + 7.0f), 0.6f, 0.6f,
						0.5f * (barrel - 7.0f), kMatGunFrame);
					AddBoxAA(out, 0.0f, 4.0f, g.fore_end_cm, 1.6f, 1.5f, 7.5f, furniture);
					break;
				case LongStyle::Rifle:
					// A long fore-end, and a scope over the receiver.
					AddBoxAA(out, 0.0f, 4.9f, g.fore_end_cm, 1.5f, 1.3f, 11.0f, furniture);
					AddBoxAA(out, 0.0f, 9.6f, -6.0f, 0.9f, 0.9f, 7.0f, kMatGunFrame);
					AddBoxAA(out, 0.0f, 9.6f, -13.5f, 1.2f, 1.2f, 0.6f, kMatGunFrame);
					break;
				case LongStyle::Assault:
					// A handguard round the barrel, and a magazine under the receiver, raked forward.
					AddBoxAA(out, 0.0f, 5.9f, g.fore_end_cm, 1.5f, 1.6f, 7.0f, furniture);
					AddSegment(out, V3{0.0f, 4.2f, -10.5f}, V3{0.0f, -1.0f, -0.25f}, V3{0.0f, 0.0f, -1.0f}, 8.0f, 1.0f, 1.6f,
						1.0f, kMatGunFrame);
					break;
				default:
					// A fore-grip under the fat barrel.
					AddBoxAA(out, 0.0f, 2.6f, g.fore_end_cm, 1.0f, 2.2f, 1.3f, furniture);
					break;
			}
			// The muzzle ring, its face at muzzle_cm (LaserFrom), a front bead, and a rear sight.
			AddBoxAA(out, 0.0f, kBoreYCm, g.muzzle_cm + 0.12f, r + 0.1f, r + 0.1f, 0.12f, kMatGunFrame);
			if (!launcher)
				AddBoxAA(out, 0.0f, kBoreYCm + r + 0.3f, g.muzzle_cm + 1.0f, 0.2f, 0.3f, 0.3f, kMatGunFrame);
			AddBoxAA(out, 0.0f, 8.3f, 1.5f, 0.7f, 0.3f, 0.4f, kMatGunFrame);
			if (flash)
			{
				// A bigger four-point star than the pistol's, just past the muzzle.
				const float z = g.muzzle_cm - 0.7f;
				AddBoxAA(out, 0.0f, kBoreYCm, z, 2.2f, 0.3f, 0.3f, kMatFlash);
				AddBoxAA(out, 0.0f, kBoreYCm, z, 0.3f, 2.2f, 0.3f, kMatFlash);
			}
		}

		// A melee weapon through the fist (BuildHand's Fist pose), in the pointing frame: along -Z from its butt just
		// behind the middle of the fist, as long as MeleeFor says, with its blade or head at the far end.
		void BuildMelee(std::vector<Tri>& out, u16 item_id)
		{
			const MeleeDef& m = MeleeFor(item_id);
			const V3 fwd = V3{0.0f, 0.0f, -1.0f};
			const V3 vert = V3{0.0f, 1.0f, 0.0f};
			const float y = kMeleeAxisYCm;
			const float zb = kMeleeButtCm;
			const float zt = zb - m.length_cm;
			const float zc = 0.5f * (zb + zt);
			const float half = 0.5f * m.length_cm;
			switch (m.style)
			{
				case MeleeStyle::Blade:
				case MeleeStyle::BroadBlade:
				{
					// A handle in the fist, a small guard just in front of it, and a blade narrowing to its point.
					constexpr float kHandleCm = 10.0f;
					AddBoxAA(out, 0.0f, y, zb - 0.5f * kHandleCm, 1.1f, 1.3f, 0.5f * kHandleCm, kMatHandle);
					AddBoxAA(out, 0.0f, y, zb - kHandleCm - 0.3f, 1.4f, 1.9f, 0.3f, kMatSteel);
					const float blade_half_h = (m.style == MeleeStyle::BroadBlade) ? 2.6f : 1.8f;
					AddSegment(out, V3{0.0f, y + 0.2f, zb - kHandleCm - 0.6f}, fwd, vert, m.length_cm - kHandleCm - 0.6f, 0.3f,
						blade_half_h, 0.35f, kMatSteel);
					break;
				}
				case MeleeStyle::Pipe:
					AddBoxAA(out, 0.0f, y, zc, 1.2f, 1.2f, half, kMatSteel);
					break;
				case MeleeStyle::BentPipe:
				{
					// Straight to kBendCm short of the tip, then turned up (starting a pipe's width back, so the joint
					// shows no gap).
					const float straight = m.length_cm - kBendCm;
					AddBoxAA(out, 0.0f, y, zb - 0.5f * straight, 1.2f, 1.2f, 0.5f * straight, kMatSteel);
					const float bend = kBendDeg * kDegToRad;
					AddSegment(out, V3{0.0f, y, zt + kBendCm + 1.2f}, V3{0.0f, std::sin(bend), -std::cos(bend)},
						V3{1.0f, 0.0f, 0.0f}, kBendCm + 1.2f, 1.2f, 1.2f, 1.0f, kMatSteel);
					break;
				}
				case MeleeStyle::Wrench:
					// A flat handle, and the head with its jaw at the far end.
					AddBoxAA(out, 0.0f, y, 0.5f * (zb + zt + 6.0f), 0.7f, 1.6f, 0.5f * (m.length_cm - 6.0f), kMatSteel);
					AddBoxAA(out, 0.0f, y + 1.5f, zt + 3.0f, 0.9f, 3.4f, 3.0f, kMatSteel);
					AddBoxAA(out, 0.0f, y + 4.0f, zt + 1.0f, 0.9f, 1.0f, 1.0f, kMatSteel);
					break;
				case MeleeStyle::Pole:
					AddBoxAA(out, 0.0f, y, zc, 1.4f, 1.4f, half, kMatWood);
					break;
				case MeleeStyle::Timber:
					AddBoxAA(out, 0.0f, y, zc, 2.0f, 2.0f, half, kMatWood);
					break;
				case MeleeStyle::Spear:
				{
					// A wooden shaft and a steel head narrowing to its point, the whole length together.
					constexpr float kHeadCm = 14.0f;
					AddBoxAA(out, 0.0f, y, 0.5f * (zb + zt + kHeadCm), 1.4f, 1.4f, 0.5f * (m.length_cm - kHeadCm), kMatWood);
					AddSegment(out, V3{0.0f, y, zt + kHeadCm + 0.5f}, fwd, vert, kHeadCm + 0.5f, 0.4f, 2.0f, 0.1f, kMatSteel);
					break;
				}
				case MeleeStyle::Axe:
					// The haft, the blade standing up from its end, and the poll below.
					AddBoxAA(out, 0.0f, y, zc, 1.2f, 1.6f, half, kMatWood);
					AddSegment(out, V3{0.0f, y + 1.0f, zt + 3.5f}, vert, fwd, 9.0f, 0.6f, 3.6f, 1.3f, kMatSteel);
					AddBoxAA(out, 0.0f, y - 1.5f, zt + 3.5f, 1.0f, 2.5f, 2.2f, kMatSteel);
					break;
				case MeleeStyle::Hammer:
					AddBoxAA(out, 0.0f, y, zc, 1.2f, 1.5f, half, kMatWood);
					AddBoxAA(out, 0.0f, y + 0.5f, zt + 2.5f, 2.2f, 5.5f, 2.5f, kMatSteel);
					break;
				case MeleeStyle::Crutch:
					// Two rails, and the pad at the far end.
					AddBoxAA(out, 1.6f, y, zc, 0.6f, 0.9f, half, kMatSteel);
					AddBoxAA(out, -1.6f, y, zc, 0.6f, 0.9f, half, kMatSteel);
					AddBoxAA(out, 0.0f, y, zt + 3.0f, 3.0f, 1.5f, 3.0f, kMatHandle);
					break;
				case MeleeStyle::Rod:
					// The rod, a grip round its end in the fist, and the head with two prongs.
					AddBoxAA(out, 0.0f, y, zc, 1.2f, 1.2f, half, kMatSteel);
					AddBoxAA(out, 0.0f, y, zb - 6.0f, 1.5f, 1.5f, 6.0f, kMatHandle);
					AddBoxAA(out, 0.0f, y, zt + 2.0f, 1.5f, 1.5f, 2.0f, kMatGunFrame);
					AddBoxAA(out, 0.8f, y, zt - 0.5f, 0.25f, 0.25f, 0.5f, kMatSteel);
					AddBoxAA(out, -0.8f, y, zt - 0.5f, 0.25f, 0.25f, 0.5f, kMatSteel);
					break;
				default:
					// The scrub brush: a handle, and the block with its bristles underneath.
					AddBoxAA(out, 0.0f, y, 0.5f * (zb + zt + 5.0f), 1.0f, 1.0f, 0.5f * (m.length_cm - 5.0f), kMatWood);
					AddBoxAA(out, 0.0f, y - 1.2f, zt + 2.5f, 3.0f, 1.2f, 2.5f, kMatWood);
					AddBoxAA(out, 0.0f, y - 3.4f, zt + 2.5f, 2.8f, 1.0f, 2.3f, kMatCloth);
					break;
			}
		}

		// Something to throw in the closed fist (BuildHand's Fist pose), in the handle frame: a molotov by its neck,
		// the bottle under the fist and the rag over it; the throwable stick through the fist; anything else (the
		// concrete piece) a lump in the palm.
		void BuildThrown(std::vector<Tri>& out, u16 item_id)
		{
			if (item_id == 114)
			{
				AddBoxAA(out, 0.0f, -0.5f, 0.0f, 0.9f, 4.5f, 0.9f, kMatGlass);
				AddBoxAA(out, 0.0f, -9.5f, 0.0f, 2.6f, 5.0f, 2.6f, kMatGlass);
				AddBox(out, V3{0.0f, -9.5f, 0.0f}, V3{1.84f, 0.0f, 1.84f}, V3{0.0f, 5.0f, 0.0f}, V3{-1.84f, 0.0f, 1.84f}, kMatGlass);
				AddSegment(out, V3{0.0f, 3.5f, 0.0f}, V3{0.15f, 1.0f, -0.2f}, V3{1.0f, 0.0f, 0.0f}, 4.5f, 0.9f, 0.6f, 0.6f,
					kMatCloth);
			}
			else if (item_id == 112)
			{
				AddBoxAA(out, 0.0f, 1.0f, 0.0f, 0.9f, 11.0f, 0.9f, kMatWood);
			}
			else
			{
				AddBox(out, V3{-0.6f, 1.2f, -1.2f}, V3{2.6f, 0.5f, 0.4f}, V3{-0.4f, 2.4f, 0.3f}, V3{0.3f, -0.2f, 2.5f},
					kMatStone);
				AddBox(out, V3{-0.2f, 2.6f, -0.6f}, V3{1.8f, -0.6f, 0.6f}, V3{0.5f, 1.6f, -0.3f}, V3{-0.6f, 0.2f, 1.9f},
					kMatStone);
			}
		}

		// A spray can in the closed fist (BuildHand's Fist pose), in the handle frame: an eight-sided body along the
		// handle axis, its cap and nozzle over the fist and its base rim under it.
		void BuildSpray(std::vector<Tri>& out)
		{
			AddBoxAA(out, 0.0f, 1.5f, 0.0f, 1.9f, 8.0f, 1.9f, kMatCan);
			AddBox(out, V3{0.0f, 1.5f, 0.0f}, V3{1.34f, 0.0f, 1.34f}, V3{0.0f, 8.0f, 0.0f}, V3{-1.34f, 0.0f, 1.34f}, kMatCan);
			AddBoxAA(out, 0.0f, 9.9f, 0.0f, 1.3f, 0.4f, 1.3f, kMatSteel);
			AddBoxAA(out, 0.0f, 10.7f, -0.6f, 0.45f, 0.4f, 0.9f, kMatGunFrame);
			AddBoxAA(out, 0.0f, -6.7f, 0.0f, 1.7f, 0.2f, 1.7f, kMatSteel);
		}

		std::vector<Tri> s_mesh;

		// Builds the hand into s_mesh and moves it into the base space (metres). Optionally returns the bounds in the
		// (mirrored, tilted) local frame, in centimetres: of everything (local_*), and of the part the eye has to stay
		// out of (core_*: all but a long gun's or a melee weapon's reach past the hand).
		void BuildWorld(int hand, const HandState& h, V3* local_min, V3* local_max, V3* core_min, V3* core_max)
		{
			const bool weapon = h.gun && hand == kRight;
			// A holstered weapon is drawn alone, as it lies in the fist but without the glove: a gun keeps the grip the
			// fist closes on; nothing in it moves (no slide, flash or kick).
			const bool holstered = weapon && h.holstered;
			const Held held = weapon ? h.held : Held::Pistol;
			Pose pose = Pose::Controller;
			if (weapon)
				pose = (held == Held::Pistol || held == Held::LongGun) ? Pose::Gun : Pose::Fist;
			else if (h.support && hand == kLeft)
				pose = Pose::Fist;
			s_mesh.clear();
			s_mesh.reserve(768);
			if (!holstered)
				BuildHand(s_mesh, h.grip, h.trigger, pose);
			else if (held == Held::Pistol || held == Held::LongGun)
				AddGunGrip(s_mesh);
			// What sits in the fist goes with the hand (the handle frame); what reaches out from it lies along the
			// pointing frame.
			if (weapon && held == Held::Thrown)
				BuildThrown(s_mesh, h.item_id);
			else if (weapon && held == Held::Spray)
				BuildSpray(s_mesh);
			const size_t tilted = s_mesh.size();
			float kick = 0.0f;
			if (weapon && held == Held::Pistol)
			{
				// The slide: back while it cycles after a shot, and held back on an empty gun.
				const float slide = (std::isfinite(h.slide) && !holstered) ? std::clamp(h.slide, 0.0f, 1.0f) : 0.0f;
				const bool locked = h.slide_locked && !holstered;
				BuildGunTop(s_mesh, kSlideTravelCm * std::max(slide, locked ? 1.0f : 0.0f), h.flash && !holstered);
				kick = (std::isfinite(h.kick) && !holstered) ? std::clamp(h.kick, 0.0f, 1.0f) : 0.0f;
			}
			else if (weapon && held == Held::LongGun)
			{
				BuildLongGun(s_mesh, h.item_id, h.flash && !holstered);
				kick = (std::isfinite(h.kick) && !holstered) ? std::clamp(h.kick, 0.0f, 1.0f) : 0.0f;
			}
			else if (weapon && held == Held::Melee)
			{
				BuildMelee(s_mesh, h.item_id);
			}
			// The core is what the eye must stay out of. A long gun's is its grip, in the hand or slung (the barrel of one
			// slung over the shoulder passes beside the head); a melee weapon alone in the holster has no hand part, but the
			// hip is far from the eye, so all of it is the core.
			const bool reach = weapon && (held == Held::LongGun || (held == Held::Melee && !holstered));
			const size_t core = reach ? tilted : s_mesh.size();
			// Recoil: the hand and gun pitch up about the grip origin and come back toward the wrist.
			const bool long_gun = weapon && held == Held::LongGun;
			const float ka = (long_gun ? kLongRecoilDeg : kRecoilDeg) * kick * kDegToRad;
			const float kc = std::cos(ka);
			const float ks = std::sin(ka);
			const float kback = (long_gun ? kLongRecoilBackCm : kRecoilBackCm) * kick;

			const float ta = -kHandleTiltDeg * kDegToRad;
			const float tc = std::cos(ta);
			const float ts = std::sin(ta);
			const float mx = (hand == kLeft) ? -1.0f : 1.0f;
			float q[4];
			NormalizedQuat(h, q);
			const V3 pos = Load(h.pos);
			V3 lmin = V3{1.0e9f, 1.0e9f, 1.0e9f};
			V3 lmax = V3{-1.0e9f, -1.0e9f, -1.0e9f};
			V3 cmin = lmin;
			V3 cmax = lmax;

			for (size_t i = 0; i < s_mesh.size(); i++)
			{
				Tri& t = s_mesh[i];
				const bool tilt = (i < tilted);
				const bool in_core = (i < core);
				// Handle frame -> pointing frame (top of the handle forward), mirrored for the left hand.
				const auto local = [&](const V3& v) {
					V3 r = v;
					if (tilt)
						r = V3{v.x, v.y * tc - v.z * ts, v.y * ts + v.z * tc};
					r.x *= mx;
					if (kick > 0.0f)
						r = V3{r.x, r.y * kc - r.z * ks, r.y * ks + r.z * kc};
					return r;
				};
				for (int k = 0; k < 3; k++)
				{
					V3 l = local(t.p[k]);
					l.z += kback;
					lmin = V3{std::min(lmin.x, l.x), std::min(lmin.y, l.y), std::min(lmin.z, l.z)};
					lmax = V3{std::max(lmax.x, l.x), std::max(lmax.y, l.y), std::max(lmax.z, l.z)};
					if (in_core)
					{
						cmin = V3{std::min(cmin.x, l.x), std::min(cmin.y, l.y), std::min(cmin.z, l.z)};
						cmax = V3{std::max(cmax.x, l.x), std::max(cmax.y, l.y), std::max(cmax.z, l.z)};
					}
					t.p[k] = Add(pos, Rotate(q, Mul(l, 0.01f)));
				}
				t.n = Rotate(q, local(t.n));
			}
			if (local_min)
				*local_min = lmin;
			if (local_max)
				*local_max = lmax;
			if (core_min)
				*core_min = cmin;
			if (core_max)
				*core_max = cmax;
		}

		// The frame of a quad through centre facing the eye: n toward the eye, up as near world up as it goes (any spin
		// in the plane is invisible: the image is drawn for this exact quad; looking straight down on it, -Z instead),
		// right = up x n. False when the eye is at the centre.
		bool FacingFrame(const V3& centre, const V3& e, float* dist, V3* n, V3* up, V3* right)
		{
			const V3 to_eye = Sub(e, centre);
			const float d = Length(to_eye);
			if (!(d > 1.0e-4f) || !std::isfinite(d))
				return false;
			const V3 nn = Mul(to_eye, 1.0f / d);
			V3 up_ref = V3{0.0f, 1.0f, 0.0f};
			if (std::fabs(Dot(up_ref, nn)) > 0.97f)
				up_ref = V3{0.0f, 0.0f, -1.0f};
			const V3 uu = Normalize(Sub(up_ref, Mul(nn, Dot(up_ref, nn))));
			*dist = d;
			*n = nn;
			*up = uu;
			*right = Cross(uu, nn);
			return true;
		}

		// Rotation matrix with columns r, u, n -> quaternion (xyzw).
		void QuatFromBasis(const V3& r, const V3& u, const V3& n, float out[4])
		{
			const float m00 = r.x, m01 = u.x, m02 = n.x;
			const float m10 = r.y, m11 = u.y, m12 = n.y;
			const float m20 = r.z, m21 = u.z, m22 = n.z;
			const float trace = m00 + m11 + m22;
			float x, y, z, w;
			if (trace > 0.0f)
			{
				const float s = std::sqrt(trace + 1.0f) * 2.0f;
				w = 0.25f * s;
				x = (m21 - m12) / s;
				y = (m02 - m20) / s;
				z = (m10 - m01) / s;
			}
			else if (m00 > m11 && m00 > m22)
			{
				const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
				w = (m21 - m12) / s;
				x = 0.25f * s;
				y = (m01 + m10) / s;
				z = (m02 + m20) / s;
			}
			else if (m11 > m22)
			{
				const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
				w = (m02 - m20) / s;
				x = (m01 + m10) / s;
				y = 0.25f * s;
				z = (m12 + m21) / s;
			}
			else
			{
				const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
				w = (m10 - m01) / s;
				x = (m02 + m20) / s;
				y = (m12 + m21) / s;
				z = 0.25f * s;
			}
			const float l = std::sqrt(x * x + y * y + z * z + w * w);
			out[0] = x / l;
			out[1] = y / l;
			out[2] = z / l;
			out[3] = w / l;
		}

		struct SV
		{
			float x, y, iz; // pixel position, 1 / view depth
		};

		struct Bounds
		{
			int x0, y0, x1, y1;
		};

		u32 PackLinear(const Tables& tab, float r, float g, float b)
		{
			const auto q = [&tab](float v) -> u32 {
				const float c = (v > 0.0f) ? std::min(v, 1.0f) : 0.0f; // also catches NaN
				return tab.to_srgb[static_cast<int>(c * 1024.0f + 0.5f)];
			};
			return (0xFFu << 24) | (q(b) << 16) | (q(g) << 8) | q(r);
		}

		// Flat shading per face: ambient (a little brighter from above), Lambert from a light above
		// and to the right of the viewer, a headlight, a rim on faces turning away, and a small highlight; all of it
		// times gain (a lit holstered weapon), the muzzle flash aside.
		u32 Shade(const Tables& tab, const Tri& t, const V3& to_eye, const V3& light, const V3& up, float gain)
		{
			const Material& m = tab.mat[t.mat];
			if (t.mat == kMatFlash)
				return PackLinear(tab, m.r, m.g, m.b); // unlit, opaque
			const V3 v = Normalize(to_eye);
			const float ndl = std::max(Dot(t.n, light), 0.0f);
			const float ndv = std::clamp(Dot(t.n, v), 0.0f, 1.0f);
			const float hemi = 0.5f + 0.5f * Dot(t.n, up);
			// The headlight term keeps faces turned to the viewer readable against a dark game scene.
			const float k = 0.22f + 0.12f * hemi + 0.70f * ndl + 0.22f * ndv;
			const float edge = 1.0f - ndv;
			const float rim = m.rim * edge * edge * edge;
			const float nh = std::max(Dot(t.n, Normalize(Add(light, v))), 0.0f);
			const float nh2 = nh * nh;
			const float nh4 = nh2 * nh2;
			const float nh8 = nh4 * nh4;
			const float nh16 = nh8 * nh8;
			const float spec = m.spec * nh16 * nh16; // pow 32
			return PackLinear(tab, (m.r * k + rim * 0.50f + spec) * gain, (m.g * k + rim * 0.62f + spec) * gain,
				(m.b * k + rim * 0.56f + spec) * gain);
		}

		// Fills one screen-space triangle with a depth test (larger 1/z is nearer).
		void RasterTri(const SV& a, const SV& b_in, const SV& c_in, u32 colour, float* depth, u32* px, Bounds& bounds)
		{
			const SV* b = &b_in;
			const SV* c = &c_in;
			float area = (b->x - a.x) * (c->y - a.y) - (b->y - a.y) * (c->x - a.x);
			if (area < 0.0f)
			{
				// Faces were culled with their normals; just make the edge functions positive inside.
				std::swap(b, c);
				area = -area;
			}
			if (area < 1.0e-6f)
				return;

			const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b->x, c->x}))));
			const int x1 = std::min(kSize - 1, static_cast<int>(std::ceil(std::max({a.x, b->x, c->x}))));
			const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b->y, c->y}))));
			const int y1 = std::min(kSize - 1, static_cast<int>(std::ceil(std::max({a.y, b->y, c->y}))));
			if (x0 > x1 || y0 > y1)
				return;

			// Edge function opposite each vertex (its barycentric weight times area), stepped per pixel.
			const float sa_x = -(c->y - b->y), sa_y = c->x - b->x;
			const float sb_x = -(a.y - c->y), sb_y = a.x - c->x;
			const float sc_x = -(b->y - a.y), sc_y = b->x - a.x;
			const float px0 = static_cast<float>(x0) + 0.5f;
			const float py0 = static_cast<float>(y0) + 0.5f;
			float ra = (c->x - b->x) * (py0 - b->y) - (c->y - b->y) * (px0 - b->x);
			float rb = (a.x - c->x) * (py0 - c->y) - (a.y - c->y) * (px0 - c->x);
			float rc = (b->x - a.x) * (py0 - a.y) - (b->y - a.y) * (px0 - a.x);
			const float inv_area = 1.0f / area;
			const float za = a.iz * inv_area;
			const float zb = b->iz * inv_area;
			const float zc = c->iz * inv_area;
			bool any = false;
			for (int y = y0; y <= y1; y++)
			{
				float ea = ra, eb = rb, ec = rc;
				float* drow = depth + static_cast<size_t>(y) * kImageSize;
				u32* prow = px + static_cast<size_t>(y) * kImageSize;
				for (int x = x0; x <= x1; x++)
				{
					if (ea >= 0.0f && eb >= 0.0f && ec >= 0.0f)
					{
						const float iz = ea * za + eb * zb + ec * zc;
						if (iz > drow[x])
						{
							drow[x] = iz;
							prow[x] = colour;
							any = true;
						}
					}
					ea += sa_x;
					eb += sb_x;
					ec += sc_x;
				}
				ra += sa_y;
				rb += sb_y;
				rc += sc_y;
			}
			if (any)
			{
				bounds.x0 = std::min(bounds.x0, x0);
				bounds.y0 = std::min(bounds.y0, y0);
				bounds.x1 = std::max(bounds.x1, x1);
				bounds.y1 = std::max(bounds.y1, y1);
			}
		}

		std::vector<float> s_depth;
		std::vector<u8> s_mask;

		// Cheap edge anti-aliasing: alpha is a 3x3 tent filter of the coverage, and the pixels just
		// outside the silhouette take their covered neighbours' colour (alpha is unpremultiplied).
		void SoftenEdges(u32* px, const Bounds& bounds)
		{
			constexpr int kStride = kSize + 2;
			s_mask.assign(static_cast<size_t>(kStride) * kStride, 0);
			for (int y = bounds.y0; y <= bounds.y1; y++)
			{
				const float* drow = s_depth.data() + static_cast<size_t>(y) * kImageSize;
				u8* mrow = s_mask.data() + static_cast<size_t>(y + 1) * kStride + 1;
				for (int x = bounds.x0; x <= bounds.x1; x++)
					mrow[x] = static_cast<u8>((drow[x] > 0.0f) ? 1 : 0);
			}

			const int x0 = std::max(bounds.x0 - 1, 0);
			const int x1 = std::min(bounds.x1 + 1, kSize - 1);
			const int y0 = std::max(bounds.y0 - 1, 0);
			const int y1 = std::min(bounds.y1 + 1, kSize - 1);
			for (int y = y0; y <= y1; y++)
			{
				const u8* m = s_mask.data() + static_cast<size_t>(y + 1) * kStride + 1;
				u32* prow = px + static_cast<size_t>(y) * kImageSize;
				for (int x = x0; x <= x1; x++)
				{
					const u8* mc = m + x;
					const int w = 4 * mc[0] + 2 * (mc[-1] + mc[1] + mc[-kStride] + mc[kStride]) +
								  (mc[-kStride - 1] + mc[-kStride + 1] + mc[kStride - 1] + mc[kStride + 1]);
					if (w == 0 || w == 16)
						continue;
					const u32 alpha = static_cast<u32>(w) * 255u / 16u;
					if (mc[0])
					{
						prow[x] = (prow[x] & 0x00FFFFFFu) | (alpha << 24);
						continue;
					}
					// Outside: average the covered neighbours' colour.
					u32 r = 0, g = 0, b = 0, n = 0;
					for (int dy = -1; dy <= 1; dy++)
					{
						for (int dx = -1; dx <= 1; dx++)
						{
							const int nx = x + dx;
							const int ny = y + dy;
							if (nx < 0 || ny < 0 || nx >= kSize || ny >= kSize || !mc[dy * kStride + dx])
								continue;
							const u32 c = px[static_cast<size_t>(ny) * kImageSize + nx];
							r += c & 0xFFu;
							g += (c >> 8) & 0xFFu;
							b += (c >> 16) & 0xFFu;
							n++;
						}
					}
					if (n == 0)
						continue;
					prow[x] = (alpha << 24) | ((b / n) << 16) | ((g / n) << 8) | (r / n);
				}
			}
		}
	}

	// ---- [VR] GameWeaponModels: the hand model and the game's own weapon models, drawn with MeshRaster ----
	//
	// The hand is HandMesh's (already in the pointing frame, metres). The weapon is the GameModels model of the item,
	// placed the way the game attaches it to the character's hand (GameAttach): the game's hand bone frame is mapped onto
	// the hand model, its wrist joint on HandMesh's wrist anchor and its axes onto the pointing frame (the bone's -X, wrist
	// to fingers, along -Z; its +Z, the thumb's side, along +Y; its -Y, the palm, along -X: the hand model's own layout).
	// A gun's barrel is then turned onto -Z (the controller's aim, where the laser and the game's shot go): for the
	// pistols the game's turn is none and nothing moves; a long gun the game holds a few degrees off its hand bone is
	// turned that much about the grip position. Melee weapons, things to throw and spray cans keep the game's turn.
	namespace
	{
		// The game's hand bone frame is in Kevin's units: his skeleton is drawn 1.05 times its size (the node matrices'
		// scale), which scales the grip offset but not the weapon. His hand, wrist to knuckles, is then 9.9 cm, as the hand
		// model's is.
		constexpr float kGameHandScale = 1.05f;
		// A long gun or a melee weapon drawn with its game model may need a bigger quad than the placeholder's: the game
		// holds a shaft across the fist, so a pole is seen side on (the scrub brush is 1.4 m long). 1.6 m at the hand's
		// distance covers the headset's view round it; what reaches past that is cut at the image's edge, out of sight.
		constexpr float kQuadSizeModelLongM = 1.60f;
		// A melee weapon alone in the hip holster hangs from the belt like a sword in a frog: moved down its shaft until its
		// top end is at most this far above the holster's middle (a 1.4 m broom held at its grip would reach the head).
		constexpr float kHolsterTopCm = 12.0f;
		// 2 x 2 samples a pixel; PCSX2_VR_MESH_SAMPLES=1 or 4 (read once) for a cost or quality comparison.
		u32 MeshSamples()
		{
			static const u32 s_n = [] {
				const char* v = std::getenv("PCSX2_VR_MESH_SAMPLES");
				const int n = v ? std::atoi(v) : 2;
				return static_cast<u32>((n == 1 || n == 4) ? n : 2);
			}();
			return s_n;
		}

		std::atomic<ModelLookup> s_model_lookup{nullptr};

		const GameModels::Model* LookupModel(u16 item_id)
		{
			const ModelLookup f = s_model_lookup.load(std::memory_order_acquire);
			return f ? f(item_id) : GameModels::Find(item_id);
		}

		// Items whose weapon fell back to the placeholder, logged once each (bit set: logged).
		std::mutex s_fallback_mutex;
		std::vector<u8> s_fallback_logged;

		void LogFallbackOnce(u16 item_id, const char* why)
		{
			std::lock_guard<std::mutex> lock(s_fallback_mutex);
			if (s_fallback_logged.empty())
				s_fallback_logged.assign(65536 / 8, 0);
			u8& bits = s_fallback_logged[item_id >> 3];
			const u8 bit = static_cast<u8>(1u << (item_id & 7));
			if (bits & bit)
				return;
			bits |= bit;
			const char* name = ItemData::Name(item_id);
			Console.WriteLn("(VR) Hands: item %u (%s) keeps the placeholder shape: %s.", static_cast<unsigned>(item_id),
				name[0] ? name : "not in the item table", why);
		}

		std::atomic<u32> s_mesh_images{0};
		std::atomic<u64> s_mesh_total_us{0};
		std::atomic<u32> s_mesh_max_us{0};

		// A 3 x 3 linear map by its columns.
		struct M3
		{
			V3 c[3];
			V3 Apply(const V3& v) const { return Add(Add(Mul(c[0], v.x), Mul(c[1], v.y)), Mul(c[2], v.z)); }
		};

		// The standard rotations of column vectors about X, Y, Z by a (radians): the transposes of the game's row-vector
		// ones, so the game's v * Rx(x) * Ry(y) * Rz(z) is RotZ(z) * RotY(y) * RotX(x) * v here.
		M3 RotX(float a)
		{
			const float c = std::cos(a), s = std::sin(a);
			return M3{{V3{1.0f, 0.0f, 0.0f}, V3{0.0f, c, s}, V3{0.0f, -s, c}}};
		}
		M3 RotY(float a)
		{
			const float c = std::cos(a), s = std::sin(a);
			return M3{{V3{c, 0.0f, -s}, V3{0.0f, 1.0f, 0.0f}, V3{s, 0.0f, c}}};
		}
		M3 RotZ(float a)
		{
			const float c = std::cos(a), s = std::sin(a);
			return M3{{V3{c, s, 0.0f}, V3{-s, c, 0.0f}, V3{0.0f, 0.0f, 1.0f}}};
		}
		M3 MulM3(const M3& a, const M3& b)
		{
			return M3{{a.Apply(b.c[0]), a.Apply(b.c[1]), a.Apply(b.c[2])}};
		}
		// The game's hand bone frame to the pointing frame: (x, y, z) -> (y, z, x).
		V3 BoneToPointing(const V3& g) { return V3{g.y, g.z, g.x}; }

		bool IsGun(Held held) { return held == Held::Pistol || held == Held::LongGun; }

		// The hand model's pose for a hand: a gun's grip (the index in the trigger guard), a fist round a shaft, a can or
		// a fore-end, or round the controller (the pistol pose, its index curling in as the trigger is pulled).
		void HandPoses(int hand, const HandState& h, HandMesh::Pose* a, HandMesh::Pose* b, float* t)
		{
			const bool weapon = h.gun && hand == kRight;
			*t = 0.0f;
			if (weapon)
			{
				*a = *b = IsGun(h.held) ? HandMesh::Pose::Pistol : HandMesh::Pose::Grip;
				return;
			}
			if (h.support && hand == kLeft)
			{
				*a = *b = HandMesh::Pose::Grip;
				return;
			}
			*a = HandMesh::Pose::Pistol;
			*b = HandMesh::Pose::Grip;
			*t = std::isfinite(h.trigger) ? std::clamp(h.trigger, 0.0f, 1.0f) : 0.0f;
		}

		// Where the game model sits in the pointing frame: point_cm = L * model_cm + T.
		struct Placement
		{
			M3 L;
			V3 T;
		};

		Placement PlaceModel(const GameAttach& at, Held held)
		{
			const M3 A = MulM3(RotZ(at.rot_rad[2]), MulM3(RotY(at.rot_rad[1]), RotX(at.rot_rad[0])));
			const HandMesh::Pose pose = IsGun(held) ? HandMesh::Pose::Pistol : HandMesh::Pose::Grip;
			const HandMesh::Anchors an = HandMesh::GetAnchors(kRight, pose);
			const V3 wrist_cm = Mul(Load(an.wrist), 100.0f);
			Placement p;
			for (int k = 0; k < 3; k++)
				p.L.c[k] = BoneToPointing(A.c[k]);
			p.T = Add(BoneToPointing(Mul(Load(at.grip_cm), kGameHandScale)), wrist_cm);
			if (IsGun(held))
			{
				// The barrel (the model's -X) onto -Z, turned about the grip position.
				const V3 b = Normalize(Mul(p.L.c[0], -1.0f));
				const V3 want = V3{0.0f, 0.0f, -1.0f};
				const float d = std::clamp(Dot(b, want), -1.0f, 1.0f);
				const V3 axis = Cross(b, want);
				const float s = Length(axis);
				if (s > 1.0e-5f && d < 0.999999f)
				{
					const V3 k = Mul(axis, 1.0f / s);
					const float angle = std::atan2(s, d);
					for (int c = 0; c < 3; c++)
						p.L.c[c] = RotateAxis(p.L.c[c], k, angle);
					p.T = RotateAxis(p.T, k, angle);
				}
			}
			return p;
		}

		// PlaceModel for what h draws: in the holster, a melee weapon is moved down its shaft (the pointing frame's +Y,
		// which HolsterPose hangs down) until its top end is within kHolsterTopCm of the hold point.
		Placement PlaceFor(const HandState& h, const GameModels::Model& m)
		{
			Placement p = PlaceModel(h.attach, h.held);
			if (h.holstered && h.held == Held::Melee)
			{
				float top = 1.0e9f;
				for (int c = 0; c < 8; c++)
				{
					const V3 v = V3{(c & 1) ? m.bounds_max[0] : m.bounds_min[0], (c & 2) ? m.bounds_max[1] : m.bounds_min[1],
						(c & 4) ? m.bounds_max[2] : m.bounds_min[2]};
					top = std::min(top, Add(p.L.Apply(v), p.T).y);
				}
				if (top < -kHolsterTopCm)
					p.T.y += -kHolsterTopCm - top;
			}
			return p;
		}

		// A pistol's slide (its part in skin slot 2) runs back along the barrel as it cycles; not a revolver's cylinder.
		bool HasSlide(u16 item_id) { return item_id != 4 && item_id != 6 && item_id != 7; }

		// Kick (recoil) in the pointing frame, cm: pitched up about the grip and moved back, as BuildWorld does.
		struct Kick
		{
			float c = 1.0f, s = 0.0f, back_cm = 0.0f;
			V3 Apply(const V3& v) const { return V3{v.x, v.y * c - v.z * s, v.y * s + v.z * c + back_cm}; }
		};

		Kick KickOf(int hand, const HandState& h)
		{
			Kick k;
			const bool weapon = h.gun && hand == kRight && !h.holstered;
			if (!weapon || !IsGun(h.held))
				return k;
			const float kick = std::isfinite(h.kick) ? std::clamp(h.kick, 0.0f, 1.0f) : 0.0f;
			const bool long_gun = (h.held == Held::LongGun);
			const float a = (long_gun ? kLongRecoilDeg : kRecoilDeg) * kick * kDegToRad;
			k.c = std::cos(a);
			k.s = std::sin(a);
			k.back_cm = (long_gun ? kLongRecoilBackCm : kRecoilBackCm) * kick;
			return k;
		}

		float SlideCm(const HandState& h)
		{
			if (h.holstered || h.held != Held::Pistol || !HasSlide(h.item_id))
				return 0.0f;
			const float slide = std::isfinite(h.slide) ? std::clamp(h.slide, 0.0f, 1.0f) : 0.0f;
			return kSlideTravelCm * std::max(slide, h.slide_locked ? 1.0f : 0.0f);
		}

		// The hand model's bounds per side and pose, cm.
		struct PoseBounds
		{
			V3 lo[2][HandMesh::kPoseCount];
			V3 hi[2][HandMesh::kPoseCount];
		};

		const PoseBounds& GetPoseBounds()
		{
			static const PoseBounds s_b = [] {
				PoseBounds b;
				for (int side = 0; side < 2; side++)
				{
					for (int p = 0; p < HandMesh::kPoseCount; p++)
					{
						const HandMesh::Mesh& m = HandMesh::Get(side, static_cast<HandMesh::Pose>(p));
						V3 lo = V3{1.0e9f, 1.0e9f, 1.0e9f};
						V3 hi = V3{-1.0e9f, -1.0e9f, -1.0e9f};
						for (u32 i = 0; i < m.vertex_count; i++)
						{
							const V3 v = Mul(Load(m.position + i * 3), 100.0f);
							lo = V3{std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
							hi = V3{std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
						}
						if (m.vertex_count == 0)
							lo = hi = V3{0.0f, 0.0f, 0.0f};
						b.lo[side][p] = lo;
						b.hi[side][p] = hi;
					}
				}
				return b;
			}();
			return s_b;
		}

		void Grow(V3& lo, V3& hi, const V3& v)
		{
			lo = V3{std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
			hi = V3{std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
		}

		// The muzzle of the game model in the pointing frame (cm, before any kick), from its weapon-type entry.
		bool ModelMuzzleCm(const HandState& h, V3* out)
		{
			if (!h.attach.valid || !h.attach.has_muzzle || !IsGun(h.held))
				return false;
			const Placement p = PlaceModel(h.attach, h.held);
			*out = Add(p.L.Apply(Load(h.attach.muzzle_cm)), p.T);
			return true;
		}

		// PlaceImpostor's bounds for the mesh look, as BuildWorld gives them for the placeholder: everything drawn, and the
		// part the eye must stay out of, in the pointing frame (cm, kicked).
		bool MeshBounds(int hand, const HandState& h, V3* lmin, V3* lmax, V3* cmin, V3* cmax)
		{
			const bool weapon = h.gun && hand == kRight;
			const Kick kick = KickOf(hand, h);
			V3 lo = V3{1.0e9f, 1.0e9f, 1.0e9f};
			V3 hi = V3{-1.0e9f, -1.0e9f, -1.0e9f};
			V3 clo = lo;
			V3 chi = hi;
			const int side = (hand == kLeft) ? kLeft : kRight;
			if (!(weapon && h.holstered))
			{
				HandMesh::Pose pa, pb;
				float t;
				HandPoses(hand, h, &pa, &pb, &t);
				const PoseBounds& pbnd = GetPoseBounds();
				for (const HandMesh::Pose p : {pa, pb})
				{
					const V3 a = pbnd.lo[side][static_cast<int>(p)];
					const V3 b = pbnd.hi[side][static_cast<int>(p)];
					for (int c = 0; c < 8; c++)
					{
						const V3 v = kick.Apply(V3{(c & 1) ? b.x : a.x, (c & 2) ? b.y : a.y, (c & 4) ? b.z : a.z});
						Grow(lo, hi, v);
						Grow(clo, chi, v);
					}
				}
			}
			if (weapon)
			{
				const GameModels::Model* m = LookupModel(h.item_id);
				if (!m)
					return false;
				const Placement p = PlaceFor(h, *m);
				const bool reach = (h.held == Held::LongGun || h.held == Held::Melee);
				// Alone in the holster or the sling, a weapon that reaches far (a long gun, a pole) has no hand round it:
				// what the eye must stay out of is its hold point, as the placeholder's grip is.
				if (reach && h.holstered)
				{
					Grow(clo, chi, V3{-3.0f, -3.0f, -3.0f});
					Grow(clo, chi, V3{3.0f, 3.0f, 3.0f});
				}
				const float slide = SlideCm(h);
				for (int c = 0; c < 8; c++)
				{
					V3 v = V3{(c & 1) ? m->bounds_max[0] : m->bounds_min[0], (c & 2) ? m->bounds_max[1] : m->bounds_min[1],
						(c & 4) ? m->bounds_max[2] : m->bounds_min[2]};
					if (c & 1)
						v.x += slide;
					const V3 w = kick.Apply(Add(p.L.Apply(v), p.T));
					Grow(lo, hi, w);
					if (!reach)
						Grow(clo, chi, w);
				}
				V3 muzzle;
				if (h.flash && !h.holstered && ModelMuzzleCm(h, &muzzle))
				{
					for (int c = 0; c < 2; c++)
						Grow(lo, hi, kick.Apply(Add(muzzle, V3{0.0f, 0.0f, c ? -2.5f : 0.0f})));
				}
			}
			if (!(hi.x >= lo.x) || !(chi.x >= clo.x))
				return false;
			*lmin = lo;
			*lmax = hi;
			*cmin = clo;
			*cmax = chi;
			return true;
		}

		MeshRaster::Mat4 MatFromBasis(const V3& x, const V3& y, const V3& z, const V3& t)
		{
			MeshRaster::Mat4 m;
			const V3 cols[4] = {x, y, z, t};
			for (int c = 0; c < 4; c++)
			{
				m.m[c * 4 + 0] = cols[c].x;
				m.m[c * 4 + 1] = cols[c].y;
				m.m[c * 4 + 2] = cols[c].z;
				m.m[c * 4 + 3] = (c == 3) ? 1.0f : 0.0f;
			}
			return m;
		}

		// The pointing frame (metres) to the base space: the hand's pose, after the kick.
		MeshRaster::Mat4 HandWorld(const HandState& h, const Kick& kick)
		{
			float q[4];
			NormalizedQuat(h, q);
			const V3 x = Rotate(q, V3{1.0f, 0.0f, 0.0f});
			const V3 y = Rotate(q, V3{0.0f, kick.c, kick.s});
			const V3 z = Rotate(q, V3{0.0f, -kick.s, kick.c});
			const V3 t = Add(Load(h.pos), Rotate(q, V3{0.0f, 0.0f, kick.back_cm * 0.01f}));
			return MatFromBasis(x, y, z, t);
		}

		void SetLighting(MeshRaster::Lighting& l, bool lit)
		{
			l.enabled = true;
			l.light_in_view_space = true;
			l.light_dir[0] = 0.35f;
			l.light_dir[1] = 0.8f;
			l.light_dir[2] = 0.5f;
			l.ambient = lit ? 0.55f : 0.30f;
			l.key = 0.62f;
			l.headlight = lit ? 0.45f : 0.22f;
			l.rim = 0.30f;
			l.rim_power = 3;
			l.rim_colour[0] = 0.80f;
			l.rim_colour[1] = 0.92f;
			l.rim_colour[2] = 0.88f;
			l.linear_light = true;
		}

		struct MeshScratch
		{
			MeshRaster::Target target{kImageSize, kImageSize, MeshSamples()};
			HandMesh::Blend blend;
			std::vector<float> flash;
			std::vector<Tri> flash_tris;
		};

		MeshScratch& Scratch()
		{
			thread_local MeshScratch s;
			return s;
		}

		void RenderMesh(int hand, const HandState& h, const float eye[3], const float quad_center[3],
			const float quad_right[3], const float quad_up[3], float quad_size_m, std::vector<u32>& out_rgba)
		{
			Common::Timer timer;
			MeshScratch& sc = Scratch();
			MeshRaster::Camera cam;
			if (!MeshRaster::Camera::Impostor(eye, quad_center, quad_right, quad_up, quad_size_m, kNearM, 100.0f, &cam))
				return;
			sc.target.Clear();
			const bool weapon = h.gun && hand == kRight;
			const bool lit = weapon && h.holstered && h.lit;
			const Kick kick = KickOf(hand, h);
			const MeshRaster::Mat4 world = HandWorld(h, kick);
			const int side = (hand == kLeft) ? kLeft : kRight;

			if (!(weapon && h.holstered))
			{
				HandMesh::Pose pa, pb;
				float t;
				HandPoses(hand, h, &pa, &pb, &t);
				const HandMesh::Mesh& hm = HandMesh::Lerp(side, pa, pb, t, sc.blend);
				MeshRaster::Mesh mesh;
				mesh.positions = hm.position;
				mesh.normals = hm.normal;
				mesh.vertex_count = hm.vertex_count;
				mesh.indices16 = hm.index;
				mesh.index_count = hm.triangle_count * 3;
				MeshRaster::Material mat;
				// A light skin tone (the characters' bare hands), lit as the placeholder glove is.
				mat.base_colour[0] = 0.72f;
				mat.base_colour[1] = 0.53f;
				mat.base_colour[2] = 0.43f;
				SetLighting(mat.lighting, false);
				mat.lighting.rim = 0.38f;
				MeshRaster::Draw(sc.target, cam, mesh, world, mat);
			}

			if (weapon)
			{
				const GameModels::Model* m = LookupModel(h.item_id);
				if (m)
				{
					const Placement p = PlaceFor(h, *m);
					const float slide_cm = SlideCm(h);
					for (const GameModels::Part& part : m->parts)
					{
						if (part.vertices.empty() || part.indices.empty())
							continue;
						// cm in the model -> m in the pointing frame (the slide moved back along the model's +X first).
						const V3 shift = (part.slot == 2) ? V3{slide_cm, 0.0f, 0.0f} : V3{0.0f, 0.0f, 0.0f};
						const MeshRaster::Mat4 local = MatFromBasis(Mul(p.L.c[0], 0.01f), Mul(p.L.c[1], 0.01f),
							Mul(p.L.c[2], 0.01f), Mul(Add(p.L.Apply(shift), p.T), 0.01f));
						MeshRaster::Mesh mesh;
						mesh.positions = part.vertices[0].pos;
						mesh.normals = part.vertices[0].normal;
						mesh.uvs = part.vertices[0].uv;
						mesh.position_stride = mesh.normal_stride = mesh.uv_stride = sizeof(GameModels::Vertex);
						mesh.vertex_count = static_cast<u32>(part.vertices.size());
						mesh.indices16 = part.indices.data();
						mesh.index_count = static_cast<u32>(part.indices.size());
						MeshRaster::Texture tex;
						MeshRaster::Material mat;
						if (part.texture && !part.texture->rgba.empty())
						{
							tex.texels = part.texture->rgba.data();
							tex.width = part.texture->width;
							tex.height = part.texture->height;
							tex.filter = MeshRaster::Texture::Filter::Nearest;
							mat.texture = &tex;
							mat.alpha_cutoff = 0.5f;
						}
						else
						{
							mat.base_colour[0] = mat.base_colour[1] = mat.base_colour[2] = 0.45f;
						}
						SetLighting(mat.lighting, lit);
						MeshRaster::Draw(sc.target, cam, mesh, MeshRaster::Multiply(world, local), mat);
					}
					V3 muzzle;
					if (h.flash && !h.holstered && ModelMuzzleCm(h, &muzzle))
					{
						// The placeholder's four-point star, just past the model's muzzle.
						sc.flash_tris.clear();
						const V3 c = Add(muzzle, V3{0.0f, 0.0f, -0.6f});
						const float r = (h.held == Held::LongGun) ? 2.2f : 1.5f;
						AddBoxAA(sc.flash_tris, c.x, c.y, c.z, r, 0.25f, 0.25f, kMatFlash);
						AddBoxAA(sc.flash_tris, c.x, c.y, c.z, 0.25f, r, 0.25f, kMatFlash);
						sc.flash.clear();
						for (const Tri& tr : sc.flash_tris)
						{
							for (int k = 0; k < 3; k++)
							{
								sc.flash.push_back(tr.p[k].x * 0.01f);
								sc.flash.push_back(tr.p[k].y * 0.01f);
								sc.flash.push_back(tr.p[k].z * 0.01f);
							}
						}
						MeshRaster::Mesh mesh;
						mesh.positions = sc.flash.data();
						mesh.vertex_count = static_cast<u32>(sc.flash.size() / 3);
						MeshRaster::Material mat;
						mat.lighting.enabled = false;
						mat.cull_back = false;
						mat.base_colour[0] = 1.0f;
						mat.base_colour[1] = 0.91f;
						mat.base_colour[2] = 0.69f;
						MeshRaster::Draw(sc.target, cam, mesh, world, mat);
					}
				}
			}
			sc.target.Resolve();
			sc.target.CopyUnpremultiplied(out_rgba.data());
			const double us = timer.GetTimeNanoseconds() / 1000.0;
			const u32 us32 = static_cast<u32>(std::min(us, 4.0e9));
			s_mesh_images.fetch_add(1, std::memory_order_relaxed);
			s_mesh_total_us.fetch_add(us32, std::memory_order_relaxed);
			u32 prev = s_mesh_max_us.load(std::memory_order_relaxed);
			while (us32 > prev && !s_mesh_max_us.compare_exchange_weak(prev, us32, std::memory_order_relaxed))
			{
			}
		}
	}

	bool DecodeGameAttach(const u8* entry, size_t size, u16 type, u32 grip_offset, u32 rotation_offset, u32 muzzle_offset,
		GameAttach* out)
	{
		if (!out)
			return false;
		*out = GameAttach{};
		if (!entry || type == 0 || size < 3 || entry[2] != static_cast<u8>(type) || grip_offset + 12 > size ||
			rotation_offset + 6 > size || muzzle_offset + 12 > size)
		{
			return false;
		}
		GameAttach a;
		a.type = type;
		float muzzle[3];
		for (int k = 0; k < 3; k++)
		{
			std::memcpy(&a.grip_cm[k], entry + grip_offset + 4 * k, 4);
			s16 r;
			std::memcpy(&r, entry + rotation_offset + 2 * k, 2);
			a.rot_rad[k] = static_cast<float>(r) * (2.0f * kPi / 65536.0f);
			std::memcpy(&muzzle[k], entry + muzzle_offset + 4 * k, 4);
		}
		if (!Finite(a.grip_cm, 3) || !Finite(muzzle, 3) || Length(Load(a.grip_cm)) > 100.0f || Length(Load(muzzle)) > 200.0f)
			return false;
		a.has_muzzle = Length(Load(muzzle)) > 0.01f;
		for (int k = 0; k < 3; k++)
			a.muzzle_cm[k] = a.has_muzzle ? muzzle[k] : 0.0f;
		a.valid = true;
		*out = a;
		return true;
	}

	bool DrawsMesh(int hand, const HandState& h)
	{
		if (!h.mesh)
			return false;
		if (!(h.gun && hand == kRight))
			return true;
		if (!LookupModel(h.item_id))
		{
			LogFallbackOnce(h.item_id, "its model is not on this disc or did not decode (or the models are still loading)");
			return false;
		}
		if (!h.attach.valid)
		{
			LogFallbackOnce(h.item_id, "the game's grip for it is not known (no weapon-type entry read)");
			return false;
		}
		return true;
	}

	void SetModelLookupForTest(ModelLookup lookup)
	{
		s_model_lookup.store(lookup, std::memory_order_release);
		std::lock_guard<std::mutex> lock(s_fallback_mutex);
		s_fallback_logged.clear();
	}

	MeshDrawStats TakeMeshDrawStats()
	{
		MeshDrawStats st;
		st.images = s_mesh_images.exchange(0, std::memory_order_relaxed);
		st.total_us = s_mesh_total_us.exchange(0, std::memory_order_relaxed);
		st.max_us = s_mesh_max_us.exchange(0, std::memory_order_relaxed);
		return st;
	}

	bool ModelLaserFrom(const HandState& gun_hand, float gap_m, float out_origin[3], float out_dir[3])
	{
		if (!gun_hand.valid || !gun_hand.gun || !Finite(gun_hand.pos, 3) || !Finite(gun_hand.quat, 4) ||
			!std::isfinite(gap_m) || !DrawsMesh(kRight, gun_hand))
		{
			return false;
		}
		V3 muzzle;
		if (!ModelMuzzleCm(gun_hand, &muzzle))
			return false;
		const float n2 = gun_hand.quat[0] * gun_hand.quat[0] + gun_hand.quat[1] * gun_hand.quat[1] +
		                 gun_hand.quat[2] * gun_hand.quat[2] + gun_hand.quat[3] * gun_hand.quat[3];
		if (!std::isfinite(n2) || n2 < 1.0e-6f)
			return false;
		float q[4];
		NormalizedQuat(gun_hand, q);
		const V3 dir = Rotate(q, V3{0.0f, 0.0f, -1.0f});
		const V3 from = Add(Load(gun_hand.pos), Rotate(q, Mul(muzzle, 0.01f)));
		Store(Add(from, Mul(dir, gap_m)), out_origin);
		Store(dir, out_dir);
		return true;
	}

	WeaponHeld HeldFor(bool raised, bool item_known, u16 item_id)
	{
		WeaponHeld w;
		if (!raised)
			return w;
		w.raised = true;
		w.held = Held::Pistol;
		w.laser = true;
		if (!item_known)
			return w;
		w.known = true;
		w.item_id = item_id;
		switch (ItemData::KindOf(item_id))
		{
			case ItemData::Kind::Handgun:
			case ItemData::Kind::OtherWeapon:
			case ItemData::Kind::Unknown: // an id the table lacks: the pistol and its laser, as before the reader knew more
				break;
			case ItemData::Kind::LongGun:
				w.held = Held::LongGun;
				break;
			case ItemData::Kind::Melee:
				w.held = Held::Melee;
				w.laser = false;
				break;
			case ItemData::Kind::Thrown:
				w.held = Held::Thrown;
				w.laser = false;
				break;
			case ItemData::Kind::Spray:
				w.held = Held::Spray;
				w.laser = false;
				break;
			default: // no weapon (ammo, recovery, a character's own item, a key item): bare hands
				return WeaponHeld{};
		}
		return w;
	}

	void SetWeaponHeld(const WeaponHeld& held)
	{
		const u32 packed = PackHeld(held);
		s_weapon_held.store(packed, std::memory_order_relaxed);
		if (!held.raised || s_weapon_logged.exchange(packed, std::memory_order_relaxed) == packed)
			return;
		if (!held.known)
		{
			Console.WriteLn("(VR) Hands: the right hand shows the pistol from the stance alone (the weapon reader cannot "
							"tell what is equipped), with the laser sight.");
			return;
		}
		const char* name = ItemData::Name(held.item_id);
		Console.WriteLn("(VR) Hands: the right hand shows a %s for %s (item %u)%s.", HeldName(held.held),
			name[0] ? name : "an item the item table lacks", static_cast<unsigned>(held.item_id),
			held.laser ? ", with the laser sight" : "");
	}

	WeaponHeld GetWeaponHeld()
	{
		return UnpackHeld(s_weapon_held.load(std::memory_order_relaxed));
	}

	bool GunHeld()
	{
		return GetWeaponHeld().raised;
	}

	const char* HeldName(Held held)
	{
		switch (held)
		{
			case Held::LongGun: return "long gun";
			case Held::Melee: return "melee weapon";
			case Held::Thrown: return "thrown item";
			case Held::Spray: return "spray can";
			default: return "pistol";
		}
	}

	bool OffHandSupports(const HandState& gun_hand, const HandState& off_hand, bool was)
	{
		if (!gun_hand.valid || !gun_hand.gun || gun_hand.held != Held::LongGun || !off_hand.valid ||
			!Finite(gun_hand.pos, 3) || !Finite(gun_hand.quat, 4) || !Finite(off_hand.pos, 3))
		{
			return false;
		}
		float q[4];
		NormalizedQuat(gun_hand, q);
		const V3 fore_end_cm = V3{0.0f, kForeEndYCm, LongGunFor(gun_hand.item_id).fore_end_cm};
		const V3 fore_end = Add(Load(gun_hand.pos), Rotate(q, Mul(fore_end_cm, 0.01f)));
		const float d = Length(Sub(Load(off_hand.pos), fore_end));
		return d < (was ? kSupportReleaseM : kSupportGripM);
	}

	bool LaserFrom(Held held, u16 item_id, const float pos[3], const float quat[4], float gap_m, float out_origin[3],
		float out_dir[3])
	{
		if (!Finite(pos, 3) || !Finite(quat, 4) || !std::isfinite(gap_m))
			return false;
		const float n2 = quat[0] * quat[0] + quat[1] * quat[1] + quat[2] * quat[2] + quat[3] * quat[3];
		if (!std::isfinite(n2) || n2 < 1.0e-6f)
			return false;
		const float inv = 1.0f / std::sqrt(n2);
		const float q[4] = {quat[0] * inv, quat[1] * inv, quat[2] * inv, quat[3] * inv};
		const float muzzle_cm = (held == Held::LongGun) ? LongGunFor(item_id).muzzle_cm : kPistolMuzzleZCm;
		const V3 dir = Rotate(q, V3{0.0f, 0.0f, -1.0f});
		const V3 muzzle = Add(Load(pos), Rotate(q, V3{0.0f, kBoreYCm * 0.01f, muzzle_cm * 0.01f}));
		Store(Add(muzzle, Mul(dir, gap_m)), out_origin);
		Store(dir, out_dir);
		return true;
	}

	bool HolsterPose(Held held, const float centre[3], float yaw, float out_pos[3], float out_quat[4], bool model)
	{
		if (!Finite(centre, 3) || !std::isfinite(yaw))
			return false;
		// A pitch about the body's right (+X), then the body's yaw. A gun or a melee weapon tips its muzzle (-Z) down and
		// kHolsterCantDeg on past straight down, which stands its grip up and back toward the hand. Something held in the
		// fist (a can, a molotov) is built round the handle, whose top leans kHandleTiltDeg forward in the pointing frame:
		// tipped back that far, the handle stands upright on the belt.
		const bool upright = (held == Held::Thrown || held == Held::Spray);
		// A game model's shaft runs along +Y: turned 180 deg less the cant about the body's right, it hangs down and a
		// little back, its hold point up and forward toward the hand.
		const bool shaft_down = model && held == Held::Melee;
		const float pitch =
			(shaft_down ? (180.0f - kHolsterCantDeg) : upright ? kHandleTiltDeg : -(90.0f + kHolsterCantDeg)) * kDegToRad;
		const float q_yaw[4] = {0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw)};
		const float q_pitch[4] = {std::sin(0.5f * pitch), 0.0f, 0.0f, std::cos(0.5f * pitch)};
		QuatMul(q_yaw, q_pitch, out_quat);
		for (int k = 0; k < 3; k++)
			out_pos[k] = centre[k];
		return true;
	}

	bool SlingPose(const float centre[3], float yaw, float out_pos[3], float out_quat[4])
	{
		if (!Finite(centre, 3) || !std::isfinite(yaw))
			return false;
		// The muzzle (-Z) tipped up past straight up by kSlingBackDeg (a pitch about the body's right), then leaned out by
		// kSlingOutDeg (about the body's forward axis, the top of the barrel toward +X), then the body's yaw: the barrel
		// stands up behind the shoulder, clear of the head, and the stock hangs down the back.
		const float pitch = (90.0f + kSlingBackDeg) * kDegToRad;
		const float lean = -kSlingOutDeg * kDegToRad;
		const float q_yaw[4] = {0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw)};
		const float q_lean[4] = {0.0f, 0.0f, std::sin(0.5f * lean), std::cos(0.5f * lean)};
		const float q_pitch[4] = {std::sin(0.5f * pitch), 0.0f, 0.0f, std::cos(0.5f * pitch)};
		float q_lp[4];
		QuatMul(q_lean, q_pitch, q_lp);
		QuatMul(q_yaw, q_lp, out_quat);
		for (int k = 0; k < 3; k++)
			out_pos[k] = centre[k];
		return true;
	}

	bool PlaceImpostor(int hand, const HandState& h, const float eye[3], ImpostorQuad* out)
	{
		if (!out || !h.valid || !Finite(h.pos, 3) || !Finite(h.quat, 4) || !Finite(eye, 3))
			return false;

		V3 lmin, lmax, cmin, cmax;
		const bool mesh = DrawsMesh(hand, h);
		if (mesh)
		{
			if (!MeshBounds(hand, h, &lmin, &lmax, &cmin, &cmax))
				return false;
		}
		else
		{
			BuildWorld(hand, h, &lmin, &lmax, &cmin, &cmax);
		}
		float q[4];
		NormalizedQuat(h, q);
		const V3 pos = Load(h.pos);
		const V3 e = Load(eye);

		// The eye inside the hand (or the pistol, can or thrown item in it): nothing sensible to draw.
		const V3 core_centre = Add(pos, Rotate(q, Mul(Mul(Add(cmin, cmax), 0.5f), 0.01f)));
		const float core_radius = 0.5f * 0.01f * Length(Sub(cmax, cmin));
		const float core_dist = Length(Sub(e, core_centre));
		if (!(core_dist > core_radius * 1.1f) || core_dist < 0.05f)
			return false;

		const bool weapon = h.gun && hand == kRight;
		const float base = (weapon && h.holstered) ? kQuadSizeHolsterM : weapon ? kQuadSizeGunM : kQuadSizeM;
		const float long_max = mesh ? kQuadSizeModelLongM : kQuadSizeLongM;
		const float max_size = (weapon && (h.held == Held::LongGun || h.held == Held::Melee)) ? long_max : base * 2.0f;

		// The quad faces the eye from the middle of everything drawn, so as much of a long gun or a pole as can be lies in
		// front of the eye, and lies on the plane through the middle of the hand (with a pistol, can or thrown item in it),
		// so the hand keeps its true depth in stereo; the weapon's reach is drawn onto that plane too. Seen from the eye,
		// the corners of the whole local bounding box hold every triangle (all of it in front of the eye, its outline is
		// theirs), so the quad is centred on and sized to them there: tight round a long gun or a pole whichever way it
		// points.
		const V3 full_centre = Add(pos, Rotate(q, Mul(Mul(Add(lmin, lmax), 0.5f), 0.01f)));
		float dist = 0.0f;
		V3 n = V3{0.0f, 0.0f, 1.0f};
		V3 up = V3{0.0f, 1.0f, 0.0f};
		V3 right = V3{1.0f, 0.0f, 0.0f};
		const V3 ec = Sub(e, core_centre);
		bool fitted = FacingFrame(full_centre, e, &dist, &n, &up, &right);
		const float de = Dot(ec, n); // the eye's distance in front of the hand's plane
		fitted = fitted && de >= kFitNearM;
		float x0 = std::numeric_limits<float>::max();
		float y0 = std::numeric_limits<float>::max();
		float x1 = -std::numeric_limits<float>::max();
		float y1 = -std::numeric_limits<float>::max();
		for (int c = 0; c < 8 && fitted; c++)
		{
			const V3 corner = V3{(c & 1) ? lmax.x : lmin.x, (c & 2) ? lmax.y : lmin.y, (c & 4) ? lmax.z : lmin.z};
			const V3 d = Sub(Add(pos, Rotate(q, Mul(corner, 0.01f))), e);
			const float z = -Dot(d, n);
			if (!(z > kFitNearM))
			{
				fitted = false;
				break;
			}
			const float s = de / z;
			const float x = Dot(ec, right) + s * Dot(d, right);
			const float y = Dot(ec, up) + s * Dot(d, up);
			x0 = std::min(x0, x);
			x1 = std::max(x1, x);
			y0 = std::min(y0, y);
			y1 = std::max(y1, y);
		}
		V3 centre = core_centre;
		float size = fitted ? std::max(x1 - x0, y1 - y0) * kFitMargin : 0.0f;
		if (fitted && std::isfinite(size))
		{
			float cx = 0.5f * (x0 + x1);
			float cy = 0.5f * (y0 + y1);
			// A game model too big for the largest quad (a pole seen side on): the quad keeps the hand (the core, at 0, 0
			// on this plane) in it and holds as much of the weapon as fits; what is cut is the far end.
			if (mesh && size > max_size)
			{
				const float room = std::max(0.0f, 0.5f * max_size - 1.1f * core_radius);
				cx = std::clamp(cx, -room, room);
				cy = std::clamp(cy, -room, room);
			}
			centre = Add(centre, Add(Mul(right, cx), Mul(up, cy)));
		}
		else
		{
			// Part of a long gun or a melee weapon is beside or behind the eye, so no quad holds all of it: the quad faces
			// the eye from the hand and goes round the hand's bounding sphere seen from the eye, with room for some of
			// the weapon (the raster leaves out what is nearer the eye than kNearM).
			if (!FacingFrame(core_centre, e, &dist, &n, &up, &right))
				return false;
			size = kFallbackGrow * 2.04f * dist * core_radius / std::sqrt(dist * dist - core_radius * core_radius);
		}
		out->size_m = std::isfinite(size) ? std::clamp(size, base, max_size) : max_size;
		Store(centre, out->centre);
		Store(right, out->right);
		Store(up, out->up);
		QuatFromBasis(right, up, n, out->quat);
		return true;
	}

	void RenderImpostor(int hand, const HandState& h, const float eye[3], const float quad_center[3],
		const float quad_right[3], const float quad_up[3], float quad_size_m, std::vector<u32>& out_rgba)
	{
		out_rgba.assign(kPixels, 0u);
		if (!h.valid || !(quad_size_m > 0.0f) || !Finite(h.pos, 3) || !Finite(h.quat, 4) || !Finite(eye, 3) ||
			!Finite(quad_center, 3) || !Finite(quad_right, 3) || !Finite(quad_up, 3))
		{
			return;
		}

		const V3 e = Load(eye);
		const V3 c = Load(quad_center);
		const V3 r = Normalize(Load(quad_right));
		const V3 n = Normalize(Cross(r, Load(quad_up)));
		const V3 u = Cross(n, r);
		const V3 ec = Sub(e, c);
		const float de = Dot(ec, n); // eye's distance in front of the quad
		if (de < 0.02f)
			return;
		const float ec_r = Dot(ec, r);
		const float ec_u = Dot(ec, u);
		const float px_per_m = static_cast<float>(kImageSize) / quad_size_m;
		const float half = 0.5f * static_cast<float>(kImageSize);

		if (DrawsMesh(hand, h))
		{
			RenderMesh(hand, h, eye, quad_center, quad_right, quad_up, quad_size_m, out_rgba);
			return;
		}

		BuildWorld(hand, h, nullptr, nullptr, nullptr, nullptr);
		s_depth.assign(kPixels, 0.0f);
		const Tables& tab = GetTables();
		const V3 light = Normalize(Add(Add(Mul(r, 0.35f), Mul(u, 0.8f)), Mul(n, 0.5f)));
		const float gain = (hand == kRight && h.gun && h.holstered && h.lit) ? kHolsterLitGain : 1.0f;
		Bounds bounds = {kSize, kSize, -1, -1};

		for (const Tri& t : s_mesh)
		{
			const V3 centroid = Mul(Add(Add(t.p[0], t.p[1]), t.p[2]), 1.0f / 3.0f);
			const V3 to_eye = Sub(e, centroid);
			if (Dot(t.n, to_eye) <= 0.0f)
				continue;

			// Central projection from the eye onto the quad plane.
			SV sv[3];
			bool ok = true;
			for (int k = 0; k < 3; k++)
			{
				const V3 d = Sub(t.p[k], e);
				const float z = -Dot(d, n);
				if (!(z > kNearM))
				{
					ok = false;
					break;
				}
				const float s = de / z;
				sv[k].x = half + (ec_r + s * Dot(d, r)) * px_per_m;
				sv[k].y = half - (ec_u + s * Dot(d, u)) * px_per_m;
				sv[k].iz = 1.0f / z;
				if (!std::isfinite(sv[k].x) || !std::isfinite(sv[k].y))
				{
					ok = false;
					break;
				}
			}
			if (!ok)
				continue;
			RasterTri(sv[0], sv[1], sv[2], Shade(tab, t, to_eye, light, u, gain), s_depth.data(), out_rgba.data(), bounds);
		}

		if (bounds.x1 >= bounds.x0 && bounds.y1 >= bounds.y0)
			SoftenEdges(out_rgba.data(), bounds);
	}

	WeaponHeld FakeWeapon()
	{
		static const WeaponHeld s_fake = [] {
			const char* v = std::getenv("PCSX2_VR_FAKE_WEAPON");
			if (!v)
				return HeldFor(true, false, 0);
			char* end = nullptr;
			const long id = std::strtol(v, &end, 10);
			if (end == v || id < 0 || id > 0xFFFF)
			{
				Console.Warning("(VR) PCSX2_VR_FAKE_WEAPON=%s is not an item id; the test hands hold the pistol.", v);
				return HeldFor(true, false, 0);
			}
			const WeaponHeld w = HeldFor(true, true, static_cast<u16>(id));
			const char* name = ItemData::Name(static_cast<u16>(id));
			Console.WriteLn("(VR) Fake hands: the gun is item %ld (%s): %s%s.", id, name[0] ? name : "not in the item table",
				w.raised ? HeldName(w.held) : "no weapon, so bare hands", (w.raised && w.laser) ? ", with the laser sight" : "");
			return w;
		}();
		return s_fake;
	}

	HandState FakeHandState(int hand, const float head_pos[3], const float head_quat[4], bool gun)
	{
		HandState h;
		if (!Finite(head_pos, 3) || !Finite(head_quat, 4))
			return h;

		// Head yaw only, the same way the compositor levels the screen.
		const float qx = head_quat[0], qy = head_quat[1], qz = head_quat[2], qw = head_quat[3];
		float fx = -2.0f * (qx * qz + qw * qy);
		float fz = -(1.0f - 2.0f * (qx * qx + qy * qy));
		const float fl = std::sqrt(fx * fx + fz * fz);
		if (fl > 1.0e-4f)
		{
			fx /= fl;
			fz /= fl;
		}
		else
		{
			fx = 0.0f;
			fz = -1.0f;
		}
		const float side = (hand == kRight) ? 1.0f : -1.0f;
		const V3 fwd = V3{fx, 0.0f, fz};
		const V3 rgt = V3{-fz, 0.0f, fx};
		const V3 p = Add(Add(Load(head_pos), Mul(rgt, 0.22f * side)), Add(V3{0.0f, -0.30f, 0.0f}, Mul(fwd, 0.35f)));
		Store(p, h.pos);

		// Pointing along the head's yaw, turned in a little toward the middle, 15 degrees down, and
		// rolled in 20 degrees the way controllers are usually held (so the back of the hand shows).
		const float yaw = std::atan2(-fx, -fz) + side * 6.0f * kDegToRad;
		const float pitch = -15.0f * kDegToRad;
		const float roll = side * 20.0f * kDegToRad;
		const float q_yaw[4] = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
		const float q_pitch[4] = {std::sin(pitch * 0.5f), 0.0f, 0.0f, std::cos(pitch * 0.5f)};
		const float q_roll[4] = {0.0f, 0.0f, std::sin(roll * 0.5f), std::cos(roll * 0.5f)};
		float q_yp[4];
		QuatMul(q_yaw, q_pitch, q_yp);
		QuatMul(q_yp, q_roll, h.quat);
		h.grip = 0.5f;
		h.trigger = gun ? 0.0f : 0.15f;
		const WeaponHeld fake = FakeWeapon();
		h.gun = gun && hand == kRight && fake.raised;
		if (h.gun)
		{
			h.held = fake.held;
			h.item_id = fake.item_id;
		}
		// With a long gun the left hand is held just under its fore-end, pointing along it, so the grip on it shows.
		if (hand == kLeft && fake.raised && fake.held == Held::LongGun)
		{
			const HandState r = FakeHandState(kRight, head_pos, head_quat, false);
			const V3 under_cm = V3{0.0f, kForeEndYCm - 3.0f, LongGunFor(fake.item_id).fore_end_cm};
			Store(Add(Load(r.pos), Rotate(r.quat, Mul(under_cm, 0.01f))), h.pos);
			for (int k = 0; k < 4; k++)
				h.quat[k] = r.quat[k];
		}
		h.valid = true;
		return h;
	}

	namespace
	{
		// Unit quaternion (xyzw) from q; identity when it is unusable.
		void UnitQuat(const float q[4], float out[4])
		{
			const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
			if (!(l > 1.0e-6f) || !std::isfinite(l))
			{
				out[0] = out[1] = out[2] = 0.0f;
				out[3] = 1.0f;
				return;
			}
			for (int i = 0; i < 4; i++)
				out[i] = q[i] / l;
		}

		// The inverse of a unit quaternion.
		void Conjugate(const float q[4], float out[4])
		{
			out[0] = -q[0];
			out[1] = -q[1];
			out[2] = -q[2];
			out[3] = q[3];
		}
	}

	void RePoseRigid(const float ref_pos[3], const float ref_quat[4], const float now_pos[3], const float now_quat[4],
		const float in_centre[3], const float in_quat[4], float out_centre[3], float out_quat[4])
	{
		if (!Finite(ref_pos, 3) || !Finite(ref_quat, 4) || !Finite(now_pos, 3) || !Finite(now_quat, 4) ||
			!Finite(in_centre, 3) || !Finite(in_quat, 4))
		{
			for (int i = 0; i < 3; i++)
				out_centre[i] = in_centre[i];
			for (int i = 0; i < 4; i++)
				out_quat[i] = in_quat[i];
			return;
		}
		float qr[4];
		float qn[4];
		float qr_inv[4];
		float qd_raw[4];
		float qd[4];
		UnitQuat(ref_quat, qr);
		UnitQuat(now_quat, qn);
		Conjugate(qr, qr_inv);
		QuatMul(qn, qr_inv, qd_raw);
		UnitQuat(qd_raw, qd);
		float q_raw[4];
		QuatMul(qd, in_quat, q_raw);
		const V3 moved = Add(Load(now_pos), Rotate(qd, Sub(Load(in_centre), Load(ref_pos))));
		Store(moved, out_centre);
		UnitQuat(q_raw, out_quat);
	}

	ViewDrift MeasureViewDrift(const float ref_pos[3], const float ref_quat[4], const float ref_eye[3],
		const float ref_centre[3], const float now_pos[3], const float now_quat[4], const float now_eye[3])
	{
		ViewDrift d;
		if (!Finite(ref_pos, 3) || !Finite(ref_quat, 4) || !Finite(ref_eye, 3) || !Finite(ref_centre, 3) ||
			!Finite(now_pos, 3) || !Finite(now_quat, 4) || !Finite(now_eye, 3))
		{
			return d;
		}
		float qr[4];
		float qn[4];
		float qr_inv[4];
		float qn_inv[4];
		UnitQuat(ref_quat, qr);
		UnitQuat(now_quat, qn);
		Conjugate(qr, qr_inv);
		Conjugate(qn, qn_inv);
		// The image's centre as a point on the hand, and where that point is now.
		const V3 local = Rotate(qr_inv, Sub(Load(ref_centre), Load(ref_pos)));
		const V3 centre_now = Add(Load(now_pos), Rotate(qn, local));
		const V3 to_eye_ref = Sub(Load(ref_eye), Load(ref_centre));
		const V3 to_eye_now = Sub(Load(now_eye), centre_now);
		const float dist_ref = Length(to_eye_ref);
		const float dist_now = Length(to_eye_now);
		if (!(dist_ref >= 0.05f) || !(dist_now >= 0.05f) || !std::isfinite(dist_ref) || !std::isfinite(dist_now))
			return d;
		// The direction to the eye in the hand's own frame, then and now. The angle between them is acos of their dot
		// product, taken as atan2 of the cross and dot products, which keeps its precision near 0.
		const V3 v_ref = Rotate(qr_inv, Mul(to_eye_ref, 1.0f / dist_ref));
		const V3 v_now = Rotate(qn_inv, Mul(to_eye_now, 1.0f / dist_now));
		const float angle = std::atan2(Length(Cross(v_ref, v_now)), Dot(v_ref, v_now));
		d.angle_deg = angle / kDegToRad;
		d.distance_ratio = dist_now / dist_ref;
		d.valid = std::isfinite(d.angle_deg) && std::isfinite(d.distance_ratio);
		return d;
	}

	bool NeedsReraster(const HandState& ref, const float ref_eye[3], const float ref_centre[3], const HandState& now,
		const float now_eye[3], u64 age_ms)
	{
		if (!ref.valid || !now.valid)
			return true;
		if (ref.gun != now.gun || ref.held != now.held || ref.item_id != now.item_id || ref.support != now.support ||
			ref.slide_locked != now.slide_locked || ref.holstered != now.holstered || ref.lit != now.lit ||
			ref.mesh != now.mesh || ref.attach.valid != now.attach.valid || ref.attach.type != now.attach.type)
		{
			return true;
		}
		// A shot plays out frame by frame (recoil, flash, the slide cycling): every frame of it is drawn.
		if (now.kick > 0.0f || ref.kick > 0.0f || now.flash || ref.flash || now.slide > 0.0f || ref.slide > 0.0f)
			return true;
		if (!(std::fabs(now.grip - ref.grip) <= kRerasterArticulation) ||
			!(std::fabs(now.trigger - ref.trigger) <= kRerasterArticulation))
		{
			return true;
		}
		const ViewDrift d = MeasureViewDrift(ref.pos, ref.quat, ref_eye, ref_centre, now.pos, now.quat, now_eye);
		if (!d.valid || d.angle_deg > kRerasterDriftDeg || std::fabs(d.distance_ratio - 1.0f) > kRerasterDistanceRatio)
			return true;
		return age_ms > kRerasterMaxAgeMs;
	}

	void ApplyFakeSweep(int hand, s64 t_ns, HandState* h)
	{
		if (!h)
			return;
		// The cycle position by integer modulo first, so a large display time keeps its precision.
		constexpr s64 kCycleNs = 2000000000;
		constexpr double kPiD = 3.14159265358979323846;
		s64 into = t_ns % kCycleNs;
		if (into < 0)
			into += kCycleNs;
		double phase = 2.0 * kPiD * static_cast<double>(into) / static_cast<double>(kCycleNs);
		if (hand == kLeft)
			phase += kPiD;
		h->pos[0] += static_cast<float>(0.12 * std::sin(phase));
		h->pos[1] += static_cast<float>(0.05 * std::sin(2.0 * phase));
		const float yaw = static_cast<float>(0.35 * std::sin(phase));
		const float ry[4] = {0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw)};
		float q[4];
		QuatMul(ry, h->quat, q);
		for (int i = 0; i < 4; i++)
			h->quat[i] = q[i];
	}

	bool SelfTest()
	{
		int checks = 0;
		int failed = 0;
		const auto check = [&checks, &failed](bool ok, const char* name) {
			checks++;
			if (!ok)
			{
				failed++;
				Console.Error("(VR) Hands self-test FAIL: %s", name);
			}
		};
		const auto near3 = [](const float a[3], const float b[3], float tol) {
			return std::fabs(a[0] - b[0]) <= tol && std::fabs(a[1] - b[1]) <= tol && std::fabs(a[2] - b[2]) <= tol;
		};
		// The same orientation: q and -q are the same rotation.
		const auto same_rot = [](const float a[4], const float b[4], float tol) {
			bool same = true;
			bool negated = true;
			for (int i = 0; i < 4; i++)
			{
				same = same && std::fabs(a[i] - b[i]) <= tol;
				negated = negated && std::fabs(a[i] + b[i]) <= tol;
			}
			return same || negated;
		};
		const auto axis_angle = [](float ax, float ay, float az, float deg, float out[4]) {
			const float l = std::sqrt(ax * ax + ay * ay + az * az);
			const float half = 0.5f * deg * kDegToRad;
			const float sn = std::sin(half) / l;
			out[0] = ax * sn;
			out[1] = ay * sn;
			out[2] = az * sn;
			out[3] = std::cos(half);
		};
		const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};

		// RePoseRigid.
		{
			float qa[4];
			float qi[4];
			axis_angle(0.3f, 1.0f, -0.2f, 37.0f, qa);
			axis_angle(-0.5f, 0.2f, 0.8f, 110.0f, qi);
			const float p[3] = {0.25f, 1.05f, -0.45f};
			const float c[3] = {0.31f, 1.12f, -0.52f};
			float oc[3];
			float oq[4];
			RePoseRigid(p, qa, p, qa, c, qi, oc, oq);
			check(near3(oc, c, 1.0e-5f) && same_rot(oq, qi, 1.0e-5f),
				"RePoseRigid: a hand that did not move leaves the layer");

			const float t[3] = {0.1f, -0.2f, 0.3f};
			const float p_moved[3] = {p[0] + t[0], p[1] + t[1], p[2] + t[2]};
			const float c_moved[3] = {c[0] + t[0], c[1] + t[1], c[2] + t[2]};
			RePoseRigid(p, qa, p_moved, qa, c, qi, oc, oq);
			check(near3(oc, c_moved, 1.0e-5f) && same_rot(oq, qi, 1.0e-5f),
				"RePoseRigid: a pure translation moves the layer by exactly that and keeps its orientation");

			const float grip[3] = {1.0f, 0.0f, 0.0f};
			float ry90[4];
			axis_angle(0.0f, 1.0f, 0.0f, 90.0f, ry90);
			const float in_c[3] = {1.0f, 0.0f, -0.1f};
			const float want_c[3] = {0.9f, 0.0f, 0.0f};
			float want_q[4];
			QuatMul(ry90, qi, want_q);
			RePoseRigid(grip, identity, grip, ry90, in_c, qi, oc, oq);
			check(near3(oc, want_c, 1.0e-5f) && same_rot(oq, want_q, 1.0e-5f),
				"RePoseRigid: +90 deg of yaw about the grip turns the layer about the grip");

			const float ry90_neg[4] = {-ry90[0], -ry90[1], -ry90[2], -ry90[3]};
			float oc_neg[3];
			float oq_neg[4];
			RePoseRigid(grip, identity, grip, ry90_neg, in_c, qi, oc_neg, oq_neg);
			check(near3(oc_neg, oc, 1.0e-6f) && same_rot(oq_neg, oq, 1.0e-6f), "RePoseRigid: q and -q give the same pose");

			const float bad[3] = {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
			RePoseRigid(p, qa, bad, qa, c, qi, oc, oq);
			check(near3(oc, c, 0.0f) && same_rot(oq, qi, 0.0f), "RePoseRigid: a pose that is not finite leaves the layer");
		}

		// MeasureViewDrift: the hand 0.6 m ahead of the eye, the image's centre 5 cm along its x axis.
		const float hand_pos[3] = {0.0f, 1.0f, -0.5f};
		const float centre[3] = {0.05f, 1.0f, -0.5f};
		const float eye[3] = {0.0f, 1.0f, 0.1f};
		{
			ViewDrift d = MeasureViewDrift(hand_pos, identity, eye, centre, hand_pos, identity, eye);
			check(d.valid && d.angle_deg < 0.01f && std::fabs(d.distance_ratio - 1.0f) < 1.0e-5f,
				"MeasureViewDrift: the same view drifts 0 deg at a distance ratio of 1");

			// Turned 10 deg about its x axis, across the line of sight (the centre, on that axis, stays put).
			float rx10[4];
			axis_angle(1.0f, 0.0f, 0.0f, 10.0f, rx10);
			d = MeasureViewDrift(hand_pos, identity, eye, centre, hand_pos, rx10, eye);
			check(d.valid && std::fabs(d.angle_deg - 10.0f) <= 0.5f,
				"MeasureViewDrift: the hand turned 10 deg across the view drifts 10 deg");

			// The eye moved straight back along its line to the centre, to twice the distance.
			const float back[3] = {centre[0] + 2.0f * (eye[0] - centre[0]), centre[1] + 2.0f * (eye[1] - centre[1]),
				centre[2] + 2.0f * (eye[2] - centre[2])};
			d = MeasureViewDrift(hand_pos, identity, eye, centre, hand_pos, identity, back);
			check(d.valid && d.angle_deg < 0.1f && std::fabs(d.distance_ratio - 2.0f) <= 0.01f,
				"MeasureViewDrift: the eye twice as far back drifts 0 deg at a distance ratio of 2");

			d = MeasureViewDrift(hand_pos, identity, centre, centre, hand_pos, identity, centre);
			check(!d.valid, "MeasureViewDrift: an eye at the hand cannot be measured");
		}

		// NeedsReraster, from the same hand and view.
		{
			HandState ref;
			ref.valid = true;
			for (int k = 0; k < 3; k++)
				ref.pos[k] = hand_pos[k];
			ref.grip = 0.5f;
			ref.trigger = 0.15f;
			HandState now = ref;
			check(!NeedsReraster(ref, eye, centre, now, eye, 0), "NeedsReraster: the same hand and view keeps the image");

			now = ref;
			now.grip += 0.1f;
			check(NeedsReraster(ref, eye, centre, now, eye, 0), "NeedsReraster: grip +0.1 draws it again");

			now = ref;
			now.trigger += 0.02f;
			check(!NeedsReraster(ref, eye, centre, now, eye, 0), "NeedsReraster: trigger +0.02 keeps the image");

			float rx3[4];
			axis_angle(1.0f, 0.0f, 0.0f, 3.0f, rx3);
			now = ref;
			for (int k = 0; k < 4; k++)
				now.quat[k] = rx3[k];
			check(NeedsReraster(ref, eye, centre, now, eye, 0), "NeedsReraster: 3 deg of view drift draws it again");

			float rx1[4];
			axis_angle(1.0f, 0.0f, 0.0f, 1.0f, rx1);
			now = ref;
			for (int k = 0; k < 4; k++)
				now.quat[k] = rx1[k];
			check(!NeedsReraster(ref, eye, centre, now, eye, 0), "NeedsReraster: 1 deg of view drift keeps the image");

			now = ref;
			now.gun = true;
			check(NeedsReraster(ref, eye, centre, now, eye, 0), "NeedsReraster: the pistol taken or put away draws it again");

			now = ref;
			now.held = Held::LongGun;
			check(NeedsReraster(ref, eye, centre, now, eye, 0), "NeedsReraster: another kind of weapon draws it again");

			now = ref;
			now.item_id = 103;
			check(NeedsReraster(ref, eye, centre, now, eye, 0), "NeedsReraster: another item of the kind draws it again");

			now = ref;
			now.support = true;
			check(NeedsReraster(ref, eye, centre, now, eye, 0),
				"NeedsReraster: the left hand closing round a fore-end draws it again");

			now = ref;
			now.kick = 0.5f;
			check(NeedsReraster(ref, eye, centre, now, eye, 0), "NeedsReraster: recoil draws it again");

			now = ref;
			check(NeedsReraster(ref, eye, centre, now, eye, 600), "NeedsReraster: a 600 ms old image is drawn again");
		}

		// ApplyFakeSweep, at a large display time.
		{
			HandState base;
			base.valid = true;
			base.pos[0] = 0.22f;
			base.pos[1] = 1.3f;
			base.pos[2] = -0.35f;
			float q0[4];
			axis_angle(0.2f, 1.0f, 0.1f, 25.0f, q0);
			for (int k = 0; k < 4; k++)
				base.quat[k] = q0[k];
			base.grip = 0.5f;
			base.trigger = 0.15f;
			constexpr s64 kCycleNs = 2000000000;
			constexpr s64 t0 = 987654321012345678;

			HandState a = base;
			HandState b = base;
			ApplyFakeSweep(kRight, t0, &a);
			ApplyFakeSweep(kRight, t0 + kCycleNs, &b);
			check(near3(a.pos, b.pos, 1.0e-5f) && same_rot(a.quat, b.quat, 1.0e-5f), "ApplyFakeSweep: periodic over 2 s");
			const float len =
				std::sqrt(a.quat[0] * a.quat[0] + a.quat[1] * a.quat[1] + a.quat[2] * a.quat[2] + a.quat[3] * a.quat[3]);
			check(std::fabs(len - 1.0f) < 1.0e-4f, "ApplyFakeSweep: the orientation stays a unit quaternion");
			check(a.grip == base.grip && a.trigger == base.trigger && a.gun == base.gun && a.valid,
				"ApplyFakeSweep: grip, trigger and the pistol untouched");

			// A quarter of a cycle in: the right hand at +12 cm along x, the left at -12 cm.
			const s64 quarter = t0 - (t0 % kCycleNs) + kCycleNs / 4;
			HandState r = base;
			HandState l = base;
			ApplyFakeSweep(kRight, quarter, &r);
			ApplyFakeSweep(kLeft, quarter, &l);
			const float right_dx = r.pos[0] - base.pos[0];
			const float left_dx = l.pos[0] - base.pos[0];
			check(std::fabs(right_dx - 0.12f) < 1.0e-4f && std::fabs(left_dx + 0.12f) < 1.0e-4f,
				"ApplyFakeSweep: left and right in opposite phase");

			// Sampled at 90 Hz over a whole cycle: smooth, and no faster than the sweep allows.
			constexpr s64 kStepNs = 11111111;
			float prev1[3] = {0.0f, 0.0f, 0.0f};
			float prev2[3] = {0.0f, 0.0f, 0.0f};
			float max_second = 0.0f;
			float max_step = 0.0f;
			for (int n = 0; n <= 180; n++)
			{
				HandState h = base;
				ApplyFakeSweep(kRight, t0 + static_cast<s64>(n) * kStepNs, &h);
				if (n >= 1)
				{
					const V3 step = Sub(Load(h.pos), Load(prev1));
					max_step = std::max(max_step, Length(step));
				}
				if (n >= 2)
				{
					const V3 second = Add(Sub(Load(h.pos), Mul(Load(prev1), 2.0f)), Load(prev2));
					max_second = std::max(max_second, Length(second));
				}
				for (int k = 0; k < 3; k++)
				{
					prev2[k] = prev1[k];
					prev1[k] = h.pos[k];
				}
			}
			check(max_second < 0.0005f && max_step < 0.0065f,
				"ApplyFakeSweep: smooth at 90 Hz (second difference under 0.5 mm, step under 6.5 mm)");
		}

		// HeldFor: the weapon reader's verdict to what the hands show, and the laser sight by kind (ItemData).
		{
			const auto shows = [](const WeaponHeld& w, Held held, bool laser) {
				return w.raised && w.held == held && w.laser == laser;
			};
			check(!HeldFor(false, true, 11).raised && !HeldFor(false, false, 0).raised, "HeldFor: nothing raised shows nothing");
			const WeaponHeld stance_only = HeldFor(true, false, 0);
			check(shows(stance_only, Held::Pistol, true) && !stance_only.known,
				"HeldFor: with the reader unsure, the pistol and its laser from the stance alone");
			check(shows(HeldFor(true, true, 400), Held::Pistol, true), "HeldFor: Kevin's 45 Auto is the pistol, with the laser");
			check(shows(HeldFor(true, true, 30), Held::Pistol, true), "HeldFor: the stun gun is the pistol, with the laser");
			check(shows(HeldFor(true, true, 11), Held::LongGun, true), "HeldFor: the shotgun is a long gun, with the laser");
			check(shows(HeldFor(true, true, 20), Held::LongGun, true), "HeldFor: the rocket launcher is a long gun");
			check(shows(HeldFor(true, true, 100), Held::Melee, false), "HeldFor: the survival knife is melee, no laser");
			check(shows(HeldFor(true, true, 106), Held::Melee, false), "HeldFor: the long pole is melee, no laser");
			check(shows(HeldFor(true, true, 114), Held::Thrown, false), "HeldFor: a molotov is thrown, no laser");
			check(shows(HeldFor(true, true, 28), Held::Spray, false), "HeldFor: the pesticide spray is a spray can, no laser");
			check(!HeldFor(true, true, 300).raised, "HeldFor: a green herb (no weapon) leaves the hands bare");
			check(shows(HeldFor(true, true, 9), Held::Pistol, true), "HeldFor: an id the item table lacks keeps the pistol");
			const WeaponHeld wrench = UnpackHeld(PackHeld(HeldFor(true, true, 408)));
			check(wrench.raised && wrench.held == Held::Melee && wrench.item_id == 408 && wrench.known && !wrench.laser,
				"WeaponHeld: one word and back (the monkey wrench)");
			const WeaponHeld rifle = UnpackHeld(PackHeld(HeldFor(true, true, 14)));
			check(rifle.raised && rifle.held == Held::LongGun && rifle.item_id == 14 && rifle.known && rifle.laser,
				"WeaponHeld: one word and back (the assault rifle)");
			check(!UnpackHeld(PackHeld(WeaponHeld{})).raised, "WeaponHeld: nothing raised stays nothing raised");
		}

		// LaserFrom: from just past each gun's muzzle along its barrel.
		{
			const float at_origin[3] = {0.0f, 0.0f, 0.0f};
			const float gap = 0.02f;
			float from[3];
			float dir[3];
			check(LaserFrom(Held::Pistol, 0, at_origin, identity, gap, from, dir) && std::fabs(from[0]) < 1.0e-6f &&
					std::fabs(from[1] - 0.067f) < 1.0e-5f && std::fabs(from[2] + 0.1414f + gap) < 1.0e-5f &&
					std::fabs(dir[2] + 1.0f) < 1.0e-6f,
				"LaserFrom: the pistol's beam starts 2 cm past its muzzle (HudCards' kPistolMuzzle)");
			check(LaserFrom(Held::LongGun, 11, at_origin, identity, gap, from, dir) && std::fabs(from[1] - 0.067f) < 1.0e-5f &&
					std::fabs(from[2] + 0.52f + gap) < 1.0e-5f,
				"LaserFrom: the shotgun's beam starts past its own muzzle, 52 cm out on the pistol's line");
			check(LaserFrom(Held::LongGun, 13, at_origin, identity, gap, from, dir) && std::fabs(from[2] + 0.58f + gap) < 1.0e-5f,
				"LaserFrom: the hunting rifle's 58 cm out");
			float turn_left[4];
			axis_angle(0.0f, 1.0f, 0.0f, 90.0f, turn_left);
			const float grip_at[3] = {0.3f, 1.1f, -0.4f};
			check(LaserFrom(Held::LongGun, 11, grip_at, turn_left, gap, from, dir) && std::fabs(dir[0] + 1.0f) < 1.0e-5f &&
					std::fabs(from[0] - (0.3f - 0.52f - gap)) < 1.0e-4f && std::fabs(from[1] - (1.1f + 0.067f)) < 1.0e-4f &&
					std::fabs(from[2] + 0.4f) < 1.0e-4f,
				"LaserFrom: turned 90 deg left, the beam runs along -X from the turned muzzle");
			const float no_quat[4] = {0.0f, 0.0f, 0.0f, 0.0f};
			check(!LaserFrom(Held::Pistol, 0, grip_at, no_quat, gap, from, dir), "LaserFrom: no beam from an unusable aim");
		}

		// OffHandSupports: the left hand at the long gun's fore-end closes round it, with hysteresis.
		{
			HandState gun_hand;
			gun_hand.valid = true;
			gun_hand.pos[0] = 0.2f;
			gun_hand.pos[1] = 1.0f;
			gun_hand.pos[2] = -0.4f;
			gun_hand.gun = true;
			gun_hand.held = Held::LongGun;
			gun_hand.item_id = 11;
			// The shotgun's fore-end: 25 cm ahead of the grip and 4.5 cm over it.
			const float fore[3] = {0.2f, 1.045f, -0.65f};
			HandState off_hand;
			off_hand.valid = true;
			const auto off_at = [&off_hand, &fore](float dx) {
				off_hand.pos[0] = fore[0] + dx;
				off_hand.pos[1] = fore[1];
				off_hand.pos[2] = fore[2];
			};
			off_at(0.0f);
			check(OffHandSupports(gun_hand, off_hand, false), "OffHandSupports: the left hand on the fore-end holds it");
			off_at(0.20f);
			check(OffHandSupports(gun_hand, off_hand, false), "OffHandSupports: 20 cm from it still takes hold");
			off_at(0.265f);
			check(!OffHandSupports(gun_hand, off_hand, false) && OffHandSupports(gun_hand, off_hand, true),
				"OffHandSupports: 26.5 cm from it takes no new hold but keeps one");
			off_at(0.30f);
			check(!OffHandSupports(gun_hand, off_hand, true), "OffHandSupports: 30 cm from it lets go");
			off_at(0.0f);
			HandState pistol_hand = gun_hand;
			pistol_hand.held = Held::Pistol;
			check(!OffHandSupports(pistol_hand, off_hand, true), "OffHandSupports: a pistol has no fore-end");
			HandState lowered = gun_hand;
			lowered.gun = false;
			check(!OffHandSupports(lowered, off_hand, true), "OffHandSupports: no weapon raised, nothing to hold");
			HandState off_gone = off_hand;
			off_gone.valid = false;
			check(!OffHandSupports(gun_hand, off_gone, true), "OffHandSupports: no left hand, no hold");
			float turn_left[4];
			axis_angle(0.0f, 1.0f, 0.0f, 90.0f, turn_left);
			HandState turned = gun_hand;
			for (int k = 0; k < 4; k++)
				turned.quat[k] = turn_left[k];
			HandState off_turned = off_hand;
			off_turned.pos[0] = 0.2f - 0.25f;
			off_turned.pos[2] = -0.4f;
			check(OffHandSupports(turned, off_turned, false) && !OffHandSupports(turned, off_hand, false),
				"OffHandSupports: the fore-end turns with the gun");
		}

		// The shapes, drawn by the rasteriser: from one eye onto one quad (placed for the long pole, the biggest), each
		// weapon covers enough of it and differs from every other and from the bare hand; drawn on the quad
		// PlaceImpostor gives it, nothing touches the image's edge (the quad holds it all); and the left hand closed
		// round a fore-end differs from it holding the controller.
		{
			struct ShapeCase
			{
				const char* name;
				bool gun;
				Held held;
				u16 item;
			};
			const ShapeCase cases[] = {
				{"the hand on the controller", false, Held::Pistol, 0},
				{"the pistol", true, Held::Pistol, 0},
				{"the shotgun", true, Held::LongGun, 11},
				{"the assault rifle", true, Held::LongGun, 14},
				{"the survival knife", true, Held::Melee, 100},
				{"the iron pipe", true, Held::Melee, 103},
				{"the long pole", true, Held::Melee, 106},
				{"the axe", true, Held::Melee, 109},
				{"the molotov", true, Held::Thrown, 114},
				{"the spray can", true, Held::Spray, 28},
			};
			const size_t kCases = std::size(cases);
			constexpr size_t kPole = 6;
			const float view_eye[3] = {0.0f, 1.2f, 0.0f};
			const auto hand_for = [](const ShapeCase& sc) {
				HandState hs;
				hs.valid = true;
				hs.pos[0] = 0.22f;
				hs.pos[1] = 0.90f;
				hs.pos[2] = -0.35f;
				hs.grip = 0.5f;
				hs.gun = sc.gun;
				hs.held = sc.held;
				hs.item_id = sc.item;
				return hs;
			};
			char what[160];
			ImpostorQuad shared;
			const bool shared_placed = PlaceImpostor(kRight, hand_for(cases[kPole]), view_eye, &shared);
			check(shared_placed, "shapes: the long pole's quad is placed");
			std::vector<u8> masks(kCases * kPixels, 0);
			std::vector<u32> px;
			for (size_t i = 0; i < kCases && shared_placed; i++)
			{
				const HandState hs = hand_for(cases[i]);
				RenderImpostor(kRight, hs, view_eye, shared.centre, shared.right, shared.up, shared.size_m, px);
				u32 covered = 0;
				for (size_t p = 0; p < kPixels; p++)
				{
					const u8 on = ((px[p] >> 24) != 0) ? 1 : 0;
					masks[i * kPixels + p] = on;
					covered += on;
				}
				std::snprintf(what, sizeof(what), "shapes: %s covers %u px of the shared quad (at least 1500)", cases[i].name,
					covered);
				check(covered >= 1500, what);

				ImpostorQuad own;
				bool held_in = PlaceImpostor(kRight, hs, view_eye, &own);
				u32 own_covered = 0;
				if (held_in)
				{
					RenderImpostor(kRight, hs, view_eye, own.centre, own.right, own.up, own.size_m, px);
					for (int y = 0; y < kSize; y++)
					{
						for (int x = 0; x < kSize; x++)
						{
							if ((px[static_cast<size_t>(y) * kImageSize + x] >> 24) == 0)
								continue;
							own_covered++;
							if (x == 0 || y == 0 || x == kSize - 1 || y == kSize - 1)
								held_in = false;
						}
					}
				}
				std::snprintf(what, sizeof(what), "shapes: %s fits the quad it is placed on (%.2f m, %u px)", cases[i].name,
					static_cast<double>(own.size_m), own_covered);
				check(held_in && own_covered > 0, what);
			}
			// The closest pair still differs in a good many pixels.
			u32 fewest = std::numeric_limits<u32>::max();
			size_t pair_a = 0;
			size_t pair_b = 0;
			for (size_t a = 0; a < kCases && shared_placed; a++)
			{
				for (size_t b = a + 1; b < kCases; b++)
				{
					u32 differ = 0;
					for (size_t p = 0; p < kPixels; p++)
						differ += (masks[a * kPixels + p] != masks[b * kPixels + p]) ? 1u : 0u;
					if (differ < fewest)
					{
						fewest = differ;
						pair_a = a;
						pair_b = b;
					}
				}
			}
			std::snprintf(what, sizeof(what), "shapes: every two differ in at least 200 px (closest: %s and %s, %u px)",
				cases[pair_a].name, cases[pair_b].name, shared_placed ? fewest : 0u);
			check(shared_placed && fewest >= 200, what);

			// The left hand, on the controller and closed round a fore-end, on one quad.
			HandState left_hand;
			left_hand.valid = true;
			left_hand.pos[0] = -0.22f;
			left_hand.pos[1] = 0.90f;
			left_hand.pos[2] = -0.35f;
			left_hand.grip = 0.5f;
			ImpostorQuad left_quad;
			u32 left_differ = 0;
			const bool left_placed = PlaceImpostor(kLeft, left_hand, view_eye, &left_quad);
			if (left_placed)
			{
				std::vector<u32> open_px;
				RenderImpostor(kLeft, left_hand, view_eye, left_quad.centre, left_quad.right, left_quad.up, left_quad.size_m,
					open_px);
				left_hand.support = true;
				RenderImpostor(kLeft, left_hand, view_eye, left_quad.centre, left_quad.right, left_quad.up, left_quad.size_m, px);
				for (size_t p = 0; p < kPixels; p++)
					left_differ += (((open_px[p] >> 24) != 0) != ((px[p] >> 24) != 0)) ? 1u : 0u;
			}
			std::snprintf(what, sizeof(what), "shapes: the left hand closed round a fore-end differs from it on the controller "
				"(%u px, at least 200)", left_differ);
			check(left_placed && left_differ >= 200, what);
		}

		// [VR] GameWeaponModels: the game's attach entry, the placement, the fallback and the mesh images, with a synthetic
		// weapon model (no disc data): a 22 x 2.6 x 14 cm "pistol" lying as the game's do (barrel toward -X, top toward +Z),
		// its origin in the middle, a frame part and a slide part (skin slot 2), a 16 x 16 checker texture.
		{
			static GameModels::Texture s_tex;
			static GameModels::Model s_gun;
			static GameModels::Model s_pole;
			if (s_gun.parts.empty())
			{
				s_tex.width = s_tex.height = 16;
				s_tex.rgba.resize(256);
				for (int i = 0; i < 256; i++)
					s_tex.rgba[i] = (((i & 1) ^ ((i >> 4) & 1)) != 0) ? 0xFF505050u : 0xFF9A9A9Au;
				s_gun.item_id = 0;
				const float lo[3] = {-10.9f, -1.3f, -6.6f};
				const float hi[3] = {11.1f, 1.3f, 7.3f};
				for (int k = 0; k < 3; k++)
				{
					s_gun.bounds_min[k] = lo[k];
					s_gun.bounds_max[k] = hi[k];
				}
				// Two boxes: the grip and frame (slot 0) below z = 4, the slide (slot 2) above.
				const auto box = [](GameModels::Part& part, const float b0[3], const float b1[3]) {
					static const int kFace[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
					static const float kNormal[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
					for (int f = 0; f < 6; f++)
					{
						const u16 base = static_cast<u16>(part.vertices.size());
						for (int c = 0; c < 4; c++)
						{
							const int corner = kFace[f][c];
							GameModels::Vertex v;
							v.pos[0] = (corner & 1) ? b1[0] : b0[0];
							v.pos[1] = (corner & 2) ? b1[1] : b0[1];
							v.pos[2] = (corner & 4) ? b1[2] : b0[2];
							for (int k = 0; k < 3; k++)
								v.normal[k] = kNormal[f][k];
							v.uv[0] = (c == 1 || c == 2) ? 1.0f : 0.0f;
							v.uv[1] = (c >= 2) ? 1.0f : 0.0f;
							part.vertices.push_back(v);
						}
						// Wound counter-clockwise seen from outside (along the face's normal).
						const float* n = kNormal[f];
						const GameModels::Vertex& a = part.vertices[base];
						const GameModels::Vertex& b = part.vertices[base + 1];
						const GameModels::Vertex& c2 = part.vertices[base + 2];
						const V3 e1 = Sub(Load(b.pos), Load(a.pos));
						const V3 e2 = Sub(Load(c2.pos), Load(a.pos));
						const bool ccw = Dot(Cross(e1, e2), V3{n[0], n[1], n[2]}) > 0.0f;
						const u16 q[6] = {base, static_cast<u16>(base + (ccw ? 1 : 2)), static_cast<u16>(base + (ccw ? 2 : 1)),
							base, static_cast<u16>(base + (ccw ? 2 : 3)), static_cast<u16>(base + (ccw ? 3 : 2))};
						part.indices.insert(part.indices.end(), q, q + 6);
					}
				};
				GameModels::Part frame;
				frame.slot = 0;
				frame.texture = &s_tex;
				const float f0[3] = {-10.9f, -1.3f, -6.6f}, f1[3] = {11.1f, 1.3f, 4.0f};
				box(frame, f0, f1);
				GameModels::Part slide;
				slide.slot = 2;
				slide.texture = &s_tex;
				const float s0[3] = {-10.9f, -1.0f, 4.0f}, s1[3] = {11.1f, 1.0f, 7.3f};
				box(slide, s0, s1);
				s_gun.parts.push_back(std::move(frame));
				s_gun.parts.push_back(std::move(slide));
				s_gun.triangles = 24;
				// A 1.3 m pole (as item 106), along X and centred, as the game's melee models lie.
				s_pole.item_id = 106;
				const float p0[3] = {-65.0f, -1.4f, -1.4f}, p1[3] = {65.0f, 1.4f, 1.4f};
				for (int k = 0; k < 3; k++)
				{
					s_pole.bounds_min[k] = p0[k];
					s_pole.bounds_max[k] = p1[k];
				}
				GameModels::Part shaft;
				shaft.texture = &s_tex;
				box(shaft, p0, p1);
				s_pole.parts.push_back(std::move(shaft));
				s_pole.triangles = 12;
			}
			SetModelLookupForTest(+[](u16 id) -> const GameModels::Model* {
				return (id == 0) ? &s_gun : (id == 106) ? &s_pole : nullptr;
			});

			// DecodeGameAttach: an entry as File #1's weapon-type table lays it out (0x44 bytes).
			u8 entry[0x44] = {};
			entry[2] = 7;
			const float grip[3] = {-13.45f, -1.598f, 2.135f};
			const float muzzle[3] = {-9.5f, 0.0f, 5.09f};
			std::memcpy(entry + 0x0C, grip, 12);
			std::memcpy(entry + 0x34, muzzle, 12);
			GameAttach at;
			check(DecodeGameAttach(entry, sizeof(entry), 7, 0x0C, 0x24, 0x34, &at) && at.valid && at.type == 7 &&
					  near3(at.grip_cm, grip, 0.0f) && at.has_muzzle && near3(at.muzzle_cm, muzzle, 0.0f) && at.rot_rad[0] == 0.0f,
				"DecodeGameAttach: the grip, no turn and the muzzle of an entry");
			GameAttach bad;
			check(!DecodeGameAttach(entry, sizeof(entry), 8, 0x0C, 0x24, 0x34, &bad) && !bad.valid,
				"DecodeGameAttach: an entry whose byte +2 names another type is refused");
			check(!DecodeGameAttach(entry, 0x30, 7, 0x0C, 0x24, 0x34, &bad) && !DecodeGameAttach(entry, sizeof(entry), 0, 0x0C, 0x24, 0x34, &bad),
				"DecodeGameAttach: a short entry or type 0 is refused");
			u8 broken[0x44];
			std::memcpy(broken, entry, sizeof(broken));
			const float nan = std::numeric_limits<float>::quiet_NaN();
			std::memcpy(broken + 0x10, &nan, 4);
			check(!DecodeGameAttach(broken, sizeof(broken), 7, 0x0C, 0x24, 0x34, &bad), "DecodeGameAttach: a grip that is not finite is refused");
			// A spray can's entry turn, (101.8, -23.6, -77.7) degrees as s16: the model's +Y (the can's axis) then lies along
			// (-0.283, 0.339, 0.897) in the hand bone's frame, as the savestates show for the game's own matrix.
			u8 spray[0x44];
			std::memcpy(spray, entry, sizeof(spray));
			spray[2] = 27;
			const s16 turn[3] = {static_cast<s16>(0x4864), static_cast<s16>(0xEF39), static_cast<s16>(0xC8C2)};
			std::memcpy(spray + 0x24, turn, 6);
			GameAttach sp;
			const bool sp_ok = DecodeGameAttach(spray, sizeof(spray), 27, 0x0C, 0x24, 0x34, &sp);
			const M3 A = MulM3(RotZ(sp.rot_rad[2]), MulM3(RotY(sp.rot_rad[1]), RotX(sp.rot_rad[0])));
			const V3 can_axis = A.Apply(V3{0.0f, 1.0f, 0.0f});
			check(sp_ok && std::fabs(can_axis.x + 0.283f) < 0.01f && std::fabs(can_axis.y - 0.339f) < 0.01f &&
					  std::fabs(can_axis.z - 0.897f) < 0.01f,
				"GameAttach: the turn is X, then Y, then Z, as the game's own matrix (a spray can's axis)");

			// PlaceModel: the pistol's origin lands at the wrist anchor plus the grip (Kevin's scale), its barrel along -Z.
			const Placement pl = PlaceModel(at, Held::Pistol);
			const HandMesh::Anchors an = HandMesh::GetAnchors(kRight, HandMesh::Pose::Pistol);
			const V3 want_origin = Add(Mul(Load(an.wrist), 100.0f), V3{grip[1] * kGameHandScale, grip[2] * kGameHandScale,
				grip[0] * kGameHandScale});
			const V3 barrel = Normalize(pl.L.Apply(V3{-1.0f, 0.0f, 0.0f}));
			check(Length(Sub(pl.T, want_origin)) < 1.0e-4f && barrel.z < -0.99999f && pl.L.Apply(V3{0.0f, 0.0f, 1.0f}).y > 0.99999f,
				"PlaceModel: the pistol's origin at the wrist + the game's grip, its barrel along -Z, its top up");
			// The origin then sits close to where the placeholder pistol has its trigger (0, 2, -4.6) cm.
			char what2[160];
			std::snprintf(what2, sizeof(what2), "PlaceModel: Kevin's 45 Auto's origin (%.1f, %.1f, %.1f) cm is within 3 cm of the "
				"placeholder's trigger", static_cast<double>(pl.T.x), static_cast<double>(pl.T.y), static_cast<double>(pl.T.z));
			check(Length(Sub(pl.T, V3{0.0f, 2.0f, -4.6f})) < 3.0f, what2);
			GameAttach yawed = at;
			yawed.rot_rad[1] = 10.6f * kDegToRad;
			const Placement plg = PlaceModel(yawed, Held::LongGun);
			const Placement plm = PlaceModel(yawed, Held::Melee);
			check(Normalize(plg.L.Apply(V3{-1.0f, 0.0f, 0.0f})).z < -0.99999f && Normalize(plm.L.Apply(V3{-1.0f, 0.0f, 0.0f})).z > -0.99,
				"PlaceModel: a gun the game holds 10.6 deg off is turned onto -Z; a melee weapon keeps the game's turn");

			// DrawsMesh and the fallback.
			HandState hs;
			hs.valid = true;
			hs.pos[0] = 0.20f;
			hs.pos[1] = 0.90f;
			hs.pos[2] = -0.40f;
			hs.grip = 1.0f;
			hs.gun = true;
			hs.held = Held::Pistol;
			hs.item_id = 0;
			hs.attach = at;
			check(!DrawsMesh(kRight, hs), "DrawsMesh: without [VR] GameWeaponModels, today's look");
			hs.mesh = true;
			check(DrawsMesh(kRight, hs), "DrawsMesh: a weapon with its model and the game's grip is drawn as a mesh");
			HandState no_model = hs;
			no_model.item_id = 3;
			HandState no_attach = hs;
			no_attach.attach = GameAttach{};
			check(!DrawsMesh(kRight, no_model) && !DrawsMesh(kRight, no_attach),
				"DrawsMesh: no model or no grip for the weapon keeps the placeholder");
			HandState bare = hs;
			bare.gun = false;
			check(DrawsMesh(kRight, bare) && DrawsMesh(kLeft, bare), "DrawsMesh: bare hands are the hand model");

			const float view_eye[3] = {0.0f, 1.2f, 0.0f};
			const auto coverage = [](const std::vector<u32>& img, bool* touches_edge) {
				u32 n = 0;
				bool edge = false;
				for (int y = 0; y < kSize; y++)
				{
					for (int x = 0; x < kSize; x++)
					{
						if ((img[static_cast<size_t>(y) * kImageSize + x] >> 24) == 0)
							continue;
						n++;
						edge = edge || x == 0 || y == 0 || x == kSize - 1 || y == kSize - 1;
					}
				}
				if (touches_edge)
					*touches_edge = edge;
				return n;
			};
			std::vector<u32> img_mesh, img_old, img_fb_old, img_fb_mesh;
			ImpostorQuad q;
			bool edge = true;
			u32 cov = 0;
			if (PlaceImpostor(kRight, hs, view_eye, &q))
			{
				RenderImpostor(kRight, hs, view_eye, q.centre, q.right, q.up, q.size_m, img_mesh);
				cov = coverage(img_mesh, &edge);
				HandState old_look = hs;
				old_look.mesh = false;
				RenderImpostor(kRight, old_look, view_eye, q.centre, q.right, q.up, q.size_m, img_old);
			}
			std::snprintf(what2, sizeof(what2), "mesh: the hand and the model pistol fill their own quad (%u px, %.2f m) and stay "
				"off its edge", cov, static_cast<double>(q.size_m));
			check(cov >= 1500 && !edge, what2);
			u32 differ = 0;
			for (size_t p = 0; p < img_mesh.size() && p < img_old.size(); p++)
				differ += (((img_mesh[p] >> 24) != 0) != ((img_old[p] >> 24) != 0)) ? 1u : 0u;
			check(img_old.size() == kPixels && differ >= 200, "mesh: the mesh image differs from the placeholder's");

			// The fallback is exactly today's image.
			HandState fb_old = no_model;
			fb_old.mesh = false;
			ImpostorQuad qf;
			bool same = PlaceImpostor(kRight, no_model, view_eye, &qf);
			ImpostorQuad qo;
			same = same && PlaceImpostor(kRight, fb_old, view_eye, &qo) && qf.size_m == qo.size_m;
			if (same)
			{
				RenderImpostor(kRight, no_model, view_eye, qf.centre, qf.right, qf.up, qf.size_m, img_fb_mesh);
				RenderImpostor(kRight, fb_old, view_eye, qo.centre, qo.right, qo.up, qo.size_m, img_fb_old);
				same = img_fb_mesh == img_fb_old;
			}
			check(same, "mesh: a weapon without a model is drawn exactly as today (quad and image)");

			// In the holster: the weapon alone, smaller than with the hand round it.
			HandState hol = hs;
			hol.holstered = true;
			const float hip[3] = {0.22f, 0.78f, -0.05f};
			HolsterPose(Held::Pistol, hip, 0.0f, hol.pos, hol.quat);
			ImpostorQuad qh;
			u32 cov_h = 0;
			bool edge_h = true;
			if (PlaceImpostor(kRight, hol, view_eye, &qh))
			{
				std::vector<u32> img;
				RenderImpostor(kRight, hol, view_eye, qh.centre, qh.right, qh.up, qh.size_m, img);
				cov_h = coverage(img, &edge_h);
			}
			std::snprintf(what2, sizeof(what2), "mesh: the holstered model pistol is drawn in its own quad (%u px)", cov_h);
			check(cov_h >= 300 && !edge_h, what2);

			// The laser from the model's muzzle.
			HandState aim = hs;
			aim.pos[0] = aim.pos[1] = aim.pos[2] = 0.0f;
			for (int k = 0; k < 4; k++)
				aim.quat[k] = identity[k];
			float from[3], dir[3];
			const V3 want_muzzle = Add(pl.L.Apply(Load(muzzle)), pl.T);
			check(ModelLaserFrom(aim, 0.02f, from, dir) && std::fabs(from[0] - want_muzzle.x * 0.01f) < 1.0e-5f &&
					  std::fabs(from[1] - want_muzzle.y * 0.01f) < 1.0e-5f &&
					  std::fabs(from[2] - (want_muzzle.z * 0.01f - 0.02f)) < 1.0e-5f && dir[2] < -0.99999f,
				"ModelLaserFrom: the beam starts 2 cm past the model's muzzle, along -Z");
			check(!ModelLaserFrom(no_model, 0.02f, from, dir), "ModelLaserFrom: none for a weapon drawn as the placeholder");

			// NeedsReraster sees the look and the grip change.
			HandState mesh_off = hs;
			mesh_off.mesh = false;
			HandState other_type = hs;
			other_type.attach.type = 8;
			check(NeedsReraster(hs, view_eye, q.centre, mesh_off, view_eye, 0) && NeedsReraster(hs, view_eye, q.centre, other_type, view_eye, 0) &&
					  !NeedsReraster(hs, view_eye, q.centre, hs, view_eye, 0),
				"NeedsReraster: the look or the weapon type changing draws it again");

			// What a hand-and-pistol image costs here (a lit, textured mesh at 2 x 2 samples).
			if (cov > 0)
			{
				TakeMeshDrawStats();
				constexpr int kRuns = 40;
				for (int r = 0; r < kRuns; r++)
					RenderImpostor(kRight, hs, view_eye, q.centre, q.right, q.up, q.size_m, img_mesh);
				const MeshDrawStats st = TakeMeshDrawStats();
				Common::Timer t_old;
				HandState old_look = hs;
				old_look.mesh = false;
				for (int r = 0; r < kRuns; r++)
					RenderImpostor(kRight, old_look, view_eye, q.centre, q.right, q.up, q.size_m, img_old);
				const double old_ms = t_old.GetTimeMilliseconds() / kRuns;
				HandState bare_hand = hs;
				bare_hand.gun = false;
				TakeMeshDrawStats();
				for (int r = 0; r < kRuns; r++)
					RenderImpostor(kRight, bare_hand, view_eye, q.centre, q.right, q.up, q.size_m, img_fb_mesh);
				const MeshDrawStats st_bare = TakeMeshDrawStats();
				Console.WriteLn("(VR) Hands mesh timing: a hand and model pistol image takes %.3f ms (max %.3f) with MeshRaster at "
								"%u x %u samples, the hand alone %.3f ms, the placeholder's %.3f ms (%u px)",
					st.images ? static_cast<double>(st.total_us) / 1000.0 / st.images : 0.0, static_cast<double>(st.max_us) / 1000.0,
					MeshSamples(), MeshSamples(),
					st_bare.images ? static_cast<double>(st_bare.total_us) / 1000.0 / st_bare.images : 0.0, old_ms, cov);
				check(st.images == static_cast<u32>(kRuns), "mesh: every mesh image is counted in the draw stats");
			}
			// A pole held side on, too long for the largest quad: the quad still holds the hand (the pixel the grip
			// position lands on is drawn), and the pole hangs from the holster with its top end near the belt.
			{
				HandState pole = hs;
				pole.held = Held::Melee;
				pole.item_id = 106;
				pole.attach.rot_rad[0] = pole.attach.rot_rad[2] = 0.0f;
				pole.attach.rot_rad[1] = 0.5f * kPi;
				pole.attach.grip_cm[0] = -8.0f;
				pole.attach.grip_cm[1] = -2.5f;
				pole.attach.grip_cm[2] = 0.0f;
				pole.attach.has_muzzle = false;
				const float near_eye[3] = {0.2f, 1.25f, -0.15f};
				ImpostorQuad qp;
				bool hand_in = false;
				float qp_size = 0.0f;
				if (PlaceImpostor(kRight, pole, near_eye, &qp))
				{
					std::vector<u32> img;
					RenderImpostor(kRight, pole, near_eye, qp.centre, qp.right, qp.up, qp.size_m, img);
					MeshRaster::Camera cam;
					float px[2];
					if (MeshRaster::Camera::Impostor(near_eye, qp.centre, qp.right, qp.up, qp.size_m, kNearM, 100.0f, &cam) &&
						MeshRaster::ProjectPoint(cam, kImageSize, kImageSize, pole.pos, px) && px[0] >= 0.0f && px[1] >= 0.0f &&
						px[0] < kImageSize && px[1] < kImageSize)
					{
						hand_in = (img[static_cast<size_t>(px[1]) * kImageSize + static_cast<size_t>(px[0])] >> 24) != 0;
					}
					qp_size = qp.size_m;
				}
				std::snprintf(what2, sizeof(what2), "mesh: a 1.3 m pole seen side on keeps the hand in its quad (%.2f m)",
					static_cast<double>(qp_size));
				check(hand_in && qp_size <= kQuadSizeModelLongM + 1.0e-4f, what2);
				HandState hung = pole;
				hung.holstered = true;
				HolsterPose(Held::Melee, hip, 0.0f, hung.pos, hung.quat, true);
				const Placement ph = PlaceFor(hung, s_pole);
				float top = 1.0e9f, bottom = -1.0e9f;
				for (int c = 0; c < 2; c++)
				{
					const float y = Add(ph.L.Apply(V3{c ? 65.0f : -65.0f, 0.0f, 0.0f}), ph.T).y;
					top = std::min(top, y);
					bottom = std::max(bottom, y);
				}
				check(std::fabs(top + kHolsterTopCm) < 0.2f && bottom > 100.0f,
					"mesh: in the holster a pole hangs from the belt, its top end 12 cm over the hold point");
			}
			SetModelLookupForTest(nullptr);
		}

		if (failed == 0)
			Console.WriteLn(Color_StrongGreen, "(VR) Hands self-test: %d checks passed", checks);
		else
			Console.WriteLn(Color_StrongRed, "(VR) Hands self-test: %d of %d failed", failed, checks);
		return failed == 0;
	}

	int DumpImages(const char* folder, const DumpItem* items, int count)
	{
		if (!folder || !folder[0] || !items || count <= 0)
			return 0;
		FileSystem::CreateDirectoryPath(folder, true);
		// A seated head; the right hand 40 cm ahead, 30 cm below and 20 cm to the right, pointing ahead; and a second eye
		// 45 cm to the right of the hand, looking at it from the side, where the grip in the fist shows best.
		const float eye[3] = {0.0f, 1.20f, 0.0f};
		const float side_eye[3] = {0.65f, 0.92f, -0.40f};
		const float hip[3] = {0.22f, 0.78f, -0.05f};
		const float shoulder[3] = {0.18f, 1.12f, 0.12f};
		int written = 0;
		std::vector<u32> px;
		for (int i = 0; i < count; i++)
		{
			const WeaponHeld w = HeldFor(true, true, items[i].item_id);
			if (!w.raised)
				continue;
			HandState h;
			h.valid = true;
			h.mesh = true;
			h.gun = true;
			h.held = w.held;
			h.item_id = items[i].item_id;
			h.attach = items[i].attach;
			h.grip = 1.0f;
			h.pos[0] = 0.20f;
			h.pos[1] = 0.90f;
			h.pos[2] = -0.40f;
			if (!DrawsMesh(kRight, h))
				continue;
			HandState hol = h;
			hol.holstered = true;
			if (w.held == Held::LongGun)
				SlingPose(shoulder, 0.0f, hol.pos, hol.quat);
			else
				HolsterPose(w.held, hip, 0.0f, hol.pos, hol.quat, true);
			struct View
			{
				const char* name;
				const HandState* hs;
				const float* eye;
			};
			const float far_eye[3] = {1.80f, 0.95f, -0.40f};
			const View views[] = {{"hand", &h, eye}, {"side", &h, side_eye}, {"holster", &hol, eye}, {"far", &h, far_eye}};
			double ms[4] = {0.0, 0.0, 0.0, 0.0};
			for (int v = 0; v < 4; v++)
			{
				ImpostorQuad q;
				if (!PlaceImpostor(kRight, *views[v].hs, views[v].eye, &q))
					continue;
				Common::Timer t;
				RenderImpostor(kRight, *views[v].hs, views[v].eye, q.centre, q.right, q.up, q.size_m, px);
				ms[v] = t.GetTimeMilliseconds();
				const std::string path = Path::Combine(folder, fmt::format("{}_{:05}.png", views[v].name, h.item_id));
				if (px.size() == kPixels && RGBA8Image(kImageSize, kImageSize, px.data()).SaveToFile(path.c_str()))
					written++;
			}
			const char* name = ItemData::Name(h.item_id);
			Console.WriteLn("(VR) Hand image dump: item %u (%s, a %s, type %u): in the hand %.2f ms, from the side %.2f ms, %s "
							"%.2f ms.",
				static_cast<unsigned>(h.item_id), name[0] ? name : "?", HeldName(w.held), static_cast<unsigned>(h.attach.type),
				ms[0], ms[1], (w.held == Held::LongGun) ? "slung" : "holstered", ms[2]);
		}
		Console.WriteLn("(VR) Hand image dump: %d image(s) in %s (the game's own models: keep them on this PC).", written, folder);
		return written;
	}
}
