/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * An auto pan whose behaviour follows reference renders (plan, section 15, "Auto Pan"). Written
 * for Oscairn; follows the sst-effects effect contract. Signal flow:
 *  - a three-band EQ: first-order low and high shelves (Rotary A's law) and a peak whose Q is the
 *    width times 10^(-|gain| / 40), designed at 44.1 kHz and rate-matched elsewhere;
 *  - Mono feeds both channels the mean of the input, Stereo each channel its own input;
 *  - one LFO; each channel reads it at its own phase offset, set by the direction (L<>R and L/R:
 *    the right half a turn after the left; L>>R and L<<R: the left or the right a sixth of a turn
 *    ahead; L turn and R turn: a quarter turn ahead);
 *  - per channel an attenuation u in [0, 1]: in directions L<>R to R turn, a triangle from 0 (the
 *    LFO's peak) to 1 (its trough) times (1 + wave / 128)^2, clipped at 1, so higher waves dwell
 *    longer at silence; in direction L/R, a sine's distance below a fixed threshold times a
 *    measured steepness per wave, clipped to [0, 1];
 *  - the channel's gain is 1 - (L/R depth) u; in the turn directions its delay also grows by the
 *    F/R depth times u (a Doppler swing: quieter is further away), the other directions have none.
 * Everything is in seconds and hertz, so every sample rate behaves as 44.1 kHz does.
 */
#ifndef OSCAIRN_CLAP_EFFECTS_AUTOPAN_H
#define OSCAIRN_CLAP_EFFECTS_AUTOPAN_H

#include "PartAndMasterEq.h"
#include "RotarySpeaker.h"

#include <algorithm>
#include <cmath>

namespace oscairn_fx::rotary
{
// The auto pan type's three-band EQ: the indices of its seven parameters.
struct InsertEqParams
{
    int lowFreq, lowGain, highFreq, highGain, midFreq, midGain, midWidth;
};
template <typename Last> void insertEq(std::array<eq::Biquad, 3> &bands, const Last &l, const InsertEqParams &p)
{
    eq::firstOrderShelf(bands[0], l[p.lowFreq], l[p.lowGain], false);
    eq::peaking(bands[1], eq::designRate, l[p.midFreq], l[p.midGain], l[p.midWidth], false);
    eq::firstOrderShelf(bands[2], l[p.highFreq], l[p.highGain], true);
}

// The delay swing's line: above the largest swing at 192 kHz, plus the interpolator.
inline constexpr int panLineSize = 1024;
// Full F/R depth: the delay grows by this much (124 samples at 44.1 kHz) at full attenuation.
inline constexpr double frFullSeconds = 124 / 44100.0;
// Direction L/R: the cosine below which a channel starts to fall, and the steepness per wave.
inline constexpr double lrThreshold = 0.069;
inline constexpr double lrSteepness[29] = {0.8499, 0.8897, 0.9304, 0.9716, 1.014, 1.057, 1.101, 1.146, 1.192, 1.239,
                                           1.286,  1.335,  1.384,  1.434,  1.485, 1.537, 1.59,  1.644, 1.925, 2.074,
                                           2.552,  3.075,  3.644,  4.256,  5.13,  6.08,  7.089, 8.685, 34.13};

template <typename FXConfig> struct AutoPan : eq::SeriesEq<FXConfig, 3, 13, AutoPan<FXConfig>>
{
    enum params
    {
        lfoSpeed,
        lrDepth,
        frDepth,
        direction,
        wave,
        lowFreq,
        lowGain,
        highFreq,
        highGain,
        midFreq,
        midGain,
        midWidth,
        inputMode,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-auto-pan"};
    static constexpr const char *displayName{"Auto Pan"};
    using Base = eq::SeriesEq<FXConfig, 3, 13, AutoPan<FXConfig>>;
    using Base::Base;
    friend Base;

    void initialize()
    {
        Base::initialize();
        phase = 0;
        for (auto &c : line)
            std::fill(std::begin(c), std::end(c), 0.f);
        w = 0;
    }

    void processBlock(float *__restrict L, float *__restrict R)
    {
        Base::processBlock(L, R); // the EQ
        const double inc = lfoHz / this->sampleRate(), swing = frSeconds * this->sampleRate();
        for (int i = 0; i < FXConfig::blockSize; ++i)
        {
            const float in[2] = {mono ? 0.5f * (L[i] + R[i]) : L[i], mono ? 0.5f * (L[i] + R[i]) : R[i]};
            double out[2];
            for (int c = 0; c < 2; ++c)
            {
                double x = phase + offset[c];
                x -= std::floor(x);
                const double u = std::clamp(lr ? lrK * (lrThreshold - std::cos(2 * M_PI * x)) : triK * 2 * std::min(x, 1 - x), 0.0, 1.0);
                line[c][w] = in[c];
                const double y = swing > 0 ? cubicRead(line[c], w, 1 + swing * u) : in[c];
                out[c] = (1 - depth * u) * y;
            }
            L[i] = (float)out[0];
            R[i] = (float)out[1];
            w = (w + 1) & (panLineSize - 1);
            phase += inc;
            phase -= std::floor(phase);
        }
    }

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        auto hz = [](const char *n, float lo, float hi, float d) {
            return pmd().asFloat().withName(n).withRange(lo, hi).withDefault(d).withLinearScaleFormatting("Hz").withDecimalPlaces(2);
        };
        auto db = [](const char *n) {
            return pmd().asFloat().withName(n).withRange(-12.f, 12.f).withDefault(0.f).withLinearScaleFormatting("dB").withDecimalPlaces(1);
        };
        auto pct = [](const char *n) {
            return pmd().asFloat().withName(n).withRange(0.f, 100.f).withDefault(0.f).withLinearScaleFormatting("%").withDecimalPlaces(1);
        };
        switch (idx)
        {
        case lfoSpeed: return hz("LFO Speed", 0.f, 39.7f, 1.304f);
        case lrDepth: return pct("L/R Depth");
        case frDepth: return pct("F/R Depth");
        case direction:
            return pmd().asInt().withName("Pan Direction").withRange(0, 5).withDefault(5).withUnorderedMapFormatting(
                {{0, "L<>R"}, {1, "L>>R"}, {2, "L<<R"}, {3, "L turn"}, {4, "R turn"}, {5, "L/R"}});
        case wave: return pmd().asInt().withName("LFO Wave").withRange(0, 28).withDefault(20);
        case lowFreq: return hz("Low Shelf Frequency", 32.f, 2000.f, 250.f);
        case lowGain: return db("Low Shelf Gain");
        case highFreq: return hz("High Shelf Frequency", 500.f, 16000.f, 5000.f);
        case highGain: return db("High Shelf Gain");
        case midFreq: return hz("Mid Frequency", 100.f, 10000.f, 1600.f);
        case midGain: return db("Mid Gain");
        case midWidth:
            return pmd().asFloat().withName("Mid Width").withRange(0.1f, 12.f).withDefault(0.7f).withLinearScaleFormatting("").withDecimalPlaces(1);
        case inputMode:
            return pmd().asInt().withName("Input Mode").withRange(0, 1).withDefault(1).withUnorderedMapFormatting({{0, "Mono"}, {1, "Stereo"}});
        }
        return pmd().withName("Unused").withType(pmd::NONE);
    }

  private:
    void design(double)
    {
        auto &l = this->last;
        this->setActive(0, l[lowGain] != 0);
        this->setActive(1, l[midGain] != 0);
        this->setActive(2, l[highGain] != 0);
        insertEq(this->bands, l, {lowFreq, lowGain, highFreq, highGain, midFreq, midGain, midWidth});
        this->gain = 1;
        lfoHz = std::max(0.f, l[lfoSpeed]);
        depth = std::clamp(l[lrDepth], 0.f, 100.f) / 100;
        const int d = std::clamp((int)std::lround(l[direction]), 0, 5), wv = std::clamp((int)std::lround(l[wave]), 0, 28);
        // The channels' phase offsets in turns, by direction.
        static constexpr double offsets[6][2] = {{0, 0.5}, {1.0 / 6, 0}, {0, 1.0 / 6}, {0.25, 0}, {0, 0.25}, {0, 0.5}};
        offset[0] = offsets[d][0];
        offset[1] = offsets[d][1];
        lr = d == 5;
        lrK = lrSteepness[wv];
        triK = (1 + wv / 128.0) * (1 + wv / 128.0);
        frSeconds = d == 3 || d == 4 ? frFullSeconds * std::clamp(l[frDepth], 0.f, 100.f) / 100 : 0;
        mono = std::lround(l[inputMode]) == 0;
    }

    float line[2][panLineSize]{};
    int w{0};
    double phase{0}, lfoHz{0}, depth{0}, offset[2]{}, lrK{0}, triK{1}, frSeconds{0};
    bool lr{false}, mono{false};
};
} // namespace oscairn_fx::rotary

#endif
