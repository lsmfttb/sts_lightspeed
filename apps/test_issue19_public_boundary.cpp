#include "../bindings/slaythespire.cpp"

#include <pybind11/embed.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace pybind11;

std::string screenIdentity(StepSimulator &simulator) {
    const auto projection = simulator.publicProjection();
    return projection["screen_identity"].cast<dict>()["value"].cast<std::string>();
}

dict checkedProjection(StepSimulator &simulator) {
    auto projection = simulator.publicProjection();
    const auto observed = projection["candidate_actions"].cast<dict>()["value"].cast<list>();
    const auto nativeActions = simulator.legalActions();
    if (observed.size() != nativeActions.size()) {
        throw std::logic_error("public projection candidate count differs from legal actions");
    }
    for (std::size_t index = 0; index < observed.size(); ++index) {
        const auto candidate = observed[index].cast<dict>();
        if (candidate.contains("bits")) {
            throw std::logic_error("public candidate exposed opaque native action bits");
        }
        if (!candidate.equal(publicProjectionActionSnapshot(nativeActions[index]))) {
            throw std::logic_error("public candidate order/identity differs from legal actions");
        }
    }
    return projection;
}

std::vector<int> hiddenDrawOrder(const StepSimulator &simulator) {
    std::vector<int> result;
    result.reserve(simulator.bc.cards.drawPile.size());
    for (const auto &card : simulator.bc.cards.drawPile) {
        result.push_back(card.getUniqueId());
    }
    return result;
}

nlohmann::json availabilitySummary(const dict &projection, const char *fieldName) {
    const auto field = projection[fieldName].cast<dict>();
    nlohmann::json result = {
        {"availability", field["availability"].cast<std::string>()}
    };
    if (field.contains("reason")) {
        result["reason"] = field["reason"].cast<std::string>();
    }
    return result;
}

nlohmann::json publicCandidateLabels(const dict &projection) {
    const auto candidates = projection["candidate_actions"].cast<dict>()
            ["value"].cast<list>();
    nlohmann::json labels = nlohmann::json::array();
    for (const auto &item : candidates) {
        labels.push_back(item.cast<dict>()["label"].cast<std::string>());
    }
    return labels;
}

}  // namespace

int main() {
    try {
        scoped_interpreter interpreter{};
        constexpr std::uint64_t seed = 49;
        constexpr int ascension = 20;
        constexpr int maxDiagnosticPreludeSteps = 256;
        StepSimulator anchor(CharacterClass::IRONCLAD, seed, ascension);

        int preludeSteps = 0;
        nlohmann::json noncombatScreenSamples = nlohmann::json::array();
        while (screenIdentity(anchor) != "BATTLE"
                && preludeSteps < maxDiagnosticPreludeSteps) {
            const auto actions = anchor.legalActions();
            if (actions.empty()) {
                throw std::runtime_error("diagnostic prelude reached a nonterminal screen without actions");
            }
            const auto projection = checkedProjection(anchor);
            const auto screen = projection["screen_identity"].cast<dict>()
                    ["value"].cast<std::string>();
            if (screen != "BATTLE") {
                noncombatScreenSamples.push_back({
                    {"screen", screen},
                    {"screen_payload", availabilitySummary(projection, "screen_payload")},
                    {"visible_map_graph", availabilitySummary(projection, "visible_map_graph")},
                    {"current_map_node", availabilitySummary(projection, "current_map_node")},
                    {"immediately_legal_routes", availabilitySummary(projection, "immediately_legal_routes")},
                    {"candidate_labels", publicCandidateLabels(projection)}
                });
            }
            (void) anchor.step(actions.front());
            ++preludeSteps;
        }
        if (screenIdentity(anchor) != "BATTLE") {
            throw std::runtime_error("seed 49 did not reach battle within the bounded diagnostic prelude");
        }

        const auto rootProjection = checkedProjection(anchor);
        const auto rootBattleState = anchor.publicBattleState();
        if (rootBattleState["information_fidelity"].cast<std::string>() != "supported") {
            throw std::runtime_error("seed 49 battle root is not supported by public battle state");
        }
        auto firstFuture = anchor.samplePublicConsistentHiddenFuture(0x19A2026ULL, 0);
        const auto firstPublic = firstFuture.publicBattleState();
        const auto firstProjection = firstFuture.publicProjection();
        const auto firstDrawOrder = hiddenDrawOrder(firstFuture);

        bool foundDistinctHiddenFuture = false;
        std::uint64_t distinctParticleIndex = 0;
        for (std::uint64_t particleIndex = 1; particleIndex <= 64; ++particleIndex) {
            auto candidate = anchor.samplePublicConsistentHiddenFuture(
                    0x19A2026ULL, particleIndex);
            const auto candidatePublic = candidate.publicBattleState();
            const auto candidateProjection = candidate.publicProjection();
            if (!firstPublic.equal(candidatePublic)
                    || !firstProjection.equal(candidateProjection)) {
                throw std::logic_error("sampled hidden future changed the public battle observation");
            }
            if (hiddenDrawOrder(candidate) != firstDrawOrder) {
                foundDistinctHiddenFuture = true;
                distinctParticleIndex = particleIndex;
                break;
            }
        }
        if (!foundDistinctHiddenFuture) {
            throw std::runtime_error("could not find a distinct hidden draw order in 64 deterministic particles");
        }

        const auto rootCandidates = rootProjection["candidate_actions"].cast<dict>()["value"].cast<list>();
        nlohmann::json report = {
            {"schema_id", "issue19-public-boundary-smoke-v1"},
            {"native_base_commit", "ab2b11bc3b5b6c6b68d9d855bc9545e9aca62a28"},
            {"character", "IRONCLAD"},
            {"seed", seed},
            {"ascension", ascension},
            {"diagnostic_prelude", "first legal native action at each public screen; mechanics probe only"},
            {"prelude_steps_to_battle", preludeSteps},
            {"noncombat_screen_samples", noncombatScreenSamples},
            {"battle_fidelity", "supported"},
            {"public_projection_schema", rootProjection["schema_id"].cast<std::string>()},
            {"battle_candidate_count", rootCandidates.size()},
            {"candidate_order_matches_legal_actions", true},
            {"opaque_action_bits_in_public_candidates", false},
            {"distinct_hidden_future_found", true},
            {"distinct_particle_index", distinctParticleIndex},
            {"public_battle_state_equal", true},
            {"public_projection_equal", true},
            {"outcome_claim", "none"},
            {"interpretation", "battle public projection is invariant across sampled hidden draw orders; noncombat screen payload gaps are evaluated separately"}
        };
        std::cout << report.dump(2) << '\n';
        std::cout << "ISSUE19_PUBLIC_BOUNDARY_PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "ISSUE19_PUBLIC_BOUNDARY_FAIL: " << error.what() << '\n';
        return 1;
    }
}
