<#
Tools/build_editor_win64.ps1 — build the editor target of a Win64 host project.

    powershell -ExecutionPolicy Bypass -File Tools\build_editor_win64.ps1
    powershell -ExecutionPolicy Bypass -File Tools\build_editor_win64.ps1 `
        -Project C:\Other\VcHost\VcHost.uproject -Target OtherEditor

Repo-only: Tools/ is not in the plugin package (BuildPlugin's default filter has no rule
for it), so this is a dev-loop helper and not something a buyer receives.

The defaults describe the machine the Win64 matrix was executed on, where the host project
keeps the stock TP_ThirdPerson template module and target names and only the .uproject was
renamed — hence a target that does not match the project's file name. Override both when
the layout differs.

NO UE_SDKS_ROOT HERE, DELIBERATELY. The 2026-08-03 pass needed an AutoSDK shim because that
machine had no .NET Framework SDK and `SwarmInterface.Build.cs:29-34` throws when
`NetFxSdkDir` is null, which no editor target survives. The SDK was installed on 2026-08-04
and the shim was deleted (bead akj.10.9), so re-introducing the variable here would rebuild
the workaround for a problem that no longer exists — which is exactly the trap that bead's
closure warns about. If a fresh machine hits the same wall, install the VS component
`Microsoft.Net.Component.4.6.2.SDK` rather than shimming it.
#>
[CmdletBinding()]
param(
	[string] $Project = 'C:\VaCuusWin64Test\VcHost\VcHost.uproject',
	[string] $Target  = 'TP_ThirdPersonEditor',
	[string] $Engine  = 'C:\Program Files\Epic Games\UE_5.8'
)

$Build = Join-Path $Engine 'Engine\Build\BatchFiles\Build.bat'
if (-not (Test-Path $Build)) { Write-Host "ABORT: no Build.bat at '$Build'"; exit 2 }
if (-not (Test-Path $Project)) { Write-Host "ABORT: no project at '$Project'"; exit 2 }

# NO EDITOR MAY BE RUNNING: it holds the module DLLs open and the link step fails on a file
# it cannot replace. Report the PIDs instead of killing, so the caller decides — and never
# kill by name pattern, which on this project has historically matched the caller's own
# shell (CLAUDE.md's dev-loop hazards).
$Running = Get-Process UnrealEditor, UnrealEditor-Cmd -ErrorAction SilentlyContinue
if ($Running) {
	Write-Host 'ABORT: an editor is running and holds the module DLLs. PIDs:'
	$Running | Select-Object Id, ProcessName | Format-Table -AutoSize
	exit 2
}

& $Build $Target Win64 Development -project="$Project" -waitmutex
exit $LASTEXITCODE
