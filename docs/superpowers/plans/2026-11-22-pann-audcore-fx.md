# Pann Audcore FX Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the `pann-v1.0.0.fx.audcore` Audcore FX rack plugin implementing $f(z) = z \cdot e^{i\omega}$ stereo rotation with a single `-180°` to `+180°` slider, a 45°-CCW rotated post-signal Lissajous scope in a native Win32/GDI+ editor, and a bottom-right `@久嘉-Reon` watermark.

**Architecture:** A dependency-free C++17 shared module (`PANNLocalFx`) built with CMake and MSVC (`MultiThreaded` static CRT) following `audcore-fx-bilibili`. `src/Dsp.h` encapsulates the complex rotation ($z = R + iL$), 5 ms linear angle smoother, 45°-CCW scope projection, and lock-free SPSC telemetry ring buffer; `src/Rack.cpp` implements `AudcoreFxRack` ABI v2 and `audcore_fx_get_definition` ABI v1; `src/Panel.cpp` implements `AudcoreFxEditor` ABI v1 using a double-buffered GDI+ child window.

**Tech Stack:** C++17, Win32 (`user32`, `gdi32`), GDI+ (`gdiplus`), CMake >= 3.22, MSVC (x64), Python 3 (`ctypes` verification suite).

**Spec:** `D:/Dev/Pann/docs/superpowers/specs/2026-11-22-pann-audcore-fx-design.md`

## Global Constraints

- Platform & toolchain: Windows x64 (`MSVC`, C++17, CMake >= 3.22), static CRT (`MultiThreaded$<$<CONFIG:Debug>:Debug>`), output filename `pann-v1.0.0.fx.audcore` in `build/package/`.
- Audcore FX Rack ABI v2 (`audcore_fx.h`): `code = "PANN"`, `fuid = "A1B2C3D4E5F647898A9B0C1D2E3F4A5B"`, `product_version = "1.0.0"`, `latency = 0`.
- Definition JSON (`src/definition.json`): single parameter `id = "angle"` at index `0`, range `[-180, 180]`, step `0.1`, default `0`, units `"deg"`, component `"slider"`, `displayDecimals = 1`, `visual = {"component": "none"}`.
- DSP formula: $z = R + iL$, $f(z) = z \cdot e^{i\omega}$ where $\omega = (2n - 1)\pi \in [-\pi, +\pi]$ for normalized $n \in [0, 1]$, yielding $\text{out}_L = L\cos\omega + R\sin\omega$ and $\text{out}_R = R\cos\omega - L\sin\omega$.
- Audio thread safety: no oversampling, no dynamic allocation, no locks, and no UI notifications in `process()`.
- Custom editor (`audcore_fx_editor.h` v1): default logical size `420 x 480`, resizable (`AUDCORE_FX_EDITOR_RESIZABLE`), Nytera dark palette (`#0E1116` main, `#161B24` card, `#0A0D12` plot, `#F59E0B` amber trace), 45°-CCW rotated post-signal Lissajous scope (`rotate45CCW(r, l) = ((r - l)/sqrt(2), (r + l)/sqrt(2))`), bipolar `-180°..+180°` slider, and `@久嘉-Reon` in small muted gray (`#5A6577`, 11px logical) at the bottom-right corner.
- Reference directories `audcore-fx-demo`, `audcore-fx-bilibili`, and `Lissajous-src` must remain unmodified.

## Review Focus

- **Out-of-range or corrupt parameter/state inputs:** `set()` with `index != 0`, `NaN`, `< 0.0f`, or `> 1.0f`, and `restore()` with wrong version byte, wrong non-zero byte length, or out-of-range float payload must return `0` without altering the active angle.
- **Mono transport (`channels == 1` in `prepare`):** Audcore still passes `2 * frames` interleaved floats where input lives in `stereo[2 * i]`; `process()` must set `inR = inL = stereo[2 * i]` before rotation and write the rotated stereo pair to `stereo[2 * i]` and `stereo[2 * i + 1]`.
- **Telemetry gating & ring-buffer overflow:** When `visual_active(instance, 0)` is disabled, `process()` must push zero points (`visual()` returns `0`); when `visual_active(instance, 1)` is active across more than 8192 samples without draining, the SPSC ring buffer must drop excess points without corrupting unread samples or blocking.
- **5 ms angle smoothing vs. instant snap on `prepare`/`restore`:** A jump via `set()` must interpolate linearly over `max(1, round(0.005 * rate))` samples during `process()`, whereas `prepare()` and `restore()` must snap `currentNormalized` to `targetNormalized` immediately.
- **Non-finite (`NaN`/`Inf`) and oversized telemetry batches in the editor:** `AudcoreFxEditor::visual()` receiving `> 512` points or `NaN`/`Inf` floats must stride-downsample (`stride = (pts + 511) / 512`) and convert non-finite points into subpath break markers without crashing GDI+ `paint()`.

---

## File Map

- Create: `D:/Dev/Pann/include/audcore_fx.h` — Audcore FX Rack ABI v2 header (copied from `audcore-fx-bilibili/include/audcore_fx.h`).
- Create: `D:/Dev/Pann/include/audcore_fx_editor.h` — Audcore FX Editor ABI v1 header (copied from `audcore-fx-bilibili/include/audcore_fx_editor.h`).
- Create: `D:/Dev/Pann/src/definition.json` — Immutable UTF-8 JSON definition for `"PANN"`.
- Create: `D:/Dev/Pann/src/Pack.cmake` — CMake script packing `src/definition.json` into `generated/local/PANN/Assets.h` under `namespace pann`.
- Create: `D:/Dev/Pann/src/Dsp.h` — Pure C++17 header containing angle mapping, `rotateStereo(inL, inR, omega)`, `rotate45CCW(r, l)`, `LinearSmoother`, `ScopeRingBuffer` (lock-free SPSC capacity 8192), and `ScopeHistory` (2048-point rolling buffer with stride downsampling and `NaN` sanitization).
- Create: `D:/Dev/Pann/src/Rack.cpp` — Implements `AudcoreFxRack` v2 (`audcore_fx_get_rack`) and `audcore_fx_get_definition` v1.
- Create: `D:/Dev/Pann/src/Panel.cpp` — Implements `AudcoreFxEditor` v1 (`audcore_fx_get_editor`) and `audcore_fx_editor_flags` v1 with Win32 + GDI+ double-buffered 45°-CCW Lissajous scope, bipolar `-180°..+180°` slider, and `@久嘉-Reon` watermark.
- Create: `D:/Dev/Pann/tests/PannTests.cpp` — Native C++ unit tests for `Dsp.h` math, smoother, ring buffer, and `ScopeHistory`.
- Create: `D:/Dev/Pann/scripts/test-pann-fx.py` — Python `ctypes` integration test loading `pann-v1.0.0.fx.audcore` and verifying rack ABI, DSP rotation, mono transport, smoothing, telemetry, state persistence, and offscreen Win32 editor lifecycle.
- Create: `D:/Dev/Pann/scripts/build-local-pann.ps1` — PowerShell build & verification script matching `audcore-fx-bilibili/scripts/build-local-bilibili.ps1`.
- Create: `D:/Dev/Pann/CMakeLists.txt` — CMake build definition for `PANNLocalFx` (`pann-v1.0.0.fx.audcore`) and `PannTests`.

---

### Task 1: DSP Core (`src/Dsp.h`), Rack ABI (`src/Rack.cpp`), and C++ Unit Tests (`tests/PannTests.cpp`)

**Files:**
- Create: `D:/Dev/Pann/include/audcore_fx.h`
- Create: `D:/Dev/Pann/include/audcore_fx_editor.h`
- Create: `D:/Dev/Pann/src/definition.json`
- Create: `D:/Dev/Pann/src/Pack.cmake`
- Create: `D:/Dev/Pann/src/Dsp.h`
- Create: `D:/Dev/Pann/src/Rack.cpp`
- Create: `D:/Dev/Pann/src/Panel.cpp` (minimal ABI stub in Task 1, full GDI+ UI in Task 2)
- Create: `D:/Dev/Pann/tests/PannTests.cpp`
- Create: `D:/Dev/Pann/CMakeLists.txt`

**Interfaces:**
- Produces in `D:/Dev/Pann/src/Dsp.h` (`namespace pann`):
  - `inline constexpr double kPi = 3.14159265358979323846;`
  - `inline constexpr float kInvSqrt2 = 0.70710678118654752440f;`
  - `struct StereoPoint { float r = 0.0f; float l = 0.0f; };`
  - `double normalizedToDegrees(double norm) noexcept` -> `-180.0 + std::clamp(norm, 0.0, 1.0) * 360.0`
  - `double degreesToNormalized(double deg) noexcept` -> `std::clamp((deg + 180.0) / 360.0, 0.0, 1.0)`
  - `double normalizedToRadians(double norm) noexcept` -> `(2.0 * std::clamp(norm, 0.0, 1.0) - 1.0) * kPi`
  - `std::pair<float, float> rotateStereo(float inL, float inR, double omegaRad) noexcept` -> `{inL * cos(w) + inR * sin(w), inR * cos(w) - inL * sin(w)}`
  - `std::pair<float, float> rotate45CCW(float r, float l) noexcept` -> `{(r - l) * kInvSqrt2, (r + l) * kInvSqrt2}`
  - `class LinearSmoother`: `void prepare(uint32_t sampleRate, double seconds = 0.005) noexcept`, `void setCurrentAndTarget(double value) noexcept`, `void setTarget(double value) noexcept`, `double getNextValue() noexcept`, `double current() const noexcept`
  - `class ScopeRingBuffer`: lock-free SPSC ring buffer (`kCapacity = 8192`) with `void reset() noexcept`, `void push(float r, float l) noexcept`, `uint32_t popAvailable( float* outInterleavedRL, uint32_t maxFloats) noexcept` (returns number of `float`s written, always even)
  - `class ScopeHistory`: rolling history (`kHistorySize = 2048`, `kNumBuckets = 8`) with `void clear() noexcept`, `void pushInterleavedRL(const float* values, size_t floatCount) noexcept`, `int count() const noexcept`, `StereoPoint pointAt(int logicalIndex) const noexcept`
- Produces in `D:/Dev/Pann/src/Rack.cpp`:
  - `extern "C" AUDCORE_FX_EXPORT const AudcoreFxRack* audcore_fx_get_rack(uint32_t version)`
  - `extern "C" AUDCORE_FX_EXPORT const char* audcore_fx_get_definition(uint32_t version, uint32_t* size)`

- [ ] **Step 1: Initialize git repo (keeping reference folders untracked) and write `tests/PannTests.cpp`**

Run `git init` in `D:/Dev/Pann` and create `.gitignore` ignoring `build/`, `audcore-fx-demo/`, `audcore-fx-bilibili/`, and `Lissajous-src/`.
Create `D:/Dev/Pann/tests/PannTests.cpp` testing:
```cpp
#include "Dsp.h"
#include <cassert>
#include <cmath>
#include <limits>
#include <vector>

int main() {
    using namespace pann;
    // 1. Angle conversions
    assert(std::abs(normalizedToDegrees(0.0) - (-180.0)) < 1e-9);
    assert(std::abs(normalizedToDegrees(0.5) - 0.0) < 1e-9);
    assert(std::abs(normalizedToDegrees(1.0) - 180.0) < 1e-9);
    assert(std::abs(normalizedToRadians(0.5) - 0.0) < 1e-9);
    assert(std::abs(normalizedToRadians(0.75) - (kPi * 0.5)) < 1e-9);

    // 2. Complex stereo rotation f(z) = z * exp(i * omega) with z = R + iL
    {
        auto [l0, r0] = rotateStereo(0.6f, -0.3f, 0.0);
        assert(std::abs(l0 - 0.6f) < 1e-6f && std::abs(r0 - (-0.3f)) < 1e-6f);

        // +90 deg (+pi/2): outL = R, outR = -L
        auto [l90, r90] = rotateStereo(0.6f, 0.2f, kPi * 0.5);
        assert(std::abs(l90 - 0.2f) < 1e-5f && std::abs(r90 - (-0.6f)) < 1e-5f);

        // +180 deg (+pi): outL = -L, outR = -R
        auto [l180, r180] = rotateStereo(0.6f, -0.2f, kPi);
        assert(std::abs(l180 - (-0.6f)) < 1e-5f && std::abs(r180 - 0.2f) < 1e-5f);

        // +45 deg on mono L=R=1/sqrt(2) -> outL = 1.0, outR = 0.0
        auto [l45, r45] = rotateStereo(kInvSqrt2, kInvSqrt2, kPi * 0.25);
        assert(std::abs(l45 - 1.0f) < 1e-5f && std::abs(r45 - 0.0f) < 1e-5f);
    }

    // 3. 45-deg CCW Lissajous projection: mono (r==l) is vertical (rx == 0, ry > 0)
    {
        auto [rxMono, ryMono] = rotate45CCW(kInvSqrt2, kInvSqrt2);
        assert(std::abs(rxMono) < 1e-6f && std::abs(ryMono - 1.0f) < 1e-5f);
        auto [rxR, ryR] = rotate45CCW(1.0f, 0.0f);
        assert(rxR > 0.7f && ryR > 0.7f); // +R -> top-right (+45 deg)
        auto [rxL, ryL] = rotate45CCW(0.0f, 1.0f);
        assert(rxL < -0.7f && ryL > 0.7f); // +L -> top-left (+135 deg)
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
            assert(v > prev && v < 1.0);
            prev = v;
        }
        assert(std::abs(smoother.getNextValue() - 1.0) < 1e-9);
    }

    // 5. ScopeRingBuffer overflow resilience & even-float popping
    {
        ScopeRingBuffer ring;
        for (int i = 0; i < 10000; ++i) {
            ring.push(static_cast<float>(i), static_cast<float>(-i));
        }
        std::vector<float> out(512, 0.0f);
        uint32_t popped = ring.popAvailable(out.data(), 512);
        assert(popped == 512);
        assert(out[0] == 0.0f && out[1] == 0.0f);
        assert(out[2] == 1.0f && out[3] == -1.0f);
    }

    // 6. ScopeHistory stride downsampling (>512 pts) & NaN sanitization
    {
        ScopeHistory hist;
        std::vector<float> batch(2048, 0.5f); // 1024 points -> stride 2 -> 512 points stored
        batch[4] = std::numeric_limits<float>::quiet_NaN();
        hist.pushInterleavedRL(batch.data(), batch.size());
        assert(hist.count() == 512);
        assert(!std::isfinite(hist.pointAt(1).r) && !std::isfinite(hist.pointAt(1).l));
    }
    return 0;
}
```

- [ ] **Step 2: Create headers, `src/definition.json`, `src/Pack.cmake`, `src/Dsp.h`, `src/Rack.cpp`, stub `src/Panel.cpp`, and `CMakeLists.txt`**

Copy `audcore_fx.h` and `audcore_fx_editor.h` from `D:/Dev/Pann/audcore-fx-bilibili/include/` into `D:/Dev/Pann/include/`.
Write `D:/Dev/Pann/src/definition.json` with exact fields from Spec Section 5.1 (`code: "PANN"`, `fuid: "A1B2C3D4E5F647898A9B0C1D2E3F4A5B"`, `vendor: "久嘉-Reon"`, `label: {"zh-CN": "声场旋转", "en": "Pann", "ja": "音場回転"}`, `state: "pann-1"`, `primary: "angle"`, `sidechain: false`, single parameter `angle` `[-180, 180]`, `visual: {"component": "none"}`).
Write `D:/Dev/Pann/src/Pack.cmake` (identical to `audcore-fx-bilibili/src/Pack.cmake` with `namespace pann`).
Implement `D:/Dev/Pann/src/Dsp.h` and `D:/Dev/Pann/src/Rack.cpp`:
- `Rack::prepare` validates `rate >= 8000 && rate <= 384000 && frames > 0 && latency != nullptr && (channels == 1 || channels == 2)`, configures smoother for `rate`, snaps smoother to current atomic normalized angle, clears `ScopeRingBuffer`, sets `*latency = 0`, returns `1`.
- `Rack::process` loops `frames` samples, steps smoother toward atomic `targetNormalized`, computes `omega = normalizedToRadians(smoothed)`, reads `inL = stereo[2 * i]` and `inR = (channels == 1) ? inL : stereo[2 * i + 1]`, writes `rotateStereo(inL, inR, omega)` to `stereo[2 * i]` and `stereo[2 * i + 1]`, and if `visualActive` is non-zero pushes `(outR, outL)` to `ScopeRingBuffer`.
- `Rack::save` / `Rack::restore` implement the 5-byte `[version=1, float32 normalized]` format from Spec Section 5.2 (and `size == 0` resets to `0.5f`).
- `Rack::visual_active` toggles an atomic flag and resets the ring buffer on disable; `Rack::visual` pops up to `min(capacity, 1024u)` floats (`512` stereo points) from `ScopeRingBuffer`.

- [ ] **Step 3: Build `PannTests` and `PANNLocalFx` and run `PannTests`**

Run: `cmake -S D:/Dev/Pann -B D:/Dev/Pann/build -A x64 && cmake --build D:/Dev/Pann/build --config Release --target PannTests PANNLocalFx && D:/Dev/Pann/build/Release/PannTests.exe`
Expected: `PannTests.exe` exits with code `0`, and `D:/Dev/Pann/build/package/pann-v1.0.0.fx.audcore` is produced.

- [ ] **Step 4: Commit Task 1**

```bash
git add .gitignore CMakeLists.txt include/ src/ tests/
git commit -m "feat: 实现声场旋转 DSP 与 AudcoreFxRack 接口"
```

---

### Task 2: Native Win32/GDI+ Custom Editor (`src/Panel.cpp`), Python Integration Suite (`scripts/test-pann-fx.py`), and Build Script (`scripts/build-local-pann.ps1`)

**Files:**
- Modify: `D:/Dev/Pann/src/Panel.cpp`
- Create: `D:/Dev/Pann/scripts/test-pann-fx.py`
- Create: `D:/Dev/Pann/scripts/build-local-pann.ps1`

**Interfaces:**
- Consumes from `D:/Dev/Pann/src/Dsp.h`: `StereoPoint`, `normalizedToDegrees`, `degreesToNormalized`, `rotate45CCW`, `ScopeHistory`
- Produces in `D:/Dev/Pann/src/Panel.cpp`:
  - `extern "C" AUDCORE_FX_EXPORT const AudcoreFxEditor* audcore_fx_get_editor(uint32_t version)` (`version == 1`, `width = 420`, `height = 480`)
  - `extern "C" AUDCORE_FX_EXPORT uint32_t audcore_fx_editor_flags(uint32_t version)` (`version == 1 ? AUDCORE_FX_EDITOR_RESIZABLE : 0`)

- [ ] **Step 1: Write the failing Python ABI & Editor test `scripts/test-pann-fx.py`**

Create `D:/Dev/Pann/scripts/test-pann-fx.py` verifying:
1. `audcore_fx_get_rack(2)` returns valid `AudcoreFxRack` with `code == b"PANN"`, `fuid == b"A1B2C3D4E5F647898A9B0C1D2E3F4A5B"`, and `audcore_fx_get_definition(1)` matches JSON schema (`label["zh-CN"] == "声场旋转"`, parameter `"angle"` in `[-180, 180]`).
2. `prepare` rejects invalid rates/channels and succeeds with `latency == 0` for mono (`1`) and stereo (`2`).
3. DSP verification:
   - Default (`0.5` -> `0°`) leaves stereo input unchanged.
   - `+90°` (`0.75`) after 5 ms warmup maps `(L=0.6, R=0.2)` to `(outL=0.2, outR=-0.6)`.
   -5 ms smoothing: stepping from `0.5` (`0°`) to `1.0` (`+180°`) at `48000 Hz` produces intermediate values during the first `240` samples rather than jumping on sample `0`.
   - Mono (`channels == 1`) with `0.625` (`+45°`) rotates mono `L=0.70710678` into `(outL=1.0, outR=0.0)`.
4. State persistence & input validation:
   - `set` rejects `index=1`, `-0.1`, `1.1`, `NaN`.
   - `save` produces 5 bytes; `restore` round-trips across instances, rejects invalid version/size/out-of-range float, and resets to `0.5` when `size == 0`.
5. Visual telemetry:
   - With `visual_active(inst, 0)`, `visual()` returns `0`.
   - With `visual_active(inst, 1)`, `process()` populates `(outR, outL)` pairs read by `visual()`.
6. Custom Editor (`audcore_fx_get_editor(1)` & `audcore_fx_editor_flags(1)`):
   - Flags equal `1` (`AUDCORE_FX_EDITOR_RESIZABLE`), default size `(420, 480)`.
   - Creates a real offscreen Win32 parent `HWND`, calls `editor.create(hwnd, ...)`, exercises `editor.parameter(ed, 0, 0.75)`, `editor.visual(ed, floats, 2048)` (including `NaN` floats and `> 512` points), `editor.resize(ed, 500, 560, 1.25)`, `editor.translate(...)`, pumps pending Win32 `WM_PAINT` messages, and calls `editor.destroy(ed)`.

- [ ] **Step 2: Run `scripts/test-pann-fx.py` to verify it fails on the stub editor**

Run: `python D:/Dev/Pann/scripts/test-pann-fx.py D:/Dev/Pann/build/package/pann-v1.0.0.fx.audcore`
Expected: FAIL on `audcore_fx_get_editor(1)` / editor creation assertions.

- [ ] **Step 3: Implement the native Win32 + GDI+ editor in `src/Panel.cpp` and `scripts/build-local-pann.ps1`**

In `D:/Dev/Pann/src/Panel.cpp`, implement `Panel` following `audcore-fx-bilibili/src/Panel.cpp` and Spec Section 5.4:
- Register window class `L"Audcore.Pann.Panel.1"`, start GDI+, create child `HWND` of `parent`.
- Double-buffered `WM_PAINT` using `Gdiplus::Bitmap` and `g.ScaleTransform(scale, scale)`:
  - Fill background `#0E1116`.
  - **Top Scope Card (`#161B24`, border `#263040`):**
    - Header: amber dot (`#F59E0B`), localized title (`"声场相位"` / `"POST LISSAJOUS"` / `"位相スコープ"`), right hint `"L 45 / R 45"` (`Consolas`, `#5A6577`).
    - Centered 1:1 square plot (`#0A0D12`, border `#263040`): $|z|=0.5$ inner circle, $|z|=1.0$ unit circle, `M`/`S` crosshairs, 45°-CCW diagonal `L` and `R` axes, `"L"`, `"M"`, `"R"` labels.
    - 8-bucket phosphor trace from `ScopeHistory` using `rotate45CCW(pt.r, pt.l)` with quadratic alpha (`0.06f + 0.89f * ageFrac * ageFrac`), stroke width (`1.0f + 0.85f * ageFrac`), and outer glow on buckets 6 and 7 (`width + 2.8f`, alpha `0.10f` / `0.22f`), skipping `NaN` points.
  - **Bottom Slider Card (`#161B24`, border `#263040`):**
    - Label (`"旋转角度"` / `"ROTATION"` / `"回転角度"`) on left, formatted degree value (`"+0.0°"`, `"%+.1f°"`) on right.
    - Bipolar slider track centered at `0°` (`norm = 0.5`), amber accent bar (`#F59E0B`) from center to thumb, circular thumb with hover/drag highlight.
    - Mouse handling: left-button down/drag/up emits `editCallback(editContext, 2, 0, norm)`, `editCallback(editContext, 0, 0, norm)`, `editCallback(editContext, 3, 0, norm)`; double-click (`WM_LBUTTONDBLCLK`), right-click (`WM_RBUTTONDOWN`), or `Ctrl`+left-click resets to `0.5` (`0°`); mouse wheel (`WM_MOUSEWHEEL`) nudges by `1.0°` (`0.1°` with `Ctrl`, `5.0°` with `Shift`).
  - **Bottom-Right Watermark:**
    - Right-aligned in bottom margin (`{width - 130.f, height - 20.f, 118.f, 16.f}`), `11.f` font (`Microsoft YaHei UI`), color `#5A6577` (`0xff5a6577`), text `L"@久嘉-Reon"`.
- Create `D:/Dev/Pann/scripts/build-local-pann.ps1` modeled after `audcore-fx-bilibili/scripts/build-local-bilibili.ps1` (acquires `Global\Utae.StreamingBuild.Single`, builds `PANNLocalFx` and `PannTests` at `BelowNormal` priority, runs `PannTests.exe` and `scripts/test-pann-fx.py`).

- [ ] **Step 4: Run `scripts/build-local-pann.ps1` and verify all tests pass**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File D:/Dev/Pann/scripts/build-local-pann.ps1`
Expected: Builds `D:/Dev/Pann/build/package/pann-v1.0.0.fx.audcore`, passes `PannTests.exe` and `scripts/test-pann-fx.py`, and prints the output artifact path.

- [ ] **Step 5: Commit Task 2**

```bash
git add src/Panel.cpp scripts/
git commit -m "feat: 实现 45 度旋转李萨如自定义界面与构建校验脚本"
```
