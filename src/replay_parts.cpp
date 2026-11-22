// One sample of what hangs under the roots: turret and gun rotations (`jn`,
// `j`), engines (`g`) and soldiers' bodies (`st`), with the animation state
// table they refer to (`anim`). All of it is read at offsets in this
// BF1942.exe, checked once before any of it is trusted.

#include "replay_internal.h"

#include "bf/generic.h"
#include "bf/object.h"
#include "debug.h"

#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <string>

// A hull's moving parts, its engines and its soldiers' bodies, holding
// what was last written. A soldier is a root and keys by its network id; a
// part or an engine is a child object, whose networkable carries no id (a
// root's ghost carries its children's state), so it keys by the id the
// recorder gives it (childIdOf).
struct PartState {
    Quat rel;       // rotation relative to the root object
};
static std::map<int, PartState> g_parts;
struct EngineState {
    float revs;
    float throttle;
    int flags;
    int gear;
};
static std::map<int, EngineState> g_engines;
struct ChildId {
    int id;
    uint16_t root;      // the root's network id
    const void* tmpl;
};
static std::map<const void*, ChildId> g_childIds;
static int g_nextChildId = 1;
struct SoldierState {
    int lower;
    int upper;
    int item;
    int bits;
    float pitch;
    float twist;
};
static std::map<uint16_t, SoldierState> g_soldiers;
static bool g_animStatesWritten = false;

void resetPartSampler()
{
    g_parts.clear();
    g_engines.clear();
    g_childIds.clear();
    g_nextChildId = 1;
    g_soldiers.clear();
    g_animStatesWritten = false;
}

// ---------------------------------------------------------------------------
// Moving parts, engines and soldiers' bodies
// ---------------------------------------------------------------------------
//
// What the server replicates beyond a root's transform, read where the client
// has applied it:
//
//   RotationalBundle (vtable 0x008FE1B0): a turret, a gun base, a hull MG's
//     mount. Its RotationalBundleNetworkable carries the bundle's angles, and
//     RotationalBundle::setState 0x0057D470 turns them into the object's
//     relative transform every frame on the client, so its absolute rotation
//     against its root's is the traverse and elevation.
//   Engine (vtable 0x008FE4A0): the EngineNetworkable's running flag (+0x15C),
//     disabled flag (+0x15D) and throttle servo angle (+0x124), and on its
//     PhysicsEngine (+0x60, vtable 0x008FDEC0) the revs (+0xA0) -- what the
//     engine note is played from -- and the gear (+0xBC).
//   BFSoldier (vtable 0x008EB128): the BFSoldierNetworkable's aim pitch
//     (+0x2B0) and torso twist (+0x2B4), the lower and upper body animation
//     machines' state indices (+0x2E0, +0x324), the held item (+0x3E8) and the
//     state bits (+0x416). A state's name and flags come out of the global
//     state table (*0x009C9664, the one getCurrentStateFlags 0x00613440
//     reads), written once per file: stance is the lower state's flags (0x20
//     crouching, 0x40 lying), firing and reloading the upper state's name.
//
// Every vtable slot and code signature is checked once before any of it is
// read (partsVerified), and every read goes through safeCopy, so another build
// of the game still records the objects but none of this.

static const uintptr_t VT_ROTATIONAL_BUNDLE = 0x008FE1B0;
static const uintptr_t VT_ENGINE = 0x008FE4A0;
static const uintptr_t VT_PHYSICS_ENGINE = 0x008FDEC0;
static const uintptr_t VT_SOLDIER = 0x008EB128;
static const uintptr_t ANIM_STATE_TABLE = 0x009C9664;

static bool partsVerified()
{
    static int verified = -1;
    if (verified >= 0) return verified == 1;
    struct Slot { uintptr_t vtable; int slot; uintptr_t target; };
    static const Slot slots[] = {
        { VT_ROTATIONAL_BUNDLE, 20, 0x0057D010 },   // RotationalBundle::handleUpdate
        { VT_ROTATIONAL_BUNDLE, 15, 0x004A2250 },   // getAbsoluteTransformation
        { VT_ENGINE, 20, 0x0057E1D0 },              // Engine::handleUpdate
        { VT_PHYSICS_ENGINE, 36, 0x0057BFB0 },      // PhysicsEngine::updatePhysics
        { VT_SOLDIER, 37, 0x00500190 },
        { VT_SOLDIER, 15, 0x004A2250 },
    };
    struct Code { uintptr_t at; uint8_t bytes[16]; size_t length; };
    static const Code code[] = {
        // Engine::handleUpdate's T1 = [esi+124h] / maxRotation.z
        { 0x0057E296, { 0xD9, 0x86, 0x24, 0x01, 0x00, 0x00, 0xD8, 0x70, 0x08 }, 9 },
        // PhysicsEngine::updatePhysics reading the revs at +0xA0
        { 0x0057BFBE, { 0xD9, 0x86, 0xA0, 0x00, 0x00, 0x00 }, 6 },
        // getCurrentStateFlags: [machine+20h], then the table at 0x009C9664
        { 0x00613440, { 0x8B, 0x49, 0x20, 0x85, 0xC9, 0x7D, 0x03, 0x33, 0xC0, 0xC3, 0x8B, 0x15, 0x64, 0x96, 0x9C, 0x00 }, 16 },
    };
    bool ok = true;
    for (auto& s : slots) {
        if (readAt<uintptr_t>((void*)s.vtable, 4 * s.slot) != s.target) ok = false;
    }
    for (auto& c : code) {
        uint8_t have[16] = {};
        if (safeCopy(have, (void*)c.at, c.length) != c.length || memcmp(have, c.bytes, c.length) != 0) ok = false;
    }
    verified = ok ? 1 : 0;
    if (!ok) debuglogt("replay: vehicle parts, engines and soldier state do not match this BF1942.exe, not recorded\n");
    return ok;
}

static IObject* rootObjectOf(IObject* obj)
{
    IObject* root = obj;
    for (int depth = 0; root && root->getParent() && depth < 32; depth++) root = root->getParent();
    return root;
}

// A child object's id in this file, by the object: `first` when it is new --
// first seen, or an address the allocator has handed to an object under
// another root or of another template.
static int childIdOf(const void* obj, uint16_t root, const void* tmpl, bool& first)
{
    auto it = g_childIds.find(obj);
    first = it == g_childIds.end() || it->second.root != root || it->second.tmpl != tmpl;
    if (!first) return it->second.id;
    int id = g_nextChildId++;
    g_childIds[obj] = ChildId{ id, root, tmpl };
    return id;
}

// A point in `m`'s own frame: its offset from m's position on m's rows, the X,
// Y and Z axes toQuat reads.
static Pos3 localPosition(const Mat4& m, const Pos3& p)
{
    float dx = p.x - m.position.x, dy = p.y - m.position.y, dz = p.z - m.position.z;
    return Pos3(dx * m.a.x + dy * m.a.y + dz * m.a.z,
                dx * m.b.x + dy * m.b.y + dz * m.b.z,
                dx * m.c.x + dy * m.c.y + dz * m.c.z);
}

static Quat quatConjugate(const Quat& q) { return Quat{ -q.x, -q.y, -q.z, q.w }; }

static Quat quatMultiply(const Quat& a, const Quat& b)
{
    return Quat{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

// An MSVC 7 std::string (bfs::string's layout: allocator, 16-byte buffer or
// pointer, length, capacity) copied out without calling into the engine.
static std::string readEngineString(const void* at)
{
    struct Raw { uint32_t allocator; char buffer[16]; uint32_t length; uint32_t capacity; } raw{};
    if (safeCopy(&raw, at, sizeof(raw)) != sizeof(raw) || raw.length > 256) return std::string();
    char text[257] = {};
    const void* chars = raw.capacity >= 16 ? *(const char* const*)raw.buffer : (const void*)((const uint8_t*)at + 4);
    if (safeCopy(text, chars, raw.length) != raw.length) return std::string();
    return jsonEscape(text, strnlen(text, raw.length));
}

// How many states the table may hold before we treat the pointer as garbage
// rather than a table. This is only a sanity bound on a raw read: it has to
// clear the largest table a real mod builds, and a mod's is nothing like
// vanilla's. Measured from the installs: bf1942 has 1,395 states, FHSWEurope
// 58,059 -- forty-two times vanilla, because FHSW copies the lower-body states
// per weapon in hand (`LowersNormalWeapons.inc`: 924 Lb_RunForward* alone,
// 6,439 Lb_Crouch*). A bound of 4,096 clears vanilla and both expansion packs
// but no FHSW-family mod. 2^18 leaves room for a mod larger again than FHSW
// while still bounding the loop.
static const size_t MAX_ANIM_STATES = 1 << 18;

// The animation state table, once per file: index, name, flags.
static void writeAnimStates(double t)
{
    uintptr_t table = readAt<uintptr_t>((void*)ANIM_STATE_TABLE, 0);
    if (!table) return;
    uintptr_t begin = readAt<uintptr_t>((void*)table, 0x18);
    uintptr_t end = readAt<uintptr_t>((void*)table, 0x1C);
    if (!begin || end < begin) return;
    // Say so in the file rather than returning in silence: without the table
    // a reader cannot interpret the `st` rows, and a file that states its own
    // refusal costs one line and tells the reader why.
    size_t count = (size_t)((end - begin) / 4);
    if (count > MAX_ANIM_STATES) {
        writeLine(std::format(
            "{{\"k\":\"anim\",\"t\":{:.3f},\"skipped\":{},\"limit\":{},\"states\":[]}}",
            t, count, MAX_ANIM_STATES));
        g_animStatesWritten = true;
        return;
    }
    std::string out;
    for (uintptr_t at = begin; at < end; at += 4) {
        uintptr_t state = readAt<uintptr_t>((void*)at, 0);
        if (!state) continue;
        int index = (int)((at - begin) / 4);
        if (!out.empty()) out += ',';
        out += std::format("[{},\"{}\",{}]", index, readEngineString((void*)(state + 0x130)), readAt<uint32_t>((void*)state, 0x2C));
    }
    writeLine(std::format("{{\"k\":\"anim\",\"t\":{:.3f},\"states\":[{}]}}", t, out));
    g_animStatesWritten = true;
}

void sampleParts(double t)
{
    if (!partsVerified()) return;
    std::string joints, jointNames, engines, soldiers;
    auto& allObjs = ObjectManager_getAllRegisteredObjects();
    for (auto node = allObjs.head->left; node != allObjs.head; node = node->next()) {
        auto obj = node->pair.second;
        if (!obj) continue;
        uint32_t flags = obj->getFlags();
        if (flags & 1) continue;   // disabled
        auto net = obj->getNetworkable();
        if (!net) continue;
        uintptr_t vt = vtableOf(obj);
        bool root = (flags & 0x02000000) != 0;

        if (root && vt == VT_SOLDIER) {
            SoldierState s{
                readAt<int>(obj, 0x2E0), readAt<int>(obj, 0x324), readAt<uint8_t>(obj, 0x3E8),
                readAt<uint16_t>(obj, 0x416), readAt<float>(obj, 0x2B0), readAt<float>(obj, 0x2B4),
            };
            uint16_t id = net->getID();
            auto prev = g_soldiers.find(id);
            if (prev != g_soldiers.end() && prev->second.lower == s.lower && prev->second.upper == s.upper
                && prev->second.item == s.item && prev->second.bits == s.bits
                && fabsf(prev->second.pitch - s.pitch) < 1.0f && fabsf(prev->second.twist - s.twist) < 1.0f) continue;
            g_soldiers[id] = s;
            if (!soldiers.empty()) soldiers += ',';
            soldiers += std::format("[{},{},{},{:.1f},{:.1f},{},{}]", id, s.lower, s.upper, s.pitch, s.twist, s.item, s.bits);
            continue;
        }
        if (root) continue;

        if (vt == VT_ROTATIONAL_BUNDLE) {
            IObject* top = rootObjectOf(obj);
            auto topNet = top ? top->getNetworkable() : nullptr;
            if (!topNet || top == obj) continue;
            const Mat4& topM = top->getAbsoluteTransformation();
            const Mat4& partM = obj->getAbsoluteTransformation();
            Quat rel = quatMultiply(quatConjugate(toQuat(topM)), toQuat(partM));
            uint16_t rootId = (uint16_t)topNet->getID();
            auto tmpl = obj->getTemplate();
            bool first = false;
            int id = childIdOf(obj, rootId, tmpl, first);
            if (first) {
                // Named on first sight, with where it sits on its root: a
                // viewer finds the part in the root's model by its template's
                // name, and among same-named parts (a ship's AA guns) by place.
                Pos3 at = localPosition(topM, partM.position);
                if (!jointNames.empty()) jointNames += ',';
                jointNames += std::format("[{},{},\"{}\",{:.2f},{:.2f},{:.2f}]", rootId, id,
                    tmpl ? jsonEscape(std::string(tmpl->getName())) : std::string(), at.x, at.y, at.z);
            }
            else {
                auto prev = g_parts.find(id);
                if (prev != g_parts.end() && !changed(prev->second.rel, rel, 0.002f)) continue;
            }
            g_parts[id] = PartState{ rel };
            if (!joints.empty()) joints += ',';
            joints += std::format("[{},{},{:.4f},{:.4f},{:.4f},{:.4f}]", rootId, id, rel.x, rel.y, rel.z, rel.w);
            continue;
        }

        if (vt == VT_ENGINE) {
            uintptr_t pe = readAt<uintptr_t>(obj, 0x60);
            if (!pe || vtableOf((void*)pe) != VT_PHYSICS_ENGINE) continue;
            IObject* top = rootObjectOf(obj);
            auto topNet = top ? top->getNetworkable() : nullptr;
            if (!topNet) continue;
            EngineState e{
                readAt<float>((void*)pe, 0xA0), readAt<float>(obj, 0x124),
                (readAt<uint8_t>(obj, 0x15C) ? 1 : 0) | (readAt<uint8_t>(obj, 0x15D) ? 2 : 0),
                readAt<int>((void*)pe, 0xBC),
            };
            uint16_t rootId = (uint16_t)topNet->getID();
            bool first = false;
            int id = childIdOf(obj, rootId, obj->getTemplate(), first);
            auto prev = g_engines.find(id);
            if (!first && prev != g_engines.end() && fabsf(prev->second.revs - e.revs) < 0.01f
                && fabsf(prev->second.throttle - e.throttle) < 0.01f && prev->second.flags == e.flags
                && prev->second.gear == e.gear) continue;
            g_engines[id] = e;
            if (!engines.empty()) engines += ',';
            engines += std::format("[{},{:.3f},{:.3f},{},{},{}]", rootId, e.revs, e.throttle, e.flags, e.gear, id);
        }
    }
    if (!soldiers.empty() && !g_animStatesWritten) writeAnimStates(t);
    if (!jointNames.empty()) writeLine(std::format("{{\"k\":\"jn\",\"t\":{:.3f},\"o\":[{}]}}", t, jointNames));
    if (!joints.empty()) writeLine(std::format("{{\"k\":\"j\",\"t\":{:.3f},\"o\":[{}]}}", t, joints));
    if (!engines.empty()) writeLine(std::format("{{\"k\":\"g\",\"t\":{:.3f},\"o\":[{}]}}", t, engines));
    if (!soldiers.empty()) writeLine(std::format("{{\"k\":\"st\",\"t\":{:.3f},\"o\":[{}]}}", t, soldiers));
}
