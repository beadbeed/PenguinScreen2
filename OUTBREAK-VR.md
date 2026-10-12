# Outbreak VR branch

This branch is part of **Outbreak VR**: Resident Evil Outbreak (PS2, Japanese File #1 / #2
with the English patch) as a true-3D, first-person VR game for the Valve Steam Frame.

**Start with `tools/outbreak-vr/STATUS.md`**: where things stand, how to test, what's next and what needs investigation.

It is one of two builds that share the same VR ideas:

- **PenguinScreen2** (this repo; working branches `pc-firstperson` and `coop-firstperson`): the Windows PC build, streamed to the Frame through SteamVR. It has first person, stereo, controllers, hands, menus and co-op safety.
- **ARMSX2, branch `outbreak-vr`** (https://github.com/beadbeed/ARMSX2/tree/outbreak-vr): the Steam Frame standalone build (a Linux arm64 AppImage built by GitHub Actions). Per-eye stereo is done; the headset session is not. The plan to bring this repo's VR layer over is `tools/outbreak-vr/STEAM-FRAME.md`.

The Outbreak-specific VR settings are in `bin/resources/vr-profiles/SLPM-65428.yaml` and `SLPM-65692.yaml`.
This repo holds code only. No game files, BIOS or saves are ever committed here.
