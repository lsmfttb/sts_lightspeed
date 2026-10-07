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

constexpr int kGameSeeds[] = {1, 2, 3, 4, 5, 6, 7, 8};
constexpr int kReplicateSeeds[] = {17, 101, 1009};
constexpr int kBudgets[] = {96, 384};
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
    bool battleStarted = false;
    bool battleCompleted = false;
    bool fullySupportedCompleted = false;
    bool won = false;
    int hpLost = 0;
    int publicDecisionsAttempted = 0;
    int supportedDecisions = 0;
    std::uint64_t simulations = 0;
    std::uint64_t nativeActionSteps = 0;
    std::string firstUnsupportedCategory;
    double trialWallSeconds = 0.0;
    double battleWallSeconds = 0.0;
    std::vector<DecisionLatency> latencies;
};

Attempt runAttempt(
        const int gameSeed,
        const int replicateSeed,
        const int budget,
        const std::string &simulatorCommit,
        const std::string &runnerCommit) {
    Attempt attempt;
    attempt.gameSeed = gameSeed;
    attempt.replicateSeed = replicateSeed;
    attempt.budget = budget;
    attempt.evidence = pb::dict();
    const auto trialStart = Clock::now();
    pb::list decisions;

    try {
        StepSimulator simulator(CharacterClass::IRONCLAD,
                static_cast<std::uint64_t>(gameSeed), 0);
        int setupSteps = 0;
        bool reachedBattle = false;
        for (; setupSteps < kSetupStepLimit; ++setupSteps) {
            const auto snapshot = simulator.snapshot();
            if (stringField(snapshot, "screen_state") == "BATTLE"
                    && snapshot["battle_active"].cast<bool>()) {
                reachedBattle = true;
                break;
            }
            const auto actions = simulator.legalActions();
            if (actions.empty()) {
                break;
            }
            (void) simulator.step(actions.front());
        }
        if (!reachedBattle) {
            attempt.evidence["setup_status"] = "failed_to_reach_battle_within_step_limit";
            attempt.evidence["pre_battle_setup_steps"] = setupSteps;
            attempt.evidence["pre_battle_setup_step_limit"] = kSetupStepLimit;
            throw std::runtime_error("native seeded setup did not reach a battle");
        }

        auto startState = simulator.publicBattleState();
        if (containsPrivateField(startState)) {
            throw std::logic_error("battle start public state contains private fields");
        }
        attempt.battleStarted = true;
        const auto startPlayer = startState["player"].cast<pb::dict>();
        attempt.battleStartIdentity["game_seed"] = gameSeed;
        attempt.battleStartIdentity["act"] = intField(startState, "act");
        attempt.battleStartIdentity["floor_num"] = intField(startState, "floor_num");
        attempt.battleStartIdentity["encounter_id"] = stringField(startState, "encounter_id");
        attempt.battleStartIdentity["information_fidelity"] =
                stringField(startState, "information_fidelity");
        attempt.battleStartIdentity["input_state"] = stringField(startState, "input_state");
        attempt.battleStartIdentity["player"] = startPlayer;
        attempt.battleStartIdentity["hand"] = startState["hand"];
        attempt.battleStartIdentity["monsters"] = startState["monsters"];
        attempt.battleStartIdentity["draw_pile_size"] = startState["draw_pile_size"];
        attempt.battleStartIdentity["draw_pile_membership"] =
                startState["draw_pile_membership"];
        attempt.battleStartIdentity["persistent_resources"] =
                startState["persistent_resources"];
        attempt.evidence["setup_status"] = "battle_started";
        attempt.evidence["pre_battle_setup_steps"] = setupSteps;
        attempt.evidence["battle_start_identity_ref"] = gameSeed;
        attempt.evidence["encounter_id"] = stringField(startState, "encounter_id");
        attempt.evidence["act"] = intField(startState, "act");
        attempt.evidence["floor_num"] = intField(startState, "floor_num");
        attempt.evidence["starting_player_hp"] = intField(
                startState["player"].cast<pb::dict>(), "current_hp");
        attempt.evidence["starting_player_max_hp"] = intField(
                startState["player"].cast<pb::dict>(), "max_hp");
        attempt.evidence["public_start_information_fidelity"] =
                stringField(startState, "information_fidelity");
        attempt.evidence["controller_semantics"] =
                "one UCT tree per decision, shared across sampled particles; keyed by public state/action/result history; only public root statistics choose the executed action";

        const int startingHp = intField(startState["player"].cast<pb::dict>(), "current_hp");
        const auto battleStart = Clock::now();
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
            decisionEvidence["sampler_seed"] = samplerSeed;
            decisionEvidence["search_seed"] = searchSeed;
            ++attempt.publicDecisionsAttempted;

            std::string phase = "public_state";
            try {
                auto publicState = simulator.publicBattleState();
                decisionEvidence["information_fidelity"] =
                        stringField(publicState, "information_fidelity");
                if (stringField(publicState, "information_fidelity") != "supported") {
                    throw std::runtime_error("public state reports unsupported_fidelity");
                }
                if (containsPrivateField(publicState)) {
                    throw std::logic_error("public state contains a private field");
                }

                phase = "sampler";
                auto preflightParticle = simulator.samplePublicConsistentHiddenFuture(
                        samplerSeed, 0);
                if (canonicalJson(preflightParticle.publicBattleState())
                        != canonicalJson(publicState)) {
                    throw std::logic_error("sampler preflight changed public state");
                }
                ++attempt.supportedDecisions;

                phase = "shared_search";
                const auto searchStart = Clock::now();
                const auto search = searchDecision(simulator, publicState, budget,
                        samplerSeed, searchSeed);
                const double searchSeconds = std::chrono::duration<double>(
                        Clock::now() - searchStart).count();
                decisionEvidence["search"] = search.evidence;
                attempt.simulations += static_cast<std::uint64_t>(budget);
                attempt.nativeActionSteps += search.evidence[
                        "native_public_action_steps"].cast<std::uint64_t>();

                phase = "real_public_action";
                const auto transition = simulator.stepPublicAction(search.selectedAction);
                ++attempt.nativeActionSteps;
                decisionEvidence["selected_public_action"] = search.selectedAction;
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
                decisionEvidence["decision_wall_seconds"] = totalSeconds;
                decisionEvidence["search_wall_seconds"] = searchSeconds;
                attempt.latencies.push_back({budget, totalSeconds, searchSeconds});
            } catch (const std::exception &error) {
                const auto category = failureCategory(phase, error.what());
                decisionEvidence["decision_status"] = phase == "sampler"
                        ? "sampler_unsupported" : "controller_error";
                decisionEvidence["failure_category"] = category;
                decisionEvidence["failure_reason"] = error.what();
                if (phase == "sampler" || phase == "public_state") {
                    attempt.firstUnsupportedCategory = category;
                    stopReason = "unsupported_sampler_decision";
                    pb::dict unsupported;
                    unsupported["decision_index"] = decisionIndex;
                    unsupported["category"] = category;
                    unsupported["reason"] = error.what();
                    attempt.evidence["first_unsupported_decision"] = unsupported;
                } else {
                    stopReason = "controller_error";
                    attempt.evidence["controller_error"] = error.what();
                }
                stop = true;
                decisionEvidence["decision_wall_seconds"] = std::chrono::duration<double>(
                        Clock::now() - decisionStart).count();
            }
            decisions.append(decisionEvidence);
        }

        attempt.battleWallSeconds = std::chrono::duration<double>(
                Clock::now() - battleStart).count();
        attempt.fullySupportedCompleted = attempt.battleCompleted
                && attempt.firstUnsupportedCategory.empty()
                && !attempt.evidence.contains("controller_error")
                && attempt.publicDecisionsAttempted == attempt.supportedDecisions;
        attempt.evidence["stop_reason"] = stopReason;
        attempt.evidence["battle_completed_under_normal_public_controller"] =
                attempt.battleCompleted;
        attempt.evidence["fully_supported_completed"] = attempt.fullySupportedCompleted;
        attempt.evidence["battle_outcome"] = attempt.battleCompleted
                ? (attempt.won ? "win" : "loss") : "incomplete";
        attempt.evidence["combat_ending_player_hp"] = simulator.bc.player.curHp;
        attempt.evidence["persistent_ending_player_hp"] = simulator.gc.curHp;
        attempt.evidence["hp_lost_during_battle"] = attempt.battleCompleted
                ? pb::object(pb::int_(attempt.hpLost)) : pb::object(pb::none());
    } catch (const std::exception &error) {
        if (!attempt.evidence.contains("setup_status")) {
            attempt.evidence["setup_status"] = "setup_error";
        }
        attempt.evidence["setup_error"] = error.what();
    }

    attempt.evidence["schema_id"] = "normal-public-complete-battle-run-v1";
    attempt.evidence["simulator_base_commit"] = simulatorCommit;
    attempt.evidence["study_runner_commit"] = runnerCommit;
    attempt.evidence["character"] = "IRONCLAD";
    attempt.evidence["ascension"] = 0;
    attempt.evidence["game_seed"] = gameSeed;
    attempt.evidence["controller_replicate_seed"] = replicateSeed;
    attempt.evidence["search_budget_per_supported_decision"] = budget;
    attempt.evidence["particle_count"] = kParticleCount;
    attempt.evidence["rollout_decision_limit"] = kRolloutDecisionLimit;
    attempt.evidence["pre_battle_setup_policy"] =
            "advance native seeded game with first currently legal action until the first battle";
    attempt.evidence["public_decisions_attempted"] = attempt.publicDecisionsAttempted;
    attempt.evidence["sampling_supported_decisions"] = attempt.supportedDecisions;
    attempt.evidence["sampling_supported_fraction"] = attempt.publicDecisionsAttempted > 0
            ? pb::object(pb::float_(static_cast<double>(attempt.supportedDecisions)
                    / attempt.publicDecisionsAttempted))
            : pb::object(pb::none());
    attempt.evidence["total_shared_search_simulations"] = attempt.simulations;
    attempt.evidence["search_and_controller_native_public_action_steps"] =
            attempt.nativeActionSteps;
    attempt.evidence["battle_wall_seconds"] = attempt.battleWallSeconds;
    attempt.evidence["trial_wall_seconds_including_setup"] = std::chrono::duration<double>(
            Clock::now() - trialStart).count();
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

pb::dict summarizeBudget(const int budget, const std::vector<Attempt> &attempts) {
    int count = 0;
    int started = 0;
    int completed = 0;
    int fullySupported = 0;
    int wins = 0;
    int losses = 0;
    std::uint64_t simulations = 0;
    std::uint64_t publicSteps = 0;
    int attemptedDecisions = 0;
    int supportedDecisions = 0;
    std::map<std::string, int> unsupportedCategories;
    std::vector<double> totalWall;
    std::vector<double> decisionWall;
    std::vector<double> searchWall;
    std::vector<double> hpLosses;

    for (const auto &attempt : attempts) {
        if (attempt.budget != budget) {
            continue;
        }
        ++count;
        started += attempt.battleStarted;
        completed += attempt.battleCompleted;
        fullySupported += attempt.fullySupportedCompleted;
        if (attempt.fullySupportedCompleted) {
            wins += attempt.won;
            losses += !attempt.won;
            hpLosses.push_back(attempt.hpLost);
        }
        simulations += attempt.simulations;
        publicSteps += attempt.nativeActionSteps;
        attemptedDecisions += attempt.publicDecisionsAttempted;
        supportedDecisions += attempt.supportedDecisions;
        if (!attempt.firstUnsupportedCategory.empty()) {
            ++unsupportedCategories[attempt.firstUnsupportedCategory];
        }
        totalWall.push_back(attempt.battleWallSeconds);
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
    summary["search_budget_per_decision"] = budget;
    summary["particle_count"] = kParticleCount;
    summary["attempted_battles_all_denominator"] = count;
    summary["battles_reaching_native_start"] = started;
    summary["battles_completed_under_normal_public_controller"] = completed;
    summary["fully_supported_completed_battles"] = fullySupported;
    summary["wins_among_fully_supported_completed"] = wins;
    summary["losses_among_fully_supported_completed"] = losses;
    summary["fully_supported_completion_fraction_all_attempts"] = count > 0
            ? pb::object(pb::float_(static_cast<double>(fullySupported) / count))
            : pb::object(pb::none());
    summary["win_fraction_among_fully_supported_completed"] = fullySupported > 0
            ? pb::object(pb::float_(static_cast<double>(wins) / fullySupported))
            : pb::object(pb::none());
    summary["mean_hp_lost_during_fully_supported_completed_battles"] = optionalMean(hpLosses);
    summary["public_decisions_attempted_all_runs"] = attemptedDecisions;
    summary["sampling_supported_decisions_all_runs"] = supportedDecisions;
    summary["sampling_supported_fraction_all_decisions"] = attemptedDecisions > 0
            ? pb::object(pb::float_(static_cast<double>(supportedDecisions) / attemptedDecisions))
            : pb::object(pb::none());
    summary["unsupported_first_decision_categories"] = categories;
    summary["total_shared_search_simulations"] = simulations;
    summary["mean_shared_search_simulations_per_attempted_battle"] = count > 0
            ? pb::object(pb::float_(static_cast<double>(simulations) / count))
            : pb::object(pb::none());
    summary["search_and_controller_native_public_action_steps"] = publicSteps;
    summary["battle_wall_seconds_p50_all_attempts"] = totalWall.empty()
            ? pb::object(pb::none()) : pb::object(pb::float_(percentile(totalWall, 0.50)));
    summary["battle_wall_seconds_p95_all_attempts"] = totalWall.empty()
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

} // namespace

int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "usage: study-end-to-end-public-battle <simulator-base-sha> <runner-sha> <output-json>\n";
        return 2;
    }
    try {
        pb::scoped_interpreter interpreter{};
        const std::string simulatorCommit = argv[1];
        const std::string runnerCommit = argv[2];
        const std::string outputPath = argv[3];
        pb::dict report;
        report["schema_id"] = "normal-public-end-to-end-battle-study-v1";
        report["simulator_base_commit"] = simulatorCommit;
        report["study_runner_commit"] = runnerCommit;
        report["information_regime"] = "normal_public";
        report["controller"] = "adaptive shared-public-tree UCT; one tree per actual decision, shared by 32 sampled particles; public state/action/result history keys; action selected from shared root visits/value only";
        report["sampler_interpretation"] = "reproducible proposal distribution, not an exact posterior";
        report["leaf_evaluation"] = "terminal battle win/loss is +1/-1; at 24 public-action decisions clamp(player_hp/max_hp - 0.5 * sum(monster_hp/max_hp), -1, 1) using public state";
        report["cohort_generation"] = "IRONCLAD, ascension 0, game seeds 1..8; first native battle reached by advancing each seeded run with its first currently legal action; no encounter or sampler-support filtering";
        report["game_seeds"] = pb::make_tuple(1, 2, 3, 4, 5, 6, 7, 8);
        report["replicate_seeds"] = pb::make_tuple(17, 101, 1009);
        report["search_budgets_per_decision"] = pb::make_tuple(96, 384);
        report["particle_count"] = kParticleCount;
        report["budget_semantics"] = "total shared-tree simulations per actual public decision; not multiplied by particle count; particle index cycles simulation_index modulo 32";
        report["max_public_decisions_per_battle"] = kBattleDecisionLimit;
        report["rollout_decision_limit"] = kRolloutDecisionLimit;
        report["work_counter_definition"] = "native public-action steps count step_public_action calls in sampled search trajectories plus real controller actions; simulator-internal queue steps are not separately instrumented";

        const std::string progressPath = outputPath + ".progress.jsonl";
        std::ofstream progress(progressPath, std::ios::trunc);
        if (!progress) {
            throw std::runtime_error("could not open durable study progress file: " + progressPath);
        }
        std::vector<Attempt> attempts;
        int runIndex = 0;
        constexpr int runCount = static_cast<int>(
                sizeof(kGameSeeds) / sizeof(kGameSeeds[0])
                * sizeof(kReplicateSeeds) / sizeof(kReplicateSeeds[0])
                * sizeof(kBudgets) / sizeof(kBudgets[0]));
        for (const int gameSeed : kGameSeeds) {
            for (const int replicateSeed : kReplicateSeeds) {
                for (const int budget : kBudgets) {
                    ++runIndex;
                    std::cerr << "run game_seed=" << gameSeed
                              << " replicate_seed=" << replicateSeed
                              << " budget=" << budget << '\n';
                    pb::dict progressRow;
                    progressRow["stage"] = "started";
                    progressRow["run_index"] = runIndex;
                    progressRow["run_count"] = runCount;
                    progressRow["game_seed"] = gameSeed;
                    progressRow["replicate_seed"] = replicateSeed;
                    progressRow["budget"] = budget;
                    progress << canonicalJson(progressRow) << '\n';
                    progress.flush();
                    if (!progress) {
                        throw std::runtime_error("failed flushing study progress start record");
                    }

                    attempts.push_back(runAttempt(gameSeed, replicateSeed, budget,
                            simulatorCommit, runnerCommit));
                    progressRow["stage"] = "finished";
                    progressRow["result"] = attempts.back().evidence;
                    progress << canonicalJson(progressRow) << '\n';
                    progress.flush();
                    if (!progress) {
                        throw std::runtime_error("failed flushing study progress result record");
                    }
                }
            }
        }
        progress.close();

        pb::list runRows;
        std::map<int, pb::dict> cohortBySeed;
        for (const auto &attempt : attempts) {
            runRows.append(attempt.evidence);
            auto cohortPosition = cohortBySeed.find(attempt.gameSeed);
            if (cohortPosition == cohortBySeed.end()) {
                pb::dict cohortRow;
                cohortRow["game_seed"] = attempt.gameSeed;
                cohortRow["battle_started"] = attempt.battleStarted;
                if (attempt.battleStarted) {
                    cohortRow["battle_start_identity"] = attempt.battleStartIdentity;
                } else if (attempt.evidence.contains("setup_error")) {
                    cohortRow["setup_error"] = attempt.evidence["setup_error"];
                }
                cohortPosition = cohortBySeed.emplace(attempt.gameSeed, cohortRow).first;
            }
            auto &cohortRow = cohortPosition->second;
            if (attempt.battleStarted) {
                if (cohortRow.contains("battle_start_identity")) {
                    if (canonicalJson(cohortRow["battle_start_identity"].cast<pb::dict>())
                            != canonicalJson(attempt.battleStartIdentity)) {
                        throw std::logic_error(
                                "same game seed did not reproduce its battle-start identity");
                    }
                } else {
                    cohortRow["battle_started"] = true;
                    cohortRow["battle_start_identity"] = attempt.battleStartIdentity;
                }
            }
        }
        report["attempted_battles"] = runRows;
        pb::list cohortRows;
        for (const auto &[seed, row] : cohortBySeed) {
            (void) seed;
            cohortRows.append(row);
        }
        report["cohort_manifest"] = cohortRows;
        pb::list summaries;
        for (const int budget : kBudgets) {
            summaries.append(summarizeBudget(budget, attempts));
        }
        report["aggregate_by_budget"] = summaries;

        int pairedCompleted = 0;
        int pairedWinChanges = 0;
        std::vector<double> pairedHpLossDeltas;
        for (const int gameSeed : kGameSeeds) {
            for (const int replicateSeed : kReplicateSeeds) {
                const Attempt *lower = nullptr;
                const Attempt *higher = nullptr;
                for (const auto &attempt : attempts) {
                    if (attempt.gameSeed != gameSeed || attempt.replicateSeed != replicateSeed) {
                        continue;
                    }
                    if (attempt.budget == kBudgets[0]) lower = &attempt;
                    if (attempt.budget == kBudgets[1]) higher = &attempt;
                }
                if (lower == nullptr || higher == nullptr
                        || !lower->fullySupportedCompleted
                        || !higher->fullySupportedCompleted) {
                    continue;
                }
                ++pairedCompleted;
                pairedWinChanges += lower->won != higher->won;
                pairedHpLossDeltas.push_back(
                        static_cast<double>(higher->hpLost - lower->hpLost));
            }
        }
        pb::dict paired;
        paired["paired_fully_supported_completed_runs"] = pairedCompleted;
        paired["paired_win_outcome_changes"] = pairedWinChanges;
        paired["mean_hp_lost_delta_384_minus_96"] = optionalMean(pairedHpLossDeltas);
        paired["interpretation"] = "descriptive paired signal only; small fixed cohort is not a strength estimate";
        report["paired_budget_comparison"] = paired;
        report["conclusion"] = "pending_summary_interpretation";
        report["limitations"] = pb::make_tuple(
                "small deterministic first-battle cohort; not a broad strength or A20 claim",
                "first-legal-action setup before the first battle is fixed and may not represent a player route policy",
                "unsupported sampler decisions stop the controller without fallback",
                "search rollouts truncate at 24 public-action decisions with a public HP-fraction heuristic",
                "wall times are local single-process measurements and exclude compilation");

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
        std::cout << "NORMAL_PUBLIC_END_TO_END_STUDY_PASS\n";
        std::cout << "attempts=" << attempts.size()
                  << " paired_supported_completions=" << pairedCompleted << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "NORMAL_PUBLIC_END_TO_END_STUDY_FAIL: " << error.what() << '\n';
        return 1;
    }
}
