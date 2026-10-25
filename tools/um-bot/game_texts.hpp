// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "../um-multitool/text_groups.hpp"
#include "../um-multitool/viewer/item_texts.hpp"

namespace gametext {
namespace fs = std::filesystem;

struct Entry { std::string name; std::string description; };
struct Pack { std::string id; std::string label; std::vector<std::string> paths; };

class Catalog {
public:
    std::vector<Pack> packs;
    std::string selectedId;
    std::string error;

    // Later sources win: game, active mod, then the selected language/manual overlay.
    // Discovery is deliberately local: named resource directories around the supplied paths,
    // not a recursive scan of the user's disk, registry, Wine prefixes or shortcuts.
    void Discover(const std::string& databasePath, const std::string& characterPath,
                  const std::string& extraPath) try {
        packs.clear();
        discoveryError_.clear();
        std::vector<fs::path> roots;
        AddRoots(characterPath, roots);
        AddRoots(databasePath, roots);
        std::vector<std::string> base;
        std::map<std::string, std::vector<std::string>> languages;
        for (const fs::path& root : roots) {
            // Common base-install layouts, relative to a supplied path's ancestors.
            for (const char* parent : {"GOG Games", "Games", "Program Files", "Program Files (x86)"})
                ScanRoot(Child(Child(root, parent), "Evil Islands"), base, languages);
            ScanRoot(Child(root, "Evil Islands"), base, languages);
            ScanRoot(root, base, languages);
            // A repository checkout and a conventional EIStarter installation.
            ScanRoot(Child(root, "Universal-Mod"), base, languages);
            ScanRoot(Child(Child(root, "Mods"), "Universal-Mod"), base, languages);
            ScanRoot(Child(Child(root, "resources"), "universal-mod"), base, languages);
        }
        if (!base.empty()) packs.push_back({"local", "Game / active mod", base});
        for (const auto& language : languages) {
            std::vector<std::string> paths = base;
            for (const std::string& p : language.second) Append(paths, p);
            packs.push_back({"lang:" + language.first, language.first, std::move(paths)});
        }
        if (!extraPath.empty()) {
            const fs::path extra = Normal(extraPath);
            std::vector<std::string> manual;
            std::map<std::string, std::vector<std::string>> extraLanguages;
            std::error_code ec;
            if (fs::is_directory(extra, ec)) {
                ScanRoot(extra, manual, extraLanguages);
                if (IsTextFolder(extra)) Append(manual, extra.string());
                for (const auto& language : extraLanguages) {
                    std::vector<std::string> paths = base;
                    for (const std::string& p : manual) Append(paths, p);
                    for (const std::string& p : language.second) Append(paths, p);
                    packs.push_back({"extra:" + extra.generic_string() + ":" + language.first,
                                     "Selected source / " + language.first, std::move(paths)});
                }
            } else if (fs::is_regular_file(extra, ec)) {
                Append(manual, extra.string());
            } else {
                discoveryError_ = "Text source does not exist or cannot be accessed: " + extra.string();
            }
            if (!manual.empty()) {
                std::vector<std::string> paths = base;
                for (const std::string& p : manual) Append(paths, p);
                packs.push_back({"extra:" + extra.generic_string(), "Selected source: " + extra.filename().string(), std::move(paths)});
            } else if (extraLanguages.empty() && discoveryError_.empty()) {
                discoveryError_ = "No text archives, .umtexts groups or loose text keys found in: " + extra.string();
            }
        }
        if (packs.empty() && discoveryError_.empty())
            discoveryError_ = "No local game texts found. Select a texts.res/textslmp.res archive or a folder of .umtexts/loose text files.";
        // Rediscovery must not replace a working cache. Select/Reload commits it explicitly.
        error = discoveryError_;
    } catch (const fs::filesystem_error& failure) {
        discoveryError_ = failure.what();
        error = discoveryError_;
    }

    bool Select(const std::string& id) {
        const auto pack = std::find_if(packs.begin(), packs.end(), [&](const Pack& p) { return p.id == id; });
        if (pack == packs.end()) { error = "Text pack is unavailable: " + id; return false; }
        std::map<std::string, Entry> next;
        std::string problem;
        try {
            for (const std::string& path : pack->paths)
                if (!Load(path, next, problem)) { error = problem; return false; }
        } catch (const fs::filesystem_error& failure) {
            error = failure.what();
            return false;
        }
        if (next.empty()) { error = "No readable text entries in pack: " + pack->label; return false; }
        // All I/O and decoding completed before publishing. Failed loads retain the previous pack.
        std::string nextId = id;
        entries_.swap(next);
        selectedId.swap(nextId);
        error = discoveryError_;
        return true;
    }

    bool Reload() { return Select(selectedId); }

    // Returned pointers survive unsuccessful loads, but not a successful Select/Reload.
    const Entry* Find(const std::string& key) const {
        const auto entry = entries_.find(Lower(key));
        return entry == entries_.end() ? nullptr : &entry->second;
    }

private:
    std::map<std::string, Entry> entries_;
    std::string discoveryError_;

    static std::string Lower(const std::string& s) { return res::Archive::ToLower(s); }

    static fs::path Normal(const fs::path& p) {
        std::error_code ec;
        const fs::path canonical = fs::weakly_canonical(p, ec);
        if (!ec) return canonical;
        const fs::path absolute = fs::absolute(p, ec);
        return ec ? p.lexically_normal() : absolute.lexically_normal();
    }

    static void Append(std::vector<std::string>& paths, const std::string& path) {
        if (!path.empty() && std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
    }

    static fs::path Child(const fs::path& dir, const std::string& name) {
        if (dir.empty()) return {};
        std::error_code ec;
        const std::string key = Lower(name);
        for (const auto& child : fs::directory_iterator(dir, ec))
            if (Lower(child.path().filename().string()) == key) return child.path();
        return {};
    }

    static void AddRoots(const std::string& input, std::vector<fs::path>& roots) {
        if (input.empty()) return;
        fs::path path = Normal(input);
        std::error_code ec;
        if (!fs::is_directory(path, ec)) path = path.parent_path();
        std::vector<fs::path> ancestors;
        // Include this path's filesystem root: Windows commonly installs the game and
        // EIStarter as siblings on C:\. Discovery still inspects only named children,
        // never another drive or a recursive filesystem search.
        for (; !path.empty(); path = path.parent_path()) {
            ancestors.push_back(path);
            if (path == path.root_path()) break; // A filesystem root is its own parent.
        }
        for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it)
            if (std::find(roots.begin(), roots.end(), *it) == roots.end()) roots.push_back(*it);
    }

    static bool IsTextKey(const std::string& name) {
        const size_t space = name.find(' ');
        return space != std::string::npos && space > 0 && space + 1 < name.size() &&
               name.find_first_of("\r\n/\\") == std::string::npos;
    }

    static bool IsTextFolder(const fs::path& dir) {
        if (dir.empty()) return false;
        std::error_code ec;
        for (const auto& child : fs::directory_iterator(dir, ec)) {
            if (!child.is_regular_file(ec)) continue;
            if (Lower(child.path().extension().string()) == ".umtexts" ||
                IsTextKey(child.path().filename().string())) return true;
        }
        return false;
    }

    static void Sources(const fs::path& dir, std::vector<std::string>& paths) {
        if (dir.empty()) return;
        for (const char* name : {"texts.res", "textslmp.res", "texts_res", "textslmp_res"}) {
            const fs::path path = Child(dir, name);
            std::error_code ec;
            if (!path.empty() && (fs::is_regular_file(path, ec) || IsTextFolder(path)))
                Append(paths, Normal(path).string());
        }
    }

    static void ScanRoot(const fs::path& root, std::vector<std::string>& base,
                         std::map<std::string, std::vector<std::string>>& languages) {
        if (root.empty()) return;
        Sources(root, base);
        const fs::path resDir = Child(root, "res");
        Sources(resDir, base);
        if (IsTextFolder(resDir)) Append(base, Normal(resDir).string());
        const fs::path languageRoot = Lower(root.filename().string()) == "lang-packs" ? root : Child(root, "lang-packs");
        std::error_code ec;
        if (!languageRoot.empty()) {
            std::vector<fs::path> dirs;
            for (const auto& child : fs::directory_iterator(languageRoot, ec))
                if (child.is_directory(ec)) dirs.push_back(child.path());
            std::sort(dirs.begin(), dirs.end());
            for (const fs::path& dir : dirs) {
                std::vector<std::string> paths;
                Sources(dir, paths);
                const fs::path resDir = Child(dir, "res");
                Sources(resDir, paths);
                if (IsTextFolder(resDir)) Append(paths, Normal(resDir).string());
                if (!paths.empty()) {
                    auto& language = languages[Lower(dir.filename().string())];
                    for (const std::string& path : paths) Append(language, path);
                }
            }
        }
        const fs::path groupedRoot = Lower(root.filename().string()) == "res-texts" ? root : Child(root, "res-texts");
        if (groupedRoot.empty()) return;
        Sources(groupedRoot, base);
        std::vector<fs::path> dirs;
        for (const auto& child : fs::directory_iterator(groupedRoot, ec))
            if (child.is_directory(ec)) dirs.push_back(child.path());
        std::sort(dirs.begin(), dirs.end());
        // texts-<language>_res, then textslmp-<language>_res (the latter overrides).
        for (const char* prefix : {"texts-", "textslmp-"}) {
            for (const fs::path& dir : dirs) {
                const std::string name = Lower(dir.filename().string());
                const size_t start = std::strlen(prefix);
                if (name.rfind(prefix, 0) != 0 || name.size() <= start + 4 ||
                    name.compare(name.size() - 4, 4, "_res") != 0 || !IsTextFolder(dir)) continue;
                Append(languages[name.substr(start, name.size() - start - 4)], Normal(dir).string());
            }
        }
    }

    static bool Read(const fs::path& path, std::vector<uint8_t>& bytes, std::string& error) {
        if (!textgroups::ReadAll(path, bytes)) { error = "Cannot read text source: " + path.string(); return false; }
        return true;
    }

    // ParseArchive intentionally skips damaged records; text selection must report them instead.
    static bool ValidateArchive(const std::vector<uint8_t>& bytes, std::string& error) {
        if (bytes.size() < 16) { error = "too small for RES header"; return false; }
        auto u32 = [&](size_t offset) { uint32_t value; std::memcpy(&value, bytes.data() + offset, 4); return value; };
        const size_t count = u32(4), table = u32(8), names = u32(12);
        if (u32(0) != res::kResMagic) { error = "bad RES magic"; return false; }
        if (table > bytes.size() || count > (bytes.size() - table) / 22 || names > bytes.size() ||
            table + count * 22 > bytes.size() - names) {
            error = "corrupted RES directory bounds"; return false;
        }
        for (size_t index = 0; index < count; ++index) {
            const size_t offset = table + index * 22;
            uint16_t length;
            std::memcpy(&length, bytes.data() + offset + 16, 2);
            if (!length) continue; // Empty hash-table slot.
            const size_t nameOffset = u32(offset + 18), dataOffset = u32(offset + 8), dataLength = u32(offset + 4);
            if (nameOffset > names || length > names - nameOffset ||
                dataOffset > bytes.size() || dataLength > bytes.size() - dataOffset) {
                error = "corrupted RES entry " + std::to_string(index); return false;
            }
        }
        return true;
    }

    static bool Group(const fs::path& path, const std::vector<uint8_t>& bytes,
                      textgroups::Entries& out, std::string& error) {
        const std::string header = "# um-texts 1";
        if (bytes.size() < header.size() ||
            !std::equal(header.begin(), header.end(), bytes.begin()) ||
            (bytes.size() > header.size() && bytes[header.size()] != '\r' && bytes[header.size()] != '\n')) {
            error = "Invalid .umtexts header: " + path.string(); return false;
        }
        textgroups::Parse(bytes, out);
        if (out.empty()) { error = "No entries in text group: " + path.string(); return false; }
        for (const auto& entry : out)
            if (!IsTextKey(entry.first)) { error = "Invalid text key in: " + path.string(); return false; }
        return true;
    }

    static void AddEncodingEvidence(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& evidence) {
        // UTF-8 entries may coexist with legacy text in mod overrides. They need no
        // legacy-codepage decision and must not skew the source's 8-bit evidence.
        if (texts::IsValidUtf8(bytes)) return;
        evidence.insert(evidence.end(), bytes.begin(), bytes.end());
        evidence.push_back('\n'); // Never create a multibyte pair across entry boundaries.
    }

    static std::string SourceEncoding(const std::vector<uint8_t>& evidence) {
        // Detect once across a source, not a short label: Polish "włócznik" alone
        // can resemble CP949, while the surrounding Polish descriptions disambiguate it.
        // The shared CP949 probe lacks its table's upper bound (0xFE); 0xFF is
        // Russian CP1251, never a Korean lead/trail.
        for (size_t i = 0; i < evidence.size(); ++i) {
            if (evidence[i] < 0x80) continue;
            if (evidence[i] == 0xFF || (i + 1 < evidence.size() && evidence[i + 1] == 0xFF))
                return cp1250::Looks(evidence) ? "cp1250" : "cp1251";
            if (i + 1 < evidence.size()) ++i;
        }
        if (texts::LooksPolish(evidence)) return "cp1250";
        return texts::LooksKorean(evidence) ? "cp949" : "cp1251";
    }

    static bool Decode(const std::string& key, const std::vector<uint8_t>& bytes,
                       const std::string& encoding, std::map<std::string, Entry>& out, std::string& error) {
        if (!IsTextKey(key)) return true; // Archives also contain map.txt and non-description files.
        size_t size = bytes.size();
        while (size && bytes[size - 1] == 0) --size;
        for (size_t i = 0; i < size; ++i)
            if (bytes[i] < 32 && bytes[i] != '\r' && bytes[i] != '\n' && bytes[i] != '\t') {
                error = "Non-text data in entry: " + key; return false;
            }
        std::vector<uint8_t> trimmed;
        if (size != bytes.size()) trimmed.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size));
        const auto& body = size == bytes.size() ? bytes : trimmed;
        const std::string decoded = texts::IsValidUtf8(body)
            ? std::string(body.begin(), body.end()) : texts::DecodeAs(body, encoding);
        std::string clean;
        clean.reserve(decoded.size());
        const size_t first = decoded.rfind("\xEF\xBB\xBF", 0) == 0 ? 3 : 0;
        for (size_t i = first; i < decoded.size(); ++i) {
            if (decoded[i] == '\r') {
                clean += '\n';
                if (i + 1 < decoded.size() && decoded[i + 1] == '\n') ++i;
            } else clean += decoded[i];
        }
        const size_t newline = clean.find('\n');
        Entry entry;
        entry.name = clean.substr(0, newline);
        if (newline != std::string::npos) {
            entry.description = clean.substr(newline + 1);
            while (!entry.description.empty() && entry.description.back() == '\n') entry.description.pop_back();
        }
        out[Lower(key)] = std::move(entry);
        return true;
    }

    static bool Load(const fs::path& path, std::map<std::string, Entry>& out, std::string& error) {
        std::error_code ec;
        std::vector<fs::path> groups, loose;
        if (fs::is_directory(path, ec)) {
            for (const auto& child : fs::directory_iterator(path, ec)) {
                if (!child.is_regular_file(ec)) continue;
                if (Lower(child.path().extension().string()) == ".umtexts") groups.push_back(child.path());
                else if (IsTextKey(child.path().filename().string()) && child.path().extension() != ".tmp")
                    loose.push_back(child.path());
            }
            if (ec) { error = "Cannot enumerate text folder: " + path.string() + ": " + ec.message(); return false; }
            std::sort(groups.begin(), groups.end());
            std::sort(loose.begin(), loose.end());
        } else if (fs::is_regular_file(path, ec)) {
            const std::string extension = Lower(path.extension().string());
            if (extension == ".res") {
                std::vector<uint8_t> bytes;
                if (!Read(path, bytes, error)) return false;
                res::Archive archive;
                if (!ValidateArchive(bytes, error) || !res::ParseArchive(bytes, archive, error)) {
                    error = path.string() + ": " + error; return false;
                }
                std::vector<uint8_t> evidence;
                bool found = false;
                for (const auto& entry : archive.entries) {
                    if (!IsTextKey(entry.second.originalName)) continue;
                    found = true;
                    AddEncodingEvidence(entry.second.data, evidence);
                }
                if (!found) { error = "No text entries in archive: " + path.string(); return false; }
                const std::string encoding = SourceEncoding(evidence);
                for (const auto& entry : archive.entries)
                    if (!Decode(entry.second.originalName, entry.second.data, encoding, out, error)) {
                        error = path.string() + ": " + error; return false;
                    }
                return true;
            }
            if (extension == ".umtexts") groups.push_back(path);
            else if (IsTextKey(path.filename().string())) loose.push_back(path);
            else { error = "Unsupported text source: " + path.string(); return false; }
        } else { error = "Text source is missing or inaccessible: " + path.string(); return false; }
        if (groups.empty() && loose.empty()) { error = "No text entries in: " + path.string(); return false; }
        textgroups::Entries source;
        for (const fs::path& group : groups) {
            std::vector<uint8_t> bytes;
            textgroups::Entries entries;
            if (!Read(group, bytes, error) || !Group(group, bytes, entries, error)) return false;
            for (auto& entry : entries) source[Lower(entry.first)] = std::move(entry.second);
        }
        for (const fs::path& file : loose) {
            std::vector<uint8_t> bytes;
            if (!Read(file, bytes, error)) return false;
            source[Lower(file.filename().string())] = std::move(bytes);
        }
        std::vector<uint8_t> evidence;
        for (const auto& entry : source) AddEncodingEvidence(entry.second, evidence);
        const std::string encoding = SourceEncoding(evidence);
        for (const auto& entry : source)
            if (!Decode(entry.first, entry.second, encoding, out, error)) {
                error = path.string() + ": " + error; return false;
            }
        return true;
    }
};

} // namespace gametext
