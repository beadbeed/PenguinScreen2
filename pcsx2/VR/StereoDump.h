// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

class GSTexture;

// Headset-free stereo measurement: PCSX2_VR_STEREODUMP=<dir>[,everyN] (default every 30 frames) copies both
// layers of the first-person multiview frame to the host and writes <dir>/stereo_<frame>_L.png and _R.png with
// a JSON sidecar, for tools/outbreak-vr/stereo_metric.py. Offline only: refused while the network adapter is on.
// The readback is recorded into the GS device's own command buffer and collected once its fence has passed, so
// it never waits on the GPU and never changes emulation speed.
namespace VR::StereoDump
{
	// GS thread, once per presented frame, right after VR::EndOfFrame (nullptr for a blank frame).
	void MaybeCapture(GSTexture* current);
}
