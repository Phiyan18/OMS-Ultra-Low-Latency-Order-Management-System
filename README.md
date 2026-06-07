# OMS — Ultra-Low Latency Order Management System

A **C++17 order management system** with an interactive desktop-style console UI, browser-based HTML reports, price-time matching, lock-free L2 reads, SPSC multi-threaded ingress, WAL replay, and a built-in alpha signal pipeline.

**Shippable product:** one executable — `oms.exe` (Windows) or `oms` (Linux/macOS) — no Python, no separate demo binaries required.

---

## Who Is This For?

| Audience | How OMS Helps |
|----------|----------------|
| **Quant / systematic traders** | Prototype signal → execution loops: OBI, VPIN, momentum, composite alpha, and a simple backtest with cost sensitivity before wiring a production stack. |
| **Market microstructure researchers** | Study price-time FIFO matching, L2 depth, alpha decay (IC vs horizon), and WAL-auditable state reconstruction. |
| **C++ / HFT engineers** | Learn patterns used in production: pool allocators, SPSC queues, double-buffered snapshots, fixed-point prices, and sub-microsecond book ops (Release builds). |
| **Students & portfolio builders** | A complete, visual, documented codebase suitable for interviews, theses, or extending into a full trading simulator. |

---

## Why Use This Tool?

1. **End-to-end in one binary** — Live L2 ladder, signal bars, matching trades, multithreaded feed, and HTML charts without juggling multiple programs.
2. **Performance-first design** — Hot path avoids heap allocation; targets ~80 ns inserts and ~12 ns top-of-book (see benchmark mode).
3. **Production-shaped architecture** — Producer threads push `OrderCommand` into an SPSC ring; a single consumer owns the book, matcher, and WAL — the same separation used in real OMS/EMS stacks.
4. **Observable** — Terminal ANSI visuals plus `oms_report.html` for sharing results with teammates or reviewers.
5. **Easy to extend** — Header-only modules; swap signal weights, add gateways, or plug in your own alpha without rewriting the book.

---

## What You Can Improve With OMS

| Goal | Starting point in this repo |
|------|-----------------------------|
| Faster matching / custom auction rules | `include/engine/matching_engine.hpp` |
| New alpha factors | `include/signals/` + hook in `CompositeSignal::compute` |
| Real market data feed | `include/io/lobster_feed.hpp`, `binance_trades_feed.hpp`, `market_replay.hpp` |
| Risk limits & kill switch | Wrap `OmsEngine::submit_*` with pre-trade checks |
| Persistence / compliance | Extend `WalWriter` / `WalReplayer` record types |
| GUI or web dashboard | Consume L2 snapshots from `L2BookView` or parse `oms_report.html` |
| Distributed deployment | Partition by symbol; keep one consumer per book shard |

---

## Screenshots & Visuals

### Interactive console (menu option 1)

- Animated **bid/ask ladder** with depth bars  
- **OBI / VPIN / momentum / composite** bar gauges  
- **Sparkline** of composite signal over the session  

Requires a terminal with **ANSI color** support (Windows Terminal, VS Code terminal, iTerm2, any modern Linux console).

### HTML report (menu option 6 or `--showcase`)

Generates **`oms_report.html`** in the project directory:

- Backtest metrics (Sharpe, return, drawdown)  
- Final L2 depth table with bar widths  
- Alpha decay table (IC vs horizon)  
- SVG chart of composite signal  

Open the file in any browser — no server needed.

---

## Real Market Data (NEW)

OMS replays **real exchange data** — not just synthetic RNG feeds.

| Source | What you get | Free access |
|--------|----------------|-------------|
| **LOBSTER / NASDAQ** | Limit-order submissions, cancels, executions (TotalView-ITCH) | [Sample files](https://lobsterdata.com/info/DataSamples.php) + bundled `data/lobster/AMZN_sample_message.csv` |
| **Binance Vision** | Spot trade tape (price, qty, aggressor side) | [data.binance.vision](https://data.binance.vision) daily CSV archives |

### One-command data setup

```powershell
.\scripts\fetch_market_data.ps1
```

```bash
chmod +x scripts/fetch_market_data.sh && ./scripts/fetch_market_data.sh
```

### Replay NASDAQ limit-order events

```bash
./build/oms --replay data/lobster/AMZN_sample_message.csv
./build/oms --replay path/to/AAPL_message_5.csv --max 50000 --report replay.html
```

Rebuilds the **L3 order book** from real message types (submit / partial cancel / delete / execute), computes **OBI, VPIN, alpha decay**, spread analytics, and volume-at-price profile.

### Analyze Binance trade tape

```bash
./build/oms --trades data/binance/BTCUSDT-trades-2024-06-01.csv --max 100000
```

Feeds **VPIN** and **momentum** from real crypto prints with price sparklines.

See [`data/README.md`](data/README.md) for format details.

---

## Quick Start

### Prerequisites

- **CMake 3.16+**
- **C++17 compiler** (GCC 10+, Clang 12+, MSVC 2019+)

| Platform | Recommended setup |
|----------|-------------------|
| Windows | [MSYS2 UCRT64](https://www.msys2.org/): `pacman -S mingw-w64-ucrt-x86_64-gcc cmake ninja` |
| Windows | Visual Studio 2022 with **Desktop development with C++** |
| Linux | `sudo apt install build-essential cmake ninja-build` |
| macOS | Xcode CLT + `brew install cmake` |

### Build (Release)

**MSYS2 UCRT64** (matches the environment used for CI-style builds):

```bash
cd "/c/Users/You/Documents/Projects/c++_oms"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

**Visual Studio:**

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

**Linux / macOS:**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Disable AVX2 on older CPUs:

```bash
cmake -B build -DOMS_ENABLE_AVX2=OFF
cmake --build build
```

### Run the product

```bash
# Interactive menu (recommended first run)
./build/oms

# Full automated demo + HTML report (CI, demos, sharing)
./build/oms --showcase
./build/oms --showcase --report /path/to/report.html
```

**Windows:** `build\oms.exe` or `build\Release\oms.exe` (VS).

> **MinGW / MSYS2:** Add `C:\msys64\ucrt64\bin` to your `PATH` (or run from the **UCRT64** shell) so `libgcc_s_seh-1.dll` and related runtime DLLs are found.

### Developer tools (optional)

```bash
./build/oms_test          # unit tests
./build/oms_benchmark     # detailed latency suite
ctest --test-dir build    # CTest
```

Legacy standalone demos (`oms_demo`, `oms_matching_demo`) are available with:

```bash
cmake -B build -DOMS_BUILD_LEGACY_DEMOS=ON
cmake --build build
```

---

## Menu Guide

| Key | Mode | Description |
|-----|------|-------------|
| **1** | Live Trading Desk | Simulated order flow with refreshing L2 ladder and signals |
| **2** | Matching Engine | Aggressive order crosses resting liquidity; prints trades |
| **3** | SPSC + WAL | Producer/consumer threads, `oms.wal` log, replay verification |
| **4** | Alpha Decay | IC vs horizon from 10 ms to 10 s |
| **5** | Quick Benchmark | ~50k ops timing for add/cancel/best |
| **6** | Full Showcase | Runs key demos and writes `oms_report.html` |
| **7** | NASDAQ Replay | LOBSTER CSV → L3 book + signals + `oms_replay_report.html` |
| **8** | Binance Trades | Real BTC trade tape → VPIN / momentum sparklines |
| **9** | Data Setup | Verify files + download instructions |
| **0** | Exit | |

---

## Architecture

```
┌─────────────────── Producer Thread(s) ───────────────────┐
│  submit_add / submit_cancel / submit_modify              │
│           │ lock-free SPSC push                          │
└───────────┼──────────────────────────────────────────────┘
            ▼
┌─────────────────── Consumer Thread ──────────────────────┐
│  OmsEngine::process_all()                                │
│    ├── MatchingEngine (price-time FIFO)                  │
│    ├── OrderBook L3 (O(1) add/modify/cancel/execute)     │
│    └── WalWriter (64-byte fixed records)                 │
├──────────────────────────────────────────────────────────┤
│  L2BookView — double-buffered atomic snapshot (readers)  │
├──────────────────────────────────────────────────────────┤
│  Signals: OBI | VPIN | Momentum | Composite              │
│  Backtest: Alpha Decay IC | Sharpe | Cost Sensitivity    │
└──────────────────────────────────────────────────────────┘
            │
            ▼ replay
┌─────────────────── WalReplayer ──────────────────────────┐
│  Rebuild book state from oms.wal                         │
└──────────────────────────────────────────────────────────┘
```

---

## Features

| Feature | Implementation |
|---------|----------------|
| L3 order book | Per-order FIFO queues at each price level |
| L2 aggregation | `L2Snapshot` up to 64 levels |
| O(1) hot path | Hash index + intrusive lists + pool allocator |
| Lock-free reads | Double-buffered `L2BookView` |
| SPSC ingress | `SPSCQueue<OrderCommand>` |
| Matching | Price-time priority crossing |
| WAL | Fixed-size records; append + replay |
| Signals | OBI, VPIN, momentum, composite blend |
| Backtest | Threshold strategy + linear costs |
| Visuals | ANSI console UI + HTML report |

---

## Project Layout

```
include/
├── book/           OrderBook, L2BookView, Trade
├── engine/         MatchingEngine, OmsEngine
├── io/             OrderCommand, WalWriter, LobsterFeed, BinanceTrades, MarketReplay
├── analytics/      MarketStats, VolumeProfile
├── queue/          SPSCQueue
├── memory/         PoolAllocator
├── signals/        OBI, VPIN, Momentum, Composite
├── backtest/       AlphaDecay, BacktestEngine
├── app/            real_data_modes.hpp
└── ui/             console_visual.hpp, html_report.hpp
data/
├── lobster/        LOBSTER-format NASDAQ message CSVs
└── binance/        Binance daily trade CSVs (after fetch)
scripts/            fetch_market_data.ps1 / .sh, generate_lobster_sample.py
src/
└── oms_main.cpp    Unified product entry point
benchmarks/         oms_benchmark (dev)
tests/              oms_test (dev)
```

---

## API Examples

### Direct book + L2 publish

```cpp
#include "book/order_book.hpp"

oms::OrderBook<> book;
book.add_order(1, oms::Side::Bid, 1'000'000, 100);

oms::L2BookView view;
book.publish_l2(view);  // lock-free for reader threads
oms::L2Snapshot snap = view.load();
```

### Matching engine

```cpp
#include "engine/matching_engine.hpp"

oms::OrderBook<> book;
oms::MatchingEngine engine(book);
engine.submit_order(1, oms::Side::Ask, 1'000'100, 100);
auto result = engine.submit_order(2, oms::Side::Bid, 1'000'150, 120);
```

### SPSC + WAL

```cpp
#include "engine/oms_engine.hpp"
#include "io/wal.hpp"

oms::OmsEngine<> engine;
oms::WalWriter wal("oms.wal");
engine.submit_add(id, oms::Side::Bid, price, qty);  // producer
engine.process_all(&wal);                            // consumer
```

---

## Performance Targets (Release, bare metal)

| Operation | Target |
|-----------|--------|
| Order insert | ~80 ns |
| Cancel | ~45 ns |
| Best bid/ask | ~12 ns |
| Throughput | ~12M ops/sec |

Always benchmark with **Release** builds. Debug builds are 10–50× slower.

---

## Price Format

Prices are **fixed-point `int64_t`**: multiply dollars by `10_000`.

| Display | Internal |
|---------|----------|
| $100.00 | `1'000'000` |
| $100.01 | `1'000'100` |

---

## Distribution (single EXE)

Copy only:

- `oms.exe` (or `oms`)
- Optionally `oms_report.html` after running `--showcase`

No runtime DLLs beyond the C++ standard library and `pthread` on MinGW (linked statically in many setups).

To install system-wide (optional):

```bash
cmake --install build --prefix /usr/local
# Installs bin/oms when install rules are enabled
```

---

## Disclaimer

This project is for **research, education, and prototyping**. It is not production trading software. No warranty; use at your own risk. Not financial advice.

---

## License

MIT
