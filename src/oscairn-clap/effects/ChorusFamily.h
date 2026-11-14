/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * The chorus family on one four-tap modulation network (plan, section 15, "Chorus family"). Written
 * for Oscairn; follows the sst-effects effect contract. Signal flow, in each mode:
 *  - the insertion types' three-band EQ on the input (AutoPan.h), dry and wet alike;
 *  - two delay lines, left and right (in Mono the left one alone, fed the mean of the input);
 *    each line's input is clipped to [-1, 1) and held in a delay-line word (lineWord);
 *  - four taps B, A, C, D (B and C on the left line, A and D on the right one, or all four on the
 *    left in Mono), each read with linear interpolation at a delay of its base plus 2048 samples
 *    times its position, the position being its depth times its LFO (a 24-bit phase in the
 *    platform's 16-bit waveforms), plus in one mode a triangle folded from a fast saw;
 *  - per tap an AM gain 1 + depth x LFO, held to 1/32768;
 *  - feedback from taps into the lines, and the outputs: the dry input and the clipped sum of the
 *    mode's taps, each at its balance word, clipped to [-1, 1).
 * Every delay and LFO speed is held in samples and steps at 44.1 kHz and scaled to the running
 * rate, so every rate behaves as 44.1 kHz does (the delays read between samples there).
 */
#ifndef OSCAIRN_CLAP_EFFECTS_CHORUSFAMILY_H
#define OSCAIRN_CLAP_EFFECTS_CHORUSFAMILY_H

#include "AutoPan.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace oscairn_fx::chorus
{
inline constexpr double word = 1.0 / 32768, top = 32767.0 / 32768;
inline double clip(double x) { return x > top ? top : (x < -1 ? -1 : x); }
// A product held to 1/32768, as the network's multipliers keep it.
inline double held(double x) { return std::trunc(x * 32768 - (x < 0 ? 1 : 0)) * word; }

// The quarter sine the LFOs interpolate: floor(65535 sin(k pi / 64)), k = 0-32.
struct QuarterSine
{
    int64_t v[33];
    QuarterSine()
    {
        for (int k = 0; k <= 32; ++k)
            v[k] = (int64_t)std::floor(65535 * std::sin(k * M_PI / 64)) << 15;
    }
};
inline const QuarterSine quarterSine{};

// An LFO: a 24-bit phase advanced by its step each sample (before it is read), its waveform a
// 16-bit word, read as [0, 1) (unipolar) or [-1, 1).
struct Lfo
{
    enum Wave
    {
        triangle,
        sine,
        saw
    };
    Wave wave{triangle};
    bool bipolar{false};
    double phase{0}, step{0}; // in 2^-24 turns; step per sample at the running rate
    // The phase codes 0-15: multiples of 45 degrees and, for odd codes, 30 or 15 degrees more.
    static double degrees(int code) { return (code >> 1) * 45 + (code & 1 ? (code & 2 ? 15 : 30) : 0); }
    void start(int code) { phase = std::floor(degrees(code) / 360 * 16777216); }
    static int word16(Wave w, uint32_t p)
    {
        switch (w)
        {
        case triangle:
        {
            uint32_t m = p & 0x3fffff;
            const uint32_t q = (p >> 22) & 3;
            if (q == 1 || q == 2)
                m ^= 0x3fffff;
            const int v = (p & 0x800000) ? (int)m - 0x400000 : (int)m;
            return v >> 7;
        }
        case sine:
        {
            uint32_t m = p & 0x3fffff;
            if (p & 0x400000)
                m ^= 0x3fffff;
            const int i = (int)(m >> 17), f = (int)((m >> 9) & 0xff);
            int64_t v = (quarterSine.v[i + 1] - quarterSine.v[i]) * f + (quarterSine.v[i] << 8);
            if (p & 0x800000)
                v = 0xffffffffffLL & ~v;
            return (int16_t)(((uint64_t)v >> 24) & 0xffff);
        }
        case saw:
        {
            const int32_t s = (int32_t)(((p + 0x800000u) & 0xffffffu) << 8) >> 8;
            return s >> 8;
        }
        }
        return 0;
    }
    void skip(int n)
    {
        phase += step * n;
        phase -= 16777216 * std::floor(phase / 16777216);
    }
    double next()
    {
        phase += step;
        phase -= 16777216 * std::floor(phase / 16777216);
        const int v = word16(wave, (uint32_t)phase);
        return bipolar ? v * word : ((v + 32768) & 0xffff) / 65536.0;
    }
};

// The platform's dry/wet curve: round-half-down of 32767 sin(pi (v - 1) / 252), 32767 from 127.
inline int balanceWord(int v)
{
    if (v < 1)
        return 0;
    if (v >= 127)
        return 32767;
    return (int)std::ceil(32767 * std::sin(M_PI * (v - 1) / 252) - 0.5);
}
// A balance word at a quarter of full scale, times a level in dB held as the platform's float.
inline double balanceGain(int v, double db)
{
    const float g = (float)std::pow(10.0, db / 20);
    return std::trunc((float)(balanceWord(v) >> 2) * g) * word;
}

// The family's delay time: tenths of a ms to samples at 44.1 kHz, floor(t x 18063 / 4096).
inline int tenthsToSamples(double ms) { return (int)std::floor(std::lround(ms * 10) * 18063.0 / 4096); }

// One tap: its line, the age of its nearest sample at 44.1 kHz, LFO, depth, AM and folded triangle.
struct Tap
{
    bool right{false};
    int age{1};
    int lfo{0};
    bool clamped{false}; // taps B and A read their LFO clipped below 1
    double pm{0}, am{0}, triGain{0}, triPhase{0};
};

// The network's settings, all in the platform's words (x 1/32768).
struct Settings
{
    Lfo::Wave wave[4]{};
    int phaseCode[4]{};
    int step{0};          // the slow LFOs' step at 44.1 kHz
    int fastStepMul{0};   // the fast saw's step, in slow steps (0: off)
    int fastPhaseCode{0}; // its phase
    Tap tap[4];           // B, A, C, D
    double inLL{top}, inLR{0}, inRR{top};
    double fbLB{0}, fbLC{0}, fbRA{0}, fbRD{0};
    double outB{0}, outC0{0}, outA{0}, outD{0}, outC1{0};
    double wet{0}, dry{0};
};

// Coefficient glides after a change: depths move by at most 2^-14 x 2^(2 - e) of the gap per
// 44.1 kHz sample (e its binary exponent, at most a quarter of it), the mix words by 2^-9.
struct Glide
{
    double now{NAN};
    void depth(double target, double per)
    {
        if (std::isnan(now))
            now = target;
        const double d = target - now;
        if (std::abs(d) <= 1e-8)
            now = target;
        else
        {
            int e;
            std::frexp(d, &e);
            now += std::min(0.25, 0x1p-14 * std::ldexp(1.0, 2 - e) * per) * d;
        }
    }
    void mix(double target, double per)
    {
        if (std::isnan(now))
            now = target;
        const double d = target - now;
        now = std::abs(d) <= 1e-8 ? target : now + std::min(1.0, 0x1p-9 * per) * d;
    }
};

template <typename FXConfig, int P, typename Derived> struct Network : eq::SeriesEq<FXConfig, 3, P, Derived>
{
    using Base = eq::SeriesEq<FXConfig, 3, P, Derived>;
    Network(typename FXConfig::GlobalStorage *s, typename FXConfig::EffectStorage *e, typename FXConfig::ValueStorage *p)
        : Base(s, e, p), lineL(bufferSamples(this->sampleRate())), lineR(bufferSamples(this->sampleRate()))
    {
    }

    // The longest age any mode reaches (50 ms plus the widest swing and spread) at 44.1 kHz.
    static constexpr int longest = 3600;
    static int bufferSamples(double rate)
    {
        int n = 1;
        while (n < (int)std::ceil(longest * rate / rotary::designRate) + 4)
            n <<= 1;
        return n;
    }
    static size_t heapBytesAt(double rate) { return 2 * sizeof(float) * (size_t)bufferSamples(rate) + Base::heapBytesAt(rate); }
    size_t heapBytes() const { return (lineL.size() + lineR.size()) * sizeof(float) + Base::heapBytes(); }
    int getRingoutDecay() const { return -1; }
    double quietSeconds() const { return longest / rotary::designRate + 0.1; }

    void initialize()
    {
        std::fill(lineL.begin(), lineL.end(), 0.f);
        std::fill(lineR.begin(), lineR.end(), 0.f);
        w = 0;
        zeroRun = 0;
        for (auto &g : glides)
            g.now = NAN;
        started = false;
        Base::initialize();
    }

    void processBlock(float *__restrict L, float *__restrict R)
    {
        Base::processBlock(L, R); // the EQ, and the settings when a parameter changed
        const Settings &s = set;
        const double rate = this->sampleRate(), per = rotary::designRate / rate;
        if (!started)
        {
            // The LFOs start with the effect, each at its phase.
            for (int k = 0; k < 4; ++k)
                lfo[k].start(s.phaseCode[k]);
            fast.start(s.fastPhaseCode);
            started = true;
        }
        for (int k = 0; k < 4; ++k)
            lfo[k].wave = s.wave[k], lfo[k].bipolar = false, lfo[k].step = s.step * per;
        fast.wave = Lfo::saw, fast.bipolar = true, fast.step = s.step * s.fastStepMul * per;
        const int mask = (int)lineL.size() - 1;
        // Silence in, the lines empty and the glides settled: the block is silence out. Only the
        // LFOs and the write position move on, as they would sample by sample.
        if (zeroRun > mask && settled() && quiet(L, R))
        {
            for (auto *o : {&lfo[0], &lfo[1], &lfo[2], &lfo[3], &fast})
                o->skip(FXConfig::blockSize);
            w = (w - FXConfig::blockSize) & mask;
            return;
        }
        for (int i = 0; i < FXConfig::blockSize; ++i)
        {
            w = (w - 1) & mask;
            double l[4];
            for (int k = 0; k < 4; ++k)
                l[k] = lfo[k].next();
            const double f = fast.next();
            double x[4];
            for (int t = 0; t < 4; ++t)
            {
                const Tap &tp = s.tap[t];
                Glide &gp = glides[2 * t], &ga = glides[2 * t + 1];
                gp.depth(tp.pm, per);
                ga.depth(tp.am, per);
                const double m = tp.clamped ? std::min(l[tp.lfo], top) : l[tp.lfo];
                double pos = gp.now * m;
                if (tp.triGain != 0)
                {
                    double u = tp.triPhase + f + 1;
                    u -= 2 * std::floor(u / 2);
                    pos += std::min(top, std::abs(tp.triGain * (u - 1)));
                }
                // The nearest sample's age and the fraction past it, at the running rate.
                const double age = (tp.age + 2048 * pos) * rate / rotary::designRate;
                const int n = (int)age;
                const auto &line = tp.right ? lineR : lineL;
                const double a = line[(w + n) & mask], b = line[(w + n + 1) & mask];
                const double y = a + (age - n) * (b - a);
                x[t] = y + held(ga.now * m) * y;
            }
            const double eL = L[i], eR = R[i];
            glides[8].mix(s.wet, per);
            glides[9].mix(s.dry, per);
            glides[10].mix(s.fbLB, per);
            glides[11].mix(s.fbLC, per);
            glides[12].mix(s.fbRA, per);
            glides[13].mix(s.fbRD, per);
            const double wet = glides[8].now, dry = glides[9].now;
            lineL[w] = store(glides[10].now * x[0] + glides[11].now * x[2] + s.inLL * eL + s.inLR * eR);
            lineR[w] = store(glides[12].now * x[1] + glides[13].now * x[3] + s.inRR * eR);
            const double sum0 = clip(s.outB * x[0] + s.outC0 * x[2]), sum1 = clip(s.outA * x[1] + s.outD * x[3] + s.outC1 * x[2]);
            L[i] = (float)clip(4 * (wet * sum0 + dry * eL));
            R[i] = (float)clip(4 * (wet * sum1 + dry * eR));
            zeroRun = lineL[w] == 0 && lineR[w] == 0 && L[i] == 0 && R[i] == 0 ? zeroRun + 1 : 0;
        }
    }

  protected:
    Settings set;
    // The common parts of a mode's settings: speed, EQ, balance (dry/wet at v = mix + 64 and the
    // wet level in dB), and the line inputs and tap lines for Mono or Stereo.
    void common(double hz, double mix, double wetDb, bool mono, const rotary::InsertEqParams &eqp)
    {
        auto &l = this->last;
        this->setActive(0, l[eqp.lowGain] != 0);
        this->setActive(1, l[eqp.midGain] != 0);
        this->setActive(2, l[eqp.highGain] != 0);
        rotary::insertEq(this->bands, l, eqp);
        set.step = (int)std::lround(std::max(0.0, hz) * 16777216 / rotary::designRate);
        const int v = std::clamp((int)std::lround(mix) + 64, 1, 127);
        set.wet = balanceGain(v, wetDb);
        set.dry = balanceGain(128 - v, 0);
        set.inLL = mono ? 0.5 : top;
        set.inLR = mono ? 0.5 : 0;
        set.inRR = top;
        set.tap[1].right = set.tap[3].right = !mono;
    }

  private:
    // A line's input, clipped and held in a delay-line word; below 2^-32 (-193 dB) it is zero, so a
    // line fed silence empties in seconds instead of decaying through denormal floats.
    static float store(double x) { return std::abs(x) < 0x1p-32 ? 0.f : rotary::lineWord(clip(x)); }
    bool settled() const
    {
        const Settings &s = set;
        const double target[14] = {s.tap[0].pm, s.tap[0].am, s.tap[1].pm, s.tap[1].am, s.tap[2].pm, s.tap[2].am, s.tap[3].pm,
                                   s.tap[3].am, s.wet, s.dry, s.fbLB, s.fbLC, s.fbRA, s.fbRD};
        for (int k = 0; k < 14; ++k)
            if (!(glides[k].now == target[k]))
                return false;
        return true;
    }
    static bool quiet(const float *L, const float *R)
    {
        for (int i = 0; i < FXConfig::blockSize; ++i)
            if (L[i] != 0 || R[i] != 0)
                return false;
        return true;
    }
    std::vector<float> lineL, lineR;
    int w{0}, zeroRun{0};
    Lfo lfo[4], fast;
    Glide glides[14];
    bool started{false};
};

// The insertion EQ's seven parameters, as the tremolo modes show them.
inline sst::basic_blocks::params::ParamMetaData eqParam(int k)
{
    using pmd = sst::basic_blocks::params::ParamMetaData;
    auto hz = [](const char *n, float lo, float hi, float d) {
        return pmd().asFloat().withName(n).withRange(lo, hi).withDefault(d).withLinearScaleFormatting("Hz").withDecimalPlaces(2);
    };
    auto db = [](const char *n) {
        return pmd().asFloat().withName(n).withRange(-12.f, 12.f).withDefault(0.f).withLinearScaleFormatting("dB").withDecimalPlaces(1);
    };
    switch (k)
    {
    case 0: return hz("Low Shelf Frequency", 32.f, 2000.f, 250.f);
    case 1: return db("Low Shelf Gain");
    case 2: return hz("High Shelf Frequency", 500.f, 16000.f, 5000.f);
    case 3: return db("High Shelf Gain");
    case 4: return hz("Mid Frequency", 100.f, 10000.f, 1600.f);
    case 5: return db("Mid Gain");
    case 6: return pmd().asFloat().withName("Mid Width").withRange(0.1f, 12.f).withDefault(0.7f).withLinearScaleFormatting("").withDecimalPlaces(1);
    }
    return pmd().withName("Unused").withType(pmd::NONE);
}
inline sst::basic_blocks::params::ParamMetaData speedParam(float dflt)
{
    using pmd = sst::basic_blocks::params::ParamMetaData;
    return pmd().asFloat().withName("LFO Speed").withRange(0.f, 39.71f).withDefault(dflt).withLinearScaleFormatting("Hz").withDecimalPlaces(2);
}
inline sst::basic_blocks::params::ParamMetaData intParam(const char *name, int lo, int hi, int dflt)
{
    using pmd = sst::basic_blocks::params::ParamMetaData;
    return pmd().asInt().withName(name).withRange(lo, hi).withDefault(dflt);
}
inline sst::basic_blocks::params::ParamMetaData offsetParam(float dflt)
{
    using pmd = sst::basic_blocks::params::ParamMetaData;
    return pmd().asFloat().withName("Delay Offset").withRange(0.f, 50.f).withDefault(dflt).withLinearScaleFormatting("ms").withDecimalPlaces(1);
}
inline sst::basic_blocks::params::ParamMetaData inputParam(int dflt)
{
    using pmd = sst::basic_blocks::params::ParamMetaData;
    return pmd().asInt().withName("Input Mode").withRange(0, 1).withDefault(dflt).withUnorderedMapFormatting({{0, "Mono"}, {1, "Stereo"}});
}
// The family's speed step, 2^-24 turns per 44.1 kHz sample, as the converter's speeds give it.
inline double speedHz(int step) { return step * rotary::designRate / 16777216; }

// The depth word floor(8192 v / 127) the platform's LFO depths use.
inline int depthWord(int v) { return (int)std::floor(8192.0 * v / 127); }

#define OSCAIRN_CHORUS_COMMON_PARAMS lowFreq, lowGain, highFreq, highGain, midFreq, midGain, midWidth, mix

// Chorus A: four taps, triangle LFOs on B and A (120 and 240 degrees), sine LFOs on C and D (300 and
// 60), each line fed back from its two taps, C and D centred on B and A's swing.
template <typename FXConfig> struct ChorusA : Network<FXConfig, 13, ChorusA<FXConfig>>
{
    enum params
    {
        lfoSpeed,
        pmDepth,
        feedback,
        delayOffset,
        OSCAIRN_CHORUS_COMMON_PARAMS,
        inputMode,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-chorus-a"};
    static constexpr const char *displayName{"Chorus A"};
    using Base = Network<FXConfig, 13, ChorusA<FXConfig>>;
    using Base::Base;
    friend typename Base::Base;

    // The swing of taps B and A in samples at 44.1 kHz per depth step: fitted to the platform's
    // law (floor(8.0637 v + 0.05), within one sample of it at every v); C and D's is exact.
    static int swingBA(int v) { return (int)std::floor(8.0637 * v + 0.05); }
    static int swingCD(int v) { return (int)std::floor(5.17473 * v); }

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        switch (idx)
        {
        case lfoSpeed: return speedParam((float)speedHz(80));
        case pmDepth: return intParam("PM Depth", 0, 127, 54);
        case feedback: return intParam("Feedback", -63, 63, 9);
        case delayOffset: return offsetParam(9.9f);
        case mix: return intParam("Mix", -63, 63, -63);
        case inputMode: return inputParam(1);
        default: return eqParam(idx - lowFreq);
        }
    }

  private:
    void design(double)
    {
        auto &l = this->last;
        auto &s = this->set;
        this->common(l[lfoSpeed], l[mix], 0, std::lround(l[inputMode]) == 0, {lowFreq, lowGain, highFreq, highGain, midFreq, midGain, midWidth});
        const int v = std::clamp((int)std::lround(l[pmDepth]), 0, 127), ba = swingBA(v), cd = swingCD(v);
        const int age = tenthsToSamples(l[delayOffset]) + 1, centre = (ba - cd) / 2;
        const Lfo::Wave waves[4] = {Lfo::triangle, Lfo::triangle, Lfo::sine, Lfo::sine};
        const int phases[4] = {5, 11, 13, 3};
        for (int t = 0; t < 4; ++t)
        {
            s.wave[t] = waves[t], s.phaseCode[t] = phases[t];
            auto &tp = s.tap[t];
            tp.lfo = t, tp.clamped = t < 2, tp.am = 0;
            tp.pm = ((t < 2 ? ba : cd) << 4) * word;
            tp.age = age + (t < 2 ? 0 : centre);
        }
        const double fb = std::clamp((int)std::lround(l[feedback]), -63, 63) * 250 * word;
        s.fbLB = s.fbLC = s.fbRA = s.fbRD = fb;
        s.outB = s.outC0 = s.outA = s.outD = top;
        s.outC1 = 0;
    }
};

// Chorus B: four sine-LFO taps (0, 120, 240 and 240 degrees), C and D 64 samples behind B and A,
// each line fed back from B or A alone; the wet 1 dB down.
template <typename FXConfig> struct ChorusB : Network<FXConfig, 13, ChorusB<FXConfig>>
{
    enum params
    {
        lfoSpeed,
        lfoDepth,
        feedback,
        delayOffset,
        OSCAIRN_CHORUS_COMMON_PARAMS,
        inputMode,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-chorus-b"};
    static constexpr const char *displayName{"Chorus B"};
    using Base = Network<FXConfig, 13, ChorusB<FXConfig>>;
    using Base::Base;
    friend typename Base::Base;

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        switch (idx)
        {
        case lfoSpeed: return speedParam((float)speedHz(192));
        case lfoDepth: return intParam("LFO Depth", 0, 127, 23);
        case feedback: return intParam("Feedback", -63, 63, 0);
        case delayOffset: return offsetParam(0.1f);
        case mix: return intParam("Mix", -63, 63, 63);
        case inputMode: return inputParam(1);
        default: return eqParam(idx - lowFreq);
        }
    }

  private:
    void design(double)
    {
        auto &l = this->last;
        auto &s = this->set;
        this->common(l[lfoSpeed], l[mix], -1, std::lround(l[inputMode]) == 0, {lowFreq, lowGain, highFreq, highGain, midFreq, midGain, midWidth});
        const double pm = depthWord(2 * std::clamp((int)std::lround(l[lfoDepth]), 0, 127)) * word;
        const int age = tenthsToSamples(l[delayOffset]) + 1;
        const int phases[4] = {0, 5, 11, 11};
        for (int t = 0; t < 4; ++t)
        {
            s.wave[t] = Lfo::sine, s.phaseCode[t] = phases[t];
            auto &tp = s.tap[t];
            tp.lfo = t, tp.clamped = t < 2, tp.am = 0, tp.pm = pm;
            tp.age = age + (t < 2 ? 0 : 64);
        }
        const double fb = std::clamp((int)std::lround(l[feedback]), -63, 63) * 376 * word;
        s.fbLB = s.fbRA = fb;
        s.fbLC = s.fbRD = 0;
        s.outB = s.outC0 = s.outA = s.outD = top;
        s.outC1 = 0;
    }
};

// Dual Modulator: one tap per channel, B on the left and A on the right, triangle LFOs half a turn
// apart, with AM (1 - depth x LFO) and feedback from each tap into its line; the wet 3 dB up.
template <typename FXConfig> struct DualModulator : Network<FXConfig, 14, DualModulator<FXConfig>>
{
    enum params
    {
        lfoSpeed,
        amDepth,
        pmDepth,
        feedback,
        delayOffset,
        OSCAIRN_CHORUS_COMMON_PARAMS,
        inputMode,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-dual-modulator"};
    static constexpr const char *displayName{"Dual Modulator"};
    using Base = Network<FXConfig, 14, DualModulator<FXConfig>>;
    using Base::Base;
    friend typename Base::Base;

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        switch (idx)
        {
        case lfoSpeed: return speedParam((float)speedHz(96));
        case amDepth: return intParam("AM Depth", 0, 127, 0);
        case pmDepth: return intParam("PM Depth", 0, 127, 54);
        case feedback: return intParam("Feedback", -63, 63, 13);
        case delayOffset: return offsetParam(17.1f);
        case mix: return intParam("Mix", -63, 63, -63);
        case inputMode: return inputParam(0);
        default: return eqParam(idx - lowFreq);
        }
    }

  private:
    void design(double)
    {
        auto &l = this->last;
        auto &s = this->set;
        this->common(l[lfoSpeed], l[mix], 3, std::lround(l[inputMode]) == 0, {lowFreq, lowGain, highFreq, highGain, midFreq, midGain, midWidth});
        const double pm = depthWord(2 * std::clamp((int)std::lround(l[pmDepth]), 0, 127)) * word;
        const int a = std::clamp((int)std::lround(l[amDepth]), 0, 127);
        const double am = -std::trunc(32768.0 * a / 127) * word;
        const int age = tenthsToSamples(l[delayOffset]) + 1;
        const int phases[4] = {0, 8, 0, 0};
        for (int t = 0; t < 4; ++t)
        {
            s.wave[t] = Lfo::triangle, s.phaseCode[t] = phases[t];
            auto &tp = s.tap[t];
            tp.lfo = t, tp.clamped = t < 2, tp.am = am, tp.pm = pm;
            tp.age = age + (t < 2 ? 0 : 64);
        }
        const double fb = std::clamp((int)std::lround(l[feedback]), -63, 63) * 376 * word;
        s.fbLB = s.fbRA = fb;
        s.fbLC = s.fbRD = 0;
        s.outB = s.outA = top;
        s.outC0 = s.outD = s.outC1 = 0;
    }
};

// Chorus C: three taps on one line fed the mean of the input, B, A and C at 0, 120 and 240 degrees of
// a sine LFO, each with a triangle folded from a saw at eight times the speed (a third of its turn
// apart); B and C on the left, A and C on the right; the wet 1 dB down.
template <typename FXConfig> struct ChorusC : Network<FXConfig, 12, ChorusC<FXConfig>>
{
    enum params
    {
        lfoSpeed,
        lfoDepth,
        delayOffset,
        OSCAIRN_CHORUS_COMMON_PARAMS,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-chorus-c"};
    static constexpr const char *displayName{"Chorus C"};
    using Base = Network<FXConfig, 12, ChorusC<FXConfig>>;
    using Base::Base;
    friend typename Base::Base;

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        switch (idx)
        {
        case lfoSpeed: return speedParam((float)speedHz(176));
        case lfoDepth: return intParam("LFO Depth", 0, 127, 25);
        case delayOffset: return offsetParam(0.1f);
        case mix: return intParam("Mix", -63, 63, -63);
        default: return eqParam(idx - lowFreq);
        }
    }

  private:
    void design(double)
    {
        auto &l = this->last;
        auto &s = this->set;
        this->common(l[lfoSpeed], l[mix], -1, true, {lowFreq, lowGain, highFreq, highGain, midFreq, midGain, midWidth});
        const int v = std::clamp((int)std::lround(l[lfoDepth]), 0, 127);
        const double pm = depthWord((5 * v) >> 3) * word, tri = depthWord((3 * v) >> 3) * word;
        const int s0 = tenthsToSamples(l[delayOffset]);
        const int phases[4] = {0, 5, 11, 0};
        const int ages[4] = {s0 + 2, s0 + 130, s0 + 66, 1};
        const double triPhase[4] = {0, 21845 * word, -21846 * word, 0};
        for (int t = 0; t < 4; ++t)
        {
            s.wave[t] = Lfo::sine, s.phaseCode[t] = phases[t];
            auto &tp = s.tap[t];
            tp.lfo = t, tp.clamped = t < 2, tp.am = 0;
            tp.pm = t < 3 ? pm : 0;
            tp.triGain = t < 3 ? tri : 0;
            tp.triPhase = triPhase[t];
            tp.age = ages[t];
            tp.right = false;
        }
        s.fastStepMul = 8;
        s.fastPhaseCode = 12;
        s.fbLB = s.fbLC = s.fbRA = s.fbRD = 0;
        s.outB = s.outC0 = s.outA = s.outC1 = top;
        s.outD = 0;
    }
};
#undef OSCAIRN_CHORUS_COMMON_PARAMS
} // namespace oscairn_fx::chorus

#endif
