#include <pybind11/embed.h>

#include "../bindings/slaythespire.cpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pb = pybind11;

namespace {

constexpr int kGameSeed = 49;
constexpr int kAscension = 20;
constexpr int kParticleCount = 32;
constexpr int kRolloutDecisionLimit = 24;
constexpr int kSearchBudget = 192;
constexpr std::uint64_t kSamplerSeed = 0x6e6f726d70756231ULL;
constexpr std::uint64_t kSearchSeed = 0x7368617265647472ULL;

bool sameRandomState(const Random &lhs, const Random &rhs) {
    return lhs.counter == rhs.counter && lhs.seed0 == rhs.seed0 && lhs.seed1 == rhs.seed1;
}

bool privateFutureDiffers(const StepSimulator &lhs, const StepSimulator &rhs) {
    return !sameRandomState(lhs.gc.cardRandomRng, rhs.gc.cardRandomRng)
            || !sameRandomState(lhs.bc.aiRng, rhs.bc.aiRng)
            || !sameRandomState(lhs.bc.shuffleRng, rhs.bc.shuffleRng)
            || !sameRandomState(lhs.bc.miscRng, rhs.bc.miscRng)
            || !sameRandomState(lhs.bc.monsterHpRng, rhs.bc.monsterHpRng)
            || !sameRandomState(lhs.bc.potionRng, rhs.bc.potionRng);
}

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

#include "study_issue24_public_tactical_heuristic.hpp"

std::string projectionScreen(const pb::dict &projection) {
    const auto identity = projection["screen_identity"].cast<pb::dict>();
    return stringField(identity, "value");
}

pb::list projectionCandidates(const pb::dict &projection) {
    const auto candidates = projection["candidate_actions"].cast<pb::dict>();
    if (stringField(candidates, "availability") != "available") {
        throw std::logic_error("native v3 candidate actions are unavailable");
    }
    return candidates["value"].cast<pb::list>();
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
            throw std::logic_error("sampled particle did not preserve the public battle root");
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
        throw std::logic_error("root visits did not equal the total per-decision budget");
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
    pb::list rootStatistics;
    for (const auto &edge : root.edges) {
        pb::dict row;
        row["action"] = edge.action;
        row["visits"] = edge.visits;
        row["mean_value"] = edge.visits > 0
                ? pb::object(pb::float_(edge.valueSum / edge.visits))
                : pb::object(pb::none());
        rootStatistics.append(row);
    }
    evidence["root_action_statistics"] = rootStatistics;
    evidence["action_choice_source"] = "shared public root visits and values only";
    evidence["tree_key_source"] = "public state and public action/result history only";
    return SearchResult{root.edges[bestIndex].action, evidence};
}

std::size_t publicActionIndex(
        const std::vector<pb::dict> &actions,
        const pb::dict &selected) {
    const auto selectedKey = canonicalJson(selected);
    std::size_t matches = 0;
    std::size_t matchIndex = 0;
    for (std::size_t i = 0; i < actions.size(); ++i) {
        if (canonicalJson(actions[i]) == selectedKey) {
            ++matches;
            matchIndex = i;
        }
    }
    if (matches != 1) {
        throw std::logic_error("selected public action did not map uniquely to root order");
    }
    return matchIndex;
}

pb::dict eligibleDecisionState(const pb::dict &battle, const pb::list &indices) {
    const auto all = battle["ordered_public_legal_actions"].cast<pb::list>();
    pb::list selected;
    std::unordered_set<int> seen;
    for (const auto handle : indices) {
        const int index = pb::reinterpret_borrow<pb::int_>(handle).cast<int>();
        if (index < 0 || index >= all.size() || !seen.insert(index).second) {
            throw std::logic_error("public eligible-action ordinals are invalid");
        }
        selected.append(all[index]);
    }
    if (selected.empty()) {
        throw std::logic_error("public Battle input has no eligible candidates");
    }
    pb::dict result;
    for (const auto item : battle) result[item.first] = item.second;
    result["ordered_public_legal_actions"] = selected;
    return result;
}

pb::dict readInputDocument() {
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    const auto input = buffer.str();
    if (input.empty()) throw std::runtime_error("public policy input was empty");
    return pb::module_::import("json").attr("loads")(input).cast<pb::dict>();
}

pb::dict replayToPublicRoot(
        StepSimulator &simulator,
        const pb::dict &publicContext,
        const pb::dict &expectedProjection,
        const pb::dict &expectedBattle) {
    const auto history = publicContext["history"].cast<pb::list>();
    for (const auto entryHandle : history) {
        const auto entry = pb::reinterpret_borrow<pb::dict>(entryHandle);
        const auto selected = entry["selected_action"].cast<pb::dict>();
        const auto identity = selected["identity"].cast<pb::dict>();
        const auto projection = simulator.publicProjection();
        const auto candidates = projectionCandidates(projection);
        const auto legal = simulator.legalActions();
        if (candidates.size() != legal.size() || candidates.empty()) {
            throw std::logic_error("PUBLIC_REPLAY_MISMATCH: candidate/legal-action count changed");
        }
        std::size_t matches = 0;
        std::size_t selectedIndex = legal.size();
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const auto candidate = pb::reinterpret_borrow<pb::dict>(candidates[i]);
            const auto native = publicProjectionActionSnapshot(legal[i]);
            if (canonicalJson(candidate) != canonicalJson(native)) {
                throw std::logic_error("PUBLIC_REPLAY_MISMATCH: native candidate order changed");
            }
            if (canonicalJson(candidate) == canonicalJson(identity)) {
                ++matches;
                selectedIndex = i;
            }
        }
        if (matches != 1) {
            throw std::logic_error("PUBLIC_REPLAY_MISMATCH: history action is not uniquely legal");
        }
        (void) simulator.step(legal[selectedIndex]);
    }

    const auto projection = simulator.publicProjection();
    const auto battle = simulator.publicBattleState();
    if (canonicalJson(projection) != canonicalJson(expectedProjection)
            || canonicalJson(battle) != canonicalJson(expectedBattle)) {
        throw std::logic_error("PUBLIC_REPLAY_MISMATCH: replayed root differs from executor root");
    }
    if (projectionScreen(projection) != "BATTLE"
            || stringField(battle, "information_regime") != "normal_public"
            || stringField(battle, "information_fidelity") != "supported") {
        throw std::logic_error("NATURAL_SEARCH_INTEGRATION_BLOCKED: root is not supported normal Battle");
    }
    if (containsPrivateField(projection) || containsPrivateField(battle)) {
        throw std::logic_error("PUBLIC_REPLAY_MISMATCH: private field appeared in public root");
    }
    return battle;
}

bool privateCounterFaultChangesPublicTransition(
        const StepSimulator &anchor,
        const pb::dict &rootState) {
    const auto actions = publicActions(rootState);
    auto endTurn = std::find_if(
            actions.begin(), actions.end(), [](const pb::dict &action) {
        return stringField(action, "kind") == "end_turn";
    });
    if (endTurn == actions.end()) {
        throw std::logic_error("public counter fault control has no end-turn action");
    }

    const auto baseSeed = publicFutureParticleSeed(kSamplerSeed, 7);
    const auto anchorCounter = static_cast<std::uint64_t>(anchor.bc.aiRng.counter);
    auto faultyA = anchor.samplePublicConsistentHiddenFutureFromParticleSeed(
            splitMix64(baseSeed ^ anchorCounter));
    if (canonicalJson(faultyA.publicBattleState()) != canonicalJson(rootState)) {
        throw std::logic_error("counter-fault control changed its public root");
    }
    const auto transitionA = faultyA.stepPublicAction(*endTurn);

    for (std::uint64_t delta = 1; delta <= 64; ++delta) {
        auto privateCounterAnchor = anchor;
        privateCounterAnchor.bc.aiRng.counter += static_cast<std::int32_t>(delta);
        const auto privateCounter = static_cast<std::uint64_t>(
                privateCounterAnchor.bc.aiRng.counter);
        auto faultyB = privateCounterAnchor
                .samplePublicConsistentHiddenFutureFromParticleSeed(
                        splitMix64(baseSeed ^ privateCounter));
        if (canonicalJson(faultyB.publicBattleState())
                != canonicalJson(rootState)) {
            throw std::logic_error("counter-fault control changed its public root");
        }
        const auto transitionB = faultyB.stepPublicAction(*endTurn);
        if (canonicalJson(transitionA) != canonicalJson(transitionB)) {
            return true;
        }
    }
    return false;
}

int parseIntArg(int argc, char **argv, const std::string &name) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == name) return std::stoi(argv[i + 1]);
    }
    throw std::invalid_argument("missing argument " + name);
}

std::string parseStringArg(int argc, char **argv, const std::string &name) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == name) return argv[i + 1];
    }
    throw std::invalid_argument("missing argument " + name);
}

}  // namespace

int main(int argc, char **argv) {
    try {
        pb::scoped_interpreter interpreter{};
        const auto mode = parseStringArg(argc, argv, "--mode");
        const int gameSeed = parseIntArg(argc, argv, "--seed");
        const int ascension = parseIntArg(argc, argv, "--ascension");
        const auto input = readInputDocument();
        if (!input.contains("schema_id")
                || input["schema_id"].cast<std::string>() != "issue24-v1-public-policy-input"
                || !input.contains("decision_context")
                || !input.contains("native_public_projection")
                || !input.contains("native_public_battle_observation")) {
            throw std::invalid_argument("invalid Issue 24 public-policy input schema");
        }
        std::cerr << "NATIVE_STAGE input_validated\n";
        const auto decisionContext = input["decision_context"].cast<pb::dict>();
        if (!decisionContext.contains("public_run_context")
                || !decisionContext.contains("eligible_action_indices")) {
            throw std::invalid_argument("Issue 24 decision context is missing public fields");
        }
        const auto publicContext = decisionContext["public_run_context"].cast<pb::dict>();
        const auto expectedProjection = input["native_public_projection"].cast<pb::dict>();
        const auto expectedBattle = input["native_public_battle_observation"].cast<pb::dict>();
        const auto eligible = decisionContext
                ["eligible_action_indices"].cast<pb::list>();

        StepSimulator anchor(CharacterClass::IRONCLAD, gameSeed, ascension);
        std::cerr << "NATIVE_STAGE replay_started\n";
        const auto rootState = replayToPublicRoot(
                anchor, publicContext, expectedProjection, expectedBattle);
        std::cerr << "NATIVE_STAGE public_root_replayed\n";
        const auto allRootActions = publicActions(rootState);
        const auto decisionState = eligibleDecisionState(rootState, eligible);
        const auto eligibleRootActions = publicActions(decisionState);
        const auto history = publicContext["history"].cast<pb::list>();
        std::cerr << "NATIVE_STAGE candidates_checked\n";
        if (eligibleRootActions.size() != allRootActions.size()) {
            throw std::logic_error("ACTION_SPACE_MISMATCH: issue24 requires the full current public legal surface");
        }
        const auto baselineAction = selectPublicHeuristicAction(decisionState);
        std::cerr << "NATIVE_STAGE baseline_selected\n";
        const auto baselineIndex = publicActionIndex(allRootActions, baselineAction);
        StepSimulator baselineExecution = anchor;
        (void) baselineExecution.stepPublicAction(baselineAction);
        std::cerr << "PUBLIC_ROOT_REPLAY_VALIDATED decisions=" << history.size()
                << " candidates=" << allRootActions.size()
                << " baseline_action_accepted=true"
                << " baseline_ordinal=" << baselineIndex
                << " baseline_identity=" << canonicalJson(baselineAction)
                << "\n";

        pb::dict report;
        report["schema_id"] = "issue24-natural-shared-public-search-decision-v1";
        report["seed"] = gameSeed;
        report["ascension"] = ascension;
        report["mode"] = mode;
        report["proposal_law"] =
                "Q(particle | current_public_observation, observed_public_history, sampler_seed, particle_index)";
        report["proposal_semantics"] =
                "reproducible public-consistent proposal; not an exact posterior";
        report["proposal_is_exact_posterior"] = false;
        report["natural_history_decisions_before_root"] = history.size();
        report["root_public_projection_equal_to_executor"] = true;
        report["root_public_battle_state_equal_to_executor"] = true;
        report["native_root_replay_action_count"] = history.size();
        report["eligible_public_candidate_count"] = eligibleRootActions.size();
        report["all_public_candidate_count"] = allRootActions.size();
        report["baseline_public_action"] = baselineAction;
        report["baseline_public_ordinal"] = baselineIndex;
        report["baseline_native_action_accepted"] = true;
        report["baseline_controller_source"] =
                "Issue 17 public-tactical-v1-blocked-lethal-canonical-monster-id; source commit 9a2792e1e02157124b4f90edc91b7ad8765d5d10";

        if (mode == "search") {
            if (history.size() != 4) {
                throw std::logic_error("NATURAL_SEARCH_INTEGRATION_BLOCKED: expected four natural noncombat decisions before first Battle");
            }
            const bool negativeCaseRejected = privateCounterFaultChangesPublicTransition(
                    anchor, rootState);
            if (!negativeCaseRejected) {
                throw std::logic_error(
                        "ANCHOR_INVARIANCE_BLOCKED: operative private-counter fault did not change a public transition");
            }
            std::cerr << "PRIVATE_ANCHOR_COUNTER_FAULT_CHANGED_PUBLIC_TRANSITION=true\n";
            std::cerr << "NATIVE_STAGE negative_control_passed\n";

            StepSimulator alternateAnchor = anchor.samplePublicConsistentHiddenFuture(
                    kSamplerSeed, 0);
            bool hiddenAnchorsDiffer = privateFutureDiffers(anchor, alternateAnchor);
            for (int index = 1; !hiddenAnchorsDiffer && index < kParticleCount; ++index) {
                alternateAnchor = anchor.samplePublicConsistentHiddenFuture(
                        kSamplerSeed, static_cast<std::uint64_t>(index));
                hiddenAnchorsDiffer = privateFutureDiffers(anchor, alternateAnchor);
            }
            if (!hiddenAnchorsDiffer
                    || canonicalJson(alternateAnchor.publicProjection()) != canonicalJson(expectedProjection)
                    || canonicalJson(alternateAnchor.publicBattleState()) != canonicalJson(expectedBattle)) {
                throw std::logic_error("ANCHOR_INVARIANCE_BLOCKED: no different hidden anchor preserved the public root");
            }

            bool sampledFuturesEqual = true;
            for (int index = 0; index < kParticleCount; ++index) {
                auto first = anchor.samplePublicConsistentHiddenFuture(
                        kSamplerSeed, static_cast<std::uint64_t>(index));
                auto second = alternateAnchor.samplePublicConsistentHiddenFuture(
                        kSamplerSeed, static_cast<std::uint64_t>(index));
                if (canonicalJson(first.publicBattleState()) != canonicalJson(second.publicBattleState())
                        || canonicalJson(first.snapshot()) != canonicalJson(second.snapshot())) {
                    sampledFuturesEqual = false;
                    break;
                }
            }
            if (!sampledFuturesEqual) {
                throw std::logic_error("ANCHOR_INVARIANCE_BLOCKED: public-consistent sampler depends on hidden anchor");
            }
            std::cerr << "PUBLIC_SAMPLE_POOL_EQUAL=true particles=" << kParticleCount << "\n";
            std::cerr << "NATIVE_STAGE public_sample_pool_equal\n";

            const auto searchStarted = std::chrono::steady_clock::now();
            const auto search = searchDecision(
                    anchor, rootState, kSearchBudget, kSamplerSeed, kSearchSeed);
            std::cerr << "NATIVE_STAGE first_search_completed\n";
            const auto searchFinished = std::chrono::steady_clock::now();
            const auto auditStarted = std::chrono::steady_clock::now();
            const auto invariantSearch = searchDecision(
                    alternateAnchor, rootState, kSearchBudget, kSamplerSeed, kSearchSeed);
            std::cerr << "NATIVE_STAGE invariance_search_completed\n";
            const auto auditFinished = std::chrono::steady_clock::now();
            const bool selectedActionEqual = canonicalJson(search.selectedAction)
                    == canonicalJson(invariantSearch.selectedAction);
            const bool rootStatisticsEqual = canonicalJson(search.evidence["root_action_statistics"])
                    == canonicalJson(invariantSearch.evidence["root_action_statistics"]);
            std::cerr << "PUBLIC_SEARCH_AUDIT_COMPLETED search_simulations=" << kSearchBudget
                    << " audit_simulations=" << kSearchBudget
                    << " selected_action_equal=" << (selectedActionEqual ? "true" : "false")
                    << " root_statistics_equal=" << (rootStatisticsEqual ? "true" : "false")
                    << "\n";
            if (!selectedActionEqual || !rootStatisticsEqual) {
                throw std::logic_error("ANCHOR_INVARIANCE_BLOCKED: same-public roots changed search action statistics");
            }
            const auto searchIndex = publicActionIndex(allRootActions, search.selectedAction);
            StepSimulator searchExecution = anchor;
            (void) searchExecution.stepPublicAction(search.selectedAction);
            report["selected_public_ordinal"] = searchIndex;
            report["selected_public_action"] = search.selectedAction;
            report["selected_action_mode"] = "shared_public_tree_B192";
            report["shared_public_search"] = search.evidence;
            report["search_wall_seconds"] = std::chrono::duration<double>(
                    searchFinished - searchStarted).count();
            report["anchor_invariance_audit_wall_seconds"] = std::chrono::duration<double>(
                    auditFinished - auditStarted).count();
            report["anchor_invariance_audit_simulations"] = kSearchBudget;
            pb::dict invariance;
            invariance["public_root_equal"] = true;
            invariance["hidden_anchor_futures_differ"] = true;
            invariance["public_sample_pool_equal_for_all_particles"] = true;
            invariance["root_action_statistics_equal"] = true;
            invariance["private_counter_fault_control_passed"] = negativeCaseRejected;
            invariance["proposal_source"] = "fixed_public_consistent_seed";
            invariance["private_snapshot_values_emitted"] = false;
            report["anchor_invariance"] = invariance;
        } else if (mode == "heuristic") {
            report["selected_public_ordinal"] = publicActionIndex(allRootActions, baselineAction);
            report["selected_public_action"] = baselineAction;
            report["selected_action_mode"] = "issue17_public_tactical_heuristic";
        } else {
            throw std::invalid_argument("unsupported native decision mode");
        }
        std::cout << indentedJson(report) << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
