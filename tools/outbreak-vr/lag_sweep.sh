#!/usr/bin/env bash
# Try poseLagMs values with a temporary user override profile; measure world stability for each.
SHIP="D:/Games/PS2/PenguinScreen2/resources/vr-profiles/SLPM-65428.yaml"
OVR="D:/Games/PS2/PenguinScreen2/vrprofiles/SLPM-65428.yaml"
for LAG in "$@"; do
  sed "s/poseLagMs: [0-9.]*/poseLagMs: $LAG/" "$SHIP" > "$OVR"
  powershell -NoProfile -Command "\$p = Get-Process pcsx2-qt -ErrorAction SilentlyContinue | Select-Object -First 1; if (\$p) { \$p.CloseMainWindow() | Out-Null; if (-not \$p.WaitForExit(8000)) { Stop-Process -Id \$p.Id -Force } }; Start-Sleep 2; \$env:XR_RUNTIME_JSON = 'E:\ProgramFile\Steam\steamapps\common\SteamVR\steamxr_win64.json'; \$env:PCSX2_VR_FAKE_HEADPOSE = '25'; Start-Process -FilePath 'D:\Games\PS2\PenguinScreen2\pcsx2-qt.exe' -ArgumentList '--vr','--','\"D:\Games\PS2\games\Biohazard Outbreak File 1 (SLPM-65428) EN-patched.iso\"' -WorkingDirectory 'D:\Games\PS2\PenguinScreen2'"
  PYTHONIOENCODING=utf-8 python - <<'PY'
import sys, time; sys.path.insert(0,'.')
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
  echo -n "poseLagMs=$LAG: "; PYTHONIOENCODING=utf-8 python sway_metric.py 24 0.17
done
rm -f "$OVR"; echo "override removed"
