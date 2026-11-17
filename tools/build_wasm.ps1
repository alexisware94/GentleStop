# Builds the production gentlestop.wasm with the MSFS 2024 SDK's standalone clang + wasm-ld,
# linking the C library statically (an earlier build left printf/fopen/exp... as unresolved
# imports, which the sim cannot satisfy).
#
# Safety contract (H2.8.7.6.4):
#   * Source, tool and package paths are resolved from this script's own location, so the build
#     works from any current directory.
#   * The default output is a unique temporary directory outside the repository.
#   * -OutDir selects an explicit external output root; pointing it inside this repository
#     (including gentlestop-stage-panel\) is refused, so the approved module cannot be overwritten
#     by accident.
#   * The script only compiles, links and validates. It never regenerates layout.json or
#     manifest.json and never copies anything into the canonical package.
#   * Promoting a validated module into gentlestop-stage-panel\modules\ is a separate, reviewed
#     release step (see wasm-source\BUILD.md).
#
# The final module is written to <output root>\modules\gentlestop.wasm; the object file goes to
# <output root>\obj\. SHA-256, size and path are printed on success.
param(
    [string]$Sdk = $(if ($env:MSFS2024_SDK) { $env:MSFS2024_SDK } elseif ($env:MSFS_SDK) { $env:MSFS_SDK } else { 'C:\MSFS 2024 SDK' }),
    [string]$OutDir,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

try {
    # --- repository-relative paths -------------------------------------------------------
    $root = Split-Path -Parent $PSScriptRoot
    if (-not $root) { throw "Cannot derive the repository root from '$PSScriptRoot'." }
    $src = Join-Path $root 'wasm-source'
    $sourceFile = Join-Path $src 'GentleStop.cpp'
    if (-not (Test-Path -LiteralPath $sourceFile -PathType Leaf)) {
        throw "Source file not found: $sourceFile"
    }

    # --- output selection ----------------------------------------------------------------
    if ($OutDir) {
        $outRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir)
    } else {
        $tempBase = if ($env:TEMP) { $env:TEMP } else { [IO.Path]::GetTempPath() }
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        $outRoot = Join-Path $tempBase ("gentlestop-wasm-$stamp-$PID")
    }
    $outRoot = [IO.Path]::GetFullPath($outRoot)

    $repoFull = [IO.Path]::GetFullPath($root).TrimEnd('\', '/')
    $outFull = $outRoot.TrimEnd('\', '/')
    if ($outFull.Equals($repoFull, [StringComparison]::OrdinalIgnoreCase) -or
        $outFull.StartsWith($repoFull + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to write build output inside the repository: $outRoot. " +
              "Pass an external -OutDir (for example $env:TEMP\gentlestop-build); the canonical " +
              "package is only updated by an explicitly reviewed release step."
    }

    $out = Join-Path $outRoot 'modules'
    $objDir = Join-Path $outRoot 'obj'
    $obj = Join-Path $objDir 'gentlestop.o'
    $wasm = Join-Path $out 'gentlestop.wasm'

    if (Test-Path -LiteralPath $outRoot -PathType Leaf) {
        throw "Invalid output directory: $outRoot exists and is a file."
    }
    try {
        New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
    } catch {
        throw "Cannot create the output directory '$outRoot': $($_.Exception.Message)"
    }

    if (Test-Path -LiteralPath $wasm -PathType Leaf) {
        if (-not $Force) {
            throw "Output already exists: $wasm. Re-run with -Force to overwrite it, or choose a new -OutDir."
        }
        Remove-Item -LiteralPath $wasm -Force
    }

    # --- toolchain -------------------------------------------------------------------------
    if (-not (Test-Path -LiteralPath $Sdk -PathType Container)) {
        throw "MSFS 2024 SDK not found: $Sdk (pass -Sdk 'path\to\MSFS 2024 SDK', or set MSFS2024_SDK / MSFS_SDK)."
    }
    $clang = Join-Path $Sdk 'WASM\llvm\bin\clang-cl.exe'
    $wasmLd = Join-Path $Sdk 'WASM\llvm\bin\wasm-ld.exe'
    if (-not (Test-Path -LiteralPath $clang -PathType Leaf)) { throw "Compiler not found: $clang" }
    if (-not (Test-Path -LiteralPath $wasmLd -PathType Leaf)) { throw "Linker not found: $wasmLd" }

    $sysroot = Join-Path $Sdk 'WASM\wasi-sysroot'
    if (-not (Test-Path -LiteralPath (Join-Path $sysroot 'include') -PathType Container)) {
        throw "WASI sysroot not found: $sysroot (expected $sysroot\include)."
    }

    $libc = Get-ChildItem -Path (Join-Path $Sdk 'WASM') -Recurse -Filter 'libc.a' -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match 'wasm32' } | Select-Object -First 1
    if (-not $libc) {
        throw "libc.a for wasm32 not found under $Sdk\WASM - this is the C library that must be linked in."
    }
    $rt = Get-ChildItem -Path (Join-Path $Sdk 'WASM') -Recurse -Filter 'libclang_rt.builtins-wasm32.a' -ErrorAction SilentlyContinue | Select-Object -First 1

    $python = Get-Command python -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $python) {
        throw "Python 3 was not found on PATH; it is required for tools\check_wasm_imports.py."
    }

    Write-Host "SDK    : $Sdk"
    Write-Host "source : $sourceFile"
    Write-Host "output : $wasm"
    Write-Host "libc   : $($libc.FullName)"
    if ($rt) { Write-Host "builtins: $($rt.FullName)" } else { Write-Host "builtins: (not found, continuing)" }

    # --- compile ---------------------------------------------------------------------------
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null

    $inc = @("-I`"$src`"", "-I`"$Sdk\WASM\include`"", "-I`"$Sdk\SimConnect SDK\include`"", "-I`"$sysroot\include`"")
    # NOTE: deliberately NO libc++ include path (include\c++\v1): it breaks against this wasi sysroot. The sources use only C headers.

    # /GS- : no stack protector (it would add an unresolved __stack_chk_fail import)
    # Production build with Learn-Only feature enabled. Config path is explicitly set to production path.
    $cc = @('-target','wasm32-wasi','/c','/O2','/GS-','-fms-extensions','-D__wasi__','-D_WINDLL',
            '-DGS_DECLARE_L_API','-DGS_USE_LEARN','-DGS_CFG_PATH=\"\\work\gentlestop.cfg\"','-Wno-everything','-fno-producers',
            "-fdebug-prefix-map=$src=.", "-fdebug-prefix-map=$Sdk=.", '-frandom-seed=gentlestop') + $inc +
          @("""$sourceFile""", "-o", """$obj""")
    & $clang @cc
    if ($LASTEXITCODE -ne 0) { throw "compile failed (clang-cl exit code $LASTEXITCODE)" }
    if (-not (Test-Path -LiteralPath $obj -PathType Leaf)) { throw "Compilation reported success but the object file is missing: $obj" }

    # --- link ------------------------------------------------------------------------------
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    $ld = @($obj, '-o', $wasm, '--no-entry', '--export=module_init', '--export=module_deinit', '--export=malloc', '--export=free',
            '--export=__wasm_call_ctors', '--allow-undefined', '--export-dynamic', '--export-table', '--no-eac-patch', '--strip-debug', $libc.FullName)
    if ($rt) { $ld += $rt.FullName }
    & $wasmLd @ld
    if ($LASTEXITCODE -ne 0) { throw "link failed (wasm-ld exit code $LASTEXITCODE)" }
    if (-not (Test-Path -LiteralPath $wasm -PathType Leaf)) { throw "Linking reported success but the module is missing: $wasm" }

    # --- validation -------------------------------------------------------------------------
    & $python.Source (Join-Path $PSScriptRoot 'check_wasm_imports.py') $wasm
    if ($LASTEXITCODE -ne 0) { throw "import validation failed (check_wasm_imports.py exit code $LASTEXITCODE)" }
    if (-not (Test-Path -LiteralPath $wasm -PathType Leaf)) { throw "Module vanished during validation: $wasm" }

    $item = Get-Item -LiteralPath $wasm
    $hash = (Get-FileHash -LiteralPath $wasm -Algorithm SHA256).Hash

    Write-Host ''
    Write-Host 'Build complete. The canonical package was not modified.'
    Write-Host "  output : $($item.FullName)"
    Write-Host "  size   : $($item.Length) bytes"
    Write-Host "  sha256 : $hash"
    Write-Host '  next   : promote this file into gentlestop-stage-panel\modules\ only after an'
    Write-Host '           explicit release review (see wasm-source\BUILD.md).'
    exit 0
}
catch {
    Write-Host ''
    Write-Host "BUILD FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
