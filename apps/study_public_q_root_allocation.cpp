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
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pb = pybind11;
using namespace pybind11::literals;

namespace {

constexpr std::uint64_t kSamplerSeedBase = 7007001;
constexpr std::uint64_t kSearchSeedBase = 9071001;
constexpr int kParticleCount = 32;
constexpr int kReplicates = 6;
constexpr int kBudgets[] = {384, 1536};
constexpr int kMaxDecisionDepth = 64;
constexpr int kAnchorSeeds[] = {1, 2, 3, 4, 5, 6};

enum class RootMode {
    Adaptive,
    Stratified,
};

const char *rootModeName(const RootMode mode) {
    return mode == RootMode::Adaptive ? "adaptive_root" : "stratified_root";
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

void writeExecutionProgress(
        const std::string &progressPath,
        const std::string &status,
        const std::size_t completedReplicates,
        const std::string &activeMode,
        const std::string &activeAnchor,
        const int activeReplicate,
        const int lastCompletedBudget) {
    pb::dict progress;
    progress["status"] = status;
    progress["completed_mode_anchor_replicates"] = completedReplicates;
    progress["active_mode"] = activeMode;
    progress["active_anchor"] = activeAnchor;
    progress["active_replicate"] = activeReplicate;
    progress["last_completed_budget"] = lastCompletedBudget;
    const std::string temporaryPath = progressPath + ".tmp";
    {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("could not open execution progress file");
        }
        output << indentedJson(progress) << '\n';
        if (!output.good()) {
            throw std::runtime_error("failed while writing execution progress file");
        }
    }
    if (std::rename(temporaryPath.c_str(), progressPath.c_str()) != 0) {
        throw std::runtime_error("could not atomically update execution progress file");
    }
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
    // This private value is used only in-process to establish hidden variation.
    // It is never used in a public node key, action choice, or retained output.
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
    std::vector<std::string> rootActionKeys;
    for (const auto &action : publicActions(anchor.publicState)) {
        if (containsPrivateIdentityField(action)) {
            throw std::logic_error("normal-public action contains a private identity field");
        }
        rootActionKeys.push_back(canonicalJson(action));
    }

    std::unordered_set<std::string> hiddenOrders;
    for (std::uint64_t particleIndex = 0; particleIndex < kParticleCount; ++particleIndex) {
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

struct ActionEstimate {
    pb::dict action;
    std::string key;
    std::uint64_t visits = 0;
    double sum = 0.0;
    double mean = 0.0;
    int rank = 0;
};

struct CheckpointRun {
    int budget = 0;
    std::vector<ActionEstimate> actions;
    std::string topActionKey;
    double topTwoGap = 0.0;
    std::uint64_t nativeActionSteps = 0;
    std::size_t sharedNodes = 0;
    std::size_t sharedEdges = 0;
    std::uint64_t terminalSimulations = 0;
    std::uint64_t cappedSimulations = 0;
    std::uint64_t decisionDepthSum = 0;
    double cumulativeWallSeconds = 0.0;
    double intervalWallSeconds = 0.0;
    std::vector<std::uint64_t> particleExposure;
    std::vector<std::vector<std::uint64_t>> rootActionParticleExposure;
};

struct ReplicateRun {
    int anchorSeed = 0;
    int replicate = 0;
    RootMode rootMode = RootMode::Adaptive;
    std::uint64_t samplerSeed = 0;
    std::uint64_t searchSeed = 0;
    std::vector<CheckpointRun> checkpoints;
};

std::vector<ActionEstimate> rootActionEstimates(const Node &root) {
    std::vector<ActionEstimate> result;
    result.reserve(root.edges.size());
    for (const auto &edge : root.edges) {
        ActionEstimate row;
        row.action = edge.publicAction;
        row.key = edge.actionKey;
        row.visits = edge.visits;
        row.sum = edge.valueSum;
        if (edge.visits > 0) {
            row.mean = edge.valueSum / static_cast<double>(edge.visits);
        }
        result.push_back(std::move(row));
    }
    std::vector<std::size_t> order;
    for (std::size_t idx = 0; idx < result.size(); ++idx) {
        if (result[idx].visits > 0) {
            order.push_back(idx);
        }
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        if (result[left].mean != result[right].mean) {
            return result[left].mean > result[right].mean;
        }
        return left < right;
    });
    for (std::size_t rank = 0; rank < order.size(); ++rank) {
        result[order[rank]].rank = static_cast<int>(rank + 1);
    }
    return result;
}

CheckpointRun makeCheckpoint(
        const int budget,
        const Tree &tree,
        const std::string &rootKey,
        const std::uint64_t nativeActionSteps,
        const std::uint64_t terminalSimulations,
        const std::uint64_t cappedSimulations,
        const std::uint64_t decisionDepthSum,
        const std::vector<std::uint64_t> &particleExposure,
        const std::vector<std::vector<std::uint64_t>> &rootActionParticleExposure,
        const std::chrono::steady_clock::time_point &start,
        const double previousWallSeconds) {
    const auto &root = tree.nodes.at(rootKey);
    if (std::accumulate(particleExposure.begin(), particleExposure.end(), std::uint64_t{0})
            != static_cast<std::uint64_t>(budget)) {
        throw std::logic_error("total particle exposure does not match the simulation budget");
    }
    if (root.visits != static_cast<std::uint64_t>(budget)) {
        throw std::logic_error("root shared visits do not equal the checkpoint budget");
    }
    std::uint64_t totalVisits = 0;
    for (const auto &edge : root.edges) {
        totalVisits += edge.visits;
    }
    if (totalVisits != static_cast<std::uint64_t>(budget)) {
        throw std::logic_error("root public action visits do not sum to checkpoint budget");
    }
    if (rootActionParticleExposure.size() != root.edges.size()) {
        throw std::logic_error("root action particle exposure does not match legal actions");
    }
    for (std::size_t actionIndex = 0; actionIndex < root.edges.size(); ++actionIndex) {
        const auto &exposure = rootActionParticleExposure[actionIndex];
        const auto [minimumExposure, maximumExposure] = std::minmax_element(
                exposure.begin(), exposure.end());
        const auto total = std::accumulate(exposure.begin(), exposure.end(), std::uint64_t{0});
        if (exposure.size() != kParticleCount || minimumExposure == exposure.end()
                || total != root.edges[actionIndex].visits
                || *maximumExposure - *minimumExposure > 1) {
            throw std::logic_error("particle exposure is not balanced within a root action");
        }
    }

    CheckpointRun checkpoint;
    checkpoint.budget = budget;
    checkpoint.actions = rootActionEstimates(root);
    checkpoint.nativeActionSteps = nativeActionSteps;
    checkpoint.sharedNodes = tree.nodes.size();
    for (const auto &entry : tree.nodes) {
        checkpoint.sharedEdges += entry.second.edges.size();
    }
    checkpoint.terminalSimulations = terminalSimulations;
    checkpoint.cappedSimulations = cappedSimulations;
    checkpoint.decisionDepthSum = decisionDepthSum;
    checkpoint.particleExposure = particleExposure;
    checkpoint.rootActionParticleExposure = rootActionParticleExposure;
    const auto now = std::chrono::steady_clock::now();
    checkpoint.cumulativeWallSeconds = std::chrono::duration<double>(now - start).count();
    checkpoint.intervalWallSeconds = checkpoint.cumulativeWallSeconds - previousWallSeconds;

    std::vector<std::size_t> visited;
    for (std::size_t idx = 0; idx < checkpoint.actions.size(); ++idx) {
        if (checkpoint.actions[idx].visits > 0) {
            visited.push_back(idx);
        }
    }
    if (visited.empty()) {
        throw std::logic_error("search did not visit a root action");
    }
    std::stable_sort(visited.begin(), visited.end(), [&](std::size_t left, std::size_t right) {
        if (checkpoint.actions[left].mean != checkpoint.actions[right].mean) {
            return checkpoint.actions[left].mean > checkpoint.actions[right].mean;
        }
        return left < right;
    });
    checkpoint.topActionKey = checkpoint.actions[visited.front()].key;
    if (visited.size() > 1) {
        checkpoint.topTwoGap = checkpoint.actions[visited[0]].mean
                - checkpoint.actions[visited[1]].mean;
    }
    return checkpoint;
}

ReplicateRun runReplicate(const Anchor &anchor, const int replicate, const RootMode rootMode) {
    ReplicateRun run;
    run.anchorSeed = anchor.gameSeed;
    run.replicate = replicate;
    run.rootMode = rootMode;
    run.samplerSeed = kSamplerSeedBase
            + static_cast<std::uint64_t>(anchor.gameSeed) * 100000ULL
            + static_cast<std::uint64_t>(replicate) * 1009ULL;
    run.searchSeed = kSearchSeedBase
            + static_cast<std::uint64_t>(anchor.gameSeed) * 100000ULL
            + static_cast<std::uint64_t>(replicate) * 1009ULL;

    Tree tree;
    const std::vector<std::string> rootHistory{canonicalJson(anchor.publicState)};
    const auto rootKey = historyNodeKey(rootHistory);
    (void) tree.getOrCreate(rootKey, anchor.publicState);
    std::vector<std::vector<std::uint64_t>> rootActionParticleExposure(
            tree.nodes.at(rootKey).edges.size(),
            std::vector<std::uint64_t>(kParticleCount, 0));
    std::vector<std::uint64_t> rootActionExposureCounts(
            tree.nodes.at(rootKey).edges.size(), 0);
    std::unordered_set<std::string> observedRootKeys;
    std::vector<std::uint64_t> particleExposure(kParticleCount, 0);
    std::uint64_t nativeActionSteps = 0;
    std::uint64_t terminalSimulations = 0;
    std::uint64_t cappedSimulations = 0;
    std::uint64_t decisionDepthSum = 0;
    const auto start = std::chrono::steady_clock::now();
    double previousWallSeconds = 0.0;
    int nextBudgetIndex = 0;

    for (int simulation = 0; simulation < kBudgets[1]; ++simulation) {
        // Adaptive mode uses ordinary root UCT. Stratified mode forces an
        // ordered cyclic schedule at the root for evaluation only. Both modes
        // use the same public-tree UCT selection for every continuation step.
        const auto &rootNode = tree.nodes.at(rootKey);
        const auto rootEdgeIndex = rootMode == RootMode::Adaptive
                ? selectSharedEdge(rootNode, rootKey, run.searchSeed)
                : static_cast<std::size_t>(simulation) % rootNode.edges.size();
        const auto rootAction = rootNode.edges[rootEdgeIndex].publicAction;
        const auto rootActionKey = rootNode.edges[rootEdgeIndex].actionKey;
        const auto actionExposure = rootActionExposureCounts[rootEdgeIndex];
        const auto scheduleOffset = splitMix64(run.samplerSeed
                ^ static_cast<std::uint64_t>(rootEdgeIndex)) % kParticleCount;
        const auto particleIndex = (actionExposure * 13ULL + scheduleOffset) % kParticleCount;
        ++rootActionExposureCounts[rootEdgeIndex];
        ++rootActionParticleExposure[rootEdgeIndex][particleIndex];
        ++particleExposure[particleIndex];
        auto particle = anchor.simulator->samplePublicConsistentHiddenFuture(
                run.samplerSeed, particleIndex);
        auto currentState = particle.publicBattleState();
        if (canonicalJson(currentState) != canonicalJson(anchor.publicState)) {
            throw std::logic_error("simulation particle did not start at the common public root");
        }

        std::vector<std::string> publicHistory{canonicalJson(currentState)};
        std::vector<PathEdge> path{{rootKey, rootEdgeIndex}};
        bool terminal = false;
        int decisions = 0;
        double value = 0.0;
        for (; decisions < kMaxDecisionDepth; ++decisions) {
            const auto nodeKey = historyNodeKey(publicHistory);
            if (decisions == 0) {
                observedRootKeys.insert(nodeKey);
            }
            std::size_t edgeIndex = rootEdgeIndex;
            pb::dict action;
            std::string actionKey;
            if (decisions == 0) {
                action = rootAction;
                actionKey = rootActionKey;
            } else {
                auto &node = tree.getOrCreate(nodeKey, currentState);
                edgeIndex = selectSharedEdge(node, nodeKey, run.searchSeed);
                action = node.edges[edgeIndex].publicAction;
                actionKey = node.edges[edgeIndex].actionKey;
                path.push_back(PathEdge{nodeKey, edgeIndex});
            }

            (void) particle.stepPublicAction(action);
            ++nativeActionSteps;
            publicHistory.push_back(actionKey);
            if (!particle.battleActive) {
                value = particle.bc.outcome == Outcome::PLAYER_VICTORY ? 1.0 : -1.0;
                terminal = true;
                ++decisions;
                break;
            }

            currentState = particle.publicBattleState();
            if (containsPrivateIdentityField(currentState)) {
                throw std::logic_error("transition exposed a private field to public tree input");
            }
            publicHistory.push_back(canonicalJson(currentState));
        }
        decisionDepthSum += static_cast<std::uint64_t>(decisions);
        if (terminal) {
            ++terminalSimulations;
        } else {
            ++cappedSimulations;
            value = truncatedPublicEvaluation(currentState);
        }

        for (const auto &entry : path) {
            auto &node = tree.nodes.at(entry.nodeKey);
            ++node.visits;
            auto &edge = node.edges.at(entry.edgeIndex);
            ++edge.visits;
            edge.valueSum += value;
        }

        const int completed = simulation + 1;
        if (nextBudgetIndex < 2 && completed == kBudgets[nextBudgetIndex]) {
            if (observedRootKeys.size() != 1 || *observedRootKeys.begin() != rootKey) {
                throw std::logic_error("particle source partitioned the root public node");
            }
            auto checkpoint = makeCheckpoint(
                    completed, tree, rootKey, nativeActionSteps,
                    terminalSimulations, cappedSimulations, decisionDepthSum,
                    particleExposure, rootActionParticleExposure,
                    start, previousWallSeconds);
            previousWallSeconds = checkpoint.cumulativeWallSeconds;
            run.checkpoints.push_back(std::move(checkpoint));
            ++nextBudgetIndex;
        }
    }

    if (run.checkpoints.size() != 2) {
        throw std::logic_error("one or more cumulative budget checkpoints were not captured");
    }
    // Verify the highest-mean public root action maps back through the native
    // public-action execution surface. This is not used to label the action.
    const auto &last = run.checkpoints.back();
    const auto best = std::find_if(last.actions.begin(), last.actions.end(), [&](const auto &row) {
        return row.key == last.topActionKey;
    });
    if (best == last.actions.end()) {
        throw std::logic_error("reported best action is missing from root action estimates");
    }
    auto actionProbe = anchor.simulator->samplePublicConsistentHiddenFuture(run.samplerSeed, 0);
    const auto probeResult = actionProbe.stepPublicAction(best->action);
    if (stringField(probeResult, "screen_state").empty()) {
        throw std::logic_error("root action did not map through public action execution");
    }
    return run;
}

double sampleSd(const std::vector<double> &values) {
    if (values.size() < 2) {
        return 0.0;
    }
    const double mean = std::accumulate(values.begin(), values.end(), 0.0)
            / static_cast<double>(values.size());
    double squared = 0.0;
    for (const double value : values) {
        squared += (value - mean) * (value - mean);
    }
    return std::sqrt(squared / static_cast<double>(values.size() - 1));
}

pb::dict actionRunJson(const ActionEstimate &action) {
    pb::dict row;
    row["public_action"] = action.action;
    row["shared_visits"] = action.visits;
    row["empirical_mean_return"] = action.visits > 0
            ? pb::cast(action.mean) : pb::object(pb::none());
    row["rank_by_empirical_mean"] = action.visits > 0
            ? pb::cast(action.rank) : pb::object(pb::none());
    return row;
}

pb::dict runJson(
        const Anchor &anchor,
        const ReplicateRun &run,
        const CheckpointRun &checkpoint) {
    pb::dict row;
    row["anchor_id"] = anchor.identity;
    row["game_seed"] = run.anchorSeed;
    row["replicate"] = run.replicate;
    row["root_evaluation_mode"] = rootModeName(run.rootMode);
    row["sampler_seed"] = run.samplerSeed;
    row["search_seed"] = run.searchSeed;
    row["particle_count"] = kParticleCount;
    row["budget_checkpoint"] = checkpoint.budget;
    row["total_simulations_at_checkpoint"] = checkpoint.budget;
    row["top_action_by_empirical_mean"] = pb::module_::import("json").attr("loads")(
            checkpoint.topActionKey);
    row["top_two_empirical_mean_gap"] = checkpoint.topTwoGap;
    row["root_actions"] = pb::list();
    auto actions = row["root_actions"].cast<pb::list>();
    for (std::size_t actionIndex = 0; actionIndex < checkpoint.actions.size(); ++actionIndex) {
        const auto &action = checkpoint.actions[actionIndex];
        auto actionRow = actionRunJson(action);
        const auto &exposure = checkpoint.rootActionParticleExposure[actionIndex];
        const auto [minExposure, maxExposure] = std::minmax_element(
                exposure.begin(), exposure.end());
        actionRow["particle_exposure"] = pb::dict(
                "distinct_particles_used"_a = std::count_if(
                        exposure.begin(), exposure.end(), [](std::uint64_t count) { return count > 0; }),
                "minimum_simulations_per_particle"_a = *minExposure,
                "maximum_simulations_per_particle"_a = *maxExposure,
                "balanced_within_one"_a = *maxExposure - *minExposure <= 1);
        actions.append(actionRow);
    }
    const auto [minExposure, maxExposure] = std::minmax_element(
            checkpoint.particleExposure.begin(), checkpoint.particleExposure.end());
    row["particle_exposure"] = pb::dict(
            "distinct_particles_used"_a = std::count_if(
                    checkpoint.particleExposure.begin(), checkpoint.particleExposure.end(),
                    [](std::uint64_t count) { return count > 0; }),
            "minimum_simulations_per_particle_across_actions"_a = *minExposure,
            "maximum_simulations_per_particle_across_actions"_a = *maxExposure,
            "balanced_within_each_root_action_to_one"_a = true);
    row["work"] = pb::dict(
            "total_simulations"_a = checkpoint.budget,
            "native_action_steps"_a = checkpoint.nativeActionSteps,
            "shared_nodes"_a = checkpoint.sharedNodes,
            "shared_edges"_a = checkpoint.sharedEdges,
            "terminal_simulations"_a = checkpoint.terminalSimulations,
            "depth_capped_simulations"_a = checkpoint.cappedSimulations,
            "mean_decision_depth"_a = static_cast<double>(checkpoint.decisionDepthSum)
                    / static_cast<double>(checkpoint.budget),
            "cumulative_wall_time_seconds"_a = checkpoint.cumulativeWallSeconds,
            "interval_wall_time_seconds"_a = checkpoint.intervalWallSeconds);
    return row;
}

struct AggregateAction {
    std::string key;
    pb::dict action;
    std::vector<double> means;
    std::vector<int> ranks;
    std::vector<std::uint64_t> visits;
};

struct AggregateCheckpoint {
    pb::dict evidence;
    double topGap = 0.0;
    double gapSe = 0.0;
    double maxAbsMeanChangeFromPrevious = 0.0;
    double medianAbsMeanChangeFromPrevious = 0.0;
};

double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    if (values.size() % 2 == 1) {
        return values[middle];
    }
    return (values[middle - 1] + values[middle]) / 2.0;
}

AggregateCheckpoint aggregateCheckpoint(
        const Anchor &anchor,
        const std::vector<ReplicateRun> &replicates,
        const std::size_t checkpointIndex,
        const AggregateCheckpoint *previous) {
    std::vector<AggregateAction> actions;
    for (const auto &row : replicates.front().checkpoints[checkpointIndex].actions) {
        actions.push_back(AggregateAction{row.key, row.action, {}, {}, {}});
    }
    for (const auto &replicate : replicates) {
        const auto &checkpoint = replicate.checkpoints[checkpointIndex];
        if (checkpoint.actions.size() != actions.size()) {
            throw std::logic_error("replicates do not share the same root legal action count");
        }
        for (std::size_t idx = 0; idx < actions.size(); ++idx) {
            if (checkpoint.actions[idx].key != actions[idx].key) {
                throw std::logic_error("replicates do not share ordered public root actions");
            }
            if (checkpoint.actions[idx].visits > 0) {
                actions[idx].means.push_back(checkpoint.actions[idx].mean);
                actions[idx].ranks.push_back(checkpoint.actions[idx].rank);
            }
            actions[idx].visits.push_back(checkpoint.actions[idx].visits);
        }
    }
    for (std::size_t idx = 0; idx < actions.size(); ++idx) {
        if (actions[idx].means.size() != replicates.size()) {
            throw std::logic_error("a root action was not estimated in every replicate");
        }
    }

    std::vector<std::size_t> order(actions.size());
    std::iota(order.begin(), order.end(), 0);
    const auto aggregateMean = [&](const std::size_t idx) {
        return std::accumulate(actions[idx].means.begin(), actions[idx].means.end(), 0.0)
                / static_cast<double>(actions[idx].means.size());
    };
    std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        const double leftMean = aggregateMean(left);
        const double rightMean = aggregateMean(right);
        if (leftMean != rightMean) {
            return leftMean > rightMean;
        }
        return left < right;
    });
    const auto bestIdx = order.front();
    const auto secondIdx = order.size() > 1 ? order[1] : order.front();
    const double bestMean = aggregateMean(bestIdx);
    const double secondMean = aggregateMean(secondIdx);
    const double gap = bestMean - secondMean;
    std::vector<double> pairedGaps;
    for (std::size_t replicateIndex = 0; replicateIndex < replicates.size(); ++replicateIndex) {
        pairedGaps.push_back(actions[bestIdx].means[replicateIndex]
                - actions[secondIdx].means[replicateIndex]);
    }
    const double gapSe = sampleSd(pairedGaps) / std::sqrt(static_cast<double>(pairedGaps.size()));

    std::vector<std::string> replicateWinners;
    std::vector<double> replicateTopGaps;
    std::size_t bestActionVotes = 0;
    for (const auto &replicate : replicates) {
        const auto &rows = replicate.checkpoints[checkpointIndex].actions;
        std::vector<std::size_t> runOrder;
        for (std::size_t idx = 0; idx < rows.size(); ++idx) {
            if (rows[idx].visits > 0) {
                runOrder.push_back(idx);
            }
        }
        std::stable_sort(runOrder.begin(), runOrder.end(), [&](std::size_t left, std::size_t right) {
            if (rows[left].mean != rows[right].mean) {
                return rows[left].mean > rows[right].mean;
            }
            return left < right;
        });
        replicateWinners.push_back(rows[runOrder.front()].key);
        replicateTopGaps.push_back(runOrder.size() > 1
                ? rows[runOrder[0]].mean - rows[runOrder[1]].mean : 0.0);
        if (rows[runOrder.front()].key == actions[bestIdx].key) {
            ++bestActionVotes;
        }
    }
    const double winnerAgreement = static_cast<double>(bestActionVotes)
            / static_cast<double>(replicates.size());

    pb::dict evidence;
    evidence["anchor_id"] = anchor.identity;
    evidence["budget_checkpoint"] = replicates.front().checkpoints[checkpointIndex].budget;
    evidence["particle_count"] = kParticleCount;
    evidence["independent_search_replicates"] = replicates.size();
    evidence["uncertainty_estimator"] =
            "sample standard error across independent replicate means; top-two gap SE uses paired replicate differences";
    evidence["top_action_by_aggregate_mean"] = actions[bestIdx].action;
    evidence["top_two_gap"] = gap;
    evidence["top_two_gap_standard_error"] = gapSe;
    if (gapSe > 0.0) {
        evidence["top_two_gap_to_se_ratio"] = gap / gapSe;
    } else {
        evidence["top_two_gap_to_se_ratio"] = pb::none();
    }
    evidence["top_action_replicate_agreement_fraction"] = winnerAgreement;
    evidence["replicate_top_two_gap_mean"] = std::accumulate(
            replicateTopGaps.begin(), replicateTopGaps.end(), 0.0)
            / static_cast<double>(replicateTopGaps.size());
    evidence["replicate_top_two_gap_standard_deviation"] = sampleSd(replicateTopGaps);
    pb::list winnerRows;
    for (std::size_t idx = 0; idx < replicates.size(); ++idx) {
        winnerRows.append(pb::dict(
                "replicate"_a = replicates[idx].replicate,
                "top_action"_a = pb::module_::import("json").attr("loads")(replicateWinners[idx]),
                "top_two_gap"_a = replicateTopGaps[idx]));
    }
    evidence["replicate_winners"] = winnerRows;

    pb::list actionRows;
    std::vector<double> absoluteMeanChanges;
    for (std::size_t rank = 0; rank < order.size(); ++rank) {
        const auto idx = order[rank];
        const double mean = aggregateMean(idx);
        const double sd = sampleSd(actions[idx].means);
        const double se = sd / std::sqrt(static_cast<double>(actions[idx].means.size()));
        const auto [minRank, maxRank] = std::minmax_element(
                actions[idx].ranks.begin(), actions[idx].ranks.end());
        const double rank1Fraction = static_cast<double>(std::count(
                actions[idx].ranks.begin(), actions[idx].ranks.end(), 1))
                / static_cast<double>(actions[idx].ranks.size());
        pb::list replicateMeans;
        for (std::size_t repIdx = 0; repIdx < actions[idx].means.size(); ++repIdx) {
            replicateMeans.append(pb::dict(
                    "replicate"_a = replicates[repIdx].replicate,
                    "mean_return"_a = actions[idx].means[repIdx],
                    "rank"_a = actions[idx].ranks[repIdx],
                    "visits"_a = actions[idx].visits[repIdx]));
        }
        pb::dict row;
        row["public_action"] = actions[idx].action;
        row["aggregate_rank"] = rank + 1;
        row["aggregate_mean_return"] = mean;
        row["replicate_standard_deviation"] = sd;
        row["replicate_standard_error"] = se;
        row["rank_range_across_replicates"] = pb::make_tuple(*minRank, *maxRank);
        row["rank_one_fraction"] = rank1Fraction;
        row["replicate_estimates"] = replicateMeans;
        if (previous != nullptr) {
            const auto priorActions = previous->evidence["root_actions"].cast<pb::list>();
            for (const auto priorHandle : priorActions) {
                const auto prior = pb::reinterpret_borrow<pb::dict>(priorHandle);
                if (canonicalJson(prior["public_action"]) == actions[idx].key) {
                    row["mean_change_from_previous_budget"] = mean
                            - prior["aggregate_mean_return"].cast<double>();
                    absoluteMeanChanges.push_back(std::abs(
                            row["mean_change_from_previous_budget"].cast<double>()));
                    break;
                }
            }
        }
        actionRows.append(row);
    }
    evidence["root_actions"] = actionRows;

    double maxAbsChange = 0.0;
    if (previous != nullptr) {
        for (const auto actionHandle : actionRows) {
            const auto actionRow = pb::reinterpret_borrow<pb::dict>(actionHandle);
            if (actionRow.contains("mean_change_from_previous_budget")) {
                maxAbsChange = std::max(maxAbsChange, std::abs(
                        actionRow["mean_change_from_previous_budget"].cast<double>()));
            }
        }
    }
    evidence["max_absolute_root_action_mean_change_from_previous_budget"] =
            previous == nullptr ? pb::object(pb::none()) : pb::cast(maxAbsChange);
    const double medianAbsChange = median(absoluteMeanChanges);
    evidence["median_absolute_root_action_mean_change_from_previous_budget"] =
            previous == nullptr ? pb::object(pb::none()) : pb::cast(medianAbsChange);

    bool allBalanced = true;
    std::uint64_t minExposure = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t maxExposure = 0;
    for (const auto &replicate : replicates) {
        for (const auto &exposure : replicate.checkpoints[checkpointIndex].rootActionParticleExposure) {
            const auto minmax = std::minmax_element(exposure.begin(), exposure.end());
            minExposure = std::min(minExposure, *minmax.first);
            maxExposure = std::max(maxExposure, *minmax.second);
            allBalanced = allBalanced && (*minmax.second - *minmax.first <= 1);
        }
    }
    evidence["particle_exposure"] = pb::dict(
            "balanced_within_each_root_action"_a = allBalanced,
            "minimum_simulations_per_particle_within_one_root_action"_a = minExposure,
            "maximum_simulations_per_particle_within_one_root_action"_a = maxExposure);

    AggregateCheckpoint result;
    result.evidence = evidence;
    result.topGap = gap;
    result.gapSe = gapSe;
    result.maxAbsMeanChangeFromPrevious = maxAbsChange;
    result.medianAbsMeanChangeFromPrevious = medianAbsChange;
    return result;
}

pb::dict compareAtEqualBudget(
        const AggregateCheckpoint &adaptive,
        const AggregateCheckpoint &stratified) {
    const auto adaptiveActions = adaptive.evidence["root_actions"].cast<pb::list>();
    const auto stratifiedActions = stratified.evidence["root_actions"].cast<pb::list>();
    if (adaptiveActions.size() != stratifiedActions.size()) {
        throw std::logic_error("root modes produced different public legal action counts");
    }

    pb::list actionRows;
    std::vector<double> absoluteDifferences;
    for (const auto adaptiveHandle : adaptiveActions) {
        const auto adaptiveRow = pb::reinterpret_borrow<pb::dict>(adaptiveHandle);
        const auto actionKey = canonicalJson(adaptiveRow["public_action"]);
        const pb::dict *stratifiedRowPtr = nullptr;
        pb::dict stratifiedRow;
        for (const auto stratifiedHandle : stratifiedActions) {
            const auto candidate = pb::reinterpret_borrow<pb::dict>(stratifiedHandle);
            if (canonicalJson(candidate["public_action"]) == actionKey) {
                stratifiedRow = candidate;
                stratifiedRowPtr = &stratifiedRow;
                break;
            }
        }
        if (stratifiedRowPtr == nullptr) {
            throw std::logic_error("root modes produced different public actions");
        }

        const double adaptiveMean = adaptiveRow["aggregate_mean_return"].cast<double>();
        const double stratifiedMean = stratifiedRow["aggregate_mean_return"].cast<double>();
        std::vector<double> pairedDifferences;
        const auto adaptiveReplicates = adaptiveRow["replicate_estimates"].cast<pb::list>();
        const auto stratifiedReplicates = stratifiedRow["replicate_estimates"].cast<pb::list>();
        if (adaptiveReplicates.size() != stratifiedReplicates.size()) {
            throw std::logic_error("root modes have different replicate counts");
        }
        for (const auto adaptiveReplicateHandle : adaptiveReplicates) {
            const auto adaptiveReplicate = pb::reinterpret_borrow<pb::dict>(adaptiveReplicateHandle);
            const int replicateId = intField(adaptiveReplicate, "replicate");
            for (const auto stratifiedReplicateHandle : stratifiedReplicates) {
                const auto stratifiedReplicate =
                        pb::reinterpret_borrow<pb::dict>(stratifiedReplicateHandle);
                if (intField(stratifiedReplicate, "replicate") == replicateId) {
                    pairedDifferences.push_back(
                            stratifiedReplicate["mean_return"].cast<double>()
                            - adaptiveReplicate["mean_return"].cast<double>());
                    break;
                }
            }
        }
        if (pairedDifferences.size() != adaptiveReplicates.size()) {
            throw std::logic_error("root modes are missing paired replicate estimates");
        }
        const double absoluteDifference = std::abs(stratifiedMean - adaptiveMean);
        absoluteDifferences.push_back(absoluteDifference);

        pb::dict row;
        row["public_action"] = adaptiveRow["public_action"];
        row["adaptive_aggregate_rank"] = adaptiveRow["aggregate_rank"];
        row["stratified_aggregate_rank"] = stratifiedRow["aggregate_rank"];
        row["adaptive_mean_return"] = adaptiveMean;
        row["stratified_mean_return"] = stratifiedMean;
        row["stratified_minus_adaptive_mean"] = stratifiedMean - adaptiveMean;
        row["absolute_mode_difference"] = absoluteDifference;
        row["paired_replicate_difference_standard_error"] =
                sampleSd(pairedDifferences) / std::sqrt(static_cast<double>(pairedDifferences.size()));
        row["adaptive_visits_by_replicate"] = pb::list();
        row["stratified_visits_by_replicate"] = pb::list();
        auto adaptiveVisits = row["adaptive_visits_by_replicate"].cast<pb::list>();
        auto stratifiedVisits = row["stratified_visits_by_replicate"].cast<pb::list>();
        for (const auto replicateHandle : adaptiveReplicates) {
            const auto replicate = pb::reinterpret_borrow<pb::dict>(replicateHandle);
            adaptiveVisits.append(pb::dict(
                    "replicate"_a = replicate["replicate"],
                    "visits"_a = replicate["visits"]));
        }
        for (const auto replicateHandle : stratifiedReplicates) {
            const auto replicate = pb::reinterpret_borrow<pb::dict>(replicateHandle);
            stratifiedVisits.append(pb::dict(
                    "replicate"_a = replicate["replicate"],
                    "visits"_a = replicate["visits"]));
        }
        actionRows.append(row);
    }

    pb::dict result;
    result["budget_checkpoint"] = adaptive.evidence["budget_checkpoint"];
    result["adaptive_top_action"] = adaptive.evidence["top_action_by_aggregate_mean"];
    result["stratified_top_action"] = stratified.evidence["top_action_by_aggregate_mean"];
    result["adaptive_top_two_gap"] = adaptive.evidence["top_two_gap"];
    result["adaptive_top_two_gap_standard_error"] = adaptive.evidence["top_two_gap_standard_error"];
    result["stratified_top_two_gap"] = stratified.evidence["top_two_gap"];
    result["stratified_top_two_gap_standard_error"] = stratified.evidence["top_two_gap_standard_error"];
    result["adaptive_winner_replicate_agreement_fraction"] =
            adaptive.evidence["top_action_replicate_agreement_fraction"];
    result["stratified_winner_replicate_agreement_fraction"] =
            stratified.evidence["top_action_replicate_agreement_fraction"];
    result["maximum_absolute_action_mean_difference"] =
            absoluteDifferences.empty() ? 0.0
                    : *std::max_element(absoluteDifferences.begin(), absoluteDifferences.end());
    result["median_absolute_action_mean_difference"] = median(absoluteDifferences);
    result["root_actions"] = actionRows;
    return result;
}

pb::dict verifyPublicRootSharing(const Anchor &anchor) {
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
    const auto nodeKey = historyNodeKey(std::vector<std::string>{publicStateKey});
    for (std::uint64_t particleIndex = 0; particleIndex < kParticleCount; ++particleIndex) {
        auto particle = anchor.simulator->samplePublicConsistentHiddenFuture(
                kSamplerSeedBase + static_cast<std::uint64_t>(anchor.gameSeed), particleIndex);
        const auto state = particle.publicBattleState();
        if (canonicalJson(state) != publicStateKey) {
            throw std::logic_error("preflight particles do not share public root state");
        }
        std::vector<std::string> actions;
        for (const auto &action : publicActions(state)) {
            actions.push_back(canonicalJson(action));
        }
        if (actions != rootActionKeys) {
            throw std::logic_error("preflight particles do not share ordered legal actions");
        }
        rootKeys.insert(nodeKey);
        auto &node = tree.getOrCreate(nodeKey, state);
        ++node.visits;
        ++node.edges.front().visits;
    }
    if (rootKeys.size() != 1 || tree.nodes.size() != 1) {
        throw std::logic_error("root node identity depends on particle source");
    }
    const auto &root = tree.nodes.begin()->second;
    if (root.visits != kParticleCount || root.edges.front().visits != kParticleCount) {
        throw std::logic_error("particle sources do not update common root statistics");
    }
    return pb::dict(
            "root_public_state_identical_for_32_particles"_a = true,
            "ordered_public_actions_identical_for_32_particles"_a = true,
            "single_shared_root_node_count"_a = tree.nodes.size(),
            "shared_root_visits_from_particle_sources"_a = root.visits,
            "same_root_edge_visits_from_particle_sources"_a = root.edges.front().visits,
            "tree_key_source"_a = "public battle state plus public action/result history",
            "private_particle_identity_in_node_key"_a = false);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: study-public-q-root-allocation <code-commit> <output-json>\n";
        return 2;
    }

    try {
        pb::scoped_interpreter interpreter{};
        const std::string codeCommit = argv[1];
        const std::string outputPath = argv[2];
        const std::string progressPath = outputPath + ".progress.json";
        writeExecutionProgress(progressPath, "anchor_setup", 0, "", "", -1, 0);
        pb::dict report;
        report["schema_id"] = "public-root-q-allocation-comparison-v1";
        report["code_commit"] = codeCommit;
        report["simulator_base"] = "origin/spire/main@f6279292f685026f05d57cda9140650185d76043";
        report["information_regime"] = "normal_public";
        report["sampler_interpretation"] = "reproducible proposal distribution; not an exact posterior";
        report["root_modes"] = pb::make_tuple("adaptive_root", "stratified_root");
        report["adaptive_root_policy"] = "ordinary UCT root action selection from shared public-node statistics";
        report["stratified_root_policy"] = "evaluation-only cyclic schedule: simulation i forces ordered public root action i modulo the number of legal root actions; at budget B, the first B modulo action_count actions receive one extra visit";
        report["continuation_policy"] = "both root modes use the same shared UCT selection keyed by public state and public action/result history; selection uses no hidden particle identity/state";
        report["return_definition"] = "terminal victory/loss is +1/-1; at 64 public decision steps use clamp(player_hp/max_hp - 0.5 * sum(monster_hp/max_hp), -1, 1) from the public state";
        report["uncertainty_definition"] = "sample standard error across "
                + std::to_string(kReplicates)
                + " independent search and sampler replicate means; equal-budget adaptive/stratified differences use paired replicate means; top-two gap SE uses paired replicate differences";
        report["budget_schedule"] = pb::make_tuple(kBudgets[0], kBudgets[1]);
        report["budget_semantics"] = "nested cumulative prefixes at B=384 and 1536; each mode uses the same fixed total simulation budget and is not multiplied by particle count";
        report["particle_count"] = kParticleCount;
        report["replicates_per_anchor"] = kReplicates;
        report["maximum_simulations_per_mode_replicate"] = kBudgets[1];
        report["decision_depth_cap"] = kMaxDecisionDepth;
        report["native_action_step_counter"] = "counts calls to stepPublicAction in searched trajectories; simulator-internal queue steps are not separately instrumented";
        report["materiality_policy"] = "No numerical cutoff for a material reduction was specified in Issue #8. The artifact reports per-anchor max/median budget shifts, rank-one stability, and paired equal-budget mode differences for independent Reviewer interpretation.";

        std::vector<Anchor> anchors;
        pb::list anchorRows;
        pb::list skippedAnchors;
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
                row["distinct_hidden_draw_orders_in_32_samples"] = anchor.distinctHiddenOrders;
                const auto actions = publicActions(anchor.publicState);
                row["normal_public_legal_action_count"] = actions.size();
                row["root_public_actions"] = actions;
                pb::list remainderSchedules;
                for (const int budget : kBudgets) {
                    const auto actionCount = actions.size();
                    const auto remainder = static_cast<std::size_t>(budget) % actionCount;
                    pb::list visitsByAction;
                    for (std::size_t actionIndex = 0; actionIndex < actionCount; ++actionIndex) {
                        visitsByAction.append(pb::dict(
                                "ordered_action_index"_a = actionIndex,
                                "public_action"_a = actions[actionIndex],
                                "root_visits"_a = static_cast<std::size_t>(budget) / actionCount
                                        + (actionIndex < remainder ? 1 : 0)));
                    }
                    remainderSchedules.append(pb::dict(
                            "budget"_a = budget,
                            "visits_by_ordered_public_action"_a = visitsByAction));
                }
                row["stratified_root_visits_by_budget"] = remainderSchedules;
                anchorRows.append(row);
                anchors.push_back(std::move(anchor));
            } catch (const std::exception &error) {
                skippedAnchors.append(pb::dict("game_seed"_a = seed, "reason"_a = error.what()));
            }
        }
        if (anchors.size() < 2) {
            throw std::runtime_error("fewer than two supported anchors with hidden-future uncertainty");
        }
        report["anchors"] = anchorRows;
        report["skipped_anchors"] = skippedAnchors;
        report["total_maximum_simulations"] = anchors.size() * 2 * kReplicates * kBudgets[1];

        pb::list semanticPreflight;
        const RootMode rootModes[] = {RootMode::Adaptive, RootMode::Stratified};
        std::vector<std::vector<std::vector<ReplicateRun>>> runsByMode(
                2, std::vector<std::vector<ReplicateRun>>(anchors.size()));
        pb::list runRows;
        std::size_t completedReplicates = 0;
        for (std::size_t anchorIndex = 0; anchorIndex < anchors.size(); ++anchorIndex) {
            const auto &anchor = anchors[anchorIndex];
            pb::dict preflight = verifyPublicRootSharing(anchor);
            preflight["anchor_id"] = anchor.identity;
            semanticPreflight.append(preflight);

            for (std::size_t modeIndex = 0; modeIndex < 2; ++modeIndex) {
                for (int replicateIndex = 0; replicateIndex < kReplicates; ++replicateIndex) {
                    writeExecutionProgress(
                            progressPath, "replicate_running", completedReplicates,
                            rootModeName(rootModes[modeIndex]), anchor.identity,
                            replicateIndex, 0);
                    auto run = runReplicate(anchor, replicateIndex, rootModes[modeIndex]);
                    for (const auto &checkpoint : run.checkpoints) {
                        runRows.append(runJson(anchor, run, checkpoint));
                    }
                    runsByMode[modeIndex][anchorIndex].push_back(std::move(run));
                    ++completedReplicates;
                    writeExecutionProgress(
                            progressPath, "replicate_completed", completedReplicates,
                            rootModeName(rootModes[modeIndex]), anchor.identity,
                            replicateIndex, kBudgets[1]);
                }
            }
        }
        report["execution_state"] = pb::dict(
                "completed_mode_anchor_replicates"_a = completedReplicates,
                "expected_mode_anchor_replicates"_a = anchors.size() * 2 * kReplicates,
                "last_completed_budget"_a = kBudgets[1]);
        report["semantic_preflight"] = semanticPreflight;
        report["runs"] = runRows;

        std::vector<std::vector<std::vector<AggregateCheckpoint>>> aggregatesByMode(
                2, std::vector<std::vector<AggregateCheckpoint>>(anchors.size()));
        pb::list modeAnalyses;
        for (std::size_t modeIndex = 0; modeIndex < 2; ++modeIndex) {
            pb::dict modeAnalysis;
            modeAnalysis["root_evaluation_mode"] = rootModeName(rootModes[modeIndex]);
            modeAnalysis["policy_is_deployment_controller"] = false;
            pb::list anchorAnalyses;
            for (std::size_t anchorIndex = 0; anchorIndex < anchors.size(); ++anchorIndex) {
                pb::dict anchorAnalysis;
                anchorAnalysis["anchor_id"] = anchors[anchorIndex].identity;
                pb::list checkpoints;
                auto &aggregates = aggregatesByMode[modeIndex][anchorIndex];
                for (std::size_t checkpointIndex = 0; checkpointIndex < 2; ++checkpointIndex) {
                    const auto *previous = aggregates.empty() ? nullptr : &aggregates.back();
                    auto aggregate = aggregateCheckpoint(
                            anchors[anchorIndex], runsByMode[modeIndex][anchorIndex],
                            checkpointIndex, previous);
                    checkpoints.append(aggregate.evidence);
                    aggregates.push_back(std::move(aggregate));
                }
                anchorAnalysis["budget_results"] = checkpoints;
                anchorAnalyses.append(anchorAnalysis);
            }
            modeAnalysis["anchor_analyses"] = anchorAnalyses;
            modeAnalyses.append(modeAnalysis);
        }
        report["mode_analyses"] = modeAnalyses;

        pb::list comparisons;
        for (std::size_t anchorIndex = 0; anchorIndex < anchors.size(); ++anchorIndex) {
            const auto &adaptive = aggregatesByMode[0][anchorIndex];
            const auto &stratified = aggregatesByMode[1][anchorIndex];
            pb::dict comparison;
            comparison["anchor_id"] = anchors[anchorIndex].identity;
            pb::list equalBudgetRows;
            for (std::size_t checkpointIndex = 0; checkpointIndex < 2; ++checkpointIndex) {
                equalBudgetRows.append(compareAtEqualBudget(
                        adaptive[checkpointIndex], stratified[checkpointIndex]));
            }
            comparison["equal_budget_mode_comparisons"] = equalBudgetRows;
            comparison["adaptive_max_absolute_mean_shift_384_to_1536"] =
                    adaptive[1].maxAbsMeanChangeFromPrevious;
            comparison["adaptive_median_absolute_mean_shift_384_to_1536"] =
                    adaptive[1].medianAbsMeanChangeFromPrevious;
            comparison["stratified_max_absolute_mean_shift_384_to_1536"] =
                    stratified[1].maxAbsMeanChangeFromPrevious;
            comparison["stratified_median_absolute_mean_shift_384_to_1536"] =
                    stratified[1].medianAbsMeanChangeFromPrevious;
            comparison["max_shift_reduction_fraction"] =
                    adaptive[1].maxAbsMeanChangeFromPrevious > 0.0
                    ? pb::cast(1.0 - stratified[1].maxAbsMeanChangeFromPrevious
                            / adaptive[1].maxAbsMeanChangeFromPrevious)
                    : pb::object(pb::none());
            comparison["median_shift_reduction_fraction"] =
                    adaptive[1].medianAbsMeanChangeFromPrevious > 0.0
                    ? pb::cast(1.0 - stratified[1].medianAbsMeanChangeFromPrevious
                            / adaptive[1].medianAbsMeanChangeFromPrevious)
                    : pb::object(pb::none());
            comparison["adaptive_winner_agreement_at_384"] =
                    adaptive[0].evidence["top_action_replicate_agreement_fraction"];
            comparison["stratified_winner_agreement_at_384"] =
                    stratified[0].evidence["top_action_replicate_agreement_fraction"];
            comparison["adaptive_winner_agreement_at_1536"] =
                    adaptive[1].evidence["top_action_replicate_agreement_fraction"];
            comparison["stratified_winner_agreement_at_1536"] =
                    stratified[1].evidence["top_action_replicate_agreement_fraction"];
            comparisons.append(comparison);
        }
        report["anchor_comparisons"] = comparisons;
        report["diagnostic_status"] = "DIAGNOSTIC_COMPLETE";
        report["claim_boundary"] = "This study diagnoses root-action measurement allocation only; it makes no claim that stratified visitation should be used by a deployed controller and no Oracle-agreement, exact-posterior, broad-strength, or online-readiness claim.";

        writeExecutionProgress(
                progressPath, "writing_result_artifact", completedReplicates,
                "", "", -1, kBudgets[1]);
        std::ofstream output(outputPath, std::ios::binary);
        if (!output) {
            throw std::runtime_error("could not open output JSON path");
        }
        output << indentedJson(report) << '\n';
        if (!output.good()) {
            throw std::runtime_error("failed while writing output JSON");
        }
        (void) std::remove(progressPath.c_str());
        std::cout << "result=DIAGNOSTIC_COMPLETE anchors=" << anchors.size()
                << " replicates_per_anchor=" << kReplicates
                << " modes=adaptive_root,stratified_root budgets=384,1536 particles=" << kParticleCount << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "study-public-q-root-allocation failed: " << error.what() << '\n';
        return 1;
    }
}
