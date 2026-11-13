/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * A stereo tremolo whose behaviour follows reference renders (plan, section 15, "Tremolo").
 * Written for Oscairn; follows the sst-effects effect contract. Signal flow:
 *  - the insertion types' three-band EQ (AutoPan.h), then a fixed gain of +2 dB;
 *  - Mono feeds both channels the mean of the input, Stereo each channel its own input;
 *  - one sine LFO; the left channel reads it at its phase, the right at its phase plus the phase
 *    difference;
 *  - per channel a gain of 1 - (AM depth) (1 - cos) / 2 (unity at the LFO's peak) and a delay that
 *    swings by the PM depth around its centre, shortest at the LFO's peak (a vibrato).
 * Everything is in seconds and hertz, so every sample rate behaves as 44.1 kHz does.
 */
#ifndef OSCAIRN_CLAP_EFFECTS_TREMOLO_H
#define OSCAIRN_CLAP_EFFECTS_TREMOLO_H

#include "AutoPan.h"

namespace oscairn_fx::rotary
{
// Full PM depth: the delay's peak swing (62 samples at 44.1 kHz).
inline constexpr double pmFullSeconds = 62 / 44100.0;
// The level with no modulation, +2 dB.
inline constexpr double tremoloGain = 1.2589254117941673;

template <typename FXConfig> struct Tremolo : eq::SeriesEq<FXConfig, 3, 12, Tremolo<FXConfig>>
{
    enum params
    {
        lfoSpeed,
        amDepth,
        pmDepth,
        lowFreq,
        lowGain,
        highFreq,
        highGain,
        midFreq,
        midGain,
        midWidth,
        phaseDiff,
        inputMode,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-tremolo"};
    static constexpr const char *displayName{"Tremolo"};
    using Base = eq::SeriesEq<FXConfig, 3, 12, Tremolo<FXConfig>>;
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
        Base::processBlock(L, R); // the EQ and the fixed gain
        const double inc = lfoHz / this->sampleRate(), swing = pmSeconds * this->sampleRate();
        for (int i = 0; i < FXConfig::blockSize; ++i)
        {
            const float in[2] = {mono ? 0.5f * (L[i] + R[i]) : L[i], mono ? 0.5f * (L[i] + R[i]) : R[i]};
            double out[2];
            for (int c = 0; c < 2; ++c)
            {
                const double co = std::cos(2 * M_PI * (phase + (c ? diff : 0)));
                line[c][w] = in[c];
                const double y = swing > 0 ? cubicRead(line[c], w, 1 + swing * (1 - co)) : in[c];
                out[c] = (1 - am * (1 - co) / 2) * y;
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
        case lfoSpeed: return hz("LFO Speed", 0.f, 39.7f, 4.88f);
        case amDepth: return pct("AM Depth");
        case pmDepth: return pct("PM Depth");
        case lowFreq: return hz("Low Shelf Frequency", 32.f, 2000.f, 250.f);
        case lowGain: return db("Low Shelf Gain");
        case highFreq: return hz("High Shelf Frequency", 500.f, 16000.f, 5000.f);
        case highGain: return db("High Shelf Gain");
        case midFreq: return hz("Mid Frequency", 100.f, 10000.f, 1600.f);
        case midGain: return db("Mid Gain");
        case midWidth:
            return pmd().asFloat().withName("Mid Width").withRange(0.1f, 12.f).withDefault(0.7f).withLinearScaleFormatting("").withDecimalPlaces(1);
        case phaseDiff:
            return pmd().asFloat().withName("LFO Phase Difference").withRange(-180.f, 180.f).withDefault(0.f).withLinearScaleFormatting("deg").withDecimalPlaces(0);
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
        this->gain = (float)tremoloGain;
        lfoHz = std::max(0.f, l[lfoSpeed]);
        am = std::clamp(l[amDepth], 0.f, 100.f) / 100;
        pmSeconds = pmFullSeconds * std::clamp(l[pmDepth], 0.f, 100.f) / 100;
        diff = std::clamp(l[phaseDiff], -180.f, 180.f) / 360.0;
        mono = std::lround(l[inputMode]) == 0;
    }

    float line[2][panLineSize]{};
    int w{0};
    double phase{0}, lfoHz{0}, am{0}, pmSeconds{0}, diff{0};
    bool mono{false};
};
} // namespace oscairn_fx::rotary

#endif
