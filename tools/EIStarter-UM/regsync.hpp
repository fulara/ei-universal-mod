// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// Writes what the loader chain reads (HKCU\Software\Gipat.Ru\EI_Starter, see docs/EIStarter-UM.md) from the launcher's config file, so nobody has to
// run "Init Starter Registry.bat" or add the `path` value by hand:
//   EI_Starter\path            the launcher's folder (Engine\dinput.dll looks for addon.dll there)
//   EI_Starter\settings        AddonPath (the chosen mod's folder) and the addon.dll options
//   EI_Starter\EvilIslands     the game's settings (the clone of Nival's key that addon.dll points game.exe at)
#pragma once

#include <filesystem>
#include <vector>
#include <string>

#include "config.hpp"
#include "mods.hpp"

namespace launcher {

// A path the way Windows writes it (backslashes, the registry's strings).
inline std::string WinPath(std::string p) {
#ifdef _WIN32
    for (char& c : p) if (c == '/') c = '\\';
#endif
    return p;
}
inline std::string WithSlash(std::string p) {
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    if (!p.empty() && p.back() != '\\' && p.back() != '/') p += sep;
    return p;
}

// Whether `dir` looks like an Evil Islands installation (the original game's files).
inline bool LooksLikeGameFolder(const std::string& dir) {
    std::error_code ec;
    const fs::path d(dir);
    if (dir.empty() || !fs::is_directory(d, ec)) return false;
    auto has = [&](const char* name) {
        for (const fs::directory_entry& e : fs::directory_iterator(d, ec)) if (LowerAscii(e.path().filename().string()) == name) return true;
        return false;
    };
    return has("game.exe") && has("res");
}

// Whether the program runs under Wine (it has to ask Wine to use Engine\\dinput.dll, see SyncRegistry).
inline bool RunningUnderWine() {
#ifdef _WIN32
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
#else
    return false;
#endif
}

// Makes the path settings usable: GOG's installer writes a relative CD-ROM Path (".\\"), which the game resolves against Engine\\ and then asks for a disc;
// a missing path becomes the game folder (the original starter's convention: CD = game folder, resources = <game folder>\\res\\). A path that exists
// (the CD drive of the CD edition, D:\\) is kept. Returns true when something changed.
inline bool NormalizePaths(GameSettings& g) {
    std::error_code ec;
    const std::string work = g.GetString("Path Settings", "Work Path");
    if (!LooksLikeGameFolder(work)) return false;
    auto usable = [&](const std::string& p) { return p.size() >= 2 && (p[1] == ':' || p[0] == '/') && fs::is_directory(fs::path(p), ec); };
    bool changed = false;
    if (!usable(g.GetString("Path Settings", "CD-ROM Path"))) { g.SetString("Path Settings", "CD-ROM Path", WithSlash(work)); changed = true; }
    if (!usable(g.GetString("Path Settings", "Resources Path"))) { g.SetString("Path Settings", "Resources Path", WithSlash(WithSlash(work) + "res")); changed = true; }
    // The game builds its file names by appending to these paths ("<CD-ROM Path>3DfpFPU.dll", "<..>textures\\"): one without its closing backslash (typed by hand in
    // Options, or imported from another starter) makes "D:\Evil Islands3DfpFPU.dll", the game cannot load its 3D library and crashes on its missing renderer.
    for (const char* key : {"CD-ROM Path", "Resources Path", "Work Path"}) {
        const std::string v = g.GetString("Path Settings", key);
        if (!v.empty() && v != WithSlash(v)) { g.SetString("Path Settings", key, WithSlash(v)); changed = true; }
    }
    return changed;
}

struct SyncResult { bool ok = true; std::string error; };

// macdrv caches these options before the game creates its first window. Restore
// the user's values at that point, not when the game exits, so other modes and
// direct launches never inherit the framed renderer's GDI/OpenGL layer order.
class WineWindowStartup {
public:
    bool Pending() const { return pending_; }
    bool Begin(const fs::path& exe, bool enhanced, bool framed, std::string& error) {
#ifdef _WIN32
        if (pending_) { error = "A game is still starting."; return false; }
        if (!RunningUnderWine()) return true;
        StartupLock guard;
        if (!guard.Acquire(error) || !RecoverLocked(error)) return false;
        if (!enhanced) return true;
        const std::wstring name = exe.filename().wstring();
        const std::wstring driver = L"Software\\Wine\\AppDefaults\\" + name + L"\\Mac Driver";
        Key target, journal;
        LONG status = RegCreateKeyExW(HKEY_CURRENT_USER, driver.c_str(), 0, nullptr, 0,
            KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &target.h, nullptr);
        if (status != ERROR_SUCCESS) return Fail(error, "Cannot open Wine's game driver settings", status);
        status = RegCreateKeyExW(HKEY_CURRENT_USER, Journal(), 0, nullptr, 0,
            KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &journal.h, nullptr);
        if (status != ERROR_SUCCESS) return Fail(error, "Cannot save Wine's startup settings", status);
        if (!Write(journal.h, L"Exe", REG_SZ, reinterpret_cast<const BYTE*>(name.c_str()),
                   static_cast<DWORD>((name.size() + 1) * sizeof(wchar_t)), error) ||
            !Number(journal.h, L"Options", framed ? 3 : 2, error)) return false;
        const Spec* options = Options();
        for (int i = framed ? 0 : 1; i < 2; ++i) {
            Raw original;
            if (!Read(target.h, options[i].name, original, error) ||
                !Number(journal.h, options[i].present, original.present ? 1 : 0, error)) return false;
            if (original.present && (!Number(journal.h, options[i].type, original.type, error) ||
                !Write(journal.h, options[i].data, REG_BINARY, original.data.data(),
                       static_cast<DWORD>(original.data.size()), error))) return false;
        }
        // Commit recovery data before changing any driver option. A launcher
        // crash is recovered at the next Play, including a different window mode.
        if (!Number(journal.h, L"Pending", 1, error)) return false;
        pending_ = true;
        startupLock_.h = guard.h; guard.h = nullptr;
        for (int i = framed ? 0 : 1; i < 2; ++i) {
            if (!Write(target.h, options[i].name, REG_SZ, reinterpret_cast<const BYTE*>(options[i].replacement),
                       static_cast<DWORD>((lstrlenW(options[i].replacement) + 1) * sizeof(wchar_t)), error)) {
                std::string restoreError;
                if (!Restore(restoreError)) error += "; " + restoreError;
                return false;
            }
        }
#else
        (void)exe; (void)enhanced; (void)framed; (void)error;
#endif
        return true;
    }
    bool Restore(std::string& error) {
        if (!pending_) return true;
        if (!RecoverLocked(error)) return false;
#ifdef _WIN32
        startupLock_.Release();
#endif
        pending_ = false;
        return true;
    }
    static bool Recover(std::string& error) {
#ifdef _WIN32
        if (!RunningUnderWine()) return true;
        StartupLock guard;
        if (!guard.Acquire(error)) return false;
#endif
        return RecoverLocked(error);
    }
private:
    static bool RecoverLocked(std::string& error) {
#ifdef _WIN32
        Key journal;
        LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, Journal(), 0, KEY_QUERY_VALUE, &journal.h);
        if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return true;
        if (status != ERROR_SUCCESS) return Fail(error, "Cannot open Wine's startup recovery data", status);
        bool committed = false;
        DWORD pending = 0;
        if (!ReadNumber(journal.h, L"Pending", committed, pending, error)) return false;
        if (committed) {
            if (pending != 1) return Invalid(error);
            bool maskPresent = false;
            DWORD mask = 0;
            if (!ReadNumber(journal.h, L"Options", maskPresent, mask, error)) return false;
            if (!maskPresent || (mask != 2 && mask != 3)) return Invalid(error);
            Raw savedExe;
            if (!Read(journal.h, L"Exe", savedExe, error)) return false;
            if (!savedExe.present || savedExe.type != REG_SZ || savedExe.data.size() < sizeof(wchar_t) ||
                savedExe.data.size() % sizeof(wchar_t)) return Invalid(error);
            const wchar_t* text = reinterpret_cast<const wchar_t*>(savedExe.data.data());
            const size_t count = savedExe.data.size() / sizeof(wchar_t);
            if (text[count - 1] != 0) return Invalid(error);
            const std::wstring name(text, count - 1);
            if (name.empty() || name.find(L'\0') != std::wstring::npos || name.find_first_of(L"\\/:") != std::wstring::npos)
                return Invalid(error);
            const std::wstring driver = L"Software\\Wine\\AppDefaults\\" + name + L"\\Mac Driver";
            Key target;
            status = RegCreateKeyExW(HKEY_CURRENT_USER, driver.c_str(), 0, nullptr, 0,
                KEY_SET_VALUE, nullptr, &target.h, nullptr);
            if (status != ERROR_SUCCESS) return Fail(error, "Cannot restore Wine's game driver settings", status);
            for (int i = 0; i < 2; ++i) {
                if (!(mask & (1u << i))) continue;
                const Spec& option = Options()[i];
                bool presence = false;
                DWORD hadValue = 0;
                if (!ReadNumber(journal.h, option.present, presence, hadValue, error)) return false;
                if (!presence || hadValue > 1) return Invalid(error);
                if (hadValue) {
                    Raw data;
                    bool typePresent = false;
                    DWORD originalType = 0;
                    if (!ReadNumber(journal.h, option.type, typePresent, originalType, error) ||
                        !Read(journal.h, option.data, data, error)) return false;
                    if (!typePresent || !data.present || data.type != REG_BINARY) return Invalid(error);
                    if (!Write(target.h, option.name, originalType, data.data.data(),
                               static_cast<DWORD>(data.data.size()), error)) return false;
                } else {
                    status = RegDeleteValueW(target.h, option.name);
                    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
                        return Fail(error, "Cannot remove Wine's temporary driver option", status);
                }
            }
        }
        journal.Close();
        status = RegDeleteKeyW(HKEY_CURRENT_USER, Journal());
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
            return Fail(error, "Cannot remove Wine's startup recovery data", status);
#else
        (void)error;
#endif
        return true;
    }
    bool pending_ = false;
#ifdef _WIN32
    struct Key {
        HKEY h = nullptr;
        Key() = default;
        Key(const Key&) = delete;
        Key& operator=(const Key&) = delete;
        ~Key() { Close(); }
        void Close() { if (h) { RegCloseKey(h); h = nullptr; } }
    };
    // Shared by launcher instances in this Wine session. A second start must
    // not recover the journal while the first game's driver is still loading.
    struct StartupLock {
        HANDLE h = nullptr;
        StartupLock() = default;
        StartupLock(const StartupLock&) = delete;
        StartupLock& operator=(const StartupLock&) = delete;
        ~StartupLock() { Release(); }
        bool Acquire(std::string& error) {
            HANDLE created = CreateMutexW(nullptr, FALSE, L"Local\\EIStarterUM.WineWindowStartup");
            if (!created) return Fail(error, "Cannot lock Wine's startup settings", GetLastError());
            const DWORD wait = WaitForSingleObject(created, 0);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) { h = created; return true; }
            const DWORD status = GetLastError();
            CloseHandle(created);
            if (wait == WAIT_TIMEOUT) { error = "Another game is starting. Wait for its window before starting another."; return false; }
            return Fail(error, "Cannot lock Wine's startup settings", status);
        }
        void Release() { if (h) { ReleaseMutex(h); CloseHandle(h); h = nullptr; } }
    };
    StartupLock startupLock_;
    struct Raw { bool present = false; DWORD type = 0; std::vector<BYTE> data; };
    struct Spec { const wchar_t* name; const wchar_t* present; const wchar_t* type; const wchar_t* data; const wchar_t* replacement; };
    static const wchar_t* Journal() { return L"Software\\Gipat.Ru\\EI_Starter\\WindowStartup"; }
    static const Spec* Options() {
        static const Spec options[] = {
            {L"OpenGLSurfaceMode", L"GL.Present", L"GL.Type", L"GL.Data", L"behind"},
            {L"LeftOptionIsAlt", L"Alt.Present", L"Alt.Type", L"Alt.Data", L"Y"}
        };
        return options;
    }
    static bool Fail(std::string& error, const char* operation, LONG status) {
        error = std::string(operation) + " (registry error " + std::to_string(status) + ").";
        return false;
    }
    static bool Invalid(std::string& error) {
        error = "Wine's window-startup recovery data is incomplete. The saved driver options were not discarded.";
        return false;
    }
    static bool Read(HKEY key, const wchar_t* name, Raw& out, std::string& error) {
        DWORD size = 0;
        LONG status = RegQueryValueExW(key, name, nullptr, &out.type, nullptr, &size);
        if (status == ERROR_FILE_NOT_FOUND) return true;
        if (status != ERROR_SUCCESS) return Fail(error, "Cannot read Wine's driver option", status);
        out.present = true; out.data.resize(size);
        status = RegQueryValueExW(key, name, nullptr, &out.type, out.data.data(), &size);
        if (status != ERROR_SUCCESS) return Fail(error, "Cannot read Wine's driver option", status);
        out.data.resize(size);
        return true;
    }
    static bool Write(HKEY key, const wchar_t* name, DWORD type, const BYTE* data, DWORD size, std::string& error) {
        const LONG status = RegSetValueExW(key, name, 0, type, data, size);
        return status == ERROR_SUCCESS || Fail(error, "Cannot write Wine's driver option", status);
    }
    static bool Number(HKEY key, const wchar_t* name, DWORD value, std::string& error) {
        return Write(key, name, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof value, error);
    }
    static bool ReadNumber(HKEY key, const wchar_t* name, bool& present, DWORD& value, std::string& error) {
        DWORD type = 0, size = sizeof value;
        const LONG status = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
        if (status == ERROR_FILE_NOT_FOUND) { present = false; return true; }
        if (status == ERROR_MORE_DATA) return Invalid(error);
        if (status != ERROR_SUCCESS) return Fail(error, "Cannot read Wine's startup recovery data", status);
        present = true;
        return (type == REG_DWORD && size == sizeof value) || Invalid(error);
    }
#endif
};

inline SyncResult SyncRegistry(const LauncherConfig& cfg, const fs::path& launcherDir, const std::string& modFolder, void (*log)(const std::string&) = nullptr) {
    SyncResult r;
    const std::string dir = WinPath(launcherDir.string());
    if (!winreg::Set(kStarterKey, "path", winreg::Value::String(dir))) { r.ok = false; r.error = "cannot write the registry (HKCU\\" + std::string(kStarterKey) + ")"; return r; }

    // The addon options: AddonPath always, the others only when set (a missing value keeps the game's behaviour).
    winreg::Set(kAddonKey, "AddonPath", winreg::Value::String(WinPath((launcherDir / "mods" / modFolder).string())));
    if (log) log(std::string("HKCU\\") + kStarterKey + "\\path = " + dir + "; AddonPath = " + WinPath((launcherDir / "mods" / modFolder).string()));
    const AddonOptions& a = cfg.addon;
    auto dword = [&](const char* name, int v, bool write) {
        if (!write) return;
        winreg::Set(kAddonKey, name, winreg::Value::Dword(static_cast<uint32_t>(v)));
        if (log) log(std::string("HKCU\\") + kAddonKey + "\\" + name + " = dword:" + std::to_string(v));
    };
    dword("LandscapeDrawRadius", a.landscapeDrawRadius, a.landscapeDrawRadius > 0);
    dword("ObjectsDrawRadius", a.objectsDrawRadius, a.objectsDrawRadius > 0);
    dword("PauseOnMinimize", a.pauseOnMinimize, true);
    dword("MP Speed-control", a.mpSpeedControl, true);
    dword("MQ client", a.mqClient, true);
    dword("MP console", a.mpConsole, true);
    dword("LogLevel", a.logLevel, true);
    dword("ConsoleLogLevel", a.consoleLogLevel, true);
    dword("Xoptions Active", 1, true);   // what EIStarter's Options dialog always wrote
    dword("ModesFiltration", 0, true);

    // Wine uses its own dinput / ddraw unless told otherwise: Engine\\dinput.dll is the loader that brings addon.dll in (EIStarter needed this set by hand,
    // in winecfg or WINEDLLOVERRIDES). A Windows system has no use for the key.
    if (RunningUnderWine()) {
        std::error_code ec;
        winreg::Set("Software\\Wine\\DllOverrides", "dinput", winreg::Value::String("native,builtin"));
        if (fs::exists(launcherDir / "Engine" / "ddraw.dll", ec)) winreg::Set("Software\\Wine\\DllOverrides", "ddraw", winreg::Value::String("native,builtin"));
    }

    // The game's own settings, over the clone (created from Nival's key when it does not exist yet).
    if (!winreg::KeyExists(kCloneKey) && winreg::KeyExists(kNivalKey)) winreg::CopyTree(kNivalKey, kCloneKey);
    cfg.game.ApplyToRegistry(kCloneKey, log);
    return r;
}

} // namespace launcher
