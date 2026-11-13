/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * A two-rotor speaker whose behaviour follows reference renders (plan, section 15, "Rotary A").
 * Written for Oscairn; follows the sst-effects effect contract. Signal flow:
 *  - the mono sum of the input is split by a crossover: a low-pass feeds the rotor, a high-pass
 *    the horn; a first-order low shelf acts on the rotor's band only, a high shelf on the horn's;
 *  - each rotor turns at its slow or fast speed; on a switch the rotor's speed moves linearly in
 *    Hz, over its ramp time for the whole slow-fast difference, and the horn's exponentially with
 *    its time constant (0.925 times as long speeding up);
 *  - each rotor writes a delay line read by modulated taps: a tap per output channel, its angle
 *    offset by half the effective mic angle (minus on the left, plus on the right; the angle acts
 *    in measured steps), whose delay swings by the depth times the sine of that angle around a
 *    fixed centre and whose gain dips by its AM depth, 90 degrees ahead; the rotor adds a centre tap
 *    (no mic offset) to both channels;
 *  - the rotor and horn (inverted) are mixed by the balance (cos and sin of a quarter turn).
 * Depths are in percent of the platform's full range; the swing is proportional to it, the AM depth
 * reaches its full value at a low setting; swing and AM depth differ between slow and fast and glide
 * over 20 ms on a switch. Everything is in seconds and hertz, so every sample rate behaves as
 * 44.1 kHz does; the crossover keeps the 44.1 kHz design's corners, and at other rates the shelves
 * run a rate-matched replacement of their 44.1 kHz design (FiveBandEq.h).
 */
#ifndef OSCAIRN_CLAP_EFFECTS_ROTARYSPEAKER_H
#define OSCAIRN_CLAP_EFFECTS_ROTARYSPEAKER_H

#include "sst/effects/EffectCore.h"
#include "sst/basic-blocks/params/ParamMetadata.h"
#include "FiveBandEq.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace oscairn_fx::rotary
{
constexpr double designRate = 44100;

// Fitted constants (plan, section 15, "Rotary A"); [0] the rotor, [1] the horn, [.][0] slow, [.][1] fast.
struct Fit
{
    double crossHz[2];         // rotor low-pass, horn high-pass corners
    double crossQ[2];          // their Q
    double pitchMsFull[2][2];  // delay swing (peak, ms) at 100 % depth
    double amFull[2][2];       // AM depth at full effect
    double amFullAtPct[2][2];  // the depth setting (%) from which the AM depth is full
    double amLeadDeg[2];       // AM ahead of the delay swing
    double centreTap[2];       // weight of the centre tap
    double centreAmRatio[2];   // the centre tap's AM depth relative to the channel taps'
    double gain[2];            // the rotor's and horn's gains (the horn's inverted)
    double levelDbAtFullAm[2]; // level change with the AM at its full depth, in proportion below
};
inline constexpr Fit fitted{{619.5, 999.2}, {0.70, 0.70}, {{2.13, 1.72}, {1.69, 1.21}},
                            {{0.73, 0.74}, {0.45, 0.71}}, {{14.0, 26.7}, {17.4, 40.3}}, {90, 90},
                            {0.83, 0}, {1.42, 1}, {2.876, -2.190}, {0.5, -2.2}};

// The horn speeds up with a shorter time constant than it slows down with (ramps, fitted).
inline constexpr double hornUpRatio = 0.925;

// The mic angle acts in steps (batch b, every platform value 0-60): the angle (degrees) from which
// each step's effective L/R angle applies, per rotor. Below the first step the taps coincide.
struct MicStep
{
    double from, angle;
};
inline constexpr MicStep rotorMicSteps[] = {{28.5, 30}, {34.5, 60}, {76.5, 90}, {106.5, 120}, {148.5, 150}, {154.5, 180}};
inline constexpr MicStep hornMicSteps[] = {{34.5, 31}, {40.5, 61}, {76.5, 75}, {82.5, 88}, {106.5, 105}, {112.5, 121}, {154.5, 180}};
template <size_t N> double micStep(const MicStep (&steps)[N], double deg)
{
    double a = 0;
    for (const auto &s : steps)
        if (deg >= s.from)
            a = s.angle;
    return a;
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
// A power of two above twice the largest swing, plus the interpolator, at 768 kHz.
inline constexpr int lineSize = 4096;

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
        update();
        // The rotors start at the speed the switch selects, without a ramp.
        for (int k = 0; k < 2; ++k)
        {
            rot[k].hz = target[k];
            rot[k].pitch = pitchTarget[k];
            rot[k].am = amTarget[k];
            rot[k].centre = centreTarget[k];
        }
    }
    void suspendProcessing() { initialize(); }
    int getRingoutDecay() const { return -1; }
    void onSampleRateChanged() { initialize(); }

    void processBlock(float *__restrict L, float *__restrict R)
    {
        update();
        const double fs = this->sampleRate(), dt = FXConfig::blockSize / fs;
        // Per block: speeds follow their ramps, depths glide to the switch's values over 20 ms.
        const double glide = 1 - std::exp(-dt / 0.02);
        double step[2], pitchS[2], am[2], level[2], base[2];
        for (int k = 0; k < 2; ++k)
        {
            auto &r = rot[k];
            const double d = target[k] - r.hz;
            if (k == 0)
                r.hz = rampSeconds[0] <= 0 ? target[0] : r.hz + std::clamp(d, -slew * dt, slew * dt);
            else
                r.hz = rampSeconds[1] <= 0 ? target[1] : r.hz + d * (1 - std::exp(-dt / (rampSeconds[1] * (d > 0 ? hornUpRatio : 1))));
            r.pitch += glide * (pitchTarget[k] - r.pitch);
            r.am += glide * (amTarget[k] - r.am);
            step[k] = 2 * M_PI * r.hz / fs;
            pitchS[k] = r.pitch * fs; // swing in samples
            // The taps' centre: the larger of the two speeds' swings, so the switch leaves it, plus a sample.
            base[k] = r.centre * fs + 1;
            r.centre += glide * (centreTarget[k] - r.centre);
            am[k] = r.am;
            level[k] = mix[k] * std::pow(10.0, fitted.levelDbAtFullAm[k] / 20 * r.am / fitted.amFull[k][fastNow]);
        }
        // The bands of the mono sum, each through its shelf: the low shelf acts on the rotor only,
        // the high shelf on the horn only.
        constexpr int n = FXConfig::blockSize;
        float bands[2][n], spare[n];
        for (int i = 0; i < n; ++i)
        {
            const float m = 0.5f * (L[i] + R[i]);
            bands[0][i] = xover[0].run(0, m);
            bands[1][i] = xover[1].run(0, m);
        }
        for (int k = 0; k < 2; ++k)
            if (matched)
                shelfMatch[k].process(bands[k], spare, n, 1.0);
            else
                for (int i = 0; i < n; ++i)
                    bands[k][i] = shelves[k].run(0, bands[k][i]);
        for (int i = 0; i < n; ++i)
        {
            const float band[2] = {bands[0][i], bands[1][i]};
            double out[2] = {0, 0};
            for (int k = 0; k < 2; ++k)
            {
                auto &r = rot[k];
                r.line[r.w] = band[k];
                const double th = r.angle;
                double y[2];
                for (int c = 0; c < 2; ++c)
                {
                    const double ph = th + (c ? mu[k] : -mu[k]);
                    y[c] = tap(r, ph, base[k], pitchS[k], am[k], k);
                }
                if (fitted.centreTap[k] > 0)
                {
                    const double ctr = fitted.centreTap[k] * tap(r, th, base[k], pitchS[k], std::min(1.0, am[k] * fitted.centreAmRatio[k]), k);
                    y[0] = (y[0] + ctr) / (1 + fitted.centreTap[k]);
                    y[1] = (y[1] + ctr) / (1 + fitted.centreTap[k]);
                }
                out[0] += level[k] * y[0];
                out[1] += level[k] * y[1];
                r.w = (r.w + 1) & (lineSize - 1);
                r.angle += step[k];
                if (r.angle >= 2 * M_PI)
                    r.angle -= 2 * M_PI;
            }
            L[i] = (float)out[0];
            R[i] = (float)out[1];
        }
    }

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        auto hz = [](const char *n, float lo, float hi, float def) {
            return pmd().asFloat().withName(n).withRange(lo, hi).withDefault(def).withLinearScaleFormatting("Hz").withDecimalPlaces(3);
        };
        auto sec = [](const char *n, float hi, float def) {
            return pmd().asFloat().withName(n).withRange(0.f, hi).withDefault(def).withLinearScaleFormatting("s").withDecimalPlaces(3);
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
        case rotorRamp: return sec("Rotor Ramp Time", 12.25f, 7.42f);
        case hornRamp: return sec("Horn Time Constant", 5.1f, 0.51f);
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
        double angle{0}, hz{0}, pitch{0}, am{0}, centre{0};
    };
    Rotor rot[2];
    eq::Biquad xover[2], shelves[2]; // shelves: the 44.1 kHz design (low on the rotor, high on the horn)
    eq::MatchedCascade<1> shelfMatch[2]; // their replacements at other rates (FiveBandEq.h)
    bool matched{false};
    double target[2]{}, pitchTarget[2]{}, centreTarget[2]{}, amTarget[2]{}, rampSeconds[2]{}, slew{0}, mix[2]{1, 0}, mu[2]{},
        amLead[2]{};
    int fastNow{0};
    float last[numParamsEnum]{};

    // A tap at angle ph: the line read at base + swing * sin(ph) (4-point cubic), times the AM gain.
    double tap(const Rotor &r, double ph, double base, double swing, double am, int k) const
    {
        const double x = cubicRead(r.line, r.w, base + swing * std::sin(ph));
        return x * (1 - am * (1 - std::cos(ph + amLead[k])) / 2);
    }

    // The 44.1 kHz design's prewarped corner, as the tangent at the running rate.
    double corner(double hz) const { return std::tan(M_PI * std::min(hz, 0.49 * designRate) / designRate) * designRate / this->sampleRate(); }
    void design()
    {
        const double fs = this->sampleRate();
        for (int k = 0; k < 2; ++k)
        {
            const double K = corner(fitted.crossHz[k]), q = fitted.crossQ[k];
            if (k == 0)
                xover[k].set(K * K, 2 * K * K, K * K, 1 + K / q + K * K, 2 * (K * K - 1), 1 - K / q + K * K);
            else
                xover[k].set(1, -2, 1, 1 + K / q + K * K, 2 * (K * K - 1), 1 - K / q + K * K);
        }
        eq::firstOrderShelf(shelves[0], last[lowFreq], last[lowGain], false);
        eq::firstOrderShelf(shelves[1], last[highFreq], last[highGain], true);
        matched = fs != designRate;
        if (matched)
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
        const int fast = last[speed] > 0.5f ? 1 : 0;
        fastNow = fast;
        target[0] = std::max(0.f, fast ? last[rotorFast] : last[rotorSlow]);
        target[1] = std::max(0.f, fast ? last[hornFast] : last[hornSlow]);
        rampSeconds[0] = std::max(0.f, last[rotorRamp]);
        rampSeconds[1] = std::max(0.f, last[hornRamp]);
        slew = rampSeconds[0] > 0 ? std::abs(last[rotorFast] - last[rotorSlow]) / rampSeconds[0] : 0;
        const float depth[2] = {std::clamp(last[rotorDepth], 0.f, 100.f), std::clamp(last[hornDepth], 0.f, 100.f)};
        for (int k = 0; k < 2; ++k)
        {
            pitchTarget[k] = fitted.pitchMsFull[k][fast] * 1e-3 * depth[k] / 100;
            centreTarget[k] = std::max(fitted.pitchMsFull[k][0], fitted.pitchMsFull[k][1]) * 1e-3 * depth[k] / 100;
            amTarget[k] = fitted.amFull[k][fast] * std::min(1.0, depth[k] / fitted.amFullAtPct[k][fast]);
            amLead[k] = fitted.amLeadDeg[k] * M_PI / 180;
        }
        const double x = M_PI / 4 * (1 + std::clamp(last[balance], -100.f, 100.f) / 100.0);
        mix[0] = fitted.gain[0] * std::cos(x);
        mix[1] = fitted.gain[1] * std::sin(x);
        // Half the effective angle each side.
        mu[0] = micStep(rotorMicSteps, last[micAngle]) * M_PI / 360;
        mu[1] = micStep(hornMicSteps, last[micAngle]) * M_PI / 360;
        design();
    }
};
} // namespace oscairn_fx::rotary

#endif
