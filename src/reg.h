#pragma once
#include "common.h"

struct FormatGroup {
    const wchar_t* key;        // nome do valor em HKCU\Software\DesignThumbs\Formats
    const wchar_t* label;      // texto na interface
    const wchar_t* exts[4];
};

extern const FormatGroup kFormats[];
extern const int kFormatCount;

bool RegisterClsid(const std::wstring& dllPath);
void UnregisterClsid();
void SetExtensionHandler(const wchar_t* ext, bool enable, bool removeOverlay);
void ApplyFormats(const bool enabled[], bool removeOverlay);   // registra/desregistra todos
void UnregisterAllExtensions();
bool IsGroupRegistered(int group);
void NotifyShell();
