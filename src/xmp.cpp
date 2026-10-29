// Miniatura JPEG embutida nos metadados XMP (xmpGImg:image), usada por Illustrator, InDesign,
// Photoshop e EPS do Illustrator.
#include "common.h"
#include <algorithm>
#include <cstring>

namespace {

int B64(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Decodifica base64 ignorando espacos e entidades XML (&#xA; etc.)
std::vector<uint8_t> DecodeBase64(const uint8_t* s, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n * 3 / 4);
    uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = s[i];
        if (c == '&') {                     // pula entidade
            while (i < n && s[i] != ';') ++i;
            continue;
        }
        if (c == '=') break;
        int v = B64(c);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)(acc >> bits)); }
    }
    return out;
}

const uint8_t* Find(const uint8_t* b, const uint8_t* e, const char* pat) {
    size_t m = strlen(pat);
    if ((size_t)(e - b) < m) return nullptr;
    const uint8_t* r = std::search(b, e, (const uint8_t*)pat, (const uint8_t*)pat + m);
    return r == e ? nullptr : r;
}

// Procura o maior thumbnail XMP dentro de um bloco
bool FindInBuffer(const std::vector<uint8_t>& buf, std::vector<uint8_t>& best) {
    const uint8_t* b = buf.data();
    const uint8_t* e = b + buf.size();
    const uint8_t* p = b;
    bool found = false;
    while ((p = Find(p, e, "xmpGImg:image")) != nullptr) {
        p += 13;
        const uint8_t *start = nullptr, *end = nullptr;
        if (p < e && *p == '>') {                              // <xmpGImg:image>....</xmpGImg:image>
            start = p + 1;
            end = Find(start, e, "</xmpGImg:image>");
        } else if (p + 1 < e && *p == '=' && (p[1] == '"' || p[1] == '\'')) {   // atributo
            char q = (char)p[1];
            start = p + 2;
            end = std::find(start, e, (uint8_t)q);
            if (end == e) end = nullptr;
        }
        if (start && end && end > start && (size_t)(end - start) < (48u << 20)) {
            std::vector<uint8_t> jpg = DecodeBase64(start, end - start);
            if (jpg.size() > 100 && jpg[0] == 0xFF && jpg[1] == 0xD8 && jpg.size() > best.size()) {
                best.swap(jpg);
                found = true;
            }
            p = end;
        }
    }
    return found;
}

} // namespace

bool DecodeXMPThumb(Source& src, int target, Image& out) {
    const uint64_t size = src.size();
    const uint64_t window = 32ull << 20;
    std::vector<uint8_t> best, buf;
    // inicio do arquivo
    uint64_t n = std::min(size, window);
    buf.resize((size_t)n);
    if (n && src.read(0, buf.data(), buf.size())) FindInBuffer(buf, best);
    // fim do arquivo (InDesign e alguns AI guardam o XMP no final)
    if (best.empty() && size > window) {
        uint64_t off = std::max<uint64_t>(window, size - window);
        buf.resize((size_t)(size - off));
        if (src.read(off, buf.data(), buf.size())) FindInBuffer(buf, best);
    }
    if (best.empty()) return false;
    return DecodeWithWIC(best.data(), best.size(), target, out);
}
