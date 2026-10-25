// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <limits>

#include "growth.hpp"

namespace development {

struct Skill { int offset; const char* key; const char* name; };
// UI order is not the database's skill-index order: index 2 there is unused Backstab.
// The camp's Use/Steal purchase raises Science at 0x6C8, not Backstab at 0x6CD.
inline constexpr Skill kSkills[] = {
    {0x6CB, "PERK Melee", "Melee"},
    {0x6CC, "PERK Archery", "Archery"},
    {0x6C8, "PERK Science", "Use/Steal"},
    {0x6CE, "PERK Elemental", "Elemental Magic"},
    {0x6CF, "PERK Sense", "Sensory Magic"},
    {0x6D0, "PERK Astral", "Astral Magic"},
};

struct Quote { int cost = 0; bool allowed = false; std::string reason; };

inline bool HasStats(const mp::Member& member) {
    return member.stats.size() >= static_cast<size_t>(mp::kPerkBase + mp::kPerkGroups);
}

inline Quote ExperienceQuote(const mp::Member& member, int cost) {
    if (cost <= 0) return {cost, false, "Experience cost is unavailable or out of range."};
    const float total = mp::GetF(member.stats, mp::kExpTotal);
    const float spent = mp::GetF(member.stats, mp::kExpSpent);
    if (!std::isfinite(total) || !std::isfinite(spent) || total < 0 || spent < 0)
        return {cost, false, "Invalid character experience."};
    if (static_cast<double>(total) - spent < cost)
        return {cost, false, "Insufficient free experience."};
    return {cost, true, {}};
}

inline Quote SkillQuote(const items::Database&, const mp::Member& member, size_t skillIndex) {
    if (skillIndex >= sizeof kSkills / sizeof kSkills[0]) return {0, false, "Invalid skill index."};
    if (!HasStats(member)) return {0, false, "Character statistics are incomplete."};
    const int level = growth::Level(member, kSkills[skillIndex].offset);
    if (level >= growth::kSkillCap) return {0, false, "Skill is at its maximum level."};
    return ExperienceQuote(member, growth::SkillCost(level));
}

inline Quote PerkQuote(const items::Database& db, const mp::Member& member, size_t perkRow) {
    if (perkRow >= db.perks.size() || perkRow / 3 >= static_cast<size_t>(mp::kPerkGroups))
        return {0, false, "Invalid ability row."};
    if (!HasStats(member)) return {0, false, "Character statistics are incomplete."};
    const auto& perk = db.perks[perkRow];
    const int have = growth::PerkLevel(member, perkRow / 3);
    if (have >= 3) return {0, false, "Ability is at its maximum level."};
    if (have != static_cast<int>(perkRow % 3)) return {0, false, "Only the next ability level can be purchased."};
    const int cost = growth::PerkCost(db, perkRow, growth::AbilitiesOwned(member));
    // CanLearn remains authoritative. The checks below only explain its first unmet rule.
    // Prerequisite skill indices are database bytes, not kSkills UI order.
    if (!growth::CanLearn(db, member, perkRow)) {
        if (!perk.enabled) return {cost, false, "Ability is disabled in the active database."};
        static constexpr const char* attributes[] = {"Strength", "Dexterity", "Intelligence"};
        for (int attribute = 0; attribute < 3; ++attribute) {
            const float current = mp::GetF(member.stats, 0x10 + attribute * 8);
            if (perk.attrMin[attribute] > current)
                return {cost, false, std::string("Requires ") + attributes[attribute] + " >= " +
                    std::to_string(perk.attrMin[attribute]) + " (current " + std::to_string(current) + ")."};
        }
        if (!perk.skill.empty() && perk.skill != "none") {
            if (perk.skillIndex < 0 || perk.skillIndex >= growth::kSkills)
                return {cost, false, "Required skill has an invalid database index: " + perk.skill + "."};
            const int current = growth::Level(member, growth::kSkillBase + perk.skillIndex);
            if (perk.skillLevel > current)
                return {cost, false, "Requires skill " + perk.skill + " >= " + std::to_string(perk.skillLevel) +
                    " (current " + std::to_string(current) + ")."};
        }
        if (!perk.prereq.empty() && perk.prereq != "none") {
            size_t prerequisite = db.perks.size();
            for (size_t row = 0; row < db.perks.size(); ++row)
                if (db.perks[row].key == perk.prereq) { prerequisite = row; break; }
            if (prerequisite >= db.perks.size())
                return {cost, false, "Required ability is missing from the database: " + perk.prereq + "."};
            const int required = static_cast<int>(prerequisite % 3) + 1;
            const int current = growth::PerkLevel(member, prerequisite / 3);
            if (current != required)
                return {cost, false, "Requires ability " + perk.prereq + " at exactly level " +
                    std::to_string(required) + " (current level " + std::to_string(current) + ")."};
        }
        for (size_t category = 0; category < static_cast<size_t>(mp::kPerkGroups) && category * 3 < db.perks.size(); ++category) {
            const int level = growth::PerkLevel(member, category);
            if (category == perkRow / 3 || level <= 0) continue;
            const size_t other = category * 3 + static_cast<size_t>(level) - 1;
            if (other < db.perks.size() && db.perks[other].group == perk.group)
                return {cost, false, "Conflicts with owned ability " + db.perks[other].key + "."};
        }
        return {cost, false, "Ability cannot be learned."};
    }
    return ExperienceQuote(member, cost);
}

// Indices are stable UI skill indices / database perk rows, never localized display names.
struct Purchase { bool perk; size_t index; int from; int to; int cost; };

struct Draft {
    mp::Character character;
    std::vector<Purchase> purchases;
    int totalCost = 0;

    void Begin(const mp::Character& source) {
        character = source;
        purchases.clear();
        totalCost = 0;
    }

    void Reset() {
        character = {};
        purchases.clear();
        totalCost = 0;
    }

    bool Dirty() const { return !purchases.empty(); }

    bool BuySkill(const items::Database& db, size_t index, std::string& error) {
        if (character.members.empty()) { error = "No character loaded."; return false; }
        const Quote quote = SkillQuote(db, character.members[0], index);
        if (!CanSpend(quote, error)) return false;
        mp::Member& member = character.members[0];
        const float carryScale = growth::CarryScale(db, member);
        if (carryScale <= 0) { error = "Invalid source carrying capacity or Strength."; return false; }
        const int offset = kSkills[index].offset;
        const int from = growth::Level(member, offset);
        purchases.push_back({false, index, from, from + 1, quote.cost});
        member.stats[static_cast<size_t>(offset)] = static_cast<uint8_t>(from + 1);
        Spend(db, member, quote.cost, carryScale);
        error.clear();
        return true;
    }

    bool BuyPerk(const items::Database& db, size_t row, std::string& error) {
        if (character.members.empty()) { error = "No character loaded."; return false; }
        const Quote quote = PerkQuote(db, character.members[0], row);
        if (!CanSpend(quote, error)) return false;
        mp::Member& member = character.members[0];
        const float carryScale = growth::CarryScale(db, member);
        if (carryScale <= 0) { error = "Invalid source carrying capacity or Strength."; return false; }
        const int from = growth::PerkLevel(member, row / 3);
        const int to = static_cast<int>(row % 3) + 1;
        purchases.push_back({true, row, from, to, quote.cost});
        member.stats[static_cast<size_t>(mp::kPerkBase) + row / 3] = static_cast<uint8_t>(to);
        Spend(db, member, quote.cost, carryScale);
        error.clear();
        return true;
    }

private:
    bool CanSpend(const Quote& quote, std::string& error) const {
        if (!quote.allowed) { error = quote.reason; return false; }
        if (quote.cost > std::numeric_limits<int>::max() - totalCost) {
            error = "Draft experience cost is out of range.";
            return false;
        }
        return true;
    }

    void Spend(const items::Database& db, mp::Member& member, int cost, float carryScale) {
        totalCost += cost;
        mp::SetF(member.stats, mp::kExpSpent, mp::GetF(member.stats, mp::kExpSpent) + static_cast<float>(cost));
        growth::Refresh(db, member, carryScale);
    }
};

} // namespace development
