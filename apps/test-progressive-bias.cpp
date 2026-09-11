#include "sim/search/BattleScumSearcher2.h"
#include "game/GameContext.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace sts;
using Search = search::BattleScumSearcher2;

void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("progressive bias mechanism check failed");
    }
}

void close(double actual, double expected) {
    require(std::abs(actual - expected) < 1e-14);
}

int main() {
    BattleContext state{};
    state.player.curHp = 60;
    state.player.maxHp = 80;
    state.player.block = 20;
    state.turn = 5;
    state.monsters.monsterCount = 3;
    for (auto &monster : state.monsters.arr) {
        monster = {};
    }
    state.monsters.arr[0].curHp = 30;
    state.monsters.arr[0].maxHp = 40;
    state.monsters.arr[1].curHp = 10;
    state.monsters.arr[1].maxHp = 60;
    state.monsters.arr[2].curHp = 999;
    state.monsters.arr[2].maxHp = 999;
    state.monsters.arr[2].isEscapingB = true;
    require(state.monsters.arr[0].isTargetable());
    require(!state.monsters.arr[2].isTargetable());
    const auto h1 = Search::combatHandcraftedH1(state);
    require(h1.available);
    close(h1.playerHpFraction, 0.75);
    close(h1.activeEnemyHpFraction, 0.4);
    close(h1.blockFraction, 0.2);
    close(h1.turnFraction, 0.25);
    close(h1.raw, 0.775);
    close(h1.value, std::tanh(0.775));
    state.turn = 200;
    close(Search::combatHandcraftedH1(state).turnFraction, 1);
    state.turn = -5;
    close(Search::combatHandcraftedH1(state).turnFraction, 0);
    state.monsters.monsterCount = 0;
    close(Search::combatHandcraftedH1(state).activeEnemyHpFraction, 0);
    state.player.maxHp = 0;
    require(Search::combatHandcraftedH1(state).unavailableReason == "invalid_h1_input");
    state.outcome = Outcome::PLAYER_VICTORY;
    require(Search::combatHandcraftedH1(state).unavailableReason == "terminal_state");

    Search searcher(state);
    Search::Node parent;
    parent.simulationCount = 20;
    parent.expansionOrdinal = 7;
    parent.edges.resize(1);
    parent.edges[0].node.simulationCount = 3;
    parent.edges[0].node.evaluationSum = 12;
    parent.childHeuristics = {h1};
    searcher.bestActionSequence = {search::Action(search::ActionType::END_TURN)};
    searcher.bestActionValue = 8;
    searcher.minActionValue = 2;
    // Independent frozen exploitation and exploration formula, no helper reuse.
    const double frozen = (12.0 / 4.0) / 6.0
            + 3 * std::sqrt(2.0) * std::sqrt(std::log(21.0) / 4.0);
    close(searcher.evaluateEdge(parent, 0), frozen);
    close(searcher.evaluateTreePolicyEdge(parent, 0, 2), frozen);
    require(searcher.progressiveBiasScoreCount == 0);
    require(searcher.progressiveBiasAudit.empty());
    searcher.progressiveBiasEnabled = true;
    searcher.progressiveBiasAuditLimit = 2;
    close(searcher.evaluateTreePolicyEdge(parent, 0, 2), frozen + 0.5 * h1.value / 4);
    const double firstBias = searcher.progressiveBiasAudit.back().biasContribution;
    parent.edges[0].node.simulationCount = 7;
    searcher.evaluateTreePolicyEdge(parent, 0, 2);
    close(searcher.progressiveBiasAudit.back().biasContribution, firstBias / 2);
    require(searcher.progressiveBiasAudit.back().parentDepth == 2);
    require(searcher.progressiveBiasAudit.back().parentExpansionOrdinal == 7);
    parent.childHeuristics[0].value = -0.8;
    close(searcher.evaluateTreePolicyEdge(parent, 0, 2),
            searcher.evaluateEdge(parent, 0) - 0.05);
    require(searcher.progressiveBiasScoreCount == 3);
    require(searcher.progressiveBiasAudit.size() == 2);
    parent.childHeuristics[0].available = false;
    close(searcher.evaluateTreePolicyEdge(parent, 0, 2), searcher.evaluateEdge(parent, 0));
    searcher.progressiveBiasEnabled = false;
    close(searcher.evaluateTreePolicyEdge(parent, 0, 2), searcher.evaluateEdge(parent, 0));

    // Existing zero-range behavior is part of the frozen base, not a repair.
    searcher.minActionValue = searcher.bestActionValue;
    require(std::isinf(searcher.evaluateTreePolicyEdge(parent, 0, 2)));
    parent.edges[0].node.evaluationSum = 0;
    require(std::isnan(searcher.evaluateTreePolicyEdge(parent, 0, 2)));

    // This is a real, bounded traversal rather than a direct helper call.
    // The fixed small simulation count revisits an expanded root child, so
    // selection records at least one non-root parent depth.
    GameContext game(CharacterClass::IRONCLAD, 880088, 0);
    BattleContext battle;
    battle.init(game, MonsterEncounter::CULTIST);
    Search traversedSearcher(battle);
    traversedSearcher.includePotions = false;
    traversedSearcher.progressiveBiasEnabled = true;
    traversedSearcher.progressiveBiasAuditLimit = 256;
    traversedSearcher.search(32);
    require(traversedSearcher.progressiveBiasScoreCount > 0);
    require(!traversedSearcher.progressiveBiasAudit.empty());
    bool observedBeyondRoot = false;
    for (const auto &row : traversedSearcher.progressiveBiasAudit) {
        if (row.parentDepth > 0) {
            observedBeyondRoot = true;
            require(row.childH1.available);
            close(row.biasContribution,
                    Search::progressiveBiasWeight * row.childH1.value
                            / (1 + row.childVisitCount));
        }
    }
    require(observedBeyondRoot);
    require(traversedSearcher.heuristicSuccessorTransitionCount > 0);
    require(traversedSearcher.rolloutCount > 0);
    require(traversedSearcher.terminalUtilityEvaluationCount > 0);

    // Re-entering search starts a separate work/audit report even if its tree
    // remains available for normal search continuation semantics.
    traversedSearcher.search(0);
    require(traversedSearcher.actionExecutionCount == 0);
    require(traversedSearcher.expandedNodeCount == 0);
    require(traversedSearcher.policyPriorCallCount == 0);
    require(traversedSearcher.leafValueCallCount == 0);
    require(traversedSearcher.rolloutCount == 0);
    require(traversedSearcher.terminalUtilityEvaluationCount == 0);
    require(traversedSearcher.heuristicSuccessorTransitionCount == 0);
    require(traversedSearcher.heuristicAvailableCount == 0);
    require(traversedSearcher.heuristicTerminalUnavailableCount == 0);
    require(traversedSearcher.heuristicInvalidUnavailableCount == 0);
    require(traversedSearcher.progressiveBiasScoreCount == 0);
    require(traversedSearcher.progressiveBiasAudit.empty());
    std::cout << "H1 components, signed all-node bias, decay, disabled recovery, "
                 "audit bound, depth>0 traversal, independent-call counter reset, "
                 "and frozen zero-range UCT checks passed\n";
}
