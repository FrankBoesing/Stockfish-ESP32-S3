# Run the native Stockfish GUI rule regression tests from Windows PowerShell.
$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $RepoRoot

$Compiler = if ($env:CXX) { $env:CXX } else { "g++" }
if (-not (Get-Command $Compiler -ErrorAction SilentlyContinue)) {
    Write-Error "C++ compiler '$Compiler' was not found. Install a native MinGW-w64 g++ (or set CXX to a compatible compiler) and make sure it is in PATH."
    exit 1
}

$OutputExe = Join-Path ([System.IO.Path]::GetTempPath()) "stockfish-gui-rules-tests-$PID.exe"

$StockfishSources = @(
    "src/stockfish/attacks.cpp"
    "src/stockfish/benchmark.cpp"
    "src/stockfish/bitboard.cpp"
    "src/stockfish/evaluate.cpp"
    "src/stockfish/misc.cpp"
    "src/stockfish/movegen.cpp"
    "src/stockfish/movepick.cpp"
    "src/stockfish/position.cpp"
    "src/stockfish/search.cpp"
    "src/stockfish/thread.cpp"
    "src/stockfish/timeman.cpp"
    "src/stockfish/tt.cpp"
    "src/stockfish/uci.cpp"
    "src/stockfish/ucioption.cpp"
    "src/stockfish/tune.cpp"
    "src/stockfish/syzygy/tbprobe.cpp"
    "src/stockfish/nnue/nnue_accumulator.cpp"
    "src/stockfish/nnue/nnue_misc.cpp"
    "src/stockfish/nnue/network.cpp"
    "src/stockfish/nnue/features/p_hm.cpp"
    "src/stockfish/engine.cpp"
    "src/stockfish/score.cpp"
    "src/stockfish/memory.cpp"
    "src/stockfish/esp_fatal.cpp"
    "src/stockfish/position_status.cpp"
)

try {
    Write-Host "Building native Stockfish GUI rule regression tests..."

    $CompilerArgs = @(
        "-std=c++17"
        "-O2"
        "-pthread"
        "-DNNUE_EMBEDDING_OFF"
        "-DNO_TABLEBASES"
        "-DNO_PREFETCH"
        "-Isrc"
        "-Isrc/stockfish"
        "-Itests/native/stubs"
        "tests/native/test_chess_rules.cpp"
    ) + $StockfishSources + @("-o", $OutputExe)

    & $Compiler @CompilerArgs
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    Write-Host "Running native Stockfish GUI rule regression tests..."
    & $OutputExe
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}
finally {
    if (Test-Path $OutputExe) {
        Remove-Item $OutputExe -Force
    }
}
