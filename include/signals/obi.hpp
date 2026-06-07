#pragma once

#include "book/l2_book.hpp"
#include "common/types.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace oms {

// Order Book Imbalance: (bid_qty - ask_qty) / (bid_qty + ask_qty)
struct OBI {
    static double compute(const L2Snapshot& snap, int depth = 5) noexcept {
        int d = std::min(depth, std::max(snap.bid_count, snap.ask_count));
        if (d == 0) return 0.0;

        int64_t bid_sum = 0;
        int64_t ask_sum = 0;

#if defined(__AVX2__)
        if (d >= 4) {
            __m256i bid_acc = _mm256_setzero_si256();
            __m256i ask_acc = _mm256_setzero_si256();
            int i = 0;
            for (; i + 4 <= d; i += 4) {
                __m256i bq = _mm256_set_epi64x(
                    snap.bids[i + 3].qty, snap.bids[i + 2].qty,
                    snap.bids[i + 1].qty, snap.bids[i].qty);
                __m256i aq = _mm256_set_epi64x(
                    snap.asks[i + 3].qty, snap.asks[i + 2].qty,
                    snap.asks[i + 1].qty, snap.asks[i].qty);
                bid_acc = _mm256_add_epi64(bid_acc, bq);
                ask_acc = _mm256_add_epi64(ask_acc, aq);
            }
            alignas(32) int64_t bt[4], at[4];
            _mm256_store_si256(reinterpret_cast<__m256i*>(bt), bid_acc);
            _mm256_store_si256(reinterpret_cast<__m256i*>(at), ask_acc);
            for (int j = 0; j < 4; ++j) {
                bid_sum += bt[j];
                ask_sum += at[j];
            }
            for (; i < d; ++i) {
                bid_sum += snap.bids[i].qty;
                ask_sum += snap.asks[i].qty;
            }
        } else
#endif
        {
            for (int i = 0; i < d; ++i) {
                bid_sum += snap.bids[i].qty;
                ask_sum += snap.asks[i].qty;
            }
        }

        int64_t total = bid_sum + ask_sum;
        if (total == 0) return 0.0;
        return static_cast<double>(bid_sum - ask_sum) / static_cast<double>(total);
    }

    // Top-of-book OBI — fastest path (~few ns)
    static double top_of_book(Quantity bid_qty, Quantity ask_qty) noexcept {
        int64_t total = bid_qty + ask_qty;
        if (total == 0) return 0.0;
        return static_cast<double>(bid_qty - ask_qty) / static_cast<double>(total);
    }
};

}  // namespace oms
