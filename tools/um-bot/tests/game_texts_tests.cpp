// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone: c++ -std=c++17 -I. -I../um-multitool tests/game_texts_tests.cpp -o /tmp/game_texts_tests
#include "../game_texts.hpp"

#include <cassert>
#include <chrono>
#include <iostream>

namespace fs = std::filesystem;

static std::vector<uint8_t> Bytes(const std::string& s) { return {s.begin(), s.end()}; }
static void Save(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    assert(textgroups::WriteAll(path, bytes));
}
static void Archive(const fs::path& path, const std::map<std::string, std::vector<uint8_t>>& entries) {
    Save(path, res::WriteArchive(entries, 0));
}

struct Fixture {
    fs::path path = fs::temp_directory_path() /
        ("um-bot-texts-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Fixture() { std::error_code ec; fs::remove_all(path, ec); }
};

int main() {
    Fixture fixture;
    const fs::path game = fixture.path / "GOG Games" / "Evil Islands";
    const fs::path mod = fixture.path / "EIStarter-UM" / "mods" / "Universal-Mod";
    const fs::path database = mod / "res" / "database.res";
    const fs::path character = mod / "mp" / "test.mp";
    Archive(game / "res" / "texts.res", {
        {"PERK Melee", Bytes("Walka wr\xEA" "cz\r\nZwi\xEA" "ksza si\xB3\xEA.\r\n")},
        {"PERK sword0", Bytes("Group\r\nBase group description\r\n")},
        {"PERK spear1", Bytes("Specjalista w\xB3\xF3" "cznik")},
        {"PERK PolishContext", Bytes("Zwi\xEA" "kszenie si\xB3" "y i zr\xEA" "czno\x9C" "ci.\nOpis zwi\xEA" "kszenia si\xB3" "y.")},
        {"PERK Utf8Base", Bytes(u8"Épée\nUTF-8 entry in a Polish source")},
        {"string infoskill_18", Bytes("Information\nDetails")}
    });
    Archive(mod / "res" / "texts.res", {{"PERK sword1", Bytes("Mod name\nMod description")}});
    Archive(mod / "lang-packs" / "eng" / "res" / "texts.res",
            {{"PERK Melee", Bytes("Melee\r\nEnglish description")}});
    Archive(mod / "lang-packs" / "eng" / "res" / "textslmp.res",
            {{"string infoskill_18", Bytes("Translated information\nTranslated details")}});
    const fs::path russian = fixture.path / "russian.res";
    Archive(russian, {
        {"PERK Russian", {0xCC,0xE5,0xF7,'\n',0xCE,0xEF,0xE8,0xF1,0xE0,0xED,0xE8,0xE5}},
        {"PERK RussianYa", {0xFF,'\n',0xFF}}
    });
    const fs::path korean = fixture.path / "korean.res";
    Archive(korean, {{"PERK Korean", {0xB0,0xA1,'\n',0xB0,0xA1}}});
    gametext::Catalog legacy;
    legacy.Discover("", "", russian.string());
    assert(legacy.Select(legacy.packs.front().id));
    assert(legacy.Find("PERK Russian")->name == u8"Меч");
    assert(legacy.Find("PERK Russian")->description == u8"Описание");
    assert(legacy.Find("PERK RussianYa")->name == u8"я");
    legacy.Discover("", "", korean.string());
    assert(legacy.Select(legacy.packs.front().id));
    assert(legacy.Find("PERK Korean")->name == u8"가");

    gametext::Catalog catalog;
    catalog.Discover(database.string(), character.string(), "");
    assert(catalog.error.empty());
    assert(catalog.Select("local"));
    assert(catalog.Find("pErK mElEe")->name == u8"Walka wręcz");
    assert(catalog.Find("PERK Melee")->description == u8"Zwiększa siłę.");
    assert(catalog.Find("PERK spear1")->name == u8"Specjalista włócznik");
    assert(catalog.Find("PERK Utf8Base")->name == u8"Épée");
    assert(catalog.Find("PERK sword1")->name == "Mod name");
    assert(catalog.Find("PERK missing") == nullptr);
    assert(catalog.Select("lang:eng"));
    assert(catalog.Find("PERK Melee")->name == "Melee");
    assert(catalog.Find("string infoskill_18")->name == "Translated information");
    assert(catalog.Find("PERK sword0")->description == "Base group description");

    const fs::path loose = fixture.path / "selected-texts";
    Save(loose / "PERK.umtexts", textgroups::Write({
        {"PERK sword1", Bytes("Grouped\nDescription\n=== literal line\n\\literal slash")},
        {"PERK Escaped", Bytes("Escaped\n=== literal line\n\\literal slash")},
        {"PERK spear1", Bytes("Specjalista w\xB3\xF3" "cznik")},
        {"PERK PolishContext", Bytes("Zwi\xEA" "kszenie si\xB3" "y i zr\xEA" "czno\x9C" "ci.\nOpis zwi\xEA" "kszenia si\xB3" "y.")},
        {"PERK Utf8", Bytes(u8"Épée\r\nUTF-8 description")}
    }));
    Save(loose / "perk SWORD1", Bytes("Loose\r\nLoose wins\r\n"));
    catalog.Discover(database.string(), character.string(), loose.string());
    assert(catalog.selectedId == "lang:eng"); // Discovery does not invalidate the live cache.
    const auto manual = std::find_if(catalog.packs.begin(), catalog.packs.end(),
                                    [](const gametext::Pack& p) { return p.id.rfind("extra:", 0) == 0; });
    assert(manual != catalog.packs.end());
    const std::string manualId = manual->id;
    assert(catalog.Select(manualId));
    assert(catalog.Find("PERK sword1")->name == "Loose");
    assert(catalog.Find("PERK sword1")->description == "Loose wins");
    assert(catalog.Find("PERK Utf8")->name == u8"Épée");
    assert(catalog.Find("PERK Escaped")->description == "=== literal line\n\\literal slash");
    assert(catalog.Find("PERK spear1")->name == u8"Specjalista włócznik");
    gametext::Catalog standaloneGroup;
    standaloneGroup.Discover("", "", (loose / "PERK.umtexts").string());
    assert(standaloneGroup.Select(standaloneGroup.packs.front().id));
    assert(standaloneGroup.Find("PERK spear1")->name == u8"Specjalista włócznik");
    Save(loose / "perk SWORD1", Bytes("Reloaded\nChanged"));
    assert(catalog.Reload());
    assert(catalog.Find("PERK sword1")->name == "Reloaded");
    catalog.Discover(database.string(), character.string(), loose.string());
    assert(catalog.Select(manualId)); // IDs do not depend on enumeration/index.

    Save(loose / "PERK.umtexts", Bytes("corrupt group"));
    const gametext::Entry* retained = catalog.Find("PERK sword1");
    assert(!catalog.Reload());
    assert(!catalog.error.empty());
    assert(catalog.selectedId == manualId);
    assert(catalog.Find("PERK sword1") == retained);
    assert(retained->name == "Reloaded");
    fs::remove(loose / "PERK.umtexts");
    fs::remove(loose / "perk SWORD1");
    assert(!catalog.Reload());
    assert(catalog.Find("PERK sword1") == retained);
    assert(!catalog.Select("missing-pack"));
    assert(catalog.selectedId == manualId);

    // The shared RES reader skips damaged records. Catalog must reject the whole reload.
    auto damaged = res::WriteArchive({{"PERK Broken", Bytes("Name\nDescription")}}, 0);
    uint32_t table;
    std::memcpy(&table, damaged.data() + 8, sizeof(table));
    const uint32_t impossibleOffset = 0xFFFFFFFFu;
    std::memcpy(damaged.data() + table + 8, &impossibleOffset, sizeof(impossibleOffset));
    const fs::path damagedPath = fixture.path / "broken.res";
    Save(damagedPath, damaged);
    catalog.Discover(database.string(), character.string(), damagedPath.string());
    const std::string brokenId = "extra:" + fs::weakly_canonical(damagedPath).generic_string();
    assert(!catalog.Select(brokenId));
    assert(catalog.error.find("corrupted RES entry") != std::string::npos);
    assert(catalog.Find("PERK sword1") == retained);

    gametext::Catalog absent;
    absent.Discover("", "", (fixture.path / "does-not-exist").string());
    assert(absent.packs.empty());
    assert(!absent.error.empty());
    assert(absent.Find("PERK Melee") == nullptr);
    std::cout << "game_texts catalog behavior passed\n";
}
