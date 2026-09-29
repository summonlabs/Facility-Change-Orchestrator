<#
.SYNOPSIS
    Runs the complete validation matrix for the Facility Change Orchestrator.

.DESCRIPTION
    Release and Debug builds with warnings as errors, the full test suite in both
    configurations, the package/installed-artifact validation, and the
    fresh-clone closure check. No step applies a timeout: a hanging test is a
    defect to diagnose, not a condition to tolerate.

.PARAMETER SkipFreshClone
    Skips the fresh-clone stage (useful while iterating).

.PARAMETER SkipDebug
    Skips the Debug configuration.
#>
[CmdletBinding()]
param(
    [switch] $SkipFreshClone,
    [switch] $SkipDebug
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot

function Invoke-Stage([string] $name, [scriptblock] $action) {
    Write-Host ''
    Write-Host ('### ' + $name)
    & $action
    if ($LASTEXITCODE -ne 0) { throw ($name + ' failed with exit code ' + $LASTEXITCODE) }
}

function Invoke-Configuration([string] $configuration) {
    $buildDir = Join-Path $repoRoot ('build-' + $configuration.ToLowerInvariant())
    if (Test-Path $buildDir) { Remove-Item -Recurse -Force $buildDir }

    Invoke-Stage ($configuration + ' configure') {
        cmake -S $repoRoot -B $buildDir -G Ninja ('-DCMAKE_BUILD_TYPE=' + $configuration) ('-DFCO_BUILD_TESTS=ON')
    }
    Invoke-Stage ($configuration + ' build') { cmake --build $buildDir --parallel }
    Invoke-Stage ($configuration + ' tests') {
        ctest --test-dir $buildDir --output-on-failure -C $configuration
    }

    if (Test-Path $buildDir) { Remove-Item -Recurse -Force $buildDir }
}

Invoke-Configuration 'Release'
if (-not $SkipDebug) { Invoke-Configuration 'Debug' }

Invoke-Stage 'package validation' {
    & (Join-Path $PSScriptRoot 'validate_package.ps1') -Configuration 'Release'
}

if (-not $SkipFreshClone) {
    Invoke-Stage 'fresh clone closure' {
        & (Join-Path $PSScriptRoot 'fresh_clone_check.ps1') -Configuration 'Release'
    }
}

Write-Host ''
Write-Host 'full validation matrix: PASS'
exit 0
