#include "../bindings/slaythespire.cpp"

#include <pybind11/embed.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace pybind11;

dict projection(StepSimulator &simulator) {
    return simulator.publicProjection();
}

dict screenPayload(const dict &projectionValue) {
    const auto field = projectionValue["screen_payload"].cast<dict>();
    if (field["availability"].cast<std::string>() != "available") {
        throw std::logic_error("screen coverage metadata is unavailable");
    }
    return field["value"].cast<dict>();
}

std::vector<LightSpeedAction> checkCandidateParity(
        StepSimulator &simulator,
        const dict &projectionValue) {
    const auto candidateField = projectionValue["candidate_actions"].cast<dict>();
    const auto candidates = candidateField["value"].cast<list>();
    const auto actions = simulator.legalActions();
    if (candidates.size() != static_cast<ssize_t>(actions.size())) {
        throw std::logic_error("candidate list count differs from native legal actions");
    }
    for (std::size_t index = 0; index < actions.size(); ++index) {
        const auto candidate = candidates[index].cast<dict>();
        if (candidate.contains("bits")
                || candidate["label"].cast<std::string>().find("bits=") != std::string::npos) {
            throw std::logic_error("opaque native action bits reached public candidates");
        }
        if (!candidate.equal(publicProjectionActionSnapshot(actions[index]))) {
            throw std::logic_error("candidate identity/order differs from native legal actions");
        }
    }
    return actions;
}

void checkChoiceAssociations(const list &choices, const list &candidates) {
    std::set<std::size_t> associated;
    for (const auto &choiceHandle : choices) {
        const auto choice = choiceHandle.cast<dict>();
        const auto candidateIndex = choice["candidate_index"].cast<std::size_t>();
        if (candidateIndex >= static_cast<std::size_t>(candidates.size())) {
            throw std::logic_error("choice references a candidate outside the ordered list");
        }
        if (!associated.insert(candidateIndex).second) {
            throw std::logic_error("one ordered candidate is associated with multiple choices");
        }
        const auto publicCandidate = choice["candidate_action"].cast<dict>();
        const auto orderedCandidate = candidates[static_cast<ssize_t>(candidateIndex)].cast<dict>();
        if (publicCandidate.contains("bits")
                || publicCandidate["label"].cast<std::string>().find("bits=") != std::string::npos
                || !publicCandidate.equal(orderedCandidate)) {
            throw std::logic_error("choice binding is not the exact sanitized candidate");
        }
    }
}

dict withDifferentHiddenFuture(const StepSimulator &simulator) {
    auto hiddenFuture = simulator;
    hiddenFuture.gc.seed ^= 0x22002026ULL;
    hiddenFuture.gc.eventRng = sts::Random(0x22012026ULL);
    hiddenFuture.gc.monsterRng = sts::Random(0x22022026ULL);
    hiddenFuture.gc.cardRng = sts::Random(0x22032026ULL);
    hiddenFuture.gc.relicRng = sts::Random(0x22042026ULL);
    hiddenFuture.gc.secondBoss = MonsterEncounter::AWAKENED_ONE;
    return projection(hiddenFuture);
}

void checkHiddenFutureInvariance(StepSimulator &simulator, const dict &baseline) {
    if (!baseline.equal(withDifferentHiddenFuture(simulator))) {
        throw std::logic_error("noncombat public projection changed with hidden future state");
    }
}

void advanceFirstLegal(StepSimulator &simulator) {
    const auto actions = simulator.legalActions();
    if (actions.empty()) {
        throw std::logic_error("fixture reached a nonterminal state without legal actions");
    }
    (void) simulator.step(actions.front());
}

void advanceToMap(StepSimulator &simulator, const int maxSteps) {
    for (int index = 0; index < maxSteps; ++index) {
        if (simulator.gc.screenState == ScreenState::MAP_SCREEN) {
            return;
        }
        advanceFirstLegal(simulator);
    }
    throw std::logic_error("fixed seed did not reach MAP_SCREEN within the bounded path");
}

std::string coverageStatus(const dict &projectionValue) {
    return screenPayload(projectionValue)["coverage_status"].cast<std::string>();
}

}  // namespace

int main() {
    try {
        scoped_interpreter interpreter{};
        constexpr int ascension = 20;
        constexpr std::uint64_t firstSeed = 49;
        constexpr std::uint64_t secondSeed = 50;
        nlohmann::json evidence = {
            {"schema_id", "native-public-noncombat-projection-smoke-v1"},
            {"native_base_commit", "ab2b11bc3b5b6c6b68d9d855bc9545e9aca62a28"},
            {"character", "IRONCLAD"},
            {"ascension", ascension},
            {"seeds", nlohmann::json::array({firstSeed, secondSeed})},
            {"outcome_claim", "none"}
        };

        StepSimulator run(CharacterClass::IRONCLAD, firstSeed, ascension);
        auto eventProjection = projection(run);
        if (eventProjection["schema_id"].cast<std::string>()
                != "native-public-projection-v3") {
            throw std::logic_error("public projection schema was not advanced to v3");
        }
        const auto eventActions = checkCandidateParity(run, eventProjection);
        const auto event = screenPayload(eventProjection);
        if (event["coverage_status"].cast<std::string>() != "supported") {
            throw std::logic_error("seed 49 Neow screen is not faithfully described");
        }
        if (!event.contains("event_phase")
                || event["event_phase"].cast<dict>()["availability"].cast<std::string>()
                        != "not_applicable") {
            throw std::logic_error("Neow screen did not state that no separate phase is tracked");
        }
        const auto eventChoices = event["choices"].cast<list>();
        const auto eventCandidates = eventProjection["candidate_actions"].cast<dict>()
                ["value"].cast<list>();
        checkChoiceAssociations(eventChoices, eventCandidates);
        if (event["event_identity"].cast<std::string>().empty()) {
            throw std::logic_error("Event identity is missing");
        }
        nlohmann::json neowDescriptions = nlohmann::json::array();
        for (const auto &choiceHandle : eventChoices) {
            const auto choice = choiceHandle.cast<dict>();
            if (!choice.contains("description")
                    || choice["description"].cast<std::string>().empty()) {
                throw std::logic_error("Neow option lacks its visible description");
            }
            neowDescriptions.push_back(choice["description"].cast<std::string>());
        }
        evidence["neow"] = {
            {"coverage_status", "supported"},
            {"event_identity", event["event_identity"].cast<std::string>()},
            {"choice_count", eventChoices.size()},
            {"visible_options", neowDescriptions},
            {"candidate_order_matches_legal_actions", true}
        };
        checkHiddenFutureInvariance(run, eventProjection);

        StepSimulator alternateNeow(CharacterClass::IRONCLAD, secondSeed, ascension);
        const auto alternateNeowProjection = projection(alternateNeow);
        const auto alternateNeowActions = checkCandidateParity(alternateNeow, alternateNeowProjection);
        const auto alternateNeowPayload = screenPayload(alternateNeowProjection);
        if (alternateNeowPayload["coverage_status"].cast<std::string>() != "supported") {
            throw std::logic_error("seed 50 Neow screen is not faithfully described");
        }
        const auto alternateNeowChoices = alternateNeowPayload["choices"].cast<list>();
        checkChoiceAssociations(
                alternateNeowChoices,
                alternateNeowProjection["candidate_actions"].cast<dict>()["value"].cast<list>());
        nlohmann::json alternateNeowDescriptions = nlohmann::json::array();
        for (const auto &choiceHandle : alternateNeowChoices) {
            const auto choice = choiceHandle.cast<dict>();
            alternateNeowDescriptions.push_back(choice["description"].cast<std::string>());
        }
        if (eventChoices.equal(alternateNeowChoices)) {
            throw std::logic_error("different native Neow seed did not change visible option content");
        }
        checkHiddenFutureInvariance(alternateNeow, alternateNeowProjection);
        evidence["alternate_neow"] = {
            {"coverage_status", "supported"},
            {"choice_count", alternateNeowChoices.size()},
            {"visible_options", alternateNeowDescriptions},
            {"visible_choices_differ_from_seed_49", true},
            {"candidate_order_matches_legal_actions", true}
        };
        (void) alternateNeowActions;

        // An unsupported event family must fail closed before any option text
        // is emitted. Match and Keep's console renderer includes hidden faces.
        StepSimulator unsupportedEvent(CharacterClass::IRONCLAD, firstSeed, ascension);
        unsupportedEvent.gc.screenState = ScreenState::EVENT_SCREEN;
        unsupportedEvent.gc.curEvent = Event::MATCH_AND_KEEP;
        const auto unsupportedProjection = projection(unsupportedEvent);
        const auto unsupportedPayload = screenPayload(unsupportedProjection);
        if (unsupportedPayload["coverage_status"].cast<std::string>() != "unsupported"
                || unsupportedPayload["reason"].cast<std::string>().find("unrevealed")
                        == std::string::npos) {
            throw std::logic_error("unsupported hidden-face Event did not fail closed");
        }
        evidence["match_and_keep"] = {
            {"coverage_status", "unsupported"},
            {"reason", unsupportedPayload["reason"].cast<std::string>()}
        };

        StepSimulator phasedEvent(CharacterClass::IRONCLAD, firstSeed, ascension);
        phasedEvent.gc.screenState = ScreenState::EVENT_SCREEN;
        phasedEvent.gc.curEvent = Event::DEAD_ADVENTURER;
        phasedEvent.gc.info.phase = 2;
        phasedEvent.gc.info.eventData = 0;
        const auto phasedEventProjection = projection(phasedEvent);
        const auto phasedEventActions = checkCandidateParity(phasedEvent, phasedEventProjection);
        const auto phasedEventPayload = screenPayload(phasedEventProjection);
        const auto phaseField = phasedEventPayload["event_phase"].cast<dict>();
        if (phasedEventPayload["coverage_status"].cast<std::string>() != "supported"
                || phaseField["availability"].cast<std::string>() != "available"
                || phaseField["value"].cast<int>() != 2) {
            throw std::logic_error("Dead Adventurer public phase does not use the simulator's visible phase field");
        }
        checkChoiceAssociations(
                phasedEventPayload["choices"].cast<list>(),
                phasedEventProjection["candidate_actions"].cast<dict>()["value"].cast<list>());
        checkHiddenFutureInvariance(phasedEvent, phasedEventProjection);
        evidence["phased_event"] = {
            {"event_identity", phasedEventPayload["event_identity"].cast<std::string>()},
            {"event_phase", phaseField["value"].cast<int>()},
            {"coverage_status", "supported"},
            {"candidate_order_matches_legal_actions", true}
        };
        (void) phasedEventActions;

        StepSimulator unsupportedPhase(CharacterClass::IRONCLAD, firstSeed, ascension);
        unsupportedPhase.gc.screenState = ScreenState::EVENT_SCREEN;
        unsupportedPhase.gc.curEvent = Event::CURSED_TOME;
        unsupportedPhase.gc.info.eventData = 5;
        const auto unsupportedPhasePayload = screenPayload(projection(unsupportedPhase));
        if (unsupportedPhasePayload["coverage_status"].cast<std::string>() != "unsupported"
                || unsupportedPhasePayload["reason"].cast<std::string>().find("phase")
                        == std::string::npos) {
            throw std::logic_error("out-of-range Cursed Tome phase did not fail closed");
        }
        evidence["exceptional_event_phase"] = {
            {"event_identity", unsupportedPhasePayload["event_identity"].cast<std::string>()},
            {"event_phase", 5},
            {"coverage_status", "unsupported"},
            {"reason", unsupportedPhasePayload["reason"].cast<std::string>()}
        };

        // Follow the actual seed-49 path through its presented reward options.
        advanceFirstLegal(run);
        if (run.gc.screenState != ScreenState::REWARDS) {
            throw std::logic_error("seed 49 Neow action did not reach a reward screen");
        }
        const auto rewardProjection = projection(run);
        const auto rewardActions = checkCandidateParity(run, rewardProjection);
        const auto reward = screenPayload(rewardProjection);
        if (reward["coverage_status"].cast<std::string>() != "supported") {
            throw std::logic_error("seed 49 reward offers are not fully described");
        }
        const auto rewardChoices = reward["choices"].cast<list>();
        const auto rewardCandidates = rewardProjection["candidate_actions"].cast<dict>()
                ["value"].cast<list>();
        checkChoiceAssociations(rewardChoices, rewardCandidates);
        bool sawCardOffer = false;
        nlohmann::json rewardTypes = nlohmann::json::array();
        nlohmann::json cardOffers = nlohmann::json::array();
        nlohmann::json goldOffers = nlohmann::json::array();
        for (const auto &choiceHandle : rewardChoices) {
            const auto choice = choiceHandle.cast<dict>();
            const auto type = choice["choice_type"].cast<std::string>();
            rewardTypes.push_back(type);
            if (choice.contains("card")) {
                sawCardOffer = true;
                const auto card = choice["card"].cast<dict>();
                if (!card.contains("id") || !card.contains("id_label")
                        || !card.contains("name") || !card.contains("type")
                        || !card.contains("rarity") || !card.contains("upgraded")) {
                    throw std::logic_error("card reward omitted its public identity/upgrade fields");
                }
                cardOffers.push_back({
                    {"id", card["id"].cast<int>()},
                    {"id_label", card["id_label"].cast<std::string>()},
                    {"name", card["name"].cast<std::string>()},
                    {"type", card["type"].cast<std::string>()},
                    {"rarity", card["rarity"].cast<std::string>()},
                    {"upgraded", card["upgraded"].cast<bool>()}
                });
            }
            if (choice.contains("gold")) {
                goldOffers.push_back({
                    {"candidate_index", choice["candidate_index"].cast<std::size_t>()},
                    {"action_index", choice["candidate_action"].cast<dict>()["idx1"].cast<int>()},
                    {"gold", choice["gold"].cast<int>()}
                });
            }
        }
        if (!sawCardOffer) {
            throw std::logic_error("seed 49 reward fixture did not expose a card offer");
        }
        evidence["reward_screen"] = {
            {"coverage_status", "supported"},
            {"choice_count", rewardChoices.size()},
            {"choice_types", rewardTypes},
            {"card_offers", cardOffers},
            {"gold_offers", goldOffers},
            {"actual_card_contents_exposed", true},
            {"candidate_order_matches_legal_actions", true}
        };
        checkHiddenFutureInvariance(run, rewardProjection);
        (void) rewardActions;

        // The reward enumerator must preserve each native indexed gold offer;
        // this focused fixture guards the two-entry Rewards container path.
        StepSimulator indexedGold(CharacterClass::IRONCLAD, firstSeed, ascension);
        indexedGold.gc.screenState = ScreenState::REWARDS;
        indexedGold.gc.info.rewardsContainer.clear();
        indexedGold.gc.info.rewardsContainer.addGold(17);
        indexedGold.gc.info.rewardsContainer.addGold(41);
        indexedGold.gc.info.rewardsContainer.addRelic(RelicId::ANCHOR);
        indexedGold.gc.info.rewardsContainer.addPotion(Potion::FIRE_POTION);
        indexedGold.gc.info.rewardsContainer.sapphireKey = true;
        const auto indexedGoldProjection = projection(indexedGold);
        const auto indexedGoldActions = checkCandidateParity(indexedGold, indexedGoldProjection);
        const auto indexedGoldPayload = screenPayload(indexedGoldProjection);
        if (indexedGoldPayload["coverage_status"].cast<std::string>() != "supported") {
            throw std::logic_error("indexed gold reward fixture is not supported");
        }
        const auto indexedGoldChoices = indexedGoldPayload["choices"].cast<list>();
        checkChoiceAssociations(
                indexedGoldChoices,
                indexedGoldProjection["candidate_actions"].cast<dict>()["value"].cast<list>());
        int indexedGoldChoiceCount = 0;
        bool sawRelic = false;
        bool sawPotion = false;
        bool sawSapphireKey = false;
        for (const auto &choiceHandle : indexedGoldChoices) {
            const auto choice = choiceHandle.cast<dict>();
            const auto type = choice["choice_type"].cast<std::string>();
            if (type == "relic") {
                const auto relic = choice["relic"].cast<dict>();
                sawRelic = relic["id"].cast<int>() == static_cast<int>(RelicId::ANCHOR)
                        && !relic["id_label"].cast<std::string>().empty()
                        && relic["name"].cast<std::string>() == getRelicName(RelicId::ANCHOR)
                        && choice["sapphire_key_is_lost"].cast<bool>();
            } else if (type == "potion") {
                const auto potion = choice["potion"].cast<dict>();
                sawPotion = potion["id"].cast<int>() == static_cast<int>(Potion::FIRE_POTION)
                        && !potion["id_label"].cast<std::string>().empty()
                        && potion["name"].cast<std::string>() == potionLabel(Potion::FIRE_POTION);
            } else if (type == "key") {
                sawSapphireKey = choice["key"].cast<std::string>() == "SAPPHIRE_KEY"
                        && choice["relic_removed_with_key"].cast<dict>()
                                ["id"].cast<int>() == static_cast<int>(RelicId::ANCHOR);
            }
            if (type != "gold") {
                continue;
            }
            const auto candidateAction = choice["candidate_action"].cast<dict>();
            const int actionIndex = candidateAction["idx1"].cast<int>();
            const int expectedValue = actionIndex == 0 ? 17 : actionIndex == 1 ? 41 : -1;
            if (choice["gold"].cast<int>() != expectedValue) {
                throw std::logic_error("gold reward value does not match its indexed legal action");
            }
            ++indexedGoldChoiceCount;
        }
        if (indexedGoldChoiceCount != 2 || indexedGoldActions.size() != 6
                || !sawRelic || !sawPotion || !sawSapphireKey) {
            throw std::logic_error("native reward fixture mismatch: gold="
                    + std::to_string(indexedGoldChoiceCount)
                    + " candidates=" + std::to_string(indexedGoldActions.size())
                    + " relic=" + std::to_string(sawRelic)
                    + " potion=" + std::to_string(sawPotion)
                    + " key=" + std::to_string(sawSapphireKey));
        }
        const int initialGold = indexedGold.gc.gold;
        auto firstGoldExecution = indexedGold;
        auto secondGoldExecution = indexedGold;
        (void) firstGoldExecution.step(indexedGoldActions[0]);
        (void) secondGoldExecution.step(indexedGoldActions[1]);
        if (firstGoldExecution.gc.gold != initialGold + 17
                || secondGoldExecution.gc.gold != initialGold + 41) {
            throw std::logic_error("indexed gold actions do not execute their associated visible amounts");
        }
        evidence["indexed_gold_regression"] = {
            {"fixture", "native Rewards container with distinct indexed gold, relic, potion, and sapphire key entries"},
            {"visible_amounts", {17, 41}},
            {"relic", getRelicName(RelicId::ANCHOR)},
            {"potion", potionLabel(Potion::FIRE_POTION)},
            {"key", "SAPPHIRE_KEY; removes final relic offer"},
            {"legal_candidate_count_including_skip", indexedGoldActions.size()},
            {"both_candidate_mappings_execute_exact_amount", true}
        };

        // The current map is public, but hidden event/RNG values and the Act 3
        // second-boss selector must not enter its projection.
        advanceToMap(run, 8);
        const auto mapProjection = projection(run);
        const auto mapActions = checkCandidateParity(run, mapProjection);
        const auto mapField = mapProjection["visible_map_graph"].cast<dict>();
        const auto graph = mapField["value"].cast<list>();
        const auto routeField = mapProjection["immediately_legal_routes"].cast<dict>();
        if (routeField["availability"].cast<std::string>() != "available") {
            throw std::logic_error("MAP_SCREEN legal routes are unavailable");
        }
        const auto routes = routeField["value"].cast<list>();
        const auto screen = screenPayload(mapProjection);
        if (screen["coverage_status"].cast<std::string>() != "partial") {
            throw std::logic_error("Map screen must state its unrevealed boss limitation");
        }
        if (routes.size() < 2) {
            throw std::logic_error("native Map screen did not expose multiple legal route branches");
        }
        checkChoiceAssociations(routes, mapProjection["candidate_actions"].cast<dict>()
                ["value"].cast<list>());
        bool sawUnknownRoom = false;
        std::set<std::pair<int, int>> graphCoordinates;
        for (const auto &nodeHandle : graph) {
            const auto node = nodeHandle.cast<dict>();
            if (!node.contains("x") || !node.contains("y")
                    || !node.contains("outgoing_edges")) {
                throw std::logic_error("public map node omitted coordinates or outgoing edges");
            }
            graphCoordinates.emplace(node["x"].cast<int>(), node["y"].cast<int>());
            sawUnknownRoom |= node["room_symbol"].cast<std::string>() == "?";
            if (node.contains("room") || node.contains("event_identity")) {
                throw std::logic_error("map graph exposed private room/event identity");
            }
        }
        for (const auto &nodeHandle : graph) {
            const auto node = nodeHandle.cast<dict>();
            const auto edges = node["outgoing_edges"].cast<list>();
            for (const auto &edgeHandle : edges) {
                const auto edge = edgeHandle.cast<dict>();
                const auto coordinate = std::make_pair(edge["x"].cast<int>(), edge["y"].cast<int>());
                if (coordinate.second == 15 && node["y"].cast<int>() == 14) {
                    continue;  // The Act boss entry is displayed outside the 15 map rows.
                }
                if (graphCoordinates.find(coordinate) == graphCoordinates.end()) {
                    throw std::logic_error("public map edge points outside the visible graph");
                }
            }
        }
        if (!sawUnknownRoom) {
            throw std::logic_error("public map fixture did not retain hidden rooms as '?' symbols");
        }
        for (const auto &routeHandle : routes) {
            const auto route = routeHandle.cast<dict>();
            const auto destination = route["destination"].cast<dict>();
            const auto location = destination["location"].cast<std::string>();
            if (location == "map_node") {
                const auto coordinate = std::make_pair(
                        destination["x"].cast<int>(), destination["y"].cast<int>());
                if (graphCoordinates.find(coordinate) == graphCoordinates.end()) {
                    throw std::logic_error("legal route destination is absent from the visible map graph");
                }
            } else if (location != "boss_entry") {
                throw std::logic_error("legal route has an unknown public destination location");
            }
        }
        const auto currentNode = mapProjection["current_map_node"].cast<dict>()
                ["value"].cast<dict>();
        if (currentNode["location"].cast<std::string>() != "before_first_route") {
            throw std::logic_error("seed 49 starting map location was not exposed faithfully");
        }
        if (mapProjection["visible_act_boss"].cast<dict>()
                ["availability"].cast<std::string>() != "unavailable") {
            throw std::logic_error("untracked boss identity was exposed from hidden simulator state");
        }
        evidence["map_screen"] = {
            {"coverage_status", "partial"},
            {"visible_node_count", graph.size()},
            {"legal_route_count", routes.size()},
            {"contains_unknown_room_symbol", true},
            {"candidate_route_bindings_valid", true},
            {"act_boss_identity", "unavailable; reveal state is not tracked"},
            {"candidate_order_matches_legal_actions", true}
        };
        checkHiddenFutureInvariance(run, mapProjection);

        // Seed 49's first-legal diagnostic reaches Battle; keep the pre-existing
        // public Battle schema and hidden-future invariant unchanged.
        advanceToMap(run, 8);
        auto mapActionsAtRoot = run.legalActions();
        (void) run.step(mapActionsAtRoot.front());
        while (run.gc.screenState != ScreenState::BATTLE) {
            advanceFirstLegal(run);
        }
        const auto battleProjection = projection(run);
        const auto battleState = run.publicBattleState();
        auto firstHiddenFuture = run.samplePublicConsistentHiddenFuture(0x20A2026ULL, 0);
        auto secondHiddenFuture = run.samplePublicConsistentHiddenFuture(0x20A2026ULL, 1);
        if (!battleState.equal(firstHiddenFuture.publicBattleState())
                || !battleState.equal(secondHiddenFuture.publicBattleState())
                || !battleProjection.equal(projection(firstHiddenFuture))
                || !battleProjection.equal(projection(secondHiddenFuture))) {
            throw std::logic_error("noncombat projection changed Battle public-state semantics");
        }
        evidence["battle_unchanged"] = {
            {"public_battle_state_equal", true},
            {"projection_equal_across_hidden_futures", true},
            {"candidate_order_matches_legal_actions",
                checkCandidateParity(run, battleProjection).size()
                    == battleProjection["candidate_actions"].cast<dict>()["value"].cast<list>().size()}
        };

        pybind11::list unsupportedScreens;
        for (const auto unsupported : {
                ScreenState::BOSS_RELIC_REWARDS,
                ScreenState::CARD_SELECT,
                ScreenState::TREASURE_ROOM,
                ScreenState::REST_ROOM,
                ScreenState::SHOP_ROOM}) {
            StepSimulator screenFixture(CharacterClass::IRONCLAD, firstSeed, ascension);
            screenFixture.gc.screenState = unsupported;
            const auto unsupportedScreen = screenPayload(projection(screenFixture));
            if (unsupportedScreen["coverage_status"].cast<std::string>() != "unsupported"
                    || !unsupportedScreen.contains("reason")) {
                throw std::logic_error("unsupported noncombat screen lacks explicit coverage reason");
            }
            unsupportedScreens.append(screenStateLabel(unsupported));
        }
        evidence["other_screen_coverage"] = {
            {"status", "explicitly_unsupported"},
            {"screens", unsupportedScreens.size()}
        };

        std::cout << evidence.dump(2) << '\n';
        std::cout << "PUBLIC_NONCOMBAT_PROJECTION_PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "PUBLIC_NONCOMBAT_PROJECTION_FAIL: " << error.what() << '\n';
        return 1;
    }
}
