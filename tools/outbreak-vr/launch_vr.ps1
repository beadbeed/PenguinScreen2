# Launch PenguinScreen2 in VR through SteamVR (per-process XR_RUNTIME_JSON; system default untouched).
param([string]$Iso = 'D:\Games\PS2\games\Biohazard Outbreak File 1 (SLPM-65428) EN-patched.iso')
$env:XR_RUNTIME_JSON = 'E:\ProgramFile\Steam\steamapps\common\SteamVR\steamxr_win64.json'
Remove-Item Env:PCSX2_VR_FAKE_HEADPOSE -ErrorAction SilentlyContinue
Start-Process -FilePath 'D:\Games\PS2\PenguinScreen2\pcsx2-qt.exe' -ArgumentList '--vr', '--', "`"$Iso`"" -WorkingDirectory 'D:\Games\PS2\PenguinScreen2'
