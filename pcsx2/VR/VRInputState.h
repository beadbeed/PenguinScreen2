// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include <array>
#include <cstdint>

namespace VR
{
struct VRPose
{
  std::array<float, 4> orientation_xyzw{0.f, 0.f, 0.f, 1.f};
  std::array<float, 3> position_xyz{0.f, 0.f, 0.f};
  bool valid = false;
};

struct VRHandState
{
  bool a = false;
  bool b = false;
  bool x = false;
  bool y = false;
  bool menu = false;
  bool thumbstick_click = false;
  // Only bound on gamepad-shaped controllers (Valve Steam Frame: D-pad and View on the left).
  bool dpad_up = false;
  bool dpad_down = false;
  bool dpad_left = false;
  bool dpad_right = false;
  bool bumper = false;
  bool view = false;
  float trigger = 0.f;
  float grip = 0.f;
  float thumbstick_x = 0.f;
  float thumbstick_y = 0.f;
  VRPose aim_pose;
  VRPose grip_pose;
  std::array<float, 3> grip_linear_velocity{0.f, 0.f, 0.f};
  // Some action on this hand reported isActive, i.e. the runtime has a device bound for it.
  bool input_active = false;
};

struct VRScreenHit
{
  float u = 0.5f;
  float v = 0.5f;
  bool valid = false;
  // The hand whose aim ray made the hit (VRInputSnapshot::LEFT / RIGHT); -1 for none.
  int hand = -1;
  // Metres along that aim ray from the aim pose to the hit (0 for a PINE test hit).
  float distance = 0.f;
  // The hit in the XR base space, the space the hands and the screen quad are in (zero when not known).
  std::array<float, 3> point{};
  // A scripted PINE test hit (XRInput::SetTestScreenHit) rather than a ray: it has no aim pose to start a beam from.
  bool test = false;
};

struct VRInputSnapshot
{
  static constexpr int LEFT = 0;
  static constexpr int RIGHT = 1;
  std::array<VRHandState, 2> hands;
  VRScreenHit screen_hit;
  VRPose head_pose;
  bool actions_active = false;
  std::uint64_t generation = 0;
  // Monotonic time (ms) of the publish; GetInputSnapshot() treats an old one as neutral.
  std::uint64_t published_ms = 0;
  std::int64_t display_time = 0; // XrTime the poses were located for (VRInput::Update); 0 = none
};

VRInputSnapshot GetInputSnapshot();

struct VRScreenTransform
{
  // The screen quad's centre in the XR base space, with its full orientation: +X along its width, +Y up its height, +Z
  // its face normal (pointing back at the viewer).
  VRPose center;
  float half_width = 0.f;
  float half_height = 0.f;
  // A flat world-locked quad was placed at the last present (the world screen or the menu tablet); false for the
  // first-person, curved, head-following or split screen, or none.
  bool valid = false;
  // The menu tablet placement rather than the world screen.
  bool tablet = false;
  // Monotonic time (ms) of the publish; GetScreenTransform() treats an old one as invalid (the GS is idle).
  std::uint64_t published_ms = 0;
  // Counts publishes.
  std::uint32_t generation = 0;
};

VRScreenTransform GetScreenTransform();

// The compositor, every present (any thread): the flat screen quad the menu pointer aims at, or an invalid one when there
// is none. Stamps published_ms and generation.
void PublishScreenTransform(const VRScreenTransform& transform);

// Pure: where the ray from the aim pose's position along its -Z meets the screen quad's front face, 0.05-10 m out.
// Valid only on the quad (0 <= u, v <= 1; u runs left to right, v top to bottom, like the game picture); hand is left at -1.
VRScreenHit RayHitScreen(const VRScreenTransform& screen, const VRPose& aim);

// Pure: the menu pointer's hit as GetInputSnapshot takes it (before a PINE test hit is laid over it): the right hand's aim
// ray; the left's only standing in for a right controller whose aim is not tracked (playing with one controller), or
// when left_test (the left hand is a PINE test hand). So the left controller never takes the pointer while the right one
// merely points off the screen. hand says which ray made it.
VRScreenHit AimScreenHit(const VRScreenTransform& screen, const VRPose& left_aim, const VRPose& right_aim, bool left_test);
}
