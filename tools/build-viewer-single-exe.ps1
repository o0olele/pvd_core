#requires -Version 5.1
<#
.SYNOPSIS
Build the full Windows viewer as a Release EXE with static PVD and MSVC libraries.
.EXAMPLE
.\tools\build-viewer-single-exe.ps1
.EXAMPLE
.\tools\build-viewer-single-exe.ps1 -Parallel 8
#>
[CmdletBinding()]
param(
    [string] $BuildDirectory,
    [ValidateRange(1, 256)]
    [int] $Parallel = 4
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($env:OS -ne 'Windows_NT') {
    throw 'The viewer requires Windows, Visual Studio 2022 C++ tools and a Windows SDK.'
}

$sourceDirectory = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $sourceDirectory 'build-single-exe'
}
$BuildDirectory = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDirectory)
$cmake = (Get-Command cmake -CommandType Application -ErrorAction Stop |
    Select-Object -First 1).Source

$configureArguments = @(
    '-S', $sourceDirectory,
    '-B', $BuildDirectory,
    '-G', 'Visual Studio 17 2022',
    '-A', 'x64',
    '-DBUILD_SHARED_LIBS=OFF',
    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
    '-DPVD_BUILD_VIEWER=ON',
    '-DPVD_BUILD_RENDER=ON',
    '-DPVD_BUILD_TRANSPORT=ON',
    '-DPVD_BUILD_ANALYSIS=ON',
    '-DPVD_BUILD_REPX=ON',
    '-DPVD_BUILD_PHYSX34_VALIDATION=OFF',
    '-DPVD_BUILD_TOOLS=OFF',
    '-DBUILD_TESTING=OFF'
)
& $cmake @configureArguments
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed (exit code $LASTEXITCODE)."
}

& $cmake --build $BuildDirectory --config Release --target pvd_viewer --parallel $Parallel
if ($LASTEXITCODE -ne 0) {
    throw "Viewer build failed (exit code $LASTEXITCODE)."
}

$executable = Join-Path $BuildDirectory 'viewer/Release/pvd_viewer.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "The build did not produce the expected EXE: $executable"
}

Write-Host "`nSingle-file viewer: $executable"
Write-Host 'PVD and MSVC libraries are linked statically; Windows system DLLs are still required.'
