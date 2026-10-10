#!/usr/bin/env bash
# First-person world stability on SteamVR's null headset (python null_driver.py on first): for each
# variant, write a temporary user override of the File #1 profile, restart in VR with a fake swaying
# head, load slot 2 and measure how much the J's Bar neon sign moves in the Headset Window.
#   ./sync_sweep.sh time 0 1 2      # "time" = no viewMatrix (poseLagMs guess); a number = syncFramesBack
cd "$(dirname "$0")"
SHIP="D:/Games/PS2/PenguinScreen2/resources/vr-profiles/SLPM-65428.yaml"
OVR="D:/Games/PS2/PenguinScreen2/vrprofiles/SLPM-65428.yaml"
mkdir -p "$(dirname "$OVR")"
for V in "$@"; do
  if [ "$V" = "time" ]; then
    sed "/^      viewMatrix:/d" "$SHIP" > "$OVR"
  else
    sed "s/^      syncFramesBack: [0-9]*/      syncFramesBack: $V/" "$SHIP" > "$OVR"
  fi
  powershell -NoProfile -Command "\$p = Get-Process pcsx2-qt -ErrorAction SilentlyContinue | Select-Object -First 1; if (\$p) { \$p.CloseMainWindow() | Out-Null; if (-not \$p.WaitForExit(10000)) { Stop-Process -Id \$p.Id -Force } }; Start-Sleep 2; \$env:XR_RUNTIME_JSON = 'E:\ProgramFile\Steam\steamapps\common\SteamVR\steamxr_win64.json'; \$env:PCSX2_VR_FAKE_HEADPOSE = '25'; \$env:PCSX2_VR_SYNCLOG = '1'; Start-Process -FilePath 'D:\Games\PS2\PenguinScreen2\pcsx2-qt.exe' -ArgumentList '--vr','--','\"D:\Games\PS2\games\Biohazard Outbreak File 1 (SLPM-65428) EN-patched.iso\"' -WorkingDirectory 'D:\Games\PS2\PenguinScreen2'"
  PYTHONIOENCODING=utf-8 python - <<'PY'
import sys, time; sys.path.insert(0, '.')
from pine import wait_for_pine, PineError
p = wait_for_pine(seconds=120)
for _ in range(120):
    try:
        if p.game_id() and p.status() == "running": break
    except PineError: pass
    time.sleep(1)
time.sleep(5); p.load_state(2); time.sleep(6)
PY
  powershell -NoProfile -ExecutionPolicy Bypass -File mute.ps1 >/dev/null
  echo -n "variant $V: "; PYTHONIOENCODING=utf-8 python sway_metric.py 30 0.13
  cp "D:/Games/PS2/PenguinScreen2/logs/emulog.txt" "D:/Games/PS2/research/sync-$V-emulog.txt" 2>/dev/null
done
rm -f "$OVR"; echo "override removed"
