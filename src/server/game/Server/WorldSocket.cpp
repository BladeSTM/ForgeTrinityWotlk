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

#include "WorldSocket.h"
#include "BigNumber.h"
#include "ClientBuildInfo.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "CryptoHash.h"
#include "CryptoRandom.h"
#include "IPLocation.h"
#include "Opcodes.h"
#include "PacketLog.h"
#include "Random.h"
#include "RBAC.h"
#include "Realm.h"
#include "ScriptMgr.h"
#ifdef FORGE
#include "LuaEngine.h"
#endif
#include "World.h"
#include "WorldSession.h"
#include <memory>

using boost::asio::ip::tcp;

WorldSocket::WorldSocket(tcp::socket&& socket)
    : Socket(std::move(socket)), _OverSpeedPings(0), _worldSession(nullptr), _authed(false), _sendBufferSize(4096)
{
    Trinity::Crypto::GetRandomBytes(_authSeed);
    _headerBuffer.Resize(sizeof(ClientPktHeader));
}

WorldSocket::~WorldSocket() = default;

void WorldSocket::Start()
{
    std::string ip_address = GetRemoteIpAddress().to_string();
    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_IP_INFO);
    stmt->setString(0, ip_address);

    _queryProcessor.AddCallback(LoginDatabase.AsyncQuery(stmt).WithPreparedCallback(std::bind(&WorldSocket::CheckIpCallback, this, std::placeholders::_1)));
}

void WorldSocket::CheckIpCallback(PreparedQueryResult result)
{
    if (result)
    {
        bool banned = false;
        do
        {
            Field* fields = result->Fetch();
            if (fields[0].GetUInt64() != 0)
                banned = true;

        } while (result->NextRow());

        if (banned)
        {
            SendAuthResponseError(AUTH_REJECT);
            TC_LOG_ERROR("network", "WorldSocket::CheckIpCallback: Sent Auth Response (IP {} banned).", GetRemoteIpAddress().to_string());
            DelayedCloseSocket();
            return;
        }
    }

    AsyncRead();
    HandleSendAuthSession();
}

bool WorldSocket::Update()
{
    EncryptablePacket* queued;
    if (_bufferQueue.Dequeue(queued))
    {
        // Allocate buffer only when it's needed but not on every Update() call.
        MessageBuffer buffer(_sendBufferSize);
        do
        {
            ServerPktHeader header(queued->size() + 2, queued->GetOpcode());
            if (queued->NeedsEncryption())
                _authCrypt.EncryptSend(header.header, header.getHeaderLength());

            if (buffer.GetRemainingSpace() < queued->size() + header.getHeaderLength())
            {
                QueuePacket(std::move(buffer));
                buffer.Resize(_sendBufferSize);
            }

            if (buffer.GetRemainingSpace() >= queued->size() + header.getHeaderLength())
            {
                buffer.Write(header.header, header.getHeaderLength());
                if (!queued->empty())
                    buffer.Write(queued->contents(), queued->size());
            }
            else    // single packet larger than buffer size
            {
                MessageBuffer packetBuffer(queued->size() + header.getHeaderLength());
                packetBuffer.Write(header.header, header.getHeaderLength());
                if (!queued->empty())
                    packetBuffer.Write(queued->contents(), queued->size());

                QueuePacket(std::move(packetBuffer));
            }

            delete queued;
        } while (_bufferQueue.Dequeue(queued));

        if (buffer.GetActiveSize() > 0)
            QueuePacket(std::move(buffer));
    }

    if (!BaseSocket::Update())
        return false;

    _queryProcessor.ProcessReadyCallbacks();

    return true;
}

void WorldSocket::HandleSendAuthSession()
{
    WorldPacket packet(SMSG_AUTH_CHALLENGE, 40);
    packet << uint32(1);                                    // 1...31
    packet.append(_authSeed);

    packet.append(Trinity::Crypto::GetRandomBytes<32>());               // new encryption seeds

    SendPacketAndLogOpcode(packet);
}

void WorldSocket::OnClose()
{
    {
        std::lock_guard<std::mutex> sessionGuard(_worldSessionLock);
        _worldSession = nullptr;
    }
}

void WorldSocket::ReadHandler()
{
    if (!IsOpen())
        return;

    MessageBuffer& packet = GetReadBuffer();
    while (packet.GetActiveSize() > 0)
    {
        if (_headerBuffer.GetRemainingSpace() > 0)
        {
            // need to receive the header
            std::size_t readHeaderSize = std::min(packet.GetActiveSize(), _headerBuffer.GetRemainingSpace());
            _headerBuffer.Write(packet.GetReadPointer(), readHeaderSize);
            packet.ReadCompleted(readHeaderSize);

            if (_headerBuffer.GetRemainingSpace() > 0)
            {
                // Couldn't receive the whole header this time.
                ASSERT(packet.GetActiveSize() == 0);
                break;
            }

            // We just received nice new header
            if (!ReadHeaderHandler())
            {
                CloseSocket();
                return;
            }
        }

        // We have full read header, now check the data payload
        if (_packetBuffer.GetRemainingSpace() > 0)
        {
            // need more data in the payload
            std::size_t readDataSize = std::min(packet.GetActiveSize(), _packetBuffer.GetRemainingSpace());
            _packetBuffer.Write(packet.GetReadPointer(), readDataSize);
            packet.ReadCompleted(readDataSize);

            if (_packetBuffer.GetRemainingSpace() > 0)
            {
                // Couldn't receive the whole data this time.
                ASSERT(packet.GetActiveSize() == 0);
                break;
            }
        }

        // just received fresh new payload
        ReadDataHandlerResult result = ReadDataHandler();
        _headerBuffer.Reset();
        if (result != ReadDataHandlerResult::Ok)
        {
            if (result != ReadDataHandlerResult::WaitingForQuery)
                CloseSocket();

            return;
        }
    }

    AsyncRead();
}

bool WorldSocket::ReadHeaderHandler()
{
    ASSERT(_headerBuffer.GetActiveSize() == sizeof(ClientPktHeader));

    if (_authCrypt.IsInitialized())
        _authCrypt.DecryptRecv(_headerBuffer.GetReadPointer(), sizeof(ClientPktHeader));

    ClientPktHeader* header = reinterpret_cast<ClientPktHeader*>(_headerBuffer.GetReadPointer());
    EndianConvertReverse(header->size);
    EndianConvert(header->cmd);

    if (!header->IsValidSize() || !header->IsValidOpcode())
    {
        TC_LOG_ERROR("network", "WorldSocket::ReadHeaderHandler(): client {} sent malformed packet (size: {}, cmd: {})",
            GetRemoteIpAddress().to_string(), header->size, header->cmd);
        return false;
    }

    header->size -= sizeof(header->cmd);
    _packetBuffer.Resize(header->size);
    return true;
}

struct AuthSession
{
    uint32 BattlegroupID = 0;
    uint32 LoginServerType = 0;
    uint32 RealmID = 0;
    uint32 Build = 0;
    std::array<uint8, 4> LocalChallenge = {};
    uint32 LoginServerID = 0;
    uint32 RegionID = 0;
    uint64 DosResponse = 0;
    Trinity::Crypto::SHA1::Digest Digest = {};
    std::string Account;
    ByteBuffer AddonInfo;
};

struct AccountInfo
{
    uint32 Id;
    ::SessionKey SessionKey;
    std::string LastIP;
    bool IsLockedToIP;
    std::string LockCountry;
    uint8 Expansion;
    int64 MuteTime;
    LocaleConstant Locale;
    uint32 Recruiter;
    std::string OS;
    Minutes TimezoneOffset;
    bool IsRectuiter;
    AccountTypes Security;
    bool IsBanned;

    explicit AccountInfo(Field const* fields)
    {
        //           0             1          2         3               4            5           6         7            8     9                 10                11
        // SELECT a.id, a.sessionkey, a.last_ip, a.locked, a.lock_country, a.expansion, a.mutetime, a.locale, a.recruiter, a.os, a.timezone_offset, aa.SecurityLevel,
        //                                                           12    13
        // ab.unbandate > UNIX_TIMESTAMP() OR ab.unbandate = ab.bandate, r.id
        // FROM account a
        // LEFT JOIN account_access aa ON a.id = aa.AccountID AND aa.RealmID IN (-1, ?)
        // LEFT JOIN account_banned ab ON a.id = ab.id
        // LEFT JOIN account r ON a.id = r.recruiter
        // WHERE a.username = ? ORDER BY aa.RealmID DESC LIMIT 1
        Id = fields[0].GetUInt32();
        SessionKey = fields[1].GetBinary<SESSION_KEY_LENGTH>();
        LastIP = fields[2].GetString();
        IsLockedToIP = fields[3].GetBool();
        LockCountry = fields[4].GetString();
        Expansion = fields[5].GetUInt8();
        MuteTime = fields[6].GetInt64();
        Locale = LocaleConstant(fields[7].GetUInt8());
        Recruiter = fields[8].GetUInt32();
        OS = fields[9].GetString();
        TimezoneOffset = Minutes(fields[10].GetInt16());
        Security = AccountTypes(fields[11].GetUInt8());
        IsBanned = fields[12].GetUInt64() != 0;
        IsRectuiter = fields[13].GetUInt32() != 0;

        uint32 world_expansion = sWorld->getIntConfig(CONFIG_EXPANSION);
        if (Expansion > world_expansion)
            Expansion = world_expansion;

        if (Locale >= TOTAL_LOCALES)
            Locale = LOCALE_enUS;
    }
};

WorldSocket::ReadDataHandlerResult WorldSocket::ReadDataHandler()
{
    ClientPktHeader* header = reinterpret_cast<ClientPktHeader*>(_headerBuffer.GetReadPointer());
    OpcodeClient opcode = static_cast<OpcodeClient>(header->cmd);

    WorldPacket packet(opcode, std::move(_packetBuffer));
    WorldPacket* packetToQueue;

    if (sPacketLog->CanLogPacket())
        sPacketLog->LogPacket(packet, CLIENT_TO_SERVER, GetRemoteIpAddress(), GetRemotePort());

    std::unique_lock<std::mutex> sessionGuard(_worldSessionLock, std::defer_lock);

    switch (opcode)
    {
        case CMSG_PING:
        {
            LogOpcodeText(opcode, sessionGuard);
            try
            {
                return HandlePing(packet) ? ReadDataHandlerResult::Ok : ReadDataHandlerResult::Error;
            }
            catch (ByteBufferException const&)
            {
            }
            TC_LOG_ERROR("network", "WorldSocket::ReadDataHandler(): client {} sent malformed CMSG_PING", GetRemoteIpAddress().to_string());
            return ReadDataHandlerResult::Error;
        }
        case CMSG_AUTH_SESSION:
        {
            LogOpcodeText(opcode, sessionGuard);
            if (_authed)
            {
                // locking just to safely log offending user is probably overkill but we are disconnecting him anyway
                if (sessionGuard.try_lock())
                    TC_LOG_ERROR("network", "WorldSocket::ProcessIncoming: received duplicate CMSG_AUTH_SESSION from {}", _worldSession->GetPlayerInfo());
                return ReadDataHandlerResult::Error;
            }

            try
            {
                HandleAuthSession(packet);
                return ReadDataHandlerResult::WaitingForQuery;
            }
            catch (ByteBufferException const&)
            {
            }
            TC_LOG_ERROR("network", "WorldSocket::ReadDataHandler(): client {} sent malformed CMSG_AUTH_SESSION", GetRemoteIpAddress().to_string());
            return ReadDataHandlerResult::Error;
        }
        case CMSG_KEEP_ALIVE: // todo: handle this packet in the same way of CMSG_TIME_SYNC_RESP
            sessionGuard.lock();
            LogOpcodeText(opcode, sessionGuard);
            if (_worldSession)
            {
                _worldSession->ResetTimeOutTime(true);
                return ReadDataHandlerResult::Ok;
            }
            TC_LOG_ERROR("network", "WorldSocket::ReadDataHandler: client {} sent CMSG_KEEP_ALIVE without being authenticated", GetRemoteIpAddress().to_string());
            return ReadDataHandlerResult::Error;
        case CMSG_TIME_SYNC_RESP:
            packetToQueue = new WorldPacket(std::move(packet), std::chrono::steady_clock::now());
            break;

        default:
            packetToQueue = new WorldPacket(std::move(packet));
            break;
    }

    sessionGuard.lock();

    LogOpcodeText(opcode, sessionGuard);

    if (!_worldSession)
    {
        TC_LOG_ERROR("network.opcode", "ProcessIncoming: Client not authed opcode = {}", uint32(opcode));
        delete packetToQueue;
        return ReadDataHandlerResult::Error;
    }

    OpcodeHandler const* handler = opcodeTable[opcode];
    if (!handler)
    {
        TC_LOG_ERROR("network.opcode", "No defined handler for opcode {} sent by {}", GetOpcodeNameForLogging(static_cast<OpcodeClient>(packet.GetOpcode())), _worldSession->GetPlayerInfo());
        delete packetToQueue;
        return ReadDataHandlerResult::Error;
    }

    // Our Idle timer will reset on any non PING opcodes on login screen, allowing us to catch people idling.
    _worldSession->ResetTimeOutTime(false);

    // Copy the packet to the heap before enqueuing
    _worldSession->QueuePacket(packetToQueue);

    return ReadDataHandlerResult::Ok;
}

void WorldSocket::LogOpcodeText(OpcodeClient opcode, std::unique_lock<std::mutex> const& guard) const
{
    if (!guard)
    {
        TC_LOG_TRACE("network.opcode", "C->S: {} {}", GetRemoteIpAddress().to_string(), GetOpcodeNameForLogging(opcode));
    }
    else
    {
        TC_LOG_TRACE("network.opcode", "C->S: {} {}", (_worldSession ? _worldSession->GetPlayerInfo() : GetRemoteIpAddress().to_string()),
            GetOpcodeNameForLogging(opcode));
    }
}

void WorldSocket::SendPacketAndLogOpcode(WorldPacket const& packet)
{
    TC_LOG_TRACE("network.opcode", "S->C: {} {}", GetRemoteIpAddress().to_string(), GetOpcodeNameForLogging(static_cast<OpcodeServer>(packet.GetOpcode())));
    SendPacket(packet);
}

void WorldSocket::SendPacket(WorldPacket const& packet)
{
    if (!IsOpen())
        return;

    if (sPacketLog->CanLogPacket())
        sPacketLog->LogPacket(packet, SERVER_TO_CLIENT, GetRemoteIpAddress(), GetRemotePort());

    _bufferQueue.Enqueue(new EncryptablePacket(packet, _authCrypt.IsInitialized()));
}

void WorldSocket::HandleAuthSession(WorldPacket& recvPacket)
{
    std::shared_ptr<AuthSession> authSession = std::make_shared<AuthSession>();

    // Read the content of the packet
    recvPacket >> authSession->Build;
    recvPacket >> authSession->LoginServerID;
    recvPacket >> authSession->Account;
    recvPacket >> authSession->LoginServerType;
    recvPacket.read(authSession->LocalChallenge);
    recvPacket >> authSession->RegionID;
    recvPacket >> authSession->BattlegroupID;
    recvPacket >> authSession->RealmID;               // realmId from auth_database.realmlist table
    recvPacket >> authSession->DosResponse;
    recvPacket.read(authSession->Digest);
    authSession->AddonInfo.resize(recvPacket.size() - recvPacket.rpos());
    recvPacket.read(authSession->AddonInfo.contents(), authSession->AddonInfo.size()); // .contents will throw if empty, thats what we want

    // Get the account information from the auth database
    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_ACCOUNT_INFO_BY_NAME);
    stmt->setInt32(0, int32(realm.Id.Realm));
    stmt->setString(1, authSession->Account);

    _queryProcessor.AddCallback(LoginDatabase.AsyncQuery(stmt).WithPreparedCallback([this, authSession = std::move(authSession)](PreparedQueryResult result) mutable
    {
        HandleAuthSessionCallback(std::move(authSession), std::move(result));
    }));
}

void WorldSocket::HandleAuthSessionCallback(std::shared_ptr<AuthSession> authSession, PreparedQueryResult result)
{
    // Stop if the account is not found
    if (!result)
    {
        // We can not log here, as we do not know the account. Thus, no accountId.
        SendAuthResponseError(AUTH_UNKNOWN_ACCOUNT);
        TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: Sent Auth Response (unknown account).");
        DelayedCloseSocket();
        return;
    }

    AccountInfo account(result->Fetch());

    // For hook purposes, we get Remoteaddress at this point.
    std::string address = GetRemoteIpAddress().to_string();

    LoginDatabasePreparedStatement* stmt = nullptr;

    if (sWorld->getBoolConfig(CONFIG_ALLOW_LOGGING_IP_ADDRESSES_IN_DATABASE))
    {
        // As we don't know if attempted login process by ip works, we update last_attempt_ip right away
        stmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_LAST_ATTEMPT_IP);
        stmt->setString(0, address);
        stmt->setString(1, authSession->Account);
        LoginDatabase.Execute(stmt);
        // This also allows to check for possible "hack" attempts on account
    }

    // even if auth credentials are bad, try using the session key we have - client cannot read auth response error without it
    _authCrypt.Init(account.SessionKey);

    // First reject the connection if packet contains invalid data or realm state doesn't allow logging in
    if (sWorld->IsClosed())
    {
        SendAuthResponseError(AUTH_REJECT);
        TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: World closed, denying client ({}).", GetRemoteIpAddress().to_string());
        DelayedCloseSocket();
        return;
    }

    if (authSession->RealmID != realm.Id.Realm)
    {
        SendAuthResponseError(REALM_LIST_REALM_NOT_FOUND);
        TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: Client {} requested connecting with realm id {} but this realm has id {} set in config.",
            GetRemoteIpAddress().to_string(), authSession->RealmID, realm.Id.Realm);
        DelayedCloseSocket();
        return;
    }

    // Must be done before WorldSession is created
    bool wardenActive = sWorld->getBoolConfig(CONFIG_WARDEN_ENABLED);
    if (wardenActive && !ClientBuild::Platform::IsValid(account.OS))
    {
        SendAuthResponseError(AUTH_REJECT);
        TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: Client {} attempted to log in using invalid client OS ({}).", address, account.OS);
        DelayedCloseSocket();
        return;
    }

    // Check that Key and account name are the same on client and server
    uint8 t[4] = { 0x00,0x00,0x00,0x00 };

    Trinity::Crypto::SHA1 sha;
    sha.UpdateData(authSession->Account);
    sha.UpdateData(t);
    sha.UpdateData(authSession->LocalChallenge);
    sha.UpdateData(_authSeed);
    sha.UpdateData(account.SessionKey);
    sha.Finalize();

    if (sha.GetDigest() != authSession->Digest)
    {
        SendAuthResponseError(AUTH_FAILED);
        TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: Authentication failed for account: {} ('{}') address: {}", account.Id, authSession->Account, address);
        DelayedCloseSocket();
        return;
    }

    if (IpLocationRecord const* location = sIPLocation->GetLocationRecord(address))
        _ipCountry = location->CountryCode;

    ///- Re-check ip locking (same check as in auth).
    if (account.IsLockedToIP)
    {
        if (account.LastIP != address)
        {
            SendAuthResponseError(AUTH_FAILED);
            TC_LOG_DEBUG("network", "WorldSocket::HandleAuthSession: Sent Auth Response (Account IP differs. Original IP: {}, new IP: {}).", account.LastIP, address);
            // We could log on hook only instead of an additional db log, however action logger is config based. Better keep DB logging as well
            sScriptMgr->OnFailedAccountLogin(account.Id);
            DelayedCloseSocket();
            return;
        }
    }
    else if (!account.LockCountry.empty() && account.LockCountry != "00" && !_ipCountry.empty())
    {
        if (account.LockCountry != _ipCountry)
        {
            SendAuthResponseError(AUTH_FAILED);
            TC_LOG_DEBUG("network", "WorldSocket::HandleAuthSession: Sent Auth Response (Account country differs. Original country: {}, new country: {}).", account.LockCountry, _ipCountry);
            // We could log on hook only instead of an additional db log, however action logger is config based. Better keep DB logging as well
            sScriptMgr->OnFailedAccountLogin(account.Id);
            DelayedCloseSocket();
            return;
        }
    }

    int64 mutetime = account.MuteTime;
    //! Negative mutetime indicates amount of seconds to be muted effective on next login - which is now.
    if (mutetime < 0)
    {
        mutetime = GameTime::GetGameTime() + std::llabs(mutetime);

        stmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_MUTE_TIME_LOGIN);
        stmt->setInt64(0, mutetime);
        stmt->setUInt32(1, account.Id);
        LoginDatabase.Execute(stmt);
    }

    if (account.IsBanned)
    {
        SendAuthResponseError(AUTH_BANNED);
        TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: Sent Auth Response (Account banned).");
        sScriptMgr->OnFailedAccountLogin(account.Id);
        DelayedCloseSocket();
        return;
    }

    // Check locked state for server
    AccountTypes allowedAccountType = sWorld->GetPlayerSecurityLimit();
    TC_LOG_DEBUG("network", "Allowed Level: {} Player Level {}", allowedAccountType, account.Security);
    if (allowedAccountType > SEC_PLAYER && account.Security < allowedAccountType)
    {
        SendAuthResponseError(AUTH_UNAVAILABLE);
        TC_LOG_DEBUG("network", "WorldSocket::HandleAuthSession: User tries to login but his security level is not enough");
        sScriptMgr->OnFailedAccountLogin(account.Id);
        DelayedCloseSocket();
        return;
    }

    TC_LOG_DEBUG("network", "WorldSocket::HandleAuthSession: Client '{}' authenticated successfully from {}.", authSession->Account, address);

    if (sWorld->getBoolConfig(CONFIG_ALLOW_LOGGING_IP_ADDRESSES_IN_DATABASE))
    {
        // Update the last_ip in the database as it was successful for login
        stmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_LAST_IP);

        stmt->setString(0, address);
        stmt->setString(1, authSession->Account);

        LoginDatabase.Execute(stmt);
    }

    // At this point, we can safely hook a successful login
    sScriptMgr->OnAccountLogin(account.Id);

    _authed = true;
    _worldSession = new WorldSession(account.Id, std::move(authSession->Account), shared_from_this(), account.Security,
        account.Expansion, mutetime, account.TimezoneOffset, account.Locale, account.Recruiter, account.IsRectuiter);
    _worldSession->ReadAddonsInfo(authSession->AddonInfo);

    // Initialize Warden system only if it is enabled by config
    if (wardenActive)
        _worldSession->InitWarden(account.SessionKey, account.OS);

    _queryProcessor.AddCallback(_worldSession->LoadPermissionsAsync().WithPreparedCallback(std::bind(&WorldSocket::LoadSessionPermissionsCallback, this, std::placeholders::_1)));
    AsyncRead();
}

void WorldSocket::LoadSessionPermissionsCallback(PreparedQueryResult result)
{
    // RBAC must be loaded before adding session to check for skip queue permission
    _worldSession->GetRBACData()->LoadFromDBCallback(result);

    sWorld->AddSession(_worldSession);
}

void WorldSocket::SendAuthResponseError(uint8 code)
{
    WorldPacket packet(SMSG_AUTH_RESPONSE, 1);
    packet << uint8(code);

    SendPacketAndLogOpcode(packet);
}

bool WorldSocket::HandlePing(WorldPacket& recvPacket)
{
    using namespace std::chrono;

    uint32 ping;
    uint32 latency;

    // Get the ping packet content
    recvPacket >> ping;
    recvPacket >> latency;

    if (_LastPingTime == steady_clock::time_point())
    {
        _LastPingTime = steady_clock::now();
    }
    else
    {
        steady_clock::time_point now = steady_clock::now();

        steady_clock::duration diff = now - _LastPingTime;

        _LastPingTime = now;

        if (diff < seconds(27))
        {
            ++_OverSpeedPings;

            uint32 maxAllowed = sWorld->getIntConfig(CONFIG_MAX_OVERSPEED_PINGS);

            if (maxAllowed && _OverSpeedPings > maxAllowed)
            {
                std::unique_lock<std::mutex> sessionGuard(_worldSessionLock);

                if (_worldSession && !_worldSession->HasPermission(rbac::RBAC_PERM_SKIP_CHECK_OVERSPEED_PING))
                {
                    TC_LOG_ERROR("network", "WorldSocket::HandlePing: {} kicked for over-speed pings (address: {})",
                        _worldSession->GetPlayerInfo(), GetRemoteIpAddress().to_string());

                    return false;
                }
            }
        }
        else
            _OverSpeedPings = 0;
    }

    {
        std::lock_guard<std::mutex> sessionGuard(_worldSessionLock);

        if (_worldSession)
            _worldSession->SetLatency(latency);
        else
        {
            TC_LOG_ERROR("network", "WorldSocket::HandlePing: peer sent CMSG_PING, but is not authenticated or got recently kicked, address = {}", GetRemoteIpAddress().to_string());
            return false;
        }
    }

    WorldPacket packet(SMSG_PONG, 4);
    packet << ping;
    SendPacketAndLogOpcode(packet);
    return true;
}
