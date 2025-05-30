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
#include "InstanceScript.h"
#include "ScriptedCreature.h"
#include "the_black_morass.h"

enum DejaTexts
{
    SAY_ENTER                   = 0,
    SAY_AGGRO                   = 1,
    SAY_BANISH                  = 2,
    SAY_SLAY                    = 3,
    SAY_DEATH                   = 4
};

enum DejaSpells
{
    SPELL_ARCANE_BLAST          = 31457,
    SPELL_ARCANE_DISCHARGE      = 31472,
    SPELL_TIME_LAPSE            = 31467,
    SPELL_ATTRACTION            = 38540                       //Not Implemented (Heroic mode)
};

enum DejaEvents
{
    EVENT_ARCANE_BLAST          = 1,
    EVENT_TIME_LAPSE,
    EVENT_ARCANE_DISCHARGE,
    EVENT_ATTRACTION
};

// 17879 - Chrono Lord Deja
struct boss_chrono_lord_deja : public BossAI
{
    boss_chrono_lord_deja(Creature* creature) : BossAI(creature, TYPE_CRONO_LORD_DEJA) { }

    void Reset() override { }

    void JustEngagedWith(Unit* /*who*/) override
    {
        events.ScheduleEvent(EVENT_ARCANE_BLAST, 18s, 23s);
        events.ScheduleEvent(EVENT_TIME_LAPSE, 10s, 15s);
        events.ScheduleEvent(EVENT_ARCANE_DISCHARGE, 20s, 30s);
        if (IsHeroic())
            events.ScheduleEvent(EVENT_ATTRACTION, 25s, 35s);

        Talk(SAY_AGGRO);
    }

    void MoveInLineOfSight(Unit* who) override
    {
        //Despawn Time Keeper
        if (who->GetTypeId() == TYPEID_UNIT && who->GetEntry() == NPC_TIME_KEEPER)
        {
            if (me->IsWithinDistInMap(who, 20.0f))
            {
                Talk(SAY_BANISH);
                Unit::DealDamage(me, who, who->GetHealth(), nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
            }
        }

        ScriptedAI::MoveInLineOfSight(who);
    }

    void KilledUnit(Unit* /*victim*/) override
    {
        Talk(SAY_SLAY);
    }

    void JustDied(Unit* /*killer*/) override
    {
        Talk(SAY_DEATH);

        instance->SetData(TYPE_RIFT, SPECIAL);
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
                case EVENT_ARCANE_BLAST:
                    DoCastVictim(SPELL_ARCANE_BLAST);
                    events.Repeat(15s, 25s);
                    break;
                case EVENT_TIME_LAPSE:
                    Talk(SAY_BANISH);
                    DoCastSelf(SPELL_TIME_LAPSE);
                    events.Repeat(15s, 25s);
                    break;
                case EVENT_ARCANE_DISCHARGE:
                    DoCastSelf(SPELL_ARCANE_DISCHARGE);
                    events.Repeat(20s, 30s);
                    break;
                case EVENT_ATTRACTION:
                    DoCastSelf(SPELL_ATTRACTION);
                    events.Repeat(25s, 35s);
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

void AddSC_boss_chrono_lord_deja()
{
    RegisterBlackMorassCreatureAI(boss_chrono_lord_deja);
}
