// PDF e Illustrator (.ai salvo com "Criar arquivo compativel com PDF") via PDFium
#include "common.h"
#include "fpdfview.h"
#include <algorithm>
#include <cstring>

namespace {

struct Pdfium {
    decltype(&FPDF_InitLibraryWithConfig) InitLibraryWithConfig = nullptr;
    decltype(&FPDF_LoadCustomDocument) LoadCustomDocument = nullptr;
    decltype(&FPDF_CloseDocument) CloseDocument = nullptr;
    decltype(&FPDF_GetPageCount) GetPageCount = nullptr;
    decltype(&FPDF_LoadPage) LoadPage = nullptr;
    decltype(&FPDF_ClosePage) ClosePage = nullptr;
    decltype(&FPDF_GetPageWidthF) GetPageWidthF = nullptr;
    decltype(&FPDF_GetPageHeightF) GetPageHeightF = nullptr;
    decltype(&FPDFBitmap_Create) BitmapCreate = nullptr;
    decltype(&FPDFBitmap_FillRect) BitmapFillRect = nullptr;
    decltype(&FPDFBitmap_GetBuffer) BitmapGetBuffer = nullptr;
    decltype(&FPDFBitmap_GetStride) BitmapGetStride = nullptr;
    decltype(&FPDFBitmap_Destroy) BitmapDestroy = nullptr;
    decltype(&FPDF_RenderPageBitmap) RenderPageBitmap = nullptr;
    bool ok = false;
};

INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION g_lock;   // PDFium nao e thread-safe
Pdfium g_pdf;

template <class T> bool Load(HMODULE m, const char* name, T& fn) {
    fn = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    return fn != nullptr;
}

BOOL CALLBACK InitPdfium(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_lock);
    std::wstring path = ModuleDir() + L"\\pdfium.dll";
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) return TRUE;
    Pdfium& p = g_pdf;
    bool ok = Load(m, "FPDF_InitLibraryWithConfig", p.InitLibraryWithConfig) &&
              Load(m, "FPDF_LoadCustomDocument", p.LoadCustomDocument) &&
              Load(m, "FPDF_CloseDocument", p.CloseDocument) &&
              Load(m, "FPDF_GetPageCount", p.GetPageCount) &&
              Load(m, "FPDF_LoadPage", p.LoadPage) &&
              Load(m, "FPDF_ClosePage", p.ClosePage) &&
              Load(m, "FPDF_GetPageWidthF", p.GetPageWidthF) &&
              Load(m, "FPDF_GetPageHeightF", p.GetPageHeightF) &&
              Load(m, "FPDFBitmap_Create", p.BitmapCreate) &&
              Load(m, "FPDFBitmap_FillRect", p.BitmapFillRect) &&
              Load(m, "FPDFBitmap_GetBuffer", p.BitmapGetBuffer) &&
              Load(m, "FPDFBitmap_GetStride", p.BitmapGetStride) &&
              Load(m, "FPDFBitmap_Destroy", p.BitmapDestroy) &&
              Load(m, "FPDF_RenderPageBitmap", p.RenderPageBitmap);
    if (ok) {
        FPDF_LIBRARY_CONFIG cfg = {};
        cfg.version = 2;
        p.InitLibraryWithConfig(&cfg);
        p.ok = true;
    }
    return TRUE;
}

int GetBlock(void* param, unsigned long pos, unsigned char* buf, unsigned long size) {
    return static_cast<Source*>(param)->read(pos, buf, size) ? 1 : 0;
}

} // namespace

bool DecodePDF(Source& src, int target, const Settings& st, Image& out, bool transparentPage) {
    InitOnceExecuteOnce(&g_once, InitPdfium, nullptr, nullptr);
    if (!g_pdf.ok) return false;
    if (src.size() == 0 || src.size() > 0xFFFFFFFFull) return false;   // limite da API de leitura

    EnterCriticalSection(&g_lock);
    bool ok = false;
    FPDF_FILEACCESS fa = {};
    fa.m_FileLen = (unsigned long)src.size();
    fa.m_GetBlock = GetBlock;
    fa.m_Param = &src;
    FPDF_DOCUMENT doc = g_pdf.LoadCustomDocument(&fa, nullptr);
    if (doc) {
        int count = g_pdf.GetPageCount(doc);
        int idx = std::min<int>((int)st.pdfPage, std::max(1, count)) - 1;
        FPDF_PAGE page = count > 0 ? g_pdf.LoadPage(doc, idx) : nullptr;
        if (page) {
            float pw = g_pdf.GetPageWidthF(page), ph = g_pdf.GetPageHeightF(page);
            int ow, oh;
            FitSize(pw, ph, target, ow, oh);
            if (ow > 0 && oh > 0) {
                FPDF_BITMAP bmp = g_pdf.BitmapCreate(ow, oh, 1);
                if (bmp) {
                    g_pdf.BitmapFillRect(bmp, 0, 0, ow, oh, transparentPage ? 0x00FFFFFF : 0xFFFFFFFF);
                    g_pdf.RenderPageBitmap(bmp, page, 0, 0, ow, oh, 0, FPDF_ANNOT);
                    const uint8_t* buf = (const uint8_t*)g_pdf.BitmapGetBuffer(bmp);
                    int stride = g_pdf.BitmapGetStride(bmp);
                    out.alloc(ow, oh);
                    for (int y = 0; y < oh; ++y)
                        memcpy(&out.px[(size_t)y * ow * 4], buf + (size_t)y * stride, (size_t)ow * 4);
                    g_pdf.BitmapDestroy(bmp);
                    ok = true;
                }
            }
            g_pdf.ClosePage(page);
        }
        g_pdf.CloseDocument(doc);
    }
    LeaveCriticalSection(&g_lock);
    return ok;
}
