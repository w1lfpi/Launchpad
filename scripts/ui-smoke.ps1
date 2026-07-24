param(
    [ValidateSet(
        "Info",
        "Show",
        "Capture",
        "Drag",
        "PostDrag",
        "PostDragEscape",
        "PostDragReturn",
        "Click",
        "PostClick",
        "ConfirmDelete",
        "ConfirmYes",
        "PostType",
        "PostEscape")]
    [string]$Action = "Info",
    [double]$FromX = 0.0,
    [double]$FromY = 0.0,
    [double]$ToX = 0.0,
    [double]$ToY = 0.0,
    [int]$HoldMilliseconds = 120,
    [string]$Text = "",
    [switch]$Clear,
    [string]$Output = ""
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$nativeSource = @"
using System;
using System.Runtime.InteropServices;

public static class LaunchpadSmokeNative
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string className, string title);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out RECT rectangle);

    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr window);

    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr window, int id);

    [DllImport("user32.dll")]
    public static extern IntPtr SendMessage(
        IntPtr window,
        uint message,
        UIntPtr wParam,
        IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr window);

    [DllImport(
        "user32.dll",
        EntryPoint = "PostMessageW",
        CharSet = CharSet.Unicode)]
    public static extern bool PostMessage(
        IntPtr window,
        uint message,
        UIntPtr wParam,
        IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern void mouse_event(
        uint flags,
        uint x,
        uint y,
        uint data,
        UIntPtr extraInfo);
}
"@
Add-Type -TypeDefinition $nativeSource

$window = [LaunchpadSmokeNative]::FindWindow(
    "WindowsLaunchpad.Window",
    [NullString]::Value)
if ($window -eq [IntPtr]::Zero) {
    throw "WindowsLaunchpad.Window is not running."
}

$rectangle = New-Object LaunchpadSmokeNative+RECT
if (-not [LaunchpadSmokeNative]::GetWindowRect(
        $window,
        [ref]$rectangle)) {
    throw "GetWindowRect failed."
}
[LaunchpadSmokeNative]::SetForegroundWindow($window) | Out-Null
Start-Sleep -Milliseconds 120
$width = $rectangle.Right - $rectangle.Left
$height = $rectangle.Bottom - $rectangle.Top
$dpiScale =
    [LaunchpadSmokeNative]::GetDpiForWindow($window) / 96.0

function Convert-Point([double]$x, [double]$y) {
    [Drawing.Point]::new(
        [int][Math]::Round(
            ($rectangle.Left + $x * $width) * $dpiScale),
        [int][Math]::Round(
            ($rectangle.Top + $y * $height) * $dpiScale))
}

function Invoke-Click([Drawing.Point]$point) {
    [LaunchpadSmokeNative]::SetCursorPos($point.X, $point.Y) |
        Out-Null
    [LaunchpadSmokeNative]::mouse_event(
        0x0002,
        0,
        0,
        0,
        [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 45
    [LaunchpadSmokeNative]::mouse_event(
        0x0004,
        0,
        0,
        0,
        [UIntPtr]::Zero)
}

function Save-Capture([string]$path) {
    $bitmap = [Drawing.Bitmap]::new($width, $height)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen(
            $rectangle.Left,
            $rectangle.Top,
            0,
            0,
            [Drawing.Size]::new($width, $height))
        $bitmap.Save($path, [Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

function New-MouseLParam([int]$x, [int]$y) {
    [IntPtr](($y -band 0xFFFF) -shl 16 -bor ($x -band 0xFFFF))
}

function Confirm-DeleteModal {
    $x = [int][Math]::Round(
        $width * 0.5 + 72.0)
    $y = [int][Math]::Round(
        $height * 0.5 + 80.0)
    [LaunchpadSmokeNative]::PostMessage(
        $window,
        0x0201,
        [UIntPtr]::new(1),
        (New-MouseLParam $x $y)) | Out-Null
    Start-Sleep -Milliseconds 45
    [LaunchpadSmokeNative]::PostMessage(
        $window,
        0x0202,
        [UIntPtr]::Zero,
        (New-MouseLParam $x $y)) | Out-Null
}

switch ($Action) {
    "Info" {
        [pscustomobject]@{
            Handle = $window
            Left = $rectangle.Left
            Top = $rectangle.Top
            Width = $width
            Height = $height
            DpiScale = $dpiScale
        }
    }
    "Show" {
        [LaunchpadSmokeNative]::PostMessage(
            $window,
            0x802A,
            [UIntPtr]::Zero,
            [IntPtr]::Zero) | Out-Null
        Start-Sleep -Milliseconds 500
    }
    "Capture" {
        if ([string]::IsNullOrWhiteSpace($Output)) {
            throw "-Output is required for Capture."
        }
        Save-Capture $Output
        Get-Item $Output
    }
    "Click" {
        Invoke-Click (Convert-Point $FromX $FromY)
    }
    "PostClick" {
        $x = [int][Math]::Round($FromX * $width)
        $y = [int][Math]::Round($FromY * $height)
        [LaunchpadSmokeNative]::PostMessage(
            $window,
            0x0201,
            [UIntPtr]::new(1),
            (New-MouseLParam $x $y)) | Out-Null
        Start-Sleep -Milliseconds 45
        [LaunchpadSmokeNative]::PostMessage(
            $window,
            0x0202,
            [UIntPtr]::Zero,
            (New-MouseLParam $x $y)) | Out-Null
    }
    "ConfirmYes" {
        Confirm-DeleteModal
    }
    "ConfirmDelete" {
        Confirm-DeleteModal
    }
    "PostType" {
        if ($Clear) {
            foreach ($step in 1..48) {
                [LaunchpadSmokeNative]::PostMessage(
                    $window,
                    0x0102,
                    [UIntPtr]::new(8),
                    [IntPtr]::new(1)) | Out-Null
            }
        }
        foreach ($character in $Text.ToCharArray()) {
            [LaunchpadSmokeNative]::PostMessage(
                $window,
                0x0102,
                [UIntPtr]::new([uint32][char]$character),
                [IntPtr]::new(1)) | Out-Null
            Start-Sleep -Milliseconds 18
        }
    }
    "PostEscape" {
        [LaunchpadSmokeNative]::PostMessage(
            $window,
            0x0100,
            [UIntPtr]::new(27),
            [IntPtr]::new(1)) | Out-Null
    }
    "Drag" {
        $from = Convert-Point $FromX $FromY
        $to = Convert-Point $ToX $ToY
        [LaunchpadSmokeNative]::SetCursorPos($from.X, $from.Y) |
            Out-Null
        [LaunchpadSmokeNative]::mouse_event(
            0x0002,
            0,
            0,
            0,
            [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 380
        foreach ($step in 1..36) {
            $progress = $step / 36.0
            $x = [int][Math]::Round(
                $from.X + ($to.X - $from.X) * $progress)
            $y = [int][Math]::Round(
                $from.Y + ($to.Y - $from.Y) * $progress)
            [LaunchpadSmokeNative]::SetCursorPos($x, $y) |
                Out-Null
            Start-Sleep -Milliseconds 8
        }
        Start-Sleep -Milliseconds 120
        if (-not [string]::IsNullOrWhiteSpace($Output)) {
            Save-Capture $Output
        }
        [LaunchpadSmokeNative]::mouse_event(
            0x0004,
            0,
            0,
            0,
            [UIntPtr]::Zero)
    }
    "PostDrag" {
        # PostMessage performs DPI virtualization between this PowerShell
        # process and the per-monitor-aware target window. Supply the
        # coordinates reported by GetWindowRect exactly once.
        $fromXpx = [int][Math]::Round(
            $FromX * $width)
        $fromYpx = [int][Math]::Round(
            $FromY * $height)
        $toXpx = [int][Math]::Round(
            $ToX * $width)
        $toYpx = [int][Math]::Round(
            $ToY * $height)
        [LaunchpadSmokeNative]::PostMessage(
            $window,
            0x0201,
            [UIntPtr]::new(1),
            (New-MouseLParam $fromXpx $fromYpx)) | Out-Null
        Start-Sleep -Milliseconds 390
        foreach ($step in 1..36) {
            $progress = $step / 36.0
            $x = [int][Math]::Round(
                $fromXpx + ($toXpx - $fromXpx) * $progress)
            $y = [int][Math]::Round(
                $fromYpx + ($toYpx - $fromYpx) * $progress)
            [LaunchpadSmokeNative]::PostMessage(
                $window,
                0x0200,
                [UIntPtr]::new(1),
                (New-MouseLParam $x $y)) | Out-Null
            Start-Sleep -Milliseconds 8
        }
        Start-Sleep -Milliseconds $HoldMilliseconds
        [LaunchpadSmokeNative]::PostMessage(
            $window,
            0x0202,
            [UIntPtr]::Zero,
            (New-MouseLParam $toXpx $toYpx)) | Out-Null
    }
    "PostDragEscape" {
        $fromXpx = [int][Math]::Round($FromX * $width)
        $fromYpx = [int][Math]::Round($FromY * $height)
        $toXpx = [int][Math]::Round($ToX * $width)
        $toYpx = [int][Math]::Round($ToY * $height)
        [LaunchpadSmokeNative]::PostMessage(
            $window,
            0x0201,
            [UIntPtr]::new(1),
            (New-MouseLParam $fromXpx $fromYpx)) | Out-Null
        Start-Sleep -Milliseconds 390
        foreach ($step in 1..36) {
            $progress = $step / 36.0
            $x = [int][Math]::Round(
                $fromXpx + ($toXpx - $fromXpx) * $progress)
            $y = [int][Math]::Round(
                $fromYpx + ($toYpx - $fromYpx) * $progress)
            [LaunchpadSmokeNative]::PostMessage(
                $window,
                0x0200,
                [UIntPtr]::new(1),
                (New-MouseLParam $x $y)) | Out-Null
            Start-Sleep -Milliseconds 8
        }
        Start-Sleep -Milliseconds $HoldMilliseconds
        [LaunchpadSmokeNative]::PostMessage(
            $window,
            0x0100,
            [UIntPtr]::new(27),
            [IntPtr]::new(1)) | Out-Null
        Start-Sleep -Milliseconds 120
        [LaunchpadSmokeNative]::PostMessage(
            $window,
            0x0202,
            [UIntPtr]::Zero,
            (New-MouseLParam $toXpx $toYpx)) | Out-Null
    }
    "PostDragReturn" {
        $fromXpx = [int][Math]::Round($FromX * $width)
        $fromYpx = [int][Math]::Round($FromY * $height)
        $toXpx = [int][Math]::Round($ToX * $width)
        $toYpx = [int][Math]::Round($ToY * $height)
        [LaunchpadSmokeNative]::PostMessage(
            $window,
            0x0201,
            [UIntPtr]::new(1),
            (New-MouseLParam $fromXpx $fromYpx)) | Out-Null
        foreach ($step in 1..18) {
            $progress = $step / 18.0
            $x = [int][Math]::Round(
                $fromXpx + ($toXpx - $fromXpx) * $progress)
            $y = [int][Math]::Round(
                $fromYpx + ($toYpx - $fromYpx) * $progress)
            [LaunchpadSmokeNative]::PostMessage(
                $window,
                0x0200,
                [UIntPtr]::new(1),
                (New-MouseLParam $x $y)) | Out-Null
            Start-Sleep -Milliseconds 8
        }
        foreach ($step in 1..18) {
            $progress = $step / 18.0
            $x = [int][Math]::Round(
                $toXpx + ($fromXpx - $toXpx) * $progress)
            $y = [int][Math]::Round(
                $toYpx + ($fromYpx - $toYpx) * $progress)
            [LaunchpadSmokeNative]::PostMessage(
                $window,
                0x0200,
                [UIntPtr]::new(1),
                (New-MouseLParam $x $y)) | Out-Null
            Start-Sleep -Milliseconds 8
        }
        [LaunchpadSmokeNative]::PostMessage(
            $window,
            0x0202,
            [UIntPtr]::Zero,
            (New-MouseLParam $fromXpx $fromYpx)) | Out-Null
    }
}
