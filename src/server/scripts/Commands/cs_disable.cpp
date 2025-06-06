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

/* ScriptData
Name: disable_commandscript
%Complete: 100
Comment: All disable related commands
Category: commandscripts
EndScriptData */

#include "ScriptMgr.h"
#include "Chat.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "DisableMgr.h"
#include "Language.h"
#include "ObjectMgr.h"
#include "OutdoorPvP.h"
#include "Player.h"
#include "RBAC.h"
#include "SpellMgr.h"

using namespace Trinity::ChatCommands;

class disable_commandscript : public CommandScript
{
public:
    disable_commandscript() : CommandScript("disable_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable removeDisableCommandTable =
        {
            { "spell",                HandleRemoveDisableSpellCommand,               rbac::RBAC_PERM_COMMAND_DISABLE_REMOVE_SPELL,                Console::Yes },
            { "quest",                HandleRemoveDisableQuestCommand,               rbac::RBAC_PERM_COMMAND_DISABLE_REMOVE_QUEST,                Console::Yes },
            { "map",                  HandleRemoveDisableMapCommand,                 rbac::RBAC_PERM_COMMAND_DISABLE_REMOVE_MAP,                  Console::Yes },
            { "battleground",         HandleRemoveDisableBattlegroundCommand,        rbac::RBAC_PERM_COMMAND_DISABLE_REMOVE_BATTLEGROUND,         Console::Yes },
            { "achievement_criteria", HandleRemoveDisableAchievementCriteriaCommand, rbac::RBAC_PERM_COMMAND_DISABLE_REMOVE_ACHIEVEMENT_CRITERIA, Console::Yes },
            { "outdoorpvp",           HandleRemoveDisableOutdoorPvPCommand,          rbac::RBAC_PERM_COMMAND_DISABLE_REMOVE_OUTDOORPVP,           Console::Yes },
            { "vmap",                 HandleRemoveDisableVmapCommand,                rbac::RBAC_PERM_COMMAND_DISABLE_REMOVE_VMAP,                 Console::Yes },
            { "mmap",                 HandleRemoveDisableMMapCommand,                rbac::RBAC_PERM_COMMAND_DISABLE_REMOVE_MMAP,                 Console::Yes },
        };
        static ChatCommandTable addDisableCommandTable =
        {
            { "spell",                HandleAddDisableSpellCommand,                  rbac::RBAC_PERM_COMMAND_DISABLE_ADD_SPELL,                Console::Yes },
            { "quest",                HandleAddDisableQuestCommand,                  rbac::RBAC_PERM_COMMAND_DISABLE_ADD_QUEST,                Console::Yes },
            { "map",                  HandleAddDisableMapCommand,                    rbac::RBAC_PERM_COMMAND_DISABLE_ADD_MAP,                  Console::Yes },
            { "battleground",         HandleAddDisableBattlegroundCommand,           rbac::RBAC_PERM_COMMAND_DISABLE_ADD_BATTLEGROUND,         Console::Yes },
            { "achievement_criteria", HandleAddDisableAchievementCriteriaCommand,    rbac::RBAC_PERM_COMMAND_DISABLE_ADD_ACHIEVEMENT_CRITERIA, Console::Yes },
            { "outdoorpvp",           HandleAddDisableOutdoorPvPCommand,             rbac::RBAC_PERM_COMMAND_DISABLE_ADD_OUTDOORPVP,           Console::Yes },
            { "vmap",                 HandleAddDisableVmapCommand,                   rbac::RBAC_PERM_COMMAND_DISABLE_ADD_VMAP,                 Console::Yes },
            { "mmap",                 HandleAddDisableMMapCommand,                   rbac::RBAC_PERM_COMMAND_DISABLE_ADD_MMAP,                 Console::Yes },
        };
        static ChatCommandTable disableCommandTable =
        {
            { "add",    addDisableCommandTable },
            { "remove", removeDisableCommandTable },
        };
        static ChatCommandTable commandTable =
        {
            { "disable", disableCommandTable },
        };
        return commandTable;
    }

    static bool HandleAddDisables(ChatHandler* handler, DisableType disableType, uint32 entry, Optional<uint16> flags, Tail disableComment)
    {
        char const* disableTypeStr = "";

        switch (disableType)
        {
            case DISABLE_TYPE_SPELL:
            {
                if (!sSpellMgr->GetSpellInfo(entry))
                {
                    handler->PSendSysMessage(LANG_COMMAND_NOSPELLFOUND);
                    handler->SetSentErrorMessage(true);
                    return false;
                }
                disableTypeStr = "spell";
                break;
            }
            case DISABLE_TYPE_QUEST:
            {
                if (!sObjectMgr->GetQuestTemplate(entry))
                {
                    handler->PSendSysMessage(LANG_COMMAND_QUEST_NOTFOUND, entry);
                    handler->SetSentErrorMessage(true);
                    return false;
                }
                disableTypeStr = "quest";
                break;
            }
            case DISABLE_TYPE_MAP:
            {
                if (!sMapStore.LookupEntry(entry))
                {
                    handler->PSendSysMessage(LANG_COMMAND_NOMAPFOUND);
                    handler->SetSentErrorMessage(true);
                    return false;
                }
                disableTypeStr = "map";
                break;
            }
            case DISABLE_TYPE_BATTLEGROUND:
            {
                if (!sBattlemasterListStore.LookupEntry(entry))
                {
                    handler->PSendSysMessage(LANG_COMMAND_NO_BATTLEGROUND_FOUND);
                    handler->SetSentErrorMessage(true);
                    return false;
                }
                disableTypeStr = "battleground";
                break;
            }
            case DISABLE_TYPE_ACHIEVEMENT_CRITERIA:
            {
                if (!sAchievementCriteriaStore.LookupEntry(entry))
                {
                    handler->PSendSysMessage(LANG_COMMAND_NO_ACHIEVEMENT_CRITERIA_FOUND);
                    handler->SetSentErrorMessage(true);
                    return false;
                }
                disableTypeStr = "achievement criteria";
                break;
            }
            case DISABLE_TYPE_OUTDOORPVP:
            {
                if (entry > MAX_OUTDOORPVP_TYPES)
                {
                    handler->PSendSysMessage(LANG_COMMAND_NO_OUTDOOR_PVP_FORUND);
                    handler->SetSentErrorMessage(true);
                    return false;
                }
                disableTypeStr = "outdoorpvp";
                break;
            }
            case DISABLE_TYPE_VMAP:
            {
                if (!sMapStore.LookupEntry(entry))
                {
                    handler->PSendSysMessage(LANG_COMMAND_NOMAPFOUND);
                    handler->SetSentErrorMessage(true);
                    return false;
                }
                disableTypeStr = "vmap";
                break;
            }
            case DISABLE_TYPE_MMAP:
            {
                if (!sMapStore.LookupEntry(entry))
                {
                    handler->PSendSysMessage(LANG_COMMAND_NOMAPFOUND);
                    handler->SetSentErrorMessage(true);
                    return false;
                }
                disableTypeStr = "mmap";
                break;
            }
            case DISABLE_TYPE_LFG_MAP:
            {
                if (!sMapStore.LookupEntry(entry))
                {
                    handler->PSendSysMessage(LANG_COMMAND_NOMAPFOUND);
                    handler->SetSentErrorMessage(true);
                    return false;
                }
                disableTypeStr = "lfg map";
                break;
            }
            default:
                break;
        }

        WorldDatabasePreparedStatement* stmt = WorldDatabase.GetPreparedStatement(WORLD_SEL_DISABLES);
        stmt->setUInt32(0, entry);
        stmt->setUInt8(1, disableType);
        PreparedQueryResult result = WorldDatabase.Query(stmt);
        if (result)
        {
            handler->PSendSysMessage("This %s (Id: %u) is already disabled.", disableTypeStr, entry);
            handler->SetSentErrorMessage(true);
            return false;
        }

        stmt = WorldDatabase.GetPreparedStatement(WORLD_INS_DISABLES);
        stmt->setUInt32(0, entry);
        stmt->setUInt8(1, disableType);
        stmt->setUInt16(2, flags.value_or<uint16>(0));
        stmt->setStringView(3, disableComment);
        WorldDatabase.Execute(stmt);

        handler->PSendSysMessage("Add Disabled %s (Id: %u) for reason " STRING_VIEW_FMT, disableTypeStr, entry, STRING_VIEW_FMT_ARG(disableComment));
        return true;
    }

    static bool HandleAddDisableSpellCommand(ChatHandler* handler, uint32 entry, Optional<uint16> flags, Tail disableComment)
    {
        return HandleAddDisables(handler, DISABLE_TYPE_SPELL, entry, flags, disableComment);
    }

    static bool HandleAddDisableQuestCommand(ChatHandler* handler, uint32 entry, Optional<uint16> flags, Tail disableComment)
    {
        return HandleAddDisables(handler, DISABLE_TYPE_QUEST, entry, flags, disableComment);
    }

    static bool HandleAddDisableMapCommand(ChatHandler* handler, uint32 entry, Optional<uint16> flags, Tail disableComment)
    {
        return HandleAddDisables(handler, DISABLE_TYPE_MAP, entry, flags, disableComment);
    }

    static bool HandleAddDisableBattlegroundCommand(ChatHandler* handler, uint32 entry, Optional<uint16> flags, Tail disableComment)
    {
        return HandleAddDisables(handler, DISABLE_TYPE_BATTLEGROUND, entry, flags, disableComment);
    }

    static bool HandleAddDisableAchievementCriteriaCommand(ChatHandler* handler, uint32 entry, Optional<uint16> flags, Tail disableComment)
    {
        return HandleAddDisables(handler, DISABLE_TYPE_ACHIEVEMENT_CRITERIA, entry, flags, disableComment);
    }

    static bool HandleAddDisableOutdoorPvPCommand(ChatHandler* handler, uint32 entry, Optional<uint16> flags, Tail disableComment)
    {
        return HandleAddDisables(handler, DISABLE_TYPE_OUTDOORPVP, entry, flags, disableComment);
    }

    static bool HandleAddDisableVmapCommand(ChatHandler* handler, uint32 entry, Optional<uint16> flags, Tail disableComment)
    {
        return HandleAddDisables(handler, DISABLE_TYPE_VMAP, entry, flags, disableComment);
    }

    static bool HandleAddDisableMMapCommand(ChatHandler* handler, uint32 entry, Optional<uint16> flags, Tail disableComment)
    {
        return HandleAddDisables(handler, DISABLE_TYPE_MMAP, entry, flags, disableComment);
    }

    static bool HandleRemoveDisables(ChatHandler* handler, DisableType disableType, uint32 entry)
    {
        std::string disableTypeStr = "";

        switch (disableType)
        {
            case DISABLE_TYPE_SPELL:
                disableTypeStr = "spell";
                break;
            case DISABLE_TYPE_QUEST:
                disableTypeStr = "quest";
                break;
            case DISABLE_TYPE_MAP:
                disableTypeStr = "map";
                break;
            case DISABLE_TYPE_BATTLEGROUND:
                disableTypeStr = "battleground";
                break;
            case DISABLE_TYPE_ACHIEVEMENT_CRITERIA:
                disableTypeStr = "achievement criteria";
                break;
            case DISABLE_TYPE_OUTDOORPVP:
                disableTypeStr = "outdoorpvp";
                break;
            case DISABLE_TYPE_VMAP:
                disableTypeStr = "vmap";
                break;
            case DISABLE_TYPE_MMAP:
                disableTypeStr = "mmap";
                break;
            case DISABLE_TYPE_LFG_MAP:
                disableTypeStr = "lfg map";
                break;
            default:
                break;
        }

        WorldDatabasePreparedStatement* stmt = WorldDatabase.GetPreparedStatement(WORLD_SEL_DISABLES);
        stmt->setUInt32(0, entry);
        stmt->setUInt8(1, disableType);
        PreparedQueryResult result = WorldDatabase.Query(stmt);
        if (!result)
        {
            handler->PSendSysMessage("This %s (Id: %u) is not disabled.", disableTypeStr.c_str(), entry);
            handler->SetSentErrorMessage(true);
            return false;
        }

        stmt = WorldDatabase.GetPreparedStatement(WORLD_DEL_DISABLES);
        stmt->setUInt32(0, entry);
        stmt->setUInt8(1, disableType);
        WorldDatabase.Execute(stmt);

        handler->PSendSysMessage("Remove Disabled %s (Id: %u)", disableTypeStr.c_str(), entry);
        return true;
    }

    static bool HandleRemoveDisableSpellCommand(ChatHandler* handler, uint32 entry)
    {
        return HandleRemoveDisables(handler, DISABLE_TYPE_SPELL, entry);
    }

    static bool HandleRemoveDisableQuestCommand(ChatHandler* handler, uint32 entry)
    {
        return HandleRemoveDisables(handler, DISABLE_TYPE_QUEST, entry);
    }

    static bool HandleRemoveDisableMapCommand(ChatHandler* handler, uint32 entry)
    {
        return HandleRemoveDisables(handler, DISABLE_TYPE_MAP, entry);
    }

    static bool HandleRemoveDisableBattlegroundCommand(ChatHandler* handler, uint32 entry)
    {
        return HandleRemoveDisables(handler, DISABLE_TYPE_BATTLEGROUND, entry);
    }

    static bool HandleRemoveDisableAchievementCriteriaCommand(ChatHandler* handler, uint32 entry)
    {
        return HandleRemoveDisables(handler, DISABLE_TYPE_ACHIEVEMENT_CRITERIA, entry);
    }

    static bool HandleRemoveDisableOutdoorPvPCommand(ChatHandler* handler, uint32 entry)
    {
        return HandleRemoveDisables(handler, DISABLE_TYPE_OUTDOORPVP, entry);
    }

    static bool HandleRemoveDisableVmapCommand(ChatHandler* handler, uint32 entry)
    {
        return HandleRemoveDisables(handler, DISABLE_TYPE_VMAP, entry);
    }

    static bool HandleRemoveDisableMMapCommand(ChatHandler* handler, uint32 entry)
    {
        return HandleRemoveDisables(handler, DISABLE_TYPE_MMAP, entry);
    }
};

void AddSC_disable_commandscript()
{
    new disable_commandscript();
}
