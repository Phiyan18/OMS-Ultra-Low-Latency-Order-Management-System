# Market Data

OMS supports **real market data** from two free public sources:

## 1. LOBSTER / NASDAQ (limit order messages)

- **Format:** CSV message file (no header): `Time, Type, OrderID, Size, Price, Direction`
- **Source:** [LOBSTER academic data](https://lobsterdata.com/info/DataSamples.php) — reconstructed from NASDAQ TotalView-ITCH
- **Bundled sample:** `lobster/AMZN_sample_message.csv` (synthetic, LOBSTER-compatible)
- **Full free samples:** Download AMZN/AAPL/GOOG message files from LOBSTER (level 1–10)

```bash
oms --replay data/lobster/AMZN_sample_message.csv
oms --replay path/to/AAPL_2012-06-21_message_5.csv --max 50000
```

## 2. Binance spot trades (crypto tape)

- **Format:** CSV with header: `id,price,qty,quoteQty,time,isBuyerMaker`
- **Source:** [data.binance.vision](https://data.binance.vision) — daily public archives
- **Fetch:** `scripts/fetch_market_data.ps1` or `fetch_market_data.sh`

```bash
oms --trades data/binance/BTCUSDT-trades-2024-06-01.csv --max 100000
```

## Setup

```powershell
.\scripts\fetch_market_data.ps1
```

This generates the LOBSTER sample and downloads one day of BTCUSDT trades.
