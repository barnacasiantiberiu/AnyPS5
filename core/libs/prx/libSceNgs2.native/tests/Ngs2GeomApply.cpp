#include "Ngs2Test.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>

extern "C" int APS5_VABI sceNgs2GeomApply(const Ngs2GeomListenerWork*, const Ngs2GeomSourceParam*, Ngs2GeomAttribute*, std::uint32_t);

static void Near(float actual, double expected) { Require(std::isfinite(actual) && std::abs(actual - expected) < 0.00005); }
struct Scene {
    Ngs2GeomListenerParam param{};
    Ngs2GeomListenerWork listener{};
    Ngs2GeomSourceParam source{};
    Scene() {
        Require(sceNgs2GeomResetListenerParam(&param) == 0);
        Require(sceNgs2GeomCalcListener(&param, &listener, 0) == 0);
        Require(sceNgs2GeomResetSourceParam(&source) == 0);
        source.matrix_format = 1;
    }
    Ngs2GeomAttribute Apply() {
        struct Guarded { std::uint32_t before; Ngs2GeomAttribute output; std::uint32_t after; } value;
        std::memset(&value, 0x5a, sizeof(value));
        Require(sceNgs2GeomApply(&listener, &source, &value.output, 0) == 0);
        Require(value.before == 0x5a5a5a5a && value.after == 0x5a5a5a5a);
        for (auto reserved : value.output.reserved) Require(reserved == 0);
        for (auto reserved : value.output.a3d_attrib.reserved) Require(reserved == 0);
        for (unsigned row = 0; row < 8; ++row)
            for (unsigned column = 0; column < 8; ++column)
                if (row >= source.matrix_format || column >= source.num_speakers) Require(value.output.level[row * 8 + column] == 0);
        return value.output;
    }
};
static void DistanceAndCone() {
    Scene s;
    s.source.position.z = 4;
    Near(s.Apply().a3d_attrib.volume, 0.25);
    s.source.rolloff.model = 1;
    s.source.rolloff.max_distance = 7;
    Near(s.Apply().a3d_attrib.volume, 0.5);
    s.source.rolloff.model = 2;
    s.source.rolloff.rolloff_factor = 2;
    Near(s.Apply().a3d_attrib.volume, 0.0625);
    s.source.rolloff.rolloff_factor = 0;
    s.source.cone = {1, 60, 0.2f, 120};
    s.source.direction = {0, 0, -1};
    Near(s.Apply().a3d_attrib.volume, 1);
    s.source.direction = {1, 0, -1};
    Near(s.Apply().a3d_attrib.volume, 0.6);
    s.source.direction = {0, 0, 1};
    Near(s.Apply().a3d_attrib.volume, 0.2);
    s.source.min_level = 0.4f;
    Near(s.Apply().a3d_attrib.volume, 0.4);
    s.source.max_level = 0.5f;
    s.source.direction = {0, 0, -1};
    Near(s.Apply().a3d_attrib.volume, 0.5);
}
static void Panning() {
    Scene s;
    s.source.rolloff.rolloff_factor = 0;
    s.source.position = {1, 0, 0};
    auto result = s.Apply();
    Near(result.level[0], 0); Near(result.level[1], 1);
    s.source.position.x = -1;
    result = s.Apply();
    Near(result.level[0], 1); Near(result.level[1], 0);
    s.source.position = {0, 0, 1};
    result = s.Apply();
    Near(result.level[0], std::sqrt(0.5)); Near(result.level[1], std::sqrt(0.5));
    s.source.position = {0, 1, 0};
    result = s.Apply();
    Near(result.level[0], std::sqrt(0.5)); Near(result.level[1], std::sqrt(0.5));
    for (unsigned speakers : {1u, 2u, 6u, 8u}) {
        s.source.num_speakers = speakers;
        s.source.matrix_format = 8;
        for (int angle = -180; angle < 180; angle += 5) {
            const double radians = angle * std::acos(-1.0) / 180;
            s.source.position = {static_cast<float>(std::sin(radians)), 0, static_cast<float>(std::cos(radians))};
            result = s.Apply();
            double power = 0;
            for (unsigned channel = 0; channel < speakers; ++channel)
                if (!(speakers >= 6 && channel == 3)) power += result.level[channel] * result.level[channel];
            Require(std::abs(power - 1) < 0.0001);
            for (unsigned row = 1; row < 8; ++row)
                for (unsigned channel = 0; channel < 8; ++channel) Near(result.level[row * 8 + channel], result.level[channel]);
        }
        s.source.radius = 2;
        result = s.Apply();
        const double spread = 1 / std::sqrt(static_cast<double>(speakers >= 6 ? speakers - 1 : speakers));
        for (unsigned channel = 0; channel < speakers; ++channel) Near(result.level[channel], speakers >= 6 && channel == 3 ? 1 : spread);
        s.source.radius = 0;
    }
}
static void Doppler() {
    Scene s;
    s.source.position.z = 10;
    s.source.velocity.z = -34.3f;
    Near(s.Apply().pitch_ratio, 10.0 / 9.0);
    s.source.velocity.z = 34.3f;
    Near(s.Apply().pitch_ratio, 10.0 / 11.0);
    s.source.velocity = {};
    s.param.velocity.z = 34.3f;
    Require(sceNgs2GeomCalcListener(&s.param, &s.listener, 0) == 0);
    Near(s.Apply().pitch_ratio, 1.1);
    s.source.doppler_factor = 0;
    Near(s.Apply().pitch_ratio, 1);
    s.param.position = {3, 4, 5};
    s.source.position = {4, 6, 8};
    Require(sceNgs2GeomCalcListener(&s.param, &s.listener, 0) == 0);
    const auto result = s.Apply();
    Near(result.a3d_attrib.position.x, 1); Near(result.a3d_attrib.position.y, 2); Near(result.a3d_attrib.position.z, 3);
    Scene handed;
    handed.source.position = {1, 0, 0};
    Require(sceNgs2GeomCalcListener(&handed.param, &handed.listener, 1) == 0);
    Near(handed.Apply().level[0], 1); Near(handed.Apply().level[1], 0);
    handed.param.orient_front = {1, 0, 0};
    Require(sceNgs2GeomCalcListener(&handed.param, &handed.listener, 0) == 0);
    Near(handed.Apply().level[0], std::sqrt(0.5)); Near(handed.Apply().level[1], std::sqrt(0.5));
    handed.source.velocity = {-34300, 0, 0};
    Near(handed.Apply().pitch_ratio, 100);
}
static void Invalid() {
    Scene s;
    Ngs2GeomAttribute output;
    std::memset(&output, 0x5a, sizeof(output));
    const auto saved = output;
    const auto fails = [&](const Ngs2GeomListenerWork* listener, const Ngs2GeomSourceParam* source, unsigned flags) {
        bool threw = false;
        try { sceNgs2GeomApply(listener, source, &output, flags); } catch (const std::exception&) { threw = true; }
        Require(threw && std::memcmp(&output, &saved, sizeof(output)) == 0);
    };
    Require(sceNgs2GeomApply(&s.listener, &s.source, nullptr, 0) == SCE_NGS2_ERROR_INVALID_OUT_ADDRESS);
    fails(nullptr, &s.source, 0); fails(&s.listener, nullptr, 0); fails(&s.listener, &s.source, 1);
    const auto original = s.source;
    s.source.rolloff.reference_distance = 0; fails(&s.listener, &s.source, 0); s.source = original;
    s.source.num_speakers = 3; fails(&s.listener, &s.source, 0); s.source = original;
    s.source.matrix_format = 9; fails(&s.listener, &s.source, 0); s.source = original;
    s.source.position.x = std::numeric_limits<float>::infinity(); fails(&s.listener, &s.source, 0); s.source = original;
    s.source.cone.outer_level = std::numeric_limits<float>::quiet_NaN(); fails(&s.listener, &s.source, 0); s.source = original;
    s.listener.sound_speed = 0; fails(&s.listener, &s.source, 0);
}
static void Audio() {
    Scene scene;
    scene.source.position.x = 1;
    const auto geometry = scene.Apply();
    const auto system = CreateSystem();
    const auto master = Mastering(system, 2);
    const auto sampler = Voice(CreateRack(system, SCE_NGS2_RACK_ID_SAMPLER));
    const std::vector<std::int16_t> pcm(Grain, 8192);
    Control(sampler, SCE_NGS2_SAMPLER_VOICE_PARAM_SETUP, Ngs2SamplerVoiceSetupParam{{}, {SCE_NGS2_WAVEFORM_TYPE_PCM_I16L, 1, 48000, 0, 0, 0}});
    const Ngs2WaveformBlock block{0, pcm.size() * sizeof(std::int16_t), 0, 0, Grain, 0, 0};
    Control(sampler, SCE_NGS2_SAMPLER_VOICE_PARAM_ADD_WAVEFORM_BLOCKS, Ngs2SamplerVoiceWaveformBlocksParam{{}, pcm.data(), 0, 1, &block});
    Patch(sampler, master);
    Control(sampler, SCE_NGS2_VOICE_PARAM_MATRIX_LEVELS, Ngs2VoiceMatrixLevelsParam{{}, 0, 2, geometry.level});
    Control(sampler, SCE_NGS2_VOICE_PARAM_PORT_MATRIX, Ngs2VoicePortMatrixParam{{}, 0, 0});
    Event(sampler, SCE_NGS2_VOICE_EVENT_PLAY);
    std::array<float, Grain * 2> pcmOut{};
    const Ngs2RenderBufferInfo output{pcmOut.data(), sizeof(pcmOut), SCE_NGS2_WAVEFORM_TYPE_PCM_F32L, 2};
    Require(sceNgs2SystemRender(system, &output, 1) == 0);
    for (unsigned frame = 0; frame < Grain; ++frame) { Near(pcmOut[frame * 2], 0); Near(pcmOut[frame * 2 + 1], 0.25); }
    Require(sceNgs2SystemDestroy(system, nullptr) == 0);
}
int main(int argc, char** argv) {
    Require(argc == 2);
    const std::string_view mode(argv[1]);
    if (mode == "distance") DistanceAndCone();
    else if (mode == "pan") Panning();
    else if (mode == "doppler") Doppler();
    else if (mode == "invalid") Invalid();
    else { Require(mode == "audio"); Audio(); }
}
