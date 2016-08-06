/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos-zero>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "SpellMgr.h"
#include "extras/Mod.h"
#include "ObjectMgr.h"
#include "SpellAuraDefines.h"
#include "ProgressBar.h"
#include "DBCStores.h"
#include "World.h"
#include "Chat.h"
#include "Spell.h"
#include "BattleGroundMgr.h"
#include "MapManager.h"
#include "Unit.h"

SpellMgr::SpellMgr()
{
}

SpellMgr::~SpellMgr()
{
}

SpellMgr& SpellMgr::Instance()
{
    static SpellMgr spellMgr;
    return spellMgr;
}

int32 GetSpellDuration(SpellEntry const *spellInfo)
{
    if(!spellInfo)
        return 0;
    SpellDurationEntry const *du = sSpellDurationStore.LookupEntry(spellInfo->DurationIndex);
    if(!du)
        return 0;
    return (du->Duration[0] == -1) ? -1 : abs(du->Duration[0]);
}

int32 GetSpellMaxDuration(SpellEntry const *spellInfo)
{
    if(!spellInfo)
        return 0;
    SpellDurationEntry const *du = sSpellDurationStore.LookupEntry(spellInfo->DurationIndex);
    if(!du)
        return 0;
    return (du->Duration[2] == -1) ? -1 : abs(du->Duration[2]);
}

int32 CalculateSpellDuration(SpellEntry const *spellInfo, Unit const* caster)
{
    int32 duration = GetSpellDuration(spellInfo);

    if (duration != -1 && caster)
    {
        int32 maxduration = GetSpellMaxDuration(spellInfo);

        if (duration != maxduration && caster->GetTypeId() == TYPEID_PLAYER)
            duration += int32((maxduration - duration) * ((Player*)caster)->GetComboPoints() / 5);

        if (Player* modOwner = caster->GetSpellModOwner())
        {
            modOwner->ApplySpellMod(spellInfo->Id, SPELLMOD_DURATION, duration);

            if (duration < 0)
                duration = 0;
        }
    }

    return duration;
}

uint32 GetSpellCastTime(SpellEntry const* spellInfo, Spell const* spell)
{
    if (spell)
    {
        // some triggered spells have data only usable for client
        if (spell->IsTriggeredSpellWithRedundentData())
            return 0;

        // spell targeted to non-trading trade slot item instant at trade success apply
        if (spell->GetCaster()->GetTypeId()==TYPEID_PLAYER)
            if (TradeData* my_trade = ((Player*)(spell->GetCaster()))->GetTradeData())
                if (Item* nonTrade = my_trade->GetTraderData()->GetItem(TRADE_SLOT_NONTRADED))
                    if (nonTrade == spell->m_targets.getItemTarget())
                        return 0;
    }

    SpellCastTimesEntry const *spellCastTimeEntry = sSpellCastTimesStore.LookupEntry(spellInfo->CastingTimeIndex);

    // not all spells have cast time index and this is all is pasiive abilities
    if (!spellCastTimeEntry)
        return 0;

    int32 castTime = spellCastTimeEntry->CastTime;

    if (spell)
    {
		if (castTime > 0) // Instant cast spells should not consume casting time reducing effects.
		{
			if (Player* modOwner = spell->GetCaster()->GetSpellModOwner())
				modOwner->ApplySpellMod(spellInfo->Id, SPELLMOD_CASTING_TIME, castTime, spell);
		}

        if (!(spellInfo->Attributes & (SPELL_ATTR_UNK4|SPELL_ATTR_TRADESPELL)))
            castTime = int32(castTime * spell->GetCaster()->GetFloatValue(UNIT_MOD_CAST_SPEED));
        else
        {
            if (spell->IsRangedSpell() && !spell->IsAutoRepeat())
                castTime = int32(castTime * spell->GetCaster()->m_modAttackSpeedPct[RANGED_ATTACK]);
        }
    }

    if (spellInfo->Attributes & SPELL_ATTR_RANGED && (!spell || !spell->IsAutoRepeat()))
        castTime += 500;

    sMod.getSpellCastTime(spellInfo,spell,castTime);

    return (castTime > 0) ? uint32(castTime) : 0;
}

uint32 GetSpellCastTimeForBonus( SpellEntry const *spellProto, DamageEffectType damagetype )
{
    uint32 CastingTime = !IsChanneledSpell(spellProto) ? GetSpellCastTime(spellProto) : GetSpellDuration(spellProto);

    if (CastingTime > 7000) CastingTime = 7000;
    if (CastingTime < 1500) CastingTime = 1500;

    if(damagetype == DOT && !IsChanneledSpell(spellProto))
        CastingTime = 3500;

    int32 overTime    = 0;
    uint8 effects     = 0;
    bool DirectDamage = false;
    bool AreaEffect   = false;

    for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
        if (IsAreaEffectTarget(Targets(spellProto->EffectImplicitTargetA[i])) || IsAreaEffectTarget(Targets(spellProto->EffectImplicitTargetB[i])))
            AreaEffect = true;

    for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        switch (spellProto->Effect[i])
        {
            case SPELL_EFFECT_SCHOOL_DAMAGE:
            case SPELL_EFFECT_POWER_DRAIN:
            case SPELL_EFFECT_HEALTH_LEECH:
            case SPELL_EFFECT_ENVIRONMENTAL_DAMAGE:
            case SPELL_EFFECT_POWER_BURN:
            case SPELL_EFFECT_HEAL:
                DirectDamage = true;
                break;
            case SPELL_EFFECT_APPLY_AURA:
                switch (spellProto->EffectApplyAuraName[i])
                {
                    case SPELL_AURA_PERIODIC_DAMAGE:
                    case SPELL_AURA_PERIODIC_HEAL:
                    case SPELL_AURA_PERIODIC_LEECH:
                        if ( GetSpellDuration(spellProto) )
                            overTime = GetSpellDuration(spellProto);
                        break;
                    // Penalty for additional effects
                    case SPELL_AURA_DUMMY:
                        ++effects;
                        break;
                    case SPELL_AURA_MOD_DECREASE_SPEED:
                        ++effects;
                        break;
                    case SPELL_AURA_MOD_CONFUSE:
                    case SPELL_AURA_MOD_STUN:
                    case SPELL_AURA_MOD_ROOT:
                        // -10% per effect
                        effects += 2;
                        break;
                    default:
                        break;
                }
                break;
            default:
                break;
        }
    }

    // Combined Spells with Both Over Time and Direct Damage
    if (overTime > 0 && CastingTime > 0 && DirectDamage)
    {
        // mainly for DoTs which are 3500 here otherwise
        uint32 OriginalCastTime = GetSpellCastTime(spellProto);
        if (OriginalCastTime > 7000) OriginalCastTime = 7000;
        if (OriginalCastTime < 1500) OriginalCastTime = 1500;
        // Portion to Over Time
        float PtOT = (overTime / 15000.0f) / ((overTime / 15000.0f) + (OriginalCastTime / 3500.0f));

        if (damagetype == DOT)
            CastingTime = uint32(CastingTime * PtOT);
        else if (PtOT < 1.0f)
            CastingTime  = uint32(CastingTime * (1 - PtOT));
        else
            CastingTime = 0;
    }

    // Area Effect Spells receive only half of bonus
    if (AreaEffect)
        CastingTime /= 2;

    // 50% for damage and healing spells for leech spells from damage bonus and 0% from healing
    for(int j = 0; j < MAX_EFFECT_INDEX; ++j)
    {
        if (spellProto->Effect[j] == SPELL_EFFECT_HEALTH_LEECH ||
            (spellProto->Effect[j] == SPELL_EFFECT_APPLY_AURA && spellProto->EffectApplyAuraName[j] == SPELL_AURA_PERIODIC_LEECH))
        {
            CastingTime /= 2;
            break;
        }
    }

    // -5% of total per any additional effect (multiplicative)
    for (int i = 0; i < effects; ++i)
        CastingTime *= 0.95f;

    return CastingTime;
}

uint16 GetSpellAuraMaxTicks(SpellEntry const* spellInfo)
{
    int32 DotDuration = GetSpellDuration(spellInfo);
    if(DotDuration == 0)
        return 1;

    // 200% limit
    if(DotDuration > 30000)
        DotDuration = 30000;

    for (int j = 0; j < MAX_EFFECT_INDEX; ++j)
    {
        if (spellInfo->Effect[j] == SPELL_EFFECT_APPLY_AURA && (
            spellInfo->EffectApplyAuraName[j] == SPELL_AURA_PERIODIC_DAMAGE ||
            spellInfo->EffectApplyAuraName[j] == SPELL_AURA_PERIODIC_HEAL ||
            spellInfo->EffectApplyAuraName[j] == SPELL_AURA_PERIODIC_LEECH) )
        {
            if (spellInfo->EffectAmplitude[j] != 0)
                return DotDuration / spellInfo->EffectAmplitude[j];
            break;
        }
    }

    return 6;
}

float CalculateDefaultCoefficient(SpellEntry const *spellProto, DamageEffectType const damagetype)
{
    // Damage over Time spells bonus calculation
    float DotFactor = 1.0f;
    if (damagetype == DOT)
    {
        if (!IsChanneledSpell(spellProto))
            DotFactor = GetSpellDuration(spellProto) / 15000.0f;

        if (uint16 DotTicks = GetSpellAuraMaxTicks(spellProto))
            DotFactor /= DotTicks;
    }

    // Distribute Damage over multiple effects, reduce by AoE
    float coeff = GetSpellCastTimeForBonus(spellProto, damagetype) / 3500.0f;

    return coeff * DotFactor;
}

WeaponAttackType GetWeaponAttackType(SpellEntry const *spellInfo)
{
    if(!spellInfo)
        return BASE_ATTACK;

    switch (spellInfo->DmgClass)
    {
        case SPELL_DAMAGE_CLASS_MELEE:
            if (spellInfo->AttributesEx3 & SPELL_ATTR_EX3_REQ_OFFHAND)
                return OFF_ATTACK;
            else
                return BASE_ATTACK;
            break;
        case SPELL_DAMAGE_CLASS_RANGED:
            return RANGED_ATTACK;
            break;
        default:
                                                            // Wands
            if (spellInfo->AttributesEx2 & SPELL_ATTR_EX2_AUTOREPEAT_FLAG)
                return RANGED_ATTACK;
            else
                return BASE_ATTACK;
            break;
    }
}

bool IsPassiveSpell(uint32 spellId)
{
    SpellEntry const *spellInfo = sSpellStore.LookupEntry(spellId);
    if (!spellInfo)
        return false;
    return IsPassiveSpell(spellInfo);
}

bool IsPassiveSpell(SpellEntry const *spellInfo)
{
    return (spellInfo->Attributes & SPELL_ATTR_PASSIVE) != 0;
}

bool IsNoStackAuraDueToAura(uint32 spellId_1, uint32 spellId_2)
{
    SpellEntry const *spellInfo_1 = sSpellStore.LookupEntry(spellId_1);
    SpellEntry const *spellInfo_2 = sSpellStore.LookupEntry(spellId_2);
    if(!spellInfo_1 || !spellInfo_2) return false;
    if(spellInfo_1->Id == spellId_2) return false;

	// Mighty Rage Potion and Elixir of Giants should stack.
	if ((spellInfo_1->Id == 11405 && spellInfo_2->Id == 17528) || (spellInfo_2->Id == 11405 && spellInfo_1->Id == 17528))
		return false;

	// Flask of the Titans and Elixir of (Minor) Fortitude should stack.
	if ((spellInfo_1->Id == 17626 && spellInfo_2->Id == 2378) || (spellInfo_2->Id == 17626 && spellInfo_1->Id == 2378) ||
        (spellInfo_1->Id == 3593 && spellInfo_2->Id == 17626) || (spellInfo_1->Id == 17626 && spellInfo_2->Id == 3593))
		return false;

    // Mongoose and agility elixir should stack with Grilled Squid.
    if((spellInfo_1->Id == 18192 && spellInfo_2->Id == 17538) || (spellInfo_2->Id == 18192 && spellInfo_1->Id == 17538) || 
        (spellInfo_2->Id == 11328 && spellInfo_1->Id == 18192) || (spellInfo_2->Id == 18192 && spellInfo_1->Id == 11328)  || 
        (spellInfo_2->Id == 11334 && spellInfo_1->Id == 18192) || (spellInfo_2->Id == 18192 && spellInfo_1->Id == 11334))
        return false;

    // Flask of Supreme Power should stack with (Greater) Arcane Elixir.
    if ((spellInfo_1->Id == 17628  && spellInfo_2->Id == 17539) ||
        (spellInfo_2->Id == 17628  && spellInfo_1->Id == 17539) ||
        (spellInfo_2->Id == 17628  && spellInfo_1->Id == 11390) ||
        (spellInfo_1->Id == 17628  && spellInfo_2->Id == 11390))
        return false;

    for (int32 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        for (int32 j = 0; j < MAX_EFFECT_INDEX; ++j)
        {
            if (spellInfo_1->Effect[i] == spellInfo_2->Effect[j]
                && spellInfo_1->EffectApplyAuraName[i] == spellInfo_2->EffectApplyAuraName[j]
                && spellInfo_1->EffectMiscValue[i] == spellInfo_2->EffectMiscValue[j]
                && spellInfo_1->EffectItemType[i] == spellInfo_2->EffectItemType[j]
                && (spellInfo_1->Effect[i] != 0 || spellInfo_1->EffectApplyAuraName[i] != 0 ||
                    spellInfo_1->EffectMiscValue[i] != 0 || spellInfo_1->EffectItemType[i] != 0))
                return true;
        }
    }

    return false;
}

int32 CompareAuraRanks(uint32 spellId_1, uint32 spellId_2)
{
    SpellEntry const*spellInfo_1 = sSpellStore.LookupEntry(spellId_1);
    SpellEntry const*spellInfo_2 = sSpellStore.LookupEntry(spellId_2);
    if(!spellInfo_1 || !spellInfo_2) return 0;
    if (spellId_1 == spellId_2) return 0;

    for (int32 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        if (spellInfo_1->Effect[i] != 0 && spellInfo_2->Effect[i] != 0 && spellInfo_1->Effect[i] == spellInfo_2->Effect[i])
        {
            int32 diff = spellInfo_1->EffectBasePoints[i] - spellInfo_2->EffectBasePoints[i];
            if (spellInfo_1->CalculateSimpleValue(SpellEffectIndex(i)) < 0 && spellInfo_2->CalculateSimpleValue(SpellEffectIndex(i)) < 0)
                return -diff;
            else return diff;
        }
    }
    return 0;
}

SpellSpecific GetSpellSpecific(uint32 spellId)
{
    SpellEntry const *spellInfo = sSpellStore.LookupEntry(spellId);
    if(!spellInfo)
        return SPELL_NORMAL;

    switch(spellInfo->SpellFamilyName)
    {
        case SPELLFAMILY_GENERIC:
        {
            // Aspect of the Beast
            if (spellInfo->Id == 13161)
                return SPELL_ASPECT;

            // Food / Drinks (mostly)
            if (spellInfo->AuraInterruptFlags & AURA_INTERRUPT_FLAG_NOT_SEATED)
            {
                bool food = false;
                bool drink = false;
                for(int i = 0; i < MAX_EFFECT_INDEX; ++i)
                {
                    switch(spellInfo->EffectApplyAuraName[i])
                    {
                        // Food
                        case SPELL_AURA_MOD_REGEN:
                        case SPELL_AURA_OBS_MOD_HEALTH:
                            food = true;
                            break;
                        // Drink
                        case SPELL_AURA_MOD_POWER_REGEN:
                        case SPELL_AURA_OBS_MOD_MANA:
                            drink = true;
                            break;
                        default:
                            break;
                    }
                }

                if (food && drink)
                    return SPELL_FOOD_AND_DRINK;
                else if (food)
                    return SPELL_FOOD;
                else if (drink)
                    return SPELL_DRINK;
            }
            else
            {
                // Well Fed buffs (must be exclusive with Food / Drink replenishment effects, or else Well Fed will cause them to be removed)
                // SpellIcon 2560 is Spell 46687, does not have this flag
                if (spellInfo->AttributesEx2 & SPELL_ATTR_EX2_FOOD_BUFF)
                    return SPELL_WELL_FED;
            }
            break;
        }
        case SPELLFAMILY_MAGE:
        {
            // family flags 18(Molten), 25(Frost/Ice), 28(Mage)
            if (spellInfo->SpellFamilyFlags & UI64LIT(0x12000000))
                return SPELL_MAGE_ARMOR;

            if (spellInfo->EffectApplyAuraName[EFFECT_INDEX_0]==SPELL_AURA_MOD_CONFUSE && spellInfo->PreventionType==SPELL_PREVENTION_TYPE_SILENCE)
                return SPELL_MAGE_POLYMORPH;

            break;
        }
        case SPELLFAMILY_WARRIOR:
        {
            if (spellInfo->SpellFamilyFlags & UI64LIT(0x00008000010000))
                return SPELL_POSITIVE_SHOUT;

            break;
        }
        case SPELLFAMILY_WARLOCK:
        {
            // only warlock curses have this
            if (spellInfo->Dispel == DISPEL_CURSE)
                return SPELL_CURSE;
            break;
        }
        case SPELLFAMILY_PRIEST:
        {
            // "Well Fed" buff from Blessed Sunfruit, Blessed Sunfruit Juice, Alterac Spring Water
            if ((spellInfo->Attributes & SPELL_ATTR_CASTABLE_WHILE_SITTING) &&
                (spellInfo->InterruptFlags & SPELL_INTERRUPT_FLAG_AUTOATTACK) &&
                (spellInfo->SpellIconID == 52 || spellInfo->SpellIconID == 79))
                return SPELL_WELL_FED;
            break;
        }
        case SPELLFAMILY_HUNTER:
        {
            // only hunter stings have this
            if (spellInfo->Dispel == DISPEL_POISON)
                return SPELL_STING;

            // only hunter aspects have this (one have generic family), if exclude Auto Shot
            if (spellInfo->activeIconID == 122 && spellInfo->Id != 75)
                return SPELL_ASPECT;

            break;
        }
        case SPELLFAMILY_PALADIN:
        {
            if (IsSealSpell(spellInfo))
                return SPELL_SEAL;

            if (spellInfo->IsFitToFamilyMask(UI64LIT(0x0000000010000100)))
                return SPELL_BLESSING;

            if ((spellInfo->IsFitToFamilyMask(UI64LIT(0x0000000020180400))) && spellInfo->baseLevel != 0)
                return SPELL_JUDGEMENT;

            for (int i = 0; i < 3; ++i)
            {
                // only paladin auras have this
                if (spellInfo->Effect[i] == SPELL_EFFECT_APPLY_AREA_AURA_PARTY)
                    return SPELL_AURA;
            }
            break;
        }
        case SPELLFAMILY_SHAMAN:
        {
            if (IsElementalShield(spellInfo))
                return SPELL_ELEMENTAL_SHIELD;

            break;
        }

        case SPELLFAMILY_POTION:
            return sSpellMgr.GetSpellElixirSpecific(spellInfo->Id);
    }

    // only warlock armor/skin have this (in additional to family cases)
    if( spellInfo->SpellVisual == 130 && spellInfo->SpellIconID == 89)
    {
        return SPELL_WARLOCK_ARMOR;
    }

    // Tracking spells (exclude Well Fed, some other always allowed cases)
    if ((IsSpellHaveAura(spellInfo, SPELL_AURA_TRACK_CREATURES) ||
        IsSpellHaveAura(spellInfo, SPELL_AURA_TRACK_RESOURCES)  ||
        IsSpellHaveAura(spellInfo, SPELL_AURA_TRACK_STEALTHED)) &&
        ((spellInfo->AttributesEx & SPELL_ATTR_EX_UNK17) || (spellInfo->Attributes & SPELL_ATTR_CASTABLE_WHILE_MOUNTED)))
        return SPELL_TRACKER;

    // elixirs can have different families, but potion most ofc.
    if (SpellSpecific sp = sSpellMgr.GetSpellElixirSpecific(spellInfo->Id))
        return sp;

    return SPELL_NORMAL;
}

// target not allow have more one spell specific from same caster
bool IsSingleFromSpellSpecificPerTargetPerCaster(SpellSpecific spellSpec1,SpellSpecific spellSpec2)
{
    switch(spellSpec1)
    {
        case SPELL_BLESSING:
        case SPELL_AURA:
        case SPELL_STING:
        case SPELL_CURSE:
        case SPELL_ASPECT:
        case SPELL_JUDGEMENT: 
            return spellSpec1==spellSpec2;
        default:
            return false;
    }
}

// target not allow have more one ranks from spell from spell specific per target
bool IsSingleFromSpellSpecificSpellRanksPerTarget(SpellSpecific spellSpec1,SpellSpecific spellSpec2)
{
    switch(spellSpec1)
    {
        case SPELL_BLESSING:
        case SPELL_AURA:
        case SPELL_CURSE:
        case SPELL_ASPECT:
            return spellSpec1==spellSpec2;
        default:
            return false;
    }
}

// target not allow have more one spell specific per target from any caster
bool IsSingleFromSpellSpecificPerTarget(SpellSpecific spellSpec1,SpellSpecific spellSpec2)
{
    switch(spellSpec1)
    {
        case SPELL_SEAL:
        case SPELL_TRACKER:
        case SPELL_WARLOCK_ARMOR:
        case SPELL_MAGE_ARMOR:
        case SPELL_ELEMENTAL_SHIELD:
        case SPELL_MAGE_POLYMORPH:
        case SPELL_WELL_FED:
            return spellSpec1==spellSpec2;
        case SPELL_BATTLE_ELIXIR:
            return spellSpec2==SPELL_BATTLE_ELIXIR
                || spellSpec2==SPELL_FLASK_ELIXIR;
        case SPELL_GUARDIAN_ELIXIR:
            return spellSpec2==SPELL_GUARDIAN_ELIXIR
                || spellSpec2==SPELL_FLASK_ELIXIR;
        case SPELL_FLASK_ELIXIR:
            return spellSpec2==SPELL_BATTLE_ELIXIR
                || spellSpec2==SPELL_GUARDIAN_ELIXIR
                || spellSpec2==SPELL_FLASK_ELIXIR;
        case SPELL_FOOD:
            return spellSpec2==SPELL_FOOD
                || spellSpec2==SPELL_FOOD_AND_DRINK;
        case SPELL_DRINK:
            return spellSpec2==SPELL_DRINK
                || spellSpec2==SPELL_FOOD_AND_DRINK;
        case SPELL_FOOD_AND_DRINK:
            return spellSpec2==SPELL_FOOD
                || spellSpec2==SPELL_DRINK
                || spellSpec2==SPELL_FOOD_AND_DRINK;
        default:
            return false;
    }
}

bool IsPositiveTarget(uint32 targetA, uint32 targetB)
{
    switch(targetA)
    {
        // non-positive targets
        case TARGET_CHAIN_DAMAGE:
        case TARGET_ALL_ENEMY_IN_AREA:
        case TARGET_ALL_ENEMY_IN_AREA_INSTANT:
        case TARGET_IN_FRONT_OF_CASTER:
        case TARGET_ALL_ENEMY_IN_AREA_CHANNELED:
        case TARGET_CURRENT_ENEMY_COORDINATES:
            return false;
        // positive or dependent
        case TARGET_CASTER_COORDINATES:
            return (targetB == TARGET_ALL_PARTY || targetB == TARGET_ALL_FRIENDLY_UNITS_AROUND_CASTER);
        default:
            break;
    }
    if (targetB)
        return IsPositiveTarget(targetB, 0);
    return true;
}

bool IsExplicitPositiveTarget(uint32 targetA)
{
    // positive targets that in target selection code expect target in m_targers, so not that auto-select target by spell data by m_caster and etc
    switch(targetA)
    {
        case TARGET_SINGLE_FRIEND:
        case TARGET_SINGLE_PARTY:
        case TARGET_CHAIN_HEAL:
        case TARGET_SINGLE_FRIEND_2:
        case TARGET_AREAEFFECT_PARTY_AND_CLASS:
            return true;
        default:
            break;
    }
    return false;
}

bool IsExplicitNegativeTarget(uint32 targetA)
{
    // non-positive targets that in target selection code expect target in m_targers, so not that auto-select target by spell data by m_caster and etc
    switch(targetA)
    {
        case TARGET_CHAIN_DAMAGE:
        case TARGET_CURRENT_ENEMY_COORDINATES:
            return true;
        default:
            break;
    }
    return false;
}

bool IsBinaryEffect(SpellEntry const *spellproto, SpellEffectIndex effIndex)
{
    // Exceptions
    switch (spellproto->Id)
    {
        case 25989: // Toxin at Viscidus.
            return false;
    }

	switch (spellproto->Effect[effIndex])
	{
	case SPELL_EFFECT_SCHOOL_DAMAGE:
	case SPELL_EFFECT_ENVIRONMENTAL_DAMAGE:
	case SPELL_EFFECT_HEAL:
	case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
	case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
	case SPELL_EFFECT_WEAPON_DAMAGE:
	case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
		return false;
	case SPELL_EFFECT_APPLY_AURA:
		{
			switch (spellproto->EffectApplyAuraName[effIndex])
			{
			case SPELL_AURA_PERIODIC_DAMAGE:
			case SPELL_AURA_PERIODIC_HEAL:
			case SPELL_AURA_PROC_TRIGGER_DAMAGE:
			case SPELL_AURA_PERIODIC_HEALTH_FUNNEL:
			case SPELL_AURA_PERIODIC_LEECH:
				return false;
			default:
				return true;
			}
		}
		break;
	default:
		return true;
	}
}

bool IsPositiveEffect(SpellEntry const *spellproto, SpellEffectIndex effIndex)
{
    switch (spellproto->Id)
    {
    case 5024: // Skull of Impending Doom "Flee" should be a positive effect
            return true;
    case 9632: // Bladestorm (Ravager procc) should be a positive effect
            return true;        
    case 12042: // Arcane power should be a positive effect.
        return true;
	case 12613: // Dark Iron Taskmaster Death should be a positive effect.
		return true;
	case 16713:	// Ghostly is not a positive effect.
		return false;
	case 17246: // Baroness Anastari's Possess is not a positive effect.
		return false;
    case 23603: // The Wild Polymorph spell is not a positive effect.
        return false;
    case 19451: // Magmadar's Frenzy should be a positive effect.
        return true;
	case 20545: // Lord Lakmaeran - Lightning Shield should be a positive effect. 
		return true;
    case 21153: // The Bonereaver's Edge effect should be positive.
        return true;
	case 23260: // Klinfran the Crazed's Entropic Sting is not a positive effect.
		return false;
	case 23275: // Solenor the Slayer's Dreadful Fright is not a positive effect.
		return false;
	case 23298: // Artorius the Doombringer's Demoinc Doom is not a positive effect.
		return false;
	case 23299: // Artorius the Doombringer's Stinging Trauma should be positive effect.
		return true;
	case 23205: // Simone the Seductress' Temptress' Kiss is not a positive effect.
		return false;
	case 23444:	// Ultrasafe Transporter - Gadgetzan, malfunction(poly) is not a positive effect.
		return false;
	case 25040:	// Mark of Nature is not a positive effect
		return false;
    }
            
    if ((spellproto->Attributes & SPELL_ATTR_HIDDEN) != 0)
    {
        //Hidden buffs are the glue that makes talents work, and many of them were being considered negative effects!
        //Some of them are in fact negative though!
        switch (spellproto->Id)
        {
        case 23183:			// Aura of Frost triggered Spell
        case 25042:			// Aura of Nature triggered Spell
            return false;
        default:
            break;
        }
        return true;
    }

    switch(spellproto->Effect[effIndex])
    {
    case SPELL_EFFECT_INSTAKILL:
        switch (spellproto->Id)
        {
        case 20479:			//Geddon's Armageddon
            return true;
        }
        break;
    case SPELL_EFFECT_DUMMY:
        // some explicitly required dummy effect sets
        switch(spellproto->Id)
        {
        case 28441:                                 // AB Effect 000
            return false;
        default:
            break;
        }
        break;
        // always positive effects (check before target checks that provided non-positive result in some case for positive effects)
        case SPELL_EFFECT_HEAL:
        case SPELL_EFFECT_LEARN_SPELL:
        case SPELL_EFFECT_SKILL_STEP:
		case SPELL_EFFECT_TRIGGER_SPELL:
            return true;

            // non-positive aura use
        case SPELL_EFFECT_APPLY_AURA:
        {
            switch(spellproto->EffectApplyAuraName[effIndex])
            {
                case SPELL_AURA_DUMMY:
                {
                    // dummy aura can be positive or negative dependent from casted spell
                    switch(spellproto->Id)
                    {
                        case 13139:                         // net-o-matic special effect
                        case 23445:                         // evil twin
                        case 35679:                         // Protectorate Demolitionist
                        case 38637:                         // Nether Exhaustion (red)
                        case 38638:                         // Nether Exhaustion (green)
                        case 38639:                         // Nether Exhaustion (blue)
                        case 44689:                         // Relay Race Accept Hidden Debuff - DND
						case 18172:							// Quest Kodo Roundup player debuff
						case 23182:							// Mark of Frost
						case 24776:							// Mark of Nature
						case 6767:							// Mark of Shame
                            return false;
                        // some spells have unclear target modes for selection, so just make effect positive
                        case 27184:
                        case 27190:
                        case 27191:
                        case 27201:
                        case 27202:
                        case 27203:
                            return true;
                        default:
                            break;
                    }
                }   break;
                case SPELL_AURA_MOD_DAMAGE_DONE:            // dependent from base point sign (negative -> negative)
                case SPELL_AURA_MOD_RESISTANCE:
                case SPELL_AURA_MOD_STAT:
                case SPELL_AURA_MOD_SKILL:
                case SPELL_AURA_MOD_DODGE_PERCENT:
                case SPELL_AURA_MOD_HEALING_PCT:
                case SPELL_AURA_MOD_HEALING_DONE:
					if (spellproto->Id == 20553)			// Golemagg's Trust
						return true;
                    else if(spellproto->CalculateSimpleValue(effIndex) < 0)
                        return false;
					else if (spellproto->Id == 19779)		//Sulfuron's Inspiration
						return true;
                    break;
				case SPELL_AURA_MOD_MELEE_HASTE:
					if (spellproto->Id ==20553)				// Golemagg's Trust
						return true;
					else if (spellproto->Id == 19779)		// Sulfuron's Inspiration
						return true;
					break;
                case SPELL_AURA_MOD_DAMAGE_TAKEN:           // dependent from bas point sign (positive -> negative)
				case SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN:
                    if (spellproto->CalculateSimpleValue(effIndex) < 0)
                        return true;
                    // let check by target modes (for Amplify Magic cases/etc)
                    break;
                case SPELL_AURA_MOD_SPELL_CRIT_CHANCE:
                case SPELL_AURA_MOD_INCREASE_HEALTH_PERCENT:
                case SPELL_AURA_MOD_DAMAGE_PERCENT_DONE:
                    if(spellproto->CalculateSimpleValue(effIndex) > 0)
                        return true;                        // some expected positive spells have SPELL_ATTR_EX_NEGATIVE or unclear target modes
                    break;
                case SPELL_AURA_ADD_TARGET_TRIGGER:
				case SPELL_AURA_REFLECT_SPELLS:
				case SPELL_AURA_DAMAGE_SHIELD:
                    return true;
                case SPELL_AURA_PERIODIC_TRIGGER_SPELL:
					if (spellproto->Id == 20478 ||		// Geddon's Armageddon
						spellproto->Id == 19636)		// Fire Blossom
					   return true;

                    if (spellproto->Id != spellproto->EffectTriggerSpell[effIndex])
                    {
                        uint32 spellTriggeredId = spellproto->EffectTriggerSpell[effIndex];
                        SpellEntry const *spellTriggeredProto = sSpellStore.LookupEntry(spellTriggeredId);

                        if (spellTriggeredProto)
                        {
                            // non-positive targets of main spell return early
                            for(int i = 0; i < MAX_EFFECT_INDEX; ++i)
                            {
                                // if non-positive trigger cast targeted to positive target this main cast is non-positive
                                // this will place this spell auras as debuffs
                                if (spellTriggeredProto->Effect[i] &&
                                    IsPositiveTarget(spellTriggeredProto->EffectImplicitTargetA[i], spellTriggeredProto->EffectImplicitTargetB[i]) &&
                                    !IsPositiveEffect(spellTriggeredProto, SpellEffectIndex(i)))
                                    return false;
                            }
                        }
                    }
                    break;
                case SPELL_AURA_PROC_TRIGGER_SPELL:
                    // many positive auras have negative triggered spells at damage for example and this not make it negative (it can be canceled for example)
                    break;
                case SPELL_AURA_MOD_STUN:                   //have positive and negative spells, we can't sort its correctly at this moment.
                    if (effIndex == EFFECT_INDEX_0 && spellproto->Effect[EFFECT_INDEX_1] == 0 && spellproto->Effect[EFFECT_INDEX_2] == 0)
                        return false;                       // but all single stun aura spells is negative

                    // Petrification
                    if(spellproto->Id == 17624)
                        return false;
                    break;
				case SPELL_AURA_MOD_PACIFY:
					if (spellproto->Id == 20478)		// Geddon's Armageddon
						return true;
					break;
                case SPELL_AURA_MOD_PACIFY_SILENCE:
                    if(spellproto->Id == 24740)             // Wisp Costume
                        return true;
					else if (spellproto->Id == 19695)		// Geddon's Inferno
						return true;
					
                    return false;
                case SPELL_AURA_MOD_SILENCE:
					if (spellproto->Id == 24732)		// Bat costume
						return true;
					return false;
                case SPELL_AURA_MOD_ROOT:
				   switch (spellproto->Id)
				   {
					   case 19695:							//Geddon's Inferno
					   case 20478:							//Geddon's Armageddon
					   case 19636:							//Fire Blossom
						   return true;
					   default:
						   return false;
				   }
				   return false;
                case SPELL_AURA_GHOST:
                case SPELL_AURA_PERIODIC_LEECH:
                case SPELL_AURA_MOD_STALKED:
                case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
                    return false;
                case SPELL_AURA_PERIODIC_DAMAGE:            // used in positive spells also.
                    // part of negative spell if casted at self (prevent cancel)
                    if(spellproto->EffectImplicitTargetA[effIndex] == TARGET_SELF)
                        return false;
                    break;
                case SPELL_AURA_MOD_DECREASE_SPEED:         // used in positive spells also
                    // part of positive spell if casted at self
                    if (spellproto->EffectImplicitTargetA[effIndex] == TARGET_SELF &&
                        spellproto->SpellFamilyName == SPELLFAMILY_GENERIC)
                        return false;
                    // but not this if this first effect (don't found better check)
                    if (spellproto->Attributes & 0x4000000 && effIndex == EFFECT_INDEX_0)
                        return false;
                    break;
                case SPELL_AURA_TRANSFORM:
                    // some spells negative
                    switch(spellproto->Id)
                    {
                        case 36897:                         // Transporter Malfunction (race mutation to horde)
                        case 36899:                         // Transporter Malfunction (race mutation to alliance)
                            return false;
                    }
                    break;
                case SPELL_AURA_MOD_SCALE:
                    // some spells negative
                    switch(spellproto->Id)
                    {
                        case 802:                           // Mutate Bug, wrongly negative by target modes
						case 20553:							// Golemagg's Trust
						case 19779:							// Sulfuron's Inspiration
                            return true;
                        case 36900:                         // Soul Split: Evil!
                        case 36901:                         // Soul Split: Good
                        case 36893:                         // Transporter Malfunction (decrease size case)
                        case 36895:                         // Transporter Malfunction (increase size case)
                            return false;
                    }
                    break;
                case SPELL_AURA_MECHANIC_IMMUNITY:
                {
                    // non-positive immunities
                    switch(spellproto->EffectMiscValue[effIndex])
                    {
                        case MECHANIC_BANDAGE:
                        case MECHANIC_SHIELD:
                        case MECHANIC_MOUNT:
                        case MECHANIC_INVULNERABILITY:
                            return false;
                        default:
                            break;
                    }
                }   break;
                case SPELL_AURA_ADD_FLAT_MODIFIER:          // mods
                case SPELL_AURA_ADD_PCT_MODIFIER:
                {
                    // non-positive mods
                    switch(spellproto->EffectMiscValue[effIndex])
                    {
                        case SPELLMOD_COST:                 // dependent from bas point sign (negative -> positive)
                            if(spellproto->CalculateSimpleValue(effIndex) > 0)
                                return false;
                            break;
                        default:
                            break;
                    }
                }   break;
                default:
                    break;
            }
            break;
        }
        default:
            break;
    }

    // non-positive targets
    if(!IsPositiveTarget(spellproto->EffectImplicitTargetA[effIndex],spellproto->EffectImplicitTargetB[effIndex]))
        return false;

    // AttributesEx check
    if(spellproto->AttributesEx & SPELL_ATTR_EX_NEGATIVE)
        return false;

    // ok, positive
    return true;
}

bool IsBinarySpell(uint32 spellId)
{
    SpellEntry const *spellproto = sSpellStore.LookupEntry(spellId);
    if (!spellproto)
        return false;

    return IsBinarySpell(spellproto);
}

bool IsPositiveSpell(uint32 spellId)
{
    SpellEntry const *spellproto = sSpellStore.LookupEntry(spellId);
    if (!spellproto)
        return false;

    return IsPositiveSpell(spellproto);
}

bool IsBinarySpell(SpellEntry const *spellproto)
{
	//Any spell with a negative binary effect is binary
	for (int i = 0; i < MAX_EFFECT_INDEX; i++)
	{
		if (spellproto->Effect[i] && !IsPositiveEffect(spellproto, SpellEffectIndex(i)) && IsBinaryEffect(spellproto,SpellEffectIndex(i)))
			return true;
	}

	return false;
}

bool IsPositiveSpell(SpellEntry const *spellproto)
{
    // spells with at least one negative effect are considered negative
    // some self-applied spells have negative effects but in self casting case negative check ignored.
    for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
        if (spellproto->Effect[i] && !IsPositiveEffect(spellproto, SpellEffectIndex(i)))
            return false;
    return true;
}

bool IsSingleTargetSpell(SpellEntry const *spellInfo)
{
    // hunter's mark and similar
    if(spellInfo->SpellVisual == 3239)
        return true;

    // exceptions (have spellInfo->AttributesEx & (1<<18) but not single targeted)
    switch(spellInfo->Id)
    {
        case 1833:                                          // Cheap Shot
        case 4538:                                          // Extract Essence (group targets)
        case 5106:                                          // Crystal Flash (group targets)
        case 5530:                                          // Mace Stun Effect
        case 5648:                                          // Stunning Blast, rank 1
        case 5649:                                          // Stunning Blast, rank 2
        case 5726:                                          // Stunning Blow, Rank 1
        case 5727:                                          // Stunning Blow, Rank 2
        case 6927:                                          // Shadowstalker Slash, Rank 1
        case 8399:                                          // Sleep (group targets)
        case 9159:                                          // Sleep (armor triggred affect)
        case 9256:                                          // Deep Sleep (group targets)
        case 13902:                                         // Fist of Ragnaros
        case 14902:                                         // Cheap Shot
        case 16104:                                         // Crystallize (group targets)
        case 17286:                                         // Crusader's Hammer (group targets)
        case 20277:                                         // Fist of Ragnaros (group targets)
        case 20669:                                         // Sleep (group targets)
        case 20683:                                         // Highlord's Justice
        case 24664:                                         // Sleep (group targets)
            return false;
    }
    // cannot be cast on another target while not cooled down anyway
    if (GetSpellDuration(spellInfo) < int32(GetSpellRecoveryTime(spellInfo)))
        return false;

    // all other single target spells have if it has AttributesEx
    if (spellInfo->AttributesEx & (1<<18))
        return true;

    // other single target
    //Fear
    if ((spellInfo->SpellIconID == 98 && spellInfo->SpellVisual == 336)
        //Banish
        || (spellInfo->SpellIconID == 96 && spellInfo->SpellVisual == 1305)
        ) return true;

    // TODO - need found Judgements rule
    switch(GetSpellSpecific(spellInfo->Id))
    {
        case SPELL_JUDGEMENT:
            return true;
        default:
            break;
    }

    return false;
}

bool IsSingleTargetSpells(SpellEntry const *spellInfo1, SpellEntry const *spellInfo2)
{
    // TODO - need better check
    // Equal icon and spellfamily
    if( spellInfo1->SpellFamilyName == spellInfo2->SpellFamilyName &&
        spellInfo1->SpellIconID == spellInfo2->SpellIconID )
        return true;

    // TODO - need found Judgements rule
    SpellSpecific spec1 = GetSpellSpecific(spellInfo1->Id);
    // spell with single target specific types
    switch(spec1)
    {
        case SPELL_JUDGEMENT:
        case SPELL_MAGE_POLYMORPH:
            if(GetSpellSpecific(spellInfo2->Id) == spec1)
                return true;
            break;
        default:
            break;
    }

    return false;
}

SpellCastResult GetErrorAtShapeshiftedCast (SpellEntry const *spellInfo, uint32 form)
{
    // talents that learn spells can have stance requirements that need ignore
    // (this requirement only for client-side stance show in talent description)
    if( GetTalentSpellCost(spellInfo->Id) > 0 &&
        (spellInfo->Effect[EFFECT_INDEX_0] == SPELL_EFFECT_LEARN_SPELL || spellInfo->Effect[EFFECT_INDEX_1] == SPELL_EFFECT_LEARN_SPELL || spellInfo->Effect[EFFECT_INDEX_2] == SPELL_EFFECT_LEARN_SPELL) )
        return SPELL_CAST_OK;

    uint32 stanceMask = (form ? 1 << (form - 1) : 0);

    if (stanceMask & spellInfo->StancesNot)                 // can explicitly not be casted in this stance
        return SPELL_FAILED_NOT_SHAPESHIFT;

    if (stanceMask & spellInfo->Stances)                    // can explicitly be casted in this stance
        return SPELL_CAST_OK;

    bool actAsShifted = false;
    if (form > 0)
    {
        SpellShapeshiftFormEntry const *shapeInfo = sSpellShapeshiftFormStore.LookupEntry(form);
        if (!shapeInfo)
        {
            sLog.outError("GetErrorAtShapeshiftedCast: unknown shapeshift %u", form);
            return SPELL_CAST_OK;
        }
        actAsShifted = !(shapeInfo->flags1 & 1);            // shapeshift acts as normal form for spells
    }

    if(actAsShifted)
    {
        if (spellInfo->Attributes & SPELL_ATTR_NOT_SHAPESHIFT) // not while shapeshifted
            return SPELL_FAILED_NOT_SHAPESHIFT;
        else if (spellInfo->Stances != 0)                   // needs other shapeshift
            return SPELL_FAILED_ONLY_SHAPESHIFT;
    }
    else
    {
        // needs shapeshift
        if(!(spellInfo->AttributesEx2 & SPELL_ATTR_EX2_NOT_NEED_SHAPESHIFT) && spellInfo->Stances != 0)
            return SPELL_FAILED_ONLY_SHAPESHIFT;
    }

    return SPELL_CAST_OK;
}

void SpellMgr::LoadSpellTargetPositions()
{
    mSpellTargetPositions.clear();                                // need for reload case

    uint32 count = 0;

    //                                                0   1           2                  3                  4                  5
    QueryResult *result = WorldDatabase.Query("SELECT id, target_map, target_position_x, target_position_y, target_position_z, target_orientation FROM spell_target_position");
    if (!result)
    {
        BarGoLink bar(1);

        bar.step();

        sLog.outString();
        sLog.outString(">> Loaded %u spell target destination coordinates", count);
        return;
    }

    BarGoLink bar(result->GetRowCount());

    do
    {
        Field *fields = result->Fetch();

        bar.step();

        uint32 Spell_ID = fields[0].GetUInt32();

        SpellTargetPosition st;

        st.target_mapId       = fields[1].GetUInt32();
        st.target_X           = fields[2].GetFloat();
        st.target_Y           = fields[3].GetFloat();
        st.target_Z           = fields[4].GetFloat();
        st.target_Orientation = fields[5].GetFloat();

        MapEntry const* mapEntry = sMapStore.LookupEntry(st.target_mapId);
        if (!mapEntry)
        {
            sLog.outErrorDb("Spell (ID:%u) target map (ID: %u) does not exist in `Map.dbc`.",Spell_ID,st.target_mapId);
            continue;
        }

        if (st.target_X==0 && st.target_Y==0 && st.target_Z==0)
        {
            sLog.outErrorDb("Spell (ID:%u) target coordinates not provided.",Spell_ID);
            continue;
        }

        SpellEntry const* spellInfo = sSpellStore.LookupEntry(Spell_ID);
        if (!spellInfo)
        {
            sLog.outErrorDb("Spell (ID:%u) listed in `spell_target_position` does not exist.",Spell_ID);
            continue;
        }

        bool found = false;
        for(int i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if (spellInfo->EffectImplicitTargetA[i]==TARGET_TABLE_X_Y_Z_COORDINATES || spellInfo->EffectImplicitTargetB[i]==TARGET_TABLE_X_Y_Z_COORDINATES)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            sLog.outErrorDb("Spell (Id: %u) listed in `spell_target_position` does not have target TARGET_TABLE_X_Y_Z_COORDINATES (17).",Spell_ID);
            continue;
        }

        mSpellTargetPositions[Spell_ID] = st;
        ++count;

    } while( result->NextRow() );

    delete result;

    sLog.outString();
    sLog.outString(">> Loaded %u spell target destination coordinates", count);
}

template <typename EntryType, typename WorkerType, typename StorageType>
struct SpellRankHelper
{
    SpellRankHelper(SpellMgr &_mgr, StorageType &_storage): mgr(_mgr), worker(_storage), customRank(0) {}
    void RecordRank(EntryType &entry, uint32 spell_id)
    {
        const SpellEntry *spell = sSpellStore.LookupEntry(spell_id);
        if (!spell)
        {
            sLog.outErrorDb("Spell %u listed in `%s` does not exist", spell_id, worker.TableName());
            return;
        }

        uint32 first_id = mgr.GetFirstSpellInChain(spell_id);

        // most spell ranks expected same data
        if(first_id)
        {
            firstRankSpells.insert(first_id);

            if (first_id != spell_id)
            {
                if (!worker.IsValidCustomRank(entry, spell_id, first_id))
                    return;
                // for later check that first rank also added
                else
                {
                    firstRankSpellsWithCustomRanks.insert(first_id);
                    ++customRank;
                }
            }
        }

        worker.AddEntry(entry, spell);
    }
    void FillHigherRanks()
    {
        // check that first rank added for custom ranks
        for (std::set<uint32>::const_iterator itr = firstRankSpellsWithCustomRanks.begin(); itr != firstRankSpellsWithCustomRanks.end(); ++itr)
            if (!worker.HasEntry(*itr))
                sLog.outErrorDb("Spell %u must be listed in `%s` as first rank for listed custom ranks of spell but not found!", *itr, worker.TableName());

        // fill absent non first ranks data base at first rank data
        for (std::set<uint32>::const_iterator itr = firstRankSpells.begin(); itr != firstRankSpells.end(); ++itr)
        {
            if (worker.SetStateToEntry(*itr))
                mgr.doForHighRanks(*itr, worker);
        }
    }
    std::set<uint32> firstRankSpells;
    std::set<uint32> firstRankSpellsWithCustomRanks;

    SpellMgr &mgr;
    WorkerType worker;
    uint32 customRank;
};

struct DoSpellProcEvent
{
    DoSpellProcEvent(SpellProcEventMap& _spe_map) : spe_map(_spe_map), customProc(0), count(0) {}
    void operator() (uint32 spell_id)
    {
        SpellProcEventEntry const& spe = state->second;
        // add ranks only for not filled data (some ranks have ppm data different for ranks for example)
        SpellProcEventMap::const_iterator spellItr = spe_map.find(spell_id);
        if (spellItr == spe_map.end())
            spe_map[spell_id] = spe;
        // if custom rank data added then it must be same except ppm
        else
        {
            SpellProcEventEntry const& r_spe = spellItr->second;
            if (spe.schoolMask != r_spe.schoolMask)
                sLog.outErrorDb("Spell %u listed in `spell_proc_event` as custom rank have different schoolMask from first rank in chain", spell_id);

            if (spe.spellFamilyName != r_spe.spellFamilyName)
                sLog.outErrorDb("Spell %u listed in `spell_proc_event` as custom rank have different spellFamilyName from first rank in chain", spell_id);

            for (int32 i = 0; i < MAX_EFFECT_INDEX; ++i)
            {
                if (spe.spellFamilyMask[i] != r_spe.spellFamilyMask[i])
                {
                    sLog.outErrorDb("Spell %u listed in `spell_proc_event` as custom rank have different spellFamilyMask from first rank in chain", spell_id);
                    break;
                }
            }

            if (spe.procFlags != r_spe.procFlags)
                sLog.outErrorDb("Spell %u listed in `spell_proc_event` as custom rank have different procFlags from first rank in chain", spell_id);

            if (spe.procEx != r_spe.procEx)
                sLog.outErrorDb("Spell %u listed in `spell_proc_event` as custom rank have different procEx from first rank in chain", spell_id);

            // only ppm allowed has been different from first rank

            if (spe.customChance != r_spe.customChance)
                sLog.outErrorDb("Spell %u listed in `spell_proc_event` as custom rank have different customChance from first rank in chain", spell_id);

            if (spe.cooldown != r_spe.cooldown)
                sLog.outErrorDb("Spell %u listed in `spell_proc_event` as custom rank have different cooldown from first rank in chain", spell_id);
        }
    }

    const char* TableName() { return "spell_proc_event"; }
    bool IsValidCustomRank(SpellProcEventEntry const &spe, uint32 entry, uint32 first_id)
    {
        // let have independent data in table for spells with ppm rates (exist rank dependent ppm rate spells)
        if (!spe.ppmRate)
        {
            sLog.outErrorDb("Spell %u listed in `spell_proc_event` is not first rank (%u) in chain", entry, first_id);
            // prevent loading since it won't have an effect anyway
            return false;
        }
        return true;
    }
    void AddEntry(SpellProcEventEntry const &spe, SpellEntry const *spell)
    {
        spe_map[spell->Id] = spe;

        bool isCustom = false;

        if (spe.procFlags == 0)
        {
            if (spell->procFlags==0)
                sLog.outErrorDb("Spell %u listed in `spell_proc_event` probally not triggered spell (no proc flags)", spell->Id);
        }
        else
        {
            if (spell->procFlags==spe.procFlags)
                sLog.outErrorDb("Spell %u listed in `spell_proc_event` has exactly same proc flags as in spell.dbc, field value redundant", spell->Id);
            else
                isCustom = true;
        }

        if (spe.customChance == 0)
        {
            /* enable for re-check cases, 0 chance ok for some cases because in some cases it set by another spell/talent spellmod)
            if (spell->procChance==0 && !spe.ppmRate)
                sLog.outErrorDb("Spell %u listed in `spell_proc_event` probally not triggered spell (no chance or ppm)", spell->Id);
            */
        }
        else
        {
            if (spell->procChance==spe.customChance)
                sLog.outErrorDb("Spell %u listed in `spell_proc_event` has exactly same custom chance as in spell.dbc, field value redundant", spell->Id);
            else
                isCustom = true;
        }

        // totally redundant record
        if (!spe.schoolMask && !spe.procFlags &&
            !spe.procEx && !spe.ppmRate && !spe.customChance && !spe.cooldown)
        {
            bool empty = !spe.spellFamilyName ? true : false;
            for (int32 i = 0; i < MAX_EFFECT_INDEX; ++i)
            {
                if (spe.spellFamilyMask[i])
                {
                    empty = false;
                    break;
                }
            }
            if (empty)
                sLog.outErrorDb("Spell %u listed in `spell_proc_event` not have any useful data", spell->Id);
        }

        if (isCustom)
            ++customProc;
        else
            ++count;
    }

    bool HasEntry(uint32 spellId) { return spe_map.count(spellId) > 0; }
    bool SetStateToEntry(uint32 spellId) { return (state = spe_map.find(spellId)) != spe_map.end(); }
    SpellProcEventMap& spe_map;
    SpellProcEventMap::const_iterator state;

    uint32 customProc;
    uint32 count;
};

void SpellMgr::LoadSpellProcEvents()
{
    mSpellProcEventMap.clear();                             // need for reload case

    //                                                0      1           2                3                 4                 5                 6          7       8        9             10
    QueryResult *result = WorldDatabase.Query("SELECT entry, SchoolMask, SpellFamilyName, SpellFamilyMask0, SpellFamilyMask1, SpellFamilyMask2, procFlags, procEx, ppmRate, CustomChance, Cooldown FROM spell_proc_event");
    if (!result)
    {
        BarGoLink bar(1);
        bar.step();
        sLog.outString();
        sLog.outString(">> No spell proc event conditions loaded");
        return;
    }

    SpellRankHelper<SpellProcEventEntry, DoSpellProcEvent, SpellProcEventMap> rankHelper(*this, mSpellProcEventMap);

    BarGoLink bar(result->GetRowCount());
    do
    {
        Field *fields = result->Fetch();

        bar.step();

        uint32 entry = fields[0].GetUInt32();

        SpellProcEventEntry spe;

        spe.schoolMask      = fields[1].GetUInt32();
        spe.spellFamilyName = fields[2].GetUInt32();

        for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
            spe.spellFamilyMask[i] = ClassFamilyMask(fields[3+i].GetUInt64());

        spe.procFlags       = fields[6].GetUInt32();
        spe.procEx          = fields[7].GetUInt32();
        spe.ppmRate         = fields[8].GetFloat();
        spe.customChance    = fields[9].GetFloat();
        spe.cooldown        = fields[10].GetUInt32();

        rankHelper.RecordRank(spe, entry);

    } while (result->NextRow());

    rankHelper.FillHigherRanks();

    delete result;

    sLog.outString();
    sLog.outString( ">> Loaded %u extra spell proc event conditions +%u custom proc (inc. +%u custom ranks)",  rankHelper.worker.count, rankHelper.worker.customProc, rankHelper.customRank);
}

struct DoSpellProcItemEnchant
{
    DoSpellProcItemEnchant(SpellProcItemEnchantMap& _procMap, float _ppm) : procMap(_procMap), ppm(_ppm) {}
    void operator() (uint32 spell_id) { procMap[spell_id] = ppm; }

    SpellProcItemEnchantMap& procMap;
    float ppm;
};

void SpellMgr::LoadSpellProcItemEnchant()
{
    mSpellProcItemEnchantMap.clear();                       // need for reload case

    uint32 count = 0;

    //                                                0      1
    QueryResult *result = WorldDatabase.Query("SELECT entry, ppmRate FROM spell_proc_item_enchant");
    if (!result)
    {

        BarGoLink bar(1);

        bar.step();

        sLog.outString();
        sLog.outString(">> Loaded %u proc item enchant definitions", count);
        return;
    }

    BarGoLink bar(result->GetRowCount());

    do
    {
        Field *fields = result->Fetch();

        bar.step();

        uint32 entry = fields[0].GetUInt32();
        float ppmRate = fields[1].GetFloat();

        SpellEntry const* spellInfo = sSpellStore.LookupEntry(entry);

        if (!spellInfo)
        {
            sLog.outErrorDb("Spell %u listed in `spell_proc_item_enchant` does not exist", entry);
            continue;
        }

        uint32 first_id = GetFirstSpellInChain(entry);

        if ( first_id != entry )
        {
            sLog.outErrorDb("Spell %u listed in `spell_proc_item_enchant` is not first rank (%u) in chain", entry, first_id);
            // prevent loading since it won't have an effect anyway
            continue;
        }

        mSpellProcItemEnchantMap[entry] = ppmRate;

        // also add to high ranks
        DoSpellProcItemEnchant worker(mSpellProcItemEnchantMap, ppmRate);
        doForHighRanks(entry,worker);

        ++count;
    } while( result->NextRow() );

    delete result;

    sLog.outString();
    sLog.outString( ">> Loaded %u proc item enchant definitions", count );
}

struct DoSpellBonuses
{
    DoSpellBonuses(SpellBonusMap& _spellBonusMap, SpellBonusEntry const& _spellBonus) : spellBonusMap(_spellBonusMap), spellBonus(_spellBonus) {}
    void operator() (uint32 spell_id) { spellBonusMap[spell_id] = spellBonus; }

    SpellBonusMap& spellBonusMap;
    SpellBonusEntry const& spellBonus;
};

void SpellMgr::LoadSpellBonuses()
{
    mSpellBonusMap.clear();                             // need for reload case
    uint32 count = 0;
    //                                                0      1             2          3
    QueryResult *result = WorldDatabase.Query("SELECT entry, direct_bonus, dot_bonus, ap_bonus, ap_dot_bonus FROM spell_bonus_data");
    if (!result)
    {
        BarGoLink bar(1);
        bar.step();
        sLog.outString();
        sLog.outString(">> Loaded %u spell bonus data", count);
        return;
    }

    BarGoLink bar(result->GetRowCount());
    do
    {
        Field *fields = result->Fetch();
        bar.step();
        uint32 entry = fields[0].GetUInt32();

        SpellEntry const* spell = sSpellStore.LookupEntry(entry);
        if (!spell)
        {
            sLog.outErrorDb("Spell %u listed in `spell_bonus_data` does not exist", entry);
            continue;
        }

        uint32 first_id = GetFirstSpellInChain(entry);

        if ( first_id != entry )
        {
            sLog.outErrorDb("Spell %u listed in `spell_bonus_data` is not first rank (%u) in chain", entry, first_id);
            // prevent loading since it won't have an effect anyway
            continue;
        }

        SpellBonusEntry sbe;

        sbe.direct_damage = fields[1].GetFloat();
        sbe.dot_damage    = fields[2].GetFloat();
        sbe.ap_bonus      = fields[3].GetFloat();
        sbe.ap_dot_bonus   = fields[4].GetFloat();

        bool need_dot = false;
        bool need_direct = false;
        uint32 x = 0;                                       // count all, including empty, meaning: not all existing effect is DoTs/HoTs
        for(int i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if (!spell->Effect[i])
            {
                ++x;
                continue;
            }

            // DoTs/HoTs
            switch(spell->EffectApplyAuraName[i])
            {
                case SPELL_AURA_PERIODIC_DAMAGE:
                case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
                case SPELL_AURA_PERIODIC_LEECH:
                case SPELL_AURA_PERIODIC_HEAL:
                case SPELL_AURA_OBS_MOD_HEALTH:
                case SPELL_AURA_PERIODIC_MANA_LEECH:
                case SPELL_AURA_OBS_MOD_MANA:
                case SPELL_AURA_POWER_BURN_MANA:
                    need_dot = true;
                    ++x;
                    break;
                default:
                    break;
            }
        }

        //TODO: maybe add explicit list possible direct damage spell effects...
        if (x < MAX_EFFECT_INDEX)
            need_direct = true;

        // Check if direct_bonus is needed in `spell_bonus_data`
        float direct_calc;
        float direct_diff = 1000.0f;                        // for have big diff if no DB field value
        if (sbe.direct_damage)
        {
            direct_calc = CalculateDefaultCoefficient(spell, SPELL_DIRECT_DAMAGE);
            direct_diff = std::abs(sbe.direct_damage - direct_calc);
        }

        // Check if dot_bonus is needed in `spell_bonus_data`
        float dot_calc;
        float dot_diff = 1000.0f;                           // for have big diff if no DB field value
        if (sbe.dot_damage)
        {
            dot_calc = CalculateDefaultCoefficient(spell, DOT);
            dot_diff = std::abs(sbe.dot_damage - dot_calc);
        }

        if (direct_diff < 0.02f && !need_dot && !sbe.ap_bonus && !sbe.ap_dot_bonus)
            sLog.outErrorDb("`spell_bonus_data` entry for spell %u `direct_bonus` not needed (data from table: %f, calculated %f, difference of %f) and `dot_bonus` also not used",
                entry, sbe.direct_damage, direct_calc, direct_diff);
        else if (direct_diff < 0.02f && dot_diff < 0.02f && !sbe.ap_bonus && !sbe.ap_dot_bonus)
        {
            sLog.outErrorDb("`spell_bonus_data` entry for spell %u `direct_bonus` not needed (data from table: %f, calculated %f, difference of %f) and ",
                entry, sbe.direct_damage, direct_calc, direct_diff);
            sLog.outErrorDb("                                  ... `dot_bonus` not needed (data from table: %f, calculated %f, difference of %f)",
                sbe.dot_damage, dot_calc, dot_diff);
        }
        else if (!need_direct && dot_diff < 0.02f && !sbe.ap_bonus && !sbe.ap_dot_bonus)
            sLog.outErrorDb("`spell_bonus_data` entry for spell %u `dot_bonus` not needed (data from table: %f, calculated %f, difference of %f) and direct also not used",
            entry, sbe.dot_damage, dot_calc, dot_diff);
        else if (!need_direct && sbe.direct_damage)
            sLog.outErrorDb("`spell_bonus_data` entry for spell %u `direct_bonus` not used (spell not have non-periodic affects)", entry);
        else if (!need_dot && sbe.dot_damage)
            sLog.outErrorDb("`spell_bonus_data` entry for spell %u `dot_bonus` not used (spell not have periodic affects)", entry);

        if (!need_direct && sbe.ap_bonus)
            sLog.outErrorDb("`spell_bonus_data` entry for spell %u `ap_bonus` not used (spell not have non-periodic affects)", entry);
        else if (!need_dot && sbe.ap_dot_bonus)
            sLog.outErrorDb("`spell_bonus_data` entry for spell %u `ap_dot_bonus` not used (spell not have periodic affects)", entry);

        mSpellBonusMap[entry] = sbe;

        // also add to high ranks
        DoSpellBonuses worker(mSpellBonusMap, sbe);
        doForHighRanks(entry, worker);

        ++count;

    } while( result->NextRow() );

    delete result;

    sLog.outString();
    sLog.outString( ">> Loaded %u extra spell bonus data",  count);
}

bool SpellMgr::IsSpellProcEventCanTriggeredBy(SpellProcEventEntry const * spellProcEvent, uint32 EventProcFlag, SpellEntry const * procSpell, uint32 procFlags, uint32 procExtra)
{
    // No extra req need
    uint32 procEvent_procEx = PROC_EX_NONE;

	//Events that ask that you kill, be killed, or which are tied into the trap system... ONLY trigger in those scenarios!
	uint32 demandingFlags = (PROC_FLAG_KILLED | PROC_FLAG_KILL | PROC_FLAG_ON_TRAP_ACTIVATION);
	if ( (EventProcFlag & demandingFlags) != (procFlags & demandingFlags))
		return false;

	bool hadDemandingFlags = ( (EventProcFlag & demandingFlags) != 0);
	//They should have something else in common, even if they have the same demanding flags
	EventProcFlag &= ~demandingFlags;
	procFlags &= ~demandingFlags;

	if (EventProcFlag == 0 && hadDemandingFlags)
		return true; //It's all we cared about anyway

    // check prockFlags for condition
    if((procFlags & EventProcFlag) == 0)
        return false;

	//If we got this far and had demanding flags then we're pretty much good
	if (hadDemandingFlags)
		return true;

    if (spellProcEvent)     // Exist event data
    {
        // Store extra req
        procEvent_procEx = spellProcEvent->procEx;

        // For melee triggers
        if (procSpell == NULL)
        {
            // Check (if set) for school (melee attack have Normal school)
            if(spellProcEvent->schoolMask && (spellProcEvent->schoolMask & SPELL_SCHOOL_MASK_NORMAL) == 0)
                return false;
        }
        else // For spells need check school/spell family/family mask
        {
            // Check (if set) for school
            if(spellProcEvent->schoolMask && (spellProcEvent->schoolMask & GetSchoolMask(procSpell->School)) == 0)
                return false;

            // Check (if set) for spellFamilyName
            if(spellProcEvent->spellFamilyName && (spellProcEvent->spellFamilyName != procSpell->SpellFamilyName))
                return false;
        }
    }

    // Check for extra req (if none) and hit/crit
    if (procEvent_procEx == PROC_EX_NONE)
    {
        // Don't allow proc from periodic heal if no extra requirement is defined
        if (EventProcFlag & (PROC_FLAG_ON_DO_PERIODIC | PROC_FLAG_ON_TAKE_PERIODIC) && (procExtra & PROC_EX_PERIODIC_POSITIVE))
            return false;

        // No extra req, so can trigger for (damage/healing present) and hit/crit
        if(procExtra & (PROC_EX_NORMAL_HIT|PROC_EX_CRITICAL_HIT))
            return true;
    }
    else // all spells hits here only if resist/reflect/immune/evade
    {
        // Exist req for PROC_EX_EX_TRIGGER_ALWAYS
        if (procEvent_procEx & PROC_EX_EX_TRIGGER_ALWAYS)
            return true;
        // Check Extra Requirement like (hit/crit/miss/resist/parry/dodge/block/immune/reflect/absorb and other)
        if (procEvent_procEx & procExtra)
            return true;
    }
    return false;
}

void SpellMgr::LoadSpellElixirs()
{
    mSpellElixirs.clear();                                  // need for reload case

    uint32 count = 0;

    //                                                0      1
    QueryResult *result = WorldDatabase.Query("SELECT entry, mask FROM spell_elixir");
    if (!result)
    {

        BarGoLink bar(1);

        bar.step();

        sLog.outString();
        sLog.outString(">> Loaded %u spell elixir definitions", count);
        return;
    }

    BarGoLink bar(result->GetRowCount());

    do
    {
        Field *fields = result->Fetch();

        bar.step();

        uint32 entry = fields[0].GetUInt32();
        uint8 mask = fields[1].GetUInt8();

        SpellEntry const* spellInfo = sSpellStore.LookupEntry(entry);

        if (!spellInfo)
        {
            sLog.outErrorDb("Spell %u listed in `spell_elixir` does not exist", entry);
            continue;
        }

        mSpellElixirs[entry] = mask;

        ++count;
    } while( result->NextRow() );

    delete result;

    sLog.outString();
    sLog.outString( ">> Loaded %u spell elixir definitions", count );
}

struct DoSpellThreat
{
    DoSpellThreat(SpellThreatMap& _threatMap) : threatMap(_threatMap), count(0) {}
    void operator() (uint32 spell_id)
    {
        SpellThreatEntry const &ste = state->second;
        // add ranks only for not filled data (spells adding flat threat are usually different for ranks)
        SpellThreatMap::const_iterator spellItr = threatMap.find(spell_id);
        if (spellItr == threatMap.end())
            threatMap[spell_id] = ste;

        // just assert that entry is not redundant
        else
        {
            SpellThreatEntry const& r_ste = spellItr->second;
            if (ste.threat == r_ste.threat && ste.multiplier == r_ste.multiplier && ste.ap_bonus == r_ste.ap_bonus)
                sLog.outErrorDb("Spell %u listed in `spell_threat` as custom rank has same data as Rank 1, so redundant", spell_id);
        }
    }
    const char* TableName() { return "spell_threat"; }
    bool IsValidCustomRank(SpellThreatEntry const &ste, uint32 entry, uint32 first_id)
    {
        if (!ste.threat)
        {
            sLog.outErrorDb("Spell %u listed in `spell_threat` is not first rank (%u) in chain and has no threat", entry, first_id);
            // prevent loading unexpected data
            return false;
        }
        return true;
    }
    void AddEntry(SpellThreatEntry const &ste, SpellEntry const *spell)
    {
        threatMap[spell->Id] = ste;

        // flat threat bonus and attack power bonus currently only work properly when all
        // effects have same targets, otherwise, we'd need to seperate it by effect index
        if (ste.threat || ste.ap_bonus != 0.f)
        {
            const uint32 *targetA = spell->EffectImplicitTargetA;
            //const uint32 *targetB = spell->EffectImplicitTargetB;
            if ((targetA[EFFECT_INDEX_1] && targetA[EFFECT_INDEX_1] != targetA[EFFECT_INDEX_0]) ||
                (targetA[EFFECT_INDEX_2] && targetA[EFFECT_INDEX_2] != targetA[EFFECT_INDEX_0]))
                sLog.outErrorDb("Spell %u listed in `spell_threat` has effects with different targets, threat may be assigned incorrectly", spell->Id);
        }
        ++count;
    }
    bool HasEntry(uint32 spellId) { return threatMap.count(spellId) > 0; }
    bool SetStateToEntry(uint32 spellId) { return (state = threatMap.find(spellId)) != threatMap.end(); }

    SpellThreatMap& threatMap;
    SpellThreatMap::const_iterator state;
    uint32 count;
};

void SpellMgr::LoadSpellThreats()
{
    mSpellThreatMap.clear();                                // need for reload case

    //                                                0      1       2           3
    QueryResult *result = WorldDatabase.Query("SELECT entry, Threat, multiplier, ap_bonus FROM spell_threat");
    if (!result)
    {
        BarGoLink bar(1);
        bar.step();
        sLog.outString();
        sLog.outString(">> No spell threat entries loaded.");
        return;
    }

    SpellRankHelper<SpellThreatEntry, DoSpellThreat, SpellThreatMap> rankHelper(*this, mSpellThreatMap);

    BarGoLink bar(result->GetRowCount());

    do
    {
        Field *fields = result->Fetch();

        bar.step();

        uint32 entry = fields[0].GetUInt32();

        SpellThreatEntry ste;
        ste.threat = fields[1].GetUInt16();
        ste.multiplier = fields[2].GetFloat();
        ste.ap_bonus = fields[3].GetFloat();

        rankHelper.RecordRank(ste, entry);

    } while( result->NextRow() );

    rankHelper.FillHigherRanks();

    delete result;

    sLog.outString();
    sLog.outString( ">> Loaded %u spell threat entries", rankHelper.worker.count );
}

bool SpellMgr::IsRankSpellDueToSpell(SpellEntry const *spellInfo_1,uint32 spellId_2) const
{
    SpellEntry const *spellInfo_2 = sSpellStore.LookupEntry(spellId_2);
    if(!spellInfo_1 || !spellInfo_2) return false;
    if(spellInfo_1->Id == spellId_2) return false;

    return GetFirstSpellInChain(spellInfo_1->Id)==GetFirstSpellInChain(spellId_2);
}

bool SpellMgr::canStackSpellRanksInSpellBook(SpellEntry const *spellInfo) const
{
    if (IsPassiveSpell(spellInfo))                          // ranked passive spell
        return false;

	// Exception for the Faeire Fire (Feral) spell.
	if (spellInfo->Id == 16857 || spellInfo->Id == 17390 || 
		spellInfo->Id == 17391 || spellInfo->Id == 17392)
		return true;

    // Paladin Seal of Righteousness double r1, only show the newest
    if (spellInfo->Id == 20154 || spellInfo->Id == 21084)
        return false;
    
    if (spellInfo->powerType != POWER_MANA && spellInfo->powerType != POWER_HEALTH)
        return false;
    if (IsProfessionOrRidingSpell(spellInfo->Id))
        return false;

    if (IsSkillBonusSpell(spellInfo->Id))
        return false;

    // All stance spells. if any better way, change it.
    for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        switch(spellInfo->SpellFamilyName)
        {
            case SPELLFAMILY_PALADIN:
                // Paladin aura Spell
                if (spellInfo->Effect[i]==SPELL_EFFECT_APPLY_AREA_AURA_PARTY)
                    return false;
                break;
            case SPELLFAMILY_DRUID:
                // Druid form Spell
                if (spellInfo->Effect[i]==SPELL_EFFECT_APPLY_AURA &&
                    spellInfo->EffectApplyAuraName[i] == SPELL_AURA_MOD_SHAPESHIFT)
                    return false;
                break;
            case SPELLFAMILY_ROGUE:
                // Rogue Stealth
                if (spellInfo->Effect[i]==SPELL_EFFECT_APPLY_AURA &&
                    spellInfo->EffectApplyAuraName[i] == SPELL_AURA_MOD_STEALTH)
                    return false;
                break;
        }
    }
    return true;
}

bool SpellMgr::IsNoStackSpellDueToSpell(uint32 spellId_1, uint32 spellId_2) const
{
    SpellEntry const *spellInfo_1 = sSpellStore.LookupEntry(spellId_1);
    SpellEntry const *spellInfo_2 = sSpellStore.LookupEntry(spellId_2);

    if (!spellInfo_1 || !spellInfo_2)
        return false;

    if (spellId_1 == spellId_2)
        return false;

    // Resurrection sickness
    if ((spellInfo_1->Id == SPELL_ID_PASSIVE_RESURRECTION_SICKNESS) != (spellInfo_2->Id==SPELL_ID_PASSIVE_RESURRECTION_SICKNESS))
        return false;

    // Allow stack passive and not passive spells
    if ((spellInfo_1->Attributes & SPELL_ATTR_PASSIVE)!=(spellInfo_2->Attributes & SPELL_ATTR_PASSIVE))
        return false;

	// Don't allow halloween costumes to stack
	// 24712 - Leper Gnome Male
	// 24713 - Leper Gnome Female
	// 24732 - Bat
	// 24735 - Ghost Male
	// 24736 - Ghost Female
	// 24710 - Ninja Male
	// 24711 - Ninja Female
	// 24708 - Pirate Male
	// 24709 - Pirate Female
	// 24723 - Skeleton
	// 24740 - Wisp
	if ( (spellInfo_1->Id == 24712 || spellInfo_1->Id == 24713 || spellInfo_1->Id == 24732 || spellInfo_1->Id == 24735 ||
			spellInfo_1->Id == 24736 || spellInfo_1->Id == 24710 || spellInfo_1->Id == 24711 || spellInfo_1->Id == 24708 ||
			spellInfo_1->Id == 24709 || spellInfo_1->Id == 24723 || spellInfo_1->Id == 24740) &&
		(spellInfo_2->Id == 24712 || spellInfo_2->Id == 24713 || spellInfo_2->Id == 24732 || spellInfo_2->Id == 24735 ||
			spellInfo_2->Id == 24736 || spellInfo_2->Id == 24710 || spellInfo_2->Id == 24711 || spellInfo_2->Id == 24708 ||
			spellInfo_2->Id == 24709 || spellInfo_2->Id == 24723 || spellInfo_2->Id == 24740))
	{
		return true;
	}

	// Arcane Brilliance and Intellect Scrolls should not stack.
	if (spellInfo_1->EffectApplyAuraName[0] == 29 && spellInfo_2->EffectApplyAuraName[0] == 29 &&
		((spellInfo_1->SpellIconID == 1694 && spellInfo_2->SpellIconID == 125) ||
		(spellInfo_1->SpellIconID == 125 && spellInfo_2->SpellIconID == 1694) ||
		(spellInfo_1->SpellIconID == 125 && spellInfo_2->SpellIconID == 125)))
	{
		return true;
	}

	// (Prayer of ) Spirit and Spirit scrolls should not stack.
	if (spellInfo_1->EffectApplyAuraName[0] == 29 && spellInfo_2->EffectApplyAuraName[0] == 29 &&
		(((spellInfo_1->SpellIconID == 1879 || spellInfo_1->SpellIconID == 1870) && spellInfo_2->SpellIconID == 208) ||
		(spellInfo_1->SpellIconID == 208 && (spellInfo_2->SpellIconID == 1879 || spellInfo_2->SpellIconID == 1870))))
	{
		return true;
	}

	// (Prayer of ) Spirit should stack with Blessed Sunfruit Juice or Bottled Alterac Spring Water.
	if (spellInfo_1->EffectApplyAuraName[0] == 29 && spellInfo_2->EffectApplyAuraName[0] == 29 && 
		(((spellInfo_1->SpellIconID == 1879 || spellInfo_1->SpellIconID == 1870) && spellInfo_2->SpellIconID == 79) ||
		(spellInfo_1->SpellIconID == 79 && (spellInfo_2->SpellIconID == 1879 || spellInfo_2->SpellIconID == 1870))))
	{
		return false;
	}

	// (Prayer of ) Fortitude and Fortitude scrolls/Elixir of brute force should not stack.
	if (spellInfo_1->EffectApplyAuraName[0] == 29 && spellInfo_2->EffectApplyAuraName[0] == 29 &&
		((((spellInfo_1->SpellIconID == 685 || spellInfo_1->SpellIconID == 1669) && spellInfo_2->SpellIconID == 312) ||
		(spellInfo_1->SpellIconID == 312 && (spellInfo_2->SpellIconID == 685 || spellInfo_2->SpellIconID == 1669))) ||
        (((spellInfo_1->SpellIconID == 685 || spellInfo_1->SpellIconID == 1669) && spellInfo_2->Id == 17537) ||
        (spellInfo_1->Id == 17537 && (spellInfo_2->SpellIconID == 685 || spellInfo_2->SpellIconID == 1669)))))
	{
		return true;
	}

    // Mongoose and agility elixir should stack with Grilled Squid.
    if((spellInfo_1->Id == 18192 && spellInfo_2->Id == 17538) || (spellInfo_2->Id == 18192 && spellInfo_1->Id == 17538) || 
        (spellInfo_2->Id == 11328 && spellInfo_1->Id == 18192) || (spellInfo_2->Id == 18192 && spellInfo_1->Id == 11328) || 
        (spellInfo_2->Id == 11334 && spellInfo_1->Id == 18192) || (spellInfo_2->Id == 18192 && spellInfo_1->Id == 11334))
        return false;

    // Arcane Intellect should not stack with Elixir of Greater Intellect
    if ((spellInfo_1->Id == 11396 && sSpellMgr.GetFirstSpellInChain(spellInfo_2->Id) == 1459) || 
        (spellInfo_2->Id == 11396 && sSpellMgr.GetFirstSpellInChain(spellInfo_1->Id) == 1459))
            return true;

    // Arcane Intellect should not stack with Elixir of the Sages.
    if ((spellInfo_1->Id == 17535 && sSpellMgr.GetFirstSpellInChain(spellInfo_2->Id) == 1459) || 
        (spellInfo_2->Id == 17535 && sSpellMgr.GetFirstSpellInChain(spellInfo_1->Id) == 1459))
            return true;

    // Arcane Intellect should not stack with Elixir of Wisdom.
    if ((spellInfo_1->Id == 3166 && sSpellMgr.GetFirstSpellInChain(spellInfo_2->Id) == 1459) || 
        (spellInfo_2->Id == 3166 && sSpellMgr.GetFirstSpellInChain(spellInfo_1->Id) == 1459))
            return true;
            
    // Juju Guilde should not stack with Arcane Intellect 
    if ((spellInfo_1->Id == 16327 && sSpellMgr.GetFirstSpellInChain(spellInfo_2->Id) == 1459) || 
        (spellInfo_2->Id == 16327 && sSpellMgr.GetFirstSpellInChain(spellInfo_1->Id) == 1459))
            return true;

    // Juju Guilde should not stack with Elixir of Wisdom.
    if ((spellInfo_1->Id == 3166 && sSpellMgr.GetFirstSpellInChain(spellInfo_2->Id) == 16327) || 
        (spellInfo_2->Id == 3166 && sSpellMgr.GetFirstSpellInChain(spellInfo_1->Id) == 16327))
            return true;

    // Elixir of the Sages should not stack with Exlixir of Greater Intellect
    if ((spellInfo_1->Id == 17535 && spellInfo_2->Id == 11396) || 
        (spellInfo_2->Id == 17535 && spellInfo_1->Id == 11396))
            return true;

    // Elixir of the Sages should not stack with Juju Guile.
    if ((spellInfo_1->Id == 17535 && spellInfo_2->Id == 16327) || 
        (spellInfo_2->Id == 17535 && spellInfo_1->Id == 16327))
            return true;
            
    // Divine spirit, Prayer of spirit, elixir of sages and crystal force shouldn't stack        
    if((spellInfo_1->Id == 15231 || spellInfo_1->Id == 17535 || (spellInfo_1->Id == 27681 || sSpellMgr.GetFirstSpellInChain(spellInfo_1->Id) == 14752)) &&
        (spellInfo_2->Id == 15231 || spellInfo_2->Id == 17535 || (spellInfo_2->Id == 27681 || sSpellMgr.GetFirstSpellInChain(spellInfo_2->Id) == 14752)))
        return true;

    // Juju Guile should not stack with Exlixir of Greater Intellect
    if ((spellInfo_1->Id == 16327 && spellInfo_2->Id == 11396) || 
        (spellInfo_2->Id == 16327 && spellInfo_1->Id == 11396))
            return true;
            
    // Juju Guile should not stack with Arcane Brilliance
    if ((spellInfo_1->Id == 16327 && spellInfo_2->Id == 23028) || 
        (spellInfo_2->Id == 16327 && spellInfo_1->Id == 23028))
            return true;

    // Arcane Brilliance should not stack with Elixir of Greater Intellect
    if ((spellInfo_1->Id == 11396 && spellInfo_2->Id == 23028) || 
        (spellInfo_2->Id == 11396 && spellInfo_1->Id == 23028))
            return true;

    // Arcane Brilliance should not stack with Elixir of the Sages.
    if ((spellInfo_1->Id == 17535 && spellInfo_2->Id == 23028) || 
        (spellInfo_2->Id == 17535 && spellInfo_1->Id == 23028))
            return true;

    // Arcane Brilliance should not stack with Elixir of Wisdom.
    if ((spellInfo_1->Id == 3166 && spellInfo_2->Id == 23028) || 
        (spellInfo_2->Id == 3166 && spellInfo_1->Id == 23028))
            return true;

    // Flask of Supreme Power should stack with (Greater) Arcane Elixir.
    if ((spellInfo_1->Id == 17628  && spellInfo_2->Id == 17539) ||
        (spellInfo_2->Id == 17628  && spellInfo_1->Id == 17539) ||
        (spellInfo_2->Id == 17628  && spellInfo_1->Id == 11390) ||
        (spellInfo_1->Id == 17628  && spellInfo_2->Id == 11390))
        return false;
    
    // Elixir of Giants, Brute force and Juju power(str) shouldn't stack
    // TODO, Giants can't be overwritten by Juju even tho Juju has higher stats and Giants can't overwrite Juju
    if(spellInfo_1->Id == 11405 || spellInfo_1->Id == 17537|| spellInfo_1->Id == 16323)
    {
        switch(spellInfo_2->Id)
        {
        case 11405:
        case 17537:
        case 16323:
            return true;
        default:
            break;
        }
    }
    
    // Juju Might should not stack with Winterfall Firewater        
    if ((spellInfo_1->Id == 16329 && spellInfo_2->Id == 17038) || 
    (spellInfo_2->Id == 16329 && spellInfo_1->Id == 17038))
        return true;
        
    // Spirit of Zanza should not stack with Lung Juice Cocktail
    if ((spellInfo_1->Id == 24382 && spellInfo_2->Id == 10668) || 
    (spellInfo_2->Id == 24382 && spellInfo_1->Id == 10668))
        return true;
            
    // Beer that shouldn't stack
    if(spellInfo_1->Id == 22789 || spellInfo_1->Id == 20875|| spellInfo_1->Id == 25722
        || spellInfo_1->Id == 25804 || spellInfo_1->Id == 25037)
    {
        switch(spellInfo_2->Id)
        {
        case 22789: // Gordok Green Grog
        case 20875: // Rumsey Rum
        case 25722: // Rumsey Rum Dark
        case 25804: // Rumsey Rum Black Label
        case 25037: // Rumsey Rum Light
            return true;
        default:
            break;
        }
    }
    
    // Food buffs that shouldn't stack
    if(spellInfo_1->Id == 25661 || spellInfo_1->Id == 24799 || spellInfo_1->Id == 25941 
        || spellInfo_1->Id == 25694 || spellInfo_1->Id == 22730 || spellInfo_1->Id ==  18194 
        || spellInfo_1->Id ==  18222 || spellInfo_1->Id ==  18192 || spellInfo_1->Id ==  18193 
        || spellInfo_1->Id ==  23697 || spellInfo_1->Id ==  18141 || spellInfo_1->Id ==  18125 
        || spellInfo_1->Id ==  18191 || spellInfo_1->Id ==  19710 || spellInfo_1->Id ==  19709 
        || spellInfo_1->Id ==  19708 || spellInfo_1->Id ==  19706 || spellInfo_1->Id ==  19705)
    {
        switch(spellInfo_2->Id)
        {
        case 18141: // blessed sunfruit juice
        case 18125: // blessed sunfruit
        case 18191: // windblossom berries
        case 18192: // grilled squid
        case 18193: // hot smoked bass
        case 18194: // nightfin soup
        case 18222: //poached sunscale salmon
        case 19705: // 2 stam, 2 spirit 
        case 19706: // 4 stam, 4 spirit 
        case 19708: // 6 stam, 6 spirit            
        case 19709: // 8 stam, 8 spirit 
        case 19710: // 12 stam, 12 spirit                 
        case 22730: // runn tum tuber surprise       
        case 23697: // bottled alterac spring water
        case 24799:  //smoked desert dumplings                  
        case 25661: // dirge's kickin' chimaerok chops
        case 25694: // smoked sagefish
        case 25941:  // sagefish delight      
            return true;
        default:
            break;
        }
    }
    
     // Priest t2 8 piece set bonus hot should stack with renew
    if ((spellInfo_1->Id == 22009 || sSpellMgr.GetFirstSpellInChain(spellInfo_1->Id) == 139) && 
        (spellInfo_2->Id == 22009 || sSpellMgr.GetFirstSpellInChain(spellInfo_2->Id) == 139))
        return false;
    
    // inspiration(priest) and ancestral fortitude(shaman) shouldn't stack, with each other or different ranks
    // TODO, r1 overwrites r3 and so on, this shouldn't be the case
     if(spellInfo_1->Id == 14893 || spellInfo_1->Id == 15357 || spellInfo_1->Id == 15359 
         || spellInfo_1->Id == 16177 || spellInfo_1->Id == 16236 || spellInfo_1->Id == 16237)
    {
        switch(spellInfo_2->Id)
        {
        case 14893:
        case 15357:
        case 15359:
        case 16177:
        case 16236:
        case 16237:
            return true;
        default:
            break;
        }
    }
        
    // Specific spell family spells
    switch(spellInfo_1->SpellFamilyName)
    {
        case SPELLFAMILY_GENERIC:
            switch(spellInfo_2->SpellFamilyName)
            {
                case SPELLFAMILY_GENERIC:                   // same family case
                {
                    // Thunderfury
                    if ((spellInfo_1->Id == 21992 && spellInfo_2->Id == 27648) ||
                        (spellInfo_2->Id == 21992 && spellInfo_1->Id == 27648))
                        return false;

                    // Lightning Speed (Mongoose) and Fury of the Crashing Waves (Tsunami Talisman)
                    if ((spellInfo_1->Id == 28093 && spellInfo_2->Id == 42084) ||
                        (spellInfo_2->Id == 28093 && spellInfo_1->Id == 42084))
                        return false;

                    // Soulstone Resurrection and Twisting Nether (resurrector)
                    if (spellInfo_1->SpellIconID == 92 && spellInfo_2->SpellIconID == 92 && (
                        (spellInfo_1->SpellVisual == 99 && spellInfo_2->SpellVisual == 0) ||
                        (spellInfo_2->SpellVisual == 99 && spellInfo_1->SpellVisual == 0)))
                        return false;

                    // Heart of the Wild and (Primal Instinct (Idol of Terror) triggering spell or Agility)
                    if (spellInfo_1->SpellIconID == 240 && spellInfo_2->SpellIconID == 240 && (
                        (spellInfo_1->SpellVisual == 0 && spellInfo_2->SpellVisual == 78) ||
                        (spellInfo_2->SpellVisual == 0 && spellInfo_1->SpellVisual == 78)))
                        return false;

                    // Personalized Weather (thunder effect should overwrite rainy aura)
                    if (spellInfo_1->SpellIconID == 2606 && spellInfo_2->SpellIconID == 2606)
                        return false;

                    // Brood Affliction: Bronze
                    if ((spellInfo_1->Id == 23170 && spellInfo_2->Id == 23171) ||
                        (spellInfo_2->Id == 23170 && spellInfo_1->Id == 23171))
                        return false;

                    // Regular and Night Elf Ghost
                    if ((spellInfo_1->Id == 8326 && spellInfo_2->Id == 20584) ||
                        (spellInfo_2->Id == 8326 && spellInfo_1->Id == 20584))
                         return false;

                    // Blood Siphon
                    if ((spellInfo_1->Id == 24322 && spellInfo_2->Id == 24324) ||
                        (spellInfo_2->Id == 24322 && spellInfo_1->Id == 24324))
                         return false;

					// Mind Control at Razorgore in BWL.
					if (spellInfo_1->Id == 19832 && spellInfo_2->Id == 23014)
					 return false;


					// General scrolls and the like should not stack with eachother.
					if (spellInfo_1->EffectApplyAuraName[0] == 29 && spellInfo_2->EffectApplyAuraName[0] == 29 &&
						IsPositiveSpell(spellInfo_1) && IsPositiveSpell(spellInfo_2) &&
						spellInfo_1->SpellIconID == spellInfo_2->SpellIconID)
					{
						return true;
					}

                    break;
                }
                case SPELLFAMILY_MAGE:
                    // Arcane Intellect and Insight
                    if (spellInfo_2->SpellIconID == 125 && spellInfo_1->Id == 18820)
                        return false;
                    break;
                case SPELLFAMILY_WARRIOR:
                {
                    // Scroll of Protection and Defensive Stance (multi-family check)
                    if (spellInfo_1->SpellIconID == 276 && spellInfo_1->SpellVisual == 196 && spellInfo_2->Id == 71)
                        return false;

                    // Improved Hamstring -> Hamstring (multi-family check)
                    if ((spellInfo_2->SpellFamilyFlags & UI64LIT(0x2)) && spellInfo_1->Id == 23694)
                        return false;

                    
                    //Thunderfury and Thunderclap
                    if(spellInfo_1->Id == 27648)
                    {
                        switch(spellInfo_2->Id)
                        {
                        case 6343:
                        case 8198:
                        case 8204:
                        case 8205:
                        case 11580:
                        case 11581:
                            return true;
                        default:
                            break;
                        }
                    }

                    break;
                }
                case SPELLFAMILY_DRUID:
                {
                    // Scroll of Stamina and Leader of the Pack (multi-family check)
                    if (spellInfo_1->SpellIconID == 312 && spellInfo_1->SpellVisual == 216 && spellInfo_2->Id == 24932)
                        return false;

                    // Dragonmaw Illusion (multi-family check)
                    if (spellId_1 == 40216 && spellId_2 == 42016)
                        return false;

                    if(spellInfo_1->Id == 27648)
                    {
                        switch(spellInfo_2->Id)  
                        {
                        case 16914:
                        case 17401:
                        case 17402:
                            return true;
                        default:
                            break;
                        }
                    }


                    break;
                }
                case SPELLFAMILY_ROGUE:
                {
                    // Garrote-Silence -> Garrote (multi-family check)
                    if (spellInfo_1->SpellIconID == 498 && spellInfo_1->SpellVisual == 0 && spellInfo_2->SpellIconID == 498)
                        return false;

					// Improved Sprint && Sprint
					if (spellInfo_1 ->SpellIconID == 516 && spellInfo_2->SpellIconID == 516)
						return false;

					// Touch of Zanzil && Stealth
					if (spellInfo_1->Id == 9991 && spellInfo_2->SpellVisual == 184 && spellInfo_2->SpellIconID == 250)
						return false;

                    break;
                }
                case SPELLFAMILY_HUNTER:
                {
                    // Concussive Shot and Imp. Concussive Shot (multi-family check)
                    if (spellInfo_1->Id == 19410 && spellInfo_2->Id == 5116)
                        return false;

                    // Improved Wing Clip -> Wing Clip (multi-family check)
                    if ((spellInfo_2->SpellFamilyFlags & UI64LIT(0x40)) && spellInfo_1->Id == 19229)
                        return false;
                    break;
                }
                case SPELLFAMILY_PALADIN:
                {
                    // Unstable Currents and other -> *Sanctity Aura (multi-family check)
                    if (spellInfo_2->SpellIconID==502 && spellInfo_1->SpellIconID==502 && spellInfo_1->SpellVisual==969)
                        return false;

                    // *Band of Eternal Champion and Seal of Command(multi-family check)
                    if (spellId_1 == 35081 && spellInfo_2->SpellIconID==561 && spellInfo_2->SpellVisual==7992)
                        return false;

                    break;
                }
            }
            // Dragonmaw Illusion, Blood Elf Illusion, Human Illusion, Illidari Agent Illusion, Scarlet Crusade Disguise
            if(spellInfo_1->SpellIconID == 1691 && spellInfo_2->SpellIconID == 1691)
                return false;
            break;
        case SPELLFAMILY_MAGE:
            if( spellInfo_2->SpellFamilyName == SPELLFAMILY_MAGE )
            {
                // Blizzard & Chilled (and some other stacked with blizzard spells
                if (((spellInfo_1->SpellFamilyFlags & UI64LIT(0x80)) && (spellInfo_2->SpellFamilyFlags & UI64LIT(0x100000))) ||
                        ((spellInfo_2->SpellFamilyFlags & UI64LIT(0x80)) && (spellInfo_1->SpellFamilyFlags & UI64LIT(0x100000))))
                    return false;

                // Blink & Improved Blink
                if (((spellInfo_1->SpellFamilyFlags & UI64LIT(0x0000000000010000)) && (spellInfo_2->SpellVisual == 72 && spellInfo_2->SpellIconID == 1499)) ||
                        ((spellInfo_2->SpellFamilyFlags & UI64LIT(0x0000000000010000)) && (spellInfo_1->SpellVisual == 72 && spellInfo_1->SpellIconID == 1499)))
                    return false;

                // Fireball & Pyroblast (Dots)
                if (((spellInfo_1->SpellFamilyFlags & UI64LIT(0x1)) && (spellInfo_2->SpellFamilyFlags & UI64LIT(0x400000))) ||
                        ((spellInfo_2->SpellFamilyFlags & UI64LIT(0x1)) && (spellInfo_1->SpellFamilyFlags & UI64LIT(0x400000))))
                    return false;
            }
            // Detect Invisibility and Mana Shield (multi-family check)
            if (spellInfo_2->Id == 132 && spellInfo_1->SpellIconID == 209 && spellInfo_1->SpellVisual == 968)
                return false;

            // Combustion and Fire Protection Aura (multi-family check)
            if (spellInfo_1->Id == 11129 && spellInfo_2->SpellIconID == 33 && spellInfo_2->SpellVisual == 321)
                return false;

            // Arcane Intellect and Insight
            if (spellInfo_1->SpellIconID == 125 && spellInfo_2->Id == 18820)
                return false;

            // Arcane Missiles should not overwrite Arcane Missiles
            if (spellInfo_1->SpellIconID == 225 && spellInfo_2->SpellIconID == 225)
                return false;

            break;
        case SPELLFAMILY_WARLOCK:
            if (spellInfo_2->SpellFamilyName == SPELLFAMILY_WARLOCK)
            {
                // Siphon Life and Drain Life
                if ((spellInfo_1->SpellIconID == 152 && spellInfo_2->SpellIconID == 546) ||
                    (spellInfo_2->SpellIconID == 152 && spellInfo_1->SpellIconID == 546))
                    return false;

                //Corruption & Seed of corruption
                if ((spellInfo_1->SpellIconID == 313 && spellInfo_2->SpellIconID == 1932) ||
                    (spellInfo_2->SpellIconID == 313 && spellInfo_1->SpellIconID == 1932))
                    if(spellInfo_1->SpellVisual != 0 && spellInfo_2->SpellVisual != 0)
                        return true;                        // can't be stacked

                // Corruption and (Curse of Agony or Curse of Doom)
                if ((spellInfo_1->SpellIconID == 313 && (spellInfo_2->SpellIconID == 544  || spellInfo_2->SpellIconID == 91)) ||
                    (spellInfo_2->SpellIconID == 313 && (spellInfo_1->SpellIconID == 544  || spellInfo_1->SpellIconID == 91)))
                    return false;
            }
            // Detect Invisibility and Mana Shield (multi-family check)
            if (spellInfo_1->Id == 132 && spellInfo_2->SpellIconID == 209 && spellInfo_2->SpellVisual == 968)
                return false;
            break;
        case SPELLFAMILY_WARRIOR:
			{
				if (spellInfo_2->SpellFamilyName == SPELLFAMILY_WARRIOR)
				{
					// Rend and Deep Wound
					if (((spellInfo_1->SpellFamilyFlags & UI64LIT(0x20)) && (spellInfo_2->SpellFamilyFlags & UI64LIT(0x1000000000))) ||
						((spellInfo_2->SpellFamilyFlags & UI64LIT(0x20)) && (spellInfo_1->SpellFamilyFlags & UI64LIT(0x1000000000))))
						return false;

					// Battle Shout and Rampage
					if ((spellInfo_1->SpellIconID == 456 && spellInfo_2->SpellIconID == 2006) ||
						(spellInfo_2->SpellIconID == 456 && spellInfo_1->SpellIconID == 2006))
						return false;
                    
                    // Battle Shout and Lord General's Sword should stack
                    if ((spellInfo_1->SpellIconID == 456 && spellInfo_2->Id == 15602) ||
                        (spellInfo_2->SpellIconID == 456 && spellInfo_1->Id == 15602))
                        return false;
				}

				// Hamstring -> Improved Hamstring (multi-family check)
				if ((spellInfo_1->SpellFamilyFlags & UI64LIT(0x2)) && spellInfo_2->Id == 23694)
					return false;

				// Defensive Stance and Scroll of Protection (multi-family check)
				if (spellInfo_1->Id == 71 && spellInfo_2->SpellIconID == 276 && spellInfo_2->SpellVisual == 196)
					return false;

				// Bloodlust and Bloodthirst (multi-family check)
				if (spellInfo_2->Id == 2825 && spellInfo_1->SpellIconID == 38 && spellInfo_1->SpellVisual == 0)
					return false;

				// Sunder Armor -> Expose Armor
				if (spellInfo_1->SpellIconID == 565 && spellInfo_1->SpellVisual == 406 && spellInfo_2->SpellIconID == 563 && spellInfo_2->SpellVisual == 3441)
					return true;

				// Don't let a positive shout overwrite a negative shout or vice versa. 
				// (Or any spell for that matter so if there are other spell where a negative spell overwriteas a positive and vice versa this code might need to be moved to handle all spells.)
				if (IsPositiveSpell(spellInfo_1) != IsPositiveSpell(spellInfo_2))
					return false;

                // Thunderfury and Thunderclap
                switch(spellInfo_1->Id)
                {
                case 6343:
                case 8198:
                case 8204:
                case 8205:
                case 11580:
                case 11581:
                    if(spellInfo_2->Id == 27648)
                        return true;
                default:
                    break;
                }


				break;
			}
        case SPELLFAMILY_PRIEST:
            if (spellInfo_2->SpellFamilyName == SPELLFAMILY_PRIEST)
            {
                //Devouring Plague and Shadow Vulnerability
                if (((spellInfo_1->SpellFamilyFlags & UI64LIT(0x2000000)) && (spellInfo_2->SpellFamilyFlags & UI64LIT(0x800000000))) ||
                    ((spellInfo_2->SpellFamilyFlags & UI64LIT(0x2000000)) && (spellInfo_1->SpellFamilyFlags & UI64LIT(0x800000000))))
                    return false;

                //StarShards and Shadow Word: Pain
                if (((spellInfo_1->SpellFamilyFlags & UI64LIT(0x200000)) && (spellInfo_2->SpellFamilyFlags & UI64LIT(0x8000))) ||
                    ((spellInfo_2->SpellFamilyFlags & UI64LIT(0x200000)) && (spellInfo_1->SpellFamilyFlags & UI64LIT(0x8000))))
                    return false;

				// Mind Flay
				if ((spellId_1 == 15407 || spellId_1 == 17311 ||spellId_1 == 17312 || spellId_1 == 17313 || spellId_1 == 17314 || spellId_1 == 18807) &&
					(spellId_2 == 15407 || spellId_2 == 17311 ||spellId_2 == 17312 || spellId_2 == 17313 || spellId_2 == 17314 || spellId_2 == 18807))
					return false;
            }
            break;
        case SPELLFAMILY_DRUID:
            if (spellInfo_2->SpellFamilyName == SPELLFAMILY_DRUID)
            {
                // Cat Form and Feline Swiftness Passive* (1.x specific conflict)
                if (spellInfo_1->SpellIconID == 493 && spellInfo_2->SpellIconID == 493)
                    return false;

                //Omen of Clarity and Blood Frenzy
                if (((spellInfo_1->SpellFamilyFlags == UI64LIT(0x0) && spellInfo_1->SpellIconID == 108) && (spellInfo_2->SpellFamilyFlags & UI64LIT(0x20000000000000))) ||
                    ((spellInfo_2->SpellFamilyFlags == UI64LIT(0x0) && spellInfo_2->SpellIconID == 108) && (spellInfo_1->SpellFamilyFlags & UI64LIT(0x20000000000000))))
                    return false;

                // Wrath of Elune and Nature's Grace
                if ((spellInfo_1->Id == 16886 && spellInfo_2->Id == 46833) ||
                    (spellInfo_2->Id == 16886 && spellInfo_1->Id == 46833))
                    return false;

                // Bear Rage (Feral T4 (2)) and Omen of Clarity
                if ((spellInfo_1->Id == 16864 && spellInfo_2->Id == 37306) ||
                    (spellInfo_2->Id == 16864 && spellInfo_1->Id == 37306))
                    return false;

                // Cat Energy (Feral T4 (2)) and Omen of Clarity
                if ((spellInfo_1->Id == 16864 && spellInfo_2->Id == 37311) ||
                    (spellInfo_2->Id == 16864 && spellInfo_1->Id == 37311))
                    return false;
                
                // Mark of the Wild and Gift of the Wild should not stack.
                if (IsEQSpells(spellId_1, spellId_2, 21849, 1126) || IsEQSpells(spellId_1, spellId_2, 21849, 5232) ||
                    IsEQSpells(spellId_1, spellId_2, 21849, 6756) || IsEQSpells(spellId_1, spellId_2, 21850, 1126) ||
                    IsEQSpells(spellId_1, spellId_2, 21850, 5232) || IsEQSpells(spellId_1, spellId_2, 21850, 6756))
                    return true;
            }

            // Hurricane and Thunderfury
            switch(spellInfo_1->Id)
            {
            case 16914:
            case 17401:
            case 17402:
                if(spellInfo_2->Id == 27648)
                    return  true;
            default:
                break;
            }

            // Leader of the Pack and Scroll of Stamina (multi-family check)
            if (spellInfo_1->Id == 24932 && spellInfo_2->SpellIconID == 312 && spellInfo_2->SpellVisual == 216)
                return false;

            // Dragonmaw Illusion (multi-family check)
            if (spellId_1 == 42016 && spellId_2 == 40216 )
                return false;

            break;
        case SPELLFAMILY_ROGUE:
            if (spellInfo_2->SpellFamilyName == SPELLFAMILY_ROGUE)
            {
                // Master of Subtlety
                if ((spellId_1 == 31665 && spellId_2 == 31666) ||
                    (spellId_1 == 31666 && spellId_2 == 31665))
                    return false;
            }

            // Garrote -> Garrote-Silence (multi-family check)
            if (spellInfo_1->SpellIconID == 498 && spellInfo_2->SpellIconID == 498 && spellInfo_2->SpellVisual == 0)
                return false;

			// Expose Armor -> Sunder Armor
			if (spellInfo_1->SpellIconID == 563 && spellInfo_1->SpellVisual == 3441 && spellInfo_2->SpellIconID == 565 && spellInfo_2->SpellVisual == 406)
				return true;

			// Stealth && Touch of Zanzil
			if (spellInfo_2->Id == 9991 && spellInfo_1->SpellVisual == 184 && spellInfo_1->SpellIconID == 250)
				return false;

            break;
        case SPELLFAMILY_HUNTER:
            if (spellInfo_2->SpellFamilyName == SPELLFAMILY_HUNTER)
            {
                // Rapid Fire & Quick Shots
                if (((spellInfo_1->SpellFamilyFlags & UI64LIT(0x20)) && (spellInfo_2->SpellFamilyFlags & UI64LIT(0x20000000000))) ||
                    ((spellInfo_2->SpellFamilyFlags & UI64LIT(0x20)) && (spellInfo_1->SpellFamilyFlags & UI64LIT(0x20000000000))) )
                    return false;

                // Serpent Sting & (Immolation/Explosive Trap Effect)
                if (((spellInfo_1->SpellFamilyFlags & UI64LIT(0x4)) && (spellInfo_2->SpellFamilyFlags & UI64LIT(0x00000004000))) ||
                    ((spellInfo_2->SpellFamilyFlags & UI64LIT(0x4)) && (spellInfo_1->SpellFamilyFlags & UI64LIT(0x00000004000))))
                    return false;

                // Bestial Wrath
                if (spellInfo_1->SpellIconID == 1680 && spellInfo_2->SpellIconID == 1680)
                    return false;

                // Viper sting shouldn't stack with itself.
                switch(spellInfo_1->Id)
                {
                case 3034:
                case 14279:
                case 14280:
                    if(spellInfo_2->Id == spellInfo_1->Id)
                        return true;
                default:
                    break;
                }
            }

            // Wing Clip -> Improved Wing Clip (multi-family check)
            if ((spellInfo_1->SpellFamilyFlags & UI64LIT(0x40)) && spellInfo_2->Id == 19229)
                return false;

            // Concussive Shot and Imp. Concussive Shot (multi-family check)
            if (spellInfo_2->Id == 19410 && spellInfo_1->Id == 5116)
                return false;
            break;
        case SPELLFAMILY_PALADIN:
            if (spellInfo_2->SpellFamilyName == SPELLFAMILY_PALADIN)
            {
                // Paladin Seals
                if (IsSealSpell(spellInfo_1) && IsSealSpell(spellInfo_2))
                    return true;
                // Concentration Aura and Improved Concentration Aura and Aura Mastery
                if ((spellInfo_1->SpellIconID == 1487) && (spellInfo_2->SpellIconID == 1487))
                    return false;
            }

            // Combustion and Fire Protection Aura (multi-family check)
            if (spellInfo_2->Id == 11129 && spellInfo_1->SpellIconID == 33 && spellInfo_1->SpellVisual == 321)
                return false;

            // *Sanctity Aura -> Unstable Currents and other (multi-family check)
            if (spellInfo_1->SpellIconID==502 && spellInfo_2->SpellFamilyName == SPELLFAMILY_GENERIC && spellInfo_2->SpellIconID==502 && spellInfo_2->SpellVisual==969)
                return false;

            // *Seal of Command and Band of Eternal Champion (multi-family check)
            if (spellInfo_1->SpellIconID==561 && spellInfo_1->SpellVisual==7992 && spellId_2 == 35081)
                return false;

            if (!IsPositiveSpell(spellInfo_1) && !IsPositiveSpell(spellInfo_2))
            {
                // For stacking of the different Judgements.
                std::string spellName1(*spellInfo_1->SpellName);
                std::string spellName2(*spellInfo_2->SpellName);
                if (spellName1.find("Wisdom") != std::string::npos && 
                    spellName2.find("Light") != std::string::npos)
                    return false;

                if (spellName1.find("Crusader") != std::string::npos && 
                    spellName2.find("Crusader") != std::string::npos)
                    return true;

                if (spellName1.find("Wisdom") != std::string::npos && 
                    spellName2.find("Wisdom") != std::string::npos)
                    return true;

                if (spellName1.find("Light") != std::string::npos && 
                    spellName2.find("Light") != std::string::npos)
                    return true;
            }

            break;
        case SPELLFAMILY_SHAMAN:
            if (spellInfo_2->SpellFamilyName == SPELLFAMILY_SHAMAN)
            {
                // Windfury weapon
                if (spellInfo_1->SpellIconID==220 && spellInfo_2->SpellIconID==220 &&
                    !spellInfo_1->IsFitToFamilyMask(spellInfo_2->SpellFamilyFlags))
                    return false;               
            }
            // Bloodlust and Bloodthirst (multi-family check)
            if (spellInfo_1->Id == 2825 && spellInfo_2->SpellIconID == 38 && spellInfo_2->SpellVisual == 0)
                return false;
            break;
		case SPELLFAMILY_POTION:
			{
				// Mighty Rage Potion and Elixir of Giants should stack.
				if ((spellInfo_1->Id == 11405 && spellInfo_2->Id == 17528) || (spellInfo_2->Id == 11405 && spellInfo_1->Id == 17528))
					return false;

				// Flask of the Titans and Elixir of Minor Fortitude should stack.
				if ((spellInfo_1->Id == 17626 && spellInfo_2->Id == 2378) || (spellInfo_2->Id == 17626 && spellInfo_1->Id == 2378) ||
                    (spellInfo_1->Id == 17626 && spellInfo_2->Id == 3593) || (spellInfo_2->Id == 17626 && spellInfo_1->Id == 3593))
					return false;
			}
        default:
            break;
    }

    // more generic checks
	if (IsRankSpellDueToSpell(spellInfo_1, spellId_2))
	{
		bool isDoT = true;
		for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
		{
			if (spellInfo_1->EffectApplyAuraName[i] != 0								  &&
				spellInfo_1->EffectApplyAuraName[i] != SPELL_AURA_PERIODIC_DAMAGE		  &&
				spellInfo_1->EffectApplyAuraName[i] != SPELL_AURA_PERIODIC_LEECH	      &&
				spellInfo_1->EffectApplyAuraName[i] != SPELL_AURA_PERIODIC_MANA_LEECH     &&
				spellInfo_1->EffectApplyAuraName[i] != SPELL_AURA_PERIODIC_DAMAGE_PERCENT)
				isDoT = false;
		}
		if (isDoT)
			return false;
		else
			return true;
	}

    if (spellInfo_1->SpellFamilyName == 0 || spellInfo_2->SpellFamilyName == 0)
        return false;

    if (spellInfo_1->SpellFamilyName != spellInfo_2->SpellFamilyName)
        return false;

    bool dummy_only = true;
    for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        if (spellInfo_1->Effect[i] != spellInfo_2->Effect[i] ||
            spellInfo_1->EffectItemType[i] != spellInfo_2->EffectItemType[i] ||
            spellInfo_1->EffectMiscValue[i] != spellInfo_2->EffectMiscValue[i] ||
            spellInfo_1->EffectApplyAuraName[i] != spellInfo_2->EffectApplyAuraName[i])
            return false;

        // ignore dummy only spells
        if (spellInfo_1->Effect[i] && spellInfo_1->Effect[i] != SPELL_EFFECT_DUMMY && spellInfo_1->EffectApplyAuraName[i] != SPELL_AURA_DUMMY)
            dummy_only = false;
    }
    if (dummy_only)
        return false;

    return true;
}

bool SpellMgr::IsEQSpells(uint32 spellId_1, uint32 spellId_2, uint32 givenId_1, uint32 givenId_2)
{
    return (spellId_1 == givenId_1 || spellId_2 == givenId_1) && (spellId_1 == givenId_2 || spellId_2 == givenId_2);
}

bool SpellMgr::IsProfessionOrRidingSpell(uint32 spellId)
{
    SpellEntry const *spellInfo = sSpellStore.LookupEntry(spellId);
    if(!spellInfo)
        return false;

    if(spellInfo->Effect[1] != SPELL_EFFECT_SKILL)
        return false;

    uint32 skill = spellInfo->EffectMiscValue[1];

    return IsProfessionOrRidingSkill(skill);
}

bool SpellMgr::IsProfessionSpell(uint32 spellId)
{
    SpellEntry const *spellInfo = sSpellStore.LookupEntry(spellId);
    if(!spellInfo)
        return false;

    if (spellInfo->Effect[EFFECT_INDEX_1] != SPELL_EFFECT_SKILL)
        return false;

    uint32 skill = spellInfo->EffectMiscValue[EFFECT_INDEX_1];

    return IsProfessionSkill(skill);
}

bool SpellMgr::IsPrimaryProfessionSpell(uint32 spellId)
{
    SpellEntry const *spellInfo = sSpellStore.LookupEntry(spellId);
    if(!spellInfo)
        return false;

    if (spellInfo->Effect[EFFECT_INDEX_1] != SPELL_EFFECT_SKILL)
        return false;

    uint32 skill = spellInfo->EffectMiscValue[EFFECT_INDEX_1];

    return IsPrimaryProfessionSkill(skill);
}

bool SpellMgr::IsPrimaryProfessionFirstRankSpell(uint32 spellId) const
{
    return IsPrimaryProfessionSpell(spellId) && GetSpellRank(spellId)==1;
}

bool SpellMgr::IsSkillBonusSpell(uint32 spellId) const
{
    SkillLineAbilityMapBounds bounds = GetSkillLineAbilityMapBounds(spellId);

    for(SkillLineAbilityMap::const_iterator _spell_idx = bounds.first; _spell_idx != bounds.second; ++_spell_idx)
    {
        SkillLineAbilityEntry const *pAbility = _spell_idx->second;
        if (!pAbility || pAbility->learnOnGetSkill != ABILITY_LEARNED_ON_GET_PROFESSION_SKILL)
            continue;

        if (pAbility->req_skill_value > 0)
            return true;
    }

    return false;
}

SpellEntry const* SpellMgr::SelectAuraRankForLevel(SpellEntry const* spellInfo, uint32 level) const
{
    // fast case
    if (level + 10 >= spellInfo->spellLevel)
        return spellInfo;

    // ignore selection for passive spells
    if (IsPassiveSpell(spellInfo))
        return spellInfo;

    bool needRankSelection = false;
    for(int i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        // for simple aura in check apply to any non caster based targets, in rank search mode to any explicit targets
        if (((spellInfo->Effect[i] == SPELL_EFFECT_APPLY_AURA &&
            (IsExplicitPositiveTarget(spellInfo->EffectImplicitTargetA[i]) ||
            IsAreaEffectPossitiveTarget(Targets(spellInfo->EffectImplicitTargetA[i])))) ||
            spellInfo->Effect[i] == SPELL_EFFECT_APPLY_AREA_AURA_PARTY) &&
            IsPositiveEffect(spellInfo, SpellEffectIndex(i)))
        {
            needRankSelection = true;
            break;
        }
    }

    // not required (rank check more slow so check it here)
    if (!needRankSelection || GetSpellRank(spellInfo->Id) == 0)
        return spellInfo;

    for(uint32 nextSpellId = spellInfo->Id; nextSpellId != 0; nextSpellId = GetPrevSpellInChain(nextSpellId))
    {
        SpellEntry const *nextSpellInfo = sSpellStore.LookupEntry(nextSpellId);
        if (!nextSpellInfo)
            break;

        // if found appropriate level
        if (level + 10 >= spellInfo->spellLevel)
            return nextSpellInfo;

        // one rank less then
    }

    // not found
    return NULL;
}

typedef UNORDERED_MAP<uint32,uint32> AbilitySpellPrevMap;

static void LoadSpellChains_AbilityHelper(SpellChainMap& chainMap, AbilitySpellPrevMap const& prevRanks, uint32 spell_id, uint32 prev_id, uint32 deep = 30)
{
    // spell already listed in chains store
    SpellChainMap::const_iterator chain_itr = chainMap.find(spell_id);
    if (chain_itr != chainMap.end())
    {
        MANGOS_ASSERT(chain_itr->second.prev == prev_id && "LoadSpellChains_AbilityHelper: Conflicting data in talents or spell abilities dbc");
        return;
    }

    // prev rank listed in main chain table (can fill correct data directly)
    SpellChainMap::const_iterator prev_chain_itr = chainMap.find(prev_id);
    if (prev_chain_itr != chainMap.end())
    {
        SpellChainNode node;
        node.prev  = prev_id;
        node.first = prev_chain_itr->second.first;
        node.rank  = prev_chain_itr->second.rank+1;
        node.req   = 0;
        chainMap[spell_id] = node;
        return;
    }

    // prev spell not listed in prev ranks store, so it first rank
    AbilitySpellPrevMap::const_iterator prev_itr = prevRanks.find(prev_id);
    if (prev_itr == prevRanks.end())
    {
        SpellChainNode prev_node;
        prev_node.prev  = 0;
        prev_node.first = prev_id;
        prev_node.rank  = 1;
        prev_node.req   = 0;
        chainMap[prev_id] = prev_node;

        SpellChainNode node;
        node.prev  = prev_id;
        node.first = prev_id;
        node.rank  = 2;
        node.req   = 0;
        chainMap[spell_id] = node;
        return;
    }

    if (deep == 0)
    {
        MANGOS_ASSERT(false && "LoadSpellChains_AbilityHelper: Infinity cycle in spell ability data");
        return;
    }

    // prev rank listed, so process it first
    LoadSpellChains_AbilityHelper(chainMap, prevRanks, prev_id, prev_itr->second, deep-1);

    // prev rank must be listed now
    prev_chain_itr = chainMap.find(prev_id);
    if (prev_chain_itr == chainMap.end())
        return;

    SpellChainNode node;
    node.prev  = prev_id;
    node.first = prev_chain_itr->second.first;
    node.rank  = prev_chain_itr->second.rank+1;
    node.req   = 0;
    chainMap[spell_id] = node;
}

void SpellMgr::LoadSpellChains()
{
    mSpellChains.clear();                                   // need for reload case
    mSpellChainsNext.clear();                               // need for reload case

    // load known data for talents
    for (unsigned int i = 0; i < sTalentStore.GetNumRows(); ++i)
    {
        TalentEntry const *talentInfo = sTalentStore.LookupEntry(i);
        if (!talentInfo)
            continue;

        // not add ranks for 1 ranks talents (if exist non ranks spells then it will included in table data)
        if (!talentInfo->RankID[1])
            continue;

        for (int j = 0; j < MAX_TALENT_RANK; j++)
        {
            uint32 spell_id = talentInfo->RankID[j];
            if (!spell_id)
                continue;

            if (!sSpellStore.LookupEntry(spell_id))
            {
                //sLog.outErrorDb("Talent %u not exist as spell",spell_id);
                continue;
            }

            SpellChainNode node;
            node.prev  = (j > 0) ? talentInfo->RankID[j-1] : 0;
            node.first = talentInfo->RankID[0];
            node.rank  = j+1;
            node.req   = 0;

            mSpellChains[spell_id] = node;
        }
    }

    // load known data from spell abilities
    {
        // we can calculate ranks only after full data generation
        AbilitySpellPrevMap prevRanks;
        for(SkillLineAbilityMap::const_iterator ab_itr = mSkillLineAbilityMap.begin(); ab_itr != mSkillLineAbilityMap.end(); ++ab_itr)
        {
            uint32 spell_id = ab_itr->first;

            // some forward spells not exist and can be ignored (some outdated data)
            SpellEntry const* spell_entry = sSpellStore.LookupEntry(spell_id);
            if (!spell_entry)                                   // no cases
                continue;

            // ignore spell without forwards (non ranked or missing info in skill abilities)
            uint32 forward_id = ab_itr->second->forward_spellid;

            // by some strange reason < 3.x clients not have forward spell for 2366
            if (spell_id == 2366)                           // Herb Gathering, Apprentice
                forward_id = 2368;

            if (!forward_id)
                continue;

            // some forward spells not exist and can be ignored (some outdated data)
            SpellEntry const* forward_entry = sSpellStore.LookupEntry(forward_id);
            if (!forward_entry)
                continue;

            // some forward spells still exist but excluded from real use as ranks and not listed in skill abilities now
            SkillLineAbilityMapBounds bounds = mSkillLineAbilityMap.equal_range(forward_id);
            if (bounds.first == bounds.second)
                continue;

            // spell already listed in chains store
            SpellChainMap::const_iterator chain_itr = mSpellChains.find(forward_id);
            if (chain_itr != mSpellChains.end())
            {
                MANGOS_ASSERT(chain_itr->second.prev == spell_id && "Conflicting data in talents or spell abilities dbc");
                continue;
            }

            // spell already listed in prev ranks store
            AbilitySpellPrevMap::const_iterator prev_itr = prevRanks.find(forward_id);
            if (prev_itr != prevRanks.end())
            {
                MANGOS_ASSERT(prev_itr->second == spell_id && "Conflicting data in talents or spell abilities dbc");
                continue;
            }

            // prev rank listed in main chain table (can fill correct data directly)
            SpellChainMap::const_iterator prev_chain_itr = mSpellChains.find(spell_id);
            if (prev_chain_itr != mSpellChains.end())
            {
                SpellChainNode node;
                node.prev  = spell_id;
                node.first = prev_chain_itr->second.first;
                node.rank  = prev_chain_itr->second.rank+1;
                node.req   = 0;

                mSpellChains[forward_id] = node;
                continue;
            }

            // need temporary store for later rank calculation
            prevRanks[forward_id] = spell_id;
        }

        while (!prevRanks.empty())
        {
            uint32 spell_id = prevRanks.begin()->first;
            uint32 prev_id  = prevRanks.begin()->second;
            prevRanks.erase(prevRanks.begin());

            LoadSpellChains_AbilityHelper(mSpellChains, prevRanks, spell_id, prev_id);
        }
    }

    // load custom case
    QueryResult *result = WorldDatabase.Query("SELECT spell_id, prev_spell, first_spell, rank, req_spell FROM spell_chain");
    if (!result)
    {
        BarGoLink bar(1);
        bar.step();

        sLog.outString();
        sLog.outString(">> Loaded 0 spell chain records");
        sLog.outErrorDb("`spell_chains` table is empty!");
        return;
    }

    uint32 dbc_count = mSpellChains.size();
    uint32 new_count = 0;
    uint32 req_count = 0;

    BarGoLink bar(result->GetRowCount());
    do
    {
        bar.step();
        Field *fields = result->Fetch();

        uint32 spell_id = fields[0].GetUInt32();

        SpellChainNode node;
        node.prev  = fields[1].GetUInt32();
        node.first = fields[2].GetUInt32();
        node.rank  = fields[3].GetUInt8();
        node.req   = fields[4].GetUInt32();

        if (!sSpellStore.LookupEntry(spell_id))
        {
            sLog.outErrorDb("Spell %u listed in `spell_chain` does not exist",spell_id);
            continue;
        }

        SpellChainMap::iterator chain_itr = mSpellChains.find(spell_id);
        if (chain_itr != mSpellChains.end())
        {
            if (chain_itr->second.rank != node.rank)
            {
                sLog.outErrorDb("Spell %u (prev: %u, first: %u, rank: %d, req: %u) listed in `spell_chain` expected rank %u by DBC data.",
                    spell_id,node.prev,node.first,node.rank,node.req,chain_itr->second.rank);
                continue;
            }

            if (chain_itr->second.prev != node.prev)
            {
                sLog.outErrorDb("Spell %u (prev: %u, first: %u, rank: %d, req: %u) listed in `spell_chain` expected prev %u by DBC data.",
                    spell_id,node.prev,node.first,node.rank,node.req,chain_itr->second.prev);
                continue;
            }

            if (chain_itr->second.first != node.first)
            {
                sLog.outErrorDb("Spell %u (prev: %u, first: %u, rank: %d, req: %u) listed in `spell_chain` expected first %u by DBC data.",
                    spell_id,node.prev,node.first,node.rank,node.req,chain_itr->second.first);
                continue;
            }

            // update req field by table data
            if (node.req)
            {
                chain_itr->second.req = node.req;
                ++req_count;
                continue;
            }

            // in other case redundant
            sLog.outErrorDb("Spell %u (prev: %u, first: %u, rank: %d, req: %u) already added (talent or spell ability with forward) and non need in `spell_chain`",
                spell_id,node.prev,node.first,node.rank,node.req);
            continue;
        }

        if (node.prev != 0 && !sSpellStore.LookupEntry(node.prev))
        {
            sLog.outErrorDb("Spell %u (prev: %u, first: %u, rank: %d, req: %u) listed in `spell_chain` has nonexistent previous rank spell.",
                spell_id,node.prev,node.first,node.rank,node.req);
            continue;
        }

        if(!sSpellStore.LookupEntry(node.first))
        {
            sLog.outErrorDb("Spell %u (prev: %u, first: %u, rank: %d, req: %u) listed in `spell_chain` has not existing first rank spell.",
                spell_id,node.prev,node.first,node.rank,node.req);
            continue;
        }

        // check basic spell chain data integrity (note: rank can be equal 0 or 1 for first/single spell)
        if( (spell_id == node.first) != (node.rank <= 1) ||
            (spell_id == node.first) != (node.prev == 0) ||
            (node.rank <= 1) != (node.prev == 0) )
        {
            sLog.outErrorDb("Spell %u (prev: %u, first: %u, rank: %d, req: %u) listed in `spell_chain` has not compatible chain data.",
                spell_id,node.prev,node.first,node.rank,node.req);
            continue;
        }

        if(node.req!=0 && !sSpellStore.LookupEntry(node.req))
        {
            sLog.outErrorDb("Spell %u (prev: %u, first: %u, rank: %d, req: %u) listed in `spell_chain` has not existing required spell.",
                spell_id,node.prev,node.first,node.rank,node.req);
            continue;
        }

        // talents not required data in spell chain for work, but must be checked if present for integrity
        if(TalentSpellPos const* pos = GetTalentSpellPos(spell_id))
        {
            if(node.rank!=pos->rank+1)
            {
                sLog.outErrorDb("Talent %u (prev: %u, first: %u, rank: %d, req: %u) listed in `spell_chain` has wrong rank.",
                    spell_id,node.prev,node.first,node.rank,node.req);
                continue;
            }

            if(TalentEntry const* talentEntry = sTalentStore.LookupEntry(pos->talent_id))
            {
                if(node.first!=talentEntry->RankID[0])
                {
                    sLog.outErrorDb("Talent %u (prev: %u, first: %u, rank: %d, req: %u) listed in `spell_chain` has wrong first rank spell.",
                        spell_id,node.prev,node.first,node.rank,node.req);
                    continue;
                }

                if(node.rank > 1 && node.prev != talentEntry->RankID[node.rank-1-1])
                {
                    sLog.outErrorDb("Talent %u (prev: %u, first: %u, rank: %d, req: %u) listed in `spell_chain` has wrong prev rank spell.",
                        spell_id,node.prev,node.first,node.rank,node.req);
                    continue;
                }

                if(node.req!=talentEntry->DependsOnSpell)
                {
                    sLog.outErrorDb("Talent %u (prev: %u, first: %u, rank: %d, req: %u) listed in `spell_chain` has wrong required spell.",
                        spell_id,node.prev,node.first,node.rank,node.req);
                    continue;
                }
            }
        }

        mSpellChains[spell_id] = node;

        ++new_count;
    } while( result->NextRow() );

    delete result;

    // additional integrity checks
    for(SpellChainMap::const_iterator i = mSpellChains.begin(); i != mSpellChains.end(); ++i)
    {
        if(i->second.prev)
        {
            SpellChainMap::const_iterator i_prev = mSpellChains.find(i->second.prev);
            if(i_prev == mSpellChains.end())
            {
                sLog.outErrorDb("Spell %u (prev: %u, first: %u, rank: %d, req: %u) listed in `spell_chain` has not found previous rank spell in table.",
                    i->first,i->second.prev,i->second.first,i->second.rank,i->second.req);
            }
            else if( i_prev->second.first != i->second.first )
            {
                sLog.outErrorDb("Spell %u (prev: %u, first: %u, rank: %d, req: %u) listed in `spell_chain` has different first spell in chain compared to previous rank spell (prev: %u, first: %u, rank: %d, req: %u).",
                    i->first,i->second.prev,i->second.first,i->second.rank,i->second.req,
                    i_prev->second.prev,i_prev->second.first,i_prev->second.rank,i_prev->second.req);
            }
            else if( i_prev->second.rank+1 != i->second.rank )
            {
                sLog.outErrorDb("Spell %u (prev: %u, first: %u, rank: %d, req: %u) listed in `spell_chain` has different rank compared to previous rank spell (prev: %u, first: %u, rank: %d, req: %u).",
                    i->first,i->second.prev,i->second.first,i->second.rank,i->second.req,
                    i_prev->second.prev,i_prev->second.first,i_prev->second.rank,i_prev->second.req);
            }
        }

        if(i->second.req)
        {
            SpellChainMap::const_iterator i_req = mSpellChains.find(i->second.req);
            if(i_req == mSpellChains.end())
            {
                sLog.outErrorDb("Spell %u (prev: %u, first: %u, rank: %d, req: %u) listed in `spell_chain` has not found required rank spell in table.",
                    i->first,i->second.prev,i->second.first,i->second.rank,i->second.req);
            }
            else if( i_req->second.first == i->second.first )
            {
                sLog.outErrorDb("Spell %u (prev: %u, first: %u, rank: %d, req: %u) listed in `spell_chain` has required rank spell from same spell chain (prev: %u, first: %u, rank: %d, req: %u).",
                    i->first,i->second.prev,i->second.first,i->second.rank,i->second.req,
                    i_req->second.prev,i_req->second.first,i_req->second.rank,i_req->second.req);
            }
            else if( i_req->second.req )
            {
                sLog.outErrorDb("Spell %u (prev: %u, first: %u, rank: %d, req: %u) listed in `spell_chain` has required rank spell with required spell (prev: %u, first: %u, rank: %d, req: %u).",
                    i->first,i->second.prev,i->second.first,i->second.rank,i->second.req,
                    i_req->second.prev,i_req->second.first,i_req->second.rank,i_req->second.req);
            }
        }
    }

    // fill next rank cache
    for(SpellChainMap::const_iterator i = mSpellChains.begin(); i != mSpellChains.end(); ++i)
    {
        uint32 spell_id = i->first;
        SpellChainNode const& node = i->second;

        if(node.prev)
            mSpellChainsNext.insert(SpellChainMapNext::value_type(node.prev,spell_id));

        if(node.req)
            mSpellChainsNext.insert(SpellChainMapNext::value_type(node.req,spell_id));
    }

    // check single rank redundant cases (single rank talents not added by default so this can be only custom cases)
    for(SpellChainMap::const_iterator i = mSpellChains.begin(); i != mSpellChains.end(); ++i)
    {
        // skip non-first ranks, and spells with additional reqs
        if (i->second.rank > 1 || i->second.req)
            continue;

        if (mSpellChainsNext.find(i->first) == mSpellChainsNext.end())
        {
            sLog.outErrorDb("Spell %u (prev: %u, first: %u, rank: %d, req: %u) listed in `spell_chain` has single rank data, so redundant.",
                i->first,i->second.prev,i->second.first,i->second.rank,i->second.req);
        }
    }

    sLog.outString();
    sLog.outString( ">> Loaded %u spell chain records (%u from DBC data with %u req field updates, and %u loaded from table)", dbc_count+new_count, dbc_count, req_count, new_count);
}

void SpellMgr::LoadSpellLearnSkills()
{
    mSpellLearnSkills.clear();                              // need for reload case

    // search auto-learned skills and add its to map also for use in unlearn spells/talents
    uint32 dbc_count = 0;
    BarGoLink bar(sSpellStore.GetNumRows());
    for (uint32 spell = 0; spell < sSpellStore.GetNumRows(); ++spell)
    {
        bar.step();
        SpellEntry const* entry = sSpellStore.LookupEntry(spell);

        if (!entry)
            continue;

        for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if (entry->Effect[i] == SPELL_EFFECT_SKILL)
            {
                SpellLearnSkillNode dbc_node;
                dbc_node.skill    = entry->EffectMiscValue[i];
                dbc_node.step     = entry->CalculateSimpleValue(SpellEffectIndex(i));
                if (dbc_node.skill != SKILL_RIDING)
                    dbc_node.value = 1;
                else
                    dbc_node.value = dbc_node.step * 75;
                dbc_node.maxvalue = dbc_node.step * 75;

                mSpellLearnSkills[spell] = dbc_node;
                ++dbc_count;
                break;
            }
        }
    }

    sLog.outString();
    sLog.outString(">> Loaded %u Spell Learn Skills from DBC", dbc_count);
}

void SpellMgr::LoadSpellLearnSpells()
{
    mSpellLearnSpells.clear();                              // need for reload case

    //                                                0      1        2
    QueryResult *result = WorldDatabase.Query("SELECT entry, SpellID, Active FROM spell_learn_spell");
    if (!result)
    {
        BarGoLink bar(1);
        bar.step();

        sLog.outString();
        sLog.outString(">> Loaded 0 spell learn spells");
        sLog.outErrorDb("`spell_learn_spell` table is empty!");
        return;
    }

    uint32 count = 0;

    BarGoLink bar(result->GetRowCount());
    do
    {
        bar.step();
        Field *fields = result->Fetch();

        uint32 spell_id    = fields[0].GetUInt32();

        SpellLearnSpellNode node;
        node.spell      = fields[1].GetUInt32();
        node.active     = fields[2].GetBool();
        node.autoLearned= false;

        if (!sSpellStore.LookupEntry(spell_id))
        {
            sLog.outErrorDb("Spell %u listed in `spell_learn_spell` does not exist",spell_id);
            continue;
        }

        if (!sSpellStore.LookupEntry(node.spell))
        {
            sLog.outErrorDb("Spell %u listed in `spell_learn_spell` learning nonexistent spell %u",spell_id,node.spell);
            continue;
        }

        if (GetTalentSpellCost(node.spell))
        {
            sLog.outErrorDb("Spell %u listed in `spell_learn_spell` attempt learning talent spell %u, skipped",spell_id,node.spell);
            continue;
        }

        mSpellLearnSpells.insert(SpellLearnSpellMap::value_type(spell_id,node));

        ++count;
    } while (result->NextRow());

    delete result;

    // search auto-learned spells and add its to map also for use in unlearn spells/talents
    uint32 dbc_count = 0;
    for(uint32 spell = 0; spell < sSpellStore.GetNumRows(); ++spell)
    {
        SpellEntry const* entry = sSpellStore.LookupEntry(spell);

        if (!entry)
            continue;

        for(int i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if(entry->Effect[i]==SPELL_EFFECT_LEARN_SPELL)
            {
                SpellLearnSpellNode dbc_node;
                dbc_node.spell       = entry->EffectTriggerSpell[i];
                dbc_node.active      = true;                // all dbc based learned spells is active (show in spell book or hide by client itself)

                // ignore learning nonexistent spells (broken/outdated/or generic learning spell 483
                if (!sSpellStore.LookupEntry(dbc_node.spell))
                    continue;

                // talent or passive spells or skill-step spells auto-casted and not need dependent learning,
                // pet teaching spells don't must be dependent learning (casted)
                // other required explicit dependent learning
                dbc_node.autoLearned = entry->EffectImplicitTargetA[i]==TARGET_PET || GetTalentSpellCost(spell) > 0 || IsPassiveSpell(entry) || IsSpellHaveEffect(entry,SPELL_EFFECT_SKILL_STEP);

                SpellLearnSpellMapBounds db_node_bounds = GetSpellLearnSpellMapBounds(spell);

                bool found = false;
                for(SpellLearnSpellMap::const_iterator itr = db_node_bounds.first; itr != db_node_bounds.second; ++itr)
                {
                    if (itr->second.spell == dbc_node.spell)
                    {
                        sLog.outErrorDb("Spell %u auto-learn spell %u in spell.dbc then the record in `spell_learn_spell` is redundant, please fix DB.",
                            spell,dbc_node.spell);
                        found = true;
                        break;
                    }
                }

                if (!found)                                 // add new spell-spell pair if not found
                {
                    mSpellLearnSpells.insert(SpellLearnSpellMap::value_type(spell,dbc_node));
                    ++dbc_count;
                }
            }
        }
    }

    sLog.outString();
    sLog.outString( ">> Loaded %u spell learn spells + %u found in DBC", count, dbc_count );
}

void SpellMgr::LoadSpellScriptTarget()
{
    mSpellScriptTarget.clear();                             // need for reload case

    uint32 count = 0;

    QueryResult *result = WorldDatabase.Query("SELECT entry,type,targetEntry FROM spell_script_target");

    if (!result)
    {
        BarGoLink bar(1);

        bar.step();

        sLog.outString();
        sLog.outErrorDb(">> Loaded 0 SpellScriptTarget. DB table `spell_script_target` is empty.");
        return;
    }

    BarGoLink bar(result->GetRowCount());

    do
    {
        Field *fields = result->Fetch();
        bar.step();

        uint32 spellId     = fields[0].GetUInt32();
        uint32 type        = fields[1].GetUInt32();
        uint32 targetEntry = fields[2].GetUInt32();

        SpellEntry const* spellProto = sSpellStore.LookupEntry(spellId);

        if (!spellProto)
        {
            sLog.outErrorDb("Table `spell_script_target`: spellId %u listed for TargetEntry %u does not exist.",spellId,targetEntry);
            continue;
        }

        bool targetfound = false;
        for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if( spellProto->EffectImplicitTargetA[i] == TARGET_SCRIPT ||
                spellProto->EffectImplicitTargetB[i] == TARGET_SCRIPT ||
                spellProto->EffectImplicitTargetA[i] == TARGET_SCRIPT_COORDINATES ||
                spellProto->EffectImplicitTargetB[i] == TARGET_SCRIPT_COORDINATES ||
                spellProto->EffectImplicitTargetA[i] == TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT ||
                spellProto->EffectImplicitTargetB[i] == TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT ||
                spellProto->EffectImplicitTargetA[i] == TARGET_AREAEFFECT_INSTANT ||
                spellProto->EffectImplicitTargetB[i] == TARGET_AREAEFFECT_INSTANT ||
                spellProto->EffectImplicitTargetA[i] == TARGET_AREAEFFECT_CUSTOM ||
                spellProto->EffectImplicitTargetB[i] == TARGET_AREAEFFECT_CUSTOM ||
                spellProto->EffectImplicitTargetA[i] == TARGET_AREAEFFECT_GO_AROUND_DEST ||
                spellProto->EffectImplicitTargetB[i] == TARGET_AREAEFFECT_GO_AROUND_DEST)
            {
                targetfound = true;
                break;
            }
        }
        if (!targetfound)
        {
            sLog.outErrorDb("Table `spell_script_target`: spellId %u listed for TargetEntry %u does not have any implicit target TARGET_SCRIPT(38) or TARGET_SCRIPT_COORDINATES (46) or TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT (40).", spellId, targetEntry);
            continue;
        }

        if (type >= MAX_SPELL_TARGET_TYPE)
        {
            sLog.outErrorDb("Table `spell_script_target`: target type %u for TargetEntry %u is incorrect.",type,targetEntry);
            continue;
        }

        // Checks by target type
        switch (type)
        {
            case SPELL_TARGET_TYPE_GAMEOBJECT:
            {
                if (!targetEntry)
                    break;

                if (!sGOStorage.LookupEntry<GameObjectInfo>(targetEntry))
                {
                    sLog.outErrorDb("Table `spell_script_target`: gameobject template entry %u does not exist.",targetEntry);
                    continue;
                }
                break;
            }
            default:
                if (!targetEntry)
                {
                    sLog.outErrorDb("Table `spell_script_target`: target entry == 0 for not GO target type (%u).",type);
                    continue;
                }
                if (const CreatureInfo* cInfo = sCreatureStorage.LookupEntry<CreatureInfo>(targetEntry))
                {
                    if (spellId == 30427 && !cInfo->SkinLootId)
                    {
                        sLog.outErrorDb("Table `spell_script_target` has creature %u as a target of spellid 30427, but this creature has no skinlootid. Gas extraction will not work!", cInfo->Entry);
                        continue;
                    }
                }
                else
                {
                    sLog.outErrorDb("Table `spell_script_target`: creature template entry %u does not exist.",targetEntry);
                    continue;
                }
                break;
        }

        mSpellScriptTarget.insert(SpellScriptTarget::value_type(spellId,SpellTargetEntry(SpellTargetType(type),targetEntry)));

        ++count;
    } while (result->NextRow());

    delete result;

    // Check all spells
    /* Disabled (lot errors at this moment)
    for(uint32 i = 1; i < sSpellStore.nCount; ++i)
    {
        SpellEntry const * spellInfo = sSpellStore.LookupEntry(i);
        if(!spellInfo)
            continue;

        bool found = false;
        for(int j = 0; j < MAX_EFFECT_INDEX; ++j)
        {
            if( spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT || spellInfo->EffectImplicitTargetA[j] != TARGET_SELF && spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT )
            {
                SpellScriptTarget::const_iterator lower = GetBeginSpellScriptTarget(spellInfo->Id);
                SpellScriptTarget::const_iterator upper = GetEndSpellScriptTarget(spellInfo->Id);
                if(lower==upper)
                {
                    sLog.outErrorDb("Spell (ID: %u) has effect EffectImplicitTargetA/EffectImplicitTargetB = %u (TARGET_SCRIPT), but does not have record in `spell_script_target`",spellInfo->Id,TARGET_SCRIPT);
                    break;                                  // effects of spell
                }
            }
        }
    }
    */

    sLog.outString();
    sLog.outString(">> Loaded %u Spell Script Targets", count);
}

void SpellMgr::LoadSpellPetAuras()
{
    mSpellPetAuraMap.clear();                                  // need for reload case

    uint32 count = 0;

    //                                                0      1    2
    QueryResult *result = WorldDatabase.Query("SELECT spell, pet, aura FROM spell_pet_auras");
    if (!result)
    {

        BarGoLink bar(1);

        bar.step();

        sLog.outString();
        sLog.outString(">> Loaded %u spell pet auras", count);
        return;
    }

    BarGoLink bar(result->GetRowCount());

    do
    {
        Field *fields = result->Fetch();

        bar.step();

        uint32 spell = fields[0].GetUInt32();
        uint32 pet = fields[1].GetUInt32();
        uint32 aura = fields[2].GetUInt32();

        SpellPetAuraMap::iterator itr = mSpellPetAuraMap.find(spell);
        if(itr != mSpellPetAuraMap.end())
        {
            itr->second.AddAura(pet, aura);
        }
        else
        {
            SpellEntry const* spellInfo = sSpellStore.LookupEntry(spell);
            if (!spellInfo)
            {
                sLog.outErrorDb("Spell %u listed in `spell_pet_auras` does not exist", spell);
                continue;
            }
            int i = 0;
            for(; i < MAX_EFFECT_INDEX; ++i)
                if((spellInfo->Effect[i] == SPELL_EFFECT_APPLY_AURA &&
                    spellInfo->EffectApplyAuraName[i] == SPELL_AURA_DUMMY) ||
                    spellInfo->Effect[i] == SPELL_EFFECT_DUMMY)
                    break;

            if(i == MAX_EFFECT_INDEX)
            {
                sLog.outError("Spell %u listed in `spell_pet_auras` does not have dummy aura or dummy effect", spell);
                continue;
            }

            SpellEntry const* spellInfo2 = sSpellStore.LookupEntry(aura);
            if (!spellInfo2)
            {
                sLog.outErrorDb("Aura %u listed in `spell_pet_auras` does not exist", aura);
                continue;
            }

            PetAura pa(pet, aura, spellInfo->EffectImplicitTargetA[i] == TARGET_PET, spellInfo->CalculateSimpleValue(SpellEffectIndex(i)));
            mSpellPetAuraMap[spell] = pa;
        }

        ++count;
    } while( result->NextRow() );

    delete result;

    sLog.outString();
    sLog.outString( ">> Loaded %u spell pet auras", count );
}

/// Some checks for spells, to prevent adding deprecated/broken spells for trainers, spell book, etc
bool SpellMgr::IsSpellValid(SpellEntry const* spellInfo, Player* pl, bool msg)
{
    // not exist
    if(!spellInfo)
        return false;

    bool need_check_reagents = false;

    // check effects
    for(int i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        switch(spellInfo->Effect[i])
        {
            case 0:
                continue;

            // craft spell for crafting nonexistent item (break client recipes list show)
            case SPELL_EFFECT_CREATE_ITEM:
            {
                if(!ObjectMgr::GetItemPrototype( spellInfo->EffectItemType[i] ))
                {
                    if(msg)
                    {
                        if(pl)
                            ChatHandler(pl).PSendSysMessage("Craft spell %u create item (Entry: %u) but item does not exist in item_template.",spellInfo->Id,spellInfo->EffectItemType[i]);
                        else
                            sLog.outErrorDb("Craft spell %u create item (Entry: %u) but item does not exist in item_template.",spellInfo->Id,spellInfo->EffectItemType[i]);
                    }
                    return false;
                }

                need_check_reagents = true;
                break;
            }
            case SPELL_EFFECT_LEARN_SPELL:
            {
                SpellEntry const* spellInfo2 = sSpellStore.LookupEntry(spellInfo->EffectTriggerSpell[i]);
                if( !IsSpellValid(spellInfo2,pl,msg) )
                {
                    if(msg)
                    {
                        if(pl)
                            ChatHandler(pl).PSendSysMessage("Spell %u learn to broken spell %u, and then...",spellInfo->Id,spellInfo->EffectTriggerSpell[i]);
                        else
                            sLog.outErrorDb("Spell %u learn to invalid spell %u, and then...",spellInfo->Id,spellInfo->EffectTriggerSpell[i]);
                    }
                    return false;
                }
                break;
            }
        }
    }

    if(need_check_reagents)
    {
        for(int j = 0; j < MAX_SPELL_REAGENTS; ++j)
        {
            if(spellInfo->Reagent[j] > 0 && !ObjectMgr::GetItemPrototype( spellInfo->Reagent[j] ))
            {
                if(msg)
                {
                    if(pl)
                        ChatHandler(pl).PSendSysMessage("Craft spell %u requires reagent item (Entry: %u) but item does not exist in item_template.",spellInfo->Id,spellInfo->Reagent[j]);
                    else
                        sLog.outErrorDb("Craft spell %u requires reagent item (Entry: %u) but item does not exist in item_template.",spellInfo->Id,spellInfo->Reagent[j]);
                }
                return false;
            }
        }
    }

    return true;
}

void SpellMgr::LoadSpellAreas()
{
    mSpellAreaMap.clear();                                  // need for reload case
    mSpellAreaForQuestMap.clear();
    mSpellAreaForActiveQuestMap.clear();
    mSpellAreaForQuestEndMap.clear();
    mSpellAreaForAuraMap.clear();

    uint32 count = 0;

    //                                                0      1     2            3                   4          5           6         7       8
    QueryResult *result = WorldDatabase.Query("SELECT spell, area, quest_start, quest_start_active, quest_end, aura_spell, racemask, gender, autocast FROM spell_area");

    if (!result)
    {
        BarGoLink bar(1);

        bar.step();

        sLog.outString();
        sLog.outString(">> Loaded %u spell area requirements", count);
        return;
    }

    BarGoLink bar(result->GetRowCount());

    do
    {
        Field *fields = result->Fetch();

        bar.step();

        uint32 spell = fields[0].GetUInt32();
        SpellArea spellArea;
        spellArea.spellId             = spell;
        spellArea.areaId              = fields[1].GetUInt32();
        spellArea.questStart          = fields[2].GetUInt32();
        spellArea.questStartCanActive = fields[3].GetBool();
        spellArea.questEnd            = fields[4].GetUInt32();
        spellArea.auraSpell           = fields[5].GetInt32();
        spellArea.raceMask            = fields[6].GetUInt32();
        spellArea.gender              = Gender(fields[7].GetUInt8());
        spellArea.autocast            = fields[8].GetBool();

        if (!sSpellStore.LookupEntry(spell))
        {
            sLog.outErrorDb("Spell %u listed in `spell_area` does not exist", spell);
            continue;
        }

        {
            bool ok = true;
            SpellAreaMapBounds sa_bounds = GetSpellAreaMapBounds(spellArea.spellId);
            for (SpellAreaMap::const_iterator itr = sa_bounds.first; itr != sa_bounds.second; ++itr)
            {
                if (spellArea.spellId != itr->second.spellId)
                    continue;
                if (spellArea.areaId != itr->second.areaId)
                    continue;
                if (spellArea.questStart != itr->second.questStart)
                    continue;
                if (spellArea.auraSpell != itr->second.auraSpell)
                    continue;
                if ((spellArea.raceMask & itr->second.raceMask) == 0)
                    continue;
                if (spellArea.gender != itr->second.gender)
                    continue;

                // duplicate by requirements
                ok =false;
                break;
            }

            if (!ok)
            {
                sLog.outErrorDb("Spell %u listed in `spell_area` already listed with similar requirements.", spell);
                continue;
            }

        }

        if (spellArea.areaId && !GetAreaEntryByAreaID(spellArea.areaId))
        {
            sLog.outErrorDb("Spell %u listed in `spell_area` have wrong area (%u) requirement", spell,spellArea.areaId);
            continue;
        }

        if (spellArea.questStart && !sObjectMgr.GetQuestTemplate(spellArea.questStart))
        {
            sLog.outErrorDb("Spell %u listed in `spell_area` have wrong start quest (%u) requirement", spell,spellArea.questStart);
            continue;
        }

        if (spellArea.questEnd)
        {
            if (!sObjectMgr.GetQuestTemplate(spellArea.questEnd))
            {
                sLog.outErrorDb("Spell %u listed in `spell_area` have wrong end quest (%u) requirement", spell,spellArea.questEnd);
                continue;
            }

            if (spellArea.questEnd==spellArea.questStart && !spellArea.questStartCanActive)
            {
                sLog.outErrorDb("Spell %u listed in `spell_area` have quest (%u) requirement for start and end in same time", spell,spellArea.questEnd);
                continue;
            }
        }

        if (spellArea.auraSpell)
        {
            SpellEntry const* spellInfo = sSpellStore.LookupEntry(abs(spellArea.auraSpell));
            if (!spellInfo)
            {
                sLog.outErrorDb("Spell %u listed in `spell_area` have wrong aura spell (%u) requirement", spell,abs(spellArea.auraSpell));
                continue;
            }

            switch (spellInfo->EffectApplyAuraName[EFFECT_INDEX_0])
            {
                case SPELL_AURA_DUMMY:
                case SPELL_AURA_GHOST:
                    break;
                default:
                    sLog.outErrorDb("Spell %u listed in `spell_area` have aura spell requirement (%u) without dummy/phase/ghost aura in effect 0", spell,abs(spellArea.auraSpell));
                    continue;
            }

            if (uint32(abs(spellArea.auraSpell))==spellArea.spellId)
            {
                sLog.outErrorDb("Spell %u listed in `spell_area` have aura spell (%u) requirement for itself", spell, abs(spellArea.auraSpell));
                continue;
            }

            // not allow autocast chains by auraSpell field (but allow use as alternative if not present)
            if (spellArea.autocast && spellArea.auraSpell > 0)
            {
                bool chain = false;
                SpellAreaForAuraMapBounds saBound = GetSpellAreaForAuraMapBounds(spellArea.spellId);
                for (SpellAreaForAuraMap::const_iterator itr = saBound.first; itr != saBound.second; ++itr)
                {
                    if (itr->second->autocast && itr->second->auraSpell > 0)
                    {
                        chain = true;
                        break;
                    }
                }

                if (chain)
                {
                    sLog.outErrorDb("Spell %u listed in `spell_area` have aura spell (%u) requirement that itself autocast from aura", spell,spellArea.auraSpell);
                    continue;
                }

                SpellAreaMapBounds saBound2 = GetSpellAreaMapBounds(spellArea.auraSpell);
                for (SpellAreaMap::const_iterator itr2 = saBound2.first; itr2 != saBound2.second; ++itr2)
                {
                    if (itr2->second.autocast && itr2->second.auraSpell > 0)
                    {
                        chain = true;
                        break;
                    }
                }

                if (chain)
                {
                    sLog.outErrorDb("Spell %u listed in `spell_area` have aura spell (%u) requirement that itself autocast from aura", spell,spellArea.auraSpell);
                    continue;
                }
            }
        }

        if (spellArea.raceMask && (spellArea.raceMask & RACEMASK_ALL_PLAYABLE)==0)
        {
            sLog.outErrorDb("Spell %u listed in `spell_area` have wrong race mask (%u) requirement", spell,spellArea.raceMask);
            continue;
        }

        if (spellArea.gender!=GENDER_NONE && spellArea.gender!=GENDER_FEMALE && spellArea.gender!=GENDER_MALE)
        {
            sLog.outErrorDb("Spell %u listed in `spell_area` have wrong gender (%u) requirement", spell,spellArea.gender);
            continue;
        }

        SpellArea const* sa = &mSpellAreaMap.insert(SpellAreaMap::value_type(spell,spellArea))->second;

        // for search by current zone/subzone at zone/subzone change
        if (spellArea.areaId)
            mSpellAreaForAreaMap.insert(SpellAreaForAreaMap::value_type(spellArea.areaId,sa));

        // for search at quest start/reward
        if (spellArea.questStart)
        {
            if (spellArea.questStartCanActive)
                mSpellAreaForActiveQuestMap.insert(SpellAreaForQuestMap::value_type(spellArea.questStart,sa));
            else
                mSpellAreaForQuestMap.insert(SpellAreaForQuestMap::value_type(spellArea.questStart,sa));
        }

        // for search at quest start/reward
        if (spellArea.questEnd)
            mSpellAreaForQuestEndMap.insert(SpellAreaForQuestMap::value_type(spellArea.questEnd,sa));

        // for search at aura apply
        if (spellArea.auraSpell)
            mSpellAreaForAuraMap.insert(SpellAreaForAuraMap::value_type(abs(spellArea.auraSpell),sa));

        ++count;
    } while (result->NextRow());

    delete result;

    sLog.outString();
    sLog.outString(">> Loaded %u spell area requirements", count);
}

SpellCastResult SpellMgr::GetSpellAllowedInLocationError(SpellEntry const *spellInfo, uint32 map_id, uint32 zone_id, uint32 area_id, Player const* player)
{
    // DB base check (if non empty then must fit at least single for allow)
    SpellAreaMapBounds saBounds = GetSpellAreaMapBounds(spellInfo->Id);
    if (saBounds.first != saBounds.second)
    {
        for(SpellAreaMap::const_iterator itr = saBounds.first; itr != saBounds.second; ++itr)
        {
            if(itr->second.IsFitToRequirements(player,zone_id,area_id))
                return SPELL_CAST_OK;
        }
        return SPELL_FAILED_REQUIRES_AREA;
    }

    // bg spell checks

    // Spell casted only on battleground
    if ((spellInfo->AttributesEx3 & SPELL_ATTR_EX3_BATTLEGROUND))
        if (!player || !player->InBattleGround())
            return SPELL_FAILED_ONLY_BATTLEGROUNDS;

    switch(spellInfo->Id)
    {
        // a trinket in alterac valley allows to teleport to the boss
        case 22564:                                         // recall
        case 22563:                                         // recall
		case 23696:											// Alterac Heavy Runecloth Bandage
        {
            if (!player)
                return SPELL_FAILED_REQUIRES_AREA;
            BattleGround* bg = player->GetBattleGround();
            return map_id == 30 && bg
                && bg->GetStatus() != STATUS_WAIT_JOIN ? SPELL_CAST_OK : SPELL_FAILED_REQUIRES_AREA;
        }
        case 23333:                                         // Warsong Flag
        case 23335:                                         // Silverwing Flag
            return map_id == 489 && player && player->InBattleGround() ? SPELL_CAST_OK : SPELL_FAILED_REQUIRES_AREA;
        case 2584:                                          // Waiting to Resurrect
        {
            return player && player->InBattleGround() ? SPELL_CAST_OK : SPELL_FAILED_ONLY_BATTLEGROUNDS;
        }
        case 22011:                                         // Spirit Heal Channel
        case 22012:                                         // Spirit Heal
        case 24171:                                         // Resurrection Impact Visual
        {
            MapEntry const* mapEntry = sMapStore.LookupEntry(map_id);
            if (!mapEntry)
                return SPELL_FAILED_REQUIRES_AREA;
            return mapEntry->IsBattleGround()? SPELL_CAST_OK : SPELL_FAILED_ONLY_BATTLEGROUNDS;
        }
    }

    return SPELL_CAST_OK;
}

void SpellMgr::LoadSkillLineAbilityMap()
{
    mSkillLineAbilityMap.clear();

    BarGoLink bar(sSkillLineAbilityStore.GetNumRows());
    uint32 count = 0;

    for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
    {
        bar.step();
        SkillLineAbilityEntry const *SkillInfo = sSkillLineAbilityStore.LookupEntry(i);
        if(!SkillInfo)
            continue;

        mSkillLineAbilityMap.insert(SkillLineAbilityMap::value_type(SkillInfo->spellId,SkillInfo));
        ++count;
    }

    sLog.outString();
    sLog.outString(">> Loaded %u SkillLineAbility MultiMap Data", count);
}

void SpellMgr::LoadSkillRaceClassInfoMap()
{
    mSkillRaceClassInfoMap.clear();

    BarGoLink bar(sSkillRaceClassInfoStore.GetNumRows());
    uint32 count = 0;

    for (uint32 i = 0; i < sSkillRaceClassInfoStore.GetNumRows(); ++i)
    {
        bar.step();
        SkillRaceClassInfoEntry const *skillRCInfo = sSkillRaceClassInfoStore.LookupEntry(i);
        if (!skillRCInfo)
            continue;

        // not all skills really listed in ability skills list
        if (!sSkillLineStore.LookupEntry(skillRCInfo->skillId))
            continue;

        mSkillRaceClassInfoMap.insert(SkillRaceClassInfoMap::value_type(skillRCInfo->skillId,skillRCInfo));

        ++count;
    }

    sLog.outString();
    sLog.outString(">> Loaded %u SkillRaceClassInfo MultiMap Data", count);
}

void SpellMgr::CheckUsedSpells(char const* table)
{
    uint32 countSpells = 0;
    uint32 countMasks = 0;

    //                                                 0       1               2               3         4           5             6          7          8         9    10
    QueryResult *result = WorldDatabase.PQuery("SELECT spellid,SpellFamilyName,SpellFamilyMask,SpellIcon,SpellVisual,SpellCategory,EffectType,EffectAura,EffectIdx,Name,Code FROM %s",table);

    if (!result)
    {
        BarGoLink bar(1);

        bar.step();

        sLog.outString();
        sLog.outErrorDb("`%s` table is empty!",table);
        return;
    }

    BarGoLink bar(result->GetRowCount());

    do
    {
        Field *fields = result->Fetch();

        bar.step();

        uint32 spell       = fields[0].GetUInt32();
        int32  family      = fields[1].GetInt32();
        uint64 familyMask  = fields[2].GetUInt64();
        int32  spellIcon   = fields[3].GetInt32();
        int32  spellVisual = fields[4].GetInt32();
        int32  category    = fields[5].GetInt32();
        int32  effectType  = fields[6].GetInt32();
        int32  auraType    = fields[7].GetInt32();
        int32  effectIdx   = fields[8].GetInt32();
        std::string name   = fields[9].GetCppString();
        std::string code   = fields[10].GetCppString();

        // checks of correctness requirements itself

        if (family < -1 || family > SPELLFAMILY_UNK3)
        {
            sLog.outError("Table '%s' for spell %u have wrong SpellFamily value(%u), skipped.",table,spell,family);
            continue;
        }

        // TODO: spellIcon check need dbc loading
        if (spellIcon < -1)
        {
            sLog.outError("Table '%s' for spell %u have wrong SpellIcon value(%u), skipped.",table,spell,spellIcon);
            continue;
        }

        // TODO: spellVisual check need dbc loading
        if (spellVisual < -1)
        {
            sLog.outError("Table '%s' for spell %u have wrong SpellVisual value(%u), skipped.",table,spell,spellVisual);
            continue;
        }

        // TODO: for spellCategory better check need dbc loading
        if (category < -1 || (category >=0 && sSpellCategoryStore.find(category) == sSpellCategoryStore.end()))
        {
            sLog.outError("Table '%s' for spell %u have wrong SpellCategory value(%u), skipped.",table,spell,category);
            continue;
        }

        if (effectType < -1 || effectType >= TOTAL_SPELL_EFFECTS)
        {
            sLog.outError("Table '%s' for spell %u have wrong SpellEffect type value(%u), skipped.",table,spell,effectType);
            continue;
        }

        if (auraType < -1 || auraType >= TOTAL_AURAS)
        {
            sLog.outError("Table '%s' for spell %u have wrong SpellAura type value(%u), skipped.",table,spell,auraType);
            continue;
        }

        if (effectIdx < -1 || effectIdx >= 3)
        {
            sLog.outError("Table '%s' for spell %u have wrong EffectIdx value(%u), skipped.",table,spell,effectIdx);
            continue;
        }

        // now checks of requirements

        if(spell)
        {
            ++countSpells;

            SpellEntry const* spellEntry = sSpellStore.LookupEntry(spell);
            if(!spellEntry)
            {
                sLog.outError("Spell %u '%s' not exist but used in %s.",spell,name.c_str(),code.c_str());
                continue;
            }

            if (family >= 0 && spellEntry->SpellFamilyName != uint32(family))
            {
                sLog.outError("Spell %u '%s' family(%u) <> %u but used in %s.",spell,name.c_str(),spellEntry->SpellFamilyName,family,code.c_str());
                continue;
            }

            if(familyMask != UI64LIT(0xFFFFFFFFFFFFFFFF))
            {
                if(familyMask == UI64LIT(0x0000000000000000))
                {
                    if (spellEntry->SpellFamilyFlags)
                    {
                        sLog.outError("Spell %u '%s' not fit to (" UI64FMTD ") but used in %s.",
                            spell, name.c_str(), familyMask, code.c_str());
                        continue;
                    }

                }
                else
                {
                    if (!spellEntry->IsFitToFamilyMask(familyMask))
                    {
                        sLog.outError("Spell %u '%s' not fit to (" I64FMT ") but used in %s.",spell,name.c_str(),familyMask,code.c_str());
                        continue;
                    }

                }
            }

            if (spellIcon >= 0 && spellEntry->SpellIconID != uint32(spellIcon))
            {
                sLog.outError("Spell %u '%s' icon(%u) <> %u but used in %s.",spell,name.c_str(),spellEntry->SpellIconID,spellIcon,code.c_str());
                continue;
            }

            if (spellVisual >= 0 && spellEntry->SpellVisual != uint32(spellVisual))
            {
                sLog.outError("Spell %u '%s' visual(%u) <> %u but used in %s.",spell,name.c_str(),spellEntry->SpellVisual,spellVisual,code.c_str());
                continue;
            }

            if (category >= 0 && spellEntry->Category != uint32(category))
            {
                sLog.outError("Spell %u '%s' category(%u) <> %u but used in %s.",spell,name.c_str(),spellEntry->Category,category,code.c_str());
                continue;
            }

            if (effectIdx >= EFFECT_INDEX_0)
            {
                if (effectType >= 0 && spellEntry->Effect[effectIdx] != uint32(effectType))
                {
                    sLog.outError("Spell %u '%s' effect%d <> %u but used in %s.",spell,name.c_str(),effectIdx+1,effectType,code.c_str());
                    continue;
                }

                if (auraType >= 0 && spellEntry->EffectApplyAuraName[effectIdx] != uint32(auraType))
                {
                    sLog.outError("Spell %u '%s' aura%d <> %u but used in %s.",spell,name.c_str(),effectIdx+1,auraType,code.c_str());
                    continue;
                }

            }
            else
            {
                if (effectType >= 0 && !IsSpellHaveEffect(spellEntry,SpellEffects(effectType)))
                {
                    sLog.outError("Spell %u '%s' not have effect %u but used in %s.",spell,name.c_str(),effectType,code.c_str());
                    continue;
                }

                if (auraType >= 0 && !IsSpellHaveAura(spellEntry, AuraType(auraType)))
                {
                    sLog.outError("Spell %u '%s' not have aura %u but used in %s.",spell,name.c_str(),auraType,code.c_str());
                    continue;
                }
            }
        }
        else
        {
            ++countMasks;

            bool found = false;
            for(uint32 spellId = 1; spellId < sSpellStore.GetNumRows(); ++spellId)
            {
                SpellEntry const* spellEntry = sSpellStore.LookupEntry(spellId);
                if (!spellEntry)
                    continue;

                if (family >=0 && spellEntry->SpellFamilyName != uint32(family))
                    continue;

                if (familyMask != UI64LIT(0xFFFFFFFFFFFFFFFF))
                {
                    if(familyMask == UI64LIT(0x0000000000000000))
                    {
                        if (spellEntry->SpellFamilyFlags)
                            continue;
                    }
                    else
                    {
                        if (!spellEntry->IsFitToFamilyMask(familyMask))
                            continue;
                    }
                }

                if (spellIcon >= 0 && spellEntry->SpellIconID != uint32(spellIcon))
                    continue;

                if (spellVisual >= 0 && spellEntry->SpellVisual != uint32(spellVisual))
                    continue;

                if (category >= 0 && spellEntry->Category != uint32(category))
                    continue;

                if (effectIdx >= 0)
                {
                    if (effectType >=0 && spellEntry->Effect[effectIdx] != uint32(effectType))
                        continue;

                    if (auraType >=0 && spellEntry->EffectApplyAuraName[effectIdx] != uint32(auraType))
                        continue;
                }
                else
                {
                    if (effectType >=0 && !IsSpellHaveEffect(spellEntry,SpellEffects(effectType)))
                        continue;

                    if (auraType >=0 && !IsSpellHaveAura(spellEntry,AuraType(auraType)))
                        continue;
                }

                found = true;
                break;
            }

            if (!found)
            {
                if (effectIdx >= 0)
                    sLog.outError("Spells '%s' not found for family %i (" I64FMT ") icon(%i) visual(%i) category(%i) effect%d(%i) aura%d(%i) but used in %s",
                        name.c_str(),family,familyMask,spellIcon,spellVisual,category,effectIdx+1,effectType,effectIdx+1,auraType,code.c_str());
                else
                    sLog.outError("Spells '%s' not found for family %i (" I64FMT ") icon(%i) visual(%i) category(%i) effect(%i) aura(%i) but used in %s",
                        name.c_str(),family,familyMask,spellIcon,spellVisual,category,effectType,auraType,code.c_str());
                continue;
            }
        }

    } while( result->NextRow() );

    delete result;

    sLog.outString();
    sLog.outString( ">> Checked %u spells and %u spell masks", countSpells, countMasks );
}

DiminishingGroup GetDiminishingReturnsGroupForSpell(SpellEntry const* spellproto, bool triggered, uint32 casting_item_entry)
{
    // Explicit Diminishing Groups
    switch(spellproto->SpellFamilyName)
    {
        case SPELLFAMILY_ROGUE:
        {
            //Cheap Shot
            if (spellproto->SpellFamilyFlags & UI64LIT(0x00000000400))
                return DIMINISHING_OPENER_STUN;
            break;
        }
        case SPELLFAMILY_HUNTER:
        {
            // Freezing trap
            if (spellproto->SpellFamilyFlags & UI64LIT(0x00000000008))
                return DIMINISHING_INCAPACITATE;
            // Scattershot
            if (spellproto->SpellFamilyFlags & UI64LIT(0x00000040000))
                return DIMINISHING_SCATTER;
            break;
        }
        case SPELLFAMILY_WARLOCK:
        {
            // Seduction
            if (spellproto->SpellFamilyFlags & UI64LIT(0x0000000040000000) && spellproto->Mechanic==MECHANIC_CHARM)
                return DIMINISHING_FEAR;
            break;
        }
        case SPELLFAMILY_DRUID:
        {
            //Pounce
            if (spellproto->SpellFamilyFlags & UI64LIT(0x00000020000))
                return DIMINISHING_OPENER_STUN;

			// Hibernate
			if (spellproto->Id == 2637 || spellproto->Id == 18657 || spellproto->Id == 18658)
				return DIMINISHING_INCAPACITATE;
            break;
        }
        case SPELLFAMILY_SHAMAN:
        {   // Frost shock should have diminishing
            if (spellproto->Id == 8056 || spellproto->Id == 8058 || spellproto->Id == 10472 || spellproto->Id == 10473)
                return DIMINISHING_CONTROL_ROOT;
            break;
        }
        case SPELLFAMILY_GENERIC:
        {
            //All bomb/grenade type items should share the controlled stun DR - most of them are already stuns
            switch(spellproto->Id)
            {
                case 5134:          //Flash bomb - 5134
                case 19821:         // Arcane Bomb - 19821
                {
                    return DIMINISHING_CONTROL_STUN;
                }
                case 23454:         //Stun from The Unstoppable Force doesn't DR
                {
                    return DIMINISHING_NONE;
                }
                default:
                    break;            
            }
            switch(casting_item_entry)
            {
                case 12796:        // Stun from Hammer of the Titans.
                {
                    return DIMINISHING_TRIGGER_STUN;
                }
                default:
                    break;
            }
        }
        default:
            break;
    }

    // Get by mechanic
    uint32 mechanic = GetAllSpellMechanicMask(spellproto);
    if (!mechanic)
        return DIMINISHING_NONE;

    if (mechanic & (1<<(MECHANIC_STUN-1)))
        return triggered ? DIMINISHING_TRIGGER_STUN : DIMINISHING_CONTROL_STUN;
    if (mechanic & ((1<<(MECHANIC_SLEEP-1))|(1<<(MECHANIC_DISORIENTED-1))))
        return DIMINISHING_DISORIENT;
    if (mechanic & (1<<(MECHANIC_POLYMORPH-1)))
        return DIMINISHING_POLYMORPH;
    if (mechanic & (1<<(MECHANIC_ROOT-1)))
        return triggered ? DIMINISHING_TRIGGER_ROOT : DIMINISHING_CONTROL_ROOT;
    if (mechanic & (1<<(MECHANIC_FEAR-1)))
        return DIMINISHING_FEAR;
    if (mechanic & (1<<(MECHANIC_CHARM-1)))
        return DIMINISHING_CHARM;
    if (mechanic & (1<<(MECHANIC_BANISH-1)))
        return DIMINISHING_BANISH;
    if (mechanic & ((1<<(MECHANIC_KNOCKOUT-1))|(1<<(MECHANIC_SAPPED-1))))
        return DIMINISHING_INCAPACITATE;

    return DIMINISHING_NONE;
}

bool IsDiminishingReturnsGroupDurationLimited(DiminishingGroup group)
{
    switch(group)
    {
        case DIMINISHING_CONTROL_STUN:
        case DIMINISHING_TRIGGER_STUN:
        case DIMINISHING_OPENER_STUN:
        case DIMINISHING_DISORIENT:
        case DIMINISHING_CONTROL_ROOT:
        case DIMINISHING_TRIGGER_ROOT:
        case DIMINISHING_FEAR:
        case DIMINISHING_CHARM:
        case DIMINISHING_POLYMORPH:
        case DIMINISHING_SCATTER:
        case DIMINISHING_INCAPACITATE:
        case DIMINISHING_BANISH:
            return true;
        default:
            return false;
    }
    return false;
}

DiminishingReturnsType GetDiminishingReturnsGroupType(DiminishingGroup group)
{
    switch(group)
    {
        case DIMINISHING_DISORIENT:
        case DIMINISHING_CONTROL_STUN:
        case DIMINISHING_TRIGGER_STUN:
        case DIMINISHING_OPENER_STUN:
            return DRTYPE_ALL;
        case DIMINISHING_CONTROL_ROOT:
        case DIMINISHING_TRIGGER_ROOT:
        case DIMINISHING_FEAR:
        case DIMINISHING_CHARM:
        case DIMINISHING_POLYMORPH:
        case DIMINISHING_SCATTER:
        case DIMINISHING_INCAPACITATE:
        case DIMINISHING_BANISH:
            return DRTYPE_PLAYER;
        default:
            break;
    }

    return DRTYPE_NONE;
}

bool SpellArea::IsFitToRequirements(Player const* player, uint32 newZone, uint32 newArea) const
{
    if(gender!=GENDER_NONE)
    {
        // not in expected gender
        if(!player || gender != player->getGender())
            return false;
    }

    if(raceMask)
    {
        // not in expected race
        if(!player || !(raceMask & player->getRaceMask()))
            return false;
    }

    if(areaId)
    {
        // not in expected zone
        if(newZone!=areaId && newArea!=areaId)
            return false;
    }

    if(questStart)
    {
        // not in expected required quest state
        if(!player || (!questStartCanActive || (!player->IsActiveQuest(questStart) && !player->GetQuestRewardStatus(questStart))))
            return false;
    }

    if(questEnd)
    {
        // not in expected forbidden quest state
        if(!player || player->GetQuestRewardStatus(questEnd))
            return false;
    }

    if(auraSpell)
    {
        // not have expected aura
        if(!player)
            return false;
        if(auraSpell > 0)
            // have expected aura
            return player->HasAura(auraSpell, EFFECT_INDEX_0);
        else
            // not have expected aura
            return !player->HasAura(-auraSpell, EFFECT_INDEX_0);
    }

    return true;
}

void SpellMgr::LoadSpellAffects()
{
    mSpellAffectMap.clear();                                // need for reload case

    uint32 count = 0;

    //                                                0      1         2
    QueryResult *result = WorldDatabase.Query("SELECT entry, effectId, SpellFamilyMask FROM spell_affect");
    if (!result)
    {

        BarGoLink bar(1);

        bar.step();

        sLog.outString();
        sLog.outString( ">> Loaded %u spell affect definitions", count );
        return;
    }

    BarGoLink bar(result->GetRowCount());

    do
    {
        Field *fields = result->Fetch();

        bar.step();

        uint32 entry = fields[0].GetUInt32();
        uint8 effectId = fields[1].GetUInt8();

        SpellEntry const* spellInfo = sSpellStore.LookupEntry(entry);

        if (!spellInfo)
        {
            sLog.outErrorDb("Spell %u listed in `spell_affect` does not exist", entry);
            continue;
        }

        if (effectId >= MAX_EFFECT_INDEX)
        {
            sLog.outErrorDb("Spell %u listed in `spell_affect` have invalid effect index (%u)", entry,effectId);
            continue;
        }

        if (spellInfo->Effect[effectId] != SPELL_EFFECT_APPLY_AURA || (
            spellInfo->EffectApplyAuraName[effectId] != SPELL_AURA_ADD_FLAT_MODIFIER &&
            spellInfo->EffectApplyAuraName[effectId] != SPELL_AURA_ADD_PCT_MODIFIER  &&
            spellInfo->EffectApplyAuraName[effectId] != SPELL_AURA_ADD_TARGET_TRIGGER &&
            spellInfo->EffectApplyAuraName[effectId] != SPELL_AURA_OVERRIDE_CLASS_SCRIPTS))
        {
            sLog.outErrorDb("Spell %u listed in `spell_affect` have not SPELL_AURA_ADD_FLAT_MODIFIER (%u) or SPELL_AURA_ADD_PCT_MODIFIER (%u) or SPELL_AURA_ADD_TARGET_TRIGGER (%u) or SPELL_AURA_OVERRIDE_CLASS_SCRIPTS (%u) for effect index (%u)", entry,SPELL_AURA_ADD_FLAT_MODIFIER,SPELL_AURA_ADD_PCT_MODIFIER,SPELL_AURA_ADD_TARGET_TRIGGER,SPELL_AURA_OVERRIDE_CLASS_SCRIPTS,effectId);
            continue;
        }

        uint64 spellAffectMask = fields[2].GetUInt64();

        // Spell.dbc have own data for low part of SpellFamilyMask
        if (spellInfo->EffectItemType[effectId])
        {
            if (static_cast<uint64>(spellInfo->EffectItemType[effectId]) == spellAffectMask)
            {
                sLog.outErrorDb("Spell %u listed in `spell_affect` have redundant (same with EffectItemType%d) data for effect index (%u) and not needed, skipped.", entry,effectId+1,effectId);
                continue;
            }
        }

        mSpellAffectMap.insert(SpellAffectMap::value_type((entry<<8) + effectId,spellAffectMask));

        ++count;
    } while( result->NextRow() );

    delete result;

    sLog.outString();
    sLog.outString( ">> Loaded %u spell affect definitions", count );

    for (uint32 id = 0; id < sSpellStore.GetNumRows(); ++id)
    {
        SpellEntry const* spellInfo = sSpellStore.LookupEntry(id);
        if (!spellInfo)
            continue;

        for (int effectId = 0; effectId < MAX_EFFECT_INDEX; ++effectId)
        {
            if (spellInfo->Effect[effectId] != SPELL_EFFECT_APPLY_AURA || (
                spellInfo->EffectApplyAuraName[effectId] != SPELL_AURA_ADD_FLAT_MODIFIER &&
                spellInfo->EffectApplyAuraName[effectId] != SPELL_AURA_ADD_PCT_MODIFIER  &&
                spellInfo->EffectApplyAuraName[effectId] != SPELL_AURA_ADD_TARGET_TRIGGER))
                continue;

            if (spellInfo->EffectItemType[effectId] != 0)
                continue;

            if (mSpellAffectMap.find((id<<8) + effectId) !=  mSpellAffectMap.end())
                continue;

            sLog.outErrorDb("Spell %u (%s) misses spell_affect for effect %u",id,spellInfo->SpellName[sWorld.GetDefaultDbcLocale()], effectId);
        }
    }
}

void SpellMgr::LoadFacingCasterFlags()
{
    mSpellFacingFlagMap.clear();
    uint32 count = 0;

    //                                                0              1
    QueryResult *result = WorldDatabase.Query("SELECT entry, facingcasterflag FROM spell_facing");
    if (!result)
    {
        BarGoLink bar(1);
        bar.step();
        sLog.outString();
        sLog.outString(">> Loaded %u facing caster flags", count);
        return;
    }

    BarGoLink bar(result->GetRowCount());

    do
    {
        Field *fields = result->Fetch();

        bar.step();

        uint32 entry              = fields[0].GetUInt32();
        uint32 FacingCasterFlags  = fields[1].GetUInt32();

        SpellEntry const* spellInfo = sSpellStore.LookupEntry(entry);
        if (!spellInfo)
        {
            sLog.outErrorDb("Spell %u listed in `spell_facing` does not exist", entry);
            continue;
        }
        mSpellFacingFlagMap[entry]    = FacingCasterFlags;

        ++count;
    }
    while( result->NextRow() );

    delete result;

    sLog.outString();
    sLog.outString(">> Loaded %u facing caster flags", count);
}
