#pragma once
#include <cstdint>
#include <random>

namespace pitchframe {

enum class LossPolicy { None, EveryNth, RandomPct, Burst };

// Test-only receive-path filter — gated behind a CLI flag, never in the
// production path (CLAUDE.md). Seeded RNG makes any run's drop sequence
// exactly reproducible for a given seed.
//
// Deliberately does not know about the end-of-stream sentinel. The
// sentinel's survival is guaranteed by the caller (pitchframe_net) never
// routing a zero-length ("sentinel") packet through should_drop() at all —
// see udp_receiver.cpp. Keeping that exemption at the call site, rather than
// baking sentinel-awareness into this class, keeps the loss policy itself
// general-purpose and testable in isolation.
class PacketLossInjector {
public:
    PacketLossInjector() = default;

    void configure_none() { policy_ = LossPolicy::None; }

    void configure_every_nth(uint64_t n) {
        policy_  = LossPolicy::EveryNth;
        every_n_ = n;
    }

    void configure_random_pct(double pct, uint64_t seed) {
        policy_ = LossPolicy::RandomPct;
        pct_    = pct;
        rng_.seed(static_cast<uint32_t>(seed));
    }

    void configure_burst(uint64_t burst_start_seq, uint64_t burst_len) {
        policy_           = LossPolicy::Burst;
        burst_start_seq_  = burst_start_seq;
        burst_len_        = burst_len;
    }

    bool should_drop(uint64_t seq) {
        switch (policy_) {
            case LossPolicy::None:      return false;
            case LossPolicy::EveryNth:  return every_n_ > 0 && (seq % every_n_ == 0);
            case LossPolicy::RandomPct: return dist_(rng_) < pct_;
            case LossPolicy::Burst:
                return seq >= burst_start_seq_ && seq < burst_start_seq_ + burst_len_;
        }
        return false;
    }

private:
    LossPolicy policy_          = LossPolicy::None;
    uint64_t   every_n_         = 0;
    double     pct_             = 0.0;
    uint64_t   burst_start_seq_ = 0;
    uint64_t   burst_len_       = 0;

    std::mt19937                           rng_;
    std::uniform_real_distribution<double> dist_{0.0, 1.0};
};

} // namespace pitchframe
