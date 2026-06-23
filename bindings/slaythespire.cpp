//
// Created by keega on 9/16/2021.
//

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl_bind.h>
#include <pybind11/functional.h>

#include <sstream>
#include <algorithm>
#include <stdexcept>

#include "sim/ConsoleSimulator.h"
#include "sim/search/ScumSearchAgent2.h"
#include "sim/SimHelpers.h"
#include "sim/PrintHelpers.h"
#include "combat/BattleContext.h"
#include "combat/InputState.h"
#include "sim/search/Action.h"
#include "sim/search/GameAction.h"
#include "game/GameContext.h"
#include "game/Game.h"

#include "slaythespire.h"


using namespace sts;



namespace {

struct LightSpeedAction {
    std::string scope;
    std::uint32_t bits = 0;
    std::string kind;
    int idx1 = 0;
    int idx2 = 0;
    int idx3 = 0;
    std::string label;
};

std::string gameOutcomeLabel(const GameOutcome outcome) {
    switch (outcome) {
        case GameOutcome::PLAYER_LOSS:
            return "PLAYER_LOSS";
        case GameOutcome::UNDECIDED:
            return "UNDECIDED";
        case GameOutcome::PLAYER_VICTORY:
            return "PLAYER_VICTORY";
        default:
            return "UNKNOWN";
    }
}

std::string battleOutcomeLabel(const Outcome outcome) {
    switch (outcome) {
        case Outcome::UNDECIDED:
            return "UNDECIDED";
        case Outcome::PLAYER_VICTORY:
            return "PLAYER_VICTORY";
        case Outcome::PLAYER_LOSS:
            return "PLAYER_LOSS";
        default:
            return "UNKNOWN";
    }
}

std::string screenStateLabel(const ScreenState screenState) {
    switch (screenState) {
        case ScreenState::INVALID:
            return "INVALID";
        case ScreenState::EVENT_SCREEN:
            return "EVENT_SCREEN";
        case ScreenState::REWARDS:
            return "REWARDS";
        case ScreenState::BOSS_RELIC_REWARDS:
            return "BOSS_RELIC_REWARDS";
        case ScreenState::CARD_SELECT:
            return "CARD_SELECT";
        case ScreenState::MAP_SCREEN:
            return "MAP_SCREEN";
        case ScreenState::TREASURE_ROOM:
            return "TREASURE_ROOM";
        case ScreenState::REST_ROOM:
            return "REST_ROOM";
        case ScreenState::SHOP_ROOM:
            return "SHOP_ROOM";
        case ScreenState::BATTLE:
            return "BATTLE";
        default:
            return "UNKNOWN";
    }
}

std::string inputStateLabel(const InputState inputState) {
    switch (inputState) {
        case InputState::EXECUTING_ACTIONS:
            return "EXECUTING_ACTIONS";
        case InputState::PLAYER_NORMAL:
            return "PLAYER_NORMAL";
        case InputState::CARD_SELECT:
            return "CARD_SELECT";
        default:
            return "OTHER";
    }
}

std::string rewardActionLabel(search::GameAction::RewardsActionType type) {
    switch (type) {
        case search::GameAction::RewardsActionType::CARD:
            return "reward_card";
        case search::GameAction::RewardsActionType::GOLD:
            return "reward_gold";
        case search::GameAction::RewardsActionType::KEY:
            return "reward_key";
        case search::GameAction::RewardsActionType::POTION:
            return "reward_potion";
        case search::GameAction::RewardsActionType::RELIC:
            return "reward_relic";
        case search::GameAction::RewardsActionType::CARD_REMOVE:
            return "card_remove";
        case search::GameAction::RewardsActionType::SKIP:
            return "skip";
        default:
            return "reward_unknown";
    }
}

std::string gameActionKind(const GameContext &gc, const search::GameAction &action) {
    if (action.isPotionAction()) {
        return action.isPotionDiscard() ? "game_potion_discard" : "game_potion_use";
    }

    switch (gc.screenState) {
        case ScreenState::EVENT_SCREEN:
            return "event";
        case ScreenState::REWARDS:
            return rewardActionLabel(action.getRewardsActionType());
        case ScreenState::BOSS_RELIC_REWARDS:
            return "boss_relic";
        case ScreenState::CARD_SELECT:
            return "card_select";
        case ScreenState::MAP_SCREEN:
            return "map";
        case ScreenState::TREASURE_ROOM:
            return action.getIdx1() == 0 ? "treasure_open" : "treasure_leave";
        case ScreenState::REST_ROOM:
            return "rest";
        case ScreenState::SHOP_ROOM:
            return "shop_" + rewardActionLabel(action.getRewardsActionType());
        default:
            return "game_unknown";
    }
}

LightSpeedAction makeGameAction(const GameContext &gc, const search::GameAction &action) {
    auto kind = gameActionKind(gc, action);
    std::ostringstream label;
    label
        << "game." << kind
        << " bits=" << action.bits
        << " idx1=" << action.getIdx1()
        << " idx2=" << action.getIdx2()
        << " idx3=" << action.getIdx3();

    return {
        "game",
        action.bits,
        kind,
        action.getIdx1(),
        action.getIdx2(),
        action.getIdx3(),
        label.str(),
    };
}

std::string battleActionKind(const search::Action &action) {
    switch (action.getActionType()) {
        case search::ActionType::CARD:
            return "card";
        case search::ActionType::POTION:
            return action.getTargetIdx() > 5 ? "potion_discard" : "potion";
        case search::ActionType::SINGLE_CARD_SELECT:
            return "single_card_select";
        case search::ActionType::MULTI_CARD_SELECT:
            return "multi_card_select";
        case search::ActionType::END_TURN:
            return "end_turn";
        default:
            return "battle_unknown";
    }
}

LightSpeedAction makeBattleAction(const BattleContext &bc, const search::Action &action) {
    std::ostringstream label;
    action.printDesc(label, bc);
    if (label.tellp() <= 0) {
        label
            << "battle." << battleActionKind(action)
            << " bits=" << action.bits
            << " source=" << action.getSourceIdx()
            << " target=" << action.getTargetIdx();
    }

    return {
        "battle",
        action.bits,
        battleActionKind(action),
        action.getSourceIdx(),
        action.getTargetIdx(),
        action.getSelectIdx(),
        label.str(),
    };
}

std::vector<search::Action> enumerateBattleActions(const BattleContext &bc) {
    std::vector<search::Action> actions;
    if (bc.outcome != Outcome::UNDECIDED) {
        return actions;
    }

    if (bc.inputState == InputState::CARD_SELECT) {
        return search::Action::enumerateCardSelectActions(bc);
    }

    if (bc.inputState != InputState::PLAYER_NORMAL) {
        return actions;
    }

    search::Action endTurn(search::ActionType::END_TURN);
    if (endTurn.isValidAction(bc)) {
        actions.push_back(endTurn);
    }

    for (int source = 0; source < bc.cards.cardsInHand; ++source) {
        const auto &card = bc.cards.hand[source];
        if (card.requiresTarget()) {
            for (int target = 0; target < 5; ++target) {
                search::Action action(search::ActionType::CARD, source, target);
                if (action.isValidAction(bc)) {
                    actions.push_back(action);
                }
            }
        } else {
            search::Action action(search::ActionType::CARD, source, 0);
            if (action.isValidAction(bc)) {
                actions.push_back(action);
            }
        }
    }

    for (int source = 0; source < bc.potionCapacity; ++source) {
        for (int target = 0; target < 5; ++target) {
            search::Action action(search::ActionType::POTION, source, target);
            if (action.isValidAction(bc)) {
                actions.push_back(action);
            }
        }

        search::Action discard(search::ActionType::POTION, source, 6);
        if (discard.isValidAction(bc)) {
            actions.push_back(discard);
        }
    }

    return actions;
}


std::string cardTypeLabel(CardType type) {
    const auto idx = static_cast<int>(type);
    if (idx >= 0 && idx <= static_cast<int>(CardType::INVALID)) {
        return cardTypeStrings[idx];
    }
    return "UNKNOWN";
}

std::string potionLabel(Potion potion) {
    const auto idx = static_cast<int>(potion);
    if (idx >= 0 && idx <= static_cast<int>(Potion::WEAK_POTION)) {
        return potionNames[idx];
    }
    return "UNKNOWN";
}

std::string monsterIdLabel(MonsterId id) {
    const auto idx = static_cast<int>(id);
    if (idx >= 0 && idx <= static_cast<int>(MonsterId::WRITHING_MASS)) {
        return monsterIdStrings[idx];
    }
    return "UNKNOWN";
}

pybind11::dict playerSnapshot(const Player &player) {
    pybind11::dict ret;
    ret["current_hp"] = player.curHp;
    ret["max_hp"] = player.maxHp;
    ret["energy"] = player.energy;
    ret["energy_per_turn"] = player.energyPerTurn;
    ret["block"] = player.block;
    ret["strength"] = player.strength;
    ret["dexterity"] = player.dexterity;
    ret["artifact"] = player.artifact;
    ret["focus"] = player.focus;
    ret["vulnerable"] = player.getStatusRuntime(PS::VULNERABLE);
    ret["weak"] = player.getStatusRuntime(PS::WEAK);
    ret["frail"] = player.getStatusRuntime(PS::FRAIL);
    ret["cards_played_this_turn"] = player.cardsPlayedThisTurn;
    ret["attacks_played_this_turn"] = player.attacksPlayedThisTurn;
    ret["skills_played_this_turn"] = player.skillsPlayedThisTurn;
    ret["cards_discarded_this_turn"] = player.cardsDiscardedThisTurn;
    ret["times_damaged_this_combat"] = player.timesDamagedThisCombat;
    return ret;
}

pybind11::dict cardSnapshot(
        const BattleContext &bc,
        const CardInstance &card,
        int pileIdx,
        bool cardIsPlayable) {
    pybind11::dict ret;
    ret["pile_index"] = pileIdx;
    ret["id"] = static_cast<int>(card.getId());
    ret["name"] = std::string(card.getName());
    ret["type"] = cardTypeLabel(card.getType());
    ret["cost"] = card.cost;
    ret["cost_for_turn"] = card.costForTurn;
    ret["upgraded"] = card.isUpgraded();
    ret["upgrade_count"] = card.getUpgradeCount();
    ret["requires_target"] = card.requiresTarget();
    ret["playable"] = cardIsPlayable && card.canUseOnAnyTarget(bc);
    ret["free_to_play_once"] = card.freeToPlayOnce;
    ret["retain"] = card.retain;
    ret["ethereal"] = card.isEthereal();
    ret["exhausts"] = card.doesExhaust();
    return ret;
}

pybind11::dict monsterSnapshot(const BattleContext &bc, int monsterIdx) {
    const auto &monster = bc.monsters.arr[monsterIdx];
    const auto damage = monster.getMoveBaseDamage(bc);

    pybind11::dict ret;
    ret["monster_index"] = monsterIdx;
    ret["id"] = static_cast<int>(monster.id);
    ret["id_label"] = monsterIdLabel(monster.id);
    ret["name"] = std::string(monster.getName());
    ret["current_hp"] = monster.curHp;
    ret["max_hp"] = monster.maxHp;
    ret["block"] = monster.block;
    ret["alive"] = monster.isAlive();
    ret["targetable"] = monster.isTargetable();
    ret["attacking"] = monster.isAttacking();
    ret["intent_category"] = monster.isAttacking() ? "ATTACK" : "NON_ATTACK";
    ret["current_move"] = std::string(monsterMoveStrings[static_cast<int>(monster.moveHistory[0])]);
    ret["move_id"] = static_cast<int>(monster.moveHistory[0]);
    ret["last_move_id"] = static_cast<int>(monster.moveHistory[1]);
    ret["move_base_damage"] = damage.damage;
    ret["move_hits"] = damage.attackCount;
    ret["strength"] = monster.strength;
    ret["vulnerable"] = monster.vulnerable;
    ret["weak"] = monster.weak;
    ret["artifact"] = monster.artifact;
    ret["poison"] = monster.poison;
    ret["metallicize"] = monster.metallicize;
    ret["plated_armor"] = monster.platedArmor;
    ret["regen"] = monster.regen;
    ret["half_dead"] = monster.halfDead;
    ret["misc_info"] = monster.miscInfo;
    ret["unique_power_0"] = monster.uniquePower0;
    ret["unique_power_1"] = monster.uniquePower1;
    return ret;
}

pybind11::dict potionSnapshot(const BattleContext &bc, int potionIdx) {
    const auto potion = bc.potions[potionIdx];
    pybind11::dict ret;
    ret["potion_index"] = potionIdx;
    ret["id"] = static_cast<int>(potion);
    ret["name"] = potionLabel(potion);
    return ret;
}

pybind11::list handSnapshot(const BattleContext &bc) {
    pybind11::list ret;
    for (int idx = 0; idx < bc.cards.cardsInHand; ++idx) {
        ret.append(cardSnapshot(bc, bc.cards.hand[idx], idx, true));
    }
    return ret;
}

template <typename Pile>
pybind11::list pileSnapshot(const BattleContext &bc, const Pile &pile) {
    pybind11::list ret;
    for (int idx = 0; idx < static_cast<int>(pile.size()); ++idx) {
        ret.append(cardSnapshot(bc, pile[idx], idx, false));
    }
    return ret;
}

pybind11::list monsterGroupSnapshot(const BattleContext &bc) {
    pybind11::list ret;
    for (int idx = 0; idx < bc.monsters.monsterCount; ++idx) {
        ret.append(monsterSnapshot(bc, idx));
    }
    return ret;
}

pybind11::list potionListSnapshot(const BattleContext &bc) {
    pybind11::list ret;
    for (int idx = 0; idx < bc.potionCapacity; ++idx) {
        ret.append(potionSnapshot(bc, idx));
    }
    return ret;
}

pybind11::dict relicSnapshot(const RelicInstance &relic, int relicIdx) {
    pybind11::dict ret;
    ret["relic_index"] = relicIdx;
    ret["id"] = static_cast<int>(relic.id);
    ret["name"] = std::string(getRelicName(relic.id));
    ret["counter"] = relic.data;
    return ret;
}

pybind11::list relicListSnapshot(const GameContext &gc) {
    pybind11::list ret;
    for (int idx = 0; idx < gc.relics.size(); ++idx) {
        ret.append(relicSnapshot(gc.relics.relics[idx], idx));
    }
    return ret;
}

struct StepSimulatorCheckpoint {
    GameContext gc;
    BattleContext bc;
    bool battleActive = false;
};
pybind11::dict publicProjectionAvailable(
        const pybind11::object &value,
        const char *source) {
    pybind11::dict ret;
    ret["availability"] = "available";
    ret["source"] = source;
    ret["value"] = value;
    return ret;
}

pybind11::dict publicProjectionUnavailable(
        const char *availability,
        const char *reason) {
    pybind11::dict ret;
    ret["availability"] = availability;
    ret["reason"] = reason;
    return ret;
}

pybind11::dict publicProjectionActionSnapshot(const LightSpeedAction &action) {
    pybind11::dict ret;
    ret["scope"] = action.scope;
    ret["bits"] = action.bits;
    ret["kind"] = action.kind;
    ret["idx1"] = action.idx1;
    ret["idx2"] = action.idx2;
    ret["idx3"] = action.idx3;
    ret["label"] = action.label;
    return ret;
}

struct StepSimulator {
    GameContext gc;
    BattleContext bc;
    bool battleActive = false;

    StepSimulator(CharacterClass cc, std::uint64_t seed, int ascension) : gc(cc, seed, ascension) {}

    void reset(CharacterClass cc, std::uint64_t seed, int ascension) {
        gc = GameContext(cc, seed, ascension);
        bc = BattleContext();
        battleActive = false;
    }

    void ensureBattleContext() {
        if (gc.outcome != GameOutcome::UNDECIDED) {
            battleActive = false;
            return;
        }
        if (gc.screenState == ScreenState::BATTLE && !battleActive) {
            bc = BattleContext();
            bc.init(gc);
            battleActive = true;
        }
        if (gc.screenState != ScreenState::BATTLE) {
            battleActive = false;
        }
    }

    pybind11::dict snapshot() {
        ensureBattleContext();
        pybind11::dict ret;
        ret["screen_state"] = screenStateLabel(gc.screenState);
        ret["outcome"] = gameOutcomeLabel(gc.outcome);
        ret["act"] = gc.act;
        ret["floor_num"] = gc.floorNum;
        ret["cur_hp"] = gc.curHp;
        ret["max_hp"] = gc.maxHp;
        ret["gold"] = gc.gold;
        ret["ascension"] = gc.ascension;
        ret["room_type"] = roomStrings[static_cast<int>(gc.curRoom)];
        ret["potion_count"] = gc.potionCount;
        ret["potion_capacity"] = gc.potionCapacity;
        ret["battle_active"] = battleActive;
        if (battleActive) {
            ret["encounter_id"] = monsterEncounterEnumNames[static_cast<int>(bc.encounter)];
            ret["battle_outcome"] = battleOutcomeLabel(bc.outcome);
            ret["battle_input_state"] = inputStateLabel(bc.inputState);
            ret["battle_turn"] = bc.turn;
            ret["battle_player_hp"] = bc.player.curHp;
            ret["battle_player_energy"] = bc.player.energy;
            ret["battle_player_block"] = bc.player.block;
            ret["battle_player"] = playerSnapshot(bc.player);
            ret["battle_hand_size"] = bc.cards.cardsInHand;
            ret["battle_hand"] = handSnapshot(bc);
            ret["battle_draw_pile_size"] = static_cast<int>(bc.cards.drawPile.size());
            ret["battle_discard_pile_size"] = static_cast<int>(bc.cards.discardPile.size());
            ret["battle_exhaust_pile_size"] = static_cast<int>(bc.cards.exhaustPile.size());
            ret["battle_discard_pile"] = pileSnapshot(bc, bc.cards.discardPile);
            ret["battle_exhaust_pile"] = pileSnapshot(bc, bc.cards.exhaustPile);
            ret["battle_monster_count"] = bc.monsters.monsterCount;
            ret["battle_monsters_alive"] = bc.monsters.monstersAlive;
            ret["battle_monsters"] = monsterGroupSnapshot(bc);
            ret["battle_potion_count"] = bc.potionCount;
            ret["battle_potion_capacity"] = bc.potionCapacity;
            ret["battle_potions"] = potionListSnapshot(bc);
            ret["battle_relics"] = relicListSnapshot(gc);
        }
        return ret;
    }

    std::vector<int> observation() const {
        const auto obs = NNInterface::getInstance()->getObservation(gc);
        return {obs.begin(), obs.end()};
    }

    std::vector<LightSpeedAction> legalActions() {
        ensureBattleContext();
        std::vector<LightSpeedAction> result;
        if (gc.outcome != GameOutcome::UNDECIDED) {
            return result;
        }

        if (gc.screenState == ScreenState::BATTLE) {
            for (const auto &action : enumerateBattleActions(bc)) {
                result.push_back(makeBattleAction(bc, action));
            }
            return result;
        }

        for (const auto &action : search::GameAction::getAllActionsInState(gc)) {
            result.push_back(makeGameAction(gc, action));
        }

        for (int idx = 0; idx < gc.potionCapacity; ++idx) {
            const auto potionIdx = static_cast<std::uint32_t>(idx);
            const search::GameAction useAction(0x80000000U | potionIdx);
            if (useAction.isValidAction(gc)) {
                result.push_back(makeGameAction(gc, useAction));
            }

            const search::GameAction discardAction(0xC0000000U | potionIdx);
            if (discardAction.isValidAction(gc)) {
                result.push_back(makeGameAction(gc, discardAction));
            }
        }

        return result;
    }

    pybind11::dict publicProjection() {
        ensureBattleContext();
        pybind11::dict ret;
        ret["schema_id"] = "native-public-projection-v1";
        ret["external_base_commit"] = "7476a81";
        ret["patch_identity"] = "sts_lightspeed_public_projection.patch";
        ret["screen_identity"] = publicProjectionAvailable(
                pybind11::str(screenStateLabel(gc.screenState)),
                "GameContext::screenState");
        ret["visible_act_boss"] = publicProjectionUnavailable(
                "unavailable",
                "no audited native source is exposed by this patch");
        ret["visible_map_graph"] = publicProjectionUnavailable(
                "unavailable",
                "no audited native source is exposed by this patch");
        ret["current_map_node"] = publicProjectionUnavailable(
                "unavailable",
                "no audited native source is exposed by this patch");
        ret["immediately_legal_routes"] = publicProjectionUnavailable(
                "unavailable",
                "no audited native source is exposed by this patch");

        pybind11::dict resourceFields;
        if (battleActive) {
            resourceFields["current_hp"] = publicProjectionAvailable(
                    pybind11::int_(bc.player.curHp), "BattleContext::player.curHp");
            resourceFields["max_hp"] = publicProjectionAvailable(
                    pybind11::int_(bc.player.maxHp), "BattleContext::player.maxHp");
            resourceFields["gold"] = publicProjectionAvailable(
                    pybind11::int_(bc.player.gold), "BattleContext::player.gold");
            resourceFields["potion_count"] = publicProjectionAvailable(
                    pybind11::int_(bc.potionCount), "BattleContext::potionCount");
            resourceFields["potion_capacity"] = publicProjectionAvailable(
                    pybind11::int_(bc.potionCapacity), "BattleContext::potionCapacity");
        } else {
            resourceFields["current_hp"] = publicProjectionAvailable(
                    pybind11::int_(gc.curHp), "GameContext::curHp");
            resourceFields["max_hp"] = publicProjectionAvailable(
                    pybind11::int_(gc.maxHp), "GameContext::maxHp");
            resourceFields["gold"] = publicProjectionAvailable(
                    pybind11::int_(gc.gold), "GameContext::gold");
            resourceFields["potion_count"] = publicProjectionAvailable(
                    pybind11::int_(gc.potionCount), "GameContext::potionCount");
            resourceFields["potion_capacity"] = publicProjectionAvailable(
                    pybind11::int_(gc.potionCapacity), "GameContext::potionCapacity");
        }
        resourceFields["deck"] = publicProjectionUnavailable(
                "unavailable", "not yet audited for raw projection");
        resourceFields["relics"] = publicProjectionUnavailable(
                "unavailable", "not yet audited for raw projection");
        resourceFields["potion_identities"] = publicProjectionUnavailable(
                "unavailable", "not yet audited for raw projection");
        resourceFields["keys"] = publicProjectionUnavailable(
                "unavailable", "not yet audited for raw projection");
        ret["persistent_resources"] = publicProjectionAvailable(
                resourceFields, "StepSimulator::publicProjection");
        ret["screen_payload"] = publicProjectionUnavailable(
                "unsupported", "screen-specific payloads are not exposed by this patch");

        pybind11::list candidates;
        for (const auto &action : legalActions()) {
            candidates.append(publicProjectionActionSnapshot(action));
        }
        ret["candidate_actions"] = publicProjectionAvailable(
                candidates, "StepSimulator::legalActions");
        return ret;
    }

    pybind11::dict step(const LightSpeedAction &action) {
        ensureBattleContext();
        if (action.scope == "battle") {
            if (!battleActive) {
                throw std::runtime_error("battle action requested outside battle");
            }
            search::Action battleAction(action.bits);
            if (!battleAction.isValidAction(bc)) {
                throw std::invalid_argument("invalid battle action");
            }
            battleAction.execute(bc);
            if (bc.outcome != Outcome::UNDECIDED) {
                const auto completedBattleOutcome = battleOutcomeLabel(bc.outcome);
                bc.exitBattle(gc);
                battleActive = false;
                auto result = snapshot();
                result["completed_battle_outcome"] = completedBattleOutcome;
                return result;
            }
            return snapshot();
        }

        if (action.scope == "game") {
            if (gc.screenState == ScreenState::BATTLE) {
                throw std::runtime_error("game action requested during battle");
            }
            search::GameAction gameAction(action.bits);
            if (!gameAction.isValidAction(gc)) {
                throw std::invalid_argument("invalid game action");
            }
            gameAction.execute(gc);
            return snapshot();
        }

        throw std::invalid_argument("unknown action scope");
    }

    StepSimulatorCheckpoint captureCheckpoint() const {
        return StepSimulatorCheckpoint{gc, bc, battleActive};
    }

    pybind11::dict restoreCheckpoint(const StepSimulatorCheckpoint &checkpoint) {
        gc = checkpoint.gc;
        bc = checkpoint.bc;
        battleActive = checkpoint.battleActive;
        return snapshot();
    }
};

}


PYBIND11_MODULE(slaythespire, m) {
    m.doc() = "pybind11 example plugin"; // optional module docstring
    m.def("play", &sts::py::play, "play Slay the Spire Console");
    m.def("get_seed_str", &SeedHelper::getString, "gets the integral representation of seed string used in the game ui");
    m.def("get_seed_long", &SeedHelper::getLong, "gets the seed string representation of an integral seed");
    m.def("getNNInterface", &sts::NNInterface::getInstance, "gets the NNInterface object");

    pybind11::class_<NNInterface> nnInterface(m, "NNInterface");
    nnInterface.def("getObservation", &NNInterface::getObservation, "get observation array given a GameContext")
        .def("getObservationMaximums", &NNInterface::getObservationMaximums, "get the defined maximum values of the observation space")
        .def_property_readonly("observation_space_size", []() { return NNInterface::observation_space_size; });

    pybind11::class_<search::ScumSearchAgent2> agent(m, "Agent");
    agent.def(pybind11::init<>());
    agent.def_readwrite("simulation_count_base", &search::ScumSearchAgent2::simulationCountBase, "number of simulations the agent uses for monte carlo tree search each turn")
        .def_readwrite("boss_simulation_multiplier", &search::ScumSearchAgent2::bossSimulationMultiplier, "bonus multiplier to the simulation count for boss fights")
        .def_readwrite("pause_on_card_reward", &search::ScumSearchAgent2::pauseOnCardReward, "causes the agent to pause so as to cede control to the user when it encounters a card reward choice")
        .def_readwrite("print_logs", &search::ScumSearchAgent2::printLogs, "when set to true, the agent prints state information as it makes actions")
        .def("playout", &search::ScumSearchAgent2::playout);


    pybind11::class_<LightSpeedAction>(m, "LightSpeedAction")
        .def_readonly("scope", &LightSpeedAction::scope)
        .def_readonly("bits", &LightSpeedAction::bits)
        .def_readonly("kind", &LightSpeedAction::kind)
        .def_readonly("idx1", &LightSpeedAction::idx1)
        .def_readonly("idx2", &LightSpeedAction::idx2)
        .def_readonly("idx3", &LightSpeedAction::idx3)
        .def_readonly("label", &LightSpeedAction::label)
        .def("__repr__", [](const LightSpeedAction &action) {
            return "<LightSpeedAction " + action.label + ">";
        });

    pybind11::class_<StepSimulatorCheckpoint>(m, "StepSimulatorCheckpoint");

    pybind11::class_<StepSimulator>(m, "StepSimulator")
        .def(pybind11::init<CharacterClass, std::uint64_t, int>())
        .def("reset", &StepSimulator::reset)
        .def("snapshot", &StepSimulator::snapshot)
        .def("observation", &StepSimulator::observation)
        .def("legal_actions", &StepSimulator::legalActions)
        .def("public_projection", &StepSimulator::publicProjection)
        .def("capture_checkpoint", &StepSimulator::captureCheckpoint)
        .def("restore_checkpoint", &StepSimulator::restoreCheckpoint)
        .def("step", &StepSimulator::step);

    pybind11::class_<GameContext> gameContext(m, "GameContext");
    gameContext.def(pybind11::init<CharacterClass, std::uint64_t, int>())
        .def("pick_reward_card", &sts::py::pickRewardCard, "choose to obtain the card at the specified index in the card reward list")
        .def("skip_reward_cards", &sts::py::skipRewardCards, "choose to skip the card reward (increases max_hp by 2 with singing bowl)")
        .def("get_card_reward", &sts::py::getCardReward, "return the current card reward list")
        .def_property_readonly("encounter", [](const GameContext &gc) { return gc.info.encounter; })
        .def_property_readonly("deck",
               [](const GameContext &gc) { return std::vector(gc.deck.cards.begin(), gc.deck.cards.end());},
               "returns a copy of the list of cards in the deck"
        )
        .def("obtain_card",
             [](GameContext &gc, Card card) { gc.deck.obtain(gc, card); },
             "add a card to the deck"
        )
        .def("remove_card",
            [](GameContext &gc, int idx) {
                if (idx < 0 || idx >= gc.deck.size()) {
                    std::cerr << "invalid remove deck remove idx" << std::endl;
                    return;
                }
                gc.deck.remove(gc, idx);
            },
             "remove a card at a idx in the deck"
        )
        .def_property_readonly("relics",
               [] (const GameContext &gc) { return std::vector(gc.relics.relics); },
               "returns a copy of the list of relics"
        )
        .def("__repr__", [](const GameContext &gc) {
            std::ostringstream oss;
            oss << "<" << gc << ">";
            return oss.str();
        }, "returns a string representation of the GameContext");

    gameContext.def_readwrite("outcome", &GameContext::outcome)
        .def_readwrite("act", &GameContext::act)
        .def_readwrite("floor_num", &GameContext::floorNum)
        .def_readwrite("screen_state", &GameContext::screenState)

        .def_readwrite("seed", &GameContext::seed)
        .def_readwrite("cur_map_node_x", &GameContext::curMapNodeX)
        .def_readwrite("cur_map_node_y", &GameContext::curMapNodeY)
        .def_readwrite("cur_room", &GameContext::curRoom)
//        .def_readwrite("cur_event", &GameContext::curEvent) // todo standardize event names
        .def_readwrite("boss", &GameContext::boss)

        .def_readwrite("cur_hp", &GameContext::curHp)
        .def_readwrite("max_hp", &GameContext::maxHp)
        .def_readwrite("gold", &GameContext::gold)

        .def_readwrite("blue_key", &GameContext::blueKey)
        .def_readwrite("green_key", &GameContext::greenKey)
        .def_readwrite("red_key", &GameContext::redKey)

        .def_readwrite("card_rarity_factor", &GameContext::cardRarityFactor)
        .def_readwrite("potion_chance", &GameContext::potionChance)
        .def_readwrite("monster_chance", &GameContext::monsterChance)
        .def_readwrite("shop_chance", &GameContext::shopChance)
        .def_readwrite("treasure_chance", &GameContext::treasureChance)

        .def_readwrite("shop_remove_count", &GameContext::shopRemoveCount)
        .def_readwrite("speedrun_pace", &GameContext::speedrunPace)
        .def_readwrite("note_for_yourself_card", &GameContext::noteForYourselfCard);

    pybind11::class_<RelicInstance> relic(m, "Relic");
    relic.def_readwrite("id", &RelicInstance::id)
        .def_readwrite("data", &RelicInstance::data);

    pybind11::class_<Map> map(m, "SpireMap");
    map.def(pybind11::init<std::uint64_t, int,int,bool>());
    map.def("get_room_type", &sts::py::getRoomType);
    map.def("has_edge", &sts::py::hasEdge);
    map.def("get_nn_rep", &sts::py::getNNMapRepresentation);
    map.def("__repr__", [](const Map &m) {
        return m.toString(true);
    });

    pybind11::class_<Card> card(m, "Card");
    card.def(pybind11::init<CardId>())
        .def("__repr__", [](const Card &c) {
            std::string s("<slaythespire.Card ");
            s += c.getName();
            if (c.isUpgraded()) {
                s += '+';
                if (c.id == sts::CardId::SEARING_BLOW) {
                    s += std::to_string(c.getUpgraded());
                }
            }
            return s += ">";
        }, "returns a string representation of a Card")
        .def("upgrade", &Card::upgrade)
        .def_readwrite("misc", &Card::misc, "value internal to the simulator used for things like ritual dagger damage");

    card.def_property_readonly("id", &Card::getId)
        .def_property_readonly("upgraded", &Card::isUpgraded)
        .def_property_readonly("upgrade_count", &Card::getUpgraded)
        .def_property_readonly("innate", &Card::isInnate)
        .def_property_readonly("transformable", &Card::canTransform)
        .def_property_readonly("upgradable", &Card::canUpgrade)
        .def_property_readonly("is_strikeCard", &Card::isStrikeCard)
        .def_property_readonly("is_starter_strike_or_defend", &Card::isStarterStrikeOrDefend)
        .def_property_readonly("rarity", &Card::getRarity)
        .def_property_readonly("type", &Card::getType);

    pybind11::enum_<GameOutcome> gameOutcome(m, "GameOutcome");
    gameOutcome.value("UNDECIDED", GameOutcome::UNDECIDED)
        .value("PLAYER_VICTORY", GameOutcome::PLAYER_VICTORY)
        .value("PLAYER_LOSS", GameOutcome::PLAYER_LOSS);

    pybind11::enum_<ScreenState> screenState(m, "ScreenState");
    screenState.value("INVALID", ScreenState::INVALID)
        .value("EVENT_SCREEN", ScreenState::EVENT_SCREEN)
        .value("REWARDS", ScreenState::REWARDS)
        .value("BOSS_RELIC_REWARDS", ScreenState::BOSS_RELIC_REWARDS)
        .value("CARD_SELECT", ScreenState::CARD_SELECT)
        .value("MAP_SCREEN", ScreenState::MAP_SCREEN)
        .value("TREASURE_ROOM", ScreenState::TREASURE_ROOM)
        .value("REST_ROOM", ScreenState::REST_ROOM)
        .value("SHOP_ROOM", ScreenState::SHOP_ROOM)
        .value("BATTLE", ScreenState::BATTLE);

    pybind11::enum_<CharacterClass> characterClass(m, "CharacterClass");
    characterClass.value("IRONCLAD", CharacterClass::IRONCLAD)
            .value("SILENT", CharacterClass::SILENT)
            .value("DEFECT", CharacterClass::DEFECT)
            .value("WATCHER", CharacterClass::WATCHER)
            .value("INVALID", CharacterClass::INVALID);

    pybind11::enum_<Room> roomEnum(m, "Room");
    roomEnum.value("SHOP", Room::SHOP)
        .value("REST", Room::REST)
        .value("EVENT", Room::EVENT)
        .value("ELITE", Room::ELITE)
        .value("MONSTER", Room::MONSTER)
        .value("TREASURE", Room::TREASURE)
        .value("BOSS", Room::BOSS)
        .value("BOSS_TREASURE", Room::BOSS_TREASURE)
        .value("NONE", Room::NONE)
        .value("INVALID", Room::INVALID);

    pybind11::enum_<CardRarity>(m, "CardRarity")
        .value("COMMON", CardRarity::COMMON)
        .value("UNCOMMON", CardRarity::UNCOMMON)
        .value("RARE", CardRarity::RARE)
        .value("BASIC", CardRarity::BASIC)
        .value("SPECIAL", CardRarity::SPECIAL)
        .value("CURSE", CardRarity::CURSE)
        .value("INVALID", CardRarity::INVALID);

    pybind11::enum_<CardColor>(m, "CardColor")
        .value("RED", CardColor::RED)
        .value("GREEN", CardColor::GREEN)
        .value("PURPLE", CardColor::PURPLE)
        .value("COLORLESS", CardColor::COLORLESS)
        .value("CURSE", CardColor::CURSE)
        .value("INVALID", CardColor::INVALID);

    pybind11::enum_<CardType>(m, "CardType")
        .value("ATTACK", CardType::ATTACK)
        .value("SKILL", CardType::SKILL)
        .value("POWER", CardType::POWER)
        .value("CURSE", CardType::CURSE)
        .value("STATUS", CardType::STATUS)
        .value("INVALID", CardType::INVALID);

    pybind11::enum_<CardId>(m, "CardId")
        .value("INVALID", CardId::INVALID)
        .value("ACCURACY", CardId::ACCURACY)
        .value("ACROBATICS", CardId::ACROBATICS)
        .value("ADRENALINE", CardId::ADRENALINE)
        .value("AFTER_IMAGE", CardId::AFTER_IMAGE)
        .value("AGGREGATE", CardId::AGGREGATE)
        .value("ALCHEMIZE", CardId::ALCHEMIZE)
        .value("ALL_FOR_ONE", CardId::ALL_FOR_ONE)
        .value("ALL_OUT_ATTACK", CardId::ALL_OUT_ATTACK)
        .value("ALPHA", CardId::ALPHA)
        .value("AMPLIFY", CardId::AMPLIFY)
        .value("ANGER", CardId::ANGER)
        .value("APOTHEOSIS", CardId::APOTHEOSIS)
        .value("APPARITION", CardId::APPARITION)
        .value("ARMAMENTS", CardId::ARMAMENTS)
        .value("ASCENDERS_BANE", CardId::ASCENDERS_BANE)
        .value("AUTO_SHIELDS", CardId::AUTO_SHIELDS)
        .value("A_THOUSAND_CUTS", CardId::A_THOUSAND_CUTS)
        .value("BACKFLIP", CardId::BACKFLIP)
        .value("BACKSTAB", CardId::BACKSTAB)
        .value("BALL_LIGHTNING", CardId::BALL_LIGHTNING)
        .value("BANDAGE_UP", CardId::BANDAGE_UP)
        .value("BANE", CardId::BANE)
        .value("BARRAGE", CardId::BARRAGE)
        .value("BARRICADE", CardId::BARRICADE)
        .value("BASH", CardId::BASH)
        .value("BATTLE_HYMN", CardId::BATTLE_HYMN)
        .value("BATTLE_TRANCE", CardId::BATTLE_TRANCE)
        .value("BEAM_CELL", CardId::BEAM_CELL)
        .value("BECOME_ALMIGHTY", CardId::BECOME_ALMIGHTY)
        .value("BERSERK", CardId::BERSERK)
        .value("BETA", CardId::BETA)
        .value("BIASED_COGNITION", CardId::BIASED_COGNITION)
        .value("BITE", CardId::BITE)
        .value("BLADE_DANCE", CardId::BLADE_DANCE)
        .value("BLASPHEMY", CardId::BLASPHEMY)
        .value("BLIND", CardId::BLIND)
        .value("BLIZZARD", CardId::BLIZZARD)
        .value("BLOODLETTING", CardId::BLOODLETTING)
        .value("BLOOD_FOR_BLOOD", CardId::BLOOD_FOR_BLOOD)
        .value("BLUDGEON", CardId::BLUDGEON)
        .value("BLUR", CardId::BLUR)
        .value("BODY_SLAM", CardId::BODY_SLAM)
        .value("BOOT_SEQUENCE", CardId::BOOT_SEQUENCE)
        .value("BOUNCING_FLASK", CardId::BOUNCING_FLASK)
        .value("BOWLING_BASH", CardId::BOWLING_BASH)
        .value("BRILLIANCE", CardId::BRILLIANCE)
        .value("BRUTALITY", CardId::BRUTALITY)
        .value("BUFFER", CardId::BUFFER)
        .value("BULLET_TIME", CardId::BULLET_TIME)
        .value("BULLSEYE", CardId::BULLSEYE)
        .value("BURN", CardId::BURN)
        .value("BURNING_PACT", CardId::BURNING_PACT)
        .value("BURST", CardId::BURST)
        .value("CALCULATED_GAMBLE", CardId::CALCULATED_GAMBLE)
        .value("CALTROPS", CardId::CALTROPS)
        .value("CAPACITOR", CardId::CAPACITOR)
        .value("CARNAGE", CardId::CARNAGE)
        .value("CARVE_REALITY", CardId::CARVE_REALITY)
        .value("CATALYST", CardId::CATALYST)
        .value("CHAOS", CardId::CHAOS)
        .value("CHARGE_BATTERY", CardId::CHARGE_BATTERY)
        .value("CHILL", CardId::CHILL)
        .value("CHOKE", CardId::CHOKE)
        .value("CHRYSALIS", CardId::CHRYSALIS)
        .value("CLASH", CardId::CLASH)
        .value("CLAW", CardId::CLAW)
        .value("CLEAVE", CardId::CLEAVE)
        .value("CLOAK_AND_DAGGER", CardId::CLOAK_AND_DAGGER)
        .value("CLOTHESLINE", CardId::CLOTHESLINE)
        .value("CLUMSY", CardId::CLUMSY)
        .value("COLD_SNAP", CardId::COLD_SNAP)
        .value("COLLECT", CardId::COLLECT)
        .value("COMBUST", CardId::COMBUST)
        .value("COMPILE_DRIVER", CardId::COMPILE_DRIVER)
        .value("CONCENTRATE", CardId::CONCENTRATE)
        .value("CONCLUDE", CardId::CONCLUDE)
        .value("CONJURE_BLADE", CardId::CONJURE_BLADE)
        .value("CONSECRATE", CardId::CONSECRATE)
        .value("CONSUME", CardId::CONSUME)
        .value("COOLHEADED", CardId::COOLHEADED)
        .value("CORE_SURGE", CardId::CORE_SURGE)
        .value("CORPSE_EXPLOSION", CardId::CORPSE_EXPLOSION)
        .value("CORRUPTION", CardId::CORRUPTION)
        .value("CREATIVE_AI", CardId::CREATIVE_AI)
        .value("CRESCENDO", CardId::CRESCENDO)
        .value("CRIPPLING_CLOUD", CardId::CRIPPLING_CLOUD)
        .value("CRUSH_JOINTS", CardId::CRUSH_JOINTS)
        .value("CURSE_OF_THE_BELL", CardId::CURSE_OF_THE_BELL)
        .value("CUT_THROUGH_FATE", CardId::CUT_THROUGH_FATE)
        .value("DAGGER_SPRAY", CardId::DAGGER_SPRAY)
        .value("DAGGER_THROW", CardId::DAGGER_THROW)
        .value("DARKNESS", CardId::DARKNESS)
        .value("DARK_EMBRACE", CardId::DARK_EMBRACE)
        .value("DARK_SHACKLES", CardId::DARK_SHACKLES)
        .value("DASH", CardId::DASH)
        .value("DAZED", CardId::DAZED)
        .value("DEADLY_POISON", CardId::DEADLY_POISON)
        .value("DECAY", CardId::DECAY)
        .value("DECEIVE_REALITY", CardId::DECEIVE_REALITY)
        .value("DEEP_BREATH", CardId::DEEP_BREATH)
        .value("DEFEND_BLUE", CardId::DEFEND_BLUE)
        .value("DEFEND_GREEN", CardId::DEFEND_GREEN)
        .value("DEFEND_PURPLE", CardId::DEFEND_PURPLE)
        .value("DEFEND_RED", CardId::DEFEND_RED)
        .value("DEFLECT", CardId::DEFLECT)
        .value("DEFRAGMENT", CardId::DEFRAGMENT)
        .value("DEMON_FORM", CardId::DEMON_FORM)
        .value("DEUS_EX_MACHINA", CardId::DEUS_EX_MACHINA)
        .value("DEVA_FORM", CardId::DEVA_FORM)
        .value("DEVOTION", CardId::DEVOTION)
        .value("DIE_DIE_DIE", CardId::DIE_DIE_DIE)
        .value("DISARM", CardId::DISARM)
        .value("DISCOVERY", CardId::DISCOVERY)
        .value("DISTRACTION", CardId::DISTRACTION)
        .value("DODGE_AND_ROLL", CardId::DODGE_AND_ROLL)
        .value("DOOM_AND_GLOOM", CardId::DOOM_AND_GLOOM)
        .value("DOPPELGANGER", CardId::DOPPELGANGER)
        .value("DOUBLE_ENERGY", CardId::DOUBLE_ENERGY)
        .value("DOUBLE_TAP", CardId::DOUBLE_TAP)
        .value("DOUBT", CardId::DOUBT)
        .value("DRAMATIC_ENTRANCE", CardId::DRAMATIC_ENTRANCE)
        .value("DROPKICK", CardId::DROPKICK)
        .value("DUALCAST", CardId::DUALCAST)
        .value("DUAL_WIELD", CardId::DUAL_WIELD)
        .value("ECHO_FORM", CardId::ECHO_FORM)
        .value("ELECTRODYNAMICS", CardId::ELECTRODYNAMICS)
        .value("EMPTY_BODY", CardId::EMPTY_BODY)
        .value("EMPTY_FIST", CardId::EMPTY_FIST)
        .value("EMPTY_MIND", CardId::EMPTY_MIND)
        .value("ENDLESS_AGONY", CardId::ENDLESS_AGONY)
        .value("ENLIGHTENMENT", CardId::ENLIGHTENMENT)
        .value("ENTRENCH", CardId::ENTRENCH)
        .value("ENVENOM", CardId::ENVENOM)
        .value("EQUILIBRIUM", CardId::EQUILIBRIUM)
        .value("ERUPTION", CardId::ERUPTION)
        .value("ESCAPE_PLAN", CardId::ESCAPE_PLAN)
        .value("ESTABLISHMENT", CardId::ESTABLISHMENT)
        .value("EVALUATE", CardId::EVALUATE)
        .value("EVISCERATE", CardId::EVISCERATE)
        .value("EVOLVE", CardId::EVOLVE)
        .value("EXHUME", CardId::EXHUME)
        .value("EXPERTISE", CardId::EXPERTISE)
        .value("EXPUNGER", CardId::EXPUNGER)
        .value("FAME_AND_FORTUNE", CardId::FAME_AND_FORTUNE)
        .value("FASTING", CardId::FASTING)
        .value("FEAR_NO_EVIL", CardId::FEAR_NO_EVIL)
        .value("FEED", CardId::FEED)
        .value("FEEL_NO_PAIN", CardId::FEEL_NO_PAIN)
        .value("FIEND_FIRE", CardId::FIEND_FIRE)
        .value("FINESSE", CardId::FINESSE)
        .value("FINISHER", CardId::FINISHER)
        .value("FIRE_BREATHING", CardId::FIRE_BREATHING)
        .value("FISSION", CardId::FISSION)
        .value("FLAME_BARRIER", CardId::FLAME_BARRIER)
        .value("FLASH_OF_STEEL", CardId::FLASH_OF_STEEL)
        .value("FLECHETTES", CardId::FLECHETTES)
        .value("FLEX", CardId::FLEX)
        .value("FLURRY_OF_BLOWS", CardId::FLURRY_OF_BLOWS)
        .value("FLYING_KNEE", CardId::FLYING_KNEE)
        .value("FLYING_SLEEVES", CardId::FLYING_SLEEVES)
        .value("FOLLOW_UP", CardId::FOLLOW_UP)
        .value("FOOTWORK", CardId::FOOTWORK)
        .value("FORCE_FIELD", CardId::FORCE_FIELD)
        .value("FOREIGN_INFLUENCE", CardId::FOREIGN_INFLUENCE)
        .value("FORESIGHT", CardId::FORESIGHT)
        .value("FORETHOUGHT", CardId::FORETHOUGHT)
        .value("FTL", CardId::FTL)
        .value("FUSION", CardId::FUSION)
        .value("GENETIC_ALGORITHM", CardId::GENETIC_ALGORITHM)
        .value("GHOSTLY_ARMOR", CardId::GHOSTLY_ARMOR)
        .value("GLACIER", CardId::GLACIER)
        .value("GLASS_KNIFE", CardId::GLASS_KNIFE)
        .value("GOOD_INSTINCTS", CardId::GOOD_INSTINCTS)
        .value("GO_FOR_THE_EYES", CardId::GO_FOR_THE_EYES)
        .value("GRAND_FINALE", CardId::GRAND_FINALE)
        .value("HALT", CardId::HALT)
        .value("HAND_OF_GREED", CardId::HAND_OF_GREED)
        .value("HAVOC", CardId::HAVOC)
        .value("HEADBUTT", CardId::HEADBUTT)
        .value("HEATSINKS", CardId::HEATSINKS)
        .value("HEAVY_BLADE", CardId::HEAVY_BLADE)
        .value("HEEL_HOOK", CardId::HEEL_HOOK)
        .value("HELLO_WORLD", CardId::HELLO_WORLD)
        .value("HEMOKINESIS", CardId::HEMOKINESIS)
        .value("HOLOGRAM", CardId::HOLOGRAM)
        .value("HYPERBEAM", CardId::HYPERBEAM)
        .value("IMMOLATE", CardId::IMMOLATE)
        .value("IMPATIENCE", CardId::IMPATIENCE)
        .value("IMPERVIOUS", CardId::IMPERVIOUS)
        .value("INDIGNATION", CardId::INDIGNATION)
        .value("INFERNAL_BLADE", CardId::INFERNAL_BLADE)
        .value("INFINITE_BLADES", CardId::INFINITE_BLADES)
        .value("INFLAME", CardId::INFLAME)
        .value("INJURY", CardId::INJURY)
        .value("INNER_PEACE", CardId::INNER_PEACE)
        .value("INSIGHT", CardId::INSIGHT)
        .value("INTIMIDATE", CardId::INTIMIDATE)
        .value("IRON_WAVE", CardId::IRON_WAVE)
        .value("JAX", CardId::JAX)
        .value("JACK_OF_ALL_TRADES", CardId::JACK_OF_ALL_TRADES)
        .value("JUDGMENT", CardId::JUDGMENT)
        .value("JUGGERNAUT", CardId::JUGGERNAUT)
        .value("JUST_LUCKY", CardId::JUST_LUCKY)
        .value("LEAP", CardId::LEAP)
        .value("LEG_SWEEP", CardId::LEG_SWEEP)
        .value("LESSON_LEARNED", CardId::LESSON_LEARNED)
        .value("LIKE_WATER", CardId::LIKE_WATER)
        .value("LIMIT_BREAK", CardId::LIMIT_BREAK)
        .value("LIVE_FOREVER", CardId::LIVE_FOREVER)
        .value("LOOP", CardId::LOOP)
        .value("MACHINE_LEARNING", CardId::MACHINE_LEARNING)
        .value("MADNESS", CardId::MADNESS)
        .value("MAGNETISM", CardId::MAGNETISM)
        .value("MALAISE", CardId::MALAISE)
        .value("MASTERFUL_STAB", CardId::MASTERFUL_STAB)
        .value("MASTER_OF_STRATEGY", CardId::MASTER_OF_STRATEGY)
        .value("MASTER_REALITY", CardId::MASTER_REALITY)
        .value("MAYHEM", CardId::MAYHEM)
        .value("MEDITATE", CardId::MEDITATE)
        .value("MELTER", CardId::MELTER)
        .value("MENTAL_FORTRESS", CardId::MENTAL_FORTRESS)
        .value("METALLICIZE", CardId::METALLICIZE)
        .value("METAMORPHOSIS", CardId::METAMORPHOSIS)
        .value("METEOR_STRIKE", CardId::METEOR_STRIKE)
        .value("MIND_BLAST", CardId::MIND_BLAST)
        .value("MIRACLE", CardId::MIRACLE)
        .value("MULTI_CAST", CardId::MULTI_CAST)
        .value("NECRONOMICURSE", CardId::NECRONOMICURSE)
        .value("NEUTRALIZE", CardId::NEUTRALIZE)
        .value("NIGHTMARE", CardId::NIGHTMARE)
        .value("NIRVANA", CardId::NIRVANA)
        .value("NORMALITY", CardId::NORMALITY)
        .value("NOXIOUS_FUMES", CardId::NOXIOUS_FUMES)
        .value("OFFERING", CardId::OFFERING)
        .value("OMEGA", CardId::OMEGA)
        .value("OMNISCIENCE", CardId::OMNISCIENCE)
        .value("OUTMANEUVER", CardId::OUTMANEUVER)
        .value("OVERCLOCK", CardId::OVERCLOCK)
        .value("PAIN", CardId::PAIN)
        .value("PANACEA", CardId::PANACEA)
        .value("PANACHE", CardId::PANACHE)
        .value("PANIC_BUTTON", CardId::PANIC_BUTTON)
        .value("PARASITE", CardId::PARASITE)
        .value("PERFECTED_STRIKE", CardId::PERFECTED_STRIKE)
        .value("PERSEVERANCE", CardId::PERSEVERANCE)
        .value("PHANTASMAL_KILLER", CardId::PHANTASMAL_KILLER)
        .value("PIERCING_WAIL", CardId::PIERCING_WAIL)
        .value("POISONED_STAB", CardId::POISONED_STAB)
        .value("POMMEL_STRIKE", CardId::POMMEL_STRIKE)
        .value("POWER_THROUGH", CardId::POWER_THROUGH)
        .value("PRAY", CardId::PRAY)
        .value("PREDATOR", CardId::PREDATOR)
        .value("PREPARED", CardId::PREPARED)
        .value("PRESSURE_POINTS", CardId::PRESSURE_POINTS)
        .value("PRIDE", CardId::PRIDE)
        .value("PROSTRATE", CardId::PROSTRATE)
        .value("PROTECT", CardId::PROTECT)
        .value("PUMMEL", CardId::PUMMEL)
        .value("PURITY", CardId::PURITY)
        .value("QUICK_SLASH", CardId::QUICK_SLASH)
        .value("RAGE", CardId::RAGE)
        .value("RAGNAROK", CardId::RAGNAROK)
        .value("RAINBOW", CardId::RAINBOW)
        .value("RAMPAGE", CardId::RAMPAGE)
        .value("REACH_HEAVEN", CardId::REACH_HEAVEN)
        .value("REAPER", CardId::REAPER)
        .value("REBOOT", CardId::REBOOT)
        .value("REBOUND", CardId::REBOUND)
        .value("RECKLESS_CHARGE", CardId::RECKLESS_CHARGE)
        .value("RECURSION", CardId::RECURSION)
        .value("RECYCLE", CardId::RECYCLE)
        .value("REFLEX", CardId::REFLEX)
        .value("REGRET", CardId::REGRET)
        .value("REINFORCED_BODY", CardId::REINFORCED_BODY)
        .value("REPROGRAM", CardId::REPROGRAM)
        .value("RIDDLE_WITH_HOLES", CardId::RIDDLE_WITH_HOLES)
        .value("RIP_AND_TEAR", CardId::RIP_AND_TEAR)
        .value("RITUAL_DAGGER", CardId::RITUAL_DAGGER)
        .value("RUPTURE", CardId::RUPTURE)
        .value("RUSHDOWN", CardId::RUSHDOWN)
        .value("SADISTIC_NATURE", CardId::SADISTIC_NATURE)
        .value("SAFETY", CardId::SAFETY)
        .value("SANCTITY", CardId::SANCTITY)
        .value("SANDS_OF_TIME", CardId::SANDS_OF_TIME)
        .value("SASH_WHIP", CardId::SASH_WHIP)
        .value("SCRAPE", CardId::SCRAPE)
        .value("SCRAWL", CardId::SCRAWL)
        .value("SEARING_BLOW", CardId::SEARING_BLOW)
        .value("SECOND_WIND", CardId::SECOND_WIND)
        .value("SECRET_TECHNIQUE", CardId::SECRET_TECHNIQUE)
        .value("SECRET_WEAPON", CardId::SECRET_WEAPON)
        .value("SEEING_RED", CardId::SEEING_RED)
        .value("SEEK", CardId::SEEK)
        .value("SELF_REPAIR", CardId::SELF_REPAIR)
        .value("SENTINEL", CardId::SENTINEL)
        .value("SETUP", CardId::SETUP)
        .value("SEVER_SOUL", CardId::SEVER_SOUL)
        .value("SHAME", CardId::SHAME)
        .value("SHIV", CardId::SHIV)
        .value("SHOCKWAVE", CardId::SHOCKWAVE)
        .value("SHRUG_IT_OFF", CardId::SHRUG_IT_OFF)
        .value("SIGNATURE_MOVE", CardId::SIGNATURE_MOVE)
        .value("SIMMERING_FURY", CardId::SIMMERING_FURY)
        .value("SKEWER", CardId::SKEWER)
        .value("SKIM", CardId::SKIM)
        .value("SLICE", CardId::SLICE)
        .value("SLIMED", CardId::SLIMED)
        .value("SMITE", CardId::SMITE)
        .value("SNEAKY_STRIKE", CardId::SNEAKY_STRIKE)
        .value("SPIRIT_SHIELD", CardId::SPIRIT_SHIELD)
        .value("SPOT_WEAKNESS", CardId::SPOT_WEAKNESS)
        .value("STACK", CardId::STACK)
        .value("STATIC_DISCHARGE", CardId::STATIC_DISCHARGE)
        .value("STEAM_BARRIER", CardId::STEAM_BARRIER)
        .value("STORM", CardId::STORM)
        .value("STORM_OF_STEEL", CardId::STORM_OF_STEEL)
        .value("STREAMLINE", CardId::STREAMLINE)
        .value("STRIKE_BLUE", CardId::STRIKE_BLUE)
        .value("STRIKE_GREEN", CardId::STRIKE_GREEN)
        .value("STRIKE_PURPLE", CardId::STRIKE_PURPLE)
        .value("STRIKE_RED", CardId::STRIKE_RED)
        .value("STUDY", CardId::STUDY)
        .value("SUCKER_PUNCH", CardId::SUCKER_PUNCH)
        .value("SUNDER", CardId::SUNDER)
        .value("SURVIVOR", CardId::SURVIVOR)
        .value("SWEEPING_BEAM", CardId::SWEEPING_BEAM)
        .value("SWIFT_STRIKE", CardId::SWIFT_STRIKE)
        .value("SWIVEL", CardId::SWIVEL)
        .value("SWORD_BOOMERANG", CardId::SWORD_BOOMERANG)
        .value("TACTICIAN", CardId::TACTICIAN)
        .value("TALK_TO_THE_HAND", CardId::TALK_TO_THE_HAND)
        .value("TANTRUM", CardId::TANTRUM)
        .value("TEMPEST", CardId::TEMPEST)
        .value("TERROR", CardId::TERROR)
        .value("THE_BOMB", CardId::THE_BOMB)
        .value("THINKING_AHEAD", CardId::THINKING_AHEAD)
        .value("THIRD_EYE", CardId::THIRD_EYE)
        .value("THROUGH_VIOLENCE", CardId::THROUGH_VIOLENCE)
        .value("THUNDERCLAP", CardId::THUNDERCLAP)
        .value("THUNDER_STRIKE", CardId::THUNDER_STRIKE)
        .value("TOOLS_OF_THE_TRADE", CardId::TOOLS_OF_THE_TRADE)
        .value("TRANQUILITY", CardId::TRANQUILITY)
        .value("TRANSMUTATION", CardId::TRANSMUTATION)
        .value("TRIP", CardId::TRIP)
        .value("TRUE_GRIT", CardId::TRUE_GRIT)
        .value("TURBO", CardId::TURBO)
        .value("TWIN_STRIKE", CardId::TWIN_STRIKE)
        .value("UNLOAD", CardId::UNLOAD)
        .value("UPPERCUT", CardId::UPPERCUT)
        .value("VAULT", CardId::VAULT)
        .value("VIGILANCE", CardId::VIGILANCE)
        .value("VIOLENCE", CardId::VIOLENCE)
        .value("VOID", CardId::VOID)
        .value("WALLOP", CardId::WALLOP)
        .value("WARCRY", CardId::WARCRY)
        .value("WAVE_OF_THE_HAND", CardId::WAVE_OF_THE_HAND)
        .value("WEAVE", CardId::WEAVE)
        .value("WELL_LAID_PLANS", CardId::WELL_LAID_PLANS)
        .value("WHEEL_KICK", CardId::WHEEL_KICK)
        .value("WHIRLWIND", CardId::WHIRLWIND)
        .value("WHITE_NOISE", CardId::WHITE_NOISE)
        .value("WILD_STRIKE", CardId::WILD_STRIKE)
        .value("WINDMILL_STRIKE", CardId::WINDMILL_STRIKE)
        .value("WISH", CardId::WISH)
        .value("WORSHIP", CardId::WORSHIP)
        .value("WOUND", CardId::WOUND)
        .value("WRAITH_FORM", CardId::WRAITH_FORM)
        .value("WREATH_OF_FLAME", CardId::WREATH_OF_FLAME)
        .value("WRITHE", CardId::WRITHE)
        .value("ZAP", CardId::ZAP);

    pybind11::enum_<MonsterEncounter> meEnum(m, "MonsterEncounter");
    meEnum.value("INVALID", ME::INVALID)
        .value("CULTIST", ME::CULTIST)
        .value("JAW_WORM", ME::JAW_WORM)
        .value("TWO_LOUSE", ME::TWO_LOUSE)
        .value("SMALL_SLIMES", ME::SMALL_SLIMES)
        .value("BLUE_SLAVER", ME::BLUE_SLAVER)
        .value("GREMLIN_GANG", ME::GREMLIN_GANG)
        .value("LOOTER", ME::LOOTER)
        .value("LARGE_SLIME", ME::LARGE_SLIME)
        .value("LOTS_OF_SLIMES", ME::LOTS_OF_SLIMES)
        .value("EXORDIUM_THUGS", ME::EXORDIUM_THUGS)
        .value("EXORDIUM_WILDLIFE", ME::EXORDIUM_WILDLIFE)
        .value("RED_SLAVER", ME::RED_SLAVER)
        .value("THREE_LOUSE", ME::THREE_LOUSE)
        .value("TWO_FUNGI_BEASTS", ME::TWO_FUNGI_BEASTS)
        .value("GREMLIN_NOB", ME::GREMLIN_NOB)
        .value("LAGAVULIN", ME::LAGAVULIN)
        .value("THREE_SENTRIES", ME::THREE_SENTRIES)
        .value("SLIME_BOSS", ME::SLIME_BOSS)
        .value("THE_GUARDIAN", ME::THE_GUARDIAN)
        .value("HEXAGHOST", ME::HEXAGHOST)
        .value("SPHERIC_GUARDIAN", ME::SPHERIC_GUARDIAN)
        .value("CHOSEN", ME::CHOSEN)
        .value("SHELL_PARASITE", ME::SHELL_PARASITE)
        .value("THREE_BYRDS", ME::THREE_BYRDS)
        .value("TWO_THIEVES", ME::TWO_THIEVES)
        .value("CHOSEN_AND_BYRDS", ME::CHOSEN_AND_BYRDS)
        .value("SENTRY_AND_SPHERE", ME::SENTRY_AND_SPHERE)
        .value("SNAKE_PLANT", ME::SNAKE_PLANT)
        .value("SNECKO", ME::SNECKO)
        .value("CENTURION_AND_HEALER", ME::CENTURION_AND_HEALER)
        .value("CULTIST_AND_CHOSEN", ME::CULTIST_AND_CHOSEN)
        .value("THREE_CULTIST", ME::THREE_CULTIST)
        .value("SHELLED_PARASITE_AND_FUNGI", ME::SHELLED_PARASITE_AND_FUNGI)
        .value("GREMLIN_LEADER", ME::GREMLIN_LEADER)
        .value("SLAVERS", ME::SLAVERS)
        .value("BOOK_OF_STABBING", ME::BOOK_OF_STABBING)
        .value("AUTOMATON", ME::AUTOMATON)
        .value("COLLECTOR", ME::COLLECTOR)
        .value("CHAMP", ME::CHAMP)
        .value("THREE_DARKLINGS", ME::THREE_DARKLINGS)
        .value("ORB_WALKER", ME::ORB_WALKER)
        .value("THREE_SHAPES", ME::THREE_SHAPES)
        .value("SPIRE_GROWTH", ME::SPIRE_GROWTH)
        .value("TRANSIENT", ME::TRANSIENT)
        .value("FOUR_SHAPES", ME::FOUR_SHAPES)
        .value("MAW", ME::MAW)
        .value("SPHERE_AND_TWO_SHAPES", ME::SPHERE_AND_TWO_SHAPES)
        .value("JAW_WORM_HORDE", ME::JAW_WORM_HORDE)
        .value("WRITHING_MASS", ME::WRITHING_MASS)
        .value("GIANT_HEAD", ME::GIANT_HEAD)
        .value("NEMESIS", ME::NEMESIS)
        .value("REPTOMANCER", ME::REPTOMANCER)
        .value("AWAKENED_ONE", ME::AWAKENED_ONE)
        .value("TIME_EATER", ME::TIME_EATER)
        .value("DONU_AND_DECA", ME::DONU_AND_DECA)
        .value("SHIELD_AND_SPEAR", ME::SHIELD_AND_SPEAR)
        .value("THE_HEART", ME::THE_HEART)
        .value("LAGAVULIN_EVENT", ME::LAGAVULIN_EVENT)
        .value("COLOSSEUM_EVENT_SLAVERS", ME::COLOSSEUM_EVENT_SLAVERS)
        .value("COLOSSEUM_EVENT_NOBS", ME::COLOSSEUM_EVENT_NOBS)
        .value("MASKED_BANDITS_EVENT", ME::MASKED_BANDITS_EVENT)
        .value("MUSHROOMS_EVENT", ME::MUSHROOMS_EVENT)
        .value("MYSTERIOUS_SPHERE_EVENT", ME::MYSTERIOUS_SPHERE_EVENT);

    pybind11::enum_<RelicId> relicEnum(m, "RelicId");
    relicEnum.value("AKABEKO", RelicId::AKABEKO)
        .value("ART_OF_WAR", RelicId::ART_OF_WAR)
        .value("BIRD_FACED_URN", RelicId::BIRD_FACED_URN)
        .value("BLOODY_IDOL", RelicId::BLOODY_IDOL)
        .value("BLUE_CANDLE", RelicId::BLUE_CANDLE)
        .value("BRIMSTONE", RelicId::BRIMSTONE)
        .value("CALIPERS", RelicId::CALIPERS)
        .value("CAPTAINS_WHEEL", RelicId::CAPTAINS_WHEEL)
        .value("CENTENNIAL_PUZZLE", RelicId::CENTENNIAL_PUZZLE)
        .value("CERAMIC_FISH", RelicId::CERAMIC_FISH)
        .value("CHAMPION_BELT", RelicId::CHAMPION_BELT)
        .value("CHARONS_ASHES", RelicId::CHARONS_ASHES)
        .value("CHEMICAL_X", RelicId::CHEMICAL_X)
        .value("CLOAK_CLASP", RelicId::CLOAK_CLASP)
        .value("DARKSTONE_PERIAPT", RelicId::DARKSTONE_PERIAPT)
        .value("DEAD_BRANCH", RelicId::DEAD_BRANCH)
        .value("DUALITY", RelicId::DUALITY)
        .value("ECTOPLASM", RelicId::ECTOPLASM)
        .value("EMOTION_CHIP", RelicId::EMOTION_CHIP)
        .value("FROZEN_CORE", RelicId::FROZEN_CORE)
        .value("FROZEN_EYE", RelicId::FROZEN_EYE)
        .value("GAMBLING_CHIP", RelicId::GAMBLING_CHIP)
        .value("GINGER", RelicId::GINGER)
        .value("GOLDEN_EYE", RelicId::GOLDEN_EYE)
        .value("GREMLIN_HORN", RelicId::GREMLIN_HORN)
        .value("HAND_DRILL", RelicId::HAND_DRILL)
        .value("HAPPY_FLOWER", RelicId::HAPPY_FLOWER)
        .value("HORN_CLEAT", RelicId::HORN_CLEAT)
        .value("HOVERING_KITE", RelicId::HOVERING_KITE)
        .value("ICE_CREAM", RelicId::ICE_CREAM)
        .value("INCENSE_BURNER", RelicId::INCENSE_BURNER)
        .value("INK_BOTTLE", RelicId::INK_BOTTLE)
        .value("INSERTER", RelicId::INSERTER)
        .value("KUNAI", RelicId::KUNAI)
        .value("LETTER_OPENER", RelicId::LETTER_OPENER)
        .value("LIZARD_TAIL", RelicId::LIZARD_TAIL)
        .value("MAGIC_FLOWER", RelicId::MAGIC_FLOWER)
        .value("MARK_OF_THE_BLOOM", RelicId::MARK_OF_THE_BLOOM)
        .value("MEDICAL_KIT", RelicId::MEDICAL_KIT)
        .value("MELANGE", RelicId::MELANGE)
        .value("MERCURY_HOURGLASS", RelicId::MERCURY_HOURGLASS)
        .value("MUMMIFIED_HAND", RelicId::MUMMIFIED_HAND)
        .value("NECRONOMICON", RelicId::NECRONOMICON)
        .value("NILRYS_CODEX", RelicId::NILRYS_CODEX)
        .value("NUNCHAKU", RelicId::NUNCHAKU)
        .value("ODD_MUSHROOM", RelicId::ODD_MUSHROOM)
        .value("OMAMORI", RelicId::OMAMORI)
        .value("ORANGE_PELLETS", RelicId::ORANGE_PELLETS)
        .value("ORICHALCUM", RelicId::ORICHALCUM)
        .value("ORNAMENTAL_FAN", RelicId::ORNAMENTAL_FAN)
        .value("PAPER_KRANE", RelicId::PAPER_KRANE)
        .value("PAPER_PHROG", RelicId::PAPER_PHROG)
        .value("PEN_NIB", RelicId::PEN_NIB)
        .value("PHILOSOPHERS_STONE", RelicId::PHILOSOPHERS_STONE)
        .value("POCKETWATCH", RelicId::POCKETWATCH)
        .value("RED_SKULL", RelicId::RED_SKULL)
        .value("RUNIC_CUBE", RelicId::RUNIC_CUBE)
        .value("RUNIC_DOME", RelicId::RUNIC_DOME)
        .value("RUNIC_PYRAMID", RelicId::RUNIC_PYRAMID)
        .value("SACRED_BARK", RelicId::SACRED_BARK)
        .value("SELF_FORMING_CLAY", RelicId::SELF_FORMING_CLAY)
        .value("SHURIKEN", RelicId::SHURIKEN)
        .value("SNECKO_EYE", RelicId::SNECKO_EYE)
        .value("SNECKO_SKULL", RelicId::SNECKO_SKULL)
        .value("SOZU", RelicId::SOZU)
        .value("STONE_CALENDAR", RelicId::STONE_CALENDAR)
        .value("STRANGE_SPOON", RelicId::STRANGE_SPOON)
        .value("STRIKE_DUMMY", RelicId::STRIKE_DUMMY)
        .value("SUNDIAL", RelicId::SUNDIAL)
        .value("THE_ABACUS", RelicId::THE_ABACUS)
        .value("THE_BOOT", RelicId::THE_BOOT)
        .value("THE_SPECIMEN", RelicId::THE_SPECIMEN)
        .value("TINGSHA", RelicId::TINGSHA)
        .value("TOOLBOX", RelicId::TOOLBOX)
        .value("TORII", RelicId::TORII)
        .value("TOUGH_BANDAGES", RelicId::TOUGH_BANDAGES)
        .value("TOY_ORNITHOPTER", RelicId::TOY_ORNITHOPTER)
        .value("TUNGSTEN_ROD", RelicId::TUNGSTEN_ROD)
        .value("TURNIP", RelicId::TURNIP)
        .value("TWISTED_FUNNEL", RelicId::TWISTED_FUNNEL)
        .value("UNCEASING_TOP", RelicId::UNCEASING_TOP)
        .value("VELVET_CHOKER", RelicId::VELVET_CHOKER)
        .value("VIOLET_LOTUS", RelicId::VIOLET_LOTUS)
        .value("WARPED_TONGS", RelicId::WARPED_TONGS)
        .value("WRIST_BLADE", RelicId::WRIST_BLADE)
        .value("BLACK_BLOOD", RelicId::BLACK_BLOOD)
        .value("BURNING_BLOOD", RelicId::BURNING_BLOOD)
        .value("MEAT_ON_THE_BONE", RelicId::MEAT_ON_THE_BONE)
        .value("FACE_OF_CLERIC", RelicId::FACE_OF_CLERIC)
        .value("ANCHOR", RelicId::ANCHOR)
        .value("ANCIENT_TEA_SET", RelicId::ANCIENT_TEA_SET)
        .value("BAG_OF_MARBLES", RelicId::BAG_OF_MARBLES)
        .value("BAG_OF_PREPARATION", RelicId::BAG_OF_PREPARATION)
        .value("BLOOD_VIAL", RelicId::BLOOD_VIAL)
        .value("BOTTLED_FLAME", RelicId::BOTTLED_FLAME)
        .value("BOTTLED_LIGHTNING", RelicId::BOTTLED_LIGHTNING)
        .value("BOTTLED_TORNADO", RelicId::BOTTLED_TORNADO)
        .value("BRONZE_SCALES", RelicId::BRONZE_SCALES)
        .value("BUSTED_CROWN", RelicId::BUSTED_CROWN)
        .value("CLOCKWORK_SOUVENIR", RelicId::CLOCKWORK_SOUVENIR)
        .value("COFFEE_DRIPPER", RelicId::COFFEE_DRIPPER)
        .value("CRACKED_CORE", RelicId::CRACKED_CORE)
        .value("CURSED_KEY", RelicId::CURSED_KEY)
        .value("DAMARU", RelicId::DAMARU)
        .value("DATA_DISK", RelicId::DATA_DISK)
        .value("DU_VU_DOLL", RelicId::DU_VU_DOLL)
        .value("ENCHIRIDION", RelicId::ENCHIRIDION)
        .value("FOSSILIZED_HELIX", RelicId::FOSSILIZED_HELIX)
        .value("FUSION_HAMMER", RelicId::FUSION_HAMMER)
        .value("GIRYA", RelicId::GIRYA)
        .value("GOLD_PLATED_CABLES", RelicId::GOLD_PLATED_CABLES)
        .value("GREMLIN_VISAGE", RelicId::GREMLIN_VISAGE)
        .value("HOLY_WATER", RelicId::HOLY_WATER)
        .value("LANTERN", RelicId::LANTERN)
        .value("MARK_OF_PAIN", RelicId::MARK_OF_PAIN)
        .value("MUTAGENIC_STRENGTH", RelicId::MUTAGENIC_STRENGTH)
        .value("NEOWS_LAMENT", RelicId::NEOWS_LAMENT)
        .value("NINJA_SCROLL", RelicId::NINJA_SCROLL)
        .value("NUCLEAR_BATTERY", RelicId::NUCLEAR_BATTERY)
        .value("ODDLY_SMOOTH_STONE", RelicId::ODDLY_SMOOTH_STONE)
        .value("PANTOGRAPH", RelicId::PANTOGRAPH)
        .value("PRESERVED_INSECT", RelicId::PRESERVED_INSECT)
        .value("PURE_WATER", RelicId::PURE_WATER)
        .value("RED_MASK", RelicId::RED_MASK)
        .value("RING_OF_THE_SERPENT", RelicId::RING_OF_THE_SERPENT)
        .value("RING_OF_THE_SNAKE", RelicId::RING_OF_THE_SNAKE)
        .value("RUNIC_CAPACITOR", RelicId::RUNIC_CAPACITOR)
        .value("SLAVERS_COLLAR", RelicId::SLAVERS_COLLAR)
        .value("SLING_OF_COURAGE", RelicId::SLING_OF_COURAGE)
        .value("SYMBIOTIC_VIRUS", RelicId::SYMBIOTIC_VIRUS)
        .value("TEARDROP_LOCKET", RelicId::TEARDROP_LOCKET)
        .value("THREAD_AND_NEEDLE", RelicId::THREAD_AND_NEEDLE)
        .value("VAJRA", RelicId::VAJRA)
        .value("ASTROLABE", RelicId::ASTROLABE)
        .value("BLACK_STAR", RelicId::BLACK_STAR)
        .value("CALLING_BELL", RelicId::CALLING_BELL)
        .value("CAULDRON", RelicId::CAULDRON)
        .value("CULTIST_HEADPIECE", RelicId::CULTIST_HEADPIECE)
        .value("DOLLYS_MIRROR", RelicId::DOLLYS_MIRROR)
        .value("DREAM_CATCHER", RelicId::DREAM_CATCHER)
        .value("EMPTY_CAGE", RelicId::EMPTY_CAGE)
        .value("ETERNAL_FEATHER", RelicId::ETERNAL_FEATHER)
        .value("FROZEN_EGG", RelicId::FROZEN_EGG)
        .value("GOLDEN_IDOL", RelicId::GOLDEN_IDOL)
        .value("JUZU_BRACELET", RelicId::JUZU_BRACELET)
        .value("LEES_WAFFLE", RelicId::LEES_WAFFLE)
        .value("MANGO", RelicId::MANGO)
        .value("MATRYOSHKA", RelicId::MATRYOSHKA)
        .value("MAW_BANK", RelicId::MAW_BANK)
        .value("MEAL_TICKET", RelicId::MEAL_TICKET)
        .value("MEMBERSHIP_CARD", RelicId::MEMBERSHIP_CARD)
        .value("MOLTEN_EGG", RelicId::MOLTEN_EGG)
        .value("NLOTHS_GIFT", RelicId::NLOTHS_GIFT)
        .value("NLOTHS_HUNGRY_FACE", RelicId::NLOTHS_HUNGRY_FACE)
        .value("OLD_COIN", RelicId::OLD_COIN)
        .value("ORRERY", RelicId::ORRERY)
        .value("PANDORAS_BOX", RelicId::PANDORAS_BOX)
        .value("PEACE_PIPE", RelicId::PEACE_PIPE)
        .value("PEAR", RelicId::PEAR)
        .value("POTION_BELT", RelicId::POTION_BELT)
        .value("PRAYER_WHEEL", RelicId::PRAYER_WHEEL)
        .value("PRISMATIC_SHARD", RelicId::PRISMATIC_SHARD)
        .value("QUESTION_CARD", RelicId::QUESTION_CARD)
        .value("REGAL_PILLOW", RelicId::REGAL_PILLOW)
        .value("SSSERPENT_HEAD", RelicId::SSSERPENT_HEAD)
        .value("SHOVEL", RelicId::SHOVEL)
        .value("SINGING_BOWL", RelicId::SINGING_BOWL)
        .value("SMILING_MASK", RelicId::SMILING_MASK)
        .value("SPIRIT_POOP", RelicId::SPIRIT_POOP)
        .value("STRAWBERRY", RelicId::STRAWBERRY)
        .value("THE_COURIER", RelicId::THE_COURIER)
        .value("TINY_CHEST", RelicId::TINY_CHEST)
        .value("TINY_HOUSE", RelicId::TINY_HOUSE)
        .value("TOXIC_EGG", RelicId::TOXIC_EGG)
        .value("WAR_PAINT", RelicId::WAR_PAINT)
        .value("WHETSTONE", RelicId::WHETSTONE)
        .value("WHITE_BEAST_STATUE", RelicId::WHITE_BEAST_STATUE)
        .value("WING_BOOTS", RelicId::WING_BOOTS)
        .value("CIRCLET", RelicId::CIRCLET)
        .value("RED_CIRCLET", RelicId::RED_CIRCLET)
        .value("INVALID", RelicId::INVALID);

#ifdef VERSION_INFO
    m.attr("__version__") = MACRO_STRINGIFY(VERSION_INFO);
#else
    m.attr("__version__") = "dev";
#endif
}

// os.add_dll_directory("C:\\Program Files\\mingw-w64\\x86_64-8.1.0-posix-seh-rt_v6-rev0\\mingw64\\bin")


