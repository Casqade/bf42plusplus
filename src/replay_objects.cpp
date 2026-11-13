// One sample of the world's root objects and what stands around them: where
// every networked object is (`o`, `s`, `d`), its hit points (`a`), the
// players' sides, seats and triggers (`p`), the recording player's hit mark
// (`hm`), the control points (`cp`) and the two sides' tickets (`tk`). Each
// record lists only what changed since it was last written.

#include "replay_internal.h"

#include "bf/generic.h"
#include "bf/object.h"
#include "bf/stl.h"
#include "debug.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <string>

struct ObjectState {
    Pos3 pos;
    Quat rot;
    int occupant;   // player id or -1
    bool seenThisSample;
    bool hasArmor;
    float hitPoints;
    int lastHitPlayer;
};
static std::map<uint16_t, ObjectState> g_objects;

struct PlayerState {
    int team;
    int vehicleNetId;   // -1 if none
    int triggers;       // 1 fire held, 2 altfire held: the server's flags on BFPlayer
};
static std::map<int, PlayerState> g_players;
static double g_hitLastAt = -1e9;   // the latest hit mark's reconstructed time ("hm")

struct ControlPointState {
    int team;
};
static std::map<int, ControlPointState> g_controlPoints;
// The two sides' tickets as last written (-1: nothing yet).
static int g_tickets[2] = { -1, -1 };

void resetObjectSampler()
{
    g_objects.clear();
    g_players.clear();
    g_hitLastAt = -1e9;
    g_controlPoints.clear();
    g_tickets[0] = g_tickets[1] = -1;
}

// Armor, the component that holds an object's hit points. Layout confirmed in
// BF1942.exe (vtable 0x008DC7A8) and against the Linux server's symbols, where
// the same getters sit in the same slots reading the same offsets:
//     slot  5 getMaxHitPoints    fld [ecx+3Ch]
//     slot  7 getHitPoints       fld [ecx+38h]
//     slot 11 getCriticalDamage  fld [ecx+0F0h]
//     slot 30 getLastHitPlayer   mov eax, [ecx+14h]
// IObject::queryComponent(IID_IArmor, IID_IArmor) returns it; the client makes
// that exact call itself at 84 sites.
static const unsigned int IID_IArmor = 0xc4a4;

struct ArmorFields {
    float hitPoints;
    float maxHitPoints;
    float criticalDamage;   // below this the object burns and loses hit points on its own
    int lastHitPlayer;      // -1 on the client even for kills: set server-side, not replicated
};

// True when this vtable's getters read the offsets readArmor reads directly.
// Checked once per vtable, so an Armor subclass with a different layout is
// skipped rather than misread.
static bool armorVtableMatches(uintptr_t vtable)
{
    static std::map<uintptr_t, bool> checked;
    auto it = checked.find(vtable);
    if (it != checked.end()) return it->second;

    struct Getter {
        int slot;
        uint8_t code[7];
        size_t length;
    };
    static const Getter expected[] = {
        { 5,  { 0xD9, 0x41, 0x3C, 0xC3 }, 4 },
        { 7,  { 0xD9, 0x41, 0x38, 0xC3 }, 4 },
        { 11, { 0xD9, 0x81, 0xF0, 0x00, 0x00, 0x00, 0xC3 }, 7 },
        { 30, { 0x8B, 0x41, 0x14, 0xC3 }, 4 },
    };
    bool ok = true;
    for (auto& getter : expected) {
        uintptr_t fn = 0;
        uint8_t code[7];
        if (safeCopy(&fn, (void*)(vtable + 4 * getter.slot), sizeof(fn)) != sizeof(fn)
            || safeCopy(code, (void*)fn, getter.length) != getter.length
            || memcmp(code, getter.code, getter.length) != 0) {
            ok = false;
            break;
        }
    }
    checked[vtable] = ok;
    if (!ok) debuglogt("replay: armor vtable %p has unexpected getters, its hit points are not recorded\n", (void*)vtable);
    return ok;
}

static bool readArmor(IObject* obj, ArmorFields& out)
{
    void* armor = obj->queryComponent(IID_IArmor, IID_IArmor);
    if (!armor) return false;
    uintptr_t vtable = 0;
    if (safeCopy(&vtable, armor, sizeof(vtable)) != sizeof(vtable) || !armorVtableMatches(vtable)) return false;
    const uint8_t* base = (const uint8_t*)armor;
    return safeCopy(&out.hitPoints, base + 0x38, 4) == 4
        && safeCopy(&out.maxHitPoints, base + 0x3C, 4) == 4
        && safeCopy(&out.criticalDamage, base + 0xF0, 4) == 4
        && safeCopy(&out.lastHitPlayer, base + 0x14, 4) == 4;
}

// The network id of the root object `obj` hangs under (itself for a root),
// or -1: a seat's hull for the player record.
static int rootNetIdOf(IObject* obj)
{
    __try {
        IObject* root = obj;
        for (int depth = 0; root && root->getParent() && depth < 16; depth++) root = root->getParent();
        auto net = root ? root->getNetworkable() : nullptr;
        return net ? (int)net->getID() : -1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Rotation of the object's absolute transform as a unit quaternion. Rows a, b, c
// of Mat4 are taken as the X, Y, Z basis vectors. If a viewer ends up mirrored
// the fix belongs there, not here: this is what the engine holds.
Quat toQuat(const Mat4& m)
{
    float m00 = m.a.x, m01 = m.a.y, m02 = m.a.z;
    float m10 = m.b.x, m11 = m.b.y, m12 = m.b.z;
    float m20 = m.c.x, m21 = m.c.y, m22 = m.c.z;
    float trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m12 - m21) / s;
        q.y = (m20 - m02) / s;
        q.z = (m01 - m10) / s;
    }
    else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m12 - m21) / s;
        q.x = 0.25f * s;
        q.y = (m10 + m01) / s;
        q.z = (m20 + m02) / s;
    }
    else if (m11 > m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m20 - m02) / s;
        q.x = (m10 + m01) / s;
        q.y = 0.25f * s;
        q.z = (m21 + m12) / s;
    }
    else {
        float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m01 - m10) / s;
        q.x = (m20 + m02) / s;
        q.y = (m21 + m12) / s;
        q.z = 0.25f * s;
    }
    return q;
}

bool changed(const Pos3& a, const Pos3& b, float eps)
{
    return fabsf(a.x - b.x) > eps || fabsf(a.y - b.y) > eps || fabsf(a.z - b.z) > eps;
}

bool changed(const Quat& a, const Quat& b, float eps)
{
    return fabsf(a.x - b.x) > eps || fabsf(a.y - b.y) > eps || fabsf(a.z - b.z) > eps || fabsf(a.w - b.w) > eps;
}

// The fire and altfire flags the server replicates on every BFPlayer (+0x184,
// +0x185; written by the BFPlayer networkable's setNetUpdate at 0x00407B9B and
// 0x00407BAF, read by GameClient's per-tick fire loop at 0x004B9143): the
// server's trigger timeline for every player, the bots' included.
static int playerTriggers(BFPlayer* player)
{
    uint8_t flags[2] = {};
    if (safeCopy(flags, (const uint8_t*)player + 0x184, sizeof(flags)) != sizeof(flags)) return 0;
    return (flags[0] ? 1 : 0) | (flags[1] ? 2 : 0);
}

// The recording player's crosshair hit mark. BF1942.exe keeps it as a float on
// the BFPlayer at +0x1cc (CrossHair/HitIndicationTime is copied from it every
// frame, 0x006aeba1). It is only ever SET to 1.0 (the dedicated server's
// control-state bool at 0x00495a24, the listen server at 0x004a888f, the
// SpecialGameEvent 1 handler at 0x00495616) and run down by the frame's dt and
// held at 0 (0x00406890). So a sample of v > 0 at t puts the latest hit at
// at = t - (1 - v).
//
// A new mark is a sample whose "at" is later than the previous sample's by
// more than 50 ms. That is the same test as v > prev - (t - prevT) + 0.05: it
// takes a plain rise, and a second hit that keeps the timer at (or near) 1.0
// across samples (its "at" moves on although v does not go up). The 50 ms
// absorbs the game's frame dt drifting from this clock. What it cannot see:
// two hits between one pair of samples (one line, the later hit), and a hit
// within 50 ms of the one before. A frame hitch where the game's dt runs short
// of the wall clock can read as a mark. The first sample of a file with v > 0
// is written too (a hit in the second before it began).
static void sampleHitMark(double t)
{
    BFPlayer* local = BFPlayer::getLocal();
    if (!local) return;
    float v = 0.0f;
    if (safeCopy(&v, (const uint8_t*)local + 0x1cc, sizeof(v)) != sizeof(v) || !(v > 0.0f && v <= 1.0f)) return;
    double at = t - (1.0 - v);
    bool mark = at > g_hitLastAt + 0.05;
    g_hitLastAt = at;
    if (mark)
        writeLine(std::format("{{\"k\":\"hm\",\"t\":{:.3f},\"pid\":{},\"at\":{:.3f}}}", t, local->getId(), at));
}

void samplePlayers(double t)
{
    auto players = BFPlayer::getPlayers();
    if (!players) return;

    std::string out;
    for (auto it = players->begin(); it != players->end(); ++it) {
        BFPlayer* player = *it;
        if (!player) continue;
        PlayerState s;
        s.team = player->getTeam();
        s.vehicleNetId = -1;
        s.triggers = playerTriggers(player);
        int rootNetId = -1;
        if (auto vehicle = player->getVehicle()) {
            if (auto net = vehicle->getNetworkable()) s.vehicleNetId = net->getID();
            rootNetId = rootNetIdOf(vehicle);
        }
        int pid = player->getId();
        auto prev = g_players.find(pid);
        if (prev != g_players.end() && prev->second.team == s.team && prev->second.vehicleNetId == s.vehicleNetId
            && prev->second.triggers == s.triggers) continue;
        g_players[pid] = s;
        if (!out.empty()) out += ',';
        // The hull the controlled object hangs under, and the seat's place
        // after it (a hull's nested PlayerControlObjects take the ids after
        // its own; 0 is the root seat, and the soldier and camera are roots).
        int seat = rootNetId >= 0 && s.vehicleNetId >= rootNetId ? s.vehicleNetId - rootNetId : 0;
        out += std::format("[{},{},{},{},{},{}]", pid, s.team, s.vehicleNetId, rootNetId, seat, s.triggers);
    }
    if (!out.empty()) writeLine(std::format("{{\"k\":\"p\",\"t\":{:.3f},\"p\":[{}]}}", t, out));
    sampleHitMark(t);
}

void sampleControlPoints(double t)
{
    auto& controlPoints = ObjectManager_getControlPointVector();
    for (auto it = controlPoints.begin(); it != controlPoints.end(); ++it) {
        auto cp = *it;
        if (!cp) continue;
        int id = cp->getID();
        int team = *(int*)((uintptr_t)cp + 0x190);   // same offset renderer.cpp reads for the 3D map
        auto prev = g_controlPoints.find(id);
        if (prev != g_controlPoints.end() && prev->second.team == team) continue;
        bool isNew = prev == g_controlPoints.end();
        g_controlPoints[id].team = team;
        auto& pos = cp->getAbsolutePosition();
        if (isNew) {
            auto& cpName = *(bfs::string*)((uintptr_t)cp->getTemplate() + 0x2D0);
            writeLine(std::format("{{\"k\":\"cp\",\"t\":{:.3f},\"id\":{},\"name\":\"{}\",\"tmpl\":\"{}\",\"pos\":[{:.2f},{:.2f},{:.2f}],\"team\":{}}}",
                t, id, jsonEscape(std::string(cpName)), jsonEscape(std::string(cp->getTemplate()->getName())),
                pos.x, pos.y, pos.z, team));
        }
        else {
            writeLine(std::format("{{\"k\":\"cp\",\"t\":{:.3f},\"id\":{},\"team\":{}}}", t, id, team));
        }
    }
}

void sampleObjects(double t)
{
    for (auto& kv : g_objects) kv.second.seenThisSample = false;

    std::string out;
    std::string armorOut;
    auto addArmor = [&armorOut](uint16_t netId, const ArmorFields& armor) {
        if (!armorOut.empty()) armorOut += ',';
        armorOut += std::format("[{},{:.1f},{}]", netId, armor.hitPoints, armor.lastHitPlayer);
    };
    auto& allObjs = ObjectManager_getAllRegisteredObjects();
    for (auto node = allObjs.head->left; node != allObjs.head; node = node->next()) {
        auto obj = node->pair.second;
        if (!obj) continue;
        // Root objects only (children ride with their parent), skip disabled ones.
        uint32_t flags = obj->getFlags();
        if (!(flags & 0x02000000) || (flags & 1)) continue;
        auto net = obj->getNetworkable();
        if (!net) continue;

        uint16_t netId = net->getID();
        auto& m = obj->getAbsoluteTransformation();
        Pos3 pos(m.position.x, m.position.y, m.position.z);
        Quat rot = toQuat(m);
        ArmorFields armor{};
        bool hasArmor = readArmor(obj, armor);

        auto prev = g_objects.find(netId);
        if (prev == g_objects.end()) {
            auto tmpl = obj->getTemplate();
            // Max hit points and the critical-damage threshold are fixed per
            // object, so they ride on the first-seen record.
            std::string armorFields = hasArmor
                ? std::format(",\"maxhp\":{:.1f},\"crit\":{:.1f}", armor.maxHitPoints, armor.criticalDamage)
                : std::string();
            writeLine(std::format("{{\"k\":\"o\",\"t\":{:.3f},\"id\":{},\"gid\":{},\"tmpl\":\"{}\",\"tid\":{},\"team\":{}{}}}",
                t, netId, obj->getID(), tmpl ? jsonEscape(std::string(tmpl->getName())) : std::string(),
                tmpl ? tmpl->getId() : 0, obj->getTeam(), armorFields));
            g_objects[netId] = ObjectState{ pos, rot, -1, true, hasArmor, armor.hitPoints, armor.lastHitPlayer };
            if (!out.empty()) out += ',';
            out += std::format("[{},{:.2f},{:.2f},{:.2f},{:.3f},{:.3f},{:.3f},{:.3f}]",
                netId, pos.x, pos.y, pos.z, rot.x, rot.y, rot.z, rot.w);
            if (hasArmor) addArmor(netId, armor);
            continue;
        }

        auto& s = prev->second;
        s.seenThisSample = true;
        // Hit points change on every hit, and fall steadily on their own once
        // below the critical-damage threshold (burning), so this is also the
        // record of smoke, fire and the moment of destruction.
        if (hasArmor && (!s.hasArmor || fabsf(armor.hitPoints - s.hitPoints) > 0.05f || armor.lastHitPlayer != s.lastHitPlayer)) {
            s.hasArmor = true;
            s.hitPoints = armor.hitPoints;
            s.lastHitPlayer = armor.lastHitPlayer;
            addArmor(netId, armor);
        }
        if (!changed(s.pos, pos, 0.01f) && !changed(s.rot, rot, 0.002f)) continue;
        s.pos = pos;
        s.rot = rot;
        if (!out.empty()) out += ',';
        out += std::format("[{},{:.2f},{:.2f},{:.2f},{:.3f},{:.3f},{:.3f},{:.3f}]",
            netId, pos.x, pos.y, pos.z, rot.x, rot.y, rot.z, rot.w);
    }

    if (!out.empty()) writeLine(std::format("{{\"k\":\"s\",\"t\":{:.3f},\"o\":[{}]}}", t, out));
    if (!armorOut.empty()) writeLine(std::format("{{\"k\":\"a\",\"t\":{:.3f},\"a\":[{}]}}", t, armorOut));

    // Objects that disappeared since the last sample.
    for (auto it = g_objects.begin(); it != g_objects.end();) {
        if (!it->second.seenThisSample) {
            writeLine(std::format("{{\"k\":\"d\",\"t\":{:.3f},\"id\":{}}}", t, it->first));
            it = g_objects.erase(it);
        }
        else ++it;
    }
}

// ---------------------------------------------------------------------------
// Tickets
// ---------------------------------------------------------------------------
//
// The client's ScoreManager (the pointer at 0x0097A0D8, created by the class
// factory with class id 0xC4B8 at 0x004830C0; vtable 0x008DAEB0) is the
// server's replicated through ScoreManager::setNetUpdate. Its getTeamScore
// (vtable +0x10, 0x0049FD10) is `this + 0x10 + team x 0x50` for team 1..2,
// as on the server (lnxded 0x081616C0), and a TeamScore's live count is +0x48
// (lnxded TeamScore::setTickets 0x081610A0): what the HUD draws, read at
// 0x004A8C2F and ten more places the same way.

static const uintptr_t SCORE_MANAGER_PTR = 0x0097A0D8;
static const uintptr_t VT_SCORE_MANAGER = 0x008DAEB0;

void sampleTickets(double t)
{
    static int verified = -1;
    if (verified < 0) {
        verified = readAt<uintptr_t>((void*)VT_SCORE_MANAGER, 0x10) == 0x0049FD10 ? 1 : 0;
        if (!verified) debuglogt("replay: ScoreManager does not match this BF1942.exe, tickets not recorded\n");
    }
    if (verified != 1) return;
    uintptr_t manager = readAt<uintptr_t>((void*)SCORE_MANAGER_PTR, 0);
    if (!manager || vtableOf((void*)manager) != VT_SCORE_MANAGER) return;
    int team1 = readAt<int>((void*)manager, 0x10 + 1 * 0x50 + 0x48);
    int team2 = readAt<int>((void*)manager, 0x10 + 2 * 0x50 + 0x48);
    if (team1 == g_tickets[0] && team2 == g_tickets[1]) return;
    g_tickets[0] = team1;
    g_tickets[1] = team2;
    writeLine(std::format("{{\"k\":\"tk\",\"t\":{:.3f},\"v\":[{},{}]}}", t, team1, team2));
}
