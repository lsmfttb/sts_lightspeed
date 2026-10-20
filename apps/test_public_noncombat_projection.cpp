#include "../bindings/slaythespire.cpp"

#include <pybind11/embed.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

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

void checkHiddenFutureInvariant(StepSimulator &simulator, const dict &baseline);

void checkSmokeBombBattleProjection(nlohmann::json &evidence) {
    StepSimulator simulator(CharacterClass::IRONCLAD, 52, 0);
    const auto configure = [](StepSimulator &fixture) {
        fixture.gc.screenState = ScreenState::BATTLE;
        fixture.gc.curRoom = Room::EVENT;
        fixture.gc.info.encounter = MonsterEncounter::MYSTERIOUS_SPHERE_EVENT;
        fixture.gc.potions.fill(Potion::EMPTY_POTION_SLOT);
        fixture.gc.potions[0] = Potion::SMOKE_BOMB;
        fixture.gc.potions[1] = Potion::SMOKE_BOMB;
        fixture.gc.potionCount = 2;
        fixture.gc.curHp = fixture.gc.maxHp - 1;
        fixture.gc.gold = 73;
        fixture.gc.info.stolenGold = 19;
        fixture.gc.info.rewardsContainer.clear();
        fixture.battleActive = false;
    };
    configure(simulator);

    const auto nativeProjection = projection(simulator);
    const auto actions = checkCandidateParity(simulator, nativeProjection);
    auto changedFuture = StepSimulator(CharacterClass::IRONCLAD, 52, 0);
    configure(changedFuture);
    (void) projection(changedFuture);
    changedFuture.gc.seed ^= 0x22002026ULL;
    changedFuture.gc.eventRng = Random(0x22012026ULL);
    changedFuture.gc.monsterRng = Random(0x22022026ULL);
    changedFuture.gc.cardRng = Random(0x22032026ULL);
    changedFuture.gc.relicRng = Random(0x22042026ULL);
    changedFuture.gc.secondBoss = MonsterEncounter::AWAKENED_ONE;
    require(nativeProjection.equal(projection(changedFuture)),
            "public battle projection changed with hidden future fields");

    std::array<std::size_t, 2> smokeSlots = {actions.size(), actions.size()};
    for (std::size_t index = 0; index < actions.size(); ++index) {
        const auto &action = actions[index];
        if (action.scope != "battle" || action.kind != "potion") continue;
        if (action.idx1 >= 0 && action.idx1 < static_cast<int>(smokeSlots.size())) {
            smokeSlots[action.idx1] = index;
        }
    }
    require(smokeSlots[0] < actions.size() && smokeSlots[1] < actions.size()
                    && smokeSlots[0] != smokeSlots[1],
            "public battle candidates collapsed duplicate Smoke Bomb slots");

    const int initialGold = simulator.gc.gold;
    const int initialHp = simulator.gc.curHp;
    const int initialMaxHp = simulator.gc.maxHp;
    const int initialRelics = simulator.gc.relics.relics.size();
    require(simulator.gc.relics.has(RelicId::BURNING_BLOOD),
            "public Smoke Bomb fixture is missing Ironclad's Burning Blood");
    const int expectedHpAfterEscape = std::min(initialMaxHp, initialHp + 6);
    const auto selected = actions[smokeSlots[1]];
    require(selected.idx1 == 1,
            "public candidate selection did not retain the chosen potion slot");
    const auto transition = simulator.step(selected);
    require(transition["completed_battle_outcome"].cast<std::string>() == "PLAYER_ESCAPE",
            "selected public Smoke Bomb action did not report escape");
    require(simulator.gc.screenState == ScreenState::MAP_SCREEN
                    && simulator.gc.outcome == GameOutcome::UNDECIDED,
            "Smoke Bomb escape did not continue the run at the map");
    require(simulator.gc.potions[0] == Potion::SMOKE_BOMB
                    && simulator.gc.potions[1] == Potion::EMPTY_POTION_SLOT
                    && simulator.gc.potionCount == 1,
            "selected public Smoke Bomb action consumed the wrong potion slot");
    require(simulator.gc.gold == initialGold
                    && simulator.gc.curHp == expectedHpAfterEscape
                    && simulator.gc.maxHp == initialMaxHp
                    && simulator.gc.relics.relics.size() == initialRelics
                    && simulator.gc.info.stolenGold == 0,
            "Smoke Bomb escape missed its capped combat-end heal or granted loot");
    require(simulator.gc.info.rewardsContainer.relicCount == 0
                    && simulator.gc.info.rewardsContainer.cardRewardCount == 0
                    && simulator.gc.info.rewardsContainer.potionCount == 0
                    && simulator.gc.info.rewardsContainer.goldRewardCount == 0,
            "Smoke Bomb escape produced an event or normal combat reward");

    evidence = {
        {"tested_native_source_commit", STS_LIGHTSPEED_TESTED_SOURCE_COMMIT},
        {"screen", "BATTLE"},
        {"candidate_count", actions.size()},
        {"duplicate_smoke_slots_remain_distinct", true},
        {"selected_potion_slot", selected.idx1},
        {"opaque_action_bits_withheld", true},
        {"hidden_future_projection_invariant", true},
        {"executed_outcome", "PLAYER_ESCAPE"},
        {"burning_blood_escape_heal_applied", simulator.gc.curHp == expectedHpAfterEscape},
        {"escape_heal_capped", expectedHpAfterEscape == initialMaxHp},
        {"starting_hp", initialHp},
        {"ending_hp", simulator.gc.curHp},
        {"max_hp", simulator.gc.maxHp},
        {"returned_to_map_without_reward_or_victory", true}
    };
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

void checkSapphireRewardCoupling(nlohmann::json &evidence) {
    auto adapter = module_::import("adapter");
    const auto executePublicChoice = [&](StepSimulator &simulator, std::size_t ordinal) {
        const auto publicValue = projection(simulator);
        const auto actions = checkCandidateParity(simulator, publicValue);
        const auto prepared = adapter.attr("prepare_policy_input")(
                publicValue, nativeActionMappings(actions));
        const auto serialized = prepared.attr("serialized_input").cast<std::string>();
        const auto callback = cpp_function([ordinal, serialized](const std::string &input) {
            require(input == serialized, "Sapphire callback input changed during selection");
            return int_(ordinal);
        });
        const auto selected = adapter.attr("call_policy")(prepared, callback).cast<std::size_t>();
        const auto resolved = adapter.attr("resolve_native_action")(
                prepared, selected, publicValue, nativeActionMappings(actions)).cast<dict>();
        require(selected == ordinal && resolved["bits"].cast<std::uint32_t>() == actions.at(ordinal).bits,
                "Sapphire option did not resolve to its exact native action");
        const auto semantics = nlohmann::json::parse(serialized)
                .at("legal_options").at(ordinal).at("semantics");
        simulator.step(actions[ordinal]);
        return semantics;
    };
    const auto findKey = [](StepSimulator &simulator) {
        const auto actions = simulator.legalActions();
        for (std::size_t i = 0; i < actions.size(); ++i) {
            if (actions[i].kind == "reward_key") return i;
        }
        return actions.size();
    };
    const std::array<RelicId, 3> relics = {
        RelicId::ANCHOR, RelicId::VAJRA, RelicId::BAG_OF_MARBLES
    };
    evidence = nlohmann::json::array();
    for (int count = 1; count <= static_cast<int>(relics.size()); ++count) {
        StepSimulator fixture(CharacterClass::IRONCLAD, 49, 20);
        advanceTo(fixture, ScreenState::REWARDS, 8);
        auto &rewards = fixture.gc.info.rewardsContainer;
        rewards.clear();
        // Retain a gold option so each coupled choice has a remaining reward screen.
        rewards.addGold(1);
        for (int i = 0; i < count; ++i) rewards.addRelic(relics[i]);
        rewards.sapphireKey = true;
        const auto actions = fixture.legalActions();
        int checked = 0;
        for (std::size_t ordinal = 0; ordinal < actions.size(); ++ordinal) {
            const auto &action = actions[ordinal];
            if (action.kind != "reward_relic" && action.kind != "reward_key") continue;
            auto execution = fixture;
            const auto semantics = executePublicChoice(execution, ordinal);
            auto &remaining = execution.gc.info.rewardsContainer;
            require(remaining.relicCount == count - 1,
                    "coupled choice did not remove exactly one offered relic");
            if (action.kind == "reward_relic") {
                const bool associated = action.idx1 == count - 1;
                require(semantics.at("relic").at("id") == static_cast<int>(relics[action.idx1])
                                && execution.gc.relics.has(relics[action.idx1]),
                        "selected Sapphire fixture relic was not awarded");
                require(semantics.value("sapphire_key_is_lost", false) == associated
                                && remaining.sapphireKey == !associated && !execution.gc.blueKey,
                        "relic execution disagrees with projected Sapphire coupling");
                const auto keyOrdinal = findKey(execution);
                if (associated) {
                    require(keyOrdinal == execution.legalActions().size(),
                            "Sapphire Key remained collectible after taking its relic");
                } else {
                    require(keyOrdinal < execution.legalActions().size(),
                            "unassociated relic incorrectly removed the Sapphire Key option");
                    const auto keySemantics = executePublicChoice(execution, keyOrdinal);
                    require(keySemantics.at("relic_removed_with_key").at("id")
                                    == static_cast<int>(relics[count - 1])
                                    && execution.gc.blueKey && !remaining.sapphireKey
                                    && remaining.relicCount == count - 2
                                    && !execution.gc.relics.has(relics[count - 1]),
                            "Sapphire selection after compaction did not remove its associated relic");
                }
                evidence.push_back({{"initial_relic_count", count},
                    {"selected_relic_index", action.idx1},
                    {"projected_sapphire_key_is_lost", associated},
                    {"native_coupling_matches_projection", true}});
            } else {
                require(semantics.at("key") == "SAPPHIRE_KEY"
                                && semantics.at("relic_removed_with_key").at("id")
                                        == static_cast<int>(relics[count - 1])
                                && execution.gc.blueKey && !remaining.sapphireKey,
                        "key-first selection did not remove the associated last relic");
                evidence.push_back({{"initial_relic_count", count},
                    {"selected_key_first", true}, {"native_coupling_matches_projection", true}});
            }
            // Collect all surviving relic options; the unselected coupled relic must stay unavailable.
            const bool choseAssociatedRelic = action.kind == "reward_relic" && action.idx1 == count - 1;
            for (int i = 0; i < count; ++i) {
                const bool shouldRemain = i != count - 1
                        && !(action.kind == "reward_relic" && i == action.idx1);
                bool offered = false;
                for (int slot = 0; slot < remaining.relicCount; ++slot) {
                    offered |= remaining.relics[slot] == relics[i];
                }
                require(offered == shouldRemain, "coupled choice removed the wrong surviving relic");
            }
            while (remaining.relicCount > 0) {
                const auto next = execution.legalActions();
                std::size_t relicOrdinal = next.size();
                for (std::size_t i = 0; i < next.size(); ++i) {
                    if (next[i].kind == "reward_relic") { relicOrdinal = i; break; }
                }
                require(relicOrdinal < next.size(), "surviving relic has no legal action");
                executePublicChoice(execution, relicOrdinal);
            }
            require(execution.gc.relics.has(relics[count - 1]) == choseAssociatedRelic
                            && execution.gc.blueKey == !choseAssociatedRelic,
                    "both the associated relic and Sapphire Key could be collected");
            ++checked;
        }
        require(checked == count + 1, "Sapphire regression omitted a relic or key choice");
    }
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

pybind11::dict privilegedProjection(StepSimulator &simulator) {
    return PrivilegedNoncombatProjectionAccess(
            simulator, "training_oracle_only").projectV1();
}

std::vector<LightSpeedAction> verifyPrivilegedCandidateParity(
        StepSimulator &simulator,
        const pybind11::dict &projectionValue,
        const std::string &expectedStatus = "supported") {
    require(projectionValue["schema_id"].cast<std::string>()
                    == "native-privileged-noncombat-choice-projection-v1",
            "privileged projection schema id is incorrect");
    require(projectionValue["audience"].cast<std::string>() == "training_oracle_only"
                    && !projectionValue["public_api_safe"].cast<bool>()
                    && !projectionValue["policy_training_authorized"].cast<bool>(),
            "privileged projection lacks its restrictive audience boundary");
    require(projectionValue["native_source_commit"].cast<std::string>()
                    == STS_LIGHTSPEED_TESTED_SOURCE_COMMIT,
            "privileged projection omitted the tested native source identity");
    require(projectionValue["coverage_status"].cast<std::string>() == expectedStatus,
            "privileged projection returned an unexpected fail-closed status");

    const auto actions = simulator.legalActions();
    const auto candidates = projectionValue["candidates"].cast<pybind11::list>();
    require(candidates.size() == static_cast<ssize_t>(actions.size())
                    && projectionValue["candidate_count"].cast<std::size_t>()
                            == actions.size(),
            "privileged candidate count differs from the native legal enumerator");
    std::set<std::size_t> occurrences;
    for (std::size_t index = 0; index < actions.size(); ++index) {
        const auto candidate = candidates[static_cast<ssize_t>(index)].cast<pybind11::dict>();
        const auto bindingField = candidate["candidate_binding"].cast<pybind11::dict>();
        require(bindingField["availability"].cast<std::string>() == "available"
                        && bindingField["visibility"].cast<std::string>() == "oracle_only",
                "candidate occurrence binding is not explicitly oracle-only");
        const auto binding = bindingField["value"].cast<pybind11::dict>();
        const auto occurrence = binding["candidate_index"].cast<std::size_t>();
        require(occurrence == index && occurrences.insert(occurrence).second,
                "ordered native candidate occurrence was lost or deduplicated");
        const auto boundAction = binding["candidate_action"].cast<pybind11::dict>();
        require(!boundAction.contains("bits")
                        && boundAction.equal(publicChoiceActionSnapshot(actions[index])),
                "privileged candidate does not bind to its exact sanitized native action");

        const auto candidateStatus = candidate["coverage_status"].cast<std::string>();
        require(candidateStatus == "supported" || candidateStatus == "unsupported",
                "candidate omitted an explicit support status");
        if (expectedStatus == "supported") {
            require(candidateStatus == "supported",
                    "supported fixture contains an unsupported legal candidate");
        }
        if (candidateStatus == "unsupported") {
            require(candidate.contains("reason"),
                    "unsupported native candidate omitted its blocker");
        }
        const auto features = candidate["features"].cast<pybind11::dict>();
        require(!features.empty(), "candidate has no source-backed semantic features");
        for (const auto &featureHandle : features) {
            const auto feature = featureHandle.second.cast<pybind11::dict>();
            require(feature.contains("availability"),
                    "candidate feature omitted explicit availability");
            if (feature["availability"].cast<std::string>() == "available") {
                const auto visibility = feature["visibility"].cast<std::string>();
                require(visibility == "public_now" || visibility == "oracle_only",
                        "available feature omitted the public/oracle visibility boundary");
                require(feature.contains("source"),
                        "available feature omitted native provenance");
            }
        }
    }
    require(occurrences.size() == actions.size(),
            "privileged projection dropped or merged an ordered action occurrence");
    const auto serialized = encodePython(projectionValue);
    require(serialized.find("\"bits\"") == std::string::npos
                    && serialized.find("bits=") == std::string::npos,
            "native simulator-private action bits entered the privileged policy feature");
    return actions;
}

std::size_t findGameCandidate(
        const std::vector<LightSpeedAction> &actions,
        const std::string &kind,
        const int idx1 = -1) {
    for (std::size_t index = 0; index < actions.size(); ++index) {
        if (actions[index].kind == kind && (idx1 < 0 || actions[index].idx1 == idx1)) {
            return index;
        }
    }
    throw std::logic_error("native legal-action fixture omitted candidate " + kind);
}

void installTestMapContinuation(GameContext &gc) {
    gc.regainControlAction = [](GameContext &state) {
        state.screenState = ScreenState::MAP_SCREEN;
        state.regainControlAction = nullptr;
    };
}

void enterNativeMapThroughNeow(StepSimulator &simulator) {
    require(simulator.gc.screenState == ScreenState::EVENT_SCREEN
                    && simulator.gc.curEvent == Event::NEOW,
            "fixture did not begin in the native Neow EVENT_SCREEN representation");
    // Hand-built test fixtures need the ordinary native continuation that the
    // game installs around start-of-run choices. Keep this setup in tests; the
    // semantic capability does not alter StepSimulator transition behavior.
    installTestMapContinuation(simulator.gc);
    simulator.gc.info.neowRewards[0] = {
            Neow::Bonus::HUNDRED_GOLD, Neow::Drawback::NONE};
    const auto actions = simulator.legalActions();
    const auto index = findGameCandidate(actions, "event", 0);
    (void) simulator.step(actions[index]);
    require(simulator.gc.screenState == ScreenState::MAP_SCREEN,
            "native Neow option did not enter MAP_SCREEN through its real executor");
    simulator.gc.regainControlAction = [](GameContext &state) {
        state.screenState = ScreenState::MAP_SCREEN;
        state.regainControlAction = nullptr;
    };
}

long peakResidentSetKilobytes() {
#if defined(__unix__) || defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(__APPLE__)
        return static_cast<long>(usage.ru_maxrss / 1024);
#else
        return static_cast<long>(usage.ru_maxrss);
#endif
    }
#endif
    return -1;
}

nlohmann::json checkPrivilegedProjectionPerformance() {
    StepSimulator simulator(CharacterClass::IRONCLAD, 49, 20);
    simulator.gc.screenState = ScreenState::SHOP_ROOM;
    simulator.gc.gold = 1000;
    simulator.gc.info.shop.setup(simulator.gc);
    const auto nativeActions = simulator.legalActions();
    require(nativeActions.size() > 1, "benchmark shop fixture has too few legal candidates");
    constexpr int iterations = 10000;
    std::size_t sink = 0;
    const PrivilegedNoncombatProjectionAccess projectionAccess(
            simulator, "training_oracle_only");
    const auto legalWallStart = std::chrono::steady_clock::now();
    const auto legalCpuStart = std::clock();
    for (int i = 0; i < iterations; ++i) {
        sink += simulator.legalActions().size();
    }
    const auto legalCpuEnd = std::clock();
    const auto legalWallEnd = std::chrono::steady_clock::now();
    const long rssAfterLegal = peakResidentSetKilobytes();
    const auto projectionWallStart = std::chrono::steady_clock::now();
    const auto projectionCpuStart = std::clock();
    for (int i = 0; i < iterations; ++i) {
        const auto value = projectionAccess.projectV1();
        sink += value["candidate_count"].cast<std::size_t>();
    }
    const auto projectionCpuEnd = std::clock();
    const auto projectionWallEnd = std::chrono::steady_clock::now();
    const long rssAfterProjection = peakResidentSetKilobytes();
    const double legalCpuSeconds = static_cast<double>(legalCpuEnd - legalCpuStart) / CLOCKS_PER_SEC;
    const double projectionCpuSeconds = static_cast<double>(projectionCpuEnd - projectionCpuStart) / CLOCKS_PER_SEC;
    const double legalWallSeconds = std::chrono::duration<double>(legalWallEnd - legalWallStart).count();
    const double projectionWallSeconds = std::chrono::duration<double>(projectionWallEnd - projectionWallStart).count();
    return {
        {"schema_id", "native-privileged-noncombat-projection-performance-v1"},
        {"native_source_commit", STS_LIGHTSPEED_TESTED_SOURCE_COMMIT},
        {"screen", "SHOP_ROOM"}, {"native_candidate_count", nativeActions.size()},
        {"iterations_per_path", iterations}, {"candidate_sink", sink},
        {"legal_actions_wall_seconds", legalWallSeconds},
        {"privileged_projection_wall_seconds", projectionWallSeconds},
        {"wall_overhead_ratio", projectionWallSeconds / legalWallSeconds},
        {"legal_actions_cpu_seconds", legalCpuSeconds},
        {"privileged_projection_cpu_seconds", projectionCpuSeconds},
        {"cpu_overhead_ratio", projectionCpuSeconds / legalCpuSeconds},
        {"peak_rss_after_legal_actions_kb", rssAfterLegal},
        {"peak_rss_after_privileged_projection_kb", rssAfterProjection},
        {"peak_rss_delta_kb", rssAfterProjection < 0 || rssAfterLegal < 0
                ? -1 : rssAfterProjection - rssAfterLegal},
        {"rss_measurement", "getrusage(RUSAGE_SELF).ru_maxrss process high-water mark; not isolated per-allocation accounting"}
    };
}

nlohmann::json checkPrivilegedNoncombatProjection() {
    StepSimulator boundarySimulator(CharacterClass::IRONCLAD, 49, 20);
    bool rejectedPublicAudience = false;
    try {
        (void) PrivilegedNoncombatProjectionAccess(boundarySimulator, "public_now");
    } catch (const std::invalid_argument &) {
        rejectedPublicAudience = true;
    }
    require(rejectedPublicAudience,
            "privileged projection access accepted a public audience tag");

    nlohmann::json result = {
        {"schema_id", "native-privileged-noncombat-semantic-capability-evidence-v1"},
        {"tested_native_source_commit", STS_LIGHTSPEED_TESTED_SOURCE_COMMIT},
        {"scope", "source-backed native projection and selected candidate execution; no policy training"},
        {"coverage_matrix", nlohmann::json::object()},
        {"execution_witnesses", nlohmann::json::array()}
    };

    // Audit every source-enabled multi-choice event family against the exact
    // native enumerator.  Event state fields are initialized to valid native
    // fixture values; we do not run a second event implementation here.
    const std::vector<Event> eventFamilies = {
        Event::OMINOUS_FORGE, Event::PLEADING_VAGRANT, Event::ANCIENT_WRITING,
        Event::OLD_BEGGAR, Event::BIG_FISH, Event::CURSED_TOME, Event::DEAD_ADVENTURER,
        Event::DESIGNER_IN_SPIRE, Event::AUGMENTER, Event::DUPLICATOR, Event::FACE_TRADER,
        Event::FALLING, Event::FORGOTTEN_ALTAR, Event::THE_DIVINE_FOUNTAIN, Event::GHOSTS,
        Event::GOLDEN_IDOL, Event::GOLDEN_SHRINE, Event::WING_STATUE, Event::KNOWING_SKULL,
        Event::THE_SSSSSERPENT, Event::LIVING_WALL, Event::MASKED_BANDITS, Event::MINDBLOOM,
        Event::HYPNOTIZING_COLORED_MUSHROOMS, Event::MYSTERIOUS_SPHERE, Event::THE_NEST,
        Event::NLOTH, Event::NOTE_FOR_YOURSELF, Event::PURIFIER, Event::SCRAP_OOZE,
        Event::SECRET_PORTAL, Event::SENSORY_STONE, Event::SHINING_LIGHT, Event::THE_CLERIC,
        Event::THE_JOUST, Event::THE_LIBRARY, Event::THE_MAUSOLEUM, Event::THE_MOAI_HEAD,
        Event::THE_WOMAN_IN_BLUE, Event::TOMB_OF_LORD_RED_MASK, Event::TRANSMORGRIFIER,
        Event::UPGRADE_SHRINE, Event::VAMPIRES, Event::WE_MEET_AGAIN, Event::WINDING_HALLS,
        Event::WORLD_OF_GOOP
    };
    std::size_t eventFamilyCount = 0;
    std::size_t eventCandidateCount = 0;
    for (const auto event : eventFamilies) {
        StepSimulator fixture(CharacterClass::IRONCLAD, 49, 20);
        auto &gc = fixture.gc;
        gc.screenState = ScreenState::EVENT_SCREEN;
        gc.curEvent = event;
        gc.gold = 1000;
        gc.curHp = 60;
        gc.maxHp = 80;
        gc.floorNum = 20;
        gc.info.eventData = 0;
        gc.info.hpAmount0 = 4;
        gc.info.hpAmount1 = 5;
        gc.info.hpAmount2 = 6;
        gc.info.phase = 0;
        gc.info.rewards = {0, 1, 2};
        gc.info.upgradeOne = true;
        gc.info.cleanUpIsRemoveCard = true;
        gc.info.goldLoss = 10;
        gc.info.potionIdx = -1;
        gc.info.gold = 50;
        gc.info.cardIdx = 0;
        gc.info.relicIdx0 = 0;
        gc.info.relicIdx1 = 0;
        gc.info.skillCardDeckIdx = -1;
        gc.info.powerCardDeckIdx = -1;
        gc.info.attackCardDeckIdx = -1;
        for (int card = 0; card < gc.deck.size(); ++card) {
            const auto type = gc.deck.cards[card].getType();
            if (type == CardType::SKILL && gc.info.skillCardDeckIdx == -1)
                gc.info.skillCardDeckIdx = card;
            if (type == CardType::POWER && gc.info.powerCardDeckIdx == -1)
                gc.info.powerCardDeckIdx = card;
            if (type == CardType::ATTACK && gc.info.attackCardDeckIdx == -1)
                gc.info.attackCardDeckIdx = card;
        }
        if (event == Event::WE_MEET_AGAIN) {
            gc.obtainPotion(Potion::FRUIT_JUICE);
            gc.info.potionIdx = 0;
        }
        const auto projectionValue = privilegedProjection(fixture);
        const auto actions = fixture.legalActions();
        if (actions.size() < 2) continue;
        const auto checkedActions = verifyPrivilegedCandidateParity(fixture, projectionValue);
        require(checkedActions.size() == actions.size(),
                "event family candidate parity changed during semantic projection");
        const auto eventRows = projectionValue["candidates"].cast<pybind11::list>();
        for (const auto &rowHandle : eventRows) {
            const auto row = rowHandle.cast<pybind11::dict>();
            const auto candidateBinding = row["candidate_binding"].cast<pybind11::dict>()
                    ["value"].cast<pybind11::dict>();
            const auto candidateKind = candidateBinding["candidate_action"]
                    .cast<pybind11::dict>()["kind"].cast<std::string>();
            if (candidateKind == "event") {
                const auto features = row["features"].cast<pybind11::dict>();
                const auto choice = features["choice"].cast<pybind11::dict>();
                require(choice["visibility"].cast<std::string>() == "oracle_only",
                        "source-reconstructed event meaning was not tagged as training-oracle-only");
            }
        }
        ++eventFamilyCount;
        eventCandidateCount += actions.size();
    }
    require(eventFamilyCount == eventFamilies.size(),
            "source-enabled event fixture skipped a multi-choice family");

    // The native chooser treats 0 as gold, 2 as a relic, and leaves 1 with
    // neither immediate reward.  Force a no-encounter draw so each labeled
    // phase value can be checked against that native branch's actual effect.
    std::uint64_t noEncounterSeed = 0;
    for (; noEncounterSeed < 128; ++noEncounterSeed) {
        sts::Random rng(noEncounterSeed);
        if (rng.random(99) >= 25) break;
    }
    require(noEncounterSeed < 128,
            "could not find a deterministic Dead Adventurer no-encounter fixture seed");
    const std::array<std::pair<int, const char *>, 3> deadAdventurerRewards = {{
        {0, "gold"}, {1, "no_reward"}, {2, "relic"}
    }};
    for (const auto &[nativeCode, expectedKind] : deadAdventurerRewards) {
        StepSimulator deadAdventurer(CharacterClass::IRONCLAD, noEncounterSeed, 0);
        deadAdventurer.gc.screenState = ScreenState::EVENT_SCREEN;
        deadAdventurer.gc.curEvent = Event::DEAD_ADVENTURER;
        deadAdventurer.gc.info.phase = 0;
        deadAdventurer.gc.info.rewards = {nativeCode, 1, 2};
        deadAdventurer.gc.miscRng = sts::Random(noEncounterSeed);
        const auto projectionValue = privilegedProjection(deadAdventurer);
        const auto actions = verifyPrivilegedCandidateParity(deadAdventurer, projectionValue);
        const auto searchIndex = findGameCandidate(actions, "event", 0);
        const auto candidates = projectionValue["candidates"].cast<pybind11::list>();
        const auto row = candidates[searchIndex].cast<pybind11::dict>();
        const auto features = row["features"].cast<pybind11::dict>();
        const auto choiceFeature = features["choice"].cast<pybind11::dict>();
        require(choiceFeature["availability"].cast<std::string>() == "available"
                        && choiceFeature["visibility"].cast<std::string>() == "oracle_only"
                        && choiceFeature["source"].cast<std::string>()
                                == "GameContext::chooseEventOption DEAD_ADVENTURER reward codes",
                "Dead Adventurer reward label omitted its native source branch");
        const auto choice = choiceFeature["value"].cast<pybind11::dict>();
        require(choice["phase_reward_kind"].cast<std::string>() == expectedKind,
                "Dead Adventurer native reward code has the wrong semantic label");

        const int goldBefore = deadAdventurer.gc.gold;
        const int relicCountBefore = deadAdventurer.gc.relics.size();
        auto execute = deadAdventurer;
        (void) execute.step(actions[searchIndex]);
        require(execute.gc.screenState == ScreenState::EVENT_SCREEN
                        && execute.gc.info.phase == 1,
                "Dead Adventurer no-encounter branch did not advance its native phase");
        if (nativeCode == 0) {
            require(execute.gc.gold == goldBefore + 30
                            && execute.gc.relics.size() == relicCountBefore,
                    "native Dead Adventurer code 0 did not grant its 30 gold reward");
        } else if (nativeCode == 1) {
            require(execute.gc.gold == goldBefore
                            && execute.gc.relics.size() == relicCountBefore,
                    "native Dead Adventurer code 1 unexpectedly granted gold or a relic");
        } else {
            require(execute.gc.gold == goldBefore
                            && execute.gc.relics.size() == relicCountBefore + 1,
                    "native Dead Adventurer code 2 did not grant its relic reward");
        }
    }

    StepSimulator unknownDeadAdventurerReward(CharacterClass::IRONCLAD, 49, 0);
    unknownDeadAdventurerReward.gc.screenState = ScreenState::EVENT_SCREEN;
    unknownDeadAdventurerReward.gc.curEvent = Event::DEAD_ADVENTURER;
    unknownDeadAdventurerReward.gc.info.phase = 0;
    unknownDeadAdventurerReward.gc.info.rewards = {99, 1, 2};
    const auto unknownRewardProjection = privilegedProjection(unknownDeadAdventurerReward);
    const auto unknownRewardActions = verifyPrivilegedCandidateParity(
            unknownDeadAdventurerReward, unknownRewardProjection,
            "NATIVE_CHOICE_SEMANTIC_NOT_READY");
    const auto unknownSearchIndex = findGameCandidate(unknownRewardActions, "event", 0);
    const auto unknownRewardRow = unknownRewardProjection["candidates"]
            .cast<pybind11::list>()[unknownSearchIndex].cast<pybind11::dict>();
    require(unknownRewardRow["coverage_status"].cast<std::string>() == "unsupported"
                    && unknownRewardRow.contains("reason"),
            "unknown Dead Adventurer reward code did not fail closed per candidate");

    StepSimulator invalidDeadAdventurerPhase(CharacterClass::IRONCLAD, 49, 0);
    invalidDeadAdventurerPhase.gc.screenState = ScreenState::EVENT_SCREEN;
    invalidDeadAdventurerPhase.gc.curEvent = Event::DEAD_ADVENTURER;
    invalidDeadAdventurerPhase.gc.info.phase = 3;
    invalidDeadAdventurerPhase.gc.info.rewards = {0, 1, 2};
    const auto invalidPhaseProjection = privilegedProjection(invalidDeadAdventurerPhase);
    const auto invalidPhaseActions = verifyPrivilegedCandidateParity(
            invalidDeadAdventurerPhase, invalidPhaseProjection,
            "NATIVE_CHOICE_SEMANTIC_NOT_READY");
    const auto invalidPhaseSearchIndex = findGameCandidate(invalidPhaseActions, "event", 0);
    const auto invalidPhaseRow = invalidPhaseProjection["candidates"]
            .cast<pybind11::list>()[invalidPhaseSearchIndex].cast<pybind11::dict>();
    require(invalidPhaseRow["coverage_status"].cast<std::string>() == "unsupported"
                    && invalidPhaseRow.contains("reason"),
            "out-of-range Dead Adventurer phase did not fail closed per candidate");
    StepSimulator unsupportedColosseum(CharacterClass::IRONCLAD, 49, 20);
    unsupportedColosseum.gc.screenState = ScreenState::EVENT_SCREEN;
    unsupportedColosseum.gc.curEvent = Event::COLOSSEUM;
    unsupportedColosseum.gc.info.eventData = 1;
    const auto colosseumProjection = privilegedProjection(unsupportedColosseum);
    const auto colosseumActions = verifyPrivilegedCandidateParity(
            unsupportedColosseum, colosseumProjection, "NATIVE_CHOICE_SEMANTIC_NOT_READY");
    require(colosseumActions.size() == 2
                    && colosseumProjection["unsupported_candidates"].cast<pybind11::list>().size() == 2,
            "disabled/TODO Colosseum alternatives did not fail closed individually");
    result["coverage_matrix"]["EVENT_SCREEN:COLOSSEUM"] = {
        {"status", "unsupported"}, {"native_candidate_count", colosseumActions.size()},
        {"blocker", "native event generation disables Colosseum; chooseEventOption is TODO and both alternatives currently return to map"}
    };

    StepSimulator unsupportedLab(CharacterClass::IRONCLAD, 49, 20);
    unsupportedLab.gc.screenState = ScreenState::EVENT_SCREEN;
    unsupportedLab.gc.curEvent = Event::LAB;
    const auto labProjection = privilegedProjection(unsupportedLab);
    const auto labActions = verifyPrivilegedCandidateParity(
            unsupportedLab, labProjection, "NATIVE_CHOICE_SEMANTIC_NOT_READY");
    require(labActions.size() == 1
                    && labProjection["unsupported_candidates"].cast<pybind11::list>().size() == 1,
            "LAB action bit did not fail closed against its asserting native chooser");
    result["coverage_matrix"]["EVENT_SCREEN:LAB"] = {
        {"status", "unsupported"}, {"native_candidate_count", labActions.size()},
        {"blocker", "native action enumeration has one bit but chooseEventOption asserts no action is valid"}
    };

    StepSimulator designer(CharacterClass::IRONCLAD, 49, 20);
    designer.gc.screenState = ScreenState::EVENT_SCREEN;
    designer.gc.curEvent = Event::DESIGNER_IN_SPIRE;
    designer.gc.gold = 500;
    designer.gc.info.cleanUpIsRemoveCard = false;
    installTestMapContinuation(designer.gc);
    const auto designerProjection = privilegedProjection(designer);
    const auto designerActions = verifyPrivilegedCandidateParity(designer, designerProjection);
    const auto transformCleanupIndex = findGameCandidate(designerActions, "event", 3);
    const auto designerRows = designerProjection["candidates"].cast<pybind11::list>();
    const auto designerFeatures = designerRows[transformCleanupIndex].cast<pybind11::dict>()
            ["features"].cast<pybind11::dict>();
    const auto designerChoice = designerFeatures["choice"].cast<pybind11::dict>()
            ["value"].cast<pybind11::dict>();
    require(designerChoice["gold_cost"].cast<int>() == 185
                    && designerChoice["hp_cost"].cast<int>() == 5
                    && designerChoice["source_fallthrough_to_option_4"].cast<bool>()
                    && designerChoice["source_fallthrough_to_option_5"].cast<bool>(),
            "Designer In-Spire fall-through candidate omitted cumulative source costs/effects");
    const int designerGoldBefore = designer.gc.gold;
    const int designerHpBefore = designer.gc.curHp;
    auto executeDesigner = designer;
    (void) executeDesigner.step(designerActions[transformCleanupIndex]);
    require(executeDesigner.gc.gold == designerGoldBefore - 185
                    && executeDesigner.gc.curHp == designerHpBefore - 5
                    && executeDesigner.gc.screenState == ScreenState::MAP_SCREEN,
            "identified Designer In-Spire candidate did not run its native fall-through path");
    result["coverage_matrix"]["EVENT_SCREEN:DESIGNER_IN_SPIRE_FALLTHROUGH"] = {
        {"status", "supported"}, {"candidate_index", 3}, {"gold_cost", 185},
        {"hp_cost", 5}, {"source_fallthrough_options", {4, 5}},
        {"execution_witness", true}
    };
    result["execution_witnesses"].push_back({
        {"family", "EVENT_SCREEN:DESIGNER_IN_SPIRE"},
        {"candidate_index", transformCleanupIndex},
        {"effect", "native option 3 fall-through spent cumulative gold, applied option 5 HP loss, and returned to map"}
    });

    StepSimulator wheel(CharacterClass::IRONCLAD, 49, 20);
    wheel.gc.screenState = ScreenState::EVENT_SCREEN;
    wheel.gc.curEvent = Event::WHEEL_OF_CHANGE;
    installTestMapContinuation(wheel.gc);
    const auto wheelProjection = privilegedProjection(wheel);
    const auto wheelActions = verifyPrivilegedCandidateParity(wheel, wheelProjection);
    require(wheelActions.size() == 1,
            "Wheel of Change did not expose its single native legal candidate");
    const auto wheelRows = wheelProjection["candidates"].cast<pybind11::list>();
    const auto wheelFeatures = wheelRows[0].cast<pybind11::dict>()["features"]
            .cast<pybind11::dict>();
    const auto wheelChoice = wheelFeatures["choice"].cast<pybind11::dict>();
    const auto wheelChoiceValue = wheelChoice["value"].cast<pybind11::dict>();
    require(wheelChoiceValue["choice_type"].cast<std::string>() == "spin_wheel"
                    && wheelChoiceValue["effect_type"].cast<std::string>()
                            == "resolve_random_wheel_outcome",
            "single legal Wheel of Change candidate omitted its source-backed meaning");
    const auto wheelResult = wheelFeatures["wheel_result"].cast<pybind11::dict>();
    require(wheelResult["availability"].cast<std::string>() == "unavailable"
                    && !wheelResult["required_for_choice_semantics"].cast<bool>(),
            "Wheel of Change exposed or required an unrevealed RNG outcome");
    auto executeWheel = wheel;
    (void) executeWheel.step(wheelActions[0]);
    require(executeWheel.gc.screenState != ScreenState::EVENT_SCREEN,
            "identified single Wheel of Change candidate did not execute its native chooser");
    result["coverage_matrix"]["EVENT_SCREEN:WHEEL_OF_CHANGE"] = {
        {"status", "supported"}, {"native_candidate_count", wheelActions.size()},
        {"single_candidate_execution", true}, {"rng_result", "unavailable"}
    };

    result["coverage_matrix"]["EVENT_SCREEN:SOURCE_ENABLED_MULTI_CHOICE"] = {
        {"status", "supported"}, {"multi_choice_event_families_checked", eventFamilyCount},
        {"native_candidate_occurrences_checked", eventCandidateCount},
        {"all_candidate_semantics_available", eventFamilyCount == eventFamilies.size()},
        {"transition_witness_scope", "representative native execution witnesses include Neow, Designer In-Spire, Old Beggar, Knowing Skull, and Wheel of Change"}
    };

    // Neow is represented by the real native EVENT_SCREEN and Event::NEOW,
    // never by a fabricated ScreenState value.
    StepSimulator neow(CharacterClass::IRONCLAD, 49, 20);
    require(neow.gc.screenState == ScreenState::EVENT_SCREEN
                    && neow.gc.curEvent == Event::NEOW,
            "native start-of-run Neow state is not the expected EVENT_SCREEN");
    installTestMapContinuation(neow.gc);
    neow.gc.info.neowRewards[0] = {
            Neow::Bonus::HUNDRED_GOLD, Neow::Drawback::NONE};
    neow.gc.info.neowRewards[1] = {
            Neow::Bonus::TEN_PERCENT_HP_BONUS, Neow::Drawback::NO_GOLD};
    neow.gc.info.neowRewards[2] = {
            Neow::Bonus::REMOVE_CARD, Neow::Drawback::TEN_PERCENT_HP_LOSS};
    neow.gc.info.neowRewards[3] = {
            Neow::Bonus::THREE_ENEMY_KILL, Neow::Drawback::CURSE};
    const auto neowPublicBefore = neow.publicProjectionV3();
    const auto neowCheckpoint = neow.captureCheckpoint();
    const auto neowProjection = privilegedProjection(neow);
    const auto neowActions = verifyPrivilegedCandidateParity(neow, neowProjection);
    require(neow.publicProjectionV3().equal(neowPublicBefore),
            "separate privileged Neow projection mutated or replaced public projection");
    const auto neowEncodedBeforeReplay = encodePython(neowProjection);
    (void) neow.restoreCheckpoint(neowCheckpoint);
    require(encodePython(privilegedProjection(neow)) == neowEncodedBeforeReplay,
            "native checkpoint replay changed the deterministic Neow semantic projection");
    const auto neowChoices = neowProjection["candidates"].cast<pybind11::list>();
    const auto firstNeowFeatures = neowChoices[0].cast<pybind11::dict>()
            ["features"].cast<pybind11::dict>();
    const auto firstNeowChoice = firstNeowFeatures["choice"].cast<pybind11::dict>()
            ["value"].cast<pybind11::dict>();
    require(firstNeowChoice["bonus_effect_type"].cast<std::string>() == "obtain_100_gold"
                    && firstNeowChoice["drawback_effect_type"].cast<std::string>() == "no_drawback",
            "Neow option semantics do not match the native Bonus/Drawback enums");
    const int goldBeforeNeow = neow.gc.gold;
    auto executeNeow = neow;
    (void) executeNeow.step(neowActions[0]);
    require(executeNeow.gc.gold == goldBeforeNeow + 100,
            "identified Neow candidate did not execute the associated +100 gold option");
    result["coverage_matrix"]["EVENT_SCREEN:NEOW"] = {
        {"status", "supported"}, {"native_candidate_count", neowActions.size()},
        {"selection_meaning", "Bonus/Drawback enums and native source descriptions"},
        {"candidate_order_and_checkpoint_replay", true}, {"public_api_unchanged", true}
    };
    result["execution_witnesses"].push_back({
        {"family", "EVENT_SCREEN:NEOW"}, {"candidate_index", 0},
        {"effect", "GameContext::chooseNeowOption obtained 100 gold"}
    });

    // Cost-bearing event options prove both the event identity and the native
    // action's gold cost/next card-selection purpose.
    StepSimulator beggar(CharacterClass::IRONCLAD, 49, 20);
    enterNativeMapThroughNeow(beggar);
    beggar.gc.screenState = ScreenState::EVENT_SCREEN;
    beggar.gc.curEvent = Event::OLD_BEGGAR;
    beggar.gc.gold = 200;
    const auto beggarProjection = privilegedProjection(beggar);
    const auto beggarActions = verifyPrivilegedCandidateParity(beggar, beggarProjection);
    const auto payBeggarIndex = findGameCandidate(beggarActions, "event", 0);
    const auto beggarCandidates = beggarProjection["candidates"].cast<pybind11::list>();
    const auto beggarFeatures = beggarCandidates[payBeggarIndex].cast<pybind11::dict>()
            ["features"].cast<pybind11::dict>();
    const auto beggarChoice = beggarFeatures["choice"].cast<pybind11::dict>()
            ["value"].cast<pybind11::dict>();
    require(beggarChoice["gold_cost"].cast<int>() == 75
                    && beggarChoice["effect_type"].cast<std::string>() == "open_card_removal",
            "Old Beggar candidate omitted native cost or selection effect semantics");
    auto executeBeggar = beggar;
    (void) executeBeggar.step(beggarActions[payBeggarIndex]);
    require(executeBeggar.gc.gold == 125
                    && executeBeggar.gc.screenState == ScreenState::CARD_SELECT
                    && executeBeggar.gc.info.selectScreenType == CardSelectScreenType::REMOVE,
            "identified Old Beggar candidate did not pay 75 gold and open card removal");
    result["coverage_matrix"]["EVENT_SCREEN:OLD_BEGGAR"] = {
        {"status", "supported"}, {"native_candidate_count", beggarActions.size()},
        {"cost_and_next_screen", true}
    };
    result["execution_witnesses"].push_back({
        {"family", "EVENT_SCREEN:OLD_BEGGAR"}, {"candidate_index", payBeggarIndex},
        {"effect", "paid 75 gold and opened native CARD_SELECT:REMOVE"}
    });

    // Knowing Skull exposes native escalating HP costs and three distinct
    // reward effects, including two future-RNG outcomes tagged unavailable.
    StepSimulator knowingSkull(CharacterClass::IRONCLAD, 49, 20);
    enterNativeMapThroughNeow(knowingSkull);
    knowingSkull.gc.screenState = ScreenState::EVENT_SCREEN;
    knowingSkull.gc.curEvent = Event::KNOWING_SKULL;
    knowingSkull.gc.info.hpAmount0 = 2;
    knowingSkull.gc.info.hpAmount1 = 3;
    knowingSkull.gc.info.hpAmount2 = 4;
    const auto skullProjection = privilegedProjection(knowingSkull);
    const auto skullActions = verifyPrivilegedCandidateParity(knowingSkull, skullProjection);
    require(skullActions.size() == 4,
            "Knowing Skull fixture omitted a native option");
    const auto skullCandidates = skullProjection["candidates"].cast<pybind11::list>();
    const auto skullGoldChoice = skullCandidates[0].cast<pybind11::dict>()
            ["features"].cast<pybind11::dict>()["choice"].cast<pybind11::dict>()
            ["value"].cast<pybind11::dict>();
    require(skullGoldChoice["hp_cost"].cast<int>() == 2
                    && skullGoldChoice["gold_amount"].cast<int>() == 90,
            "Knowing Skull gold candidate omitted its native current cost or reward");
    auto executeSkullGold = knowingSkull;
    (void) executeSkullGold.step(skullActions[0]);
    require(executeSkullGold.gc.curHp == knowingSkull.gc.curHp - 2
                    && executeSkullGold.gc.gold == knowingSkull.gc.gold + 90,
            "identified Knowing Skull gold candidate did not execute its exact native cost and reward");
    auto executeSkullLeave = knowingSkull;
    (void) executeSkullLeave.step(skullActions[3]);
    require(executeSkullLeave.gc.curHp == knowingSkull.gc.curHp - 6
                    && executeSkullLeave.gc.screenState == ScreenState::MAP_SCREEN,
            "identified Knowing Skull leave candidate did not execute its native 6 HP cost");
    result["coverage_matrix"]["EVENT_SCREEN:KNOWING_SKULL"] = {
        {"status", "supported"}, {"native_candidate_count", skullActions.size()},
        {"escalating_hp_costs_and_reward_kinds", true}, {"native_execution_witnesses", 2}
    };
    result["execution_witnesses"].push_back({
        {"family", "EVENT_SCREEN:KNOWING_SKULL"}, {"candidate_index", 0},
        {"effect", "lost the configured 2 HP and obtained the native 90 gold"}
    });

    StepSimulator reward(CharacterClass::IRONCLAD, 49, 20);
    enterNativeMapThroughNeow(reward);
    reward.gc.screenState = ScreenState::REWARDS;
    reward.gc.info.rewardsContainer.clear();
    reward.gc.info.rewardsContainer.addGold(17);
    const auto rewardProjection = privilegedProjection(reward);
    const auto rewardActions = verifyPrivilegedCandidateParity(reward, rewardProjection);
    const auto goldRewardIndex = findGameCandidate(rewardActions, "reward_gold");
    auto executeGoldReward = reward;
    (void) executeGoldReward.step(rewardActions[goldRewardIndex]);
    require(executeGoldReward.gc.gold == reward.gc.gold + 17,
            "identified reward candidate did not grant its associated gold offer");
    result["coverage_matrix"]["REWARDS"] = {
        {"status", "supported"}, {"native_candidate_count", rewardActions.size()},
        {"candidate_to_visible_offer", true}
    };
    result["execution_witnesses"].push_back({
        {"family", "REWARDS"}, {"candidate_index", goldRewardIndex},
        {"effect", "native reward action granted the associated 17 gold"}
    });

    StepSimulator map(CharacterClass::IRONCLAD, 49, 20);
    enterNativeMapThroughNeow(map);
    const auto mapProjection = privilegedProjection(map);
    const auto mapActions = verifyPrivilegedCandidateParity(map, mapProjection);
    const auto mapIndex = findGameCandidate(mapActions, "map");
    const int expectedMapX = mapActions[mapIndex].idx1;
    const auto mapCandidates = mapProjection["candidates"].cast<pybind11::list>();
    const auto mapCandidate = mapCandidates[static_cast<ssize_t>(mapIndex)].cast<pybind11::dict>();
    const auto mapFeatures = mapCandidate["features"].cast<pybind11::dict>();
    const auto publicDestination = mapFeatures["public_destination"].cast<pybind11::dict>();
    require(publicDestination["visibility"].cast<std::string>() == "public_now",
            "map route omitted public destination visibility");
    const auto publicDestinationValue = publicDestination["value"].cast<pybind11::dict>();
    require(!publicDestinationValue.contains("underlying_room"),
            "map oracle room leaked inside its public destination feature");
    require(mapFeatures.contains("oracle_destination_room")
                    && mapFeatures["oracle_destination_room"].cast<pybind11::dict>()
                            ["visibility"].cast<std::string>() == "oracle_only",
            "map route omitted its separately tagged oracle room fact");
    auto executeMap = map;
    (void) executeMap.step(mapActions[mapIndex]);
    require(executeMap.gc.curMapNodeX == expectedMapX && executeMap.gc.curMapNodeY == 0,
            "identified map route did not enter its associated native destination");
    result["coverage_matrix"]["MAP_SCREEN"] = {
        {"status", "supported"}, {"native_candidate_count", mapActions.size()},
        {"public_and_oracle_destination_facts", true}
    };
    result["execution_witnesses"].push_back({
        {"family", "MAP_SCREEN"}, {"candidate_index", mapIndex},
        {"effect", "native transition entered the associated map node"}
    });

    StepSimulator boss(CharacterClass::IRONCLAD, 49, 20);
    enterNativeMapThroughNeow(boss);
    boss.gc.screenState = ScreenState::BOSS_RELIC_REWARDS;
    for (int index = 0; index < 3; ++index) {
        boss.gc.info.bossRelics[index] = boss.gc.returnRandomRelic(RelicTier::BOSS);
    }
    const auto bossProjection = privilegedProjection(boss);
    const auto bossActions = verifyPrivilegedCandidateParity(boss, bossProjection);
    const auto relicIndex = findGameCandidate(bossActions, "boss_relic", 0);
    const auto offeredRelic = boss.gc.info.bossRelics[0];
    auto executeBossRelic = boss;
    (void) executeBossRelic.step(bossActions[relicIndex]);
    require(executeBossRelic.gc.relics.has(offeredRelic),
            "identified boss-relic candidate did not award its associated offer");
    const auto bossSkipIndex = findGameCandidate(bossActions, "boss_relic", 3);
    auto executeBossSkip = boss;
    (void) executeBossSkip.step(bossActions[bossSkipIndex]);
    require(executeBossSkip.gc.screenState == ScreenState::MAP_SCREEN,
            "native boss-relic skip occurrence did not execute as skip");
    result["coverage_matrix"]["BOSS_RELIC_REWARDS"] = {
        {"status", "supported"}, {"native_candidate_count", bossActions.size()},
        {"offered_relic_and_skip_semantics", true}
    };
    result["execution_witnesses"].push_back({
        {"family", "BOSS_RELIC_REWARDS"}, {"candidate_index", relicIndex},
        {"effect", "native executor awarded the relic at the associated offer slot"}
    });

    StepSimulator cardSelect(CharacterClass::IRONCLAD, 49, 20);
    enterNativeMapThroughNeow(cardSelect);
    cardSelect.gc.openCardSelectScreen(CardSelectScreenType::UPGRADE, 1);
    const auto cardSelectProjection = privilegedProjection(cardSelect);
    const auto cardSelectActions = verifyPrivilegedCandidateParity(
            cardSelect, cardSelectProjection);
    std::map<int, std::size_t> selectableCardCounts;
    for (const auto &target : cardSelect.gc.info.toSelectCards) {
        ++selectableCardCounts[static_cast<int>(target.card.getId())];
    }
    const bool hasSameKindCandidates = std::any_of(
            selectableCardCounts.begin(), selectableCardCounts.end(),
            [](const auto &entry) { return entry.second > 1; });
    require(hasSameKindCandidates,
            "card-select native fixture omitted duplicate same-kind card occurrences");
    const auto selectIndex = 0U;
    const auto cardSelectRows = cardSelectProjection["candidates"].cast<pybind11::list>();
    const auto selectedFeatures = cardSelectRows[selectIndex].cast<pybind11::dict>()
            ["features"].cast<pybind11::dict>();
    const int selectedDeckIndex = selectedFeatures["target_deck_index"].cast<pybind11::dict>()
            ["value"].cast<int>();
    auto executeCardSelect = cardSelect;
    (void) executeCardSelect.step(cardSelectActions[selectIndex]);
    require(selectedDeckIndex >= 0
                    && executeCardSelect.gc.deck.cards[selectedDeckIndex].isUpgraded(),
            "identified card-select candidate did not upgrade its exact deck occurrence");
    result["coverage_matrix"]["CARD_SELECT"] = {
        {"status", "supported"}, {"native_candidate_count", cardSelectActions.size()},
        {"duplicate_same_kind_occurrences_preserved", true}, {"target_deck_index", selectedDeckIndex}
    };
    result["execution_witnesses"].push_back({
        {"family", "CARD_SELECT"}, {"candidate_index", selectIndex},
        {"effect", "native UPGRADE action changed the associated physical deck occurrence"}
    });

    StepSimulator rest(CharacterClass::IRONCLAD, 49, 20);
    enterNativeMapThroughNeow(rest);
    rest.gc.screenState = ScreenState::REST_ROOM;
    const auto restProjection = privilegedProjection(rest);
    const auto restActions = verifyPrivilegedCandidateParity(rest, restProjection);
    const auto smithIndex = findGameCandidate(restActions, "rest", 1);
    auto executeSmith = rest;
    (void) executeSmith.step(restActions[smithIndex]);
    require(executeSmith.gc.screenState == ScreenState::CARD_SELECT
                    && executeSmith.gc.info.selectScreenType == CardSelectScreenType::UPGRADE,
            "identified Rest Smith candidate did not open native upgrade selection");
    const auto smithTargetProjection = privilegedProjection(executeSmith);
    const auto smithTargetActions = verifyPrivilegedCandidateParity(
            executeSmith, smithTargetProjection);
    const auto smithTarget = smithTargetProjection["candidates"].cast<pybind11::list>()
            [0].cast<pybind11::dict>()["features"].cast<pybind11::dict>()
            ["target_deck_index"].cast<pybind11::dict>()["value"].cast<int>();
    auto executeSmithTarget = executeSmith;
    (void) executeSmithTarget.step(smithTargetActions[0]);
    require(executeSmithTarget.gc.deck.cards[smithTarget].isUpgraded(),
            "Smith target candidate did not upgrade its associated card");
    result["coverage_matrix"]["REST_ROOM"] = {
        {"status", "supported"}, {"native_candidate_count", restActions.size()},
        {"smith_and_card_target", true}
    };
    result["execution_witnesses"].push_back({
        {"family", "REST_ROOM"}, {"candidate_index", smithIndex},
        {"effect", "native Smith opened CARD_SELECT:UPGRADE and selected target executed"}
    });

    StepSimulator shop(CharacterClass::IRONCLAD, 49, 20);
    enterNativeMapThroughNeow(shop);
    shop.gc.gold = 1000;
    shop.gc.screenState = ScreenState::SHOP_ROOM;
    shop.gc.info.shop.setup(shop.gc);
    const auto shopProjection = privilegedProjection(shop);
    const auto shopActions = verifyPrivilegedCandidateParity(shop, shopProjection);
    const auto shopCardIndex = findGameCandidate(shopActions, "shop_reward_card");
    const auto shopCardSlot = shopActions[shopCardIndex].idx1;
    const int shopCardPrice = shop.gc.info.shop.cardPrice(shopCardSlot);
    const int shopDeckSize = shop.gc.deck.size();
    auto executeShopCard = shop;
    (void) executeShopCard.step(shopActions[shopCardIndex]);
    require(executeShopCard.gc.gold == shop.gc.gold - shopCardPrice
                    && executeShopCard.gc.deck.size() == shopDeckSize + 1,
            "identified shop-card candidate did not buy its associated card at its price");
    const auto shopPotionIndex = findGameCandidate(shopActions, "shop_reward_potion");
    const auto shopPotionSlot = shopActions[shopPotionIndex].idx1;
    const int shopPotionPrice = shop.gc.info.shop.potionPrice(shopPotionSlot);
    const int shopPotionCount = shop.gc.potionCount;
    auto executeShopPotion = shop;
    (void) executeShopPotion.step(shopActions[shopPotionIndex]);
    require(executeShopPotion.gc.gold == shop.gc.gold - shopPotionPrice
                    && executeShopPotion.gc.potionCount == shopPotionCount + 1,
            "identified shop-potion candidate did not buy its associated potion at its price");
    const auto shopRemoveIndex = findGameCandidate(shopActions, "shop_card_remove");
    const int shopRemoveCost = shop.gc.info.shop.removeCost;
    auto executeShopRemove = shop;
    (void) executeShopRemove.step(shopActions[shopRemoveIndex]);
    require(executeShopRemove.gc.gold == shop.gc.gold - shopRemoveCost
                    && executeShopRemove.gc.screenState == ScreenState::CARD_SELECT
                    && executeShopRemove.gc.info.selectScreenType == CardSelectScreenType::REMOVE,
            "identified shop-removal candidate did not pay cost and open card targets");
    const auto shopRemoveTargetProjection = privilegedProjection(executeShopRemove);
    const auto shopRemoveTargetActions = verifyPrivilegedCandidateParity(
            executeShopRemove, shopRemoveTargetProjection);
    const auto removedDeckIndex = shopRemoveTargetProjection["candidates"].cast<pybind11::list>()
            [0].cast<pybind11::dict>()["features"].cast<pybind11::dict>()
            ["target_deck_index"].cast<pybind11::dict>()["value"].cast<int>();
    auto executeShopRemoveTarget = executeShopRemove;
    (void) executeShopRemoveTarget.step(shopRemoveTargetActions[0]);
    require(executeShopRemoveTarget.gc.deck.size() == executeShopRemove.gc.deck.size() - 1,
            "shop removal target candidate did not remove exactly one deck card");
    result["coverage_matrix"]["SHOP_ROOM"] = {
        {"status", "supported"}, {"native_candidate_count", shopActions.size()},
        {"item_costs_and_card_remove_target", true}, {"removed_deck_index", removedDeckIndex}
    };
    result["execution_witnesses"].push_back({
        {"family", "SHOP_ROOM"}, {"candidate_index", shopRemoveIndex},
        {"effect", "native Shop charged the visible removal cost and the associated card target executed"}
    });

    StepSimulator treasure(CharacterClass::IRONCLAD, 49, 20);
    enterNativeMapThroughNeow(treasure);
    treasure.gc.screenState = ScreenState::TREASURE_ROOM;
    treasure.gc.info.chestSize = ChestSize::MEDIUM;
    treasure.gc.info.tier = RelicTier::RARE;
    treasure.gc.info.haveGold = false;
    const auto treasurePublicBefore = treasure.publicProjectionV3();
    const auto treasureProjection = privilegedProjection(treasure);
    const auto treasureActions = verifyPrivilegedCandidateParity(treasure, treasureProjection);
    const auto treasureRows = treasureProjection["candidates"].cast<pybind11::list>();
    const auto treasureFeatures = treasureRows[0].cast<pybind11::dict>()
            ["features"].cast<pybind11::dict>();
    require(treasureFeatures["chest_size"].cast<pybind11::dict>()
                    ["visibility"].cast<std::string>() == "public_now"
                    && treasureFeatures["relic_tier"].cast<pybind11::dict>()
                            ["visibility"].cast<std::string>() == "oracle_only"
                    && treasureFeatures["unrevealed_loot"].cast<pybind11::dict>()
                            ["availability"].cast<std::string>() == "unavailable",
            "treasure projection failed to separate visible chest type and unrevealed loot");
    StepSimulator differentHiddenTier = treasure;
    differentHiddenTier.gc.info.tier = RelicTier::COMMON;
    require(treasurePublicBefore.equal(differentHiddenTier.publicProjectionV3()),
            "hidden chest relic tier leaked into normal public projection");
    require(encodePython(treasureProjection)
                    != encodePython(privilegedProjection(differentHiddenTier)),
            "training oracle projection omitted its explicitly tagged hidden chest tier");
    const auto treasureOpenIndex = findGameCandidate(treasureActions, "treasure_open");
    auto executeTreasureOpen = treasure;
    (void) executeTreasureOpen.step(treasureActions[treasureOpenIndex]);
    require(executeTreasureOpen.gc.screenState == ScreenState::REWARDS,
            "identified treasure-open candidate did not materialize the native reward screen");
    const auto treasureLeaveIndex = findGameCandidate(treasureActions, "treasure_leave");
    auto executeTreasureLeave = treasure;
    (void) executeTreasureLeave.step(treasureActions[treasureLeaveIndex]);
    require(executeTreasureLeave.gc.screenState == ScreenState::MAP_SCREEN,
            "identified treasure-leave candidate did not leave the chest unopened");
    result["coverage_matrix"]["TREASURE_ROOM"] = {
        {"status", "supported"}, {"native_candidate_count", treasureActions.size()},
        {"visible_chest_type", "MEDIUM"}, {"oracle_only_tier", "Rare"},
        {"exact_loot_unavailable_before_open", true}, {"public_projection_hidden_state_invariant", true}
    };
    result["execution_witnesses"].push_back({
        {"family", "TREASURE_ROOM"}, {"candidate_index", treasureOpenIndex},
        {"effect", "opening the associated candidate materialized native REWARDS"}
    });
    result["execution_witnesses"].push_back({
        {"family", "TREASURE_ROOM"}, {"candidate_index", treasureLeaveIndex},
        {"effect", "leaving the associated candidate returned to MAP_SCREEN without opening"}
    });

    StepSimulator potions(CharacterClass::IRONCLAD, 49, 20);
    enterNativeMapThroughNeow(potions);
    potions.gc.obtainPotion(Potion::FRUIT_JUICE);
    potions.gc.obtainPotion(Potion::ANCIENT_POTION);
    const auto potionProjection = privilegedProjection(potions);
    const auto potionActions = verifyPrivilegedCandidateParity(potions, potionProjection);
    const auto useFruitIndex = findGameCandidate(potionActions, "game_potion_use", 0);
    const auto discardAncientIndex = findGameCandidate(potionActions, "game_potion_discard", 1);
    const int hpBeforeFruit = potions.gc.maxHp;
    auto executeFruit = potions;
    (void) executeFruit.step(potionActions[useFruitIndex]);
    require(executeFruit.gc.maxHp == hpBeforeFruit + 5,
            "identified Fruit Juice candidate did not increase maximum HP by its native effect");
    auto executeAncientDiscard = potions;
    (void) executeAncientDiscard.step(potionActions[discardAncientIndex]);
    require(executeAncientDiscard.gc.potions[1] == Potion::EMPTY_POTION_SLOT
                    && executeAncientDiscard.gc.potionCount == potions.gc.potionCount - 1,
            "identified inventory discard candidate did not empty the associated potion slot");
    result["coverage_matrix"]["GAME_POTION_USE_DISCARD"] = {
        {"status", "supported"}, {"native_candidate_count", potionActions.size()},
        {"use_and_discard_execution", true}
    };
    result["execution_witnesses"].push_back({
        {"family", "GAME_POTION_USE_DISCARD"}, {"candidate_index", useFruitIndex},
        {"effect", "native Fruit Juice action increased maximum HP"}
    });
    result["execution_witnesses"].push_back({
        {"family", "GAME_POTION_USE_DISCARD"}, {"candidate_index", discardAncientIndex},
        {"effect", "native inventory discard emptied the exact potion slot"}
    });

    result["coverage_status"] = "NATIVE_CHOICE_SEMANTIC_NOT_READY";
    result["unsupported_scope"] = {
        "COLOSSEUM remains fail-closed if forced into EVENT_SCREEN; native generation disables it and GameContext::chooseEventOption is a TODO with no distinct option effects",
        "LAB has a single legal-action bit but its native chooser asserts that no option is valid; it is not a multi-choice family",
        "independent native Reviewer information-boundary/provenance report is pending",
        "per-family execution witnesses are representative rather than an execution of every event branch"
    };
    result["no_training_or_evaluation_ran"] = true;
    return result;
}

}  // namespace

int main(const int argc, char **argv) {
    try {
        scoped_interpreter interpreter{};
        if (argc == 2 && std::string(argv[1]) == "--privileged-noncombat-only") {
            std::cout << checkPrivilegedNoncombatProjection().dump() << std::endl;
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--privileged-projection-benchmark") {
            std::cout << checkPrivilegedProjectionPerformance().dump() << std::endl;
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--smoke-bomb-only") {
            nlohmann::json smokeEvidence;
            checkSmokeBombBattleProjection(smokeEvidence);
            std::cout << smokeEvidence.dump() << std::endl;
            return 0;
        }
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
        checkSapphireRewardCoupling(evidence["sapphire_reward_coupling"]);

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
        evidence["smoke_bomb_battle_projection"] = nlohmann::json::object();
        checkSmokeBombBattleProjection(evidence["smoke_bomb_battle_projection"]);

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
