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

nlohmann::json eventRngContinuationState(const GameContext &gc) {
    const auto streamState = [](const sts::Random &stream) {
        auto continuation = stream;
        return nlohmann::json{
            {"counter", stream.counter},
            {"seed0", stream.seed0},
            {"seed1", stream.seed1},
            {"next_random_1000", continuation.random(1000)}
        };
    };
    return {
        {"misc", streamState(gc.miscRng)},
        {"relic", streamState(gc.relicRng)}
    };
}

nlohmann::json eventDeckState(const GameContext &gc) {
    nlohmann::json state = nlohmann::json::array();
    for (int index = 0; index < gc.deck.size(); ++index) {
        state.push_back({
            {"deck_index", index},
            {"card_id", static_cast<int>(gc.deck.cards[index].getId())},
            {"bottled", gc.deck.isCardBottled(index)}
        });
    }
    return state;
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

nlohmann::json checkBottledEventExclusions() {
    struct FallingCase {
        const char *name;
        CardId bottledCard;
        CardId removableCard;
        CardType type;
        int eventOption;
    };
    const std::vector<FallingCase> fallingCases = {
        {"attack", CardId::ANGER, CardId::BASH, CardType::ATTACK, 2},
        {"skill", CardId::ARMAMENTS, CardId::BURNING_PACT, CardType::SKILL, 0},
        {"power", CardId::INFLAME, CardId::BARRICADE, CardType::POWER, 1}
    };
    nlohmann::json evidence = nlohmann::json::array();

    int seed = 6900;
    for (const auto &testCase : fallingCases) {
        StepSimulator fixture(CharacterClass::IRONCLAD, seed++, 20);
        auto &gc = fixture.gc;
        gc.deck = Deck{};
        gc.deck.obtainRaw(Card(testCase.bottledCard));
        gc.deck.obtainRaw(Card(testCase.removableCard));
        gc.deck.bottleCard(0, testCase.type);
        gc.curEvent = Event::FALLING;
        gc.setupEvent();
        installTestMapContinuation(gc);

        const int selectedDeckIdx = testCase.type == CardType::ATTACK
                ? gc.info.attackCardDeckIdx
                : testCase.type == CardType::SKILL
                        ? gc.info.skillCardDeckIdx
                        : gc.info.powerCardDeckIdx;
        require(selectedDeckIdx == 1,
                std::string("Falling selected a bottled ") + testCase.name
                        + " card or omitted the only eligible card");
        const auto actions = fixture.legalActions();
        const auto selectedAction = findGameCandidate(actions, "event", testCase.eventOption);
        (void) fixture.step(actions[selectedAction]);
        require(gc.deck.size() == 1
                        && gc.deck.cards[0].getId() == testCase.bottledCard
                        && gc.deck.isCardBottled(0),
                std::string("Falling removal did not preserve the bottled ")
                        + testCase.name + " card");
        evidence.push_back({
            {"family", "FALLING"}, {"card_type", testCase.name},
            {"bottled_deck_index", 0}, {"selected_deck_index", selectedDeckIdx},
            {"native_event_option", testCase.eventOption},
            {"bottled_card_preserved_after_native_choice", true}
        });
    }

    StepSimulator allBottledFalling(CharacterClass::IRONCLAD, 6904, 20);
    auto &allBottledGc = allBottledFalling.gc;
    allBottledGc.deck = Deck{};
    allBottledGc.deck.obtainRaw(Card(CardId::BASH));
    allBottledGc.deck.obtainRaw(Card(CardId::ARMAMENTS));
    allBottledGc.deck.obtainRaw(Card(CardId::INFLAME));
    allBottledGc.deck.bottleCard(0, CardType::ATTACK);
    allBottledGc.deck.bottleCard(1, CardType::SKILL);
    allBottledGc.deck.bottleCard(2, CardType::POWER);
    allBottledGc.curEvent = Event::FALLING;
    installTestMapContinuation(allBottledGc);
    allBottledGc.setupEvent();
    require(allBottledGc.info.attackCardDeckIdx == -1
                    && allBottledGc.info.skillCardDeckIdx == -1
                    && allBottledGc.info.powerCardDeckIdx == -1,
            "Falling selected a card when every card of each type was bottled");
    const auto allBottledActions = allBottledFalling.legalActions();
    require(allBottledActions.size() == 1
                    && allBottledActions.front().kind == "event"
                    && allBottledActions.front().idx1 == 3,
            "Falling exposed a removal action when every candidate was bottled");
    (void) allBottledFalling.step(allBottledActions.front());
    require(allBottledGc.deck.size() == 3
                    && allBottledGc.deck.isCardBottled(0)
                    && allBottledGc.deck.isCardBottled(1)
                    && allBottledGc.deck.isCardBottled(2),
            "leaving all-bottled Falling changed the deck or a bottle attachment");
    evidence.push_back({
        {"family", "FALLING"}, {"all_types_ineligible", true},
        {"attack_selected_deck_index", -1}, {"skill_selected_deck_index", -1},
        {"power_selected_deck_index", -1}, {"native_legal_action_count", 1},
        {"only_leave_option_exposed", true}, {"all_bottled_cards_preserved", true}
    });

    StepSimulator fallingReplay(CharacterClass::IRONCLAD, 6905, 20);
    auto &fallingReplayGc = fallingReplay.gc;
    fallingReplayGc.deck = Deck{};
    fallingReplayGc.deck.obtainRaw(Card(CardId::BASH));
    fallingReplayGc.deck.obtainRaw(Card(CardId::ARMAMENTS));
    fallingReplayGc.deck.obtainRaw(Card(CardId::INFLAME));
    fallingReplayGc.deck.obtainRaw(Card(CardId::ANGER));
    fallingReplayGc.deck.obtainRaw(Card(CardId::POMMEL_STRIKE));
    fallingReplayGc.deck.obtainRaw(Card(CardId::BURNING_PACT));
    fallingReplayGc.deck.obtainRaw(Card(CardId::SHRUG_IT_OFF));
    fallingReplayGc.deck.obtainRaw(Card(CardId::BARRICADE));
    fallingReplayGc.deck.obtainRaw(Card(CardId::DARK_EMBRACE));
    fallingReplayGc.deck.bottleCard(0, CardType::ATTACK);
    fallingReplayGc.deck.bottleCard(1, CardType::SKILL);
    fallingReplayGc.deck.bottleCard(2, CardType::POWER);
    fallingReplayGc.curEvent = Event::FALLING;
    installTestMapContinuation(fallingReplayGc);
    const auto fallingCheckpoint = fallingReplay.captureCheckpoint();
    auto runFallingReplay = [&]() {
        fallingReplayGc.setupEvent();
        const std::array<int, 3> selectedDeckIndices = {
            fallingReplayGc.info.attackCardDeckIdx,
            fallingReplayGc.info.skillCardDeckIdx,
            fallingReplayGc.info.powerCardDeckIdx
        };
        const std::array<CardType, 3> expectedTypes = {
            CardType::ATTACK, CardType::SKILL, CardType::POWER
        };
        nlohmann::json selectedCards = nlohmann::json::array();
        for (std::size_t index = 0; index < selectedDeckIndices.size(); ++index) {
            const int deckIndex = selectedDeckIndices[index];
            require(deckIndex >= 3 && deckIndex < fallingReplayGc.deck.size()
                            && !fallingReplayGc.deck.isCardBottled(deckIndex)
                            && fallingReplayGc.deck.cards[deckIndex].getType() == expectedTypes[index],
                    "Falling replay selected a bottled, missing, or wrong-type card");
            selectedCards.push_back({
                {"deck_index", deckIndex},
                {"card_id", static_cast<int>(fallingReplayGc.deck.cards[deckIndex].getId())}
            });
        }
        const auto setupRng = eventRngContinuationState(fallingReplayGc);
        const auto actions = fallingReplay.legalActions();
        const auto selectedAttackId = fallingReplayGc.deck.cards[selectedDeckIndices[0]].getId();
        const auto attackActionIndex = findGameCandidate(actions, "event", 2);
        (void) fallingReplay.step(actions[attackActionIndex]);
        require(fallingReplayGc.screenState == ScreenState::MAP_SCREEN
                        && fallingReplayGc.deck.size() == 8
                        && std::none_of(fallingReplayGc.deck.cards.begin(),
                                fallingReplayGc.deck.cards.begin() + fallingReplayGc.deck.size(),
                                [&](const Card &card) { return card.getId() == selectedAttackId; }),
                "Falling replay did not execute exactly the selected attack removal");
        for (const auto bottledId : {CardId::BASH, CardId::ARMAMENTS, CardId::INFLAME}) {
            bool preserved = false;
            for (int index = 0; index < fallingReplayGc.deck.size(); ++index) {
                if (fallingReplayGc.deck.cards[index].getId() == bottledId
                        && fallingReplayGc.deck.isCardBottled(index)) {
                    preserved = true;
                    break;
                }
            }
            require(preserved, "Falling replay did not preserve a bottle attachment");
        }
        return nlohmann::json{
            {"selected_cards_by_type", selectedCards},
            {"setup_rng", setupRng},
            {"executed_event_action", actions[attackActionIndex].idx1},
            {"post_action_deck", eventDeckState(fallingReplayGc)},
            {"continuation_rng", eventRngContinuationState(fallingReplayGc)}
        };
    };
    const auto firstFallingReplay = runFallingReplay();
    (void) fallingReplay.restoreCheckpoint(fallingCheckpoint);
    const auto secondFallingReplay = runFallingReplay();
    require(firstFallingReplay == secondFallingReplay,
            "Falling setup/choice checkpoint replay changed card selection, bottle state, or RNG continuation");
    evidence.push_back({
        {"family", "FALLING"}, {"checkpoint_replay", true},
        {"checkpoint_stage", "before_setupEvent"}, {"replay", firstFallingReplay}
    });

    // A duplicated card is a single visible identity in this event.  When
    // its first copy is bottled, index normalization must choose the first
    // eligible copy rather than mapping back to the bottled one.
    StepSimulator duplicated(CharacterClass::IRONCLAD, 6903, 20);
    auto &duplicateGc = duplicated.gc;
    duplicateGc.deck = Deck{};
    duplicateGc.deck.obtainRaw(Card(CardId::ANGER));
    duplicateGc.deck.obtainRaw(Card(CardId::ANGER));
    duplicateGc.deck.bottleCard(0, CardType::ATTACK);
    duplicateGc.obtainPotion(Potion::FRUIT_JUICE);
    duplicateGc.gold = 100;
    duplicateGc.curEvent = Event::WE_MEET_AGAIN;
    duplicateGc.setupEvent();
    installTestMapContinuation(duplicateGc);
    require(duplicateGc.info.cardIdx == 1,
            "We Meet Again mapped an eligible duplicate back to its bottled first copy");
    const auto duplicateActions = duplicated.legalActions();
    const auto duplicateAction = findGameCandidate(duplicateActions, "event", 2);
    (void) duplicated.step(duplicateActions[duplicateAction]);
    require(duplicateGc.deck.size() == 1
                    && duplicateGc.deck.cards[0].getId() == CardId::ANGER
                    && duplicateGc.deck.isCardBottled(0),
            "We Meet Again card trade removed the bottled duplicate");
    evidence.push_back({
        {"family", "WE_MEET_AGAIN"}, {"bottled_duplicate_deck_index", 0},
        {"eligible_duplicate_deck_index", 1},
        {"selected_deck_index", 1},
        {"bottled_card_preserved_after_native_choice", true}
    });

    StepSimulator noEligibleCard(CharacterClass::IRONCLAD, 6904, 20);
    auto &noEligibleGc = noEligibleCard.gc;
    noEligibleGc.deck = Deck{};
    noEligibleGc.deck.obtainRaw(Card(CardId::ANGER));
    noEligibleGc.deck.bottleCard(0, CardType::ATTACK);
    noEligibleGc.deck.obtainRaw(Card(CardId::BASH));
    noEligibleGc.obtainPotion(Potion::FRUIT_JUICE);
    noEligibleGc.gold = 100;
    noEligibleGc.curEvent = Event::WE_MEET_AGAIN;
    noEligibleGc.setupEvent();
    require(noEligibleGc.info.cardIdx == -1,
            "We Meet Again offered a card when its only nonbasic choice was bottled");
    const auto noEligibleActions = noEligibleCard.legalActions();
    require(std::none_of(noEligibleActions.begin(), noEligibleActions.end(),
                    [](const LightSpeedAction &action) {
                        return action.kind == "event" && action.idx1 == 2;
                    }),
            "We Meet Again exposed the card-trade option without an eligible card");
    evidence.push_back({
        {"family", "WE_MEET_AGAIN"}, {"bottled_nonbasic_deck_index", 0},
        {"unbottled_basic_deck_index", 1}, {"selected_deck_index", -1},
        {"card_trade_option_exposed", false}
    });

    StepSimulator meetAgainReplay(CharacterClass::IRONCLAD, 6906, 20);
    auto &meetAgainReplayGc = meetAgainReplay.gc;
    meetAgainReplayGc.deck = Deck{};
    meetAgainReplayGc.deck.obtainRaw(Card(CardId::ANGER));
    meetAgainReplayGc.deck.obtainRaw(Card(CardId::ANGER));
    meetAgainReplayGc.deck.bottleCard(0, CardType::ATTACK);
    meetAgainReplayGc.obtainPotion(Potion::FRUIT_JUICE);
    meetAgainReplayGc.gold = 100;
    meetAgainReplayGc.curEvent = Event::WE_MEET_AGAIN;
    installTestMapContinuation(meetAgainReplayGc);
    const auto meetAgainCheckpoint = meetAgainReplay.captureCheckpoint();
    const int initialRelicCount = meetAgainReplayGc.relics.size();
    auto runMeetAgainReplay = [&]() {
        meetAgainReplayGc.setupEvent();
        require(meetAgainReplayGc.info.cardIdx == 1
                        && !meetAgainReplayGc.deck.isCardBottled(meetAgainReplayGc.info.cardIdx),
                "We Meet Again replay did not select the eligible unbottled duplicate");
        const auto selectedDeckIndex = meetAgainReplayGc.info.cardIdx;
        const auto selectedCardId = meetAgainReplayGc.deck.cards[selectedDeckIndex].getId();
        const auto setupRng = eventRngContinuationState(meetAgainReplayGc);
        const auto actions = meetAgainReplay.legalActions();
        const auto tradeActionIndex = findGameCandidate(actions, "event", 2);
        (void) meetAgainReplay.step(actions[tradeActionIndex]);
        require(meetAgainReplayGc.screenState == ScreenState::MAP_SCREEN
                        && meetAgainReplayGc.deck.size() == 1
                        && meetAgainReplayGc.deck.cards[0].getId() == CardId::ANGER
                        && meetAgainReplayGc.deck.isCardBottled(0)
                        && meetAgainReplayGc.relics.size() == initialRelicCount + 1,
                "We Meet Again replay did not trade the selected duplicate while preserving its bottle");
        return nlohmann::json{
            {"selected_deck_index", selectedDeckIndex},
            {"selected_card_id", static_cast<int>(selectedCardId)},
            {"setup_rng", setupRng},
            {"executed_event_action", actions[tradeActionIndex].idx1},
            {"post_action_deck", eventDeckState(meetAgainReplayGc)},
            {"post_action_relic_count", meetAgainReplayGc.relics.size()},
            {"continuation_rng", eventRngContinuationState(meetAgainReplayGc)}
        };
    };
    const auto firstMeetAgainReplay = runMeetAgainReplay();
    (void) meetAgainReplay.restoreCheckpoint(meetAgainCheckpoint);
    const auto secondMeetAgainReplay = runMeetAgainReplay();
    require(firstMeetAgainReplay == secondMeetAgainReplay,
            "We Meet Again setup/choice checkpoint replay changed selection, bottle state, or RNG continuation");
    evidence.push_back({
        {"family", "WE_MEET_AGAIN"}, {"checkpoint_replay", true},
        {"checkpoint_stage", "before_setupEvent"}, {"replay", firstMeetAgainReplay}
    });
    return evidence;
}

nlohmann::json checkRequiredEventMechanics() {
    nlohmann::json evidence = {
        {"coverage_matrix", nlohmann::json::object()},
        {"execution_witnesses", nlohmann::json::array()},
        {"mechanism_inventory", nlohmann::json::array()}
    };

    const auto bottledEventWitnesses = checkBottledEventExclusions();
    evidence["coverage_matrix"]["EVENT_SCREEN:FALLING_BOTTLED_CARD_EXCLUSION"] = {
        {"status", "supported"},
        {"classification", "REQUIRED_REPAIR_COMPLETED"},
        {"native_witness_count", 5}
    };
    evidence["coverage_matrix"]["EVENT_SCREEN:WE_MEET_AGAIN_BOTTLED_CARD_EXCLUSION"] = {
        {"status", "supported"},
        {"classification", "REQUIRED_REPAIR_COMPLETED"},
        {"native_witness_count", 3}
    };
    for (const auto &witness : bottledEventWitnesses) {
        evidence["execution_witnesses"].push_back(witness);
    }

    StepSimulator restrictedShard(CharacterClass::IRONCLAD, 49, 20);
    const PrivilegedNoncombatProjectionAccess shardAccess(
            restrictedShard, "training_oracle_only");
    const auto noShardQualification = shardAccess.trainingPopulationQualificationV1();
    require(noShardQualification["schema_id"].cast<std::string>()
                    == "native-training-population-qualification-v1"
                    && noShardQualification["native_source_commit"].cast<std::string>()
                            == STS_LIGHTSPEED_TESTED_SOURCE_COMMIT
                    && noShardQualification["restriction"].cast<std::string>()
                            == "PRISMATIC_SHARD_DISABLED / CROSS_CLASS_CARDS_OUT_OF_SCOPE"
                    && noShardQualification["disable_prismatic_shard"].cast<bool>()
                    && !noShardQualification["prismatic_shard_present"].cast<bool>()
                    && noShardQualification["eligible_for_restricted_no_shard_population"].cast<bool>(),
            "source-bound no-Shard training restriction is missing or ineligible at baseline");
    require(noShardQualification["real_game_trace_status"].cast<std::string>()
                    == "REAL_GAME_TRACE_NOT_VERIFIED"
                    && noShardQualification["live_game_parity_status"].cast<std::string>()
                            == "LIVE_PARITY_NOT_VERIFIED"
                    && !noShardQualification["training_authorized_by_this_api"].cast<bool>(),
            "restricted training marker implied a live parity result or training authorization");

    restrictedShard.gc.obtainRelic(RelicId::PRISMATIC_SHARD);
    const auto shardExposureQualification = shardAccess.trainingPopulationQualificationV1();
    require(shardExposureQualification["prismatic_shard_present"].cast<bool>()
                    && !shardExposureQualification["eligible_for_restricted_no_shard_population"].cast<bool>()
                    && shardExposureQualification["status"].cast<std::string>()
                            == "PRISMATIC_SHARD_EXPOSURE_REJECTED"
                    && shardExposureQualification["on_shard_exposure"].cast<std::string>()
                            == "STOP_OR_APPLY_PREDECLARED_EXCLUSION_CENSORING",
            "Prismatic Shard exposure did not fail closed for the restricted training population");

    StepSimulator shardCardPool(CharacterClass::IRONCLAD, 73, 20);
    shardCardPool.gc.obtainRelic(RelicId::PRISMATIC_SHARD);
    const auto shardCardReward = shardCardPool.gc.createCardReward(Room::EVENT);
    std::vector<int> shardRewardColors;
    for (const auto &card : shardCardReward) {
        const auto color = getCardColor(card.id);
        shardRewardColors.push_back(static_cast<int>(color));
        require(color == CardColor::RED,
                "disabled Prismatic Shard card reward left the Ironclad-only reward pool");
    }
    require(!shardRewardColors.empty(),
            "Prismatic Shard staging-effect probe produced no card reward");
    evidence["coverage_matrix"]["PRISMATIC_SHARD"] = {
        {"status", "RESTRICTED_TRAINING_EXCEPTION"},
        {"cross_class_card_behavior_disabled", GameContext::disablePrismaticShard},
        {"card_pool_source", "GameContext::createCardReward uses getRandomClassCardOfRarity while disabled"},
        {"shard_exposure_status", shardExposureQualification["status"].cast<std::string>()},
        {"exposure_policy", shardExposureQualification["on_shard_exposure"].cast<std::string>()},
        {"observed_ironclad_reward_colors_with_shard_present", shardRewardColors},
        {"eligibility", "Prismatic Shard remains obtainable from the standard relic population"},
        {"native_legal_choices", "relic choice remains a normal relic reward; no cross-class card-pool action is exposed"},
        {"post_action_effect", "holding Shard does not broaden Ironclad card rewards in the staged simulator"},
        {"rng_save_restore", "qualification marker is source-bound; no Shard-specific RNG or checkpoint witness is claimed"},
        {"severity", "restricted training exception; unrestricted real-game population is blocked"},
        {"evidence", "native Shard-present card-reward probe returned only Ironclad RED cards; exact source marker rejects observed Shard exposure"},
        {"unrestricted_real_game_qualification", "BLOCKED_UNTIL_PRISMATIC_SHARD_RESTORATION"},
        {"real_game_trace_status", "REAL_GAME_TRACE_NOT_VERIFIED"},
        {"live_game_parity_status", "LIVE_PARITY_NOT_VERIFIED"}
    };
    evidence["mechanism_inventory"].push_back({
        {"mechanism", "PRISMATIC_SHARD"},
        {"reachability", "relic is obtainable in the standard relic population"},
        {"native_switch_or_todo", "GameContext::disablePrismaticShard in createCardReward"},
        {"effect", "held relic does not enable cross-class card rewards"},
        {"classification", "RESTRICTED_TRAINING_EXCEPTION"},
        {"eligibility", "standard relic reward population; mechanic is restricted by project stage policy, not hidden from generation"},
        {"native_legal_choices", "normal relic acquisition remains legal; cross-class reward identities stay Ironclad-only"},
        {"post_action_or_terminal_effect", "training qualification fails closed on Shard exposure; no censoring is performed here"},
        {"rng_save_restore", "card reward pool behavior was sampled, not claimed as a cross-version RNG parity proof"},
        {"severity", "restricted training exception; full real-game qualification remains blocked"},
        {"evidence", "GameContext::disablePrismaticShard and exact-source trainingPopulationQualificationV1 marker"}
    });

    StepSimulator colosseum(CharacterClass::IRONCLAD, 49, 20);
    auto &colosseumGc = colosseum.gc;
    colosseumGc.act = 2;
    colosseumGc.curMapNodeX = 3;
    colosseumGc.curMapNodeY = 8;
    *colosseumGc.map = Map::fromSeed(colosseumGc.seed, colosseumGc.ascension, 2, true);
    colosseumGc.eventList = {Event::COLOSSEUM};
    colosseumGc.shrineList = {Event::ANCIENT_WRITING};
    colosseumGc.potionChance = 100;
    const bool colosseumCanBeAdded = colosseumGc.canAddEvent(Event::COLOSSEUM);
    const Event selectedEvent = colosseumGc.getEvent(colosseumGc.eventRng);
    require(colosseumCanBeAdded && selectedEvent == Event::COLOSSEUM
                    && !GameContext::disableColosseum,
            "Act 2 Colosseum was not eligible through the native event pool");
    colosseumGc.curRoom = Room::EVENT;
    colosseumGc.curEvent = selectedEvent;
    colosseumGc.info.rewardsContainer.clear();
    installTestMapContinuation(colosseumGc);
    colosseumGc.setupEvent();
    require(colosseumGc.screenState == ScreenState::BATTLE
                    && colosseumGc.info.encounter == MonsterEncounter::COLOSSEUM_EVENT_SLAVERS
                    && colosseumGc.info.eventData == 0,
            "Colosseum setup did not start the native Slavers encounter");
    GameContext expectedColosseumHiddenRoll = colosseumGc;
    Rewards expectedColosseumHiddenPotion;
    expectedColosseumHiddenRoll.addPotionRewards(expectedColosseumHiddenPotion);
    require(expectedColosseumHiddenPotion.potionCount == 1,
            "Colosseum first-fight hidden-roll fixture did not guarantee a potion drop");
    (void) colosseum.legalActions(); // initialize the real BattleContext for this encounter
    colosseum.bc.outcome = Outcome::PLAYER_VICTORY;
    colosseum.bc.exitBattle(colosseumGc); // native battle-end and event continuation
    require(colosseumGc.screenState == ScreenState::EVENT_SCREEN
                    && colosseumGc.info.eventData == 1
                    && colosseumGc.info.rewardsContainer.getTotalCount() == 0,
            "first Colosseum victory did not expose leave/challenge without ordinary loot");
    require(colosseumGc.potionChance == expectedColosseumHiddenRoll.potionChance
                    && colosseumGc.potionRng.counter
                            == expectedColosseumHiddenRoll.potionRng.counter,
            "first Colosseum victory did not consume the hidden potion roll");
    const auto colosseumPhaseOneProjection = privilegedProjection(colosseum);
    const auto colosseumChoices = verifyPrivilegedCandidateParity(
            colosseum, colosseumPhaseOneProjection);
    require(colosseumChoices.size() == 2
                    && colosseumPhaseOneProjection["coverage_status"].cast<std::string>() == "supported",
            "Colosseum leave/challenge projection did not cover its native legal choices");
    const auto colosseumCandidateRows = colosseumPhaseOneProjection["candidates"]
            .cast<pybind11::list>();
    const auto leaveFeatures = colosseumCandidateRows[0].cast<pybind11::dict>()
            ["features"].cast<pybind11::dict>();
    const auto challengeFeatures = colosseumCandidateRows[1].cast<pybind11::dict>()
            ["features"].cast<pybind11::dict>();
    require(leaveFeatures["choice"].cast<pybind11::dict>()["value"].cast<pybind11::dict>()
                    ["effect_type"].cast<std::string>() == "return_to_map_without_event_reward"
                    && challengeFeatures["choice"].cast<pybind11::dict>()["value"].cast<pybind11::dict>()
                            ["effect_type"].cast<std::string>() == "enter_COLOSSEUM_EVENT_NOBS",
            "Colosseum projection did not describe its distinct leave and challenge effects");
    const auto phaseOneCheckpoint = colosseum.captureCheckpoint();
    const int goldBeforeLeave = colosseumGc.gold;
    const auto leaveIndex = findGameCandidate(colosseumChoices, "event", 0);
    (void) colosseum.step(colosseumChoices[leaveIndex]);
    require(colosseumGc.screenState == ScreenState::MAP_SCREEN
                    && colosseumGc.gold == goldBeforeLeave
                    && colosseumGc.info.rewardsContainer.getTotalCount() == 0,
            "leaving Colosseum after the Slavers did not return without rewards");
    (void) colosseum.restoreCheckpoint(phaseOneCheckpoint);

    auto executeSecondFightAndSummarize = [&]() {
        const auto choices = colosseum.legalActions();
        const auto challengeIndex = findGameCandidate(choices, "event", 1);
        (void) colosseum.step(choices[challengeIndex]);
        require(colosseumGc.screenState == ScreenState::BATTLE
                        && colosseumGc.info.encounter == MonsterEncounter::COLOSSEUM_EVENT_NOBS
                        && colosseumGc.info.eventData == 2,
                "Colosseum challenge did not enter the native Taskmaster/Nob fight");
        (void) colosseum.legalActions();
        colosseumGc.afterBattle();
        require(colosseumGc.screenState == ScreenState::REWARDS,
                "second Colosseum victory did not enter its reward screen");
        const auto &reward = colosseumGc.info.rewardsContainer;
        require(reward.goldRewardCount == 1 && reward.gold[0] == 100
                        && reward.relicCount == 2
                        && getRelicTier(reward.relics[0]) == RelicTier::UNCOMMON
                        && getRelicTier(reward.relics[1]) == RelicTier::RARE
                        && reward.cardRewardCount == 1
                        && reward.cardRewards[0].size() > 0,
                "second Colosseum victory did not produce its event-specific rewards");
        nlohmann::json summary = {
            {"gold", reward.gold[0]},
            {"relics", {static_cast<int>(reward.relics[0]), static_cast<int>(reward.relics[1])}},
            {"card_reward", nlohmann::json::array()},
            {"potion_count", reward.potionCount},
            {"potion_chance", colosseumGc.potionChance},
            {"relic_rng_counter", colosseumGc.relicRng.counter},
            {"card_rng_counter", colosseumGc.cardRng.counter},
            {"potion_rng_counter", colosseumGc.potionRng.counter},
            {"misc_rng_counter", colosseumGc.miscRng.counter}
        };
        for (const auto &card : reward.cardRewards[0]) {
            summary["card_reward"].push_back(static_cast<int>(card.id));
        }
        return summary;
    };
    const auto firstRewardReplay = executeSecondFightAndSummarize();
    (void) colosseum.restoreCheckpoint(phaseOneCheckpoint);
    const auto secondRewardReplay = executeSecondFightAndSummarize();
    require(firstRewardReplay == secondRewardReplay,
            "Colosseum checkpoint replay changed its reward or RNG transition");
    evidence["coverage_matrix"]["EVENT_SCREEN:COLOSSEUM"] = {
        {"status", "supported"},
        {"reachability_classification", "actual_reachable_act2_event"},
        {"native_switch_disabled", GameContext::disableColosseum},
        {"can_add_at_event_floor", colosseumCanBeAdded},
        {"opening_encounter", "COLOSSEUM_EVENT_SLAVERS"},
        {"post_victory_native_choices", 2},
        {"first_fight_ordinary_rewards", false},
        {"second_encounter", "COLOSSEUM_EVENT_NOBS"},
        {"first_fight_smoke_bomb_continues_event", true},
        {"first_fight_hidden_potion_roll_consumed_but_not_shown", true},
        {"second_reward_replay", firstRewardReplay},
        {"checkpoint_replay_exact", true},
        {"real_game_trace_status", "REAL_GAME_TRACE_NOT_VERIFIED"}
    };
    evidence["execution_witnesses"].push_back({
        {"family", "EVENT_SCREEN:COLOSSEUM"},
        {"effect", "native pool eligibility, first fight continuation, distinct leave/challenge actions, second fight, and fixed event rewards"}
    });
    evidence["mechanism_inventory"].push_back({
        {"mechanism", "COLOSSEUM"},
        {"reachability", "Act 2 event after floor 7"},
        {"native_switch_or_todo", "GameContext::disableColosseum and chooseEventOption"},
        {"effect", "two encounter event, first victory no reward, leave or challenge"},
        {"classification", "REQUIRED_REPAIR_COMPLETED"},
        {"eligibility", "native Act 2 event pool after floor 7; no A20 exclusion"},
        {"native_legal_choices", "post-Slavers EVENT_SCREEN exposes leave and challenge; challenge enters Taskmaster/Nob"},
        {"post_action_or_terminal_effect", "first victory or Smoke Bomb reaches event choice with hidden potion RNG roll and no ordinary loot; second victory awards fixed event rewards"},
        {"rng_save_restore", "first-fight potion RNG/pity is advanced; second-fight relic/card/gold/potion reward replay matches checkpoint"},
        {"severity", "P0 required mechanism repaired; live-game parity trace remains unverified"},
        {"evidence", "Act 2 pool setup, native battle exit, first-fight victory/Smoke Bomb regressions, leave/challenge, second-reward checkpoint replay"}
    });

    StepSimulator matchAndKeep(CharacterClass::IRONCLAD, 49, 20);
    auto &matchGc = matchAndKeep.gc;
    matchGc.act = 1;
    matchGc.curMapNodeX = 3;
    matchGc.curMapNodeY = 1;
    matchGc.shrineList = {Event::MATCH_AND_KEEP};
    matchGc.specialOneTimeEventList.clear();
    const Event selectedShrine = matchGc.getShrine(matchGc.eventRng);
    require(selectedShrine == Event::MATCH_AND_KEEP
                    && !GameContext::disableMatchAndKeep,
            "Match and Keep was not reachable through the native shrine pool");
    matchGc.curRoom = Room::EVENT;
    matchGc.curEvent = selectedShrine;
    installTestMapContinuation(matchGc);
    matchGc.setupEvent();
    require(matchGc.screenState == ScreenState::EVENT_SCREEN
                    && matchGc.info.eventData == 5
                    && matchGc.info.toSelectCards.size() == 12,
            "Match and Keep setup did not initialize its native 12-card, five-attempt board");
    const int startingDeckSize = matchGc.deck.size();
    const auto firstPickActions = matchAndKeep.legalActions();
    require(firstPickActions.size() == 12,
            "Match and Keep did not enumerate one first-pick action per concealed card");
    require(!search::GameAction(0, 1).isValidAction(matchGc)
                    && !search::GameAction(12).isValidAction(matchGc),
            "Match and Keep accepted a pair action or an out-of-range first pick");

    const auto publicBeforeHiddenMutation = matchAndKeep.publicProjectionV3();
    const auto hiddenSlot = firstPickActions.front().idx1;
    const auto originalHiddenCard = matchGc.info.toSelectCards[hiddenSlot].card;
    const auto alternateCard = originalHiddenCard.id == CardId::BASH
            ? Card(CardId::IRON_WAVE) : Card(CardId::BASH);
    matchGc.info.toSelectCards[hiddenSlot].card = alternateCard;
    const auto publicAfterHiddenMutation = matchAndKeep.publicProjectionV3();
    require(publicBeforeHiddenMutation.equal(publicAfterHiddenMutation),
            "normal-public Match and Keep projection changed when a hidden card identity changed");
    matchGc.info.toSelectCards[hiddenSlot].card = originalHiddenCard;
    const auto matchPublicPayload = screenPayload(publicBeforeHiddenMutation);
    require(matchPublicPayload["coverage_status"].cast<std::string>() == "supported"
                    && matchPublicPayload["phase"].cast<std::string>() == "select_first_card"
                    && matchPublicPayload["choices"].cast<pybind11::list>().size() == 12,
            "normal-public Match and Keep payload does not describe first-card choices");
    const auto publicSlots = matchPublicPayload["cards"].cast<pybind11::list>();
    require(publicSlots.size() == 12
                    && !publicSlots[hiddenSlot].cast<pybind11::dict>().contains("card"),
            "normal-public Match and Keep exposed a face-down card identity");

    const auto matchProjection = privilegedProjection(matchAndKeep);
    const auto checkedFirstPickActions = verifyPrivilegedCandidateParity(
            matchAndKeep, matchProjection);
    require(checkedFirstPickActions.size() == 12,
            "privileged Match and Keep projection did not preserve first-pick candidates");
    const auto firstCandidateRows = matchProjection["candidates"].cast<pybind11::list>();
    const auto firstCandidateFeatures = firstCandidateRows[0].cast<pybind11::dict>()
            ["features"].cast<pybind11::dict>();
    require(firstCandidateFeatures["choice"].cast<pybind11::dict>()
                            ["value"].cast<pybind11::dict>()
                            ["choice_type"].cast<std::string>() == "reveal_first_card"
                    && firstCandidateFeatures["revealed_card_identity"].cast<pybind11::dict>()
                            ["visibility"].cast<std::string>() == "oracle_only",
            "privileged first-pick projection did not label the card reveal as an oracle-only transition outcome");
    std::size_t mismatchFirstActionIndex = checkedFirstPickActions.size();
    int mismatchSecondIndex = -1;
    for (std::size_t firstActionIndex = 0;
            firstActionIndex < checkedFirstPickActions.size(); ++firstActionIndex) {
        const int firstIndex = checkedFirstPickActions[firstActionIndex].idx1;
        for (const auto &secondAction : checkedFirstPickActions) {
            const int secondIndex = secondAction.idx1;
            if (firstIndex != secondIndex
                    && matchGc.info.toSelectCards[firstIndex].card.id
                            != matchGc.info.toSelectCards[secondIndex].card.id) {
                mismatchFirstActionIndex = firstActionIndex;
                mismatchSecondIndex = secondIndex;
                break;
            }
        }
        if (mismatchFirstActionIndex != checkedFirstPickActions.size()) {
            break;
        }
    }
    require(mismatchFirstActionIndex < checkedFirstPickActions.size(),
            "Match and Keep fixture has no mismatching two-step selection");
    const int mismatchFirstIndex = checkedFirstPickActions[mismatchFirstActionIndex].idx1;
    (void) matchAndKeep.step(checkedFirstPickActions[mismatchFirstActionIndex]);
    const auto intermediatePublicProjection = matchAndKeep.publicProjectionV3();
    const auto intermediatePublicPayload = screenPayload(intermediatePublicProjection);
    const auto intermediateCards = intermediatePublicPayload["cards"].cast<pybind11::list>();
    require(matchGc.info.eventData == 5
                    && matchGc.info.toSelectCards[mismatchFirstIndex].deckIdx
                            == static_cast<std::int16_t>(MatchAndKeepCardState::FIRST_PICK_REVEALED)
                    && matchGc.deck.size() == startingDeckSize
                    && intermediatePublicPayload["phase"].cast<std::string>() == "select_second_card"
                    && intermediatePublicPayload["first_card_index"].cast<int>() == mismatchFirstIndex
                    && intermediateCards[mismatchFirstIndex].cast<pybind11::dict>()
                            ["state"].cast<std::string>() == "first_pick_revealed"
                    && intermediateCards[mismatchFirstIndex].cast<pybind11::dict>().contains("card")
                    && intermediateCards[mismatchFirstIndex].cast<pybind11::dict>()
                            ["card"].cast<pybind11::dict>()["id"].cast<int>()
                            == static_cast<int>(matchGc.info.toSelectCards[mismatchFirstIndex].card.getId())
                    && intermediateCards[mismatchSecondIndex].cast<pybind11::dict>()
                            ["state"].cast<std::string>() == "face_down"
                    && !intermediateCards[mismatchSecondIndex].cast<pybind11::dict>().contains("card"),
            "first Match and Keep selection did not reveal only its own card before the second choice");
    const auto secondPickActions = matchAndKeep.legalActions();
    require(secondPickActions.size() == 11
                    && std::none_of(secondPickActions.begin(), secondPickActions.end(),
                            [mismatchFirstIndex](const auto &action) {
                                return action.idx1 == mismatchFirstIndex;
                            })
                    && !search::GameAction(mismatchFirstIndex).isValidAction(matchGc),
            "Match and Keep second-pick legal choices did not exclude the revealed first card");
    const int hiddenAfterFirstPick = secondPickActions.front().idx1 == mismatchSecondIndex
            ? secondPickActions.back().idx1 : secondPickActions.front().idx1;
    const auto hiddenAfterFirstPickOriginal = matchGc.info.toSelectCards[hiddenAfterFirstPick].card;
    matchGc.info.toSelectCards[hiddenAfterFirstPick].card =
            hiddenAfterFirstPickOriginal.id == CardId::BASH
                    ? Card(CardId::IRON_WAVE) : Card(CardId::BASH);
    require(intermediatePublicProjection.equal(matchAndKeep.publicProjectionV3()),
            "normal-public Match and Keep second-pick observation changed when an unselected hidden card changed");
    matchGc.info.toSelectCards[hiddenAfterFirstPick].card = hiddenAfterFirstPickOriginal;
    const auto secondPickProjection = privilegedProjection(matchAndKeep);
    const auto checkedSecondPickActions = verifyPrivilegedCandidateParity(
            matchAndKeep, secondPickProjection);
    require(checkedSecondPickActions.size() == 11,
            "privileged Match and Keep projection did not preserve second-pick candidates");
    const auto secondCandidateRows = secondPickProjection["candidates"].cast<pybind11::list>();
    const auto secondCandidateFeatures = secondCandidateRows[0].cast<pybind11::dict>()
            ["features"].cast<pybind11::dict>();
    require(secondCandidateFeatures["choice"].cast<pybind11::dict>()
                            ["value"].cast<pybind11::dict>()
                            ["choice_type"].cast<std::string>() == "select_second_card"
                    && secondCandidateFeatures["first_card_identity"].cast<pybind11::dict>()
                            ["visibility"].cast<std::string>() == "public_now"
                    && secondCandidateFeatures["second_card_identity"].cast<pybind11::dict>()
                            ["visibility"].cast<std::string>() == "oracle_only",
            "privileged second-pick projection did not separate public first-card and hidden second-card identities");

    const auto checkpoint = matchAndKeep.captureCheckpoint();
    const auto publicBeforeSecondPick = encodePython(intermediatePublicProjection);
    const auto privilegedBeforeSecondPick = encodePython(secondPickProjection);
    const auto mismatchRngBefore = std::array<std::int32_t, 3>{
            matchGc.cardRng.counter, matchGc.miscRng.counter, matchGc.eventRng.counter};
    std::size_t mismatchSecondActionIndex = checkedSecondPickActions.size();
    for (std::size_t index = 0; index < checkedSecondPickActions.size(); ++index) {
        if (checkedSecondPickActions[index].idx1 == mismatchSecondIndex) {
            mismatchSecondActionIndex = index;
            break;
        }
    }
    require(mismatchSecondActionIndex < checkedSecondPickActions.size(),
            "Match and Keep did not expose the selected mismatching second card");
    (void) matchAndKeep.step(checkedSecondPickActions[mismatchSecondActionIndex]);
    require(matchGc.info.eventData == 4
                    && matchGc.info.toSelectCards[mismatchFirstIndex].deckIdx
                            == static_cast<std::int16_t>(MatchAndKeepCardState::REVEALED_MISMATCH)
                    && matchGc.info.toSelectCards[mismatchSecondIndex].deckIdx
                            == static_cast<std::int16_t>(MatchAndKeepCardState::REVEALED_MISMATCH)
                    && matchGc.deck.size() == startingDeckSize,
            "Match and Keep mismatch did not resolve the pair and consume exactly one attempt");
    const auto publicAfterMismatch = encodePython(matchAndKeep.publicProjectionV3());
    const auto mismatchRngAfter = std::array<std::int32_t, 3>{
            matchGc.cardRng.counter, matchGc.miscRng.counter, matchGc.eventRng.counter};
    require(mismatchRngBefore == mismatchRngAfter,
            "Match and Keep two-step card selection unexpectedly advanced card/event RNG");
    (void) matchAndKeep.restoreCheckpoint(checkpoint);
    require(matchGc.info.eventData == 5
                    && matchGc.info.toSelectCards[mismatchFirstIndex].deckIdx
                            == static_cast<std::int16_t>(MatchAndKeepCardState::FIRST_PICK_REVEALED)
                    && mismatchRngBefore == std::array<std::int32_t, 3>{
                            matchGc.cardRng.counter, matchGc.miscRng.counter, matchGc.eventRng.counter}
                    && encodePython(matchAndKeep.publicProjectionV3()) == publicBeforeSecondPick
                    && encodePython(privilegedProjection(matchAndKeep)) == privilegedBeforeSecondPick,
            "Match and Keep checkpoint did not replay the intermediate observation, choices and RNG");
    std::size_t replaySecondActionIndex = matchAndKeep.legalActions().size();
    const auto replaySecondActions = matchAndKeep.legalActions();
    for (std::size_t index = 0; index < replaySecondActions.size(); ++index) {
        if (replaySecondActions[index].idx1 == mismatchSecondIndex) {
            replaySecondActionIndex = index;
            break;
        }
    }
    require(replaySecondActionIndex < replaySecondActions.size(),
            "restored Match and Keep intermediate board did not recreate its second-pick action");
    (void) matchAndKeep.step(replaySecondActions[replaySecondActionIndex]);
    require(matchGc.info.eventData == 4
                    && encodePython(matchAndKeep.publicProjectionV3()) == publicAfterMismatch,
            "replayed Match and Keep second pick did not reproduce the mismatch observation");

    auto chooseFirstForMatch = [&]() {
        const auto actions = matchAndKeep.legalActions();
        std::size_t firstActionIndex = actions.size();
        int secondIndex = -1;
        for (std::size_t firstIndex = 0; firstIndex < actions.size(); ++firstIndex) {
            for (const auto &candidateSecond : actions) {
                if (actions[firstIndex].idx1 != candidateSecond.idx1
                        && matchGc.info.toSelectCards[actions[firstIndex].idx1].card.id
                                == matchGc.info.toSelectCards[candidateSecond.idx1].card.id) {
                    firstActionIndex = firstIndex;
                    secondIndex = candidateSecond.idx1;
                    break;
                }
            }
            if (firstActionIndex != actions.size()) break;
        }
        require(firstActionIndex < actions.size(),
                "Match and Keep fixture has no remaining matching two-step selection");
        const int firstIndex = actions[firstActionIndex].idx1;
        (void) matchAndKeep.step(actions[firstActionIndex]);
        const auto choices = matchAndKeep.legalActions();
        std::size_t secondActionIndex = choices.size();
        for (std::size_t index = 0; index < choices.size(); ++index) {
            if (choices[index].idx1 == secondIndex) {
                secondActionIndex = index;
                break;
            }
        }
        require(secondActionIndex < choices.size(),
                "Match and Keep hid the matching second card after its first reveal");
        const int deckBeforeMatch = matchGc.deck.size();
        (void) matchAndKeep.step(choices[secondActionIndex]);
        require(matchGc.deck.size() == deckBeforeMatch + 1
                        && matchGc.info.toSelectCards[firstIndex].deckIdx
                                == static_cast<std::int16_t>(MatchAndKeepCardState::MATCHED_HIDDEN)
                        && matchGc.info.toSelectCards[secondIndex].deckIdx
                                == static_cast<std::int16_t>(MatchAndKeepCardState::MATCHED_HIDDEN),
                "Match and Keep match did not award exactly one card and hide the pair");
    };
    chooseFirstForMatch();
    int matchedRewards = 1;

    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto actions = matchAndKeep.legalActions();
        require(!actions.empty(), "Match and Keep exhausted first-pick actions before five attempts");
        const auto firstAction = actions.front();
        const int firstIndex = firstAction.idx1;
        const int deckBefore = matchGc.deck.size();
        (void) matchAndKeep.step(firstAction);
        const auto secondActions = matchAndKeep.legalActions();
        require(secondActions.size() == actions.size() - 1,
                "Match and Keep did not expose one second-pick action per other face-down card");
        std::size_t secondActionIndex = 0;
        for (std::size_t index = 0; index < secondActions.size(); ++index) {
            if (matchGc.info.toSelectCards[firstIndex].card.id
                    == matchGc.info.toSelectCards[secondActions[index].idx1].card.id) {
                secondActionIndex = index;
                break;
            }
            if (secondActionIndex == 0) secondActionIndex = index;
        }
        const int secondIndex = secondActions[secondActionIndex].idx1;
        const bool isMatch = matchGc.info.toSelectCards[firstIndex].card.id
                == matchGc.info.toSelectCards[secondIndex].card.id;
        (void) matchAndKeep.step(secondActions[secondActionIndex]);
        if (isMatch) {
            ++matchedRewards;
            require(matchGc.deck.size() == deckBefore + 1
                            && matchGc.info.toSelectCards[firstIndex].deckIdx
                                    == static_cast<std::int16_t>(MatchAndKeepCardState::MATCHED_HIDDEN)
                            && matchGc.info.toSelectCards[secondIndex].deckIdx
                                    == static_cast<std::int16_t>(MatchAndKeepCardState::MATCHED_HIDDEN),
                    "Match and Keep match did not award exactly one card and hide the pair");
        } else {
            require(matchGc.deck.size() == deckBefore
                            && matchGc.info.toSelectCards[firstIndex].deckIdx
                                    == static_cast<std::int16_t>(MatchAndKeepCardState::REVEALED_MISMATCH)
                            && matchGc.info.toSelectCards[secondIndex].deckIdx
                                    == static_cast<std::int16_t>(MatchAndKeepCardState::REVEALED_MISMATCH),
                    "Match and Keep mismatch changed the deck or failed to reveal both cards");
        }
    }
    require(matchGc.screenState == ScreenState::MAP_SCREEN
                    && matchGc.info.eventData == 0
                    && matchGc.info.toSelectCards.empty()
                    && matchGc.deck.size() == startingDeckSize + matchedRewards,
            "Match and Keep did not end after five attempts and resume the map continuation");
    evidence["coverage_matrix"]["EVENT_SCREEN:MATCH_AND_KEEP"] = {
        {"status", "supported"},
        {"reachability_classification", "actual_reachable_act1_shrine"},
        {"native_switch_disabled", GameContext::disableMatchAndKeep},
        {"starting_first_pick_actions", 12},
        {"second_pick_actions_after_first_reveal", 11},
        {"pair_actions_rejected", true},
        {"first_card_revealed_before_second_choice", true},
        {"public_projection_hides_other_face_down_card_identities", true},
        {"privileged_first_and_second_choice_candidate_counts", {checkedFirstPickActions.size(), checkedSecondPickActions.size()}},
        {"mismatch_match_and_five_attempt_progression", true},
        {"matching_pair_card_award", true},
        {"checkpoint_rng_replay", true},
        {"matches_awarded", matchedRewards},
        {"real_game_trace_status", "REAL_GAME_TRACE_NOT_VERIFIED"}
    };
    evidence["execution_witnesses"].push_back({
        {"family", "EVENT_SCREEN:MATCH_AND_KEEP"},
        {"effect", "native shrine eligibility, first-card reveal transition, identity-conditioned second-card choices, card acquisition, attempt exhaustion and map continuation"}
    });
    evidence["mechanism_inventory"].push_back({
        {"mechanism", "MATCH_AND_KEEP"},
        {"reachability", "Act 1/2/3 shrine pools"},
        {"native_switch_or_todo", "GameContext::disableMatchAndKeep and GameAction event action enumeration"},
        {"effect", "12 shuffled cards, pair matching, five attempts, card acquisition"},
        {"classification", "REQUIRED_REPAIR_COMPLETED"},
        {"eligibility", "native shrine pool in Acts 1, 2 and 3; A20 remains eligible"},
        {"native_legal_choices", "12 first-card choices, then 11 second-card choices after the first selected identity is revealed; each completed pair consumes one attempt"},
        {"post_action_or_terminal_effect", "matching pairs award the card once; after five attempts the event ends and returns to map"},
        {"rng_save_restore", "native misc/card RNG checkpoint replay is exercised by the event fixture"},
        {"severity", "P0 required mechanism repaired; live-game parity trace remains unverified"},
        {"evidence", "native shrine eligibility, separate first and second pick enumeration, match and mismatch execution, five-attempt end, public hidden-card invariance, intermediate checkpoint replay"}
    });

    evidence["mechanism_inventory"].push_back({
        {"mechanism", "FALLING_BOTTLED_CARD_EXCLUSION"},
        {"reachability", "Act 3 standard event pool; no A20 exclusion in the inspected eligibility path"},
        {"native_switch_or_todo", "GameContext::setupEvent FALLING now builds type-specific candidate indices after excluding Deck::isCardBottled"},
        {"eligibility", "reachable Act 3 event; samples one eligible attack, skill, and power independently when present"},
        {"native_legal_choices", "remove one selected unbottled attack, skill, or power, or leave"},
        {"post_action_or_terminal_effect", "focused native fixture executes each type's selected removal and preserves the bottled card"},
        {"rng_save_restore", "before-setup checkpoint replay reselects the same attack/skill/power candidates, preserves bottle attachments after removal, and reproduces misc/relic RNG state plus next draws"},
        {"severity", "scoped native repair completed; live-game parity trace remains unverified"},
        {"evidence", "native attack/skill/power fixtures execute selected removals; all-bottled fixture exposes only leave; before-setup checkpoint replay reproduces candidate indices, deck/bottle state and RNG continuation"},
        {"classification", "REQUIRED_REPAIR_COMPLETED"}
    });
    evidence["mechanism_inventory"].push_back({
        {"mechanism", "WE_MEET_AGAIN_BOTTLED_CARD_SELECTION"},
        {"reachability", "one-time event listed in both Ascension 0 and Ascension 15 event sets"},
        {"native_switch_or_todo", "GameContext::setupEvent WE_MEET_AGAIN and getRandomPlayerNonBasicCardIdx now exclude bottled cards"},
        {"eligibility", "one-time event can be inserted by the native one-time-event path at A20"},
        {"native_legal_choices", "trade a potion, gold or an unbottled nonbasic noncurse card for a relic, or decline"},
        {"post_action_or_terminal_effect", "native duplicate-card fixture trades the eligible second copy while preserving the bottled first copy; no eligible card omits the card option"},
        {"rng_save_restore", "before-setup checkpoint replay reproduces the unbottled duplicate selection, post-trade deck/bottle state, relic count, misc/relic RNG state and next draws"},
        {"severity", "scoped native repair completed; live-game parity trace remains unverified"},
        {"evidence", "native fixtures verify bottled duplicate index normalization, executed removal of the unbottled copy, absence of a card trade when only bottled nonbasic cards are present, and exact before-setup checkpoint replay"},
        {"classification", "REQUIRED_REPAIR_COMPLETED"}
    });
    evidence["mechanism_inventory"].push_back({
        {"mechanism", "MYSTERIOUS_SPHERE_GOLDEN_IDOL_EVENT_GOLD"},
        {"reachability", "Act 3 standard event pool; the encounter is entered by the event choice"},
        {"native_switch_or_todo", "GameContext::afterBattle MYSTERIOUS_SPHERE comment asks TODO gold from Golden Idol?"},
        {"eligibility", "reachable Act 3 event fight; Golden Idol is obtainable in the standard relic pool"},
        {"native_legal_choices", "fight for event rewards or take the event's direct gold option"},
        {"post_action_or_terminal_effect", "victory grants stored event gold and rare relic; whether Golden Idol scales this event gold remains GAME_RULE_UNRESOLVED"},
        {"rng_save_restore", "event gold and rare relic are rolled before battle; no Golden Idol-specific replay witness was added here"},
        {"severity", "P2 unresolved reachable reward parity blocker"},
        {"evidence", "Act3::events includes MYSTERIOUS_SPHERE; afterBattle adds info.gold directly and retains the source TODO"},
        {"classification", "GAME_RULE_UNRESOLVED"}
    });
    evidence["mechanism_inventory"].push_back({
        {"mechanism", "PRAYER_WHEEL_COMBAT_REWARD"},
        {"reachability", "Prayer Wheel is an obtainable relic and may be held at any normal monster fight"},
        {"native_switch_or_todo", "createCombatReward contains a stale TODO Prayer wheel comment followed by an explicit relic check"},
        {"eligibility", "normal monster-room combat rewards"},
        {"native_legal_choices", "the standard card reward action set contains the additional card reward"},
        {"post_action_or_terminal_effect", "hasRelic(PRAYER_WHEEL) adds a second independently generated card reward"},
        {"rng_save_restore", "each reward uses the native card reward RNG path; not separately replay-tested in this issue"},
        {"severity", "NO_ISSUE within inspected reward path"},
        {"evidence", "GameContext::createCombatReward adds a second createCardReward when Prayer Wheel is present"},
        {"classification", "NO_ISSUE"}
    });
    evidence["mechanism_inventory"].push_back({
        {"mechanism", "STOLEN_GOLD_REWARD_ORDER"},
        {"reachability", "normal monster battles against Looter/Mugger encounters"},
        {"native_switch_or_todo", "GameContext::afterBattle TODO notes the display ordering of stolen gold"},
        {"eligibility", "triggered only when native battle state reports nonzero stolenGold"},
        {"native_legal_choices", "gold is represented in the ordinary reward screen; ordering is not a player action"},
        {"post_action_or_terminal_effect", "the stolen amount is added to the reward; the TODO concerns ordering only"},
        {"rng_save_restore", "no additional RNG use is associated with this presentation-order TODO"},
        {"severity", "NO_ISSUE for mechanics within inspected path; presentation ordering not audited"},
        {"evidence", "afterBattle appends the exact info.stolenGold amount when nonzero"},
        {"classification", "NO_ISSUE"}
    });
    evidence["mechanism_inventory"].push_back({
        {"mechanism", "CURSED_TOME_NECRONOMICON_SETUP_COMMENT"},
        {"reachability", "Act 2 CURSED_TOME event remains in the standard event pool at A20"},
        {"native_switch_or_todo", "setupEvent has a generic TODO necronomicon event comment; CURSED_TOME has an implemented branch"},
        {"eligibility", "normal Act 2 event eligibility"},
        {"native_legal_choices", "Cursed Tome phase choices include continue/leave and the book reward path"},
        {"post_action_or_terminal_effect", "book reward path selects Necronomicon, Enchiridion or Nilry's Codex and opens its reward screen"},
        {"rng_save_restore", "book identity uses miscRng; no separate replay witness was added here"},
        {"severity", "NO_ISSUE within inspected event path; the comment is not a separate missing event"},
        {"evidence", "Events::CURSED_TOME is in Act2::events and chooseEventOption implements all three book outcomes"},
        {"classification", "NO_ISSUE"}
    });
    evidence["mechanism_inventory"].push_back({
        {"mechanism", "DEAD_ADVENTURER_REWARD_OPTION_MAPPING"},
        {"reachability", "Act 1 standard event pool after floor 6"},
        {"native_switch_or_todo", "chooseEventOption comment says TODO map onto unique idxs"},
        {"eligibility", "standard Act 1 event eligibility after floor 6; no A20 exclusion"},
        {"native_legal_choices", "three stored shuffled reward kinds are exposed by phase; unsupported reward/phase values fail closed in semantic projection"},
        {"post_action_or_terminal_effect", "native option index selects the mapped gold/card/relic or encounter path"},
        {"rng_save_restore", "reward order is shuffled during setup with miscRng; no new replay claim is made here"},
        {"severity", "NO_ISSUE within this bounded scan; not a full event-branch audit"},
        {"evidence", "Act1::events includes DEAD_ADVENTURER; existing projection fixture rejects out-of-range reward kinds and phases"},
        {"classification", "NO_ISSUE"}
    });

    evidence["real_game_trace_status"] = "REAL_GAME_TRACE_NOT_VERIFIED";
    evidence["live_game_parity_status"] = "LIVE_PARITY_NOT_VERIFIED";
    evidence["overall_parity_status"] = "REAL_GAME_PARITY_RESTRICTED_PRISMATIC_SHARD";
    evidence["training_authorized"] = false;
    evidence["unrestricted_real_game_qualification"] =
            "BLOCKED_UNTIL_PRISMATIC_SHARD_RESTORATION";
    evidence["source_disabled_mechanism_inventory_scope"] = {
        {"method", "bounded source scan of active GameContext disable* switches and material TODO/unsupported annotations in event setup/choice, battle-end and combat-reward paths"},
        {"inspected_paths", {
            "include/game/GameContext.h disable* declarations",
            "include/constants/Events.h standard act and one-time event pools",
            "GameContext::canAddEvent/getEvent/setupEvent/getShrine/afterBattle/chooseEventOption/createCombatReward",
            "GameAction::getAllActionsInState and event execution",
            "BattleContext::initRelics/exitBattle and potion reward RNG path"
        }},
        {"observed_classification_counts", {
            {"required_repairs_completed", 4},
            {"restricted_training_exceptions", 1},
            {"unresolved_real_game_parity_blockers", 1},
            {"no_issue_within_inspected_scope", 4}
        }},
        {"full_sts_mechanics_audit", false},
        {"note", "The bounded Batch A repairs cover Colosseum, Match and Keep, Falling bottled-card exclusion, and We Meet Again bottled-card exclusion. Mysterious Sphere Golden Idol gold remains explicitly unresolved; unrelated mechanisms were not repaired."}
    };
    return evidence;
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
        {"single_action_fixtures", nlohmann::json::object()},
        {"execution_witnesses", nlohmann::json::array()}
    };
    const auto requiredEventEvidence = checkRequiredEventMechanics();
    result["required_event_fidelity"] = requiredEventEvidence;
    result["coverage_matrix"]["PRISMATIC_SHARD"] =
            requiredEventEvidence["coverage_matrix"]["PRISMATIC_SHARD"];
    result["coverage_matrix"]["EVENT_SCREEN:COLOSSEUM"] =
            requiredEventEvidence["coverage_matrix"]["EVENT_SCREEN:COLOSSEUM"];
    result["coverage_matrix"]["EVENT_SCREEN:MATCH_AND_KEEP"] =
            requiredEventEvidence["coverage_matrix"]["EVENT_SCREEN:MATCH_AND_KEEP"];
    result["coverage_matrix"]["EVENT_SCREEN:FALLING_BOTTLED_CARD_EXCLUSION"] =
            requiredEventEvidence["coverage_matrix"]
                    ["EVENT_SCREEN:FALLING_BOTTLED_CARD_EXCLUSION"];
    result["coverage_matrix"]["EVENT_SCREEN:WE_MEET_AGAIN_BOTTLED_CARD_EXCLUSION"] =
            requiredEventEvidence["coverage_matrix"]
                    ["EVENT_SCREEN:WE_MEET_AGAIN_BOTTLED_CARD_EXCLUSION"];

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
    // Keep a small exact event-pool guard alongside the full transition test
    // above, so future source edits cannot silently disable Act 2 eligibility.
    StepSimulator colosseumEligibility(CharacterClass::IRONCLAD, 49, 20);
    colosseumEligibility.gc.act = 2;
    colosseumEligibility.gc.curMapNodeY = 8;
    colosseumEligibility.gc.eventList = {Event::COLOSSEUM};
    colosseumEligibility.gc.shrineList = {Event::ANCIENT_WRITING};
    const bool colosseumCanBeAdded =
            colosseumEligibility.gc.canAddEvent(Event::COLOSSEUM);
    const auto eventSelectedByNativePoolFilter =
            colosseumEligibility.gc.getEvent(colosseumEligibility.gc.eventRng);
    const bool colosseumIsReachable =
            eventSelectedByNativePoolFilter == Event::COLOSSEUM;
    require(colosseumCanBeAdded && colosseumIsReachable,
            "Act 2 Colosseum is not reachable through the native event pool");

    StepSimulator colosseumChoiceFixture(CharacterClass::IRONCLAD, 49, 20);
    colosseumChoiceFixture.gc.screenState = ScreenState::EVENT_SCREEN;
    colosseumChoiceFixture.gc.curEvent = Event::COLOSSEUM;
    colosseumChoiceFixture.gc.info.eventData = 1;
    const auto colosseumProjection = privilegedProjection(colosseumChoiceFixture);
    const auto colosseumActions = verifyPrivilegedCandidateParity(
            colosseumChoiceFixture, colosseumProjection);
    require(colosseumActions.size() == 2
                    && colosseumProjection["unsupported_candidates"].cast<pybind11::list>().empty(),
            "reachable Colosseum alternatives did not have source-backed semantics");
    result["coverage_matrix"]["EVENT_SCREEN:COLOSSEUM"]["native_candidate_count"] =
            colosseumActions.size();
    result["coverage_matrix"]["EVENT_SCREEN:COLOSSEUM"]["can_add_at_event_floor"] =
            colosseumCanBeAdded;
    result["coverage_matrix"]["EVENT_SCREEN:COLOSSEUM"]["native_event_pool_selected_colosseum"] =
            colosseumIsReachable;

    // LAB setup immediately opens the potion reward screen in current native
    // rules, so the event itself contributes no legal choice.  Keep the
    // forced EVENT_SCREEN projection check below as a separate fail-closed
    // consistency fixture.
    StepSimulator labSetup(CharacterClass::IRONCLAD, 49, 20);
    labSetup.gc.curRoom = Room::EVENT;
    labSetup.gc.curEvent = Event::LAB;
    labSetup.gc.setupEvent();
    const bool labAutoResolvedNoChoice =
            labSetup.gc.screenState == ScreenState::REWARDS;
    require(labAutoResolvedNoChoice,
            "Lab no longer auto-resolves to REWARDS during native setup; revisit its no-choice classification");

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
        {"status", "unsupported_if_forced"},
        {"reachability_classification", labAutoResolvedNoChoice
                ? "auto_resolved_no_choice" : "actual_reachable_blocker"},
        {"native_candidate_count", labActions.size()},
        {"native_setup_opens_rewards", labAutoResolvedNoChoice},
        {"forced_state_reason", "native event action enumeration has one bit but chooseEventOption asserts no action is valid"}
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
    result["single_action_fixtures"]["EVENT_SCREEN:WHEEL_OF_CHANGE"] = {
        {"status", "supported"}, {"native_candidate_count", wheelActions.size()},
        {"single_candidate_execution", true}, {"rng_result", "unavailable"},
        {"scope_note", "one legal spin action; excluded from registered multi-choice coverage"}
    };

    result["coverage_matrix"]["EVENT_SCREEN:SOURCE_ENABLED_MULTI_CHOICE"] = {
        {"status", "supported"}, {"multi_choice_event_families_checked", eventFamilyCount},
        {"native_candidate_occurrences_checked", eventCandidateCount},
        {"all_candidate_semantics_available", eventFamilyCount == eventFamilies.size()},
        {"reachability_classification", "reachable_multichoice_supported"},
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

    std::vector<std::string> actualReachableBlockers;
    if (!labAutoResolvedNoChoice) {
        actualReachableBlockers.push_back(
                "EVENT_SCREEN:LAB no longer auto-resolves to REWARDS during native setup");
    }

    auto &coverageMatrix = result["coverage_matrix"];
    for (auto it = coverageMatrix.begin(); it != coverageMatrix.end(); ++it) {
        if (it.value().contains("reachability_classification")) continue;
        if (it.value().value("status", std::string()) == "RESTRICTED_TRAINING_EXCEPTION") {
            it.value()["reachability_classification"] = "restricted_training_exception";
            continue;
        }
        if (it.value().value("status", std::string()) == "supported") {
            it.value()["reachability_classification"] = "reachable_multichoice_supported";
        } else {
            it.value()["reachability_classification"] = "actual_reachable_blocker";
            actualReachableBlockers.push_back(it.key() + " has unsupported semantic coverage");
        }
    }
    const bool allRegisteredMultichoiceFamiliesSupported =
            eventFamilyCount == eventFamilies.size() && actualReachableBlockers.empty();
    result["coverage_status"] = allRegisteredMultichoiceFamiliesSupported
            ? "NATIVE_FULL_NONCOMBAT_SEMANTIC_CAPABILITY_ACCEPTED"
            : "NATIVE_CHOICE_SEMANTIC_NOT_READY";
    result["registered_scope"] =
            "currently reachable nonbattle decisions with multiple native legal actions";
    result["actual_reachable_blockers"] = actualReachableBlockers;
    result["mechanism_inventory"] = requiredEventEvidence["mechanism_inventory"];
    result["mechanism_inventory"].push_back({
        {"mechanism", "LAB EVENT_SCREEN"},
        {"reachability", "setupEvent opens REWARDS directly; synthetic forced EVENT_SCREEN is not reached"},
        {"native_switch_or_todo", "getValidEventSelectBits has one candidate but chooseEventOption does not execute it"},
        {"effect", "no natural event-screen choice in the active native setup path"},
        {"classification", "NO_ISSUE"}
    });
    result["source_disabled_mechanism_inventory_scope"] =
            requiredEventEvidence["source_disabled_mechanism_inventory_scope"];
    result["coverage_limitations"] = {
        "LAB forced EVENT_SCREEN remains unsupported; native setup auto-resolves it to REWARDS, so the forced state is not treated as reachable",
        "Colosseum and Match and Keep have native transition coverage, but no matched live CommunicationMod trace was available",
        "per-family event execution witnesses are representative rather than an execution of every event branch",
        "Falling and We Meet Again bottled-card exclusions have focused native fixtures but no matched live CommunicationMod parity trace",
        "Mysterious Sphere Golden Idol scaling remains GAME_RULE_UNRESOLVED and was not changed; this is not a full STS mechanics audit"
    };
    result["overall_parity_status"] = "REAL_GAME_PARITY_RESTRICTED_PRISMATIC_SHARD";
    result["real_game_trace_status"] = "REAL_GAME_TRACE_NOT_VERIFIED";
    result["live_game_parity_status"] = "LIVE_PARITY_NOT_VERIFIED";
    result["training_authorized"] = false;
    result["unrestricted_real_game_qualification"] = "BLOCKED_UNTIL_PRISMATIC_SHARD_RESTORATION";
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
