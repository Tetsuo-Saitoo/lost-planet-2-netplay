# Bring one of the running test copies to the foreground (keyboard input only reaches the foreground copy).
#   focus_lp2.ps1 -Index 0|1     (order as written to re-work\pids.txt by the test launch)
param([int]$Index = 0)

Add-Type -Namespace LP2 -Name Focus -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
'@
$pids = (Get-Content (Join-Path $PSScriptRoot '..\re-work\pids.txt')).Split(' ')
$h = (Get-Process -Id $pids[$Index]).MainWindowHandle
# Windows only lets the foreground process hand over focus; a synthetic Alt press lifts that lock.
[LP2.Focus]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
[LP2.Focus]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
[LP2.Focus]::BringWindowToTop($h) | Out-Null
"foreground: copy $Index -> $([LP2.Focus]::SetForegroundWindow($h))"
