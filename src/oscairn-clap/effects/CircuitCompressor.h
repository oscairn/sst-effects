/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * A circuit-style compressor whose behaviour follows reference renders (plan, section 15,
 * "Circuit-style compressor"). Written for Oscairn; follows the sst-effects effect contract.
 * Signal flow, per channel unless stated:
 *  - the input level scales the signal, held to steps of 1/16384 (which is why the lowest input
 *    settings move the level in 6.02 dB steps);
 *  - a first-order shelf of -3 dB at 2 kHz, then one of +3 dB at 8 kHz whose direct part is
 *    clipped at full scale, then a 0.43 Hz DC block, clipped at full scale again;
 *  - the detector takes the louder channel after a first-order 180 Hz high-pass, through two
 *    cascaded followers (both rise at the attack rate, fall at the release rate; each moves
 *    2 / N of the way per sample, N the displayed time in 44.1 kHz samples);
 *  - a gain curve per ratio (soft knee, fitted) gives the gain c, at most 1/2; the signal is
 *    c * (g * x + (1/2 - g) * lp(x)), g = sqrt(c / 2) and lp a 15 kHz low-pass, so content above
 *    15 kHz is compressed harder;
 *  - a saturating curve (slope 4 at zero, flat at 1 from about 0.57), the output level (times 8),
 *    then the makeup gain (set by the input level, ratio, attack and release; never by the
 *    signal), each stage clipped at full scale.
 * The reference's level after the effect is 7.91 dB above its input's (fitted), so the effect
 * runs 7.91 dB down and the output is raised again and clipped.
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
constexpr double ratios[5] = {2, 4, 8, 12, 20};
// Gain curve per ratio (dB of the detector level against full scale): threshold, knee width and
// slope, fitted to the reference's curves (within 0.1 dB above the knee, 0.3 dB in it).
constexpr double curveThresholdDb[5] = {-48.708, -44.538, -43.352, -43.028, -42.790};
constexpr double curveKneeDb[5] = {6.266, 4.764, 4.513, 4.515, 4.352};
constexpr double curveSlope[5] = {0.49477, 0.74560, 0.87128, 0.91322, 0.94678};
constexpr double postGainDb = 7.91;

// The displayed attack and release times (s) for the 0..200 values the converter reads (fx7).
inline double attackSeconds(double v) { return 0.022e-3 + 50.378e-3 * std::pow(v / 200, 2.5); }
inline double releaseSeconds(double v) { return 10.88e-3 + 533.34e-3 * (v / 200) * (v / 200); }
// The displayed times as 44.1 kHz sample counts, counted from the shortest setting.
inline double attackSamples(double ms) { return 1 + (std::max(ms, 0.022) - 0.022) / 0.0226803; }
inline double releaseSamples(double ms) { return 480 + 44.1 * (std::max(ms, 10.88) - 10.88); }

// Makeup gain (linear) for an input level (linear), ratio setting and displayed times (ms):
// L16 = 16 x input; L16^(-0.75 / R) above 1, L16^(-1/2) below (L16 held at 0.005 or more), then
// L16^(-0.66 s ka) for attacks above 1 ms and L16^(0.2 s kr) for releases above about 31 ms, s =
// 1 - 1/R, ka and kr growing with the times' logarithm up to 1; times 1.414, in steps of 1/2048,
// at most 32767/2048.
inline double makeupGain(double inputLevel, int ratio, double attackMs, double releaseMs)
{
    const double R = ratios[ratio], s = 1 - 1 / R, L16 = 16 * std::max(inputLevel, 1e-9);
    double m = L16 > 1 ? std::pow(L16, -0.75 / R) : 1 / std::sqrt(std::max(L16, 0.005));
    const double na = attackSamples(attackMs), nr = releaseSamples(releaseMs);
    if (na > 44.1)
        m *= std::pow(L16, -0.66 * s * std::min(1.0, std::log10(na / 44.1) / 1.5));
    if (nr > 1378.1)
        m *= std::pow(L16, 0.2 * s * std::min(1.0, std::log10(nr / 1791.6)));
    m *= 1.414;
    return m >= 16 ? 32767.0 / 2048 : std::floor(m * 2048) / 2048;
}

// The saturating curve (odd; fitted to the reference's within 0.002).
inline double saturate(double u)
{
    const double a = std::abs(u);
    if (a >= 0.6)
        return u < 0 ? -1 : 1;
    const double z = a * (4 + a * (-5.3411 + a * (26.7011 + a * (-66.0058 + a * 85.1824))));
    return u < 0 ? -std::tanh(z) : std::tanh(z);
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
        for (auto &c : ch)
            c = Channel{};
        designFilters();
        last[0] = NAN;
        update();
    }
    void suspendProcessing() { initialize(); }
    int getRingoutDecay() const { return -1; }
    void onSampleRateChanged() { initialize(); }

    void processBlock(float *__restrict L, float *__restrict R)
    {
        update();
        const double down = std::pow(10.0, -postGainDb / 20), up = 1 / down;
        for (int i = 0; i < FXConfig::blockSize; ++i)
        {
            double h[2], peak = 0;
            for (int c = 0; c < 2; ++c)
            {
                auto &s = ch[c];
                const double a = gin * down * (c ? R[i] : L[i]);
                const double u1 = sh1.b0 * a + sh1.b1 * s.x1 - sh1.a1 * s.y1;
                s.x1 = a, s.y1 = u1;
                const double v = std::clamp(sh2.b0 * u1 + sh2.b1 * s.x2, -1.0, 1.0);
                const double u2 = v - sh2.a1 * s.y2;
                s.x2 = u1, s.y2 = u2;
                s.dc += dcK * (u2 - s.dc);
                h[c] = std::clamp(u2 - s.dc, -1.0, 1.0);
                const double d = hp.b0 * h[c] + hp.b1 * s.xd - hp.a1 * s.yd;
                s.xd = h[c], s.yd = d;
                peak = std::max(peak, std::min(std::abs(d), 1.0));
            }
            e1 += (peak > e1 ? kA : kR) * (peak - e1);
            e2 += (e1 > e2 ? kA : kR) * (e1 - e2);
            const double c1 = curveGain(e2), g = std::sqrt(c1 / 2);
            for (int c = 0; c < 2; ++c)
            {
                auto &s = ch[c];
                const double lo = lp.b0 * h[c] + lp.b1 * s.l1 + lp.b2 * s.l2 - lp.a1 * s.m1 - lp.a2 * s.m2;
                s.l2 = s.l1, s.l1 = h[c], s.m2 = s.m1, s.m1 = lo;
                const double w = saturate(16 * c1 * (g * h[c] + (0.5 - g) * lo));
                const double y = std::clamp(makeup * std::clamp(gout * w, -1.0, 1.0), -1.0, 1.0);
                (c ? R[i] : L[i]) = (float)std::clamp(up * y, -1.0, 1.0);
            }
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
    struct Channel
    {
        double x1{0}, y1{0}, x2{0}, y2{0}, dc{0}, xd{0}, yd{0}, l1{0}, l2{0}, m1{0}, m2{0};
    };
    struct FirstOrder
    {
        double b0{1}, b1{0}, a1{0};
    };
    struct SecondOrder
    {
        double b0{1}, b1{0}, b2{0}, a1{0}, a2{0};
    };
    Channel ch[2];
    FirstOrder sh1, sh2, hp;
    SecondOrder lp;
    double dcK{0}, e1{0}, e2{0}, gin{0}, gout{0}, makeup{1}, kA{1}, kR{1};
    int ratioIndex{1};
    float last[numParamsEnum]{};

    // The bilinear corner the 44.1 kHz design has, kept at every rate.
    double corner(double hz) const { return std::tan(M_PI * hz / designRate) * designRate / this->sampleRate(); }
    FirstOrder shelf(double hz, double db) const
    {
        const double K = corner(hz), V = std::pow(10.0, std::abs(db) / 20);
        const double kz = db >= 0 ? K : V * K, kp = db >= 0 ? V * K : K, g = kp / kz;
        return {g * (1 + kz) / (1 + kp), g * (kz - 1) / (1 + kp), (kp - 1) / (1 + kp)};
    }
    void designFilters()
    {
        sh1 = shelf(2000, -3);
        sh2 = shelf(8000, 3);
        const double Kh = corner(180);
        hp = {1 / (1 + Kh), -1 / (1 + Kh), (Kh - 1) / (1 + Kh)};
        const double K = corner(15000), n = 1 / (1 + M_SQRT2 * K + K * K);
        lp = {K * K * n, 2 * K * K * n, K * K * n, 2 * (K * K - 1) * n, (1 - M_SQRT2 * K + K * K) * n};
        dcK = 1 - std::exp(-2 * M_PI * 0.43 / this->sampleRate());
    }
    double curveGain(double env) const
    {
        const double d = 20 * std::log10(std::max(env, 1e-12)) - curveThresholdDb[ratioIndex];
        const double W = curveKneeDb[ratioIndex], s = curveSlope[ratioIndex];
        double db = 0;
        if (2 * d > W)
            db = -s * d;
        else if (2 * d > -W)
            db = -s * (d + W / 2) * (d + W / 2) / (2 * W);
        return 0.5 * std::pow(10.0, db / 20);
    }
    // A follower coefficient at 44.1 kHz, kept as the same time constant at the rate.
    double coefficient(double k44) const
    {
        if (k44 >= 1)
            return 1;
        return 1 - std::pow(1 - k44, designRate / this->sampleRate());
    }
    // A level in dB held to steps of 1/16384, as the reference holds it; the bottom is off. The
    // 1e-4 keeps a level that falls on a step (the platform's value 25 is 32/16384) from dropping
    // a step through the dB parameter's float rounding.
    static double heldLevel(float db) { return db < -138.5f ? 0.0 : std::floor(16384 * std::pow(10.0, db / 20.0) + 1e-4) / 16384; }

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
        kA = coefficient(std::min(1.0, 2 / attackSamples(last[attack])));
        kR = coefficient(std::min(1.0, 2 / releaseSamples(last[release])));
        gin = heldLevel(last[inputLevel]);
        gout = 8 * heldLevel(last[outputLevel]);
        makeup = makeupGain(last[inputLevel] < -138.5f ? 0.0 : std::pow(10.0, last[inputLevel] / 20.0), ratioIndex, last[attack], last[release]);
    }
};
} // namespace oscairn_fx::dynamics

#endif
