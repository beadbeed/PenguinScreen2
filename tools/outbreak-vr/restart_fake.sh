#!/usr/bin/env bash
# Restart PenguinScreen2 headset-free with a fake swaying head pose (amplitude in degrees, default 25),
# load a savestate slot (default 2) and mute it. Keeps the previous emulog as logs/emulog-<time>.txt.
AMP="${1:-25}"; SLOT="${2:-2}"
cd "$(dirname "$0")"
powershell -NoProfile -Command "\$p = Get-Process pcsx2-qt -ErrorAction SilentlyContinue | Select-Object -First 1; if (\$p) { \$p.CloseMainWindow() | Out-Null; if (-not \$p.WaitForExit(10000)) { Stop-Process -Id \$p.Id -Force } }; Start-Sleep 1; \$l = 'D:\Games\PS2\PenguinScreen2\logs\emulog.txt'; if (Test-Path \$l) { Copy-Item \$l (\"D:\Games\PS2\PenguinScreen2\logs\emulog-\" + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.txt') }"
powershell -NoProfile -ExecutionPolicy Bypass -File launch_fakevr.ps1 -Amplitude "$AMP" >/dev/null
PYTHONIOENCODING=utf-8 python - "$SLOT" <<'PY'
import sys, time; sys.path.insert(0, '.')
from pine import wait_for_pine, PineError
p = wait_for_pine(seconds=120)
for _ in range(120):
    try:
        if p.game_id() and p.status() == "running": break
    except PineError: pass
    time.sleep(1)
time.sleep(4); p.load_state(int(sys.argv[1])); time.sleep(4)
print("loaded slot", sys.argv[1])
PY
powershell -NoProfile -ExecutionPolicy Bypass -File mute.ps1 | tail -1
