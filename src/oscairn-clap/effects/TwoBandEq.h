/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * A two-band EQ: two sections, each off, a first-order low- or high-pass, a low or high shelf or
 * a peak, then an output level. Written for Oscairn; follows the sst-effects effect contract.
 * Laws fitted to reference renders at 44.1 kHz (plan, section 15, M1): first-order bilinear
 * passes at the set frequency; shelves as the five-band EQ's (slope 0.5, the Q ignored); peaks
 * with an effective Q of Q * 10^(-|gain| / 40) and no widening at high frequencies.
 */
#ifndef OSCAIRN_CLAP_EFFECTS_TWOBANDEQ_H
#define OSCAIRN_CLAP_EFFECTS_TWOBANDEQ_H

#include "FiveBandEq.h"

namespace oscairn_fx::eq
{
template <typename FXConfig> struct TwoBandEq : sst::effects::core::EffectTemplateBase<FXConfig>
{
    enum params
    {
        type1,
        freq1,
        gain1,
        q1,
        type2,
        freq2,
        gain2,
        q2,
        output,
        numParamsEnum
    };
    enum Type
    {
        off,
        lowPass,
        highPass,
        lowShelf,
        highShelf,
        peak
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-two-band-eq"};
    static constexpr const char *displayName{"Two-band EQ"};

    TwoBandEq(typename FXConfig::GlobalStorage *s, typename FXConfig::EffectStorage *e,
              typename FXConfig::ValueStorage *p)
        : sst::effects::core::EffectTemplateBase<FXConfig>(s, e, p)
    {
    }

    void initialize()
    {
        for (auto &b : bands)
            b.reset();
        cascade.reset();
        cascade.n = 0;
        last.fill(NAN);
        update();
    }
    void suspendProcessing() { initialize(); }
    int getRingoutDecay() const { return 32; }
    void onSampleRateChanged() { initialize(); }

    void processBlock(float *__restrict L, float *__restrict R)
    {
        update();
        if (matched)
        {
            for (int i = 0; i < FXConfig::blockSize; ++i)
            {
                L[i] = cascade.run(0, L[i]) * gain;
                R[i] = cascade.run(1, R[i]) * gain;
            }
            return;
        }
        for (int i = 0; i < FXConfig::blockSize; ++i)
        {
            float l = L[i], r = R[i];
            for (int b = 0; b < 2; ++b)
                if (active[b])
                {
                    l = bands[b].run(0, l);
                    r = bands[b].run(1, r);
                }
            L[i] = l * gain;
            R[i] = r * gain;
        }
    }

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        auto type = [](const char *n) {
            return pmd().asInt().withName(n).withRange(0, 5).withDefault(0).withUnorderedMapFormatting(
                {{off, "Off"}, {lowPass, "Low-pass 6 dB"}, {highPass, "High-pass 6 dB"}, {lowShelf, "Low Shelf"},
                 {highShelf, "High Shelf"}, {peak, "Peak"}});
        };
        auto hz = [](const char *n) {
            return pmd().asFloat().withName(n).withRange(20, 20000).withDefault(63).withLinearScaleFormatting("Hz").withDecimalPlaces(1);
        };
        auto db = [](const char *n) {
            return pmd().asFloat().withName(n).withRange(-12, 12).withDefault(0).withLinearScaleFormatting("dB").withDecimalPlaces(1);
        };
        auto q = [](const char *n) {
            return pmd().asFloat().withName(n).withRange(0.1f, 12.f).withDefault(0.1f).withLinearScaleFormatting("").withDecimalPlaces(2);
        };
        switch (idx)
        {
        case type1: return type("Band 1 Type");
        case freq1: return hz("Band 1 Frequency");
        case gain1: return db("Band 1 Gain");
        case q1: return q("Band 1 Q");
        case type2: return type("Band 2 Type");
        case freq2: return hz("Band 2 Frequency");
        case gain2: return db("Band 2 Gain");
        case q2: return q("Band 2 Q");
        case output: return db("Output Level");
        }
        return pmd().withName("Unused").withType(pmd::NONE);
    }

  private:
    std::array<Biquad, 2> bands;
    std::array<bool, 2> active{};
    std::array<float, numParamsEnum> last{};
    float gain{1};
    bool matched{false};          // at another rate than 44.1 kHz the cascade runs instead
    MatchedCascade<2> cascade;

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
        const double rate = designRate; // designed at 44.1 kHz; matched below at other rates
        for (int b = 0; b < 2; ++b)
        {
            const int t = (int)std::lround(last[b * 4 + type1]);
            const double f = last[b * 4 + freq1], g = last[b * 4 + gain1], q = last[b * 4 + q1];
            const bool was = active[b];
            active[b] = t != off;
            if (active[b] && !was)
                bands[b].reset();
            switch (t)
            {
            case lowPass: lowpass1(bands[b], rate, f); break;
            case highPass: highpass1(bands[b], rate, f); break;
            case lowShelf: shelf(bands[b], rate, f, g, false); break;
            case highShelf: shelf(bands[b], rate, f, g, true); break;
            case peak: peaking(bands[b], rate, f, g, q, false); break;
            default: break;
            }
        }
        gain = std::pow(10.f, last[output] / 20);
        matched = this->sampleRate() != designRate;
        if (matched)
        {
            const Biquad *design[2];
            int count = 0;
            for (int b = 0; b < 2; ++b)
                if (active[b])
                    design[count++] = &bands[b];
            cascade.set(design, count, this->sampleRate());
        }
    }
};
} // namespace oscairn_fx::eq

#endif
