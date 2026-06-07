# Download free real market data for OMS replay.
# Requires: curl (Windows 10+), tar or 7z for Binance zip.

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$LobsterDir = Join-Path $Root "data\lobster"
$BinanceDir = Join-Path $Root "data\binance"

New-Item -ItemType Directory -Force -Path $LobsterDir | Out-Null
New-Item -ItemType Directory -Force -Path $BinanceDir | Out-Null

Write-Host "=== OMS Market Data Fetch ===" -ForegroundColor Cyan

# 1) Synthetic LOBSTER-format NASDAQ sample (always generated)
Write-Host "`n[1/2] Generating LOBSTER-format AMZN sample..."
python (Join-Path $PSScriptRoot "generate_lobster_sample.py")
if ($LASTEXITCODE -ne 0) {
    Write-Host "  Python not found — using committed sample if present." -ForegroundColor Yellow
}

# 2) Binance public spot trades (real crypto tape, ~1 day)
$Symbol = "BTCUSDT"
$Date = "2024-06-01"
$ZipName = "$Symbol-trades-$Date.zip"
$Url = "https://data.binance.vision/data/spot/daily/trades/$Symbol/$ZipName"
$ZipPath = Join-Path $BinanceDir $ZipName
$CsvPath = Join-Path $BinanceDir "$Symbol-trades-$Date.csv"

if (-not (Test-Path $CsvPath)) {
    Write-Host "`n[2/2] Downloading Binance $Symbol trades ($Date)..."
    curl.exe -fL $Url -o $ZipPath
    Expand-Archive -Path $ZipPath -DestinationPath $BinanceDir -Force
    $Extracted = Join-Path $BinanceDir "$Symbol-trades-$Date.csv"
    if (Test-Path $Extracted) {
        Write-Host "  Saved: $Extracted"
    } else {
        Write-Host "  Extract manually from $ZipPath" -ForegroundColor Yellow
    }
} else {
    Write-Host "`n[2/2] Binance CSV already present: $CsvPath"
}

Write-Host "`n=== Full LOBSTER NASDAQ samples (manual) ===" -ForegroundColor Cyan
Write-Host "  Free academic samples: https://lobsterdata.com/info/DataSamples.php"
Write-Host "  Download AMZN/AAPL message CSV, place in data\lobster\, then:"
Write-Host "    .\build\oms.exe --replay data\lobster\YOUR_FILE.csv"
Write-Host "`nDone."
