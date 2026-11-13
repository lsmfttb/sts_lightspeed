#include "../bindings/slaythespire.cpp"
#include <pybind11/embed.h>
#include <sstream>
#include <stdexcept>
#include <iostream>

using namespace sts;
using PotionAction = search::Action;
using search::ActionType;
using Search = search::BattleScumSearcher2;

void require(bool ok) {
    if (!ok) throw std::runtime_error("potion occurrence identity regression");
}

// Exercise the production bit-equality reporter with distinct fixture visits
// per occurrence; this does not run an episode or change the search policy.
void checkReport(const BattleContext &bc) {
    Search searcher(bc);
    searcher.enumeratePotionActions(searcher.root, bc);
    for (int i = 0; i < static_cast<int>(searcher.root.edges.size()); ++i) {
        auto &node = searcher.root.edges[i].node;
        node.simulationCount = i + 1;
        node.evaluationSum = 10 * node.simulationCount;
        searcher.root.simulationCount += node.simulationCount;
    }
    StepSimulator simulator(CharacterClass::IRONCLAD, 437016, 20);
    const auto report = simulator.buildBattleSearchReport(
            bc, searcher, searcher.root.simulationCount, true,
            "fixture", "fixture", nullptr, nullptr, pybind11::none());
    require(report["unmapped_search_edge_count"].cast<int>() == 0);
    const auto rows = report["root_rows"].cast<pybind11::list>();
    for (int i = 0; i < static_cast<int>(searcher.root.edges.size()); ++i) {
        int matches = 0;
        for (const auto value : rows) {
            const auto row = pybind11::reinterpret_borrow<pybind11::dict>(value);
            if (row["bits"].cast<std::uint32_t>() != searcher.root.edges[i].action.bits) continue;
            require(row["search_tree_present"].cast<bool>());
            require(row["search_edge_index"].cast<int>() == i);
            require(row["visits"].cast<int>() == i + 1);
            ++matches;
        }
        require(matches == 1);
    }
}

int main() {
    pybind11::scoped_interpreter interpreter{};
    BattleContext bc{};
    bc.inputState = InputState::PLAYER_NORMAL;
    bc.player.curHp = bc.player.maxHp = 80;
    bc.potionCapacity = 3;
    bc.potionCount = 2;
    bc.potions[0] = bc.potions[1] = Potion::FAIRY_POTION;
    Search searcher(bc);
    Search::Node node;
    searcher.enumeratePotionActions(node, bc);
    require(node.edges.size() == 2);
    for (int slot = 0; slot < 2; ++slot) {
        const auto action = node.edges[slot].action;
        require(action == PotionAction(ActionType::POTION, slot, 6));
        require(action.isValidAction(bc));
        std::ostringstream label;
        action.printDesc(label, bc);
        require(label.str().find("discard potion") != std::string::npos);
        BattleContext canonical = bc, historical = bc;
        action.execute(canonical);
        PotionAction(ActionType::POTION, slot, -1).execute(historical);
        require(canonical.potions == historical.potions);
        require(canonical.potionCount == historical.potionCount);
        require(canonical.potions[slot] == Potion::EMPTY_POTION_SLOT);
    }
    require(node.edges[0].action.bits != node.edges[1].action.bits);
    checkReport(bc);

    bc.potions[0] = Potion::FIRE_POTION;
    bc.potions[1] = Potion::EMPTY_POTION_SLOT;
    bc.potionCount = 1;
    bc.monsters.monsterCount = 2;
    for (auto &monster : bc.monsters.arr) monster = {};
    bc.monsters.arr[0].curHp = bc.monsters.arr[0].maxHp = 20;
    bc.monsters.arr[1].curHp = bc.monsters.arr[1].maxHp = 20;
    node = {};
    searcher.enumeratePotionActions(node, bc);
    require(node.edges.size() == 2);
    for (int target = 0; target < 2; ++target) {
        require(node.edges[target].action == PotionAction(ActionType::POTION, 0, target));
        require(node.edges[target].action.isValidAction(bc));
    }
    checkReport(bc);

    for (auto &monster : bc.monsters.arr) monster.curHp = 0;
    node = {};
    searcher.enumeratePotionActions(node, bc);
    require(node.edges.size() == 1);
    require(node.edges[0].action == PotionAction(ActionType::POTION, 0, 6));
    require(node.edges[0].action.isValidAction(bc));
    std::ostringstream label;
    node.edges[0].action.printDesc(label, bc);
    require(label.str().find("discard potion") != std::string::npos);
    require(label.str().find(" -> ") == std::string::npos);
    checkReport(bc);

    bc.potions[0] = Potion::BLOCK_POTION;
    node = {};
    searcher.enumeratePotionActions(node, bc);
    require(node.edges.size() == 1);
    require(node.edges[0].action == PotionAction(ActionType::POTION, 0));
    require(node.edges[0].action.isValidAction(bc));
    checkReport(bc);
    std::cout << "POTION_DISCARD_IDENTITY_PASS\n";
}

