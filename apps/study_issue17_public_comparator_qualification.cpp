#include <pybind11/embed.h>

#include "../bindings/slaythespire.cpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace pb = pybind11;

namespace {

constexpr int kAscension = 20;
constexpr int kSetupStepLimit = 512;
constexpr int kBattleDecisionLimit = 128;
constexpr int kDevelopmentSeedFirst = 1;
constexpr int kDevelopmentSeedLast = 48;
using Clock = std::chrono::steady_clock;
std::string canonicalJson(pb::handle value) {
    return pb::module_::import("json").attr("dumps")(
            pb::reinterpret_borrow<pb::object>(value),
            pb::arg("sort_keys") = true,
            pb::arg("separators") = pb::make_tuple(",", ":"))
            .cast<std::string>();
}

std::string indentedJson(pb::handle value) {
    return pb::module_::import("json").attr("dumps")(
            pb::reinterpret_borrow<pb::object>(value),
            pb::arg("sort_keys") = true,
            pb::arg("indent") = 2)
            .cast<std::string>();
}

std::string stringField(const pb::dict &value, const char *key) {
    return value[pb::str(key)].cast<std::string>();
}

int intField(const pb::dict &value, const char *key) {
    return value[pb::str(key)].cast<int>();
}

bool forbiddenPublicFieldName(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return name == "bits" || name == "unique_id" || name == "particle"
            || name == "particle_id" || name == "particle_index"
            || name == "hidden_draw_order" || name.find("rng") != std::string::npos
            || name.find("seed") != std::string::npos;
}

bool containsPrivateField(pb::handle value) {
    if (pb::isinstance<pb::dict>(value)) {
        const auto dict = pb::reinterpret_borrow<pb::dict>(value);
        for (const auto item : dict) {
            if (forbiddenPublicFieldName(pb::str(item.first).cast<std::string>())
                    || containsPrivateField(item.second)) {
                return true;
            }
        }
    } else if (pb::isinstance<pb::list>(value)
            || pb::isinstance<pb::tuple>(value)) {
        for (const auto item : pb::reinterpret_borrow<pb::sequence>(value)) {
            if (containsPrivateField(item)) {
                return true;
            }
        }
    }
    return false;
}

std::vector<pb::dict> publicActions(const pb::dict &state) {
    std::vector<pb::dict> actions;
    const auto identities = state["ordered_public_legal_actions"].cast<pb::list>();
    actions.reserve(static_cast<std::size_t>(identities.size()));
    for (const auto item : identities) {
        auto action = pb::reinterpret_borrow<pb::dict>(item);
        if (containsPrivateField(action)) {
            throw std::logic_error("public legal action contains a private field");
        }
        actions.push_back(action);
    }
    return actions;
}


struct ElitePath {
    bool found = false;
    int eliteY = std::numeric_limits<int>::max();
    int eliteX = std::numeric_limits<int>::max();
    int firstX = std::numeric_limits<int>::max();
    int restCount = 0;
    int interveningMonsterCount = 0;
};

bool betterElitePath(const ElitePath &candidate, const ElitePath &current) {
    if (!candidate.found) return false;
    if (!current.found) return true;
    if (candidate.eliteY != current.eliteY) return candidate.eliteY < current.eliteY;
    if (candidate.restCount != current.restCount) {
        return candidate.restCount > current.restCount;
    }
    if (candidate.interveningMonsterCount != current.interveningMonsterCount) {
        return candidate.interveningMonsterCount < current.interveningMonsterCount;
    }
    return std::tie(candidate.eliteX, candidate.firstX)
            < std::tie(current.eliteX, current.firstX);
}

void considerElitePath(
        const Map &map,
        int x,
        int y,
        int firstX,
        int restCount,
        int interveningMonsterCount,
        ElitePath &best) {
    const auto &node = map.getNode(x, y);
    if (node.room == Room::ELITE) {
        ElitePath candidate{true, y, x, firstX, restCount,
                interveningMonsterCount};
        if (betterElitePath(candidate, best)) {
            best = candidate;
        }
        return;
    }
    if (y >= 14) return;
    const int nextRestCount = restCount + (node.room == Room::REST);
    const int nextMonsterCount = interveningMonsterCount + (node.room == Room::MONSTER);
    for (int edge = 0; edge < node.edgeCount; ++edge) {
        considerElitePath(map, node.edges[edge], y + 1, firstX,
                nextRestCount, nextMonsterCount, best);
    }
}

LightSpeedAction chooseMapActionTowardElite(StepSimulator &simulator) {
    const auto actions = simulator.legalActions();
    if (actions.empty()) {
        throw std::runtime_error("map screen has no legal actions");
    }
    if (simulator.gc.map == nullptr) return actions.front();
    const int nextY = simulator.gc.curMapNodeY + 1;
    ElitePath best;
    int bestActionIndex = -1;
    for (int index = 0; index < static_cast<int>(actions.size()); ++index) {
        const auto &action = actions[index];
        if (action.kind != "map" || nextY < 0 || nextY >= 15) continue;
        const auto previousBest = best;
        considerElitePath(*simulator.gc.map, action.idx1, nextY, action.idx1,
                0, 0, best);
        if (betterElitePath(best, previousBest)) {
            bestActionIndex = index;
        }
    }
    return bestActionIndex < 0 ? actions.front() : actions[bestActionIndex];
}


int estimatedBlock(const pb::dict &card) {
    const auto name = stringField(card, "name");
    const bool upgraded = card["upgraded"].cast<bool>();
    if (name == "Defend") return upgraded ? 8 : 5;
    if (name == "Shrug It Off") return upgraded ? 11 : 8;
    if (name == "Ghostly Armor") return upgraded ? 13 : 10;
    if (name == "Flame Barrier") return upgraded ? 16 : 12;
    if (name == "Power Through") return 15;
    if (name == "Impervious") return upgraded ? 40 : 30;
    if (name == "True Grit") return upgraded ? 9 : 7;
    if (name == "Iron Wave") return upgraded ? 7 : 5;
    if (name == "Auto-Shields") return upgraded ? 15 : 11;
    return 0;
}

int estimatedAttackDamage(
        const pb::dict &card,
        const pb::dict &player,
        const pb::dict &monster) {
    const int cardId = intField(card, "id");
    const bool upgraded = card["upgraded"].cast<bool>();
    const int base = std::max(0, getBaseDamage(static_cast<CardId>(cardId), upgraded));
    int damage = std::max(0, base + intField(player, "strength"));
    if (intField(player, "weak") > 0) damage = damage * 3 / 4;
    if (intField(monster, "vulnerable") > 0) damage = damage * 3 / 2;
    // The estimate is HP damage after the target's visible block is applied.
    return std::max(0, damage - intField(monster, "block"));
}

pb::dict selectPublicHeuristicAction(const pb::dict &state, pb::dict *audit = nullptr) {
    if (containsPrivateField(state)) {
        throw std::logic_error("public heuristic received a private field");
    }
    const auto actions = state["ordered_public_legal_actions"].cast<pb::list>();
    const auto hand = state["hand"].cast<pb::list>();
    const auto monsters = state["monsters"].cast<pb::list>();
    const auto player = state["player"].cast<pb::dict>();
    const auto resources = state["persistent_resources"].cast<pb::dict>();
    const auto potions = resources["potions"].cast<pb::list>();
    int incomingDamage = 0;
    for (const auto monsterHandle : monsters) {
        const auto monster = pb::reinterpret_borrow<pb::dict>(monsterHandle);
        if (!monster["alive"].cast<bool>() || !monster["attacking"].cast<bool>()) continue;
        incomingDamage += intField(monster, "move_base_damage")
                * intField(monster, "move_hits");
    }
    const int currentBlock = intField(player, "block");
    const int hp = intField(player, "current_hp");
    const int maxHp = std::max(1, intField(player, "max_hp"));
    bool gremlinNobAlive = false;
    bool safeSetupWindow = true;
    for (const auto monsterHandle : monsters) {
        const auto monster = pb::reinterpret_borrow<pb::dict>(monsterHandle);
        if (!monster["alive"].cast<bool>()) continue;
        gremlinNobAlive = gremlinNobAlive
                || stringField(monster, "id_label") == "GREMLIN_NOB";
        safeSetupWindow = safeSetupWindow && !monster["attacking"].cast<bool>();
    }

    int bestIndex = -1;
    double bestScore = -std::numeric_limits<double>::infinity();
    int bestTargetTie = std::numeric_limits<int>::min();
    std::string bestReason;
    int bestEstimatedDamage = 0;
    int bestEstimatedBlock = 0;
    pb::list powerActionScores;
    for (int index = 0; index < actions.size(); ++index) {
        const auto action = pb::reinterpret_borrow<pb::dict>(actions[index]);
        const auto kind = stringField(action, "kind");
        double score = -std::numeric_limits<double>::infinity();
        int targetTie = 0;
        int estimatedDamage = 0;
        int estimatedBlockValue = 0;
        std::string reason;
        if (kind == "potion") {
            const int potionIndex = intField(action, "idx1");
            if (potionIndex >= 0 && potionIndex < potions.size()) {
                const auto potion = pb::reinterpret_borrow<pb::dict>(potions[potionIndex]);
                const auto potionName = stringField(potion, "name");
                const int risk = std::max(0, incomingDamage - currentBlock);
                if (potionName == "Blood Potion"
                        && hp * 100 <= maxHp * 35
                        && maxHp - hp >= maxHp / 5) {
                    score = 400.0;
                    reason = "heal only below 35% HP with meaningful missing health";
                } else if (potionName == "Block Potion"
                        && risk > 0
                        && (risk >= hp || hp - risk <= maxHp / 5)) {
                    score = 390.0;
                    reason = "block potion only when visible incoming damage is dangerous";
                } else if (potionName == "Fire Potion") {
                    const int target = intField(action, "idx2");
                    if (target >= 0 && target < monsters.size()) {
                        const auto monster = pb::reinterpret_borrow<pb::dict>(monsters[target]);
                        if (monster["alive"].cast<bool>()
                                && intField(monster, "current_hp")
                                        + intField(monster, "block") <= 20) {
                            score = 350.0;
                            targetTie = -intField(monster, "current_hp");
                            reason = "fire potion secures a visible lethal";
                        }
                    }
                }
            }
        } else if (kind == "card") {
            const int source = intField(action, "idx1");
            if (source < 0 || source >= hand.size()) continue;
            const auto card = pb::reinterpret_borrow<pb::dict>(hand[source]);
            const auto type = stringField(card, "type");
            const int cost = intField(card, "cost_for_turn");
            const int energyCost = card["free_to_play_once"].cast<bool>()
                    ? 0 : std::max(0, cost);
            const int target = intField(action, "idx2");
            if (type == "ATTACK" || (type == "SKILL" && estimatedBlock(card) > 0)) {
                int selectedDamage = 0;
                int selectedTargetHp = std::numeric_limits<int>::max();
                int selectedTargetEffectiveHp = std::numeric_limits<int>::max();
                for (int monsterIndex = 0; monsterIndex < monsters.size(); ++monsterIndex) {
                    if (target >= 0 && target < monsters.size() && target != monsterIndex) continue;
                    const auto monster = pb::reinterpret_borrow<pb::dict>(monsters[monsterIndex]);
                    if (!monster["alive"].cast<bool>() || !monster["targetable"].cast<bool>()) continue;
                    const int damage = estimatedAttackDamage(card, player, monster);
                    const int targetHp = intField(monster, "current_hp");
                    const int targetEffectiveHp = targetHp + intField(monster, "block");
                    if (damage > selectedDamage
                            || (damage == selectedDamage
                                    && targetEffectiveHp < selectedTargetEffectiveHp)) {
                        selectedDamage = damage;
                        selectedTargetHp = targetHp;
                        selectedTargetEffectiveHp = targetEffectiveHp;
                        targetTie = -targetEffectiveHp;
                    }
                }
                estimatedDamage = selectedDamage;
                score = (type == "ATTACK" ? 35.0 : 20.0)
                        + std::min(estimatedDamage, 30) * 2.0 - energyCost * 10.0;
                if (selectedTargetHp != std::numeric_limits<int>::max()
                        && estimatedDamage >= selectedTargetHp) {
                    score += 100.0;
                    reason = "estimated immediate lethal from public card damage and target HP";
                } else {
                    reason = "public card damage weighed against energy cost";
                }
                estimatedBlockValue = estimatedBlock(card);
                const int uncovered = std::max(0, incomingDamage - currentBlock);
                const int prevented = std::min(uncovered, estimatedBlockValue);
                score += prevented * 3.0;
                if (uncovered >= hp && prevented > 0) score += 45.0;
                if (prevented > 0) reason += "; visible incoming damage makes block valuable";
            } else if (type == "SKILL") {
                estimatedBlockValue = estimatedBlock(card);
                const int uncovered = std::max(0, incomingDamage - currentBlock);
                const int prevented = std::min(uncovered, estimatedBlockValue);
                score = 12.0 + prevented * 3.0 - energyCost * 10.0;
                if (uncovered >= hp && prevented > 0) score += 45.0;
                reason = prevented > 0
                        ? "spend energy on public defensive value against visible intent"
                        : "defer non-defensive setup when no visible damage requires it";
            } else if (type == "POWER") {
                score = safeSetupWindow && !gremlinNobAlive ? 32.0 : 8.0;
                score -= energyCost * 10.0;
                if (gremlinNobAlive) {
                    score -= 50.0;
                    reason = "defer Powers against publicly identified Gremlin Nob";
                } else {
                    reason = safeSetupWindow
                            ? "use setup only in a visible non-attacking window"
                            : "preserve energy for current-turn tactics";
                }
            } else {
                score = 5.0 - energyCost * 10.0;
                reason = "low-priority non-tactical card";
            }
            if (type == "POWER") {
                pb::dict scoreRow;
                scoreRow["action_index"] = index;
                scoreRow["public_action"] = action;
                scoreRow["priority_score"] = score;
                scoreRow["nob_identity_penalty_applied"] = gremlinNobAlive;
                scoreRow["branch"] = gremlinNobAlive
                        ? "identified_gremlin_nob"
                        : safeSetupWindow ? "visible_non_attacking_setup_window"
                                          : "visible_attacking_window";
                powerActionScores.append(scoreRow);
            }
            if (target >= 0 && target < monsters.size()) {
                const auto monster = pb::reinterpret_borrow<pb::dict>(monsters[target]);
                if (monster["alive"].cast<bool>() && monster["attacking"].cast<bool>()) {
                    targetTie = intField(monster, "move_base_damage")
                            * intField(monster, "move_hits");
                }
            }
        } else if (kind == "end_turn") {
            score = 0;
            reason = "end turn when no tactical action has positive value";
        } else if (kind == "single_card_select" || kind == "multi_card_select") {
            score = 1;
            targetTie = -intField(action, "idx1");
            reason = "deterministic first legal choice for generic card selection";
        }
        if (score > bestScore || (score == bestScore && targetTie > bestTargetTie)) {
            bestScore = score;
            bestTargetTie = targetTie;
            bestIndex = index;
            bestReason = reason;
            bestEstimatedDamage = estimatedDamage;
            bestEstimatedBlock = estimatedBlockValue;
        }
    }
    if (bestIndex < 0) {
        throw std::runtime_error("public heuristic found no usable legal action");
    }
    if (audit != nullptr) {
        (*audit)["visible_incoming_damage"] = incomingDamage;
        (*audit)["current_block"] = currentBlock;
        (*audit)["priority_score"] = bestScore;
        (*audit)["estimated_card_damage"] = bestEstimatedDamage;
        (*audit)["estimated_card_block"] = bestEstimatedBlock;
        (*audit)["priority_reason"] = bestReason;
        (*audit)["gremlin_nob_alive_by_canonical_public_id"] = gremlinNobAlive;
        (*audit)["power_action_scores"] = powerActionScores;
        (*audit)["heuristic_id"] =
                "public-tactical-v1-blocked-lethal-canonical-monster-id";
    }
    return pb::reinterpret_borrow<pb::dict>(actions[bestIndex]);
}


pb::dict mockPublicAction(const std::string &kind, const int source, const int target) {
    pb::dict action;
    action["kind"] = kind;
    action["idx1"] = source;
    action["idx2"] = target;
    action["idx3"] = 0;
    return action;
}

pb::dict mockCard(const CardId id, const char *name, const char *type, const int cost) {
    pb::dict card;
    card["id"] = static_cast<int>(id);
    card["name"] = name;
    card["type"] = type;
    card["cost_for_turn"] = cost;
    card["upgraded"] = false;
    card["free_to_play_once"] = false;
    return card;
}

pb::dict mockMonster(
        const char *name, const int hp, const int incoming, const bool attacking) {
    pb::dict monster;
    monster["name"] = name;
    monster["id_label"] = std::string(name) == "GREMLIN_NOB"
            ? "GREMLIN_NOB" : "JAW_WORM";
    monster["current_hp"] = hp;
    monster["max_hp"] = std::max(1, hp);
    monster["block"] = 0;
    monster["alive"] = true;
    monster["targetable"] = true;
    monster["attacking"] = attacking;
    monster["move_base_damage"] = incoming;
    monster["move_hits"] = 1;
    monster["vulnerable"] = 0;
    return monster;
}

pb::dict mockPublicState(
        const int hp,
        const std::vector<pb::dict> &cards,
        const std::vector<pb::dict> &monsters,
        const std::vector<pb::dict> &actions,
        const std::vector<pb::dict> &potions = {}) {
    pb::dict player;
    player["current_hp"] = hp;
    player["max_hp"] = 80;
    player["block"] = 0;
    player["strength"] = 0;
    player["weak"] = 0;
    pb::list hand;
    for (const auto &card : cards) hand.append(card);
    pb::list monsterRows;
    for (const auto &monster : monsters) monsterRows.append(monster);
    pb::list actionRows;
    for (const auto &action : actions) actionRows.append(action);
    pb::list potionRows;
    for (const auto &potion : potions) potionRows.append(potion);
    pb::dict resources;
    resources["potions"] = potionRows;
    pb::dict state;
    state["player"] = player;
    state["hand"] = hand;
    state["monsters"] = monsterRows;
    state["ordered_public_legal_actions"] = actionRows;
    state["persistent_resources"] = resources;
    return state;
}

void runHeuristicSanityChecks() {
    const auto endTurn = mockPublicAction("end_turn", 0, 0);
    {
        const auto strike = mockCard(CardId::STRIKE_RED, "Strike", "ATTACK", 1);
        const auto state = mockPublicState(70, {strike},
                {mockMonster("JAW_WORM", 5, 0, false)},
                {mockPublicAction("card", 0, 0), endTurn});
        const auto selected = selectPublicHeuristicAction(state);
        if (stringField(selected, "kind") != "card") {
            throw std::logic_error("public heuristic missed a visible immediate lethal");
        }
    }
    {
        const auto strike = mockCard(CardId::STRIKE_RED, "Strike", "ATTACK", 1);
        const auto inflame = mockCard(CardId::INFLAME, "Inflame", "POWER", 0);
        auto blockedTarget = mockMonster("JAW_WORM", 3, 0, false);
        blockedTarget["block"] = 3;
        const auto state = mockPublicState(70, {strike, inflame},
                {blockedTarget},
                {mockPublicAction("card", 0, 0), mockPublicAction("card", 1, 0), endTurn});
        pb::dict audit;
        const auto selected = selectPublicHeuristicAction(state, &audit);
        if (intField(selected, "idx1") != 0
                || intField(audit, "estimated_card_damage") != 3
                || stringField(audit, "priority_reason").find("immediate lethal")
                        == std::string::npos) {
            throw std::logic_error("public heuristic missed a lethal against visible target block");
        }
    }
    {
        const auto strike = mockCard(CardId::STRIKE_RED, "Strike", "ATTACK", 1);
        const auto defend = mockCard(CardId::DEFEND_RED, "Defend", "SKILL", 1);
        const auto state = mockPublicState(20, {strike, defend},
                {mockMonster("JAW_WORM", 40, 20, true)},
                {mockPublicAction("card", 0, 0), mockPublicAction("card", 1, 0), endTurn});
        auto selected = selectPublicHeuristicAction(state);
        if (intField(selected, "idx1") != 1) {
            throw std::logic_error("public heuristic did not prioritize block under lethal pressure");
        }
    }
    {
        const auto strike = mockCard(CardId::STRIKE_RED, "Strike", "ATTACK", 1);
        const auto power = mockCard(CardId::INFLAME, "Inflame", "POWER", 1);
        const auto state = mockPublicState(70, {strike, power},
                {mockMonster("GREMLIN_NOB", 40, 0, false)},
                {mockPublicAction("card", 0, 0), mockPublicAction("card", 1, 0), endTurn});
        const auto selected = selectPublicHeuristicAction(state);
        if (intField(selected, "idx1") != 0) {
            throw std::logic_error("public heuristic did not defer setup against Gremlin Nob");
        }
    }
    {
        pb::dict firePotion;
        firePotion["name"] = "Fire Potion";
        const auto strike = mockCard(CardId::STRIKE_RED, "Strike", "ATTACK", 1);
        const auto state = mockPublicState(70, {strike},
                {mockMonster("JAW_WORM", 80, 0, false)},
                {mockPublicAction("card", 0, 0), mockPublicAction("potion", 0, 0), endTurn},
                {firePotion});
        const auto selected = selectPublicHeuristicAction(state);
        if (stringField(selected, "kind") != "card") {
            throw std::logic_error("public heuristic spent a damage potion without a lethal");
        }
    }
    {
        pb::dict bloodPotion;
        bloodPotion["name"] = "Blood Potion";
        const auto strike = mockCard(CardId::STRIKE_RED, "Strike", "ATTACK", 1);
        const auto state = mockPublicState(20, {strike},
                {mockMonster("JAW_WORM", 80, 0, false)},
                {mockPublicAction("card", 0, 0), mockPublicAction("potion", 0, 0), endTurn},
                {bloodPotion});
        const auto selected = selectPublicHeuristicAction(state);
        if (stringField(selected, "kind") != "potion") {
            throw std::logic_error("public heuristic did not preserve low-HP healing policy");
        }
    }
    {
        const auto strike = mockCard(CardId::STRIKE_RED, "Strike", "ATTACK", 1);
        auto state = mockPublicState(70, {strike},
                {mockMonster("JAW_WORM", 40, 0, false)},
                {mockPublicAction("card", 0, 0), endTurn});
        state["particle_index"] = 0;
        bool rejectedPrivateInput = false;
        try {
            (void) selectPublicHeuristicAction(state);
        } catch (const std::logic_error &) {
            rejectedPrivateInput = true;
        }
        if (!rejectedPrivateInput) {
            throw std::logic_error("public heuristic accepted private particle metadata");
        }
    }
    std::cout << "ISSUE17_PUBLIC_HEURISTIC_SANITY_PASS cases=7\n";
}


struct PreparedBattleStart {
    int gameSeed = 0;
    bool battleStarted = false;
    std::shared_ptr<StepSimulatorCheckpoint> checkpoint;
    pb::dict battleStartIdentity;
    pb::dict evidence;
    double setupWallSeconds = 0.0;
};

pb::dict battleStartIdentityFromPublicState(
        const int gameSeed,
        const pb::dict &startState) {
    pb::dict identity;
    identity["game_seed"] = gameSeed;
    identity["ascension"] = kAscension;
    identity["act"] = intField(startState, "act");
    identity["floor_num"] = intField(startState, "floor_num");
    identity["encounter_id"] = stringField(startState, "encounter_id");
    identity["information_fidelity"] = stringField(startState, "information_fidelity");
    identity["input_state"] = stringField(startState, "input_state");
    identity["player"] = startState["player"];
    identity["hand"] = startState["hand"];
    identity["monsters"] = startState["monsters"];
    identity["draw_pile_size"] = startState["draw_pile_size"];
    identity["draw_pile_membership"] = startState["draw_pile_membership"];
    identity["persistent_resources"] = startState["persistent_resources"];
    return identity;
}

void copyDictEntries(const pb::dict &source, pb::dict &destination) {
    for (const auto item : source) {
        destination[item.first] = item.second;
    }
}

PreparedBattleStart prepareBattleStart(const int gameSeed) {
    PreparedBattleStart prepared;
    prepared.gameSeed = gameSeed;
    prepared.evidence = pb::dict();
    const auto setupStart = Clock::now();
    try {
        StepSimulator simulator(CharacterClass::IRONCLAD,
                static_cast<std::uint64_t>(gameSeed), kAscension);
        int setupSteps = 0;
        int preludeBattleCount = 0;
        int preludeDecisionCount = 0;
        bool wasInPreludeBattle = false;
        bool reachedTargetBattle = false;
        for (; setupSteps < kSetupStepLimit; ++setupSteps) {
            const auto snapshot = simulator.snapshot();
            if (stringField(snapshot, "screen_state") == "BATTLE"
                    && snapshot["battle_active"].cast<bool>()) {
                if (simulator.gc.act == 1 && simulator.gc.curRoom == Room::ELITE) {
                    reachedTargetBattle = true;
                    break;
                }
                if (!wasInPreludeBattle) ++preludeBattleCount;
                wasInPreludeBattle = true;
                const auto preludeState = simulator.publicBattleState();
                if (containsPrivateField(preludeState)) {
                    throw std::logic_error("prelude public state contains private fields");
                }
                const auto preludeAction = selectPublicHeuristicAction(preludeState);
                (void) simulator.stepPublicAction(preludeAction);
                ++preludeDecisionCount;
                continue;
            }
            wasInPreludeBattle = false;
            const auto actions = simulator.legalActions();
            if (actions.empty()) {
                break;
            }
            if (stringField(snapshot, "screen_state") == "MAP_SCREEN"
                    && simulator.gc.act == 1) {
                (void) simulator.step(chooseMapActionTowardElite(simulator));
            } else {
                (void) simulator.step(actions.front());
            }
        }

        prepared.evidence["pre_battle_setup_steps"] = setupSteps;
        prepared.evidence["pre_battle_setup_step_limit"] = kSetupStepLimit;
        prepared.evidence["prelude_battles"] = preludeBattleCount;
        prepared.evidence["prelude_public_decisions"] = preludeDecisionCount;
        if (!reachedTargetBattle) {
            const auto finalSnapshot = simulator.snapshot();
            prepared.evidence["setup_status"] =
                    simulator.gc.outcome == GameOutcome::PLAYER_LOSS
                            ? "prelude_player_loss_before_act1_elite"
                            : "act1_elite_not_reached";
            prepared.evidence["setup_final_screen"] = stringField(finalSnapshot, "screen_state");
            prepared.evidence["setup_final_game_outcome"] = stringField(finalSnapshot, "outcome");
            prepared.evidence["setup_final_act"] = intField(finalSnapshot, "act");
            prepared.evidence["setup_final_floor"] = intField(finalSnapshot, "floor_num");
            prepared.evidence["setup_final_player_hp"] = intField(finalSnapshot, "cur_hp");
        } else {
            auto startState = simulator.publicBattleState();
            if (containsPrivateField(startState)) {
                throw std::logic_error("battle start public state contains private fields");
            }
            if (stringField(startState, "information_fidelity") != "supported") {
                throw std::logic_error("target battle start has unsupported public information");
            }
            prepared.battleStarted = true;
            prepared.battleStartIdentity = battleStartIdentityFromPublicState(
                    gameSeed, startState);
            prepared.checkpoint = std::make_shared<StepSimulatorCheckpoint>(
                    simulator.captureCheckpoint());
            const auto player = startState["player"].cast<pb::dict>();
            prepared.evidence["setup_status"] = "battle_started";
            prepared.evidence["battle_start_identity_ref"] = gameSeed;
            prepared.evidence["encounter_id"] = stringField(startState, "encounter_id");
            prepared.evidence["act"] = intField(startState, "act");
            prepared.evidence["floor_num"] = intField(startState, "floor_num");
            prepared.evidence["starting_player_hp"] = intField(player, "current_hp");
            prepared.evidence["starting_player_max_hp"] = intField(player, "max_hp");
            prepared.evidence["public_start_information_fidelity"] =
                    stringField(startState, "information_fidelity");
            prepared.evidence["battle_start_identity"] = prepared.battleStartIdentity;
        }
    } catch (const std::exception &error) {
        prepared.battleStarted = false;
        prepared.checkpoint.reset();
        prepared.evidence["setup_status"] = "setup_error";
        prepared.evidence["setup_error"] = error.what();
    }
    prepared.setupWallSeconds = std::chrono::duration<double>(
            Clock::now() - setupStart).count();
    prepared.evidence["prelude_setup_wall_seconds"] = prepared.setupWallSeconds;
    return prepared;
}

void requireQualification(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

bool hasEligiblePowerAndCompetitor(const pb::dict &state, int *powerHandIndex) {
    const auto hand = state["hand"].cast<pb::list>();
    const auto actions = state["ordered_public_legal_actions"].cast<pb::list>();
    int candidatePowerIndex = -1;
    bool hasOtherAction = false;
    for (const auto actionHandle : actions) {
        const auto action = pb::reinterpret_borrow<pb::dict>(actionHandle);
        if (stringField(action, "kind") != "card") {
            hasOtherAction = true;
            continue;
        }
        const int handIndex = intField(action, "idx1");
        if (handIndex < 0 || handIndex >= hand.size()) {
            throw std::logic_error("public card action index is outside the emitted hand");
        }
        const auto card = pb::reinterpret_borrow<pb::dict>(hand[handIndex]);
        if (stringField(card, "type") == "POWER") candidatePowerIndex = handIndex;
        else hasOtherAction = true;
    }
    if (candidatePowerIndex < 0 || !hasOtherAction) return false;
    if (powerHandIndex != nullptr) *powerHandIndex = candidatePowerIndex;
    return true;
}

pb::dict copyWithNonNobCanonicalId(const pb::dict &state) {
    pb::dict counterfactual;
    for (const auto item : state) counterfactual[item.first] = item.second;
    const auto monsters = state["monsters"].cast<pb::list>();
    pb::list changedMonsters;
    bool changed = false;
    for (const auto monsterHandle : monsters) {
        pb::dict monster;
        for (const auto item : pb::reinterpret_borrow<pb::dict>(monsterHandle)) {
            monster[item.first] = item.second;
        }
        if (!changed && stringField(monster, "id_label") == "GREMLIN_NOB") {
            // Keep all tactical facts and the display name fixed. Change only the
            // canonical identity pair so this tests the typed-ID branch.
            monster["id"] = static_cast<int>(MonsterId::JAW_WORM);
            monster["id_label"] = "JAW_WORM";
            changed = true;
        }
        changedMonsters.append(monster);
    }
    requireQualification(changed, "real public Nob identity was absent from causal fixture");
    counterfactual["encounter_id"] = "JAW_WORM";
    counterfactual["monsters"] = changedMonsters;
    return counterfactual;
}

pb::dict powerScoreRow(const pb::dict &audit, const int handIndex) {
    const auto rows = audit["power_action_scores"].cast<pb::list>();
    for (const auto rowHandle : rows) {
        const auto row = pb::reinterpret_borrow<pb::dict>(rowHandle);
        const auto action = row["public_action"].cast<pb::dict>();
        if (intField(action, "idx1") == handIndex) return row;
    }
    throw std::runtime_error("eligible public Power has no recorded priority score");
}

pb::dict qualifyCausalNobBranch(const pb::dict &realNobState, const int powerHandIndex) {
    requireQualification(!containsPrivateField(realNobState),
            "real Nob fixture contains a private field");
    const auto counterfactual = copyWithNonNobCanonicalId(realNobState);
    requireQualification(!containsPrivateField(counterfactual),
            "non-Nob public counterfactual contains a private field");
    pb::dict nobAudit;
    pb::dict nonNobAudit;
    (void) selectPublicHeuristicAction(realNobState, &nobAudit);
    (void) selectPublicHeuristicAction(counterfactual, &nonNobAudit);
    requireQualification(nobAudit["gremlin_nob_alive_by_canonical_public_id"].cast<bool>(),
            "real GREMLIN_NOB id did not execute the Nob branch");
    requireQualification(!nonNobAudit[
                    "gremlin_nob_alive_by_canonical_public_id"].cast<bool>(),
            "non-Nob canonical id still executed the Nob branch");
    const auto nobPower = powerScoreRow(nobAudit, powerHandIndex);
    const auto nonNobPower = powerScoreRow(nonNobAudit, powerHandIndex);
    const double nobScore = nobPower["priority_score"].cast<double>();
    const double nonNobScore = nonNobPower["priority_score"].cast<double>();
    const double scoreDelta = nonNobScore - nobScore;
    requireQualification(nobPower["nob_identity_penalty_applied"].cast<bool>()
                    && !nonNobPower["nob_identity_penalty_applied"].cast<bool>()
                    && scoreDelta >= 49.999,
            "Power priority score did not change under the canonical Nob identity intervention");

    pb::dict evidence;
    const auto monsters = realNobState["monsters"].cast<pb::list>();
    pb::dict realNob;
    for (const auto monsterHandle : monsters) {
        const auto monster = pb::reinterpret_borrow<pb::dict>(monsterHandle);
        if (stringField(monster, "id_label") == "GREMLIN_NOB") {
            realNob = monster;
            break;
        }
    }
    evidence["actual_encounter_id"] = stringField(realNobState, "encounter_id");
    evidence["actual_monster_id"] = intField(realNob, "id");
    evidence["actual_monster_id_label"] = stringField(realNob, "id_label");
    evidence["actual_monster_name"] = stringField(realNob, "name");
    evidence["counterfactual_changes"] = "encounter_id plus monster id and id_label changed to JAW_WORM; display name, HP/block, intent, hand, legal actions, and all other public fields unchanged";
    evidence["power_hand_index"] = powerHandIndex;
    evidence["power_card"] = realNobState["hand"].cast<pb::list>()[powerHandIndex];
    evidence["nob_branch"] = nobPower["branch"];
    evidence["nob_power_priority_score"] = nobScore;
    evidence["non_nob_branch"] = nonNobPower["branch"];
    evidence["non_nob_power_priority_score"] = nonNobScore;
    evidence["non_nob_minus_nob_power_score"] = scoreDelta;
    evidence["nob_penalty_applied"] = nobPower["nob_identity_penalty_applied"];
    evidence["non_nob_penalty_applied"] = nonNobPower["nob_identity_penalty_applied"];
    return evidence;
}

void validateNativeSnapshotSchema(const pb::dict &state) {
    requireQualification(!containsPrivateField(state),
            "native normal-public snapshot contained a private field");
    requireQualification(stringField(state, "schema_id") == "native-public-battle-state-v1",
            "unexpected native public battle schema id");
    requireQualification(stringField(state, "information_regime") == "normal_public",
            "snapshot is not in the normal-public information regime");
    requireQualification(stringField(state, "information_fidelity") == "supported",
            "snapshot public information is unsupported");

    const auto player = state["player"].cast<pb::dict>();
    for (const char *key : {"current_hp", "max_hp", "block", "energy", "strength", "weak"}) {
        (void) player[pb::str(key)].cast<int>();
    }
    const auto hand = state["hand"].cast<pb::list>();
    for (const auto cardHandle : hand) {
        const auto card = pb::reinterpret_borrow<pb::dict>(cardHandle);
        for (const char *key : {"id", "cost_for_turn"}) {
            (void) card[pb::str(key)].cast<int>();
        }
        for (const char *key : {"name", "type"}) {
            (void) card[pb::str(key)].cast<std::string>();
        }
        (void) card["upgraded"].cast<bool>();
        (void) card["free_to_play_once"].cast<bool>();
    }
    const auto monsters = state["monsters"].cast<pb::list>();
    requireQualification(monsters.size() > 0, "native public snapshot has no monsters");
    for (const auto monsterHandle : monsters) {
        const auto monster = pb::reinterpret_borrow<pb::dict>(monsterHandle);
        (void) monster["id"].cast<int>();
        for (const char *key : {"id_label", "name", "intent_category"}) {
            (void) monster[pb::str(key)].cast<std::string>();
        }
        for (const char *key : {"current_hp", "max_hp", "block"}) {
            (void) monster[pb::str(key)].cast<int>();
        }
        (void) monster["alive"].cast<bool>();
        (void) monster["targetable"].cast<bool>();
        (void) monster["attacking"].cast<bool>();
        (void) monster["move_base_damage"].cast<int>();
        (void) monster["move_hits"].cast<int>();
    }
    const auto resources = state["persistent_resources"].cast<pb::dict>();
    const auto potions = resources["potions"].cast<pb::list>();
    for (const auto potionHandle : potions) {
        const auto potion = pb::reinterpret_borrow<pb::dict>(potionHandle);
        (void) potion["name"].cast<std::string>();
        (void) potion["id_label"].cast<std::string>();
        (void) potion["potion_index"].cast<int>();
    }
    const auto actions = state["ordered_public_legal_actions"].cast<pb::list>();
    requireQualification(actions.size() > 0, "native public snapshot has no legal actions");
    for (const auto actionHandle : actions) {
        const auto action = pb::reinterpret_borrow<pb::dict>(actionHandle);
        (void) action["scope"].cast<std::string>();
        (void) action["kind"].cast<std::string>();
        for (const char *key : {"idx1", "idx2", "idx3"}) {
            (void) action[pb::str(key)].cast<int>();
        }
    }
}

pb::dict summarizeRealSnapshot(const pb::dict &state, const int gameSeed,
        const int decisionIndex, const pb::dict &selectedAction, const pb::dict &audit) {
    pb::dict summary;
    summary["development_seed"] = gameSeed;
    summary["act"] = intField(state, "act");
    summary["floor_num"] = intField(state, "floor_num");
    summary["turn"] = intField(state, "turn");
    summary["decision_index_in_elite_battle"] = decisionIndex;
    summary["encounter_id"] = stringField(state, "encounter_id");
    summary["schema_id"] = stringField(state, "schema_id");
    summary["information_regime"] = stringField(state, "information_regime");
    summary["information_fidelity"] = stringField(state, "information_fidelity");
    const auto player = state["player"].cast<pb::dict>();
    pb::dict playerRow;
    for (const char *key : {"current_hp", "max_hp", "block", "energy", "weak"}) {
        playerRow[key] = player[pb::str(key)];
    }
    summary["player"] = playerRow;

    const auto hand = state["hand"].cast<pb::list>();
    pb::list handRows;
    for (const auto cardHandle : hand) {
        const auto card = pb::reinterpret_borrow<pb::dict>(cardHandle);
        pb::dict row;
        for (const char *key : {"id", "name", "type", "cost_for_turn", "upgraded",
                     "free_to_play_once", "playable"}) {
            row[key] = card[pb::str(key)];
        }
        handRows.append(row);
    }
    summary["hand"] = handRows;

    const auto resources = state["persistent_resources"].cast<pb::dict>();
    const auto potions = resources["potions"].cast<pb::list>();
    pb::list potionRows;
    for (const auto potionHandle : potions) {
        const auto potion = pb::reinterpret_borrow<pb::dict>(potionHandle);
        pb::dict row;
        for (const char *key : {"potion_index", "id", "id_label", "name"}) {
            row[key] = potion[pb::str(key)];
        }
        potionRows.append(row);
    }
    summary["potions"] = potionRows;

    const auto monsters = state["monsters"].cast<pb::list>();
    pb::list monsterRows;
    for (const auto monsterHandle : monsters) {
        const auto monster = pb::reinterpret_borrow<pb::dict>(monsterHandle);
        pb::dict row;
        for (const char *key : {"id", "id_label", "name", "current_hp", "max_hp", "block",
                     "alive", "targetable", "attacking", "intent_category", "current_move",
                     "move_base_damage", "move_hits"}) {
            if (monster.contains(pb::str(key))) row[key] = monster[pb::str(key)];
        }
        monsterRows.append(row);
    }
    summary["monsters"] = monsterRows;

    const auto actions = state["ordered_public_legal_actions"].cast<pb::list>();
    pb::list actionRows;
    std::set<std::string> actionKinds;
    for (const auto actionHandle : actions) {
        const auto action = pb::reinterpret_borrow<pb::dict>(actionHandle);
        actionKinds.insert(stringField(action, "kind"));
        if (actionRows.size() < 8) actionRows.append(action);
    }
    pb::list actionKindRows;
    for (const auto &kind : actionKinds) actionKindRows.append(kind);
    summary["public_action_count"] = actions.size();
    summary["public_action_kinds"] = actionKindRows;
    summary["sample_public_actions"] = actionRows;
    summary["selected_legal_action"] = selectedAction;
    summary["selected_priority_score"] = audit["priority_score"];
    summary["selected_priority_reason"] = audit["priority_reason"];
    return summary;
}

pb::dict runPublicComparatorQualification(const std::string &nativeBaseSha,
        const std::string &sourceMaterialSha, std::ofstream &progress) {
    runHeuristicSanityChecks();
    pb::dict report;
    report["schema_id"] = "issue17-stage-a-public-comparator-qualification-v1";
    report["native_base_branch"] = "spire/main";
    report["native_base_commit"] = nativeBaseSha;
    report["issue16_source_material_commit"] = sourceMaterialSha;
    report["baseline_id"] = "public-tactical-v1-blocked-lethal-canonical-monster-id";
    report["information_regime"] = "normal_public";
    report["cohort_role"] = "development/schema qualification only; seeds 1..48 were previously exposed";
    report["future_seed_outcomes_consumed"] = false;
    report["nob_identity_field"] = "public monster id_label == GREMLIN_NOB";
    report["display_name_used_for_nob_identity"] = false;
    report["power_deferral_semantics"] = "a frozen heuristic score preference only; not a simulator rule or a claim that Gremlin Nob mechanically punishes Powers";

    pb::list developmentSeedsTried;
    std::map<std::string, pb::dict> examplesByEncounter;
    pb::dict causalEvidence;
    bool haveCausalPowerExample = false;
    int checkedNativeEliteSnapshots = 0;
    std::set<std::string> observedCardNameTypes;
    std::set<std::string> observedPotionNames;
    std::set<std::string> observedMonsterIdentities;
    std::set<std::string> observedActionKinds;
    const std::set<std::string> act1Elites{
            "GREMLIN_NOB", "LAGAVULIN", "THREE_SENTRIES"};
    for (int gameSeed = kDevelopmentSeedFirst;
            gameSeed <= kDevelopmentSeedLast; ++gameSeed) {
        pb::dict progressRow;
        progressRow["stage"] = "development_seed_started";
        progressRow["game_seed"] = gameSeed;
        progress << canonicalJson(progressRow) << '\n' << std::flush;
        if (!progress) throw std::runtime_error("failed flushing development seed progress");
        developmentSeedsTried.append(gameSeed);
        auto prepared = prepareBattleStart(gameSeed);
        progressRow["stage"] = "development_seed_setup_finished";
        progressRow["setup_status"] = stringField(prepared.evidence, "setup_status");
        if (prepared.battleStarted) {
            progressRow["encounter_id"] =
                    stringField(prepared.battleStartIdentity, "encounter_id");
        }
        progress << canonicalJson(progressRow) << '\n' << std::flush;
        if (!progress) throw std::runtime_error("failed flushing elite setup progress");
        if (!prepared.battleStarted) continue;
        const auto encounter = stringField(prepared.battleStartIdentity, "encounter_id");
        if (act1Elites.find(encounter) == act1Elites.end()) continue;
        const bool needExample = examplesByEncounter.find(encounter)
                == examplesByEncounter.end();
        const bool needCausal = encounter == "GREMLIN_NOB" && !haveCausalPowerExample;

        StepSimulator simulator(CharacterClass::IRONCLAD,
                static_cast<std::uint64_t>(gameSeed), kAscension);
        (void) simulator.restoreCheckpoint(*prepared.checkpoint);
        for (int decision = 0; decision < kBattleDecisionLimit; ++decision) {
            const auto nativeState = simulator.publicBattleState();
            validateNativeSnapshotSchema(nativeState);
            ++checkedNativeEliteSnapshots;
            for (const auto cardHandle : nativeState["hand"].cast<pb::list>()) {
                const auto card = pb::reinterpret_borrow<pb::dict>(cardHandle);
                observedCardNameTypes.insert(stringField(card, "type") + ":"
                        + stringField(card, "name"));
            }
            const auto resources = nativeState["persistent_resources"].cast<pb::dict>();
            for (const auto potionHandle : resources["potions"].cast<pb::list>()) {
                const auto potion = pb::reinterpret_borrow<pb::dict>(potionHandle);
                observedPotionNames.insert(stringField(potion, "name"));
            }
            for (const auto monsterHandle : nativeState["monsters"].cast<pb::list>()) {
                const auto monster = pb::reinterpret_borrow<pb::dict>(monsterHandle);
                observedMonsterIdentities.insert(stringField(monster, "id_label") + ":"
                        + stringField(monster, "name"));
            }
            for (const auto actionHandle : nativeState[
                    "ordered_public_legal_actions"].cast<pb::list>()) {
                observedActionKinds.insert(stringField(
                        pb::reinterpret_borrow<pb::dict>(actionHandle), "kind"));
            }
            const auto currentEncounter = stringField(nativeState, "encounter_id");
            if (currentEncounter != encounter) {
                throw std::logic_error("encounter identity changed inside the qualification battle");
            }
            pb::dict audit;
            const auto selected = selectPublicHeuristicAction(nativeState, &audit);
            bool selectedActionWasEmitted = false;
            const auto legalActions = nativeState["ordered_public_legal_actions"].cast<pb::list>();
            for (const auto actionHandle : legalActions) {
                if (selected.equal(pb::reinterpret_borrow<pb::dict>(actionHandle))) {
                    selectedActionWasEmitted = true;
                    break;
                }
            }
            requireQualification(selectedActionWasEmitted,
                    "baseline selected an action absent from the native public legal action list");

            if (needExample && examplesByEncounter.find(encounter)
                    == examplesByEncounter.end()) {
                examplesByEncounter.emplace(encounter,
                        summarizeRealSnapshot(nativeState, gameSeed, decision, selected, audit));
            }
            if (needCausal && !haveCausalPowerExample) {
                int powerIndex = -1;
                if (hasEligiblePowerAndCompetitor(nativeState, &powerIndex)) {
                    causalEvidence = qualifyCausalNobBranch(nativeState, powerIndex);
                    causalEvidence["development_seed"] = gameSeed;
                    causalEvidence["floor_num"] = intField(nativeState, "floor_num");
                    causalEvidence["turn"] = intField(nativeState, "turn");
                    causalEvidence["decision_index_in_elite_battle"] = decision;
                    haveCausalPowerExample = true;
                }
            }

            // The native simulator resolves the public identity against its
            // current legal actions, providing a focused legality integration check.
            progressRow["stage"] = "elite_public_decision_checked";
            progressRow["game_seed"] = gameSeed;
            progressRow["encounter_id"] = encounter;
            progressRow["turn"] = intField(nativeState, "turn");
            progressRow["decision_index"] = decision;
            progressRow["selected_action"] = selected;
            progress << canonicalJson(progressRow) << '\n' << std::flush;
            if (!progress) throw std::runtime_error("failed flushing elite decision progress");
            (void) simulator.stepPublicAction(selected);
            if (examplesByEncounter.find(encounter) != examplesByEncounter.end()
                    && (encounter != "GREMLIN_NOB" || haveCausalPowerExample)) {
                break;
            }
            const auto after = simulator.snapshot();
            if (stringField(after, "screen_state") != "BATTLE"
                    || !after["battle_active"].cast<bool>()) {
                break;
            }
        }
    }

    pb::dict examplesById;
    for (const auto &[encounter, summary] : examplesByEncounter) {
        examplesById[pb::str(encounter)] = summary;
    }
    report["real_native_elite_snapshots"] = examplesById;
    report["encounter_examples_found"] = static_cast<int>(examplesByEncounter.size());
    report["encounter_examples_expected"] = static_cast<int>(act1Elites.size());
    report["checked_native_elite_snapshots"] = checkedNativeEliteSnapshots;
    auto stringsToList = [](const std::set<std::string> &values) {
        pb::list rows;
        for (const auto &value : values) rows.append(value);
        return rows;
    };
    report["observed_card_type_and_names"] = stringsToList(observedCardNameTypes);
    report["observed_potion_names"] = stringsToList(observedPotionNames);
    report["observed_monster_id_labels_and_names"] = stringsToList(observedMonsterIdentities);
    report["observed_public_action_kinds"] = stringsToList(observedActionKinds);
    report["development_seeds_checked"] = developmentSeedsTried;
    report["causal_power_score_evidence"] = causalEvidence;
    report["causal_power_state_found"] = haveCausalPowerExample;
    requireQualification(examplesByEncounter.find("GREMLIN_NOB")
                    != examplesByEncounter.end(),
            "no supported naturally reached Gremlin Nob public snapshot was found in exposed development seeds");
    requireQualification(haveCausalPowerExample,
            "no naturally reached Gremlin Nob snapshot with a legal Power and competing action was found in exposed development seeds");
    pb::dict progressRow;
    progressRow["stage"] = "qualification_finished";
    progressRow["encounter_examples_found"] = static_cast<int>(examplesByEncounter.size());
    progressRow["causal_power_state_found"] = haveCausalPowerExample;
    progress << canonicalJson(progressRow) << '\n' << std::flush;
    if (!progress) throw std::runtime_error("failed flushing completed qualification progress");
    return report;
}


} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--heuristic-check") {
        try {
            pb::scoped_interpreter interpreter{};
            runHeuristicSanityChecks();
            return 0;
        } catch (const std::exception &error) {
            std::cerr << "ISSUE17_PUBLIC_HEURISTIC_SANITY_FAIL: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc != 5 || std::string(argv[1]) != "--qualify-public-comparator") {
        std::cerr << "usage: study-issue17-public-comparator-qualification "
                     "--qualify-public-comparator <native-base-sha> "
                     "<issue16-source-material-sha> <output-json>\n";
        return 2;
    }
    try {
        pb::scoped_interpreter interpreter{};
        const std::string progressPath = std::string(argv[4]) + ".progress.jsonl";
        std::ofstream progress(progressPath, std::ios::binary | std::ios::trunc);
        if (!progress) throw std::runtime_error("could not open durable qualification progress file");
        const auto report = runPublicComparatorQualification(argv[2], argv[3], progress);
        progress.close();
        if (!progress) throw std::runtime_error("failed closing qualification progress file");
        std::ofstream output(argv[4], std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("could not open qualification evidence output");
        output << pb::module_::import("json").attr("dumps")(
                report, pb::arg("sort_keys") = true, pb::arg("indent") = 2)
                       .cast<std::string>() << '\n';
        output.close();
        if (!output) throw std::runtime_error("failed writing qualification evidence output");
        std::remove(progressPath.c_str());
        std::cout << "ISSUE17_STAGE_A_PUBLIC_COMPARATOR_QUALIFICATION_PASS\n";
        const auto causal = report["causal_power_score_evidence"].cast<pb::dict>();
        std::cout << "native_elite_examples="
                  << report["encounter_examples_found"].cast<int>()
                  << " causal_power_score_delta="
                  << causal["non_nob_minus_nob_power_score"].cast<double>() << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "ISSUE17_STAGE_A_PUBLIC_COMPARATOR_QUALIFICATION_FAIL: "
                  << error.what() << '\n';
        return 1;
    }
}
