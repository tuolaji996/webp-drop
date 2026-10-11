[CmdletBinding()]
param([switch]$DebugBuild, [switch]$GuiTest)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$deps = Join-Path $root '.build-deps'
$build = Join-Path $root 'build'
$zigVersion = '0.15.2'
$zigHash = '3a0ed1e8799a2f8ce2a6e6290a9ff22e6906f8227865911fb7ddedc3cc14cb0c'
$webpVersion = '1.6.0'
$webpHash = 'e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564'
New-Item -ItemType Directory -Force -Path $deps, $build | Out-Null

function Get-Dependency([string]$Url, [string]$File, [string]$ExpectedHash) {
    if (-not (Test-Path -LiteralPath $File)) {
        Write-Host "Downloading $([IO.Path]::GetFileName($File))..."
        $partial = "$File.partial"
        Invoke-WebRequest -Uri $Url -OutFile $partial
        Move-Item -LiteralPath $partial -Destination $File -Force
    }
    if ($ExpectedHash -and (Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash -ne $ExpectedHash) {
        throw "SHA256 mismatch: $File"
    }
}

$zigRoot = Join-Path $deps "zig-x86_64-windows-$zigVersion"
$zig = Join-Path $zigRoot 'zig.exe'
if (-not (Test-Path -LiteralPath $zig)) {
    $zip = Join-Path $deps "zig-$zigVersion.zip"
    Get-Dependency "https://ziglang.org/download/$zigVersion/zig-x86_64-windows-$zigVersion.zip" $zip $zigHash
    & tar -xf $zip -C $deps
    if ($LASTEXITCODE -ne 0) { throw 'Cannot extract Zig.' }
}

$webpRoot = Join-Path $deps "libwebp-$webpVersion"
if (-not (Test-Path -LiteralPath (Join-Path $webpRoot 'src\webp\decode.h'))) {
    $archive = Join-Path $deps "libwebp-$webpVersion.tar.gz"
    Get-Dependency "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$webpVersion.tar.gz" $archive $webpHash
    & tar -xf $archive -C $deps
    if ($LASTEXITCODE -ne 0) { throw 'Cannot extract libwebp.' }
}

if (-not (Test-Path -LiteralPath (Join-Path $root 'src\main.c'))) {
    Write-Host 'Dependencies ready. The C sources will be compiled when present.'
    exit 0
}

$sources = @()
foreach ($directory in @('dec', 'dsp', 'utils', 'demux')) {
    $sources += Get-ChildItem -LiteralPath (Join-Path $webpRoot "src\$directory") -Filter '*.c' |
        Where-Object {
            if ($directory -eq 'dsp') {
                $_.BaseName -match '^(alpha_processing|cpu|dec|filters|lossless|rescaler|upsampling|yuv)(_|$)' -and
                    $_.BaseName -notmatch '_enc'
            } elseif ($directory -eq 'utils') {
                $_.Name -notin @('bit_writer_utils.c', 'huffman_encode_utils.c', 'quant_levels_utils.c')
            } else { $true }
        } |
        Select-Object -ExpandProperty FullName
}
$optimization = if ($DebugBuild) { '-O0' } else { '-O2' }
$symbolsFlag = if ($DebugBuild) { '-g' } else { '-s' }
$resource = Join-Path $build 'app.res'
$mainSource = if ($GuiTest) { 'tests\gui_smoke.c' } else { 'src\main.c' }
$executable = if ($GuiTest) { Join-Path $build 'GuiSmoke.exe' } else { Join-Path $root 'WebpDrop.exe' }
Push-Location $root
try {
    & $zig rc '/fo' $resource 'src\app.rc'
    if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed.' }
    $arguments = @(
        'cc', '-target', 'x86_64-windows-gnu', '-std=c11', $optimization, $symbolsFlag,
        '-DUNICODE', '-D_UNICODE', '-D_WIN32_WINNT=0x0a00',
        '-DWIN32_LEAN_AND_MEAN', '-DWEBP_USE_THREAD', '-I', $webpRoot,
        '-Wall', '-Wextra', '-Wno-unused-parameter',
        $mainSource, 'src\convert.c'
    ) + $sources + @(
        $resource, '-Wl,--subsystem,windows', '-municode', '-static',
        '-lcomctl32', '-lcomdlg32', '-lole32', '-loleaut32', '-luuid', '-lwindowscodecs',
        '-lshell32', '-lshlwapi', '-lgdi32', '-luser32',
        '-o', $executable
    )
    & $zig @arguments
    if ($LASTEXITCODE -ne 0) { throw 'C compilation failed.' }
    $symbols = [IO.Path]::ChangeExtension($executable, '.pdb')
    if (-not $GuiTest -and (Test-Path -LiteralPath $symbols)) {
        Move-Item -LiteralPath $symbols -Destination (Join-Path $build 'WebpDrop.pdb') -Force
    }
    Copy-Item -LiteralPath (Join-Path $webpRoot 'COPYING') -Destination (Join-Path $root 'LIBWEBP-LICENSE.txt') -Force
    Write-Host "Built: $executable"
}
finally {
    Pop-Location
}
