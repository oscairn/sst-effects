/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * A five-band EQ: a low shelf, three peaking bands and a high shelf, then an output level.
 * Written for Oscairn, not part of sst-effects; it follows the sst-effects effect contract
 * (EffectCore.h) so the plugin wrapper runs it like any stock effect.
 *
 * Every section is a second-order bilinear filter designed at 44.1 kHz, the rate the reference
 * renders at (at other rates the plugin runs a cascade matched to that design, RateMatch.h); the
 * frequency, gain and Q laws were fitted to reference renders (plan, section 15, M1):
 *  - peaking bands form a boost-and-cut network, as console EQs do: with BP_i a unity-peak
 *    band-pass at band i's frequency, the bands give (1 + sum over boosts of (G_i - 1) BP_i) /
 *    (1 + sum over cuts of (1/G_i - 1) BP_i), G_i the band's linear gain. Alone, a band is a
 *    peaking section with effective Q = Q * 10^(-|gain| / 40) and a cut is the exact inverse of
 *    the same boost; together, overlapping boosts add rather than multiply. The band-pass Q is
 *    Q * r, r = min(1, 1.25517 - 4.8172e-5 * f / Hz - 0.1 * log10(Q)), which widens high,
 *    narrow bands;
 *  - shelves: slope 0.5 (Q from the slope and gain), with the set frequency as the corner on
 *    the flat side: the prewarped frequency is scaled by 10^(|gain| / 40), up for the low shelf
 *    and down for the high shelf, to give the half-gain point.
 */
#ifndef OSCAIRN_CLAP_EFFECTS_FIVEBANDEQ_H
#define OSCAIRN_CLAP_EFFECTS_FIVEBANDEQ_H

#include "sst/effects/EffectCore.h"
#include "sst/basic-blocks/params/ParamMetadata.h"
#include "RateMatch.h"
#include "../OffThread.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace oscairn_fx::eq
{
// One second-order section in transposed direct form II, double precision, two channels.
struct Biquad
{
    double b0{1}, b1{0}, b2{0}, a1{0}, a2{0};
    double z1[2]{}, z2[2]{};
    inline float run(int ch, float x)
    {
        const double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return (float)y;
    }
    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
    void set(double c0, double c1, double c2, double d0, double d1, double d2)
    {
        b0 = c0 / d0;
        b1 = c1 / d0;
        b2 = c2 / d0;
        a1 = d1 / d0;
        a2 = d2 / d0;
    }
};

// The fitted laws are for the 44.1 kHz design the reference renders at.
constexpr double designRate = 44100;

// At any other rate an EQ runs its 44.1 kHz sections' rate-matched replacement (RateMatch.h):
// up to three biquads per section. Section i of the design keeps slot i, so its biquads keep their
// states when other sections come and go.
//
// Matching costs 0.1-3 ms, longer than a block, so it runs off the audio thread (plan, decision
// 2): a change posts the new design to the engine's worker (OffThread.h) and the previous
// coefficients keep playing until the result arrives; it is swapped in at a block boundary with a
// short crossfade from the previous cascade. Only the newest design is matched, and a result is
// never applied over a newer one. Without a worker, or when the host renders offline, the match
// runs in process instead, so offline renders do not depend on timing. The first match (at
// initialize, on the main thread when the plugin builds the effect) is applied at once.
//
// Everything matching needs (58 KB of matcher scratch, the exchange buffers and the second
// cascade) is allocated by prepare, on the main thread, and only at rates other than 44.1 kHz.
template <int N> class MatchedCascade final : public Job
{
  public:
    static constexpr double fadeSeconds = 0.002;

    MatchedCascade() = default;
    MatchedCascade(const MatchedCascade &) = delete;
    MatchedCascade &operator=(const MatchedCascade &) = delete;
    ~MatchedCascade()
    {
        if (st && st->worker)
            st->worker->remove(this);
    }
    // At construction. Allocates only when rate is not 44.1 kHz; may throw.
    void prepare(double rate, Worker *worker, const std::atomic<bool> *offline)
    {
        if (rate == designRate)
            return;
        st = std::make_unique<State>();
        st->rate = rate;
        st->offline = offline;
        st->fadeLen = std::max(1, (int)std::lround(fadeSeconds * rate));
        if (worker)
        {
            worker->add(this);
            st->worker = worker;
        }
    }
    // What prepare allocates at a rate.
    static size_t heapBytesAt(double rate) { return rate == designRate ? 0 : sizeof(State); }
    size_t heapBytes() const { return st ? sizeof(State) : 0; }

    // Clears the filters' states; the coefficients stay, but a result the worker has matched and
    // not yet applied is applied now, without a crossfade (nothing is heard yet after a CLAP reset).
    void reset()
    {
        if (!st)
            return;
        auto &S = *st;
        S.cur.reset();
        S.old.reset();
        S.fade = 0;
        if (const Result *r = S.results.fresh() ? S.results.read() : nullptr; r && r->gen > S.applied)
            apply(*r, false);
    }

    // The 44.1 kHz design, one entry per slot, nullptr for a section left out (identity).
    void set(const Biquad *const *design)
    {
        auto &S = *st;
        Request req{};
        for (int i = 0; i < N; ++i)
            if (design[i])
            {
                const Biquad &d = *design[i];
                auto &x = req.s[req.count];
                x.b[0] = d.b0, x.b[1] = d.b1, x.b[2] = d.b2, x.a[1] = d.a1, x.a[2] = d.a2;
                req.slot[req.count++] = i;
            }
        if (S.have && sameDesign(req, S.want))
            return;
        S.want = req;
        req.gen = ++S.posted;
        if (!S.have)
        {
            // The first design: matched now and applied without a crossfade.
            S.have = true;
            matchExclusive(req);
            if (const Result *r = S.results.read())
                apply(*r, false);
            return;
        }
        if (!S.worker || (S.offline && S.offline->load(std::memory_order_relaxed)))
        {
            matchExclusive(req); // applied by process, at this block or after a crossfade
            return;
        }
        S.requests.writeSlot() = req;
        S.requests.publish();
        S.worker->wake();
    }

    // Runs a block through the cascade, scaled by gain (as the EQs did before the crossfade).
    template <typename G> void process(float *__restrict L, float *__restrict R, int n, G gain)
    {
        auto &S = *st;
        if (S.fade == 0)
            if (const Result *r = S.results.fresh() ? S.results.read() : nullptr; r && r->gen > S.applied)
                apply(*r, true);
        for (int i = 0; i < n; ++i)
        {
            const float l = S.cur.run(0, L[i]), r = S.cur.run(1, R[i]);
            if (S.fade == 0)
            {
                L[i] = (float)(l * gain);
                R[i] = (float)(r * gain);
                continue;
            }
            const double w = (double)S.fade / S.fadeLen;
            const double ol = S.old.run(0, L[i]), orr = S.old.run(1, R[i]);
            L[i] = (float)((l + w * (ol - l)) * gain);
            R[i] = (float)((r + w * (orr - r)) * gain);
            --S.fade;
        }
    }

    bool service() noexcept override
    {
        auto &S = *st;
        if (!S.requests.fresh() || S.busy.exchange(true, std::memory_order_acquire))
            return false;
        bool did = false;
        if (const Request *r = S.requests.read())
        {
            matchLocked(*r);
            did = true;
        }
        S.busy.store(false, std::memory_order_release);
        return did;
    }

  private:
    struct Cascade
    {
        Biquad sec[N][3];
        int cnt[N]{};
        inline float run(int ch, float x)
        {
            for (int s = 0; s < N; ++s)
                for (int k = 0; k < cnt[s]; ++k)
                    x = sec[s][k].run(ch, x);
            return x;
        }
        void reset()
        {
            for (auto &s : sec)
                for (auto &b : s)
                    b.reset();
        }
    };
    struct Request
    {
        rate::Section s[N];
        int slot[N];
        int count;
        uint64_t gen;
    };
    struct Result
    {
        rate::Section s[N][3];
        int cnt[N];
        uint64_t gen;
    };
    struct State
    {
        rate::Matcher matcher;
        TripleBuffer<Request> requests; // audio thread -> worker
        TripleBuffer<Result> results;   // whoever holds busy -> audio thread
        std::atomic<bool> busy{false};  // held while matching: the matcher and the results' writer
        Worker *worker{nullptr};
        const std::atomic<bool> *offline{nullptr};
        double rate{0};
        // audio thread (and the main thread before processing starts)
        Request want{};
        bool have{false};
        uint64_t posted{0}, applied{0};
        Cascade cur, old;
        int fade{0}, fadeLen{1};
    };
    std::unique_ptr<State> st;

    static bool sameDesign(const Request &a, const Request &b)
    {
        if (a.count != b.count)
            return false;
        for (int i = 0; i < a.count; ++i)
            if (a.slot[i] != b.slot[i] || !std::equal(a.s[i].b, a.s[i].b + 3, b.s[i].b) ||
                !std::equal(a.s[i].a, a.s[i].a + 3, b.s[i].a))
                return false;
        return true;
    }
    // Matches in this thread, waiting for the worker if it is matching (offline only, or before
    // processing starts).
    void matchExclusive(const Request &req)
    {
        while (st->busy.exchange(true, std::memory_order_acquire))
            std::this_thread::yield();
        matchLocked(req);
        st->busy.store(false, std::memory_order_release);
    }
    void matchLocked(const Request &req)
    {
        auto &S = *st;
        Result &out = S.results.writeSlot();
        rate::Section flat[3 * N];
        int counts[N]{};
        if (req.count > 0)
            S.matcher.match(req.s, req.count, S.rate, flat, req.slot, counts);
        std::fill(out.cnt, out.cnt + N, 0);
        for (int i = 0, at = 0; i < req.count; at += counts[i++])
        {
            out.cnt[req.slot[i]] = counts[i];
            std::copy(flat + at, flat + at + counts[i], out.s[req.slot[i]]);
        }
        out.gen = req.gen;
        S.results.publish();
    }
    // A slot's biquads keep their states where they remain; new ones start from silence.
    void apply(const Result &r, bool crossfade)
    {
        auto &S = *st;
        S.applied = r.gen;
        if (crossfade)
        {
            S.old = S.cur;
            S.fade = S.fadeLen;
        }
        for (int s = 0; s < N; ++s)
        {
            for (int k = 0; k < r.cnt[s]; ++k)
            {
                auto &b = S.cur.sec[s][k];
                if (k >= S.cur.cnt[s])
                    b.reset();
                b.b0 = r.s[s][k].b[0];
                b.b1 = r.s[s][k].b[1];
                b.b2 = r.s[s][k].b[2];
                b.a1 = r.s[s][k].a[1];
                b.a2 = r.s[s][k].a[2];
            }
            S.cur.cnt[s] = r.cnt[s];
        }
    }
};
// The network's three band-passes, weights and roles at 44.1 kHz as up to three biquads.
inline int networkBiquads(const Biquad *bp, const double *weight, const int *role, Biquad *out)
{
    rate::Section in[3], sec[3];
    for (int i = 0; i < 3; ++i)
    {
        in[i].b[0] = bp[i].b0;
        in[i].b[1] = bp[i].b1;
        in[i].b[2] = bp[i].b2;
        in[i].a[1] = bp[i].a1;
        in[i].a[2] = bp[i].a2;
    }
    const int n = rate::networkSections(in, weight, role, sec);
    for (int i = 0; i < n; ++i)
        out[i].set(sec[i].b[0], sec[i].b[1], sec[i].b[2], 1, sec[i].a[1], sec[i].a[2]);
    return n;
}
inline bool isIdentity(const Biquad &b) { return b.b0 == 1 && b.b1 == b.a1 && b.b2 == b.a2; }

// Q at `rate` for a peaking section whose 44.1 kHz design has centre hz and Q q: keeps the
// centre and both mid-gain edge frequencies (in Hz) of the 44.1 kHz response. The plugins now
// design at 44.1 kHz only (rate matching replaced this rule), so it returns q there. In the prewarped
// domain a peaking section's mid-gain edges W+ and W- satisfy W+ W- = 1 and W+ - W- = 1/Q.
inline double peakingQAt(double rate, double hz, double q)
{
    if (rate == designRate)
        return q;
    const double u = 1 / q, k1 = std::tan(M_PI * hz / designRate);
    const double root = std::sqrt(u * u + 4);
    const double hi = designRate / M_PI * std::atan(k1 * (root + u) / 2);
    const double lo = designRate / M_PI * std::atan(k1 * (root - u) / 2);
    const double ratio = std::tan(M_PI * hi / rate) / std::tan(M_PI * lo / rate);
    return 1 / (2 * std::sinh(std::log(ratio) / 2));
}

// Coefficients, exposed for the tests.
// widen: the five-band EQ's widening of high, narrow bands (the two-band EQ has none).
inline void peaking(Biquad &f, double rate, double hz, double gainDb, double q, bool widen = true)
{
    const double A = std::pow(10.0, gainDb / 40);
    hz = std::min(hz, 0.49 * designRate);
    const double r = widen ? std::clamp(1.25517 - 4.8172e-5 * hz - 0.1 * std::log10(q), 0.05, 1.0) : 1.0;
    const double qe = peakingQAt(rate, hz, q * std::pow(10.0, -std::abs(gainDb) / 40) * r);
    const double w0 = 2 * M_PI * hz / rate;
    const double al = std::sin(w0) / (2 * qe), c = std::cos(w0);
    f.set(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}

inline void shelf(Biquad &f, double rate, double hz, double gainDb, bool high)
{
    const double A = std::pow(10.0, std::abs(gainDb) / 40), G = std::pow(10.0, gainDb / 40);
    // The half-gain frequency of the 44.1 kHz design, kept in Hz at other rates.
    const double W = std::tan(M_PI * std::min(hz, 0.49 * designRate) / designRate) * (high ? 1 / A : A);
    const double halfGainHz = std::atan(W) * designRate / M_PI;
    const double w0 = 2 * M_PI * std::min(halfGainHz, 0.49 * rate) / rate;
    const double slope = 0.5, q = 1 / std::sqrt((G + 1 / G) * (1 / slope - 1) + 2);
    const double c = std::cos(w0), s2 = 2 * std::sqrt(G) * std::sin(w0) / (2 * q);
    if (high)
        f.set(G * ((G + 1) + (G - 1) * c + s2), -2 * G * ((G - 1) + (G + 1) * c),
              G * ((G + 1) + (G - 1) * c - s2), (G + 1) - (G - 1) * c + s2,
              2 * ((G - 1) - (G + 1) * c), (G + 1) - (G - 1) * c - s2);
    else
        f.set(G * ((G + 1) - (G - 1) * c + s2), 2 * G * ((G - 1) - (G + 1) * c),
              G * ((G + 1) - (G - 1) * c - s2), (G + 1) + (G - 1) * c + s2,
              -2 * ((G - 1) + (G + 1) * c), (G + 1) + (G - 1) * c - s2);
}

// First-order shelves, bilinear at 44.1 kHz with K = tan(pi f / 44100): a boost puts the pole at
// K and the zero at K x gain (low: towards DC; high: K / gain, towards Nyquist), a cut the zero
// at K and the pole beyond it (the five-band shelf at slope 0.5 has the same response). Rotary A and
// the auto pan mode use it.
inline void firstOrderShelf(Biquad &b, double hz, double db, bool high)
{
    const double K = std::tan(M_PI * std::min(hz, 0.49 * designRate) / designRate), A = std::pow(10.0, std::abs(db) / 20);
    // As (c1 s + c0) / (d1 s + d0), s = (1 - 1/z) / (1 + 1/z).
    double c1 = 1, c0 = K, d1 = 1, d0 = K;
    if (!high)
        (db >= 0 ? c0 : d0) = K * A;
    else
        (db >= 0 ? c1 : d1) = A;
    b.set(c1 + c0, c0 - c1, 0, d1 + d0, d0 - d1, 0);
}

// First-order bilinear low- and high-pass sections at hz (prewarped).
// A unity-peak band-pass (constant 0 dB peak gain) at hz with Q, for the five-band network: the
// Q is kept as the 44.1 kHz design's at other rates (peakingQAt).
inline void bandpass(Biquad &f, double rate, double hz, double q)
{
    hz = std::min(hz, 0.49 * designRate);
    const double r = std::clamp(1.25517 - 4.8172e-5 * hz - 0.1 * std::log10(q), 0.05, 1.0);
    const double qe = peakingQAt(rate, hz, q * r);
    const double w0 = 2 * M_PI * hz / rate, al = std::sin(w0) / (2 * qe);
    f.set(al, 0, -al, 1 + al, -2 * std::cos(w0), 1 - al);
}

inline void lowpass1(Biquad &f, double rate, double hz)
{
    const double K = std::tan(M_PI * std::min(hz, 0.49 * rate) / rate);
    f.set(K, K, 0, 1 + K, K - 1, 0);
}
inline void highpass1(Biquad &f, double rate, double hz)
{
    const double K = std::tan(M_PI * std::min(hz, 0.49 * rate) / rate);
    f.set(1, -1, 0, 1 + K, K - 1, 0);
}

template <typename FXConfig> struct FiveBandEq : sst::effects::core::EffectTemplateBase<FXConfig>
{
    enum params
    {
        lowFreq,
        lowGain,
        q2,
        freq2,
        gain2,
        q3,
        freq3,
        gain3,
        q4,
        freq4,
        gain4,
        highFreq,
        highGain,
        output,
        numParamsEnum
    };
    static constexpr int numParams{numParamsEnum};
    static constexpr const char *streamingName{"oscairn-five-band-eq"};
    static constexpr const char *displayName{"Five-band EQ"};

    FiveBandEq(typename FXConfig::GlobalStorage *s, typename FXConfig::EffectStorage *e,
               typename FXConfig::ValueStorage *p)
        : sst::effects::core::EffectTemplateBase<FXConfig>(s, e, p)
    {
        if constexpr (requires { s->worker; s->offline; })
            cascade.prepare(this->sampleRate(), s->worker, s->offline);
        else
            cascade.prepare(this->sampleRate(), nullptr, nullptr);
    }

    void initialize()
    {
        for (auto &b : shelves)
            b.reset();
        for (auto &b : peaks)
            b.reset();
        cascade.reset();
        last.fill(NAN);
        update();
    }
    void suspendProcessing() { initialize(); }
    int getRingoutDecay() const { return 32; }
    void onSampleRateChanged() { initialize(); }

    void processBlock(float *__restrict L, float *__restrict R)
    {
        update();
        if (matched)
            return cascade.process(L, R, FXConfig::blockSize, gain);
        for (int i = 0; i < FXConfig::blockSize; ++i)
        {
            L[i] = (float)(network(0, shelves[1].run(0, shelves[0].run(0, L[i]))) * gain);
            R[i] = (float)(network(1, shelves[1].run(1, shelves[0].run(1, R[i]))) * gain);
        }
    }

    static size_t heapBytesAt(double rate) { return decltype(cascade)::heapBytesAt(rate); }
    size_t heapBytes() const { return cascade.heapBytes(); }

    sst::basic_blocks::params::ParamMetaData paramAt(int idx) const
    {
        using pmd = sst::basic_blocks::params::ParamMetaData;
        auto hz = [](const char *n, float lo, float hi, float d) {
            return pmd().asFloat().withName(n).withRange(lo, hi).withDefault(d).withLinearScaleFormatting("Hz").withDecimalPlaces(1);
        };
        auto q = [](const char *n) {
            return pmd().asFloat().withName(n).withRange(0.5f, 16.f).withDefault(0.7f).withLinearScaleFormatting("").withDecimalPlaces(2);
        };
        auto db = [](const char *n, float r) {
            return pmd().asFloat().withName(n).withRange(-r, r).withDefault(0.f).withLinearScaleFormatting("dB").withDecimalPlaces(1);
        };
        switch (idx)
        {
        case lowFreq: return hz("Low Shelf Frequency", 20, 2000, 100);
        case lowGain: return db("Low Shelf Gain", 12);
        case q2: return q("Band 2 Q");
        case freq2: return hz("Band 2 Frequency", 20, 20000, 400);
        case gain2: return db("Band 2 Gain", 18);
        case q3: return q("Band 3 Q");
        case freq3: return hz("Band 3 Frequency", 20, 20000, 1000);
        case gain3: return db("Band 3 Gain", 18);
        case q4: return q("Band 4 Q");
        case freq4: return hz("Band 4 Frequency", 20, 20000, 4000);
        case gain4: return db("Band 4 Gain", 18);
        case highFreq: return hz("High Shelf Frequency", 500, 20000, 8000);
        case highGain: return db("High Shelf Gain", 12);
        case output: return db("Output Level", 12);
        }
        return pmd().withName("Unused").withType(pmd::NONE);
    }

  private:
    std::array<Biquad, 2> shelves;  // low, high
    std::array<Biquad, 3> peaks;    // band-passes of bands 2-4
    std::array<double, 3> weight{}; // G - 1 for a boost, 1/G - 1 for a cut, 0 when flat
    std::array<int, 3> role{};      // +1 boost, -1 cut, 0 flat
    std::array<float, numParamsEnum> last{};
    double gain{1};
    bool matched{false};          // running at another rate than 44.1 kHz: the cascade below
    MatchedCascade<5> cascade;    // shelves and the network, matched to the 44.1 kHz design

    // The boost sum, then the cut network's delay-free loop solved for its output z:
    // z = y - sum c_j BP_j(z), each BP_j(z) = b0_j z + its state.
    inline double network(int ch, double x)
    {
        double y = x;
        for (int b = 0; b < 3; ++b)
            if (role[b] > 0)
            {
                auto &f = peaks[b];
                const double bp = f.b0 * x + f.z1[ch];
                f.z1[ch] = -f.a1 * bp + f.z2[ch];
                f.z2[ch] = f.b2 * x - f.a2 * bp;
                y += weight[b] * bp;
            }
        double acc = y, b0sum = 0;
        for (int b = 0; b < 3; ++b)
            if (role[b] < 0)
            {
                acc -= weight[b] * peaks[b].z1[ch];
                b0sum += weight[b] * peaks[b].b0;
            }
        const double z = acc / (1 + b0sum);
        for (int b = 0; b < 3; ++b)
            if (role[b] < 0)
            {
                auto &f = peaks[b];
                const double bp = f.b0 * z + f.z1[ch];
                f.z1[ch] = -f.a1 * bp + f.z2[ch];
                f.z2[ch] = f.b2 * z - f.a2 * bp;
            }
        return z;
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
        const double running = this->sampleRate();
        matched = running != designRate;
        const double rate = designRate; // designed at 44.1 kHz; matched below at other rates
        shelf(shelves[0], rate, last[lowFreq], last[lowGain], false);
        shelf(shelves[1], rate, last[highFreq], last[highGain], true);
        const int fi[3] = {freq2, freq3, freq4}, gi[3] = {gain2, gain3, gain4}, qi[3] = {q2, q3, q4};
        for (int b = 0; b < 3; ++b)
        {
            const double G = std::pow(10.0, last[gi[b]] / 20);
            const int was = role[b];
            role[b] = last[gi[b]] > 0 ? 1 : (last[gi[b]] < 0 ? -1 : 0);
            weight[b] = role[b] > 0 ? G - 1 : (role[b] < 0 ? 1 / G - 1 : 0);
            if (role[b] != was)
                peaks[b].reset(); // its state belonged to the other side of the network
            bandpass(peaks[b], rate, last[fi[b]], last[qi[b]]);
        }
        gain = std::pow(10.0, last[output] / 20);
        if (matched)
        {
            Biquad net[3];
            const int nn = networkBiquads(peaks.data(), weight.data(), role.data(), net);
            // slots: the shelves, then the network's sections
            const Biquad *design[5]{};
            for (int i = 0; i < 2; ++i)
                design[i] = isIdentity(shelves[i]) ? nullptr : &shelves[i];
            for (int i = 0; i < nn; ++i)
                design[2 + i] = isIdentity(net[i]) ? nullptr : &net[i];
            cascade.set(design);
        }
    }
};
} // namespace oscairn_fx::eq

#endif
