#include <cassert>
#include <iostream>
#include <vector>

#include "game/GameContext.h"
#include "sim/search/GameAction.h"

namespace {

using sts::GameContext;
using sts::Rewards;
using sts::ScreenState;
using sts::search::GameAction;

std::vector<GameAction> goldActions(const GameContext &gc) {
    std::vector<GameAction> result;
    for (const auto &action : GameAction::getAllActionsInState(gc)) {
        if (action.getRewardsActionType() == GameAction::RewardsActionType::GOLD) {
            result.push_back(action);
        }
    }
    return result;
}

GameContext rewardsContext(Rewards rewards) {
    GameContext gc;
    gc.screenState = ScreenState::REWARDS;
    gc.info.rewardsContainer = rewards;
    return gc;
}

void checkTwoDifferentGoldSlots() {
    Rewards rewards;
    rewards.addGold(17);
    rewards.addGold(41);

    const auto listed = goldActions(rewardsContext(rewards));
    assert(listed.size() == 2);
    assert(listed[0].getIdx1() == 0);
    assert(listed[1].getIdx1() == 1);
    assert(listed[0].bits != listed[1].bits);
    assert(listed[0].isValidAction(rewardsContext(rewards)));
    assert(listed[1].isValidAction(rewardsContext(rewards)));

    auto takeFirst = rewardsContext(rewards);
    listed[0].execute(takeFirst);
    assert(takeFirst.gold == 99 + 17);
    assert(takeFirst.info.rewardsContainer.goldRewardCount == 1);
    assert(takeFirst.info.rewardsContainer.gold[0] == 41);

    auto takeSecond = rewardsContext(rewards);
    listed[1].execute(takeSecond);
    assert(takeSecond.gold == 99 + 41);
    assert(takeSecond.info.rewardsContainer.goldRewardCount == 1);
    assert(takeSecond.info.rewardsContainer.gold[0] == 17);
}

void checkSameAmountStillHasDistinctSlots() {
    Rewards rewards;
    rewards.addGold(23);
    rewards.addGold(23);

    const auto listed = goldActions(rewardsContext(rewards));
    assert(listed.size() == 2);
    assert(listed[0].getIdx1() == 0);
    assert(listed[1].getIdx1() == 1);
    assert(listed[0].bits != listed[1].bits);

    auto takeSecond = rewardsContext(rewards);
    listed[1].execute(takeSecond);
    assert(takeSecond.gold == 99 + 23);
    assert(takeSecond.info.rewardsContainer.goldRewardCount == 1);
    assert(takeSecond.info.rewardsContainer.gold[0] == 23);
}

void checkSingleGoldWithUnrelatedReward() {
    Rewards rewards;
    rewards.addGold(17);
    rewards.addPotion(sts::Potion::BLOOD_POTION);
    const auto gc = rewardsContext(rewards);

    const auto allActions = GameAction::getAllActionsInState(gc);
    const auto listed = goldActions(gc);
    assert(listed.size() == 1);
    assert(listed[0].getIdx1() == 0);
    assert(listed[0].isValidAction(gc));
    bool hasPotionAction = false;
    for (const auto &action : allActions) {
        if (action.getRewardsActionType() == GameAction::RewardsActionType::POTION) {
            hasPotionAction = action.getIdx1() == 0 && action.isValidAction(gc);
        }
    }
    assert(hasPotionAction);
}

} // namespace

int main() {
    checkTwoDifferentGoldSlots();
    checkSameAmountStillHasDistinctSlots();
    checkSingleGoldWithUnrelatedReward();
    std::cout << "gold reward action identity regression passed\n";
}
