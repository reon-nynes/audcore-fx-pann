#include "Dsp.h"
#include "Assets.h"
#include "audcore_fx.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

namespace pann {
namespace {

constexpr char kCode[] = "PANN";
constexpr char kFuid[] = "A1B2C3D4E5F647898A9B0C1D2E3F4A5B";
constexpr float kDefaultNormalized = 0.5f; // 0.0 deg
constexpr uint8_t kStateVersion = 1u;
constexpr uint32_t kStateBytes = 5u;
constexpr uint32_t kMaxVisualFloatsPerPoll = 1024u; // 512 stereo points

struct Rack {
    std::atomic<float> normalizedAngle{kDefaultNormalized};
    std::atomic<int> visualActive{0};
    std::atomic<uint32_t> snapPending{1};
    uint32_t sampleRate = 48000;
    uint32_t channels = 2;
    LinearSmoother smoother;
    ScopeRingBuffer ringBuffer;

    Rack() noexcept {
        smoother.prepare(sampleRate, 0.005);
        smoother.setCurrentAndTarget(static_cast<double>(kDefaultNormalized));
    }
};

void* create() {
    return new (std::nothrow) Rack();
}

void destroy(void* handle) {
    delete static_cast<Rack*>(handle);
}

int prepare(void* handle, uint32_t rate, uint32_t frames, uint32_t channels, uint32_t* latency) {
    if (!handle || !latency || !frames || rate < 8000 || rate > 384000 || (channels != 1 && channels != 2)) {
        return 0;
    }
    auto* rack = static_cast<Rack*>(handle);
    rack->sampleRate = rate;
    rack->channels = channels;
    rack->smoother.prepare(rate, 0.005);
    rack->smoother.setCurrentAndTarget(
        static_cast<double>(rack->normalizedAngle.load(std::memory_order_relaxed)));
    rack->snapPending.store(1, std::memory_order_release);
    rack->ringBuffer.reset();
    *latency = 0;
    return 1;
}

void process(void* handle, float* stereo, const float* sidechain, uint32_t frames, double tempo) {
    (void)sidechain;
    (void)tempo;
    if (!handle || !stereo || frames == 0) {
        return;
    }
    auto* rack = static_cast<Rack*>(handle);
    const float targetNorm = rack->normalizedAngle.load(std::memory_order_relaxed);
    if (rack->snapPending.exchange(0, std::memory_order_acq_rel) != 0) {
        rack->smoother.setCurrentAndTarget(static_cast<double>(targetNorm));
    } else {
        rack->smoother.setTarget(static_cast<double>(targetNorm));
    }

    const bool recordVisual = rack->visualActive.load(std::memory_order_relaxed) != 0;
    const uint32_t ch = rack->channels;

    for (uint32_t i = 0; i < frames; ++i) {
        const double norm = rack->smoother.getNextValue();
        const double omega = normalizedToRadians(norm);
        const float inL = stereo[2 * i];
        const float inR = (ch == 1) ? inL : stereo[2 * i + 1];
        const auto [outL, outR] = rotateStereo(inL, inR, omega);
        stereo[2 * i] = outL;
        stereo[2 * i + 1] = outR;
        if (recordVisual) {
            rack->ringBuffer.push(outR, outL);
        }
    }
}

float get(void* handle, uint32_t index) {
    if (!handle || index != 0) {
        return 0.0f;
    }
    return static_cast<Rack*>(handle)->normalizedAngle.load(std::memory_order_relaxed);
}

int set(void* handle, uint32_t index, float normalized) {
    if (!handle || index != 0 || !std::isfinite(normalized) || !(normalized >= 0.0f && normalized <= 1.0f)) {
        return 0;
    }
    static_cast<Rack*>(handle)->normalizedAngle.store(normalized, std::memory_order_relaxed);
    return 1;
}

uint32_t save(void* handle, uint8_t* output, uint32_t capacity) {
    if (!handle) {
        return 0;
    }
    if (!output) {
        return kStateBytes;
    }
    if (capacity < kStateBytes) {
        return 0;
    }
    const float norm = static_cast<Rack*>(handle)->normalizedAngle.load(std::memory_order_relaxed);
    output[0] = kStateVersion;
    std::memcpy(output + 1, &norm, sizeof(float));
    return kStateBytes;
}

int restore(void* handle, const uint8_t* input, uint32_t size) {
    if (!handle) {
        return 0;
    }
    auto* rack = static_cast<Rack*>(handle);
    if (size == 0) {
        rack->normalizedAngle.store(kDefaultNormalized, std::memory_order_relaxed);
        rack->smoother.setCurrentAndTarget(static_cast<double>(kDefaultNormalized));
        rack->snapPending.store(1, std::memory_order_release);
        return 1;
    }
    if (!input || size != kStateBytes || input[0] != kStateVersion) {
        return 0;
    }
    float norm = 0.0f;
    std::memcpy(&norm, input + 1, sizeof(float));
    if (!std::isfinite(norm) || !(norm >= 0.0f && norm <= 1.0f)) {
        return 0;
    }
    rack->normalizedAngle.store(norm, std::memory_order_relaxed);
    rack->smoother.setCurrentAndTarget(static_cast<double>(norm));
    rack->snapPending.store(1, std::memory_order_release);
    return 1;
}

void visual_active(void* handle, int enabled) {
    if (!handle) {
        return;
    }
    auto* rack = static_cast<Rack*>(handle);
    const int prev = rack->visualActive.exchange(enabled ? 1 : 0, std::memory_order_acq_rel);
    if (!enabled || !prev) {
        rack->ringBuffer.reset();
    }
}

uint32_t visual(void* handle, float* out, uint32_t capacity) {
    if (!handle || !out || capacity < 2) {
        return 0;
    }
    auto* rack = static_cast<Rack*>(handle);
    if (rack->visualActive.load(std::memory_order_relaxed) == 0) {
        return 0;
    }
    const uint32_t maxFloats = std::min<uint32_t>(capacity, kMaxVisualFloatsPerPoll);
    return rack->ringBuffer.popAvailable(out, maxFloats);
}

} // namespace
} // namespace pann

extern "C" AUDCORE_FX_EXPORT const AudcoreFxRack* audcore_fx_get_rack(uint32_t version) {
    using namespace pann;
    static const AudcoreFxRack api{
        sizeof(AudcoreFxRack),
        AUDCORE_FX_RACK_ABI_VERSION,
        kCode,
        kFuid,
        PANN_VERSION,
        create,
        destroy,
        prepare,
        process,
        get,
        set,
        save,
        restore,
        visual_active,
        visual
    };
    return version == AUDCORE_FX_RACK_ABI_VERSION ? &api : nullptr;
}

extern "C" AUDCORE_FX_EXPORT const char* audcore_fx_get_definition(uint32_t version, uint32_t* size) {
    if (version != 1 || !size) {
        return nullptr;
    }
    *size = sizeof(pann::definition) - 1;
    return pann::definition;
}
