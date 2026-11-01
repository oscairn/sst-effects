/*
 * Runs a WAV file through one of the Oscairn effects plugins, for measuring them against
 * reference renders (doc/oscairn-plan.md, section 11). GPL-3.0-or-later, see ../OscairnEffects.cpp.
 *
 *   oscairn-effects-render <plugin-id> <in.wav> <out.wav> ["Module/Name=value" ...]
 *
 * Processes at the input file's sample rate in 512-frame blocks, removes the plugin's latency,
 * and writes 32-bit float stereo. Reads 16/24/32-bit PCM and 32-bit float WAV, mono or stereo.
 */
#include "OscairnEffects.h"

#include <clap/clap.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{
bool readWav(const char *path, std::vector<float> &L, std::vector<float> &R, uint32_t &rate)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<char> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4))
        return false;
    auto u16 = [&](size_t o) { return (uint32_t)(uint8_t)d[o] | (uint32_t)(uint8_t)d[o + 1] << 8; };
    auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
    uint32_t fmt = 0, ch = 0, bits = 0;
    for (size_t o = 12; o + 8 <= d.size();)
    {
        const uint32_t n = u32(o + 4);
        if (!std::memcmp(d.data() + o, "fmt ", 4) && n >= 16)
        {
            fmt = u16(o + 8);
            ch = u16(o + 10);
            rate = u32(o + 12);
            bits = u16(o + 22);
            if (fmt == 0xFFFE && n >= 40)
                fmt = u16(o + 32);
        }
        else if (!std::memcmp(d.data() + o, "data", 4) && ch)
        {
            const size_t bytes = bits / 8, frames = std::min<size_t>(n, d.size() - o - 8) / (bytes * ch);
            L.resize(frames);
            R.resize(frames);
            for (size_t i = 0; i < frames; ++i)
                for (uint32_t c = 0; c < std::min(ch, 2u); ++c)
                {
                    const char *p = d.data() + o + 8 + (i * ch + c) * bytes;
                    float v = 0;
                    if (fmt == 3 && bits == 32)
                        std::memcpy(&v, p, 4);
                    else if (fmt == 1 && bits == 16)
                        v = (int16_t)u16(p - d.data()) / 32768.f;
                    else if (fmt == 1 && bits == 24)
                        v = (int32_t)(u32(p - d.data()) << 8 & 0xFFFFFF00u) / 2147483648.f;
                    else if (fmt == 1 && bits == 32)
                        v = (int32_t)u32(p - d.data()) / 2147483648.f;
                    else
                        return false;
                    (c ? R : L)[i] = v;
                }
            if (ch == 1)
                R = L;
            return true;
        }
        o += 8 + n + (n & 1);
    }
    return false;
}

bool writeWav(const char *path, const std::vector<float> &L, const std::vector<float> &R, uint32_t rate)
{
    std::ofstream f(path, std::ios::binary);
    auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<const char *>(&v), 4); }; // little-endian hosts
    auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<const char *>(&v), 2); };
    const uint32_t bytes = (uint32_t)L.size() * 8;
    f.write("RIFF", 4);
    w32(36 + bytes);
    f.write("WAVEfmt ", 8);
    w32(16);
    w16(3);
    w16(2);
    w32(rate);
    w32(rate * 8);
    w16(8);
    w16(32);
    f.write("data", 4);
    w32(bytes);
    for (size_t i = 0; i < L.size(); ++i)
    {
        f.write(reinterpret_cast<const char *>(&L[i]), 4);
        f.write(reinterpret_cast<const char *>(&R[i]), 4);
    }
    return bool(f);
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: %s <plugin-id> <in.wav> <out.wav> [\"Module/Name=value\" ...]\n", argv[0]);
        return 2;
    }
    std::vector<float> L, R;
    uint32_t rate = 0;
    if (!readWav(argv[2], L, R, rate))
    {
        std::fprintf(stderr, "cannot read %s\n", argv[2]);
        return 1;
    }
    clap_host_t host{CLAP_VERSION, nullptr, "render", "", "", "0",
                     [](const clap_host_t *, const char *) -> const void * { return nullptr; },
                     [](const clap_host_t *) {}, [](const clap_host_t *) {}, [](const clap_host_t *) {}};
    auto *factory = static_cast<const clap_plugin_factory_t *>(oscairn_effects_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    const clap_plugin_t *p = factory->create_plugin(factory, &host, argv[1]);
    if (!p || !p->init(p))
    {
        std::fprintf(stderr, "no plugin %s\n", argv[1]);
        return 1;
    }
    auto *params = static_cast<const clap_plugin_params_t *>(p->get_extension(p, CLAP_EXT_PARAMS));
    struct Ev
    {
        std::vector<clap_event_param_value_t> e;
    } ev;
    for (int a = 4; a < argc; ++a)
    {
        std::string arg = argv[a];
        const auto eq = arg.rfind('=');
        const std::string key = arg.substr(0, eq);
        const double value = std::atof(arg.c_str() + eq + 1);
        bool found = false;
        for (uint32_t i = 0; i < params->count(p); ++i)
        {
            clap_param_info_t info;
            params->get_info(p, i, &info);
            const std::string full = std::string(info.module) + (info.module[0] ? "/" : "") + info.name;
            if (full == key)
            {
                clap_event_param_value_t x{};
                x.header = {sizeof(x), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
                x.param_id = info.id;
                x.note_id = x.port_index = x.channel = x.key = -1;
                x.value = value;
                ev.e.push_back(x);
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
                               return (uint32_t) static_cast<const Ev *>(l->ctx)->e.size();
                           },
                           [](const clap_input_events_t *l, uint32_t i) -> const clap_event_header_t * {
                               return &static_cast<const Ev *>(l->ctx)->e[i].header;
                           }};
    params->flush(p, &in, nullptr);
    if (!p->activate(p, rate, 1, 512) || !p->start_processing(p))
        return 1;
    const size_t latency = oscairn_fx::latencySamples, total = L.size() + latency;
    L.resize(total, 0.f);
    R.resize(total, 0.f);
    std::vector<float> oL(total), oR(total);
    clap_input_events_t none{nullptr, [](const clap_input_events_t *) -> uint32_t { return 0; },
                             [](const clap_input_events_t *, uint32_t) -> const clap_event_header_t * { return nullptr; }};
    for (size_t at = 0; at < total; at += 512)
    {
        const uint32_t n = (uint32_t)std::min<size_t>(512, total - at);
        float *ins[2] = {L.data() + at, R.data() + at}, *outs[2] = {oL.data() + at, oR.data() + at};
        clap_audio_buffer_t ib{ins, nullptr, 2, 0, 0}, ob{outs, nullptr, 2, 0, 0};
        clap_process_t pr{};
        pr.frames_count = n;
        pr.audio_inputs = &ib;
        pr.audio_outputs = &ob;
        pr.audio_inputs_count = 1;
        pr.audio_outputs_count = 1;
        pr.in_events = &none;
        if (p->process(p, &pr) == CLAP_PROCESS_ERROR)
            return 1;
    }
    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
    oL.erase(oL.begin(), oL.begin() + latency);
    oR.erase(oR.begin(), oR.begin() + latency);
    return writeWav(argv[3], oL, oR, rate) ? 0 : 1;
}
