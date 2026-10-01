$ErrorActionPreference = "Stop"

$Distro = "Ubuntu-26.04"
$Project = $PSScriptRoot
$Sdk = "/opt/ps5-payload-sdk"
$OutputBase = Join-Path $Project "artifacts"
$WindowsTar = Join-Path $env:SystemRoot "System32\tar.exe"

if (-not (Test-Path $WindowsTar)) {
    throw "Windows tar.exe was not found: $WindowsTar"
}

function Convert-ToWslPath {
    param([Parameter(Mandatory)][string]$Path)

    $FullPath = [IO.Path]::GetFullPath($Path)
    if ($FullPath -notmatch '^([A-Za-z]):\\?(.*)$') {
        throw "Cannot convert to WSL path: $FullPath"
    }

    return "/mnt/$($Matches[1].ToLowerInvariant())/$($Matches[2] -replace '\\', '/')"
}

$VersionLine = Get-Content (Join-Path $Project "source\main.cpp") |
    Where-Object { $_ -match 'kVersion\s*=\s*"([^"]+)"' } |
    Select-Object -First 1
if (-not $VersionLine -or $VersionLine -notmatch 'kVersion\s*=\s*"([^"]+)"') {
    throw "Could not find kVersion in source/main.cpp"
}
$Version = $Matches[1]

$Branch = (git -C $Project branch --show-current).Trim()
if (-not $Branch) {
    $Branch = "detached-$(git -C $Project rev-parse --short HEAD)"
}
$SafeBranch = $Branch -replace '[\\/:*?"<>|\s]', '_'
$OutputDir = Join-Path $OutputBase $SafeBranch
$BuildTempDir = Join-Path $Project ".build-tmp"
$SourceArchive = Join-Path $BuildTempDir "wsl-source.tar"
$ResultArchive = Join-Path $BuildTempDir "wsl-result.tar"
$TempScript = Join-Path $BuildTempDir "wsl-build.sh"

$BuildScript = @'
set -euo pipefail

source_archive="$1"
result_archive="$2"
sdk="$3"
version="$4"

work_dir="$(mktemp -d "$HOME/singledpi-build-XXXXXX")"
source_dir="$work_dir/source"
artifact_dir="$work_dir/artifacts"
cleanup() { rm -rf "$work_dir"; }
trap cleanup EXIT

mkdir -p "$source_dir" "$artifact_dir"
tar --warning=no-unknown-keyword -xf "$source_archive" -C "$source_dir"
cd "$source_dir"

for command in make gcc g++; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "ERROR: missing build tool: $command" >&2
        echo "Install with: sudo apt-get update && sudo apt-get install -y build-essential" >&2
        exit 20
    }
done

export PS5_PAYLOAD_SDK="$sdk"
export PATH="$sdk/bin:$PATH"

echo "SDK: $PS5_PAYLOAD_SDK"
echo "Building singleDPI with $(nproc) jobs..."
make clean
make -j"$(nproc)" PS5_PAYLOAD_SDK="$PS5_PAYLOAD_SDK"

test -f bin/singleDPI.elf || {
    echo "ERROR: bin/singleDPI.elf was not produced" >&2
    exit 21
}

output_name="singleDPI_v${version}.elf"
cp bin/singleDPI.elf "$artifact_dir/$output_name"
tar -cf "$work_dir/result.tar" -C "$artifact_dir" .
cp "$work_dir/result.tar" "$result_archive"
echo "Built: $output_name"
'@

try {
    New-Item -ItemType Directory -Force $BuildTempDir | Out-Null
    $SourceArchiveWsl = Convert-ToWslPath $SourceArchive
    $ResultArchiveWsl = Convert-ToWslPath $ResultArchive
    $TempScriptWsl = Convert-ToWslPath $TempScript

    $NormalizedScript = $BuildScript -replace "`r`n", "`n"
    [IO.File]::WriteAllText($TempScript, $NormalizedScript, [Text.UTF8Encoding]::new($false))

    Write-Host "Project : $Project"
    Write-Host "Distro  : $Distro"
    Write-Host "Version : $Version"

    & $WindowsTar --exclude="./artifacts" --exclude="./.build-tmp" --exclude="./.git" `
        --exclude="./.idea" --exclude="./bin/*.elf" -cf $SourceArchive -C $Project .
    if ($LASTEXITCODE -ne 0) { throw "Source archive failed, exit code: $LASTEXITCODE" }

    & wsl.exe -d $Distro -- bash $TempScriptWsl $SourceArchiveWsl $ResultArchiveWsl $Sdk $Version
    if ($LASTEXITCODE -ne 0) { throw "WSL build failed, exit code: $LASTEXITCODE" }

    $OutputDir = New-Item -ItemType Directory -Force $OutputDir
    & $WindowsTar -xf $ResultArchive -C $OutputDir.FullName
    if ($LASTEXITCODE -ne 0) { throw "Artifact extraction failed, exit code: $LASTEXITCODE" }

    Write-Host "Build completed: $($OutputDir.FullName)"
}
finally {
    Remove-Item $SourceArchive, $ResultArchive, $TempScript -Force -ErrorAction SilentlyContinue
}
