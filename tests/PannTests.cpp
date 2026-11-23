#include "Dsp.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1; \
        } \
    } while (0)

int main() {
    using namespace pann;

    // 1. Angle conversions & 1-deg integer quantization
    CHECK(std::abs(normalizedToDegrees(0.0) - (-180.0)) < 1e-9);
    CHECK(std::abs(normalizedToDegrees(0.5) - 0.0) < 1e-9);
    CHECK(std::abs(normalizedToDegrees(1.0) - 180.0) < 1e-9);
    CHECK(std::abs(degreesToNormalized(-180.0) - 0.0) < 1e-9);
    CHECK(std::abs(degreesToNormalized(0.0) - 0.5) < 1e-9);
    CHECK(std::abs(degreesToNormalized(180.0) - 1.0) < 1e-9);
    CHECK(normalizedToIntDegrees(0.0) == -180);
    CHECK(normalizedToIntDegrees(0.5) == 0);
    CHECK(normalizedToIntDegrees(1.0) == 180);
    CHECK(normalizedToIntDegrees(degreesToNormalized(37.4)) == 37);
    CHECK(normalizedToIntDegrees(degreesToNormalized(-37.6)) == -38);
    CHECK(std::abs(quantizeNormalizedToIntDegrees(degreesToNormalized(37.4)) - intDegreesToNormalized(37)) < 1e-12);
    CHECK(std::abs(normalizedToRadians(0.5) - 0.0) < 1e-9);
    CHECK(std::abs(normalizedToRadians(0.75) - (kPi * 0.5)) < 1e-9);

    // 2. Complex stereo rotation f(z) = z * exp(i * omega) with z = R + iL
    {
        auto [l0, r0] = rotateStereo(0.6f, -0.3f, 0.0);
        CHECK(std::abs(l0 - 0.6f) < 1e-6f && std::abs(r0 - (-0.3f)) < 1e-6f);

        // +90 deg (+pi/2): outL = R, outR = -L
        auto [l90, r90] = rotateStereo(0.6f, 0.2f, kPi * 0.5);
        CHECK(std::abs(l90 - 0.2f) < 1e-5f && std::abs(r90 - (-0.6f)) < 1e-5f);

        // +180 deg (+pi): outL = -L, outR = -R
        auto [l180, r180] = rotateStereo(0.6f, -0.2f, kPi);
        CHECK(std::abs(l180 - (-0.6f)) < 1e-5f && std::abs(r180 - 0.2f) < 1e-5f);

        // +45 deg on mono L=R=1/sqrt(2) -> outL = 1.0, outR = 0.0
        auto [l45, r45] = rotateStereo(kInvSqrt2, kInvSqrt2, kPi * 0.25);
        CHECK(std::abs(l45 - 1.0f) < 1e-5f && std::abs(r45 - 0.0f) < 1e-5f);
    }

    // 3. 45-deg CCW Lissajous projection: mono (r==l) is vertical (rx == 0, ry > 0)
    {
        auto [rxMono, ryMono] = rotate45CCW(kInvSqrt2, kInvSqrt2);
        CHECK(std::abs(rxMono) < 1e-6f && std::abs(ryMono - 1.0f) < 1e-5f);
        auto [rxR, ryR] = rotate45CCW(1.0f, 0.0f);
        CHECK(rxR > 0.7f && ryR > 0.7f); // +R -> top-right (+45 deg)
        auto [rxL, ryL] = rotate45CCW(0.0f, 1.0f);
        CHECK(rxL < -0.7f && ryL > 0.7f); // +L -> top-left (+135 deg)
    }

    // 4. 5 ms linear smoother
    {
        LinearSmoother smoother;
        smoother.prepare(1000, 0.005); // 5 samples
        smoother.setCurrentAndTarget(0.5);
        smoother.setTarget(1.0);
        double prev = 0.5;
        for (int i = 0; i < 4; ++i) {
            double v = smoother.getNextValue();
            CHECK(v > prev && v < 1.0);
            prev = v;
        }
        CHECK(std::abs(smoother.getNextValue() - 1.0) < 1e-9);
    }

    // 5. ScopeRingBuffer latest-overwriting zero-lag behavior & even-float popping
    {
        ScopeRingBuffer ring;
        for (int i = 0; i < 10000; ++i) {
            ring.push(static_cast<float>(i), static_cast<float>(-i));
        }
        std::vector<float> out(512, 0.0f); // 256 points
        uint32_t popped = ring.popAvailable(out.data(), 512);
        CHECK(popped == 512);
        // Must skip old backlog and return the most recent 256 points (9744..9999)
        CHECK(out[0] == 9744.0f && out[1] == -9744.0f);
        CHECK(out[510] == 9999.0f && out[511] == -9999.0f);
    }

    // 6. ScopeHistory stride downsampling (>512 pts), fadeTick, & NaN sanitization
    {
        ScopeHistory hist;
        std::vector<float> batch(2048, 0.5f); // 1024 points -> stride 2 -> 512 points stored
        batch[4] = std::numeric_limits<float>::quiet_NaN();
        hist.pushInterleavedRL(batch.data(), batch.size());
        CHECK(hist.count() == 512);
        CHECK(!std::isfinite(hist.pointAt(1).r) && !std::isfinite(hist.pointAt(1).l));
        const float alphaBefore = hist.scatterPointAt(511).alpha;
        CHECK(hist.fadeTick(0.86f));
        CHECK(hist.scatterPointAt(511).alpha < alphaBefore);
    }

    std::printf("All PannTests passed.\n");
    return 0;
}
