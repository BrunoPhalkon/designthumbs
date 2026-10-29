// Teste: render.exe <tamanho> <saida_dir> arquivo1 arquivo2 ...  -> gera PNG de cada miniatura
#include <windows.h>
#include <wincodec.h>
#include <cstdio>
#include <string>

typedef HRESULT(WINAPI* RenderFn)(const wchar_t*, UINT, HBITMAP*, DWORD*);

static bool SavePng(HBITMAP hb, const std::wstring& path) {
    BITMAP bm = {};
    GetObjectW(hb, sizeof(bm), &bm);
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f))) return false;
    IWICBitmap* wb = nullptr;
    HRESULT hr = f->CreateBitmapFromHBITMAP(hb, nullptr, WICBitmapUseAlpha, &wb);
    IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr;
    if (SUCCEEDED(hr)) hr = f->CreateStream(&st);
    if (SUCCEEDED(hr)) hr = st->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&fr, nullptr);
    if (SUCCEEDED(hr)) hr = fr->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = fr->WriteSource(wb, nullptr);
    if (SUCCEEDED(hr)) hr = fr->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (fr) fr->Release(); if (enc) enc->Release(); if (st) st->Release(); if (wb) wb->Release();
    f->Release();
    return SUCCEEDED(hr);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) { wprintf(L"uso: render <px> <saida> arquivos...\n"); return 1; }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HMODULE m = LoadLibraryW(L"DesignThumbs.dll");
    if (!m) { wprintf(L"falha ao carregar DLL: %lu\n", GetLastError()); return 2; }
    RenderFn fn = (RenderFn)(void*)GetProcAddress(m, "DT_RenderFile");
    UINT px = (UINT)_wtoi(argv[1]);
    std::wstring outDir = argv[2];
    for (int i = 3; i < argc; ++i) {
        HBITMAP hb = nullptr; DWORD alpha = 0;
        DWORD t0 = GetTickCount();
        HRESULT hr = fn(argv[i], px, &hb, &alpha);
        DWORD dt = GetTickCount() - t0;
        std::wstring name = argv[i];
        size_t s = name.find_last_of(L"\\/");
        if (s != std::wstring::npos) name = name.substr(s + 1);
        if (SUCCEEDED(hr) && hb) {
            BITMAP bm = {}; GetObjectW(hb, sizeof(bm), &bm);
            std::wstring out = outDir + L"\\" + name + L".png";
            bool ok = SavePng(hb, out);
            wprintf(L"OK   %ls  %dx%d alpha=%lu  %lums  png=%d\n", name.c_str(), bm.bmWidth, bm.bmHeight, alpha, dt, ok);
            DeleteObject(hb);
        } else {
            wprintf(L"FAIL %ls  hr=0x%08lx  %lums\n", name.c_str(), (unsigned long)hr, dt);
        }
    }
    return 0;
}
