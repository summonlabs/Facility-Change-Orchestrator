<#
.SYNOPSIS
    Builds, installs, and validates the Facility Change Orchestrator package.

.DESCRIPTION
    Performs the complete downstream-consumption proof:

      1. configure and build the library, the CLI and the test suite;
      2. install into a private prefix through the CMake install rules;
      3. verify the installed file set, the exported targets file and the package
         config are present and self-consistent;
      4. build an INDEPENDENT out-of-tree consumer that knows nothing about this
         source tree and reaches the library only through
         find_package(fco CONFIG REQUIRED) and the imported target fco::fco;
      5. run that consumer, which exercises the installed artifact;
      6. run the installed command line tool from the installed prefix.

    No step applies a timeout. A failure stops the script and returns non-zero.

.PARAMETER Configuration
    CMake configuration to build and install. Defaults to Release.

.PARAMETER Prefix
    Install prefix. Defaults to <repository>/build-pkg/<configuration>/prefix.

.PARAMETER KeepBuildTrees
    Keeps the intermediate build trees. They are removed by default so that a
    validation run never leaves generated state behind.
#>
[CmdletBinding()]
param(
    [string] $Configuration = 'Release',
    [string] $Prefix,
    [switch] $KeepBuildTrees
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Prefix) {
    $Prefix = Join-Path $repoRoot ("build-pkg/" + $Configuration + "/prefix")
}
$buildDir = Join-Path $repoRoot ("build-pkg/" + $Configuration + "/build")
$consumerBuild = Join-Path $repoRoot ("build-pkg/" + $Configuration + "/consumer")
$consumerSource = Join-Path $repoRoot 'tests/consumer'

function Write-Step([string] $text) {
    Write-Host ''
    Write-Host ("=== " + $text)
}

function Invoke-Checked([string] $description, [scriptblock] $action) {
    & $action
    if ($LASTEXITCODE -ne 0) {
        throw ($description + " failed with exit code " + $LASTEXITCODE)
    }
}

# Builds a -DNAME=VALUE argument explicitly instead of relying on variable
# expansion inside a cmdlet argument list.
function New-Define([string] $name, [string] $value) {
    return ('-D' + $name + '=' + $value)
}

if (Test-Path $buildDir) { Remove-Item -Recurse -Force $buildDir }
if (Test-Path $Prefix) { Remove-Item -Recurse -Force $Prefix }
if (Test-Path $consumerBuild) { Remove-Item -Recurse -Force $consumerBuild }

Write-Step "configure ($Configuration)"
Invoke-Checked 'configure' {
    cmake -S $repoRoot -B $buildDir -G Ninja (New-Define 'CMAKE_BUILD_TYPE' $Configuration) (New-Define 'FCO_BUILD_TESTS' 'ON')
}

Write-Step 'build'
Invoke-Checked 'build' { cmake --build $buildDir --parallel }

Write-Step 'install'
Invoke-Checked 'install' {
    cmake --install $buildDir --prefix $Prefix --config $Configuration
}

Write-Step 'verify installed file set'
$required = @(
    'include/fco/version.hpp',
    'include/fco/error.hpp',
    'include/fco/digest.hpp',
    'include/fco/strong.hpp',
    'include/fco/domain.hpp',
    'include/fco/authority.hpp',
    'include/fco/facility.hpp',
    'include/fco/model.hpp',
    'include/fco/planner.hpp',
    'include/fco/evidence.hpp',
    'include/fco/codec.hpp',
    'include/fco/state.hpp',
    'include/fco/store.hpp',
    'include/fco/ports.hpp',
    'include/fco/engine.hpp',
    'include/fco/render.hpp',
    'include/fco/sim.hpp',
    'include/fco/json.hpp',
    'include/fco/cli.hpp',
    'lib/cmake/fco/fcoConfig.cmake',
    'lib/cmake/fco/fcoConfigVersion.cmake',
    'lib/cmake/fco/fcoTargets.cmake'
)
$missing = @()
foreach ($relative in $required) {
    $candidate = Join-Path $Prefix $relative
    if (-not (Test-Path $candidate)) { $missing += $relative }
}
if ($missing.Count -gt 0) {
    throw ("installed package is missing: " + ($missing -join ', '))
}
Write-Host ("all " + $required.Count + " required installed files are present")

$targetsFile = Join-Path $Prefix 'lib/cmake/fco/fcoTargets.cmake'
$targetsText = Get-Content -Raw -LiteralPath $targetsFile
if ($targetsText -notmatch 'fco::fco') {
    throw 'the exported targets file does not declare the namespaced imported target fco::fco'
}
Write-Host 'the exported targets file declares the namespaced imported target fco::fco'

Write-Step 'build the out-of-tree consumer against the installed prefix'
New-Item -ItemType Directory -Force -Path $consumerBuild | Out-Null
Invoke-Checked 'consumer configure' {
    cmake -S $consumerSource -B $consumerBuild -G Ninja (New-Define 'CMAKE_BUILD_TYPE' $Configuration) (New-Define 'CMAKE_PREFIX_PATH' $Prefix)
}
Invoke-Checked 'consumer build' { cmake --build $consumerBuild --parallel }

Write-Step 'run the consumer'
$consumerExe = Join-Path $consumerBuild 'fco_consumer.exe'
if (-not (Test-Path $consumerExe)) { $consumerExe = Join-Path $consumerBuild 'fco_consumer' }
Invoke-Checked 'consumer run' { & $consumerExe }

Write-Step 'run the installed command line tool'
$cliExe = Join-Path $Prefix 'bin/fco.exe'
if (-not (Test-Path $cliExe)) { $cliExe = Join-Path $Prefix 'bin/fco' }
Invoke-Checked 'installed cli' { & $cliExe version }

if (-not $KeepBuildTrees) {
    Write-Step 'clean up validation build trees'
    foreach ($tree in @($buildDir, $consumerBuild)) {
        if (Test-Path $tree) { Remove-Item -Recurse -Force $tree }
    }
    Write-Host 'removed intermediate build trees; the installed prefix is preserved'
}

Write-Host ''
Write-Host 'package validation: PASS'
exit 0
