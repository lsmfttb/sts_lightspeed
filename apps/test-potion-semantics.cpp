#include <algorithm>
#include <array>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

#include "combat/BattleContext.h"
#include "game/GameContext.h"
#include "sim/search/Action.h"
#include "sim/search/BattleScumSearcher2.h"

namespace {

using namespace sts;

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::logic_error(message);
    }
}

void requireNextPotionRewardMatches(
        GameContext &actual, GameContext &expected, const std::string &context) {
    Rewards actualNextReward;
    Rewards expectedNextReward;
    actual.addPotionRewards(actualNextReward);
    expected.addPotionRewards(expectedNextReward);

    require(actualNextReward.potionCount == expectedNextReward.potionCount,
            context + ": next potion reward count differs after hidden roll");
    for (int i = 0; i < actualNextReward.potionCount; ++i) {
        require(actualNextReward.potions[i] == expectedNextReward.potions[i],
                context + ": next potion reward differs after hidden roll");
    }
    require(actual.potionChance == expected.potionChance
                    && actual.potionRng.counter == expected.potionRng.counter,
            context + ": next potion reward did not continue from the hidden-roll state");
}

GameContext gameFixture() {
    GameContext gc(CharacterClass::IRONCLAD, 52026, 0);
    gc.potions.fill(Potion::EMPTY_POTION_SLOT);
    gc.potionCapacity = 3;
    gc.potionCount = 0;
    return gc;
}

BattleContext battleFixture(
        GameContext &gc,
        const MonsterEncounter encounter,
        const Room room = Room::MONSTER) {
    gc.screenState = ScreenState::BATTLE;
    gc.curRoom = room;
    gc.info.encounter = encounter;
    BattleContext bc;
    bc.init(gc, encounter);
    bc.inputState = InputState::PLAYER_NORMAL;
    return bc;
}

int expectedHeal(const int maxHp, const bool sacredBark) {
    return static_cast<int>(static_cast<float>(maxHp)
            * (sacredBark ? 0.40f : 0.20f));
}

void checkBloodPotion(const bool inBattle, const bool sacredBark, const bool capped) {
    auto gc = gameFixture();
    gc.maxHp = 73;
    gc.curHp = capped ? 72 : 40;
    if (sacredBark) {
        gc.relics.add({RelicId::SACRED_BARK, 0});
    }
    gc.potions[0] = Potion::BLOOD_POTION;
    gc.potionCount = 1;

    const int expected = std::min(gc.maxHp,
            gc.curHp + expectedHeal(gc.maxHp, sacredBark));
    if (inBattle) {
        auto bc = battleFixture(gc, MonsterEncounter::CULTIST);
        bc.player.maxHp = gc.maxHp;
        bc.player.curHp = gc.curHp;
        const search::Action useBlood(search::ActionType::POTION, 0);
        require(useBlood.isValidAction(bc), "battle Blood Potion was not legal");
        useBlood.execute(bc);
        require(bc.player.curHp == expected,
                "battle Blood Potion percentage, floor rounding, or HP cap was wrong");
        require(bc.potionCount == 0 && bc.potions[0] == Potion::EMPTY_POTION_SLOT,
                "battle Blood Potion did not consume its selected slot");
    } else {
        gc.drinkPotionAtIdx(0);
        require(gc.curHp == expected,
                "noncombat Blood Potion percentage, floor rounding, or HP cap was wrong");
        require(gc.potionCount == 0 && gc.potions[0] == Potion::EMPTY_POTION_SLOT,
                "noncombat Blood Potion did not consume its selected slot");
    }
}

void checkFruitJuice(const bool sacredBark) {
    auto gc = gameFixture();
    gc.maxHp = 80;
    gc.curHp = 70;
    gc.potions[0] = Potion::FRUIT_JUICE;
    gc.potions[1] = Potion::FIRE_POTION;
    gc.potions[2] = Potion::SPEED_POTION;
    gc.potionCount = 3;
    if (sacredBark) {
        gc.relics.add({RelicId::SACRED_BARK, 0});
    }
    gc.drinkPotionAtIdx(0);
    const int increase = sacredBark ? 10 : 5;
    require(gc.maxHp == 80 + increase && gc.curHp == 70 + increase,
            "Fruit Juice max HP/heal regression changed");
    require(gc.potionCount == 2 && gc.potions[1] == Potion::FIRE_POTION
                    && gc.potions[2] == Potion::SPEED_POTION,
            "Fruit Juice changed unrelated occupied potion slots");
}

void checkEntropicBrew(const int initialCapacity, const bool sacredBark) {
    auto gc = gameFixture();
    if (initialCapacity == 5) {
        gc.obtainRelic(RelicId::POTION_BELT);
        require(gc.relics.has(RelicId::POTION_BELT), "Potion Belt fixture relic was not acquired");
        require(gc.potionCapacity == 5, "Potion Belt did not add its two slots");
    }
    if (sacredBark) {
        gc.relics.add({RelicId::SACRED_BARK, 0});
    }
    gc.potions[0] = Potion::ENTROPIC_BREW;
    gc.potions[1] = Potion::FIRE_POTION;
    gc.potions[2] = Potion::BLOCK_POTION;
    gc.potionCount = 3;
    gc.drinkPotionAtIdx(0);
    require(gc.potionCount == gc.potionCapacity,
            "Entropic Brew did not fill every empty slot");
    for (int index = 0; index < gc.potionCapacity; ++index) {
        require(gc.potions[index] != Potion::EMPTY_POTION_SLOT,
                "Entropic Brew left a potion slot empty");
    }
    require(gc.potions[1] == Potion::FIRE_POTION
                    && gc.potions[2] == Potion::BLOCK_POTION,
            "Entropic Brew changed already occupied non-brew slots");

    auto withoutBark = gameFixture();
    if (initialCapacity == 5) {
        withoutBark.obtainRelic(RelicId::POTION_BELT);
    }
    withoutBark.potions[0] = Potion::ENTROPIC_BREW;
    withoutBark.potions[1] = Potion::FIRE_POTION;
    withoutBark.potions[2] = Potion::BLOCK_POTION;
    withoutBark.potionCount = 3;
    withoutBark.drinkPotionAtIdx(0);
    require(gc.potions == withoutBark.potions,
            "Sacred Bark unexpectedly changed Entropic Brew's random potion result");
}

void checkSozuEntropicBrew(
        const bool inBattle,
        const int initialCapacity,
        const bool sacredBark) {
    auto gc = gameFixture();
    if (initialCapacity == 5) {
        gc.obtainRelic(RelicId::POTION_BELT);
        require(gc.potionCapacity == 5, "Potion Belt did not add its two slots");
    }
    gc.obtainRelic(RelicId::SOZU);
    if (sacredBark) {
        gc.relics.add({RelicId::SACRED_BARK, 0});
    }

    gc.potions[0] = Potion::ENTROPIC_BREW;
    gc.potions[1] = Potion::FIRE_POTION;
    gc.potions[2] = Potion::BLOCK_POTION;
    if (initialCapacity == 5) {
        gc.potions[4] = Potion::SPEED_POTION;
    }
    gc.potionCount = initialCapacity == 5 ? 4 : 3;

    auto expectedPotions = gc.potions;
    expectedPotions[0] = Potion::EMPTY_POTION_SLOT;
    const int expectedPotionCount = gc.potionCount - 1;

    if (inBattle) {
        auto bc = battleFixture(gc, MonsterEncounter::CULTIST);
        const search::Action useBrew(search::ActionType::POTION, 0);
        require(useBrew.isValidAction(bc), "Sozu Entropic Brew was not legal");
        useBrew.execute(bc);
        require(bc.potionCount == expectedPotionCount && bc.potions == expectedPotions,
                "battle Sozu Entropic Brew did not consume only itself and preserve held slots");
    } else {
        gc.drinkPotionAtIdx(0);
        require(gc.potionCount == expectedPotionCount && gc.potions == expectedPotions,
                "noncombat Sozu Entropic Brew gained potions or changed held slots");
    }
}

int countPotionActions(const search::BattleScumSearcher2::Node &node) {
    int count = 0;
    for (const auto &edge : node.edges) {
        if (edge.action.getActionType() == search::ActionType::POTION) {
            ++count;
        }
    }
    return count;
}

search::Action potionActionForSlot(
        const search::BattleScumSearcher2::Node &node,
        const int slot) {
    for (const auto &edge : node.edges) {
        if (edge.action.getActionType() == search::ActionType::POTION
                && edge.action.getSourceIdx() == slot) {
            return edge.action;
        }
    }
    throw std::logic_error("search action list omitted the requested potion slot");
}

bool sameSearchTree(
        const search::BattleScumSearcher2::Node &left,
        const search::BattleScumSearcher2::Node &right) {
    if (left.simulationCount != right.simulationCount
            || left.evaluationSum != right.evaluationSum
            || left.edges.size() != right.edges.size()
            || left.policyPriors != right.policyPriors) {
        return false;
    }
    for (std::size_t index = 0; index < left.edges.size(); ++index) {
        if (left.edges[index].action.bits != right.edges[index].action.bits
                || !sameSearchTree(left.edges[index].node, right.edges[index].node)) {
            return false;
        }
    }
    return true;
}

void requireNoPotionActionFromSlot(
        const search::BattleScumSearcher2::Node &node,
        const int slot) {
    for (const auto &edge : node.edges) {
        require(edge.action.getActionType() != search::ActionType::POTION
                        || edge.action.getSourceIdx() != slot,
                "protected potion slot was enumerated in a tree node");
        requireNoPotionActionFromSlot(edge.node, slot);
    }
}

int countNewEntropicPotions(
        const BattleContext &beforeBrew,
        const BattleContext &afterBrew) {
    const int afterConsumingBrew = beforeBrew.potionCount - 1;
    return afterBrew.potionCount - afterConsumingBrew;
}

void checkForbiddenPotionSlotSearch() {
    auto gc = gameFixture();
    gc.potions[0] = Potion::FIRE_POTION;
    gc.potions[1] = Potion::BLOCK_POTION;
    gc.potionCount = 2;
    const auto bc = battleFixture(gc, MonsterEncounter::CULTIST);

    search::BattleScumSearcher2 baseline(bc);
    baseline.includePotions = true;
    baseline.search(400);

    search::BattleScumSearcher2 explicitDefault(bc);
    explicitDefault.includePotions = true;
    explicitDefault.setForbiddenPotionSlot(-1);
    explicitDefault.search(400);
    require(sameSearchTree(baseline.root, explicitDefault.root)
                    && baseline.bestActionValue == explicitDefault.bestActionValue
                    && baseline.minActionValue == explicitDefault.minActionValue
                    && baseline.actionExecutionCount == explicitDefault.actionExecutionCount
                    && baseline.expandedNodeCount == explicitDefault.expandedNodeCount,
            "disabled forbidden-slot configuration changed exact Search-v2 results");

    search::BattleScumSearcher2 constrained(bc);
    constrained.includePotions = true;
    constrained.setForbiddenPotionSlot(0);
    constrained.search(400);
    require(constrained.getForbiddenPotionSlot() == 0,
            "Search-v2 lost the requested protected potion slot");
    require(constrained.expandedNodeCount > 1 && constrained.rolloutCount > 0,
            "protected-slot witness did not reach deeper tree nodes and rollouts");
    require(constrained.treeForbiddenPotionSlotExclusionCount > 0
                    && constrained.rolloutForbiddenPotionSlotExclusionCount > 0,
            "protected slot was not excluded in both tree expansion and random rollout");
    requireNoPotionActionFromSlot(constrained.root, 0);

    bool foundOtherPotion = false;
    bool foundOtherPotionVisited = false;
    for (const auto &edge : constrained.root.edges) {
        if (edge.action.getActionType() != search::ActionType::POTION) {
            continue;
        }
        require(edge.action.getSourceIdx() != 0,
                "protected potion action appeared at the Search-v2 root");
        if (edge.action.getSourceIdx() == 1) {
            foundOtherPotion = true;
            foundOtherPotionVisited = edge.node.simulationCount > 0;
        }
    }
    require(foundOtherPotion && foundOtherPotionVisited,
            "an unprotected potion action was absent or unvisited at the root");

    const search::Action protectedAction(search::ActionType::POTION, 0);
    const search::Action otherAction(search::ActionType::POTION, 1);
    require(protectedAction.isValidAction(bc) && otherAction.isValidAction(bc),
            "Search-only restriction changed normal simulator action legality");

    auto changedSlot = bc;
    changedSlot.potions[0] = Potion::BOTTLED_MIRACLE;
    search::BattleScumSearcher2::Node staleStateActions;
    bool failedClosedWithRepro = false;
    try {
        constrained.enumerateActionsForNode(staleStateActions, changedSlot, false);
    } catch (const std::runtime_error &error) {
        const std::string message = error.what();
        failedClosedWithRepro = message.find("NATIVE_SLOT_CONSTRAINT_BLOCKED") != std::string::npos
                && message.find("seed=52026") != std::string::npos
                && message.find("protected_slot=0") != std::string::npos
                && message.find("inventory=[") != std::string::npos;
    }
    require(failedClosedWithRepro,
            "a changed protected slot did not fail closed with its concrete state");
}

void checkProtectedEntropicBrewOccupancy() {
    // Full inventory with duplicate potion types: the same slot occurrence
    // identity must remain distinguishable even though slots 0 and 1 match.
    auto fullGame = gameFixture();
    fullGame.potions[0] = Potion::FIRE_POTION;
    fullGame.potions[1] = Potion::FIRE_POTION;
    fullGame.potions[2] = Potion::ENTROPIC_BREW;
    fullGame.potionCount = 3;
    const auto fullRoot = battleFixture(fullGame, MonsterEncounter::CULTIST);

    search::BattleScumSearcher2 fullSearch(fullRoot);
    fullSearch.includePotions = true;
    fullSearch.setForbiddenPotionSlot(0);
    fullSearch.enumerateActionsForNode(fullSearch.root, fullRoot, false);
    require(fullRoot.potions[0] == fullRoot.potions[1],
            "duplicate-potion fixture did not contain identical types in separate slots");
    require(countPotionActions(fullSearch.root) == 2
                    && !std::any_of(fullSearch.root.edges.begin(), fullSearch.root.edges.end(),
                            [](const auto &edge) {
                                return edge.action.getActionType() == search::ActionType::POTION
                                        && edge.action.getSourceIdx() == 0;
                            }),
            "protected duplicate slot was not distinguished from the usable duplicate");
    const auto unprotectedDuplicate = potionActionForSlot(fullSearch.root, 1);
    const auto protectedEntropic = potionActionForSlot(fullSearch.root, 2);
    require(unprotectedDuplicate.isValidAction(fullRoot)
                    && protectedEntropic.isValidAction(fullRoot),
            "an allowed duplicate or Entropic Brew action lost normal legality");

    auto scenarioA = fullRoot;
    const search::Action consumeProtected(search::ActionType::POTION, 0);
    consumeProtected.execute(scenarioA);
    require(scenarioA.potions[0] == Potion::EMPTY_POTION_SLOT,
            "A-side setup did not naturally empty the protected slot");
    const auto scenarioABeforeBrew = scenarioA;
    const search::Action scenarioABrew(search::ActionType::POTION, 2);
    scenarioABrew.execute(scenarioA);

    auto scenarioB = fullRoot;
    protectedEntropic.execute(scenarioB);
    require(scenarioB.potions[0] == Potion::FIRE_POTION
                    && scenarioB.potions[1] == Potion::FIRE_POTION,
            "B-side Entropic Brew changed either occupied protected/duplicate slot");
    require(scenarioB.potions[2] != Potion::ENTROPIC_BREW,
            "B-side Entropic Brew did not replace its own naturally empty slot");
    const int generatedA = countNewEntropicPotions(scenarioABeforeBrew, scenarioA);
    const int generatedB = countNewEntropicPotions(fullRoot, scenarioB);
    require(generatedA == 2 && generatedB == 1,
            "full-inventory A/B Entropic Brew did not preserve occupancy-dependent generation");
    require(scenarioA.potions[0] != Potion::EMPTY_POTION_SLOT
                    && scenarioA.potions[1] == Potion::FIRE_POTION
                    && scenarioA.potions[2] != Potion::ENTROPIC_BREW,
            "A-side Entropic Brew did not fill the newly empty protected-slot index naturally");

    // The partially empty case verifies that the protected potion stays put
    // while Entropic Brew fills only the other naturally empty slots.
    auto partialGame = gameFixture();
    partialGame.potions[0] = Potion::BLOCK_POTION;
    partialGame.potions[1] = Potion::ENTROPIC_BREW;
    partialGame.potionCount = 2;
    const auto partialRoot = battleFixture(partialGame, MonsterEncounter::CULTIST);
    search::BattleScumSearcher2 partialSearch(partialRoot);
    partialSearch.includePotions = true;
    partialSearch.setForbiddenPotionSlot(0);
    partialSearch.enumerateActionsForNode(partialSearch.root, partialRoot, false);
    const auto partialAllowedBrew = potionActionForSlot(partialSearch.root, 1);
    auto partialB = partialRoot;
    partialAllowedBrew.execute(partialB);
    require(partialB.potions[0] == Potion::BLOCK_POTION
                    && partialB.potions[1] != Potion::ENTROPIC_BREW
                    && partialB.potions[2] != Potion::EMPTY_POTION_SLOT
                    && partialB.potionCount == partialB.potionCapacity,
            "partial-inventory Entropic Brew failed to preserve slot 0 and fill other empty slots");

    auto partialA = partialRoot;
    const search::Action consumeProtectedBlock(search::ActionType::POTION, 0);
    consumeProtectedBlock.execute(partialA);
    const search::Action partialABrew(search::ActionType::POTION, 1);
    partialABrew.execute(partialA);
    require(partialA.potions[0] != Potion::EMPTY_POTION_SLOT
                    && partialA.potions[1] != Potion::ENTROPIC_BREW
                    && partialA.potions[2] != Potion::EMPTY_POTION_SLOT
                    && partialA.potionCount == partialA.potionCapacity,
            "partial-inventory A-side Entropic Brew did not fill the newly empty slot naturally");
}

void checkSmokeBombLegalityAndSearch() {
    auto gc = gameFixture();
    gc.potions[0] = Potion::SMOKE_BOMB;
    gc.potions[1] = Potion::SMOKE_BOMB;
    gc.potionCount = 2;
    auto bc = battleFixture(gc, MonsterEncounter::CULTIST);
    require(bc.canUseSmokeBomb(), "normal encounter rejected Smoke Bomb");

    search::BattleScumSearcher2 excluded(bc);
    excluded.includePotions = false;
    excluded.enumerateActionsForNode(excluded.root, bc);
    require(excluded.expandedNodeCount == 1 && countPotionActions(excluded.root) == 0,
            "include_potions=false exposed potion search actions or bad telemetry");

    search::BattleScumSearcher2 enabled(bc);
    enabled.includePotions = true;
    enabled.enumerateActionsForNode(enabled.root, bc);
    require(enabled.expandedNodeCount == 1 && countPotionActions(enabled.root) == 2,
            "include_potions=true did not expose one search action per occupied slot");
    const auto selected = potionActionForSlot(enabled.root, 1);
    require(selected.isValidAction(bc), "selected Smoke Bomb search occurrence was not legal");

    auto execution = bc;
    selected.execute(execution);
    require(execution.outcome == Outcome::PLAYER_ESCAPE,
            "Smoke Bomb did not produce a distinct terminal escape outcome");
    require(execution.potionCount == 1
                    && execution.potions[0] == Potion::SMOKE_BOMB
                    && execution.potions[1] == Potion::EMPTY_POTION_SLOT,
            "selected duplicate Smoke Bomb action consumed the wrong potion slot");
    auto victory = execution;
    victory.outcome = Outcome::PLAYER_VICTORY;
    require(search::BattleScumSearcher2::evaluateEndState(victory)
                    > search::BattleScumSearcher2::evaluateEndState(execution),
            "escape was evaluated as a phantom terminal victory");

    auto eliteGame = gameFixture();
    eliteGame.potions[0] = Potion::SMOKE_BOMB;
    eliteGame.potionCount = 1;
    auto elite = battleFixture(eliteGame, MonsterEncounter::GREMLIN_NOB, Room::ELITE);
    require(elite.canUseSmokeBomb()
                    && search::Action(search::ActionType::POTION, 0).isValidAction(elite),
            "legal non-boss elite Smoke Bomb was rejected");

    auto bossGame = gameFixture();
    bossGame.potions[0] = Potion::SMOKE_BOMB;
    bossGame.potionCount = 1;
    auto boss = battleFixture(bossGame, MonsterEncounter::SLIME_BOSS, Room::BOSS);
    require(!boss.canUseSmokeBomb()
                    && !search::Action(search::ActionType::POTION, 0).isValidAction(boss),
            "Boss encounter allowed Smoke Bomb use");
    require(search::Action(search::ActionType::POTION, 0, 6).isValidAction(boss),
            "Boss encounter incorrectly prevented discarding Smoke Bomb");
    boss.drinkPotion(0);
    require(boss.potionCount == 1 && boss.potions[0] == Potion::SMOKE_BOMB,
            "direct invalid Boss use consumed Smoke Bomb");

    auto shieldGame = gameFixture();
    shieldGame.potions[0] = Potion::SMOKE_BOMB;
    shieldGame.potionCount = 1;
    auto shield = battleFixture(shieldGame, MonsterEncounter::SHIELD_AND_SPEAR, Room::ELITE);
    require(shield.monsters.monstersAlive == 2 && !shield.canUseSmokeBomb(),
            "Smoke Bomb was legal while both Act 4 elite enemies surrounded the player");
    shield.monsters.monstersAlive = 1;
    require(shield.canUseSmokeBomb()
                    && search::Action(search::ActionType::POTION, 0).isValidAction(shield),
            "Smoke Bomb remained blocked after one Shield/Spear enemy was defeated");

    for (const auto encounter : {
            MonsterEncounter::COLOSSEUM_EVENT_SLAVERS,
            MonsterEncounter::COLOSSEUM_EVENT_NOBS}) {
        auto colosseumGame = gameFixture();
        colosseumGame.potions[0] = Potion::SMOKE_BOMB;
        colosseumGame.potionCount = 1;
        auto colosseum = battleFixture(colosseumGame, encounter, Room::EVENT);
        require(colosseum.canUseSmokeBomb()
                        && search::Action(search::ActionType::POTION, 0).isValidAction(colosseum),
                "Colosseum encounter did not allow its real Smoke Bomb escape");
        require(search::Action(search::ActionType::POTION, 0, 6).isValidAction(colosseum),
                "Colosseum encounter incorrectly prevented potion discard");
        colosseum.drinkPotion(0);
        require(colosseum.outcome == Outcome::PLAYER_ESCAPE
                        && colosseum.potionCount == 0,
                "Colosseum Smoke Bomb use did not consume its potion and produce escape");
    }
}

void checkSmokeBombExitSkipsRewards() {
    auto gc = gameFixture();
    gc.curHp = 30;
    gc.maxHp = 60;
    gc.gold = 99;
    gc.curRoom = Room::EVENT;
    gc.curEvent = Event::MYSTERIOUS_SPHERE;
    gc.info.encounter = MonsterEncounter::MYSTERIOUS_SPHERE_EVENT;
    gc.info.gold = 45;
    gc.info.bossRelics[0] = RelicId::ANCHOR;
    gc.info.stolenGold = 13;
    gc.relics.remove(RelicId::BURNING_BLOOD);
    gc.potions[0] = Potion::SMOKE_BOMB;
    gc.potionCount = 1;
    gc.regainControlAction = [](GameContext &next) { next.afterBattle(); };
    const int startingGold = gc.gold;
    const int startingMaxHp = gc.maxHp;
    const int startingRelicCount = gc.relics.size();
    const int startingDeckSize = gc.deck.size();

    auto bc = battleFixture(gc, MonsterEncounter::MYSTERIOUS_SPHERE_EVENT, Room::EVENT);
    bc.player.curHp = gc.curHp;
    bc.player.maxHp = gc.maxHp;
    bc.drinkPotion(0);
    require(bc.outcome == Outcome::PLAYER_ESCAPE,
            "event Smoke Bomb did not reach terminal escape");
    bc.exitBattle(gc);

    require(gc.screenState == ScreenState::MAP_SCREEN
                    && gc.regainControlAction == nullptr
                    && gc.outcome == GameOutcome::UNDECIDED,
            "Smoke Bomb did not return to the map and preserve run continuation");
    require(gc.gold == startingGold && gc.info.stolenGold == 0
                    && gc.info.rewardsContainer.goldRewardCount == 0
                    && gc.info.rewardsContainer.relicCount == 0
                    && gc.info.rewardsContainer.potionCount == 0
                    && gc.info.rewardsContainer.cardRewardCount == 0,
            "event escape granted or retained battle reward loot");
    require(gc.maxHp == startingMaxHp && gc.curHp == 30
                    && gc.relics.size() == startingRelicCount
                    && gc.deck.size() == startingDeckSize
                    && gc.potionCount == 0,
            "event escape applied a victory reward, altered the run, or failed to consume potion");
}

void checkColosseumSmokeBombTransitions() {
    for (const auto encounter : {
            MonsterEncounter::COLOSSEUM_EVENT_SLAVERS,
            MonsterEncounter::COLOSSEUM_EVENT_NOBS}) {
        auto gc = gameFixture();
        gc.curRoom = Room::EVENT;
        gc.curEvent = Event::COLOSSEUM;
        gc.info.eventData = encounter == MonsterEncounter::COLOSSEUM_EVENT_SLAVERS ? 0 : 2;
        gc.info.rewardsContainer.clear();
        gc.potions[0] = Potion::SMOKE_BOMB;
        gc.potionCount = 1;
        gc.gold = 99;
        gc.potionChance = 100; // guarantee the hidden first-fight roll creates a discarded potion
        gc.regainControlAction = [](GameContext &next) { next.afterBattle(); };
        const int startingGold = gc.gold;
        const int startingDeckSize = gc.deck.size();
        const int startingRelicCount = gc.relics.size();
        const int startingPotionChance = gc.potionChance;
        const int startingPotionRngCounter = gc.potionRng.counter;
        GameContext expectedHiddenRoll = gc;
        Rewards expectedHiddenPotion;
        expectedHiddenRoll.addPotionRewards(expectedHiddenPotion);
        require(expectedHiddenPotion.potionCount == 1,
                "first Colosseum hidden-roll fixture did not guarantee a potion drop");

        auto bc = battleFixture(gc, encounter, Room::EVENT);
        bc.drinkPotion(0);
        require(bc.outcome == Outcome::PLAYER_ESCAPE,
                "Colosseum Smoke Bomb did not reach terminal escape");
        bc.exitBattle(gc);
        if (encounter == MonsterEncounter::COLOSSEUM_EVENT_SLAVERS) {
            require(gc.screenState == ScreenState::EVENT_SCREEN
                            && gc.info.eventData == 1
                            && gc.regainControlAction != nullptr
                            && gc.outcome == GameOutcome::UNDECIDED,
                    "first-fight Smoke Bomb did not continue to the Colosseum choice");
            require(gc.potionChance == expectedHiddenRoll.potionChance
                            && gc.potionRng.counter == expectedHiddenRoll.potionRng.counter,
                    "first-fight Smoke Bomb skipped or changed the hidden potion roll");
            requireNextPotionRewardMatches(
                    gc, expectedHiddenRoll, "first-fight Smoke Bomb");
        } else {
            require(gc.screenState == ScreenState::MAP_SCREEN
                            && gc.regainControlAction == nullptr
                            && gc.outcome == GameOutcome::UNDECIDED,
                    "second-fight Smoke Bomb did not abandon Colosseum and resume at the map");
            require(gc.potionChance == startingPotionChance
                            && gc.potionRng.counter == startingPotionRngCounter,
                    "second-fight Smoke Bomb incorrectly consumed a first-fight potion roll");
        }
        require(gc.gold == startingGold && gc.deck.size() == startingDeckSize
                        && gc.relics.size() == startingRelicCount
                        && gc.info.rewardsContainer.getTotalCount() == 0
                        && gc.potionCount == 0,
                "Colosseum escape granted victory loot or failed to consume Smoke Bomb");
    }
}

void checkColosseumFirstFightVictoryHiddenPotionRoll() {
    auto gc = gameFixture();
    gc.curRoom = Room::EVENT;
    gc.curEvent = Event::COLOSSEUM;
    gc.info.eventData = 0;
    gc.info.rewardsContainer.clear();
    gc.potionChance = 100; // guarantee the hidden roll produces, but does not show, a potion
    gc.regainControlAction = [](GameContext &next) { next.afterBattle(); };

    GameContext expectedHiddenRoll = gc;
    Rewards expectedHiddenPotion;
    expectedHiddenRoll.addPotionRewards(expectedHiddenPotion);
    require(expectedHiddenPotion.potionCount == 1,
            "first Colosseum victory fixture did not guarantee a hidden potion drop");

    auto bc = battleFixture(
            gc, MonsterEncounter::COLOSSEUM_EVENT_SLAVERS, Room::EVENT);
    bc.outcome = Outcome::PLAYER_VICTORY;
    bc.exitBattle(gc);

    require(gc.screenState == ScreenState::EVENT_SCREEN && gc.info.eventData == 1,
            "first Colosseum victory did not continue to its leave/challenge choice");
    require(gc.info.rewardsContainer.getTotalCount() == 0,
            "first Colosseum victory exposed ordinary or hidden potion rewards");
    require(gc.potionChance == expectedHiddenRoll.potionChance
                    && gc.potionRng.counter == expectedHiddenRoll.potionRng.counter,
            "first Colosseum victory skipped or changed the hidden potion roll");
    requireNextPotionRewardMatches(gc, expectedHiddenRoll, "first-fight victory");
}

void checkColosseumSecondFightEliteStartRelics() {
    const auto encounter = MonsterEncounter::COLOSSEUM_EVENT_NOBS;
    auto makeColosseumContext = [](MonsterEncounter encounter, int phase, RelicId relic) {
        auto gc = gameFixture();
        gc.curRoom = Room::EVENT;
        gc.curEvent = Event::COLOSSEUM;
        gc.info.eventData = phase;
        gc.info.encounter = encounter;
        if (relic != RelicId::INVALID) {
            gc.relics.add({relic, 0});
        }
        return gc;
    };

    auto baseline = makeColosseumContext(encounter, 2, RelicId::INVALID);
    auto baselineBattle = battleFixture(baseline, encounter, Room::EVENT);

    auto preservedInsect = makeColosseumContext(encounter, 2, RelicId::PRESERVED_INSECT);
    auto preservedInsectBattle = battleFixture(preservedInsect, encounter, Room::EVENT);
    for (int i = 0; i < preservedInsectBattle.monsters.monsterCount; ++i) {
        const auto &monster = preservedInsectBattle.monsters.arr[i];
        require(monster.curHp == static_cast<int>(monster.maxHp * .75),
                "second Colosseum fight did not apply Preserved Insect to every enemy");
    }

    auto slaversCollar = makeColosseumContext(encounter, 2, RelicId::SLAVERS_COLLAR);
    auto slaversCollarBattle = battleFixture(slaversCollar, encounter, Room::EVENT);
    require(slaversCollarBattle.player.energyPerTurn
                    == baselineBattle.player.energyPerTurn + 1,
            "second Colosseum fight did not apply Slaver's Collar elite energy");

    auto slingOfCourage = makeColosseumContext(encounter, 2, RelicId::SLING_OF_COURAGE);
    auto slingOfCourageBattle = battleFixture(slingOfCourage, encounter, Room::EVENT);
    require(slingOfCourageBattle.player.strength == baselineBattle.player.strength + 2,
            "second Colosseum fight did not apply Sling of Courage elite strength");

    const auto firstEncounter = MonsterEncounter::COLOSSEUM_EVENT_SLAVERS;
    auto firstFightBaseline = makeColosseumContext(firstEncounter, 0, RelicId::INVALID);
    auto firstFightBaselineBattle = battleFixture(
            firstFightBaseline, firstEncounter, Room::EVENT);
    auto firstFightInsect = makeColosseumContext(
            firstEncounter, 0, RelicId::PRESERVED_INSECT);
    auto firstFightInsectBattle = battleFixture(
            firstFightInsect, firstEncounter, Room::EVENT);
    for (int i = 0; i < firstFightInsectBattle.monsters.monsterCount; ++i) {
        const auto &monster = firstFightInsectBattle.monsters.arr[i];
        require(monster.curHp == monster.maxHp,
                "ordinary first Colosseum fight was incorrectly elite-classified for Preserved Insect");
    }
    auto firstFightCollar = makeColosseumContext(
            firstEncounter, 0, RelicId::SLAVERS_COLLAR);
    auto firstFightCollarBattle = battleFixture(
            firstFightCollar, firstEncounter, Room::EVENT);
    require(firstFightCollarBattle.player.energyPerTurn
                    == firstFightBaselineBattle.player.energyPerTurn,
            "ordinary first Colosseum fight was incorrectly elite-classified for Slaver's Collar");
    auto firstFightSling = makeColosseumContext(
            firstEncounter, 0, RelicId::SLING_OF_COURAGE);
    auto firstFightSlingBattle = battleFixture(
            firstFightSling, firstEncounter, Room::EVENT);
    require(firstFightSlingBattle.player.strength
                    == firstFightBaselineBattle.player.strength,
            "ordinary first Colosseum fight was incorrectly elite-classified for Sling of Courage");

    auto blackStar = makeColosseumContext(encounter, 2, RelicId::BLACK_STAR);
    blackStar.regainControlAction = [](GameContext &next) { next.afterBattle(); };
    auto blackStarBattle = battleFixture(blackStar, encounter, Room::EVENT);
    blackStarBattle.outcome = Outcome::PLAYER_VICTORY;
    blackStarBattle.exitBattle(blackStar);
    const auto &blackStarReward = blackStar.info.rewardsContainer;
    require(blackStar.screenState == ScreenState::REWARDS
                    && blackStarReward.relicCount == 2
                    && getRelicTier(blackStarReward.relics[0]) == RelicTier::UNCOMMON
                    && getRelicTier(blackStarReward.relics[1]) == RelicTier::RARE,
            "event-specific second-fight reward incorrectly received Black Star's extra relic");
}

void checkCombatEndRelic(
        const RelicId relic,
        const Outcome outcome,
        const int startingHp,
        const int startingMaxHp,
        const int expectedHp,
        const int expectedMaxHp,
        const bool markOfTheBloom = false) {
    auto gc = gameFixture();
    gc.relics.remove(RelicId::BURNING_BLOOD);
    gc.relics.add({relic, 0});
    if (markOfTheBloom) {
        gc.relics.add({RelicId::MARK_OF_THE_BLOOM, 0});
    }
    gc.curHp = startingHp;
    gc.maxHp = startingMaxHp;
    gc.gold = 99;
    gc.info.stolenGold = 13;
    if (outcome == Outcome::PLAYER_ESCAPE) {
        gc.potions[0] = Potion::SMOKE_BOMB;
        gc.potionCount = 1;
    }
    gc.regainControlAction = [](GameContext &next) { next.afterBattle(); };

    auto bc = battleFixture(gc, MonsterEncounter::CULTIST);
    bc.player.curHp = startingHp;
    bc.player.maxHp = startingMaxHp;
    if (outcome == Outcome::PLAYER_ESCAPE) {
        bc.drinkPotion(0);
        require(bc.outcome == Outcome::PLAYER_ESCAPE,
                "Smoke Bomb did not set the escape result for combat-end relic test");
    } else {
        bc.outcome = outcome;
    }
    bc.exitBattle(gc);

    require(gc.curHp == expectedHp && gc.maxHp == expectedMaxHp,
            "combat-end relic health/max-HP hook was missing, doubled, or misapplied");
    require(gc.outcome == GameOutcome::UNDECIDED,
            "combat-end relic hook falsely completed or lost the run");
    if (outcome == Outcome::PLAYER_ESCAPE) {
        require(gc.screenState == ScreenState::MAP_SCREEN
                        && gc.gold == 99 && gc.info.stolenGold == 0
                        && gc.info.rewardsContainer.goldRewardCount == 0
                        && gc.info.rewardsContainer.relicCount == 0
                        && gc.info.rewardsContainer.potionCount == 0
                        && gc.info.rewardsContainer.cardRewardCount == 0,
                "escape relic hook granted rewards or did not continue at the map");
    }
}

void checkSmokeBombCombatEndRelics() {
    struct RelicExpectation {
        RelicId relic;
        int healedHp;
        int healedMaxHp;
        int markedMaxHp;
    };
    constexpr std::array<RelicExpectation, 4> expectations = {{
            {RelicId::BURNING_BLOOD, 36, 60, 60},
            {RelicId::BLACK_BLOOD, 42, 60, 60},
            {RelicId::MEAT_ON_THE_BONE, 42, 60, 60},
            {RelicId::FACE_OF_CLERIC, 31, 61, 61},
    }};

    for (const auto &expected : expectations) {
        for (const auto outcome : {Outcome::PLAYER_ESCAPE, Outcome::PLAYER_VICTORY}) {
            checkCombatEndRelic(expected.relic, outcome, 30, 60,
                    expected.healedHp, expected.healedMaxHp);
            checkCombatEndRelic(expected.relic, outcome,
                    30, 60, 30, expected.markedMaxHp, true);
            const int cappedHp = expected.relic == RelicId::FACE_OF_CLERIC ? 61 : 60;
            const int cappedMaxHp = expected.relic == RelicId::FACE_OF_CLERIC ? 61 : 60;
            checkCombatEndRelic(expected.relic, outcome,
                    60, 60, cappedHp, cappedMaxHp);
        }
    }
    for (const auto outcome : {Outcome::PLAYER_ESCAPE, Outcome::PLAYER_VICTORY}) {
        checkCombatEndRelic(RelicId::MEAT_ON_THE_BONE, outcome,
                31, 60, 31, 60);
    }
}

void checkMeatOnTheBoneOrdering(
        const RelicId bloodRelic,
        const bool meatFirst,
        const Outcome outcome,
        const int startingHp,
        const int maxHp,
        const bool markOfTheBloom = false) {
    auto gc = gameFixture();
    gc.relics.remove(RelicId::BURNING_BLOOD);
    if (meatFirst) {
        gc.relics.add({RelicId::MEAT_ON_THE_BONE, 0});
        gc.relics.add({bloodRelic, 0});
    } else {
        gc.relics.add({bloodRelic, 0});
        gc.relics.add({RelicId::MEAT_ON_THE_BONE, 0});
    }
    if (markOfTheBloom) {
        gc.relics.add({RelicId::MARK_OF_THE_BLOOM, 0});
    }
    gc.curHp = startingHp;
    gc.maxHp = maxHp;
    gc.regainControlAction = [](GameContext &next) { next.afterBattle(); };
    if (outcome == Outcome::PLAYER_ESCAPE) {
        gc.potions[0] = Potion::SMOKE_BOMB;
        gc.potionCount = 1;
    }

    auto bc = battleFixture(gc, MonsterEncounter::CULTIST);
    bc.player.curHp = startingHp;
    bc.player.maxHp = maxHp;
    if (outcome == Outcome::PLAYER_ESCAPE) {
        bc.drinkPotion(0);
        require(bc.outcome == Outcome::PLAYER_ESCAPE,
                "Smoke Bomb did not set escape for combined relic-order test");
    } else {
        bc.outcome = outcome;
    }
    bc.exitBattle(gc);

    const int bloodHeal = bloodRelic == RelicId::BLACK_BLOOD ? 12 : 6;
    const int meatHeal = startingHp <= maxHp / 2 ? 12 : 0;
    const int expectedHp = markOfTheBloom
            ? startingHp
            : std::min(maxHp, startingHp + meatHeal + bloodHeal);
    require(gc.curHp == expectedHp,
            "Meat on the Bone eligibility depended on blood relic order or healed more than once");
}

void checkFaceOfClericEventVictory(const Event event, const int eventOption,
        const MonsterEncounter encounter) {
    auto gc = gameFixture();
    gc.curHp = 30;
    gc.maxHp = 60;
    gc.curRoom = Room::EVENT;
    gc.curEvent = event;
    gc.relics.remove(RelicId::BURNING_BLOOD);
    gc.relics.add({RelicId::FACE_OF_CLERIC, 0});
    gc.setupEvent();
    gc.chooseEventOption(eventOption);
    require(gc.screenState == ScreenState::BATTLE
                    && gc.info.encounter == encounter,
            "event option did not enter its expected native encounter");

    auto bc = battleFixture(gc, encounter, Room::EVENT);
    bc.player.curHp = gc.curHp;
    bc.player.maxHp = gc.maxHp;
    bc.outcome = Outcome::PLAYER_VICTORY;
    bc.exitBattle(gc);

    require(gc.curHp == 31 && gc.maxHp == 61,
            "Face of Cleric was missed or applied more than once on an event victory");
    require(gc.screenState == ScreenState::REWARDS,
            "event victory no longer reached its event-specific reward callback");
}

void checkCombinedCombatEndRelicOrdering() {
    for (const auto bloodRelic : {RelicId::BURNING_BLOOD, RelicId::BLACK_BLOOD}) {
        for (const auto outcome : {Outcome::PLAYER_ESCAPE, Outcome::PLAYER_VICTORY}) {
            for (const bool meatFirst : {false, true}) {
                checkMeatOnTheBoneOrdering(bloodRelic, meatFirst, outcome, 30, 60);
                checkMeatOnTheBoneOrdering(bloodRelic, meatFirst, outcome, 31, 60);
                checkMeatOnTheBoneOrdering(bloodRelic, meatFirst, outcome, 15, 30);
                checkMeatOnTheBoneOrdering(bloodRelic, meatFirst, outcome, 30, 60, true);
            }
        }
    }
}

void checkSozuPotionRefusal() {
    auto gc = gameFixture();
    gc.obtainRelic(RelicId::SOZU);
    gc.obtainPotion(Potion::FIRE_POTION);
    require(gc.potionCount == 0 && gc.potions[0] == Potion::EMPTY_POTION_SLOT,
            "Sozu no longer blocks a standalone potion acquisition");
}

} // namespace

int main() {
    try {
        for (bool sacredBark : {false, true}) {
            for (bool capped : {false, true}) {
                checkBloodPotion(true, sacredBark, capped);
                checkBloodPotion(false, sacredBark, capped);
            }
            checkFruitJuice(sacredBark);
            checkEntropicBrew(3, sacredBark);
        }
        checkEntropicBrew(5, false);
        checkSmokeBombLegalityAndSearch();
        checkForbiddenPotionSlotSearch();
        checkProtectedEntropicBrewOccupancy();
        checkSmokeBombExitSkipsRewards();
        checkColosseumSmokeBombTransitions();
        checkColosseumFirstFightVictoryHiddenPotionRoll();
        checkColosseumSecondFightEliteStartRelics();
        checkSmokeBombCombatEndRelics();
        checkCombinedCombatEndRelicOrdering();
        checkFaceOfClericEventVictory(Event::MASKED_BANDITS, 1,
                MonsterEncounter::MASKED_BANDITS_EVENT);
        checkFaceOfClericEventVictory(Event::HYPNOTIZING_COLORED_MUSHROOMS, 0,
                MonsterEncounter::MUSHROOMS_EVENT);
        for (const int capacity : {3, 5}) {
            for (const bool sacredBark : {false, true}) {
                checkSozuEntropicBrew(false, capacity, sacredBark);
                checkSozuEntropicBrew(true, capacity, sacredBark);
            }
        }
        checkSozuPotionRefusal();
        std::cout << "native potion semantics regression passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
