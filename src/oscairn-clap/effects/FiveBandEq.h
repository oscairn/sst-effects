/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * A five-band EQ: a low shelf, three peaking bands and a high shelf, then an output level.
 * Written for Oscairn, not part of sst-effects; it follows the sst-effects effect contract
 * (EffectCore.h) so the plugin wrapper runs it like any stock effect.
 *
 * Every section is a second-order bilinear filter designed at the running sample rate; the
 * frequency, gain and Q laws were fitted to reference renders (plan, section 15, M1):
 *  - peaking bands: centre at the set frequency, the set gain, and an effective Q of
 *    Q * 10^(-|gain| / 40) * r, so a cut is the exact inverse of the same boost, where
 *    r = min(1, 1.25517 - 4.8172e-5 * f / Hz - 0.1 * log10(Q)) widens high, narrow bands;
 *  - shelves: slope 0.5 (Q from the slope and gain), with the set frequency as the corner on
 *    the flat side: the prewarped frequency is scaled by 10^(|gain| / 40), up for the low shelf
 *    and down for the high shelf, to give the half-gain point.
 */
#ifndef OSCAIRN_CLAP_EFFECTS_FIVEBANDEQ_H
#define OSCAIRN_CLAP_EFFECTS_FIVEBANDEQ_H

#include "sst/effects/EffectCore.h"
#include "sst/basic-blocks/params/ParamMetadata.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace oscairn_fx::eq
{
// One second-order section in transposed direct form II, double precision, two channels.
struct Biquad
{
    double b0{1}, b1{0}, b2{0}, a1{0}, a2{0};
    double z1[2]{}, z2[2]{};
    inline float run(int ch, float x)
    {
        const double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return (float)y;
    }
    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
    void set(double c0, double c1, double c2, double d0, double d1, double d2)
    {
        b0 = c0 / d0;
        b1 = c1 / d0;
        b2 = c2 / d0;
        a1 = d1 / d0;
        a2 = d2 / d0;
    }
};

// The fitted laws are for the 44.1 kHz design the reference renders at.
constexpr double designRate = 44100;

// Q at `rate` for a peaking section whose 44.1 kHz design has centre hz and Q q: keeps the
// centre and both mid-gain edge frequencies (in Hz) of the 44.1 kHz response. In the prewarped
// domain a peaking section's mid-gain edges W+ and W- satisfy W+ W- = 1 and W+ - W- = 1/Q.
inline double peakingQAt(double rate, double hz, double q)
{
    if (rate == designRate)
        return q;
    const double u = 1 / q, k1 = std::tan(M_PI * hz / designRate);
    const double root = std::sqrt(u * u + 4);
    const double hi = designRate / M_PI * std::atan(k1 * (root + u) / 2);
    const double lo = designRate / M_PI * std::atan(k1 * (root - u) / 2);
    const double ratio = std::tan(M_PI * hi / rate) / std::tan(M_PI * lo / rate);
    return 1 / (2 * std::sinh(std::log(ratio) / 2));
}

// Coefficients, exposed for the tests.
inline void peaking(Biquad &f, double rate, double hz, double gainDb, double q)
{
    const double A = std::pow(10.0, gainDb / 40);
    hz = std::min(hz, 0.49 * designRate);
    const double r = std::clamp(1.25517 - 4.8172e-5 * hz - 0.1 * std::log10(q), 0.05, 1.0);
    const double qe = peakingQAt(rate, hz, q * std::pow(10.0, -std::abs(gainDb) / 40) * r);
    const double w0 = 2 * M_PI * hz / rate;
    const double al = std::sin(w0) / (2 * qe), c = std::cos(w0);
    f.set(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}

inline void shelf(Biquad &f, double rate, double hz, double gainDb, bool high)
{
    const double A = std::pow(10.0, std::abs(gainDb) / 40), G = std::pow(10.0, gainDb / 40);
    const double W = std::tan(M_PI * std::min(hz, 0.49 * rate) / rate) * (high ? 1 / A : A);
    const double w0 = 2 * std::atan(W);
    const double slope = 0.5, q = 1 / std::sqrt((G + 1 / G) * (1 / slope - 1) + 2);
    const double c = std::cos(w0), s2 = 2 * std::sqrt(G) * std::sin(w0) / (2 * q);
    if (high)
        f.set(G * ((G + 1) + (G - 1) * c + s2), -2 * G * ((G - 1) + (G + 1) * c),
              G * ((G + 1) + (G - 1) * c - s2), (G + 1) - (G - 1) * c + s2,
              2 * ((G - 1) - (G + 1) * c), (G + 1) - (G - 1) * c - s2);
    else
        f.set(G * ((G + 1) - (G - 1) * c + s2), 2 * G * ((G - 1) - (G + 1) * c),
              G * ((G + 1) - (G - 1) * c - s2), (G + 1) + (G - 1) * c + s2,
              -2 * ((G - 1) + (G + 1) * c), (G + 1) + (G - 1) * c - s2);
}

template <typename FXConfig> struct FiveBandEq : sst::effects::core::EffectTemplateBase<FXConfig>
{
    enum params
    {
        lowFreq,
        lowGain,
        q2,
        freq2,
        gain2,
        q3,
        freq3,
        gain3,
        q4,
        freq4,
        gain4,
        highFreq,
        highGain,
        output,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-five-band-eq"};
    static constexpr const char *displayName{"Five-band EQ"};

    FiveBandEq(typename FXConfig::GlobalStorage *s, typename FXConfig::EffectStorage *e,
               typename FXConfig::ValueStorage *p)
        : sst::effects::core::EffectTemplateBase<FXConfig>(s, e, p)
    {
    }

    void initialize()
    {
        for (auto &b : bands)
            b.reset();
        last.fill(NAN);
        update();
    }
    void suspendProcessing() { initialize(); }
    int getRingoutDecay() const { return 32; }
    void onSampleRateChanged() { initialize(); }

    void processBlock(float *__restrict L, float *__restrict R)
    {
        update();
        for (int i = 0; i < FXConfig::blockSize; ++i)
        {
            float l = L[i], r = R[i];
            for (auto &b : bands)
            {
                l = b.run(0, l);
                r = b.run(1, r);
            }
            L[i] = l * gain;
            R[i] = r * gain;
        }
    }

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        auto hz = [](const char *n, float lo, float hi, float d) {
            return pmd().asFloat().withName(n).withRange(lo, hi).withDefault(d).withLinearScaleFormatting("Hz").withDecimalPlaces(1);
        };
        auto q = [](const char *n) {
            return pmd().asFloat().withName(n).withRange(0.5f, 16.f).withDefault(0.7f).withLinearScaleFormatting("").withDecimalPlaces(2);
        };
        auto db = [](const char *n, float r) {
            return pmd().asFloat().withName(n).withRange(-r, r).withDefault(0.f).withLinearScaleFormatting("dB").withDecimalPlaces(1);
        };
        switch (idx)
        {
        case lowFreq: return hz("Low Shelf Frequency", 20, 2000, 100);
        case lowGain: return db("Low Shelf Gain", 12);
        case q2: return q("Band 2 Q");
        case freq2: return hz("Band 2 Frequency", 20, 20000, 400);
        case gain2: return db("Band 2 Gain", 18);
        case q3: return q("Band 3 Q");
        case freq3: return hz("Band 3 Frequency", 20, 20000, 1000);
        case gain3: return db("Band 3 Gain", 18);
        case q4: return q("Band 4 Q");
        case freq4: return hz("Band 4 Frequency", 20, 20000, 4000);
        case gain4: return db("Band 4 Gain", 18);
        case highFreq: return hz("High Shelf Frequency", 500, 20000, 8000);
        case highGain: return db("High Shelf Gain", 12);
        case output: return db("Output Level", 12);
        }
        return pmd().withName("Unused").withType(pmd::NONE);
    }

  private:
    std::array<Biquad, 5> bands;
    std::array<float, numParamsEnum> last{};
    float gain{1};

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
        const double rate = this->sampleRate();
        shelf(bands[0], rate, last[lowFreq], last[lowGain], false);
        peaking(bands[1], rate, last[freq2], last[gain2], last[q2]);
        peaking(bands[2], rate, last[freq3], last[gain3], last[q3]);
        peaking(bands[3], rate, last[freq4], last[gain4], last[q4]);
        shelf(bands[4], rate, last[highFreq], last[highGain], true);
        gain = std::pow(10.f, last[output] / 20);
    }
};
} // namespace oscairn_fx::eq

#endif
