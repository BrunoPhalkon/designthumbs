#include "common.h"

// ---------------------------------------------------------------- Settings
static DWORD RegDword(HKEY k, const wchar_t* name, DWORD def) {
    DWORD v = 0, sz = sizeof(v), type = 0;
    if (RegQueryValueExW(k, name, nullptr, &type, (BYTE*)&v, &sz) == ERROR_SUCCESS && type == REG_DWORD)
        return v;
    return def;
}

Settings LoadSettings() {
    Settings s;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, DT_REG_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        s.maxFileMB  = RegDword(k, L"MaxFileMB", s.maxFileMB);
        s.maxThumb   = RegDword(k, L"MaxThumb", s.maxThumb);
        s.background = RegDword(k, L"Background", s.background);
        s.border     = RegDword(k, L"Border", s.border);
        s.psdMode    = RegDword(k, L"PsdMode", s.psdMode);
        s.aiMode     = RegDword(k, L"AiMode", s.aiMode);
        s.pdfPage    = RegDword(k, L"PdfPage", s.pdfPage);
        RegCloseKey(k);
    }
    if (s.maxThumb < 64) s.maxThumb = 64;
    if (s.maxThumb > 4096) s.maxThumb = 4096;
    if (s.background > 2) s.background = BG_WHITE;
    if (s.pdfPage < 1) s.pdfPage = 1;
    return s;
}

