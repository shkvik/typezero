# Shows how the debugger of Visual Studio sees the variables of a tz program
# built with `tz build --debug` (docs/debug.md, section 5): the views of
# tz.natvis, without hands. Needs Visual Studio 2022 (Community will do).
#
# The program reads a line from stdin and then stops at `debugger`:
#
#   import { readLine } from "tz:io"
#   function look() {
#     const user = { name: "Ann" }
#     readLine()
#     debugger
#   }
#
#   powershell -File editors\debug\vs-check.ps1 -Exe app.exe -Function look
#
# The script starts the program, attaches the debugger through EnvDTE, sends
# the line, waits for the stop, and prints the variables of the frame of
# -Function down to -Depth levels of members. With -BreakFile and -BreakLine
# it also puts a breakpoint there and continues to it; -Steps steps over that
# many times and prints the line of each step.
param(
  [Parameter(Mandatory = $true)][string]$Exe,
  [string]$Function = "",
  [int]$Depth = 1,
  [string]$BreakFile = "",
  [int]$BreakLine = 0,
  [int]$Steps = 0
)

$ErrorActionPreference = "Stop"

# Visual Studio answers "busy" while it works; such calls are repeated.
function Retry([scriptblock]$Action) {
  for ($i = 0; $i -lt 100; $i++) {
    try {
      return & $Action
    } catch {
      $busy = $_.Exception.HResult -eq -2147418111 -or $_.Exception.HResult -eq -2147417846
      if (-not $busy) { throw }
      Start-Sleep -Milliseconds 200
    }
  }
  throw "Visual Studio stays busy"
}

function Where-Now($dte) {
  try {
    $document = $dte.ActiveDocument
    return $document.Name + ":" + $document.Selection.ActivePoint.Line
  } catch {
    return "?"
  }
}

function Wait-Break($dte) {
  for ($i = 0; $i -lt 150; $i++) {
    if ((Retry { $dte.Debugger.CurrentMode }) -eq 2) { return $true }
    Start-Sleep -Milliseconds 200
  }
  return $false
}

function Show($expression, $indent, $level) {
  "{0}{1} = {2}  [{3}]" -f $indent, $expression.Name, $expression.Value, $expression.Type
  if ($level -lt $Depth) {
    foreach ($member in $expression.DataMembers) {
      if ($member.Name -ne "[Raw View]") { Show $member ($indent + "    ") ($level + 1) }
    }
  }
}

$dte = New-Object -ComObject VisualStudio.DTE.17.0
$process = $null
try {
  Retry { $dte.SuppressUI = $true } | Out-Null
  $start = New-Object System.Diagnostics.ProcessStartInfo (Resolve-Path $Exe).Path
  $start.RedirectStandardInput = $true
  $start.UseShellExecute = $false
  $process = [System.Diagnostics.Process]::Start($start)
  Start-Sleep -Milliseconds 500
  $target = Retry { $dte.Debugger.LocalProcesses | Where-Object { $_.ProcessID -eq $process.Id } }
  if (-not $target) { throw "the program is not among the processes Visual Studio sees" }
  Retry { $target.Attach() } | Out-Null
  Start-Sleep -Milliseconds 1500
  if ($BreakFile) {
    Retry { $dte.ItemOperations.OpenFile((Resolve-Path $BreakFile).Path) } | Out-Null
    Retry { $dte.ActiveDocument.Selection.GotoLine($BreakLine) } | Out-Null
    Retry { $dte.ExecuteCommand("Debug.ToggleBreakpoint") } | Out-Null
  }
  $process.StandardInput.WriteLine("go")
  $process.StandardInput.Flush()
  if (-not (Wait-Break $dte)) { throw "the program did not stop" }
  "stopped at " + (Where-Now $dte)
  if ($BreakFile -and -not ((Where-Now $dte) -like "*:$BreakLine")) {
    Retry { $dte.Debugger.Go($true) } | Out-Null
    "stopped at " + (Where-Now $dte)
  }
  for ($k = 1; $k -le $Steps; $k++) {
    Retry { $dte.Debugger.StepOver($true) } | Out-Null
    "step ${k}: " + (Where-Now $dte)
  }
  $chosen = $null
  foreach ($frame in (Retry { $dte.Debugger.CurrentThread.StackFrames })) {
    $name = Retry { $frame.FunctionName }
    "frame: $name"
    if (-not $chosen -and $Function -and $name -eq $Function) { $chosen = $frame }
  }
  if ($chosen) {
    Retry { $dte.Debugger.CurrentStackFrame = $chosen } | Out-Null
    foreach ($local in (Retry { $chosen.Locals })) { Show $local "" 0 }
  }
  Retry { $dte.Debugger.TerminateAll() } | Out-Null
} finally {
  try { $dte.Quit() } catch {}
  if ($process -and -not $process.HasExited) { $process.Kill() }
}
