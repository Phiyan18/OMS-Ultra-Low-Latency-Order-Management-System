#pragma once

#include "book/l2_book.hpp"
#include "book/order_book.hpp"
#include "signals/momentum.hpp"
#include "signals/obi.hpp"
#include "signals/vpin.hpp"

namespace oms {

struct CompositeSignal {
    double obi{0.0};
    double vpin{0.0};
    double momentum{0.0};
    double composite{0.0};
    TimestampNs timestamp{0};

    static CompositeSignal compute(const L2Snapshot& snap,
                                   VPIN& vpin_tracker,
                                   MidPriceMomentum& mom,
                                   double w_obi = 0.4,
                                   double w_vpin = 0.3,
                                   double w_mom = 0.3) noexcept {
        CompositeSignal sig{};
        sig.timestamp = snap.timestamp;
        sig.obi = OBI::compute(snap, 5);
        sig.vpin = vpin_tracker.value();
        mom.update(snap.mid_price());
        sig.momentum = mom.signal();
        sig.composite = w_obi * sig.obi + w_vpin * sig.vpin + w_mom * sig.momentum;
        return sig;
    }
};

}  // namespace oms
