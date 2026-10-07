std::size_t knownDrawTopCount(const BattleContext &bc) {
    const auto &known = bc.knownDrawTopUniqueIds;
    if (known.size() > bc.cards.drawPile.size()) {
        return 0;
    }
    for (std::size_t idx = 0; idx < known.size(); ++idx) {
        const auto drawIdx = bc.cards.drawPile.size() - 1 - idx;
        if (bc.cards.drawPile[drawIdx].getUniqueId() != known[idx]) {
            // Never publish a remembered ordering unless it still agrees with
            // the native state.  This is a conservative fallback for any
            // uninstrumented random/reordering mechanic.
            return 0;
        }
    }
    for (const auto &[position, uniqueId] : bc.knownDrawPositionUniqueIds) {
        if (position < 0 || static_cast<std::size_t>(position) >= bc.cards.drawPile.size()) {
            return 0;
        }
        const auto drawIdx = bc.cards.drawPile.size() - 1
                - static_cast<std::size_t>(position);
        if (bc.cards.drawPile[drawIdx].getUniqueId() != uniqueId) {
            return 0;
        }
    }
    return known.size();
}

pybind11::dict publicDrawCardFace(const CardInstance &card) {
    pybind11::dict ret;
    ret["id"] = static_cast<int>(card.getId());
    ret["name"] = std::string(card.getName());
    ret["type"] = cardTypeLabel(card.getType());
    ret["upgraded"] = card.isUpgraded();
    ret["upgrade_count"] = card.getUpgradeCount();
    addPublicCardSpecialData(ret, card);
    return ret;
}

bool drawPilePublicMembershipKnown(const GameContext &gc, const BattleContext &bc) {
    for (const auto &card : bc.cards.drawPile) {
        const auto uniqueId = static_cast<int>(card.getUniqueId());
        const bool persistent = uniqueId >= 0
                && uniqueId < static_cast<int>(gc.deck.size())
                && card.getId() == gc.deck.cards[uniqueId].getId();
        if (persistent) {
            continue;
        }
        const auto known = bc.knownGeneratedCardPublicIdentity.find(card.getUniqueId());
        if (known == bc.knownGeneratedCardPublicIdentity.end() || !known->second) {
            return false;
        }
    }
    return true;
}

pybind11::list drawPilePublicMultiset(const BattleContext &bc) {
    using DrawPileFace = std::tuple<int, bool, int, bool, int>;
    std::map<DrawPileFace, int> counts;
    for (const auto &card : bc.cards.drawPile) {
        const bool hasSpecialData = card.usesSpecialData();
        ++counts[{static_cast<int>(card.getId()), card.isUpgraded(),
                card.getUpgradeCount(), hasSpecialData,
                hasSpecialData ? card.specialData : 0}];
    }
    pybind11::list ret;
    for (const auto &[face, count] : counts) {
        pybind11::dict row;
        row["id"] = std::get<0>(face);
        row["upgraded"] = std::get<1>(face);
        row["upgrade_count"] = std::get<2>(face);
        if (std::get<3>(face)) {
            row["special_data"] = std::get<4>(face);
        }
        row["count"] = count;
        ret.append(row);
    }
    return ret;
}

bool knownDrawStateConsistent(const BattleContext &bc) {
    const auto &known = bc.knownDrawTopUniqueIds;
    if (known.size() > bc.cards.drawPile.size()) {
        return false;
    }
    for (std::size_t idx = 0; idx < known.size(); ++idx) {
        const auto drawIdx = bc.cards.drawPile.size() - 1 - idx;
        if (bc.cards.drawPile[drawIdx].getUniqueId() != known[idx]) {
            return false;
        }
    }
    if (bc.knownDrawInsertionBaseSize >= 0) {
        if (!bc.knownDrawPositionUniqueIds.empty()
                || bc.knownDrawInsertionBaseSize
                                + static_cast<std::int32_t>(
                                        bc.knownDrawInsertionCards.size())
                        != static_cast<std::int32_t>(bc.cards.drawPile.size())) {
            return false;
        }
        std::map<std::int16_t, const CardInstance *> insertionById;
        for (const auto &insertion : bc.knownDrawInsertionCards) {
            if (insertion.uniqueId < 0
                    || !insertionById.emplace(insertion.uniqueId, nullptr).second) {
                return false;
            }
        }
        for (int drawIdx = 0;
                drawIdx < static_cast<int>(bc.cards.drawPile.size()); ++drawIdx) {
            const auto &card = bc.cards.drawPile[drawIdx];
            const auto insertion = insertionById.find(card.getUniqueId());
            if (insertion != insertionById.end()) {
                insertion->second = &card;
            }
        }
        std::vector<const CardInstance *> baseline;
        baseline.reserve(static_cast<std::size_t>(bc.knownDrawInsertionBaseSize));
        for (auto it = bc.cards.drawPile.rbegin(); it != bc.cards.drawPile.rend(); ++it) {
            if (insertionById.find(it->getUniqueId()) == insertionById.end()) {
                baseline.push_back(&*it);
            }
        }
        if (baseline.size()
                != static_cast<std::size_t>(bc.knownDrawInsertionBaseSize)) {
            return false;
        }
        std::map<std::int32_t, std::int16_t> anchorsByPosition;
        std::map<std::int16_t, std::int32_t> anchorPositionById;
        for (const auto &anchor : bc.knownDrawInsertionAnchors) {
            if (anchor.basePositionFromTop < 0
                    || static_cast<std::size_t>(anchor.basePositionFromTop)
                            >= baseline.size()
                    || !anchorsByPosition.emplace(
                            anchor.basePositionFromTop, anchor.uniqueId).second
                    || !anchorPositionById.emplace(
                            anchor.uniqueId, anchor.basePositionFromTop).second
                    || baseline[anchor.basePositionFromTop]->getUniqueId()
                            != anchor.uniqueId) {
                return false;
            }
        }
        for (const auto &[uniqueId, card] : insertionById) {
            if (card == nullptr) {
                return false;
            }
            const auto drawIdx = static_cast<std::int32_t>(
                    card - bc.cards.drawPile.data());
            const auto rankFromTop = static_cast<std::int32_t>(
                    bc.cards.drawPile.size() - 1) - drawIdx;
            const auto insertion = std::find_if(
                    bc.knownDrawInsertionCards.begin(),
                    bc.knownDrawInsertionCards.end(),
                    [&](const DrawKnowledgeInsertion &candidate) {
                        return candidate.uniqueId == uniqueId;
                    });
            if (insertion == bc.knownDrawInsertionCards.end()
                    || insertion->minimumPositionFromTop < 0
                    || rankFromTop < insertion->minimumPositionFromTop) {
                return false;
            }
            if (insertion->beforeAnchorUniqueId >= 0) {
                const auto anchor = anchorPositionById.find(
                        insertion->beforeAnchorUniqueId);
                if (anchor == anchorPositionById.end()) {
                    return false;
                }
                const auto anchorCard = baseline[anchor->second];
                const auto anchorDrawIdx = static_cast<std::int32_t>(
                        anchorCard - bc.cards.drawPile.data());
                const auto anchorRankFromTop = static_cast<std::int32_t>(
                        bc.cards.drawPile.size() - 1) - anchorDrawIdx;
                if (rankFromTop >= anchorRankFromTop) {
                    return false;
                }
            }
        }
        return true;
    }
    for (const auto &[position, uniqueId] : bc.knownDrawPositionUniqueIds) {
        if (position < static_cast<std::int32_t>(known.size())
                || static_cast<std::size_t>(position) >= bc.cards.drawPile.size()) {
            return false;
        }
        const auto drawIdx = bc.cards.drawPile.size() - 1
                - static_cast<std::size_t>(position);
        if (bc.cards.drawPile[drawIdx].getUniqueId() != uniqueId) {
            return false;
        }
    }
    return true;
}

pybind11::list knownDrawTopSnapshot(const BattleContext &bc, std::size_t count) {
    pybind11::list ret;
    for (std::size_t idx = 0; idx < count; ++idx) {
        const auto drawIdx = static_cast<int>(bc.cards.drawPile.size() - 1 - idx);
        ret.append(cardSnapshot(bc, bc.cards.drawPile[drawIdx], drawIdx, false));
    }
    return ret;
}

pybind11::list knownDrawPositionSnapshot(const BattleContext &bc) {
    pybind11::list ret;
    for (const auto &[position, uniqueId] : bc.knownDrawPositionUniqueIds) {
        const auto drawIdx = static_cast<int>(bc.cards.drawPile.size() - 1
                - static_cast<std::size_t>(position));
        pybind11::dict fact;
        fact["position_from_top"] = position;
        fact["card"] = cardSnapshot(bc, bc.cards.drawPile[drawIdx], drawIdx, false);
        ret.append(fact);
    }
    return ret;
}

bool publicInformationUnsupported(const GameContext &gc, const BattleContext &bc) {
    if (bc.player.cc == CharacterClass::DEFECT) {
        return true;
    }
    if (bc.knownDrawUnsupportedReasons != 0 || !knownDrawStateConsistent(bc)) {
        return true;
    }
    if (!drawPilePublicMembershipKnown(gc, bc)) {
        return true;
    }
    for (int idx = 0; idx < bc.monsters.monsterCount; ++idx) {
        if (publicMonsterCounterKnowledge(bc, bc.monsters.arr[idx])
                == PublicMonsterCounterKnowledge::MIXED_UNSUPPORTED) {
            return true;
        }
    }
    return false;
}

pybind11::list drawKnowledgeUnsupportedReasonSnapshot(const std::uint8_t reasons) {
    pybind11::list ret;
    if (reasons & static_cast<std::uint8_t>(DrawKnowledgeUnsupportedReason::SUBSET_MEMBERSHIP)) {
        ret.append("subset_membership");
    }
    if (reasons & static_cast<std::uint8_t>(DrawKnowledgeUnsupportedReason::UNKNOWN_INSERTION)) {
        ret.append("unknown_insertion");
    }
    if (reasons & static_cast<std::uint8_t>(DrawKnowledgeUnsupportedReason::INCONSISTENT_EXACT_FACT)) {
        ret.append("inconsistent_exact_fact");
    }
    if (reasons & static_cast<std::uint8_t>(DrawKnowledgeUnsupportedReason::INSERTION_DRAW_IDENTITY_AMBIGUOUS)) {
        ret.append("insertion_draw_identity_ambiguous");
    }
    if (reasons & static_cast<std::uint8_t>(DrawKnowledgeUnsupportedReason::INSERTION_NON_TOP_DRAW_UNREPRESENTED)) {
        ret.append("insertion_non_top_draw_unrepresented");
    }
    if (reasons & static_cast<std::uint8_t>(DrawKnowledgeUnsupportedReason::INSERTION_CONSTRAINT_INCONSISTENT)) {
        ret.append("insertion_constraint_inconsistent");
    }
    if (reasons & static_cast<std::uint8_t>(DrawKnowledgeUnsupportedReason::INSERTION_MEMBERSHIP_UNREPRESENTED)) {
        ret.append("insertion_membership_unrepresented");
    }
    return ret;
}

pybind11::dict makePublicBattleState(
        const GameContext &gc,
        const BattleContext &bc,
        const std::vector<LightSpeedAction> &actions) {
    const bool drawStateConsistent = knownDrawStateConsistent(bc);
    auto projectedDrawUnsupportedReasons = static_cast<std::uint8_t>(
            bc.knownDrawUnsupportedReasons
            | (drawStateConsistent ? 0 : static_cast<std::uint8_t>(
                    DrawKnowledgeUnsupportedReason::INCONSISTENT_EXACT_FACT)));
    if (!drawPilePublicMembershipKnown(gc, bc)) {
        projectedDrawUnsupportedReasons |= static_cast<std::uint8_t>(
                DrawKnowledgeUnsupportedReason::INSERTION_MEMBERSHIP_UNREPRESENTED);
    }
    pybind11::dict ret;
    ret["schema_id"] = "native-public-battle-state-v1";
    ret["information_regime"] = "normal_public";
    ret["screen_identity"] = "BATTLE";
    ret["act"] = gc.act;
    ret["floor_num"] = gc.floorNum;
    ret["encounter_id"] = monsterEncounterEnumNames[static_cast<int>(bc.encounter)];
    ret["turn"] = bc.turn;
    ret["input_state"] = inputStateLabel(bc.inputState);
    ret["battle_outcome"] = battleOutcomeLabel(bc.outcome);
    ret["player"] = playerSnapshot(bc.player);
    ret["hand"] = handSnapshot(bc);
    ret["discard_pile"] = pileSnapshot(bc, bc.cards.discardPile);
    ret["exhaust_pile"] = pileSnapshot(bc, bc.cards.exhaustPile);
    ret["draw_pile_size"] = static_cast<int>(bc.cards.drawPile.size());
    ret["monsters"] = publicMonsterGroupSnapshot(bc);
    ret["information_fidelity"] = publicInformationUnsupported(gc, bc)
            ? "unsupported_fidelity" : "supported";
    ret["draw_knowledge_unsupported_reasons"] =
            drawKnowledgeUnsupportedReasonSnapshot(projectedDrawUnsupportedReasons);
    pybind11::dict resources;
    resources["deck"] = deckSnapshot(gc);
    resources["relics"] = relicListSnapshot(gc);
    resources["potions"] = potionListSnapshot(bc);
    resources["gold"] = gc.gold;
    resources["blue_key"] = gc.blueKey;
    resources["green_key"] = gc.greenKey;
    resources["red_key"] = gc.redKey;
    ret["persistent_resources"] = resources;

    pybind11::dict visibility;
    pybind11::dict drawOrder;
    const bool frozenEye = bc.player.hasRelic<R::FROZEN_EYE>();
    const auto knownTopCount = frozenEye ? bc.cards.drawPile.size() : knownDrawTopCount(bc);
    const bool knownStateConsistent = drawStateConsistent;
    const bool hasKnownPositions = knownStateConsistent
            && !bc.knownDrawPositionUniqueIds.empty();
    const bool hasInsertionConstraints = bc.knownDrawInsertionBaseSize >= 0;
    if (projectedDrawUnsupportedReasons != 0 || !knownStateConsistent) {
        drawOrder["classification"] = "unsupported_fidelity";
        drawOrder["constraint"] = "an information-changing draw transition is not modeled exactly";
        drawOrder["fidelity"] = "unsupported_fidelity";
        drawOrder["unsupported_reasons"] = drawKnowledgeUnsupportedReasonSnapshot(
                projectedDrawUnsupportedReasons);
        if (knownStateConsistent && knownTopCount > 0) {
            drawOrder["known_top_prefix"] = knownDrawTopSnapshot(bc, knownTopCount);
        }
        if (hasKnownPositions) {
            drawOrder["known_positions"] = knownDrawPositionSnapshot(bc);
        }
    } else if (frozenEye) {
        drawOrder["classification"] = "full_public_exact";
        drawOrder["constraint"] = "Frozen Eye makes the current draw order visible";
        drawOrder["fidelity"] = "native-current-information-v3";
        drawOrder["visible_order_from_top"] =
                knownDrawTopSnapshot(bc, knownTopCount);
    } else if (hasInsertionConstraints) {
        drawOrder["classification"] = "random_insertion_constraints";
        drawOrder["constraint"] =
                "known inserted-card membership and public position domains over a hidden baseline permutation";
        drawOrder["fidelity"] = "native-current-information-v3";
        if (knownTopCount > 0) {
            drawOrder["known_top_prefix"] = knownDrawTopSnapshot(bc, knownTopCount);
        }
        pybind11::list baselineAnchors;
        for (const auto &anchor : bc.knownDrawInsertionAnchors) {
            const auto card = std::find_if(
                    bc.cards.drawPile.begin(), bc.cards.drawPile.end(),
                    [&](const CardInstance &candidate) {
                        return candidate.getUniqueId() == anchor.uniqueId;
                    });
            if (card == bc.cards.drawPile.end()) {
                continue;
            }
            pybind11::dict fact;
            fact["baseline_position_from_top"] = anchor.basePositionFromTop;
            fact["card"] = publicDrawCardFace(*card);
            baselineAnchors.append(fact);
        }
        drawOrder["baseline_order_anchors"] = baselineAnchors;
        using InsertionFace = std::tuple<int, bool, int, bool, int, int, int>;
        std::map<InsertionFace, int> groupedInsertions;
        for (const auto &insertion : bc.knownDrawInsertionCards) {
            const auto card = std::find_if(
                    bc.cards.drawPile.begin(), bc.cards.drawPile.end(),
                    [&](const CardInstance &candidate) {
                        return candidate.getUniqueId() == insertion.uniqueId;
                    });
            if (card == bc.cards.drawPile.end()) {
                continue;
            }
            int beforeAnchorPosition = -1;
            if (insertion.beforeAnchorUniqueId >= 0) {
                const auto anchor = std::find_if(
                        bc.knownDrawInsertionAnchors.begin(),
                        bc.knownDrawInsertionAnchors.end(),
                        [&](const DrawKnowledgeAnchor &candidate) {
                            return candidate.uniqueId
                                    == insertion.beforeAnchorUniqueId;
                        });
                if (anchor != bc.knownDrawInsertionAnchors.end()) {
                    beforeAnchorPosition = anchor->basePositionFromTop;
                }
            }
            const bool hasSpecialData = card->usesSpecialData();
            ++groupedInsertions[{static_cast<int>(card->getId()),
                    card->isUpgraded(), card->getUpgradeCount(), hasSpecialData,
                    hasSpecialData ? card->specialData : 0,
                    insertion.minimumPositionFromTop, beforeAnchorPosition}];
        }
        pybind11::list insertionConstraints;
        for (const auto &[key, count] : groupedInsertions) {
            pybind11::dict constraint;
            pybind11::dict cardFace;
            cardFace["id"] = std::get<0>(key);
            cardFace["upgraded"] = std::get<1>(key);
            cardFace["upgrade_count"] = std::get<2>(key);
            if (std::get<3>(key)) {
                cardFace["special_data"] = std::get<4>(key);
            }
            constraint["card"] = cardFace;
            constraint["count"] = count;
            constraint["minimum_position_from_top"] = std::get<5>(key);
            if (std::get<6>(key) >= 0) {
                constraint["before_baseline_position_from_top"] = std::get<6>(key);
            }
            insertionConstraints.append(constraint);
        }
        drawOrder["inserted_card_constraints"] = insertionConstraints;
    } else if (knownTopCount > 0 || hasKnownPositions) {
        const auto classification = hasKnownPositions ? "known_positions" : "known_prefix";
        drawOrder["classification"] = classification;
        drawOrder["constraint"] = hasKnownPositions
                ? "public deterministic exact draw-pile positions"
                : "public deterministic top-of-draw-pile placement";
        drawOrder["fidelity"] = "native-current-information-v3";
        if (knownTopCount > 0) {
            drawOrder["known_top_prefix"] = knownDrawTopSnapshot(bc, knownTopCount);
        }
        if (hasKnownPositions) {
            drawOrder["known_positions"] = knownDrawPositionSnapshot(bc);
        }
    } else {
        drawOrder["classification"] = "hidden";
        drawOrder["constraint"] = "ordinary draw order is not exposed";
        drawOrder["fidelity"] = "native-current-information-v3";
    }
    visibility["draw_order"] = drawOrder;
    visibility["information_fidelity"] = publicInformationUnsupported(gc, bc)
            ? "unsupported_fidelity" : "supported";
    pybind11::dict enemyIntent;
    if (bc.player.hasRelic<R::RUNIC_DOME>()) {
        enemyIntent["classification"] = "hidden";
        enemyIntent["fidelity"] = "native-current-information-v3";
    } else {
        enemyIntent["classification"] = "public_exact";
        enemyIntent["source"] = "native Monster move state";
        enemyIntent["fidelity"] = "native-current-information-v3";
    }
    visibility["enemy_intent"] = enemyIntent;
    ret["visibility"] = visibility;

    pybind11::list publicActions;
    for (const auto &action : actions) {
        publicActions.append(publicActionIdentity(action));
    }
    ret["ordered_public_legal_actions"] = publicActions;
    pybind11::dict membership;
    membership["classification"] = projectedDrawUnsupportedReasons != 0
            ? "unsupported_fidelity" : frozenEye ? "full_public_exact" : "public_constraint";
    if (projectedDrawUnsupportedReasons != 0) {
        membership["unsupported_reasons"] = drawKnowledgeUnsupportedReasonSnapshot(
                projectedDrawUnsupportedReasons);
    }
    if (frozenEye) {
        membership["visible_order_from_top"] = knownDrawTopSnapshot(bc, knownTopCount);
    }
    if (drawPilePublicMembershipKnown(gc, bc)) {
        membership["multiset_counts"] = drawPilePublicMultiset(bc);
    }
    ret["draw_pile_membership"] = membership;
    return ret;
}
