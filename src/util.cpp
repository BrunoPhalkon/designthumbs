#include "common.h"
#include <wincodec.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cstring>

// ---------------------------------------------------------------- Source
static const size_t kCacheBlock = 256 * 1024;

Source::Source(IStream* s) : s_(s) {
    if (s_) s_->AddRef();
    STATSTG st = {};
    if (s_ && SUCCEEDED(s_->Stat(&st, STATFLAG_DEFAULT))) {
        size_ = st.cbSize.QuadPart;
        if (st.pwcsName) { name_ = st.pwcsName; CoTaskMemFree(st.pwcsName); }
    }
}
Source::~Source() { if (s_) s_->Release(); }

bool Source::rawRead(uint64_t off, void* buf, size_t n) {
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
    if (FAILED(s_->Seek(li, STREAM_SEEK_SET, nullptr))) return false;
    uint8_t* p = (uint8_t*)buf;
    while (n > 0) {
        ULONG chunk = (ULONG)std::min<size_t>(n, 64u << 20);
        ULONG got = 0;
        HRESULT hr = s_->Read(p, chunk, &got);
        if (FAILED(hr) || got == 0) return false;
        p += got; n -= got;
    }
    return true;
}

bool Source::read(uint64_t off, void* buf, size_t n) {
    if (!s_ || n == 0) return n == 0;
    if (off > size_ || n > size_ - off) return false;
    if (n >= kCacheBlock / 2) return rawRead(off, buf, n);
    uint8_t* dst = (uint8_t*)buf;
    while (n > 0) {
        if (cacheOff_ == ~0ull || off < cacheOff_ || off >= cacheOff_ + cacheLen_) {
            uint64_t start = off & ~(uint64_t)(kCacheBlock - 1);
            size_t len = (size_t)std::min<uint64_t>(kCacheBlock, size_ - start);
            cache_.resize(kCacheBlock);
            if (!rawRead(start, cache_.data(), len)) { cacheOff_ = ~0ull; return false; }
            cacheOff_ = start; cacheLen_ = len;
        }
        size_t inBlock = (size_t)(off - cacheOff_);
        size_t take = std::min(n, cacheLen_ - inBlock);
        memcpy(dst, cache_.data() + inBlock, take);
        dst += take; off += take; n -= take;
    }
    return true;
}

bool Source::readAll(std::vector<uint8_t>& out, uint64_t maxBytes) {
    if (size_ > maxBytes) return false;
    out.resize((size_t)size_);
    return rawRead(0, out.data(), out.size());
}

// ---------------------------------------------------------------- Imagem
void FitSize(double w, double h, int target, int& ow, int& oh) {
    if (w <= 0 || h <= 0) { ow = oh = 0; return; }
    double s = std::min(target / w, target / h);
    ow = std::max(1, (int)std::lround(w * s));
    oh = std::max(1, (int)std::lround(h * s));
    ow = std::min(ow, target); oh = std::min(oh, target);
}

// Reducao por media de area (box filter), com alpha pre-multiplicado para nao "sujar" as bordas
void ResizeImage(const Image& in, int ow, int oh, Image& out) {
    out.alloc(ow, oh);
    if (in.empty()) return;
    const double sx = (double)in.w / ow, sy = (double)in.h / oh;
    for (int y = 0; y < oh; ++y) {
        double y0 = y * sy, y1 = (y + 1) * sy;
        int iy0 = (int)y0, iy1 = std::min(in.h, (int)std::ceil(y1));
        for (int x = 0; x < ow; ++x) {
            double x0 = x * sx, x1 = (x + 1) * sx;
            int ix0 = (int)x0, ix1 = std::min(in.w, (int)std::ceil(x1));
            double b = 0, g = 0, r = 0, a = 0, wsum = 0;
            for (int yy = iy0; yy < std::max(iy0 + 1, iy1); ++yy) {
                double wy = std::min<double>(yy + 1, y1) - std::max<double>(yy, y0);
                if (wy <= 0) wy = 1e-6;
                const uint8_t* row = &in.px[((size_t)yy * in.w) * 4];
                for (int xx = ix0; xx < std::max(ix0 + 1, ix1); ++xx) {
                    double wx = std::min<double>(xx + 1, x1) - std::max<double>(xx, x0);
                    if (wx <= 0) wx = 1e-6;
                    double wgt = wx * wy;
                    const uint8_t* p = row + (size_t)xx * 4;
                    double al = p[3] / 255.0;
                    b += p[0] * al * wgt; g += p[1] * al * wgt; r += p[2] * al * wgt;
                    a += p[3] * wgt; wsum += wgt;
                }
            }
            uint8_t* o = &out.px[((size_t)y * ow + x) * 4];
            if (wsum <= 0 || a <= 0) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            double A = a / wsum;            // 0..255
            double inv = 1.0 / (A / 255.0 * wsum);
            o[0] = (uint8_t)std::min(255.0, b * inv + 0.5);
            o[1] = (uint8_t)std::min(255.0, g * inv + 0.5);
            o[2] = (uint8_t)std::min(255.0, r * inv + 0.5);
            o[3] = (uint8_t)std::min(255.0, A + 0.5);
        }
    }
}

// ---------------------------------------------------------------- WIC (JPEG, TIFF, PNG...)
bool DecodeWithWIC(const uint8_t* data, size_t n, int target, Image& out) {
    if (!data || n == 0) return false;
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWICImagingFactory, (void**)&f)))
        return false;
    bool ok = false;
    IStream* ms = SHCreateMemStream(data, (UINT)n);
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    do {
        if (!ms) break;
        if (FAILED(f->CreateDecoderFromStream(ms, nullptr, WICDecodeMetadataCacheOnDemand, &dec))) break;
        if (FAILED(dec->GetFrame(0, &frame))) break;
        UINT w = 0, h = 0;
        if (FAILED(frame->GetSize(&w, &h)) || !w || !h) break;
        IWICBitmapSource* srcBmp = frame;
        int ow = (int)w, oh = (int)h;
        if ((int)w > target || (int)h > target) {
            FitSize(w, h, target, ow, oh);
            if (FAILED(f->CreateBitmapScaler(&scaler))) break;
            if (FAILED(scaler->Initialize(frame, ow, oh, WICBitmapInterpolationModeFant))) break;
            srcBmp = scaler;
        }
        if (FAILED(f->CreateFormatConverter(&conv))) break;
        if (FAILED(conv->Initialize(srcBmp, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                    nullptr, 0, WICBitmapPaletteTypeCustom))) break;
        out.alloc(ow, oh);
        if (FAILED(conv->CopyPixels(nullptr, ow * 4, (UINT)out.px.size(), out.px.data()))) break;
        ok = true;
    } while (false);
    if (conv) conv->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (ms) ms->Release();
    f->Release();
    return ok;
}

// ---------------------------------------------------------------- Modulo
HMODULE ThisModule() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&ThisModule, &m);
    return m;
}

std::wstring ModuleDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(ThisModule(), buf, MAX_PATH * 2);
    std::wstring p(buf, n);
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : p.substr(0, pos);
}
