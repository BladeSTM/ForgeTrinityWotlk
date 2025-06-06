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

/* Script Data Start
SDName: Dalaran
SDAuthor: WarHead, MaXiMiUS
SD%Complete: 99%
SDComment: For what is 63990+63991? Same function but don't work correct...
SDCategory: Dalaran
Script Data End */

#include "ScriptMgr.h"
#include "Containers.h"
#include "DatabaseEnv.h"
#include "Mail.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "SpellScript.h"
#include "WorldSession.h"

/*******************************************************
 * npc_mageguard_dalaran
 *******************************************************/

enum Spells
{
    SPELL_TRESPASSER_A                     = 54028,
    SPELL_TRESPASSER_H                     = 54029,

    SPELL_SUNREAVER_DISGUISE_FEMALE        = 70973,
    SPELL_SUNREAVER_DISGUISE_MALE          = 70974,
    SPELL_SILVER_COVENANT_DISGUISE_FEMALE  = 70971,
    SPELL_SILVER_COVENANT_DISGUISE_MALE    = 70972,
};

enum NPCs // All outdoor guards are within 35.0f of these NPCs
{
    NPC_APPLEBOUGH_A                       = 29547,
    NPC_SWEETBERRY_H                       = 29715,
    NPC_SILVER_COVENANT_GUARDIAN_MAGE      = 29254,
    NPC_SUNREAVER_GUARDIAN_MAGE            = 29255,
};

struct npc_mageguard_dalaran : public ScriptedAI
{
    npc_mageguard_dalaran(Creature* creature) : ScriptedAI(creature) { }

    void Reset() override { }

    void JustEngagedWith(Unit* /*who*/) override { }

    void AttackStart(Unit* /*who*/) override { }

    void MoveInLineOfSight(Unit* who) override
    {
        if (!who || !who->IsInWorld() || who->GetZoneId() != 4395)
            return;

        if (!me->IsWithinDist(who, 65.0f, false))
            return;

        Player* player = who->GetCharmerOrOwnerPlayerOrPlayerItself();

        if (!player || player->IsGameMaster() || player->IsBeingTeleported() ||
            // If player has Disguise aura for quest A Meeting With The Magister or An Audience With The Arcanist, do not teleport it away but let it pass
            player->HasAura(SPELL_SUNREAVER_DISGUISE_FEMALE) || player->HasAura(SPELL_SUNREAVER_DISGUISE_MALE) ||
            player->HasAura(SPELL_SILVER_COVENANT_DISGUISE_FEMALE) || player->HasAura(SPELL_SILVER_COVENANT_DISGUISE_MALE) ||
            // If player has already been teleported, don't try to teleport again
            player->HasAura(SPELL_TRESPASSER_A) || player->HasAura(SPELL_TRESPASSER_H))
            return;

        switch (me->GetEntry())
        {
            case NPC_SILVER_COVENANT_GUARDIAN_MAGE:
                if (player->GetTeam() == HORDE)              // Horde unit found in Alliance area
                {
                    if (GetClosestCreatureWithEntry(me, NPC_APPLEBOUGH_A, 32.0f))
                    {
                        if (me->isInBackInMap(who, 12.0f))   // In my line of sight, "outdoors", and behind me
                            DoCast(who, SPELL_TRESPASSER_A); // Teleport the Horde unit out
                    }
                    else                                      // In my line of sight, and "indoors"
                        DoCast(who, SPELL_TRESPASSER_A);     // Teleport the Horde unit out
                }
                break;
            case NPC_SUNREAVER_GUARDIAN_MAGE:
                if (player->GetTeam() == ALLIANCE)           // Alliance unit found in Horde area
                {
                    if (GetClosestCreatureWithEntry(me, NPC_SWEETBERRY_H, 32.0f))
                    {
                        if (me->isInBackInMap(who, 12.0f))   // In my line of sight, "outdoors", and behind me
                            DoCast(who, SPELL_TRESPASSER_H); // Teleport the Alliance unit out
                    }
                    else                                      // In my line of sight, and "indoors"
                        DoCast(who, SPELL_TRESPASSER_H);     // Teleport the Alliance unit out
                }
                break;
        }
        return;
    }

    void UpdateAI(uint32 /*diff*/) override { }
};

enum MinigobData
{
    ZONE_DALARAN            = 4395,

    SPELL_MANABONKED        = 61839,
    SPELL_TELEPORT_VISUAL   = 51347,
    SPELL_IMPROVED_BLINK    = 61995,

    EVENT_SELECT_TARGET     = 1,
    EVENT_LAUGH_1           = 2,
    EVENT_WANDER            = 3,
    EVENT_PAUSE             = 4,
    EVENT_CAST              = 5,
    EVENT_LAUGH_2           = 6,
    EVENT_BLINK             = 7,
    EVENT_DESPAWN           = 8,

    MAIL_MINIGOB_ENTRY      = 264,
    MAIL_DELIVER_DELAY_MIN  = 5*MINUTE,
    MAIL_DELIVER_DELAY_MAX  = 15*MINUTE
};

struct npc_minigob_manabonk : public ScriptedAI
{
    npc_minigob_manabonk(Creature* creature) : ScriptedAI(creature)
    {
        me->setActive(true);
    }

    void Reset() override
    {
        playerGuid = ObjectGuid();
        me->SetVisible(false);
        events.ScheduleEvent(EVENT_SELECT_TARGET, 1s);
    }

    void GetPlayersInDalaran(std::vector<Player*>& playerList) const
    {
        Map::PlayerList const& players = me->GetMap()->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
            if (Player* player = itr->GetSource()->ToPlayer())
                if (player->GetZoneId() == ZONE_DALARAN && !player->IsFlying() && !player->IsMounted() && !player->IsGameMaster())
                    playerList.push_back(player);
    }

    static Player* SelectTargetInDalaran(std::vector<Player*>& PlayerInDalaranList)
    {
        if (PlayerInDalaranList.empty())
            return nullptr;

        return Trinity::Containers::SelectRandomContainerElement(PlayerInDalaranList);
    }

    void SendMailToPlayer(Player* player) const
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        int16 deliverDelay = irand(MAIL_DELIVER_DELAY_MIN, MAIL_DELIVER_DELAY_MAX);
        MailDraft(MAIL_MINIGOB_ENTRY, true).SendMailTo(trans, MailReceiver(player), MailSender(MAIL_CREATURE, me->GetEntry()), MAIL_CHECK_MASK_NONE, deliverDelay);
        CharacterDatabase.CommitTransaction(trans);
    }

    void UpdateAI(uint32 diff) override
    {
        events.Update(diff);

        while (uint32 eventId = events.ExecuteEvent())
        {
            switch (eventId)
            {
                case EVENT_SELECT_TARGET:
                {
                    std::vector<Player*> PlayerInDalaranList;
                    GetPlayersInDalaran(PlayerInDalaranList);

                    // Increases chance of event based on player count in Dalaran (100 players or more = 100% else player count%)
                    if (PlayerInDalaranList.empty() || urand(1, 100) > PlayerInDalaranList.size())
                        me->AddObjectToRemoveList();

                    me->SetVisible(true);
                    DoCastSelf(SPELL_TELEPORT_VISUAL);
                    if (Player* player = SelectTargetInDalaran(PlayerInDalaranList))
                    {
                        playerGuid = player->GetGUID();
                        Position pos = player->GetPosition();
                        float dist = frand(10.0f, 30.0f);
                        float angle = frand(0.0f, 1.0f) * M_PI * 2.0f;
                        player->MovePositionToFirstCollision(pos, dist, angle);
                        me->NearTeleportTo(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), pos.GetOrientation());
                    }
                    events.ScheduleEvent(EVENT_LAUGH_1, Seconds(2));
                    break;
                }
                case EVENT_LAUGH_1:
                    me->HandleEmoteCommand(EMOTE_ONESHOT_LAUGH_NO_SHEATHE);
                    events.ScheduleEvent(EVENT_WANDER, 3s);
                    break;
                case EVENT_WANDER:
                    me->GetMotionMaster()->MoveRandom(8);
                    events.ScheduleEvent(EVENT_PAUSE, 1min);
                    break;
                case EVENT_PAUSE:
                    me->GetMotionMaster()->MoveIdle();
                    events.ScheduleEvent(EVENT_CAST, 2s);
                    break;
                case EVENT_CAST:
                    if (Player* player = me->GetMap()->GetPlayer(playerGuid))
                    {
                        DoCast(player, SPELL_MANABONKED);
                        SendMailToPlayer(player);
                    }
                    else
                        me->AddObjectToRemoveList();

                    events.ScheduleEvent(EVENT_LAUGH_2, Seconds(8));
                    break;
                case EVENT_LAUGH_2:
                    me->HandleEmoteCommand(EMOTE_ONESHOT_LAUGH_NO_SHEATHE);
                    events.ScheduleEvent(EVENT_BLINK, 3s);
                    break;
                case EVENT_BLINK:
                    DoCastSelf(SPELL_IMPROVED_BLINK);
                    events.ScheduleEvent(EVENT_DESPAWN, 4s);
                    break;
                case EVENT_DESPAWN:
                    me->AddObjectToRemoveList();
                    break;
                default:
                    break;
            }
        }
    }

private:
    ObjectGuid playerGuid;
    EventMap events;
};

enum TeleportToDalaran
{
    SPELL_TELEPORT_TO_DALARAN     = 53360
};

// 54620 - Teleport Crystal: Teleport to Dalaran AICast Script
class spell_dalaran_teleport_to_dalaran : public SpellScript
{
    PrepareSpellScript(spell_dalaran_teleport_to_dalaran);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TELEPORT_TO_DALARAN });
    }

    void HandleScript(SpellEffIndex /*effIndex*/)
    {
        GetHitUnit()->CastSpell(GetHitUnit(), SPELL_TELEPORT_TO_DALARAN);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_dalaran_teleport_to_dalaran::HandleScript, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

enum TeleportToLakeWintergrasp
{
    SPELL_TELEPORT_TO_LAKE_WINTERGRASP     = 58681
};

// 58622 - Teleport to Lake Wintergrasp
class spell_dalaran_teleport_to_lake_wintergrasp : public SpellScript
{
    PrepareSpellScript(spell_dalaran_teleport_to_lake_wintergrasp);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TELEPORT_TO_LAKE_WINTERGRASP });
    }

    void HandleScript(SpellEffIndex /*effIndex*/)
    {
        GetHitUnit()->CastSpell(GetHitUnit(), SPELL_TELEPORT_TO_LAKE_WINTERGRASP);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_dalaran_teleport_to_lake_wintergrasp::HandleScript, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

void AddSC_dalaran()
{
    RegisterCreatureAI(npc_mageguard_dalaran);
    RegisterCreatureAI(npc_minigob_manabonk);
    RegisterSpellScript(spell_dalaran_teleport_to_dalaran);
    RegisterSpellScript(spell_dalaran_teleport_to_lake_wintergrasp);
}
