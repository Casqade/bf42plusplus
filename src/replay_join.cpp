// A file begun after the join. Recording can be switched on at any time
// (plus.recordReplays 1), and a file that begins mid-round has missed the
// join: the server, its mod, the level, the rules, and every player's
// createPlayer, which is where names come from. So the join's events, and
// what it made, are held as they pass whether or not a file is open, and a
// file that begins after them opens with them (writeHeldJoin). A new join
// lets the last one's go (forgetJoin).

#include "replay_internal.h"

#include "bf/gameevent.h"
#include "bf/generic.h"
#include "debug.h"

#include <windows.h>

#include <algorithm>
#include <format>
#include <map>
#include <string>
#include <vector>

// What the last join said, for a file that begins after it. Recording can be
// switched on at any time (plus.recordReplays 1), and a file that begins
// mid-round has missed the join: the server, its mod, the level and its game
// type, the rules, and every player's createPlayer, which is where names come
// from. The join's events are kept as they pass whether or not a file is open,
// and a file that begins after them opens with them and with who is playing
// (replay_start). A new join (0x1A) lets the last one's go.
struct HeldEvent {
    int type;
    DWORD tick;         // GetTickCount() when it arrived
    std::string line;   // the record as written, at t 0
};
static std::vector<HeldEvent> g_heldJoin;

// The objects the join made, and every one since, held the same way and let
// go with the join's events. CreateObject (0x07) comes once for every object,
// the join's database sending every one that stands, but the sampler names
// only what the server replicates, within the view distance: a file begun
// after the join would know a carrier across the map only as the root id of
// its turrets and engines. Held: each object's createObject until a
// DestroyObject (0x06) for its id; each projectile pool (0x05) until one for
// its first id (a pool goes all at once); and each player's latest pickupKit
// (0x23) until that kit is destroyed or he leaves (0x0C), a player id passing
// to whoever joins next. And each player's latest createPlayer (0x08) until
// he leaves: the join's database gives a player already spawned his soldier
// and his kit there (vehNetId, kitNetId), and sends no pickupKit for him.
struct HeldRecord {
    uint32_t order;     // arrival, across all four
    DWORD tick;         // GetTickCount() when it arrived
    uint16_t netId;     // the object, the pool's first id, the kit, or the player's own
    std::string line;   // the record as written, at t 0
};
static std::map<int, HeldRecord> g_heldPlayers;        // by player id
static std::map<uint16_t, HeldRecord> g_heldCreates;   // by network id
static std::map<uint16_t, HeldRecord> g_heldPools;     // by first network id
static std::map<int, HeldRecord> g_heldKits;           // by player id
static uint32_t g_heldOrder = 0;

// Who is playing, as `[pid, team, ai, name, local]` (local 1 for the recording
// player): for a file that begins after the join, whose players' createPlayer
// events all went by before it.
static void writeRoster(double t)
{
    auto players = BFPlayer::getPlayers();
    if (!players) return;
    const BFPlayer* local = BFPlayer::getLocal();
    std::string out;
    for (auto it = players->begin(); it != players->end(); ++it) {
        BFPlayer* player = *it;
        if (!player) continue;
        if (!out.empty()) out += ',';
        out += std::format("[{},{},{},\"{}\",{}]", player->getId(), player->getTeam(),
            player->getIsAIPlayer() ? 1 : 0, jsonEscape(std::string(player->getName())), player == local ? 1 : 0);
    }
    if (!out.empty()) writeLine(std::format("{{\"k\":\"roster\",\"t\":{:.3f},\"p\":[{}]}}", t, out));
}

// A held record written into the file with "ago", the seconds before `now`
// that it arrived.
static void writeHeld(const std::string& line, DWORD tick, DWORD now)
{
    std::string out = line;
    out.insert(out.size() - 1, std::format(",\"ago\":{:.3f}", (now - tick) / 1000.0));
    writeLine(out);
}

// Held records in the order they arrived.
template <class Key>
static void writeHeldInOrder(const std::map<Key, HeldRecord>& held, DWORD now)
{
    std::vector<const HeldRecord*> ordered;
    ordered.reserve(held.size());
    for (const auto& kv : held) ordered.push_back(&kv.second);
    std::sort(ordered.begin(), ordered.end(), [](const HeldRecord* a, const HeldRecord* b) { return a->order < b->order; });
    for (const HeldRecord* record : ordered) writeHeld(record->line, record->tick, now);
}

// Keep the join's events a file that begins after them opens with
// (g_heldJoin), the latest of each kind. Not the world clock (0x04): its time
// is the join's, and moved to the file's start it would be wrong.
void holdJoinEvent(GameEvent* event)
{
    int type = event->getType();
    std::string line;
    switch (type) {
        case BF_SetLevelEvent:
            line = setLevelLine(event, 0.0);
            break;
        case 0x14:  // ChallengeEvent: the mod and pack the server runs
        case 0x16:  // GameRulesEvent
        case 0x1A:  // ServerInfoEvent: the server's mod
        case 0x1B: {  // ServerInfoEvent2: the server's name
            size_t size = eventSize(type);
            if (size) line = decodeEvent(type, (const uint8_t*)event + 12, size, 0.0);
            break;
        }
        default:
            return;
    }
    if (line.empty()) return;
    const HeldEvent held{ type, GetTickCount(), line };
    for (auto& h : g_heldJoin) {
        if (h.type == type) {
            h = held;
            return;
        }
    }
    g_heldJoin.push_back(held);
}

// Keep the players, the objects standing, their pools and the players' kits
// for a file that begins after them (g_heldPlayers, g_heldCreates,
// g_heldPools, g_heldKits). A record for an id already held replaces it and
// takes its new place in the order.
void holdObjectEvent(GameEvent* event)
{
    int type = event->getType();
    const uint8_t* payload = (const uint8_t*)event + 12;
    switch (type) {
        case BF_CreatePlayerEvent: {
            auto ev = reinterpret_cast<CreatePlayerEvent*>(event);
            g_heldPlayers[ev->playerID] = HeldRecord{ g_heldOrder++, GetTickCount(), ev->playerNetworkID, createPlayerLine(event, 0.0) };
            break;
        }
        case BF_CreateObjectEvent: {
            uint16_t id = reinterpret_cast<CreateObjectEvent*>(event)->objectNetId;
            g_heldCreates[id] = HeldRecord{ g_heldOrder++, GetTickCount(), id, createObjectLine(event, 0.0) };
            break;
        }
        case 0x05: {  // CreateMultipleObjectsEvent: a kit's projectile pool
            size_t size = eventSize(type);
            std::string line = size ? decodeEvent(type, payload, size, 0.0) : std::string();
            if (line.empty()) break;
            uint16_t first = readAt<uint16_t>(payload, 4);
            g_heldPools[first] = HeldRecord{ g_heldOrder++, GetTickCount(), first, std::move(line) };
            break;
        }
        case BF_PickupKitEvent: {
            auto ev = reinterpret_cast<PickupKitEvent*>(event);
            g_heldKits[ev->playerID] = HeldRecord{ g_heldOrder++, GetTickCount(), ev->kitNetId, pickupKitLine(event, 0.0) };
            break;
        }
        case BF_DestroyObjectEvent: {
            uint16_t id = reinterpret_cast<DestroyObjectEvent*>(event)->objectNetId;
            g_heldCreates.erase(id);
            g_heldPools.erase(id);
            std::erase_if(g_heldKits, [id](const auto& kv) { return kv.second.netId == id; });
            break;
        }
        case BF_DestroyPlayerEvent: {
            int pid = reinterpret_cast<DestroyPlayerEvent*>(event)->playerid;
            g_heldPlayers.erase(pid);
            g_heldKits.erase(pid);
            break;
        }
        default:
            break;
    }
}

void forgetJoin()
{
    g_heldJoin.clear();
    g_heldPlayers.clear();
    g_heldCreates.clear();
    g_heldPools.clear();
    g_heldKits.clear();
}

// A file that begins after the join opens with what the join said, the
// players, the objects standing, their projectile pools and the players'
// kits, and who is playing. One that begins with the join holds nothing
// yet (its 0x1A let the last join's go) and records the join as it comes.
void writeHeldJoin()
{
    if (g_heldJoin.empty() && g_heldPlayers.empty() && g_heldCreates.empty() && g_heldPools.empty()
        && g_heldKits.empty()) return;
    DWORD now = GetTickCount();
    for (const auto& held : g_heldJoin) writeHeld(held.line, held.tick, now);
    writeHeldInOrder(g_heldPlayers, now);
    writeHeldInOrder(g_heldCreates, now);
    writeHeldInOrder(g_heldPools, now);
    writeHeldInOrder(g_heldKits, now);
    writeRoster(replayNow());
    debuglogt("replay: began after the join: %zu of its events, %zu players, %zu objects, %zu pools, %zu kits and the roster written\n",
        g_heldJoin.size(), g_heldPlayers.size(), g_heldCreates.size(), g_heldPools.size(), g_heldKits.size());
}
