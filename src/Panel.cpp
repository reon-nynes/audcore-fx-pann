#include "Dsp.h"
#include "audcore_fx.h"
#include "audcore_fx_editor.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <new>
#include <string_view>

namespace {

using namespace pann;
using namespace Gdiplus;

constexpr wchar_t kWindowClass[] = L"Audcore.Pann.Panel.1";
constexpr int32_t kDefaultWidth = 420;
constexpr int32_t kDefaultHeight = 480;
constexpr float kThumbRadius = 7.0f;

// Nytera dark palette
const Color kBgMain(0xff0e1116);
const Color kBgCard(0xff161b24);
const Color kBgField(0xff0a0d12);
const Color kBorderHairline(0xff263040);
const Color kBorderActive(0xff3b4c66);
const Color kTextPrimary(0xffe6edf3);
const Color kTextSecondary(0xff8b949e);
const Color kTextDim(0xff5a6577);
const Color kAmberTrace(0xfff59e0b);

struct Words {
    const wchar_t* scopeTitle;
    const wchar_t* rotationLabel;
};

constexpr Words kChinese{L"声场相位 · POST LISSAJOUS", L"旋转角度"};
constexpr Words kEnglish{L"POST LISSAJOUS", L"ROTATION"};
constexpr Words kJapanese{L"位相スコープ · POST LISSAJOUS", L"回転角度"};

struct Box {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    bool contains(float px, float py) const noexcept {
        return px >= x && px <= (x + w) && py >= y && py <= (y + h);
    }
};

Color withAlpha(Color c, float alpha) noexcept {
    const int a = std::clamp(static_cast<int>(std::lround(alpha * 255.0f)), 0, 255);
    return Color(static_cast<BYTE>(a), c.GetR(), c.GetG(), c.GetB());
}

void makeRoundRectPath(GraphicsPath& path, const Box& b, float radius) {
    const float r = std::clamp(radius, 0.0f, std::min(b.w, b.h) * 0.5f);
    const float d = r * 2.0f;
    if (d <= 0.01f) {
        path.AddRectangle(RectF(b.x, b.y, b.w, b.h));
        return;
    }
    path.AddArc(b.x, b.y, d, d, 180.0f, 90.0f);
    path.AddArc(b.x + b.w - d, b.y, d, d, 270.0f, 90.0f);
    path.AddArc(b.x + b.w - d, b.y + b.h - d, d, d, 0.0f, 90.0f);
    path.AddArc(b.x, b.y + b.h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void fillRoundRect(Graphics& g, const Box& b, Color color, float radius) {
    if (b.w <= 0.0f || b.h <= 0.0f) {
        return;
    }
    SolidBrush brush(color);
    if (radius <= 0.01f) {
        g.FillRectangle(&brush, RectF(b.x, b.y, b.w, b.h));
        return;
    }
    GraphicsPath path;
    makeRoundRectPath(path, b, radius);
    g.FillPath(&brush, &path);
}

void drawRoundRect(Graphics& g, const Box& b, Color color, float radius, float strokeWidth = 1.0f) {
    if (b.w <= 0.0f || b.h <= 0.0f) {
        return;
    }
    Pen pen(color, strokeWidth);
    if (radius <= 0.01f) {
        g.DrawRectangle(&pen, b.x, b.y, b.w, b.h);
        return;
    }
    GraphicsPath path;
    makeRoundRectPath(path, b, radius);
    g.DrawPath(&pen, &path);
}

void drawLabel(Graphics& g,
               const wchar_t* text,
               const Box& box,
               float fontSizePx,
               Color color,
               bool bold = false,
               bool mono = false,
               StringAlignment align = StringAlignmentNear,
               StringAlignment lineAlign = StringAlignmentCenter) {
    if (!text || box.w <= 0.0f || box.h <= 0.0f) {
        return;
    }
    const wchar_t* familyName = mono ? L"Consolas" : L"Microsoft YaHei UI";
    FontFamily family(familyName);
    const INT style = bold ? FontStyleBold : FontStyleRegular;
    Font font(&family, fontSizePx, style, UnitPixel);
    SolidBrush brush(color);
    StringFormat format(StringFormatFlagsNoWrap);
    format.SetAlignment(align);
    format.SetLineAlignment(lineAlign);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    RectF rect(box.x, box.y, box.w, box.h);
    g.DrawString(text, -1, &font, rect, &format, &brush);
}

bool tryParseIntDegrees(const wchar_t* text, int& outDeg) noexcept {
    if (!text) {
        return false;
    }
    const wchar_t* p = text;
    while (*p && std::iswspace(*p)) {
        ++p;
    }
    if (*p == L'\0') {
        return false;
    }
    wchar_t* endPtr = nullptr;
    const double parsed = std::wcstod(p, &endPtr);
    if (endPtr == p || !std::isfinite(parsed)) {
        return false;
    }
    while (*endPtr && (std::iswspace(*endPtr) || *endPtr == L'\x00B0' || *endPtr == L'd' || *endPtr == L'e' || *endPtr == L'g')) {
        ++endPtr;
    }
    if (*endPtr != L'\0') {
        return false;
    }
    outDeg = std::clamp(static_cast<int>(std::lround(parsed)), -180, 180);
    return true;
}

class Panel {
    HWND parent_ = nullptr;
    HWND window_ = nullptr;
    HWND edit_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    AudcoreFxEdit editCallback_ = nullptr;
    void* editContext_ = nullptr;
    ULONG_PTR gdiplusToken_ = 0;
    std::unique_ptr<Bitmap> backBuffer_;
    const Words* words_ = &kChinese;

    float width_ = static_cast<float>(kDefaultWidth);
    float height_ = static_cast<float>(kDefaultHeight);
    float scale_ = 1.0f;

    double normalizedAngle_ = 0.5; // 0 deg
    bool draggingSlider_ = false;
    bool hoveringSlider_ = false;
    bool trackingLeave_ = false;
    bool updatingEditText_ = false;

    Box scopeCardBox_{};
    Box sliderCardBox_{};
    Box editFrameBox_{};
    Box editInnerBox_{};
    Box sliderTrackBox_{};
    Box sliderHitBox_{};

    ScopeHistory history_;

public:
    Panel(HWND parentWindow, AudcoreFxEdit callback, void* context) noexcept
        : parent_(parentWindow), editCallback_(callback), editContext_(context) {}

    ~Panel() {
        if (edit_) {
            RemoveWindowSubclass(edit_, editSubclassProc, 1);
        }
        if (window_) {
            DestroyWindow(window_);
            window_ = nullptr;
            edit_ = nullptr;
        }
        if (editFont_) {
            DeleteObject(editFont_);
            editFont_ = nullptr;
        }
        if (editBrush_) {
            DeleteObject(editBrush_);
            editBrush_ = nullptr;
        }
        backBuffer_.reset();
        if (gdiplusToken_) {
            GdiplusShutdown(gdiplusToken_);
            gdiplusToken_ = 0;
        }
    }

    bool start(int32_t pixelWidth, int32_t pixelHeight, float initialScale) {
        static const wchar_t moduleAnchor = 0;
        HMODULE module{};
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                &moduleAnchor,
                &module)) {
            return false;
        }

        GdiplusStartupInput startup;
        if (GdiplusStartup(&gdiplusToken_, &startup, nullptr) != Ok) {
            return false;
        }

        WNDCLASSW wc{};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = windowProc;
        wc.hInstance = module;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClass;
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        const INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);

        const int32_t initW = pixelWidth > 0 ? pixelWidth : kDefaultWidth;
        const int32_t initH = pixelHeight > 0 ? pixelHeight : kDefaultHeight;
        window_ = CreateWindowExW(
            WS_EX_CONTROLPARENT,
            kWindowClass,
            L"Pann",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN,
            0,
            0,
            initW,
            initH,
            parent_,
            nullptr,
            module,
            this);
        if (!window_) {
            return false;
        }

        editBrush_ = CreateSolidBrush(RGB(10, 13, 18)); // #0A0D12 (kBgField)
        edit_ = CreateWindowExW(
            0,
            L"EDIT",
            L"0",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_RIGHT | ES_AUTOHSCROLL,
            0,
            0,
            0,
            0,
            window_,
            nullptr,
            module,
            nullptr);
        if (!edit_) {
            return false;
        }
        SendMessageW(edit_, EM_SETLIMITTEXT, 8, 0);
        SetWindowSubclass(edit_, editSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

        syncEditBoxText();
        resize(initW, initH, initialScale);
        return true;
    }

    void resize(int32_t pixelsWide, int32_t pixelsHigh, float factor) {
        scale_ = (std::isfinite(factor) && factor > 0.0f) ? std::clamp(factor, 0.5f, 4.0f) : 1.0f;
        const int32_t pw = std::max<int32_t>(1, pixelsWide);
        const int32_t ph = std::max<int32_t>(1, pixelsHigh);
        width_ = static_cast<float>(pw) / scale_;
        height_ = static_cast<float>(ph) / scale_;
        if (window_) {
            SetWindowPos(window_, nullptr, 0, 0, pw, ph, SWP_NOZORDER | SWP_NOACTIVATE);
        }

        HFONT newFont = CreateFontW(
            -std::max<int>(8, static_cast<int>(std::lround(13.0f * scale_))),
            0,
            0,
            0,
            FW_BOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            FF_MODERN | FIXED_PITCH,
            L"Consolas");
        if (newFont) {
            if (edit_) {
                SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(newFont), TRUE);
            }
            if (editFont_) {
                DeleteObject(editFont_);
            }
            editFont_ = newFont;
        }

        updateLayout();
        invalidate();
    }

    void parameter(size_t index, double value) {
        if (index != 0 || !std::isfinite(value) || draggingSlider_) {
            return;
        }
        normalizedAngle_ = quantizeNormalizedToIntDegrees(std::clamp(value, 0.0, 1.0));
        if (!edit_ || GetFocus() != edit_) {
            syncEditBoxText();
        }
        invalidate();
    }

    void visual(const float* values, size_t count) {
        if (!values || count < 2) {
            return;
        }
        history_.pushInterleavedRL(values, count);
        invalidate();
    }

    void translate(AudcoreFxTranslate callback, void* context) {
        if (!callback) {
            return;
        }
        const char* translated = callback(context, u8"声场旋转");
        if (!translated) {
            return;
        }
        const std::string_view view(translated);
        if (view == "Pann" || view == "Rotation") {
            words_ = &kEnglish;
        } else if (view == u8"音場回転") {
            words_ = &kJapanese;
        } else {
            words_ = &kChinese;
        }
        invalidate();
    }

private:
    void invalidate() noexcept {
        if (window_) {
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    void syncEditBoxText() noexcept {
        if (!edit_) {
            return;
        }
        const int deg = normalizedToIntDegrees(normalizedAngle_);
        wchar_t buf[16]{};
        std::swprintf(buf, std::size(buf), L"%d", deg);
        updatingEditText_ = true;
        SetWindowTextW(edit_, buf);
        updatingEditText_ = false;
    }

    void commitEditBox() noexcept {
        if (!edit_ || updatingEditText_) {
            return;
        }
        wchar_t buf[32]{};
        GetWindowTextW(edit_, buf, static_cast<int>(std::size(buf)));
        int parsedDeg = 0;
        if (tryParseIntDegrees(buf, parsedDeg)) {
            const double nextNorm = intDegreesToNormalized(parsedDeg);
            emitEdit(2, nextNorm);
            setAngleInteractive(nextNorm);
            emitEdit(3, nextNorm);
        } else {
            syncEditBoxText();
        }
    }

    void stepByDegrees(int deltaDeg) noexcept {
        const int curDeg = normalizedToIntDegrees(normalizedAngle_);
        const int nextDeg = std::clamp(curDeg + deltaDeg, -180, 180);
        const double nextNorm = intDegreesToNormalized(nextDeg);
        emitEdit(2, nextNorm);
        setAngleInteractive(nextNorm);
        emitEdit(3, nextNorm);
    }

    void updateLayout() noexcept {
        const float margin = 12.0f;
        const float footerHeight = 20.0f;
        const float sliderCardH = 72.0f;
        const float gap = 10.0f;

        const float contentW = std::max(60.0f, width_ - 2.0f * margin);
        const float sliderY = std::max(margin + 80.0f, height_ - footerHeight - sliderCardH - 4.0f);

        scopeCardBox_ = Box{
            margin,
            margin,
            contentW,
            std::max(60.0f, sliderY - gap - margin)
        };

        sliderCardBox_ = Box{
            margin,
            sliderY,
            contentW,
            sliderCardH
        };

        // Top-right integer input box inside sliderCardBox_
        const float frameW = 70.0f;
        const float frameH = 24.0f;
        const float frameX = sliderCardBox_.x + sliderCardBox_.w - 12.0f - frameW;
        const float frameY = sliderCardBox_.y + 8.0f;
        editFrameBox_ = Box{frameX, frameY, frameW, frameH};
        editInnerBox_ = Box{frameX + 6.0f, frameY + 4.0f, frameW - 22.0f, frameH - 8.0f};

        if (edit_) {
            const int ex = static_cast<int>(std::lround(editInnerBox_.x * scale_));
            const int ey = static_cast<int>(std::lround(editInnerBox_.y * scale_));
            const int ew = std::max<int>(10, static_cast<int>(std::lround(editInnerBox_.w * scale_)));
            const int eh = std::max<int>(10, static_cast<int>(std::lround(editInnerBox_.h * scale_)));
            SetWindowPos(edit_, nullptr, ex, ey, ew, eh, SWP_NOZORDER | SWP_NOACTIVATE);
        }

        const float trackSidePad = 48.0f;
        const float trackX = sliderCardBox_.x + trackSidePad;
        const float trackW = std::max(24.0f, sliderCardBox_.w - 2.0f * trackSidePad);
        const float trackY = sliderCardBox_.y + 46.0f;
        sliderTrackBox_ = Box{trackX, trackY, trackW, 6.0f};
        sliderHitBox_ = Box{
            sliderTrackBox_.x - kThumbRadius - 4.0f,
            sliderTrackBox_.y - 12.0f,
            sliderTrackBox_.w + 2.0f * (kThumbRadius + 4.0f),
            30.0f
        };
    }

    double normalizedFromLogicalX(float lx) const noexcept {
        if (sliderTrackBox_.w <= 1.0f) {
            return 0.5;
        }
        const double rawNorm =
            std::clamp(static_cast<double>((lx - sliderTrackBox_.x) / sliderTrackBox_.w), 0.0, 1.0);
        return quantizeNormalizedToIntDegrees(rawNorm);
    }

    void emitEdit(int32_t kind, double norm) noexcept {
        if (editCallback_) {
            editCallback_(editContext_, kind, 0, norm);
        }
    }

    void setAngleInteractive(double norm) noexcept {
        const double quantized = quantizeNormalizedToIntDegrees(norm);
        normalizedAngle_ = quantized;
        syncEditBoxText();
        emitEdit(0, quantized);
        invalidate();
    }

    void resetToCenter() noexcept {
        if (draggingSlider_) {
            ReleaseCapture();
            draggingSlider_ = false;
            emitEdit(3, normalizedAngle_);
        }
        emitEdit(2, 0.5);
        setAngleInteractive(0.5);
        emitEdit(3, 0.5);
    }

    void paint(HDC dc, const RECT& clientBounds) {
        const int pixelW = std::max<int>(1, clientBounds.right - clientBounds.left);
        const int pixelH = std::max<int>(1, clientBounds.bottom - clientBounds.top);

        if (!backBuffer_
            || backBuffer_->GetWidth() != static_cast<UINT>(pixelW)
            || backBuffer_->GetHeight() != static_cast<UINT>(pixelH)) {
            backBuffer_ = std::make_unique<Bitmap>(pixelW, pixelH, PixelFormat32bppPARGB);
        }

        {
            Graphics g(backBuffer_.get());
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetInterpolationMode(InterpolationModeBilinear);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            g.ScaleTransform(scale_, scale_);
            g.Clear(kBgMain);

            paintScopeCard(g);
            paintSliderCard(g);
            paintWatermark(g);
        }

        Graphics output(dc);
        if (edit_ && IsWindowVisible(edit_)) {
            RECT input{};
            GetWindowRect(edit_, &input);
            MapWindowPoints(HWND_DESKTOP, window_, reinterpret_cast<POINT*>(&input), 2);
            output.ExcludeClip(Rect(input.left, input.top, input.right - input.left, input.bottom - input.top));
        }
        output.DrawImage(backBuffer_.get(), 0, 0, pixelW, pixelH);
    }

    void paintScopeCard(Graphics& g) {
        fillRoundRect(g, scopeCardBox_, kBgCard, 6.0f);
        drawRoundRect(g, scopeCardBox_, kBorderHairline, 6.0f, 1.0f);

        Box inner{
            scopeCardBox_.x + 10.0f,
            scopeCardBox_.y + 8.0f,
            std::max(10.0f, scopeCardBox_.w - 20.0f),
            std::max(10.0f, scopeCardBox_.h - 16.0f)
        };

        const Box headerBounds{inner.x, inner.y, inner.w, 22.0f};
        inner.y += 26.0f;
        inner.h = std::max(0.0f, inner.h - 26.0f);

        // Header dot + title + "L 45 / R 45" scale hint
        const float dotSize = 7.0f;
        SolidBrush dotBrush(kAmberTrace);
        g.FillEllipse(
            &dotBrush,
            headerBounds.x,
            headerBounds.y + (headerBounds.h - dotSize) * 0.5f,
            dotSize,
            dotSize);

        drawLabel(
            g,
            words_->scopeTitle,
            Box{headerBounds.x + dotSize + 7.0f, headerBounds.y, std::max(10.0f, headerBounds.w - 92.0f), headerBounds.h},
            13.0f,
            kTextPrimary,
            true,
            false,
            StringAlignmentNear);

        drawLabel(
            g,
            L"L 45 / R 45",
            Box{headerBounds.x + headerBounds.w - 78.0f, headerBounds.y, 78.0f, headerBounds.h},
            10.5f,
            kTextDim,
            false,
            true,
            StringAlignmentFar);

        // Centered 1:1 square viewport
        const float side = std::floor(std::min(inner.w, inner.h));
        if (side < 24.0f) {
            return;
        }

        const Box plotRect{
            inner.x + (inner.w - side) * 0.5f,
            inner.y + (inner.h - side) * 0.5f,
            side,
            side
        };

        fillRoundRect(g, plotRect, kBgField, 4.0f);
        drawRoundRect(g, plotRect, withAlpha(kBorderHairline, 0.75f), 4.0f, 1.0f);

        const float unitMargin = std::max(8.0f, std::round(side * 0.08f));
        const Box unitBox{
            plotRect.x + unitMargin,
            plotRect.y + unitMargin,
            plotRect.w - 2.0f * unitMargin,
            plotRect.h - 2.0f * unitMargin
        };
        const float halfUnit = unitBox.w * 0.5f;
        const float cx = unitBox.x + halfUnit;
        const float cy = unitBox.y + halfUnit;

        // |z| = 0.5 inner circle & |z| = 1.0 unit circle
        {
            Pen innerCirclePen(withAlpha(kBorderHairline, 0.35f), 1.0f);
            g.DrawEllipse(&innerCirclePen, cx - halfUnit * 0.5f, cy - halfUnit * 0.5f, halfUnit, halfUnit);

            Pen unitCirclePen(withAlpha(kBorderHairline, 0.85f), 1.0f);
            g.DrawEllipse(&unitCirclePen, unitBox.x, unitBox.y, unitBox.w, unitBox.h);
        }

        // Subtle M (vertical) / S (horizontal) reference crosshairs
        {
            Pen crossPen(withAlpha(kBorderHairline, 0.45f), 1.0f);
            g.DrawLine(&crossPen, plotRect.x + 2.0f, cy, plotRect.x + plotRect.w - 2.0f, cy);
            g.DrawLine(&crossPen, cx, plotRect.y + 2.0f, cx, plotRect.y + plotRect.h - 2.0f);
        }

        // Prominent 45 deg CCW rotated L (+135 deg top-left) and R (+45 deg top-right) axes
        {
            const float diagReach = halfUnit * 1.04f;
            Pen diagPen(withAlpha(kBorderHairline, 0.95f), 1.0f);
            // R / Re axis: bottom-left to top-right (+45 deg)
            g.DrawLine(&diagPen, cx - diagReach, cy + diagReach, cx + diagReach, cy - diagReach);
            // L / Im axis: bottom-right to top-left (+135 deg)
            g.DrawLine(&diagPen, cx + diagReach, cy + diagReach, cx - diagReach, cy - diagReach);
        }

        // Axis labels: L at top-left, M at top-center, R at top-right
        drawLabel(g, L"L", Box{plotRect.x + 6.0f, plotRect.y + 4.0f, 20.0f, 12.0f}, 10.0f, kTextDim, false, true, StringAlignmentNear);
        drawLabel(g, L"R", Box{plotRect.x + plotRect.w - 26.0f, plotRect.y + 4.0f, 20.0f, 12.0f}, 10.0f, kTextDim, false, true, StringAlignmentFar);
        drawLabel(g, L"M", Box{cx - 10.0f, plotRect.y + 2.0f, 20.0f, 12.0f}, 10.0f, kTextDim, false, true, StringAlignmentCenter);

        const int validCount = history_.count();
        if (validCount <= 0) {
            return;
        }

        const GraphicsState clipState = g.Save();
        g.SetClip(RectF(plotRect.x + 1.0f, plotRect.y + 1.0f, plotRect.w - 2.0f, plotRect.h - 2.0f));

        if (validCount == 1) {
            const StereoPoint pt = history_.pointAt(0);
            if (std::isfinite(pt.r) && std::isfinite(pt.l)) {
                const auto [rx, ry] = rotate45CCW(pt.r, pt.l);
                const float px = cx + rx * halfUnit;
                const float py = cy - ry * halfUnit;
                SolidBrush glowBrush(withAlpha(kAmberTrace, 0.25f));
                g.FillEllipse(&glowBrush, px - 4.0f, py - 4.0f, 8.0f, 8.0f);
                SolidBrush coreBrush(kAmberTrace);
                g.FillEllipse(&coreBrush, px - 1.5f, py - 1.5f, 3.0f, 3.0f);
            }
            g.Restore(clipState);
            return;
        }

        const int numSegments = validCount - 1;
        for (int bucket = 0; bucket < ScopeHistory::kNumBuckets; ++bucket) {
            const int segStart = (bucket * numSegments) / ScopeHistory::kNumBuckets;
            const int segEnd = ((bucket + 1) * numSegments) / ScopeHistory::kNumBuckets;
            if (segEnd <= segStart) {
                continue;
            }

            GraphicsPath path;
            bool haveSubpath = false;
            bool hasAnySegment = false;

            for (int s = segStart; s < segEnd; ++s) {
                const StereoPoint p0 = history_.pointAt(s);
                const StereoPoint p1 = history_.pointAt(s + 1);
                if (!std::isfinite(p0.r) || !std::isfinite(p0.l)
                    || !std::isfinite(p1.r) || !std::isfinite(p1.l)) {
                    haveSubpath = false;
                    continue;
                }

                const auto [rx0, ry0] = rotate45CCW(p0.r, p0.l);
                const auto [rx1, ry1] = rotate45CCW(p1.r, p1.l);
                const float x0 = cx + rx0 * halfUnit;
                const float y0 = cy - ry0 * halfUnit;
                const float x1 = cx + rx1 * halfUnit;
                const float y1 = cy - ry1 * halfUnit;

                if (!haveSubpath) {
                    path.StartFigure();
                    haveSubpath = true;
                }
                path.AddLine(x0, y0, x1, y1);
                hasAnySegment = true;
            }

            if (!hasAnySegment) {
                continue;
            }

            const float ageFrac =
                static_cast<float>(bucket + 1) / static_cast<float>(ScopeHistory::kNumBuckets);
            const float alpha = 0.06f + 0.89f * (ageFrac * ageFrac);
            const float strokeWidth = 1.0f + 0.85f * ageFrac;

            if (bucket >= ScopeHistory::kNumBuckets - 2) {
                const float glowAlpha = (bucket == ScopeHistory::kNumBuckets - 1) ? 0.22f : 0.10f;
                Pen glowPen(withAlpha(kAmberTrace, glowAlpha), strokeWidth + 2.8f);
                glowPen.SetStartCap(LineCapRound);
                glowPen.SetEndCap(LineCapRound);
                glowPen.SetLineJoin(LineJoinRound);
                g.DrawPath(&glowPen, &path);
            }

            Pen tracePen(withAlpha(kAmberTrace, alpha), strokeWidth);
            tracePen.SetStartCap(LineCapRound);
            tracePen.SetEndCap(LineCapRound);
            tracePen.SetLineJoin(LineJoinRound);
            g.DrawPath(&tracePen, &path);
        }

        g.Restore(clipState);
    }

    void paintSliderCard(Graphics& g) {
        const bool editFocused = edit_ && (GetFocus() == edit_);
        fillRoundRect(g, sliderCardBox_, kBgCard, 6.0f);
        drawRoundRect(
            g,
            sliderCardBox_,
            (draggingSlider_ || hoveringSlider_ || editFocused) ? kBorderActive : kBorderHairline,
            6.0f,
            1.0f);

        const float headerY = sliderCardBox_.y + 10.0f;
        drawLabel(
            g,
            words_->rotationLabel,
            Box{sliderCardBox_.x + 12.0f, headerY, 160.0f, 20.0f},
            12.5f,
            kTextPrimary,
            true,
            false,
            StringAlignmentNear);

        // Integer input textbox frame + degree unit label
        fillRoundRect(g, editFrameBox_, kBgField, 4.0f);
        drawRoundRect(
            g,
            editFrameBox_,
            editFocused ? kAmberTrace : kBorderHairline,
            4.0f,
            editFocused ? 1.3f : 1.0f);
        drawLabel(
            g,
            L"\x00B0",
            Box{editFrameBox_.x + editFrameBox_.w - 15.0f, editFrameBox_.y, 12.0f, editFrameBox_.h},
            12.0f,
            kTextSecondary,
            false,
            true,
            StringAlignmentCenter);

        // Endpoint scale labels (-180 deg / +180 deg)
        drawLabel(
            g,
            L"-180\x00B0",
            Box{sliderCardBox_.x + 8.0f, sliderTrackBox_.y - 7.0f, 36.0f, 20.0f},
            10.5f,
            kTextDim,
            false,
            true,
            StringAlignmentFar);
        drawLabel(
            g,
            L"+180\x00B0",
            Box{sliderTrackBox_.x + sliderTrackBox_.w + 4.0f, sliderTrackBox_.y - 7.0f, 38.0f, 20.0f},
            10.5f,
            kTextDim,
            false,
            true,
            StringAlignmentNear);

        // Bipolar slider track
        fillRoundRect(g, sliderTrackBox_, kBgField, 3.0f);
        drawRoundRect(g, sliderTrackBox_, kBorderHairline, 3.0f, 1.0f);

        const float centerX = sliderTrackBox_.x + sliderTrackBox_.w * 0.5f;
        const float thumbX =
            sliderTrackBox_.x + sliderTrackBox_.w * static_cast<float>(std::clamp(normalizedAngle_, 0.0, 1.0));
        const float trackCenterY = sliderTrackBox_.y + sliderTrackBox_.h * 0.5f;

        // Center 0 deg tick mark
        {
            Pen tickPen(kBorderActive, 1.0f);
            g.DrawLine(&tickPen, centerX, sliderTrackBox_.y - 3.0f, centerX, sliderTrackBox_.y + sliderTrackBox_.h + 3.0f);
        }

        // Active bipolar fill from center (0 deg) to thumb
        const float fillLeft = std::min(centerX, thumbX);
        const float fillWidth = std::abs(thumbX - centerX);
        if (fillWidth > 0.5f) {
            fillRoundRect(
                g,
                Box{fillLeft, sliderTrackBox_.y + 1.0f, fillWidth, sliderTrackBox_.h - 2.0f},
                kAmberTrace,
                2.0f);
        }

        // Thumb halo + thumb circle
        if (draggingSlider_ || hoveringSlider_) {
            SolidBrush haloBrush(withAlpha(kAmberTrace, draggingSlider_ ? 0.28f : 0.16f));
            const float haloR = kThumbRadius + 4.0f;
            g.FillEllipse(&haloBrush, thumbX - haloR, trackCenterY - haloR, haloR * 2.0f, haloR * 2.0f);
        }

        SolidBrush thumbBrush(kTextPrimary);
        g.FillEllipse(
            &thumbBrush,
            thumbX - kThumbRadius,
            trackCenterY - kThumbRadius,
            kThumbRadius * 2.0f,
            kThumbRadius * 2.0f);
        Pen thumbPen(kAmberTrace, 1.8f);
        g.DrawEllipse(
            &thumbPen,
            thumbX - kThumbRadius,
            trackCenterY - kThumbRadius,
            kThumbRadius * 2.0f,
            kThumbRadius * 2.0f);
    }

    void paintWatermark(Graphics& g) {
        drawLabel(
            g,
            L"@\x4E45\x5609-Reon",
            Box{width_ - 140.0f, height_ - 20.0f, 128.0f, 16.0f},
            11.0f,
            kTextDim,
            false,
            false,
            StringAlignmentFar);
    }

    static LRESULT CALLBACK editSubclassProc(
        HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR context) {
        auto* self = reinterpret_cast<Panel*>(context);
        if (!self) {
            return DefSubclassProc(hwnd, message, wParam, lParam);
        }

        switch (message) {
            case WM_GETDLGCODE:
                return DefSubclassProc(hwnd, message, wParam, lParam) | DLGC_WANTALLKEYS;

            case WM_KEYDOWN: {
                if (wParam == VK_RETURN) {
                    self->commitEditBox();
                    if (self->window_) {
                        SetFocus(self->window_);
                    }
                    return 0;
                }
                if (wParam == VK_ESCAPE) {
                    self->syncEditBoxText();
                    if (self->window_) {
                        SetFocus(self->window_);
                    }
                    return 0;
                }
                if (wParam == VK_UP) {
                    self->stepByDegrees(1);
                    SendMessageW(hwnd, EM_SETSEL, 0, -1);
                    return 0;
                }
                if (wParam == VK_DOWN) {
                    self->stepByDegrees(-1);
                    SendMessageW(hwnd, EM_SETSEL, 0, -1);
                    return 0;
                }
                break;
            }

            case WM_CHAR:
                if (wParam == L'\r' || wParam == L'\n' || wParam == 27) {
                    return 0;
                }
                break;

            case WM_SETFOCUS: {
                const LRESULT res = DefSubclassProc(hwnd, message, wParam, lParam);
                SendMessageW(hwnd, EM_SETSEL, 0, -1);
                self->invalidate();
                return res;
            }

            case WM_KILLFOCUS: {
                self->commitEditBox();
                self->invalidate();
                break;
            }
        }
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<Panel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return TRUE;
        }
        if (!self) {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        switch (message) {
            case WM_ERASEBKGND:
                return 1;

            case WM_CTLCOLOREDIT: {
                HDC hdc = reinterpret_cast<HDC>(wParam);
                const int deg = normalizedToIntDegrees(self->normalizedAngle_);
                SetTextColor(hdc, deg == 0 ? RGB(230, 237, 243) : RGB(245, 158, 11));
                SetBkColor(hdc, RGB(10, 13, 18));
                return reinterpret_cast<LRESULT>(self->editBrush_);
            }

            case WM_PAINT: {
                PAINTSTRUCT ps{};
                HDC dc = BeginPaint(hwnd, &ps);
                RECT client{};
                GetClientRect(hwnd, &client);
                self->paint(dc, client);
                EndPaint(hwnd, &ps);
                return 0;
            }

            case WM_LBUTTONDOWN: {
                const float lx = static_cast<float>(GET_X_LPARAM(lParam)) / self->scale_;
                const float ly = static_cast<float>(GET_Y_LPARAM(lParam)) / self->scale_;
                if (self->editFrameBox_.contains(lx, ly) && self->edit_) {
                    SetFocus(self->edit_);
                    SendMessageW(self->edit_, EM_SETSEL, 0, -1);
                    return 0;
                }
                SetFocus(hwnd);
                if (self->sliderHitBox_.contains(lx, ly)) {
                    if ((wParam & MK_CONTROL) != 0) {
                        self->resetToCenter();
                        return 0;
                    }
                    self->draggingSlider_ = true;
                    SetCapture(hwnd);
                    const double norm = self->normalizedFromLogicalX(lx);
                    self->emitEdit(2, norm);
                    self->setAngleInteractive(norm);
                }
                return 0;
            }

            case WM_MOUSEMOVE: {
                const float lx = static_cast<float>(GET_X_LPARAM(lParam)) / self->scale_;
                const float ly = static_cast<float>(GET_Y_LPARAM(lParam)) / self->scale_;
                if (!self->trackingLeave_) {
                    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                    TrackMouseEvent(&tme);
                    self->trackingLeave_ = true;
                }
                if (self->draggingSlider_) {
                    self->setAngleInteractive(self->normalizedFromLogicalX(lx));
                } else {
                    const bool hot = self->sliderHitBox_.contains(lx, ly);
                    if (hot != self->hoveringSlider_) {
                        self->hoveringSlider_ = hot;
                        self->invalidate();
                    }
                }
                return 0;
            }

            case WM_LBUTTONUP: {
                if (self->draggingSlider_) {
                    ReleaseCapture();
                    self->draggingSlider_ = false;
                    self->emitEdit(3, self->normalizedAngle_);
                    self->invalidate();
                }
                return 0;
            }

            case WM_CAPTURECHANGED: {
                if (self->draggingSlider_) {
                    self->draggingSlider_ = false;
                    self->emitEdit(3, self->normalizedAngle_);
                    self->invalidate();
                }
                return 0;
            }

            case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: {
                const float lx = static_cast<float>(GET_X_LPARAM(lParam)) / self->scale_;
                const float ly = static_cast<float>(GET_Y_LPARAM(lParam)) / self->scale_;
                if (!self->editFrameBox_.contains(lx, ly)
                    && (self->sliderHitBox_.contains(lx, ly) || self->sliderCardBox_.contains(lx, ly))) {
                    self->resetToCenter();
                }
                return 0;
            }

            case WM_MOUSEWHEEL: {
                const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                if (delta != 0) {
                    const int stepDeg = ((wParam & MK_SHIFT) != 0) ? 5 : 1;
                    const int steps = (delta >= 0) ? std::max(1, delta / 120) : std::min(-1, delta / 120);
                    self->stepByDegrees(steps * stepDeg);
                }
                return 0;
            }

            case WM_MOUSELEAVE: {
                self->trackingLeave_ = false;
                if (self->hoveringSlider_) {
                    self->hoveringSlider_ = false;
                    self->invalidate();
                }
                return 0;
            }
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
};

} // namespace

extern "C" AUDCORE_FX_EXPORT const AudcoreFxEditor* audcore_fx_get_editor(uint32_t version) {
    static const AudcoreFxEditor api{
        sizeof(AudcoreFxEditor),
        1,
        kDefaultWidth,
        kDefaultHeight,
        [](void* parent,
           const double* values,
           size_t count,
           int32_t width,
           int32_t height,
           float scale,
           AudcoreFxEdit edit,
           void* context) -> void* {
            if (!parent) {
                return nullptr;
            }
            try {
                auto panel = std::make_unique<Panel>(static_cast<HWND>(parent), edit, context);
                if (values && count > 0) {
                    panel->parameter(0, values[0]);
                }
                return panel->start(width, height, scale) ? panel.release() : nullptr;
            } catch (...) {
                return nullptr;
            }
        },
        [](void* handle) { delete static_cast<Panel*>(handle); },
        [](void* handle, int32_t width, int32_t height, float scale) {
            if (handle) {
                static_cast<Panel*>(handle)->resize(width, height, scale);
            }
        },
        [](void* handle, size_t index, double value) {
            if (handle) {
                static_cast<Panel*>(handle)->parameter(index, value);
            }
        },
        [](void* handle, const float* values, size_t count) {
            if (handle) {
                static_cast<Panel*>(handle)->visual(values, count);
            }
        },
        [](void* handle, AudcoreFxTranslate callback, void* context) {
            if (handle) {
                static_cast<Panel*>(handle)->translate(callback, context);
            }
        }
    };
    return version == 1 ? &api : nullptr;
}

extern "C" AUDCORE_FX_EXPORT uint32_t audcore_fx_editor_flags(uint32_t version) {
    return version == 1 ? AUDCORE_FX_EDITOR_RESIZABLE : 0;
}
