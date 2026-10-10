# Launch PenguinScreen2 in VR through SteamVR (per-process XR_RUNTIME_JSON; system default untouched).
param([string]$Iso = 'D:\Games\PS2\games\Biohazard Outbreak File 1 (SLPM-65428) EN-patched.iso')
$env:XR_RUNTIME_JSON = 'E:\ProgramFile\Steam\steamapps\common\SteamVR\steamxr_win64.json'
Remove-Item Env:PCSX2_VR_FAKE_HEADPOSE -ErrorAction SilentlyContinue
$log = 'D:\Games\PS2\PenguinScreen2\logs\emulog.txt'
if (Test-Path $log) { Copy-Item $log ('D:\Games\PS2\PenguinScreen2\logs\emulog-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.txt') }   # keep the last session's log
Start-Process -FilePath 'D:\Games\PS2\PenguinScreen2\pcsx2-qt.exe' -ArgumentList '--vr', '--', "`"$Iso`"" -WorkingDirectory 'D:\Games\PS2\PenguinScreen2'
# Record the session (screenshots + game state, emulog copy) to D:\Games\PS2\sessions until the emulator closes.
Start-Process -FilePath 'pythonw' -ArgumentList "`"$PSScriptRoot\session_recorder.py`"" -WindowStyle Hidden -ErrorAction SilentlyContinue
