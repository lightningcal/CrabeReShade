# Builds the companion ReShade32.dll: ReShade v6.8.0 plus manual-addons.patch and crabe_manual_depth.hpp.
# Needs git and Visual Studio 2022 (or Build Tools) with the C++ workload. Deploys nothing.
[CmdletBinding()]
param([string]$SourceDir, [string]$MSBuild)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $SourceDir) { $SourceDir = Join-Path $repoRoot 'build\reshade-src' }
$SourceDir = [IO.Path]::GetFullPath($SourceDir)
$revision = '18deaa52de0c425a78b329e9cb3c497281cd00ec'
$patch = Join-Path $PSScriptRoot 'manual-addons.patch'

if (-not (Test-Path -LiteralPath $SourceDir)) {
    & git clone --branch v6.8.0 --depth 1 --recurse-submodules --shallow-submodules https://github.com/crosire/reshade.git $SourceDir
    if ($LASTEXITCODE -ne 0) { throw 'Cannot fetch the pinned ReShade source.' }
}
$actual = & git -C $SourceDir rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $actual -ne $revision) { throw "Expected ReShade revision $revision; refusing to patch another one." }

$ErrorActionPreference = 'Continue'
& git -C $SourceDir apply --reverse --check $patch 2>$null
$alreadyApplied = $LASTEXITCODE -eq 0
$ErrorActionPreference = 'Stop'
if (-not $alreadyApplied) {
    if (& git -C $SourceDir status --porcelain --untracked-files=no) { throw 'The ReShade source has other edits; use a clean checkout.' }
    & git -C $SourceDir apply $patch
    if ($LASTEXITCODE -ne 0) { throw 'manual-addons.patch does not apply.' }
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'crabe_manual_depth.hpp') -Destination (Join-Path $SourceDir 'source\crabe_manual_depth.hpp') -Force

if (-not $MSBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $installation = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath
    if (-not $installation) { throw 'Visual Studio 2022 or Build Tools with MSBuild is required.' }
    $MSBuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
}
& $MSBuild (Join-Path $SourceDir 'ReShade.vcxproj') /m /p:Configuration=Release /p:Platform=Win32 "/p:SolutionDir=$SourceDir\" /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw "The ReShade build failed ($LASTEXITCODE)." }

$output = Join-Path $repoRoot 'build\companion'
New-Item -ItemType Directory -Path $output -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $SourceDir 'bin\Win32\Release\ReShade32.dll') -Destination (Join-Path $output 'ReShade32.dll') -Force
Write-Host "Built $output\ReShade32.dll"
