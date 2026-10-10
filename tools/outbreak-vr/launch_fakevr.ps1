# Launch PenguinScreen2 "for VR" with no OpenXR runtime (bootstrap fails -> runs flat) and a fake,
# swaying head pose, so the first-person camera code can be exercised without a headset.
param([double]$Amplitude = 25, [string]$Iso = 'D:\Games\PS2\games\Biohazard Outbreak File 1 (SLPM-65428) EN-patched.iso')
$env:XR_RUNTIME_JSON = 'D:\Games\PS2\no-openxr-runtime.json'
$env:PCSX2_VR_FAKE_HEADPOSE = "$Amplitude"
$log = 'D:\Games\PS2\PenguinScreen2\logs\emulog.txt'
if (Test-Path $log) { Copy-Item $log ('D:\Games\PS2\PenguinScreen2\logs\emulog-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.txt') }   # keep the last session's log
Start-Process -FilePath 'D:\Games\PS2\PenguinScreen2\pcsx2-qt.exe' -ArgumentList '--vr', '--', "`"$Iso`"" -WorkingDirectory 'D:\Games\PS2\PenguinScreen2'
