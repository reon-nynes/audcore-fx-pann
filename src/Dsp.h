#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace pann {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr float kInvSqrt2 = 0.70710678118654752440f;

struct StereoPoint {
    float r = 0.0f;
    float l = 0.0f;
};

inline double normalizedToDegrees(double norm) noexcept {
    return -180.0 + std::clamp(norm, 0.0, 1.0) * 360.0;
}

inline double degreesToNormalized(double deg) noexcept {
    return std::clamp((deg + 180.0) / 360.0, 0.0, 1.0);
}

inline double normalizedToRadians(double norm) noexcept {
    return (2.0 * std::clamp(norm, 0.0, 1.0) - 1.0) * kPi;
}

// Complex stereo rotation f(z) = z * exp(i * omega) under z = R + iL:
// z_out = (R + iL) * (cos(w) + i*sin(w)) = (R*cos(w) - L*sin(w)) + i*(L*cos(w) + R*sin(w))
inline std::pair<float, float> rotateStereo(float inL, float inR, double omegaRad) noexcept {
    const double c = std::cos(omegaRad);
    const double s = std::sin(omegaRad);
    const double l = static_cast<double>(inL);
    const double r = static_cast<double>(inR);
    const double outL = l * c + r * s;
    const double outR = r * c - l * s;
    return {static_cast<float>(outL), static_cast<float>(outR)};
}

// Rotates (R, L) counter-clockwise by 45 degrees so mono (L == R) is vertical,
// +R points to top-right (+45 deg), and +L points to top-left (+135 deg).
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

class ScopeRingBuffer {
public:
    static constexpr uint32_t kCapacity = 8192; // Power of two

    void reset() noexcept {
        readPos_.store(0, std::memory_order_relaxed);
        writePos_.store(0, std::memory_order_relaxed);
    }

    void push(float r, float l) noexcept {
        const uint32_t w = writePos_.load(std::memory_order_relaxed);
        const uint32_t rd = readPos_.load(std::memory_order_acquire);
        if (w - rd >= kCapacity) {
            return;
        }
        buffer_[w & (kCapacity - 1)] = StereoPoint{r, l};
        writePos_.store(w + 1, std::memory_order_release);
    }

    uint32_t popAvailable(float* outInterleavedRL, uint32_t maxFloats) noexcept {
        if (outInterleavedRL == nullptr || maxFloats < 2) {
            return 0;
        }
        const uint32_t rd = readPos_.load(std::memory_order_relaxed);
        const uint32_t w = writePos_.load(std::memory_order_acquire);
        const uint32_t availPts = w - rd;
        const uint32_t toReadPts = std::min<uint32_t>(availPts, maxFloats / 2);
        if (toReadPts == 0) {
            return 0;
        }
        for (uint32_t i = 0; i < toReadPts; ++i) {
            const StereoPoint pt = buffer_[(rd + i) & (kCapacity - 1)];
            outInterleavedRL[2 * i] = pt.r;
            outInterleavedRL[2 * i + 1] = pt.l;
        }
        readPos_.store(rd + toReadPts, std::memory_order_release);
        return toReadPts * 2;
    }

private:
    std::atomic<uint32_t> writePos_{0};
    std::atomic<uint32_t> readPos_{0};
    std::array<StereoPoint, kCapacity> buffer_{};
};

class ScopeHistory {
public:
    static constexpr int kHistorySize = 2048;
    static constexpr int kNumBuckets = 8;

    void clear() noexcept {
        writeIndex_ = 0;
        validCount_ = 0;
    }

    void pushInterleavedRL(const float* values, size_t floatCount) noexcept {
        const int numPts = static_cast<int>(floatCount / 2);
        if (values == nullptr || numPts <= 0) {
            return;
        }
        const int stride = (numPts > 512) ? ((numPts + 511) / 512) : 1;
        for (int i = 0; i < numPts; i += stride) {
            const float r = values[2 * i];
            const float l = values[2 * i + 1];
            if (!std::isfinite(r) || !std::isfinite(l)) {
                history_[static_cast<size_t>(writeIndex_)] = StereoPoint{
                    std::numeric_limits<float>::quiet_NaN(),
                    std::numeric_limits<float>::quiet_NaN()
                };
            } else {
                history_[static_cast<size_t>(writeIndex_)] = StereoPoint{
                    std::clamp(r, -16.0f, 16.0f),
                    std::clamp(l, -16.0f, 16.0f)
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
        if (validCount_ <= 0 || logicalIndex < 0 || logicalIndex >= validCount_) {
            return {};
        }
        const int startRingIdx = (writeIndex_ - validCount_ + kHistorySize) % kHistorySize;
        return history_[static_cast<size_t>((startRingIdx + logicalIndex) % kHistorySize)];
    }

private:
    std::array<StereoPoint, kHistorySize> history_{};
    int writeIndex_ = 0;
    int validCount_ = 0;
};

} // namespace pann
