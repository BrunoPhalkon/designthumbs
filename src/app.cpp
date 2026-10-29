// DesignThumbs - app de instalacao e configuracao (com icone opcional na bandeja)
#include "common.h"
#include "reg.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <string>
#include <vector>

namespace {

const wchar_t* kWndClass = L"DesignThumbsMainWnd";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kUninstKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DesignThumbs";
const UINT WM_TRAY = WM_APP + 1;
const UINT WM_SHOWME = WM_APP + 2;

enum {
    ID_FMT = 100,           // 100..105 checkboxes de formato
    ID_MAXMB = 200, ID_MAXTHUMB, ID_BG, ID_BORDER, ID_OVERLAY, ID_PSDMODE, ID_AIMODE, ID_PDFPAGE,
    ID_AUTOSTART, ID_CLEARCACHE, ID_UNINSTALL, ID_APPLY, ID_CLOSE, ID_STATUS,
    IDM_OPEN = 300, IDM_CLEAR, IDM_EXIT
};

HINSTANCE g_inst;
HWND g_wnd;
HFONT g_font, g_fontBold, g_fontTitle;
double g_scale = 1.0;
bool g_trayMode = false;
bool g_installedHere = false;
UINT g_taskbarCreated = 0;
NOTIFYICONDATAW g_nid = {};

const DWORD kThumbSizes[] = {256, 512, 768, 1024, 1536, 2048};

int S(int v) { return (int)(v * g_scale + 0.5); }

std::wstring InstallDir() {
    wchar_t* p = nullptr;
    std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) d = std::wstring(p) + L"\\Programs\\DesignThumbs";
    CoTaskMemFree(p);
    return d;
}

std::wstring ExePath() {
    wchar_t b[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH * 2);
    return std::wstring(b, n);
}

std::wstring DirOf(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

bool SameDir(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

// --------------------------------------------------------------- registro de configuracoes
DWORD GetDw(const wchar_t* name, DWORD def) {
    DWORD v = def, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, DT_REG_KEY, name, RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS) return def;
    return v;
}
void SetDw(const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, DT_REG_KEY, name, REG_DWORD, &v, sizeof(v));
}
bool AutostartEnabled() {
    wchar_t b[1024]; DWORD sz = sizeof(b);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"DesignThumbs", RRF_RT_REG_SZ, nullptr, b, &sz) == ERROR_SUCCESS;
}
void SetAutostart(bool on) {
    if (on) {
        std::wstring cmd = L"\"" + InstallDir() + L"\\DesignThumbs.exe\" /tray";
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"DesignThumbs", REG_SZ, cmd.c_str(), (DWORD)((cmd.size() + 1) * 2));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"DesignThumbs");
    }
}

// --------------------------------------------------------------- instalacao
bool CopyReplacing(const std::wstring& from, const std::wstring& to) {
    if (CopyFileW(from.c_str(), to.c_str(), FALSE)) return true;
    // arquivo em uso (DLL carregada pelo Explorador): renomeia o antigo e copia o novo
    for (int i = 0; i < 50; ++i) {
        std::wstring old = to + L".old" + std::to_wstring(i);
        if (MoveFileExW(to.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) break;
    }
    return CopyFileW(from.c_str(), to.c_str(), FALSE) != 0;
}

void CleanupOldFiles(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.old*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { DeleteFileW((dir + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void CreateShortcut(const std::wstring& target) {
    wchar_t* p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &p))) return;
    std::wstring lnk = std::wstring(p) + L"\\DesignThumbs.lnk";
    CoTaskMemFree(p);
    IShellLinkW* sl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl))) {
        sl->SetPath(target.c_str());
        sl->SetDescription(L"Configurações do DesignThumbs");
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) { pf->Save(lnk.c_str(), TRUE); pf->Release(); }
        sl->Release();
    }
}

void RemoveShortcut() {
    wchar_t* p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &p))) {
        DeleteFileW((std::wstring(p) + L"\\DesignThumbs.lnk").c_str());
        CoTaskMemFree(p);
    }
}

void WriteUninstallEntry(const std::wstring& dir) {
    std::wstring exe = dir + L"\\DesignThumbs.exe";
    std::wstring un = L"\"" + exe + L"\" /uninstall";
    auto set = [&](const wchar_t* n, const std::wstring& v) {
        RegSetKeyValueW(HKEY_CURRENT_USER, kUninstKey, n, REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * 2));
    };
    set(L"DisplayName", L"DesignThumbs");
    set(L"DisplayVersion", DT_VERSION_W);
    set(L"Publisher", L"DesignThumbs");
    set(L"DisplayIcon", exe);
    set(L"InstallLocation", dir);
    set(L"UninstallString", un);
    DWORD one = 1, size = 11000;
    RegSetKeyValueW(HKEY_CURRENT_USER, kUninstKey, L"NoModify", REG_DWORD, &one, 4);
    RegSetKeyValueW(HKEY_CURRENT_USER, kUninstKey, L"NoRepair", REG_DWORD, &one, 4);
    RegSetKeyValueW(HKEY_CURRENT_USER, kUninstKey, L"EstimatedSize", REG_DWORD, &size, 4);
}

bool DoInstall(std::wstring& err) {
    std::wstring src = DirOf(ExePath()), dst = InstallDir();
    SHCreateDirectoryExW(nullptr, dst.c_str(), nullptr);
    const wchar_t* files[] = {L"DesignThumbs.exe", L"DesignThumbs.dll", L"pdfium.dll"};
    for (const wchar_t* f : files) {
        if (!SameDir(src, dst) && !CopyReplacing(src + L"\\" + f, dst + L"\\" + f)) {
            err = std::wstring(L"Não foi possível copiar ") + f + L" para " + dst;
            return false;
        }
    }
    if (!RegisterClsid(dst + L"\\DesignThumbs.dll")) { err = L"Falha ao registrar o componente."; return false; }
    CreateShortcut(dst + L"\\DesignThumbs.exe");
    WriteUninstallEntry(dst);
    return true;
}

// --------------------------------------------------------------- cache de miniaturas
void ClearThumbnailCache() {
    // encerra o Explorador da sessao atual, apaga thumbcache_*.db e reinicia
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = {sizeof(pe)};
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
                DWORD s = 0;
                ProcessIdToSessionId(pe.th32ProcessID, &s);
                if (s != session) continue;
                HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
                if (h) { TerminateProcess(h, 0); WaitForSingleObject(h, 5000); CloseHandle(h); }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    Sleep(800);

    wchar_t* p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
        std::wstring dir = std::wstring(p) + L"\\Microsoft\\Windows\\Explorer";
        CoTaskMemFree(p);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\thumbcache_*.db").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do { DeleteFileW((dir + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    std::wstring explorer = std::wstring(win) + L"\\explorer.exe";
    ShellExecuteW(nullptr, L"open", explorer.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// --------------------------------------------------------------- desinstalacao
void DoUninstall(bool quiet) {
    UnregisterAllExtensions();
    UnregisterClsid();
    SetAutostart(false);
    RemoveShortcut();
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, DT_REG_KEY);
    NotifyShell();
    // apaga a pasta depois que este processo fechar
    std::wstring dir = InstallDir();
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\cmd.exe\" /c timeout /t 3 /nobreak >nul & rmdir /s /q \"" + dir + L"\"";
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, sys, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    if (!quiet)
        MessageBoxW(nullptr, L"O DesignThumbs foi removido.\n\nAs miniaturas antigas podem continuar aparecendo até você limpar o cache de miniaturas do Windows (Limpeza de Disco > Miniaturas).",
                    L"DesignThumbs", MB_ICONINFORMATION);
}

// --------------------------------------------------------------- interface
HWND Ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT font = nullptr) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                             (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(font ? font : g_font), TRUE);
    return c;
}
HWND Label(const wchar_t* t, int x, int y, int w, int h = 18, HFONT f = nullptr) { return Ctl(L"STATIC", t, SS_LEFT, x, y, w, h, -1, f); }
HWND Check(const wchar_t* t, int x, int y, int w, int id) { return Ctl(L"BUTTON", t, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 20, id); }
HWND Combo(int x, int y, int w, int id, std::initializer_list<const wchar_t*> items) {
    HWND c = Ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, x, y, w, 200, id);
    for (auto s : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s);
    return c;
}
HWND Edit(int x, int y, int w, int id) {
    HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_RIGHT,
                             S(x), S(y), S(w), S(22), g_wnd, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}
void Section(const wchar_t* t, int y) { Label(t, 20, y, 440, 20, g_fontBold); }

void SetStatus() {
    std::wstring s;
    if (g_installedHere) s = L"Instalado em " + InstallDir();
    else s = L"Ainda não instalado. Clique em \"Instalar\" para copiar para " + InstallDir();
    SetDlgItemTextW(g_wnd, ID_STATUS, s.c_str());
}

void LoadIntoUI() {
    for (int g = 0; g < kFormatCount; ++g) {
        bool on = g_installedHere ? IsGroupRegistered(g) : true;
        CheckDlgButton(g_wnd, ID_FMT + g, on ? BST_CHECKED : BST_UNCHECKED);
    }
    Settings st = LoadSettings();
    SetDlgItemInt(g_wnd, ID_MAXMB, st.maxFileMB, FALSE);
    int sel = 3;
    for (int i = 0; i < 6; ++i) if (kThumbSizes[i] == st.maxThumb) sel = i;
    SendDlgItemMessageW(g_wnd, ID_MAXTHUMB, CB_SETCURSEL, sel, 0);
    SendDlgItemMessageW(g_wnd, ID_BG, CB_SETCURSEL, st.background, 0);
    CheckDlgButton(g_wnd, ID_BORDER, st.border ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_wnd, ID_OVERLAY, GetDw(L"RemoveOverlay", 0) ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(g_wnd, ID_PSDMODE, CB_SETCURSEL, st.psdMode ? 1 : 0, 0);
    SendDlgItemMessageW(g_wnd, ID_AIMODE, CB_SETCURSEL, st.aiMode ? 1 : 0, 0);
    SetDlgItemInt(g_wnd, ID_PDFPAGE, st.pdfPage, FALSE);
    CheckDlgButton(g_wnd, ID_AUTOSTART, AutostartEnabled() ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemTextW(g_wnd, ID_APPLY, g_installedHere ? L"Aplicar" : L"Instalar");
    EnableWindow(GetDlgItem(g_wnd, ID_UNINSTALL), g_installedHere);
    EnableWindow(GetDlgItem(g_wnd, ID_CLEARCACHE), g_installedHere);
    SetStatus();
}

void AddTrayIcon() {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_wnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy(g_nid.szTip, L"DesignThumbs - miniaturas de arquivos de design");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
void RemoveTrayIcon() { if (g_nid.hWnd) Shell_NotifyIconW(NIM_DELETE, &g_nid); g_nid.hWnd = nullptr; }

bool Apply() {
    BOOL ok;
    UINT mb = GetDlgItemInt(g_wnd, ID_MAXMB, &ok, FALSE);
    UINT page = GetDlgItemInt(g_wnd, ID_PDFPAGE, &ok, FALSE);
    if (page < 1) page = 1;
    int ts = (int)SendDlgItemMessageW(g_wnd, ID_MAXTHUMB, CB_GETCURSEL, 0, 0);
    if (ts < 0 || ts > 5) ts = 3;

    if (!g_installedHere) {
        std::wstring err;
        if (!DoInstall(err)) { MessageBoxW(g_wnd, err.c_str(), L"DesignThumbs", MB_ICONERROR); return false; }
    } else {
        RegisterClsid(InstallDir() + L"\\DesignThumbs.dll");
    }
    SetDw(L"MaxFileMB", mb);
    SetDw(L"MaxThumb", kThumbSizes[ts]);
    SetDw(L"Background", (DWORD)std::max<LRESULT>(0, SendDlgItemMessageW(g_wnd, ID_BG, CB_GETCURSEL, 0, 0)));
    SetDw(L"Border", IsDlgButtonChecked(g_wnd, ID_BORDER) == BST_CHECKED);
    SetDw(L"PsdMode", (DWORD)std::max<LRESULT>(0, SendDlgItemMessageW(g_wnd, ID_PSDMODE, CB_GETCURSEL, 0, 0)));
    SetDw(L"AiMode", (DWORD)std::max<LRESULT>(0, SendDlgItemMessageW(g_wnd, ID_AIMODE, CB_GETCURSEL, 0, 0)));
    SetDw(L"PdfPage", page);
    bool overlay = IsDlgButtonChecked(g_wnd, ID_OVERLAY) == BST_CHECKED;
    SetDw(L"RemoveOverlay", overlay);
    bool en[16];
    for (int g = 0; g < kFormatCount; ++g) {
        en[g] = IsDlgButtonChecked(g_wnd, ID_FMT + g) == BST_CHECKED;
        SetDw((std::wstring(L"Fmt_") + kFormats[g].key).c_str(), en[g]);
    }
    ApplyFormats(en, overlay);
    bool autostart = IsDlgButtonChecked(g_wnd, ID_AUTOSTART) == BST_CHECKED;
    SetAutostart(autostart);

    bool wasInstalled = g_installedHere;
    g_installedHere = true;
    LoadIntoUI();

    std::wstring msg = wasInstalled ? L"Configurações aplicadas." : L"DesignThumbs instalado.";
    msg += L"\n\nArquivos que já tinham miniatura (ou ícone) guardados só mudam depois de limpar o cache de miniaturas. "
           L"Limpar agora? O Explorador de Arquivos será reiniciado e as janelas de pasta abertas vão fechar.";
    if (MessageBoxW(g_wnd, msg.c_str(), L"DesignThumbs", MB_ICONQUESTION | MB_YESNO) == IDYES) ClearThumbnailCache();

    if (!wasInstalled && !SameDir(DirOf(ExePath()), InstallDir())) {
        // passa a rodar a copia instalada
        std::wstring exe = InstallDir() + L"\\DesignThumbs.exe";
        ShellExecuteW(nullptr, L"open", exe.c_str(), autostart ? L"/afterinstall /tray" : L"/afterinstall", nullptr, SW_SHOWNORMAL);
        DestroyWindow(g_wnd);
    } else if (autostart && !g_trayMode) {
        g_trayMode = true; AddTrayIcon();
    } else if (!autostart && g_trayMode) {
        g_trayMode = false; RemoveTrayIcon();
    }
    return true;
}

void BuildUI() {
    int y = 16;
    Label(L"DesignThumbs", 20, y, 300, 30, g_fontTitle);
    Label(L"versão " DT_VERSION_W, 380, y + 10, 100, 18);
    y += 34;
    Ctl(L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS, 20, y, 460, 18, ID_STATUS);
    y += 30;

    Section(L"Formatos com miniatura", y); y += 24;
    for (int g = 0; g < kFormatCount; ++g)
        Check(kFormats[g].label, 32 + (g % 2) * 230, y + (g / 2) * 24, 225, ID_FMT + g);
    y += ((kFormatCount + 1) / 2) * 24 + 12;

    Section(L"Desempenho", y); y += 26;
    Label(L"Ignorar arquivos maiores que (MB, 0 = sem limite)", 32, y + 3, 320);
    Edit(370, y, 110, ID_MAXMB); y += 30;
    Label(L"Resolução máxima da miniatura", 32, y + 3, 320);
    Combo(370, y, 110, ID_MAXTHUMB, {L"256 px", L"512 px", L"768 px", L"1024 px", L"1536 px", L"2048 px"}); y += 38;

    Section(L"Aparência", y); y += 26;
    Label(L"Fundo das áreas transparentes", 32, y + 3, 260);
    Combo(300, y, 180, ID_BG, {L"Branco", L"Transparente", L"Xadrez"}); y += 30;
    Check(L"Borda fina em páginas de PDF e Illustrator", 32, y, 440, ID_BORDER); y += 24;
    Check(L"Esconder o ícone do programa sobre a miniatura", 32, y, 440, ID_OVERLAY); y += 32;

    Section(L"Por formato", y); y += 26;
    Label(L"Photoshop", 32, y + 3, 150);
    Combo(200, y, 280, ID_PSDMODE, {L"Imagem composta (melhor qualidade)", L"Prévia embutida (mais rápido)"}); y += 30;
    Label(L"Illustrator", 32, y + 3, 150);
    Combo(200, y, 280, ID_AIMODE, {L"Renderizar a prancheta (nítido)", L"Miniatura embutida (mais rápido)"}); y += 30;
    Label(L"Página usada em PDF e AI", 32, y + 3, 260);
    Edit(370, y, 110, ID_PDFPAGE); y += 38;

    Section(L"Sistema", y); y += 26;
    Check(L"Iniciar com o Windows (ícone na bandeja)", 32, y, 440, ID_AUTOSTART); y += 26;
    Label(L"Opcional: as miniaturas funcionam mesmo sem isso ligado.", 32, y, 448, 18);
    y += 28;
    Ctl(L"BUTTON", L"Limpar cache de miniaturas", BS_PUSHBUTTON | WS_TABSTOP, 32, y, 210, 28, ID_CLEARCACHE);
    Ctl(L"BUTTON", L"Desinstalar", BS_PUSHBUTTON | WS_TABSTOP, 252, y, 120, 28, ID_UNINSTALL);
    y += 48;

    Ctl(L"BUTTON", L"Aplicar", BS_DEFPUSHBUTTON | WS_TABSTOP, 270, y, 100, 30, ID_APPLY);
    Ctl(L"BUTTON", L"Fechar", BS_PUSHBUTTON | WS_TABSTOP, 380, y, 100, 30, ID_CLOSE);
    y += 46;

    RECT rc = {0, 0, S(500), S(y)};
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    SetWindowPos(g_wnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
}

void ShowMain() {
    LoadIntoUI();
    ShowWindow(g_wnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_wnd);
}

void TrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_OPEN, L"Configurações...");
    AppendMenuW(m, MF_STRING, IDM_CLEAR, L"Limpar cache de miniaturas");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Sair da bandeja");
    SetMenuDefaultItem(m, IDM_OPEN, FALSE);
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, nullptr);
    DestroyMenu(m);
}

bool ConfirmClearCache(HWND owner) {
    return MessageBoxW(owner, L"O Explorador de Arquivos será reiniciado e as janelas de pasta abertas vão fechar. Continuar?",
                       L"Limpar cache de miniaturas", MB_ICONQUESTION | MB_YESNO) == IDYES;
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_trayMode) { AddTrayIcon(); return 0; }
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_APPLY: Apply(); return 0;
        case ID_CLOSE:
            if (g_trayMode) ShowWindow(h, SW_HIDE); else DestroyWindow(h);
            return 0;
        case ID_CLEARCACHE:
        case IDM_CLEAR:
            if (ConfirmClearCache(h)) ClearThumbnailCache();
            return 0;
        case ID_UNINSTALL:
            if (MessageBoxW(h, L"Remover o DesignThumbs? As miniaturas desses formatos deixam de ser geradas.",
                            L"DesignThumbs", MB_ICONQUESTION | MB_YESNO) == IDYES) {
                RemoveTrayIcon();
                DoUninstall(false);
                DestroyWindow(h);
            }
            return 0;
        case IDM_OPEN: ShowMain(); return 0;
        case IDM_EXIT: DestroyWindow(h); return 0;
        }
        break;
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK) ShowMain();
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) TrayMenu();
        return 0;
    case WM_SHOWME: ShowMain(); return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_CLOSE:
        if (g_trayMode) { ShowWindow(h, SW_HIDE); return 0; }
        break;
    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HFONT MakeFont(int pt, int weight) {
    HDC dc = GetDC(nullptr);
    int h = -MulDiv(pt, GetDeviceCaps(dc, LOGPIXELSY), 72);
    ReleaseDC(nullptr, dc);
    return CreateFontW(h, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int) {
    g_inst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring args = cmdLine ? cmdLine : L"";
    bool tray = args.find(L"/tray") != std::wstring::npos;
    bool uninstall = args.find(L"/uninstall") != std::wstring::npos;
    bool afterInstall = args.find(L"/afterinstall") != std::wstring::npos;

    if (uninstall) {
        if (MessageBoxW(nullptr, L"Remover o DesignThumbs?", L"DesignThumbs", MB_ICONQUESTION | MB_YESNO) == IDYES) {
            HWND other = FindWindowW(kWndClass, nullptr);
            if (other) PostMessageW(other, WM_COMMAND, IDM_EXIT, 0);
            DoUninstall(false);
        }
        return 0;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\DesignThumbs.Single");
    bool already = GetLastError() == ERROR_ALREADY_EXISTS;
    if (already && !afterInstall) {
        HWND other = FindWindowW(kWndClass, nullptr);
        if (other && !tray) PostMessageW(other, WM_SHOWME, 0, 0);
        return 0;
    }
    if (afterInstall && already) {
        Sleep(1500);   // espera a copia antiga fechar
    }

    g_installedHere = SameDir(DirOf(ExePath()), InstallDir()) &&
                      GetFileAttributesW((InstallDir() + L"\\DesignThumbs.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (g_installedHere) CleanupOldFiles(InstallDir());

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    HDC dc = GetDC(nullptr);
    g_scale = GetDeviceCaps(dc, LOGPIXELSX) / 96.0;
    ReleaseDC(nullptr, dc);
    g_font = MakeFont(9, FW_NORMAL);
    g_fontBold = MakeFont(10, FW_SEMIBOLD);
    g_fontTitle = MakeFont(16, FW_SEMIBOLD);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    g_wnd = CreateWindowExW(0, kWndClass, L"DesignThumbs", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, S(520), S(700), nullptr, nullptr, inst, nullptr);
    BuildUI();
    LoadIntoUI();

    g_trayMode = tray && g_installedHere && AutostartEnabled();
    if (g_trayMode) AddTrayIcon();
    if (!tray || !g_trayMode) {
        RECT r; GetWindowRect(g_wnd, &r);
        int w = r.right - r.left, hh = r.bottom - r.top;
        SetWindowPos(g_wnd, nullptr, (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - hh) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        ShowWindow(g_wnd, SW_SHOWNORMAL);
    }

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(g_wnd, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    if (mutex) CloseHandle(mutex);
    CoUninitialize();
    return 0;
}
