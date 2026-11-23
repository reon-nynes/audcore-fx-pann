#include "Dsp.h"
#include "Assets.h"
#include "audcore_fx.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>

namespace pann {
namespace {

constexpr char kCode[] = "PANN";
constexpr char kFuid[] = "A1B2C3D4E5F647898A9B0C1D2E3F4A5B";
constexpr float kDefaultNormalized = 0.5f; // 0 deg
constexpr uint8_t kStateVersionLegacy = 1u;
constexpr uint8_t kStateVersion = 2u;
constexpr uint32_t kStateBytesLegacy = 5u;
constexpr uint32_t kStateBytes = 6u;
constexpr uint32_t kMaxVisualFloatsPerPoll = 1024u; // 512 stereo points
constexpr uint32_t kTargetScatterPointsPerSec = 16000u; // ~267 scatter points per 60 FPS frame

std::mutex gRegistryMutex;
std::unordered_map<uint32_t, std::weak_ptr<SharedScopeState>> gRegistry;
std::weak_ptr<SharedScopeState> gLatestScopeState;
uint32_t gNextId = 0x00010000u;

struct Rack {
    std::atomic<float> normalizedAngle{kDefaultNormalized};
    std::atomic<int> visualActive{0};
    std::atomic<uint32_t> snapPending{1};
    uint32_t sampleRate = 48000;
    uint32_t channels = 2;
    uint32_t id = 0;
    LinearSmoother smoother;
    ScopeRingBuffer abiVisualRing;
    std::shared_ptr<SharedScopeState> scopeState = std::make_shared<SharedScopeState>();

    Rack() {
        smoother.prepare(sampleRate, 0.005);
        smoother.setCurrentAndTarget(static_cast<double>(kDefaultNormalized));
        scopeState->ringBuffer.setDecimation(sampleRate / kTargetScatterPointsPerSec);
        std::lock_guard<std::mutex> lock(gRegistryMutex);
        do {
            ++gNextId;
            if ((gNextId & 0xffffu) == 0u) {
                ++gNextId;
            }
        } while (gNextId < 0x00010001u || gRegistry.count(gNextId) != 0);
        id = gNextId;
        gRegistry.emplace(id, scopeState);
        gLatestScopeState = scopeState;
    }

    ~Rack() {
        std::lock_guard<std::mutex> lock(gRegistryMutex);
        gRegistry.erase(id);
    }
};

void* create() {
    try {
        return new Rack();
    } catch (...) {
        return nullptr;
    }
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
    rack->abiVisualRing.reset();
    rack->scopeState->sampleRate.store(rate, std::memory_order_release);
    rack->scopeState->ringBuffer.setDecimation(
        std::max<uint32_t>(1u, rate / kTargetScatterPointsPerSec));
    rack->scopeState->ringBuffer.reset();
    {
        std::lock_guard<std::mutex> lock(gRegistryMutex);
        gLatestScopeState = rack->scopeState;
    }
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

    const bool recordAbiVisual = rack->visualActive.load(std::memory_order_relaxed) != 0;
    const bool sampleLiveScope = rack->scopeState->scopeEnabled.load(std::memory_order_relaxed) != 0;
    const uint32_t ch = rack->channels;
    auto& liveRing = rack->scopeState->ringBuffer;

    for (uint32_t i = 0; i < frames; ++i) {
        const double norm = rack->smoother.getNextValue();
        const double omega = normalizedToRadians(norm);
        const float inL = stereo[2 * i];
        const float inR = (ch == 1) ? inL : stereo[2 * i + 1];
        const auto [outL, outR] = rotateStereo(inL, inR, omega);
        stereo[2 * i] = outL;
        stereo[2 * i + 1] = outR;

        // Only sample into the live scope ring when the Lissajous scope is expanded
        if (sampleLiveScope) {
            liveRing.pushSampled(outR, outL);
        }

        if (recordAbiVisual) {
            rack->abiVisualRing.push(outR, outL);
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
    auto* rack = static_cast<Rack*>(handle);
    const float norm = rack->normalizedAngle.load(std::memory_order_relaxed);
    const uint8_t collapsedFlag =
        (rack->scopeState->scopeCollapsed.load(std::memory_order_relaxed) != 0) ? 0x01u : 0x00u;
    const uint8_t extendedFlag =
        (rack->scopeState->extendedRange.load(std::memory_order_relaxed) != 0) ? 0x02u : 0x00u;
    output[0] = kStateVersion;
    std::memcpy(output + 1, &norm, sizeof(float));
    output[5] = static_cast<uint8_t>(collapsedFlag | extendedFlag);
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
        rack->scopeState->scopeCollapsed.store(0, std::memory_order_relaxed);
        rack->scopeState->scopeEnabled.store(1, std::memory_order_relaxed);
        rack->scopeState->extendedRange.store(0, std::memory_order_relaxed);
        return 1;
    }
    if (!input) {
        return 0;
    }
    const uint8_t ver = input[0];
    if ((ver == kStateVersionLegacy && size != kStateBytesLegacy)
        || (ver == kStateVersion && (size != kStateBytes || (input[5] & ~0x03u) != 0))
        || (ver != kStateVersionLegacy && ver != kStateVersion)) {
        return 0;
    }
    float norm = 0.0f;
    std::memcpy(&norm, input + 1, sizeof(float));
    if (!std::isfinite(norm) || !(norm >= 0.0f && norm <= 1.0f)) {
        return 0;
    }
    const bool collapsed = (ver == kStateVersion) && ((input[5] & 0x01u) != 0);
    const bool extended = (ver == kStateVersion)
        ? ((input[5] & 0x02u) != 0)
        : (std::abs(normalizedToIntDegrees(static_cast<double>(norm))) > 45);
    rack->normalizedAngle.store(norm, std::memory_order_relaxed);
    rack->smoother.setCurrentAndTarget(static_cast<double>(norm));
    rack->snapPending.store(1, std::memory_order_release);
    rack->scopeState->scopeCollapsed.store(collapsed ? 1 : 0, std::memory_order_relaxed);
    rack->scopeState->scopeEnabled.store(collapsed ? 0 : 1, std::memory_order_relaxed);
    rack->scopeState->extendedRange.store(extended ? 1 : 0, std::memory_order_relaxed);
    if (collapsed) {
        rack->scopeState->ringBuffer.reset();
    }
    return 1;
}

void visual_active(void* handle, int enabled) {
    if (!handle) {
        return;
    }
    auto* rack = static_cast<Rack*>(handle);
    const int prev = rack->visualActive.exchange(enabled ? 1 : 0, std::memory_order_acq_rel);
    if (!enabled || !prev) {
        rack->abiVisualRing.reset();
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
    return rack->abiVisualRing.popAvailable(out, maxFloats);
}

} // namespace

std::shared_ptr<SharedScopeState> scopeStateFor(uint32_t id) {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    const auto it = gRegistry.find(id);
    return it == gRegistry.end() ? nullptr : it->second.lock();
}

std::shared_ptr<SharedScopeState> latestScopeState() {
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    if (auto latest = gLatestScopeState.lock()) {
        return latest;
    }
    for (const auto& [id, weak] : gRegistry) {
        if (auto state = weak.lock()) {
            return state;
        }
    }
    return nullptr;
}

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
