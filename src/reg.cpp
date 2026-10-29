// Registro por usuario (HKCU) - nao precisa de administrador
#include "reg.h"
#include <shlobj.h>
#include <cwctype>

const FormatGroup kFormats[] = {
    {L"PSD",  L"Photoshop (.psd, .psb, .psdt)",      {L".psd", L".psb", L".psdt", nullptr}},
    {L"AI",   L"Illustrator (.ai, .ait)",            {L".ai", L".ait", nullptr, nullptr}},
    {L"PDF",  L"PDF (.pdf)",                         {L".pdf", nullptr, nullptr, nullptr}},
    {L"SVG",  L"SVG (.svg)",                         {L".svg", nullptr, nullptr, nullptr}},
    {L"EPS",  L"EPS (.eps, .epsf, .epsi)",           {L".eps", L".epsf", L".epsi", nullptr}},
    {L"INDD", L"InDesign (.indd, .indt)",            {L".indd", L".indt", nullptr, nullptr}},
};
const int kFormatCount = sizeof(kFormats) / sizeof(kFormats[0]);

static const std::wstring kClasses = L"Software\\Classes\\";

static bool SetStr(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& val) {
    HKEY k;
    if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    LONG r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static std::wstring GetStr(HKEY root, const std::wstring& path, const wchar_t* name) {
    wchar_t buf[512]; DWORD sz = sizeof(buf), type = 0;
    if (RegGetValueW(root, path.c_str(), name, RRF_RT_REG_SZ, &type, buf, &sz) == ERROR_SUCCESS) return buf;
    return L"";
}

static bool KeyIsEmpty(HKEY root, const std::wstring& path) {
    HKEY k;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD subkeys = 0, values = 0;
    RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(k);
    return subkeys == 0 && values == 0;
}

// Remove chaves vazias subindo pela hierarquia (sem passar de "stop")
static void PruneEmpty(const std::wstring& path, size_t stopLen) {
    std::wstring p = path;
    while (p.size() > stopLen && KeyIsEmpty(HKEY_CURRENT_USER, p)) {
        if (RegDeleteKeyW(HKEY_CURRENT_USER, p.c_str()) != ERROR_SUCCESS) break;
        size_t pos = p.find_last_of(L'\\');
        if (pos == std::wstring::npos) break;
        p = p.substr(0, pos);
    }
}

bool RegisterClsid(const std::wstring& dllPath) {
    std::wstring base = kClasses + L"CLSID\\" DT_CLSID_STR;
    bool ok = SetStr(HKEY_CURRENT_USER, base, nullptr, L"DesignThumbs Thumbnail Provider");
    ok &= SetStr(HKEY_CURRENT_USER, base + L"\\InprocServer32", nullptr, dllPath);
    ok &= SetStr(HKEY_CURRENT_USER, base + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    return ok;
}

void UnregisterClsid() {
    RegDeleteTreeW(HKEY_CURRENT_USER, (kClasses + L"CLSID\\" DT_CLSID_STR).c_str());
}

// ProgIDs associados a extensao (padrao + escolha do usuario)
static std::vector<std::wstring> ProgIdsFor(const wchar_t* ext) {
    std::vector<std::wstring> ids;
    std::wstring a = GetStr(HKEY_CLASSES_ROOT, ext, nullptr);
    if (!a.empty()) ids.push_back(a);
    std::wstring uc = GetStr(HKEY_CURRENT_USER,
        std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\") + ext + L"\\UserChoice", L"ProgId");
    if (!uc.empty() && uc != a) ids.push_back(uc);
    return ids;
}

static void SetHandlerAt(const std::wstring& keyPath, bool enable) {
    std::wstring sh = keyPath + L"\\ShellEx\\" DT_THUMB_HANDLER_IID;
    if (enable) {
        SetStr(HKEY_CURRENT_USER, sh, nullptr, DT_CLSID_STR);
    } else {
        if (_wcsicmp(GetStr(HKEY_CURRENT_USER, sh, nullptr).c_str(), DT_CLSID_STR) == 0) {
            RegDeleteKeyW(HKEY_CURRENT_USER, sh.c_str());
            PruneEmpty(keyPath + L"\\ShellEx", kClasses.size());
        }
    }
}

void SetExtensionHandler(const wchar_t* ext, bool enable, bool removeOverlay) {
    SetHandlerAt(kClasses + ext, enable);
    SetHandlerAt(kClasses + L"SystemFileAssociations\\" + ext, enable);
    for (const std::wstring& id : ProgIdsFor(ext)) {
        std::wstring pk = kClasses + id;
        SetHandlerAt(pk, enable);
        // Icone do programa sobreposto na miniatura (TypeOverlay vazio = sem icone)
        HKEY k;
        if (enable && removeOverlay) {
            SetStr(HKEY_CURRENT_USER, pk, L"TypeOverlay", L"");
            SetStr(HKEY_CURRENT_USER, pk, L"DesignThumbsOverlay", L"1");
        } else if (RegOpenKeyExW(HKEY_CURRENT_USER, pk.c_str(), 0, KEY_READ | KEY_WRITE, &k) == ERROR_SUCCESS) {
            wchar_t mark[8]; DWORD sz = sizeof(mark);
            if (RegQueryValueExW(k, L"DesignThumbsOverlay", nullptr, nullptr, (BYTE*)mark, &sz) == ERROR_SUCCESS) {
                RegDeleteValueW(k, L"TypeOverlay");
                RegDeleteValueW(k, L"DesignThumbsOverlay");
            }
            RegCloseKey(k);
        }
        PruneEmpty(pk, kClasses.size());
    }
}

void ApplyFormats(const bool enabled[], bool removeOverlay) {
    for (int g = 0; g < kFormatCount; ++g)
        for (const wchar_t* ext : kFormats[g].exts)
            if (ext) SetExtensionHandler(ext, enabled[g], removeOverlay);
    NotifyShell();
}

void UnregisterAllExtensions() {
    for (int g = 0; g < kFormatCount; ++g)
        for (const wchar_t* ext : kFormats[g].exts)
            if (ext) SetExtensionHandler(ext, false, false);
    NotifyShell();
}

bool IsGroupRegistered(int g) {
    const wchar_t* ext = kFormats[g].exts[0];
    std::wstring sh = kClasses + ext + L"\\ShellEx\\" DT_THUMB_HANDLER_IID;
    return _wcsicmp(GetStr(HKEY_CURRENT_USER, sh, nullptr).c_str(), DT_CLSID_STR) == 0;
}

void NotifyShell() { SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr); }
