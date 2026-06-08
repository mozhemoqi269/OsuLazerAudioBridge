param(
    [string]$Version = "v0.2.0-alpha.1",
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$binDir = Join-Path $repoRoot "build\bin"
$releaseRoot = Join-Path $repoRoot "release"
$packageName = "OsuLazerAudioBridge-$Version-windows-x64"
$stagingDir = Join-Path $releaseRoot $packageName
$zipPath = Join-Path $releaseRoot "$packageName.zip"

$requiredFiles = @(
    "OsuLazerAudioBridge.exe",
    "OsuLazerAudioHost.exe",
    "OsuLazerBassHook.dll"
)

foreach ($file in $requiredFiles) {
    $path = Join-Path $binDir $file
    if (-not (Test-Path $path)) {
        throw "Missing build output: $path. Run scripts\build-native.ps1 first."
    }
}

if (Test-Path $stagingDir) {
    Remove-Item -LiteralPath $stagingDir -Recurse -Force
}
New-Item -ItemType Directory -Path $stagingDir | Out-Null

foreach ($file in $requiredFiles) {
    Copy-Item -LiteralPath (Join-Path $binDir $file) -Destination $stagingDir
}

Copy-Item -LiteralPath (Join-Path $repoRoot "LICENSE") -Destination $stagingDir
Copy-Item -LiteralPath (Join-Path $repoRoot "THIRD_PARTY_NOTICES.md") -Destination $stagingDir
Copy-Item -LiteralPath (Join-Path $repoRoot "README.md") -Destination (Join-Path $stagingDir "README_PROJECT.md")
if (Test-Path (Join-Path $repoRoot "docs\images")) {
    New-Item -ItemType Directory -Path (Join-Path $stagingDir "docs") -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repoRoot "docs\images") -Destination (Join-Path $stagingDir "docs") -Recurse -Force
}

@"
# OsuLazerAudioBridge $Version

This package contains the Windows x64 release build.

## Files

- OsuLazerAudioBridge.exe: GUI launcher
- OsuLazerAudioHost.exe: audio host / injector
- OsuLazerBassHook.dll: native BASS hook DLL
- README_PROJECT.md: full project README
- THIRD_PARTY_NOTICES.md: third-party notices
- LICENSE: repository license

Keep the three executable files in the same folder.

## Recommended Setup

1. Start osu!lazer.
2. Set osu!lazer's in-game audio output to a device that is not your low-latency ASIO path.
3. Run OsuLazerAudioBridge.exe.
4. Open the Audio Output page.
5. Select ASIO.
6. Select your ASIO driver.
7. Set sample rate to 48000.
8. Leave ASIO buffer as Driver and configure the real buffer in the ASIO driver control panel.
9. Start with Mirror audio enabled and Mirror music disabled.
10. Click Start.
11. If effects are stable, enable Mirror music and test again.

## Command Line Examples

Effects only:

```powershell
.\OsuLazerAudioHost.exe --process "osu!.exe" --mirror-audio --output-backend asio --output-sample-rate 48000 --output-buffer-ms 0 --effects-volume 100 --music-volume 75 --output-device "TOPPING Pro USB Audio Device" --no-log
```

Effects + music:

```powershell
.\OsuLazerAudioHost.exe --process "osu!.exe" --mirror-audio --mirror-music --output-backend asio --output-sample-rate 48000 --output-buffer-ms 0 --effects-volume 100 --music-volume 75 --output-device "TOPPING Pro USB Audio Device" --no-log
```

List ASIO drivers:

```powershell
.\OsuLazerAudioHost.exe --list-asio-devices
```

Open ASIO control panel:

```powershell
.\OsuLazerAudioHost.exe --asio-control-panel --output-backend asio --output-device "TOPPING Pro USB Audio Device"
```

## Troubleshooting

- If there is no sound, make sure osu!lazer is already running before starting the bridge.
- If effects work but music does not, enable Mirror music.
- If audio stutters, increase the ASIO buffer in your driver control panel.
- WASAPI Exclusive is experimental. Prefer ASIO when available.
- Use logs only when debugging; normal release usage can keep logging disabled.

## Safety Boundary

This tool is for local audio output only. It does not provide gameplay assistance,
input automation, score manipulation, network behavior, or anti-detection behavior.
"@ | Set-Content -LiteralPath (Join-Path $stagingDir "README_RELEASE.md") -Encoding UTF8

if (Test-Path $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
Compress-Archive -Path (Join-Path $stagingDir "*") -DestinationPath $zipPath -Force

Write-Host "Release package created:"
Write-Host $zipPath
