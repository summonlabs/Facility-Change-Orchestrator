<#
.SYNOPSIS
    Proves closure from a fresh clone.

.DESCRIPTION
    Clones the repository at HEAD into a directory that has never been built in,
    then configures, builds, runs the complete test suite and performs the
    package validation there. Nothing is copied from the working tree except the
    committed objects, so any file that is required but untracked will be
    reported as a failure.

    The clone is removed afterwards unless -KeepClone is supplied.

.PARAMETER Configuration
    CMake configuration to build. Defaults to Release.

.PARAMETER KeepClone
    Keeps the temporary clone for inspection.
#>
[CmdletBinding()]
param(
    [string] $Configuration = 'Release',
    [switch] $KeepClone
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
$cloneRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("fco-fresh-clone-" + [Guid]::NewGuid().ToString('N'))

Write-Host ("cloning " + $repoRoot + " into " + $cloneRoot)
git clone --quiet --no-hardlinks $repoRoot $cloneRoot
if ($LASTEXITCODE -ne 0) { throw 'git clone failed' }

$head = (git -C $cloneRoot rev-parse HEAD).Trim()
Write-Host ("cloned commit " + $head)

try {
    Write-Host 'configure'
    cmake -S $cloneRoot -B (Join-Path $cloneRoot 'build-rel') -G Ninja ('-DCMAKE_BUILD_TYPE=' + $Configuration)
    if ($LASTEXITCODE -ne 0) { throw 'configure failed' }

    Write-Host 'build'
    cmake --build (Join-Path $cloneRoot 'build-rel') --parallel
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }

    Write-Host 'test'
    ctest --test-dir (Join-Path $cloneRoot 'build-rel') --output-on-failure -C $Configuration
    if ($LASTEXITCODE -ne 0) { throw 'tests failed in the fresh clone' }

    Write-Host 'package validation inside the clone'
    & (Join-Path $cloneRoot 'scripts/validate_package.ps1') -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) { throw 'package validation failed in the fresh clone' }
}
finally {
    if (-not $KeepClone) {
        if (Test-Path $cloneRoot) { Remove-Item -Recurse -Force $cloneRoot }
        Write-Host 'removed the temporary clone'
    } else {
        Write-Host ("kept the clone at " + $cloneRoot)
    }
}

Write-Host ''
Write-Host 'fresh-clone closure: PASS'
exit 0
