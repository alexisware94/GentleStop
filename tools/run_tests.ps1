#requires -Version 5.1
<#
    run_tests.ps1 - portable regression-test entry point for GentleStop.

    Builds and runs the retained host regression tests against the MSFS /
    SimConnect stand-in headers in wasm-source/tests/stubs, then runs the two
    Node.js tests. It never builds the production MSFS WASM module.

    Usage (from any directory):

        powershell -ExecutionPolicy Bypass -File tools\run_tests.ps1

    Common options:

        -BuildDirectory <path>      Use a specific temporary build directory
                                    (must be outside the repository).
        -CompilerPath <cl.exe>      Use a specific MSVC compiler executable.
        -TestTimeoutSeconds <n>     Per-process timeout (default 300).
        -KeepBuildArtifacts         Keep the generated build directory and logs.

    Prerequisites:

        * Windows PowerShell 5.1 (or PowerShell 7+).
        * cl.exe on PATH, or -CompilerPath. The easiest way is to start an
          "x64 Native Tools Command Prompt for VS" / "Developer PowerShell for
          Visual Studio" so the MSVC environment is configured.
        * Node.js on PATH for the JavaScript tests.

    Exit codes:

        0  every required test passed
        1  at least one test was built or executed and failed
        2  at least one required test could not run (missing prerequisite);
           the overall result is PARTIAL, never PASS

    All compiled binaries, object files and logs are written to a unique
    directory in the system temporary folder by default, never into the
    repository, wasm-source/tests, tools/ or the panel package.

    The legacy JavaScript parity harness (wasm-source/tests/run_parity.js) was
    retired because no legitimate, publishable reference braking engine exists
    for the public tree; see wasm-source/BUILD.md. It is reported as RETIRED,
    never as a passing test, and never counts toward the overall result.
#>
[CmdletBinding()]
param(
    [string]$BuildDirectory,
    [string]$CompilerPath,
    [int]$TestTimeoutSeconds = 300,
    [switch]$KeepBuildArtifacts
)

$ErrorActionPreference = 'Stop'

$script:RepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$script:WasmSource = Join-Path $script:RepoRoot 'wasm-source'
$script:TestsDir = Join-Path $script:WasmSource 'tests'
$script:StubsDir = Join-Path $script:TestsDir 'stubs'

# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

function Write-Section([string]$Text) {
    Write-Host ''
    Write-Host "== $Text =="
}

# Correct Windows command-line quoting (handles spaces, quotes, backslashes).
function ConvertTo-ArgumentString([string]$Value) {
    if ($null -eq $Value) { return '' }
    if ($Value -eq '') { return '""' }
    if ($Value -notmatch '[\s"]') { return $Value }
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append('"')
    $backslashes = 0
    foreach ($ch in $Value.ToCharArray()) {
        if ($ch -eq '\') { $backslashes++; continue }
        if ($ch -eq '"') {
            [void]$sb.Append(('\' * ($backslashes * 2 + 1)))
            [void]$sb.Append('"')
            $backslashes = 0
            continue
        }
        if ($backslashes -gt 0) {
            [void]$sb.Append(('\' * $backslashes))
            $backslashes = 0
        }
        [void]$sb.Append($ch)
    }
    if ($backslashes -gt 0) { [void]$sb.Append(('\' * ($backslashes * 2))) }
    [void]$sb.Append('"')
    return $sb.ToString()
}

# Runs an executable, captures both streams, enforces a timeout.
function Invoke-Process {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [int]$TimeoutSeconds = 300,
        [string]$StandardInput = $null
    )

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $FilePath
    $psi.Arguments = (@($Arguments | ForEach-Object { ConvertTo-ArgumentString ([string]$_) }) -join ' ')
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardInput = $PSBoundParameters.ContainsKey('StandardInput')
    $psi.CreateNoWindow = $true

    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    [void]$p.Start()
    if ($psi.RedirectStandardInput) {
        $p.StandardInput.Write($StandardInput)
        $p.StandardInput.Close()
    }
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()

    $timedOut = $false
    if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
        $timedOut = $true
        try { $p.Kill() } catch { }
        [void]$p.WaitForExit(10000)
    }

    $stdout = ''
    $stderr = ''
    try { $stdout = $outTask.Result } catch { }
    try { $stderr = $errTask.Result } catch { }

    $exitCode = $null
    try { $exitCode = $p.ExitCode } catch { }
    $p.Dispose()

    return [pscustomobject]@{
        ExitCode = $exitCode
        TimedOut = $timedOut
        StdOut   = $stdout
        StdErr   = $stderr
    }
}

function Show-ProcessOutput($Result, [int]$MaxLines = 15) {
    $text = (($Result.StdOut + "`n" + $Result.StdErr) -split "`r?`n" | Where-Object { $_ -ne '' })
    if (-not $text) { return }
    if ($text.Count -gt $MaxLines) {
        Write-Host ("    ... (" + ($text.Count - $MaxLines) + " earlier line(s) omitted)")
        $text = $text | Select-Object -Last $MaxLines
    }
    foreach ($line in $text) { Write-Host ('    ' + $line) }
}

function Assert-OutsideRepository([string]$Path) {
    $repo = [System.IO.Path]::GetFullPath($script:RepoRoot).TrimEnd('\')
    $candidate = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    $cmp = [System.StringComparison]::OrdinalIgnoreCase
    if ($candidate.Equals($repo, $cmp) -or $candidate.StartsWith($repo + '\', $cmp)) {
        throw "Build directory '$candidate' is inside the repository '$repo'. Test artifacts must stay outside the repository."
    }
}

# ---------------------------------------------------------------------------
# Preconditions and environment
# ---------------------------------------------------------------------------

if (-not (Test-Path -LiteralPath $script:TestsDir)) {
    Write-Host "ERROR: wasm-source/tests was not found under '$script:RepoRoot'. Run this script from a GentleStop repository." -ForegroundColor Red
    exit 2
}

if ($BuildDirectory) {
    $buildDir = [System.IO.Path]::GetFullPath($BuildDirectory)
} else {
    $stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
    $buildDir = Join-Path ([System.IO.Path]::GetTempPath()) ("gentlestop-tests-$stamp-$PID")
}
$buildDir = [System.IO.Path]::GetFullPath($buildDir)
try {
    Assert-OutsideRepository $buildDir
} catch {
    Write-Host ('ERROR: ' + $_.Exception.Message) -ForegroundColor Red
    exit 2
}
if (-not (Test-Path -LiteralPath $buildDir)) {
    [void](New-Item -ItemType Directory -Path $buildDir -Force)
}

Write-Section 'Environment'
Write-Host "Repository root : $script:RepoRoot"
Write-Host "Build directory : $buildDir"
$psVersion = $PSVersionTable.PSVersion.ToString()
Write-Host "PowerShell      : $psVersion"
if ($PSVersionTable.PSVersion.Major -lt 5 -or
    ($PSVersionTable.PSVersion.Major -eq 5 -and $PSVersionTable.PSVersion.Minor -lt 1)) {
    Write-Host 'ERROR: Windows PowerShell 5.1 or newer is required.' -ForegroundColor Red
    exit 2
}

# --- MSVC detection --------------------------------------------------------
$clExe = $null
if ($CompilerPath) {
    if (Test-Path -LiteralPath $CompilerPath) {
        $clExe = (Resolve-Path -LiteralPath $CompilerPath).Path
    } else {
        Write-Host "WARNING: -CompilerPath '$CompilerPath' does not exist." -ForegroundColor Yellow
    }
} else {
    $clCommand = Get-Command -Name 'cl.exe' -ErrorAction SilentlyContinue
    if ($clCommand) { $clExe = $clCommand.Source }
}

$clVersion = 'not available'
if ($clExe) {
    try {
        $versionInfo = (Get-Item -LiteralPath $clExe).VersionInfo
        if ($versionInfo.FileVersion) { $clVersion = $versionInfo.FileVersion }
    } catch { }
    Write-Host "MSVC compiler   : $clExe"
    Write-Host "Compiler version: $clVersion (file version of cl.exe)"
} else {
    Write-Host 'MSVC compiler   : cl.exe NOT FOUND' -ForegroundColor Yellow
    Write-Host '  The C++ regression tests will be reported NOT RUN.'
    Write-Host '  Configure MSVC first, then run this script again, for example:'
    Write-Host '    * open "x64 Native Tools Command Prompt for VS" (or "Developer PowerShell for Visual Studio")'
    Write-Host '    * or run vcvars64.bat in a console and start PowerShell from it'
    Write-Host '    * or pass -CompilerPath <full path to cl.exe>'
}

# --- Node.js detection -----------------------------------------------------
$nodeExe = $null
$nodeVersion = 'not available'
$nodeCommand = Get-Command -Name 'node.exe' -ErrorAction SilentlyContinue
if ($nodeCommand) {
    $nodeExe = $nodeCommand.Source
    $nodeResult = Invoke-Process -FilePath $nodeExe -Arguments @('--version') -WorkingDirectory $buildDir -TimeoutSeconds 60
    if ($nodeResult.ExitCode -eq 0) { $nodeVersion = $nodeResult.StdOut.Trim() }
    Write-Host "Node.js         : $nodeExe ($nodeVersion)"
} else {
    Write-Host 'Node.js         : node.exe NOT FOUND' -ForegroundColor Yellow
    Write-Host '  The JavaScript tests will be reported NOT RUN. Install Node.js and ensure it is on PATH.'
}

# ---------------------------------------------------------------------------
# Test matrix (verified against the retained sources and the historic build
# scripts; each row lists exactly what that executable needs)
# ---------------------------------------------------------------------------
$testMatrix = @(
    [pscustomobject]@{ Name = 'test_core';              Class = 'smoke';       Sources = @('test_core.cpp');                             Defines = @();                                                         CfgHeader = $false; Description = 'Core stepping harness (smoke only, non-asserting)'; StdIn = "0 0 0 0 60 0 50 0 0 0 0`n0 1 0 0 0 0 0 0.02`n" },
    [pscustomobject]@{ Name = 'test_core_assert';       Class = 'regression';  Sources = @('test_core_assert.cpp');                      Defines = @();                                                         CfgHeader = $false; Description = 'Deterministic core braking assertions' },
    [pscustomobject]@{ Name = 'test_brake_input';       Class = 'regression';  Sources = @('test_brake_input.cpp');                      Defines = @();                                                         CfgHeader = $false; Description = 'Braking input range and telemetry guards' },
    [pscustomobject]@{ Name = 'test_input_probe';       Class = 'regression';  Sources = @('test_input_probe.cpp');                      Defines = @();                                                         CfgHeader = $false; Description = 'Input-learn state machine against a mock API' },
    [pscustomobject]@{ Name = 'test_glue';              Class = 'integration'; Sources = @('test_glue.cpp', 'GentleStop.cpp');           Defines = @('GS_USE_LEARN', 'GS_TEST_CFG');                            CfgHeader = $true;  Description = 'Module glue against SimConnect/CommBus mocks' },
    [pscustomobject]@{ Name = 'test_input_learn_glue';  Class = 'integration'; Sources = @('test_input_learn_glue.cpp', 'GentleStop.cpp'); Defines = @('GS_USE_LEARN');                                          CfgHeader = $true;  Description = 'Production learned-input integration' },
    [pscustomobject]@{ Name = 'test_elevator_suppress'; Class = 'integration'; Sources = @('test_elevator_suppress.cpp', 'GentleStop.cpp'); Defines = @('GS_USE_LEARN', 'GS_USE_ELEV_SUPPRESS');                  CfgHeader = $true;  Description = 'Elevator suppression policy' },
    [pscustomobject]@{ Name = 'test_yaxis_probe';       Class = 'integration'; Sources = @('test_yaxis_probe.cpp', 'GentleStop.cpp');    Defines = @('GS_USE_LEARN', 'GS_USE_ELEV_SUPPRESS', 'GS_YAXIS_PROBE'); CfgHeader = $true;  Description = 'Y-axis diagnostic probe' }
)

$results = New-Object System.Collections.ArrayList

function Add-Result {
    param(
        [string]$Name,
        [string]$Kind,
        [string]$Class,
        [string]$Build,
        [string]$Run,
        [object]$ExitCode,
        [string]$Status,
        [string]$Detail = ''
    )
    [void]$results.Add([pscustomobject]@{
        Name = $Name; Kind = $Kind; Class = $Class; Build = $Build; Run = $Run
        ExitCode = $ExitCode; Status = $Status; Detail = $Detail
    })
}

# ---------------------------------------------------------------------------
# C++ build + run
# ---------------------------------------------------------------------------
Write-Section 'C++ regression tests'

if (-not $clExe) {
    foreach ($test in $testMatrix) {
        Add-Result -Name $test.Name -Kind 'C++' -Class $test.Class -Build 'NOT RUN' -Run 'NOT RUN' -ExitCode $null `
            -Status 'NOT RUN' -Detail 'MSVC cl.exe is not available'
    }
    Write-Host 'Skipped: cl.exe is not available.' -ForegroundColor Yellow
} else {
    foreach ($test in $testMatrix) {
        Write-Host ''
        Write-Host ("-- {0}: {1}" -f $test.Name, $test.Description)

        # Per-test config-path header so the module's cfg file lands in the
        # temporary build directory instead of \work\gentlestop.cfg.
        $forceArgs = @()
        if ($test.CfgHeader) {
            $cfgPath = (Join-Path $buildDir ($test.Name + '.cfg')).Replace('\', '/')
            $headerPath = Join-Path $buildDir ($test.Name + '-cfg.h')
            @(
                '#ifndef GS_TEST_CFG_FORCE_H',
                '#define GS_TEST_CFG_FORCE_H',
                '#ifndef GS_CFG_PATH',
                ('#define GS_CFG_PATH "{0}"' -f $cfgPath),
                '#endif',
                '#endif'
            ) | Set-Content -LiteralPath $headerPath -Encoding ASCII
            $forceArgs = @('/FI', $headerPath)
        }

        $objects = New-Object System.Collections.ArrayList
        $buildOk = $true
        $buildDetail = ''

        foreach ($source in $test.Sources) {
            $sourcePath = Join-Path $script:TestsDir $source
            if (-not (Test-Path -LiteralPath $sourcePath)) {
                $sourcePath = Join-Path $script:WasmSource $source
            }
            if (-not (Test-Path -LiteralPath $sourcePath)) {
                $buildOk = $false
                $buildDetail = "missing source file: $source"
                break
            }
            $objPath = Join-Path $buildDir ($test.Name + '__' + [System.IO.Path]::GetFileNameWithoutExtension($source) + '.obj')
            $clArgs = @('/nologo', '/c', '/EHsc', '/W3', '/D_CRT_SECURE_NO_WARNINGS')
            foreach ($define in $test.Defines) { $clArgs += ('/D' + $define) }
            $clArgs += $forceArgs
            $clArgs += @('/I', $script:StubsDir, '/I', $script:TestsDir, '/I', $script:WasmSource)
            $clArgs += @($sourcePath, ('/Fo' + $objPath))

            $compile = Invoke-Process -FilePath $clExe -Arguments $clArgs -WorkingDirectory $buildDir -TimeoutSeconds $TestTimeoutSeconds
            $logPath = Join-Path $buildDir ($test.Name + '__' + [System.IO.Path]::GetFileNameWithoutExtension($source) + '.compile.log')
            (($compile.StdOut + "`n" + $compile.StdErr).Trim()) | Set-Content -LiteralPath $logPath -Encoding UTF8
            if ($compile.TimedOut -or $compile.ExitCode -ne 0) {
                $buildOk = $false
                $buildDetail = "compile failed for $source (exit $(if ($compile.TimedOut) { 'timeout' } else { $compile.ExitCode })); log: $logPath"
                Write-Host ("   compile FAILED: {0}" -f $source) -ForegroundColor Red
                Show-ProcessOutput $compile
                break
            }
            [void]$objects.Add($objPath)
        }

        if (-not $buildOk) {
            Add-Result -Name $test.Name -Kind 'C++' -Class $test.Class -Build 'FAIL' -Run 'NOT RUN' -ExitCode $null -Status 'FAIL' -Detail $buildDetail
            continue
        }

        $exePath = Join-Path $buildDir ($test.Name + '.exe')
        $linkArgs = @('/nologo') + @($objects) + @(('/Fe' + $exePath))
        $link = Invoke-Process -FilePath $clExe -Arguments $linkArgs -WorkingDirectory $buildDir -TimeoutSeconds $TestTimeoutSeconds
        $linkLog = Join-Path $buildDir ($test.Name + '.link.log')
        (($link.StdOut + "`n" + $link.StdErr).Trim()) | Set-Content -LiteralPath $linkLog -Encoding UTF8
        if ($link.TimedOut -or $link.ExitCode -ne 0) {
            Write-Host '   link FAILED' -ForegroundColor Red
            Show-ProcessOutput $link
            Add-Result -Name $test.Name -Kind 'C++' -Class $test.Class -Build 'FAIL' -Run 'NOT RUN' -ExitCode $null -Status 'FAIL' -Detail "link failed (exit $($link.ExitCode)); log: $linkLog"
            continue
        }
        Write-Host '   build OK'

        $runLog = Join-Path $buildDir ($test.Name + '.run.log')
        if ($null -ne $test.StdIn) {
            $run = Invoke-Process -FilePath $exePath -Arguments @() -WorkingDirectory $buildDir -TimeoutSeconds $TestTimeoutSeconds -StandardInput $test.StdIn
            $runDetail = 'stdin-driven smoke run (non-asserting; this tree has no parity reference); log: ' + $runLog
        } else {
            $run = Invoke-Process -FilePath $exePath -Arguments @() -WorkingDirectory $buildDir -TimeoutSeconds $TestTimeoutSeconds
            $runDetail = "log: $runLog"
        }
        (($run.StdOut + "`n" + $run.StdErr).Trim()) | Set-Content -LiteralPath $runLog -Encoding UTF8
        if ($run.TimedOut) {
            Write-Host '   run FAILED: timeout' -ForegroundColor Red
            Add-Result -Name $test.Name -Kind 'C++' -Class $test.Class -Build 'PASS' -Run 'FAIL' -ExitCode 'timeout' -Status 'FAIL' -Detail "timed out after $TestTimeoutSeconds s; log: $runLog"
        } elseif ($run.ExitCode -eq 0) {
            Write-Host '   run PASS (exit 0)' -ForegroundColor Green
            Show-ProcessOutput $run
            Add-Result -Name $test.Name -Kind 'C++' -Class $test.Class -Build 'PASS' -Run 'PASS' -ExitCode 0 -Status 'PASS' -Detail $runDetail
        } else {
            Write-Host ("   run FAIL (exit {0})" -f $run.ExitCode) -ForegroundColor Red
            Show-ProcessOutput $run
            Add-Result -Name $test.Name -Kind 'C++' -Class $test.Class -Build 'PASS' -Run 'FAIL' -ExitCode $run.ExitCode -Status 'FAIL' -Detail "log: $runLog"
        }
    }
}

# ---------------------------------------------------------------------------
# JavaScript tests (the legacy parity harness is retired, see the summary)
# ---------------------------------------------------------------------------
Write-Section 'JavaScript tests'

$panelScript = Join-Path $script:TestsDir 'test_panel.js'

if (-not $nodeExe) {
    Add-Result -Name 'test_panel.js' -Kind 'JavaScript' -Class 'panel' -Build '-' -Run 'NOT RUN' -ExitCode $null -Status 'NOT RUN' -Detail 'Node.js is not available'
    Write-Host '-- Panel JavaScript test: NOT RUN (Node.js is not available)' -ForegroundColor Yellow
} else {
    Write-Host '-- Panel JavaScript test (wasm-source/tests/test_panel.js)'
    $panel = Invoke-Process -FilePath $nodeExe -Arguments @($panelScript) -WorkingDirectory $buildDir -TimeoutSeconds $TestTimeoutSeconds
    $panelLog = Join-Path $buildDir 'test_panel.log'
    (($panel.StdOut + "`n" + $panel.StdErr).Trim()) | Set-Content -LiteralPath $panelLog -Encoding UTF8
    Show-ProcessOutput $panel 25
    if ($panel.TimedOut) {
        Add-Result -Name 'test_panel.js' -Kind 'JavaScript' -Class 'panel' -Build '-' -Run 'FAIL' -ExitCode 'timeout' -Status 'FAIL' -Detail "timed out; log: $panelLog"
    } elseif ($panel.ExitCode -eq 0) {
        Add-Result -Name 'test_panel.js' -Kind 'JavaScript' -Class 'panel' -Build '-' -Run 'PASS' -ExitCode 0 -Status 'PASS' -Detail "log: $panelLog"
    } else {
        Add-Result -Name 'test_panel.js' -Kind 'JavaScript' -Class 'panel' -Build '-' -Run 'FAIL' -ExitCode $panel.ExitCode -Status 'FAIL' -Detail "log: $panelLog"
    }
}

Write-Host ''
Write-Host '-- Legacy parity harness: RETIRED (no legitimate public reference engine)'

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
function Get-GroupStatus([object[]]$Items) {
    if ($Items.Count -eq 0) { return 'NOT RUN' }
    if (@($Items | Where-Object { $_.Status -eq 'FAIL' }).Count -gt 0) { return 'FAIL' }
    if (@($Items | Where-Object { $_.Status -eq 'NOT RUN' }).Count -gt 0) { return 'PARTIAL' }
    return 'PASS'
}

Write-Section 'Summary'
foreach ($result in $results) {
    $color = switch ($result.Status) {
        'PASS' { 'Green' }
        'FAIL' { 'Red' }
        default { 'Yellow' }
    }
    $exitText = if ($null -eq $result.ExitCode) { '-' } else { [string]$result.ExitCode }
    $label = if ($result.Class -eq 'smoke') { $result.Name + ' (smoke)' } else { $result.Name }
    Write-Host ("  [{0,-7}] {1,-30} exit {2,-8} {3}" -f $result.Status, $label, $exitText, $result.Detail) -ForegroundColor $color
}

$cpp = @($results | Where-Object { $_.Kind -eq 'C++' })
$regression = @($results | Where-Object { $_.Class -eq 'regression' })
$integration = @($results | Where-Object { $_.Class -eq 'integration' })
$smoke = @($results | Where-Object { $_.Class -eq 'smoke' })
$panelResults = @($results | Where-Object { $_.Class -eq 'panel' })
$required = @($regression + $integration + $panelResults)

$failed = @($results | Where-Object { $_.Status -eq 'FAIL' })
$notRun = @($results | Where-Object { $_.Status -eq 'NOT RUN' })
$requiredPassed = @($required | Where-Object { $_.Status -eq 'PASS' })

$buildFailed = @($cpp | Where-Object { $_.Build -eq 'FAIL' })
$buildNotRun = @($cpp | Where-Object { $_.Build -eq 'NOT RUN' })
if ($buildFailed.Count -gt 0) { $cppBuild = 'FAIL' }
elseif ($buildNotRun.Count -eq $cpp.Count) { $cppBuild = 'NOT RUN' }
else { $cppBuild = 'PASS' }

$smokeStatus = if ($smoke.Count -eq 0) { 'NOT RUN' } elseif ((Get-GroupStatus $smoke) -eq 'PASS') { 'PASS (non-asserting)' } else { Get-GroupStatus $smoke }

Write-Host ''
Write-Host ("  C++ build               : {0}" -f $cppBuild)
Write-Host ("  C++ regression tests    : {0} ({1} suites)" -f (Get-GroupStatus $regression), $regression.Count)
Write-Host ("  C++ integration tests   : {0} ({1} suites)" -f (Get-GroupStatus $integration), $integration.Count)
Write-Host ("  C++ smoke test          : {0}" -f $smokeStatus)
Write-Host ("  Panel JavaScript        : {0}" -f (Get-GroupStatus $panelResults))
Write-Host '  Legacy parity harness   : RETIRED (no legitimate public reference engine)'

if ($failed.Count -gt 0) {
    $overall = 'FAIL'
    $color = 'Red'
} elseif ($notRun.Count -gt 0) {
    $overall = 'PARTIAL'
    $color = 'Yellow'
} else {
    $overall = 'PASS'
    $color = 'Green'
}
Write-Host ''
Write-Host ("  Overall result          : {0}  ({1} required passed, {2} failed, {3} not run; {4} smoke run(s))" -f `
    $overall, $requiredPassed.Count, $failed.Count, $notRun.Count, $smoke.Count) -ForegroundColor $color

if ($KeepBuildArtifacts) {
    Write-Host ''
    Write-Host "  Build artifacts retained (outside the repository): $buildDir"
} else {
    if (Test-Path -LiteralPath $buildDir) {
        Remove-Item -LiteralPath $buildDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host ''
    Write-Host "  Build artifacts removed (were in: $buildDir)"
}

if ($overall -eq 'PASS') { exit 0 }
if ($overall -eq 'FAIL') { exit 1 }
exit 2
