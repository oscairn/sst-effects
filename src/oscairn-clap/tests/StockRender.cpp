/*
 * The stock-vs-fork gate's render program (doc/oscairn-plan.md, section 8). Built once against
 * this branch's include/ and once against the upstream commit it is based on; the two outputs
 * must be identical. Prints one line per effect and parameter set: a hash of the rendered audio.
 * GPL-3.0-or-later, see ../OscairnEffects.cpp.
 */
#include "OscairnConfig.h"
#include "OscairnEffects.h"

#include "sst/basic-blocks/simd/setup.h"
#include "sst/effects/Bonsai.h"
#include "sst/effects/Delay.h"
#include "sst/effects/Flanger.h"
#include "sst/effects/FloatyDelay.h"
#include "sst/effects/Nimbus.h"
#include "sst/effects/NimbusImpl.h"
#include "sst/effects/Phaser.h"
#include "sst/effects/Reverb1.h"
#include "sst/effects/Reverb2.h"
#include "sst/effects/RotarySpeaker.h"
#include "sst/effects/TreeMonster.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

using Cfg = oscairn_fx::Config;
namespace sfx = sst::effects;

static uint32_t rng = 1;
static int fill = 0;

// Every allocation is filled with one byte, so members an effect leaves uninitialized hold the
// same value in every run; a different byte (argument 1) shows which effects read them. Every
// replaceable form is defined, so no allocation pairs a library's operator new with ours.
static void *allocate(std::size_t n, std::size_t align = 0) noexcept
{
    void *p;
    if (!align)
        p = std::malloc(n ? n : 1);
    else
    {
        align = std::max(align, sizeof(void *));
        p = std::aligned_alloc(align, (n + align - 1) / align * align + (n ? 0 : align));
    }
    return p ? std::memset(p, fill, n) : nullptr;
}
static void *allocateOrAbort(std::size_t n, std::size_t align = 0)
{
    if (void *p = allocate(n, align))
        return p;
    std::abort();
}
void *operator new(std::size_t n) { return allocateOrAbort(n); }
void *operator new[](std::size_t n) { return allocateOrAbort(n); }
void *operator new(std::size_t n, std::align_val_t a) { return allocateOrAbort(n, (std::size_t)a); }
void *operator new[](std::size_t n, std::align_val_t a) { return allocateOrAbort(n, (std::size_t)a); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept { return allocate(n); }
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept { return allocate(n); }
void *operator new(std::size_t n, std::align_val_t a, const std::nothrow_t &) noexcept
{
    return allocate(n, (std::size_t)a);
}
void *operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t &) noexcept
{
    return allocate(n, (std::size_t)a);
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t, const std::nothrow_t &) noexcept { std::free(p); }

static float rand01()
{
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) / 16777216.f;
}

template <typename FX> void render(const char *name)
{
    constexpr size_t n = 48000 * 2;
    for (int set = 0; set < 9; ++set)
    {
        Cfg::GS gs(48000);
        Cfg::ES es;
        auto fx = std::make_unique<FX>(&gs, &es, nullptr);
        if constexpr (requires { fx->rng.reseed(1u); })
            fx->rng.reseed(1u); // FloatyDelay seeds its generator from the clock
        if constexpr (requires { fx->modLFO.rng.reseed(1u); })
            fx->modLFO.rng.reseed(1u); // so does Phaser's modulator (random waves)
        for (int i = 0; i < FX::numParams; ++i)
        {
            const auto md = fx->paramAt(i);
            float v = md.defaultVal;
            if (set > 0) // seeded random values across the range
            {
                v = md.minVal + rand01() * (md.maxVal - md.minVal);
                if (md.type != decltype(md)::FLOAT)
                    v = std::round(v);
            }
            fx->paramStorage[i] = v;
        }
        fx->initialize();
        uint64_t h = 1469598103934665603ull;
        uint32_t x = 12345;
        alignas(16) float L[Cfg::blockSize], R[Cfg::blockSize];
        oscairn_fx::ScopedFlushDenormals ftz;
        for (size_t at = 0; at < n; at += Cfg::blockSize)
        {
            for (int k = 0; k < Cfg::blockSize; ++k)
            {
                const size_t i = at + k;
                x = x * 1664525u + 1013904223u;
                L[k] = i < 9600 ? 0.5f * (((x >> 8) / 16777216.f) * 2 - 1) : 0.f;
                R[k] = i < 24000 ? 0.4f * (float)std::sin(i * 0.031) : 0.f;
            }
            fx->processBlock(L, R);
            for (int k = 0; k < Cfg::blockSize; ++k)
                for (float v : {L[k], R[k]})
                {
                    uint32_t b;
                    std::memcpy(&b, &v, 4);
                    h = (h ^ b) * 1099511628211ull;
                }
        }
        std::printf("%s %d %016llx\n", name, set, (unsigned long long)h);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1) // a different fill byte shows which effects read uninitialized members
        fill = std::atoi(argv[1]);
    render<sfx::reverb1::Reverb1<Cfg>>("reverb1");
    render<sfx::reverb2::Reverb2<Cfg>>("reverb2");
    render<sfx::delay::Delay<Cfg>>("delay");
    render<sfx::floatydelay::FloatyDelay<Cfg>>("floatydelay");
    render<sfx::flanger::Flanger<Cfg>>("flanger");
    render<sfx::phaser::Phaser<Cfg>>("phaser");
    render<sfx::rotaryspeaker::RotarySpeaker<Cfg>>("rotaryspeaker");
    render<sfx::bonsai::Bonsai<Cfg>>("bonsai");
    render<sfx::treemonster::TreeMonster<Cfg>>("treemonster");
    render<sfx::nimbus::Nimbus<Cfg>>("nimbus");
    return 0;
}
