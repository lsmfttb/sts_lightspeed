#include "sim/search/BattleScumSearcher2.h"
#include <sstream>
#include <stdexcept>
#include <iostream>

using namespace sts;
using search::Action;
using search::ActionType;
using Search = search::BattleScumSearcher2;

void require(bool ok) {
    if (!ok) throw std::runtime_error("potion occurrence identity regression");
}

int main() {
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
        require(action == Action(ActionType::POTION, slot, 6));
        require(action.isValidAction(bc));
        std::ostringstream label;
        action.printDesc(label, bc);
        require(label.str().find("discard potion") != std::string::npos);
        BattleContext canonical = bc, historical = bc;
        action.execute(canonical);
        Action(ActionType::POTION, slot, -1).execute(historical);
        require(canonical.potions == historical.potions);
        require(canonical.potionCount == historical.potionCount);
        require(canonical.potions[slot] == Potion::EMPTY_POTION_SLOT);
    }
    require(node.edges[0].action.bits != node.edges[1].action.bits);

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
        require(node.edges[target].action == Action(ActionType::POTION, 0, target));
        require(node.edges[target].action.isValidAction(bc));
    }

    for (auto &monster : bc.monsters.arr) monster.curHp = 0;
    node = {};
    searcher.enumeratePotionActions(node, bc);
    require(node.edges.size() == 1);
    require(node.edges[0].action == Action(ActionType::POTION, 0, 6));
    require(node.edges[0].action.isValidAction(bc));
    std::ostringstream label;
    node.edges[0].action.printDesc(label, bc);
    require(label.str().find("discard potion") != std::string::npos);
    require(label.str().find(" -> ") == std::string::npos);

    bc.potions[0] = Potion::BLOCK_POTION;
    node = {};
    searcher.enumeratePotionActions(node, bc);
    require(node.edges.size() == 1);
    require(node.edges[0].action == Action(ActionType::POTION, 0));
    require(node.edges[0].action.isValidAction(bc));
    std::cout << "POTION_DISCARD_IDENTITY_PASS\n";
}
