param(
    [Parameter(Mandatory = $true)][string]$PackageDirectory,
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory
)

$ErrorActionPreference = 'Stop'
$package = (Resolve-Path $PackageDirectory).Path
New-Item -ItemType Directory -Force -Path $EvidenceDirectory | Out-Null
$evidence = (Resolve-Path $EvidenceDirectory).Path

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class TetrisWindow {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out Rect r);
    [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr h);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("gdi32.dll")] public static extern bool BitBlt(IntPtr d, int x, int y, int w, int h, IntPtr s, int sx, int sy, uint op);
}
'@

$app = Start-Process -FilePath "$package/main.exe" -WorkingDirectory $package -PassThru
function Assert-Alive {
    $app.Refresh()
    if ($app.HasExited -or $app.MainWindowHandle -eq 0) {
        throw 'The packaged game stopped or lost its window.'
    }
}
function Send-Key([int]$key) {
    Assert-Alive
    [void][TetrisWindow]::SendMessage($app.MainWindowHandle, 0x100, [IntPtr]$key, [IntPtr]1)
    [void][TetrisWindow]::SendMessage($app.MainWindowHandle, 0x101, [IntPtr]$key, [IntPtr]0xC0000001L)
    Start-Sleep -Milliseconds 80
}
function Save-Window([string]$name) {
    Assert-Alive
    # Force the real WM_PAINT path before reading the client pixels.
    [void][TetrisWindow]::SendMessage($app.MainWindowHandle, 0xF, [IntPtr]::Zero, [IntPtr]::Zero)
    $rect = [TetrisWindow+Rect]::new()
    if (-not [TetrisWindow]::GetClientRect($app.MainWindowHandle, [ref]$rect)) { throw 'GetClientRect failed.' }
    $bitmap = [System.Drawing.Bitmap]::new($rect.Right, $rect.Bottom)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $destination = $graphics.GetHdc()
    $source = [TetrisWindow]::GetDC($app.MainWindowHandle)
    try {
        if (-not [TetrisWindow]::BitBlt($destination, 0, 0, $rect.Right, $rect.Bottom, $source, 0, 0, 0x00CC0020)) {
            throw 'Capturing the real game window failed.'
        }
    } finally {
        [void][TetrisWindow]::ReleaseDC($app.MainWindowHandle, $source)
        $graphics.ReleaseHdc($destination)
        $graphics.Dispose()
    }
    try {
        $colors = [System.Collections.Generic.HashSet[int]]::new()
        for ($y = 10; $y -lt $bitmap.Height; $y += 20) {
            for ($x = 10; $x -lt $bitmap.Width; $x += 20) {
                [void]$colors.Add($bitmap.GetPixel($x, $y).ToArgb())
            }
        }
        if ($colors.Count -lt 8) { throw "Window capture appears blank: $name" }
        $bitmap.Save((Join-Path $evidence "$name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    } finally { $bitmap.Dispose() }
    Write-Output "Captured packaged executable: $name"
}

try {
    if (-not $app.WaitForInputIdle(10000)) { throw 'The packaged game did not become ready.' }
    Assert-Alive
    [void][TetrisWindow]::SetForegroundWindow($app.MainWindowHandle)
    Save-Window '01-packaged-main-menu'
    Send-Key 13 # Enter: start
    Save-Window '02-packaged-game-start'
    foreach ($key in @(37, 39, 38, 90, 40, 32, 67)) {
        Send-Key $key # Left, right, clockwise, counterclockwise, soft/hard drop, hold
    }
    Save-Window '03-packaged-movement-drop-hold'
    Send-Key 80 # P: pause
    Save-Window '04-packaged-paused'
    Send-Key 13 # Enter: continue
    Send-Key 112 # F1: settings
    Save-Window '05-packaged-settings'
    Send-Key 27 # Escape: close settings
    Send-Key 82 # R: restart
    Save-Window '06-packaged-restarted'
    # All pieces remain in their spawn columns, so the center stack tops out.
    # Space is ignored while the qualifying-score name entry is open.
    for ($i = 0; $i -lt 30; $i++) { Send-Key 32 }
    Save-Window '07-packaged-stacked-game-over'
    Send-Key 27 # Escape: submit the default leaderboard name
    Send-Key 82 # R: restart from game over
    Save-Window '08-packaged-after-game-over-restart'
    if (-not $app.CloseMainWindow() -or -not $app.WaitForExit(10000)) { throw 'The packaged game did not close cleanly.' }
    if ($app.ExitCode -ne 0) { throw "The packaged game exited with code $($app.ExitCode)." }
    foreach ($file in @('tetris_highscore.txt', 'tetris_leaderboard.txt', 'tetris_settings.txt')) {
        if (-not (Test-Path (Join-Path $package $file))) { throw "The game did not persist $file" }
    }
    $savedHighScore = [int]::Parse((Get-Content (Join-Path $package 'tetris_highscore.txt') -Raw).Trim())
    $savedLeaderboard = (Get-Content (Join-Path $package 'tetris_leaderboard.txt') -Raw).Trim()
    if ($savedHighScore -le 0 -or $savedLeaderboard -notmatch '^[A-Z0-9]{3,12}\|[1-9][0-9]*') {
        throw 'The real game did not persist the scored game-over leaderboard entry.'
    }
    @{
        executable = 'main.exe'
        result = 'passed'
        inputMethod = 'Win32 WM_KEYDOWN/WM_KEYUP sent to the shipped executable'
        scenarios = @('start', 'move left/right', 'rotate both ways', 'soft/hard drop', 'hold', 'pause/resume', 'settings open/close', 'restart', 'center-stack game over', 'leaderboard submission', 'restart after game over', 'clean exit and saved data')
        semanticAssertions = 'See native UI integration test log; this packaged-executable pass checks input dispatch, rendered frames, liveness and persistence'
        screenshots = @(Get-ChildItem $evidence -Filter '*.png' | ForEach-Object Name)
        savedHighScore = $savedHighScore
        savedLeaderboard = $savedLeaderboard
        audioListeningTest = 'not performed'
    } | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $evidence 'packaged-gameplay-result.json')
} finally {
    if (-not $app.HasExited) { $app.Kill(); [void]$app.WaitForExit(5000) }
    Remove-Item "$package/tetris_highscore.txt", "$package/tetris_leaderboard.txt", "$package/tetris_settings.txt" -ErrorAction SilentlyContinue
}
