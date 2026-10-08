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

#include "study_issue21_public_tactical_heuristic.hpp"

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

pb::dict mechanicsOnlyWitness(StepSimulator &simulator, pb::list &history) {
    bool reached = false;
    for (int step = 0; step < 512; ++step) {
        const auto raw = simulator.snapshot();
        const auto projection = simulator.publicProjection();
        const auto screen = projectionScreen(projection);
        if (screen == "BATTLE"
                && raw["battle_active"].cast<bool>()
                && stringField(raw, "battle_input_state") == "PLAYER_NORMAL") {
            reached = true;
            break;
        }

        const auto publicCandidates = projectionCandidates(projection);
        const auto actions = simulator.legalActions();
        if (static_cast<std::size_t>(publicCandidates.size()) != actions.size()
                || actions.empty()) {
            throw std::logic_error("fixture legal-action mapping was unavailable");
        }
        const auto selectedPublic = pb::reinterpret_borrow<pb::dict>(publicCandidates[0]);
        const auto nativePublic = publicProjectionActionSnapshot(actions[0]);
        if (canonicalJson(selectedPublic) != canonicalJson(nativePublic)) {
            throw std::logic_error("mechanics fixture candidate order did not match native actions");
        }
        const auto next = simulator.step(actions[0]);
        const auto nextProjection = simulator.publicProjection();
        pb::dict entry;
        entry["schema_id"] = "issue21-mechanics-fixture-history-v1";
        entry["history_index"] = step;
        entry["from_screen"] = screen;
        entry["selected_public_candidate"] = selectedPublic;
        entry["to_screen"] = projectionScreen(nextProjection);
        entry["fixture_mode"] = "mechanics_only_first_legal_setup";
        history.append(entry);
        (void) next;
    }
    if (!reached) {
        throw std::runtime_error("mechanics-only fixture did not reach a normal Battle root");
    }

    const auto projection = simulator.publicProjection();
    const auto battle = simulator.publicBattleState();
    if (projection["schema_id"].cast<std::string>() != "native-public-projection-v3"
            || stringField(battle, "information_regime") != "normal_public"
            || stringField(battle, "information_fidelity") != "supported") {
        throw std::logic_error("mechanics fixture root lacks supported native public fidelity");
    }
    if (containsPrivateField(projection) || containsPrivateField(battle)) {
        throw std::logic_error("native public fixture projection contains private fields");
    }
    if (canonicalJson(projectionCandidates(projection))
            != canonicalJson(battle["ordered_public_legal_actions"])) {
        throw std::logic_error("v3 and Battle public candidate order did not match");
    }
    return projection;
}

}  // namespace

int main(int argc, char **argv) {
    try {
        const std::filesystem::path output = argc > 1
                ? std::filesystem::path(argv[1])
                : std::filesystem::path("studies/issue21/native-witness.json");
        pb::scoped_interpreter interpreter{};

        StepSimulator anchor(CharacterClass::IRONCLAD, kGameSeed, kAscension);
        pb::list fixtureHistory;
        const auto rootProjection = mechanicsOnlyWitness(anchor, fixtureHistory);
        const auto rootState = anchor.publicBattleState();
        const auto rootActions = publicActions(rootState);

        // The following samples differ in native hidden future, while their
        // policy input source is constructed exclusively from these public
        // projections, Battle observations, and append-only fixture history.
        StepSimulator firstParticle = anchor.samplePublicConsistentHiddenFuture(kSamplerSeed, 0);
        StepSimulator secondParticle = anchor.samplePublicConsistentHiddenFuture(kSamplerSeed, 1);
        bool privateFuturesDiffer = privateFutureDiffers(firstParticle, secondParticle);
        int secondParticleIndex = 1;
        for (int index = 2; !privateFuturesDiffer && index < kParticleCount; ++index) {
            secondParticle = anchor.samplePublicConsistentHiddenFuture(
                    kSamplerSeed, static_cast<std::uint64_t>(index));
            privateFuturesDiffer = privateFutureDiffers(firstParticle, secondParticle);
            secondParticleIndex = index;
        }
        if (!privateFuturesDiffer) {
            throw std::logic_error("fixture sampler did not produce distinct private futures");
        }
        const auto firstProjection = firstParticle.publicProjection();
        const auto secondProjection = secondParticle.publicProjection();
        const auto firstBattle = firstParticle.publicBattleState();
        const auto secondBattle = secondParticle.publicBattleState();
        const bool projectionsEqual = canonicalJson(firstProjection)
                == canonicalJson(secondProjection);
        const bool battlesEqual = canonicalJson(firstBattle) == canonicalJson(secondBattle);
        if (!projectionsEqual || !battlesEqual
                || canonicalJson(firstProjection) != canonicalJson(rootProjection)
                || canonicalJson(firstBattle) != canonicalJson(rootState)) {
            throw std::logic_error("hidden-future witnesses did not preserve the public root");
        }

        const auto baselineAction = selectPublicHeuristicAction(rootState);
        const auto baselineIndex = publicActionIndex(rootActions, baselineAction);
        const auto started = std::chrono::steady_clock::now();
        const auto search = searchDecision(
                anchor,
                rootState,
                kSearchBudget,
                kSamplerSeed,
                kSearchSeed);
        const auto finished = std::chrono::steady_clock::now();
        const double searchSeconds = std::chrono::duration<double>(finished - started).count();
        const auto searchIndex = publicActionIndex(rootActions, search.selectedAction);

        const auto nativeActions = anchor.legalActions();
        const auto baselineNativeIdentity = publicProjectionActionSnapshot(
                nativeActions.at(baselineIndex));
        const auto searchNativeIdentity = publicProjectionActionSnapshot(
                nativeActions.at(searchIndex));
        const bool baselineMappingMatches = canonicalJson(baselineAction)
                == canonicalJson(baselineNativeIdentity);
        const bool searchMappingMatches = canonicalJson(search.selectedAction)
                == canonicalJson(searchNativeIdentity);
        if (!baselineMappingMatches || !searchMappingMatches) {
            throw std::logic_error("public-root identity did not map to the same native legal action");
        }

        StepSimulator baselineExecution = anchor;
        StepSimulator searchExecution = anchor;
        (void) baselineExecution.stepPublicAction(baselineAction);
        (void) searchExecution.stepPublicAction(search.selectedAction);

        pb::dict witnessA;
        witnessA["native_public_projection"] = firstProjection;
        witnessA["native_public_battle_observation"] = firstBattle;
        witnessA["public_history"] = fixtureHistory;
        pb::dict witnessB;
        witnessB["native_public_projection"] = secondProjection;
        witnessB["native_public_battle_observation"] = secondBattle;
        witnessB["public_history"] = fixtureHistory;
        pb::list witnesses;
        witnesses.append(witnessA);
        witnesses.append(witnessB);

        pb::dict mapping;
        mapping["ordered_public_candidate_count"] = rootActions.size();
        mapping["baseline_selected_ordinal"] = baselineIndex;
        mapping["baseline_selected_public_identity"] = baselineAction;
        mapping["search_selected_ordinal"] = searchIndex;
        mapping["search_selected_public_identity"] = search.selectedAction;
        mapping["baseline_public_identity_matches_native_legal_action"] = baselineMappingMatches;
        mapping["search_public_identity_matches_native_legal_action"] = searchMappingMatches;
        mapping["both_public_identities_accepted_by_step_public_action"] = true;

        pb::dict report;
        report["schema_id"] = "issue21-native-v3-firewall-witness-v1";
        report["native_base_commit"] = "a655dbb264b2274c54acf0426cd5df19a8c8ec96";
        report["fixture_mode"] = "mechanics_only_fixture";
        report["normal_public_run_claim"] = false;
        report["game_seed"] = kGameSeed;
        report["ascension"] = kAscension;
        report["mechanics_prelude_steps"] = fixtureHistory.size();
        report["mechanics_prelude_actions_are_policy_decisions"] = false;
        report["public_history"] = fixtureHistory;
        report["native_sampled_hidden_futures_differ"] = privateFuturesDiffer;
        report["second_distinct_particle_index"] = secondParticleIndex;
        report["native_v3_projection_equal_across_hidden_futures"] = projectionsEqual;
        report["native_battle_observation_equal_across_hidden_futures"] = battlesEqual;
        report["private_snapshot_values_emitted"] = false;
        report["policy_input_witnesses"] = witnesses;
        report["root_action_mapping"] = mapping;
        report["baseline_controller_source"] =
                "Issue 17 public tactical heuristic at 9a2792e1e02157124b4f90edc91b7ad8765d5d10";
        report["shared_public_search_source"] =
                "Issue 17 searchDecision/selectSharedEdge adapted from 9a2792e1e02157124b4f90edc91b7ad8765d5d10; no Search-v2 call";
        report["shared_public_search"] = search.evidence;
        report["shared_public_search_wall_seconds"] = searchSeconds;

        std::filesystem::create_directories(output.parent_path());
        std::ofstream file(output, std::ios::binary);
        if (!file) {
            throw std::runtime_error("could not open native witness output");
        }
        file << indentedJson(report) << '\n';
        std::cout << indentedJson(report) << '\n';
        std::cout << "ISSUE21_NATIVE_V3_FIREWALL_WITNESS_PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "ISSUE21_NATIVE_V3_FIREWALL_WITNESS_FAIL: " << error.what() << '\n';
        return 1;
    }
}
