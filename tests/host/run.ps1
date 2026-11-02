$ErrorActionPreference = 'Stop'

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$gccCommand = Get-Command gcc -ErrorAction SilentlyContinue
if ($null -ne $gccCommand) {
    $gccPath = $gccCommand.Source
} else {
    $fallbackGcc = 'E:\vscode_msys\ucrt64\bin\gcc.exe'
    if (-not (Test-Path -LiteralPath $fallbackGcc)) {
        throw 'GCC was not found. Install GCC or add gcc.exe to PATH.'
    }
    $gccPath = $fallbackGcc
}

$testExe = Join-Path $PSScriptRoot ("test_core_$PID.exe")
$sources = @(
    (Join-Path $projectRoot 'Application\src\fan_core.c'),
    (Join-Path $projectRoot 'Application\src\battery_math.c'),
    (Join-Path $projectRoot 'Application\src\app_input.c'),
    (Join-Path $projectRoot 'Application\src\game_engine.c'),
    (Join-Path $projectRoot 'Hardware\esp32\oled_font.c'),
    (Join-Path $PSScriptRoot 'test_core.c')
)
$includeDir = Join-Path $projectRoot 'Application\include'
$gccRoot = Split-Path (Split-Path (Split-Path $gccPath -Parent) -Parent) -Parent
$msysBash = Join-Path $gccRoot 'usr\bin\bash.exe'
$junctionPath = $null
$testAliasPath = $null

try {
    if (Test-Path -LiteralPath $msysBash) {
        # MSYS2's shell provides its expected runtime environment. The ASCII
        # junction avoids passing this project's Chinese path through MSYS argv.
        $junctionPath = Join-Path $env:TEMP ("fan_host_$PID")
        New-Item -ItemType Junction -Path $junctionPath -Target $projectRoot | Out-Null
        $posixRoot = '/e/' + ($junctionPath.Substring(3).Replace('\', '/'))
        $posixExe = "$posixRoot/tests/host/test_core_$PID.exe"
        $posixSources = @(
            "$posixRoot/Application/src/fan_core.c",
            "$posixRoot/Application/src/battery_math.c",
            "$posixRoot/Application/src/app_input.c",
            "$posixRoot/Application/src/game_engine.c",
            "$posixRoot/Hardware/esp32/oled_font.c",
            "$posixRoot/tests/host/test_core.c"
        )
        $sourceArgs = ($posixSources | ForEach-Object { "'$_'" }) -join ' '
        $compileCommand = "export MSYSTEM=UCRT64; export PATH=/ucrt64/bin:/usr/bin; /ucrt64/bin/gcc -std=c11 -Wall -Wextra -Werror -Wpedantic -I '$posixRoot/Application/include' -I '$posixRoot/Hardware/esp32' $sourceArgs -o '$posixExe'"
        $compileOutput = & $msysBash -lc $compileCommand 2>&1
        $compileExit = $LASTEXITCODE
        $testAliasPath = Join-Path $junctionPath "tests\host\test_core_$PID.exe"
    } else {
        $gccArgs = @('-std=c11', '-Wall', '-Wextra', '-Werror', '-Wpedantic',
                     '-I', $includeDir,
                     '-I', (Join-Path $projectRoot 'Hardware\esp32')) + $sources + @('-o', $testExe)
        $compileOutput = & $gccPath @gccArgs 2>&1
        $compileExit = $LASTEXITCODE
        $testAliasPath = $testExe
    }

    if ($compileOutput) {
        $compileOutput | ForEach-Object { Write-Output $_ }
    }
    if ($compileExit -ne 0) {
        throw "GCC compilation failed with exit code $compileExit"
    }

    & $testAliasPath
    if ($LASTEXITCODE -ne 0) {
        throw "Host core tests failed with exit code $LASTEXITCODE"
    }
} finally {
    if ($null -ne $testAliasPath -and (Test-Path -LiteralPath $testAliasPath)) {
        Remove-Item -LiteralPath $testAliasPath -Force
    }
    if ($null -ne $junctionPath -and (Test-Path -LiteralPath $junctionPath)) {
        [System.IO.Directory]::Delete($junctionPath, $false)
    }
}
