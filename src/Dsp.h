#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace pann {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr float kInvSqrt2 = 0.70710678118654752440f;

struct StereoPoint {
    float r = 0.0f;
    float l = 0.0f;
};

struct ScatterPoint {
    float r = 0.0f;
    float l = 0.0f;
    float alpha = 0.0f;
};

inline double normalizedToDegrees(double norm) noexcept {
    return -180.0 + std::clamp(norm, 0.0, 1.0) * 360.0;
}

inline double degreesToNormalized(double deg) noexcept {
    return std::clamp((deg + 180.0) / 360.0, 0.0, 1.0);
}

inline int normalizedToIntDegrees(double norm) noexcept {
    return std::clamp(static_cast<int>(std::lround(normalizedToDegrees(norm))), -180, 180);
}

inline double intDegreesToNormalized(int deg) noexcept {
    return degreesToNormalized(static_cast<double>(std::clamp(deg, -180, 180)));
}

inline double quantizeNormalizedToIntDegrees(double norm) noexcept {
    return intDegreesToNormalized(normalizedToIntDegrees(norm));
}

inline double normalizedToRadians(double norm) noexcept {
    return (2.0 * std::clamp(norm, 0.0, 1.0) - 1.0) * kPi;
}

// Complex stereo rotation f(z) = z * exp(i * omega) under z = L + iR:
// z_in  = L + i * R
// z_out = (L + i * R) * (cos(w) + i * sin(w))
//       = (L * cos(w) - R * sin(w)) + i * (L * sin(w) + R * cos(w))
// Re(z_out) = outL = L * cos(w) - R * sin(w)
// Im(z_out) = outR = R * cos(w) + L * sin(w)
inline std::pair<float, float> rotateStereo(float inL, float inR, double omegaRad) noexcept {
    const double c = std::cos(omegaRad);
    const double s = std::sin(omegaRad);
    const double l = static_cast<double>(inL);
    const double r = static_cast<double>(inR);
    const double outL = l * c - r * s;
    const double outR = r * c + l * s;
    return {static_cast<float>(outL), static_cast<float>(outR)};
}

// Rotates z = R + iL counter-clockwise by 45 degrees (z * exp(i * pi / 4)):
// Re(z_rot) = rx = (R - L) / sqrt(2)
// Im(z_rot) = ry = (R + L) / sqrt(2)
// Mono (R == L > 0) points straight up (+Y), +R points top-right (+45 deg), +L points top-left (+135 deg).
inline std::pair<float, float> rotate45CCW(float r, float l) noexcept {
    return {(r - l) * kInvSqrt2, (r + l) * kInvSqrt2};
}

class LinearSmoother {
public:
    void prepare(uint32_t sampleRate, double seconds = 0.005) noexcept {
        if (sampleRate > 0 && seconds > 0.0 && std::isfinite(seconds)) {
            stepsTotal_ = std::max<int>(
                1, static_cast<int>(std::lround(static_cast<double>(sampleRate) * seconds)));
        } else {
            stepsTotal_ = 1;
        }
        stepsRemaining_ = 0;
        step_ = 0.0;
    }

    void setCurrentAndTarget(double value) noexcept {
        current_ = value;
        target_ = value;
        stepsRemaining_ = 0;
        step_ = 0.0;
    }

    void setTarget(double value) noexcept {
        if (std::abs(value - target_) <= 1e-12) {
            return;
        }
        target_ = value;
        if (stepsTotal_ <= 1) {
            current_ = target_;
            stepsRemaining_ = 0;
            step_ = 0.0;
        } else {
            stepsRemaining_ = stepsTotal_;
            step_ = (target_ - current_) / static_cast<double>(stepsTotal_);
        }
    }

    double getNextValue() noexcept {
        if (stepsRemaining_ > 0) {
            --stepsRemaining_;
            if (stepsRemaining_ == 0) {
                current_ = target_;
            } else {
                current_ += step_;
            }
        }
        return current_;
    }

    double current() const noexcept { return current_; }

private:
    double current_ = 0.5;
    double target_ = 0.5;
    double step_ = 0.0;
    int stepsTotal_ = 1;
    int stepsRemaining_ = 0;
};

// Lock-free latest-overwriting ring buffer for real-time audio scope sampling.
// The producer (audio thread) never blocks and always overwrites the oldest slot;
// consumers read only the newest samples since their last cursor, skipping any backlog
// so the visual scope never lags behind the live audio signal.
class ScopeRingBuffer {
public:
    static constexpr uint32_t kCapacity = 8192; // Power of two
    static constexpr uint32_t kMask = kCapacity - 1;

    void reset() noexcept {
        const uint32_t w = writePos_.load(std::memory_order_acquire);
        readPos_.store(w, std::memory_order_release);
        decimCounter_ = 0;
        peakEnergy_ = -1.0f;
    }

    void setDecimation(uint32_t stride) noexcept {
        decimStride_ = std::max<uint32_t>(1u, stride);
        decimCounter_ = 0;
        peakEnergy_ = -1.0f;
    }

    // Push every sample directly (used when stride == 1 or by unit tests)
    void push(float r, float l) noexcept {
        const uint32_t w = writePos_.load(std::memory_order_relaxed);
        buffer_[w & kMask] = StereoPoint{r, l};
        writePos_.store(w + 1, std::memory_order_release);
    }

    // Decimated peak-preserving sampler for real-time audio: keeps 1 representative
    // point every decimStride_ samples so 48 kHz audio produces ~4800 scatter points/sec (~80/frame).
    void pushSampled(float r, float l) noexcept {
        if (decimStride_ <= 1) {
            push(r, l);
            return;
        }
        const float energy = r * r + l * l;
        if (decimCounter_ == 0 || energy >= peakEnergy_) {
            peakPoint_ = StereoPoint{r, l};
            peakEnergy_ = energy;
        }
        if (++decimCounter_ >= decimStride_) {
            push(peakPoint_.r, peakPoint_.l);
            decimCounter_ = 0;
            peakEnergy_ = -1.0f;
        }
    }

    void flushPendingSample() noexcept {
        if (decimCounter_ > 0) {
            push(peakPoint_.r, peakPoint_.l);
            decimCounter_ = 0;
            peakEnergy_ = -1.0f;
        }
    }

    // Pops up to maxFloats/2 MOST RECENT points since readPos_, automatically skipping
    // stale points if the consumer fell behind so there is zero visual latency.
    uint32_t popAvailable(float* outInterleavedRL, uint32_t maxFloats) noexcept {
        if (outInterleavedRL == nullptr || maxFloats < 2) {
            return 0;
        }
        uint32_t rd = readPos_.load(std::memory_order_relaxed);
        const uint32_t w = writePos_.load(std::memory_order_acquire);
        const uint32_t availPts = w - rd;
        if (availPts == 0) {
            return 0;
        }
        const uint32_t maxPts = std::min<uint32_t>(maxFloats / 2, kCapacity);
        if (availPts > maxPts) {
            // Skip stale backlog to stay locked to the live audio wavefront
            rd = w - maxPts;
        }
        const uint32_t toReadPts = w - rd;
        for (uint32_t i = 0; i < toReadPts; ++i) {
            const StereoPoint pt = buffer_[(rd + i) & kMask];
            outInterleavedRL[2 * i] = pt.r;
            outInterleavedRL[2 * i + 1] = pt.l;
        }
        readPos_.store(w, std::memory_order_release);
        return toReadPts * 2;
    }

    // Cursor-based reader for the 60 FPS UI timer (independent of host Rack::visual polling)
    uint32_t popLatestWithCursor(uint32_t& ioCursor, float* outInterleavedRL, uint32_t maxFloats) const noexcept {
        if (outInterleavedRL == nullptr || maxFloats < 2) {
            return 0;
        }
        const uint32_t w = writePos_.load(std::memory_order_acquire);
        const uint32_t availPts = w - ioCursor;
        if (availPts == 0) {
            return 0;
        }
        const uint32_t maxPts = std::min<uint32_t>(maxFloats / 2, kCapacity);
        if (availPts > maxPts) {
            ioCursor = w - maxPts;
        }
        const uint32_t toReadPts = w - ioCursor;
        for (uint32_t i = 0; i < toReadPts; ++i) {
            const StereoPoint pt = buffer_[(ioCursor + i) & kMask];
            outInterleavedRL[2 * i] = pt.r;
            outInterleavedRL[2 * i + 1] = pt.l;
        }
        ioCursor = w;
        return toReadPts * 2;
    }

private:
    std::atomic<uint32_t> writePos_{0};
    std::atomic<uint32_t> readPos_{0};
    std::array<StereoPoint, kCapacity> buffer_{};
    uint32_t decimStride_ = 1;
    uint32_t decimCounter_ = 0;
    float peakEnergy_ = -1.0f;
    StereoPoint peakPoint_{};
};

// Rolling scatter-point buffer with smooth per-frame exponential alpha decay.
class ScopeHistory {
public:
    static constexpr int kHistorySize = 2048;
    static constexpr int kNumBuckets = 8;

    void clear() noexcept {
        writeIndex_ = 0;
        validCount_ = 0;
    }

    // Fades all existing points by decayFactor (e.g. 0.88 per 16ms frame) so points fade out
    // smoothly even when no new samples arrive.
    bool fadeTick(float decayFactor = 0.88f) noexcept {
        bool anyVisible = false;
        for (int i = 0; i < validCount_; ++i) {
            auto& pt = history_[static_cast<size_t>(i)];
            pt.alpha *= decayFactor;
            if (pt.alpha >= 0.025f && std::isfinite(pt.r) && std::isfinite(pt.l)) {
                anyVisible = true;
            } else {
                pt.alpha = 0.0f;
            }
        }
        return anyVisible;
    }

    void pushInterleavedRL(const float* values, size_t floatCount) noexcept {
        const int numPts = static_cast<int>(floatCount / 2);
        if (values == nullptr || numPts <= 0) {
            return;
        }

        // Age existing points gently when a new batch arrives so dense scatter trails persist smoothly
        for (int i = 0; i < validCount_; ++i) {
            history_[static_cast<size_t>(i)].alpha *= 0.93f;
        }

        const int stride = (numPts > kHistorySize) ? ((numPts + kHistorySize - 1) / kHistorySize) : 1;
        const int keptTotal = std::max(1, (numPts + stride - 1) / stride);
        int keptIdx = 0;

        for (int i = 0; i < numPts; i += stride) {
            const float r = values[2 * i];
            const float l = values[2 * i + 1];
            // Ramp initial alpha across the incoming batch from 0.55 (older in batch) to 1.0 (newest)
            const float batchFrac =
                static_cast<float>(keptIdx + 1) / static_cast<float>(keptTotal);
            const float initAlpha = 0.55f + 0.45f * batchFrac;
            ++keptIdx;

            if (!std::isfinite(r) || !std::isfinite(l)) {
                history_[static_cast<size_t>(writeIndex_)] = ScatterPoint{
                    std::numeric_limits<float>::quiet_NaN(),
                    std::numeric_limits<float>::quiet_NaN(),
                    0.0f
                };
            } else {
                history_[static_cast<size_t>(writeIndex_)] = ScatterPoint{
                    std::clamp(r, -16.0f, 16.0f),
                    std::clamp(l, -16.0f, 16.0f),
                    initAlpha
                };
            }
            writeIndex_ = (writeIndex_ + 1) % kHistorySize;
            if (validCount_ < kHistorySize) {
                ++validCount_;
            }
        }
    }

    int count() const noexcept { return validCount_; }

    StereoPoint pointAt(int logicalIndex) const noexcept {
        const ScatterPoint sp = scatterPointAt(logicalIndex);
        return StereoPoint{sp.r, sp.l};
    }

    ScatterPoint scatterPointAt(int logicalIndex) const noexcept {
        if (validCount_ <= 0 || logicalIndex < 0 || logicalIndex >= validCount_) {
            return {};
        }
        const int startRingIdx = (writeIndex_ - validCount_ + kHistorySize) % kHistorySize;
        return history_[static_cast<size_t>((startRingIdx + logicalIndex) % kHistorySize)];
    }

private:
    std::array<ScatterPoint, kHistorySize> history_{};
    int writeIndex_ = 0;
    int validCount_ = 0;
};

struct SharedScopeState {
    ScopeRingBuffer ringBuffer;
    std::atomic<uint32_t> sampleRate{48000};
    std::atomic<int> scopeEnabled{1};
};

std::shared_ptr<SharedScopeState> scopeStateFor(uint32_t id);
std::shared_ptr<SharedScopeState> latestScopeState();

} // namespace pann
