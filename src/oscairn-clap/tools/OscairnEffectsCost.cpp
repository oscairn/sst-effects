/*
 * The per-block cost of one Oscairn effects plugin mode, for counting instructions on the device's
 * CPU (doc/oscairn-plan.md, section 15, "Cost"). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 *   oscairn-effects-cost <plugin-id> <rate> <blocks> <signal|low|full|silence> ["Module/Name=value" ...]
 *
 * Activates the plugin at <rate> with 128-frame blocks, runs one second to warm up, then <blocks>
 * blocks inside cost_run(), which a counter (scripts/cost-a53.sh: callgrind) measures alone. The
 * signal is deterministic noise peaking at -12 dBFS (low: -48 dBFS, full: 0 dBFS); silence is
 * zeros, after the warm-up's tail has ended. With OSCAIRN_COST_COLD set, the caches are flushed by
 * a 4 MB write before each block, so a cache simulator's misses count the lines a block touches.
 */
#include "OscairnEffects.h"

#include <clap/clap.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
constexpr uint32_t frames = 128;
struct Host
{
    const clap_plugin_t *p;
    std::vector<float> l, r, ol, orr;
    uint32_t seed{1};
    double gain{0.5}; // 0: silence
    void block()
    {
        for (uint32_t i = 0; i < frames; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            l[i] = gain ? (float)(((double)seed / 4294967296.0 - 0.5) * gain) : 0.f;
            r[i] = gain ? -l[i] : 0.f;
        }
        float *ins[2] = {l.data(), r.data()}, *outs[2] = {ol.data(), orr.data()};
        clap_audio_buffer_t ib{ins, nullptr, 2, 0, 0}, ob{outs, nullptr, 2, 0, 0};
        static const clap_input_events_t none{nullptr, [](const clap_input_events_t *) -> uint32_t { return 0; },
                                              [](const clap_input_events_t *, uint32_t) -> const clap_event_header_t * { return nullptr; }};
        clap_process_t pr{};
        pr.frames_count = frames;
        pr.audio_inputs = &ib;
        pr.audio_outputs = &ob;
        pr.audio_inputs_count = 1;
        pr.audio_outputs_count = 1;
        pr.in_events = &none;
        p->process(p, &pr);
    }
};
} // namespace

// The measured region: `blocks` process calls of 128 frames.
extern "C" __attribute__((noinline)) void cost_run(Host *h, int blocks)
{
    for (int b = 0; b < blocks; ++b)
        h->block();
}

int main(int argc, char **argv)
{
    if (argc < 5)
    {
        std::fprintf(stderr, "usage: %s <plugin-id> <rate> <blocks> <signal|silence> [\"Module/Name=value\" ...]\n", argv[0]);
        return 2;
    }
    clap_host_t host{CLAP_VERSION, nullptr, "cost", "", "", "0",
                     [](const clap_host_t *, const char *) -> const void * { return nullptr; },
                     [](const clap_host_t *) {}, [](const clap_host_t *) {}, [](const clap_host_t *) {}};
    auto *factory = static_cast<const clap_plugin_factory_t *>(oscairn_effects_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    const clap_plugin_t *p = factory->create_plugin(factory, &host, argv[1]);
    if (!p || !p->init(p))
        return 1;
    auto *params = static_cast<const clap_plugin_params_t *>(p->get_extension(p, CLAP_EXT_PARAMS));
    std::vector<clap_event_param_value_t> ev;
    for (int a = 5; a < argc; ++a)
    {
        const std::string arg = argv[a];
        const auto eq = arg.rfind('=');
        const std::string key = arg.substr(0, eq);
        bool found = false;
        for (uint32_t i = 0; i < params->count(p); ++i)
        {
            clap_param_info_t info;
            params->get_info(p, i, &info);
            if (std::string(info.module) + (info.module[0] ? "/" : "") + info.name == key)
            {
                clap_event_param_value_t x{};
                x.header = {sizeof(x), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
                x.param_id = info.id;
                x.note_id = x.port_index = x.channel = x.key = -1;
                x.value = std::atof(arg.c_str() + eq + 1);
                ev.push_back(x);
                found = true;
            }
        }
        if (eq == std::string::npos || !found)
        {
            std::fprintf(stderr, "unknown parameter %s\n", key.c_str());
            return 1;
        }
    }
    clap_input_events_t in{&ev, [](const clap_input_events_t *l) -> uint32_t {
                               return (uint32_t) static_cast<const std::vector<clap_event_param_value_t> *>(l->ctx)->size();
                           },
                           [](const clap_input_events_t *l, uint32_t i) -> const clap_event_header_t * {
                               return &(*static_cast<const std::vector<clap_event_param_value_t> *>(l->ctx))[i].header;
                           }};
    params->flush(p, &in, nullptr);
    const double rate = std::atof(argv[2]);
    if (!p->activate(p, rate, frames, frames) || !p->start_processing(p))
        return 1;
    Host h{p, std::vector<float>(frames), std::vector<float>(frames), std::vector<float>(frames), std::vector<float>(frames)};
    const std::string sig = argv[4];
    if (sig == "low")
        h.gain = 0.5 / 64;
    else if (sig == "full")
        h.gain = 2;
    const int warm = (int)std::ceil(rate / frames);
    for (int b = 0; b < warm; ++b)
        h.block();
    if (sig == "silence") // let the tail end first
    {
        h.gain = 0;
        for (int b = 0; b < 30 * warm; ++b)
            h.block();
    }
    const int blocks = std::atoi(argv[3]);
    if (std::getenv("OSCAIRN_COST_COLD"))
    {
        std::vector<char> flush(4 << 20);
        volatile char *f = flush.data(); // the writes must happen
        for (int b = 0; b < blocks; ++b)
        {
            for (size_t i = 0; i < flush.size(); i += 64)
                f[i] = (char)b;
            cost_run(&h, 1);
        }
    }
    else
        cost_run(&h, blocks);
    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
    return 0;
}
