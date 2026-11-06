/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * A hall reverb on the reference's structure (plan, section 15, "Hall A rebuilt"). Written for
 * Oscairn; follows the sst-effects effect contract. Per side:
 *  - the input passes a first-order HPF and a first-order low shelf at 800 Hz whose gain is
 *    10 x low ratio - 10 dB, held at +2 dB;
 *  - three all-passes diffuse it, and diffusion only crossfades the diffused and the plain signal;
 *  - one long line holds the initial delay, four early-reflection taps (moved by room size and the
 *    decay parameter, the later two raised by decay, the first absent at the shortest reverb time)
 *    and three taps after the first reflection that feed the tank;
 *  - the tank is a loop through both sides: an all-pass, a first-order low-pass set by the high
 *    ratio, a line, a nested all-pass, a low shelf set by the low ratio, a line, then the other
 *    side times the loop gain;
 *  - the output is the early reflections plus taps of the tank's lines, through the LPF (0.4 + 0.6
 *    times a first-order low-pass), times a level.
 * Room size changes only delays (three fitted scales), reverb time only gains (the loop gain, the
 * level, the first reflection's gain); nothing is modulated. The delays below are this design's
 * own; the scales, the gains per reverb time, the high-ratio cutoffs, the loop shelf and the
 * early-reflection laws are fitted to reference renders (fx19, fx24, fx27).
 * A parameter change costs a few first-order designs, so it is made in process, and every gain
 * and filter coefficient then ramps over 2 ms.
 */
#ifndef OSCAIRN_CLAP_EFFECTS_HALLREVERB_H
#define OSCAIRN_CLAP_EFFECTS_HALLREVERB_H

#include "sst/effects/EffectCore.h"
#include "sst/basic-blocks/params/ParamMetadata.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace oscairn_fx::hall
{
constexpr int bands = 7;
constexpr double bandHz[bands] = {125, 250, 500, 1000, 2000, 4000, 8000};

// What is fitted to reference renders. Times are the displayed reverb times (s) of the measured
// grid; sizes the measured room sizes; values in between are interpolated.
struct Fit
{
    double timeAt[10];
    double loopGain[10];  // the tank's gain per pass through one side
    double levelDb[10];   // output level
    double sizeAt[6];
    double sizeScale[6];  // delay scale (1 = the delays below)
    double highHz[10];    // high ratio 0.1 ... 1.0: the loop low-pass's cutoff (0: off)
    double lowAt[5];
    double loopShelfDb[5]; // low ratio: the loop shelf's gain per pass
    double apGain[4];     // diffusers, the tank's first all-pass, nested outer, nested inner
    double erGain[2][4];
    double erFixedMs[2][4]; // early-reflection tap = initial delay + fixed + scaled x size scale
    double erScaledMs[2][4];
    double decayAt[5];
    double decayShiftMs[5]; // the decay parameter moves the block after the initial delay
    double decayDb[5];      // and raises the later two early reflections
    double erLevel;         // early reflections' level against the tank's
    double firstTap[10];    // the first early reflection's gain, by reverb time
    double lineScale[6];    // the tank lines' delay scale, by room size (sizeAt)
    double tankApScale[6];  // the tank's all-passes' delay scale, by room size
};

inline constexpr Fit fitted{
    {0.3, 1.1, 1.9, 2.7, 3.5, 4.3, 5.5, 9.5, 13.5, 18},
    {0, 0, 0.3701, 0.5008, 0.5368, 0.6556, 0.6677, 0.7229, 0.746, 0.8263},
    {-9.1, -8.63, -8.44, -8.46, -7.94, -8.13, -7.86, -7.6, -7.11, -7.24},
    {0, 8, 15, 22, 29, 31},
    {0.2021, 0.408, 0.5778, 0.7879, 0.9687, 1.0085},
    {2326, 2743, 3234, 3814, 4497, 5303, 6253, 7373, 19744, 0},
    {0.1, 0.4, 0.7, 1, 1.4},
    {-0.67, -0.57, 0, 0.76, 1.57},
    {0.62, 0.6103, 0.572, 0.485},
    {{-0.6, 1, -1, 1}, {0.6, 0.8, -0.8, 1}},
    {{17.33, 11.8, 23.5, 21.8}, {19.96, 19.5, 16.7, 21.7}},
    {{29.7, 68, 64.5, 93.9}, {68.5, 78.5, 98, 104.9}},
    {0, 16, 32, 55, 63},
    {-56.72, -44.46, -32.41, 0, 34.33},
    {-20, -20, -20, 0, 4.96},
    0.5394,
    {0, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {0.3069, 0.4696, 0.6178, 0.7185, 0.7593, 1.012},
    {0.2309, 0.3924, 0.5762, 0.7302, 1.2083, 0.8804}};

// This design's delays (s) at size scale 1, per side.
constexpr double diffuserS[2][3] = {{0.0033, 0.0051, 0.0079}, {0.0037, 0.0057, 0.0089}};
constexpr double feedS[2][3] = {{0.020, 0.031, 0.043}, {0.023, 0.035, 0.041}};
constexpr double feedShape[3] = {1, -0.8, 0.6};
constexpr double erApS[2] = {0.0071, 0.0083};
constexpr double tankApS[2] = {0.0311, 0.0373};
constexpr double tankLine1S[2] = {0.0743, 0.0697};
constexpr double nestOuterS[2] = {0.0527, 0.0479};
constexpr double nestInnerS[2] = {0.0131, 0.0157};
constexpr double tankLine2S[2] = {0.0619, 0.0661};
// Output taps: fractions of the own side's first and second line, and of the other side's second.
constexpr double outTap[3] = {0.12, 0.33, 0.81};
constexpr double outSign[4] = {1, -1, 1, -1};
constexpr double outFrac1b = 0.61;
constexpr double maxInitialS = 0.2, maxShiftS = 0.0345;

inline double interp(const double *x, const double *y, int n, double v)
{
    if (v <= x[0])
        return y[0];
    for (int i = 1; i < n; ++i)
        if (v <= x[i])
            return y[i - 1] + (y[i] - y[i - 1]) * (v - x[i - 1]) / (x[i] - x[i - 1]);
    return y[n - 1];
}

struct Section
{
    double b0{1}, b1{0}, b2{0}, a1{0}, a2{0}, z1{0}, z2{0};
    inline double run(double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// A delay line; read(d) is the sample pushed d pushes ago (1 <= d <= capacity).
struct Line
{
    std::vector<float> b;
    int w{0};
    void reserve(int n) { b.assign((size_t)std::max(2, n + 1), 0.f); }
    void clear() { std::fill(b.begin(), b.end(), 0.f); w = 0; }
    inline double read(int d) const
    {
        int r = w - d;
        if (r < 0)
            r += (int)b.size();
        return b[(size_t)r];
    }
    inline void push(double x)
    {
        b[(size_t)w] = (float)x;
        if (++w == (int)b.size())
            w = 0;
    }
};

// A gain or coefficient that ramps linearly to a new target over a fixed number of samples.
struct Ramp
{
    double v{0}, step{0};
    int left{0};
    void to(double t, int n, bool now)
    {
        if (now)
            v = t, left = 0;
        else
            step = (t - v) / n, left = n;
    }
    inline double next()
    {
        if (left > 0)
            v += step, --left;
        return v;
    }
};

// First-order sections (bilinear at the running rate); coefficients ramp, states persist.
struct FirstOrder
{
    Ramp b0, b1, a1;
    double x1{0}, y1{0};
    inline double run(double x)
    {
        const double y = b0.next() * x + b1.next() * x1 - a1.next() * y1;
        x1 = x, y1 = y;
        return y;
    }
    void set(double c0, double c1, double d1, int n, bool now) { b0.to(c0, n, now), b1.to(c1, n, now), a1.to(d1, n, now); }
    // At other rates the low-pass keeps the 44.1 kHz design's response at 6.5 kHz (decision 13): its
    // bilinear warping there is part of the reference's high-band decay and level.
    void lowPass(double hz, double rate, int n, bool now)
    {
        if (hz <= 0 || hz >= 0.49 * 44100)
            return set(1, 0, 0, n, now);
        const double f0 = std::min(6500.0, 0.45 * rate);
        const double k = std::tan(M_PI * hz / 44100) * std::tan(M_PI * f0 / rate) / std::tan(M_PI * f0 / 44100);
        set(k / (1 + k), k / (1 + k), (k - 1) / (k + 1), n, now);
    }
    void highPass(double hz, double rate, int n, bool now)
    {
        const double k = std::tan(M_PI * std::min(hz, 0.45 * rate) / rate);
        set(1 / (1 + k), -1 / (1 + k), (k - 1) / (k + 1), n, now);
    }
    // Low shelf: gain g (dB) at DC, 1 at high frequencies, centred (geometrically) at hz.
    void lowShelf(double hz, double gDb, double rate, int n, bool now)
    {
        const double k = std::tan(M_PI * std::min(hz, 0.45 * rate) / rate), s = std::pow(10.0, gDb / 40);
        const double d = k / s + 1;
        set((k * s + 1) / d, (k * s - 1) / d, (k / s - 1) / d, n, now);
    }
    void clear() { x1 = y1 = 0; }
};

struct Settings
{
    double time{3.5}, diffusion{10}, initialDelayMs{67.8}, hpf{80}, lpf{5600}, size{29}, highRatio{0.8}, lowRatio{0.7},
        decay{55};
    bool operator==(const Settings &) const = default;
};

// Delay scale for a room size.
inline double sizeScale(const Fit &f, double size) { return interp(f.sizeAt, f.sizeScale, 6, size); }
inline double lineScale(const Fit &f, double size) { return interp(f.sizeAt, f.lineScale, 6, size); }
inline double maxScale(const Fit &f) { return *std::max_element(std::begin(f.sizeScale), std::end(f.sizeScale)); }
inline double maxLineScale(const Fit &f) { return *std::max_element(std::begin(f.lineScale), std::end(f.lineScale)); }
inline double tankApScale(const Fit &f, double size) { return interp(f.sizeAt, f.tankApScale, 6, size); }
inline double maxTankApScale(const Fit &f) { return *std::max_element(std::begin(f.tankApScale), std::end(f.tankApScale)); }

// The reverb itself, independent of the effect contract (the fitting driver uses it directly).
struct Core
{
    const Fit *fit{&fitted};
    double rate{44100};
    int fadeLen{1};
    Line input[2], diff[2][3], erAp[2], tankAp[2], line1[2], outer[2], inner[2], line2[2];
    FirstOrder hp[2], shelf[2], lp1[2], loopShelf[2], outLp[2];
    Ramp loopGain, level, erGain[2][4], mix;
    int dDiff[2][3]{}, dEr[2][4]{}, dFeed[2][3]{}, dErAp[2]{}, dTankAp[2]{}, dLine1[2]{}, dOuter[2]{}, dInner[2]{}, dLine2[2]{},
        dOut[2][4]{};
    bool hpOn{false}, lpOn{false}, have{false};
    double onset{0};

    static int samples(double s, double rate) { return (int)std::ceil(s * rate) + 1; }
    // Capacities (samples) of the lines in order, for the declared memory.
    static std::vector<int> capacities(const Fit &f, double rate)
    {
        const double m = maxScale(f), ml = maxLineScale(f), ma = maxTankApScale(f);
        double erMax = 0;
        for (int s = 0; s < 2; ++s)
            for (int k = 0; k < 4; ++k)
                erMax = std::max(erMax, (f.erFixedMs[s][k] + f.erScaledMs[s][k] * m) / 1000);
        double feedMax = 0;
        for (int s = 0; s < 2; ++s)
            for (double v : feedS[s])
                feedMax = std::max(feedMax, (f.erFixedMs[s][0] + f.erScaledMs[s][0] * m) / 1000 + v * m);
        std::vector<int> c;
        for (int s = 0; s < 2; ++s)
        {
            c.push_back(samples(maxInitialS + maxShiftS + std::max(erMax, feedMax), rate));
            for (int k = 0; k < 3; ++k)
                c.push_back(samples(diffuserS[s][k] * m, rate));
            c.push_back(samples(erApS[s] * m, rate));
            c.push_back(samples(tankApS[s] * ma, rate));
            c.push_back(samples(tankLine1S[s] * ml, rate));
            for (double v : {nestOuterS[s], nestInnerS[s]})
                c.push_back(samples(v * ma, rate));
            c.push_back(samples(tankLine2S[s] * ml, rate));
        }
        return c;
    }
    static size_t heapBytesAt(const Fit &f, double rate)
    {
        size_t n = 0;
        for (int v : capacities(f, rate))
            n += (size_t)std::max(2, v + 1);
        return n * sizeof(float);
    }
    size_t heapBytes() const
    {
        size_t n = 0;
        for (int s = 0; s < 2; ++s)
        {
            n += input[s].b.size() + erAp[s].b.size() + tankAp[s].b.size() + line1[s].b.size() + outer[s].b.size() +
                 inner[s].b.size() + line2[s].b.size();
            for (auto &d : diff[s])
                n += d.b.size();
        }
        return n * sizeof(float);
    }
    void prepare(double r)
    {
        rate = r;
        fadeLen = std::max(1, (int)std::lround(0.002 * rate));
        auto c = capacities(*fit, rate);
        size_t i = 0;
        for (int s = 0; s < 2; ++s)
        {
            input[s].reserve(c[i++]);
            for (auto &d : diff[s])
                d.reserve(c[i++]);
            for (Line *l : {&erAp[s], &tankAp[s], &line1[s], &outer[s], &inner[s], &line2[s]})
                l->reserve(c[i++]);
        }
        have = false;
    }
    void clear()
    {
        for (int s = 0; s < 2; ++s)
        {
            for (Line *l : {&input[s], &erAp[s], &tankAp[s], &line1[s], &outer[s], &inner[s], &line2[s]})
                l->clear();
            for (auto &d : diff[s])
                d.clear();
            for (FirstOrder *f : {&hp[s], &shelf[s], &lp1[s], &loopShelf[s], &outLp[s]})
                f->clear();
        }
    }
    void set(const Settings &q)
    {
        const Fit &f = *fit;
        const bool now = !have;
        have = true;
        const int n = fadeLen;
        const double S = sizeScale(f, std::clamp(q.size, 0.0, 31.0)), SL = lineScale(f, std::clamp(q.size, 0.0, 31.0)),
                     SA = tankApScale(f, std::clamp(q.size, 0.0, 31.0));
        // delays keep their 44.1 kHz length in seconds at other rates (decision 13)
        auto d = [&](double s, int cap) { return std::clamp((int)std::lround(std::round(s * 44100) / 44100 * rate), 1, cap); };
        const double shift = interp(f.decayAt, f.decayShiftMs, 5, q.decay) / 1000;
        const double base = q.initialDelayMs / 1000 + shift;
        for (int s = 0; s < 2; ++s)
        {
            const int cap = (int)input[s].b.size() - 1;
            for (int k = 0; k < 4; ++k)
                dEr[s][k] = d(base + (f.erFixedMs[s][k] + f.erScaledMs[s][k] * S) / 1000, cap);
            // the tank's feed starts after the first early reflection, so that reflection is the onset
            for (int k = 0; k < 3; ++k)
            {
                dDiff[s][k] = d(diffuserS[s][k] * S, (int)diff[s][k].b.size() - 1);
                dFeed[s][k] = d(base + (f.erFixedMs[s][0] + f.erScaledMs[s][0] * S) / 1000 + feedS[s][k] * S, cap);
            }
            dErAp[s] = d(erApS[s] * S, (int)erAp[s].b.size() - 1);
            dTankAp[s] = d(tankApS[s] * SA, (int)tankAp[s].b.size() - 1);
            dLine1[s] = d(tankLine1S[s] * SL, (int)line1[s].b.size() - 1);
            dOuter[s] = d(nestOuterS[s] * SA, (int)outer[s].b.size() - 1);
            dInner[s] = d(nestInnerS[s] * SA, (int)inner[s].b.size() - 1);
            dLine2[s] = d(tankLine2S[s] * SL, (int)line2[s].b.size() - 1);
        }
        for (int s = 0; s < 2; ++s) // after both sides' lines: the last tap reads the other side
        {
            dOut[s][0] = std::max(1, (int)std::lround(outTap[0] * dLine1[s]));
            dOut[s][1] = std::max(1, (int)std::lround(outFrac1b * dLine1[s]));
            dOut[s][2] = std::max(1, (int)std::lround(outTap[1] * dLine2[s]));
            dOut[s][3] = std::max(1, (int)std::lround(outTap[2] * dLine2[1 - s]));
        }
        onset = dEr[0][0] / rate;
        const double t = q.time;
        loopGain.to(interp(f.timeAt, f.loopGain, 10, t), n, now);
        level.to(std::pow(10.0, interp(f.timeAt, f.levelDb, 10, t) / 20), n, now);
        const double dk = std::pow(10.0, interp(f.decayAt, f.decayDb, 5, q.decay) / 20);
        const double first = interp(f.timeAt, f.firstTap, 10, t);
        for (int s = 0; s < 2; ++s)
            for (int k = 0; k < 4; ++k)
                erGain[s][k].to(f.erLevel * f.erGain[s][k] * (k >= 2 ? dk : 1) * (k == 0 ? first : 1), n, now);
        mix.to(std::clamp(std::round(q.diffusion), 0.0, 10.0) / 10, n, now);
        const int h = std::clamp((int)std::lround(q.highRatio * 10), 1, 10) - 1;
        const double inShelf = std::min(10 * q.lowRatio - 10, 2.0);
        const double loopShelfDb = interp(f.lowAt, f.loopShelfDb, 5, q.lowRatio);
        hpOn = q.hpf > 20.5;
        lpOn = q.lpf < 19999.5;
        for (int s = 0; s < 2; ++s)
        {
            hp[s].highPass(q.hpf, rate, n, now);
            // 800 Hz is the shelf's corner nearer DC, for a cut and a boost alike
            shelf[s].lowShelf(800 * std::pow(10.0, std::abs(inShelf) / 40), inShelf, rate, n, now);
            lp1[s].lowPass(f.highHz[h], rate, n, now);
            loopShelf[s].lowShelf(800, loopShelfDb, rate, n, now);
            outLp[s].lowPass(q.lpf, rate, n, now);
        }
    }

    // A Schroeder all-pass whose delayed sample z the caller read (and may have filtered).
    static inline double allpass(Line &l, double g, double x, double z)
    {
        const double v = x + g * z;
        l.push(v);
        return z - g * v;
    }

    void process(float *L, float *R, int frames)
    {
        const Fit &f = *fit;
        for (int i = 0; i < frames; ++i)
        {
            const double in[2] = {L[i], R[i]};
            const double fb = loopGain.next(), lv = level.next(), m = mix.next();
            double eg[2][4];
            for (int s = 0; s < 2; ++s)
                for (int k = 0; k < 4; ++k)
                    eg[s][k] = erGain[s][k].next();
            const double back[2] = {line2[0].read(dLine2[0]), line2[1].read(dLine2[1])};
            double out[2];
            for (int s = 0; s < 2; ++s)
            {
                double x = in[s];
                x = hp[s].run(x);
                if (!hpOn)
                    x = in[s];
                x = shelf[s].run(x);
                double u = x;
                for (int k = 0; k < 3; ++k)
                    u = allpass(diff[s][k], f.apGain[0], u, diff[s][k].read(dDiff[s][k]));
                const double v = m * u + (1 - m) * x;
                double er = 0, feed = 0;
                for (int k = 0; k < 4; ++k)
                    er += eg[s][k] * input[s].read(dEr[s][k]);
                for (int k = 0; k < 3; ++k)
                    feed += feedShape[k] * input[s].read(dFeed[s][k]);
                input[s].push(v);
                er = allpass(erAp[s], 0.5, er, erAp[s].read(dErAp[s]));
                // the tank: this side's half of the loop
                double a = feed + fb * back[1 - s];
                a = allpass(tankAp[s], f.apGain[1], a, tankAp[s].read(dTankAp[s]));
                a = lp1[s].run(a);
                const double b = line1[s].read(dLine1[s]);
                line1[s].push(a);
                const double zi = inner[s].read(dInner[s]);
                const double zo = allpass(inner[s], f.apGain[3], outer[s].read(dOuter[s]), zi);
                double c = allpass(outer[s], f.apGain[2], b, zo);
                c = loopShelf[s].run(c);
                line2[s].push(c);
                out[s] = er + outSign[0] * line1[s].read(dOut[s][0]) + outSign[1] * line1[s].read(dOut[s][1]) +
                         outSign[2] * line2[s].read(dOut[s][2]) + outSign[3] * line2[1 - s].read(dOut[s][3]);
            }
            for (int s = 0; s < 2; ++s)
            {
                double y = out[s];
                const double lo = outLp[s].run(y);
                if (lpOn)
                    y = 0.4 * y + 0.6 * lo;
                (s ? R : L)[i] = (float)(lv * y);
            }
        }
    }
};

template <typename FXConfig> struct HallReverb : sst::effects::core::EffectTemplateBase<FXConfig>
{
    enum params
    {
        time,
        diffusion,
        initialDelay,
        hpf,
        lpf,
        roomSize,
        highRatio,
        lowRatio,
        decay,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-hall-reverb"};
    static constexpr const char *displayName{"Hall A"};

    HallReverb(typename FXConfig::GlobalStorage *s, typename FXConfig::EffectStorage *e,
               typename FXConfig::ValueStorage *p)
        : sst::effects::core::EffectTemplateBase<FXConfig>(s, e, p)
    {
        core.prepare(this->sampleRate());
    }
    static size_t heapBytesAt(double rate) { return Core::heapBytesAt(fitted, rate); }
    size_t heapBytes() const { return core.heapBytes(); }
    double quietSeconds() const { return 0.1 + core.onset; }

    void initialize()
    {
        core.clear();
        core.have = false;
        have = false;
        update();
    }
    void suspendProcessing() { initialize(); }
    int getRingoutDecay() const { return -1; }
    void onSampleRateChanged() { initialize(); }

    void processBlock(float *__restrict L, float *__restrict R)
    {
        update();
        core.process(L, R, FXConfig::blockSize);
    }

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        switch (idx)
        {
        case time: return pmd().asFloat().withName("Reverb Time").withRange(0.3f, 18.f).withDefault(3.5f).withLinearScaleFormatting("s").withDecimalPlaces(1);
        case diffusion: return pmd().asInt().withName("Diffusion").withRange(0, 10).withDefault(10);
        case initialDelay: return pmd().asFloat().withName("Initial Delay").withRange(0.1f, 200.f).withDefault(67.8f).withLinearScaleFormatting("ms").withDecimalPlaces(1);
        case hpf: return pmd().asFloat().withName("HPF Cutoff").withRange(20.f, 8000.f).withDefault(80.f).withLinearScaleFormatting("Hz").withDecimalPlaces(0);
        case lpf: return pmd().asFloat().withName("LPF Cutoff").withRange(1000.f, 20000.f).withDefault(5600.f).withLinearScaleFormatting("Hz").withDecimalPlaces(0);
        case roomSize: return pmd().asInt().withName("Room Size").withRange(0, 31).withDefault(29);
        case highRatio: return pmd().asFloat().withName("High Ratio").withRange(0.1f, 1.f).withDefault(0.8f).withLinearScaleFormatting("").withDecimalPlaces(1);
        case lowRatio: return pmd().asFloat().withName("Low Ratio").withRange(0.1f, 1.4f).withDefault(0.7f).withLinearScaleFormatting("").withDecimalPlaces(1);
        case decay: return pmd().asInt().withName("Decay").withRange(0, 63).withDefault(55);
        }
        return pmd().withName("Unused").withType(pmd::NONE);
    }

    Core core;

  private:
    Settings last{};
    bool have{false};

    void update()
    {
        Settings q{this->floatValue(time), this->floatValue(diffusion), this->floatValue(initialDelay), this->floatValue(hpf),
                   this->floatValue(lpf), std::round(this->floatValue(roomSize)), this->floatValue(highRatio),
                   this->floatValue(lowRatio), std::round(this->floatValue(decay))};
        if (have && q == last)
            return;
        have = true;
        last = q;
        core.set(q);
    }
};
} // namespace oscairn_fx::hall

#endif
