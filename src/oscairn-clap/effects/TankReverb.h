/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * Tank reverbs: Hall B, Stage A and Hall C. One structure, with constants per mode fitted to
 * reference renders. Per side:
 *  - the input passes a first-order HPF into a line holding the initial delay;
 *  - early reflections: six taps of the lines (each tap reads its own side or the other) at the
 *    initial delay plus fixed offsets, through an all-pass that diffusion crossfades in;
 *  - the tank is fed from the same lines at the initial delay + the reverb delay + a fixed offset,
 *    as the mid signal plus a mode's share of the side signal, through an all-pass whose gain
 *    follows the density;
 *  - the tank is a loop through both sides: an all-pass, a line, the damping low-pass (its cutoff
 *    set so the decay at 8 kHz is the high damp's share of the decay at 1 kHz), a low shelf (its
 *    gain set so the decay at DC is a fixed share of the decay at 1 kHz), an
 *    all-pass, a line, then the other side times the loop gain (from the reverb time and the loop's
 *    length);
 *  - the output is the reflections and taps of the tank's lines, balanced by ER/Rev balance, mixed
 *    across the sides, through a first-order LPF, times a level;
 *  - Mix (for an insertion; the reference's Dry/Wet) adds the input, unfiltered, to that output:
 *    dry and wet at cos and sin of (pi / 2)(v + 63) / 126, the tempo delays' measured law (not
 *    measured on these types). Its default, D<W63, is the reverb alone.
 * Nothing is modulated: the reference's response is time-invariant. Delays keep their 44.1 kHz
 * length in seconds at other rates, and the low-passes their 44.1 kHz design's response.
 */
#ifndef OSCAIRN_CLAP_EFFECTS_TANKREVERB_H
#define OSCAIRN_CLAP_EFFECTS_TANKREVERB_H

#include "effects/HallReverb.h"

#include <string>
#include <unordered_map>

namespace oscairn_fx::tank
{
using hall::FirstOrder;
using hall::Line;
using hall::Ramp;

constexpr int erTaps = 6, outTaps = 4;

// A mode's constants. Times in ms, delays of the tank in ms at 44.1 kHz.
struct Fit
{
    const char *name, *streamingName;
    // defaults: reverb time (s), diffusion, initial delay (ms), HPF (Hz), LPF (Hz), reverb delay
    // (ms), density, ER/Rev balance, high damp
    float defaults[9];
    double erMs[2][erTaps];  // after the initial delay
    double erGain[2][erTaps];
    int erFrom[2][erTaps];   // the side a tap reads
    double erApMs[2];
    double feedMs;           // the tank's feed, after initial delay + reverb delay
    double spread;           // share of the side signal in the tank's feed (0: mid only)
    double inApMs[2];
    double apAMs[2], line1Ms[2], apBMs[2], line2Ms[2];
    double apA, apB;         // the loop's all-pass gains
    double inApAt4;          // the feed's all-pass gain at density 4 (scales with density / 4)
    int outLine[2][outTaps]; // per tap: 0 line1 own side, 1 line2 own, 2 line1 other, 3 line2 other
    double outFrac[2][outTaps];
    double outGain[2][outTaps];
    double cross;            // the tank's output mixed across the sides
    double timeScale;        // loop gain: the decay at 1 kHz is reverb time x this
    double dampA, dampB;     // decay at 8 kHz / decay at 1 kHz = dampA + dampB x high damp
    double shelfHz, shelfRatio; // the loop's low shelf: decay at DC / decay at 1 kHz
    double erLevel, revLevel, levelDb;
};

// Hall B: the reference's hall of the shared program.
inline constexpr Fit hallB{
    "Hall B", "oscairn-tank-hall-b",
    {2.1f, 10, 12.7f, 90, 5600, 0.1f, 4, -14, 0.8f},
    {{0, 7.3, 15.9, 23.1, 34.7, 46.2}, {2.88, 10.18, 18.78, 25.98, 37.58, 49.08}},
    {{0.8689, 0.0005601, 0.006744, 1.182, 0.8031, 1.151}, {1.456, 0.0005601, 0.0005579, 0.7085, 1.999, 0.0005601}},
    {{0, 0, 0, 0, 0, 0}, {1, 1, 1, 1, 1, 1}},
    {4.7, 5.3},
    2.6, 0.5,
    {3.1, 3.5},
    {9.7, 12.9}, {107.3, 101.1}, {41.3, 57.9}, {89.2, 76.4},
    0.62, 0.5, 0.65,
    {{2, 0, 1, 2}, {2, 0, 1, 2}},
    {{0, 0.37, 0.21, 0.63}, {0, 0.41, 0.27, 0.58}},
    {{0.6785, -1.666, -1.667, -1.062}, {-0.06607, 0.5574, -1.355, -1.198}},
    0.1113, 0.9262, -0.07286, 1, 500, 1.235, 0.6589, 1.46, -6};

// Stage A: the reference's stage, the same program with other constants.
inline constexpr Fit stageA{
    "Stage A", "oscairn-tank-stage-a",
    {1.9f, 10, 25.3f, 45, 10000, 4.8f, 4, -12, 0.7f},
    {{11.11, 18.41, 27.01, 34.21, 45.81, 57.31}, {0, 7.3, 15.9, 23.1, 34.7, 46.2}},
    {{0.9222, 0.1638, 0.06581, 0.09128, 0.4712, 1.247}, {0.6224, 0.2847, 0.2879, 0.9279, 0.07658, 1.951}},
    {{0, 0, 0, 0, 0, 0}, {1, 1, 1, 1, 1, 1}},
    {4.7, 5.3},
    10.46, 0,
    {3.1, 3.5},
    {9.7, 12.9}, {107.3, 101.1}, {41.3, 57.9}, {89.2, 76.4},
    0.62, 0.5, 0.65,
    {{2, 0, 1, 2}, {2, 0, 1, 2}},
    {{0, 0.37, 0.21, 0.63}, {0, 0.41, 0.27, 0.58}},
    {{-0.203, -0.417, -1.999, -0.7961}, {-0.3704, 0.3449, -0.4488, 0.73}},
    0.1212, 0.9312, -0.0919, 1, 133.6, 0.7034, 0.3825, 2.296, -6};

// Hall C: the reference's other hall, a program of its own (a stereo tank feed, a later tank).
inline constexpr Fit hallC{
    "Hall C", "oscairn-tank-hall-c",
    {2.4f, 10, 45.7f, 280, 2800, 70.9f, 4, 9, 0.9f},
    {{0, 7.3, 15.9, 23.1, 34.7, 46.2}, {2.88, 10.18, 18.78, 25.98, 37.58, 49.08}},
    {{0.5815, 0.2074, 0.5997, 0.02081, 0.0004767, 2}, {0.5736, 0.1524, 0.4086, 0.864, 0.3024, 0.0003254}},
    {{0, 0, 0, 0, 0, 0}, {1, 1, 1, 1, 1, 1}},
    {4.7, 5.3},
    18.12, 1,
    {3.1, 3.5},
    {11.3, 14.9}, {131.3, 123.7}, {50.3, 67.1}, {109.9, 94.1},
    0.62, 0.5, 0.65,
    {{2, 0, 1, 2}, {2, 0, 1, 2}},
    {{0, 0.37, 0.21, 0.63}, {0, 0.41, 0.27, 0.58}},
    {{-0.2726, 1.299, -0.7717, 1.191}, {0.3799, 0.8679, -1.69, -0.2084}},
    0.1314, 0.9953, 0.1167, 0.9, 499.7, 1.098, 0.7768, 1.922, -6};

struct Settings
{
    double time{2}, diffusion{10}, initialDelayMs{10}, hpf{20}, lpf{20000}, reverbDelayMs{0.1}, density{4}, balance{0},
        highDamp{1}, mix{63};
    bool operator==(const Settings &) const = default;
};

// |H| at f of the first-order low-pass FirstOrder::lowPass designs at 44.1 kHz for cutoff hz.
inline double lowPassMag(double hz, double f)
{
    const double k = std::tan(M_PI * hz / 44100), w = 2 * M_PI * f / 44100;
    const double b = k / (1 + k), a = (k - 1) / (k + 1);
    // H = b (1 + z^-1) / (1 + a z^-1)
    const double nr = 1 + std::cos(w), ni = -std::sin(w), dr = 1 + a * std::cos(w), di = -a * std::sin(w);
    return b * std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}
// |H| at f of FirstOrder::lowShelf(hz, gDb) designed at 44.1 kHz.
inline double lowShelfMag(double hz, double gDb, double f)
{
    const double k = std::tan(M_PI * hz / 44100), s = std::pow(10.0, gDb / 40), d = k / s + 1;
    const double b0 = (k * s + 1) / d, b1 = (k * s - 1) / d, a1 = (k / s - 1) / d, w = 2 * M_PI * f / 44100;
    const double nr = b0 + b1 * std::cos(w), ni = -b1 * std::sin(w), dr = 1 + a1 * std::cos(w), di = -a1 * std::sin(w);
    return std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

// The damping low-pass's cutoff for a decay ratio r = T60(8 kHz) / T60(1 kHz), given the loop's
// log10 gain per pass at 1 kHz (lg1k < 0); 0 when no cutoff below 20 kHz gives that much damping.
inline double dampHz(double r, double lg1k)
{
    if (r >= 1 || lg1k >= 0)
        return 0;
    r = std::max(r, 0.02);
    // log10 |H(8k)| - log10 |H(1k)| = lg1k (1 / r - 1), negative; it grows with the cutoff
    const double want = lg1k * (1 / r - 1);
    auto excess = [](double hz) { return std::log10(lowPassMag(hz, 8000) / lowPassMag(hz, 1000)); };
    if (excess(20000) <= want)
        return 0;
    double lo = std::log(50.0), hi = std::log(20000.0);
    for (int i = 0; i < 40; ++i)
    {
        const double m = 0.5 * (lo + hi);
        (excess(std::exp(m)) < want ? lo : hi) = m;
    }
    return std::exp(0.5 * (lo + hi));
}

struct Core
{
    const Fit *fit{&hallB};
    double rate{44100};
    int fadeLen{1};
    Line pre[2], erAp[2], inAp[2], apA[2], line1[2], apB[2], line2[2];
    FirstOrder hp[2], damp[2], shelf[2], outLp[2];
    Ramp loopGain, erG, revG, mix, inG, level, crossG, dryG, wetG;
    int dEr[2][erTaps]{}, dErAp[2]{}, dFeed{1}, dInAp[2]{}, dApA[2]{}, dLine1[2]{}, dApB[2]{}, dLine2[2]{},
        dOut[2][outTaps]{}, outSrc[2][outTaps]{};
    bool hpOn{false}, lpOn{false}, have{false};
    double onset{0}, quiet{0};
    int ramping{0}; // samples left in the ramps a change started

    static constexpr double maxDelayMs = 200;
    static int samples(double ms, double rate) { return (int)std::ceil(ms / 1000 * rate) + 1; }
    static double maxEr(const Fit &f)
    {
        double m = 0;
        for (auto &s : f.erMs)
            for (double v : s)
                m = std::max(m, v);
        return m;
    }
    static std::vector<int> capacities(const Fit &f, double rate)
    {
        std::vector<int> c;
        for (int s = 0; s < 2; ++s)
        {
            c.push_back(samples(2 * maxDelayMs + std::max(maxEr(f), f.feedMs), rate));
            c.push_back(samples(f.erApMs[s], rate));
            c.push_back(samples(f.inApMs[s], rate));
            for (double v : {f.apAMs[s], f.line1Ms[s], f.apBMs[s], f.line2Ms[s]})
                c.push_back(samples(v, rate));
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
    // every line, in the order of capacities(); no allocation (clear() runs on reset)
    std::array<Line *, 14> lines()
    {
        std::array<Line *, 14> l;
        for (int s = 0, i = 0; s < 2; ++s)
            for (Line *x : {&pre[s], &erAp[s], &inAp[s], &apA[s], &line1[s], &apB[s], &line2[s]})
                l[(size_t)i++] = x;
        return l;
    }
    size_t heapBytes()
    {
        size_t n = 0;
        for (Line *l : lines())
            n += l->b.size();
        return n * sizeof(float);
    }
    void prepare(double r)
    {
        rate = r;
        // gains and coefficients ramp over 5 ms (2 ms, Hall A's, clicked at -57 dBFS in Hall C)
        fadeLen = std::max(1, (int)std::lround(0.005 * rate));
        auto c = capacities(*fit, rate);
        auto l = lines();
        for (size_t i = 0; i < l.size(); ++i)
            l[i]->reserve(c[i]);
        have = false;
    }
    void clear()
    {
        for (Line *l : lines())
            l->clear();
        for (int s = 0; s < 2; ++s)
            for (FirstOrder *f : {&hp[s], &damp[s], &shelf[s], &outLp[s]})
                f->clear();
    }
    // a delay in ms kept at its 44.1 kHz length in seconds at other rates (decision 13)
    int d(double ms, const Line &l) const
    {
        return std::clamp((int)std::lround(std::round(ms / 1000 * 44100) / 44100 * rate), 1, (int)l.b.size() - 1);
    }
    void set(const Settings &q)
    {
        const Fit &f = *fit;
        const bool now = !have;
        have = true;
        const int n = fadeLen;
        ramping = now ? 0 : n;
        const double id = std::clamp(q.initialDelayMs, 0.1, maxDelayMs), rd = std::clamp(q.reverbDelayMs, 0.1, maxDelayMs);
        double first = 1e9;
        for (int s = 0; s < 2; ++s)
        {
            for (int k = 0; k < erTaps; ++k)
            {
                dEr[s][k] = d(id + f.erMs[s][k], pre[0]);
                if (f.erGain[s][k] != 0)
                    first = std::min(first, dEr[s][k] / rate);
            }
            dErAp[s] = d(f.erApMs[s], erAp[s]);
            dInAp[s] = d(f.inApMs[s], inAp[s]);
            dApA[s] = d(f.apAMs[s], apA[s]);
            dLine1[s] = d(f.line1Ms[s], line1[s]);
            dApB[s] = d(f.apBMs[s], apB[s]);
            dLine2[s] = d(f.line2Ms[s], line2[s]);
        }
        dFeed = d(id + rd + f.feedMs, pre[0]);
        for (int s = 0; s < 2; ++s)
            for (int k = 0; k < outTaps; ++k)
            {
                const int w = f.outLine[s][k];
                const int len = (w & 1) ? dLine2[w < 2 ? s : 1 - s] : dLine1[w < 2 ? s : 1 - s];
                dOut[s][k] = std::max(1, (int)std::lround(f.outFrac[s][k] * len));
                outSrc[s][k] = 2 * (w < 2 ? s : 1 - s) + (w & 1); // into run()'s taps

            }
        onset = first;
        // the longest silence while sound is held: until the tank's first output
        quiet = std::max(first, (dFeed + dLine1[0] + dLine1[1]) / rate);

        // loop gain: the decay at 1 kHz is reverb time x timeScale over a pass of one side
        const double pass = 0.5 * (dApA[0] + dLine1[0] + dApB[0] + dLine2[0] + dApA[1] + dLine1[1] + dApB[1] + dLine2[1]) / rate;
        const double t = std::clamp(q.time, 0.3, 30.0) * f.timeScale;
        const double lg = -3 * pass / t;
        const double hz = dampHz(f.dampA + f.dampB * std::clamp(q.highDamp, 0.1, 1.0), lg);
        // the shelf's gain per pass sets the decay at DC to shelfRatio of the decay at 1 kHz, at any reverb time
        const double shelfDb = 20 * lg * (1 / f.shelfRatio - 1);
        const double at1k = (hz > 0 ? lowPassMag(hz, 1000) : 1) * lowShelfMag(f.shelfHz, shelfDb, 1000);
        // bounded: no frequency gains more than 0.995 per pass (the shelf lifts DC by shelfDb)
        const double g = std::min(std::pow(10.0, lg) / at1k, 0.995 / std::max(1.0, std::pow(10.0, shelfDb / 20)));
        loopGain.to(g, n, now);
        const double b = std::clamp(std::round(q.balance), -63.0, 63.0);
        erG.to(f.erLevel * std::min(1.0, (63 - b) / 63), n, now);
        revG.to(f.revLevel * std::min(1.0, (63 + b) / 63), n, now);
        mix.to(std::clamp(std::round(q.diffusion), 0.0, 10.0) / 10, n, now);
        inG.to(f.inApAt4 * std::clamp(std::round(q.density), 0.0, 4.0) / 4, n, now);
        level.to(std::pow(10.0, f.levelDb / 20), n, now);
        crossG.to(f.cross, n, now);
        // the ends exact, so D<W63 is the reverb alone, bit for bit
        const double mx = std::clamp(std::round(q.mix), -63.0, 63.0), xm = (mx + 63) / 126;
        dryG.to(mx >= 63 ? 0 : mx <= -63 ? 1 : std::cos(M_PI / 2 * xm), n, now);
        wetG.to(mx >= 63 ? 1 : mx <= -63 ? 0 : std::sin(M_PI / 2 * xm), n, now);
        hpOn = q.hpf > 20.5;
        lpOn = q.lpf < 19999.5;
        for (int s = 0; s < 2; ++s)
        {
            hp[s].highPass(q.hpf, rate, n, now);
            damp[s].lowPass(hz, rate, n, now);
            shelf[s].lowShelf(f.shelfHz, shelfDb, rate, n, now);
            outLp[s].lowPass(q.lpf, rate, n, now);
        }
    }

    static inline double allpass(Line &l, double g, double x, double z) { return hall::Core::allpass(l, g, x, z); }

    void process(float *L, float *R, int frames)
    {
        // while a change ramps every gain and coefficient steps per sample; after that they are
        // read as they stand (the same values: a finished ramp's next() is its value)
        const int k = std::min(ramping, frames);
        if (k > 0)
            run<true, true>(L, R, 0, k);
        // the reverb alone (Mix at D<W63) needs no dry path
        if (dryG.v == 0 && wetG.v == 1)
            run<false, false>(L, R, k, frames);
        else
            run<false, true>(L, R, k, frames);
        ramping -= k;
    }
    template <bool ramp> static inline double at(Ramp &r)
    {
        if constexpr (ramp)
            return r.next();
        return r.v;
    }
    template <bool ramp> static inline double run(FirstOrder &f, double x)
    {
        if constexpr (ramp)
            return f.run(x);
        const double y = f.b0.v * x + f.b1.v * f.x1 - f.a1.v * f.y1;
        f.x1 = x, f.y1 = y;
        return y;
    }
    template <bool ramp, bool dry> void run(float *L, float *R, int from, int to)
    {
        const Fit &f = *fit;
        const Line *taps[4] = {&line1[0], &line2[0], &line1[1], &line2[1]};
        for (int i = from; i < to; ++i)
        {
            const double gl = at<ramp>(loopGain), eg = at<ramp>(erG), rg = at<ramp>(revG), m = at<ramp>(mix),
                         ig = at<ramp>(inG), lv = at<ramp>(level), cg = at<ramp>(crossG);
            double dg = 0, wg = 1;
            if constexpr (dry)
                dg = at<ramp>(dryG), wg = at<ramp>(wetG);
            // while ramping, wet only exactly as without the dry path once the gains are there
            const bool wetOnly = !dry || (dg == 0 && wg == 1);
            const double in[2] = {L[i], R[i]};
            double x[2] = {in[0], in[1]};
            for (int s = 0; s < 2; ++s)
            {
                const double h = run<ramp>(hp[s], x[s]);
                if (hpOn)
                    x[s] = h;
            }
            // the tank's feed and the reflections, read before this sample is pushed
            const double fl = pre[0].read(dFeed), fr = pre[1].read(dFeed);
            const double mid = 0.5 * (fl + fr), side = 0.5 * f.spread * (fl - fr);
            double feed[2] = {mid + side, mid - side}, er[2];
            for (int s = 0; s < 2; ++s)
            {
                double e = 0;
                for (int k = 0; k < erTaps; ++k)
                    e += f.erGain[s][k] * pre[f.erFrom[s][k]].read(dEr[s][k]);
                const double u = allpass(erAp[s], 0.6, e, erAp[s].read(dErAp[s]));
                er[s] = m * u + (1 - m) * e;
                feed[s] = allpass(inAp[s], ig, feed[s], inAp[s].read(dInAp[s]));
            }
            pre[0].push(x[0]);
            pre[1].push(x[1]);
            const double back[2] = {line2[0].read(dLine2[0]), line2[1].read(dLine2[1])};
            for (int s = 0; s < 2; ++s)
            {
                double a = feed[s] + gl * back[1 - s];
                a = allpass(apA[s], f.apA, a, apA[s].read(dApA[s]));
                double b = line1[s].read(dLine1[s]);
                line1[s].push(a);
                b = run<ramp>(shelf[s], run<ramp>(damp[s], b));
                line2[s].push(allpass(apB[s], f.apB, b, apB[s].read(dApB[s])));
            }
            double t[2] = {0, 0};
            for (int s = 0; s < 2; ++s)
                for (int k = 0; k < outTaps; ++k)
                    t[s] += f.outGain[s][k] * taps[outSrc[s][k]]->read(dOut[s][k]);
            for (int s = 0; s < 2; ++s)
            {
                // the tank's outputs mix across the sides; the reflections stay on their own side
                double y = eg * er[s] + rg * (t[s] - cg * t[1 - s]);
                const double lo = run<ramp>(outLp[s], y);
                if (lpOn)
                    y = lo;
                (s ? R : L)[i] = (float)(wetOnly ? lv * y : wg * (lv * y) + dg * in[s]);
            }
        }
    }
};

template <typename FXConfig, const Fit &F> struct TankReverb : sst::effects::core::EffectTemplateBase<FXConfig>
{
    enum params
    {
        time,
        diffusion,
        initialDelay,
        hpf,
        lpf,
        reverbDelay,
        density,
        balance,
        highDamp,
        mixParam,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{F.streamingName};
    static constexpr const char *displayName{F.name};

    TankReverb(typename FXConfig::GlobalStorage *s, typename FXConfig::EffectStorage *e, typename FXConfig::ValueStorage *p)
        : sst::effects::core::EffectTemplateBase<FXConfig>(s, e, p)
    {
        core.fit = &F;
        core.prepare(this->sampleRate());
    }
    static size_t heapBytesAt(double rate) { return Core::heapBytesAt(F, rate); }
    size_t heapBytes() const { return const_cast<Core &>(core).heapBytes(); }
    double quietSeconds() const { return 0.1 + core.quiet; }

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
        const float *d = F.defaults;
        switch (idx)
        {
        case time: return pmd().asFloat().withName("Reverb Time").withRange(0.3f, 30.f).withDefault(d[0]).withLinearScaleFormatting("s").withDecimalPlaces(1);
        case diffusion: return pmd().asInt().withName("Diffusion").withRange(0, 10).withDefault(d[1]);
        case initialDelay: return pmd().asFloat().withName("Initial Delay").withRange(0.1f, 200.f).withDefault(d[2]).withLinearScaleFormatting("ms").withDecimalPlaces(1);
        case hpf: return pmd().asFloat().withName("HPF Cutoff").withRange(20.f, 8000.f).withDefault(d[3]).withLinearScaleFormatting("Hz").withDecimalPlaces(0);
        case lpf: return pmd().asFloat().withName("LPF Cutoff").withRange(1000.f, 20000.f).withDefault(d[4]).withLinearScaleFormatting("Hz").withDecimalPlaces(0);
        case reverbDelay: return pmd().asFloat().withName("Reverb Delay").withRange(0.1f, 200.f).withDefault(d[5]).withLinearScaleFormatting("ms").withDecimalPlaces(1);
        case density: return pmd().asInt().withName("Density").withRange(0, 4).withDefault(d[6]);
        case balance: return pmd().asInt().withName("ER/Rev Balance").withRange(-63, 63).withDefault(d[7]);
        case highDamp: return pmd().asFloat().withName("High Damp").withRange(0.1f, 1.f).withDefault(d[8]).withLinearScaleFormatting("").withDecimalPlaces(1);
        case mixParam:
        {
            std::unordered_map<int, std::string> m;
            for (int v = -63; v <= 63; ++v)
                m[v] = v < 0 ? "D" + std::to_string(-v) + ">W" : (v == 0 ? "D=W" : "D<W" + std::to_string(v));
            return pmd().asInt().withName("Mix").withRange(-63, 63).withDefault(63).withUnorderedMapFormatting(m);
        }
        }
        return pmd().withName("Unused").withType(pmd::NONE);
    }

    Core core;

  private:
    Settings last{};
    bool have{false};

    void update()
    {
        Settings q{this->floatValue(time),        this->floatValue(diffusion), this->floatValue(initialDelay),
                   this->floatValue(hpf),         this->floatValue(lpf),       this->floatValue(reverbDelay),
                   std::round(this->floatValue(density)), std::round(this->floatValue(balance)), this->floatValue(highDamp),
                   std::round(this->floatValue(mixParam))};
        if (have && q == last)
            return;
        have = true;
        last = q;
        core.set(q);
    }
};

template <typename C> using HallB = TankReverb<C, hallB>;
template <typename C> using StageA = TankReverb<C, stageA>;
template <typename C> using HallC = TankReverb<C, hallC>;
} // namespace oscairn_fx::tank

#endif
