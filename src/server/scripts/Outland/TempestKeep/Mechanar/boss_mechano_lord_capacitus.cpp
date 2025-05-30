/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ScriptMgr.h"
#include "mechanar.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "SpellInfo.h"
#include "SpellScript.h"

enum CapacitusTexts
{
    SAY_AGGRO                       = 0,
    SAY_REFLECTIVE_MAGIC_SHIELD     = 1,
    SAY_REFLECTIVE_DAMAGE_SHIELD    = 2,
    SAY_SLAY                        = 3,
    SAY_DEATH                       = 4
};

enum CapacitusSpells
{
    SPELL_HEADCRACK                 = 35161,
    SPELL_REFLECTIVE_MAGIC_SHIELD   = 35158,
    SPELL_REFLECTIVE_DAMAGE_SHIELD  = 35159,
    SPELL_POLARITY_SHIFT            = 39096,
    SPELL_BERSERK                   = 26662,

    SPELL_SUMMON_NETHER_CHARGE_NE   = 35153,
    SPELL_SUMMON_NETHER_CHARGE_NW   = 35904,
    SPELL_SUMMON_NETHER_CHARGE_SE   = 35905,
    SPELL_SUMMON_NETHER_CHARGE_SW   = 35906,

    SPELL_POSITIVE_POLARITY         = 39088,
    SPELL_POSITIVE_CHARGE_STACK     = 39089,
    SPELL_POSITIVE_CHARGE           = 39090,

    SPELL_NEGATIVE_POLARITY         = 39091,
    SPELL_NEGATIVE_CHARGE_STACK     = 39092,
    SPELL_NEGATIVE_CHARGE           = 39093
};

enum CapacitusEvents
{
    EVENT_HEADCRACK                 = 1,
    EVENT_REFLECTIVE_DAMAGE_SHIELD,
    EVENT_REFLECTIVE_MAGIE_SHIELD,
    EVENT_POSITIVE_SHIFT,
    EVENT_SUMMON_NETHER_CHARGE,
    EVENT_BERSERK
};

// 19219 - Mechano-Lord Capacitus
struct boss_mechano_lord_capacitus : public BossAI
{
    boss_mechano_lord_capacitus(Creature* creature) : BossAI(creature, DATA_MECHANOLORD_CAPACITUS) { }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        Talk(SAY_AGGRO);
        events.ScheduleEvent(EVENT_HEADCRACK, 10s);
        events.ScheduleEvent(EVENT_REFLECTIVE_DAMAGE_SHIELD, 15s);
        events.ScheduleEvent(EVENT_SUMMON_NETHER_CHARGE, 10s);
        events.ScheduleEvent(EVENT_BERSERK, 3min);

        if (IsHeroic())
            events.ScheduleEvent(EVENT_POSITIVE_SHIFT, 15s);
    }

    void KilledUnit(Unit* /*victim*/) override
    {
        Talk(SAY_SLAY);
    }

    void JustDied(Unit* /*killer*/) override
    {
        _JustDied();
        Talk(SAY_DEATH);
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;

        events.Update(diff);

        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        while (uint32 eventId = events.ExecuteEvent())
        {
            switch (eventId)
            {
                case EVENT_HEADCRACK:
                    DoCastVictim(SPELL_HEADCRACK);
                    events.Repeat(10s);
                    break;
                case EVENT_REFLECTIVE_DAMAGE_SHIELD:
                    Talk(SAY_REFLECTIVE_DAMAGE_SHIELD);
                    DoCastSelf(SPELL_REFLECTIVE_DAMAGE_SHIELD);
                    events.Repeat(30s);
                    break;
                case EVENT_REFLECTIVE_MAGIE_SHIELD:
                    Talk(SAY_REFLECTIVE_MAGIC_SHIELD);
                    DoCastSelf(SPELL_REFLECTIVE_MAGIC_SHIELD);
                    events.Repeat(30s);
                    break;
                case EVENT_POSITIVE_SHIFT:
                    DoCastAOE(SPELL_POLARITY_SHIFT);
                    events.Repeat(45s, 60s);
                    break;
                case EVENT_SUMMON_NETHER_CHARGE:
                {
                    uint32 spellId = RAND(SPELL_SUMMON_NETHER_CHARGE_NE,
                                          SPELL_SUMMON_NETHER_CHARGE_NW,
                                          SPELL_SUMMON_NETHER_CHARGE_SE,
                                          SPELL_SUMMON_NETHER_CHARGE_SW);
                    Milliseconds netherChargeTimer = DUNGEON_MODE(randtime(9s, 11s), randtime(2s, 5s));
                    DoCastSelf(spellId);
                    events.Repeat(netherChargeTimer);
                    break;
                }
                case EVENT_BERSERK:
                    DoCastSelf(SPELL_BERSERK);
                    break;
                default:
                    break;
            }

            if (me->HasUnitState(UNIT_STATE_CASTING))
                return;
        }

        DoMeleeAttackIfReady();
    }
};

// 39090 - Positive Charge
// 39093 - Negative Charge
class spell_capacitus_polarity_charge : public SpellScript
{
    PrepareSpellScript(spell_capacitus_polarity_charge);

    bool Validate(SpellInfo const* /*spell*/) override
    {
        return ValidateSpellInfo(
        {
            SPELL_POSITIVE_CHARGE,
            SPELL_POSITIVE_CHARGE_STACK,
            SPELL_NEGATIVE_CHARGE,
            SPELL_NEGATIVE_CHARGE_STACK
        });
    }

    void HandleTargets(std::list<WorldObject*>& targetList)
    {
        uint8 count = 0;
        for (std::list<WorldObject*>::iterator ihit = targetList.begin(); ihit != targetList.end(); ++ihit)
            if ((*ihit)->GetGUID() != GetCaster()->GetGUID())
                if (Player* target = (*ihit)->ToPlayer())
                    if (target->HasAura(GetTriggeringSpell()->Id))
                        ++count;

        if (count)
        {
            uint32 spellId = 0;

            if (GetSpellInfo()->Id == SPELL_POSITIVE_CHARGE)
                spellId = SPELL_POSITIVE_CHARGE_STACK;
            else // if (GetSpellInfo()->Id == SPELL_NEGATIVE_CHARGE)
                spellId = SPELL_NEGATIVE_CHARGE_STACK;

            GetCaster()->SetAuraStack(spellId, GetCaster(), count);
        }
    }

    void HandleDamage(SpellEffIndex /*effIndex*/)
    {
        if (!GetTriggeringSpell())
            return;

        Unit* target = GetHitUnit();

        if (target->HasAura(GetTriggeringSpell()->Id))
            PreventHitDamage();
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_capacitus_polarity_charge::HandleDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_capacitus_polarity_charge::HandleTargets, EFFECT_0, TARGET_UNIT_SRC_AREA_ALLY);
    }
};

// 39096 - Polarity Shift
class spell_capacitus_polarity_shift : public SpellScript
{
    PrepareSpellScript(spell_capacitus_polarity_shift);

    bool Validate(SpellInfo const* /*spell*/) override
    {
        return ValidateSpellInfo({ SPELL_POSITIVE_POLARITY, SPELL_NEGATIVE_POLARITY });
    }

    void HandleDummy(SpellEffIndex /* effIndex */)
    {
        Unit* target = GetHitUnit();
        Unit* caster = GetCaster();

        target->CastSpell(target, roll_chance_i(50) ? SPELL_POSITIVE_POLARITY : SPELL_NEGATIVE_POLARITY, caster->GetGUID());
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_capacitus_polarity_shift::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

void AddSC_boss_mechano_lord_capacitus()
{
    RegisterMechanarCreatureAI(boss_mechano_lord_capacitus);
    RegisterSpellScript(spell_capacitus_polarity_charge);
    RegisterSpellScript(spell_capacitus_polarity_shift);
}
