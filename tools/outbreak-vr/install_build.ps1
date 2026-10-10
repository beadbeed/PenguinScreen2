# Install a "Build Windows (VR)" artifact over D:\Games\PS2\PenguinScreen2 without touching user data
# (inis, sstates, memcards, logs, cache, vrprofiles, snaps, portable.txt ...). The previous program files
# are copied to D:\Games\PS2\PenguinScreen2-backup-<sha> first.
#   powershell -ExecutionPolicy Bypass -File install_build.ps1 -RunId 38026719941
param([Parameter(Mandatory = $true)][string]$RunId, [string]$Repo = 'beadbeed/PenguinScreen2',
      [string]$Dest = 'D:\Games\PS2\PenguinScreen2')
$ErrorActionPreference = 'Stop'
$gh = 'C:\Program Files\GitHub CLI\gh.exe'
if (Get-Process pcsx2-qt -ErrorAction SilentlyContinue) { throw 'Close PenguinScreen2 first.' }

$name = & $gh api "repos/$Repo/actions/runs/$RunId/artifacts" --jq '.artifacts[0].name'
if (-not $name) { throw "No artifact on run $RunId" }
$sha = ($name -replace '.*sha\[([0-9a-f]+)\].*', '$1')
$tmp = Join-Path $env:TEMP "ps2vr-$RunId"
if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
& $gh run download $RunId -R $Repo -n $name -D $tmp
if (-not (Test-Path (Join-Path $tmp 'pcsx2-qt.exe'))) { throw 'Artifact has no pcsx2-qt.exe' }

# Program files = whatever the artifact ships (top-level names); user data folders are never in it.
$userData = 'inis', 'sstates', 'memcards', 'logs', 'cache', 'vrprofiles', 'snaps', 'covers', 'cheats',
            'patches', 'textures', 'videos', 'inputprofiles', 'gamesettings', 'bios', 'portable.txt', 'portable.ini'
$items = Get-ChildItem $tmp | Where-Object { $userData -notcontains $_.Name }
$oldSha = 'prev'
$backup = "D:\Games\PS2\PenguinScreen2-backup-$((Get-Date).ToString('yyyyMMdd-HHmm'))"
New-Item -ItemType Directory -Force $backup | Out-Null
foreach ($i in $items) {
    $target = Join-Path $Dest $i.Name
    if (Test-Path $target) { Copy-Item $target -Destination $backup -Recurse -Force }
}
foreach ($i in $items) {
    $target = Join-Path $Dest $i.Name
    if ($i.PSIsContainer -and (Test-Path $target)) { Remove-Item $target -Recurse -Force }
    Copy-Item $i.FullName -Destination $Dest -Recurse -Force
}
Remove-Item $tmp -Recurse -Force
"installed $name into $Dest (previous program files in $backup)"
