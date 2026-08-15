#!/usr/bin/env bash
# Download free real market data for OMS replay.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOBSTER_DIR="$ROOT/data/lobster"
BINANCE_DIR="$ROOT/data/binance"
mkdir -p "$LOBSTER_DIR" "$BINANCE_DIR"

echo "=== OMS Market Data Fetch ==="

echo "[1/2] Generating LOBSTER-format AMZN sample..."
python3 "$ROOT/scripts/generate_lobster_sample.py" || python "$ROOT/scripts/generate_lobster_sample.py"

SYMBOL="BTCUSDT"
DATE="2024-06-01"
ZIP="$BINANCE_DIR/${SYMBOL}-trades-${DATE}.zip"
CSV="$BINANCE_DIR/${SYMBOL}-trades-${DATE}.csv"
URL="https://data.binance.vision/data/spot/daily/trades/${SYMBOL}/${SYMBOL}-trades-${DATE}.zip"

if [[ ! -f "$CSV" ]]; then
  echo "[2/2] Downloading Binance $SYMBOL trades ($DATE)..."
  curl -fL "$URL" -o "$ZIP"
  unzip -o "$ZIP" -d "$BINANCE_DIR"
else
  echo "[2/2] Binance CSV already present: $CSV"
fi

echo ""
echo "Full LOBSTER NASDAQ samples: https://lobsterdata.com/info/DataSamples.php"
echo "  oms --replay data/lobster/YOUR_MESSAGE_FILE.csv"
echo "Done."
