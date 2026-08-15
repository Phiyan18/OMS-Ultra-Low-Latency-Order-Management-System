# OMS — Ultra-Low Latency Order Management System

A **C++17 order management system** with an interactive desktop-style console UI, browser-based HTML reports, price-time matching, lock-free L2 reads, SPSC multi-threaded ingress, WAL replay, and a built-in alpha signal pipeline.

**Shippable product:** one executable — `oms.exe` (Windows) or `oms` (Linux/macOS) — no Python, no separate demo binaries required.

---
## Running

<img width="1920" height="1004" alt="Recording 2026-08-15 180612" src="https://github.com/user-attachments/assets/3a245a7e-d7d4-4741-8245-1c9edc1e9598" />

---

## Who Is This For?

| Audience | How OMS Helps |
|----------|----------------|
| **Quant / systematic traders** | Prototype signal → execution loops: OBI, VPIN, momentum, composite alpha, and a simple backtest with cost sensitivity before wiring a production stack. |
| **Market microstructure researchers** | Study price-time FIFO matching, L2 depth, alpha decay (IC vs horizon), and WAL-auditable state reconstruction. |
| **C++ / HFT engineers** | Learn patterns used in production: pool allocators, SPSC queues, double-buffered snapshots, fixed-point prices, and sub-microsecond book ops (Release builds). |

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

## Real Market Data 

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
