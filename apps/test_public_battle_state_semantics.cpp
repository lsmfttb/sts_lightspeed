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
        MonsterEncounter encounter = MonsterEncounter::JAW_WORM,
        int ascension = 0) {
    StepSimulator simulator(CharacterClass::IRONCLAD, 101, ascension);
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

pybind11::dict statusRowByName(
        const pybind11::list &statuses,
        const std::string &name) {
    for (const auto &item : statuses) {
        const auto row = pybind11::cast<pybind11::dict>(item);
        if (pybind11::cast<std::string>(row["name"]) == name) {
            return row;
        }
    }
    throw std::runtime_error("public status row is missing: " + name);
}

pybind11::dict publicPlayerStatusTiming(
        const pybind11::dict &publicState,
        const std::string &statusName) {
    const auto player = pybind11::cast<pybind11::dict>(publicState["player"]);
    const auto statuses = pybind11::cast<pybind11::list>(player["active_statuses"]);
    const auto row = statusRowByName(statuses, statusName);
    require(row.contains(pybind11::str("timing")),
            "public player status timing is missing for " + statusName);
    return pybind11::cast<pybind11::dict>(row["timing"]);
}

int publicPlayerStatusValue(
        const pybind11::dict &publicState,
        const std::string &statusName) {
    const auto player = pybind11::cast<pybind11::dict>(publicState["player"]);
    const auto statuses = pybind11::cast<pybind11::list>(player["active_statuses"]);
    return pybind11::cast<int>(statusRowByName(statuses, statusName)["value"]);
}

pybind11::dict publicMonsterStatusTiming(
        const pybind11::dict &publicState,
        const std::string &statusName) {
    const auto monsters = pybind11::cast<pybind11::list>(publicState["monsters"]);
    const auto monster = pybind11::cast<pybind11::dict>(monsters[0]);
    const auto statuses = pybind11::cast<pybind11::list>(monster["public_statuses"]);
    const auto row = statusRowByName(statuses, statusName);
    require(row.contains(pybind11::str("timing")),
            "public monster status timing is missing for " + statusName);
    return pybind11::cast<pybind11::dict>(row["timing"]);
}

int publicMonsterStatusValue(
        const pybind11::dict &publicState,
        const std::string &statusName) {
    const auto monsters = pybind11::cast<pybind11::list>(publicState["monsters"]);
    const auto monster = pybind11::cast<pybind11::dict>(monsters[0]);
    const auto statuses = pybind11::cast<pybind11::list>(monster["public_statuses"]);
    return pybind11::cast<int>(statusRowByName(statuses, statusName)["value"]);
}

void requirePublicTiming(
        const pybind11::dict &timing,
        const std::string &boundary,
        const std::string &effect,
        bool scheduled,
        const std::string &caseName) {
    require(pybind11::cast<std::string>(timing["next_boundary"]) == boundary,
            caseName + " reported the wrong next boundary");
    require(pybind11::cast<std::string>(timing["effect"]) == effect,
            caseName + " reported the wrong scheduled effect");
    require(pybind11::cast<bool>(timing["scheduled"]) == scheduled,
            caseName + " reported the wrong schedule");
    require(!timing.contains(pybind11::str("just_applied")),
            caseName + " exposed the simulator's raw just-applied bit");
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
        if (pybind11::cast<std::string>(identity["scope"]) == "battle") {
            std::ostringstream expectedLabel;
            expectedLabel << "battle."
                          << pybind11::cast<std::string>(identity["kind"])
                          << " idx1=" << pybind11::cast<int>(identity["idx1"])
                          << " idx2=" << pybind11::cast<int>(identity["idx2"])
                          << " idx3=" << pybind11::cast<int>(identity["idx3"]);
            require(label == expectedLabel.str(),
                    "public battle action label retained native card-instance details");
        }

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

std::vector<Random> hiddenRandomStreams(const StepSimulator &simulator) {
    return {
            simulator.gc.aiRng, simulator.gc.cardRandomRng,
            simulator.gc.cardRng, simulator.gc.eventRng,
            simulator.gc.mathUtilRng, simulator.gc.merchantRng,
            simulator.gc.miscRng, simulator.gc.monsterHpRng,
            simulator.gc.monsterRng, simulator.gc.neowRng,
            simulator.gc.potionRng, simulator.gc.relicRng,
            simulator.gc.shuffleRng, simulator.gc.treasureRng,
            simulator.bc.aiRng, simulator.bc.cardRandomRng,
            simulator.bc.miscRng, simulator.bc.monsterHpRng,
            simulator.bc.potionRng, simulator.bc.shuffleRng};
}

bool sameHiddenRandomStreams(
        const StepSimulator &lhs,
        const StepSimulator &rhs) {
    const auto left = hiddenRandomStreams(lhs);
    const auto right = hiddenRandomStreams(rhs);
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t idx = 0; idx < left.size(); ++idx) {
        if (!sameRandomState(left[idx], right[idx])) {
            return false;
        }
    }
    return lhs.gc.seed == rhs.gc.seed && lhs.bc.seed == rhs.bc.seed;
}

void incrementHiddenRandomCounters(StepSimulator &simulator) {
#define INCREMENT_COUNTER(stream) ++simulator.stream.counter
    INCREMENT_COUNTER(gc.aiRng);
    INCREMENT_COUNTER(gc.cardRandomRng);
    INCREMENT_COUNTER(gc.cardRng);
    INCREMENT_COUNTER(gc.eventRng);
    INCREMENT_COUNTER(gc.mathUtilRng);
    INCREMENT_COUNTER(gc.merchantRng);
    INCREMENT_COUNTER(gc.miscRng);
    INCREMENT_COUNTER(gc.monsterHpRng);
    INCREMENT_COUNTER(gc.monsterRng);
    INCREMENT_COUNTER(gc.neowRng);
    INCREMENT_COUNTER(gc.potionRng);
    INCREMENT_COUNTER(gc.relicRng);
    INCREMENT_COUNTER(gc.shuffleRng);
    INCREMENT_COUNTER(gc.treasureRng);
    INCREMENT_COUNTER(bc.aiRng);
    INCREMENT_COUNTER(bc.cardRandomRng);
    INCREMENT_COUNTER(bc.miscRng);
    INCREMENT_COUNTER(bc.monsterHpRng);
    INCREMENT_COUNTER(bc.potionRng);
    INCREMENT_COUNTER(bc.shuffleRng);
#undef INCREMENT_COUNTER
    simulator.gc.seed ^= 0x123456789ULL;
    simulator.bc.seed ^= 0x987654321ULL;
}

void changeHiddenRandomSeeds(StepSimulator &simulator) {
#define CHANGE_SEED(stream) \
    simulator.stream.seed0 ^= 0x1111222233334444ULL; \
    simulator.stream.seed1 ^= 0xaaaabbbbccccddddULL
    CHANGE_SEED(gc.aiRng);
    CHANGE_SEED(gc.cardRandomRng);
    CHANGE_SEED(gc.cardRng);
    CHANGE_SEED(gc.eventRng);
    CHANGE_SEED(gc.mathUtilRng);
    CHANGE_SEED(gc.merchantRng);
    CHANGE_SEED(gc.miscRng);
    CHANGE_SEED(gc.monsterHpRng);
    CHANGE_SEED(gc.monsterRng);
    CHANGE_SEED(gc.neowRng);
    CHANGE_SEED(gc.potionRng);
    CHANGE_SEED(gc.relicRng);
    CHANGE_SEED(gc.shuffleRng);
    CHANGE_SEED(gc.treasureRng);
    CHANGE_SEED(bc.aiRng);
    CHANGE_SEED(bc.cardRandomRng);
    CHANGE_SEED(bc.miscRng);
    CHANGE_SEED(bc.monsterHpRng);
    CHANGE_SEED(bc.potionRng);
    CHANGE_SEED(bc.shuffleRng);
#undef CHANGE_SEED
    simulator.gc.seed ^= 0x43218765ULL;
    simulator.bc.seed ^= 0x56781234ULL;
}

std::vector<PublicDrawCardFaceKey> publicDrawOrder(const StepSimulator &simulator) {
    std::vector<PublicDrawCardFaceKey> result;
    result.reserve(simulator.bc.cards.drawPile.size());
    for (const auto &card : simulator.bc.cards.drawPile) {
        result.push_back(publicDrawCardFaceKey(card));
    }
    return result;
}

bool equivalentSampleFutures(
        StepSimulator &lhs,
        StepSimulator &rhs) {
    return lhs.publicBattleState().equal(rhs.publicBattleState())
            && publicDrawOrder(lhs) == publicDrawOrder(rhs)
            && sameHiddenRandomStreams(lhs, rhs);
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

template <PlayerStatus Status, typename ApplyStatus>
void verifyPublicPlayerStatusTiming(
        const std::string &caseName,
        const std::string &statusName,
        const std::string &boundary,
        const std::string &effect,
        ApplyStatus applyStatus) {
    auto justApplied = makePublicBattleFixture();
    auto alreadyApplied = justApplied;
    applyStatus(justApplied.bc.player);
    applyStatus(alreadyApplied.bc.player);
    justApplied.bc.player.setJustApplied<Status>(true);
    alreadyApplied.bc.player.setJustApplied<Status>(false);

    const auto justAppliedState = justApplied.publicBattleState();
    const auto alreadyAppliedState = alreadyApplied.publicBattleState();
    require(pybind11::cast<std::string>(justAppliedState["information_fidelity"])
                    == "supported",
            caseName + " just-applied fixture is not supported");
    require(pybind11::cast<std::string>(alreadyAppliedState["information_fidelity"])
                    == "supported",
            caseName + " ticking fixture is not supported");
    require(!justAppliedState.equal(alreadyAppliedState),
            caseName + " public state collapsed different known timing");
    require(justApplied.bc.player.template wasJustApplied<Status>()
                    != alreadyApplied.bc.player.template wasJustApplied<Status>(),
            caseName + " fixtures did not vary native timing mechanics");

    const auto justRow = statusRowByName(
            pybind11::cast<pybind11::list>(
                    pybind11::cast<pybind11::dict>(justAppliedState["player"])
                            ["active_statuses"]),
            statusName);
    const auto tickingRow = statusRowByName(
            pybind11::cast<pybind11::list>(
                    pybind11::cast<pybind11::dict>(alreadyAppliedState["player"])
                            ["active_statuses"]),
            statusName);
    require(pybind11::cast<int>(justRow["value"])
                    == pybind11::cast<int>(tickingRow["value"]),
            caseName + " fixtures do not have the same visible status amount");
    requirePublicTiming(publicPlayerStatusTiming(
                                justAppliedState, statusName),
            boundary, effect, false, caseName + " just-applied state");
    requirePublicTiming(publicPlayerStatusTiming(
                                alreadyAppliedState, statusName),
            boundary, effect, true, caseName + " ticking state");

    auto privateFutureAnchor = justApplied;
    changeRandomFuture(privateFutureAnchor.gc.cardRandomRng, 0x11a7ULL);
    changeRandomFuture(privateFutureAnchor.bc.aiRng, 0x22b9ULL);
    require(justAppliedState.equal(privateFutureAnchor.publicBattleState()),
            caseName + " private-future anchor changed public timing");
    auto sampled = justApplied.samplePublicConsistentHiddenFuture(901, 4);
    auto sampledAlternate = privateFutureAnchor.samplePublicConsistentHiddenFuture(901, 4);
    requirePublicConsistentSample(justApplied, sampled, caseName + " sampled particle");
    requirePublicConsistentSample(
            privateFutureAnchor, sampledAlternate,
            caseName + " equivalent-anchor sampled particle");
    require(sampled.publicBattleState().equal(sampledAlternate.publicBattleState()),
            caseName + " sampler output depends on an equivalent private-future anchor");
    requirePublicTiming(publicPlayerStatusTiming(
                                sampled.publicBattleState(), statusName),
            boundary, effect, false, caseName + " sampled state");

    if constexpr (Status == PS::DRAW_REDUCTION) {
        auto skipExpiry = justApplied.samplePublicConsistentHiddenFuture(902, 0);
        auto expire = alreadyApplied.samplePublicConsistentHiddenFuture(902, 0);
        skipExpiry.bc.afterMonsterTurns();
        expire.bc.afterMonsterTurns();
        require(skipExpiry.bc.player.hasStatus<Status>()
                        && skipExpiry.bc.player.cardDrawPerTurn == 4,
                caseName + " changed before the just-applied next-draw boundary");
        require(!expire.bc.player.hasStatus<Status>()
                        && expire.bc.player.cardDrawPerTurn == 5,
                caseName + " did not expire at the scheduled next-draw boundary");
    } else {
        auto skipDecrement = justApplied.samplePublicConsistentHiddenFuture(902, 0);
        auto decrement = alreadyApplied.samplePublicConsistentHiddenFuture(902, 0);
        skipDecrement.bc.player.applyAtEndOfRoundPowers();
        decrement.bc.player.applyAtEndOfRoundPowers();
        require(skipDecrement.bc.player.getStatusRuntime(Status) == 2,
                caseName + " decremented at a boundary where its timing skips");
        require(decrement.bc.player.getStatusRuntime(Status) == 1,
                caseName + " failed to decrement at its scheduled boundary");
    }
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

std::string selectedRootAction(const pybind11::dict &searchResult) {
    const auto rows = searchResult["root_rows"].cast<pybind11::list>();
    int selected = -1;
    int bestVisits = -1;
    for (int idx = 0; idx < static_cast<int>(rows.size()); ++idx) {
        const auto row = rows[idx].cast<pybind11::dict>();
        const auto visits = row["visits"].cast<int>();
        if (visits > bestVisits) {
            bestVisits = visits;
            selected = idx;
        }
    }
    require(selected >= 0, "root search returned no selectable action");
    return rows[selected].cast<pybind11::dict>()["label"].cast<std::string>();
}

pybind11::dict publicEndTurnAction(StepSimulator &simulator) {
    for (const auto &action : enumerateBattleActions(simulator.bc)) {
        if (action.getActionType() == search::ActionType::END_TURN) {
            return publicActionIdentity(makeBattleAction(simulator.bc, action));
        }
    }
    throw std::runtime_error("supported public battle fixture has no end-turn action");
}

void requireEquivalentSampleFuture(
        StepSimulator &lhs,
        StepSimulator &rhs,
        const std::string &caseName) {
    require(equivalentSampleFutures(lhs, rhs),
            caseName + " changed a canonical future, public root, or action ordering");
}

void verifySamplerIgnoresPrivateCountersAndDrawOrder() {
    auto anchor = makePublicBattleFixture();
    auto counterVaried = anchor;
    incrementHiddenRandomCounters(counterVaried);
    auto seedVaried = anchor;
    changeHiddenRandomSeeds(seedVaried);
    auto orderVaried = anchor;
    std::reverse(orderVaried.bc.cards.drawPile.begin(),
            orderVaried.bc.cards.drawPile.end());
    auto allVaried = counterVaried;
    changeHiddenRandomSeeds(allVaried);
    std::reverse(allVaried.bc.cards.drawPile.begin(),
            allVaried.bc.cards.drawPile.end());

    const auto anchorState = anchor.publicBattleState();
    require(anchorState.equal(counterVaried.publicBattleState())
                    && anchorState.equal(seedVaried.publicBattleState())
                    && anchorState.equal(orderVaried.publicBattleState())
                    && anchorState.equal(allVaried.publicBattleState()),
            "private-future fixtures do not share a public battle state");

    constexpr std::uint64_t samplerSeed = 1701;
    constexpr std::uint64_t particleIndex = 11;
    auto canonical = anchor.samplePublicConsistentHiddenFuture(
            samplerSeed, particleIndex);
    auto canonicalCounters = counterVaried.samplePublicConsistentHiddenFuture(
            samplerSeed, particleIndex);
    auto canonicalSeeds = seedVaried.samplePublicConsistentHiddenFuture(
            samplerSeed, particleIndex);
    auto canonicalOrder = orderVaried.samplePublicConsistentHiddenFuture(
            samplerSeed, particleIndex);
    auto canonicalAll = allVaried.samplePublicConsistentHiddenFuture(
            samplerSeed, particleIndex);
    requireEquivalentSampleFuture(canonical, canonicalCounters,
            "counter-varied sampler");
    requireEquivalentSampleFuture(canonical, canonicalSeeds,
            "seed-varied sampler");
    requireEquivalentSampleFuture(canonical, canonicalOrder,
            "draw-order-varied sampler");
    requireEquivalentSampleFuture(canonical, canonicalAll,
            "combined private-future sampler");

    const auto canonicalSearch = canonical.battleSearchV2(192, false);
    const auto alternateSearch = canonicalAll.battleSearchV2(192, false);
    require(pybind11::cast<pybind11::list>(canonicalSearch["root_rows"])
                    .equal(pybind11::cast<pybind11::list>(alternateSearch["root_rows"])),
            "B=192 root action statistics or order depend on private anchor fields");
    require(selectedRootAction(canonicalSearch) == selectedRootAction(alternateSearch),
            "B=192 root action selection depends on private anchor fields");

    for (int turn = 0; turn < 2; ++turn) {
        const auto actionA = publicEndTurnAction(canonical);
        const auto actionB = publicEndTurnAction(canonicalAll);
        require(actionA.equal(actionB),
                "equivalent public roots expose different end-turn actions");
        const auto resultA = canonical.stepPublicAction(actionA);
        const auto resultB = canonicalAll.stepPublicAction(actionB);
        if (!resultA.equal(resultB)) {
            const auto stateA = resultA["battle_state"].cast<pybind11::dict>();
            const auto stateB = resultB["battle_state"].cast<pybind11::dict>();
            for (const auto item : stateA) {
                const auto key = item.first.cast<std::string>();
                if (!item.second.equal(stateB[pybind11::str(key)])) {
                    std::cerr << "transition difference " << key << ": "
                              << pybind11::str(item.second).cast<std::string>()
                              << " vs "
                              << pybind11::str(stateB[pybind11::str(key)])
                                      .cast<std::string>()
                              << '\n';
                }
            }
        }
        require(resultA.equal(resultB),
                "fixed public-action transitions diverged across equivalent particles");
        if (!resultA.contains(pybind11::str("battle_state"))) {
            break;
        }
    }

    // Operative negative control: inject the private anchor RNG counter into
    // the actual sampler seed, then run the same future-equivalence assertion
    // used above. The assertion must reject the resulting particles.
    const auto publicSeed = publicFutureParticleSeed(samplerSeed, particleIndex);
    auto faultyA = anchor.samplePublicConsistentHiddenFutureFromParticleSeed(
            splitMix64(publicSeed ^ static_cast<std::uint64_t>(
                    anchor.bc.aiRng.counter)));
    bool caughtPrivateDependence = false;
    for (std::uint64_t counterDelta = 1; counterDelta <= 64; ++counterDelta) {
        auto faultyAnchor = anchor;
        faultyAnchor.bc.aiRng.counter += static_cast<std::int32_t>(counterDelta);
        auto faultyB = faultyAnchor.samplePublicConsistentHiddenFutureFromParticleSeed(
                splitMix64(publicSeed ^ static_cast<std::uint64_t>(
                        faultyAnchor.bc.aiRng.counter)));
        if (publicDrawOrder(faultyA) == publicDrawOrder(faultyB)) {
            continue;
        }
        try {
            requireEquivalentSampleFuture(
                    faultyA, faultyB, "injected private-counter negative control");
        } catch (const std::runtime_error &) {
            caughtPrivateDependence = true;
            break;
        }
    }
    require(caughtPrivateDependence,
            "future-equivalence tests did not catch injected anchor-counter dependence");
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
    inserted.bc.cards.knownGeneratedCardPublicIdentity[generated.getUniqueId()] = true;
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

    auto insertionAlternate = inserted;
    std::vector<CardInstance> alternateBaseline;
    alternateBaseline.reserve(baselineSize);
    for (const auto &card : insertionAlternate.bc.cards.drawPile) {
        if (card.getUniqueId() != generated.getUniqueId()) {
            alternateBaseline.push_back(card);
        }
    }
    require(alternateBaseline.size() > 1,
            "random-insertion fixture has too few baseline cards to vary hidden order");
    bool changedPublicOrder = false;
    for (std::size_t offset = 1; offset < alternateBaseline.size(); ++offset) {
        auto candidate = alternateBaseline;
        std::rotate(candidate.begin(), candidate.begin() + offset, candidate.end());
        std::size_t baselineIdx = 0;
        for (auto &card : insertionAlternate.bc.cards.drawPile) {
            if (card.getUniqueId() != generated.getUniqueId()) {
                card = candidate[baselineIdx++];
            }
        }
        if (publicDrawOrder(inserted) != publicDrawOrder(insertionAlternate)) {
            changedPublicOrder = true;
            break;
        }
    }
    require(changedPublicOrder,
            "random-insertion fixture could not vary the hidden baseline face order");
    incrementHiddenRandomCounters(insertionAlternate);
    changeHiddenRandomSeeds(insertionAlternate);
    require(inserted.publicBattleState().equal(
                    insertionAlternate.publicBattleState())
                    && knownDrawStateConsistent(insertionAlternate.bc),
            "cross-anchor insertion fixtures changed public state or constraints");
    auto canonicalInsertionParticle =
            inserted.samplePublicConsistentHiddenFuture(706, 5);
    auto alternateInsertionParticle =
            insertionAlternate.samplePublicConsistentHiddenFuture(706, 5);
    requireEquivalentSampleFuture(
            canonicalInsertionParticle, alternateInsertionParticle,
            "cross-anchor random-insertion sample");

    const auto canonicalInsertionSearch =
            canonicalInsertionParticle.battleSearchV2(192, false);
    const auto alternateInsertionSearch =
            alternateInsertionParticle.battleSearchV2(192, false);
    require(pybind11::cast<pybind11::list>(
                    canonicalInsertionSearch["root_rows"])
                    .equal(pybind11::cast<pybind11::list>(
                            alternateInsertionSearch["root_rows"]))
                    && selectedRootAction(canonicalInsertionSearch)
                            == selectedRootAction(alternateInsertionSearch),
            "cross-anchor insertion B=192 root statistics or selection diverged");

    auto transitionA = canonicalInsertionParticle;
    auto transitionB = alternateInsertionParticle;
    for (int turn = 0; turn < 2; ++turn) {
        const auto actionA = publicEndTurnAction(transitionA);
        const auto actionB = publicEndTurnAction(transitionB);
        require(actionA.equal(actionB),
                "cross-anchor insertion roots exposed different end-turn actions");
        const auto resultA = transitionA.stepPublicAction(actionA);
        const auto resultB = transitionB.stepPublicAction(actionB);
        require(resultA.equal(resultB),
                "cross-anchor insertion fixed-action transitions diverged");
        if (!resultA.contains(pybind11::str("battle_state"))) {
            break;
        }
    }
}

void moveCurrentDrawPileToDiscard(StepSimulator &simulator) {
    auto &cards = simulator.bc.cards;
    for (const auto &card : cards.drawPile) {
        cards.notifyRemoveFromDrawPile(card);
        cards.moveToDiscardPile(card);
    }
    cards.drawPile.clear();
}

void shuffleDiscardIntoEmptyDrawPile(StepSimulator &simulator) {
    require(simulator.bc.cards.drawPile.empty(),
            "shuffle fixture did not have an empty draw pile");
    simulator.bc.onShuffle();
    const auto shuffle = Actions::EmptyDeckShuffle();
    shuffle.actFunc(simulator.bc);
}

std::string publicDrawOrderClassification(const pybind11::dict &state) {
    const auto visibility = pybind11::cast<pybind11::dict>(state["visibility"]);
    const auto drawOrder = pybind11::cast<pybind11::dict>(visibility["draw_order"]);
    return pybind11::cast<std::string>(drawOrder["classification"]);
}

int publicDrawCountForCard(const pybind11::dict &state, CardId cardId) {
    const auto membership = pybind11::cast<pybind11::dict>(
            state["draw_pile_membership"]);
    if (!membership.contains("multiset_counts")) {
        return 0;
    }
    const auto counts = pybind11::cast<pybind11::list>(membership["multiset_counts"]);
    for (const auto &item : counts) {
        const auto row = pybind11::cast<pybind11::dict>(item);
        if (pybind11::cast<int>(row["id"]) == static_cast<int>(cardId)) {
            return pybind11::cast<int>(row["count"]);
        }
    }
    return 0;
}

bool listContainsString(const pybind11::list &values, const std::string &expected) {
    for (const auto &value : values) {
        if (pybind11::cast<std::string>(value) == expected) {
            return true;
        }
    }
    return false;
}

void requireGeneratedCardIdentityKnown(
        const CardManager &cards, const CardInstance &card,
        const std::string &caseName) {
    const auto known = cards.knownGeneratedCardPublicIdentity.find(card.getUniqueId());
    require(known != cards.knownGeneratedCardPublicIdentity.end() && known->second,
            caseName + " did not retain public generated-card identity knowledge");
}

void verifyGeneratedCardMembershipSurvivesPublicPileTransitions() {
    auto first = makePublicBattleFixture();
    auto second = makePublicBattleFixture();
    second.bc.cards.nextUniqueCardId += 13;

    const auto addSmallSlimesCards =
            Actions::MakeTempCardInDiscard({CardId::SLIMED}, 2);
    addSmallSlimesCards.actFunc(first.bc);
    addSmallSlimesCards.actFunc(second.bc);
    require(first.bc.cards.discardPile.size() == 2
                    && second.bc.cards.discardPile.size() == 2,
            "Small Slimes Slimed-card fixture did not create two discard cards");
    require(first.bc.cards.discardPile.front().getUniqueId()
                    != second.bc.cards.discardPile.front().getUniqueId(),
            "public-equivalent fixtures did not vary private generated-card ids");
    for (const auto &card : first.bc.cards.discardPile) {
        requireGeneratedCardIdentityKnown(
                first.bc.cards, card, "generated discard pile card");
    }

    moveCurrentDrawPileToDiscard(first);
    moveCurrentDrawPileToDiscard(second);
    shuffleDiscardIntoEmptyDrawPile(first);
    shuffleDiscardIntoEmptyDrawPile(second);

    const auto firstState = first.publicBattleState();
    const auto secondState = second.publicBattleState();
    require(firstState.equal(secondState),
            "public state exposed anchor-specific generated-card ids");
    require(pybind11::cast<std::string>(firstState["information_fidelity"])
                    == "supported",
            "publicly known Slimed membership remained unsupported after shuffle");
    require(publicDrawOrderClassification(firstState) == "hidden",
            "public pile shuffle exposed hidden draw order");
    require(publicDrawCountForCard(firstState, CardId::SLIMED) == 2,
            "duplicate generated Slimed faces were not represented as a public multiset");
    require(!containsPrivatePublicStateKey(firstState),
            "public state exposed a generated card's native unique id");

    auto firstParticle = first.samplePublicConsistentHiddenFuture(1201, 7);
    auto secondParticle = second.samplePublicConsistentHiddenFuture(1201, 7);
    requirePublicConsistentSample(first, firstParticle, "generated-membership sample A");
    requirePublicConsistentSample(second, secondParticle, "generated-membership sample B");
    require(firstParticle.publicBattleState().equal(secondParticle.publicBattleState()),
            "generated-card sampling depended on the private anchor ids");
    require(publicDrawCountForCard(
                    firstParticle.publicBattleState(), CardId::SLIMED) == 2,
            "sampler did not preserve known generated-card membership");

    const auto generated = std::find_if(
            first.bc.cards.drawPile.begin(), first.bc.cards.drawPile.end(),
            [](const CardInstance &card) { return card.getId() == CardId::SLIMED; });
    require(generated != first.bc.cards.drawPile.end(),
            "generated Slimed card was missing from shuffled draw pile");
    const auto drawPileIdx = static_cast<int>(
            std::distance(first.bc.cards.drawPile.begin(), generated));
    const auto drawn = *generated;
    first.bc.consumeKnownDrawAtIndex(drawPileIdx, drawn);
    first.bc.cards.removeFromDrawPileAtIdx(drawPileIdx);
    first.bc.cards.moveToHand(drawn);
    const auto afterDrawState = first.publicBattleState();
    require(pybind11::cast<std::string>(afterDrawState["information_fidelity"])
                    == "supported"
                    && publicDrawCountForCard(afterDrawState, CardId::SLIMED) == 1,
            "drawing one duplicate generated card did not remove just its draw membership");
}

void verifyHiddenGeneratedIdentityRemainsFailClosedUntilObserved() {
    auto simulator = makePublicBattleFixture();
    moveCurrentDrawPileToDiscard(simulator);
    CardInstance hidden(CardId::SLIMED);
    simulator.bc.insertTempCardRandomlyIntoDrawPile(hidden, false);
    const auto hiddenCard = simulator.bc.cards.drawPile.back();
    const auto hiddenIdentity = simulator.bc.cards.knownGeneratedCardPublicIdentity.find(
            hiddenCard.getUniqueId());
    require(hiddenIdentity != simulator.bc.cards.knownGeneratedCardPublicIdentity.end()
                    && !hiddenIdentity->second,
            "hidden random draw insertion was marked publicly known");
    const auto hiddenState = simulator.publicBattleState();
    require(pybind11::cast<std::string>(hiddenState["information_fidelity"])
                    == "unsupported_fidelity",
            "hidden random generated identity did not remain fail-closed");
    const auto hiddenMembership = pybind11::cast<pybind11::dict>(
            hiddenState["draw_pile_membership"]);
    require(listContainsString(
                    pybind11::cast<pybind11::list>(hiddenMembership["unsupported_reasons"]),
                    "insertion_membership_unrepresented"),
            "hidden random generated identity lost its membership failure reason");
    requireSamplerRejected(simulator, "hidden generated-card membership");

    const auto handSizeBeforeDraw = simulator.bc.cards.cardsInHand;
    simulator.bc.drawCards(1);
    require(simulator.bc.cards.cardsInHand == handSizeBeforeDraw + 1,
            "visible generated card was not drawn into hand");
    const auto revealed = simulator.bc.cards.hand[handSizeBeforeDraw];
    requireGeneratedCardIdentityKnown(
            simulator.bc.cards, revealed, "drawn generated hand card");
    simulator.bc.cards.removeFromHandAtIdx(handSizeBeforeDraw);
    simulator.bc.cards.moveToDiscardPile(revealed);
    requireGeneratedCardIdentityKnown(
            simulator.bc.cards, revealed, "revealed generated discard card");
    shuffleDiscardIntoEmptyDrawPile(simulator);
    const auto publicState = simulator.publicBattleState();
    require(pybind11::cast<std::string>(publicState["information_fidelity"])
                    == "supported"
                    && publicDrawOrderClassification(publicState) == "hidden"
                    && publicDrawCountForCard(publicState, CardId::SLIMED) == 1,
            "revealed generated identity did not retain membership through discard shuffle");
}

void verifyGeneratedCardIdentityIsRecordedInVisiblePiles() {
    auto simulator = makePublicBattleFixture();
    auto &cards = simulator.bc.cards;

    cards.createTempCardInHand(CardInstance(CardId::SLIMED));
    requireGeneratedCardIdentityKnown(
            cards, cards.hand[cards.cardsInHand - 1], "generated hand card");

    cards.createTempCardInDiscard(CardInstance(CardId::SLIMED));
    requireGeneratedCardIdentityKnown(
            cards, cards.discardPile.back(), "generated discard card");

    CardInstance exhausted(CardId::SLIMED);
    exhausted.setUniqueId(cards.nextUniqueCardId++);
    cards.moveToExhaustPile(exhausted);
    requireGeneratedCardIdentityKnown(
            cards, cards.exhaustPile.back(), "generated exhaust card");
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

template <MonsterStatus Status, typename ApplyStatus>
void verifyPublicMonsterStatusTiming(
        const std::string &caseName,
        const std::string &statusName,
        const std::string &effect,
        ApplyStatus applyStatus) {
    auto justApplied = makePublicBattleFixture();
    auto ticking = justApplied;
    applyStatus(justApplied.bc.monsters.arr[0]);
    applyStatus(ticking.bc.monsters.arr[0]);
    justApplied.bc.monsters.arr[0].setJustApplied<Status>(true);
    ticking.bc.monsters.arr[0].setJustApplied<Status>(false);

    const auto justAppliedState = justApplied.publicBattleState();
    const auto tickingState = ticking.publicBattleState();
    require(pybind11::cast<std::string>(justAppliedState["information_fidelity"])
                    == "supported"
                    && pybind11::cast<std::string>(tickingState["information_fidelity"])
                            == "supported",
            caseName + " status made supported public fidelity fail");
    require(!justAppliedState.equal(tickingState),
            caseName + " public state collapsed different known timing");
    require(publicMonsterStatusValue(justAppliedState, statusName)
                    == publicMonsterStatusValue(tickingState, statusName),
            caseName + " fixtures do not have the same visible status amount");
    requirePublicTiming(publicMonsterStatusTiming(
                                justAppliedState, statusName),
            "end_of_round", effect, false, caseName + " just-applied state");
    requirePublicTiming(publicMonsterStatusTiming(
                                tickingState, statusName),
            "end_of_round", effect, true, caseName + " ticking state");

    auto privateFutureAnchor = justApplied;
    changeRandomFuture(privateFutureAnchor.gc.cardRandomRng, 0x3311ULL);
    changeRandomFuture(privateFutureAnchor.bc.aiRng, 0x4423ULL);
    require(justAppliedState.equal(privateFutureAnchor.publicBattleState()),
            caseName + " private-future anchor changed public timing");
    auto sampled = justApplied.samplePublicConsistentHiddenFuture(903, 5);
    auto sampledAlternate = privateFutureAnchor.samplePublicConsistentHiddenFuture(903, 5);
    requirePublicConsistentSample(justApplied, sampled, caseName + " sampled particle");
    requirePublicConsistentSample(
            privateFutureAnchor, sampledAlternate,
            caseName + " equivalent-anchor sampled particle");
    require(sampled.publicBattleState().equal(sampledAlternate.publicBattleState()),
            caseName + " sampler output depends on an equivalent private-future anchor");
    requirePublicTiming(publicMonsterStatusTiming(
                                sampled.publicBattleState(), statusName),
            "end_of_round", effect, false, caseName + " sampled state");

    auto skipEffect = justApplied.samplePublicConsistentHiddenFuture(904, 0);
    auto applyEffect = ticking.samplePublicConsistentHiddenFuture(904, 0);
    auto &skipMonster = skipEffect.bc.monsters.arr[0];
    auto &applyMonster = applyEffect.bc.monsters.arr[0];
    const auto strengthBeforeSkip = skipMonster.strength;
    const auto strengthBeforeApply = applyMonster.strength;
    skipMonster.applyEndOfRoundPowers(skipEffect.bc);
    applyMonster.applyEndOfRoundPowers(applyEffect.bc);
    if constexpr (Status == MS::RITUAL) {
        require(skipMonster.strength == strengthBeforeSkip,
                caseName + " triggered before its scheduled end-of-round boundary");
        require(applyMonster.strength == strengthBeforeApply + 2,
                caseName + " missed its scheduled end-of-round trigger");
    } else {
        require(publicMonsterStatusValue(skipEffect.publicBattleState(), statusName) == 2,
                caseName + " decremented at a boundary where its timing skips");
        require(publicMonsterStatusValue(applyEffect.publicBattleState(), statusName) == 1,
                caseName + " failed to decrement at its scheduled boundary");
    }
}

void verifyLouseLatentProposalUsesOnlyPublicDamage() {
    auto louseFirst = makePublicBattleFixture(
            false, MonsterEncounter::TWO_LOUSE, 20);
    auto louseSecond = louseFirst;
    auto &firstLouse = louseFirst.bc.monsters.arr[0];
    auto &secondLouse = louseSecond.bc.monsters.arr[0];
    const auto nonAttackMove = firstLouse.id == MonsterId::GREEN_LOUSE
            ? MMID::GREEN_LOUSE_SPIT_WEB : MMID::RED_LOUSE_GROW;
    firstLouse.moveHistory[0] = nonAttackMove;
    secondLouse.moveHistory[0] = nonAttackMove;
    firstLouse.miscInfo = 6;
    secondLouse.miscInfo = 8;
    const auto nonAttackState = louseFirst.publicBattleState();
    require(nonAttackState.equal(louseSecond.publicBattleState()),
            "private louse-future fixtures do not have the same public battle state");
    const auto nonAttackMonsters = nonAttackState["monsters"].cast<pybind11::list>();
    const auto nonAttackSnapshot = nonAttackMonsters[0].cast<pybind11::dict>();
    require(nonAttackSnapshot["move_base_damage"].is_none()
                    && nonAttackSnapshot["move_damage_to_player"].is_none()
                    && !nonAttackSnapshot.contains(pybind11::str("misc_info")),
            "non-attacking Louse projection exposed its private bite parameter");

    std::set<int> sampledBiteValues;
    for (std::uint64_t particleIndex = 0; particleIndex < 96; ++particleIndex) {
        auto firstParticle = louseFirst.samplePublicConsistentHiddenFuture(
                1801, particleIndex);
        auto secondParticle = louseSecond.samplePublicConsistentHiddenFuture(
                1801, particleIndex);
        require(firstParticle.bc.monsters.arr[0].miscInfo
                        == secondParticle.bc.monsters.arr[0].miscInfo,
                "same indexed Louse proposal depends on the anchor's private bite value");
        const auto biteDamage = firstParticle.bc.monsters.arr[0].miscInfo;
        require(biteDamage >= 6 && biteDamage <= 8,
                "A20 Louse proposal sampled outside the native 6..8 range");
        sampledBiteValues.insert(biteDamage);
    }
    require(sampledBiteValues.size() > 1,
            "non-attacking Louse proposals collapsed genuine bite-damage uncertainty");

    auto biteFirst = makePublicBattleFixture(
            false, MonsterEncounter::TWO_LOUSE, 20);
    auto biteSecond = biteFirst;
    auto &knownBite = biteFirst.bc.monsters.arr[0];
    auto &knownBiteAlternative = biteSecond.bc.monsters.arr[0];
    const auto biteMove = knownBite.id == MonsterId::GREEN_LOUSE
            ? MMID::GREEN_LOUSE_BITE : MMID::RED_LOUSE_BITE;
    knownBite.moveHistory[0] = biteMove;
    knownBiteAlternative.moveHistory[0] = biteMove;
    knownBite.miscInfo = 6;
    knownBiteAlternative.miscInfo = 8;
    knownBite.strength = 2;
    knownBiteAlternative.strength = 2;
    knownBite.setHasStatus<MS::WEAK>();
    knownBiteAlternative.setHasStatus<MS::WEAK>();
    biteFirst.bc.player.buff<PS::INTANGIBLE>(1);
    biteSecond.bc.player.buff<PS::INTANGIBLE>(1);
    const auto biteState = biteFirst.publicBattleState();
    require(biteState.equal(biteSecond.publicBattleState()),
            "damage-modified Louse bite fixtures do not share a public root");
    const auto biteMonsters = biteState["monsters"].cast<pybind11::list>();
    const auto biteSnapshot = biteMonsters[0].cast<pybind11::dict>();
    require(biteSnapshot["move_base_damage"].is_none()
                    && biteSnapshot["move_damage_to_player"].cast<int>() == 1,
            "Louse policy projection did not expose the modified damage while hiding its base");
    auto sampledBite = biteFirst.samplePublicConsistentHiddenFuture(1802, 9);
    auto sampledBiteAlternative = biteSecond.samplePublicConsistentHiddenFuture(1802, 9);
    require(sampledBite.bc.monsters.arr[0].miscInfo
                    == sampledBiteAlternative.bc.monsters.arr[0].miscInfo,
            "modified visible bite damage did not produce an anchor-independent latent sample");
    require(sampledBite.bc.monsters.arr[0].calculateDamageToPlayer(
                            sampledBite.bc,
                            sampledBite.bc.monsters.arr[0].miscInfo) == 1,
            "sampled Louse latent value conflicts with public modified bite damage");

    auto uniquelyObserved = makePublicBattleFixture(
            false, MonsterEncounter::TWO_LOUSE, 20);
    auto &uniqueLouse = uniquelyObserved.bc.monsters.arr[0];
    uniqueLouse.moveHistory[0] = uniqueLouse.id == MonsterId::GREEN_LOUSE
            ? MMID::GREEN_LOUSE_BITE : MMID::RED_LOUSE_BITE;
    uniqueLouse.miscInfo = 7;
    auto conditioned = uniquelyObserved.samplePublicConsistentHiddenFuture(1803, 4);
    require(conditioned.bc.monsters.arr[0].miscInfo == 7,
            "visible unmodified Louse bite damage did not condition the latent proposal");
}

void verifyPrivateMonsterFutureStillFailsClosed() {
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

    auto darkling = makePublicBattleFixture(
            false, MonsterEncounter::THREE_DARKLINGS, 20);
    requireSamplerRejected(darkling, "unsupported private Darkling future");
}

void verifyPublicStatusTimingSemantics() {
    verifyPublicMonsterStatusTiming<MS::RITUAL>(
            "monster Ritual", "Ritual", "trigger_strength_gain",
            [](Monster &monster) { monster.buff<MS::RITUAL>(2); });
    verifyPublicMonsterStatusTiming<MS::WEAK>(
            "monster Weak", "Weak", "decrement_duration",
            [](Monster &monster) { monster.addDebuff<MS::WEAK>(2, false); });
    verifyPublicMonsterStatusTiming<MS::VULNERABLE>(
            "monster Vulnerable", "Vulnerable", "decrement_duration",
            [](Monster &monster) { monster.addDebuff<MS::VULNERABLE>(2, false); });

    verifyPublicPlayerStatusTiming<PS::WEAK>(
            "player Weak", "WEAK", "end_of_round", "decrement_duration",
            [](Player &player) {
                player.debuff<PS::WEAK>(2, false);
            });
    verifyPublicPlayerStatusTiming<PS::VULNERABLE>(
            "player Vulnerable", "VULNERABLE", "end_of_round", "decrement_duration",
            [](Player &player) {
                player.debuff<PS::VULNERABLE>(2, false);
            });
    verifyPublicPlayerStatusTiming<PS::FRAIL>(
            "player Frail", "FRAIL", "end_of_round", "decrement_duration",
            [](Player &player) {
                player.debuff<PS::FRAIL>(2, false);
            });
    verifyPublicPlayerStatusTiming<PS::DOUBLE_DAMAGE>(
            "player Double Damage", "DOUBLE_DAMAGE", "end_of_round", "decrement_duration",
            [](Player &player) {
                player.buff<PS::DOUBLE_DAMAGE>(2);
            });
    verifyPublicPlayerStatusTiming<PS::DRAW_REDUCTION>(
            "player Draw Reduction", "DRAW_REDUCTION",
            "player_start_after_draw", "expire",
            [](Player &player) {
                player.debuff<PS::DRAW_REDUCTION>(1, false);
            });
}

void verifyPublicStatusTimingStackingAndReapplication() {
    auto monsterWeak = makePublicBattleFixture();
    monsterWeak.bc.monsters.arr[0].addDebuff<MS::WEAK>(2, false);
    monsterWeak.bc.monsters.arr[0].setJustApplied<MS::WEAK>(false);
    requirePublicTiming(publicMonsterStatusTiming(
                                monsterWeak.publicBattleState(), "Weak"),
            "end_of_round", "decrement_duration", true,
            "monster Weak before enemy reapplication");
    monsterWeak.bc.monsters.arr[0].addDebuff<MS::WEAK>(1, true);
    require(publicMonsterStatusValue(monsterWeak.publicBattleState(), "Weak") == 3,
            "monster Weak reapplication did not stack its duration");
    requirePublicTiming(publicMonsterStatusTiming(
                                monsterWeak.publicBattleState(), "Weak"),
            "end_of_round", "decrement_duration", false,
            "monster Weak after enemy reapplication");

    auto playerWeak = makePublicBattleFixture();
    playerWeak.bc.player.debuff<PS::WEAK>(2, false);
    playerWeak.bc.player.setJustApplied<PS::WEAK>(false);
    playerWeak.bc.player.debuff<PS::WEAK>(1, true);
    require(publicPlayerStatusValue(playerWeak.publicBattleState(), "WEAK") == 3,
            "player Weak reapplication did not stack its duration");
    requirePublicTiming(publicPlayerStatusTiming(
                                playerWeak.publicBattleState(), "WEAK"),
            "end_of_round", "decrement_duration", true,
            "player Weak after monster reapplication");

    auto ritual = makePublicBattleFixture();
    ritual.bc.monsters.arr[0].buff<MS::RITUAL>(2);
    ritual.bc.monsters.arr[0].setJustApplied<MS::RITUAL>(false);
    ritual.bc.monsters.arr[0].buff<MS::RITUAL>(1);
    require(publicMonsterStatusValue(ritual.publicBattleState(), "Ritual") == 3,
            "monster Ritual reapplication did not stack its amount");
    requirePublicTiming(publicMonsterStatusTiming(
                                ritual.publicBattleState(), "Ritual"),
            "end_of_round", "trigger_strength_gain", false,
            "monster Ritual after reapplication");

    auto doubleDamage = makePublicBattleFixture();
    doubleDamage.bc.player.buff<PS::DOUBLE_DAMAGE>(1);
    doubleDamage.bc.player.setJustApplied<PS::DOUBLE_DAMAGE>(false);
    doubleDamage.bc.player.buff<PS::DOUBLE_DAMAGE>(1);
    require(publicPlayerStatusValue(
                    doubleDamage.publicBattleState(), "DOUBLE_DAMAGE") == 2,
            "player Double Damage reapplication did not stack its amount");
    requirePublicTiming(publicPlayerStatusTiming(
                                doubleDamage.publicBattleState(), "DOUBLE_DAMAGE"),
            "end_of_round", "decrement_duration", false,
            "player Double Damage after reapplication");

    auto drawReduction = makePublicBattleFixture();
    drawReduction.bc.player.debuff<PS::DRAW_REDUCTION>(1, false);
    drawReduction.bc.player.setJustApplied<PS::DRAW_REDUCTION>(false);
    drawReduction.bc.player.debuff<PS::DRAW_REDUCTION>(1, false);
    require(drawReduction.bc.player.cardDrawPerTurn == 3,
            "player Draw Reduction reapplication did not preserve both draw penalties");
    requirePublicTiming(publicPlayerStatusTiming(
                                drawReduction.publicBattleState(), "DRAW_REDUCTION"),
            "player_start_after_draw", "expire", false,
            "player Draw Reduction after reapplication");
}

void verifyHexaghostCycleCounterFailsClosed() {
    auto earlyCycle = makePublicBattleFixture(
            false, MonsterEncounter::HEXAGHOST);
    auto laterCycle = earlyCycle;
    auto &earlyHexaghost = earlyCycle.bc.monsters.arr[0];
    auto &laterHexaghost = laterCycle.bc.monsters.arr[0];
    earlyHexaghost.moveHistory[0] = MMID::HEXAGHOST_SEAR;
    earlyHexaghost.moveHistory[1] = MMID::HEXAGHOST_TACKLE;
    laterHexaghost.moveHistory[0] = MMID::HEXAGHOST_SEAR;
    laterHexaghost.moveHistory[1] = MMID::HEXAGHOST_TACKLE;
    earlyHexaghost.uniquePower0 = 0;
    laterHexaghost.uniquePower0 = 5;

    const auto earlyState = earlyCycle.publicBattleState();
    const auto laterState = laterCycle.publicBattleState();
    require(pybind11::cast<std::string>(earlyState["information_fidelity"])
                    == "supported",
            "Hexaghost cycle-counter fixture is not a supported public state");
    require(earlyState.equal(laterState),
            "Hexaghost cycle-counter fixtures do not have the same public battle state");
    require(earlyHexaghost.uniquePower0 != laterHexaghost.uniquePower0,
            "Hexaghost cycle-counter fixtures did not vary the hidden counter");
    requireSamplerRejected(earlyCycle, "Hexaghost early-cycle sampler anchor");
    requireSamplerRejected(laterCycle, "Hexaghost later-cycle sampler anchor");
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
    verifySamplerIgnoresPrivateCountersAndDrawOrder();
    verifySamplerPreservesKnownDrawConstraints();
    verifyGeneratedCardMembershipSurvivesPublicPileTransitions();
    verifyHiddenGeneratedIdentityRemainsFailClosedUntilObserved();
    verifyGeneratedCardIdentityIsRecordedInVisiblePiles();
    verifySamplerFailsClosedForUnsupportedFidelity();
    verifyLouseLatentProposalUsesOnlyPublicDamage();
    verifyPrivateMonsterFutureStillFailsClosed();
    verifyPublicStatusTimingSemantics();
    verifyPublicStatusTimingStackingAndReapplication();
    verifyHexaghostCycleCounterFailsClosed();
    std::cout << "PUBLIC_BATTLE_STATE_SEMANTICS_PASS\n";
    return 0;
}
