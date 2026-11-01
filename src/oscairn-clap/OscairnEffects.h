/*
 * Oscairn effects: sst-effects as CLAP plugins for the Oscairn host (doc/oscairn-plan.md).
 * GPL-3.0-or-later, as the rest of this repository.
 */
#ifndef OSCAIRN_CLAP_OSCAIRNEFFECTS_H
#define OSCAIRN_CLAP_OSCAIRNEFFECTS_H

#include <clap/clap.h>
#include <cstdint>

#if defined(__aarch64__) || defined(_M_ARM64)
#elif defined(__SSE2__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

// The module's entry. The plugin module exports it as clap_entry; tests call it directly.
extern const clap_plugin_entry_t oscairn_effects_entry;

namespace oscairn_fx
{
// Samples of latency: the effects run in fixed blocks of this size (OscairnConfig.h).
constexpr uint32_t latencySamples = 16;
// Contexts per engine, including the one the host created (RFC 0007).
constexpr uint32_t contextCapacity = 64;

// Flush denormals to zero while in scope, as Surge does around its effects. The plugin
// processes under it; tests and the stock gate use it to render the templates the same way.
struct ScopedFlushDenormals
{
#if defined(__aarch64__) || defined(_M_ARM64)
    uint64_t saved;
    ScopedFlushDenormals()
    {
        asm volatile("mrs %0, fpcr" : "=r"(saved));
        uint64_t v = saved | (1ull << 24);
        asm volatile("msr fpcr, %0" : : "r"(v));
    }
    ~ScopedFlushDenormals() { asm volatile("msr fpcr, %0" : : "r"(saved)); }
#elif defined(__SSE2__) || defined(_M_X64)
    unsigned saved;
    ScopedFlushDenormals() : saved(_mm_getcsr()) { _mm_setcsr(saved | 0x8040); }
    ~ScopedFlushDenormals() { _mm_setcsr(saved); }
#endif
};
} // namespace oscairn_fx

#endif
