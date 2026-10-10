// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include <string>

class GSTexture;
class GSVector4;

namespace VR
{
	std::string GetRuntimeInfoReport();

	void UpdateSettings();

	void ApplySceneStereo();

	void ClearSpatialControls();

	bool WantsVR();

	bool EffectiveVREnabled(bool cfg_enable);

	void SetLaunchRequestedVR(bool requested);

	bool LaunchRequestedVR();

	void SetLaunchSeat(int seat);
	void SetSeatCastTarget(int seat);
	int SeatCastTarget();
	bool SeatCastArmed();
	int GetLaunchSeat();
	std::string ResolveSeatRuntimeDir(int seat);
	std::string ResolveSeatRuntimeJson(int seat);

	void SetStereoRenderArmed(bool armed);

	bool StereoRenderArmed();

	constexpr bool StereoGateOpen(bool cfg_enable, bool launch_armed, bool render_armed, bool stereo_mode,
		bool profile_ok)
	{
		return (cfg_enable || launch_armed) && stereo_mode && profile_ok && (launch_armed || render_armed);
	}

	enum class SessionStatus
	{
		DisabledInConfig,
		NotLaunchedForVR,
		Inactive,
		Active,
	};
	SessionStatus GetSessionStatus();

	bool IsSessionActive();

	void EnsureFrameSubmitted();

	// GS thread: the headset's dark-room lift as ShadeBoost params (brightness, contrast, saturation,
	// gamma; 1.0 neutral), from the profile's presentation block and the VR Brightness setting. False
	// (leave the frame alone) without a running session or when the lift is neutral.
	bool PresentationLift(GSVector4* params);

	void EndOfFrame(GSTexture* current);
}
