#include "icons.h"
#include "resource.h"

#include <algorithm>
#include <cmath>
#include <objidl.h>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>
#include <shlwapi.h>

#include <map>
#include <memory>

using namespace Gdiplus;

namespace {

HINSTANCE g_instance = nullptr;
ULONG_PTR g_gdiplusToken = 0;
std::map<int, std::unique_ptr<Bitmap>> g_images;  // decoded once, reused for every redraw

// Loads an embedded PNG and copies it into a bitmap that owns its pixels.
Bitmap* LoadPng(int id) {
    auto it = g_images.find(id);
    if (it != g_images.end()) return it->second.get();

    Bitmap* result = nullptr;
    HRSRC res = FindResourceW(g_instance, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (res) {
        HGLOBAL data = LoadResource(g_instance, res);
        DWORD size = SizeofResource(g_instance, res);
        if (IStream* stream = SHCreateMemStream(static_cast<const BYTE*>(LockResource(data)), size)) {
            std::unique_ptr<Bitmap> decoded(Bitmap::FromStream(stream));
            if (decoded && decoded->GetLastStatus() == Ok) {
                result = decoded->Clone(0, 0, decoded->GetWidth(), decoded->GetHeight(), PixelFormat32bppARGB);
            }
            decoded.reset();
            stream->Release();
        }
    }
    g_images[id].reset(result);
    return result;
}

int DeviceImage(const DeviceState& d, bool light) {
    switch (d.kind) {
        case DeviceKind::Keyboard: return light ? IDR_KEYBOARD : IDR_KEYBOARD_DARK;
        case DeviceKind::Headset: return light ? IDR_HEADSET : IDR_HEADSET_DARK;
        case DeviceKind::Gamepad: return light ? IDR_GAMEPAD : IDR_GAMEPAD_DARK;
        case DeviceKind::Mouse: return light ? IDR_MOUSE : IDR_MOUSE_DARK;
        default: return 0;  // battery only
    }
}

int LevelImage(const DeviceState& d, bool light) {
    if (d.status == PowerStatus::Charging) return light ? IDR_CHARGING : IDR_CHARGING_DARK;
    if (d.percent < 0) return light ? IDR_MISSING : IDR_MISSING_DARK;
    if (d.percent < 10) return IDR_IND_10;
    if (d.percent < 50) return IDR_IND_30;
    if (d.percent < 85) return IDR_IND_50;
    return IDR_IND_100;
}

// --- crisp digits -----------------------------------------------------------
// Digits are built from solid, pixel-aligned bars (seven-segment style) with smoothing
// off, so they stay sharp at 16 px where font outlines turn into grey blur.

struct DigitLayout {
    int width = 0;       // width of a normal digit
    int height = 0;
    int stroke = 1;
    int gap = 1;
    int totalWidth = 0;
    bool fits = false;   // digits are wide enough to stay readable
};

int GlyphWidth(wchar_t c, const DigitLayout& l) { return c == L'1' ? l.stroke : l.width; }

DigitLayout LayoutDigits(const wchar_t* text, int availWidth, int size) {
    DigitLayout l;
    l.stroke = size >= 28 ? 2 : 1;  // 1 px bars up to 150% scaling, 2 px from 175%
    l.gap = l.stroke;
    l.height = std::min(size - 2, (int)std::lround(size * 0.75));
    if ((l.height - 3 * l.stroke) % 2) --l.height;  // equal upper and lower halves

    int n = (int)wcslen(text), ones = 0;
    for (const wchar_t* p = text; *p; ++p) ones += *p == L'1';
    int others = n - ones;
    int w = others ? (availWidth - ones * l.stroke - (n - 1) * l.gap) / others : l.stroke;
    w = std::min(w, (int)(l.height * 0.62));  // don't get wider than a digit should look
    l.width = w;
    // Keep a digit's shape: too tall for its width and it reads as bars ("100" -> "|||").
    if (others) {
        l.height = std::min(l.height, 2 * w + 3 * l.stroke);
        if ((l.height - 3 * l.stroke) % 2) --l.height;
    }
    // Readable only with a real gap inside "0"/"8": 1 px with 1 px bars, 2 px with 2 px bars.
    l.fits = others == 0 || w >= 3 * l.stroke;
    l.totalWidth = (n - 1) * l.gap;
    for (const wchar_t* p = text; *p; ++p) l.totalWidth += GlyphWidth(*p, l);
    return l;
}

void DrawDigits(Graphics& g, Brush& brush, const wchar_t* text, const DigitLayout& l, int x, int y) {
    // Segment bits: a=top b=upper right c=lower right d=bottom e=lower left f=upper left g=middle
    enum { A = 1, B = 2, C = 4, D = 8, E = 16, F = 32, G = 64 };
    static const int kSegments[10] = {A | B | C | D | E | F, B | C,         A | B | G | E | D, A | B | G | C | D,
                                      F | G | B | C,         A | F | G | C | D, A | F | G | E | C | D, A | B | C,
                                      A | B | C | D | E | F | G, A | B | C | D | F | G};
    const int t = l.stroke, h = l.height, mid = y + (h - t) / 2;

    SmoothingMode oldSmooth = g.GetSmoothingMode();
    PixelOffsetMode oldOffset = g.GetPixelOffsetMode();
    g.SetSmoothingMode(SmoothingModeNone);
    g.SetPixelOffsetMode(PixelOffsetModeNone);

    for (const wchar_t* p = text; *p; ++p) {
        int w = GlyphWidth(*p, l);
        if (*p == L'1') {
            g.FillRectangle(&brush, x, y, t, h);
        } else if (*p >= L'0' && *p <= L'9') {
            int s = kSegments[*p - L'0'];
            if (s & A) g.FillRectangle(&brush, x, y, w, t);
            if (s & G) g.FillRectangle(&brush, x, mid, w, t);
            if (s & D) g.FillRectangle(&brush, x, y + h - t, w, t);
            if (s & F) g.FillRectangle(&brush, x, y, t, mid - y + t);
            if (s & B) g.FillRectangle(&brush, x + w - t, y, t, mid - y + t);
            if (s & E) g.FillRectangle(&brush, x, mid, t, y + h - mid);
            if (s & C) g.FillRectangle(&brush, x + w - t, mid, t, y + h - mid);
        } else if (*p == L'?') {
            g.FillRectangle(&brush, x, y, w, t);                         // top
            g.FillRectangle(&brush, x + w - t, y, t, mid - y + t);       // upper right
            g.FillRectangle(&brush, x + (w - t) / 2, mid, w - (w - t) / 2, t);  // middle (right part)
            g.FillRectangle(&brush, x + (w - t) / 2, mid, t, (h - t) / 2 - t);  // stem
            g.FillRectangle(&brush, x + (w - t) / 2, y + h - t, t, t);   // dot
        }
        x += w + l.gap;
    }

    g.SetSmoothingMode(oldSmooth);
    g.SetPixelOffsetMode(oldOffset);
}

}  // namespace

void IconsInit(HINSTANCE instance) {
    g_instance = instance;
    GdiplusStartupInput input;
    GdiplusStartup(&g_gdiplusToken, &input, nullptr);
}

void IconsShutdown() {
    g_images.clear();
    if (g_gdiplusToken) GdiplusShutdown(g_gdiplusToken);
    g_gdiplusToken = 0;
}

bool IsLightTaskbar() {
    DWORD value = 0, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value != 0;
}

int TrayIconSize() {
    UINT dpi = 0;
    if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr)) dpi = GetDpiForWindow(tray);
    if (!dpi) dpi = GetDpiForSystem();
    return GetSystemMetricsForDpi(SM_CXSMICON, dpi);
}

HICON DrawTrayIcon(const DeviceState* device, bool numeric, bool light, int size) {
    DeviceState unknown;
    const DeviceState& d = device ? *device : unknown;

    Bitmap canvas(size, size, PixelFormat32bppARGB);
    Graphics g(&canvas);
    g.Clear(Color(0, 0, 0, 0));
    g.SetCompositingMode(CompositingModeSourceOver);
    g.SetCompositingQuality(CompositingQualityHighQuality);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetSmoothingMode(SmoothingModeHighQuality);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);

    if (numeric) {
        wchar_t text[8];
        if (d.percent < 0)
            wcscpy_s(text, L"?");
        else
            swprintf_s(text, L"%.0f", d.percent);

        SolidBrush brush(light ? Color(255, 0x00, 0x00, 0x00) : Color(255, 0xFF, 0xFF, 0xFF));

        // Digits right of the device picture; without room (e.g. "100") use the whole icon.
        Bitmap* devImg = nullptr;
        if (int devId = DeviceImage(d, light)) devImg = LoadPng(devId);
        int start = devImg ? (int)std::lround(size * 0.5) : 0;
        DigitLayout layout = LayoutDigits(text, size - start, size);
        if (devImg && !layout.fits) {
            devImg = nullptr;
            start = 0;
            layout = LayoutDigits(text, size, size);
        }

        if (devImg) {
            // Same device art and size as battery-picture mode (it sits in the left ~47%).
            ImageAttributes wrap;
            wrap.SetWrapMode(WrapModeTileFlipXY);
            Rect dest(0, 0, size, size);
            g.DrawImage(devImg, dest, 0, 0, (INT)devImg->GetWidth(), (INT)devImg->GetHeight(), UnitPixel, &wrap);
        }

        DrawDigits(g, brush, text, layout, start + (size - start - layout.totalWidth) / 2, (size - layout.height) / 2);
    } else {
        ImageAttributes wrap;
        wrap.SetWrapMode(WrapModeTileFlipXY);
        Rect dest(0, 0, size, size);
        for (int id : {LevelImage(d, light), light ? IDR_BATTERY : IDR_BATTERY_DARK, DeviceImage(d, light)}) {
            if (Bitmap* img = id ? LoadPng(id) : nullptr)
                g.DrawImage(img, dest, 0, 0, (INT)img->GetWidth(), (INT)img->GetHeight(), UnitPixel, &wrap);
        }
    }

    g.Flush(FlushIntentionSync);
    HICON icon = nullptr;
    canvas.GetHICON(&icon);
    return icon;
}
