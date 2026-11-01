/*
 * Oscairn effects: sst-effects as CLAP plugins for the Oscairn host (doc/oscairn-plan.md).
 * GPL-3.0-or-later, as the rest of this repository.
 *
 * The FXConfig the plugins run their effects with. It follows ConcreteConfig, with two changes:
 * randomness comes from a per-effect generator with a fixed seed, so a render is repeatable,
 * and there are more parameter slots.
 */
#ifndef OSCAIRN_CLAP_OSCAIRNCONFIG_H
#define OSCAIRN_CLAP_OSCAIRNCONFIG_H

#include <cmath>
#include <cstdint>

namespace oscairn_fx
{
struct Config
{
    struct BC
    {
        static constexpr uint16_t maxParamCount{32};
        float paramStorage[maxParamCount]{};
        template <typename... Types> BC(Types...) {}
    };

    struct GS
    {
        double sampleRate;
        uint32_t rng{0x9E3779B9u};
        GS(double sr) : sampleRate(sr) {}
    };
    struct ES
    {
    };

    using BaseClass = BC;
    using GlobalStorage = GS;
    using EffectStorage = ES;
    using ValueStorage = float *;
    using BiquadAdapter = Config;

    static constexpr int blockSize{16};

    static inline float floatValueAt(const BaseClass *const e, const ValueStorage *const, int idx)
    {
        return e->paramStorage[idx];
    }
    static inline int intValueAt(const BaseClass *const e, const ValueStorage *const, int idx)
    {
        return (int)std::round(e->paramStorage[idx]);
    }

    static inline float envelopeRateLinear(GlobalStorage *s, float f)
    {
        return std::pow(2.f, -f) * (blockSize / (s->sampleRate));
    }

    static inline float temposyncRatio(GlobalStorage *, EffectStorage *, int) { return 1; }

    static inline bool isDeactivated(EffectStorage *, int) { return false; }
    static inline bool isTemposynced(EffectStorage *, int) { return false; }
    static inline bool isExtended(EffectStorage *, int) { return false; }

    // xorshift32: deterministic per effect instance.
    static inline float rand01(GlobalStorage *s)
    {
        s->rng ^= s->rng << 13;
        s->rng ^= s->rng >> 17;
        s->rng ^= s->rng << 5;
        return (float)(s->rng >> 8) / (float)(1u << 24);
    }

    static inline double sampleRate(GlobalStorage *s) { return s->sampleRate; }
    static inline double sampleRateInv(GlobalStorage *s) { return 1.0 / s->sampleRate; }

    static inline float noteToPitch(GlobalStorage *, float p) { return std::pow(2.0, p / 12); }
    static inline float noteToPitchIgnoringTuning(GlobalStorage *s, float p)
    {
        return noteToPitch(s, p);
    }
    static inline float noteToPitchInv(GlobalStorage *s, float p)
    {
        return 1.0 / noteToPitch(s, p);
    }

    static inline float dbToLinear(GlobalStorage *, float f) { return std::pow(10.f, f / 20.f); }
};
} // namespace oscairn_fx

#endif
