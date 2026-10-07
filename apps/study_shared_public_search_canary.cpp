#include <pybind11/embed.h>

#include "../bindings/slaythespire.cpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pb = pybind11;
using namespace pybind11::literals;

namespace {

constexpr std::uint64_t kSamplerSeedBase = 6006001;
constexpr std::uint64_t kSearchSeedBase = 8061001;
constexpr std::int64_t kSimulationBudget = 24;
constexpr int kMaxDecisionDepth = 12;
constexpr int kParticleCounts[] = {2, 4, 8, 16};
constexpr int kReplicates = 2;
constexpr int kAnchorSeeds[] = {1, 2, 3, 4, 5, 6};

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

bool forbiddenPublicIdentityKey(const std::string &name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lower == "bits" || lower == "unique_id"
            || lower == "particle" || lower == "particle_id"
            || lower == "particle_index" || lower == "hidden_draw_order"
            || lower.find("rng") != std::string::npos;
}

bool containsPrivateIdentityField(pb::handle value) {
    if (pb::isinstance<pb::dict>(value)) {
        const auto dict = pb::reinterpret_borrow<pb::dict>(value);
        for (const auto item : dict) {
            const auto key = pb::str(item.first).cast<std::string>();
            if (forbiddenPublicIdentityKey(key)
                    || containsPrivateIdentityField(item.second)) {
                return true;
            }
        }
    } else if (pb::isinstance<pb::list>(value)
            || pb::isinstance<pb::tuple>(value)) {
        for (const auto item : pb::reinterpret_borrow<pb::sequence>(value)) {
            if (containsPrivateIdentityField(item)) {
                return true;
            }
        }
    }
    return false;
}

std::vector<pb::dict> publicActions(const pb::dict &state) {
    std::vector<pb::dict> result;
    const auto identities = state["ordered_public_legal_actions"].cast<pb::list>();
    result.reserve(static_cast<std::size_t>(identities.size()));
    for (const auto item : identities) {
        result.push_back(pb::reinterpret_borrow<pb::dict>(item));
    }
    return result;
}

std::string historyNodeKey(const std::vector<std::string> &publicHistory) {
    std::string result;
    for (const auto &component : publicHistory) {
        result += std::to_string(component.size());
        result.push_back(':');
        result += component;
    }
    return result;
}

std::string privateDrawOrderFingerprint(const StepSimulator &simulator) {
    // Used only for an in-process assertion that these anchors have genuine
    // hidden uncertainty. This identity is never used for tree keys or output.
    std::string result;
    for (const auto &card : simulator.bc.cards.drawPile) {
        result += std::to_string(card.getUniqueId());
        result.push_back(',');
    }
    return result;
}

struct Anchor {
    int gameSeed = 0;
    std::string identity;
    std::unique_ptr<StepSimulator> simulator;
    pb::dict publicState;
    std::size_t distinctHiddenOrders = 0;
};

Anchor prepareAnchor(const int gameSeed) {
    Anchor anchor;
    anchor.gameSeed = gameSeed;
    anchor.simulator = std::make_unique<StepSimulator>(
            CharacterClass::IRONCLAD, static_cast<std::uint64_t>(gameSeed), 0);

    bool reachedDecision = false;
    for (int step = 0; step < 512; ++step) {
        const auto snapshot = anchor.simulator->snapshot();
        if (stringField(snapshot, "screen_state") == "BATTLE"
                && snapshot["battle_active"].cast<bool>()
                && stringField(snapshot, "battle_input_state") == "PLAYER_NORMAL") {
            reachedDecision = true;
            break;
        }
        const auto actions = anchor.simulator->legalActions();
        if (actions.empty()) {
            break;
        }
        (void) anchor.simulator->step(actions.front());
    }
    if (!reachedDecision) {
        throw std::runtime_error("bounded anchor setup did not reach a normal battle decision");
    }

    anchor.publicState = anchor.simulator->publicBattleState();
    if (stringField(anchor.publicState, "information_fidelity") != "supported") {
        throw std::runtime_error("anchor has unsupported public-information fidelity");
    }
    if (containsPrivateIdentityField(anchor.publicState)) {
        throw std::logic_error("normal-public anchor contains a private identity field");
    }

    anchor.identity = "ironclad-a0-seed-" + std::to_string(gameSeed)
            + "-floor-" + std::to_string(intField(anchor.publicState, "floor_num"))
            + "-" + stringField(anchor.publicState, "encounter_id");

    const auto rootStateKey = canonicalJson(anchor.publicState);
    const auto rootActionKeys = [&]() {
        std::vector<std::string> keys;
        for (const auto &action : publicActions(anchor.publicState)) {
            if (containsPrivateIdentityField(action)) {
                throw std::logic_error("normal-public action contains a private identity field");
            }
            keys.push_back(canonicalJson(action));
        }
        return keys;
    }();
    std::unordered_set<std::string> hiddenOrders;
    for (std::uint64_t particleIndex = 0; particleIndex < 16; ++particleIndex) {
        auto particle = anchor.simulator->samplePublicConsistentHiddenFuture(
                kSamplerSeedBase + static_cast<std::uint64_t>(gameSeed), particleIndex);
        const auto particlePublicState = particle.publicBattleState();
        if (canonicalJson(particlePublicState) != rootStateKey) {
            throw std::logic_error("sampled anchor particle changed public state");
        }
        std::vector<std::string> particleActionKeys;
        for (const auto &action : publicActions(particlePublicState)) {
            particleActionKeys.push_back(canonicalJson(action));
        }
        if (particleActionKeys != rootActionKeys) {
            throw std::logic_error("sampled anchor particle changed ordered public legal actions");
        }
        hiddenOrders.insert(privateDrawOrderFingerprint(particle));
    }
    if (hiddenOrders.size() < 2) {
        throw std::runtime_error("anchor did not produce distinct hidden draw-order samples");
    }
    anchor.distinctHiddenOrders = hiddenOrders.size();
    return anchor;
}

struct Edge {
    pb::dict publicAction;
    std::string actionKey;
    std::uint64_t visits = 0;
    double valueSum = 0.0;
};

struct Node {
    std::string publicStateKey;
    std::vector<std::string> orderedActionKeys;
    std::vector<Edge> edges;
    std::uint64_t visits = 0;
};

struct Tree {
    std::unordered_map<std::string, Node> nodes;

    Node &getOrCreate(const std::string &nodeKey, const pb::dict &publicState) {
        if (containsPrivateIdentityField(publicState)) {
            throw std::logic_error("tree node input contains a private identity field");
        }
        const auto stateKey = canonicalJson(publicState);
        const auto actions = publicActions(publicState);
        auto [position, inserted] = nodes.try_emplace(nodeKey);
        auto &node = position->second;
        if (inserted) {
            node.publicStateKey = stateKey;
            std::unordered_set<std::string> seen;
            for (const auto &action : actions) {
                if (containsPrivateIdentityField(action)) {
                    throw std::logic_error("tree action contains a private identity field");
                }
                const auto actionKey = canonicalJson(action);
                if (!seen.insert(actionKey).second) {
                    throw std::logic_error("public action identity is not unique at a shared node");
                }
                node.orderedActionKeys.push_back(actionKey);
                node.edges.push_back(Edge{action, actionKey, 0, 0.0});
            }
        } else if (node.publicStateKey != stateKey
                || node.orderedActionKeys.size() != actions.size()) {
            throw std::logic_error("one public history key resolved to inconsistent public state/actions");
        } else {
            for (std::size_t idx = 0; idx < actions.size(); ++idx) {
                if (node.orderedActionKeys[idx] != canonicalJson(actions[idx])) {
                    throw std::logic_error("shared node action ordering changed for the same public history");
                }
            }
        }
        return node;
    }
};

std::uint64_t publicTieToken(
        const std::string &nodeKey,
        const std::uint64_t nodeVisits,
        const std::uint64_t searchSeed) {
    std::uint64_t publicKeyHash = 14695981039346656037ULL;
    for (const unsigned char byte : nodeKey) {
        publicKeyHash ^= byte;
        publicKeyHash *= 1099511628211ULL;
    }
    return splitMix64(searchSeed ^ publicKeyHash ^ splitMix64(nodeVisits));
}

std::size_t selectSharedEdge(
        const Node &node,
        const std::string &nodeKey,
        const std::uint64_t searchSeed) {
    if (node.edges.empty()) {
        throw std::logic_error("nonterminal public node has no legal actions");
    }
    std::vector<std::size_t> candidates;
    for (std::size_t idx = 0; idx < node.edges.size(); ++idx) {
        if (node.edges[idx].visits == 0) {
            candidates.push_back(idx);
        }
    }
    if (!candidates.empty()) {
        return candidates[publicTieToken(nodeKey, node.visits, searchSeed)
                % candidates.size()];
    }

    double bestScore = -std::numeric_limits<double>::infinity();
    for (std::size_t idx = 0; idx < node.edges.size(); ++idx) {
        const auto &edge = node.edges[idx];
        const double mean = edge.valueSum / static_cast<double>(edge.visits);
        const double exploration = std::sqrt(
                2.0 * std::log(static_cast<double>(std::max<std::uint64_t>(1, node.visits)))
                / static_cast<double>(edge.visits));
        const double score = mean + exploration;
        if (score > bestScore + 1e-12) {
            bestScore = score;
            candidates.assign(1, idx);
        } else if (std::abs(score - bestScore) <= 1e-12) {
            candidates.push_back(idx);
        }
    }
    return candidates[publicTieToken(nodeKey, node.visits, searchSeed)
            % candidates.size()];
}

double truncatedPublicEvaluation(const pb::dict &publicState) {
    const auto player = publicState["player"].cast<pb::dict>();
    const int hp = intField(player, "current_hp");
    const int maxHp = std::max(1, intField(player, "max_hp"));
    const auto monsters = publicState["monsters"].cast<pb::list>();
    double enemyFraction = 0.0;
    for (const auto monsterHandle : monsters) {
        const auto monster = pb::reinterpret_borrow<pb::dict>(monsterHandle);
        const int monsterMaxHp = std::max(1, intField(monster, "max_hp"));
        enemyFraction += static_cast<double>(intField(monster, "current_hp"))
                / static_cast<double>(monsterMaxHp);
    }
    return std::clamp(static_cast<double>(hp) / static_cast<double>(maxHp)
                    - 0.5 * enemyFraction,
            -1.0, 1.0);
}

struct PathEdge {
    std::string nodeKey;
    std::size_t edgeIndex = 0;
};

struct RunResult {
    pb::dict evidence;
    std::string selectedActionKey;
    double selectedActionValue = 0.0;
};

RunResult runCanary(
        const Anchor &anchor,
        const std::string &codeCommit,
        const int particleCount,
        const int replicate) {
    const auto samplerSeed = kSamplerSeedBase + static_cast<std::uint64_t>(anchor.gameSeed);
    const auto searchSeed = kSearchSeedBase + static_cast<std::uint64_t>(replicate);
    Tree tree;
    std::unordered_set<int> particlesUsed;
    std::unordered_set<std::string> rootNodeKeys;
    std::uint64_t nativeActionSteps = 0;

    const auto start = std::chrono::steady_clock::now();
    for (std::int64_t simulation = 0; simulation < kSimulationBudget; ++simulation) {
        const auto particleIndex = static_cast<std::uint64_t>(simulation % particleCount);
        particlesUsed.insert(static_cast<int>(particleIndex));
        auto particle = anchor.simulator->samplePublicConsistentHiddenFuture(
                samplerSeed, particleIndex);
        auto currentState = particle.publicBattleState();
        if (canonicalJson(currentState) != canonicalJson(anchor.publicState)) {
            throw std::logic_error("simulation particle did not start from the common public root");
        }

        std::vector<std::string> publicHistory{canonicalJson(currentState)};
        std::vector<PathEdge> path;
        std::string rootKey;
        double value = 0.0;
        bool terminal = false;
        for (int depth = 0; depth < kMaxDecisionDepth; ++depth) {
            const auto nodeKey = historyNodeKey(publicHistory);
            if (depth == 0) {
                rootKey = nodeKey;
                rootNodeKeys.insert(nodeKey);
            }
            auto &node = tree.getOrCreate(nodeKey, currentState);
            const auto edgeIndex = selectSharedEdge(node, nodeKey, searchSeed);
            const auto actionKey = node.edges[edgeIndex].actionKey;
            const auto action = node.edges[edgeIndex].publicAction;
            path.push_back(PathEdge{nodeKey, edgeIndex});

            (void) particle.stepPublicAction(action);
            ++nativeActionSteps;
            publicHistory.push_back(actionKey);
            if (!particle.battleActive) {
                value = particle.bc.outcome == Outcome::PLAYER_VICTORY ? 1.0 : -1.0;
                terminal = true;
                break;
            }

            currentState = particle.publicBattleState();
            if (containsPrivateIdentityField(currentState)) {
                throw std::logic_error("transition exposed a private field to the public tree");
            }
            publicHistory.push_back(canonicalJson(currentState));
        }
        if (!terminal) {
            value = truncatedPublicEvaluation(currentState);
        }

        for (const auto &entry : path) {
            auto &node = tree.nodes.at(entry.nodeKey);
            ++node.visits;
            auto &edge = node.edges.at(entry.edgeIndex);
            ++edge.visits;
            edge.valueSum += value;
        }
    }
    const auto finish = std::chrono::steady_clock::now();

    if (rootNodeKeys.size() != 1) {
        throw std::logic_error("particle source partitioned the public root into multiple nodes");
    }
    const auto &root = tree.nodes.at(*rootNodeKeys.begin());
    if (root.visits != static_cast<std::uint64_t>(kSimulationBudget)) {
        throw std::logic_error("root shared visit count does not equal the fixed simulation budget");
    }
    std::uint64_t rootEdgeVisits = 0;
    std::size_t visitedRootActions = 0;
    std::size_t selectedIndex = 0;
    for (std::size_t idx = 0; idx < root.edges.size(); ++idx) {
        rootEdgeVisits += root.edges[idx].visits;
        if (root.edges[idx].visits > 0) {
            ++visitedRootActions;
        }
        if (root.edges[idx].visits > root.edges[selectedIndex].visits
                || (root.edges[idx].visits == root.edges[selectedIndex].visits
                    && root.edges[idx].visits > 0
                    && root.edges[idx].valueSum / root.edges[idx].visits
                            > root.edges[selectedIndex].valueSum
                                    / std::max<std::uint64_t>(1, root.edges[selectedIndex].visits))) {
            selectedIndex = idx;
        }
    }
    if (rootEdgeVisits != static_cast<std::uint64_t>(kSimulationBudget)) {
        throw std::logic_error("root public action visits do not sum to the fixed budget");
    }
    if (root.edges.size() > static_cast<std::size_t>(kSimulationBudget)
            || visitedRootActions != root.edges.size()) {
        throw std::logic_error("shared root exploration did not visit every public legal action");
    }
    if (static_cast<int>(particlesUsed.size()) != particleCount) {
        throw std::logic_error("not every configured particle was used at the fixed budget");
    }

    pb::dict selectedAction = root.edges[selectedIndex].publicAction;
    auto actionProbe = anchor.simulator->samplePublicConsistentHiddenFuture(samplerSeed, 0);
    const auto probeResult = actionProbe.stepPublicAction(selectedAction);
    if (stringField(probeResult, "screen_state").empty()) {
        throw std::logic_error("selected root action did not map through public action execution");
    }

    pb::dict result;
    result["code_commit"] = codeCommit;
    result["simulator_base"] = "origin/spire/main@f6279292f685026f05d57cda9140650185d76043";
    result["information_regime"] = "normal_public";
    result["anchor_id"] = anchor.identity;
    result["game_seed"] = anchor.gameSeed;
    result["encounter_id"] = stringField(anchor.publicState, "encounter_id");
    result["floor_num"] = intField(anchor.publicState, "floor_num");
    result["sampler_seed"] = samplerSeed;
    result["search_seed"] = searchSeed;
    result["particle_count"] = particleCount;
    result["total_simulation_budget"] = kSimulationBudget;
    result["decision_depth_cap"] = kMaxDecisionDepth;
    result["selected_public_action"] = selectedAction;
    result["selected_action_shared_visits"] = root.edges[selectedIndex].visits;
        result["selected_action_shared_value"] = root.edges[selectedIndex].valueSum
                / static_cast<double>(root.edges[selectedIndex].visits);
        result["work_counter_definition"] = "native_action_steps counts calls to step_public_action during searched trajectories; simulator-internal queue steps are not separately instrumented";
    pb::list rootRows;
    for (const auto &edge : root.edges) {
        pb::dict row;
        row["public_action"] = edge.publicAction;
        row["shared_visits"] = edge.visits;
        row["shared_value_sum"] = edge.valueSum;
        row["shared_mean_value"] = edge.visits > 0
                ? pb::cast(edge.valueSum / static_cast<double>(edge.visits))
                : pb::none();
        rootRows.append(row);
    }
    result["root_shared_actions"] = rootRows;
    result["work"] = pb::dict(
            "total_simulations"_a = kSimulationBudget,
            "native_action_steps"_a = nativeActionSteps,
            "shared_nodes"_a = tree.nodes.size(),
            "shared_edges"_a = [&tree]() {
                std::size_t count = 0;
                for (const auto &entry : tree.nodes) {
                    count += entry.second.edges.size();
                }
                return count;
            }(),
            "distinct_particles_used"_a = particlesUsed.size(),
            "wall_time_seconds"_a = std::chrono::duration<double>(finish - start).count());
    result["semantic_checks"] = pb::dict(
            "normal_public_information_regime"_a = true,
            "root_public_state_and_ordered_actions_shared"_a = true,
            "particle_identity_excluded_from_action_selection_and_node_key"_a = true,
            "single_shared_root_node_across_particles"_a = true,
            "all_root_actions_received_shared_visits"_a = true,
            "selected_action_executed_via_public_action_surface"_a = true,
            "budget_fixed_independent_of_particle_count"_a = true);

    return RunResult{
            result,
            canonicalJson(selectedAction),
            root.edges[selectedIndex].valueSum
                    / static_cast<double>(root.edges[selectedIndex].visits)};
}

pb::dict verifySharedNodeStatistics(const Anchor &anchor) {
    const auto samplerSeed = kSamplerSeedBase + static_cast<std::uint64_t>(anchor.gameSeed);
    const auto publicStateKey = canonicalJson(anchor.publicState);
    const auto rootActionKeys = [&]() {
        std::vector<std::string> keys;
        for (const auto &action : publicActions(anchor.publicState)) {
            keys.push_back(canonicalJson(action));
        }
        return keys;
    }();

    Tree tree;
    std::unordered_set<std::string> rootKeys;
    for (std::uint64_t particleIndex = 0; particleIndex < 16; ++particleIndex) {
        auto particle = anchor.simulator->samplePublicConsistentHiddenFuture(
                samplerSeed, particleIndex);
        const auto particleState = particle.publicBattleState();
        if (canonicalJson(particleState) != publicStateKey) {
            throw std::logic_error("preflight particles do not share the root public state");
        }
        std::vector<std::string> actions;
        for (const auto &action : publicActions(particleState)) {
            actions.push_back(canonicalJson(action));
        }
        if (actions != rootActionKeys) {
            throw std::logic_error("preflight particles do not share ordered public actions");
        }

        const std::vector<std::string> history{publicStateKey};
        const auto key = historyNodeKey(history);
        rootKeys.insert(key);
        auto &node = tree.getOrCreate(key, particleState);
        ++node.visits;
        ++node.edges.front().visits;
    }
    if (rootKeys.size() != 1 || tree.nodes.size() != 1) {
        throw std::logic_error("public root node identity depends on particle source");
    }
    const auto &node = tree.nodes.begin()->second;
    if (node.visits != 16 || node.edges.front().visits != 16) {
        throw std::logic_error("simulations from different particles did not update common root statistics");
    }

    pb::dict result;
    result["root_public_state_identical_for_16_particles"] = true;
    result["ordered_public_actions_identical_for_16_particles"] = true;
    result["single_shared_root_node_count"] = tree.nodes.size();
    result["shared_root_visits_from_particle_sources"] = node.visits;
    result["same_root_edge_visits_from_particle_sources"] = node.edges.front().visits;
    result["tree_key_source"] = "public_battle_state plus ordered public action/result history";
    return result;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: study-shared-public-search-canary <code-commit> <output-json>\n";
        return 2;
    }

    try {
        pb::scoped_interpreter interpreter{};
        const std::string codeCommit = argv[1];
        const std::string outputPath = argv[2];
        pb::dict report;
        report["schema_id"] = "shared-public-search-canary-v1";
        report["code_commit"] = codeCommit;
        report["simulator_base"] = "origin/spire/main@f6279292f685026f05d57cda9140650185d76043";
        report["information_regime"] = "normal_public";
        report["sampler_interpretation"] = "reproducible proposal distribution; not an exact posterior";
        report["search_policy"] = "one UCT tree keyed by public state and public action/result history; tie selection uses only search seed, public node key, and shared visits";
        report["truncated_leaf_evaluation"] = "clamp(player_hp/max_hp - 0.5 * sum(monster_hp/max_hp), -1, 1); terminal win/loss is +1/-1";
        report["work_counter_definition"] = "native_action_steps counts calls to step_public_action during searched trajectories; simulator-internal queue steps are not separately instrumented";
        report["stability_criterion"] = "per anchor, all 8 particle-count/search-seed settings select the same action and the selected action value range is <= 0.25";

        pb::list anchorsEvidence;
        pb::list skippedAnchors;
        std::vector<Anchor> anchors;
        for (const int seed : kAnchorSeeds) {
            if (anchors.size() >= 2) {
                break;
            }
            try {
                auto anchor = prepareAnchor(seed);
                pb::dict row;
                row["anchor_id"] = anchor.identity;
                row["game_seed"] = anchor.gameSeed;
                row["encounter_id"] = stringField(anchor.publicState, "encounter_id");
                row["floor_num"] = intField(anchor.publicState, "floor_num");
                row["information_fidelity"] = stringField(anchor.publicState, "information_fidelity");
                row["distinct_hidden_draw_orders_in_16_samples"] = anchor.distinctHiddenOrders;
                row["normal_public_legal_action_count"] = publicActions(anchor.publicState).size();
                anchorsEvidence.append(row);
                anchors.push_back(std::move(anchor));
            } catch (const std::exception &error) {
                pb::dict skipped;
                skipped["game_seed"] = seed;
                skipped["reason"] = error.what();
                skippedAnchors.append(skipped);
            }
        }
        if (anchors.size() < 2) {
            throw std::runtime_error("fewer than two supported deterministic anchors with genuine hidden uncertainty");
        }
        report["anchors"] = anchorsEvidence;
        report["skipped_anchors"] = skippedAnchors;

        pb::list preflight;
        for (const auto &anchor : anchors) {
            pb::dict row = verifySharedNodeStatistics(anchor);
            row["anchor_id"] = anchor.identity;
            preflight.append(row);
        }
        report["semantic_preflight"] = preflight;

        pb::list runsEvidence;
        std::vector<std::vector<RunResult>> byAnchor;
        for (const auto &anchor : anchors) {
            std::vector<RunResult> anchorRuns;
            for (int particleCount : kParticleCounts) {
                for (int replicate = 0; replicate < kReplicates; ++replicate) {
                    auto run = runCanary(anchor, codeCommit, particleCount, replicate);
                    runsEvidence.append(run.evidence);
                    anchorRuns.push_back(std::move(run));
                }
            }
            byAnchor.push_back(std::move(anchorRuns));
        }
        report["runs"] = runsEvidence;

        bool stable = true;
        pb::list stabilityEvidence;
        for (std::size_t anchorIdx = 0; anchorIdx < anchors.size(); ++anchorIdx) {
            const auto &runs = byAnchor[anchorIdx];
            std::unordered_map<std::string, int> actionCounts;
            double minSelectedValue = std::numeric_limits<double>::infinity();
            double maxSelectedValue = -std::numeric_limits<double>::infinity();
            for (const auto &run : runs) {
                ++actionCounts[run.selectedActionKey];
                minSelectedValue = std::min(minSelectedValue, run.selectedActionValue);
                maxSelectedValue = std::max(maxSelectedValue, run.selectedActionValue);
            }
            int maxAgreement = 0;
            for (const auto &entry : actionCounts) {
                maxAgreement = std::max(maxAgreement, entry.second);
            }
            const double agreement = static_cast<double>(maxAgreement)
                    / static_cast<double>(runs.size());
            const double valueRange = maxSelectedValue - minSelectedValue;
            const bool anchorStable = maxAgreement == static_cast<int>(runs.size())
                    && valueRange <= 0.25;
            stable = stable && anchorStable;
            pb::dict row;
            row["anchor_id"] = anchors[anchorIdx].identity;
            row["settings"] = runs.size();
            row["selected_action_agreement_fraction"] = agreement;
            row["selected_action_value_range"] = valueRange;
            row["stable_under_declared_criterion"] = anchorStable;
            stabilityEvidence.append(row);
        }
        report["stability"] = stabilityEvidence;
        report["conclusion"] = stable ? "VIABLE_CANARY" : "UNSTABLE_CANARY";
        report["interpretation_limit"] = "bounded feasibility canary only; no strength, posterior, broad A20, or online-latency claim";
        report["proposed_code_disposition"] = "study-only; do not promote to spire/main from this canary";

        std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("could not open output evidence path: " + outputPath);
        }
        output << indentedJson(report) << '\n';
        output.close();
        if (!output) {
            throw std::runtime_error("failed writing output evidence path: " + outputPath);
        }
        std::cout << "SHARED_PUBLIC_SEARCH_CANARY_PASS\n";
        std::cout << "conclusion=" << (stable ? "VIABLE_CANARY" : "UNSTABLE_CANARY") << '\n';
        std::cout << "anchors=" << anchors.size() << " runs=" << runsEvidence.size() << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "SHARED_PUBLIC_SEARCH_CANARY_FAIL: " << error.what() << '\n';
        return 1;
    }
}
