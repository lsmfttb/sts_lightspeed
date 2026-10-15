#include "sim/search/Action.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

void require(const bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << "\n";
        std::exit(EXIT_FAILURE);
    }
}

constexpr std::uint32_t actionBits(
        const sts::search::ActionType type,
        const int source,
        const int target) {
    return (static_cast<std::uint32_t>(type) << 29)
            | ((static_cast<std::uint32_t>(target) & 0x1FFF) << 16)
            | (static_cast<std::uint32_t>(source) & 0xFFFF);
}

}

int main() {
    using sts::search::ActionType;
    using sts::search::sameBattleRootActionIdentity;

    const auto internalFairyDiscard = actionBits(ActionType::POTION, 0, -1);
    const auto publicFairyDiscard = actionBits(ActionType::POTION, 0, 6);
    require(internalFairyDiscard == 1073676288U,
            "internal -1 discard bits changed");
    require(publicFairyDiscard == 537264128U,
            "public target-6 discard bits changed");
    require(internalFairyDiscard != publicFairyDiscard, "discard encodings must differ");
    require(sameBattleRootActionIdentity(internalFairyDiscard, publicFairyDiscard),
            "internal and public discard encodings must map");

    require(!sameBattleRootActionIdentity(
                    internalFairyDiscard, actionBits(ActionType::POTION, 1, 6)),
            "discard from a different potion slot must not map");
    require(!sameBattleRootActionIdentity(
                    actionBits(ActionType::POTION, 0, 0), publicFairyDiscard),
            "potion use must not map to discard");

    const auto endTurn = static_cast<std::uint32_t>(ActionType::END_TURN) << 29;
    require(sameBattleRootActionIdentity(endTurn, endTurn), "end-turn identity changed");
    require(sameBattleRootActionIdentity(
                    actionBits(ActionType::CARD, 2, 1),
                    actionBits(ActionType::CARD, 2, 1)),
            "exact card action identity changed");
    require(!sameBattleRootActionIdentity(
                    actionBits(ActionType::CARD, 2, 1),
                    actionBits(ActionType::CARD, 3, 1)),
            "different card occurrences must not map");

    std::cout << "battle root action identity regression passed\n";
    return 0;
}
