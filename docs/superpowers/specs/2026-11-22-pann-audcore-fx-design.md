# Pann (Stereo Complex Rotation) Audcore FX Design

## 1. Intent and Why

Build a lightweight, zero-latency **Audcore FX rack plugin** (`.fx.audcore`) in `D:\Dev\Pann` that implements the complex-plane stereo rotation transform from `Lissajous-src` (`Presets.cpp` Preset 0: `"Stereo Rotation"`):

$$f(z) = z \cdot e^{i\omega}$$

with a single rotation angle slider from `-180°` to `+180°`, paired with a custom native editor (`AudcoreFxEditor`) that displays the post-transform ("after") signal on a 45°-counter-clockwise-rotated Lissajous XY scope and includes a small gray `@久嘉-Reon` signature in the bottom-right corner.

The plugin follows the official Audcore FX C ABI (`audcore_fx.h` v2 rack ABI, `audcore_fx_get_definition` v1 JSON ABI, and `audcore_fx_editor.h` v1 editor ABI) and the project conventions established in `audcore-fx-demo` and `audcore-fx-bilibili`.

---

## 2. Goals, Non-Goals, and Anti-Goals

### Goals
- **Fixed complex rotation DSP:** Map stereo $(L, R)$ to complex $z = R + iL$ (matching `AxisMapping::RPlusIL` in `Lissajous-src/core/DspBlocks.cpp`), multiply by $e^{i\omega} = \cos\omega + i\sin\omega$, and map back to stereo $(L, R)$:
  $$\text{out}_L = L\cos\omega + R\sin\omega, \quad \text{out}_R = R\cos\omega - L\sin\omega$$
- **Single parameter slider:** Rotation angle $\omega$ in degrees from `-180°` to `+180°` (default `0°`), smoothed over 5 ms on the audio thread so rapid slider movements never click or zipper.
- **Zero-latency, real-time-safe audio path:** No oversampling, no dynamic allocation, no locks, and no UI notifications inside `AudcoreFxRack::process`.
- **Post-signal 45°-CCW Lissajous Scope in Custom Editor:** Render the transformed ("after") stereo signal rotated 45° counter-clockwise so mono ($L = R$) is vertical along the Mid (`M`) axis, $+R$ points to top-right ($+45^\circ$), and $+L$ points to top-left ($+135^\circ$), matching `LissajousScopeComponent` in `Lissajous-src/ui/LissajousScopeComponent.cpp`.
- **Author watermark:** Display `@久嘉-Reon` in small muted gray text at the bottom-right corner of the editor window.
- **Self-contained build & verification:** Build into a `.fx.audcore` module via CMake + MSVC following `audcore-fx-bilibili`, verified with a Python `ctypes` ABI/DSP/state test script.

### Non-Goals
- **No oversampling:** Because $f(z) = z \cdot e^{i\omega}$ is a pure linear orthogonal rotation at steady $\omega$, oversampling is unnecessary and omitted by user decision.
- **No expression compiler, preset menu, DC blocker, or auto-makeup gain:** Orthogonal rotation preserves stereo energy ($L^2 + R^2$) and introduces no DC offset.
- **No pre-transform ("before") scope:** Only the post-transform ("after") Lissajous scope is shown.

### Anti-Goals
- **No reverse engineering or decompilation:** Rely strictly on the provided source trees (`audcore-fx-demo`, `audcore-fx-bilibili`, and `Lissajous-src`).
- **No audio-thread blocking or allocation:** `process()` must only read atomics and write to a pre-allocated lock-free ring buffer when visual telemetry is active.

---

## 3. Constraints

- **Platform & Toolchain:** Windows x64 (`MSVC`, C++17/20, CMake >= 3.22), statically linked CRT (`MultiThreaded$<$<CONFIG:Debug>:Debug>`), producing `<name>-v<version>.fx.audcore` (DLL with `.fx.audcore` suffix).
- **Audcore FX Rack ABI v2 (`audcore_fx.h`):**
  - Export `audcore_fx_get_rack(uint32_t version)` returning `AudcoreFxRack` for version `2`.
  - Export `audcore_fx_get_definition(uint32_t version, uint32_t *size)` returning immutable UTF-8 JSON for version `1`.
  - Transport buffer in `process()` always contains `2 * frames` interleaved floats (`[L0, R0, L1, R1, ...]`). When `prepare()` negotiates `channels == 1` (mono input), read `left = stereo[2 * i]` and treat input as mono (`inL = left, inR = left`) so rotating a mono source pans/rotates its stereo image across both output slots `stereo[2 * i]` and `stereo[2 * i + 1]`.
- **Audcore FX Editor ABI v1 (`audcore_fx_editor.h`):**
  - Export `audcore_fx_get_editor(uint32_t version)` returning `AudcoreFxEditor` for version `1`.
  - Export `audcore_fx_editor_flags(uint32_t version)` returning `AUDCORE_FX_EDITOR_RESIZABLE` (`1u`) for version `1`.
  - All `AudcoreFxEditor` callbacks run on the UI thread; `parent` is a Win32 `HWND`.

---

## 4. Approaches Considered

Note on `audcore-fx-bilibili`: Although `audcore-fx-bilibili` includes a `src/web/` folder, `audcore-fx-bilibili/AGENTS.md` and `CMakeLists.txt` confirm that `src/web/` is only a standalone external browser/OBS preview page (`scripts/preview-bilibili.mjs`) and is **not** compiled into the `.fx.audcore` binary. The actual plugin UI inside Audcore (`audcore-fx-bilibili/src/Panel.cpp`) is a native Win32 child `HWND` rendered with **GDI+ (`Gdiplus::Graphics` double-buffered onto a `Gdiplus::Bitmap`)**.

### Approach 1: Native Win32 + GDI+ Custom Editor (Picked)
- **Description:** Follow `audcore-fx-bilibili/src/Panel.cpp` and `src/Rack.cpp` directly. Build a dependency-free C++ module that links standard Windows libraries (`user32`, `gdi32`, `gdiplus`). Render the Nytera dark-palette 45°-CCW Lissajous scope, the `-180°..+180°` slider, and the bottom-right `@久嘉-Reon` label onto a double-buffered `Gdiplus::Bitmap`.
- **Pros:** Matches `audcore-fx-bilibili`'s exact build system, ABI lifecycle, DPI scaling (`g.ScaleTransform(scale, scale)`), and static CRT setup; tiny binary; zero external dependencies.
- **Cons:** Requires translating `LissajousScopeComponent::paint` (~150 lines) from `juce::Graphics` to `Gdiplus::Graphics`.

### Approach 2: JUCE-Backed DLL
- **Description:** Link `juce_gui_basics` into the `.fx.audcore` DLL and attach `LissajousScopeComponent` to the host `HWND`.
- **Pros:** Reuses `LissajousScopeComponent.cpp` directly.
- **Cons:** Heavy dependency, multi-megabyte binary, and potential message-thread lifecycle conflicts inside an Audcore C-ABI host.

### Approach 3: Embedded WebView2 Editor
- **Description:** Host a Microsoft Edge WebView2 control inside the Win32 `parent` `HWND` and stream audio points over a local loopback socket or `PostWebMessage`.
- **Pros:** HTML/CSS styling.
- **Cons:** Asynchronous WebView2 creation causes startup flash, high RAM overhead for a single-slider utility, and extra runtime loader requirements.

**Decision:** **Approach 1** (chosen by user, following `audcore-fx-bilibili`'s native GDI+ `Panel.cpp` pattern).

---

## 5. Detailed Design

### 5.1 Plugin Identity & Definition JSON (`src/definition.json`)
- **Code:** `"PANN"` (Builder call; can be adjusted if another code is preferred)
- **FUID:** `"A1B2C3D4E5F647898A9B0C1D2E3F4A5B"` (32-char uppercase hex UUID)
- **Vendor:** `"久嘉-Reon"`
- **Label:** `{"zh-CN": "声场旋转", "en": "Pann", "ja": "音場回転"}`
- **State identifier:** `"pann-1"`
- **Primary parameter:** `"angle"`
- **Parameters (1 parameter at index `0`):**
  ```json
  {
    "id": "angle",
    "name": "Rotation",
    "label": {"zh-CN": "旋转角度", "en": "Rotation", "ja": "回転角度"},
    "minimum": -180,
    "maximum": 180,
    "step": 0.1,
    "default": 0,
    "units": "deg",
    "skew": 1,
    "component": "slider",
    "displayDecimals": 1
  }
  ```
- **Visual:** `{"component": "none"}` (since custom editor `audcore_fx_get_editor` renders the Lissajous scope).

### 5.2 DSP & Audio Processing (`src/Rack.cpp`)
- **Normalized <-> Physical Parameter Mapping:**
  - Normalized `n` $\in [0.0, 1.0]$, default `0.5` ($0^\circ$).
  - Degrees: $\text{deg} = -180.0 + n \times 360.0 \in [-180^\circ, +180^\circ]$.
  - Radians: $\omega = (\text{deg} \times \pi) / 180.0 = (2n - 1)\pi \in [-\pi, +\pi]$.
- **5 ms Linear Parameter Smoothing (from `Lissajous-src/PluginProcessor.cpp`):**
  - On `prepare(rate, frames, channels, latency)`:
    - Validate `rate >= 8000 && rate <= 384000`, `frames > 0`, `channels == 1 || channels == 2`, `latency != nullptr`.
    - Set `*latency = 0`.
    - Compute smoothing step count `smoothSamples = max(1, round(0.005 * rate))`.
    - Snap current smoothed angle to target angle.
  - On each sample in `process(handle, stereo, sidechain, frames, tempo)`:
    - Advance `currentNormalized` linearly toward atomic `targetNormalized`.
    - Convert `currentNormalized` to $\omega = (2 \cdot \text{currentNormalized} - 1)\pi$.
    - Read input:
      - `inL = stereo[2 * i]`
      - `inR = (channels == 1) ? inL : stereo[2 * i + 1]`
    - Apply $f(z) = z \cdot e^{i\omega}$ under $z = R + iL$:
      - `c = cos(omega)`, `s = sin(omega)`
      - `outL = inL * c + inR * s`
      - `outR = inR * c - inL * s`
    - Write `stereo[2 * i] = outL`, `stereo[2 * i + 1] = outR`.
- **State Persistence (`save` / `restore`):**
  - Binary format (5 bytes):
    - `byte 0`: format version `1`
    - `bytes 1..4`: IEEE-754 little-endian `float` `normalized` $\in [0.0, 1.0]$
  - `save(handle, nullptr, 0)` returns `5`.
  - `restore(handle, data, size)`:
    - If `size == 0`, reset parameter `0` to default `0.5f` ($0^\circ$) and return `1`.
    - Otherwise validate `data != nullptr && size == 5 && data[0] == 1`, unpack `float norm`, validate `std::isfinite(norm) && norm >= 0.0f && norm <= 1.0f`, store into target parameter, and return `1`.

### 5.3 Visual Telemetry Bridge (`visual_active` & `visual`)
- **Dual-Path Compatible Telemetry:**
  - When `visual_active(handle, enabled)` is set (or while the editor is active), `process()` pushes post-rotation `(outR, outL)` stereo points into a lock-free single-producer single-consumer ring buffer (`kCapacity = 8192`, matching `ScopeRingBuffer.h`).
  - In `AudcoreFxRack::visual(handle, float* out, uint32_t capacity)`:
    - Pop up to `min(capacity / 2, kMaxVisualPointsPerPoll)` post-rotation points from the lock-free ring buffer and write them as interleaved `[r0, l0, r1, l1, ...]` into `out`, returning `2 * pointsPopped`.
    - In `AudcoreFxEditor::visual(editor, const float* values, size_t count)`:
      - Consume `count / 2` pairs of `(r, l)`, append them to the scope's 2048-point rolling history buffer (with stride downsampling if `count / 2 > 512`, matching `LissajousScopeComponent::pushPoints`), and invalidate the window to trigger a repaint.
  - *Fallback / Direct Instance Registry (like `audcore-fx-bilibili`):* Also maintain a thread-safe instance registry and 60 Hz UI timer so if the host passes fewer samples or runs in-process, the scope stays smooth and responsive. (To keep `visual()` purely sample-based and works-everywhere including isolated host workers, `Rack::visual` directly emits interleaved `(r, l)` float pairs whenever points are available.)

### 5.4 Custom Editor Layout & Rendering (`src/Panel.cpp`)
- **Default Dimensions:** `420 x 480` logical pixels, resizable (`AUDCORE_FX_EDITOR_RESIZABLE`).
- **Color Palette (from `NyteraLookAndFeel::Palette`):**
  - `bgMain`: `#0E1116` (`0xff0e1116`)
  - `bgCard`: `#161B24` (`0xff161b24`)
  - `bgField`: `#0A0D12` (`0xff0a0d12`)
  - `borderHairline`: `#263040` (`0xff263040`)
  - `borderActive`: `#3B4C66` (`0xff3b4c66`)
  - `textPrimary`: `#E6EDF3` (`0xffe6edf3`)
  - `textSecondary`: `#8B949E` (`0xff8b949e`)
  - `textDim`: `#5A6577` (`0xff5a6577`) — used for grid labels and the bottom-right `@久嘉-Reon` watermark
  - `amberTransformed`: `#F59E0B` (`0xfff59e0b`) — trace color for the post-transform Lissajous scope (matching `transformedScope_` in `Lissajous-src/PluginEditor.cpp`)
- **45°-CCW Lissajous Scope Card (Top Area):**
  - Card container with rounded corners (`6px` radius), header dot in amber (`#F59E0B`), title `"POST LISSAJOUS"` (or localized `"声场相位"` / `"POST LISSAJOUS"`), and `"L 45 / R 45"` hint on the right.
  - Centered 1:1 square plot viewport (`#0A0D12` fill, `4px` radius):
    - Inner circle at $|z| = 0.5$ (`alpha = 0.35`) and unit circle at $|z| = 1.0$ (`alpha = 0.85`).
    - Vertical Mid (`M`) and horizontal Side (`S`) hairlines (`alpha = 0.45`).
    - Prominent 45°-CCW rotated diagonal axes:
      - `R` axis from bottom-left ($-135^\circ$) to top-right ($+45^\circ$).
      - `L` axis from bottom-right ($-45^\circ$) to top-left ($+135^\circ$).
    - Labels `"L"` (top-left), `"M"` (top-center), `"R"` (top-right).
    - Coordinate transform from `LissajousScopeComponent::rotate45CCW(r, l)`:
      $$rx = \frac{r - l}{\sqrt{2}}, \quad ry = \frac{r + l}{\sqrt{2}}$$
      $$x = c_x + rx \cdot \text{halfUnit}, \quad y = c_y - ry \cdot \text{halfUnit}$$
    - Rolling history of `2048` points rendered across `8` age buckets with quadratic alpha decay (`0.06` to `0.95`), stroke width ramp (`1.0` to `1.85`), and outer glow on the newest 2 buckets (`alpha = 0.10` and `0.22`, width `+2.8`).
- **Rotation Slider Control (Bottom Card):**
  - Displays parameter label (`"ROTATION"` / `"旋转角度"`) and live formatted readout (`"-180.0°"` .. `"+180.0°"`).
  - Horizontal bipolar slider track centered at `0°` (`normalized = 0.5`), with a fill bar from center (`0°`) to current thumb position.
  - Mouse interactions:
    - Left-click / drag updates normalized value in $[0, 1]$ and invokes `AudcoreFxEdit(context, 2, 0, norm)` on press, `AudcoreFxEdit(context, 0, 0, norm)` on drag, and `AudcoreFxEdit(context, 3, 0, norm)` on release.
    - Double-click (or right-click / Ctrl+click) resets to `0°` (`0.5`).
    - Mouse wheel nudges angle by `1°` (`3°` with Shift, `0.1°` with Ctrl).
- **Bottom-Right Watermark:**
  - Right-aligned at the bottom-right corner (`margin-right: 12px`, `margin-bottom: 6px`), small font (`11px` logical, `Microsoft YaHei UI`), muted gray (`#5A6577` / `0xff5a6577`), text: `@久嘉-Reon`.

---

## 6. What Is Left to the Builder

- Exact directory layout inside `D:\Dev\Pann` (e.g., placing `CMakeLists.txt`, `include/`, `src/`, and `scripts/` at the root of `D:\Dev\Pann` or in a dedicated subfolder, while keeping `audcore-fx-demo`, `audcore-fx-bilibili`, and `Lissajous-src` untouched as reference directories).
- Exact GDI+ helper functions for rounded rectangles, font caching, and DPI scaling (modeled after `audcore-fx-bilibili/src/Panel.cpp`).
- Automated test script (`scripts/test-pann-fx.py`) verifying ABI exports, JSON definition, 0-latency prepare, rotation math at $0^\circ, \pm 45^\circ, \pm 90^\circ, \pm 180^\circ$, mono-to-stereo handling, visual telemetry output, and state save/restore.
