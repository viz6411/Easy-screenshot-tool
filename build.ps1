param(
    [switch]$NoManifest,   # skip the DPI manifest step (not recommended)
    [switch]$Selftest      # run the built-in DPI/capture self test afterwards
)
$ErrorActionPreference = 'Stop'

# One translation unit, MinGW-w64, C++17. Direct2D/DirectWrite paint the editor; GDI is used for
# grabbing pixels and for the clipboard/PNG path.
$root = $PSScriptRoot
$out  = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null

# The linker cannot overwrite a running executable, so clear leftover instances first.
Get-Process -Name ShotMvp* -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id $_.Id -Force }

$common = @('-std=c++17', '-O2', '-municode', '-DUNICODE', '-D_UNICODE')
$libs   = @('gdi32', 'user32', 'shell32', 'ole32', 'd2d1', 'dwrite', 'dwmapi') | ForEach-Object { "-l$_" }
$src    = Join-Path $root 'src\main.cpp'
$gui    = Join-Path $out 'ShotMvp.exe'
$console = Join-Path $out 'ShotMvp_console.exe'

g++ @common '-mwindows' $src -o $gui @libs
if ($LASTEXITCODE) { throw "build of $gui failed" }

# Console variant: the same program, so -selftest output can be read from a terminal.
g++ @common $src -o $console @libs
if ($LASTEXITCODE) { throw "build of $console failed" }

# The per-monitor DPI manifest must live inside the .rsrc of every build that shows a window.
# Without it Windows virtualises coordinates at non-100% scaling: the overlay stops covering the
# physical screen and the editor sizes its layout from the wrong number.
if (-not $NoManifest) {
    python (Join-Path $root 'tools\replace_manifest.py') $gui $console
    if ($LASTEXITCODE) { throw "manifest patch failed" }
}

Write-Host "built $gui"
if ($Selftest) { & $gui -selftest }
