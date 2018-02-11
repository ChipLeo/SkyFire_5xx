/*
 * Copyright (C) 2011-2018 Project SkyFire <http://www.projectskyfire.org/>
 * Copyright (C) 2008-2018 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2018 MaNGOS <https://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
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

#include "Common.h"
#include "DatabaseEnv.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Opcodes.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "UpdateMask.h"
#include "Path.h"
#include "WaypointMovementGenerator.h"
#include "MovementStructures.h"

void WorldSession::HandleTaxiNodeStatusQueryOpcode(WorldPacket& recvData)
{
    SF_LOG_DEBUG("network", "WORLD: Received CMSG_TAXI_NODE_STATUS_QUERY");
        
    ObjectGuid guid;

    recvData.ReadGuidMask(guid, 7, 4, 1, 3, 0, 5, 2, 6);
    recvData.ReadGuidBytes(guid, 7, 1, 5, 2, 4, 0, 6, 3);

    SendTaxiStatus(guid);
}

void WorldSession::SendTaxiStatus(uint64 guid)
{
    // cheating checks
    Creature* unit = GetPlayer()->GetMap()->GetCreature(guid);
    if (!unit)
    {
        SF_LOG_DEBUG("network", "WorldSession::SendTaxiStatus - Unit (GUID: %u) not found.", uint32(GUID_LOPART(guid)));
        return;
    }

    uint32 curloc = sObjectMgr->GetNearestTaxiNode(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ(), unit->GetMapId(), GetPlayer()->GetTeam());

    // not found nearest
    if (curloc == 0)
        return;

    SF_LOG_DEBUG("network", "WORLD: current location %u ", curloc);

    TaxiNodeStatus status = TAXISTATUS_NONE;
    if (unit->GetReactionTo(GetPlayer()) >= REP_NEUTRAL)
        status = GetPlayer()->m_taxi.IsTaximaskNodeKnown(curloc) ? TAXISTATUS_LEARNED : TAXISTATUS_UNLEARNED;
    else
        status = TAXISTATUS_NOT_ELIGIBLE;

    ObjectGuid Guid = guid;
    WorldPacket data(SMSG_TAXI_NODE_STATUS, 1 + 1 + 8);

    data.WriteGuidMask(Guid, 6, 2, 7, 5, 4, 1);
    data.WriteBits(status, 2);
    data.WriteGuidMask(Guid, 3, 0);
    data.FlushBits();
    data.WriteGuidBytes(Guid, 0, 5, 2, 1, 4, 6, 7, 3);

    SendPacket(&data);

    SF_LOG_DEBUG("network", "WORLD: Sent SMSG_TAXI_NODE_STATUS");
}

void WorldSession::HandleTaxiQueryAvailableNodes(WorldPacket& recvData)
{
    SF_LOG_DEBUG("network", "WORLD: Received CMSG_TAXI_QUERY_AVAILABLE_NODES");

    ObjectGuid guid;

    recvData.ReadGuidMask(guid, 7, 1, 0, 4, 2, 5, 6, 3);
    recvData.ReadGuidBytes(guid, 0, 3, 7, 5, 2, 6, 4, 1);

    // cheating checks
    Creature* unit = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_FLIGHTMASTER);
    if (!unit)
    {
        SF_LOG_DEBUG("network", "WORLD: HandleTaxiQueryAvailableNodes - Unit (GUID: %u) not found or you can't interact with him.", uint32(GUID_LOPART(guid)));
        return;
    }

    // remove fake death
    if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
        GetPlayer()->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

    // unknown taxi node case
    if (SendLearnNewTaxiNode(unit))
        return;

    // known taxi node case
    SendTaxiMenu(unit);
}

void WorldSession::SendTaxiMenu(Creature* unit)
{
    // find current node
    uint32 curloc = sObjectMgr->GetNearestTaxiNode(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ(), unit->GetMapId(), GetPlayer()->GetTeam());
    if (!curloc)
        return;

    bool lastTaxiCheaterState = GetPlayer()->isTaxiCheater();
    if (unit->GetEntry() == 29480)
        GetPlayer()->SetTaxiCheater(true); // Grimwing in Ebon Hold, special case. NOTE: Not perfect, Zul'Aman should not be included according to WoWhead, and I think taxicheat includes it.

    SF_LOG_DEBUG("network", "WORLD: CMSG_TAXI_NODE_STATUS_QUERY %u ", curloc);
    ObjectGuid Guid = unit->GetGUID();

    bool ShowWindow = true;

    WorldPacket data(SMSG_SHOW_TAXI_NODES, (4 + 8 + 4 + 4 * 4));
    data.WriteBit(ShowWindow);
    if (ShowWindow)
        data.WriteGuidMask(Guid, 3, 0, 4, 2, 1, 7, 6, 5);
    data.WriteBits(TaxiMaskSize, 24);

    if (ShowWindow)
    {
        data.WriteGuidBytes(Guid, 0, 3);
        data << uint32(curloc);
        data.WriteGuidBytes(Guid, 5, 2, 6, 1, 7, 4);
    }
    GetPlayer()->m_taxi.AppendTaximaskTo(data, GetPlayer()->isTaxiCheater());
    SendPacket(&data);

    SF_LOG_DEBUG("network", "WORLD: Sent SMSG_SHOW_TAXI_NODES");

    GetPlayer()->SetTaxiCheater(lastTaxiCheaterState);
}

void WorldSession::SendDoFlight(uint32 mountDisplayId, uint32 path, uint32 pathNode)
{
    // remove fake death
    if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
        GetPlayer()->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

    while (GetPlayer()->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE)
        GetPlayer()->GetMotionMaster()->MovementExpired(false);

    if (mountDisplayId)
        GetPlayer()->Mount(mountDisplayId);

    GetPlayer()->GetMotionMaster()->MoveTaxiFlight(path, pathNode);
}

bool WorldSession::SendLearnNewTaxiNode(Creature* unit)
{
    // find current node
    uint32 curloc = sObjectMgr->GetNearestTaxiNode(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ(), unit->GetMapId(), GetPlayer()->GetTeam());

    if (curloc == 0)
        return true; 
    // `true` send to avoid WorldSession::SendTaxiMenu call with one more curlock seartch with same false result.

    if (GetPlayer()->m_taxi.SetTaximaskNode(curloc))
    {
        WorldPacket msg(SMSG_NEW_TAXI_PATH, 0);
        SendPacket(&msg);

        ObjectGuid guid = unit->GetGUID();

        SendTaxiStatus(guid);

        return true;
    }
    else
        return false;
}

void WorldSession::SendDiscoverNewTaxiNode(uint32 nodeid)
{
    if (GetPlayer()->m_taxi.SetTaximaskNode(nodeid))
    {
        WorldPacket msg(SMSG_NEW_TAXI_PATH, 0);
        SendPacket(&msg);
    }
}

void WorldSession::HandleActivateTaxiExpressOpcode(WorldPacket& recvData)
{
    SF_LOG_DEBUG("network", "WORLD: Received CMSG_ACTIVATE_TAXI_EXPRESS");

    ObjectGuid guid;
    uint32 node_count;

    recvData.ReadGuidMask(guid, 6, 7);
    node_count = recvData.ReadBits(22);
    printf("nodes_count [%u]\n", node_count);
    recvData.ReadGuidMask(guid, 2, 0, 4, 3, 1, 5);
    recvData.ReadGuidBytes(guid, 2, 7, 1);

    std::vector<uint32> nodes;

    for (uint32 i = 0; i < node_count; ++i)
    {
        uint32 node;
        recvData >> node;
        if (!GetPlayer()->m_taxi.IsTaximaskNodeKnown(node) && !GetPlayer()->isTaxiCheater())
        {
            SendActivateTaxiReply(ERR_TAXINOTVISITED);
            recvData.rfinish();
            return;
        }
        nodes.push_back(node);
    }

    if (nodes.empty())
    {
        recvData.rfinish();
        return;
    }

    recvData.ReadGuidBytes(guid, 0, 5, 3, 6, 4);

    Creature* npc = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_FLIGHTMASTER);
    if (!npc)
    {
        SF_LOG_DEBUG("network", "WORLD: HandleActivateTaxiExpressOpcode - Unit (GUID: %u) not found or you can't interact with it.", uint32(GUID_LOPART(guid)));
        SendActivateTaxiReply(ERR_TAXITOOFARAWAY);
        return;
    }

    SF_LOG_DEBUG("network", "WORLD: Received CMSG_ACTIVATE_TAXI_EXPRESS from %d to %d", nodes.front(), nodes.back());

    GetPlayer()->ActivateTaxiPathTo(nodes, npc);
}

void WorldSession::HandleMoveSplineDoneOpcode(WorldPacket& recvData)
{
    SF_LOG_DEBUG("network", "WORLD: Received CMSG_MOVE_SPLINE_DONE");

    static MovementStatusElements const SplineID = MSEExtraInt32;
    Movement::ExtraMovementStatusElement extra(&SplineID);
    MovementInfo movementInfo;                              // used only for proper packet read
    _player->ReadMovementInfo(recvData, &movementInfo, &extra);

    // in taxi flight packet received in 2 case:
    // 1) end taxi path in far (multi-node) flight
    // 2) switch from one map to other in case multim-map taxi path
    // we need process only (1)

    uint32 curDest = GetPlayer()->m_taxi.GetTaxiDestination();
    if (!curDest)
        return;

    TaxiNodesEntry const* curDestNode = sTaxiNodesStore.LookupEntry(curDest);

    // far teleport case
    if (curDestNode && curDestNode->map_id != GetPlayer()->GetMapId())
    {
        if (GetPlayer()->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE)
        {
            // short preparations to continue flight
            FlightPathMovementGenerator* flight = (FlightPathMovementGenerator*)(GetPlayer()->GetMotionMaster()->top());

            flight->SetCurrentNodeAfterTeleport();
            TaxiPathNodeEntry const& node = flight->GetPath()[flight->GetCurrentNode()];
            flight->SkipCurrentNode();

            GetPlayer()->TeleportTo(curDestNode->map_id, node.x, node.y, node.z, GetPlayer()->GetOrientation());
        }
        return;
    }

    uint32 destinationnode = GetPlayer()->m_taxi.NextTaxiDestination();
    if (destinationnode > 0)                              // if more destinations to go
    {
        // current source node for next destination
        uint32 sourcenode = GetPlayer()->m_taxi.GetTaxiSource();

        // Add to taximask middle hubs in taxicheat mode (to prevent having player with disabled taxicheat and not having back flight path)
        if (GetPlayer()->isTaxiCheater())
        {
            if (GetPlayer()->m_taxi.SetTaximaskNode(sourcenode))
            {
                WorldPacket data(SMSG_NEW_TAXI_PATH, 0);
                _player->SendDirectMessage(&data);
            }
        }

        SF_LOG_DEBUG("network", "WORLD: Taxi has to go from %u to %u", sourcenode, destinationnode);

        uint32 mountDisplayId = sObjectMgr->GetTaxiMountDisplayId(sourcenode, GetPlayer()->GetTeam());

        uint32 path, cost;
        sObjectMgr->GetTaxiPath(sourcenode, destinationnode, path, cost);

        if (path && mountDisplayId)
            SendDoFlight(mountDisplayId, path, 1);        // skip start fly node
        else
            GetPlayer()->m_taxi.ClearTaxiDestinations();    // clear problematic path and next
        return;
    }
    else
        GetPlayer()->m_taxi.ClearTaxiDestinations();        // not destinations, clear source node

    GetPlayer()->CleanupAfterTaxiFlight();
    GetPlayer()->SetFallInformation(0, GetPlayer()->GetPositionZ());
    if (GetPlayer()->pvpInfo.IsHostile)
        GetPlayer()->CastSpell(GetPlayer(), 2479, true);
}

void WorldSession::HandleActivateTaxiOpcode(WorldPacket& recvData)
{
    SF_LOG_DEBUG("network", "WORLD: Received CMSG_ACTIVATE_TAXI");

    ObjectGuid guid;

    uint32 FromNode = 0;
    uint32 ToNode = 0;

    recvData >> ToNode;
    recvData >> FromNode;
    recvData.ReadGuidMask(guid, 4, 0, 1, 2, 5, 6, 7, 3);
    recvData.ReadGuidBytes(guid, 1, 0, 6, 5, 2, 4, 3, 7);

    SF_LOG_DEBUG("network", "WORLD: Received CMSG_ACTIVATE_TAXI from %d to %d", FromNode, ToNode);
    Creature* npc = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_FLIGHTMASTER);
    if (!npc)
    {
        SF_LOG_DEBUG("network", "WORLD: HandleActivateTaxiOpcode - Unit (GUID: %u) not found or you can't interact with it.", uint32(GUID_LOPART(guid)));
        SendActivateTaxiReply(ERR_TAXITOOFARAWAY);
        return;
    }

    TaxiNodesEntry const* from = sTaxiNodesStore.LookupEntry(FromNode);
    TaxiNodesEntry const* to = sTaxiNodesStore.LookupEntry(ToNode);
    if (!to)
        return;

    if (!GetPlayer()->isTaxiCheater())
    {
        if (!GetPlayer()->m_taxi.IsTaximaskNodeKnown(FromNode) || !GetPlayer()->m_taxi.IsTaximaskNodeKnown(ToNode))
        {
            SendActivateTaxiReply(ERR_TAXINOTVISITED);
            return;
        }
    }

    std::vector<uint32> nodes;
    nodes.resize(2);
    nodes[1] = ToNode;
    nodes[0] = FromNode;
    GetPlayer()->ActivateTaxiPathTo(nodes, npc);
}

void WorldSession::SendActivateTaxiReply(ActivateTaxiReply reply)
{
    WorldPacket data(SMSG_ACTIVATE_TAXI_REPLY);

    data.WriteBits(reply, 4);
    data.FlushBits();

    SendPacket(&data);

    SF_LOG_DEBUG("network", "WORLD: Sent SMSG_ACTIVATE_TAXI_REPLY");
}

void WorldSession::HandleSetTaxiBenchmarkOpcode(WorldPacket& recvData)
{
    uint8 mode = 0;

    recvData >> mode;

    mode ? _player->SetFlag(PLAYER_FIELD_PLAYER_FLAGS, PLAYER_FLAGS_TAXI_BENCHMARK) : _player->RemoveFlag(PLAYER_FIELD_PLAYER_FLAGS, PLAYER_FLAGS_TAXI_BENCHMARK);

    SF_LOG_DEBUG("network", "Client used \"/timetest %d\" command", mode);
}
