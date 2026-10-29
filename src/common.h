// DesignThumbs - gerador de miniaturas para arquivos de design (PSD, AI, PDF, SVG, EPS, INDD)
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objidl.h>
#include <cstdint>
#include <vector>
#include <string>

#define DT_VERSION_W L"1.0.0"
#define DT_REG_KEY   L"Software\\DesignThumbs"

// {7F94D085-899C-4570-B015-1FE8FF8BDC3A}
static const CLSID CLSID_DesignThumbs =
    {0x7f94d085, 0x899c, 0x4570, {0xb0, 0x15, 0x1f, 0xe8, 0xff, 0x8b, 0xdc, 0x3a}};
#define DT_CLSID_STR L"{7F94D085-899C-4570-B015-1FE8FF8BDC3A}"
#define DT_THUMB_HANDLER_IID L"{e357fccd-a995-4576-b01f-234630154e96}"

enum Background : DWORD { BG_WHITE = 0, BG_TRANSPARENT = 1, BG_CHECKER = 2 };

struct Settings {
    DWORD maxFileMB  = 2048;  // 0 = sem limite
    DWORD maxThumb   = 1024;  // resolucao maxima gerada (px)
    DWORD background = BG_WHITE;
    DWORD border     = 1;     // borda fina em paginas PDF/AI
    DWORD psdMode    = 0;     // 0 = imagem composta (qualidade), 1 = previa embutida (rapido)
    DWORD aiMode     = 0;     // 0 = renderizar artboard, 1 = miniatura embutida
    DWORD pdfPage    = 1;     // pagina usada em PDF/AI (1 = primeira)
};

Settings LoadSettings();

// Imagem BGRA com alpha nao pre-multiplicado
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    void alloc(int W, int H) { w = W; h = H; px.assign((size_t)W * H * 4, 0); }
    bool empty() const { return w <= 0 || h <= 0; }
};

// Leitura aleatoria sobre um IStream, com um pequeno cache de blocos
class Source {
public:
    explicit Source(IStream* s);
    ~Source();
    uint64_t size() const { return size_; }
    const std::wstring& name() const { return name_; }
    bool read(uint64_t off, void* buf, size_t n);
    bool readAll(std::vector<uint8_t>& out, uint64_t maxBytes);
private:
    IStream* s_;
    uint64_t size_ = 0;
    std::wstring name_;
    std::vector<uint8_t> cache_;
    uint64_t cacheOff_ = ~0ull;
    size_t cacheLen_ = 0;
    bool rawRead(uint64_t off, void* buf, size_t n);
};

// Decodificadores. "target" = maior lado desejado em pixels.
bool DecodePSD(Source& src, int target, const Settings& st, Image& out);
bool DecodePDF(Source& src, int target, const Settings& st, Image& out, bool transparentPage);
bool DecodeSVG(Source& src, int target, Image& out);
bool DecodeEPS(Source& src, int target, Image& out);
bool DecodeXMPThumb(Source& src, int target, Image& out);
bool DecodeWithWIC(const uint8_t* data, size_t n, int target, Image& out);

// Utilitarios
void FitSize(double w, double h, int target, int& ow, int& oh);
void ResizeImage(const Image& in, int ow, int oh, Image& out);
HMODULE ThisModule();
std::wstring ModuleDir();
extern LONG g_dllRefs;
