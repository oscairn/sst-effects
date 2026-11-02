/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 * Sample-rate matching for the fitted EQs (plan, decision 13): at any sample rate a fitted EQ
 * must sound as its 44.1 kHz design does. A bilinear section designed at another rate cannot
 * follow the 44.1 kHz design's warping near 20 kHz, so each 44.1 kHz section is replaced, at the
 * running rate, by a cascade of one to three biquads fitted to its magnitude response:
 *
 *  - grid: 48 log-spaced points from 20 Hz to 20 kHz, 12 more from 14 to 20 kHz, and 17 around
 *    each resonance of the section; above 20 kHz, 24 points up to Nyquist hold the 44.1 kHz
 *    response's continuation (up to 22.05 kHz, then its value there) loosely, and must not
 *    exceed it by more than 1 dB;
 *  - tolerance per point: 0.01 dB where the whole EQ is within 20 dB of its peak, 0.1 dB below;
 *  - starts, tried in order until one meets the tolerance (else the best is kept): the
 *    section's own roots moved to keep their frequencies in Hz; a Sanathanan-Koerner
 *    least-squares fit of |H|^2 as a ratio of polynomials of degree 2 in x = sin^2(pi f / rate),
 *    spectrally factored; the first with one more resonant pair near 22.05 kHz; Sanathanan-
 *    Koerner fits of degree 4 and 6;
 *  - each start is polished by Levenberg-Marquardt on the dB error, parametrised as analog
 *    quadratics at the running rate (log natural frequency and damping), which always give a
 *    stable, real biquad cascade by the bilinear transform. Poles above 20 kHz keep a damping of
 *    at least 0.05, and no pole is sharper than half the section's sharpest.
 *
 * Fixed-size arrays only; nothing allocates. The five-band EQ's band network is factored into
 * biquads first (networkSections). The reference implementation the tests were developed with
 * is described in the plan, section 15.
 */
#ifndef OSCAIRN_CLAP_EFFECTS_RATEMATCH_H
#define OSCAIRN_CLAP_EFFECTS_RATEMATCH_H

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>

namespace oscairn_fx::eq::rate
{
using C = std::complex<double>;
constexpr double designRate = 44100;

// A second-order section b0 + b1 z^-1 + b2 z^-2 over 1 + a1 z^-1 + a2 z^-2.
struct Section
{
    double b[3]{1, 0, 0}, a[3]{1, 0, 0};
};

inline C response(const Section &s, double f, double rate)
{
    const C z1 = std::polar(1.0, -2 * M_PI * f / rate), z2 = z1 * z1;
    return (s.b[0] + s.b[1] * z1 + s.b[2] * z2) / (s.a[0] + s.a[1] * z1 + s.a[2] * z2);
}
inline double responseDb(const Section *s, int n, double f, double rate)
{
    C h = 1;
    for (int i = 0; i < n; ++i)
        h *= response(s[i], f, rate);
    return 20 * std::log10(std::abs(h) + 1e-300);
}
// The same at z^-1 = z1 (precomputed).
inline double responseDbZ(const Section *s, int n, C z1)
{
    const C z2 = z1 * z1;
    C num = 1, den = 1;
    for (int i = 0; i < n; ++i)
    {
        num *= s[i].b[0] + s[i].b[1] * z1 + s[i].b[2] * z2;
        den *= s[i].a[0] + s[i].a[1] * z1 + s[i].a[2] * z2;
    }
    return 10 * std::log10(std::norm(num) / std::norm(den) + 1e-300);
}

// ---- polynomials ----
// Roots of c[0] x^n + c[1] x^(n-1) + ... + c[n] (n <= 8), by the Aberth-Ehrlich iteration.
inline int roots(const double *c, int n, C *out)
{
    while (n > 0 && c[0] == 0)
    {
        ++c;
        --n;
    }
    if (n <= 0)
        return 0;
    if (n == 1)
    {
        out[0] = -c[1] / c[0];
        return 1;
    }
    if (n == 2)
    {
        const C d = std::sqrt(C(c[1] * c[1] - 4 * c[0] * c[2]));
        // the numerically stable pair
        const C q = -0.5 * (c[1] + (c[1] >= 0 ? d : -d));
        out[0] = q / c[0];
        out[1] = std::abs(q) > 0 ? c[2] / q : C(0);
        return 2;
    }
    double a[9], da[9];
    for (int i = 0; i <= n; ++i)
        a[i] = c[i] / c[0];
    for (int i = 0; i < n; ++i)
        da[i] = a[i] * (n - i);
    // initial guesses on a circle of the roots' geometric mean radius
    const double r0 = std::pow(std::max(std::abs(a[n]), 1e-300), 1.0 / n);
    for (int i = 0; i < n; ++i)
        out[i] = std::polar(r0 * (1 + 0.1 * i / n), 2 * M_PI * (i + 0.25) / n + 0.4);
    bool done[8] = {};
    for (int it = 0; it < 200; ++it)
    {
        bool all = true;
        for (int i = 0; i < n; ++i)
        {
            if (done[i])
                continue;
            C p = 1, dp = da[0];
            for (int k = 1; k <= n; ++k)
                p = p * out[i] + a[k];
            for (int k = 1; k < n; ++k)
                dp = dp * out[i] + da[k];
            if (std::abs(p) == 0)
            {
                done[i] = true;
                continue;
            }
            const C ratio = p / dp;
            C sum = 0;
            for (int j = 0; j < n; ++j)
                if (j != i)
                    sum += 1.0 / (out[i] - out[j]);
            const C w = ratio / (1.0 - ratio * sum);
            out[i] -= w;
            done[i] = std::abs(w) <= 1e-13 * std::max(std::abs(out[i]), 1e-300);
            all = all && done[i];
        }
        if (all)
            break;
    }
    // conjugate symmetry: snap nearly real roots
    for (int i = 0; i < n; ++i)
        if (std::abs(out[i].imag()) < 1e-12 * std::max(1.0, std::abs(out[i])))
            out[i] = out[i].real();
    return n;
}

// ---- analog quadratics at the running rate: s^2 + 2 zeta w0 s + w0^2 ----
struct Quad
{
    double w0, ze; // ze < 0 marks s (s + w0): a zero at DC (first-order high-pass)
};
constexpr int maxQuads = 3;

// Analog roots (one per entry, conjugates both present, infinite allowed as NaN-free large
// values) into quadratics; a lone real root is paired with s = -2 rate.
inline int rootsToQuads(const C *s, int n, double rate, Quad *q)
{
    int nq = 0;
    double real[8];
    int nr = 0, atDc = 0;
    for (int i = 0; i < n; ++i)
    {
        if (!std::isfinite(s[i].real()) || !std::isfinite(s[i].imag()))
            continue;
        if (std::abs(s[i]) < 1e-9 * rate)
        {
            ++atDc;
            continue;
        }
        if (s[i].imag() > 1e-9 * std::max(1.0, std::abs(s[i])))
            q[nq++] = {std::abs(s[i]), -s[i].real() / std::abs(s[i])};
        else if (std::abs(s[i].imag()) <= 1e-9 * std::max(1.0, std::abs(s[i])))
            real[nr++] = s[i].real();
    }
    std::sort(real, real + nr);
    for (; atDc > 0; --atDc) // s (s - u) with the nearest real root, else s (s + 2 rate)
        q[nq++] = {std::max(nr > 0 ? -real[--nr] : (atDc > 1 ? (--atDc, 1.0) : 2 * rate), 1.0), -1.0};
    while (nr >= 2)
    {
        const double u = real[--nr], v = real[--nr], w0 = std::sqrt(std::abs(u * v));
        q[nq++] = {w0, -(u + v) / (2 * w0)};
    }
    if (nr == 1)
    {
        const double u = real[0], v = -2 * rate, w0 = std::sqrt(std::abs(u * v));
        q[nq++] = {w0, -(u + v) / (2 * w0)};
    }
    return nq;
}

inline double quadDb(double w0, double ze, double W)
{
    const double d = w0 * w0 - W * W, e = 2 * ze * w0 * W;
    return 10 * std::log10(d * d + e * e + 1e-300);
}

// The bilinear transform of the quadratics at rate: one biquad per zero/pole pair, the gain
// (dB) on the first.
// s (s + c): a zero at DC and one at -c.
inline double dcDb(double c, double W) { return 10 * std::log10(W * W * (W * W + c * c) + 1e-300); }

inline void toSections(const double *p, int nq, double rate, Section *out, const bool *dc)
{
    const double K = 2 * rate;
    for (int i = 0; i < nq; ++i)
    {
        // zeros s^2 + c1 s + c0: c1 = 2 zeta w0, c0 = w0^2, or for a zero at DC c1 = w0, c0 = 0
        const double wz = std::exp(p[1 + 2 * i]), zz = p[2 + 2 * i];
        const double c1 = dc[i] ? wz : 2 * zz * wz, c0 = dc[i] ? 0 : wz * wz;
        const double wp = std::exp(p[1 + 2 * (nq + i)]), zp = p[2 + 2 * (nq + i)];
        const double d0 = K * K + 2 * zp * wp * K + wp * wp;
        out[i].b[0] = (K * K + c1 * K + c0) / d0;
        out[i].b[1] = (2 * c0 - 2 * K * K) / d0;
        out[i].b[2] = (K * K - c1 * K + c0) / d0;
        out[i].a[0] = 1;
        out[i].a[1] = (2 * wp * wp - 2 * K * K) / d0;
        out[i].a[2] = (K * K - 2 * zp * wp * K + wp * wp) / d0;
    }
    const double g = std::pow(10.0, p[0] / 20);
    for (double &b : out[0].b)
        b *= g;
}

// ---- the matcher ----
class Matcher
{
  public:
    static constexpr int maxGrid = 48 + 12 + 4 * 17, maxHold = 12, maxRows = maxGrid + 2 * maxHold + maxQuads;
    static constexpr int maxParams = 1 + 4 * maxQuads, dense = 256;
    static constexpr double tolMain = 0.01, tolDeep = 0.1, tolHold = 6.0;

    // Fits every 44.1 kHz section of an EQ (all of `secs`, n of them) at `rate`. out receives up
    // to 3 sections per input section; returns their count. slots (optional) names each input
    // section's place in the EQ, which keys the previous fit used as the next start (default: its
    // index); counts (optional) receives the number of output sections of each input section.
    int match(const Section *secs, int n, double rate, Section *out, const int *slots = nullptr, int *counts = nullptr)
    {
        all = secs;
        nAll = n;
        lastWorstScore = 0;
        // the dense check grid, and the whole EQ's peak on it for the 20 dB rule
        const double top = std::min(20000.0, 0.45 * rate);
        peak = -1e300;
        double tot[dense];
        for (int k = 0; k < dense; ++k)
        {
            fd[k] = 20 * std::pow(top / 20, k / (dense - 1.0));
            zd44[k] = std::polar(1.0, -2 * M_PI * fd[k] / designRate);
            zdR[k] = std::polar(1.0, -2 * M_PI * fd[k] / rate);
            tot[k] = responseDbZ(secs, n, zd44[k]);
            peak = std::max(peak, tot[k]);
        }
        for (int k = 0; k < dense; ++k)
            told[k] = tot[k] > peak - 20 ? tolMain : tolDeep;
        int count = 0;
        bool used[maxSections]{};
        for (int i = 0; i < n; ++i)
        {
            const int slot = slots ? slots[i] : i;
            const int c = fitSection(secs[i], slot, rate, out + count);
            if (slot >= 0 && slot < maxSections)
                used[slot] = true;
            if (counts)
                counts[i] = c;
            count += c;
        }
        for (int i = 0; i < maxSections; ++i)
            if (!used[i])
                prev[i].nq = 0;
        return count;
    }

    // Last fit's score (the worst error over tolerance, <= 1 met) per section, for the tests.
    double lastWorstScore{0};

  private:
    const Section *all{nullptr};
    int nAll{0};
    double peak{0};

    double f[maxGrid + maxHold], t[maxGrid + maxHold], w[maxGrid + maxHold], W[maxGrid + maxHold];
    int ng{0}, nh{0};
    double cap{0}, capLoose{0}, W20{0}, zmin{0};
    double fd[dense], td[dense], told[dense];
    C zd44[dense], zdR[dense], zg44[maxGrid], zgR[maxGrid];
    static constexpr int ultra = 64;   // the check above 20 kHz
    C zuR[ultra];
    double secTop{0};
    int nu{0};
    double A[maxGrid + maxHold][13];  // least-squares scratch
    double J[maxRows][maxParams];      // Jacobian scratch

    bool isMain(C z44) const { return responseDbZ(all, nAll, z44) > peak - 20; }

    // Complex roots of a section's numerator or denominator (in z).
    static int sectionRoots(const double *p, C *r)
    {
        const double c[3] = {p[0], p[1], p[2]};
        int n = 2;
        const double *cc = c;
        while (n > 0 && cc[n] == 0) // trailing zeros: roots at z = 0 dropped
            --n;
        return roots(cc, n, r);
    }

    // The previous fit of each section, as a start for the next (parameter changes are mostly
    // small steps).
    struct Previous
    {
        double p[maxParams];
        bool dc[maxQuads]{};
        int nq{0}, tier{2};
        double rate{0};
        Section design;
    };
    bool dcq[maxQuads]{}; // the zero quadratics of the current fit that are s (s + c)
    static constexpr int maxSections = 5;
    Previous prev[maxSections];

    int fitSection(const Section &s, int index, double rate, Section *out)
    {
        auto same = [](const Section &a, const Section &b) {
            return std::equal(a.b, a.b + 3, b.b) && std::equal(a.a, a.a + 3, b.a);
        };
        buildGrid(s, rate);
        // unchanged: the previous result, if it still meets the (whole EQ's) tolerance
        if (index < maxSections && prev[index].nq > 0 && prev[index].rate == rate && same(prev[index].design, s))
        {
            Section trial[maxQuads];
            toSections(prev[index].p, prev[index].nq, rate, trial, prev[index].dc);
            int tier;
            const double sc = score(trial, prev[index].nq, rate, tier);
            if (sc <= 1 && tier <= prev[index].tier)
            {
                lastWorstScore = std::max(lastWorstScore, sc);
                std::copy(trial, trial + prev[index].nq, out);
                return prev[index].nq;
            }
        }
        // Candidates rank by: meeting the in-band tolerance, then their tier above 20 kHz, then the
        // in-band score.
        double bestScore = 1e300, p[maxParams], bestP[maxParams];
        bool bestDc[maxQuads]{};
        int bestNq = 0, bestTier = 3, extra = 0;
        auto consider = [&](int nq) {
            Section trial[maxQuads];
            toSections(p, nq, rate, trial, dcq);
            int tier;
            const double sc = score(trial, nq, rate, tier);
            const bool meets = sc <= 1, bestMeets = bestScore <= 1;
            if ((meets && !bestMeets) || (meets == bestMeets && (meets ? (tier < bestTier || (tier == bestTier && sc < bestScore)) : sc < bestScore)))
            {
                bestScore = sc;
                bestTier = tier;
                bestNq = nq;
                std::copy(p, p + 1 + 4 * nq, bestP);
                std::copy(dcq, dcq + maxQuads, bestDc);
            }
            // done when the tolerance and the tight bound above 20 kHz are met, or two more starts
            // after the tolerance alone was
            return bestScore <= 1 && (bestTier == 0 || ++extra > 2);
        };
        bool done = false;
        auto near = [](const Section &a, const Section &b) {
            double d = 0;
            for (int i = 0; i < 3; ++i)
                d = std::max({d, std::abs(a.b[i] - b.b[i]) / std::max(1.0, std::abs(a.b[i])), std::abs(a.a[i] - b.a[i])});
            return d < 0.02;
        };
        if (index < maxSections && prev[index].nq > 0 && prev[index].rate == rate && near(prev[index].design, s))
        {
            const int nq = prev[index].nq;
            std::copy(prev[index].p, prev[index].p + 1 + 4 * nq, p);
            std::copy(prev[index].dc, prev[index].dc + maxQuads, dcq);
            for (int i = nq; i < 2 * nq; ++i)
                p[2 + 2 * i] = std::max(p[2 + 2 * i], zmin);
            lm(p, nq, 10);
            done = consider(nq);
        }
        Quad zq[maxQuads + 1], pq[maxQuads + 1];
        for (int start = 0; start < 5 && !done; ++start)
        {
            int nz = 0, np = 0;
            switch (start)
            {
            case 0: warpedStart(s, rate, zq, nz, pq, np); break;
            case 1: skStart(2, rate, zq, nz, pq, np); break;
            case 2:
                warpedStart(s, rate, zq, nz, pq, np);
                if (nz < maxQuads && np < maxQuads)
                {
                    const double WN = 2 * rate * std::tan(M_PI * std::min(22050.0, 0.45 * rate) / rate);
                    zq[nz++] = {WN, 1.0};
                    pq[np++] = {WN, 0.3};
                }
                break;
            case 3: skStart(4, rate, zq, nz, pq, np); break;
            case 4: skStart(6, rate, zq, nz, pq, np); break;
            }
            if (nz == 0 && np == 0)
                continue;
            const int nq = initParams(zq, nz, pq, np, rate, p);
            if (consider(nq)) // the start alone may already do
                break;
            lm(p, nq, 100);
            done = consider(nq);
        }
        lastWorstScore = std::max(lastWorstScore, bestScore);
        if (index < maxSections)
        {
            prev[index].nq = bestNq;
            prev[index].rate = rate;
            prev[index].design = s;
            std::copy(bestP, bestP + 1 + 4 * bestNq, prev[index].p);
            std::copy(bestDc, bestDc + maxQuads, prev[index].dc);
            prev[index].tier = bestTier;
        }
        toSections(bestP, bestNq, rate, out, bestDc);
        return bestNq;
    }

    void buildGrid(const Section &s, double rate)
    {
        const double top = std::min(20000.0, 0.45 * rate);
        ng = 0;
        for (int k = 0; k < 48; ++k)
            f[ng++] = 20 * std::pow(top / 20, k / 47.0);
        for (int k = 0; k < 12; ++k)
            f[ng++] = top * (0.7 + 0.3 * k / 12);
        C r[2];
        for (const double *p : {s.b, s.a})
        {
            const int n = sectionRoots(p, r);
            for (int i = 0; i < n; ++i)
                if (r[i].imag() > 1e-9)
                {
                    const double th = std::arg(r[i]), f0 = th * designRate / (2 * M_PI);
                    const double bw = std::clamp(-std::log(std::abs(r[i])) / th, 1e-3, 2.0);
                    for (int u = 0; u < 17; ++u)
                    {
                        const double v = f0 * std::exp((-4 + 0.5 * u) * bw);
                        if (v >= 20 && v <= top && ng < maxGrid)
                            f[ng++] = v;
                    }
                }
        }
        secTop = -1e300;
        for (int k = 0; k < ng; ++k)
        {
            zg44[k] = std::polar(1.0, -2 * M_PI * f[k] / designRate);
            zgR[k] = std::polar(1.0, -2 * M_PI * f[k] / rate);
            t[k] = responseDbZ(&s, 1, zg44[k]);
            secTop = std::max(secTop, t[k]);
            w[k] = 1 / (isMain(zg44[k]) ? tolMain : tolDeep);
        }
        nh = 0;
        double holdTop = -1e300;
        if (0.5 * rate > top * 1.0001)
            for (int k = 1; k <= maxHold; ++k)
            {
                const double hz = top * std::pow(0.5 * rate / top, k / double(maxHold));
                f[ng + nh] = hz;
                t[ng + nh] = std::max(responseDb(&s, 1, std::min(hz, 0.5 * designRate), designRate), secTop - 60);
                w[ng + nh] = 1 / tolHold;
                holdTop = std::max(holdTop, t[ng + nh]);
                ++nh;
            }
        // above 20 kHz at most 1 dB over the 44.1 kHz design between 20 and 22.05 kHz (so that
        // sections whose continuations cancel in the product stay cancelled)
        cap = (nh > 0 ? holdTop : secTop) + 1;
        capLoose = std::max(secTop, holdTop) + 1;
        for (int k = 0; k < ng + nh; ++k)
            W[k] = 2 * rate * std::tan(M_PI * std::min(f[k], 0.4999 * rate) / rate);
        W20 = 2 * rate * std::tan(M_PI * top / rate);
        // no pole sharper than half the section's sharpest (at most 0.03)
        zmin = 0.03;
        const int n = sectionRoots(s.a, r);
        for (int i = 0; i < n; ++i)
            if (r[i].imag() > 1e-9 && std::abs(r[i]) > 1e-9)
            {
                const double lr = std::log(std::abs(r[i]));
                zmin = std::min(zmin, 0.5 * -lr / std::hypot(lr, std::arg(r[i])));
            }
        for (int k = 0; k < dense; ++k)
            td[k] = responseDbZ(&s, 1, zd44[k]);
        nu = 0;
        if (0.5 * rate > top * 1.0001)
            for (int k = 1; k <= ultra; ++k, ++nu)
            {
                const double hz = top * std::pow(0.4999 * rate / top, k / double(ultra));
                zuR[nu] = std::polar(1.0, -2 * M_PI * hz / rate);
            }
    }

    // The worst in-band error over its tolerance (<= 1 meets it); tier: 0 when above 20 kHz the
    // response stays within 1 dB of the design's 20-22.05 kHz level (cap), 1 within 1 dB of the
    // section's peak (capLoose), 2 otherwise.
    double score(const Section *secs, int n, double rate, int &tier) const
    {
        tier = 2;
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < 3; ++j)
                if (!std::isfinite(secs[i].b[j]) || !std::isfinite(secs[i].a[j]))
                    return HUGE_VAL;
        double sc = 0;
        for (int k = 0; k < ng; ++k)
            sc = std::max(sc, std::abs(responseDbZ(secs, n, zgR[k]) - t[k]) * w[k]);
        for (int k = 0; k < dense; ++k)
            sc = std::max(sc, std::abs(responseDbZ(secs, n, zdR[k]) - td[k]) / told[k]);
        double over = -HUGE_VAL;
        for (int k = 0; k < nu; ++k)
            over = std::max(over, responseDbZ(secs, n, zuR[k]));
        tier = !(over > cap) ? 0 : (!(over > capLoose) ? 1 : 2);
        return sc;
    }

    // The section's analog roots at 44.1 kHz, each moved to keep its frequency in Hz.
    static void warpedStart(const Section &s, double rate, Quad *zq, int &nz, Quad *pq, int &np)
    {
        auto warpMag = [rate](double m) {
            return 2 * rate * std::tan(std::atan(m / (2 * designRate)) * designRate / rate);
        };
        auto conv = [&](const double *p, Quad *q) {
            C r[2], sr[2];
            const int n = sectionRoots(p, r);
            bool nyq = false, zero = false;
            for (int i = 0; i < n; ++i)
                nyq = nyq || std::abs(r[i] + 1.0) < 1e-9;
            zero = (p[2] == 0);
            if (nyq && (n == 1 || zero)) // a root at Nyquist (first-order low-pass): a notch at 22.05 kHz
            {
                int nq = 0;
                C rest[2];
                int nr = 0;
                for (int i = 0; i < n; ++i)
                    if (!(std::abs(r[i] + 1.0) < 1e-9) && std::abs(r[i]) > 1e-12)
                        rest[nr++] = 2 * designRate * (r[i] - 1.0) / (r[i] + 1.0);
                for (int i = 0; i < nr; ++i)
                    rest[i] = rest[i] / std::abs(rest[i]) * warpMag(std::abs(rest[i]));
                nq = rootsToQuads(rest, nr, rate, q);
                q[nq++] = {warpMag(1e15), 0.0};
                return nq;
            }
            int m = 0;
            for (int i = 0; i < n; ++i)
            {
                if (std::abs(r[i] + 1.0) < 1e-9)
                {
                    sr[m++] = -warpMag(1e15);
                    continue;
                }
                const C sa = 2 * designRate * (r[i] - 1.0) / (r[i] + 1.0);
                const double mag = std::abs(sa);
                sr[m++] = mag > 0 ? sa / mag * warpMag(mag) : C(0);
            }
            return rootsToQuads(sr, m, rate, q);
        };
        nz = conv(s.b, zq);
        np = conv(s.a, pq);
    }

    // Sanathanan-Koerner fit of |H|^2 = N(x) / D(x), degree m, then spectral factorisation.
    void skStart(int m, double rate, Quad *zq, int &nz, Quad *pq, int &np)
    {
        const int rows = ng + nh, cols = 2 * m + 1;
        double x[maxGrid + maxHold], T[maxGrid + maxHold], Dp[maxGrid + maxHold];
        for (int k = 0; k < rows; ++k)
        {
            const double sn = std::sin(M_PI * std::min(f[k], 0.5 * rate) / rate);
            x[k] = sn * sn;
            T[k] = std::pow(10.0, t[k] / 10);
            Dp[k] = 1;
        }
        double n[7], d[7];
        for (int it = 0; it < 6; ++it)
        {
            // rows: s (sum n_j x^j - T sum_{j>=1} d_j x^j) = s T, s = w / (T Dp)
            double b[maxGrid + maxHold];
            for (int k = 0; k < rows; ++k)
            {
                const double s = w[k] / (T[k] * Dp[k]);
                double xp = 1;
                for (int j = 0; j <= m; ++j)
                {
                    A[k][j] = s * xp;
                    if (j >= 1)
                        A[k][m + j] = -s * T[k] * xp;
                    xp *= x[k];
                }
                b[k] = s * T[k];
            }
            double sol[13];
            if (!leastSquares(A, b, rows, cols, sol))
                break;
            for (int j = 0; j <= m; ++j)
                n[j] = sol[j];
            d[0] = 1;
            for (int j = 1; j <= m; ++j)
                d[j] = sol[m + j];
            for (int k = 0; k < rows; ++k)
            {
                double v = 0, xp = 1;
                for (int j = 0; j <= m; ++j, xp *= x[k])
                    v += d[j] * xp;
                Dp[k] = std::abs(v);
            }
        }
        nz = factor(n, m, rate, zq);
        np = factor(d, m, rate, pq);
    }

    // Roots in x of a polynomial (increasing powers) moved off (0, 1) where they change the
    // sign (adjacent pairs to a double root at their midpoint, a lone one to 1), then each mapped
    // to its left-half-plane analog root at rate.
    static int factor(const double *c, int m, double rate, Quad *q)
    {
        double desc[7];
        for (int j = 0; j <= m; ++j)
            desc[j] = c[m - j];
        C r[6];
        const int n = roots(desc, m, r);
        C kept[6];
        double inside[6];
        int nk = 0, ni = 0;
        for (int i = 0; i < n; ++i)
            if (r[i].imag() == 0 && r[i].real() > 0 && r[i].real() < 1)
                inside[ni++] = r[i].real();
            else
                kept[nk++] = r[i];
        std::sort(inside, inside + ni);
        int i = 0;
        for (; i + 1 < ni; i += 2)
        {
            kept[nk++] = 0.5 * (inside[i] + inside[i + 1]);
            kept[nk++] = 0.5 * (inside[i] + inside[i + 1]);
        }
        if (i < ni)
            kept[nk++] = 1.0;
        C s[6];
        bool flip = false;
        for (int j = 0; j < nk; ++j)
        {
            const C cc = 1.0 - 2.0 * kept[j], dsc = std::sqrt(cc * cc - 1.0);
            const C z1 = cc + dsc, z2 = cc - dsc;
            C z;
            if (std::abs(std::abs(z1) - std::abs(z2)) < 1e-12)
            {
                z = flip ? z2 : z1;
                flip = !flip;
            }
            else
                z = std::abs(z1) < std::abs(z2) ? z1 : z2;
            s[j] = std::abs(z + 1.0) > 1e-12 ? 2 * rate * (z - 1.0) / (z + 1.0) : C(HUGE_VAL, 0); // at infinity: left out
        }
        return std::min(rootsToQuads(s, nk, rate, q), maxQuads);
    }

    // Householder QR least squares with column scaling; false if rank-deficient.
    static bool leastSquares(double (*A)[13], double *b, int rows, int cols, double *sol)
    {
        double scale[13];
        for (int j = 0; j < cols; ++j)
        {
            double s = 0;
            for (int k = 0; k < rows; ++k)
                s += A[k][j] * A[k][j];
            scale[j] = s > 0 ? 1 / std::sqrt(s) : 1;
            for (int k = 0; k < rows; ++k)
                A[k][j] *= scale[j];
        }
        for (int j = 0; j < cols; ++j)
        {
            double norm = 0;
            for (int k = j; k < rows; ++k)
                norm += A[k][j] * A[k][j];
            norm = std::sqrt(norm);
            if (norm < 1e-300)
                return false;
            const double alpha = A[j][j] > 0 ? -norm : norm;
            double v0 = A[j][j] - alpha;
            // v = (v0, A[j+1..][j]); H = I - 2 v v^T / (v^T v)
            double vtv = v0 * v0;
            for (int k = j + 1; k < rows; ++k)
                vtv += A[k][j] * A[k][j];
            if (vtv < 1e-300)
                continue;
            for (int c = j + 1; c < cols; ++c)
            {
                double dot = v0 * A[j][c];
                for (int k = j + 1; k < rows; ++k)
                    dot += A[k][j] * A[k][c];
                const double fct = 2 * dot / vtv;
                A[j][c] -= fct * v0;
                for (int k = j + 1; k < rows; ++k)
                    A[k][c] -= fct * A[k][j];
            }
            double dot = v0 * b[j];
            for (int k = j + 1; k < rows; ++k)
                dot += A[k][j] * b[k];
            const double fct = 2 * dot / vtv;
            b[j] -= fct * v0;
            for (int k = j + 1; k < rows; ++k)
                b[k] -= fct * A[k][j];
            A[j][j] = alpha;
        }
        for (int j = cols - 1; j >= 0; --j)
        {
            double v = b[j];
            for (int c = j + 1; c < cols; ++c)
                v -= A[j][c] * sol[c];
            if (std::abs(A[j][j]) < 1e-300)
                return false;
            sol[j] = v / A[j][j];
        }
        for (int j = 0; j < cols; ++j)
            sol[j] *= scale[j];
        return true;
    }

    // Residuals and their Jacobian for the parameters p (nq zero and nq pole quadratics).
    int residuals(const double *p, int nq, double *r, double (*J)[maxParams]) const
    {
        constexpr double c10 = 10 / 2.302585092994046;
        const int np = 1 + 4 * nq;
        int row = 0;
        double w0s[2 * maxQuads];
        for (int i = 0; i < 2 * nq; ++i)
            w0s[i] = std::exp(p[1 + 2 * i]);
        for (int k = 0; k < ng + nh; ++k)
        {
            double m = p[0];
            double g[maxParams];
            if (J)
                g[0] = 1;
            for (int i = 0; i < 2 * nq; ++i)
            {
                if (i < nq && dcq[i])
                {
                    const double c = w0s[i], Wk = W[k], Q = Wk * Wk * (Wk * Wk + c * c) + 1e-300;
                    m += c10 * std::log(Q);
                    if (J)
                    {
                        g[1 + 2 * i] = c10 * 2 * c * c / (Wk * Wk + c * c);
                        g[2 + 2 * i] = 0;
                    }
                    continue;
                }
                const double w0 = w0s[i], ze = p[2 + 2 * i], Wk = W[k];
                const double d = w0 * w0 - Wk * Wk, e = 2 * ze * w0 * Wk, Q = d * d + e * e + 1e-300;
                const double sg = i < nq ? 1 : -1;
                m += sg * c10 * std::log(Q);
                if (J)
                {
                    g[1 + 2 * i] = sg * c10 * (4 * w0 * w0 * d + 8 * ze * ze * w0 * w0 * Wk * Wk) / Q;
                    g[2 + 2 * i] = sg * c10 * (8 * ze * w0 * w0 * Wk * Wk) / Q;
                }
            }
            r[row] = (m - t[k]) * w[k];
            if (J)
                for (int j = 0; j < np; ++j)
                    J[row][j] = g[j] * w[k];
            ++row;
            if (k >= ng) // above 20 kHz: at most 1 dB over the hold
            {
                const bool over = m > capLoose;
                r[row] = over ? (m - capLoose) * 10 : 0;
                if (J)
                    for (int j = 0; j < np; ++j)
                        J[row][j] = over ? g[j] * 10 : 0;
                ++row;
            }
        }
        for (int i = nq; i < 2 * nq; ++i) // poles above 20 kHz stay damped
        {
            const bool act = std::exp(p[1 + 2 * i]) > W20 && p[2 + 2 * i] < 0.05;
            r[row] = act ? (0.05 - p[2 + 2 * i]) * 100 : 0;
            if (J)
            {
                for (int j = 0; j < np; ++j)
                    J[row][j] = 0;
                if (act)
                    J[row][2 + 2 * i] = -100;
            }
            ++row;
        }
        return row;
    }

    int initParams(Quad *zq, int nz, Quad *pq, int np, double rate, double *p)
    {
        while (nz < np && nz < maxQuads)
            zq[nz++] = {4 * rate, 1.0};
        while (np < nz && np < maxQuads)
            pq[np++] = {4 * rate, 1.0};
        const int nq = std::min(std::min(nz, np), maxQuads);
        for (int i = 0; i < 2 * nq; ++i)
        {
            const Quad &q = i < nq ? zq[i] : pq[i - nq];
            if (i < nq)
                dcq[i] = q.ze < 0;
            p[1 + 2 * i] = std::log(std::max(q.w0, 1e-9));
            p[2 + 2 * i] = i < nq && dcq[i] ? 0.0 : std::max(q.ze, i < nq ? 0.0 : zmin);
        }
        // gain: the median offset
        double off[maxGrid + maxHold];
        for (int k = 0; k < ng + nh; ++k)
        {
            double m = 0;
            for (int i = 0; i < 2 * nq; ++i)
                m += (i < nq ? 1 : -1) * (i < nq && dcq[i] ? dcDb(std::exp(p[1 + 2 * i]), W[k])
                                                          : quadDb(std::exp(p[1 + 2 * i]), p[2 + 2 * i], W[k]));
            off[k] = t[k] - m;
        }
        std::nth_element(off, off + (ng + nh) / 2, off + ng + nh);
        p[0] = off[(ng + nh) / 2];
        return nq;
    }

    // Levenberg-Marquardt from p, at most maxIter iterations.
    void lm(double *p, int nq, int maxIter)
    {
        const int npar = 1 + 4 * nq;
        double lo[maxParams];
        lo[0] = -1e300;
        for (int i = 0; i < 2 * nq; ++i)
        {
            lo[1 + 2 * i] = -1e300;
            lo[2 + 2 * i] = i < nq ? 0.0 : zmin;
        }
        double r[maxRows], rt[maxRows], q[maxParams];
        int rows = residuals(p, nq, r, J);
        double e = 0;
        for (int k = 0; k < rows; ++k)
            e += r[k] * r[k];
        double lambda = 1e-3;
        for (int it = 0; it < maxIter && e > 1e-6; ++it)
        {
            const double e0 = e;
            double H[maxParams][maxParams], g[maxParams];
            for (int i = 0; i < npar; ++i)
            {
                g[i] = 0;
                for (int k = 0; k < rows; ++k)
                    g[i] += J[k][i] * r[k];
                for (int j = 0; j <= i; ++j)
                {
                    double s = 0;
                    for (int k = 0; k < rows; ++k)
                        s += J[k][i] * J[k][j];
                    H[i][j] = H[j][i] = s;
                }
            }
            bool improved = false;
            for (int tries = 0; tries < 8 && !improved; ++tries)
            {
                double M[maxParams][maxParams + 1];
                for (int i = 0; i < npar; ++i)
                {
                    for (int j = 0; j < npar; ++j)
                        M[i][j] = H[i][j] + (i == j ? lambda * (H[i][i] + 1e-12) : 0);
                    M[i][npar] = -g[i];
                }
                double step[maxParams];
                if (!solve(M, npar, step))
                {
                    lambda *= 10;
                    continue;
                }
                for (int i = 0; i < npar; ++i)
                    q[i] = std::max(p[i] + step[i], lo[i]);
                residuals(q, nq, rt, nullptr);
                double eq = 0;
                for (int k = 0; k < rows; ++k)
                    eq += rt[k] * rt[k];
                if (eq < e && std::isfinite(eq))
                {
                    std::copy(q, q + npar, p);
                    e = eq;
                    lambda = std::max(lambda / 3, 1e-9);
                    improved = true;
                }
                else
                    lambda *= 10;
            }
            if (!improved)
                break;
            const double before = e0;
            rows = residuals(p, nq, r, J);
            // converged: in-band errors within a third of their tolerance and no constraint
            // active, or less than 0.1% progress
            double worstBand = 0, worstRest = 0;
            for (int k = 0, row = 0; k < ng + nh; ++k)
            {
                (k < ng ? worstBand : worstRest) = std::max(k < ng ? worstBand : worstRest, k < ng ? std::abs(r[row]) : 0.0);
                row += k < ng ? 1 : 2;
                if (k >= ng)
                    worstRest = std::max(worstRest, std::abs(r[row - 1]));
            }
            for (int i = 0; i < nq; ++i)
                worstRest = std::max(worstRest, std::abs(r[rows - nq + i]));
            if ((worstBand < 0.7 && worstRest == 0) || e > 0.999 * before)
                break;
        }
    }

    // Gaussian elimination with partial pivoting on an n x (n + 1) system.
    static bool solve(double (*M)[maxParams + 1], int n, double *x)
    {
        for (int i = 0; i < n; ++i)
        {
            int piv = i;
            for (int k = i + 1; k < n; ++k)
                if (std::abs(M[k][i]) > std::abs(M[piv][i]))
                    piv = k;
            if (std::abs(M[piv][i]) < 1e-300)
                return false;
            if (piv != i)
                for (int j = 0; j <= n; ++j)
                    std::swap(M[i][j], M[piv][j]);
            for (int k = i + 1; k < n; ++k)
            {
                const double fct = M[k][i] / M[i][i];
                for (int j = i; j <= n; ++j)
                    M[k][j] -= fct * M[i][j];
            }
        }
        for (int i = n - 1; i >= 0; --i)
        {
            double v = M[i][n];
            for (int j = i + 1; j < n; ++j)
                v -= M[i][j] * x[j];
            x[i] = v / M[i][i];
        }
        return true;
    }
};

// ---- the five-band network as biquads ----
// bp: the three band-passes at 44.1 kHz; weight: G - 1 for a boost, 1/G - 1 for a cut, role +1,
// -1 or 0. Writes up to three sections (their product is the network) and returns the count.
inline int networkSections(const Section *bp, const double *weight, const int *role, Section *out)
{
    // polynomials in z^-1, up to degree 6
    auto mul = [](const double *a, int na, const double *b, int nb, double *c) {
        for (int i = 0; i <= na + nb; ++i)
            c[i] = 0;
        for (int i = 0; i <= na; ++i)
            for (int j = 0; j <= nb; ++j)
                c[i + j] += a[i] * b[j];
        return na + nb;
    };
    auto part = [&](int side, double *num, int &dn, double *den, int &dd) {
        num[0] = den[0] = 1;
        dn = dd = 0;
        for (int i = 0; i < 3; ++i)
            if (role[i] == side)
            {
                double tmp[7];
                dd = mul(den, dd, bp[i].a, 2, tmp);
                std::copy(tmp, tmp + dd + 1, den);
            }
        std::copy(den, den + dd + 1, num);
        dn = dd;
        for (int i = 0; i < 3; ++i)
            if (role[i] == side)
            {
                double term[7] = {weight[i] * bp[i].b[0], weight[i] * bp[i].b[1], weight[i] * bp[i].b[2]};
                int dt = 2;
                for (int j = 0; j < 3; ++j)
                    if (j != i && role[j] == side)
                    {
                        double tmp[7];
                        dt = mul(term, dt, bp[j].a, 2, tmp);
                        std::copy(tmp, tmp + dt + 1, term);
                    }
                for (int k = 0; k <= dt; ++k)
                    num[k] += term[k];
            }
    };
    double PB[7], DB[7], PC[7], DC[7], num[13], den[13];
    int nPB, nDB, nPC, nDC;
    part(1, PB, nPB, DB, nDB);
    part(-1, PC, nPC, DC, nDC);
    const int nn = mul(PB, nPB, DC, nDC, num), nd = mul(DB, nDB, PC, nPC, den);
    if (nd == 0)
    {
        out[0] = Section{};
        out[0].b[0] = num[0];
        return 1;
    }
    C zr[6], pr[6];
    const int nzr = roots(num, nn, zr), npr = roots(den, nd, pr);
    // Sections: each takes two poles and two zeros. Conjugate pairs stay together; real roots
    // (zeros padded with roots at z = 0 up to the pole count) may go to any section. The grouping
    // with the least root distance between each section's zeros and poles wins.
    C pc[3], zc[3];
    double prl[6], zrl[6];
    int npc = 0, nprl = 0, nzc = 0, nzrl = 0;
    for (int i = 0; i < npr; ++i)
        if (pr[i].imag() > 0)
            pc[npc++] = pr[i];
        else if (pr[i].imag() == 0)
            prl[nprl++] = pr[i].real();
    for (int i = 0; i < nzr; ++i)
        if (zr[i].imag() > 0)
            zc[nzc++] = zr[i];
        else if (zr[i].imag() == 0)
            zrl[nzrl++] = zr[i].real();
    if (nprl % 2)
        prl[nprl++] = 0;
    const int npq = npc + nprl / 2;
    while (2 * nzc + nzrl < 2 * npq && nzrl < 6)
        zrl[nzrl++] = 0;
    auto cost = [](C a, C b, const C *p) {
        return std::min(std::abs(a - p[0]) + std::abs(b - p[1]), std::abs(a - p[1]) + std::abs(b - p[0]));
    };
    C zq[3][2], pq[3][2], bestZ[3][2], bestP[3][2];
    double bestCost = 1e300;
    bool usedPC[3] = {}, usedPR[6] = {}, usedC[3] = {}, usedR[6] = {};
    auto assign = [&](auto &&self, int k, double acc) -> void {
        if (acc >= bestCost)
            return;
        if (k == npq)
        {
            bestCost = acc;
            std::copy(&zq[0][0], &zq[0][0] + 6, &bestZ[0][0]);
            std::copy(&pq[0][0], &pq[0][0] + 6, &bestP[0][0]);
            return;
        }
        // the next pole group, in a canonical order: the first unused conjugate pair, else the
        // first unused real root with any other
        auto zeros = [&](double accP) {
            for (int i = 0; i < nzc; ++i)
                if (!usedC[i])
                {
                    usedC[i] = true;
                    zq[k][0] = zc[i];
                    zq[k][1] = std::conj(zc[i]);
                    self(self, k + 1, accP + cost(zq[k][0], zq[k][1], pq[k]));
                    usedC[i] = false;
                }
            for (int i = 0; i < nzrl; ++i)
                for (int j = i + 1; j < nzrl; ++j)
                    if (!usedR[i] && !usedR[j])
                    {
                        usedR[i] = usedR[j] = true;
                        zq[k][0] = zrl[i];
                        zq[k][1] = zrl[j];
                        self(self, k + 1, accP + cost(zq[k][0], zq[k][1], pq[k]));
                        usedR[i] = usedR[j] = false;
                    }
        };
        for (int i = 0; i < npc; ++i)
            if (!usedPC[i])
            {
                usedPC[i] = true;
                pq[k][0] = pc[i];
                pq[k][1] = std::conj(pc[i]);
                zeros(acc);
                usedPC[i] = false;
                return;
            }
        int first = 0;
        while (first < nprl && usedPR[first])
            ++first;
        if (first >= nprl)
            return;
        usedPR[first] = true;
        for (int j = first + 1; j < nprl; ++j)
            if (!usedPR[j])
            {
                usedPR[j] = true;
                pq[k][0] = prl[first];
                pq[k][1] = prl[j];
                zeros(acc);
                usedPR[j] = false;
            }
        usedPR[first] = false;
    };
    assign(assign, 0, 0.0);
    for (int k = 0; k < npq; ++k)
    {
        const C *z = bestZ[k], *p = bestP[k];
        out[k].b[0] = 1;
        out[k].b[1] = -(z[0] + z[1]).real();
        out[k].b[2] = (z[0] * z[1]).real();
        out[k].a[0] = 1;
        out[k].a[1] = -(p[0] + p[1]).real();
        out[k].a[2] = (p[0] * p[1]).real();
    }
    const double g = num[0] / den[0];
    for (double &b : out[0].b)
        b *= g;
    return npq;
}
} // namespace oscairn_fx::eq::rate

#endif
