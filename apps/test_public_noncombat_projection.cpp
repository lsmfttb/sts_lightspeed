#include "../bindings/slaythespire.cpp"

#include <pybind11/embed.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef STS_LIGHTSPEED_TESTED_SOURCE_COMMIT
#define STS_LIGHTSPEED_TESTED_SOURCE_COMMIT "unknown"
#endif

namespace {

using namespace pybind11;

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::logic_error(message);
    }
}

dict projection(StepSimulator &simulator) {
    return simulator.publicProjectionV3();
}

dict screenPayload(const dict &projectionValue) {
    const auto field = projectionValue["screen_payload"].cast<dict>();
    require(field["availability"].cast<std::string>() == "available",
            "screen payload availability is missing");
    return field["value"].cast<dict>();
}

std::string encodePython(const object &value) {
    return module_::import("json").attr("dumps")(
            value, arg("sort_keys") = true).cast<std::string>();
}

void verifyJsonRoundTrip(const dict &value) {
    auto json = module_::import("json");
    const auto encoded = json.attr("dumps")(value).cast<std::string>();
    const auto decoded = json.attr("loads")(encoded).cast<dict>();
    require(value.equal(decoded), "public projection changed through JSON serialization");
}

nlohmann::json checkStudyAdapterBoundary(
        StepSimulator &simulator,
        const dict &nativeProjection,
        const std::vector<LightSpeedAction> &actions,
        const std::size_t selectedIndex);

std::vector<LightSpeedAction> checkCandidateParity(
        StepSimulator &simulator,
        const dict &projectionValue) {
    const auto candidateField = projectionValue["candidate_actions"].cast<dict>();
    require(candidateField["availability"].cast<std::string>() == "available",
            "candidate action availability is missing");
    const auto candidates = candidateField["value"].cast<list>();
    const auto actions = simulator.legalActions();
    require(candidates.size() == static_cast<ssize_t>(actions.size()),
            "candidate count differs from native legal actions");
    std::set<std::string> identities;
    for (std::size_t index = 0; index < actions.size(); ++index) {
        const auto candidate = candidates[index].cast<dict>();
        require(!candidate.contains("bits"), "opaque action bits reached public candidates");
        require(candidate["label"].cast<std::string>().find("bits=") == std::string::npos,
                "opaque action bits reached a public action label");
        require(candidate.equal(publicChoiceActionSnapshot(actions[index])),
                "candidate identity/order differs from native legal actions");
        const auto identity = encodePython(candidate);
        require(identities.insert(identity).second,
                "multiple native actions share one public candidate identity");
    }
    verifyJsonRoundTrip(projectionValue);
    return actions;
}

void checkChoiceAssociations(const list &choices, const list &candidates) {
    std::set<std::size_t> associated;
    for (const auto &choiceHandle : choices) {
        const auto choice = choiceHandle.cast<dict>();
        const auto candidateIndex = choice["candidate_index"].cast<std::size_t>();
        require(candidateIndex < static_cast<std::size_t>(candidates.size()),
                "choice references a candidate outside the ordered list");
        require(associated.insert(candidateIndex).second,
                "one ordered candidate is associated with multiple choices");
        const auto publicCandidate = choice["candidate_action"].cast<dict>();
        const auto orderedCandidate = candidates[static_cast<ssize_t>(candidateIndex)].cast<dict>();
        require(!publicCandidate.contains("bits")
                        && publicCandidate["label"].cast<std::string>().find("bits=")
                                == std::string::npos
                        && publicCandidate.equal(orderedCandidate),
                "choice binding differs from its exact sanitized candidate");
    }
    require(associated.size() == static_cast<std::size_t>(candidates.size()),
            "some legal candidates have no public choice description");
}

void advanceFirstLegal(StepSimulator &simulator) {
    const auto actions = simulator.legalActions();
    require(!actions.empty(), "nonterminal state has no legal actions");
    (void) simulator.step(actions.front());
}

void advanceTo(StepSimulator &simulator, const ScreenState target, const int maxSteps) {
    for (int index = 0; index < maxSteps; ++index) {
        if (simulator.gc.screenState == target) {
            return;
        }
        advanceFirstLegal(simulator);
    }
    require(simulator.gc.screenState == target,
            "fixed seed did not reach requested screen within the bound");
}

std::string coverage(const dict &projectionValue) {
    return screenPayload(projectionValue)["coverage_status"].cast<std::string>();
}

void checkControlledVisibleAlias(nlohmann::json &evidence) {
    StepSimulator first(CharacterClass::IRONCLAD, 49, 20);
    StepSimulator second(CharacterClass::IRONCLAD, 49, 20);
    for (auto *sim : {&first, &second}) {
        sim->gc.screenState = ScreenState::REWARDS;
        sim->gc.info.rewardsContainer.clear();
    }
    first.gc.info.rewardsContainer.addGold(17);
    second.gc.info.rewardsContainer.addGold(41);
    require(first.observation() == second.observation(),
            "controlled legacy 412-feature observations unexpectedly differ");
    const auto firstProjection = projection(first);
    const auto secondProjection = projection(second);
    require(!firstProjection.equal(secondProjection),
            "visible reward difference did not change the public choice projection");
    require(coverage(firstProjection) == "supported"
                    && coverage(secondProjection) == "supported",
            "visible-alias witness is not fully rendered");
    const auto firstActions = checkCandidateParity(first, firstProjection);
    const auto secondActions = checkCandidateParity(second, secondProjection);
    const auto firstPolicyPath = checkStudyAdapterBoundary(
            first, firstProjection, firstActions, 0);
    const auto secondPolicyPath = checkStudyAdapterBoundary(
            second, secondProjection, secondActions, 0);
    const auto firstPolicyInput = firstPolicyPath.at("serialized_policy_input");
    const auto secondPolicyInput = secondPolicyPath.at("serialized_policy_input");
    require(firstPolicyInput != secondPolicyInput,
            "adapter policy inputs did not distinguish different visible gold offers");
    const auto firstGold = firstPolicyInput.at("legal_options").at(0)
            .at("semantics").at("gold").get<int>();
    const auto secondGold = secondPolicyInput.at("legal_options").at(0)
            .at("semantics").at("gold").get<int>();
    require(firstGold == 17 && secondGold == 41,
            "adapter gold semantics do not match the associated visible offers");
    evidence["visible_alias_witness"] = {
        {"legacy_observation", "native-412-feature-NNInterface::getObservation"},
        {"legacy_observations_equal", true},
        {"visible_gold_options", {17, 41}},
        {"new_policy_inputs_differ", true},
        {"policy_input_gold_options", {firstGold, secondGold}},
        {"selected_native_actions_executed", true},
        {"structured_public_projection_differs", true},
        {"scope", "controlled native witness; no old 4737+92 model schema is shipped here"}
    };
}

void checkControlledVisibleRouteAlias(nlohmann::json &evidence) {
    StepSimulator first(CharacterClass::IRONCLAD, 49, 20);
    StepSimulator second(CharacterClass::IRONCLAD, 49, 20);
    first.gc.screenState = ScreenState::MAP_SCREEN;
    second.gc.screenState = ScreenState::MAP_SCREEN;
    require(first.gc.map != nullptr && second.gc.map != nullptr,
            "controlled route fixture has no initialized native map");
    const auto firstNativeActions = first.legalActions();
    const auto secondNativeActions = second.legalActions();
    require(firstNativeActions.size() == secondNativeActions.size(),
            "same-seed route fixtures produced different legal-action counts");
    std::size_t selectedRouteIndex = firstNativeActions.size();
    for (std::size_t index = 0; index < firstNativeActions.size(); ++index) {
        if (firstNativeActions[index].kind == "map") {
            selectedRouteIndex = index;
            break;
        }
    }
    require(selectedRouteIndex < firstNativeActions.size(),
            "controlled route fixture has no legal map destination");
    const auto &firstRoute = firstNativeActions[selectedRouteIndex];
    const auto &secondRoute = secondNativeActions[selectedRouteIndex];
    require(firstRoute.kind == "map" && secondRoute.kind == "map"
                    && firstRoute.idx1 == secondRoute.idx1,
            "same-seed route fixtures do not share the selected native route");
    const int destinationY = first.gc.curMapNodeY + 1;
    const int destinationX = firstRoute.idx1;
    require(destinationY >= 0 && destinationY < 15,
            "controlled route fixture selected an invalid destination row");
    first.gc.map->getNode(destinationX, destinationY).room = Room::SHOP;
    second.gc.map->getNode(destinationX, destinationY).room = Room::REST;
    require(first.observation() == second.observation(),
            "controlled map-room pair unexpectedly changed the legacy observation");

    const auto firstProjection = projection(first);
    const auto secondProjection = projection(second);
    require(coverage(firstProjection) == "supported"
                    && coverage(secondProjection) == "supported",
            "controlled visible route fixture is not fully rendered");
    require(!firstProjection.equal(secondProjection),
            "visible route difference did not change the public projection");
    const auto firstActions = checkCandidateParity(first, firstProjection);
    const auto secondActions = checkCandidateParity(second, secondProjection);
    const auto firstPath = checkStudyAdapterBoundary(
            first, firstProjection, firstActions, selectedRouteIndex);
    const auto secondPath = checkStudyAdapterBoundary(
            second, secondProjection, secondActions, selectedRouteIndex);
    const auto firstInput = firstPath.at("serialized_policy_input");
    const auto secondInput = secondPath.at("serialized_policy_input");
    require(firstInput != secondInput,
            "adapter policy inputs did not distinguish visible route rooms");
    const auto firstSymbol = firstInput.at("legal_options").at(selectedRouteIndex)
            .at("semantics").at("destination").at("room_symbol").get<std::string>();
    const auto secondSymbol = secondInput.at("legal_options").at(selectedRouteIndex)
            .at("semantics").at("destination").at("room_symbol").get<std::string>();
    require(firstSymbol == "$" && secondSymbol == "R",
            "adapter route options do not preserve their visible room symbols");
    evidence["visible_route_alias_witness"] = {
        {"legacy_observation", "native-412-feature-NNInterface::getObservation"},
        {"legacy_observations_equal", true},
        {"visible_route_symbols", {firstSymbol, secondSymbol}},
        {"new_policy_inputs_differ", true},
        {"selected_native_actions_executed", true},
        {"scope", "controlled native map fixture; no full frozen T089 vector equality claim"}
    };
}

void checkMapHeldPotionActions(nlohmann::json &evidence) {
    const auto verifyPotionAction = [&](const Potion potion, const std::string &choiceType) {
        StepSimulator simulator(CharacterClass::IRONCLAD, 49, 20);
        advanceTo(simulator, ScreenState::MAP_SCREEN, 16);
        require(simulator.gc.map != nullptr,
                "held-potion MAP fixture has no initialized native map");
        if (potion == Potion::BLOOD_POTION) {
            simulator.gc.curHp = simulator.gc.curHp > 10
                    ? simulator.gc.curHp - 10 : 1;
        }
        simulator.gc.obtainPotion(potion);
        const auto nativeProjection = projection(simulator);
        require(coverage(nativeProjection) == "supported",
                "held-potion MAP screen was not marked supported");
        const auto actions = checkCandidateParity(simulator, nativeProjection);
        const auto payload = screenPayload(nativeProjection);
        const auto choices = payload["choices"].cast<list>();
        const auto routeField = nativeProjection["immediately_legal_routes"].cast<dict>();
        const auto routes = routeField["value"].cast<list>();
        const auto candidates = nativeProjection["candidate_actions"].cast<dict>()
                ["value"].cast<list>();
        checkChoiceAssociations(choices, candidates);
        require(choices.size() == static_cast<ssize_t>(actions.size())
                        && routes.size() < choices.size(),
                "held-potion MAP choices are not complete or route-specific");

        std::size_t selectedCandidate = actions.size();
        int matchingChoiceCount = 0;
        for (const auto &choiceHandle : choices) {
            const auto choice = choiceHandle.cast<dict>();
            if (choice["choice_type"].cast<std::string>() != choiceType) {
                continue;
            }
            const auto visiblePotion = choice["potion"].cast<dict>();
            if (visiblePotion["id"].cast<int>() != static_cast<int>(potion)) {
                continue;
            }
            selectedCandidate = choice["candidate_index"].cast<std::size_t>();
            ++matchingChoiceCount;
        }
        require(matchingChoiceCount == 1 && selectedCandidate < actions.size(),
                "held-potion MAP action has no unique visible choice association");
        const auto execution = checkStudyAdapterBoundary(
                simulator, nativeProjection, actions, selectedCandidate);
        evidence[potion == Potion::FIRE_POTION ? "fire_potion_discard" : "blood_potion_use"] = {
            {"candidate_count", actions.size()}, {"route_count", routes.size()},
            {"choice_count", choices.size()}, {"selected_candidate_index", selectedCandidate},
            {"adapter_execution", execution}
        };
    };

    verifyPotionAction(Potion::FIRE_POTION, "potion_discard");
    verifyPotionAction(Potion::BLOOD_POTION, "potion_use");
}

void checkHiddenFutureInvariant(StepSimulator &simulator, const dict &baseline) {
    auto changedFuture = simulator;
    changedFuture.gc.seed ^= 0x22002026ULL;
    changedFuture.gc.eventRng = Random(0x22012026ULL);
    changedFuture.gc.monsterRng = Random(0x22022026ULL);
    changedFuture.gc.cardRng = Random(0x22032026ULL);
    changedFuture.gc.relicRng = Random(0x22042026ULL);
    changedFuture.gc.secondBoss = MonsterEncounter::AWAKENED_ONE;
    require(baseline.equal(projection(changedFuture)),
            "public choice projection changed with hidden future fields");
}

list nativeActionMappings(const std::vector<LightSpeedAction> &actions) {
    list result;
    for (const auto &action : actions) {
        dict item;
        item["scope"] = action.scope;
        item["kind"] = action.kind;
        item["idx1"] = action.idx1;
        item["idx2"] = action.idx2;
        item["idx3"] = action.idx3;
        item["label"] = action.label;
        item["bits"] = action.bits;
        result.append(std::move(item));
    }
    return result;
}

nlohmann::json checkStudyAdapterBoundary(
        StepSimulator &simulator,
        const dict &nativeProjection,
        const std::vector<LightSpeedAction> &actions,
        const std::size_t selectedIndex) {
    require(selectedIndex < actions.size(), "adapter integration index is out of range");
    auto adapter = module_::import("adapter");
    const auto nativeMappings = nativeActionMappings(actions);
    const auto prepared = adapter.attr("prepare_policy_input")(
            nativeProjection, nativeMappings);
    const auto serialized = prepared.attr("serialized_input").cast<std::string>();
    require(serialized.find("bits=") == std::string::npos
                    && serialized.find("\"bits\"") == std::string::npos,
            "opaque native action bits reached serialized study input");
    const auto policyInput = nlohmann::json::parse(serialized);
    require(policyInput.at("schema_id") == "noncombat-visible-choice-input-v1",
            "study adapter emitted an unexpected schema");
    const auto callback = cpp_function([selectedIndex](const std::string &onlySerialized) {
        require(onlySerialized.find("bits=") == std::string::npos
                        && onlySerialized.find("\"bits\"") == std::string::npos,
                "policy callback received opaque action bits");
        const auto input = nlohmann::json::parse(onlySerialized);
        require(!input.at("legal_options").empty(), "policy callback received no choices");
        return int_(selectedIndex);
    });
    const auto chosenIndex = adapter.attr("call_policy")(prepared, callback).cast<std::size_t>();
    require(chosenIndex == selectedIndex, "policy callback index changed across the adapter");
    const auto resolved = adapter.attr("resolve_native_action")(
            prepared, chosenIndex, nativeProjection, nativeMappings).cast<dict>();
    require(resolved["bits"].cast<std::uint32_t>() == actions[selectedIndex].bits,
            "adapter did not resolve the selected public index to the exact native action");

    auto execution = simulator;
    const auto option = policyInput.at("legal_options").at(selectedIndex);
    const auto effect = execution.step(actions[chosenIndex]);
    nlohmann::json evidence = {
        {"selected_candidate_index", chosenIndex},
        {"execution_accepted", !effect.empty()}
    };
    if (simulator.gc.screenState == ScreenState::REWARDS) {
        const auto &semantics = option.at("semantics");
        if (semantics.at("choice_type") == "gold") {
            const auto selectedGold = semantics.at("gold").get<int>();
            require(execution.gc.gold == simulator.gc.gold + selectedGold,
                    "resolved reward option did not award its exact visible gold amount");
            evidence["selected_visible_gold"] = selectedGold;
            evidence["selected_gold_awarded"] = true;
        } else if (semantics.at("choice_type") == "card") {
            const auto selectedCardId = semantics.at("card").at("id").get<int>();
            const auto countCard = [selectedCardId](const GameContext &gc) {
                int count = 0;
                for (const auto &cardHandle : deckSnapshot(gc)) {
                    const auto card = cardHandle.cast<dict>();
                    if (card["id"].cast<int>() == selectedCardId) {
                        ++count;
                    }
                }
                return count;
            };
            require(countCard(execution.gc) == countCard(simulator.gc) + 1,
                    "resolved reward option did not add its exact visible card to the deck");
            evidence["selected_visible_card_id"] = selectedCardId;
            evidence["selected_card_awarded_to_deck"] = true;
        } else {
            throw std::logic_error("adapter reward integration selected an untested subtype");
        }
    } else if (simulator.gc.screenState == ScreenState::MAP_SCREEN) {
        const auto &semantics = option.at("semantics");
        const auto choiceType = semantics.at("choice_type").get<std::string>();
        if (choiceType == "map_route") {
            const auto &destination = semantics.at("destination");
            if (destination.at("location") == "map_node") {
                require(execution.gc.curMapNodeX == destination.at("x").get<int>()
                                && execution.gc.curMapNodeY == destination.at("y").get<int>(),
                        "resolved map option did not enter its exact visible destination");
            } else {
                require(destination.at("location") == "boss_entry"
                                && execution.gc.curMapNodeY == 15,
                        "resolved final route did not enter the visible Act boss node");
            }
            evidence["selected_destination"] = destination;
            evidence["selected_route_entered"] = true;
        } else if (choiceType == "potion_use" || choiceType == "potion_discard") {
            const auto &candidate = option.at("action");
            const auto &potion = semantics.at("potion");
            const int potionIndex = candidate.at("idx1").get<int>();
            const auto expectedKind = choiceType == "potion_use"
                    ? "game_potion_use" : "game_potion_discard";
            require(candidate.at("kind") == expectedKind
                            && potion.at("potion_index").get<int>() == potionIndex
                            && potion.at("id").get<int>()
                                    == static_cast<int>(simulator.gc.potions[potionIndex]),
                    "map potion option differs from its exact native action or visible slot");
            require(execution.gc.potionCount < simulator.gc.potionCount,
                    "selected map potion action did not consume its potion");
            if (choiceType == "potion_discard") {
                require(execution.gc.potions[potionIndex] == Potion::EMPTY_POTION_SLOT,
                        "selected map potion discard did not empty its associated slot");
            } else if (simulator.gc.potions[potionIndex] == Potion::BLOOD_POTION) {
                require(execution.gc.curHp > simulator.gc.curHp,
                        "selected Blood Potion did not apply its visible healing action");
            }
            evidence["selected_potion_action"] = {
                {"choice_type", choiceType}, {"potion", potion},
                {"potion_consumed", true}
            };
        } else {
            throw std::logic_error("adapter returned an unsupported MAP choice type");
        }
    }
    evidence["serialized_policy_input"] = policyInput;
    return evidence;
}

}  // namespace

int main() {
    try {
        scoped_interpreter interpreter{};
        constexpr std::uint64_t seed = 49;
        constexpr int ascension = 20;
        nlohmann::json evidence = {
            {"schema_id", "native-public-noncombat-projection-smoke-v1"},
            {"tested_native_source_commit", STS_LIGHTSPEED_TESTED_SOURCE_COMMIT},
            {"character", "IRONCLAD"},
            {"ascension", ascension},
            {"seed", seed},
            {"outcome_claim", "none"}
        };

        checkControlledVisibleAlias(evidence);
        checkControlledVisibleRouteAlias(evidence);

        StepSimulator run(CharacterClass::IRONCLAD, seed, ascension);
        advanceTo(run, ScreenState::REWARDS, 8);
        auto rewardProjection = projection(run);
        const auto rewardActions = checkCandidateParity(run, rewardProjection);
        const auto reward = screenPayload(rewardProjection);
        require(coverage(rewardProjection) == "supported",
                "natural A20 reward screen is not fully described");
        const auto rewardChoices = reward["choices"].cast<list>();
        const auto rewardCandidates = rewardProjection["candidate_actions"].cast<dict>()
                ["value"].cast<list>();
        checkChoiceAssociations(rewardChoices, rewardCandidates);
        bool sawCard = false;
        bool sawGold = false;
        for (const auto &choiceHandle : rewardChoices) {
            const auto choice = choiceHandle.cast<dict>();
            if (choice["choice_type"].cast<std::string>() == "card") {
                const auto card = choice["card"].cast<dict>();
                require(card.contains("id") && card.contains("name")
                                && card.contains("type") && card.contains("rarity")
                                && card.contains("upgraded"),
                        "visible card reward is missing its public identity");
                sawCard = true;
            }
            if (choice["choice_type"].cast<std::string>() == "gold") {
                require(choice.contains("gold"), "visible gold reward amount is missing");
                sawGold = true;
            }
        }
        evidence["natural_reward_screen"] = {
            {"screen", "REWARDS"}, {"coverage_status", "supported"},
            {"candidate_count", rewardActions.size()},
            {"choice_count", rewardChoices.size()},
            {"card_contents_visible", sawCard}, {"gold_contents_visible", sawGold},
            {"candidate_order_matches_native_actions", true}
        };
        checkHiddenFutureInvariant(run, rewardProjection);
        evidence["examples"] = nlohmann::json::object();
        evidence["examples"]["rewards"] = nlohmann::json::parse(
                encodePython(rewardProjection));
        const auto rewardAdapterExecution = checkStudyAdapterBoundary(
                run, rewardProjection, rewardActions, rewardActions.size() - 2);
        evidence["examples"]["policy_input_rewards"] =
                rewardAdapterExecution.at("serialized_policy_input");
        evidence["adapter_online_execution"] = nlohmann::json::object();
        evidence["adapter_online_execution"]["natural_reward"] = rewardAdapterExecution;

        advanceTo(run, ScreenState::MAP_SCREEN, 8);
        const auto mapProjection = projection(run);
        const auto mapActions = checkCandidateParity(run, mapProjection);
        const auto mapPayload = screenPayload(mapProjection);
        require(coverage(mapProjection) == "supported",
                "natural A20 map is not fully described");
        const auto mapField = mapProjection["visible_map_graph"].cast<dict>();
        const auto routeField = mapProjection["immediately_legal_routes"].cast<dict>();
        require(mapField["availability"].cast<std::string>() == "available"
                        && routeField["availability"].cast<std::string>() == "available",
                "public map or legal routes are unavailable");
        const auto routes = routeField["value"].cast<list>();
        const auto choices = mapPayload["choices"].cast<list>();
        const auto mapCandidates = mapProjection["candidate_actions"].cast<dict>()
                ["value"].cast<list>();
        checkChoiceAssociations(choices, mapCandidates);
        require(routes.size() > 0, "natural A20 map has no route choices");
        bool sawUnknownRoom = false;
        for (const auto &nodeHandle : mapField["value"].cast<list>()) {
            const auto node = nodeHandle.cast<dict>();
            if (node["room_symbol"].cast<std::string>() == "?") {
                sawUnknownRoom = true;
            }
        }
        require(sawUnknownRoom,
                "natural map example did not exercise the public unknown-room symbol");
        evidence["natural_map_screen"] = {
            {"screen", "MAP_SCREEN"}, {"coverage_status", "supported"},
            {"visible_node_count", mapField["value"].cast<list>().size()},
            {"legal_route_count", routes.size()},
            {"ordered_choice_count", choices.size()},
            {"candidate_count", mapActions.size()},
            {"candidate_order_matches_native_actions", true},
            {"unknown_event_room_uses_public_symbol", sawUnknownRoom}
        };
        checkHiddenFutureInvariant(run, mapProjection);
        evidence["examples"]["map"] = nlohmann::json::parse(encodePython(mapProjection));
        const auto mapAdapterExecution = checkStudyAdapterBoundary(
                run, mapProjection, mapActions, mapActions.size() - 1);
        evidence["examples"]["policy_input_map"] =
                mapAdapterExecution.at("serialized_policy_input");
        evidence["adapter_online_execution"]["natural_map"] = mapAdapterExecution;
        evidence["held_potion_map_regression"] = nlohmann::json::object();
        checkMapHeldPotionActions(evidence["held_potion_map_regression"]);

        StepSimulator indexedGold(CharacterClass::IRONCLAD, seed, ascension);
        indexedGold.gc.screenState = ScreenState::REWARDS;
        indexedGold.gc.info.rewardsContainer.clear();
        indexedGold.gc.info.rewardsContainer.addGold(17);
        indexedGold.gc.info.rewardsContainer.addGold(41);
        indexedGold.gc.info.rewardsContainer.addRelic(RelicId::ANCHOR);
        indexedGold.gc.info.rewardsContainer.addPotion(Potion::FIRE_POTION);
        indexedGold.gc.info.rewardsContainer.sapphireKey = true;
        const auto indexedGoldProjection = projection(indexedGold);
        const auto indexedActions = checkCandidateParity(indexedGold, indexedGoldProjection);
        const auto indexedPayload = screenPayload(indexedGoldProjection);
        require(indexedPayload["coverage_status"].cast<std::string>() == "supported",
                "indexed gold fixture is unsupported");
        const auto indexedChoices = indexedPayload["choices"].cast<list>();
        const auto indexedCandidates = indexedGoldProjection["candidate_actions"].cast<dict>()
                ["value"].cast<list>();
        checkChoiceAssociations(indexedChoices, indexedCandidates);
        int mappedGoldCount = 0;
        bool sawRelic = false;
        bool sawPotion = false;
        bool sawSapphireKey = false;
        for (const auto &choiceHandle : indexedChoices) {
            const auto choice = choiceHandle.cast<dict>();
            const auto type = choice["choice_type"].cast<std::string>();
            if (type == "gold") {
                const auto index = choice["candidate_action"].cast<dict>()
                        ["idx1"].cast<int>();
                require(choice["gold"].cast<int>() == (index == 0 ? 17 : 41),
                        "gold value does not match its indexed native action");
                auto execution = indexedGold;
                const auto result = execution.step(indexedActions[choice["candidate_index"]
                        .cast<std::size_t>()]);
                require(result["gold"].cast<int>() == 99 + (index == 0 ? 17 : 41),
                        "selected indexed gold action executed the wrong visible amount");
                ++mappedGoldCount;
            } else if (type == "relic") {
                const auto relic = choice["relic"].cast<dict>();
                sawRelic = relic["id"].cast<int>() == static_cast<int>(RelicId::ANCHOR);
            } else if (type == "potion") {
                const auto potion = choice["potion"].cast<dict>();
                sawPotion = potion["id"].cast<int>() == static_cast<int>(Potion::FIRE_POTION);
            } else if (type == "key") {
                sawSapphireKey = choice["key"].cast<std::string>() == "SAPPHIRE_KEY"
                        && choice.contains("relic_removed_with_key");
            }
        }
        require(mappedGoldCount == 2 && sawRelic && sawPotion && sawSapphireKey,
                "indexed native reward fixture omitted gold/relic/potion/key semantics");
        evidence["multi_gold_action_parity"] = {
            {"gold_options", {17, 41}}, {"candidate_mappings_checked", mappedGoldCount},
            {"both_native_actions_executed_associated_amount", true},
            {"relic_potion_and_sapphire_key_rendered", true}
        };

        nlohmann::json unsupported = nlohmann::json::object();
        for (const auto screen : {ScreenState::EVENT_SCREEN, ScreenState::BOSS_RELIC_REWARDS,
                ScreenState::CARD_SELECT, ScreenState::TREASURE_ROOM, ScreenState::REST_ROOM,
                ScreenState::SHOP_ROOM}) {
            StepSimulator fixture(CharacterClass::IRONCLAD, seed, ascension);
            fixture.gc.screenState = screen;
            const auto payload = screenPayload(projection(fixture));
            require(payload["coverage_status"].cast<std::string>() == "unsupported"
                            && payload.contains("reason"),
                    "unsupported screen omitted its fail-closed reason");
            unsupported[screenStateLabel(screen)] = payload["reason"].cast<std::string>();
        }
        evidence["unsupported_screens"] = unsupported;

        // The pre-existing Battle surface remains the trusted executor contract.
        for (int step = 0; step < 32
                && run.gc.screenState != ScreenState::BATTLE
                && run.gc.outcome == GameOutcome::UNDECIDED; ++step) {
            advanceFirstLegal(run);
        }
        require(run.gc.screenState == ScreenState::BATTLE,
                "seed 49 did not reach Battle within the bounded regression path");
        {
            const auto battleProjection = projection(run);
            const auto battleCandidates = checkCandidateParity(run, battleProjection);
            const auto battleState = run.t096PublicInformationProjection();
            require(battleState.contains("ordered_public_legal_actions"),
                    "existing T096 public battle action surface disappeared");
            require(!battleProjection.contains("second_boss")
                            && !battleProjection.contains("rng")
                            && !battleProjection.contains("seed"),
                    "private game future leaked into normal public projection");
            evidence["battle_regression"] = {
                {"screen", "BATTLE"}, {"candidate_count", battleCandidates.size()},
                {"t096_public_battle_api_preserved", true}, {"opaque_action_bits_withheld", true}
            };
        }

        std::cout << evidence.dump() << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
