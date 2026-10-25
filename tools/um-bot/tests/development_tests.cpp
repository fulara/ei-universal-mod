// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include "../development.hpp"

static items::Database Database() {
    items::Database db;
    db.perks.resize(mp::kPerkGroups * 3);
    for (size_t row = 0; row < db.perks.size(); ++row) {
        auto& p = db.perks[row];
        p.key = "ability" + std::to_string(row);
        p.enabled = true;
        p.group = static_cast<int>(row / 3);
        p.costNum = static_cast<int>(row % 3 + 1) * 5;
        p.bonus = static_cast<int>(row % 3 + 1) * 10;
    }
    return db;
}

static mp::Character Character() {
    mp::Character ch;
    ch.members.emplace_back();
    auto& m = ch.members[0];
    m.stats.assign(0x704, 0);
    mp::SetF(m.stats, mp::kExpTotal, 1000000);
    for (int at : {0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20}) mp::SetF(m.stats, at, 25);
    mp::SetF(m.stats, 0x54, 375); // Universal-Mod's observed 15 capacity per effective Strength
    return ch;
}

int main() {
    auto db = Database();
    const auto source = Character();
    development::Draft draft;
    draft.Begin(source);
    std::string error;
    assert(!draft.Dirty());
    const int skillCost = growth::SkillCost(0);
    assert(draft.BuySkill(db, 2, error));
    assert(error.empty() && draft.Dirty());
    const auto& m = draft.character.members[0];
    assert(m.stats[0x6C8] == 1 && m.stats[0x6CD] == 0);
    assert(source.members[0].stats[0x6C8] == 0);
    assert(draft.totalCost == skillCost && mp::GetF(m.stats, mp::kExpSpent) == skillCost);
    assert(draft.purchases[0].index == 2 && !draft.purchases[0].perk);
    assert(draft.purchases[0].from == 0 && draft.purchases[0].to == 1);
    assert(mp::GetF(m.stats, 0x54) == 375); // unrelated purchase preserves active mod coefficient

    // Every rejected operation leaves all draft statistics and purchase accounting intact.
    auto rejectSkill = [&](size_t index) {
        const auto stats = m.stats;
        const auto count = draft.purchases.size();
        const int spent = draft.totalCost;
        assert(!draft.BuySkill(db, index, error) && !error.empty());
        assert(m.stats == stats && draft.purchases.size() == count && draft.totalCost == spent);
    };
    auto rejectPerk = [&](size_t row) {
        const auto stats = m.stats;
        const auto count = draft.purchases.size();
        const int spent = draft.totalCost;
        assert(!draft.BuyPerk(db, row, error) && !error.empty());
        assert(m.stats == stats && draft.purchases.size() == count && draft.totalCost == spent);
    };
    rejectSkill(6);
    rejectPerk(db.perks.size());
    draft.character.members[0].stats[0x6CB] = 100;
    rejectSkill(0);
    auto blockedQuote = [&](size_t row, const char* reason) {
        const auto quote = development::PerkQuote(db, m, row);
        assert(!quote.allowed);
        assert(quote.cost == growth::PerkCost(db, row, growth::AbilitiesOwned(m)));
        assert(quote.cost > 0 && quote.reason.find(reason) != std::string::npos);
    };
    rejectPerk(1); // level two cannot be bought before level one
    db.perks[0].enabled = false;
    rejectPerk(0);
    blockedQuote(0, "disabled");
    db.perks[0].enabled = true;
    db.perks[0].attrMin[0] = 26;
    rejectPerk(0);
    blockedQuote(0, "Strength");
    db.perks[0].attrMin[0] = 0;
    db.perks[0].skill = "backstab";
    db.perks[0].skillIndex = 2;
    db.perks[0].skillLevel = 1;
    rejectPerk(0); // Use/Steal point must not satisfy database Backstab index 2
    blockedQuote(0, "Requires skill backstab >= 1 (current 0)");
    db.perks[0].skill = "none";
    db.perks[0].prereq = "missing";
    rejectPerk(0);
    blockedQuote(0, "missing from the database: missing");
    db.perks[0].prereq = "ability3";
    rejectPerk(0);
    blockedQuote(0, "Requires ability ability3 at exactly level 1 (current level 0)");
    assert(draft.BuyPerk(db, 3, error));
    const int nextCost = development::PerkQuote(db, m, 0).cost;
    assert(nextCost == growth::PerkCost(db, 0, 1));
    assert(nextCost > growth::PerkCost(db, 0, 0));
    assert(draft.BuyPerk(db, 0, error));
    assert(draft.purchases.back().perk && draft.purchases.back().index == 0);
    assert(draft.purchases.back().from == 0 && draft.purchases.back().to == 1);
    db.perks[6].group = db.perks[0].group;
    rejectPerk(6);
    blockedQuote(6, "Conflicts with owned ability ability0");
    // A named prerequisite requires that exact level, not any later level.
    assert(draft.BuyPerk(db, 4, error));
    db.perks[9].prereq = "ability3";
    rejectPerk(9);
    blockedQuote(9, "Requires ability ability3 at exactly level 1 (current level 2)");
    assert(draft.BuyPerk(db, 1, error));
    assert(draft.BuyPerk(db, 2, error));
    rejectPerk(2);
    mp::SetF(draft.character.members[0].stats, mp::kExpTotal, mp::GetF(m.stats, mp::kExpSpent));
    rejectSkill(1);
    rejectPerk(12);
    blockedQuote(12, "Insufficient free experience");

    draft.Begin(source);
    assert(!draft.Dirty() && draft.totalCost == 0 && draft.character.members[0].stats == source.members[0].stats);
    draft.Reset();
    assert(!draft.Dirty() && draft.totalCost == 0 && draft.character.members.empty());
    assert(!draft.BuySkill(db, 0, error));
    assert(!draft.BuyPerk(db, 0, error));
    mp::Member truncated;
    assert(!development::SkillQuote(db, truncated, 0).allowed);
    assert(!development::PerkQuote(db, truncated, 0).allowed);
    draft.Begin(source);
    db.perks[15].attrMin[0] = 35;
    assert(!development::PerkQuote(db, draft.character.members[0], 15).allowed);
    assert(draft.BuyPerk(db, 23 * 3, error)); // Strength: recompute before quoting another perk
    assert(mp::GetF(draft.character.members[0].stats, 0x10) == 35);
    assert(mp::GetF(draft.character.members[0].stats, 0x54) == 525);
    assert(development::PerkQuote(db, draft.character.members[0], 15).allowed);
    assert(draft.BuyPerk(db, 21 * 3, error));
    assert(std::fabs(mp::GetF(draft.character.members[0].stats, 0x54) - 577.5f) < 0.001f);
    assert(draft.BuyPerk(db, 21 * 3 + 1, error));
    assert(std::fabs(mp::GetF(draft.character.members[0].stats, 0x54) - 630.0f) < 0.001f);
    assert(draft.BuySkill(db, 1, error));
    assert(std::fabs(growth::CarryScale(db, draft.character.members[0]) - 15.0f) < 0.001f);

    // The automatic path also snapshots the old coefficient before all plan mutations.
    auto automatic = source;
    growth::Plan plan;
    const int strengthCost = growth::PerkCost(db, 23 * 3, 0);
    const int liftCost = growth::PerkCost(db, 21 * 3, 1);
    plan.buys = {{true, 23 * 3, 0, strengthCost}, {true, 21 * 3, 0, liftCost}};
    plan.cost = strengthCost + liftCost;
    plan.freeBefore = 1000000;
    growth::Apply(db, automatic, plan);
    assert(mp::GetF(automatic.members[0].stats, 0x10) == 35);
    assert(std::fabs(mp::GetF(automatic.members[0].stats, 0x54) - 577.5f) < 0.001f);
    assert(mp::GetF(automatic.members[0].stats, mp::kExpSpent) == plan.cost);
    growth::Plan skillPlan;
    skillPlan.buys = {{false, growth::kArchery, 1, skillCost}};
    skillPlan.cost = skillCost;
    automatic = source;
    growth::Apply(db, automatic, skillPlan);
    assert(mp::GetF(automatic.members[0].stats, 0x54) == 375);

    // Invalid source caches are rejected rather than inventing a vanilla coefficient.
    draft.Begin(source);
    mp::SetF(draft.character.members[0].stats, 0x54, 0);
    const auto invalidStats = draft.character.members[0].stats;
    assert(!draft.BuySkill(db, 0, error));
    assert(!draft.BuyPerk(db, 23 * 3, error));
    assert(!draft.Dirty() && draft.totalCost == 0 && draft.character.members[0].stats == invalidStats);
    auto invalidAutomatic = draft.character;
    growth::Apply(db, invalidAutomatic, plan);
    assert(invalidAutomatic.members[0].stats == invalidStats);
    auto vanilla = source;
    mp::SetF(vanilla.members[0].stats, 0x54, 300);
    growth::Apply(db, vanilla, plan);
    assert(std::fabs(mp::GetF(vanilla.members[0].stats, 0x54) - 462.0f) < 0.001f);
    // An unaffordable extreme dynamic price must not overflow the integer cost conversion.
    assert(growth::PerkCost(db, 0, 78) == 0);
}
