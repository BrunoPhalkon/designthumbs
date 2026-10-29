// SVG via lunasvg
#include "common.h"
#include <lunasvg.h>
#include <algorithm>
#include <cstring>

namespace {
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION g_lock;

std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

BOOL CALLBACK InitFonts(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_lock);
    wchar_t win[MAX_PATH];
    UINT n = GetWindowsDirectoryW(win, MAX_PATH);
    std::wstring fonts = std::wstring(win, n) + L"\\Fonts\\";
    struct F { const char* family; bool b, i; const wchar_t* file; };
    static const F list[] = {
        {"sans-serif", false, false, L"arial.ttf"}, {"sans-serif", true, false, L"arialbd.ttf"},
        {"sans-serif", false, true, L"ariali.ttf"}, {"sans-serif", true, true, L"arialbi.ttf"},
        {"Arial", false, false, L"arial.ttf"},      {"Arial", true, false, L"arialbd.ttf"},
        {"Helvetica", false, false, L"arial.ttf"},  {"Helvetica", true, false, L"arialbd.ttf"},
        {"serif", false, false, L"times.ttf"},      {"serif", true, false, L"timesbd.ttf"},
        {"Times New Roman", false, false, L"times.ttf"},
        {"monospace", false, false, L"consola.ttf"},
        {"Segoe UI", false, false, L"segoeui.ttf"}, {"Segoe UI", true, false, L"segoeuib.ttf"},
        {"Verdana", false, false, L"verdana.ttf"},  {"Tahoma", false, false, L"tahoma.ttf"},
        {"Georgia", false, false, L"georgia.ttf"},  {"Calibri", false, false, L"calibri.ttf"},
    };
    for (const F& f : list) {
        std::string path = Utf8(fonts + f.file);
        lunasvg_add_font_face_from_file(f.family, f.b, f.i, path.c_str());
    }
    return TRUE;
}
} // namespace

bool DecodeSVG(Source& src, int target, Image& out) {
    std::vector<uint8_t> data;
    if (!src.readAll(data, 64ull << 20)) return false;
    if (data.size() >= 2 && data[0] == 0x1F && data[1] == 0x8B) return false;   // .svgz (gzip) nao suportado
    InitOnceExecuteOnce(&g_once, InitFonts, nullptr, nullptr);

    EnterCriticalSection(&g_lock);
    bool ok = false;
    auto doc = lunasvg::Document::loadFromData((const char*)data.data(), data.size());
    if (doc) {
        float w = doc->width(), h = doc->height();
        bool useBox = false;
        lunasvg::Box bb;
        if (w <= 0 || h <= 0) {          // sem width/height/viewBox: enquadra pelo conteudo
            bb = doc->boundingBox();
            w = bb.w; h = bb.h;
            useBox = true;
        }
        int ow, oh;
        FitSize(w, h, target, ow, oh);
        if (ow > 0 && oh > 0) {
            lunasvg::Bitmap bmp;
            if (useBox) {
                float s = std::min(ow / w, oh / h);
                bmp = lunasvg::Bitmap(ow, oh);
                bmp.clear(0x00000000);
                doc->render(bmp, lunasvg::Matrix(s, 0, 0, s, -bb.x * s, -bb.y * s));
            } else {
                bmp = doc->renderToBitmap(ow, oh, 0x00000000);
            }
            if (!bmp.isNull()) {
                out.alloc(bmp.width(), bmp.height());
                const uint8_t* d = bmp.data();
                int stride = bmp.stride();
                for (int y = 0; y < out.h; ++y) {
                    const uint8_t* s = d + (size_t)y * stride;
                    uint8_t* o = &out.px[(size_t)y * out.w * 4];
                    for (int x = 0; x < out.w; ++x) {
                        // ARGB32 pre-multiplicado (B,G,R,A na memoria) -> nao pre-multiplicado
                        uint8_t b = s[x * 4], g = s[x * 4 + 1], r = s[x * 4 + 2], a = s[x * 4 + 3];
                        if (a && a < 255) {
                            b = (uint8_t)std::min(255, b * 255 / a);
                            g = (uint8_t)std::min(255, g * 255 / a);
                            r = (uint8_t)std::min(255, r * 255 / a);
                        }
                        o[x * 4] = b; o[x * 4 + 1] = g; o[x * 4 + 2] = r; o[x * 4 + 3] = a;
                    }
                }
                ok = true;
            }
        }
    }
    LeaveCriticalSection(&g_lock);
    return ok;
}
