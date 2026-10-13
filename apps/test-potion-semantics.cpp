#include <algorithm>
#include <array>
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
        require(!colosseum.canUseSmokeBomb()
                        && !search::Action(search::ActionType::POTION, 0).isValidAction(colosseum),
                "unsupported Colosseum encounter allowed Smoke Bomb");
        require(search::Action(search::ActionType::POTION, 0, 6).isValidAction(colosseum),
                "unsupported Colosseum encounter incorrectly prevented potion discard");
        colosseum.drinkPotion(0);
        require(colosseum.potionCount == 1
                        && colosseum.potions[0] == Potion::SMOKE_BOMB,
                "direct invalid Colosseum Smoke Bomb use consumed the potion");
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
        checkSmokeBombExitSkipsRewards();
        checkSmokeBombCombatEndRelics();
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
