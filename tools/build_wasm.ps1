# Builds gentlestop.wasm with the MSFS SDK's standalone clang + wasm-ld, linking the C library statically
# (the previous build left printf/fopen/exp... as unresolved imports, which the sim cannot satisfy).
# NOT tested on a real SDK install: read the output, and finish with check_wasm_imports.py.
param(
    [string]$Sdk = $(if ($env:MSFS2024_SDK) { $env:MSFS2024_SDK } elseif ($env:MSFS_SDK) { $env:MSFS_SDK } else { 'C:\MSFS 2024 SDK' }),
    [string]$OutDir
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $root 'wasm-source'
if ($OutDir) {
    $out = Join-Path $OutDir 'modules'
} else {
    $out  = Join-Path $root 'alexisware94-gentlestop\modules'
}
$obj  = Join-Path $env:TEMP 'gentlestop.o'
$wasm = Join-Path $out 'gentlestop.wasm'

$clang  = Join-Path $Sdk 'WASM\llvm\bin\clang-cl.exe'
$wasmLd = Join-Path $Sdk 'WASM\llvm\bin\wasm-ld.exe'
$sysroot = Join-Path $Sdk 'WASM\wasi-sysroot'
foreach ($t in @($clang, $wasmLd)) { if (-not (Test-Path $t)) { throw "Not found: $t (pass -Sdk 'path\to\MSFS 2024 SDK')" } }

$libc = Get-ChildItem -Path (Join-Path $Sdk 'WASM') -Recurse -Filter 'libc.a' -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match 'wasm32' } | Select-Object -First 1
$rt   = Get-ChildItem -Path (Join-Path $Sdk 'WASM') -Recurse -Filter 'libclang_rt.builtins-wasm32.a' -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $libc) { throw "libc.a for wasm32 not found under $Sdk\WASM - this is the C library that must be linked in." }
Write-Host "libc: $($libc.FullName)"; if ($rt) { Write-Host "builtins: $($rt.FullName)" } else { Write-Host "builtins: (not found, continuing)" }

$inc = @("-I`"$src`"", "-I`"$Sdk\WASM\include`"", "-I`"$Sdk\SimConnect SDK\include`"", "-I`"$sysroot\include`"")
# NOTE: deliberately NO libc++ include path (include\c++\v1): it breaks against this wasi sysroot. The sources use only C headers.

# /GS- : no stack protector (it would add an unresolved __stack_chk_fail import)
# Production build with Learn-Only feature enabled. Config path is explicitly set to production path.
$cc = @('-target','wasm32-wasi','/c','/O2','/GS-','-fms-extensions','-D__wasi__','-D_WINDLL',
        '-DGS_DECLARE_L_API','-DGS_USE_LEARN','-DGS_CFG_PATH=\\\"\\\\work\\\\gentlestop.cfg\\\"','-Wno-everything','-fno-producers',
        "-fdebug-prefix-map=$src=.", "-fdebug-prefix-map=$Sdk=.", '-frandom-seed=gentlestop') + $inc +
      @("""$src\GentleStop.cpp""", "-o", """$obj""")
& $clang @cc; if ($LASTEXITCODE -ne 0) { throw "compile failed" }

New-Item -ItemType Directory -Force -Path $out | Out-Null
$ld = @($obj, '-o', $wasm, '--no-entry', '--export=module_init', '--export=module_deinit', '--export=malloc', '--export=free',
        '--export=__wasm_call_ctors', '--allow-undefined', '--export-dynamic', '--export-table', '--no-eac-patch', '--strip-debug', $libc.FullName)
if ($rt) { $ld += $rt.FullName }
& $wasmLd @ld; if ($LASTEXITCODE -ne 0) { throw "link failed" }

Write-Host "Built $wasm ($((Get-Item $wasm).Length) bytes)"
python (Join-Path $PSScriptRoot 'check_wasm_imports.py') $wasm
if (-not $OutDir) {
    python (Join-Path $PSScriptRoot 'update_layout.py') (Join-Path $root 'alexisware94-gentlestop')
} else {
    python (Join-Path $PSScriptRoot 'update_layout.py') (Join-Path $OutDir 'alexisware94-gentlestop')
}