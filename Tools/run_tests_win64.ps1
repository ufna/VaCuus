<#
Tools/run_tests_win64.ps1 — run the VaCuus automation suite headless on Win64.

    powershell -ExecutionPolicy Bypass -File Tools\run_tests_win64.ps1
    powershell -ExecutionPolicy Bypass -File Tools\run_tests_win64.ps1 -Filter VaCuus.Render

Repo-only dev-loop helper; Tools/ does not ship in the plugin package. The Linux twin of
this recipe is in CLAUDE.md.

THE TRAILING COMMA IN -ExecCmds IS LOAD-BEARING, and this is the one thing to copy if you
retype the command by hand. `-ExecCmds` splits on COMMAS, not semicolons
(`ParseExecCommands.cpp:11-53`); the value also swallows every argument that follows it
(`bShouldStopOnSeparator=false`, `ParseExecCommands.cpp:63`) while the launcher re-appends
arguments at the END — so a value with no trailing comma becomes one malformed command,
runs nothing, and logs no complaint anywhere. A recipe written with `;` fails the same way,
silently.

READ THE RESULT FROM THE LOG, NOT FROM STDOUT: an interleaved UnrealTraceServer fork
clobbers the tail of every run. And the editor frequently does not exit at all — `Quit` is
dispatched at frame 0, deferred, and never fires — so if this hangs after
`Sending StopTestSession`, kill it BY PID.
#>
[CmdletBinding()]
param(
	[string] $Filter  = 'VaCuus',
	[string] $Project = 'C:\VaCuusWin64Test\VcHost\VcHost.uproject',
	[string] $Engine  = 'C:\Program Files\Epic Games\UE_5.8'
)

$Editor = Join-Path $Engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if (-not (Test-Path $Editor)) { Write-Host "ABORT: no UnrealEditor-Cmd.exe at '$Editor'"; exit 2 }
if (-not (Test-Path $Project)) { Write-Host "ABORT: no project at '$Project'"; exit 2 }

$Log = Join-Path (Split-Path $Project) ('Saved\Logs\' + [IO.Path]::GetFileNameWithoutExtension($Project) + '.log')

& $Editor $Project "-ExecCmds=Automation RunTests $Filter, Quit" -unattended -nullrhi -nosplash

Write-Host ''
Write-Host "--- from $Log ---"
if (-not (Test-Path $Log)) {
	Write-Host 'no log — the session did not get far enough to write one'
	exit 1
}
Select-String -Path $Log -Pattern 'Automation Test Queue Empty' | Select-Object -Last 1 |
	ForEach-Object { $_.Line.Trim() }
$Fails = @(Select-String -Path $Log -Pattern 'Result=\{Fail\}')
Write-Host ("failures: {0}" -f $Fails.Count)
$Fails | Select-Object -First 20 | ForEach-Object { '  ' + $_.Line.Trim() }
