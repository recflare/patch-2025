<#
.SYNOPSIS
    Builds the solution (2025Patch.dll, Injector.exe and PreserveLoader.dll).

.DESCRIPTION
    Locates MSBuild via vswhere, builds 2025Patch.sln, and reports the resulting
    artifacts in x64\<Configuration>\. Release|x64 is the only working configuration:
    the ml64.exe CustomBuild step for RetSpoof.asm is conditioned on Release|x64 only,
    so a Debug build has no _spoofer_stub to link against.

    After a successful build it also stages the drop-in files -- 2025patch.ini and the two
    .bat launchers -- next to the binaries, so the output directory is the complete set to
    copy into the game folder.

.EXAMPLE
    .\build.ps1
    .\build.ps1 -Rebuild
    .\build.ps1 -Clean
    .\build.ps1 -Verbosity normal
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'x86')]
    [string]$Platform = 'x64',

    # Clean only, then exit.
    [switch]$Clean,

    # Clean then build.
    [switch]$Rebuild,

    [ValidateSet('quiet', 'minimal', 'normal', 'detailed', 'diagnostic')]
    [string]$Verbosity = 'minimal'
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$solution = Join-Path $root '2025Patch.sln'

function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -products * `
            -requires Microsoft.Component.MSBuild `
            -find 'MSBuild\**\Bin\amd64\MSBuild.exe'
        if (-not $found) {
            $found = & $vswhere -latest -products * `
                -requires Microsoft.Component.MSBuild `
                -find 'MSBuild\**\Bin\MSBuild.exe'
        }
        if ($found) { return @($found)[0] }
    }

    # Fallback: well-known install locations.
    foreach ($base in @(${env:ProgramFiles}, ${env:ProgramFiles(x86)})) {
        if (-not $base) { continue }
        foreach ($edition in 'Enterprise', 'Professional', 'Community', 'BuildTools') {
            $candidate = Join-Path $base "Microsoft Visual Studio\2022\$edition\MSBuild\Current\Bin\MSBuild.exe"
            if (Test-Path $candidate) { return $candidate }
        }
    }

    $onPath = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    throw 'MSBuild.exe not found. Install Visual Studio 2022 (Desktop development with C++) or the Build Tools.'
}

if (-not (Test-Path $solution)) {
    throw "Solution not found: $solution"
}

if ($Configuration -ne 'Release' -or $Platform -ne 'x64') {
    Write-Warning "$Configuration|$Platform is not a supported configuration. Only Release|x64 links: RetSpoof.asm's ml64 step is conditioned on Release|x64, so other configs are missing _spoofer_stub."
}

$msbuild = Find-MSBuild
Write-Host "MSBuild:       $msbuild"
Write-Host "Solution:      $solution"
Write-Host "Configuration: $Configuration|$Platform"
Write-Host ''

$targets = if ($Clean) { 'Clean' } elseif ($Rebuild) { 'Rebuild' } else { 'Build' }

$msbuildArgs = @(
    $solution
    "/t:$targets"
    "/p:Configuration=$Configuration"
    "/p:Platform=$Platform"
    "/v:$Verbosity"
    '/nologo'
    '/m'
)

& $msbuild @msbuildArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host ''
    Write-Host "BUILD FAILED (msbuild exit code $LASTEXITCODE)" -ForegroundColor Red
    exit $LASTEXITCODE
}

if ($Clean) {
    Write-Host ''
    Write-Host 'Clean succeeded.' -ForegroundColor Green
    exit 0
}

# Both projects write to the solution-level output directory.
$outDir = Join-Path $root "$Platform\$Configuration"
$artifacts = @('2025Patch.dll', 'Injector.exe', 'PreserveLoader.dll')

# Files that ship beside the binaries. The ini is byte-identical to what Config::WriteDefaultFile
# writes on first run, so it only saves the user a launch to discover the knobs; the two .bat
# launchers resolve everything relative to themselves and therefore only work from the game folder.
# Staged here rather than in the CI workflow so a local build and CI produce the same flat drop-in
# set from one source of truth -- the release zip is packed straight from this directory.
$dropIns = @('2025patch.ini', 'RecRoomScreen.bat', 'RecRoomVR.bat', 'Install-PreserveLoader.bat', 'RecRoomPreserve.bat', 'Restore-RefereeProtector.bat')

Write-Host ''
Write-Host "BUILD SUCCEEDED -> $outDir" -ForegroundColor Green

$missing = @()
foreach ($name in $artifacts) {
    $path = Join-Path $outDir $name
    if (Test-Path $path) {
        $item = Get-Item $path
        Write-Host ("  {0,-16} {1,10:N0} bytes  {2}" -f $item.Name, $item.Length, $item.LastWriteTime)
    }
    else {
        $missing += $name
    }
}

foreach ($name in $dropIns) {
    $src = Join-Path $root $name
    if (-not (Test-Path $src)) {
        Write-Warning "$name not found at the repo root; not staged into $outDir."
        continue
    }
    Copy-Item -Path $src -Destination $outDir -Force
    $item = Get-Item (Join-Path $outDir $name)
    Write-Host ("  {0,-16} {1,10:N0} bytes  {2}  (staged)" -f $item.Name, $item.Length, $item.LastWriteTime)
}

if ($missing.Count -gt 0) {
    Write-Host ''
    Write-Host ("Expected artifact(s) missing: {0}" -f ($missing -join ', ')) -ForegroundColor Red
    exit 1
}

Write-Host ''
Write-Host "Everything in $outDir is the drop-in set: copy it into the game folder."
Write-Host 'The injector defaults to 2025Patch.dll next to itself; 2025patch.ini and the game read from the game folder.'
