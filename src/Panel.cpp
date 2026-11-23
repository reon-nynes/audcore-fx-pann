#include "Dsp.h"
#include "audcore_fx.h"
#include "audcore_fx_editor.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
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
constexpr float kCollapsedLogicalHeight = 156.0f;
constexpr float kThumbRadius = 7.0f;
constexpr UINT_PTR kScopeTimerId = 1;
constexpr UINT kScopeTimerMs = 16; // ~60 FPS

// Nytera dark palette
const Color kBgMain(0xff0e1116);
const Color kBgCard(0xff161b24);
const Color kBgField(0xff0a0d12);
const Color kBgSurfaceHover(0xff1d2430);
const Color kBorderHairline(0xff263040);
const Color kBorderActive(0xff3b4c66);
const Color kTextPrimary(0xffe6edf3);
const Color kTextSecondary(0xff8b949e);
const Color kTextDim(0xff5a6577);
const Color kAmberTrace(0xfff59e0b);

struct Words {
    const wchar_t* scopeTitle;
    const wchar_t* rotationLabel;
    const wchar_t* collapseLabel;
    const wchar_t* expandLabel;
    const wchar_t* extendRangeLabel;
};

constexpr Words kChinese{L"声场相位可视化", L"旋转角度", L"收起", L"展开", L"扩展 ±180°"};
constexpr Words kEnglish{L"声场相位可视化", L"ROTATION", L"Hide", L"Show", L"Ext ±180°"};
constexpr Words kJapanese{L"声场相位可视化", L"回転角度", L"隠す", L"表示", L"拡張 ±180°"};

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

// Fast premultiplied-ARGB pixel blend into a locked 32bpp PARGB bitmap buffer
inline void blendPixelPARGB(uint8_t* scan0,
                            int stride,
                            int clipX0,
                            int clipY0,
                            int clipX1,
                            int clipY1,
                            int x,
                            int y,
                            uint8_t srcR,
                            uint8_t srcG,
                            uint8_t srcB,
                            uint8_t alpha) noexcept {
    if (x < clipX0 || x >= clipX1 || y < clipY0 || y >= clipY1 || alpha == 0) {
        return;
    }
    uint8_t* px = scan0 + y * stride + x * 4;
    const uint32_t invA = 255u - alpha;
    const uint32_t pB = (static_cast<uint32_t>(srcB) * alpha) / 255u;
    const uint32_t pG = (static_cast<uint32_t>(srcG) * alpha) / 255u;
    const uint32_t pR = (static_cast<uint32_t>(srcR) * alpha) / 255u;
    px[0] = static_cast<uint8_t>(std::min<uint32_t>(255u, pB + (px[0] * invA) / 255u));
    px[1] = static_cast<uint8_t>(std::min<uint32_t>(255u, pG + (px[1] * invA) / 255u));
    px[2] = static_cast<uint8_t>(std::min<uint32_t>(255u, pR + (px[2] * invA) / 255u));
    px[3] = 255u;
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
    float expandedLogicalHeight_ = static_cast<float>(kDefaultHeight);
    float scale_ = 1.0f;

    double normalizedAngle_ = 0.5; // 0 deg
    int lastEditDeg_ = -9999;
    bool draggingSlider_ = false;
    bool hoveringSlider_ = false;
    bool hoveringToggle_ = false;
    bool hoveringRangeBtn_ = false;
    bool scopeCollapsed_ = false;
    bool extendedRange_ = false; // false = [-45, +45] default; true = [-180, +180]
    bool inProgrammaticResize_ = false;
    bool trackingLeave_ = false;
    bool updatingEditText_ = false;

    Box scopeCardBox_{};
    Box toggleBtnBox_{};
    Box sliderCardBox_{};
    Box rangeBtnBox_{};
    Box editFrameBox_{};
    Box editInnerBox_{};
    Box sliderTrackBox_{};
    Box sliderHitBox_{};

    ScopeHistory history_;
    std::shared_ptr<SharedScopeState> liveScope_;
    uint32_t liveReadCursor_ = 0;
    std::array<float, 1024> timerScratch_{}; // Up to 512 dense sampled points per 16ms frame

public:
    Panel(HWND parentWindow, AudcoreFxEdit callback, void* context) noexcept
        : parent_(parentWindow), editCallback_(callback), editContext_(context) {}

    ~Panel() {
        if (window_) {
            KillTimer(window_, kScopeTimerId);
        }
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
        // Created hidden so normal slider dragging renders directly in the GDI+ backbuffer
        // with zero Win32 child-control repaint lag; shown only when the user clicks to type.
        edit_ = CreateWindowExW(
            0,
            L"EDIT",
            L"0",
            WS_CHILD | WS_TABSTOP | ES_RIGHT | ES_AUTOHSCROLL,
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

        liveScope_ = latestScopeState();
        if (liveScope_) {
            scopeCollapsed_ = liveScope_->scopeCollapsed.load(std::memory_order_relaxed) != 0;
            extendedRange_ = liveScope_->extendedRange.load(std::memory_order_relaxed) != 0;
            liveScope_->scopeEnabled.store(scopeCollapsed_ ? 0 : 1, std::memory_order_relaxed);
        }
        if (std::abs(normalizedToIntDegrees(normalizedAngle_)) > 45) {
            extendedRange_ = true;
            if (liveScope_) {
                liveScope_->extendedRange.store(1, std::memory_order_relaxed);
            }
        }

        syncEditBoxText();
        resize(initW, initH, initialScale);
        if (scopeCollapsed_) {
            resizeHostWindowHeight(kCollapsedLogicalHeight);
            updateLayout();
        } else {
            SetTimer(window_, kScopeTimerId, kScopeTimerMs, nullptr);
        }
        return true;
    }

    void resize(int32_t pixelsWide, int32_t pixelsHigh, float factor) {
        scale_ = (std::isfinite(factor) && factor > 0.0f) ? std::clamp(factor, 0.5f, 4.0f) : 1.0f;
        const int32_t pw = std::max<int32_t>(1, pixelsWide);
        const int32_t ph = std::max<int32_t>(1, pixelsHigh);
        width_ = static_cast<float>(pw) / scale_;
        height_ = static_cast<float>(ph) / scale_;
        if (!scopeCollapsed_ && !inProgrammaticResize_ && height_ >= 240.0f) {
            expandedLogicalHeight_ = height_;
        }
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
        if (!liveScope_) {
            liveScope_ = latestScopeState();
        }
        if (liveScope_) {
            const bool savedCollapsed = liveScope_->scopeCollapsed.load(std::memory_order_relaxed) != 0;
            const bool savedExtended = liveScope_->extendedRange.load(std::memory_order_relaxed) != 0;
            extendedRange_ = savedExtended;
            if (savedCollapsed != scopeCollapsed_) {
                scopeCollapsed_ = !savedCollapsed; // toggleScopeCollapsed flips it to savedCollapsed
                toggleScopeCollapsed(false);
            }
        }
        normalizedAngle_ = quantizeNormalizedToIntDegrees(std::clamp(value, 0.0, 1.0));
        const int deg = normalizedToIntDegrees(normalizedAngle_);
        if (std::abs(deg) > 45 && !extendedRange_) {
            extendedRange_ = true;
            if (liveScope_) {
                liveScope_->extendedRange.store(1, std::memory_order_relaxed);
            }
        }
        if (!edit_ || GetFocus() != edit_) {
            syncEditBoxText();
        }
        invalidate();
    }

    void visual(const float* values, size_t count) {
        if (scopeCollapsed_ || !values || count < 2) {
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
    int minSliderDeg() const noexcept { return extendedRange_ ? -180 : -45; }
    int maxSliderDeg() const noexcept { return extendedRange_ ? 180 : 45; }

    void invalidate() noexcept {
        if (window_) {
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    void notifyStateChanged() noexcept {
        if (editCallback_) {
            editCallback_(editContext_, 11, 0, 0.0);
            emitEdit(2, normalizedAngle_);
            emitEdit(0, normalizedAngle_);
            emitEdit(3, normalizedAngle_);
        }
    }

    void beginTextEdit() noexcept {
        if (!edit_) {
            return;
        }
        syncEditBoxText(true);
        ShowWindow(edit_, SW_SHOW);
        SetFocus(edit_);
        SendMessageW(edit_, EM_SETSEL, 0, -1);
        invalidate();
    }

    void endTextEdit(bool commit) noexcept {
        if (!edit_) {
            return;
        }
        if (commit) {
            commitEditBox();
        } else {
            syncEditBoxText(true);
        }
        ShowWindow(edit_, SW_HIDE);
        invalidate();
    }

    void resizeHostWindowHeight(float targetLogicalH) noexcept {
        if (!window_) {
            return;
        }
        const int32_t targetPixelH =
            std::max<int32_t>(1, static_cast<int32_t>(std::lround(targetLogicalH * scale_)));
        RECT childClient{};
        GetClientRect(window_, &childClient);
        const int32_t currentPixelW =
            std::max<int32_t>(1, static_cast<int32_t>(std::lround(width_ * scale_)));
        const int32_t currentPixelH =
            std::max<int32_t>(1, childClient.bottom - childClient.top);
        const int32_t deltaH = targetPixelH - currentPixelH;

        inProgrammaticResize_ = true;
        height_ = targetLogicalH;
        SetWindowPos(window_, nullptr, 0, 0, currentPixelW, targetPixelH, SWP_NOZORDER | SWP_NOACTIVATE);

        if (parent_ && IsWindow(parent_) && deltaH != 0) {
            HWND root = GetAncestor(parent_, GA_ROOT);
            if (root && root != parent_ && IsWindow(root)) {
                RECT parentRect{};
                if (GetWindowRect(parent_, &parentRect)) {
                    const int newParentH =
                        std::max<int>(targetPixelH, (parentRect.bottom - parentRect.top) + deltaH);
                    SetWindowPos(
                        parent_,
                        nullptr,
                        0,
                        0,
                        parentRect.right - parentRect.left,
                        newParentH,
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
                RECT rootRect{};
                if (GetWindowRect(root, &rootRect)) {
                    const int newRootH =
                        std::max<int>(targetPixelH, (rootRect.bottom - rootRect.top) + deltaH);
                    SetWindowPos(
                        root,
                        nullptr,
                        0,
                        0,
                        rootRect.right - rootRect.left,
                        newRootH,
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
            } else {
                RECT parentRect{};
                if (GetWindowRect(parent_, &parentRect)) {
                    const int newParentH =
                        std::max<int>(targetPixelH, (parentRect.bottom - parentRect.top) + deltaH);
                    SetWindowPos(
                        parent_,
                        nullptr,
                        0,
                        0,
                        parentRect.right - parentRect.left,
                        newParentH,
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
        }
        inProgrammaticResize_ = false;
    }

    void toggleScopeCollapsed(bool notifyHost = true) noexcept {
        if (!scopeCollapsed_ && height_ >= 240.0f) {
            expandedLogicalHeight_ = height_;
        }
        scopeCollapsed_ = !scopeCollapsed_;
        if (!liveScope_) {
            liveScope_ = latestScopeState();
        }
        if (liveScope_) {
            liveScope_->scopeCollapsed.store(scopeCollapsed_ ? 1 : 0, std::memory_order_relaxed);
        }
        if (scopeCollapsed_) {
            // Stop all UI timer ticks, clear scatter history, and stop audio-thread scope sampling
            if (window_) {
                KillTimer(window_, kScopeTimerId);
            }
            if (liveScope_) {
                liveScope_->scopeEnabled.store(0, std::memory_order_relaxed);
                liveScope_->ringBuffer.reset();
            }
            history_.clear();
            resizeHostWindowHeight(kCollapsedLogicalHeight);
        } else {
            // Restore expanded window height, resume audio-thread scope sampling and 60 FPS UI timer
            resizeHostWindowHeight(std::max(320.0f, expandedLogicalHeight_));
            if (liveScope_) {
                liveScope_->ringBuffer.reset();
                liveScope_->scopeEnabled.store(1, std::memory_order_relaxed);
            }
            if (window_) {
                SetTimer(window_, kScopeTimerId, kScopeTimerMs, nullptr);
            }
        }
        updateLayout();
        if (notifyHost) {
            notifyStateChanged();
        }
        invalidate();
    }

    void toggleExtendedRange() noexcept {
        extendedRange_ = !extendedRange_;
        if (!liveScope_) {
            liveScope_ = latestScopeState();
        }
        if (liveScope_) {
            liveScope_->extendedRange.store(extendedRange_ ? 1 : 0, std::memory_order_relaxed);
        }
        if (!extendedRange_) {
            // Clamp active angle into [-45, +45] when switching back to default range
            const int curDeg = normalizedToIntDegrees(normalizedAngle_);
            if (curDeg < -45 || curDeg > 45) {
                const double clampedNorm = intDegreesToNormalized(std::clamp(curDeg, -45, 45));
                emitEdit(2, clampedNorm);
                setAngleInteractive(clampedNorm);
                emitEdit(3, clampedNorm);
                return;
            }
        }
        notifyStateChanged();
        invalidate();
    }

    void onTimerTick() noexcept {
        if (scopeCollapsed_) {
            return;
        }
        if (!liveScope_) {
            liveScope_ = latestScopeState();
            if (liveScope_) {
                liveScope_->scopeEnabled.store(1, std::memory_order_relaxed);
            }
        }
        uint32_t poppedFloats = 0;
        if (liveScope_) {
            poppedFloats = liveScope_->ringBuffer.popLatestWithCursor(
                liveReadCursor_, timerScratch_.data(), static_cast<uint32_t>(timerScratch_.size()));
        }
        if (poppedFloats >= 2) {
            history_.pushInterleavedRL(timerScratch_.data(), poppedFloats);
            invalidate();
        } else if (history_.fadeTick(0.88f)) {
            invalidate();
        }
    }

    void syncEditBoxText(bool force = false) noexcept {
        if (!edit_) {
            return;
        }
        const int deg = normalizedToIntDegrees(normalizedAngle_);
        if (!force && deg == lastEditDeg_) {
            return;
        }
        lastEditDeg_ = deg;
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
            if (std::abs(parsedDeg) > 45 && !extendedRange_) {
                extendedRange_ = true;
                if (!liveScope_) {
                    liveScope_ = latestScopeState();
                }
                if (liveScope_) {
                    liveScope_->extendedRange.store(1, std::memory_order_relaxed);
                }
            }
            const double nextNorm = intDegreesToNormalized(parsedDeg);
            emitEdit(2, nextNorm);
            setAngleInteractive(nextNorm);
            emitEdit(3, nextNorm);
        } else {
            syncEditBoxText(true);
        }
    }

    void stepByDegrees(int deltaDeg) noexcept {
        const int curDeg = normalizedToIntDegrees(normalizedAngle_);
        const int nextDeg = std::clamp(curDeg + deltaDeg, minSliderDeg(), maxSliderDeg());
        const double nextNorm = intDegreesToNormalized(nextDeg);
        emitEdit(2, nextNorm);
        setAngleInteractive(nextNorm);
        emitEdit(3, nextNorm);
    }

    void updateLayout() noexcept {
        const float margin = 12.0f;
        const float footerHeight = 20.0f;
        const float sliderCardH = 74.0f;
        const float gap = 10.0f;
        const float collapsedScopeH = 38.0f;

        const float contentW = std::max(60.0f, width_ - 2.0f * margin);

        if (scopeCollapsed_) {
            scopeCardBox_ = Box{margin, margin, contentW, collapsedScopeH};
            sliderCardBox_ = Box{margin, margin + collapsedScopeH + gap, contentW, sliderCardH};
        } else {
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
        }

        // Collapse toggle button to the right of "声场相位可视化" title inside scopeCardBox_ header
        const float toggleW = 54.0f;
        const float toggleH = 22.0f;
        toggleBtnBox_ = Box{
            scopeCardBox_.x + scopeCardBox_.w - 10.0f - toggleW,
            scopeCardBox_.y + 8.0f,
            toggleW,
            toggleH
        };

        // Extended range toggle button next to "旋转角度" title inside sliderCardBox_ header
        rangeBtnBox_ = Box{
            sliderCardBox_.x + 76.0f,
            sliderCardBox_.y + 9.0f,
            74.0f,
            22.0f
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
        const float trackY = sliderCardBox_.y + 44.0f;
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
        const double t =
            std::clamp(static_cast<double>((lx - sliderTrackBox_.x) / sliderTrackBox_.w), 0.0, 1.0);
        const int minDeg = minSliderDeg();
        const int maxDeg = maxSliderDeg();
        const int deg = std::clamp(
            static_cast<int>(std::lround(static_cast<double>(minDeg) + t * static_cast<double>(maxDeg - minDeg))),
            minDeg,
            maxDeg);
        return intDegreesToNormalized(deg);
    }

    float sliderProportionFromNormalized(double norm) const noexcept {
        const int minDeg = minSliderDeg();
        const int maxDeg = maxSliderDeg();
        const int deg = std::clamp(normalizedToIntDegrees(norm), minDeg, maxDeg);
        return std::clamp(
            static_cast<float>(deg - minDeg) / static_cast<float>(maxDeg - minDeg),
            0.0f,
            1.0f);
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
        if (window_) {
            UpdateWindow(window_);
        }
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

        Box plotRect{};
        float cx = 0.0f;
        float cy = 0.0f;
        float halfUnit = 0.0f;

        {
            Graphics g(backBuffer_.get());
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetInterpolationMode(InterpolationModeBilinear);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            g.ScaleTransform(scale_, scale_);
            g.Clear(kBgMain);

            paintScopeGrid(g, plotRect, cx, cy, halfUnit);
            paintSliderCard(g);
            paintWatermark(g);
        }

        // Fast direct-pixel scatter plot rendering via LockBits (skipped completely when collapsed)
        if (!scopeCollapsed_ && halfUnit > 4.0f && history_.count() > 0) {
            paintScatterPointsFast(pixelW, pixelH, plotRect, cx, cy, halfUnit);
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

    void paintScopeGrid(Graphics& g, Box& outPlotRect, float& outCx, float& outCy, float& outHalfUnit) {
        fillRoundRect(g, scopeCardBox_, kBgCard, 6.0f);
        drawRoundRect(g, scopeCardBox_, kBorderHairline, 6.0f, 1.0f);

        Box inner{
            scopeCardBox_.x + 10.0f,
            scopeCardBox_.y + 8.0f,
            std::max(10.0f, scopeCardBox_.w - 20.0f),
            std::max(10.0f, scopeCardBox_.h - 16.0f)
        };

        const Box headerBounds{inner.x, inner.y, inner.w, 22.0f};

        // Header dot (amber when active, dim when collapsed) + "声场相位可视化" title + collapse toggle button
        const float dotSize = 7.0f;
        SolidBrush dotBrush(scopeCollapsed_ ? kTextDim : kAmberTrace);
        g.FillEllipse(
            &dotBrush,
            headerBounds.x,
            headerBounds.y + (headerBounds.h - dotSize) * 0.5f,
            dotSize,
            dotSize);

        drawLabel(
            g,
            words_->scopeTitle,
            Box{headerBounds.x + dotSize + 7.0f, headerBounds.y, std::max(10.0f, headerBounds.w - 75.0f), headerBounds.h},
            13.0f,
            kTextPrimary,
            true,
            false,
            StringAlignmentNear);

        // Collapse / Expand toggle button on the right of "声场相位可视化"
        fillRoundRect(
            g,
            toggleBtnBox_,
            hoveringToggle_ ? kBgSurfaceHover : kBgField,
            4.0f);
        drawRoundRect(
            g,
            toggleBtnBox_,
            hoveringToggle_ ? kBorderActive : kBorderHairline,
            4.0f,
            1.0f);

        drawLabel(
            g,
            scopeCollapsed_ ? words_->expandLabel : words_->collapseLabel,
            Box{toggleBtnBox_.x + 6.0f, toggleBtnBox_.y, toggleBtnBox_.w - 20.0f, toggleBtnBox_.h},
            11.0f,
            hoveringToggle_ ? kTextPrimary : kTextSecondary,
            false,
            false,
            StringAlignmentCenter);

        // Draw crisp chevron arrow inside toggleBtnBox_
        {
            const float chevCx = toggleBtnBox_.x + toggleBtnBox_.w - 10.0f;
            const float chevCy = toggleBtnBox_.y + toggleBtnBox_.h * 0.5f;
            Pen chevPen(hoveringToggle_ ? kTextPrimary : kTextSecondary, 1.4f);
            chevPen.SetStartCap(LineCapRound);
            chevPen.SetEndCap(LineCapRound);
            if (scopeCollapsed_) {
                // Downward chevron (expand)
                g.DrawLine(&chevPen, chevCx - 3.5f, chevCy - 1.5f, chevCx, chevCy + 2.0f);
                g.DrawLine(&chevPen, chevCx, chevCy + 2.0f, chevCx + 3.5f, chevCy - 1.5f);
            } else {
                // Upward chevron (collapse)
                g.DrawLine(&chevPen, chevCx - 3.5f, chevCy + 1.5f, chevCx, chevCy - 2.0f);
                g.DrawLine(&chevPen, chevCx, chevCy - 2.0f, chevCx + 3.5f, chevCy + 1.5f);
            }
        }

        if (scopeCollapsed_) {
            return;
        }

        inner.y += 26.0f;
        inner.h = std::max(0.0f, inner.h - 26.0f);

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

        outPlotRect = plotRect;
        outCx = cx;
        outCy = cy;
        outHalfUnit = halfUnit;

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

        // Prominent 45 deg CCW rotated L (-45 deg top-left) and R (+45 deg top-right) axes
        // plus crisp perpendicular tick marks at both 45 deg unit-circle intersections
        {
            const float diagReach = halfUnit * 0.76f;
            Pen diagPen(withAlpha(kBorderHairline, 0.90f), 1.0f);
            // R axis: bottom-left to top-right (+45 deg)
            g.DrawLine(&diagPen, cx - diagReach, cy + diagReach, cx + diagReach, cy - diagReach);
            // L axis: bottom-right to top-left (-45 deg)
            g.DrawLine(&diagPen, cx + diagReach, cy + diagReach, cx - diagReach, cy - diagReach);

            // Perpendicular ticks at +/-45 deg on the unit circle (typical far left L and far right R)
            const float u45 = halfUnit * kInvSqrt2;
            const float tickHalf = 4.5f * kInvSqrt2;
            Pen tick45Pen(withAlpha(kAmberTrace, 0.75f), 1.4f);
            // Top-Left (-45 deg -> Far Left L): point is (cx - u45, cy - u45)
            g.DrawLine(&tick45Pen,
                       (cx - u45) - tickHalf, (cy - u45) + tickHalf,
                       (cx - u45) + tickHalf, (cy - u45) - tickHalf);
            // Top-Right (+45 deg -> Far Right R): point is (cx + u45, cy - u45)
            g.DrawLine(&tick45Pen,
                       (cx + u45) - tickHalf, (cy - u45) - tickHalf,
                       (cx + u45) + tickHalf, (cy - u45) + tickHalf);
        }

        // Axis labels: L at top-left, M at top-center, R at top-right
        drawLabel(g, L"L", Box{plotRect.x + 6.0f, plotRect.y + 4.0f, 20.0f, 12.0f}, 10.0f, kTextDim, false, true, StringAlignmentNear);
        drawLabel(g, L"R", Box{plotRect.x + plotRect.w - 26.0f, plotRect.y + 4.0f, 20.0f, 12.0f}, 10.0f, kTextDim, false, true, StringAlignmentFar);
        drawLabel(g, L"M", Box{cx - 10.0f, plotRect.y + 2.0f, 20.0f, 12.0f}, 10.0f, kTextDim, false, true, StringAlignmentCenter);
    }

    void paintScatterPointsFast(int pixelW,
                                int pixelH,
                                const Box& plotRect,
                                float cx,
                                float cy,
                                float halfUnit) {
        const int validCount = history_.count();
        if (validCount <= 0 || !backBuffer_) {
            return;
        }

        const int clipX0 = std::clamp(static_cast<int>(std::ceil((plotRect.x + 1.0f) * scale_)), 0, pixelW);
        const int clipY0 = std::clamp(static_cast<int>(std::ceil((plotRect.y + 1.0f) * scale_)), 0, pixelH);
        const int clipX1 = std::clamp(static_cast<int>(std::floor((plotRect.x + plotRect.w - 1.0f) * scale_)), 0, pixelW);
        const int clipY1 = std::clamp(static_cast<int>(std::floor((plotRect.y + plotRect.h - 1.0f) * scale_)), 0, pixelH);
        if (clipX1 <= clipX0 || clipY1 <= clipY0) {
            return;
        }

        Rect lockRect(0, 0, pixelW, pixelH);
        BitmapData bmpData{};
        if (backBuffer_->LockBits(&lockRect, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppPARGB, &bmpData) != Ok) {
            return;
        }

        auto* scan0 = static_cast<uint8_t*>(bmpData.Scan0);
        const int stride = bmpData.Stride;
        const float pxCx = cx * scale_;
        const float pxCy = cy * scale_;
        const float pxHalfUnit = halfUnit * scale_;

        constexpr uint8_t kAmberR = 0xf5;
        constexpr uint8_t kAmberG = 0x9e;
        constexpr uint8_t kAmberB = 0x0b;

        for (int i = 0; i < validCount; ++i) {
            const ScatterPoint pt = history_.scatterPointAt(i);
            if (!std::isfinite(pt.r) || !std::isfinite(pt.l) || pt.alpha < 0.025f) {
                continue;
            }

            // Age-weighted fade combined with per-point exponential decay
            const float orderFrac = static_cast<float>(i + 1) / static_cast<float>(validCount);
            const float effAlpha = std::clamp(pt.alpha * (0.20f + 0.80f * orderFrac), 0.0f, 1.0f);
            if (effAlpha < 0.025f) {
                continue;
            }

            const auto [rx, ry] = rotate45CCW(pt.r, pt.l);
            const int x = static_cast<int>(std::lround(pxCx + rx * pxHalfUnit));
            const int y = static_cast<int>(std::lround(pxCy - ry * pxHalfUnit));

            const uint8_t coreA = static_cast<uint8_t>(std::clamp(static_cast<int>(effAlpha * 245.0f), 0, 255));
            const uint8_t ringA = static_cast<uint8_t>(std::clamp(static_cast<int>(effAlpha * 130.0f), 0, 255));
            const uint8_t diagA = static_cast<uint8_t>(std::clamp(static_cast<int>(effAlpha * 60.0f), 0, 255));

            // 3x3 soft anti-aliased glowing scatter point
            blendPixelPARGB(scan0, stride, clipX0, clipY0, clipX1, clipY1, x, y, kAmberR, kAmberG, kAmberB, coreA);
            blendPixelPARGB(scan0, stride, clipX0, clipY0, clipX1, clipY1, x - 1, y, kAmberR, kAmberG, kAmberB, ringA);
            blendPixelPARGB(scan0, stride, clipX0, clipY0, clipX1, clipY1, x + 1, y, kAmberR, kAmberG, kAmberB, ringA);
            blendPixelPARGB(scan0, stride, clipX0, clipY0, clipX1, clipY1, x, y - 1, kAmberR, kAmberG, kAmberB, ringA);
            blendPixelPARGB(scan0, stride, clipX0, clipY0, clipX1, clipY1, x, y + 1, kAmberR, kAmberG, kAmberB, ringA);

            if (effAlpha > 0.35f) {
                blendPixelPARGB(scan0, stride, clipX0, clipY0, clipX1, clipY1, x - 1, y - 1, kAmberR, kAmberG, kAmberB, diagA);
                blendPixelPARGB(scan0, stride, clipX0, clipY0, clipX1, clipY1, x + 1, y - 1, kAmberR, kAmberG, kAmberB, diagA);
                blendPixelPARGB(scan0, stride, clipX0, clipY0, clipX1, clipY1, x - 1, y + 1, kAmberR, kAmberG, kAmberB, diagA);
                blendPixelPARGB(scan0, stride, clipX0, clipY0, clipX1, clipY1, x + 1, y + 1, kAmberR, kAmberG, kAmberB, diagA);
            }
        }

        backBuffer_->UnlockBits(&bmpData);
    }

    void paintSliderCard(Graphics& g) {
        const bool editVisible = edit_ && IsWindowVisible(edit_);
        const bool editFocused = editVisible && (GetFocus() == edit_);
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
            Box{sliderCardBox_.x + 12.0f, headerY, 64.0f, 20.0f},
            12.5f,
            kTextPrimary,
            true,
            false,
            StringAlignmentNear);

        // Extended range toggle button ("扩展 ±180°") right next to "旋转角度"
        fillRoundRect(
            g,
            rangeBtnBox_,
            extendedRange_ ? withAlpha(kAmberTrace, 0.16f) : (hoveringRangeBtn_ ? kBgSurfaceHover : kBgField),
            4.0f);
        drawRoundRect(
            g,
            rangeBtnBox_,
            extendedRange_ ? kAmberTrace : (hoveringRangeBtn_ ? kBorderActive : kBorderHairline),
            4.0f,
            extendedRange_ ? 1.2f : 1.0f);
        drawLabel(
            g,
            words_->extendRangeLabel,
            rangeBtnBox_,
            10.5f,
            extendedRange_ ? kAmberTrace : (hoveringRangeBtn_ ? kTextPrimary : kTextSecondary),
            extendedRange_,
            false,
            StringAlignmentCenter);

        // Integer input textbox frame + degree unit label
        fillRoundRect(g, editFrameBox_, kBgField, 4.0f);
        drawRoundRect(
            g,
            editFrameBox_,
            editFocused ? kAmberTrace : kBorderHairline,
            4.0f,
            editFocused ? 1.3f : 1.0f);

        // When not actively typing in the native EDIT control, draw the integer degree
        // directly in the GDI+ backbuffer so it updates on the exact same frame as the slider thumb!
        if (!editVisible) {
            const int deg = normalizedToIntDegrees(normalizedAngle_);
            wchar_t degBuf[16]{};
            std::swprintf(degBuf, std::size(degBuf), L"%d", deg);
            drawLabel(
                g,
                degBuf,
                Box{editInnerBox_.x, editFrameBox_.y, editInnerBox_.w, editFrameBox_.h},
                13.0f,
                deg == 0 ? kTextPrimary : kAmberTrace,
                true,
                true,
                StringAlignmentFar);
        }

        drawLabel(
            g,
            L"\x00B0",
            Box{editFrameBox_.x + editFrameBox_.w - 15.0f, editFrameBox_.y, 12.0f, editFrameBox_.h},
            12.0f,
            kTextSecondary,
            false,
            true,
            StringAlignmentCenter);

        // Endpoint scale labels (-45 deg / +45 deg by default, or -180 deg / +180 deg when extendedRange_ is active)
        drawLabel(
            g,
            extendedRange_ ? L"-180\x00B0" : L"-45\x00B0",
            Box{sliderCardBox_.x + 8.0f, sliderTrackBox_.y - 7.0f, 36.0f, 20.0f},
            10.5f,
            kTextDim,
            false,
            true,
            StringAlignmentFar);
        drawLabel(
            g,
            extendedRange_ ? L"+180\x00B0" : L"+45\x00B0",
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
        const float sliderProp = sliderProportionFromNormalized(normalizedAngle_);
        const float thumbX = sliderTrackBox_.x + sliderTrackBox_.w * sliderProp;
        const float trackCenterY = sliderTrackBox_.y + sliderTrackBox_.h * 0.5f;

        // Tick marks on the slider track
        {
            Pen centerTickPen(kBorderActive, 1.2f);
            g.DrawLine(&centerTickPen, centerX, sliderTrackBox_.y - 4.0f, centerX, sliderTrackBox_.y + sliderTrackBox_.h + 4.0f);
            drawLabel(g, L"0\x00B0", Box{centerX - 14.0f, sliderTrackBox_.y + 8.0f, 28.0f, 14.0f}, 9.5f, kTextDim, false, true, StringAlignmentCenter);

            if (extendedRange_) {
                const float left45X = sliderTrackBox_.x + sliderTrackBox_.w * 0.375f;  // -45 deg
                const float right45X = sliderTrackBox_.x + sliderTrackBox_.w * 0.625f; // +45 deg
                Pen tick45Pen(withAlpha(kTextSecondary, 0.75f), 1.1f);
                g.DrawLine(&tick45Pen, left45X, sliderTrackBox_.y - 3.5f, left45X, sliderTrackBox_.y + sliderTrackBox_.h + 3.5f);
                g.DrawLine(&tick45Pen, right45X, sliderTrackBox_.y - 3.5f, right45X, sliderTrackBox_.y + sliderTrackBox_.h + 3.5f);
                drawLabel(g, L"-45\x00B0", Box{left45X - 18.0f, sliderTrackBox_.y + 8.0f, 36.0f, 14.0f}, 9.5f, kTextDim, false, true, StringAlignmentCenter);
                drawLabel(g, L"+45\x00B0", Box{right45X - 18.0f, sliderTrackBox_.y + 8.0f, 36.0f, 14.0f}, 9.5f, kTextDim, false, true, StringAlignmentCenter);
            }
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
                    self->endTextEdit(true);
                    if (self->window_) {
                        SetFocus(self->window_);
                    }
                    return 0;
                }
                if (wParam == VK_ESCAPE) {
                    self->endTextEdit(false);
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
                self->endTextEdit(true);
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

            case WM_TIMER:
                if (wParam == kScopeTimerId) {
                    self->onTimerTick();
                    return 0;
                }
                break;

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
                    self->beginTextEdit();
                    return 0;
                }
                if (self->edit_ && IsWindowVisible(self->edit_)) {
                    self->endTextEdit(true);
                }
                if (self->toggleBtnBox_.contains(lx, ly)) {
                    SetFocus(hwnd);
                    self->toggleScopeCollapsed();
                    return 0;
                }
                if (self->rangeBtnBox_.contains(lx, ly)) {
                    SetFocus(hwnd);
                    self->toggleExtendedRange();
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
                    const bool hotSlider = self->sliderHitBox_.contains(lx, ly);
                    const bool hotToggle = self->toggleBtnBox_.contains(lx, ly);
                    const bool hotRange = self->rangeBtnBox_.contains(lx, ly);
                    if (hotSlider != self->hoveringSlider_
                        || hotToggle != self->hoveringToggle_
                        || hotRange != self->hoveringRangeBtn_) {
                        self->hoveringSlider_ = hotSlider;
                        self->hoveringToggle_ = hotToggle;
                        self->hoveringRangeBtn_ = hotRange;
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
                if (message == WM_LBUTTONDBLCLK && self->toggleBtnBox_.contains(lx, ly)) {
                    self->toggleScopeCollapsed();
                    return 0;
                }
                if (message == WM_LBUTTONDBLCLK && self->rangeBtnBox_.contains(lx, ly)) {
                    self->toggleExtendedRange();
                    return 0;
                }
                if (!self->editFrameBox_.contains(lx, ly)
                    && !self->rangeBtnBox_.contains(lx, ly)
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
                if (self->hoveringSlider_ || self->hoveringToggle_ || self->hoveringRangeBtn_) {
                    self->hoveringSlider_ = false;
                    self->hoveringToggle_ = false;
                    self->hoveringRangeBtn_ = false;
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
