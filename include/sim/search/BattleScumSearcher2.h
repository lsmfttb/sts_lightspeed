//
// Created by keega on 9/17/2021.
//

#ifndef STS_LIGHTSPEED_BATTLESCUMSEARCHER2_H
#define STS_LIGHTSPEED_BATTLESCUMSEARCHER2_H

#include "sim/search/Action.h"

#include <functional>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <iostream>
#include <string>
#include <vector>
#include <limits>
#include <vector>

namespace sts::search {

    typedef std::function<double (const BattleContext&)> EvalFnc;
    typedef std::function<std::vector<double> (
            const BattleContext&, const std::vector<Action>&)> PolicyPriorFnc;
    typedef std::function<double (
            const BattleContext&, const std::vector<Action>&)> LeafValueFnc;

    struct TreeGeometryDepthRow {
        std::int64_t depth = 0;
        std::int64_t expandedNodeCount = 0;
        std::int64_t discoveredChildEdgeCount = 0;
        std::int64_t visitedChildEdgeCount = 0;
        std::map<std::int64_t, std::int64_t> branchingHistogram;
    };

    struct TreeGeometryTelemetry {
        std::int64_t totalExpandedNodeCount = 0;
        std::int64_t totalDiscoveredChildEdgeCount = 0;
        std::int64_t totalVisitedChildEdgeCount = 0;
        std::int64_t maxExpandedDepth = -1;
        std::vector<TreeGeometryDepthRow> depthRows;
    };

    // to find a solution to a battle with tree pruning
    struct BattleScumSearcher2 {
        struct StateUtilizationRecord {
            std::int64_t expansionOrdinal = 0;
            int depth = 0;
            std::string exactStateDigest;
            bool firstSeen = false;
            std::int64_t firstSeenExpansionOrdinal = 0;
            int firstSeenDepth = 0;
            std::string pathFingerprint;
        };

        struct StateUtilizationTelemetry {
            static constexpr const char *schemaId = "native-battle-search-v2-state-utilization-v1";
            bool identityComplete = true;
            std::string identityUnavailableReason;
            std::string digestAlgorithm = "fnv1a128-v1";
            int collisionCount = 0;
            std::vector<StateUtilizationRecord> records;
        };

        class Edge;
        struct Node {
            std::int64_t simulationCount = 0;
            double evaluationSum = 0;
            std::vector<Edge> edges;
            std::vector<double> policyPriors;
        };

        struct Edge {
            Action action;
            Node node;
        };

        std::unique_ptr<const BattleContext> rootState;
        Node root;

        EvalFnc evalFnc;
        PolicyPriorFnc policyPriorFnc;
        LeafValueFnc learnedLeafValueFnc;
        bool useLearnedLeafValue = false;
        double explorationParameter = 3*sqrt(2);
        double policyExplorationParameter = 1.0;

        double bestActionValue = std::numeric_limits<double>::min();
        double minActionValue = std::numeric_limits<double>::max();
        int outcomePlayerHp = 0;
        bool includePotions = true;
        std::int64_t actionExecutionCount = 0;
        std::int64_t expandedNodeCount = 0;
        std::int64_t policyPriorCallCount = 0;
        std::int64_t leafValueCallCount = 0;

        std::vector<Action> bestActionSequence;
        std::default_random_engine randGen;

        std::vector<Node*> searchStack;
        std::vector<Action> actionStack;

        bool stateUtilizationEnabled = false;
        StateUtilizationTelemetry stateUtilizationTelemetry;
        std::vector<std::string> stateUtilizationPayloads;

        explicit BattleScumSearcher2(const BattleContext &bc, EvalFnc evalFnc=&evaluateEndState);

        // public methods
        void search(int64_t simulations);
        void step();
        void stepFromRootEdge(int rootEdgeIdx);
        [[nodiscard]] TreeGeometryTelemetry buildTreeGeometryTelemetry() const;

        // private helpers
        void updateFromEvaluation(const std::vector<Node*> &stack, const std::vector<Action> &actionStack, double evaluation, const BattleContext *terminalState=nullptr);
        void updateFromPlayout(const std::vector<Node*> &stack, const std::vector<Action> &actionStack, const BattleContext &endState);
        [[nodiscard]] bool isTerminalState(const BattleContext &bc) const;

        double evaluateEdge(const Node &parent, int edgeIdx);
        int selectBestEdgeToSearch(const Node &cur);
        int selectFirstActionForLeafNode(const Node &leafNode);

        void playoutRandom(BattleContext &state, std::vector<Action> &actionStack);

        void enumerateActionsForNode(
                Node &node, const BattleContext &bc, bool applyPolicyPriors=true);
        void applyPolicyPriors(Node &node, const BattleContext &bc);
        void enumerateCardActions(Node &node, const BattleContext &bc);
        void enumeratePotionActions(Node &node, const BattleContext &bc);
        void enumerateCardSelectActions(Node &node, const BattleContext &bc);
        static double evaluateEndState(const BattleContext &bc);

        void printSearchTree(std::ostream &os, int levels);
        void printSearchStack(std::ostream &os, bool skipLast=false);

        void enableStateUtilizationTelemetry();
        void recordExpandedState(const BattleContext &state, int depth,
                                 const std::vector<Action> &path);
    };

    extern thread_local BattleScumSearcher2 *g_debug_scum_search;

}


#endif //STS_LIGHTSPEED_BATTLESCUMSEARCHER2_H
