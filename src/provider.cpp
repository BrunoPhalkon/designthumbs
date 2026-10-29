// Objeto COM IThumbnailProvider + exportacoes da DLL
#include "common.h"
#include "reg.h"
#include <thumbcache.h>
#include <propsys.h>
#include <shlwapi.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <new>

LONG g_dllRefs = 0;

// {e357fccd-a995-4576-b01f-234630154e96}
static const IID kIID_IThumbnailProvider = {0xe357fccd, 0xa995, 0x4576, {0xb0, 0x1f, 0x23, 0x46, 0x30, 0x15, 0x4e, 0x96}};

namespace {

enum class Kind { Unknown, PSD, PDF, AI, AIPostScript, EPS, INDD, SVG };

std::wstring LowerExt(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    size_t slash = name.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return L"";
    std::wstring e = name.substr(dot);
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e;
}

bool Contains(const uint8_t* b, size_t n, const char* pat) {
    size_t m = strlen(pat);
    if (n < m) return false;
    return std::search(b, b + n, pat, pat + m) != b + n;
}

Kind Sniff(Source& src) {
    uint8_t head[65536];
    size_t n = (size_t)std::min<uint64_t>(sizeof(head), src.size());
    if (n < 4 || !src.read(0, head, n)) return Kind::Unknown;
    std::wstring ext = LowerExt(src.name());
    bool aiExt = ext == L".ai" || ext == L".ait";

    if (memcmp(head, "8BPS", 4) == 0) return Kind::PSD;
    static const uint8_t inddGuid[16] = {0x06, 0x06, 0xED, 0xF5, 0xD8, 0x1D, 0x46, 0xE5,
                                         0xBD, 0x31, 0xEF, 0xE7, 0xFE, 0x74, 0xB7, 0x1D};
    if (n >= 16 && memcmp(head, inddGuid, 16) == 0) return Kind::INDD;
    if (head[0] == 0xC5 && head[1] == 0xD0 && head[2] == 0xD3 && head[3] == 0xC6) return Kind::EPS;
    if (Contains(head, std::min<size_t>(n, 1024), "%PDF-")) {
        if (aiExt) return Kind::AI;
        if (ext.empty() && Contains(head, n, "Adobe Illustrator")) return Kind::AI;
        return Kind::PDF;
    }
    if (memcmp(head, "%!PS", 4) == 0) return aiExt ? Kind::AIPostScript : Kind::EPS;
    if (ext == L".svg" || Contains(head, std::min<size_t>(n, 8192), "<svg")) return Kind::SVG;
    if (aiExt) return Kind::AIPostScript;
    return Kind::Unknown;
}

void ApplyBackground(Image& img, DWORD bg) {
    if (bg == BG_TRANSPARENT) return;
    int cell = std::max(4, std::min(img.w, img.h) / 24);
    for (int y = 0; y < img.h; ++y) {
        uint8_t* p = &img.px[(size_t)y * img.w * 4];
        for (int x = 0; x < img.w; ++x, p += 4) {
            uint8_t a = p[3];
            if (a == 255) continue;
            uint8_t back = 255;
            if (bg == BG_CHECKER) back = (((x / cell) + (y / cell)) & 1) ? 204 : 255;
            for (int c = 0; c < 3; ++c) p[c] = (uint8_t)((p[c] * a + back * (255 - a) + 127) / 255);
            p[3] = 255;
        }
    }
}

void DrawBorder(Image& img) {
    if (img.w < 3 || img.h < 3) return;
    auto set = [&](int x, int y) {
        uint8_t* p = &img.px[((size_t)y * img.w + x) * 4];
        p[0] = p[1] = p[2] = 190; p[3] = 255;
    };
    for (int x = 0; x < img.w; ++x) { set(x, 0); set(x, img.h - 1); }
    for (int y = 0; y < img.h; ++y) { set(0, y); set(img.w - 1, y); }
}

HBITMAP ToHBitmap(const Image& img, bool& hasAlpha) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = img.w;
    bi.bmiHeader.biHeight = -img.h;   // de cima para baixo
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hb || !bits) return nullptr;
    memcpy(bits, img.px.data(), img.px.size());
    hasAlpha = false;
    for (size_t i = 3; i < img.px.size(); i += 4)
        if (img.px[i] != 255) { hasAlpha = true; break; }
    return hb;
}

class ThumbProvider : public IInitializeWithStream, public IThumbnailProvider {
public:
    ThumbProvider() { InterlockedIncrement(&g_dllRefs); }
    virtual ~ThumbProvider() { if (stream_) stream_->Release(); InterlockedDecrement(&g_dllRefs); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IInitializeWithStream) *ppv = static_cast<IInitializeWithStream*>(this);
        else if (riid == kIID_IThumbnailProvider) *ppv = static_cast<IThumbnailProvider*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return r;
    }

    IFACEMETHODIMP Initialize(IStream* s, DWORD) override {
        if (stream_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        if (!s) return E_INVALIDARG;
        stream_ = s; stream_->AddRef();
        return S_OK;
    }

    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* alpha) override {
        if (!phbmp || !alpha) return E_POINTER;
        *phbmp = nullptr; *alpha = WTSAT_UNKNOWN;
        if (!stream_) return E_UNEXPECTED;
        try {
            Settings st = LoadSettings();
            Source src(stream_);
            if (src.size() == 0) return E_FAIL;
            if (st.maxFileMB && src.size() > ((uint64_t)st.maxFileMB << 20)) return E_FAIL;
            int target = (int)std::min<UINT>(cx ? cx : 256, st.maxThumb);

            Image img;
            bool page = false, ok = false;
            switch (Sniff(src)) {
            case Kind::PSD:  ok = DecodePSD(src, target, st, img); break;
            case Kind::PDF:  ok = DecodePDF(src, target, st, img, false); page = ok; break;
            case Kind::AI:
                if (st.aiMode == 1) ok = DecodeXMPThumb(src, target, img);
                if (!ok) { ok = DecodePDF(src, target, st, img, true); page = ok; }
                if (!ok) ok = DecodeXMPThumb(src, target, img);
                break;
            case Kind::AIPostScript: ok = DecodeXMPThumb(src, target, img); break;
            case Kind::EPS:  ok = DecodeEPS(src, target, img); break;
            case Kind::INDD: ok = DecodeXMPThumb(src, target, img); break;
            case Kind::SVG:  ok = DecodeSVG(src, target, img); break;
            default: break;
            }
            if (!ok || img.empty()) return E_FAIL;

            ApplyBackground(img, st.background);
            if (page && st.border) DrawBorder(img);
            bool hasAlpha = false;
            HBITMAP hb = ToHBitmap(img, hasAlpha);
            if (!hb) return E_OUTOFMEMORY;
            *phbmp = hb;
            *alpha = hasAlpha ? WTSAT_ARGB : WTSAT_RGB;
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }

private:
    LONG refs_ = 1;
    IStream* stream_ = nullptr;
};

class Factory : public IClassFactory {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { InterlockedIncrement(&g_dllRefs); return 2; }
    IFACEMETHODIMP_(ULONG) Release() override { InterlockedDecrement(&g_dllRefs); return 1; }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        ThumbProvider* p = new (std::nothrow) ThumbProvider();
        if (!p) return E_OUTOFMEMORY;
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_dllRefs); else InterlockedDecrement(&g_dllRefs);
        return S_OK;
    }
};

Factory g_factory;

} // namespace

extern "C" {

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid != CLSID_DesignThumbs) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return g_dllRefs == 0 ? S_OK : S_FALSE; }

// regsvr32 (alternativa ao app): registra para o usuario atual com todos os formatos
STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH * 2];
    GetModuleFileNameW(ThisModule(), path, MAX_PATH * 2);
    if (!RegisterClsid(path)) return E_FAIL;
    bool all[16];
    for (int i = 0; i < kFormatCount; ++i) all[i] = true;
    ApplyFormats(all, false);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    UnregisterAllExtensions();
    UnregisterClsid();
    return S_OK;
}

// Usado pelo app de teste: gera a miniatura direto de um arquivo
__declspec(dllexport) HRESULT WINAPI DT_RenderFile(const wchar_t* path, UINT cx, HBITMAP* out, DWORD* alphaType) {
    IStream* s = nullptr;
    HRESULT hr = SHCreateStreamOnFileEx(path, STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &s);
    if (FAILED(hr)) return hr;
    ThumbProvider* p = new (std::nothrow) ThumbProvider();
    if (!p) { s->Release(); return E_OUTOFMEMORY; }
    p->Initialize(s, STGM_READ);
    WTS_ALPHATYPE a = WTSAT_UNKNOWN;
    hr = p->GetThumbnail(cx, out, &a);
    if (alphaType) *alphaType = a;
    p->Release();
    s->Release();
    return hr;
}

} // extern "C"
