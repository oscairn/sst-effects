/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * A peak compressor whose behaviour follows reference renders (plan, section 15, "Compressor
 * A"). Written for Oscairn; follows the sst-effects effect contract. Signal flow:
 *  - the input scaled by the insertion level (the effect runs 7.91 dB down, the output is raised
 *    again), clipped to [-1, 1);
 *  - per channel a first-order 20 Hz high-pass (its words held to 1/16384), clipped;
 *  - the detector: |L + R| x 1.4140625, clipped, through a peak follower that falls at the
 *    release rate, then a second that follows the first at the same rate (both rise at once);
 *  - the follower's level x the threshold's scale (a 16-bit word, 10^((-54 - T) / 20)), bent by
 *    v - v^2 / 2, x 1023 indexes a 513-point gain table, read with linear interpolation: the table
 *    holds (level / 10^(-54 / 20))^(1 / ratio - 1), held below 1 and to steps of 1/32768, at the
 *    level whose bend gives the index / 1024;
 *  - the gain smoothed by a bilinear one-pole at the attack time (16-bit words), applied a sample
 *    late to both channels, times 4, clipped; the output level (16-bit word, (v / 127)^2 / 4)
 *    times 4, clipped.
 * Words are designed at 44.1 kHz; at other rates the filters' and followers' poles keep their time
 * constants and the words their DC gains.
 */
#ifndef OSCAIRN_CLAP_EFFECTS_COMPRESSORA_H
#define OSCAIRN_CLAP_EFFECTS_COMPRESSORA_H

#include "sst/effects/EffectCore.h"
#include "sst/basic-blocks/params/ParamMetadata.h"

#include <algorithm>
#include <cmath>

namespace oscairn_fx::dynamics
{
template <typename FXConfig> struct CompressorA : sst::effects::core::EffectTemplateBase<FXConfig>
{
    enum params
    {
        attack,
        release,
        threshold,
        ratio,
        outputLevel,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-compressor-a"};
    static constexpr const char *displayName{"Compressor A"};
    static constexpr int N = FXConfig::blockSize;
    static constexpr int tableSize = 513;
    // The insertion's level: fitted on reference renders within 3e-7 (residual minimum).
    static constexpr double insertionLevel = 0.4020535;
    // The release word's exponent: every value from 2.1386 to 2.1447 gives the reference's words.
    static constexpr double releaseK = 2.1417;

    CompressorA(typename FXConfig::GlobalStorage *s, typename FXConfig::EffectStorage *e,
                typename FXConfig::ValueStorage *p)
        : sst::effects::core::EffectTemplateBase<FXConfig>(s, e, p)
    {
    }

    void initialize()
    {
        for (auto &c : ch)
            c = Channel{};
        e1 = e2 = 0;
        std::fill(std::begin(last), std::end(last), NAN);
        tableRatio = NAN;
        update();
        // settled on silence, as the reference is long before a note
        out = outTarget, thr = thrTarget;
        u1 = table[0];
        g = 2 * k * u1 / (1 - b);
    }
    void suspendProcessing() { initialize(); }
    int getRingoutDecay() const { return -1; }
    void onSampleRateChanged() { initialize(); }

    void processBlock(float *__restrict L, float *__restrict R)
    {
        update();
        // the output and threshold words glide 2^-9 of the way per 44.1 kHz sample (per block here)
        auto glide = [&](double &v, double t) { v = std::abs(t - v) <= 1e-8 ? t : v + glideK * (t - v); };
        glide(out, outTarget);
        glide(thr, thrTarget);
        bool silent = true;
        for (int i = 0; i < N; ++i)
            silent = silent && L[i] == 0.f && R[i] == 0.f;
        double ee1 = e1, ee2 = e2, gg = g, uu1 = u1;
        if (silent && ch[0].zero() && ch[1].zero())
        {
            // Nothing reaches the output: only the detector and the gain move on, until they rest
            // (the followers below -240 dB end at zero, the gain at its fixed point).
            if (!rest)
            {
                double before = gg;
                for (int i = 0; i < N; ++i)
                {
                    ee1 = d * ee1;
                    ee2 = (1 - d) * ee1 + d * ee2;
                    const double u = gainAt(ee2);
                    before = gg;
                    gg = k * uu1 + b * gg + k * u;
                    uu1 = u;
                }
                if (ee1 < 1e-12 && ee2 < 1e-12)
                    ee1 = ee2 = 0;
                rest = ee1 == 0 && ee2 == 0 && uu1 == table[0] && gg == before;
            }
            std::fill(L, L + N, 0.f), std::fill(R, R + N, 0.f);
        }
        else
        {
            Channel a = ch[0], c = ch[1];
            const double o = 4 * out, up = 1 / insertionLevel;
            for (int i = 0; i < N; ++i)
            {
                const double l = a.step(clip((float)(L[i] * (float)insertionLevel)), hp);
                const double r = c.step(clip((float)(R[i] * (float)insertionLevel)), hp);
                // the gain a sample late
                L[i] = (float)(clip(o * clip(4 * (gg * l))) * up);
                R[i] = (float)(clip(o * clip(4 * (gg * r))) * up);
                const double rect = std::min(std::abs(16 * (link * l + link * r)), top);
                ee1 = std::max(rect, d * ee1);
                ee2 = std::max(rect, (1 - d) * ee1 + d * ee2);
                const double u = gainAt(ee2);
                gg = k * uu1 + b * gg + k * u;
                uu1 = u;
            }
            a.settle(), c.settle();
            ch[0] = a, ch[1] = c;
            rest = false;
        }
        e1 = ee1, e2 = ee2, g = gg, u1 = uu1;
    }

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        switch (idx)
        {
        case attack: return pmd().asFloat().withName("Attack").withRange(1.f, 40.f).withDefault(8.f).withLinearScaleFormatting("ms").withDecimalPlaces(1);
        case release: return pmd().asFloat().withName("Release").withRange(10.f, 680.f).withDefault(85.f).withLinearScaleFormatting("ms").withDecimalPlaces(1);
        case threshold: return pmd().asFloat().withName("Threshold").withRange(-48.f, -6.f).withDefault(-30.f).withLinearScaleFormatting("dB").withDecimalPlaces(1);
        case ratio: return pmd().asFloat().withName("Ratio").withRange(1.f, 20.f).withDefault(3.f).withLinearScaleFormatting("").withDecimalPlaces(1);
        case outputLevel: return pmd().asFloat().withName("Output Level").withRange(-90.f, 0.f).withDefault(-8.f).withLinearScaleFormatting("dB").withDecimalPlaces(1);
        }
        return pmd().withName("Unused").withType(pmd::NONE);
    }

  private:
    static constexpr double top = 1 - 1.0 / (1 << 24); // the largest float below 1
    static constexpr double link = 2896 / 32768.0;      // each channel's share of the detector
    static double clip(double v) { return std::min(std::max(v, -1.0), top); }
    struct HighPass
    {
        double b0{0}, b1{0}, a1{0};
    };
    struct Channel
    {
        double x1{0}, y1{0};
        double step(double x, const HighPass &h)
        {
            const double y = clip(2 * (x1 * h.b1 + h.a1 * y1 + h.b0 * x));
            x1 = x, y1 = y;
            return y;
        }
        // the decay below any level that reaches the output ends at exact zero
        void settle()
        {
            if (std::abs(x1) < 1e-30 && std::abs(y1) < 1e-15)
                x1 = y1 = 0;
        }
        bool zero() const { return x1 == 0 && y1 == 0; }
    };
    Channel ch[2];
    HighPass hp;
    float table[tableSize + 1]{};
    double e1{0}, e2{0}, g{0}, u1{0};
    bool rest{false};
    double d{0}, k{0}, b{0}, out{0}, outTarget{0}, thr{0}, thrTarget{0}, glideK{0};
    float last[numParamsEnum]{}, tableRatio{NAN};

    double gainAt(double env) const
    {
        const double v = thr * env;
        const double idx = (v - 0.5 * v * v) * 1023;
        const int i = (int)idx;
        const double f = idx - i;
        return table[i] + f * ((double)table[i + 1] - table[i]);
    }
    // A 44.1 kHz pole kept as the same time constant at the running rate.
    double atRate(double p44) const { return std::pow(p44, 44100.0 / this->sampleRate()); }

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
        constexpr double W = 32768;
        // first-order 20 Hz high-pass, fixed words (b0, b1, a1 / 2)
        const double p44 = 2 * 16337 / W, p = atRate(p44);
        hp = {16361 / W * (1 + p) / (1 + p44), -16361 / W * (1 + p) / (1 + p44), p / 2};
        // attack: bilinear one-pole at 44.1 kHz in 16-bit words, b rounded down, k to nearest
        const double wc = 1 / (2 * 44100 * 1e-3 * last[attack]);
        const double k44 = std::round(W * wc / (1 + wc)) / W, b44 = std::floor(W * (1 - wc) / (1 + wc)) / W;
        b = atRate(b44);
        k = k44 * (1 - b) / (1 - b44);
        // release: both followers' decay, a 16-bit word
        d = atRate(std::round(W * std::exp(-releaseK / (44.1 * last[release]))) / W);
        thrTarget = std::floor(W * std::pow(10.0, (-54.0 - last[threshold]) / 20)) / W;
        outTarget = last[outputLevel] < -89.5f ? 0.0 : std::floor(8192 * std::pow(10.0, last[outputLevel] / 20.0)) / W;
        glideK = 1 - std::pow(1 - 1.0 / 512, N * 44100.0 / this->sampleRate());
        if (!(last[ratio] == tableRatio))
        {
            tableRatio = last[ratio];
            const double e = 1 / std::max(1.0, (double)tableRatio) - 1, l0 = std::pow(10.0, -54.0 / 20);
            for (int i = 0; i < tableSize - 1; ++i)
            {
                const double lv = 1 - std::sqrt(1 - i / 512.0);
                table[i] = lv <= 0 ? 32767 / W : (float)(std::min(32767.0, std::floor(W * std::pow(lv / l0, e))) / W);
            }
            table[tableSize - 1] = table[tableSize] = 0; // the table's end
        }
        rest = false;
    }
};
} // namespace oscairn_fx::dynamics

#endif
