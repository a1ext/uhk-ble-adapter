# Opens Altium's DRC dialog and presses "Run Design Rule Check..." (altium-mcp guide, section 7).
Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class W {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static string Text(IntPtr h) { var s = new StringBuilder(512); GetWindowText(h, s, 512); return s.ToString(); }
  public static List<IntPtr> Tops(uint[] pids) {
    var r = new List<IntPtr>();
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (Array.IndexOf(pids, p) >= 0 && IsWindowVisible(h)) r.Add(h); return true; }, IntPtr.Zero);
    return r;
  }
  public static List<IntPtr> Children(IntPtr p) {
    var r = new List<IntPtr>();
    EnumChildWindows(p, (h, l) => { r.Add(h); return true; }, IntPtr.Zero);
    return r;
  }
}
"@
$x2 = [uint32[]]@(Get-Process -Name X2 | ForEach-Object { $_.Id })
Start-Process "C:\Program Files\Altium\AD25\X2.EXE" -ArgumentList '"-RPCB:DesignRuleCheck"'
$dlg = [IntPtr]::Zero
for ($i = 0; $i -lt 40 -and $dlg -eq [IntPtr]::Zero; $i++) {
  Start-Sleep -Milliseconds 500
  foreach ($h in [W]::Tops($x2)) { if ([W]::Text($h) -like "*Design Rule Checker*") { $dlg = $h } }
}
if ($dlg -eq [IntPtr]::Zero) { "no DRC dialog"; exit 1 }
Start-Sleep -Seconds 3
$btn = [IntPtr]::Zero
foreach ($c in [W]::Children($dlg)) { if (([W]::Text($c) -replace '&', '') -like "Run Design Rule Check*") { $btn = $c } }
if ($btn -eq [IntPtr]::Zero) { "no Run button"; exit 1 }
[W]::PostMessage($btn, 0x0201, [IntPtr]1, [IntPtr]0x000A000A) | Out-Null
[W]::PostMessage($btn, 0x0202, [IntPtr]0, [IntPtr]0x000A000A) | Out-Null
for ($i = 0; $i -lt 60; $i++) {
  Start-Sleep -Milliseconds 500
  $open = $false
  foreach ($h in [W]::Tops($x2)) { if ([W]::Text($h) -like "*Design Rule Checker*") { $open = $true } }
  if (-not $open) { "DRC done"; exit 0 }
}
"DRC dialog still open"
