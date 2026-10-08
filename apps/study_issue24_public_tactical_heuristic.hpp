#pragma once

// Shared comparator source used by the Issue 17 pre-outcome qualification and
// prospective Stage B runner. Include after defining pb, stringField,
// intField, and containsPrivateField in the including translation unit.

int estimatedBlock(const pb::dict &card) {
    const auto name = stringField(card, "name");
    const bool upgraded = card["upgraded"].cast<bool>();
    if (name == "Defend") return upgraded ? 8 : 5;
    if (name == "Shrug It Off") return upgraded ? 11 : 8;
    if (name == "Ghostly Armor") return upgraded ? 13 : 10;
    if (name == "Flame Barrier") return upgraded ? 16 : 12;
    if (name == "Power Through") return 15;
    if (name == "Impervious") return upgraded ? 40 : 30;
    if (name == "True Grit") return upgraded ? 9 : 7;
    if (name == "Iron Wave") return upgraded ? 7 : 5;
    if (name == "Auto-Shields") return upgraded ? 15 : 11;
    return 0;
}

int estimatedAttackDamage(
        const pb::dict &card,
        const pb::dict &player,
        const pb::dict &monster) {
    const int cardId = intField(card, "id");
    const bool upgraded = card["upgraded"].cast<bool>();
    const int base = std::max(0, getBaseDamage(static_cast<CardId>(cardId), upgraded));
    int damage = std::max(0, base + intField(player, "strength"));
    if (intField(player, "weak") > 0) damage = damage * 3 / 4;
    if (intField(monster, "vulnerable") > 0) damage = damage * 3 / 2;
    // The estimate is HP damage after the target's visible block is applied.
    return std::max(0, damage - intField(monster, "block"));
}

int publicIncomingDamagePerHit(const pb::dict &monster) {
    const auto baseDamage = monster["move_base_damage"];
    if (!baseDamage.is_none()) return baseDamage.cast<int>();
    const auto modifiedDamage = monster["move_damage_to_player"];
    if (!modifiedDamage.is_none()) return modifiedDamage.cast<int>();
    throw std::runtime_error(
            "public heuristic cannot score an attacking intent with unavailable damage");
}

pb::dict selectPublicHeuristicAction(const pb::dict &state, pb::dict *audit = nullptr) {
    if (containsPrivateField(state)) {
        throw std::logic_error("public heuristic received a private field");
    }
    const auto actions = state["ordered_public_legal_actions"].cast<pb::list>();
    const auto hand = state["hand"].cast<pb::list>();
    const auto monsters = state["monsters"].cast<pb::list>();
    const auto player = state["player"].cast<pb::dict>();
    const auto resources = state["persistent_resources"].cast<pb::dict>();
    const auto potions = resources["potions"].cast<pb::list>();
    int incomingDamage = 0;
    for (const auto monsterHandle : monsters) {
        const auto monster = pb::reinterpret_borrow<pb::dict>(monsterHandle);
        if (!monster["alive"].cast<bool>() || !monster["attacking"].cast<bool>()) continue;
        incomingDamage += publicIncomingDamagePerHit(monster)
                * intField(monster, "move_hits");
    }
    const int currentBlock = intField(player, "block");
    const int hp = intField(player, "current_hp");
    const int maxHp = std::max(1, intField(player, "max_hp"));
    bool gremlinNobAlive = false;
    bool safeSetupWindow = true;
    for (const auto monsterHandle : monsters) {
        const auto monster = pb::reinterpret_borrow<pb::dict>(monsterHandle);
        if (!monster["alive"].cast<bool>()) continue;
        gremlinNobAlive = gremlinNobAlive
                || stringField(monster, "id_label") == "GREMLIN_NOB";
        safeSetupWindow = safeSetupWindow && !monster["attacking"].cast<bool>();
    }

    int bestIndex = -1;
    double bestScore = -std::numeric_limits<double>::infinity();
    int bestTargetTie = std::numeric_limits<int>::min();
    std::string bestReason;
    int bestEstimatedDamage = 0;
    int bestEstimatedBlock = 0;
    pb::list powerActionScores;
    for (int index = 0; index < actions.size(); ++index) {
        const auto action = pb::reinterpret_borrow<pb::dict>(actions[index]);
        const auto kind = stringField(action, "kind");
        double score = -std::numeric_limits<double>::infinity();
        int targetTie = 0;
        int estimatedDamage = 0;
        int estimatedBlockValue = 0;
        std::string reason;
        if (kind == "potion") {
            const int potionIndex = intField(action, "idx1");
            if (potionIndex >= 0 && potionIndex < potions.size()) {
                const auto potion = pb::reinterpret_borrow<pb::dict>(potions[potionIndex]);
                const auto potionName = stringField(potion, "name");
                const int risk = std::max(0, incomingDamage - currentBlock);
                if (potionName == "Blood Potion"
                        && hp * 100 <= maxHp * 35
                        && maxHp - hp >= maxHp / 5) {
                    score = 400.0;
                    reason = "heal only below 35% HP with meaningful missing health";
                } else if (potionName == "Block Potion"
                        && risk > 0
                        && (risk >= hp || hp - risk <= maxHp / 5)) {
                    score = 390.0;
                    reason = "block potion only when visible incoming damage is dangerous";
                } else if (potionName == "Fire Potion") {
                    const int target = intField(action, "idx2");
                    if (target >= 0 && target < monsters.size()) {
                        const auto monster = pb::reinterpret_borrow<pb::dict>(monsters[target]);
                        if (monster["alive"].cast<bool>()
                                && intField(monster, "current_hp")
                                        + intField(monster, "block") <= 20) {
                            score = 350.0;
                            targetTie = -intField(monster, "current_hp");
                            reason = "fire potion secures a visible lethal";
                        }
                    }
                }
            }
        } else if (kind == "card") {
            const int source = intField(action, "idx1");
            if (source < 0 || source >= hand.size()) continue;
            const auto card = pb::reinterpret_borrow<pb::dict>(hand[source]);
            const auto type = stringField(card, "type");
            const int cost = intField(card, "cost_for_turn");
            const int energyCost = card["free_to_play_once"].cast<bool>()
                    ? 0 : std::max(0, cost);
            const int target = intField(action, "idx2");
            if (type == "ATTACK" || (type == "SKILL" && estimatedBlock(card) > 0)) {
                int selectedDamage = 0;
                int selectedTargetHp = std::numeric_limits<int>::max();
                int selectedTargetEffectiveHp = std::numeric_limits<int>::max();
                for (int monsterIndex = 0; monsterIndex < monsters.size(); ++monsterIndex) {
                    if (target >= 0 && target < monsters.size() && target != monsterIndex) continue;
                    const auto monster = pb::reinterpret_borrow<pb::dict>(monsters[monsterIndex]);
                    if (!monster["alive"].cast<bool>() || !monster["targetable"].cast<bool>()) continue;
                    const int damage = estimatedAttackDamage(card, player, monster);
                    const int targetHp = intField(monster, "current_hp");
                    const int targetEffectiveHp = targetHp + intField(monster, "block");
                    if (damage > selectedDamage
                            || (damage == selectedDamage
                                    && targetEffectiveHp < selectedTargetEffectiveHp)) {
                        selectedDamage = damage;
                        selectedTargetHp = targetHp;
                        selectedTargetEffectiveHp = targetEffectiveHp;
                        targetTie = -targetEffectiveHp;
                    }
                }
                estimatedDamage = selectedDamage;
                score = (type == "ATTACK" ? 35.0 : 20.0)
                        + std::min(estimatedDamage, 30) * 2.0 - energyCost * 10.0;
                if (selectedTargetHp != std::numeric_limits<int>::max()
                        && estimatedDamage >= selectedTargetHp) {
                    score += 100.0;
                    reason = "estimated immediate lethal from public card damage and target HP";
                } else {
                    reason = "public card damage weighed against energy cost";
                }
                estimatedBlockValue = estimatedBlock(card);
                const int uncovered = std::max(0, incomingDamage - currentBlock);
                const int prevented = std::min(uncovered, estimatedBlockValue);
                score += prevented * 3.0;
                if (uncovered >= hp && prevented > 0) score += 45.0;
                if (prevented > 0) reason += "; visible incoming damage makes block valuable";
            } else if (type == "SKILL") {
                estimatedBlockValue = estimatedBlock(card);
                const int uncovered = std::max(0, incomingDamage - currentBlock);
                const int prevented = std::min(uncovered, estimatedBlockValue);
                score = 12.0 + prevented * 3.0 - energyCost * 10.0;
                if (uncovered >= hp && prevented > 0) score += 45.0;
                reason = prevented > 0
                        ? "spend energy on public defensive value against visible intent"
                        : "defer non-defensive setup when no visible damage requires it";
            } else if (type == "POWER") {
                score = safeSetupWindow && !gremlinNobAlive ? 32.0 : 8.0;
                score -= energyCost * 10.0;
                if (gremlinNobAlive) {
                    score -= 50.0;
                    reason = "defer Powers against publicly identified Gremlin Nob";
                } else {
                    reason = safeSetupWindow
                            ? "use setup only in a visible non-attacking window"
                            : "preserve energy for current-turn tactics";
                }
            } else {
                score = 5.0 - energyCost * 10.0;
                reason = "low-priority non-tactical card";
            }
            if (type == "POWER") {
                pb::dict scoreRow;
                scoreRow["action_index"] = index;
                scoreRow["public_action"] = action;
                scoreRow["priority_score"] = score;
                scoreRow["nob_identity_penalty_applied"] = gremlinNobAlive;
                scoreRow["branch"] = gremlinNobAlive
                        ? "identified_gremlin_nob"
                        : safeSetupWindow ? "visible_non_attacking_setup_window"
                                          : "visible_attacking_window";
                powerActionScores.append(scoreRow);
            }
            if (target >= 0 && target < monsters.size()) {
                const auto monster = pb::reinterpret_borrow<pb::dict>(monsters[target]);
                if (monster["alive"].cast<bool>() && monster["attacking"].cast<bool>()) {
                    targetTie = publicIncomingDamagePerHit(monster)
                            * intField(monster, "move_hits");
                }
            }
        } else if (kind == "end_turn") {
            score = 0;
            reason = "end turn when no tactical action has positive value";
        } else if (kind == "single_card_select" || kind == "multi_card_select") {
            score = 1;
            targetTie = -intField(action, "idx1");
            reason = "deterministic first legal choice for generic card selection";
        }
        if (score > bestScore || (score == bestScore && targetTie > bestTargetTie)) {
            bestScore = score;
            bestTargetTie = targetTie;
            bestIndex = index;
            bestReason = reason;
            bestEstimatedDamage = estimatedDamage;
            bestEstimatedBlock = estimatedBlockValue;
        }
    }
    if (bestIndex < 0) {
        throw std::runtime_error("public heuristic found no usable legal action");
    }
    if (audit != nullptr) {
        (*audit)["visible_incoming_damage"] = incomingDamage;
        (*audit)["current_block"] = currentBlock;
        (*audit)["priority_score"] = bestScore;
        (*audit)["estimated_card_damage"] = bestEstimatedDamage;
        (*audit)["estimated_card_block"] = bestEstimatedBlock;
        (*audit)["priority_reason"] = bestReason;
        (*audit)["gremlin_nob_alive_by_canonical_public_id"] = gremlinNobAlive;
        (*audit)["power_action_scores"] = powerActionScores;
        (*audit)["heuristic_id"] =
                "public-tactical-v1-blocked-lethal-canonical-monster-id";
    }
    return pb::reinterpret_borrow<pb::dict>(actions[bestIndex]);
}
