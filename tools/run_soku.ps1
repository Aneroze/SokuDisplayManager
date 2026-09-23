# Autonomous Soku run-harness for the WindowResizer render investigation.
# SAFETY: refuses to launch if th123/th123e is already running (never touches a live game);
# only ever closes processes it can attribute to its own launch (guaranteed by the pre-check).
#
# Usage:
#   run_soku.ps1 -Width 1280 -Tag 1280start -WaitSec 12
#   run_soku.ps1 -Width 640  -Tag 640start  -NoClose      # leave running for manual char-select nav
#   run_soku.ps1 -CloseOnly                                # close a run left open with -NoClose

param(
  [int]$Width = 0,            # 0 = leave WR [Size] as-is; >0 = set WR [Size] Enabled=1 Width=N Height=N*3/4
  [int]$WaitSec = 12,
  [string]$Tag = "run",
  [switch]$NoClose,           # leave the game running (so you can navigate to character-select)
  [switch]$CloseOnly          # just close a previously-launched game and scoop the logs
)
$ErrorActionPreference = 'Stop'
$soku = 'F:\Games\Touhou\SokuLauncher\Soku'
$runs = 'C:\Users\Andrei\AppData\Local\Temp\claude\C--Projects-SokuMods\08395012-6829-487c-880b-bd82f744aa06\scratchpad\runs'
New-Item -ItemType Directory -Force $runs | Out-Null
$logs = @{ DrawProbe = "$soku\modules\DrawProbe\DrawProbe.log"
           SamplerProbe = "$soku\modules\SamplerProbe\SamplerProbe.log"
           DMProbe = "$soku\modules\DMProbe\DMProbe.log" }

function Scoop($tag) {
  foreach ($k in $logs.Keys) { if (Test-Path $logs[$k]) { Copy-Item $logs[$k] "$runs\$k-$tag.log" -Force; Write-Host "scooped $k -> $runs\$k-$tag.log" } }
}
function CloseGame {
  $g = Get-Process -Name th123,th123e -ErrorAction SilentlyContinue
  if ($g) { $g | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep 1; Write-Host "closed th123 (PIDs: $($g.Id -join ','))" }
  else { Write-Host "no th123 process to close" }
}

if ($CloseOnly) { Scoop $Tag; CloseGame; exit 0 }

# --- GUARD: never launch over a running game ---
$pre = Get-Process -Name th123,th123e -ErrorAction SilentlyContinue
if ($pre) { Write-Host "ABORT: th123 already running (PIDs: $($pre.Id -join ',')). Not launching."; exit 2 }

# --- set startup window size via WindowResizer.ini (kernel32 for reliability) ---
if ($Width -gt 0) {
  Add-Type -Namespace Win -Name Ini -MemberDefinition '[System.Runtime.InteropServices.DllImport("kernel32")] public static extern bool WritePrivateProfileString(string s,string k,string v,string f);' -ErrorAction SilentlyContinue
  $wr = "$soku\modules\WindowResizer\WindowResizer.ini"
  $h = [int]($Width * 3 / 4)
  [Win.Ini]::WritePrivateProfileString("Size","Enabled","1",$wr) | Out-Null
  [Win.Ini]::WritePrivateProfileString("Size","Width","$Width",$wr) | Out-Null
  [Win.Ini]::WritePrivateProfileString("Size","Height","$h",$wr) | Out-Null
  Write-Host "WR [Size] set to ${Width}x${h}"
}

# --- clear probe logs so this run is clean ---
foreach ($k in $logs.Keys) { Remove-Item $logs[$k] -ErrorAction SilentlyContinue }

# --- launch ---
Write-Host "launching th123e.exe ..."
Start-Process "$soku\th123e.exe" -WorkingDirectory $soku | Out-Null
Start-Sleep -Seconds $WaitSec

$g = Get-Process -Name th123,th123e -ErrorAction SilentlyContinue
Write-Host "after ${WaitSec}s, th123 processes: $(@($g).Count) (PIDs: $($g.Id -join ','))"

if ($NoClose) { Scoop $Tag; Write-Host "LEFT RUNNING (-NoClose). Navigate to character-select, then run with -CloseOnly -Tag $Tag."; exit 0 }

Scoop $Tag
CloseGame
Write-Host "DONE tag=$Tag"
