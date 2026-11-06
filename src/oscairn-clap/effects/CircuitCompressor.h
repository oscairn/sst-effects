/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * A circuit-style compressor whose behaviour follows reference renders (plan, section 15,
 * "Circuit-style compressor"). Written for Oscairn; follows the sst-effects effect contract.
 *  - The input level scales the signal (its gain in dB is the parameter); a peak detector on
 *    the louder channel runs two cascaded followers with the same attack and release times;
 *  - the gain falls above a threshold with a soft knee, at an effective slope set by the ratio;
 *  - a makeup gain set by the input level, ratio, attack and release (never by the signal) keeps
 *    the output level near constant across settings: with L the input gain raised by 24.08 dB,
 *    makeup = 3.01 dB + 20 log10(L) * (-e - 0.6667 s ka + 0.2025 s kr), s = 1 - 1/R, e = 0.7552 / R
 *    above L = 1 and 0.4997 below (with L held at 0.005 there), ka and kr growing with the attack
 *    and release times, capped at 23.73 dB. This is why below threshold the output level depends
 *    on the attack and release settings, and why the attack's effect changes sign at L = 1;
 *  - the output is clipped at full scale. Input or output level at its bottom (-139 dB) is off.
 * Every constant was fitted to renders (fx7, fx13, fx21 to fx23). Times are in seconds, so at
 * other rates the followers keep their 44.1 kHz time constants.
 */
#ifndef OSCAIRN_CLAP_EFFECTS_CIRCUITCOMPRESSOR_H
#define OSCAIRN_CLAP_EFFECTS_CIRCUITCOMPRESSOR_H

#include "sst/effects/EffectCore.h"
#include "sst/basic-blocks/params/ParamMetadata.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace oscairn_fx::dynamics
{
constexpr double designRate = 44100;
// Fitted (plan, section 15): the makeup law's ratio per setting, and the gain law's own slope
// ratio per setting (the gain law flattens at high ratios).
constexpr double makeupRatio[5] = {1.992, 3.890, 7.554, 11.00, 17.19};
constexpr double curveRatio[5] = {3.218, 4.044, 4.698, 4.982, 5.227};
constexpr const char *ratioNames[5] = {"2", "4", "8", "12", "20"};
constexpr double fixedGainDb = 42.03, levelOffsetDb = 24.08, makeupMaxDb = 23.73;
constexpr double thresholdDb = -36.06, kneeDb = 8.40;
constexpr double attackScale = 0.4367, releaseScale = 0.4843;

// The displayed attack and release times (s) for the 0..200 values the converter reads (fx7).
inline double attackSeconds(double v) { return 0.022e-3 + 50.378e-3 * std::pow(v / 200, 2.5); }
inline double releaseSeconds(double v) { return 10.88e-3 + 533.34e-3 * (v / 200) * (v / 200); }

// The makeup gain (dB) for an input gain (dB), ratio setting and attack and release times (s).
inline double makeupDb(double inputDb, int ratio, double attackS, double releaseS)
{
    const double R = makeupRatio[ratio], s = 1 - 1 / R;
    const double lg = (inputDb + levelOffsetDb) / 20; // log10 L
    const double base = lg > 0 ? -0.7552 / R * lg : -0.4997 * std::max(lg, std::log10(0.005));
    const double ka = attackS > 1e-3 ? std::min(1.0, std::log10(attackS / 1e-3) / 1.4996) : 0.0;
    const double kr = releaseS > 40.7e-3 ? std::min(1.0, std::log10(releaseS / 40.7e-3)) : 0.0;
    return std::min(makeupMaxDb, 20 * (base - 0.6667 * s * ka * lg + 0.2025 * s * kr * lg) + 3.01);
}

template <typename FXConfig> struct CircuitCompressor : sst::effects::core::EffectTemplateBase<FXConfig>
{
    enum params
    {
        inputLevel,
        outputLevel,
        ratio,
        attack,
        release,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-circuit-compressor"};
    static constexpr const char *displayName{"Circuit Compressor"};

    CircuitCompressor(typename FXConfig::GlobalStorage *s, typename FXConfig::EffectStorage *e,
                      typename FXConfig::ValueStorage *p)
        : sst::effects::core::EffectTemplateBase<FXConfig>(s, e, p)
    {
    }

    void initialize()
    {
        e1 = e2 = 0;
        last[0] = NAN;
        update();
    }
    void suspendProcessing() { initialize(); }
    int getRingoutDecay() const { return -1; }
    void onSampleRateChanged() { initialize(); }

    void processBlock(float *__restrict L, float *__restrict R)
    {
        update();
        const double s = 1 - 1 / curveRatio[ratioIndex];
        for (int i = 0; i < FXConfig::blockSize; ++i)
        {
            const double l = gin * L[i], r = gin * R[i];
            const double p = std::max(std::abs(l), std::abs(r));
            e1 += (p > e1 ? kA : kR) * (p - e1);
            e2 += (e1 > e2 ? kA : kR) * (e1 - e2);
            const double d = 20 * std::log10(e2 + 1e-30) - thresholdDb;
            double g = 0;
            if (2 * d > kneeDb)
                g = -s * d;
            else if (2 * d > -kneeDb)
                g = -s * (d + kneeDb / 2) * (d + kneeDb / 2) / (2 * kneeDb);
            const double k = std::pow(10.0, g / 20) * gout;
            L[i] = (float)std::clamp(l * k, -1.0, 1.0);
            R[i] = (float)std::clamp(r * k, -1.0, 1.0);
        }
    }

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        switch (idx)
        {
        case inputLevel: return pmd().asFloat().withName("Input Level").withRange(-139.f, 0.f).withDefault(-18.f).withLinearScaleFormatting("dB").withDecimalPlaces(1);
        case outputLevel: return pmd().asFloat().withName("Output Level").withRange(-139.f, 0.f).withDefault(-18.f).withLinearScaleFormatting("dB").withDecimalPlaces(1);
        case ratio: return pmd().asInt().withName("Ratio").withRange(0, 4).withDefault(1).withUnorderedMapFormatting({{0, "2"}, {1, "4"}, {2, "8"}, {3, "12"}, {4, "20"}});
        case attack: return pmd().asFloat().withName("Attack").withRange(0.022f, 50.4f).withDefault(8.9f).withLinearScaleFormatting("ms").withDecimalPlaces(3);
        case release: return pmd().asFloat().withName("Release").withRange(10.88f, 544.2f).withDefault(144.2f).withLinearScaleFormatting("ms").withDecimalPlaces(1);
        }
        return pmd().withName("Unused").withType(pmd::NONE);
    }

  private:
    double e1{0}, e2{0}, gin{0}, gout{0}, kA{1}, kR{1};
    int ratioIndex{1};
    float last[numParamsEnum]{};

    // A follower coefficient fitted at 44.1 kHz, kept as the same time constant at the rate.
    double coefficient(double tauS) const
    {
        const double k44 = std::min(1.0, 1 / (tauS * designRate));
        if (k44 >= 1)
            return 1;
        return 1 - std::pow(1 - k44, designRate / this->sampleRate());
    }

    void update()
    {
        bool changed = false;
        for (int i = 0; i < numParams; ++i)
        {
            const float v = this->floatValue(i);
            changed = changed || !(v == last[i]);
            last[i] = v;
        }
        if (!changed)
            return;
        ratioIndex = std::clamp((int)std::lround(last[ratio]), 0, 4);
        const double atk = std::max((double)last[attack], 0.022) / 1000, rel = std::max((double)last[release], 10.88) / 1000;
        kA = coefficient(attackScale * atk);
        kR = coefficient(releaseScale * rel);
        // the platform's value 1 is -138 dB; below that (its value 0) the level is off
        gin = last[inputLevel] < -138.5f ? 0.0 : std::pow(10.0, last[inputLevel] / 20.0);
        gout = last[outputLevel] < -138.5f ? 0.0
               : std::pow(10.0, (makeupDb(last[inputLevel], ratioIndex, atk, rel) + fixedGainDb + last[outputLevel]) / 20);
    }
};
} // namespace oscairn_fx::dynamics

#endif
