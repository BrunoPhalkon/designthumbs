// EPS: previa TIFF ou WMF do cabecalho DOS EPS; se nao houver, miniatura XMP (Illustrator EPS)
#include "common.h"
#include <cstring>

static inline uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static bool RenderWMF(const uint8_t* data, size_t n, int target, Image& out) {
    HENHMETAFILE emf = SetWinMetaFileBits((UINT)n, data, nullptr, nullptr);
    if (!emf) return false;
    ENHMETAHEADER hdr = {};
    GetEnhMetaFileHeader(emf, sizeof(hdr), &hdr);
    double w = hdr.rclFrame.right - hdr.rclFrame.left, h = hdr.rclFrame.bottom - hdr.rclFrame.top;
    if (w <= 0 || h <= 0) { w = hdr.rclBounds.right - hdr.rclBounds.left; h = hdr.rclBounds.bottom - hdr.rclBounds.top; }
    int ow, oh;
    FitSize(w, h, target, ow, oh);
    bool ok = false;
    if (ow > 0 && oh > 0) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = ow; bi.bmiHeader.biHeight = -oh;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP hb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (hb && bits) {
            HGDIOBJ old = SelectObject(dc, hb);
            memset(bits, 0xFF, (size_t)ow * oh * 4);
            RECT rc = {0, 0, ow, oh};
            PlayEnhMetaFile(dc, emf, &rc);
            GdiFlush();
            out.alloc(ow, oh);
            memcpy(out.px.data(), bits, out.px.size());
            for (size_t i = 3; i < out.px.size(); i += 4) out.px[i] = 255;
            SelectObject(dc, old);
            ok = true;
        }
        if (hb) DeleteObject(hb);
        DeleteDC(dc);
    }
    DeleteEnhMetaFile(emf);
    return ok;
}

bool DecodeEPS(Source& src, int target, Image& out) {
    uint8_t h[30];
    if (src.read(0, h, 30) && h[0] == 0xC5 && h[1] == 0xD0 && h[2] == 0xD3 && h[3] == 0xC6) {
        uint32_t wmfOff = le32(h + 12), wmfLen = le32(h + 16);
        uint32_t tifOff = le32(h + 20), tifLen = le32(h + 24);
        if (tifOff && tifLen && tifLen < (256u << 20)) {
            std::vector<uint8_t> t(tifLen);
            if (src.read(tifOff, t.data(), tifLen) && DecodeWithWIC(t.data(), t.size(), target, out)) return true;
        }
        if (wmfOff && wmfLen && wmfLen < (64u << 20)) {
            std::vector<uint8_t> m(wmfLen);
            if (src.read(wmfOff, m.data(), wmfLen) && RenderWMF(m.data(), m.size(), target, out)) return true;
        }
    }
    return DecodeXMPThumb(src, target, out);
}
