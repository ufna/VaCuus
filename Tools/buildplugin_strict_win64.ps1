<#
Tools/buildplugin_strict_win64.ps1 — `RunUAT BuildPlugin -StrictIncludes` for Win64
(bead akj.10.8; executed 2026-08-04, results in
docs/passport/2026-08-vacuus-win64-results.md §12.4).

    powershell -ExecutionPolicy Bypass -File Tools\buildplugin_strict_win64.ps1
    powershell -ExecutionPolicy Bypass -File Tools\buildplugin_strict_win64.ps1 `
        -Plugin C:\VaCuus\VaCuus.uplugin -OutDir C:\VaCuusStrict\Package

Repo-only dev-loop helper; Tools/ does not ship in the plugin package.

WHY THE LEG EXISTS: `-StrictIncludes` turns off unity and PCHs, so every header must include
what it uses instead of inheriting it from whatever the compiler saw first. MSVC's two-phase
name lookup is stricter than clang's, which makes Win64 the platform most likely to find one
— that is the whole argument for running it here as well as on Linux and macOS.

VERIFY THE FLAG TOOK EFFECT, DO NOT ASSUME IT: a silently-ignored flag produces exactly the
same green. The log must say `Building with precompiled headers and unity disabled`, every
UBT invocation must carry `-NoPCH -NoSharedPCH -DisableUnity`, and the compile actions must
name individual `.cpp` files rather than `Module.<X>.<n>.cpp` — the only legitimate
`Module.*` units are UHT's generated `Module.<X>.gen.cpp`, one per module.

QUOTE THE UAT ARGUMENTS, and this is not style. Written bare as `-Plugin=$Plugin`,
PowerShell passes the token LITERALLY and UAT reports
`Plugin 'C:\Program Files\Epic Games\UE_5.8\$Plugin' not found` one second in. The array of
double-quoted strings below is what makes expansion happen and survives spaces in paths.

NO UE_SDKS_ROOT: the .NET Framework SDK is installed on the matrix machine and the AutoSDK
shim was deleted (bead akj.10.9). See Tools/build_editor_win64.ps1 for the full note.
#>
[CmdletBinding()]
param(
	[string] $Plugin = 'C:\VaCuus\VaCuus.uplugin',
	[string] $OutDir = 'C:\VaCuusStrict\Package',
	[string] $Engine = 'C:\Program Files\Epic Games\UE_5.8'
)

$RunUAT = Join-Path $Engine 'Engine\Build\BatchFiles\RunUAT.bat'
if (-not (Test-Path $RunUAT)) { Write-Host "ABORT: no RunUAT.bat at '$RunUAT'"; exit 2 }
if (-not (Test-Path $Plugin)) { Write-Host "ABORT: no .uplugin at '$Plugin'"; exit 2 }

$Running = Get-Process UnrealEditor, UnrealEditor-Cmd -ErrorAction SilentlyContinue
if ($Running) {
	Write-Host 'ABORT: an editor is running:'
	$Running | Select-Object Id, ProcessName | Format-Table -AutoSize
	exit 2
}

# CLAUDE.md records that BuildPlugin's HostProject editor leg can rewrite
# Engine\Binaries\**\UnrealEditor.modules and resurrect stale platform modules, after which
# the next editor launch dies on !bIsRunningPlatform. Baseline it so "it did / did not touch
# the engine" is measured. On the Installed engine of 2026-08-04 it did not.
$Manifest = Join-Path $Engine 'Engine\Binaries\Win64\UnrealEditor.modules'
$Before = if (Test-Path $Manifest) { (Get-FileHash $Manifest -Algorithm SHA256).Hash } else { $null }
Write-Host "MANIFEST BEFORE: $Before"

if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$UatArgs = @('BuildPlugin', "-Plugin=$Plugin", "-Package=$OutDir", '-TargetPlatforms=Win64', '-StrictIncludes')
Write-Host ("UAT ARGS: " + ($UatArgs -join ' '))

$Start = Get-Date
& $RunUAT @UatArgs
$Code = $LASTEXITCODE
$Elapsed = (Get-Date) - $Start

$After = if (Test-Path $Manifest) { (Get-FileHash $Manifest -Algorithm SHA256).Hash } else { $null }

Write-Host ''
Write-Host ("EXIT={0}  ELAPSED={1:N0}s" -f $Code, $Elapsed.TotalSeconds)
Write-Host ("MANIFEST AFTER : $After")
Write-Host ("MANIFEST TOUCHED: {0}" -f ($Before -ne $After))
if (Test-Path $OutDir) {
	Write-Host ("PACKAGE FILES: {0}" -f @(Get-ChildItem $OutDir -Recurse -File |
		Where-Object { $_.FullName -notmatch '\\HostProject\\' }).Count)
}
exit $Code
