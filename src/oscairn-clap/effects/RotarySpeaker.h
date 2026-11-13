/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * A two-rotor speaker whose behaviour follows reference renders (plan, section 15, "Rotary A").
 * Written for Oscairn; follows the sst-effects effect contract. Signal flow, at 44.1 kHz:
 *  - the input's sum, times 1.25, is split by a crossover: an inverted low-pass feeds the rotor, a
 *    high-pass the horn, each followed by a gain of 0.5012, its first-order shelf (the low shelf on
 *    the rotor, the high shelf on the horn) and a gain of 0.5; every coefficient in 16-bit steps;
 *  - each band is written to a delay line whose words keep a 12-bit mantissa, read by four taps
 *    with linear interpolation: a centre tap, a second path whose swing runs opposite to the
 *    centre's, and a tap per output channel. Each tap's delay is one sample plus the band's swing
 *    times a sine of the rotor's angle (0 to 1), its gain 1 minus its AM depth times the same sine;
 *  - the rotor's taps sit at fixed angles (the centre a quarter turn ahead of the horn's), the
 *    channel taps' angles set by the mic angle in sixteen phases;
 *  - the taps are mixed by fixed weights scaled by the balance (sines of the balance's angle), and
 *    the sum times 2.825;
 *  - the rotors turn at the platform's 128 speeds; a speed switch steps each rotor's speed at once
 *    and then every 100 ms (on a 2 ms timer): the rotor linearly by the slow-fast difference over the
 *    ramp time, the horn by 2.4 / (ramp time in tenths) of the remaining difference, each held to the
 *    nearest speed on the side of its target;
 *  - depth, swing and AM change smoothly: the swing as a one-pole (2^-12 per sample), the AM depths
 *    at a bounded rate, the mix weights as a one-pole (2^-9 per sample).
 * Depths, balance and the mic angle act in the platform's steps. Times are seconds, so every sample
 * rate behaves as 44.1 kHz does; the crossover keeps the 44.1 kHz design's corners, and at other
 * rates the shelves run a rate-matched replacement of their 44.1 kHz design (FiveBandEq.h).
 */
#ifndef OSCAIRN_CLAP_EFFECTS_ROTARYSPEAKER_H
#define OSCAIRN_CLAP_EFFECTS_ROTARYSPEAKER_H

#include "sst/effects/EffectCore.h"
#include "sst/basic-blocks/params/ParamMetadata.h"
#include "FiveBandEq.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

namespace oscairn_fx::rotary
{
constexpr double designRate = 44100;

// The platform's speeds (the modulation types' speed parameter, 0-127): the rotation's phase step
// per 44.1 kHz sample in 2^-24 turns, 16 per index up to 63, then in runs whose step doubles
// (measured: 0.67291, 2.6917, 4.3739, 6.7291, 7.4020, 9.7572 and 39.7018 Hz at 16, 64, 80, 91, 93, 100, 127).
inline int speedStep(int i)
{
    i = std::clamp(i, 0, 127);
    if (i <= 63)
        return 16 * i;
    struct Run
    {
        int from, base, step;
    };
    static constexpr Run runs[] = {{126, 14080, 1024}, {114, 7424, 512}, {102, 4096, 256}, {89, 2304, 128}, {77, 1472, 64}, {64, 1024, 32}};
    for (const auto &r : runs)
        if (i >= r.from)
            return r.base + r.step * (i - r.from);
    return 0;
}
inline double speedHz(int i) { return speedStep(i) * designRate / 16777216.0; }
// The speeds a ramp steps through: each rounded to 3 decimals below 10 Hz and 2 above, in single
// precision (the ramps' measured staircases).
inline float rampHz(int i)
{
    const double f = speedHz(i), s = f < 10 ? 1000 : 100;
    return (float)(std::floor(f * s + 0.5) / s);
}
inline int nearestSpeed(double hz)
{
    int best = 0;
    for (int i = 1; i < 128; ++i)
        if (std::abs(speedHz(i) - hz) < std::abs(speedHz(best) - hz))
            best = i;
    return best;
}
// The sixteen phases (turns) a platform angle value v (4-124) selects, as measured for the
// Tremolo's phase difference: the angle 3 (v - 64) degrees above 64 and 180 (v - 62.5) / 58.5
// below, snapped to the nearest of 90 q + 0, 30, 45 or 60 degrees, a tie going nearer 0.
inline double phaseTurns(int v)
{
    v = std::clamp(v, 4, 124);
    const double a = v >= 64 ? 3.0 * (v - 64) : 180.0 * (v - 62.5) / 58.5;
    double best = 0;
    for (int q = -2; q <= 2; ++q)
        for (double o : {0.0, 30.0, 45.0, 60.0})
        {
            const double p = 90.0 * q + o;
            if (p < -180 || p > 180)
                continue;
            const double e = std::abs(p - a), eb = std::abs(best - a);
            if (e < eb - 1e-9 || (std::abs(e - eb) <= 1e-9 && std::abs(p) < std::abs(best)))
                best = p;
        }
    return (best < 0 ? best + 360 : best) / 360;
}
// The rotation's sine, 0 to 1: 128 straight segments per turn with 256 steps each, held to 16 bits.
struct SineTable
{
    double v[129];
    SineTable()
    {
        for (int i = 0; i <= 128; ++i)
            v[i] = std::sin(2 * M_PI * i / 128);
    }
};
inline const SineTable sineTable{};
inline double rotation(double turns)
{
    const uint32_t c = (uint32_t)(turns * 16777216.0) & 0xffffff;
    const int i = (int)(c >> 17), f = (int)((c >> 9) & 255);
    const double s = sineTable.v[i] + (sineTable.v[i + 1] - sineTable.v[i]) * f / 256;
    return (std::floor(s * 32767 + 0.5) + 32768) / 65536;
}
// A delay-line word: the mantissa cut to 11 bits after the point (measured noise floor near -66 dB).
inline float lineWord(double x)
{
    uint64_t b;
    std::memcpy(&b, &x, 8);
    b &= 0xfffffe0000000000ull;
    std::memcpy(&x, &b, 8);
    return (float)x;
}
// The ring `line` read d samples behind its write position w (linear; d >= 1).
template <int Size> double linearRead(const float (&line)[Size], int w, double d)
{
    static_assert((Size & (Size - 1)) == 0, "a power of two");
    const int n = (int)d;
    const double f = d - n, x0 = line[(w - n) & (Size - 1)];
    return x0 + f * (line[(w - n - 1) & (Size - 1)] - x0);
}
// The ring `line` read d samples behind its write position w (4-point cubic; d >= 1).
template <int Size> double cubicRead(const float (&line)[Size], int w, double d)
{
    static_assert((Size & (Size - 1)) == 0, "a power of two");
    const int n = (int)d;
    const double f = d - n;
    auto at = [&](int j) { return (double)line[(w - j) & (Size - 1)]; };
    const double xm = at(n - 1), x0 = at(n), x1 = at(n + 1), x2 = at(n + 2);
    const double c1 = 0.5 * (x1 - xm), c2 = xm - 2.5 * x0 + 2 * x1 - 0.5 * x2, c3 = 0.5 * (x2 - xm) + 1.5 * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}
// A power of two above the largest swing (150 samples at 44.1 kHz) at 768 kHz.
inline constexpr int lineSize = 4096;

// Per depth setting v (0-127) and speed, a band's swing (its delay's full range, in 2^-15 of 256
// samples at 44.1 kHz) and its AM depths (centre and second path, channel taps), in 1/127.
struct DepthLaw
{
    int swingSteps, am, amChannel;
};
inline DepthLaw depthLaw(int band, bool fast, int v)
{
    int d, a, ac;
    if (band == 0)
    {
        d = fast ? v : 5 * v / 4;
        a = fast ? (v < 34 ? 15 * v / 4 : 127) : (v < 18 ? 57 * v / 8 : 127);
        ac = fast ? 13 * a / 20 : 25 * a / 38;
    }
    else
    {
        d = fast ? v : 7 * v / 6;
        a = fast ? (v <= 51 ? 15 * v / 12 : 64) : (v <= 22 ? 19 * v / 6 : 70);
        ac = fast ? 26 * a / 15 : 15 * a / 19;
    }
    return {16384 * d / 127, a, ac};
}
// The AM coefficient for a depth a (1/127), as a 16-bit step below zero.
inline double amCoefficient(int a) { return std::trunc(-a * 32768.0 / 127) / 32768; }

template <typename FXConfig> struct RotarySpeaker : sst::effects::core::EffectTemplateBase<FXConfig>
{
    enum params
    {
        speed,
        rotorSlow,
        hornSlow,
        rotorFast,
        hornFast,
        rotorRamp,
        hornRamp,
        rotorDepth,
        hornDepth,
        balance,
        lowFreq,
        lowGain,
        highFreq,
        highGain,
        micAngle,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-rotary-a"};
    static constexpr const char *displayName{"Rotary A"};

    RotarySpeaker(typename FXConfig::GlobalStorage *s, typename FXConfig::EffectStorage *e,
                  typename FXConfig::ValueStorage *p)
        : sst::effects::core::EffectTemplateBase<FXConfig>(s, e, p)
    {
        for (auto &c : shelfMatch)
            if constexpr (requires { s->worker; s->offline; })
                c.prepare(this->sampleRate(), s->worker, s->offline);
            else
                c.prepare(this->sampleRate(), nullptr, nullptr);
    }

    static size_t heapBytesAt(double rate) { return 2 * eq::MatchedCascade<1>::heapBytesAt(rate); }
    size_t heapBytes() const { return shelfMatch[0].heapBytes() + shelfMatch[1].heapBytes(); }

    void initialize()
    {
        for (auto &r : rot)
            r = Rotor{};
        for (auto &b : xover)
            b.reset();
        for (auto &b : shelves)
            b.reset();
        for (auto &c : shelfMatch)
            c.reset();
        last[0] = NAN;
        timerSamples = 0;
        timerOn = false;
        update();
        // The rotors start at the speeds the switch selects, the depths and weights at their values.
        for (int k = 0; k < 2; ++k)
        {
            auto &r = rot[k];
            r.index = targetIndex(k);
            r.state = rampHz(r.index);
            r.swing = r.swingTarget;
            r.am = r.amTarget;
            r.amCh = r.amChTarget;
            rampedTo[k] = r.index;
        }
        for (int i = 0; i < 6; ++i)
            weight[i] = weightTarget[i];
    }
    void suspendProcessing() { initialize(); }
    int getRingoutDecay() const { return -1; }
    void onSampleRateChanged() { initialize(); }

    void processBlock(float *__restrict L, float *__restrict R)
    {
        update();
        constexpr int n = FXConfig::blockSize;
        const double fs = this->sampleRate(), per = designRate / fs; // 44.1 kHz samples per sample
        // A switch (or a new target speed) steps both rotors at once; then the 2 ms timer steps them
        // every 50th tick until both run at their targets.
        if (rampedTo[0] != targetIndex(0) || rampedTo[1] != targetIndex(1))
        {
            timerOn = rampStep();
            timerCount = 0;
        }
        timerSamples += n * per;
        while (timerSamples >= 88.2)
        {
            timerSamples -= 88.2;
            if (timerOn && ++timerCount == 50)
            {
                timerCount = 0;
                timerOn = rampStep();
            }
        }
        // The bands: the low-passed rotor band (inverted) and the high-passed horn band, each
        // through its shelf.
        float bands[2][n], spare[n];
        for (int i = 0; i < n; ++i)
        {
            const float u = 1.25f * (L[i] + R[i]);
            bands[0][i] = (float)(bandGain * xover[0].run(0, u));
            bands[1][i] = (float)(bandGain * xover[1].run(0, u));
        }
        for (int k = 0; k < 2; ++k)
            if (matched)
                shelfMatch[k].process(bands[k], spare, n, 1.0);
            else
                for (int i = 0; i < n; ++i)
                    bands[k][i] = shelves[k].run(0, bands[k][i]);
        const double swingPole = 1 - std::pow(1 - 0x1p-12, per), weightPole = 1 - std::pow(1 - 0x1p-9, per);
        double step[2];
        for (int k = 0; k < 2; ++k)
            step[k] = speedStep(rot[k].index) / 16777216.0 * per;
        for (int i = 0; i < n; ++i)
        {
            double out[2] = {0, 0};
            for (int j = 0; j < 6; ++j)
                weight[j] += weightPole * (weightTarget[j] - weight[j]);
            for (int k = 0; k < 2; ++k)
            {
                auto &r = rot[k];
                r.swing += swingPole * (r.swingTarget - r.swing);
                slew(r.am, r.amTarget, per);
                slew(r.amCh, r.amChTarget, per);
                r.line[r.w] = lineWord(0.5 * bands[k][i]);
                r.phase += step[k];
                r.phase -= std::floor(r.phase);
                // A tap at rotation m (0 to 1): one sample (at any rate, so a still tap stays exact) plus the
                // swing times m, and the gain 1 + AM x m in 16-bit steps (rounded down).
                const double full = 256 * r.swing * fs / designRate;
                auto tap = [&](double m, double am) {
                    const double x = am * m * 32768;
                    return linearRead(r.line, r.w, 1 + full * m) * (1 + (x < 0 ? std::trunc(x - 1) : std::trunc(x)) / 32768);
                };
                const double mc = rotation(r.phase + centreTurns[k]);
                const double centre = tap(mc, r.am), second = tap(32767.0 / 32768 - mc, r.am);
                const double left = tap(rotation(r.phase + chTurns[k][0]), r.amCh), right = tap(rotation(r.phase + chTurns[k][1]), r.amCh);
                const double common = weight[3 * k] * centre + weight[3 * k + 1] * second;
                out[0] += common + weight[3 * k + 2] * left;
                out[1] += common + weight[3 * k + 2] * right;
                r.w = (r.w + 1) & (lineSize - 1);
            }
            L[i] = (float)(outGain * out[0]);
            R[i] = (float)(outGain * out[1]);
        }
    }

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        auto hz = [](const char *n, float lo, float hi, float def) {
            return pmd().asFloat().withName(n).withRange(lo, hi).withDefault(def).withLinearScaleFormatting("Hz").withDecimalPlaces(3);
        };
        auto sec = [](const char *n, float def) {
            return pmd().asFloat().withName(n).withRange(0.f, 12.7f).withDefault(def).withLinearScaleFormatting("s").withDecimalPlaces(1);
        };
        auto pct = [](const char *n, float lo, float def) {
            return pmd().asFloat().withName(n).withRange(lo, 100.f).withDefault(def).withLinearScaleFormatting("%").withDecimalPlaces(1);
        };
        auto db = [](const char *n, float def) {
            return pmd().asFloat().withName(n).withRange(-12.f, 12.f).withDefault(def).withLinearScaleFormatting("dB").withDecimalPlaces(1);
        };
        switch (idx)
        {
        case speed: return pmd().asInt().withName("Speed").withRange(0, 1).withDefault(0).withUnorderedMapFormatting({{0, "Slow"}, {1, "Fast"}});
        case rotorSlow: return hz("Rotor Slow Speed", 0.f, 2.65f, 0.799f);
        case hornSlow: return hz("Horn Slow Speed", 0.f, 2.65f, 1.093f);
        case rotorFast: return hz("Rotor Fast Speed", 0.f, 39.7f, 6.729f);
        case hornFast: return hz("Horn Fast Speed", 0.f, 39.7f, 7.402f);
        case rotorRamp: return sec("Rotor Ramp Time", 7.7f);
        case hornRamp: return sec("Horn Ramp Time", 1.4f);
        case rotorDepth: return pct("Rotor Depth", 0.f, 38.6f);
        case hornDepth: return pct("Horn Depth", 0.f, 44.1f);
        case balance: return pct("Balance", -100.f, 14.3f);
        case lowFreq: return hz("Low Shelf Frequency", 32.f, 2000.f, 315.f);
        case lowGain: return db("Low Shelf Gain", 0.f);
        case highFreq: return hz("High Shelf Frequency", 500.f, 16000.f, 1200.f);
        case highGain: return db("High Shelf Gain", 1.f);
        case micAngle: return pmd().asFloat().withName("Mic Angle").withRange(0.f, 180.f).withDefault(180.f).withLinearScaleFormatting("deg").withDecimalPlaces(0);
        }
        return pmd().withName("Unused").withType(pmd::NONE);
    }

  private:
    struct Rotor
    {
        float line[lineSize]{};
        int w{0};
        double phase{0};
        int index{0};      // the speed it turns at
        float state{0};    // the ramp's speed (Hz), which the index follows
        double swing{0}, swingTarget{0}, am{0}, amTarget{0}, amCh{0}, amChTarget{0};
    };
    Rotor rot[2];
    eq::Biquad xover[2], shelves[2]; // shelves: the 44.1 kHz design (low on the rotor, high on the horn)
    eq::MatchedCascade<1> shelfMatch[2]; // their replacements at other rates (FiveBandEq.h)
    bool matched{false};
    static constexpr double bandGain = 8211.0 / 16384, outGain = 11571.0 / 4096;
    double weight[6]{}, weightTarget[6]{}, centreTurns[2]{0.25, 0}, chTurns[2][2]{};
    int fastNow{0}, speedIndex[2][2]{}, rampedTo[2]{-1, -1}, timerCount{0};
    double timerSamples{0};
    bool timerOn{false};
    float last[numParamsEnum]{};

    int targetIndex(int k) const { return speedIndex[k][fastNow]; }
    // The AM depths move by at most 2^-12 per 44.1 kHz sample, by a quarter of the rest near the end.
    static void slew(double &x, double target, double per)
    {
        const double d = target - x;
        if (std::abs(d) <= 1e-8)
            return;
        int e;
        std::frexp(d, &e);
        x += std::min(0.25, 0x1p-14 * std::ldexp(1.0, 2 - e) * per) * d;
    }
    // One step of both ramps; true while either rotor is short of its target.
    bool rampStep()
    {
        bool going = false;
        for (int k = 0; k < 2; ++k)
        {
            auto &r = rot[k];
            const int t = targetIndex(k);
            const float T = rampHz(t), tenths = std::max(0.f, last[k == 0 ? rotorRamp : hornRamp]) * 10;
            if (T != r.state)
            {
                if (tenths <= 0)
                    r.state = T;
                else if (k == 0)
                {
                    const float d = (rampHz(speedIndex[0][1]) - rampHz(speedIndex[0][0])) / tenths;
                    r.state = T > r.state ? std::min(T, r.state + d) : std::max(T, r.state - d);
                }
                else
                {
                    const float div = tenths / (T > r.state ? 2.4f : -2.4f);
                    const float x = T > r.state ? r.state + (T - r.state) / div : r.state + (r.state - T) / div;
                    const float y = (float)((double)x + (T > r.state ? 1e-4 : -1e-4));
                    r.state = T > r.state ? std::min(T, y) : std::max(T, y);
                }
            }
            // The speed the rotor turns at: the nearest on the side of the target.
            int i;
            if (r.state <= T)
            {
                for (i = 0; i < 127 && r.state > rampHz(i); ++i)
                    ;
                i = std::min(i, t);
            }
            else
            {
                for (i = 127; i > 0 && r.state < rampHz(i); --i)
                    ;
                i = std::max(i, t);
            }
            r.index = i;
            rampedTo[k] = t;
            going = going || i != t;
        }
        return going;
    }

    // The 44.1 kHz design's prewarped corner, as the tangent at the running rate.
    double corner(double hz) const { return std::tan(M_PI * std::min(hz, 0.49 * designRate) / designRate) * designRate / this->sampleRate(); }
    void design()
    {
        const double fs = this->sampleRate(), q = 0.7;
        // The crossover: an inverted low-pass at 619.5 Hz and a high-pass at 999.35 Hz, Q 0.7, held
        // in 16-bit steps with one integer bit at 44.1 kHz.
        const double K0 = corner(619.5), K1 = corner(999.35);
        xover[0].set(-K0 * K0, -2 * K0 * K0, -K0 * K0, 1 + K0 / q + K0 * K0, 2 * (K0 * K0 - 1), 1 - K0 / q + K0 * K0);
        xover[1].set(1, -2, 1, 1 + K1 / q + K1 * K1, 2 * (K1 * K1 - 1), 1 - K1 / q + K1 * K1);
        eq::firstOrderShelf(shelves[0], last[lowFreq], last[lowGain], false);
        eq::firstOrderShelf(shelves[1], last[highFreq], last[highGain], true);
        // Their 44.1 kHz coefficients are 16-bit words with 4 integer bits (the shelves' 32 Hz measurements).
        for (auto &s : shelves)
            eq::roundToSteps(s, 4, false);
        matched = fs != designRate;
        if (!matched)
            for (auto &x : xover)
                eq::roundToSteps(x, 1, true);
        else
            for (int k = 0; k < 2; ++k)
            {
                const eq::Biquad *d[1] = {eq::isIdentity(shelves[k]) ? nullptr : &shelves[k]};
                shelfMatch[k].set(d);
            }
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
        fastNow = last[speed] > 0.5f ? 1 : 0;
        speedIndex[0][0] = nearestSpeed(last[rotorSlow]);
        speedIndex[1][0] = nearestSpeed(last[hornSlow]);
        speedIndex[0][1] = nearestSpeed(last[rotorFast]);
        speedIndex[1][1] = nearestSpeed(last[hornFast]);
        // Depths, balance and the mic angle in the platform's steps.
        const int depth[2] = {(int)std::lround(std::clamp(last[rotorDepth], 0.f, 100.f) * 1.27f),
                              (int)std::lround(std::clamp(last[hornDepth], 0.f, 100.f) * 1.27f)};
        for (int k = 0; k < 2; ++k)
        {
            const auto law = depthLaw(k, fastNow, depth[k]);
            rot[k].swingTarget = law.swingSteps / 32768.0;
            rot[k].amTarget = amCoefficient(law.am);
            rot[k].amChTarget = amCoefficient(law.amChannel);
        }
        const int b = (int)std::clamp(std::lround(64 + std::clamp(last[balance], -100.f, 100.f) * 0.63f), 1l, 127l);
        auto sine = [](int v) { return v <= 1 ? 0 : (int)std::lround(32767 * std::sin(M_PI * (v - 1) / 252)); };
        const int t1 = sine(128 - b), t2 = sine(b);
        const int w[6] = {3 * (t1 / 4), -(t1 / 8), t1, 9 * (t2 / 20), 9 * (-t2 / 80), 9 * (t2 / 10)};
        for (int i = 0; i < 6; ++i)
            weightTarget[i] = w[i] / 32768.0;
        const int v = (int)std::lround(std::clamp(last[micAngle], 0.f, 180.f) / 3) / 2;
        chTurns[0][0] = phaseTurns(94 - v);
        chTurns[0][1] = phaseTurns(94 + v);
        chTurns[1][0] = phaseTurns(64 - v);
        chTurns[1][1] = phaseTurns(64 + v);
        design();
    }
};
} // namespace oscairn_fx::rotary

#endif
