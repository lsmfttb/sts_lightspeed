#include <pybind11/embed.h>

#include "../bindings/slaythespire.cpp"

#include <map>
#include <array>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool isPrivatePublicStateKey(std::string key) {
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return key.find("rng") != std::string::npos
            || key.find("seed") != std::string::npos
            || key.find("bits") != std::string::npos
            || key.find("unique_id") != std::string::npos
            || key.find("hidden_future") != std::string::npos;
}

bool containsPrivatePublicStateKey(pybind11::handle value) {
    if (pybind11::isinstance<pybind11::dict>(value)) {
        const auto object = pybind11::reinterpret_borrow<pybind11::dict>(value);
        for (const auto item : object) {
            const auto key = pybind11::cast<std::string>(item.first);
            if (isPrivatePublicStateKey(key)
                    || containsPrivatePublicStateKey(item.second)) {
                return true;
            }
        }
    } else if (pybind11::isinstance<pybind11::list>(value)
            || pybind11::isinstance<pybind11::tuple>(value)) {
        const auto sequence = pybind11::reinterpret_borrow<pybind11::sequence>(value);
        for (const auto item : sequence) {
            if (containsPrivatePublicStateKey(item)) {
                return true;
            }
        }
    }
    return false;
}

StepSimulator makePublicBattleFixture(
        bool withRunicDome = false,
        MonsterEncounter encounter = MonsterEncounter::JAW_WORM) {
    StepSimulator simulator(CharacterClass::IRONCLAD, 101, 0);
    if (withRunicDome) {
        simulator.gc.relics.add({R::RUNIC_DOME, 0});
    }
    simulator.bc.init(simulator.gc, encounter);
    simulator.battleActive = true;
    simulator.gc.screenState = ScreenState::BATTLE;
    return simulator;
}

void changeRandomFuture(Random &rng, std::uint64_t salt) {
    rng.seed0 ^= salt;
    rng.seed1 ^= (salt << 1) | 1;
    ++rng.counter;
}

pybind11::dict statusByName(const pybind11::list &statuses) {
    pybind11::dict result;
    for (const auto &item : statuses) {
        const auto row = pybind11::cast<pybind11::dict>(item);
        result[pybind11::str(pybind11::cast<std::string>(row["name"]))] = row["value"];
    }
    return result;
}

void verifyPublicPlayerStatusesAndStance() {
    Player player;
    player.cc = CharacterClass::IRONCLAD;
    player.buff<PS::INTANGIBLE>(2);
    player.buff<PS::BUFFER>(1);
    player.buff<PS::BARRICADE>();
    player.debuff<PS::ENTANGLED>(1, false);
    player.buff<PS::NO_DRAW>();
    player.buff<PS::THE_BOMB>(40);
    player.stance = Stance::WRATH;

    const auto snapshot = playerSnapshot(player);
    const auto statuses = statusByName(pybind11::cast<pybind11::list>(snapshot["active_statuses"]));
    require(pybind11::cast<int>(statuses["INTANGIBLE"]) == 2, "Intangible value was omitted");
    require(pybind11::cast<int>(statuses["BUFFER"]) == 1, "Buffer value was omitted");
    require(pybind11::cast<int>(statuses["BARRICADE"]) == 1, "boolean Barricade was omitted");
    require(pybind11::cast<int>(statuses["ENTANGLED"]) == 1, "Entangled value was omitted");
    require(pybind11::cast<int>(statuses["NO_DRAW"]) == 1, "No Draw value was omitted");
    const auto bomb = pybind11::cast<pybind11::dict>(statuses["THE_BOMB"]);
    require(pybind11::cast<int>(bomb["third_turn"]) == 40, "The Bomb timer was omitted");
    require(pybind11::cast<int>(snapshot["stance_id"]) == static_cast<int>(Stance::WRATH),
            "stance id was omitted");
    require(pybind11::cast<std::string>(snapshot["stance"]) == "WRATH",
            "stance label was incorrect");
}

void verifyDefectOrbStateIsExplicitlyUnsupported() {
    Player player;
    player.cc = CharacterClass::DEFECT;
    player.orbSlots = 3;

    const auto snapshot = playerSnapshot(player);
    const auto orbState = pybind11::cast<pybind11::dict>(snapshot["orb_state"]);
    require(pybind11::cast<std::string>(orbState["availability"]) == "unsupported",
            "untracked Defect orbs were reported as available");
    require(pybind11::cast<std::string>(orbState["reason"]).find("does not track orb") != std::string::npos,
            "Defect orb-state limitation was not explained");
}

void verifySearingBlowUpgradeCountsRemainDistinct() {
    BattleContext battle;
    CardInstance oneUpgrade(CardId::SEARING_BLOW);
    oneUpgrade.upgrade();
    CardInstance threeUpgrades(CardId::SEARING_BLOW);
    threeUpgrades.upgrade();
    threeUpgrades.upgrade();
    threeUpgrades.upgrade();
    battle.cards.drawPile.push_back(oneUpgrade);
    battle.cards.drawPile.push_back(threeUpgrades);

    const auto multiset = drawPilePublicMultiset(battle);
    require(multiset.size() == 2, "different Searing Blow upgrades were merged");
    std::map<int, int> countsByUpgrade;
    for (const auto &item : multiset) {
        const auto row = pybind11::cast<pybind11::dict>(item);
        countsByUpgrade[pybind11::cast<int>(row["upgrade_count"])] =
                pybind11::cast<int>(row["count"]);
        require(pybind11::cast<int>(row["special_data"])
                        == pybind11::cast<int>(row["upgrade_count"]),
                "Searing Blow special data diverged from its upgrade count");
    }
    require(countsByUpgrade.size() == 2 && countsByUpgrade[1] == 1 && countsByUpgrade[3] == 1,
            "draw-pile multiset did not preserve Searing Blow +N identity");
}

void verifySpecialDataCardFacesAndMembership() {
    BattleContext battle;
    CardInstance rampage(CardId::RAMPAGE);
    rampage.specialData = 8;
    CardInstance geneticAlgorithm(CardId::GENETIC_ALGORITHM);
    geneticAlgorithm.specialData = 17;
    CardInstance ritualDagger(CardId::RITUAL_DAGGER);
    ritualDagger.specialData = 23;
    CardInstance searingBlow(CardId::SEARING_BLOW);
    searingBlow.upgraded = true;
    searingBlow.specialData = 4;

    const std::array<CardInstance, 4> specialCards{
            rampage, geneticAlgorithm, ritualDagger, searingBlow};
    for (const auto &card : specialCards) {
        const auto snapshot = cardSnapshot(battle, card, 0, false);
        const auto drawFace = publicDrawCardFace(card);
        require(snapshot.contains("special_data"),
                "visible card snapshot omitted applicable special data");
        require(drawFace.contains("special_data"),
                "known draw-order face omitted applicable special data");
        require(pybind11::cast<int>(snapshot["special_data"]) == card.specialData,
                "visible card snapshot changed special data");
        require(pybind11::cast<int>(drawFace["special_data"]) == card.specialData,
                "known draw-order face changed special data");
    }

    CardInstance ordinaryStrike(CardId::STRIKE_RED);
    require(!cardSnapshot(battle, ordinaryStrike, 0, false).contains("special_data"),
            "ordinary card exposed an inapplicable special-data field");
    require(!publicDrawCardFace(ordinaryStrike).contains("special_data"),
            "ordinary draw face exposed an inapplicable special-data field");

    CardInstance unscaledRampage(CardId::RAMPAGE);
    CardInstance scaledRampage(CardId::RAMPAGE);
    scaledRampage.specialData = 8;
    battle.cards.drawPile.push_back(unscaledRampage);
    battle.cards.drawPile.push_back(scaledRampage);
    const auto multiset = drawPilePublicMultiset(battle);
    require(multiset.size() == 2,
            "different Rampage damage values were merged in draw-pile membership");
    std::map<int, int> countsBySpecialData;
    for (const auto &item : multiset) {
        const auto row = pybind11::cast<pybind11::dict>(item);
        countsBySpecialData[pybind11::cast<int>(row["special_data"])] =
                pybind11::cast<int>(row["count"]);
    }
    require(countsBySpecialData.size() == 2
                    && countsBySpecialData[0] == 1
                    && countsBySpecialData[8] == 1,
            "draw-pile membership did not preserve Rampage damage values");
}

void verifyHiddenDrawAndRngFuturesDoNotChangePublicState() {
    auto first = makePublicBattleFixture();
    auto second = first;
    require(first.bc.cards.drawPile.size() >= 2,
            "battle fixture needs multiple hidden draw cards");
    const auto firstTopUniqueId = first.bc.cards.drawPile.back().getUniqueId();
    std::reverse(second.bc.cards.drawPile.begin(), second.bc.cards.drawPile.end());
    require(second.bc.cards.drawPile.back().getUniqueId() != firstTopUniqueId,
            "hidden-future fixture did not change the draw order");
    changeRandomFuture(second.bc.aiRng, 1);
    changeRandomFuture(second.bc.cardRandomRng, 2);
    changeRandomFuture(second.bc.miscRng, 3);
    changeRandomFuture(second.bc.monsterHpRng, 4);
    changeRandomFuture(second.bc.potionRng, 5);
    changeRandomFuture(second.bc.shuffleRng, 6);

    const auto firstState = first.publicBattleState();
    const auto secondState = second.publicBattleState();
    require(firstState.equal(secondState),
            "different hidden draw order/RNG futures changed the public battle state");
    const auto visibility = pybind11::cast<pybind11::dict>(firstState["visibility"]);
    const auto drawOrder = pybind11::cast<pybind11::dict>(visibility["draw_order"]);
    require(pybind11::cast<std::string>(drawOrder["classification"]) == "hidden",
            "ordinary draw order was not classified as hidden");
    require(!drawOrder.contains("known_top_prefix")
                    && !drawOrder.contains("visible_order_from_top"),
            "ordinary hidden order leaked an ordering fact");
    require(!containsPrivatePublicStateKey(firstState),
            "public battle state contains an RNG, seed, hidden-future, unique-id, or action-bits key");
}

void verifyHeadbuttAndFrozenEyeExposeOnlyKnownOrder() {
    auto headbutt = makePublicBattleFixture();
    headbutt.bc.cards.discardPile.clear();
    CardInstance selected(CardId::RAMPAGE);
    selected.setUniqueId(headbutt.bc.cards.nextUniqueCardId++);
    selected.specialData = 8;
    headbutt.bc.cards.discardPile.push_back(selected);
    headbutt.bc.chooseHeadbuttCard(0);

    const auto headbuttState = headbutt.publicBattleState();
    const auto headbuttVisibility = pybind11::cast<pybind11::dict>(
            headbuttState["visibility"]);
    const auto headbuttOrder = pybind11::cast<pybind11::dict>(
            headbuttVisibility["draw_order"]);
    require(pybind11::cast<std::string>(headbuttOrder["classification"]) == "known_prefix",
            "Headbutt did not expose its known top-card prefix");
    const auto knownTop = pybind11::cast<pybind11::list>(
            headbuttOrder["known_top_prefix"]);
    require(!knownTop.empty(), "Headbutt known-top prefix is empty");
    const auto knownTopCard = pybind11::cast<pybind11::dict>(knownTop[0]);
    require(pybind11::cast<int>(knownTopCard["id"]) == static_cast<int>(CardId::RAMPAGE)
                    && pybind11::cast<int>(knownTopCard["special_data"]) == 8,
            "Headbutt known-top face did not preserve the selected card identity");

    auto frozenEye = makePublicBattleFixture();
    frozenEye.gc.relics.add({R::FROZEN_EYE, 0});
    frozenEye.bc.player.setHasRelic<R::FROZEN_EYE>(true);
    const auto frozenState = frozenEye.publicBattleState();
    const auto frozenVisibility = pybind11::cast<pybind11::dict>(
            frozenState["visibility"]);
    const auto frozenOrder = pybind11::cast<pybind11::dict>(
            frozenVisibility["draw_order"]);
    require(pybind11::cast<std::string>(frozenOrder["classification"]) == "full_public_exact",
            "Frozen Eye did not expose the full current draw order");
    const auto visibleOrder = pybind11::cast<pybind11::list>(
            frozenOrder["visible_order_from_top"]);
    require(visibleOrder.size()
                    == static_cast<pybind11::ssize_t>(frozenEye.bc.cards.drawPile.size()),
            "Frozen Eye order did not include every current draw-pile card");
    for (std::size_t position = 0; position < frozenEye.bc.cards.drawPile.size(); ++position) {
        const auto drawIdx = frozenEye.bc.cards.drawPile.size() - 1 - position;
        const auto expected = cardSnapshot(
                frozenEye.bc, frozenEye.bc.cards.drawPile[drawIdx],
                static_cast<int>(drawIdx), false);
        require(pybind11::cast<pybind11::dict>(visibleOrder[position]).equal(expected),
                "Frozen Eye public order diverged from the current top-to-bottom draw order");
    }
}

void verifyRunicDomeSuppressesMonsterIntent() {
    auto visible = makePublicBattleFixture();
    auto visibleState = visible.publicBattleState();
    auto visibleMonsters = pybind11::cast<pybind11::list>(visibleState["monsters"]);
    require(!visibleMonsters.empty(), "battle fixture has no monster for intent visibility test");
    const auto visibleMonster = pybind11::cast<pybind11::dict>(visibleMonsters[0]);
    require(visibleMonster.contains("current_move")
                    && visibleMonster.contains("move_base_damage"),
            "control state did not include normally visible monster intent");

    auto hidden = makePublicBattleFixture(true);
    const auto hiddenState = hidden.publicBattleState();
    const auto visibility = pybind11::cast<pybind11::dict>(hiddenState["visibility"]);
    const auto intent = pybind11::cast<pybind11::dict>(visibility["enemy_intent"]);
    require(pybind11::cast<std::string>(intent["classification"]) == "hidden",
            "Runic Dome did not classify enemy intent as hidden");
    const auto monsters = pybind11::cast<pybind11::list>(hiddenState["monsters"]);
    require(!monsters.empty(), "Runic Dome fixture has no monster");
    for (const auto &monsterHandle : monsters) {
        const auto monster = pybind11::cast<pybind11::dict>(monsterHandle);
        require(!monster.contains("current_move")
                        && !monster.contains("move_id")
                        && !monster.contains("last_move_id")
                        && !monster.contains("attacking")
                        && !monster.contains("intent_category")
                        && !monster.contains("move_base_damage")
                        && !monster.contains("move_hits"),
                "Runic Dome monster snapshot exposed intent details");
    }
}

void verifyPublicActionIdentitiesExecute() {
    auto initial = makePublicBattleFixture();
    const auto state = initial.publicBattleState();
    const auto identities = pybind11::cast<pybind11::list>(
            state["ordered_public_legal_actions"]);
    require(!identities.empty(), "public battle state has no legal action identities");
    for (const auto &identityHandle : identities) {
        const auto identity = pybind11::cast<pybind11::dict>(identityHandle);
        require(!containsPrivatePublicStateKey(identity),
                "public action identity exposes replay-only action information");
        const auto label = pybind11::cast<std::string>(identity["label"]);
        require(label.find("bits=") == std::string::npos,
                "public action label exposes native replay bits");

        auto executable = initial;
        const auto result = executable.stepPublicAction(identity);
        require(result.contains("screen_state"),
                "public action identity did not resolve and execute to a simulator result");
    }
}

std::vector<std::int16_t> drawOrderIdentity(const BattleContext &battle) {
    std::vector<std::int16_t> identity;
    identity.reserve(battle.cards.drawPile.size());
    for (const auto &card : battle.cards.drawPile) {
        identity.push_back(card.getUniqueId());
    }
    return identity;
}

bool sameRandomState(const Random &lhs, const Random &rhs) {
    return lhs.counter == rhs.counter
            && lhs.seed0 == rhs.seed0
            && lhs.seed1 == rhs.seed1;
}

void requirePublicConsistentSample(
        StepSimulator &anchor,
        StepSimulator &particle,
        const std::string &caseName) {
    const auto anchorState = anchor.publicBattleState();
    const auto particleState = particle.publicBattleState();
    require(anchorState.equal(particleState),
            caseName + " changed the public battle state or legal-action surface");
    require(!containsPrivatePublicStateKey(particleState),
            caseName + " emitted private sampled details in the public state");
}

void requireSamplerRejected(
        StepSimulator &anchor,
        const std::string &caseName) {
    bool rejected = false;
    try {
        (void) anchor.samplePublicConsistentHiddenFuture(900, 0);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    require(rejected, caseName + " did not fail closed");
}

void verifyPublicConsistentSamplerDiversityAndReproducibility() {
    auto anchor = makePublicBattleFixture();
    const auto anchorState = anchor.publicBattleState();
    require(pybind11::cast<std::string>(anchorState["information_fidelity"])
                    == "supported",
            "sampler diversity fixture is not supported");
    const auto anchorActions = pybind11::cast<pybind11::list>(
            anchorState["ordered_public_legal_actions"]);
    const auto anchorOrder = drawOrderIdentity(anchor.bc);

    std::set<std::vector<std::int16_t>> sampledOrders;
    for (std::uint64_t particleIndex = 0; particleIndex < 24; ++particleIndex) {
        auto particle = anchor.samplePublicConsistentHiddenFuture(701, particleIndex);
        requirePublicConsistentSample(anchor, particle, "ordinary hidden-future sample");
        const auto particleState = particle.publicBattleState();
        require(pybind11::cast<pybind11::list>(
                        particleState["ordered_public_legal_actions"]).equal(anchorActions),
                "sampled public legal-action identities differ from the anchor");
        sampledOrders.insert(drawOrderIdentity(particle.bc));
    }
    require(sampledOrders.size() > 1,
            "ordinary hidden-future samples did not vary the hidden draw order");

    auto first = anchor.samplePublicConsistentHiddenFuture(702, 9);
    auto repeated = anchor.samplePublicConsistentHiddenFuture(702, 9);
    auto neighboringIndex = anchor.samplePublicConsistentHiddenFuture(702, 10);
    require(drawOrderIdentity(first.bc) == drawOrderIdentity(repeated.bc)
                    && sameRandomState(first.gc.cardRandomRng,
                            repeated.gc.cardRandomRng)
                    && sameRandomState(first.bc.aiRng, repeated.bc.aiRng)
                    && sameRandomState(first.bc.shuffleRng, repeated.bc.shuffleRng),
            "same sampler seed/index did not reproduce the same hidden sample");
    require(!sameRandomState(first.bc.aiRng, neighboringIndex.bc.aiRng),
            "different particle indices did not address distinct RNG futures");
    require(drawOrderIdentity(anchor.bc) == anchorOrder,
            "sampling mutated the anchor simulator");
}

void verifySamplerPreservesKnownDrawConstraints() {
    auto headbutt = makePublicBattleFixture();
    headbutt.bc.cards.discardPile.clear();
    CardInstance selected(CardId::RAMPAGE);
    selected.setUniqueId(headbutt.bc.cards.nextUniqueCardId++);
    selected.specialData = 8;
    headbutt.bc.cards.discardPile.push_back(selected);
    headbutt.bc.chooseHeadbuttCard(0);
    const auto knownTop = headbutt.bc.knownDrawTopUniqueIds;
    for (std::uint64_t index = 0; index < 8; ++index) {
        auto particle = headbutt.samplePublicConsistentHiddenFuture(703, index);
        requirePublicConsistentSample(headbutt, particle, "Headbutt sample");
        require(particle.bc.knownDrawTopUniqueIds == knownTop
                        && particle.bc.cards.drawPile.back().getUniqueId()
                                == selected.getUniqueId(),
                "sampler changed Headbutt's known top card");
    }

    auto positioned = makePublicBattleFixture();
    const auto position = static_cast<std::int32_t>(2);
    require(position < static_cast<std::int32_t>(positioned.bc.cards.drawPile.size()),
            "known-position fixture has too few draw cards");
    const auto positionedId = positioned.bc.cards.drawPile[
            positioned.bc.cards.drawPile.size() - 1 - position].getUniqueId();
    positioned.bc.knownDrawPositionUniqueIds[position] = positionedId;
    for (std::uint64_t index = 0; index < 8; ++index) {
        auto particle = positioned.samplePublicConsistentHiddenFuture(704, index);
        requirePublicConsistentSample(positioned, particle, "known-position sample");
        require(particle.bc.cards.drawPile[
                        particle.bc.cards.drawPile.size() - 1 - position].getUniqueId()
                        == positionedId,
                "sampler changed an exact known draw position");
    }

    auto frozenEye = makePublicBattleFixture();
    frozenEye.gc.relics.add({R::FROZEN_EYE, 0});
    frozenEye.bc.player.setHasRelic<R::FROZEN_EYE>(true);
    const auto frozenOrder = drawOrderIdentity(frozenEye.bc);
    for (std::uint64_t index = 0; index < 8; ++index) {
        auto particle = frozenEye.samplePublicConsistentHiddenFuture(705, index);
        requirePublicConsistentSample(frozenEye, particle, "Frozen Eye sample");
        require(drawOrderIdentity(particle.bc) == frozenOrder,
                "sampler changed Frozen Eye's fully visible draw order");
    }

    auto runicDome = makePublicBattleFixture(true);
    const auto domeState = runicDome.publicBattleState();
    const auto domeVisibility = pybind11::cast<pybind11::dict>(domeState["visibility"]);
    const auto domeIntent = pybind11::cast<pybind11::dict>(
            domeVisibility["enemy_intent"]);
    require(pybind11::cast<std::string>(domeIntent["classification"]) == "hidden",
            "Runic Dome did not preserve its public hidden-intent classification");
    requireSamplerRejected(runicDome, "Runic Dome sampler");

    auto inserted = makePublicBattleFixture();
    const auto baselineSize = inserted.bc.cards.drawPile.size();
    const std::int32_t minimumPosition = 2;
    CardInstance generated(CardId::RAMPAGE);
    generated.setUniqueId(inserted.bc.cards.nextUniqueCardId++);
    generated.specialData = 11;
    const auto insertionIdx = baselineSize - minimumPosition;
    inserted.bc.cards.drawPile.insert(
            inserted.bc.cards.drawPile.begin() + insertionIdx, generated);
    inserted.bc.knownGeneratedCardPublicIdentity[generated.getUniqueId()] = true;
    inserted.bc.knownDrawInsertionBaseSize = static_cast<std::int32_t>(baselineSize);
    inserted.bc.knownDrawInsertionCards.push_back(
            {generated.getUniqueId(), minimumPosition, -1});
    require(knownDrawStateConsistent(inserted.bc),
            "random-insertion fixture is internally inconsistent");
    for (std::uint64_t index = 0; index < 8; ++index) {
        auto particle = inserted.samplePublicConsistentHiddenFuture(706, index);
        requirePublicConsistentSample(inserted, particle, "random-insertion sample");
        const auto card = std::find_if(
                particle.bc.cards.drawPile.begin(),
                particle.bc.cards.drawPile.end(),
                [&](const CardInstance &candidate) {
                    return candidate.getUniqueId() == generated.getUniqueId();
                });
        require(card != particle.bc.cards.drawPile.end()
                        && knownDrawStateConsistent(particle.bc)
                        && static_cast<std::int32_t>(particle.bc.cards.drawPile.size() - 1
                                - std::distance(particle.bc.cards.drawPile.begin(), card))
                                >= minimumPosition,
                "sampler violated a known random-insertion constraint");
    }
}

void verifySamplerFailsClosedForUnsupportedFidelity() {
    auto unsupported = makePublicBattleFixture();
    unsupported.bc.player.cc = CharacterClass::DEFECT;
    unsupported.bc.player.orbSlots = 3;
    const auto state = unsupported.publicBattleState();
    require(pybind11::cast<std::string>(state["information_fidelity"])
                    == "unsupported_fidelity",
            "Defect fixture did not report unsupported public fidelity");
    bool rejected = false;
    try {
        (void) unsupported.samplePublicConsistentHiddenFuture(707, 0);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    require(rejected,
            "sampler did not fail closed for an unsupported-fidelity anchor");
}

void verifySamplerFailsClosedForUnrepresentedMonsterFuture() {
    auto domeChomp = makePublicBattleFixture(true);
    auto domeThrash = domeChomp;
    domeChomp.bc.monsters.arr[0].moveHistory[0] = MMID::JAW_WORM_CHOMP;
    domeThrash.bc.monsters.arr[0].moveHistory[0] = MMID::JAW_WORM_THRASH;
    const auto domeChompState = domeChomp.publicBattleState();
    const auto domeThrashState = domeThrash.publicBattleState();
    require(pybind11::cast<std::string>(domeChompState["information_fidelity"])
                    == "supported",
            "Runic Dome intent regression fixture is not a supported public state");
    require(domeChompState.equal(domeThrashState),
            "Runic Dome intent fixtures do not have the same public battle state");
    require(domeChomp.bc.monsters.arr[0].moveHistory[0]
                    != domeThrash.bc.monsters.arr[0].moveHistory[0],
            "Runic Dome intent fixtures did not vary the hidden current move");
    requireSamplerRejected(domeChomp, "Runic Dome sampler anchor");
    requireSamplerRejected(domeThrash, "Runic Dome alternate-intent anchor");

    auto louseFirst = makePublicBattleFixture(false, MonsterEncounter::TWO_LOUSE);
    auto louseSecond = louseFirst;
    auto &firstLouse = louseFirst.bc.monsters.arr[0];
    auto &secondLouse = louseSecond.bc.monsters.arr[0];
    const auto nonAttackMove = firstLouse.id == MonsterId::GREEN_LOUSE
            ? MMID::GREEN_LOUSE_SPIT_WEB : MMID::RED_LOUSE_GROW;
    firstLouse.moveHistory[0] = nonAttackMove;
    secondLouse.moveHistory[0] = nonAttackMove;
    firstLouse.miscInfo = 5;
    secondLouse.miscInfo = 7;
    require(louseFirst.publicBattleState().equal(louseSecond.publicBattleState()),
            "private louse-future fixtures do not have the same public battle state");
    requireSamplerRejected(louseFirst, "private louse-future sampler anchor");
    requireSamplerRejected(louseSecond, "alternate private louse-future sampler anchor");

    auto weakApplied = makePublicBattleFixture();
    auto weakExpired = weakApplied;
    weakApplied.bc.monsters.arr[0].addDebuff<MS::WEAK>(2, false);
    weakExpired.bc.monsters.arr[0].addDebuff<MS::WEAK>(2, false);
    weakApplied.bc.monsters.arr[0].setJustApplied<MS::WEAK>(true);
    weakExpired.bc.monsters.arr[0].setJustApplied<MS::WEAK>(false);
    require(weakApplied.publicBattleState().equal(weakExpired.publicBattleState()),
            "monster status-timing fixtures do not have the same public battle state");
    requireSamplerRejected(weakApplied, "newly applied monster Weak sampler anchor");
    requireSamplerRejected(weakExpired, "existing monster Weak sampler anchor");
}

} // namespace

int main() {
    pybind11::scoped_interpreter interpreter{};
    verifyPublicPlayerStatusesAndStance();
    verifyDefectOrbStateIsExplicitlyUnsupported();
    verifySearingBlowUpgradeCountsRemainDistinct();
    verifySpecialDataCardFacesAndMembership();
    verifyHiddenDrawAndRngFuturesDoNotChangePublicState();
    verifyHeadbuttAndFrozenEyeExposeOnlyKnownOrder();
    verifyRunicDomeSuppressesMonsterIntent();
    verifyPublicActionIdentitiesExecute();
    verifyPublicConsistentSamplerDiversityAndReproducibility();
    verifySamplerPreservesKnownDrawConstraints();
    verifySamplerFailsClosedForUnsupportedFidelity();
    verifySamplerFailsClosedForUnrepresentedMonsterFuture();
    std::cout << "PUBLIC_BATTLE_STATE_SEMANTICS_PASS\n";
    return 0;
}
