/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * A fixed amp and effects chain (plan, section 15, "Amp chain"). Written for
 * Oscairn; follows the sst-effects effect contract. Every coefficient is a 16-bit word
 * (x 1/32768) that glides to its setting; every stage clips to [-1, 1). Signal flow:
 *  - input: the channels' sum times a quarter, clipped (the chain is mono until the delay), at
 *    the reference's insertion level (7.915 dB down, raised again at the output);
 *  - compressor: a full-wave detector over a threshold of half the sustain word, an envelope
 *    that rises and falls at two rates (multipliers held as a 4-bit exponent and an 11-bit
 *    mantissa), a gain curve 1 / (1 + 999 k / 1024) read at k = 1024 x level x envelope (the
 *    level a per-sample ramp to 2^-10 / sustain), the makeup gain and a soft saturation
 *    y = 2 (u - u|u| / 2); off, the input passes at an eighth;
 *  - wah and phaser: off in this version, the phaser passing the signal at 32767/32768;
 *  - distortion, one sample later: a pre-EQ (a first-order low-pass, a direct path and a
 *    first-order high-pass at gains set by the tone, then a slow high-pass), the drive and a
 *    soft saturation, a level-dependent gain g' = A + |g (A - 1 + |(A - 1) v|)| (a saturator
 *    with memory), a second saturation, a presence band (two first-order all-passes mixed with
 *    the direct signal) and a low-pass, a DC tracker that falls and rises at different rates,
 *    the same saturator again; then the speaker: two biquads, a resonant high-pass (a
 *    state-variable filter) and a one-pole low-pass; off, the input passes unchanged;
 *  - delay, one sample later again: one line read at the delay time, a first-order low-pass and
 *    a high-pass in the loop, the feedback into a soft saturation at the line's input; the
 *    outputs the wet and dry signals at their words (the right wet inverted in the stereo echo),
 *    times the output level and 16.
 * Delay times are held in seconds and filters designed at the running rate, so every rate
 * behaves as 44.1 kHz does; the words are exact there.
 */
#ifndef OSCAIRN_CLAP_EFFECTS_AMPCHAIN_H
#define OSCAIRN_CLAP_EFFECTS_AMPCHAIN_H

#include "sst/effects/EffectCore.h"
#include "sst/basic-blocks/params/ParamMetadata.h"
#include "effects/ChorusFamily.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace oscairn_fx::amp
{
using chorus::top;
// [-1, 1) as two min/max instructions.
inline double clip(double x) { return std::fmin(std::fmax(x, -1.0), top); }

// The platform's coefficient rounding: half away from zero in single precision, then held to a
// 16-bit word.
inline int q16(double x)
{
    const float s = (float)(x * 32768);
    const float r = s + (s < 0 ? -0.5f : 0.5f);
    return (int)std::clamp(r, -32768.f, 32767.f);
}
// pi as the platform's designs hold it, in single precision.
inline constexpr double pi32 = (double)(float)M_PI;
// A first-order low-pass coefficient: 1 - exp(-2 pi f / fs).
inline double onePole(double hz, double rate) { return 1 - std::exp(-2 * pi32 * hz / rate); }
// A multiplier held as a 4-bit exponent and an 11-bit mantissa: word w gives
// (1/2 + mantissa / 4096) / 2^(15 - exponent); zero below zero.
inline double expGain(int w)
{
    if (w < 0)
        return 0;
    const int m = 0x4000 | ((w & 0x7ff) << 3), e = 15 & ~(w >> 11);
    return m / 32768.0 / (double)(1 << e);
}
// The inverse: x in (0, 1) to the word, the mantissa rounded.
inline int expWord(double x)
{
    if (x >= 1)
        return 0x7fff;
    if (x <= 0)
        return 0;
    int e = 15;
    while (x < 0.5 && e > 0)
    {
        x += x;
        --e;
    }
    x -= 0.5;
    if (x < 0)
        return e << 11;
    int m = (int)(x * 4096 + 0.5);
    if (m >= 0x800)
    {
        if (e <= 14)
            m = 0, ++e;
        else
            m -= 1;
    }
    return m + (e << 11);
}
inline int fixWord(double x) { return (int)std::clamp(std::trunc(x * 32768), -2147483648.0, 2147483647.0); }
// The stages' saturation: 2 (u - u|u| / 2), u clipped, clipped again.
inline double soft(double u)
{
    u = clip(u);
    return clip(2 * (u - 0.5 * u * std::abs(u)));
}
inline double clipUp(double x) { return std::fmin(std::fmax(x, 0.0), top); }

// The compressor's gain curve, 1 / (1 + 999 k / 1024), held in the multiplier format, k = 0-1024.
struct CurveTable
{
    std::array<double, 1025> v;
    CurveTable()
    {
        for (int k = 0; k <= 1024; ++k)
        {
            double g = 1 / (1 + 999.0 * k / 1024);
            int w;
            if (g >= 1)
                w = 32767;
            else
            {
                int e = 15;
                while (g < 0.5 && e > 0)
                    g *= 2, --e;
                int m = (int)std::floor((g - 0.5) * 4096 + 0.5);
                if (m >= 2048)
                    m -= 2048, ++e;
                w = (e << 11) + m;
            }
            v[k] = w / 32768.0;
        }
    }
};
inline const CurveTable curveTable{};

// The reference's insertions run 7.915 dB below the level it renders (fitted to the compressor's
// limiting; the circuit compressor's fit found 7.91 dB): the chain runs that much down and its
// output is raised again and clipped at full scale.
inline constexpr double insertionLevel = 0.40201;

// The chain's coefficient words.
enum W : int
{
    eqLowB = 0x00, eqLowA = 0x01, eqHighK = 0x02, eqLowG = 0x04, eqDirG = 0x05, eqHighG = 0x06, eqDcK = 0x08,
    cmpSus = 0x09, cmpFb = 0x0a, drive = 0x0b, cmpRel = 0x0c, cmpLvl = 0x0e, dstFloor = 0x0f, cmpAtk = 0x10,
    sat1Am1 = 0x12, sat1A = 0x13, sat1Out = 0x16, prsAp1 = 0x19, prsAp2 = 0x1a, cmpMix = 0x20, prsMix = 0x21,
    prsLp = 0x23, dcFall = 0x25, dcRise = 0x27, sat2Am1 = 0x2b, sat2A = 0x2c, wahDry = 0x34, wahWet = 0x35,
    phsIn = 0x3a, phsTap = 0x42, sat2Out = 0x40, dlyTime = 0x41, spk1B1 = 0x44, spk1B0 = 0x45, spk1A1 = 0x46,
    spk1A2 = 0x47, spk2B1 = 0x48, spk2B2 = 0x49, spk2B0 = 0x4a, spk2A1 = 0x4b, spk2A2 = 0x4c, loopB = 0x4e,
    loopA = 0x4f, spkDamp = 0x50, loopHp = 0x52, wetL = 0x54, dryL = 0x55, wetR = 0x56, dryR = 0x57, fb = 0x58,
    outL = 0x59, outR = 0x5a, spkLp = 0x5c, spkHpF = 0x5d, dstOn = 0x5e, numWords = 0x60
};
// The words that glide at the speaker's rate; every other one at the common rate.
inline bool speakerRate(int w) { return w >= spk1B1 && w <= spk2A2; }

// The settings of one chain, as words.
struct Words
{
    std::array<int, numWords> w{};
};

template <typename FXConfig> struct AmpChain : sst::effects::core::EffectTemplateBase<FXConfig>
{
    enum params
    {
        sustain,      // 0 off, 0.1-10
        distortion,   // option list (distNames)
        driveLevel,   // 0-10
        preEq,        // option list (eqNames)
        tone,         // 0-10
        presence,     // 0-10
        speaker,      // option list (speakerNames)
        delayType,    // option list (delayNames)
        delayControl, // 0-127, the delay's dry/wet
        delayTime,    // 0-127
        outputLevel,  // 0-127
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-amp-chain"};
    static constexpr const char *displayName{"Amp Chain"};

    AmpChain(typename FXConfig::GlobalStorage *s, typename FXConfig::EffectStorage *e, typename FXConfig::ValueStorage *p)
        : sst::effects::core::EffectTemplateBase<FXConfig>(s, e, p), line(bufferSamples(this->sampleRate()))
    {
    }
    // The longest delay, 0.675 s, and the interpolation's sample; a power of two.
    static int bufferSamples(double rate)
    {
        int n = 1;
        while (n < (int)std::ceil(0.675 * rate) + 4)
            n <<= 1;
        return n;
    }
    static size_t heapBytesAt(double rate) { return sizeof(float) * (size_t)bufferSamples(rate); }
    size_t heapBytes() const { return line.size() * sizeof(float); }
    double quietSeconds() const { return delaySeconds + 0.1; }

    void initialize()
    {
        std::fill(line.begin(), line.end(), 0.f);
        ptr = 0;
        lineSound = 0;
        s = State{};
        std::fill(std::begin(last), std::end(last), NAN);
        settled = false;
        first = true;
        quiet = false;
        delayOn = false;
        update();
    }
    void suspendProcessing() { initialize(); }
    int getRingoutDecay() const { return -1; }
    void onSampleRateChanged() { initialize(); }

    void processBlock(float *__restrict L, float *__restrict R)
    {
        update();
        bool silent = true;
        for (int i = 0; i < N; ++i)
            silent = silent && L[i] == 0.f && R[i] == 0.f;
        if (!settled)
            glide(N); // the words move a block at a time
        prepare();
        if (silent && quiet)
        {
            // Every state that reaches the output and the line at zero: the block is silence
            // out. The saturators' gains and the detector move on as silence moves them.
            for (int i = 0; i < FXConfig::blockSize; ++i)
            {
                if (cur[cmpMix] != 0)
                    follow(0);
                if (cur[dstOn] != 0)
                {
                    s.gain1 = cur[sat1A] + std::abs(s.gain1 * cur[sat1Am1]);
                    s.gain2 = cur[sat2A] + std::abs(s.gain2 * cur[sat2Am1]);
                }
            }
            return;
        }
        // Stage by stage over the block: the distortion takes the phaser's output one sample
        // late and the delay the distortion's, so each stage can run the whole block in turn.
        double a[N], b[N];
        front(L, R, a);
        distort(a, b);
        echo(b, L, R);
        quiet = silent && s.zero() && lineSound == 0;
    }

    static inline const std::unordered_map<int, std::string> distNames{{0, "Off"}, {1, "Clean 1"}, {2, "Crunch 1"}};
    static inline const std::unordered_map<int, std::string> eqNames{{0, "Pre-EQ A"}, {1, "Pre-EQ B"}};
    static inline const std::unordered_map<int, std::string> speakerNames{{0, "Off"}, {1, "Speaker A"}, {2, "Speaker B"}};
    static inline const std::unordered_map<int, std::string> delayNames{{0, "Off"}, {1, "Stereo Echo A"}, {2, "Mono Delay A"}};

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        auto tenth = [](const char *n, float d) { return pmd().asFloat().withName(n).withRange(0.f, 10.f).withDefault(d).withLinearScaleFormatting("").withDecimalPlaces(1); };
        switch (idx)
        {
        case sustain: return tenth("Compressor Sustain", 1.8f);
        case distortion: return pmd().asInt().withName("Distortion").withRange(0, 2).withDefault(1).withUnorderedMapFormatting(distNames);
        case driveLevel: return tenth("Drive", 1.8f);
        case preEq: return pmd().asInt().withName("Pre-EQ").withRange(0, 1).withDefault(0).withUnorderedMapFormatting(eqNames);
        case tone: return tenth("Tone", 1.5f);
        case presence: return tenth("Presence", 5.f);
        case speaker: return pmd().asInt().withName("Speaker").withRange(0, 2).withDefault(1).withUnorderedMapFormatting(speakerNames);
        case delayType: return pmd().asInt().withName("Delay").withRange(0, 2).withDefault(1).withUnorderedMapFormatting(delayNames);
        case delayControl: return pmd().asInt().withName("Delay Control").withRange(0, 127).withDefault(40);
        case delayTime: return pmd().asInt().withName("Delay Time").withRange(0, 127).withDefault(48);
        case outputLevel: return pmd().asInt().withName("Output Level").withRange(0, 127).withDefault(109);
        }
        return pmd().withName("Unused").withType(pmd::NONE);
    }

    // The settings' words at a rate (the laws; public for the tests).
    struct Setting
    {
        int sus, dist, drive, eq, tone, presence, speaker, delay, control, time, level;
    };
    static Words words(const Setting &p, double rate);

  private:
    struct State
    {
        double phsOut{0}, x1{0}, eqLp{0}, eqHp{0}, eqDc{0}, ap1{0}, ap1x{0}, ap2{0}, prsOut{0}, dc{0}, s1x1{0},
            s1x2{0}, s1y1{0}, s1y2{0}, s2y1{0}, s2y2{0}, bp{0}, lp{0}, spkOut{0}, dstOut{0}, tap1{0}, loopLp{0}, loopHp{0};
        // the detector and the saturators' gains: no output without input
        double env{0}, lvl{0}, gain1{0}, gain2{0};
        bool zero() const
        {
            for (const double *p = &phsOut; p <= &loopHp; ++p)
                if (*p != 0)
                    return false;
            return true;
        }
    };
    State s;
    std::vector<float> line;
    int ptr{0};
    long lineSound{0}; // how many of the line's samples are not zero
    // Constant while no word moves: the envelope's two multipliers, the delay's read offset.
    double gAtk{0}, gRel{0}, dFrac{0};
    int dInt{0};
    double cur[numWords]{}, tgt[numWords]{};
    double rateCommon{0}, rateSpeaker{0}, rateTime{0};
    double delaySeconds{0};
    bool settled{false}, first{true}, quiet{false}, delayOn{false};
    static constexpr int N = FXConfig::blockSize;
    float last[numParamsEnum]{};

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
        auto tenths = [&](int i) { return (int)std::lround(std::clamp(last[i], 0.f, 10.f) * 10); };
        auto pick = [&](int i, int hi) { return std::clamp((int)std::lround(last[i]), 0, hi); };
        const Setting p{tenths(sustain), pick(distortion, 2), tenths(driveLevel), pick(preEq, 1), tenths(tone), tenths(presence),
                        pick(speaker, 2), pick(delayType, 2), pick(delayControl, 127), pick(delayTime, 127), pick(outputLevel, 127)};
        const double rate = this->sampleRate();
        const Words w = words(p, rate);
        for (int k = 0; k < numWords; ++k)
            tgt[k] = w.w[k] / 32768.0;
        if (first)
            std::copy(std::begin(tgt), std::end(tgt), std::begin(cur));
        first = false;
        settled = false;
        // Per-sample glide rates at 44.1 kHz (2^-9, 7/32, and 2^-14 for the delay time's
        // exponent-scaled glide), the same time constants at the rate.
        const double per = 44100.0 / rate;
        rateCommon = std::min(1.0, 0x1p-9 * per);
        rateSpeaker = std::min(1.0, 0.21875 * per);
        rateTime = 0x1.4p-11 * per;
        delaySeconds = p.delay ? 0.075 + 0.6 * (p.time / 127.0) : 0.0;
    }

    // n samples of every word's glide; the words then hold for the block.
    void glide(int n)
    {
        bool done = true;
        for (int k = 0; k < numWords; ++k)
        {
            if (cur[k] == tgt[k])
                continue;
            for (int i = 0; i < n; ++i)
            {
                const double d = tgt[k] - cur[k];
                if (d >= -1e-8 && d <= 1e-8)
                {
                    cur[k] = tgt[k];
                    break;
                }
                if (k == dlyTime)
                {
                    int e;
                    std::frexp(d, &e);
                    cur[k] += std::min(0.25, rateTime * std::ldexp(1.0, 2 - e)) * d;
                }
                else
                    cur[k] += d * (speakerRate(k) ? rateSpeaker : rateCommon);
            }
            done = done && cur[k] == tgt[k];
        }
        settled = done;
    }

    void prepare()
    {
        gAtk = expGain(fixWord(cur[cmpAtk]));
        gRel = expGain(fixWord(cur[cmpRel]));
        const double pos = 131072 * clip(cur[dlyTime] * 0.25), fl = std::floor(pos);
        dInt = (int)fl;
        dFrac = pos - fl;
    }

    // The compressor's envelope and level on x.
    void follow(double x)
    {
        const double *c = cur;
        const double e = clipUp(2 * (std::abs(x) + c[cmpSus] * -0.5));
        const double d = e + c[cmpFb] * s.env;
        s.env = clip(s.env + clip(d * 0.25) * (d < 0 ? gRel : gAtk));
        s.lvl = 32 / 32768.0 - (c[cmpSus] * s.lvl - s.lvl);
    }

    // The compressor's detector and gain on x; returns the compressed, saturated signal.
    double detect(double x)
    {
        const double *c = cur;
        const double Kc = -0.5;
        follow(x);
        const double make = clip(2 * (s.lvl * c[cmpLvl]));
        const double h = 2 * (Kc + s.lvl * s.env);
        const double idx = h < -1 ? -512.0 : std::min(h, top) * 512, fl = std::floor(idx);
        const int k = 512 + (int)fl;
        const double curve = curveTable.v[k] + (idx - fl) * (curveTable.v[k + 1] - curveTable.v[k]);
        return soft(256 * (make * clip((x + x) * expGain(fixWord(curve)))));
    }

    // Input, compressor, wah and phaser over the block into a.
    void front(const float *L, const float *R, double *a)
    {
        const double *c = cur;
        const double k = 16 * (512 / 32768.0) * insertionLevel, mix = c[cmpMix], w = c[wahDry] * 0.5 * 4, ph = c[phsIn];
        if (mix == 0)
            for (int i = 0; i < N; ++i)
                a[i] = ph * clip(w * clip(0.5 * clip(k * ((double)L[i] + (double)R[i])) * 0.25));
        else
            for (int i = 0; i < N; ++i)
            {
                const double x = clip(k * ((double)L[i] + (double)R[i]));
                a[i] = ph * clip(w * clip((0.5 * x + mix * (x - detect(x))) * 0.25));
            }
    }

    // Distortion and speaker over the block, on the phaser's output one sample late, into b.
    void distort(const double *a, double *b)
    {
        const double *c = cur;
        if (c[dstOn] == 0)
        {
            // off: the input passes; the stage's states rest
            b[0] = s.phsOut;
            for (int i = 1; i < N; ++i)
                b[i] = a[i - 1];
            s.x1 = a[N - 2];
            s.phsOut = a[N - 1];
            return;
        }
        State t = s;
        const double on = c[dstOn], fall = c[dcFall], rise = c[dcRise];
        for (int i = 0; i < N; ++i)
        {
            const double X = i ? a[i - 1] : t.phsOut;
            // pre-EQ
            const double lo = c[eqLowB] * (t.x1 + X) + c[eqLowA] * t.eqLp;
            const double hs = t.eqHp + c[eqHighK] * (X - t.eqHp);
            const double t12 = c[eqLowG] * lo - t.eqDc + c[eqDirG] * X + c[eqHighG] * (X - 0.5 * (t.eqHp + hs));
            t.eqLp = lo, t.eqHp = hs, t.x1 = X;
            t.eqDc += c[eqDcK] * t12;
            // drive, the first saturator with memory, the second saturation
            const double v = soft(8 * (t12 * c[drive]));
            t.gain1 = c[sat1A] + std::abs(t.gain1 * (c[sat1Am1] + std::abs(c[sat1Am1] * v)));
            const double u = clip(256 * (c[sat1Out] * std::max(v * t.gain1, c[dstFloor])));
            const double y = clip((u - 0.5 * u * std::abs(u)) * 0.5);
            // presence: two all-passes mixed with the direct signal, then a low-pass
            const double a1 = t.ap1x + c[prsAp1] * (t.ap1 - y);
            const double a2 = t.ap1 + c[prsAp2] * (t.ap2 - a1);
            t.ap1x = y, t.ap1 = a1, t.ap2 = a2;
            t.prsOut += c[prsLp] * (clip(2 * (y + c[prsMix] * (a2 - y))) - t.prsOut);
            // the DC tracker and the second saturator with memory
            const double t94 = t.prsOut - t.dc;
            t.dc += (t94 < 0 ? fall : rise) * t94;
            t.gain2 = c[sat2A] + std::abs(t.gain2 * (c[sat2Am1] + std::abs(c[sat2Am1] * t94)));
            const double x2 = soft(256 * (c[sat2Out] * (t94 * t.gain2)));
            // speaker: two biquads, a resonant high-pass and a low-pass
            const double y1 = clip(4 * ((t.s1x1 * c[spk1B1] + c[spk1B0] * (t.s1x2 + x2)) * 0.0625 + c[spk1A1] * t.s1y1 + c[spk1A2] * t.s1y2));
            const double z = clip(4 * (t.s1y1 * c[spk2B1] + c[spk2B2] * t.s1y2 + c[spk2B0] * y1 + c[spk2A1] * t.s2y1 + c[spk2A2] * t.s2y2));
            t.s1x2 = t.s1x1, t.s1x1 = x2, t.s1y2 = t.s1y1, t.s1y1 = y1, t.s2y2 = t.s2y1, t.s2y1 = z;
            const double hp = z - (t.lp + 2 * c[spkDamp] * t.bp);
            t.bp += c[spkHpF] * hp;
            t.lp += c[spkHpF] * t.bp;
            t.spkOut += c[spkLp] * (hp - t.spkOut);
            b[i] = X + on * (X - t.spkOut);
        }
        t.phsOut = a[N - 1];
        s = t;
    }

    // Delay and outputs over the block, on the distortion's output one sample late.
    void echo(const double *b, float *L, float *R)
    {
        const double *c = cur;
        const bool on = c[wetL] != 0 || c[wetR] != 0 || c[fb] != 0;
        if (on != delayOn)
        {
            // switched on or off: the line and its filters start from silence (they rest while off)
            delayOn = on;
            std::fill(line.begin(), line.end(), 0.f);
            lineSound = 0;
            s.tap1 = s.loopLp = s.loopHp = 0;
        }
        double din[N], wet[N];
        din[0] = s.dstOut;
        for (int i = 1; i < N; ++i)
            din[i] = b[i - 1];
        s.dstOut = b[N - 1];
        if (on)
        {
            const int mask = (int)line.size() - 1;
            double tap1 = s.tap1, lp = s.loopLp, hpS = s.loopHp;
            for (int i = 0; i < N; ++i)
            {
                ptr = (ptr - 1) & mask;
                const double p = line[(ptr + 32768 + dInt) & mask], q = line[(ptr + 32769 + dInt) & mask];
                const double tap = p + dFrac * (q - p);
                const double w = c[loopB] * (tap1 + tap) + c[loopA] * lp;
                tap1 = tap, lp = w;
                const double hs = hpS + c[loopHp] * (w - hpS);
                const double back = clip((w - 0.5 * (hpS + hs)) * 0.5);
                hpS = hs;
                const float in = (float)soft(2 * (din[i] + c[fb] * back));
                lineSound += (in != 0.f) - (line[ptr] != 0.f);
                line[ptr] = in;
                wet[i] = 0.25 * w;
            }
            s.tap1 = tap1, s.loopLp = lp, s.loopHp = hpS;
        }
        else
            std::fill(wet, wet + N, 0.0);
        const double gL = 16 * (1 + c[outL]), gR = 16 * (1 + c[outR]), up = 1 / insertionLevel;
        for (int i = 0; i < N; ++i)
        {
            L[i] = (float)std::fmax(std::fmin(clip(gL * (c[wetL] * wet[i] + c[dryL] * din[i])) * up, 1.0), -1.0);
            R[i] = (float)std::fmax(std::fmin(clip(gR * (c[wetR] * wet[i] + c[dryR] * din[i])) * up, 1.0), -1.0);
        }
    }
};

// The amp-chain laws: settings to words.
namespace law
{
// 10^(n / 40) as the platform takes it: tenths of 10 by repeated multiplication, then the rest
// from a single-precision power.
inline double level05(int n)
{
    double d = 1;
    if (n < 0)
        while (n < 0)
            d *= 0.1, n += 40;
    else
        while (n >= 40)
            d *= 10, n -= 40;
    return d * (double)(float)std::pow(10.0, n / 40.0);
}
// The platform's volume curve (a fifth-order polynomial, clipped to [0, 1]).
inline double volume(double x)
{
    double v = x * 0x1.c3fc93aee4001p+0 + -0x1.6fe98f123edfep+0;
    v = v * x + 0x1.4c5274a602e00p-1;
    v = v * x + -0x1.55735141bc800p-10;
    v = v * x + 0x1.8647792aaf4f7p-6;
    v *= x;
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}
// A cosine law for the delay's balance (a quartic, clipped to [0, 1]).
inline double cosLevel(double x)
{
    double v = x * 0x1.6604f78a52480p-3 + 0x1.6fceb667e4980p-4;
    v = v * x + -0x1.44e0440fe6e71p+0;
    v = v * x + 0x1.22b9b81e54c03p-8;
    v = v * x + 1;
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}
inline double tanK(double hz, double rate) { return std::tan(pi32 * hz / rate); }
// A biquad's numerator and denominator, quantised to words at a quarter; the numerator's rounding
// errors are kept within half a step by moving the middle word (so the DC gain stays put).
inline void biquadWords(const double b[3], const double a[3], int out[5])
{
    double err = 0;
    for (int k = 0; k < 3; ++k)
    {
        out[k] = q16(b[k] / 4);
        err += out[k] - b[k] / 4 * 32768;
    }
    if (err > 0.5)
        out[1] -= 1;
    else if (err < -0.5)
        out[1] += 1;
    out[3] = q16(-a[1] / 4);
    out[4] = q16(-a[2] / 4);
}
// The bilinear second-order section of a prototype with zeros (fz, qz) at gain g and poles
// (fp, qp); fz = 0 gives a low-pass numerator (k^2, 2 k^2, k^2).
inline void section(double fz, double qz, double g, double fp, double qp, double rate, int out[5])
{
    const double kp = tanK(fp, rate), D = 1 + kp / qp + kp * kp;
    const double a[3] = {1, 2 * (kp * kp - 1) / D, (1 - kp / qp + kp * kp) / D};
    double b[3];
    if (fz == 0)
        b[0] = b[2] = kp * kp / D, b[1] = 2 * kp * kp / D;
    else
    {
        const double kz = tanK(fz, rate);
        b[0] = g * (1 + kz / qz + kz * kz) / D, b[1] = g * 2 * (kz * kz - 1) / D, b[2] = g * (1 - kz / qz + kz * kz) / D;
    }
    biquadWords(b, a, out);
}
} // namespace law

template <typename FXConfig> Words AmpChain<FXConfig>::words(const Setting &p, double rate)
{
    Words r;
    auto &w = r.w;
    const int fsInt = (int)(rate + 0.5);
    // Fixed words: the wah off (its dry word at a half), the phaser off (its input at full scale),
    // the delay's loop filters (a 5 kHz first-order low-pass and a 100 Hz one-pole high-pass).
    w[wahDry] = 16384;
    w[phsIn] = 32767;
    {
        const double k = law::tanK(5000, fsInt);
        w[loopB] = q16(k / (1 + k));
        w[loopA] = q16((1 - k) / (1 + k));
        w[loopHp] = q16(onePole(100, rate));
    }
    // Compressor: the sustain word 2^(1 - n) (1 - f / 2) x 16384 for a sustain of n + f; the
    // makeup word from it; the envelope's rates for 0.1 ms attack and 1.487 s release.
    if (p.sus == 0)
    {
        w[cmpSus] = 32767, w[cmpLvl] = 16384, w[cmpFb] = -16384, w[cmpRel] = 16384, w[cmpAtk] = 16384, w[cmpMix] = 0;
    }
    else
    {
        const double x = p.sus / 100.0;
        const int n = (int)(x * 10);
        const int t = (int)((n - x * 10 + 2) * -16384.0);
        w[cmpSus] = std::min(0x7fff, (-(t >> n)) & 0xffff);
        w[cmpLvl] = (int)((w[cmpSus] * 0.29 / 32767 + 0.71) * 16384);
        const double attack = (double)0.0001f, release = (double)1.487f;
        // the envelope's normalisation for these two times
        const double ratio = std::log((double)(1.487f / 0.0001f)) / 2.302585092994046 * 3.3219280948873622;
        const double f = ratio - 13, norm = std::fma(1 - f, (double)0.998f, f * (double)0.999f);
        w[cmpFb] = q16(-norm);
        w[cmpAtk] = expWord(4 * (1 - std::exp(-1 / attack / rate)) / norm);
        w[cmpRel] = expWord(4 * (1 - std::exp(-1 / release / rate)) / norm);
        w[cmpMix] = -16384;
    }
    // Distortion: Clean 1 or Crunch 1 (the two saturators' a, b and c, d; the DC tracker's rising
    // and falling corners; presence points, interpolated below its middle for Crunch 1 only)
    if (p.dist == 1 || p.dist == 2)
    {
        struct Dist
        {
            double a, b, c, d, rise, fall, f1[3], f2[3], g[3], f3[3];
            bool lowerFixed;
        };
        static const Dist clean{33, (double)1.45f, 9, (double)1.6f, 30, 20, {10, 10, 7}, {8500, 8500, 5000}, {-5.5, -7, -12}, {1250, 5000, 30000}, true};
        static const Dist crunch{60, (double)1.84f, 50, (double)1.69f, 12, 8.5, {180, 150, 10}, {2000, 500, 320}, {-13.5, -7, -3}, {15000, 15000, 15000}, false};
        const Dist &D = p.dist == 1 ? clean : crunch;
        const double e0 = D.a, e1 = D.b, e2 = D.c, e3 = D.d;
        w[sat1A] = q16(1 / e0 / e1), w[sat1Am1] = q16(1 / e0 / e1 - 1), w[sat1Out] = q16(-e0 / 256);
        w[sat2A] = q16(1 / e2 / e3), w[sat2Am1] = q16(1 / e2 / e3 - 1), w[sat2Out] = q16(-e2 / 256);
        w[dstFloor] = -32768;
        w[dcRise] = q16(onePole(D.rise, rate)), w[dcFall] = q16(onePole(D.fall, rate));
        w[dstOn] = -32768;
        // presence: two all-passes, their mix and a low-pass, by the presence in two segments
        const double pr = p.presence / 100.0, xx = pr + pr;
        const int i = xx >= 1 ? 1 : 0, j = i + 1;
        const double fr = xx - (xx >= 1 ? 1 : 0);
        const double *f1s = D.f1, *f2s = D.f2, *gs = D.g, *f3s = D.f3;
        double f1 = f1s[i], f2 = f2s[i];
        if (xx >= 1 || !D.lowerFixed)
            f1 *= std::pow(f1s[j] / f1s[i], fr), f2 *= std::pow(f2s[j] / f2s[i], fr);
        const double k1 = std::tan(pi32 * f1 / rate), k2 = std::tan(pi32 * f2 / rate);
        w[prsAp1] = q16((1 - k1) / (k1 + 1));
        w[prsAp2] = q16((1 - k2) / (k2 + 1));
        w[prsMix] = q16((1 - std::pow(10.0, ((1 - fr) * gs[i] + fr * gs[j]) / 20)) * 0.5);
        w[prsLp] = q16(onePole(f3s[i] * std::pow(f3s[j] / f3s[i], fr), rate));
        // drive: 10^((d - 100) / 40) x 16403 - 19
        w[drive] = (int)(law::level05(p.drive - 100) * 16403.0 - 19);
    }
    // Pre-EQ A or B by the tone (five points; frequencies geometric, gains linear between them):
    // low-pass f and gain, high-pass f and gain, the direct path's gain, the slow high-pass's f
    {
        struct Eq
        {
            double fl[5], fh[5], gl[5], gd[5], gh[5], fd[5];
        };
        static const Eq eqA{{200, 500, 1500, 2300, 7000}, {1250, 500, 1500, 2300, 7000}, {-32, (double)-20.1f, -10, -20, (double)-30.5f},
                            {-60, -60, -60, -60, -60}, {0, -2, -4, 0, 0}, {5, 7, 10, 10, 10}};
        static const Eq eqB{{470, 470, 470, 76, 45}, {470, 470, 470, 3150, 2000}, {0, 0, 0, -2, (double)-0.915f},
                            {-60, -60, -60, (double)-23.1f, -20}, {0, 0, 0, (double)-0.63f, (double)-0.915f}, {2000, 500, 100, 500, 1000}};
        const Eq &E = p.eq == 1 ? eqB : eqA;
        const double tn = p.tone / 100.0;
        const int n = (int)(tn * 4), m = n < 4 ? n + 1 : n;
        const double fr = tn * 4 - n;
        const double *fl = E.fl, *fh = E.fh, *fd = E.fd, *gl = E.gl, *gh = E.gh;
        auto geo = [&](const double *v) { return std::pow(v[m] / v[n], fr) * v[n]; };
        auto db = [&](const double *v) {
            const double g = (1 - fr) * v[n] + fr * v[m];
            return g > (double)-59.99 ? std::pow(10.0, g / 20) : 0.0;
        };
        const double kl = law::tanK(geo(fl), fsInt);
        w[eqLowB] = q16(kl / (1 + kl)), w[eqLowA] = q16((1 - kl) / (1 + kl));
        const double kh = std::tan(pi32 * geo(fh) / rate);
        w[eqHighK] = q16((kh + kh) / (kh + 1));
        const double kd = std::tan(pi32 * geo(fd) / rate);
        w[eqDcK] = q16((kd + kd) / (kd + 1));
        const double scale = w[eqDcK] * -0.5 * 0x1p-15 + 1;
        w[eqLowG] = q16(scale * db(gl));
        w[eqDirG] = q16(scale * db(E.gd)); // -60 dB and below: off
        w[eqHighG] = q16(scale * db(gh));
    }
    // Speaker A: a 5.8 kHz low-pass (Q 0.92), a section with poles at 1.77 kHz (Q 1.4) and zeros at 1.43 kHz (Q 1.73, fitted), a 60 Hz
    // resonant high-pass (Q 0.8) and a 3.35 kHz one-pole low-pass
    if (p.speaker == 1)
    {
        int s1[5], s2[5];
        law::section(0, 0, 1, 5800, (double)0.92f, fsInt, s1);
        law::section(1430.945, 1.734767, 1.645954, 1770.302, 1.400885, fsInt, s2);
        w[spk1B0] = s1[0], w[spk1B1] = s1[1], w[spk1A1] = s1[3], w[spk1A2] = s1[4];
        w[spk2B0] = s2[0], w[spk2B1] = s2[1], w[spk2B2] = s2[2], w[spk2A1] = s2[3], w[spk2A2] = s2[4];
        w[spkDamp] = q16(0.5 / (double)0.8f);
        w[spkHpF] = q16(onePole(60, rate));
        w[spkLp] = q16(onePole(3350, rate));
    }
    else if (p.speaker == 2)
    {
        // Speaker B: a 4.3 kHz low-pass (Q 1.1), a section with poles at 876 Hz (Q 0.42) and zeros
        // at 828 Hz (Q 0.91, fitted), a 105 Hz resonant high-pass (Q 2) and a 7.6 kHz low-pass
        int s1[5], s2[5];
        law::section(0, 0, 1, 4300, (double)1.1f, fsInt, s1);
        law::section(828.3597, 0.912838, 1.300046, 876.0021, 0.420153, fsInt, s2);
        w[spk1B0] = s1[0], w[spk1B1] = s1[1], w[spk1A1] = s1[3], w[spk1A2] = s1[4];
        w[spk2B0] = s2[0], w[spk2B1] = s2[1], w[spk2B2] = s2[2], w[spk2A1] = s2[3], w[spk2A2] = s2[4];
        w[spkDamp] = q16(0.5 / 2.0);
        w[spkHpF] = q16(onePole(105, rate));
        w[spkLp] = q16(onePole(7600, rate));
    }
    else
    {
        // off: the first section a sample's delay, the second a wire, the high-pass at 2 Hz (Q 0.7)
        w[spk1B1] = 8192, w[spk2B0] = 8192;
        w[spkDamp] = q16(0.5 / (double)0.7f);
        w[spkHpF] = q16(onePole(2, rate));
        w[spkLp] = 32767;
    }
    // Delay: Stereo Echo A, 75-675 ms, the feedback falling 20 dB in 0.8 s, the balance on the
    // cosine law, the right wet inverted
    w[dlyTime] = -32767;
    w[dryL] = w[dryR] = 16384;
    if (p.delay == 1 || p.delay == 2)
    {
        // Mono Delay A: the same line without feedback, the right wet not inverted
        const double sec = 0.075 + (p.time / 127.0) * 0.6;
        w[dlyTime] = (int)(rate * sec + -32768.0 + 0.5);
        w[fb] = p.delay == 1 ? (int)(std::pow(10.0, -sec / (double)0.8f) * 16384) : 0;
        const double cv = p.control / 127.0;
        const double dry = law::cosLevel(cv * 0.5), wet = law::cosLevel(1 + cv * -0.5);
        w[dryL] = w[dryR] = (int)(dry * 16384);
        w[wetL] = (int)(wet * 16384);
        w[wetR] = (int)((p.delay == 1 ? -wet : wet) * 16384);
    }
    // Output level: 2 x 2^(-1/3) x the volume curve, less one
    w[outL] = w[outR] = q16(2 * (double)0.7937005259840998f * law::volume(p.level / 127.0) - 1);
    return r;
}

} // namespace oscairn_fx::amp

#endif
