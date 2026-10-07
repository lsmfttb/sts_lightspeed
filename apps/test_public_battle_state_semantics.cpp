#include <pybind11/embed.h>

#include "../bindings/slaythespire.cpp"

#include <map>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

pybind11::dict statusByName(const pybind11::list &statuses) {
    pybind11::dict result;
    for (const auto &item : statuses) {
        const auto row = pybind11::cast<pybind11::dict>(item);
        result[pybind11::str(pybind11::cast<std::string>(row["name"]))] = row["value"];
    }
    return result;
}

void verifyPublicPlayerStatusesAndStance() {
    Player player;
    player.cc = CharacterClass::IRONCLAD;
    player.buff<PS::INTANGIBLE>(2);
    player.buff<PS::BUFFER>(1);
    player.buff<PS::BARRICADE>();
    player.debuff<PS::ENTANGLED>(1, false);
    player.buff<PS::NO_DRAW>();
    player.buff<PS::THE_BOMB>(40);
    player.stance = Stance::WRATH;

    const auto snapshot = playerSnapshot(player);
    const auto statuses = statusByName(pybind11::cast<pybind11::list>(snapshot["active_statuses"]));
    require(pybind11::cast<int>(statuses["INTANGIBLE"]) == 2, "Intangible value was omitted");
    require(pybind11::cast<int>(statuses["BUFFER"]) == 1, "Buffer value was omitted");
    require(pybind11::cast<int>(statuses["BARRICADE"]) == 1, "boolean Barricade was omitted");
    require(pybind11::cast<int>(statuses["ENTANGLED"]) == 1, "Entangled value was omitted");
    require(pybind11::cast<int>(statuses["NO_DRAW"]) == 1, "No Draw value was omitted");
    const auto bomb = pybind11::cast<pybind11::dict>(statuses["THE_BOMB"]);
    require(pybind11::cast<int>(bomb["third_turn"]) == 40, "The Bomb timer was omitted");
    require(pybind11::cast<int>(snapshot["stance_id"]) == static_cast<int>(Stance::WRATH),
            "stance id was omitted");
    require(pybind11::cast<std::string>(snapshot["stance"]) == "WRATH",
            "stance label was incorrect");
}

void verifyDefectOrbStateIsExplicitlyUnsupported() {
    Player player;
    player.cc = CharacterClass::DEFECT;
    player.orbSlots = 3;

    const auto snapshot = playerSnapshot(player);
    const auto orbState = pybind11::cast<pybind11::dict>(snapshot["orb_state"]);
    require(pybind11::cast<std::string>(orbState["availability"]) == "unsupported",
            "untracked Defect orbs were reported as available");
    require(pybind11::cast<std::string>(orbState["reason"]).find("does not track orb") != std::string::npos,
            "Defect orb-state limitation was not explained");
}

void verifySearingBlowUpgradeCountsRemainDistinct() {
    BattleContext battle;
    CardInstance oneUpgrade(CardId::SEARING_BLOW);
    oneUpgrade.upgrade();
    CardInstance threeUpgrades(CardId::SEARING_BLOW);
    threeUpgrades.upgrade();
    threeUpgrades.upgrade();
    threeUpgrades.upgrade();
    battle.cards.drawPile.push_back(oneUpgrade);
    battle.cards.drawPile.push_back(threeUpgrades);

    const auto multiset = drawPilePublicMultiset(battle);
    require(multiset.size() == 2, "different Searing Blow upgrades were merged");
    std::map<int, int> countsByUpgrade;
    for (const auto &item : multiset) {
        const auto row = pybind11::cast<pybind11::dict>(item);
        countsByUpgrade[pybind11::cast<int>(row["upgrade_count"])] =
                pybind11::cast<int>(row["count"]);
    }
    require(countsByUpgrade.size() == 2 && countsByUpgrade[1] == 1 && countsByUpgrade[3] == 1,
            "draw-pile multiset did not preserve Searing Blow +N identity");
}

} // namespace

int main() {
    pybind11::scoped_interpreter interpreter{};
    verifyPublicPlayerStatusesAndStance();
    verifyDefectOrbStateIsExplicitlyUnsupported();
    verifySearingBlowUpgradeCountsRemainDistinct();
    std::cout << "PUBLIC_BATTLE_STATE_SEMANTICS_PASS\n";
    return 0;
}
