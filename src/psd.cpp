// Photoshop PSD / PSB
// 1) Imagem composta (dados "merged" no fim do arquivo) - so as linhas necessarias sao lidas
// 2) Miniatura embutida (recurso 1036/1033, JPEG)
// 3) Miniatura XMP
#include "common.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

static inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

namespace {

struct PsdInfo {
    int version = 1;
    int channels = 0;
    uint32_t width = 0, height = 0;
    int depth = 0, mode = 0;
    bool realMerged = true;
    uint64_t thumbOff = 0; uint32_t thumbLen = 0; bool thumbBGR = false;
    uint64_t imageDataOff = 0;
};

bool ParseHeader(Source& src, PsdInfo& pi) {
    uint8_t h[26];
    if (!src.read(0, h, 26) || memcmp(h, "8BPS", 4) != 0) return false;
    pi.version = be16(h + 4);
    if (pi.version != 1 && pi.version != 2) return false;
    pi.channels = be16(h + 12);
    pi.height = be32(h + 14);
    pi.width = be32(h + 18);
    pi.depth = be16(h + 22);
    pi.mode = be16(h + 24);
    if (!pi.width || !pi.height || pi.width > 300000 || pi.height > 300000 || pi.channels < 1 || pi.channels > 56)
        return false;

    uint64_t pos = 26;
    uint8_t b[8];
    if (!src.read(pos, b, 4)) return false;
    pos += 4 + be32(b);                                  // color mode data

    if (!src.read(pos, b, 4)) return false;
    uint32_t irLen = be32(b);
    uint64_t irStart = pos + 4;
    pos = irStart + irLen;
    if (pos > src.size()) return false;

    // Recursos de imagem (normalmente poucos MB)
    if (irLen > 0 && irLen < (96u << 20)) {
        std::vector<uint8_t> ir(irLen);
        if (src.read(irStart, ir.data(), irLen)) {
            size_t p = 0;
            while (p + 12 <= ir.size()) {
                if (memcmp(&ir[p], "8BIM", 4) && memcmp(&ir[p], "MeSa", 4) && memcmp(&ir[p], "AgHg", 4) &&
                    memcmp(&ir[p], "PHUT", 4) && memcmp(&ir[p], "DCSR", 4))
                    break;
                uint16_t id = be16(&ir[p + 4]);
                size_t nameLen = ir[p + 6];
                size_t q = p + 6 + ((nameLen + 2) & ~(size_t)1);   // nome pascal, alinhado em par
                if (q + 4 > ir.size()) break;
                uint32_t sz = be32(&ir[q]);
                size_t data = q + 4;
                if (data + sz > ir.size()) break;
                if ((id == 1036 || id == 1033) && sz > 28) {
                    uint32_t fmt = be32(&ir[data]);
                    if (fmt == 1 && (pi.thumbLen == 0 || id == 1036)) {   // 1 = JPEG
                        pi.thumbOff = irStart + data + 28;
                        pi.thumbLen = sz - 28;
                        pi.thumbBGR = (id == 1033);
                    }
                } else if (id == 1057 && sz >= 5) {
                    pi.realMerged = ir[data + 4] != 0;
                }
                p = data + ((sz + 1) & ~(size_t)1);
            }
        }
    }

    // Camadas e mascaras
    if (pi.version == 1) {
        if (!src.read(pos, b, 4)) return false;
        pos += 4 + (uint64_t)be32(b);
    } else {
        if (!src.read(pos, b, 8)) return false;
        pos += 8 + be64(b);
    }
    if (pos + 2 > src.size()) return false;
    pi.imageDataOff = pos;
    return true;
}

// Descompacta uma linha PackBits
bool UnpackBits(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen) {
    size_t i = 0, o = 0;
    while (o < outLen && i < inLen) {
        int n = (int8_t)in[i++];
        if (n >= 0) {
            size_t cnt = (size_t)n + 1;
            if (i + cnt > inLen) cnt = inLen - i;
            if (o + cnt > outLen) cnt = outLen - o;
            memcpy(out + o, in + i, cnt);
            i += cnt; o += cnt;
        } else if (n != -128) {
            size_t cnt = (size_t)(1 - n);
            if (i >= inLen) break;
            uint8_t v = in[i++];
            if (o + cnt > outLen) cnt = outLen - o;
            memset(out + o, v, cnt);
            o += cnt;
        }
    }
    if (o < outLen) memset(out + o, 0, outLen - o);
    return true;
}

bool DecodeComposite(Source& src, const PsdInfo& pi, int target, Image& out) {
    int baseCh;
    if (pi.mode == 3) baseCh = 3;         // RGB
    else if (pi.mode == 1) baseCh = 1;    // Tons de cinza
    else if (pi.mode == 4) baseCh = 4;    // CMYK
    else return false;
    if (pi.channels < baseCh) return false;
    if (pi.depth != 8 && pi.depth != 16) return false;

    uint8_t cb[2];
    if (!src.read(pi.imageDataOff, cb, 2)) return false;
    int compression = be16(cb);
    if (compression != 0 && compression != 1) return false;

    const uint32_t W = pi.width, H = pi.height;
    const int bps = pi.depth / 8;
    const size_t rowBytes = (size_t)W * bps;
    uint64_t pos = pi.imageDataOff + 2;

    // Deslocamento de cada linha de cada canal
    std::vector<uint64_t> rowOff((size_t)baseCh * H);
    std::vector<uint32_t> rowLen((size_t)baseCh * H);
    if (compression == 1) {
        const int cntSize = pi.version == 1 ? 2 : 4;
        size_t entries = (size_t)baseCh * H;
        std::vector<uint8_t> tbl(entries * cntSize);
        if (!src.read(pos, tbl.data(), tbl.size())) return false;
        uint64_t dataStart = pos + (uint64_t)pi.channels * H * cntSize;
        uint64_t acc = dataStart;
        for (size_t i = 0; i < entries; ++i) {
            uint32_t n = cntSize == 2 ? be16(&tbl[i * 2]) : be32(&tbl[i * 4]);
            rowOff[i] = acc; rowLen[i] = n; acc += n;
        }
        if (acc > src.size()) return false;
    } else {
        for (size_t i = 0; i < (size_t)baseCh * H; ++i) { rowOff[i] = pos + i * rowBytes; rowLen[i] = (uint32_t)rowBytes; }
        if (pos + (uint64_t)baseCh * H * rowBytes > src.size()) return false;
    }

    int ow, oh;
    FitSize(W, H, std::min<int>(target, (int)std::max(W, H)), ow, oh);
    const int ky = std::min(4, std::max(1, (int)std::ceil((double)H / oh)));
    const int kx = std::min(4, std::max(1, (int)std::ceil((double)W / ow)));

    // Colunas de amostragem pre-calculadas
    std::vector<uint32_t> sxs((size_t)ow * kx);
    for (int x = 0; x < ow; ++x)
        for (int i = 0; i < kx; ++i) {
            double fx = (x + (i + 0.5) / kx) / ow * W;
            sxs[(size_t)x * kx + i] = std::min<uint32_t>(W - 1, (uint32_t)fx);
        }

    out.alloc(ow, oh);
    std::vector<std::vector<uint8_t>> rows(baseCh, std::vector<uint8_t>(rowBytes));
    std::vector<uint8_t> comp;
    std::vector<uint32_t> acc((size_t)ow * 3);
    uint32_t lastSy = 0xFFFFFFFF;

    for (int y = 0; y < oh; ++y) {
        std::fill(acc.begin(), acc.end(), 0);
        for (int j = 0; j < ky; ++j) {
            double fy = (y + (j + 0.5) / ky) / oh * H;
            uint32_t sy = std::min<uint32_t>(H - 1, (uint32_t)fy);
            if (sy != lastSy) {
                for (int c = 0; c < baseCh; ++c) {
                    size_t idx = (size_t)c * H + sy;
                    if (compression == 1) {
                        comp.resize(rowLen[idx]);
                        if (rowLen[idx] && !src.read(rowOff[idx], comp.data(), rowLen[idx])) return false;
                        UnpackBits(comp.data(), comp.size(), rows[c].data(), rowBytes);
                    } else {
                        if (!src.read(rowOff[idx], rows[c].data(), rowBytes)) return false;
                    }
                }
                lastSy = sy;
            }
            for (int x = 0; x < ow; ++x) {
                uint32_t r = 0, g = 0, b = 0;
                for (int i = 0; i < kx; ++i) {
                    size_t sx = (size_t)sxs[(size_t)x * kx + i] * bps;   // byte alto em 16 bits (big endian)
                    if (baseCh == 3) {
                        r += rows[0][sx]; g += rows[1][sx]; b += rows[2][sx];
                    } else if (baseCh == 1) {
                        uint8_t v = rows[0][sx]; r += v; g += v; b += v;
                    } else {
                        // CMYK no PSD fica invertido (255 = sem tinta)
                        uint32_t C = rows[0][sx], M = rows[1][sx], Y = rows[2][sx], K = rows[3][sx];
                        r += C * K / 255; g += M * K / 255; b += Y * K / 255;
                    }
                }
                acc[x * 3 + 0] += r; acc[x * 3 + 1] += g; acc[x * 3 + 2] += b;
            }
        }
        const uint32_t div = (uint32_t)(kx * ky);
        uint8_t* o = &out.px[(size_t)y * ow * 4];
        for (int x = 0; x < ow; ++x) {
            o[x * 4 + 2] = (uint8_t)((acc[x * 3 + 0] + div / 2) / div);
            o[x * 4 + 1] = (uint8_t)((acc[x * 3 + 1] + div / 2) / div);
            o[x * 4 + 0] = (uint8_t)((acc[x * 3 + 2] + div / 2) / div);
            o[x * 4 + 3] = 255;
        }
    }
    return true;
}

bool DecodeEmbedded(Source& src, const PsdInfo& pi, int target, Image& out) {
    if (!pi.thumbLen || pi.thumbLen > (64u << 20)) return false;
    std::vector<uint8_t> jpg(pi.thumbLen);
    if (!src.read(pi.thumbOff, jpg.data(), jpg.size())) return false;
    if (!DecodeWithWIC(jpg.data(), jpg.size(), target, out)) return false;
    if (pi.thumbBGR)
        for (size_t i = 0; i + 3 < out.px.size(); i += 4) std::swap(out.px[i], out.px[i + 2]);
    return true;
}

} // namespace

bool DecodePSD(Source& src, int target, const Settings& st, Image& out) {
    PsdInfo pi;
    if (!ParseHeader(src, pi)) return false;
    if (st.psdMode == 0 && pi.realMerged && DecodeComposite(src, pi, target, out)) return true;
    if (DecodeEmbedded(src, pi, target, out)) return true;
    if (st.psdMode != 0 && DecodeComposite(src, pi, target, out)) return true;
    return DecodeXMPThumb(src, target, out);
}
