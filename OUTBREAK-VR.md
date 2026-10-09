# Outbreak VR branch

This branch is part of **Outbreak VR**: Resident Evil Outbreak (PS2, Japanese File #1 / #2
with the English patch) as a true-3D VR game for the Valve Steam Frame.

It is one of two builds that share the same VR ideas:

- **PenguinScreen2, branch `windows-steamvr`** (this repo): Windows PC build streamed to the Frame through SteamVR; stereo, controllers and gesture zones
- **ARMSX2, branch `outbreak-vr`** (https://github.com/beadbeed/ARMSX2/tree/outbreak-vr): Steam Frame standalone (Linux arm64 AppImage built by GitHub Actions); per-eye stereo is done, the headset session is not

Outbreak-specific VR settings are in `bin/resources/vr-profiles/SLPM-65428.yaml and SLPM-65692.yaml`.
This repo holds code only. No game files, BIOS or saves are ever committed here.
