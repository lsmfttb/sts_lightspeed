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
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pb = pybind11;
using namespace pybind11::literals;

namespace {

constexpr int kGameSeeds[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
constexpr int kReplicateSeeds[] = {17, 101};
constexpr int kBudgets[] = {96, 384};
constexpr int kAscension = 20;
constexpr int kParticleCount = 32;
constexpr int kSetupStepLimit = 512;
constexpr int kBattleDecisionLimit = 128;
constexpr int kRolloutDecisionLimit = 24;
constexpr std::uint64_t kSamplerSeedBase = 0x6e6f726d70756231ULL;
constexpr std::uint64_t kSearchSeedBase = 0x7368617265647472ULL;

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

std::string historyNodeKey(const std::vector<std::string> &history) {
    std::string result;
    for (const auto &component : history) {
        result += std::to_string(component.size());
        result.push_back(':');
        result += component;
    }
    return result;
}

std::uint64_t publicTieToken(
        const std::string &nodeKey,
        const std::uint64_t visits,
        const std::uint64_t searchSeed) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : nodeKey) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return splitMix64(searchSeed ^ hash ^ splitMix64(visits));
}

struct Edge {
    pb::dict action;
    std::string actionKey;
    std::uint64_t visits = 0;
    double valueSum = 0.0;
};

struct Node {
    std::string stateKey;
    std::vector<std::string> orderedActionKeys;
    std::vector<Edge> edges;
    std::uint64_t visits = 0;
};

struct PublicTree {
    std::unordered_map<std::string, Node> nodes;

    Node &getOrCreate(const std::string &nodeKey, const pb::dict &state) {
        if (containsPrivateField(state)) {
            throw std::logic_error("private field reached public tree");
        }
        const auto stateKey = canonicalJson(state);
        const auto actions = publicActions(state);
        auto [position, inserted] = nodes.try_emplace(nodeKey);
        auto &node = position->second;
        if (inserted) {
            node.stateKey = stateKey;
            std::unordered_set<std::string> seen;
            for (const auto &action : actions) {
                const auto actionKey = canonicalJson(action);
                if (!seen.insert(actionKey).second) {
                    throw std::logic_error("public legal actions are not uniquely identified");
                }
                node.orderedActionKeys.push_back(actionKey);
                node.edges.push_back(Edge{action, actionKey, 0, 0.0});
            }
            if (node.edges.empty()) {
                throw std::logic_error("public decision node has no legal actions");
            }
        } else {
            if (node.stateKey != stateKey || node.orderedActionKeys.size() != actions.size()) {
                throw std::logic_error("public history key resolved to inconsistent state/actions");
            }
            for (std::size_t i = 0; i < actions.size(); ++i) {
                if (node.orderedActionKeys[i] != canonicalJson(actions[i])) {
                    throw std::logic_error("public action ordering changed at shared node");
                }
            }
        }
        return node;
    }
};

std::size_t selectSharedEdge(
        const Node &node,
        const std::string &nodeKey,
        const std::uint64_t searchSeed) {
    std::vector<std::size_t> unvisited;
    for (std::size_t i = 0; i < node.edges.size(); ++i) {
        if (node.edges[i].visits == 0) {
            unvisited.push_back(i);
        }
    }
    if (!unvisited.empty()) {
        return unvisited[publicTieToken(nodeKey, node.visits, searchSeed) % unvisited.size()];
    }

    double best = -std::numeric_limits<double>::infinity();
    std::vector<std::size_t> ties;
    for (std::size_t i = 0; i < node.edges.size(); ++i) {
        const auto &edge = node.edges[i];
        const double mean = edge.valueSum / static_cast<double>(edge.visits);
        const double explore = std::sqrt(
                2.0 * std::log(static_cast<double>(std::max<std::uint64_t>(1, node.visits)))
                / static_cast<double>(edge.visits));
        const double score = mean + explore;
        if (score > best + 1e-12) {
            best = score;
            ties.assign(1, i);
        } else if (std::abs(score - best) <= 1e-12) {
            ties.push_back(i);
        }
    }
    return ties[publicTieToken(nodeKey, node.visits, searchSeed) % ties.size()];
}

double truncatedPublicEvaluation(const pb::dict &state) {
    const auto player = state["player"].cast<pb::dict>();
    const int hp = intField(player, "current_hp");
    const int maxHp = std::max(1, intField(player, "max_hp"));
    const auto monsters = state["monsters"].cast<pb::list>();
    double enemyHpFraction = 0.0;
    for (const auto item : monsters) {
        const auto monster = pb::reinterpret_borrow<pb::dict>(item);
        const int maximum = std::max(1, intField(monster, "max_hp"));
        enemyHpFraction += static_cast<double>(intField(monster, "current_hp"))
                / static_cast<double>(maximum);
    }
    return std::clamp(static_cast<double>(hp) / maxHp - 0.5 * enemyHpFraction,
            -1.0, 1.0);
}

std::uint64_t decisionSeed(
        const std::uint64_t base,
        const int gameSeed,
        const int replicateSeed,
        const int decisionIndex) {
    return splitMix64(base ^ splitMix64(static_cast<std::uint64_t>(gameSeed))
            ^ splitMix64(static_cast<std::uint64_t>(replicateSeed))
            ^ splitMix64(static_cast<std::uint64_t>(decisionIndex)));
}

struct SearchResult {
    pb::dict selectedAction;
    pb::dict evidence;
};

SearchResult searchDecision(
        const StepSimulator &anchor,
        const pb::dict &rootState,
        const int budget,
        const std::uint64_t samplerSeed,
        const std::uint64_t searchSeed) {
    PublicTree tree;
    std::unordered_set<std::string> rootKeys;
    std::uint64_t nativeActionSteps = 0;
    std::uint64_t terminalRollouts = 0;
    std::uint64_t truncatedRollouts = 0;

    for (int simulation = 0; simulation < budget; ++simulation) {
        const auto particleIndex = static_cast<std::uint64_t>(simulation % kParticleCount);
        auto particle = anchor.samplePublicConsistentHiddenFuture(samplerSeed, particleIndex);
        auto state = particle.publicBattleState();
        if (canonicalJson(state) != canonicalJson(rootState)) {
            throw std::logic_error("sampled particle did not preserve the decision public root");
        }

        std::vector<std::string> history{canonicalJson(state)};
        std::vector<std::pair<std::string, std::size_t>> path;
        double value = 0.0;
        bool terminal = false;
        for (int depth = 0; depth < kRolloutDecisionLimit; ++depth) {
            const auto nodeKey = historyNodeKey(history);
            if (depth == 0) {
                rootKeys.insert(nodeKey);
            }
            auto &node = tree.getOrCreate(nodeKey, state);
            const auto edgeIndex = selectSharedEdge(node, nodeKey, searchSeed);
            const auto actionKey = node.edges[edgeIndex].actionKey;
            const auto action = node.edges[edgeIndex].action;
            path.emplace_back(nodeKey, edgeIndex);

            const auto transition = particle.stepPublicAction(action);
            ++nativeActionSteps;
            if (!transition.contains("battle_state")) {
                ++terminalRollouts;
                value = particle.bc.outcome == Outcome::PLAYER_VICTORY ? 1.0 : -1.0;
                terminal = true;
                break;
            }
            state = transition["battle_state"].cast<pb::dict>();
            if (containsPrivateField(state)) {
                throw std::logic_error("public transition exposed a private field");
            }
            history.push_back(actionKey);
            history.push_back(canonicalJson(state));
        }
        if (!terminal) {
            ++truncatedRollouts;
            value = truncatedPublicEvaluation(state);
        }

        for (const auto &[nodeKey, edgeIndex] : path) {
            auto &node = tree.nodes.at(nodeKey);
            ++node.visits;
            auto &edge = node.edges.at(edgeIndex);
            ++edge.visits;
            edge.valueSum += value;
        }
    }

    if (rootKeys.size() != 1 || tree.nodes.empty()) {
        throw std::logic_error("particles did not share exactly one public root node");
    }
    const auto &root = tree.nodes.at(*rootKeys.begin());
    std::uint64_t rootVisits = 0;
    std::size_t bestIndex = 0;
    for (std::size_t i = 0; i < root.edges.size(); ++i) {
        rootVisits += root.edges[i].visits;
        const auto &candidate = root.edges[i];
        const auto &best = root.edges[bestIndex];
        const double candidateMean = candidate.visits > 0
                ? candidate.valueSum / static_cast<double>(candidate.visits)
                : -std::numeric_limits<double>::infinity();
        const double bestMean = best.visits > 0
                ? best.valueSum / static_cast<double>(best.visits)
                : -std::numeric_limits<double>::infinity();
        if (candidate.visits > best.visits
                || (candidate.visits == best.visits && candidateMean > bestMean + 1e-12)) {
            bestIndex = i;
        }
    }
    if (root.visits != static_cast<std::uint64_t>(budget) || rootVisits != budget) {
        throw std::logic_error("root visits did not equal the per-decision total budget");
    }

    std::size_t visitedRootActions = 0;
    for (const auto &edge : root.edges) {
        visitedRootActions += edge.visits > 0 ? 1 : 0;
    }

    pb::dict evidence;
    evidence["simulations"] = budget;
    evidence["particle_count"] = kParticleCount;
    evidence["distinct_particles_used"] = std::min(budget, kParticleCount);
    evidence["shared_public_nodes"] = tree.nodes.size();
    evidence["root_action_count"] = root.edges.size();
    evidence["root_actions_visited"] = visitedRootActions;
    evidence["terminal_rollouts"] = terminalRollouts;
    evidence["truncated_rollouts"] = truncatedRollouts;
    evidence["native_public_action_steps"] = nativeActionSteps;
    evidence["selected_public_action"] = root.edges[bestIndex].action;
    evidence["selected_action_visits"] = root.edges[bestIndex].visits;
    evidence["selected_action_mean_value"] = root.edges[bestIndex].visits > 0
            ? pb::object(pb::float_(root.edges[bestIndex].valueSum
                    / root.edges[bestIndex].visits))
            : pb::object(pb::none());
    evidence["action_choice_source"] = "shared public root visits and values only";
    evidence["tree_key_source"] = "public state and public action/result history only";
    return SearchResult{root.edges[bestIndex].action, evidence};
}

std::string failureCategory(const std::string &phase, const std::string &message) {
    if (message.find("unsupported public fidelity") != std::string::npos
            || message.find("unsupported_fidelity") != std::string::npos) {
        return "unsupported_public_fidelity";
    }
    if (message.find("Runic Dome") != std::string::npos) {
        return "runic_dome_hidden_intent";
    }
    if (message.find("private monster future counters") != std::string::npos) {
        return "private_monster_future_counter";
    }
    if (message.find("hidden monster status timing") != std::string::npos) {
        return "monster_status_timing";
    }
    if (message.find("hidden player status timing") != std::string::npos) {
        return "player_status_timing";
    }
    if (message.find("Hexaghost") != std::string::npos) {
        return "hexaghost_hidden_move_cycle_counter";
    }
    if (phase == "sampler") {
        return "sampler_rejected_other";
    }
    return phase + "_error";
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

pb::dict selectPublicHeuristicAction(const pb::dict &state) {
    const auto actions = state["ordered_public_legal_actions"].cast<pb::list>();
    const auto hand = state["hand"].cast<pb::list>();
    const auto monsters = state["monsters"].cast<pb::list>();
    const auto player = state["player"].cast<pb::dict>();
    int incomingDamage = 0;
    for (const auto monsterHandle : monsters) {
        const auto monster = pb::reinterpret_borrow<pb::dict>(monsterHandle);
        if (!monster["alive"].cast<bool>() || !monster["attacking"].cast<bool>()) continue;
        incomingDamage += intField(monster, "move_base_damage")
                * intField(monster, "move_hits");
    }
    const int currentBlock = intField(player, "block");
    int bestIndex = -1;
    int bestScore = std::numeric_limits<int>::min();
    int bestTargetTie = std::numeric_limits<int>::min();
    for (int index = 0; index < actions.size(); ++index) {
        const auto action = pb::reinterpret_borrow<pb::dict>(actions[index]);
        const auto kind = stringField(action, "kind");
        int score = std::numeric_limits<int>::min();
        int targetTie = 0;
        if (kind == "potion") {
            score = 500;
            const int target = intField(action, "idx2");
            if (target >= 0 && target < monsters.size()) {
                const auto monster = pb::reinterpret_borrow<pb::dict>(monsters[target]);
                if (monster["alive"].cast<bool>()) {
                    targetTie = intField(monster, "current_hp");
                }
            }
        } else if (kind == "card") {
            const int source = intField(action, "idx1");
            if (source < 0 || source >= hand.size()) continue;
            const auto card = pb::reinterpret_borrow<pb::dict>(hand[source]);
            const auto type = stringField(card, "type");
            const int cost = intField(card, "cost_for_turn");
            if (type == "POWER") {
                score = 300 + cost;
            } else if (type == "ATTACK") {
                score = 200 + cost;
            } else if (type == "SKILL") {
                score = (incomingDamage > currentBlock ? 250 : 100) + cost;
            } else {
                score = 50 + cost;
            }
            const int target = intField(action, "idx2");
            if (target >= 0 && target < monsters.size()) {
                const auto monster = pb::reinterpret_borrow<pb::dict>(monsters[target]);
                if (monster["alive"].cast<bool>() && monster["attacking"].cast<bool>()) {
                    targetTie = intField(monster, "move_base_damage")
                            * intField(monster, "move_hits") * 100
                            - intField(monster, "current_hp");
                }
            }
        } else if (kind == "end_turn") {
            score = 0;
        } else if (kind == "single_card_select" || kind == "multi_card_select") {
            score = 600;
            targetTie = -intField(action, "idx1");
        }
        if (score > bestScore || (score == bestScore && targetTie > bestTargetTie)) {
            bestScore = score;
            bestTargetTie = targetTie;
            bestIndex = index;
        }
    }
    if (bestIndex < 0) {
        throw std::runtime_error("public heuristic found no usable legal action");
    }
    return pb::reinterpret_borrow<pb::dict>(actions[bestIndex]);
}

struct DecisionLatency {
    int budget = 0;
    double totalSeconds = 0.0;
    double searchSeconds = 0.0;
};

struct Attempt {
    pb::dict evidence;
    pb::dict battleStartIdentity;
    int gameSeed = 0;
    int replicateSeed = 0;
    int budget = 0;
    std::string policy;
    bool battleStarted = false;
    bool battleCompleted = false;
    bool fullySupportedCompleted = false;
    bool won = false;
    int hpLost = 0;
    int publicDecisionsAttempted = 0;
    int supportedDecisions = 0;
    int unsupportedPublicStateDecisions = 0;
    std::uint64_t simulations = 0;
    std::uint64_t nativeActionSteps = 0;
    std::string firstUnsupportedCategory;
    double trialWallSeconds = 0.0;
    double battleWallSeconds = 0.0;
    double controllerWallSeconds = 0.0;
    std::vector<DecisionLatency> latencies;
};

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

Attempt runAttempt(
        const PreparedBattleStart &prepared,
        const int replicateSeed,
        const int budget,
        const std::string &policy,
        const std::string &simulatorCommit,
        const std::string &runnerCommit) {
    Attempt attempt;
    const int gameSeed = prepared.gameSeed;
    attempt.gameSeed = gameSeed;
    attempt.replicateSeed = replicateSeed;
    attempt.budget = budget;
    attempt.policy = policy;
    attempt.evidence = pb::dict();
    const auto trialStart = Clock::now();
    pb::list decisions;

    try {
        StepSimulator simulator(CharacterClass::IRONCLAD,
                static_cast<std::uint64_t>(gameSeed), kAscension);
        if (!prepared.battleStarted || !prepared.checkpoint) {
            copyDictEntries(prepared.evidence, attempt.evidence);
        } else {
            simulator.restoreCheckpoint(*prepared.checkpoint);
            auto startState = simulator.publicBattleState();
            if (containsPrivateField(startState)) {
                throw std::logic_error("battle start public state contains private fields");
            }
            if (canonicalJson(battleStartIdentityFromPublicState(gameSeed, startState))
                    != canonicalJson(prepared.battleStartIdentity)) {
                throw std::logic_error("restored battle checkpoint changed its public start identity");
            }
            attempt.battleStarted = true;
            attempt.battleStartIdentity = prepared.battleStartIdentity;
            copyDictEntries(prepared.evidence, attempt.evidence);
            attempt.evidence["prelude_setup_wall_seconds"] = prepared.setupWallSeconds;
            attempt.evidence["battle_start_identity"] = attempt.battleStartIdentity;
            attempt.evidence["policy"] = policy;
        attempt.evidence["controller_semantics"] = policy == "shared_public_search"
                ? "one UCT tree per decision, shared across sampled particles; keyed by public state/action/result history; only public root statistics choose the executed action"
                : "deterministic normal-public heuristic: use legal potions first, play Powers before Attacks, prioritize Skills when visible incoming intent exceeds block, prefer higher-cost cards, and target the lowest-HP incoming attacker; card-selection ties use the first legal option";

        const int startingHp = intField(startState["player"].cast<pb::dict>(), "current_hp");
        const auto battleStart = Clock::now();
        double controllerWallSeconds = 0.0;
        bool stop = false;
        std::string stopReason = "battle_decision_limit";

        for (int decisionIndex = 0;
                decisionIndex < kBattleDecisionLimit && !stop;
                ++decisionIndex) {
            const auto decisionStart = Clock::now();
            pb::dict decisionEvidence;
            decisionEvidence["decision_index"] = decisionIndex;
            decisionEvidence["search_budget"] = budget;
            decisionEvidence["particle_count"] = kParticleCount;
            const auto samplerSeed = decisionSeed(kSamplerSeedBase,
                    gameSeed, replicateSeed, decisionIndex);
            const auto searchSeed = decisionSeed(kSearchSeedBase,
                    gameSeed, replicateSeed, decisionIndex);
            ++attempt.publicDecisionsAttempted;
            try {
                auto publicState = simulator.publicBattleState();
                decisionEvidence["information_fidelity"] =
                        stringField(publicState, "information_fidelity");
                if (containsPrivateField(publicState)) {
                    throw std::logic_error("public state contains a private field");
                }
                if (stringField(publicState, "information_fidelity") != "supported") {
                    ++attempt.unsupportedPublicStateDecisions;
                }

                const bool isSearch = policy == "shared_public_search";
                pb::dict selectedAction;
                double searchSeconds = 0.0;
                const auto controllerSelectStart = Clock::now();
                if (!isSearch) {
                    selectedAction = selectPublicHeuristicAction(publicState);
                }

                const auto samplerProbeStart = Clock::now();
                bool samplerSupported = false;
                std::string samplerError;
                try {
                    auto preflightParticle = simulator.samplePublicConsistentHiddenFuture(
                            samplerSeed, 0);
                    if (canonicalJson(preflightParticle.publicBattleState())
                            != canonicalJson(publicState)) {
                        throw std::logic_error("sampler preflight changed public state");
                    }
                    samplerSupported = true;
                    ++attempt.supportedDecisions;
                    decisionEvidence["sampler_support"] = "supported";
                } catch (const std::exception &error) {
                    samplerError = error.what();
                    const auto category = failureCategory("sampler", samplerError);
                    if (attempt.firstUnsupportedCategory.empty()) {
                        attempt.firstUnsupportedCategory = category;
                    }
                    decisionEvidence["sampler_support"] = "unsupported";
                    decisionEvidence["sampler_failure_category"] = category;
                    decisionEvidence["sampler_failure_reason"] = samplerError;
                }
                const double samplerProbeSeconds = std::chrono::duration<double>(
                        Clock::now() - samplerProbeStart).count();
                decisionEvidence["sampler_probe_wall_seconds"] = samplerProbeSeconds;

                if (isSearch && !samplerSupported) {
                    stopReason = "unsupported_sampler_decision";
                    pb::dict unsupported;
                    unsupported["decision_index"] = decisionIndex;
                    unsupported["category"] = failureCategory("sampler", samplerError);
                    unsupported["reason"] = samplerError;
                    attempt.evidence["first_unsupported_decision"] = unsupported;
                    decisionEvidence["decision_status"] = "sampler_unsupported";
                    decisionEvidence["decision_wall_seconds"] = std::chrono::duration<double>(
                            Clock::now() - decisionStart).count();
                    attempt.latencies.push_back({budget,
                            decisionEvidence["decision_wall_seconds"].cast<double>(), 0.0});
                    decisions.append(decisionEvidence);
                    stop = true;
                    continue;
                }

                if (isSearch) {
                    decisionEvidence["sampler_seed"] = samplerSeed;
                    decisionEvidence["search_seed"] = searchSeed;
                    const auto searchStart = Clock::now();
                    const auto search = searchDecision(simulator, publicState, budget,
                            samplerSeed, searchSeed);
                    searchSeconds = std::chrono::duration<double>(
                            Clock::now() - searchStart).count();
                    decisionEvidence["search"] = search.evidence;
                    selectedAction = search.selectedAction;
                    attempt.simulations += static_cast<std::uint64_t>(budget);
                    attempt.nativeActionSteps += search.evidence[
                            "native_public_action_steps"].cast<std::uint64_t>();
                }

                const double controllerSelectSeconds = std::chrono::duration<double>(
                        Clock::now() - controllerSelectStart).count();
                const auto transition = simulator.stepPublicAction(selectedAction);
                ++attempt.nativeActionSteps;
                decisionEvidence["selected_public_action"] = selectedAction;
                decisionEvidence["real_action_screen_state"] =
                        stringField(transition, "screen_state");
                if (!transition.contains("battle_state")) {
                    if (simulator.bc.outcome == Outcome::UNDECIDED) {
                        throw std::runtime_error(
                                "public action left battle without a terminal battle outcome");
                    }
                    attempt.battleCompleted = true;
                    attempt.won = simulator.bc.outcome == Outcome::PLAYER_VICTORY;
                    attempt.hpLost = startingHp - simulator.bc.player.curHp;
                    stopReason = "battle_completed";
                    stop = true;
                }
                decisionEvidence["decision_status"] = stop
                        ? "battle_completed" : "action_executed";
                const double totalSeconds = std::chrono::duration<double>(
                        Clock::now() - decisionStart).count();
                const double controllerSeconds = totalSeconds
                        - (!isSearch ? samplerProbeSeconds : 0.0);
                decisionEvidence["decision_wall_seconds"] = totalSeconds;
                decisionEvidence["controller_decision_wall_seconds"] = controllerSeconds;
                decisionEvidence["controller_selection_wall_seconds"] = controllerSelectSeconds;
                decisionEvidence["search_wall_seconds"] = searchSeconds;
                controllerWallSeconds += controllerSeconds;
                attempt.latencies.push_back({budget, controllerSeconds, searchSeconds});
            } catch (const std::exception &error) {
                decisionEvidence["decision_status"] = "controller_error";
                decisionEvidence["failure_category"] = failureCategory(
                        "controller", error.what());
                decisionEvidence["failure_reason"] = error.what();
                stopReason = "controller_error";
                attempt.evidence["controller_error"] = error.what();
                stop = true;
                decisionEvidence["decision_wall_seconds"] = std::chrono::duration<double>(
                        Clock::now() - decisionStart).count();
            }
            decisions.append(decisionEvidence);
        }

        attempt.battleWallSeconds = std::chrono::duration<double>(
                Clock::now() - battleStart).count();
        attempt.controllerWallSeconds = controllerWallSeconds;
        attempt.fullySupportedCompleted = attempt.battleCompleted
                && !attempt.evidence.contains("controller_error")
                && (policy != "shared_public_search"
                        || attempt.publicDecisionsAttempted == attempt.supportedDecisions);
        attempt.evidence["stop_reason"] = stopReason;
        attempt.evidence["battle_completed"] = attempt.battleCompleted;
        attempt.evidence["fully_supported_completed"] = attempt.fullySupportedCompleted;
        attempt.evidence["battle_outcome"] = attempt.battleCompleted
                ? (attempt.won ? "win" : "loss") : "incomplete";
        attempt.evidence["combat_ending_player_hp"] = simulator.bc.player.curHp;
        attempt.evidence["persistent_ending_player_hp"] = simulator.gc.curHp;
        attempt.evidence["hp_lost_during_battle"] = attempt.battleCompleted
                ? pb::object(pb::int_(attempt.hpLost)) : pb::object(pb::none());
        }
    } catch (const std::exception &error) {
        if (!attempt.evidence.contains("setup_status")) {
            attempt.evidence["setup_status"] = "setup_error";
        }
        attempt.evidence["setup_error"] = error.what();
    }

    if (!attempt.evidence.contains("battle_outcome")) {
        attempt.evidence["battle_outcome"] = "incomplete";
    }
    if (!attempt.evidence.contains("battle_completed")) {
        attempt.evidence["battle_completed"] = attempt.battleCompleted;
    }
    if (!attempt.evidence.contains("fully_supported_completed")) {
        attempt.evidence["fully_supported_completed"] = attempt.fullySupportedCompleted;
    }
    attempt.evidence["battle_started"] = attempt.battleStarted;
    attempt.evidence["first_sampler_unsupported_category"] =
            attempt.firstUnsupportedCategory.empty()
                    ? pb::object(pb::none())
                    : pb::object(pb::str(attempt.firstUnsupportedCategory));

    attempt.evidence["schema_id"] = "normal-public-complete-battle-run-v1";
    attempt.evidence["simulator_base_commit"] = simulatorCommit;
    attempt.evidence["study_runner_commit"] = runnerCommit;
    attempt.evidence["character"] = "IRONCLAD";
    attempt.evidence["ascension"] = kAscension;
    attempt.evidence["game_seed"] = gameSeed;
    attempt.evidence["controller_replicate_seed"] = replicateSeed;
    attempt.evidence["policy"] = policy;
    attempt.evidence["search_budget_per_supported_decision"] = budget;
    attempt.evidence["particle_count"] = kParticleCount;
    attempt.evidence["rollout_decision_limit"] = kRolloutDecisionLimit;
    attempt.evidence["pre_battle_setup_policy"] =
            "fixed A20 Ironclad seed; at Act 1 map screens follow a shortest reachable route to the earliest Elite, breaking ties toward more rest nodes and fewer intervening Monsters; elsewhere select the first legal game action; clear intervening battles with the deterministic public heuristic; stop at the first Act 1 Elite";
    attempt.evidence["pre_battle_setup_target"] = "first_act1_elite";
    attempt.evidence["public_decisions_attempted"] = attempt.publicDecisionsAttempted;
    attempt.evidence["sampling_supported_decisions"] = attempt.supportedDecisions;
    attempt.evidence["sampling_supported_fraction"] = attempt.publicDecisionsAttempted > 0
            ? pb::object(pb::float_(static_cast<double>(attempt.supportedDecisions)
                    / attempt.publicDecisionsAttempted))
            : pb::object(pb::none());
    attempt.evidence["unsupported_public_state_decisions"] =
            attempt.unsupportedPublicStateDecisions;
    attempt.evidence["total_shared_search_simulations"] = attempt.simulations;
    attempt.evidence["search_and_controller_native_public_action_steps"] =
            attempt.nativeActionSteps;
    attempt.evidence["battle_wall_seconds"] = attempt.battleWallSeconds;
    attempt.evidence["controller_wall_seconds_excluding_baseline_support_probe"] =
            attempt.controllerWallSeconds;
    attempt.evidence["attempt_wall_seconds_excluding_shared_setup"] =
            std::chrono::duration<double>(Clock::now() - trialStart).count();
    attempt.evidence["decisions"] = decisions;
    return attempt;
}

double percentile(std::vector<double> values, const double q) {
    if (values.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::ceil(q * values.size())) - 1;
    return values[std::min(index, values.size() - 1)];
}

pb::object optionalMean(const std::vector<double> &values) {
    if (values.empty()) {
        return pb::none();
    }
    double total = 0.0;
    for (const auto value : values) {
        total += value;
    }
    return pb::float_(total / static_cast<double>(values.size()));
}

pb::dict summarizePolicy(
        const std::string &policy,
        const int budget,
        const std::vector<Attempt> &attempts) {
    int count = 0;
    int started = 0;
    int completed = 0;
    int fullySupported = 0;
    int wins = 0;
    int losses = 0;
    int incompleteUnsupported = 0;
    int setupFailed = 0;
    std::uint64_t simulations = 0;
    std::uint64_t publicSteps = 0;
    int attemptedDecisions = 0;
    int supportedDecisions = 0;
    int unsupportedPublicStateDecisions = 0;
    std::map<std::string, int> unsupportedCategories;
    std::vector<double> totalWall;
    std::vector<double> decisionWall;
    std::vector<double> searchWall;
    std::vector<double> hpLosses;

    for (const auto &attempt : attempts) {
        if (attempt.policy != policy || attempt.budget != budget) {
            continue;
        }
        ++count;
        started += attempt.battleStarted;
        completed += attempt.battleCompleted;
        fullySupported += policy == "shared_public_search"
                && attempt.fullySupportedCompleted;
        if (!attempt.battleStarted) ++setupFailed;
        if (!attempt.battleCompleted && !attempt.firstUnsupportedCategory.empty()) {
            ++incompleteUnsupported;
        }
        if (attempt.battleCompleted) {
            wins += attempt.won;
            losses += !attempt.won;
            hpLosses.push_back(attempt.hpLost);
        }
        simulations += attempt.simulations;
        publicSteps += attempt.nativeActionSteps;
        attemptedDecisions += attempt.publicDecisionsAttempted;
        supportedDecisions += attempt.supportedDecisions;
        unsupportedPublicStateDecisions += attempt.unsupportedPublicStateDecisions;
        if (!attempt.firstUnsupportedCategory.empty()) {
            ++unsupportedCategories[attempt.firstUnsupportedCategory];
        }
        totalWall.push_back(attempt.controllerWallSeconds);
        for (const auto &latency : attempt.latencies) {
            decisionWall.push_back(latency.totalSeconds);
            searchWall.push_back(latency.searchSeconds);
        }
    }

    pb::dict categories;
    for (const auto &[name, value] : unsupportedCategories) {
        categories[pb::str(name)] = value;
    }
    pb::dict summary;
    summary["policy"] = policy;
    summary["search_budget_per_decision"] = budget;
    summary["particle_count"] = kParticleCount;
    summary["attempted_battles_all_denominator"] = count;
    summary["battles_reaching_act1_elite_start"] = started;
    summary["battles_with_actual_terminal_outcome"] = completed;
    summary["wins_among_completed_battles"] = wins;
    summary["losses_among_completed_battles"] = losses;
    summary["prelude_setup_failures_before_target"] = setupFailed;
    summary["incomplete_sampler_unsupported_attempts"] = incompleteUnsupported;
    summary["all_sampler_supported_search_completions"] = fullySupported;
    summary["battle_completion_fraction_all_attempts"] = count > 0
            ? pb::object(pb::float_(static_cast<double>(completed) / count))
            : pb::object(pb::none());
    summary["sampler_supported_completion_fraction_all_attempts"] =
            policy == "shared_public_search" && count > 0
                    ? pb::object(pb::float_(static_cast<double>(fullySupported) / count))
                    : pb::object(pb::none());
    summary["win_fraction_among_completed_battles"] = completed > 0
            ? pb::object(pb::float_(static_cast<double>(wins) / completed))
            : pb::object(pb::none());
    summary["mean_hp_lost_during_completed_battles"] = optionalMean(hpLosses);
    summary["public_decisions_attempted_all_runs"] = attemptedDecisions;
    summary["sampling_supported_decisions_all_runs"] = supportedDecisions;
    summary["unsupported_public_state_decisions_all_runs"] = unsupportedPublicStateDecisions;
    summary["sampling_supported_fraction_all_decisions"] = attemptedDecisions > 0
            ? pb::object(pb::float_(static_cast<double>(supportedDecisions) / attemptedDecisions))
            : pb::object(pb::none());
    summary["unsupported_first_decision_categories"] = categories;
    summary["total_shared_search_simulations"] = simulations;
    summary["mean_shared_search_simulations_per_attempted_battle"] = count > 0
            ? pb::object(pb::float_(static_cast<double>(simulations) / count))
            : pb::object(pb::none());
    summary["search_and_controller_native_public_action_steps"] = publicSteps;
    summary["controller_battle_wall_seconds_p50_all_attempts"] = totalWall.empty()
            ? pb::object(pb::none()) : pb::object(pb::float_(percentile(totalWall, 0.50)));
    summary["controller_battle_wall_seconds_p95_all_attempts"] = totalWall.empty()
            ? pb::object(pb::none()) : pb::object(pb::float_(percentile(totalWall, 0.95)));
    summary["decision_wall_seconds_p50_supported_decisions"] = decisionWall.empty()
            ? pb::object(pb::none()) : pb::object(pb::float_(percentile(decisionWall, 0.50)));
    summary["decision_wall_seconds_p95_supported_decisions"] = decisionWall.empty()
            ? pb::object(pb::none()) : pb::object(pb::float_(percentile(decisionWall, 0.95)));
    summary["search_wall_seconds_p50_supported_decisions"] = searchWall.empty()
            ? pb::object(pb::none()) : pb::object(pb::float_(percentile(searchWall, 0.50)));
    summary["search_wall_seconds_p95_supported_decisions"] = searchWall.empty()
            ? pb::object(pb::none()) : pb::object(pb::float_(percentile(searchWall, 0.95)));
    return summary;
}

pb::dict clusterBootstrapSummary(
        const std::map<int, std::vector<double>> &valuesByGameSeed,
        const std::uint64_t bootstrapSeed) {
    constexpr int kBootstrapReplicates = 10000;
    std::vector<double> clusterMeans;
    for (const auto &[gameSeed, values] : valuesByGameSeed) {
        (void) gameSeed;
        if (values.empty()) continue;
        double total = 0.0;
        for (const auto value : values) total += value;
        clusterMeans.push_back(total / static_cast<double>(values.size()));
    }

    pb::dict summary;
    summary["resampling_unit"] = "game_seed_cluster";
    summary["complete_seed_clusters"] = static_cast<int>(clusterMeans.size());
    summary["bootstrap_replicates"] = kBootstrapReplicates;
    if (clusterMeans.empty()) {
        summary["mean"] = pb::none();
        summary["percentile_95_ci_low"] = pb::none();
        summary["percentile_95_ci_high"] = pb::none();
        return summary;
    }

    double observedTotal = 0.0;
    for (const auto value : clusterMeans) observedTotal += value;
    summary["mean"] = observedTotal / static_cast<double>(clusterMeans.size());

    std::mt19937_64 random(bootstrapSeed);
    std::uniform_int_distribution<std::size_t> chooseCluster(
            0, clusterMeans.size() - 1);
    std::vector<double> bootstrapMeans;
    bootstrapMeans.reserve(kBootstrapReplicates);
    for (int replicate = 0; replicate < kBootstrapReplicates; ++replicate) {
        double total = 0.0;
        for (std::size_t draw = 0; draw < clusterMeans.size(); ++draw) {
            total += clusterMeans[chooseCluster(random)];
        }
        bootstrapMeans.push_back(total / static_cast<double>(clusterMeans.size()));
    }
    summary["percentile_95_ci_low"] = percentile(bootstrapMeans, 0.025);
    summary["percentile_95_ci_high"] = percentile(bootstrapMeans, 0.975);
    return summary;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 4 && argc != 5) {
        std::cerr << "usage: study-issue14 <simulator-base-sha> <runner-sha> <output-json> [--smoke]\n";
        return 2;
    }
    try {
        const bool smoke = argc == 5 && std::string(argv[4]) == "--smoke";
        if (argc == 5 && !smoke) {
            throw std::invalid_argument("the only optional mode is --smoke");
        }
        const std::vector<int> activeGameSeeds = smoke
                ? std::vector<int>{kGameSeeds[0]}
                : std::vector<int>(std::begin(kGameSeeds), std::end(kGameSeeds));
        const std::vector<int> activeReplicateSeeds = smoke
                ? std::vector<int>{kReplicateSeeds[0]}
                : std::vector<int>(std::begin(kReplicateSeeds), std::end(kReplicateSeeds));
        const std::vector<int> activeBudgets = smoke
                ? std::vector<int>{kBudgets[0]}
                : std::vector<int>(std::begin(kBudgets), std::end(kBudgets));
        pb::scoped_interpreter interpreter{};
        const std::string simulatorCommit = argv[1];
        const std::string runnerCommit = argv[2];
        const std::string outputPath = argv[3];
        pb::dict report;
        report["schema_id"] = "paired-normal-public-outcome-signal-v1";
        report["simulator_base_commit"] = simulatorCommit;
        report["study_runner_commit"] = runnerCommit;
        report["information_regime"] = "normal_public";
        report["run_mode"] = smoke ? "smoke" : "full_study";
        report["scientific_status"] = "exploratory paired outcome signal; not a strength estimate";
        report["controller"] = "adaptive shared-public-tree UCT, one tree per decision and 32 sampled particles";
        report["sampler_interpretation"] = "reproducible proposal distribution, not an exact posterior";
        report["baseline"] = "deterministic normal-public heuristic; use all legal potions first, play Powers before Attacks, prioritize Skills when visible incoming intent exceeds block, prefer higher-cost cards, target the lowest-HP currently attacking monster, and pick the first legal card-selection option";
        report["cohort_generation"] = "fixed prospective Ironclad A20 game seeds 1..12; on the visible Act 1 map follow a shortest reachable route to the earliest Elite, breaking ties toward more rest nodes and fewer intervening Monsters; elsewhere choose the first legal game action; use the deterministic public heuristic in prelude battles; retain every seed including setup losses";
        report["target_battle_start"] = "first naturally reached Act 1 Elite battle from the fixed native seeded run";
        pb::list gameSeedRows;
        for (const auto seed : activeGameSeeds) gameSeedRows.append(seed);
        pb::list replicateSeedRows;
        for (const auto seed : activeReplicateSeeds) replicateSeedRows.append(seed);
        pb::list budgetRows;
        for (const auto budget : activeBudgets) budgetRows.append(budget);
        report["game_seeds"] = gameSeedRows;
        report["replicate_seeds"] = replicateSeedRows;
        report["search_budgets_per_decision"] = budgetRows;
        report["particle_count"] = kParticleCount;
        report["budget_semantics"] = "total shared-tree simulations per actual public decision; not multiplied by particle count; particle index cycles simulation_index modulo 32";
        report["max_public_decisions_per_battle"] = kBattleDecisionLimit;
        report["rollout_decision_limit"] = kRolloutDecisionLimit;
        report["leaf_evaluation"] = "terminal battle win/loss is +1/-1; at 24 public-action decisions clamp(player_hp/max_hp - 0.5 * sum(monster_hp/max_hp), -1, 1) using public state";
        report["work_counter_definition"] = "native public-action steps count step_public_action calls in sampled search trajectories plus real controller actions; simulator-internal queue steps are not separately instrumented";
        report["pairing"] = "For each fixed game seed, prepare the target once before controller outcomes and capture one authoritative full simulator checkpoint. Each policy restores a copy of that exact checkpoint; the complete public start identity is rechecked after restore. Later RNG streams may diverge after different actions.";
        report["uncertainty_method"] = "For paired effects, average controller replicate effects within each game seed, then bootstrap complete game-seed clusters with replacement (10,000 replicates, fixed bootstrap seed); percentile intervals describe this small fixed cohort and are not a confirmatory significance claim.";
        report["paired_outcome_semantics"] = "Paired win deltas use terminal battle pairs. Paired HP-loss deltas include both terminal wins and losses, not only wins. Setup failures and incomplete or sampler-unsupported attempts remain in the all-attempt summaries and are not recoded as defeats.";

        const std::string progressPath = outputPath + ".progress.jsonl";
        std::ofstream progress(progressPath, std::ios::trunc);
        if (!progress) {
            throw std::runtime_error("could not open durable study progress file: " + progressPath);
        }

        std::map<int, PreparedBattleStart> preparedStarts;
        int cohortSetupIndex = 0;
        for (const int gameSeed : activeGameSeeds) {
            ++cohortSetupIndex;
            pb::dict progressRow;
            progressRow["stage"] = "cohort_setup_started";
            progressRow["cohort_seed_index"] = cohortSetupIndex;
            progressRow["cohort_seed_count"] = static_cast<int>(activeGameSeeds.size());
            progressRow["game_seed"] = gameSeed;
            progress << canonicalJson(progressRow) << '\n';
            progress.flush();
            if (!progress) {
                throw std::runtime_error("failed flushing cohort setup start record");
            }
            auto prepared = prepareBattleStart(gameSeed);
            progressRow["stage"] = "cohort_setup_finished";
            progressRow["result"] = prepared.evidence;
            progress << canonicalJson(progressRow) << '\n';
            progress.flush();
            if (!progress) {
                throw std::runtime_error("failed flushing cohort setup result record");
            }
            preparedStarts.emplace(gameSeed, std::move(prepared));
        }

        struct RunConfig {
            std::string policy;
            int replicateSeed;
            int budget;
        };
        std::vector<RunConfig> configs;
        configs.push_back({"public_heuristic_baseline", 0, 0});
        for (const int replicateSeed : activeReplicateSeeds) {
            for (const int budget : activeBudgets) {
                configs.push_back({"shared_public_search", replicateSeed, budget});
            }
        }
        const int gameSeedCount = static_cast<int>(activeGameSeeds.size());
        const int runCount = gameSeedCount * static_cast<int>(configs.size());
        std::vector<Attempt> attempts;
        attempts.reserve(static_cast<std::size_t>(runCount));
        int runIndex = 0;
        for (const int gameSeed : activeGameSeeds) {
            for (const auto &config : configs) {
                ++runIndex;
                std::cerr << "run game_seed=" << gameSeed
                          << " policy=" << config.policy
                          << " replicate_seed=" << config.replicateSeed
                          << " budget=" << config.budget << '\n';
                pb::dict progressRow;
                progressRow["stage"] = "started";
                progressRow["run_index"] = runIndex;
                progressRow["run_count"] = runCount;
                progressRow["game_seed"] = gameSeed;
                progressRow["policy"] = config.policy;
                progressRow["replicate_seed"] = config.replicateSeed;
                progressRow["budget"] = config.budget;
                progress << canonicalJson(progressRow) << '\n';
                progress.flush();
                if (!progress) {
                    throw std::runtime_error("failed flushing study progress start record");
                }

                attempts.push_back(runAttempt(preparedStarts.at(gameSeed), config.replicateSeed,
                        config.budget, config.policy, simulatorCommit, runnerCommit));
                progressRow["stage"] = "finished";
                progressRow["result"] = attempts.back().evidence;
                progress << canonicalJson(progressRow) << '\n';
                progress.flush();
                if (!progress) {
                    throw std::runtime_error("failed flushing study progress result record");
                }
            }
        }
        progress.close();

        pb::list runRows;
        std::map<int, pb::dict> cohortBySeed;
        std::map<int, std::string> setupStatusBySeed;
        for (const auto &attempt : attempts) {
            runRows.append(attempt.evidence);
            const auto status = stringField(attempt.evidence, "setup_status");
            const auto knownStatus = setupStatusBySeed.find(attempt.gameSeed);
            if (knownStatus == setupStatusBySeed.end()) {
                setupStatusBySeed.emplace(attempt.gameSeed, status);
                pb::dict cohortRow;
                cohortRow["game_seed"] = attempt.gameSeed;
                cohortRow["ascension"] = kAscension;
                cohortRow["setup_status"] = status;
                cohortRow["target_start_reached"] = attempt.battleStarted;
                cohortRow["prelude_battles"] = attempt.evidence.contains("prelude_battles")
                        ? attempt.evidence["prelude_battles"] : pb::object(pb::none());
                cohortRow["prelude_public_decisions"] =
                        attempt.evidence.contains("prelude_public_decisions")
                                ? attempt.evidence["prelude_public_decisions"]
                                : pb::object(pb::none());
                if (attempt.battleStarted) {
                    cohortRow["battle_start_identity"] = attempt.battleStartIdentity;
                    cohortRow["pre_battle_setup_steps"] =
                            attempt.evidence["pre_battle_setup_steps"];
                } else if (attempt.evidence.contains("setup_error")) {
                    cohortRow["setup_error"] = attempt.evidence["setup_error"];
                    cohortRow["setup_final_screen"] = attempt.evidence.contains("setup_final_screen")
                            ? attempt.evidence["setup_final_screen"] : pb::object(pb::none());
                    cohortRow["setup_final_game_outcome"] =
                            attempt.evidence.contains("setup_final_game_outcome")
                                    ? attempt.evidence["setup_final_game_outcome"]
                                    : pb::object(pb::none());
                }
                cohortBySeed.emplace(attempt.gameSeed, cohortRow);
                continue;
            }
            if (knownStatus->second != status) {
                throw std::logic_error("fixed setup did not reproduce a consistent status for one seed");
            }
            auto &cohortRow = cohortBySeed.at(attempt.gameSeed);
            if (attempt.battleStarted) {
                if (!cohortRow.contains("battle_start_identity")
                        || canonicalJson(cohortRow["battle_start_identity"].cast<pb::dict>())
                                != canonicalJson(attempt.battleStartIdentity)) {
                    throw std::logic_error("controller runs did not reproduce an identical battle start");
                }
            }
        }

        report["attempted_battles"] = runRows;
        pb::list cohortRows;
        int reachedStarts = 0;
        int setupFailures = 0;
        std::map<std::string, int> encounterCounts;
        for (const auto &[seed, row] : cohortBySeed) {
            (void) seed;
            cohortRows.append(row);
            if (row["target_start_reached"].cast<bool>()) {
                ++reachedStarts;
                const auto identity = row["battle_start_identity"].cast<pb::dict>();
                ++encounterCounts[stringField(identity, "encounter_id")];
            } else {
                ++setupFailures;
            }
        }
        report["cohort_manifest"] = cohortRows;
        report["fixed_game_seed_count"] = sizeof(kGameSeeds) / sizeof(kGameSeeds[0]);
        report["run_game_seed_count"] = gameSeedCount;
        report["target_act1_elite_starts_reached"] = reachedStarts;
        report["prelude_setup_failures"] = setupFailures;
        pb::dict encounterSummary;
        for (const auto &[encounter, count] : encounterCounts) {
            encounterSummary[pb::str(encounter)] = count;
        }
        report["target_encounter_counts"] = encounterSummary;

        pb::list summaries;
        summaries.append(summarizePolicy("public_heuristic_baseline", 0, attempts));
        for (const int budget : activeBudgets) {
            summaries.append(summarizePolicy("shared_public_search", budget, attempts));
        }
        report["aggregate_by_policy"] = summaries;

        auto findAttempt = [&](const int gameSeed, const std::string &policy,
                const int replicateSeed, const int budget) -> const Attempt * {
            for (const auto &attempt : attempts) {
                if (attempt.gameSeed == gameSeed && attempt.policy == policy
                        && attempt.replicateSeed == replicateSeed
                        && attempt.budget == budget) {
                    return &attempt;
                }
            }
            return nullptr;
        };

        pb::list pairedRows;
        pb::list pairedBudgetSummaries;
        for (const int budget : activeBudgets) {
            int targetPairs = 0;
            int bothCompleted = 0;
            int winChanges = 0;
            int searchWins = 0;
            int baselineWins = 0;
            std::vector<double> winDeltas;
            std::vector<double> hpLossDeltas;
            std::map<int, std::vector<double>> seedWinDeltas;
            std::map<int, std::vector<double>> seedHpLossDeltas;
            for (const int gameSeed : activeGameSeeds) {
                const auto baseline = findAttempt(gameSeed,
                        "public_heuristic_baseline", 0, 0);
                std::vector<double> thisSeedWinDeltas;
                std::vector<double> thisSeedHpLossDeltas;
                for (const int replicateSeed : activeReplicateSeeds) {
                    const auto search = findAttempt(gameSeed,
                            "shared_public_search", replicateSeed, budget);
                    if (search == nullptr || baseline == nullptr) {
                        throw std::logic_error("missing paired controller attempt");
                    }
                    pb::dict pairRow;
                    pairRow["game_seed"] = gameSeed;
                    pairRow["replicate_seed"] = replicateSeed;
                    pairRow["search_budget"] = budget;
                    pairRow["baseline_policy"] = baseline->policy;
                    pairRow["baseline_outcome"] = baseline->evidence["battle_outcome"];
                    pairRow["search_outcome"] = search->evidence["battle_outcome"];
                    pairRow["baseline_first_unsupported_category"] =
                            baseline->firstUnsupportedCategory.empty()
                                    ? pb::object(pb::none())
                                    : pb::object(pb::str(baseline->firstUnsupportedCategory));
                    pairRow["search_first_unsupported_category"] =
                            search->firstUnsupportedCategory.empty()
                                    ? pb::object(pb::none())
                                    : pb::object(pb::str(search->firstUnsupportedCategory));
                    pairRow["same_target_start"] = baseline->battleStarted
                            && search->battleStarted;
                    std::string pairStatus = "setup_failed";
                    if (baseline->battleStarted && search->battleStarted) {
                        ++targetPairs;
                        if (baseline->battleCompleted && search->battleCompleted) {
                            pairStatus = "both_battles_terminal";
                            ++bothCompleted;
                            searchWins += search->won;
                            baselineWins += baseline->won;
                            const double winDelta = (search->won ? 1.0 : 0.0)
                                    - (baseline->won ? 1.0 : 0.0);
                            winDeltas.push_back(winDelta);
                            const double hpLossDelta = static_cast<double>(
                                    search->hpLost - baseline->hpLost);
                            hpLossDeltas.push_back(hpLossDelta);
                            thisSeedWinDeltas.push_back(winDelta);
                            thisSeedHpLossDeltas.push_back(hpLossDelta);
                            winChanges += winDelta != 0.0;
                            pairRow["search_win_minus_baseline"] = winDelta;
                            pairRow["search_hp_lost_minus_baseline"] =
                                    search->hpLost - baseline->hpLost;
                        } else if (!search->battleCompleted
                                && !search->firstUnsupportedCategory.empty()) {
                            pairStatus = "search_sampler_unsupported";
                        } else if (!search->battleCompleted
                                && search->evidence.contains("controller_error")) {
                            pairStatus = "search_controller_error";
                        } else if (!baseline->battleCompleted) {
                            pairStatus = "baseline_incomplete";
                        } else {
                            pairStatus = "search_incomplete";
                        }
                    }
                    pairRow["pair_status"] = pairStatus;
                    pairedRows.append(pairRow);
                }
                if (thisSeedWinDeltas.size() == activeReplicateSeeds.size()
                        && thisSeedHpLossDeltas.size() == activeReplicateSeeds.size()) {
                    seedWinDeltas.emplace(gameSeed, thisSeedWinDeltas);
                    seedHpLossDeltas.emplace(gameSeed, thisSeedHpLossDeltas);
                }
            }
            pb::dict pairedSummary;
            pairedSummary["search_budget_per_decision"] = budget;
            pairedSummary["all_search_baseline_pair_attempts"] = gameSeedCount
                    * static_cast<int>(activeReplicateSeeds.size());
            pairedSummary["paired_target_start_attempts"] = targetPairs;
            pairedSummary["both_battles_terminal"] = bothCompleted;
            pairedSummary["search_wins_among_completed_pairs"] = searchWins;
            pairedSummary["baseline_wins_among_completed_pairs"] = baselineWins;
            pairedSummary["paired_win_outcome_changes"] = winChanges;
            pairedSummary["mean_win_delta_search_minus_baseline"] = optionalMean(winDeltas);
            pairedSummary["mean_hp_loss_delta_search_minus_baseline"] = optionalMean(hpLossDeltas);
            pairedSummary["paired_hp_loss_delta_samples"] = static_cast<int>(hpLossDeltas.size());
            pairedSummary["win_delta_game_seed_cluster_bootstrap"] =
                    clusterBootstrapSummary(seedWinDeltas,
                            kSamplerSeedBase + static_cast<std::uint64_t>(budget));
            pairedSummary["hp_loss_delta_game_seed_cluster_bootstrap"] =
                    clusterBootstrapSummary(seedHpLossDeltas,
                            kSearchSeedBase + static_cast<std::uint64_t>(budget));
            pairedBudgetSummaries.append(pairedSummary);
        }
        report["paired_search_vs_baseline_attempts"] = pairedRows;
        report["paired_search_vs_baseline_summary"] = pairedBudgetSummaries;

        pb::list budgetPairRows;
        std::map<int, std::vector<double>> budgetWinDeltasBySeed;
        std::map<int, std::vector<double>> budgetHpLossDeltasBySeed;
        if (activeBudgets.size() == 2) for (const int gameSeed : activeGameSeeds) {
            std::vector<double> thisSeedWinDeltas;
            std::vector<double> thisSeedHpLossDeltas;
            for (const int replicateSeed : activeReplicateSeeds) {
                const auto lower = findAttempt(gameSeed, "shared_public_search",
                        replicateSeed, kBudgets[0]);
                const auto higher = findAttempt(gameSeed, "shared_public_search",
                        replicateSeed, kBudgets[1]);
                if (lower == nullptr || higher == nullptr) {
                    throw std::logic_error("missing paired search-budget attempt");
                }
                pb::dict row;
                row["game_seed"] = gameSeed;
                row["replicate_seed"] = replicateSeed;
                row["lower_budget_outcome"] = lower->evidence["battle_outcome"];
                row["higher_budget_outcome"] = higher->evidence["battle_outcome"];
                row["both_battles_terminal"] = lower->battleCompleted
                        && higher->battleCompleted;
                if (lower->battleCompleted && higher->battleCompleted) {
                    const double winDelta = (higher->won ? 1 : 0)
                            - (lower->won ? 1 : 0);
                    const double hpLossDelta = static_cast<double>(
                            higher->hpLost - lower->hpLost);
                    row["win_delta_384_minus_96"] = winDelta;
                    row["hp_loss_delta_384_minus_96"] = hpLossDelta;
                    thisSeedWinDeltas.push_back(winDelta);
                    thisSeedHpLossDeltas.push_back(hpLossDelta);
                }
                budgetPairRows.append(row);
            }
            if (thisSeedWinDeltas.size() == activeReplicateSeeds.size()
                    && thisSeedHpLossDeltas.size() == activeReplicateSeeds.size()) {
                budgetWinDeltasBySeed.emplace(gameSeed, thisSeedWinDeltas);
                budgetHpLossDeltasBySeed.emplace(gameSeed, thisSeedHpLossDeltas);
            }
        }
        report["paired_budget_attempts"] = budgetPairRows;
        pb::dict budgetPairSummary;
        budgetPairSummary["win_delta_384_minus_96_game_seed_cluster_bootstrap"] =
                clusterBootstrapSummary(budgetWinDeltasBySeed,
                        kSamplerSeedBase + static_cast<std::uint64_t>(kBudgets[0]));
        budgetPairSummary["hp_loss_delta_384_minus_96_game_seed_cluster_bootstrap"] =
                clusterBootstrapSummary(budgetHpLossDeltasBySeed,
                        kSearchSeedBase + static_cast<std::uint64_t>(kBudgets[1]));
        report["paired_budget_seed_summary"] = budgetPairSummary;
        report["limitations"] = pb::make_tuple(
                "fixed prospective seed cohort has 12 A20 setup attempts; seed attempts that lose before the target Elite remain in denominators",
                "target starts are Act 1 Elites reached through a deterministic public-heuristic prelude and do not represent a complete run or broad A20 claim",
                "the baseline deliberately remains simple and does not estimate card effects; it uses public legal actions, visible card type/cost, visible monster intent, and visible HP",
                "paired battles share the same complete initial state and seed, but future random streams may diverge after actions differ",
                "sampler-unsupported search attempts stop without fallback and are not counted as defeats",
                "search rollouts truncate at 24 public-action decisions with a public HP-fraction heuristic",
                "wall times are local single-process measurements and exclude compilation",
                "small clustered cohort and support-conditioned outcomes do not establish statistical significance or policy strength");
        report["conclusion"] = "pending_review_of_paired_outcome_signal";

        std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("could not open study evidence output: " + outputPath);
        }
        output << indentedJson(report) << '\n';
        output.close();
        if (!output) {
            throw std::runtime_error("failed writing study evidence output: " + outputPath);
        }
        std::remove(progressPath.c_str());
        std::cout << "ISSUE14_PAIRED_PUBLIC_OUTCOME_STUDY_PASS\n";
        std::cout << "runs=" << attempts.size()
                  << " elite_starts=" << reachedStarts
                  << " setup_failures=" << setupFailures << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "ISSUE14_PAIRED_PUBLIC_OUTCOME_STUDY_FAIL: "
                  << error.what() << '\n';
        return 1;
    }
}
