param(
    [switch]$Selftest      # run the built-in DPI/capture self test afterwards
)
$ErrorActionPreference = 'Stop'

# One translation unit, MinGW-w64, C++17. Direct2D/DirectWrite paint the editor; GDI is used for
# grabbing pixels and for the clipboard, GDI+ for the PNG file.
$root = $PSScriptRoot
$out  = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null

# The linker cannot overwrite a running executable, so clear leftover instances first.
Get-Process -Name ShotMvp* -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id $_.Id -Force }

$common = @('-std=c++17', '-O2', '-municode', '-DUNICODE', '-D_UNICODE')
$libs   = @('gdi32', 'user32', 'shell32', 'ole32', 'comdlg32', 'comctl32', 'd2d1', 'dwrite', 'dwmapi', 'gdiplus') | ForEach-Object { "-l$_" }
$src    = Join-Path $root 'src\main.cpp'
$gui    = Join-Path $out 'ShotMvp.exe'
$console = Join-Path $out 'ShotMvp_console.exe'

# The per-monitor DPI manifest is compiled into the exe as a real resource (RT_MANIFEST, id 1).
# It used to be patched into the finished binary by a Python script that also read its XML from
# outside the repository; building it is what makes DPI awareness part of the build (#18).
# Without the manifest Windows virtualises coordinates at non-100% scaling: the overlay stops
# covering the physical screen and the editor sizes its layout from the wrong number.
$res = Join-Path $out 'app_res.o'
windres '-J' 'rc' '-i' (Join-Path $root 'src\app.rc') '-O' 'coff' '-o' $res '-I' (Join-Path $root 'src')
if ($LASTEXITCODE) { throw "windres failed for $res" }

# ld prints ".rsrc merge failure: multiple non-default manifests" on every link here: MinGW's own
# default manifest (asInvoker + longPathAware) and src\app.rc's manifest (PerMonitorV2 DPI +
# Common-Controls 6) both arrive as RT_MANIFEST id 1. The one that survives is ours - proven by
# tests/dpi_verify.py (the overlay covers the physical screen only when DPI virtualisation is off)
# and by the localised TaskDialog card #22 added. -DNO_DEFAULT_MANIFEST does not silence the
# warning on this toolchain, so the warning is cosmetic and the build stays as it is.
g++ @common '-mwindows' $src $res -o $gui @libs
if ($LASTEXITCODE) { throw "build of $gui failed" }

# Console variant: the same program, so -selftest output can be read from a terminal.
g++ @common $src $res -o $console @libs
if ($LASTEXITCODE) { throw "build of $console failed" }

Write-Host "built $gui"
if ($Selftest) { & $gui -selftest }
