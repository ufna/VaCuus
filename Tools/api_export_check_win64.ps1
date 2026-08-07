<#
Tools/api_export_check_win64.ps1 -- the Win64 twin of Tools/api_export_check.sh (bead
VaCuus-dgl for the invariant, VaCuus-akj.10.7 for this file). Does the DELIVERED BINARY
still export the supported C++ surface?

    powershell -ExecutionPolicy Bypass -File Tools\api_export_check_win64.ps1 <package-dir>

<package-dir> is any tree with a *.uplugin at its root and Binaries\Win64 next to it: a
`RunUAT BuildPlugin -Package=<dir>` output with FilterPlugin.ini's `-/Binaries/...` rule
disabled, or a host project's Plugins\VaCuus after an editor build.

WHY A SECOND SCRIPT INSTEAD OF A PLATFORM SWITCH. The .sh is ELF to the bone -- `nm -D`,
Itanium mangling, and a visibility model Windows does not have. Only the QUESTION ports.
Keeping them apart lets each state its own mechanism instead of a shared comment that is
half wrong on both.

THE MECHANISM THIS GUARDS, AND HOW IT DIFFERS FROM LINUX. On Linux UBT compiles modular
targets with `-fvisibility-ms-compat` (LinuxToolChain.cs:439) = global TYPES keep default
visibility, global functions and variables go hidden -- so a UCLASS with no _API macro
still has an exported vtable and typeinfo while every hand-written member is invisible.
Windows has no such split: without `__declspec(dllexport)` -- which is all the module's
`<MODULE>_API` macro is -- a symbol simply is not in the export table. And UHT does not
paper over it: the generated class body takes `Module.Api` only for a `MinimalAPI` class
and `NO_API` otherwise (UhtHeaderCodeGeneratorHFile.cs:833), so what the class declares
about itself is what the buyer gets.

The FAILURE SHAPE is nevertheless identical, which is the whole reason this check is worth
writing twice: registration does not need a single export of its own. The .gen.cpp's
registration objects are constructed inside this module at static init and call into
CoreUObject's exports, not this module's -- so a UCLASS with no _API macro registers fine,
Blueprint keeps working, the editor shows the class, and a buyer with precompiled binaries
cannot link one line of C++ against it. "Blueprint works" is not evidence on either
platform. That is the bug shape VaCuus actually shipped for four milestones (bead dgl:
UVaCuusWidget, header in Private/, no VACUUSRENDER_API), and no in-process automation test
can see it -- a test links against the same module it tests, where hidden symbols resolve
fine. The export table is the only observable there is.

THE CHECK MUST BE SEEN TO FAIL BEFORE IT MAY PASS -- Tools/fab_scan.sh's rule, and the .sh's.
Five legs, three of which can fail the run:
  1. REQUIRED   each supported class must have >= 1 exported member in its module;
  2. INTERNAL   FVaCuusRmlDocumentHost must have ZERO, because the CreateView seam
                deliberately does not hand buyers a constructible host (VaCuusRender.Build.cs
                carries that decision) -- this leg fails if someone exports the render
                backend by reflex;
  3. SELF-TEST  the REQUIRED matcher is run once against a class name that cannot exist. If
                that does not come back 0, the matcher is broken and the run ABORTS rather
                than reporting a green it did not earn;
  4. QUICKJS    vendored patch #1 keeps every JS_* symbol out of VaCuusJs's export table
                (VaCuusJs.Build.cs states the same check for ELF). Zero is the pass;
  5. TOTALS     printed, never asserted -- a number to eyeball across builds, not a gate.

WHY THE MATCHER IS THE SAME STRING AS THE LINUX ONE. `dumpbin /EXPORTS` prints the
undecorated form in parentheses after the decorated name, so a member reads as
`... (public: void __cdecl UVaCuusView::Foo(void))` and a class used only as a PARAMETER
reads as `class UVaCuusView *` -- no `::`. Matching "<Class>::" therefore separates members
from mentions without demangling anything by hand, exactly as `nm --demangle | grep
'Class::'` does on Linux. Both are line matches, so both count a member once per exported
symbol (overloads and each `const`/non-const pair count separately); the number is a
tripwire, not a census.

PROVENANCE, stated because it is short and the alternative is a reader trusting the wrong
thing: this file was written 2026-08-03 against the DLLs the Win64 pass built (§6 of
docs/passport/2026-08-vacuus-win64-results.md reports that pass's run). The pass's own
script did not survive its scratch tree, so this is a rewrite that had to re-earn its green
rather than a file recovered from that day.

ONE NUMBER DIFFERS FROM §6, AND IT IS THIS SCRIPT THAT IS RIGHT. Against the identical
DLLs every count reproduces -- 27 / 53 / 39 / 14, the four module totals 385 / 143 / 2 /
1746 / 2, zero JS_ -- except UVaCuusView, which §6 reports as 66 and this reports as 65.
The cause is one symbol:

    ?GetImeStatus@UVaCuusView@@QEBA?AUFImeStatus@1@XZ
        (public: struct UVaCuusView::FImeStatus __cdecl UVaCuusView::GetImeStatus(void)const)

whose undecorated form names the class TWICE -- once as the nested return type, once as the
qualified method name. §6's script counted occurrences; this one counts exported symbols,
which is both the honest unit ("how many symbols can a buyer link") and the unit the Linux
twin already uses (`grep -c` is a line count). The two scripts agreeing matters more than
either agreeing with a number from a lost file.
#>

[CmdletBinding()]
param(
	[Parameter(Position = 0)]
	[string] $PackageDir,

	# Escape hatch for a machine where vswhere cannot find the toolchain.
	[string] $Dumpbin
)

$ErrorActionPreference = 'Stop'

function Abort([string] $Message) {
	Write-Host "ABORT: $Message"
	exit 2
}

if ([string]::IsNullOrWhiteSpace($PackageDir)) {
	Write-Host "usage: powershell -ExecutionPolicy Bypass -File Tools\api_export_check_win64.ps1 <package-dir>"
	exit 2
}

# Fail closed on anything that is not a package with Win64 binaries: a missing directory
# must never read as "no violations found".
if (-not (Test-Path -LiteralPath $PackageDir)) {
	Abort "'$PackageDir' does not exist"
}
if (-not (Get-ChildItem -LiteralPath $PackageDir -Filter *.uplugin -File -ErrorAction SilentlyContinue)) {
	Abort "no *.uplugin at '$PackageDir' -- that is not a plugin package"
}

$BinDir = Join-Path $PackageDir 'Binaries\Win64'
if (-not (Test-Path -LiteralPath $BinDir)) {
	Abort @"
'$BinDir' does not exist -- this package carries no Win64 binaries,
       so there is nothing to check. (Source-only packages are the Fab upload shape;
       comment out FilterPlugin.ini's -/Binaries/... to get a binary drop.)
"@
}

if ([string]::IsNullOrWhiteSpace($Dumpbin)) {
	$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
	if (-not (Test-Path -LiteralPath $VsWhere)) {
		Abort "vswhere.exe not found at '$VsWhere' -- pass -Dumpbin <path to dumpbin.exe>"
	}
	# -find returns every host/target combination; the x64-hosted x64 one is what an
	# engine build uses, and any of them reads a PE the same way.
	$Found = & $VsWhere -latest -products * -find '**\Hostx64\x64\dumpbin.exe' 2>$null
	$Dumpbin = $Found | Select-Object -First 1
}
if ([string]::IsNullOrWhiteSpace($Dumpbin) -or -not (Test-Path -LiteralPath $Dumpbin)) {
	Abort "dumpbin.exe not found -- install the MSVC toolset or pass -Dumpbin <path>"
}

# THE SUPPORTED SURFACE, as module/class. Adding a class here is how you promise it; see
# VaCuusRender.Build.cs for what is deliberately absent and why. Kept byte-identical in
# spirit to the .sh's REQUIRED list -- the two must never drift, because a class promised
# on one platform and not the other is the same defect wearing a different mangling.
$Required = @(
	@{ Module = 'VaCuusRender'; Class = 'UVaCuusWidget' }          # screen host -- UMG, or any Slate tree via TakeWidget()
	@{ Module = 'VaCuusRender'; Class = 'UVaCuusWorldComponent' }  # world host -- a panel on a quad
	@{ Module = 'VaCuus'; Class = 'UVaCuusView' }                  # the handle everything is driven through
	@{ Module = 'VaCuus'; Class = 'UVaCuusSubsystem' }             # owns views; the CreateView extension seam
	@{ Module = 'VaCuus'; Class = 'UVaCuusStyleSet' }              # RCSS material decorators (rcss-matrix.md)
	@{ Module = 'VaCuusRender'; Class = 'SVaCuusWidget' }         # the hand-composition door: SUBCLASSED to route
	                                                              # input between stacked views (bead VaCuus-akj.25)
)

# Deliberately NOT exported. Zero members is the pass.
#
# THIS LEG SURVIVED akj.25 UNCHANGED, and that is the point of keeping it: the bead added
# VaCuusSlateView::MakeDocumentHost(), which HANDS BACK an FVaCuusRmlDocumentHost, without
# exporting a single one of its members. The factory is a free function; the host crosses as
# the public IVaCuusDocumentHost interface it implements, and the caller can only call
# through that vtable. If a future edit exports the class itself, the render backend has
# become ABI and this line is what says so.
#
# FVaCuusSlateElement is the same promise for the other half of the pair: MakeElement()
# returns TSharedRef<FVaCuusSlateElement> to an INCOMPLETE type, so a consumer can hold the
# handle and drop it and do nothing else. A member export here would mean the glass distiller
# and the replay contract had leaked into the supported surface.
#
# MSVC MAKES THIS LEG SHARPER THAN THE ELF ONE, which is why it is worth stating on the
# Windows twin: a dllexported class exports EVERY member including implicit ones, so an
# accidental VACUUSRENDER_API on either of these would show up here as dozens of members
# rather than as one symbol.
$Forbidden = @(
	@{ Module = 'VaCuusRender'; Class = 'FVaCuusRmlDocumentHost' }
	@{ Module = 'VaCuusRender'; Class = 'FVaCuusSlateElement' }
)

$Modules = @('VaCuus', 'VaCuusRender', 'VaCuusJs', 'VaCuusRml', 'VaCuusEditor')

# One dumpbin run per DLL, cached: five legs read the same tables and dumpbin is not fast.
$script:ExportCache = @{}

function Get-ExportLines([string] $Module) {
	if ($script:ExportCache.ContainsKey($Module)) {
		return $script:ExportCache[$Module]
	}

	# Editor-target naming, same assumption the .sh makes with libUnrealEditor-*.so. A
	# packaged game names its modules after the target, so this script is for editor and
	# BuildPlugin drops; a game drop needs the prefix parameterised, and would deserve a
	# fixture before it is trusted.
	$Dll = Join-Path $BinDir "UnrealEditor-$Module.dll"
	if (-not (Test-Path -LiteralPath $Dll)) {
		$script:ExportCache[$Module] = $null
		return $null
	}

	$Raw = & $Dumpbin /NOLOGO /EXPORTS $Dll 2>&1
	# Export rows only: "  ordinal hint RVA name...". The header block above them carries
	# its own decimal counts (" 385 number of names"), which would otherwise be counted as
	# exports by a looser filter.
	$Lines = @($Raw | Where-Object { $_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+\S' })
	$script:ExportCache[$Module] = $Lines
	return $Lines
}

# Counts exported member symbols of Class:: in one module's DLL. Returns -1 for "no DLL",
# which every leg must handle rather than treat as zero -- a missing binary reading as
# "0 exported members" would fail REQUIRED for the right number and pass FORBIDDEN for the
# wrong reason.
function Measure-ClassMembers([string] $Module, [string] $Class) {
	$Lines = Get-ExportLines $Module
	if ($null -eq $Lines) {
		return -1
	}
	return @($Lines | Where-Object { $_ -like "*$Class`::*" }).Count
}

$Failures = 0

Write-Host "== supported surface (must be reachable from a buyer's C++) =="
foreach ($Entry in $Required) {
	$N = Measure-ClassMembers $Entry.Module $Entry.Class
	if ($N -lt 0) {
		Write-Host ("  FAIL  {0}/{1}: UnrealEditor-{0}.dll not in the package" -f $Entry.Module, $Entry.Class)
		$Failures++
	}
	elseif ($N -lt 1) {
		Write-Host ("  FAIL  {0}/{1}: 0 exported members -- the class registers but cannot be linked" -f $Entry.Module, $Entry.Class)
		$Failures++
	}
	else {
		Write-Host ("  ok    {0}/{1}: {2} exported member references" -f $Entry.Module, $Entry.Class, $N)
	}
}

Write-Host "== internals (must stay unreachable) =="
foreach ($Entry in $Forbidden) {
	$N = Measure-ClassMembers $Entry.Module $Entry.Class
	if ($N -lt 0) {
		Write-Host ("  FAIL  {0}/{1}: UnrealEditor-{0}.dll not in the package" -f $Entry.Module, $Entry.Class)
		$Failures++
	}
	elseif ($N -gt 0) {
		Write-Host ("  FAIL  {0}/{1}: {2} exported members -- the render backend leaked into the ABI" -f $Entry.Module, $Entry.Class, $N)
		$Failures++
	}
	else {
		Write-Host ("  ok    {0}/{1}: 0 exported members" -f $Entry.Module, $Entry.Class)
	}
}

# Leg 3. The matcher is asked a question whose answer must be 0. Anything else and every
# "ok" above is unearned, so this aborts instead of failing: a broken matcher is not a
# finding about the binary.
$Sentinel = Measure-ClassMembers 'VaCuusRender' 'UVaCuusThisClassDoesNotExist'
if ($Sentinel -ne 0) {
	Abort @"
self-test broken -- a class that cannot exist reported '$Sentinel' members,
       so the REQUIRED leg above proved nothing.
"@
}
Write-Host "== self-test: absent class correctly reports 0 (the FAIL path works) =="

# Leg 4. Vendored patch #1 hides quickjs's own API. On ELF that is `nm -D | grep ' JS_'`
# (VaCuusJs.Build.cs says so in the module that owns the patch); on PE the export table is
# the same question with no visibility attribute involved -- if a JS_ symbol is exported,
# the patch was lost in a re-vendoring and VENDORED_TAG.txt has the procedure to restore it.
Write-Host "== quickjs containment =="
$JsLines = Get-ExportLines 'VaCuusJs'
if ($null -eq $JsLines) {
	Write-Host "  FAIL  UnrealEditor-VaCuusJs.dll not in the package"
	$Failures++
}
else {
	$JsExports = @($JsLines | Where-Object {
			# The name column, before dumpbin's " = decorated (undecorated)" tail.
			$Name = ($_ -replace '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+', '') -split ' = ' | Select-Object -First 1
			$Name -match '^JS_'
		})
	if ($JsExports.Count -gt 0) {
		Write-Host ("  FAIL  {0} exported JS_* symbol(s) -- vendored patch #1 is gone" -f $JsExports.Count)
		$Failures++
	}
	else {
		Write-Host "  ok    exported JS_* symbols: 0 (expected 0)"
	}
}

# Leg 5. Reported, never asserted: the totals move with every legitimate code change, so a
# gate on them would cry wolf. They are here because "VaCuusRml exports 1746 and VaCuusJs
# exports 2" is the one line that makes the containment result legible at a glance.
Write-Host "== module export totals (reported, not gated) =="
foreach ($Module in $Modules) {
	$Lines = Get-ExportLines $Module
	if ($null -eq $Lines) {
		Write-Host ("  --    {0}: no DLL in this package" -f $Module)
	}
	else {
		Write-Host ("        {0}: {1} exported symbols" -f $Module, $Lines.Count)
	}
}

if ($Failures -gt 0) {
	Write-Host "RESULT: $Failures violation(s)."
	exit 1
}
Write-Host "RESULT: clean -- the supported C++ surface survives binary delivery on Win64."
exit 0
