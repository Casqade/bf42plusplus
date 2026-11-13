// Every game event the server sends, written as an `e` record: the ones with
// a struct in gameevent.h straight from it, the rest decoded from their
// payload by the layouts below, and anything still unknown dumped raw and
// logged. docs/replay-format.md says what each field means.

#include "replay_internal.h"

#include "bf/gameevent.h"
#include "bf/object.h"
#include "debug.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <format>
#include <string>

static std::string hexDump(const void* src, size_t len)
{
    // Large enough for the biggest event the engine defines: SetLevel, 158 bytes.
    uint8_t buf[256];
    if (len > sizeof(buf)) len = sizeof(buf);
    size_t got = safeCopy(buf, src, len);
    std::string out;
    out.reserve(got * 2);
    static const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < got; i++) {
        out += hex[buf[i] >> 4];
        out += hex[buf[i] & 15];
    }
    return out;
}

static std::string asciiDump(const void* src, size_t len)
{
    uint8_t buf[256];
    if (len > sizeof(buf)) len = sizeof(buf);
    size_t got = safeCopy(buf, src, len);
    std::string out;
    out.reserve(got);
    for (size_t i = 0; i < got; i++) {
        uint8_t c = buf[i];
        if (c >= 0x20 && c < 0x7F) out += (char)c;
        else out += '.';
    }
    return out;
}

// Every event the recorder could not decode, as hex and printable text, next to
// the debug log: the same bytes as the file's "raw" record, but in one place
// across every recording, for mapping the events still unknown.
#ifndef TARGET_BF1942_R
static const char* UNKNOWN_EVENTS_LOG = "logs\\bf42++_unknown_events.log";
#else
static const char* UNKNOWN_EVENTS_LOG = "logs\\bf42_r++_unknown_events.log";
#endif

static FILE* g_unknownLogFile = nullptr;
static void logUnknownEvent(int type, const uint8_t* payload, size_t size, double t)
{
    if (!g_unknownLogFile) {
        CreateDirectoryA("logs", nullptr);
        g_unknownLogFile = fopen(UNKNOWN_EVENTS_LOG, "a+b");
        if (!g_unknownLogFile) return;
    }
    time_t now = time(nullptr);
    struct tm lt;
    localtime_s(&lt, &now);
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%dT%H:%M:%S", &lt);

    size_t dumpLen = size > 12 ? size - 12 : 48;
    std::string hex = hexDump(payload, dumpLen);
    std::string ascii = asciiDump(payload, dumpLen);

    fprintf(g_unknownLogFile, "[%s] t=%.3fs type=0x%02X (%d) size=%zu payload=%s ascii=\"%s\"\n",
        timeStr, t, type, type, size, hex.c_str(), ascii.c_str());
    fflush(g_unknownLogFile);
}

// sizeof() of a game event class, read out of the engine's own code rather than
// guessed. Every GameEventMaker::createEvent in BF1942.exe opens with
//     mov ecx, sizeof(T)
//     call GameEvent::allocate
// -- true of all 51 makers the exe registers, and in agreement with every struct
// size gameevent.h static_asserts. Returns 0 when there is no maker or its code
// has a different shape, e.g. a maker added by another DLL, so the caller never
// trusts a bad read.
size_t eventSize(int type)
{
    static const uintptr_t GAMEEVENT_ALLOCATE = 0x004A6290;
    static int16_t cache[256];
    static bool cacheReady = false;
    if (!cacheReady) {
        std::fill(std::begin(cache), std::end(cache), (int16_t)-1);
        cacheReady = true;
    }
    bool cacheable = type >= 0 && type < 256;
    if (cacheable && cache[type] >= 0) return (size_t)cache[type];

    size_t size = 0;
    if (auto maker = GameEvent_getEventMaker((GameEventID)type)) {
        uintptr_t vtable = 0, createEvent = 0;
        uint8_t code[10];
        if (safeCopy(&vtable, maker, sizeof(vtable)) == sizeof(vtable)
            && safeCopy(&createEvent, (void*)(vtable + 4), sizeof(createEvent)) == sizeof(createEvent)
            && safeCopy(code, (void*)createEvent, sizeof(code)) == sizeof(code)
            && code[0] == 0xB9 && code[5] == 0xE8) {
            int32_t rel;
            uint32_t imm;
            memcpy(&rel, code + 6, sizeof(rel));
            memcpy(&imm, code + 1, sizeof(imm));
            if (createEvent + 10 + rel == GAMEEVENT_ALLOCATE && imm >= 12 && imm <= 4096) size = imm;
        }
    }
    if (cacheable) cache[type] = (int16_t)size;
    return size;
}

// Object-creation events (0x07: u32 templateId, u16 networkId, u8, vec3
// position, vec3 rotation) identify their object only by template id. Name it
// while the template table is at hand, so objects the client never receives
// updates for -- anything beyond the ~520 m relevance radius -- are named too.
static std::string rawEventExtraFields(int type, const uint8_t* payload, size_t size)
{
    if (type != 0x07 || size < 12 + sizeof(uint32_t)) return std::string();
    uint32_t templateId = 0;
    if (safeCopy(&templateId, payload, sizeof(templateId)) != sizeof(templateId)) return std::string();
    auto tmpl = ObjectTemplateManager_getTemplate(templateId);
    if (!tmpl) return std::string();
    return std::format(",\"tmpl\":\"{}\"", jsonEscape(std::string(tmpl->getName())));
}

// --- decoded engine events ------------------------------------------------
//
// Layouts read out of the Linux server's own event classes (registered by id
// in GameEvent::initEvents 0x0811bee0, whose in-memory layout the client's
// GameClient::processEvent 0x004933D0 reads at the same offsets) and checked
// against real recordings and the server's own event log.
// docs/replay-format.md says what each field means. Offsets are into the
// payload, after the 12-byte header. Every read goes through safeCopy, and an
// event whose size the engine does not confirm is dumped raw.

template <class T>
static T payloadAt(const uint8_t* payload, size_t offset)
{
    T value{};
    safeCopy(&value, payload + offset, sizeof(T));
    return value;
}

static std::string payloadString(const uint8_t* payload, size_t offset, size_t max)
{
    char buf[80] = {};
    if (max >= sizeof(buf)) max = sizeof(buf) - 1;
    size_t got = safeCopy(buf, payload + offset, max);
    return jsonEscape(buf, strnlen(buf, got));
}

// The name of an object template by its network id, or "" -- for a weapon id
// in a kill, a pool's projectile, a round-end tally's key. Template ids are
// the load order of this client's templates, so only a name survives the
// recording; guarded because a kill's weapon field is garbage for kinds that
// do not set it.
static ObjectTemplate* templateById(uint32_t tid)
{
    __try {
        return ObjectTemplateManager_getTemplate(tid);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static std::string templateNameOf(uint32_t tid)
{
    if (tid == 0 || tid > 0x00FFFFFF) return std::string();
    ObjectTemplate* tmpl = templateById(tid);
    return tmpl ? jsonEscape(std::string(tmpl->getName())) : std::string();
}

// A decoded event as a JSON record, or "" to fall back to the raw dump.
std::string decodeEvent(int type, const uint8_t* p, size_t size, double t)
{
    switch (type) {
        case 0x04:  // SimulationEvent: u8 running, f32 world time at this join
            if (size != 17) break;
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"simStart\",\"running\":{},\"worldTime\":{:.3f}}}",
                t, payloadAt<uint8_t>(p, 0), payloadAt<float>(p, 1));
        case 0x05: {  // CreateMultipleObjectsEvent: a kit's projectile pool
            if (size != 22) break;
            uint32_t tid = payloadAt<uint32_t>(p, 0);
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"projPool\",\"tid\":{},\"tmpl\":\"{}\",\"netId\":{},\"count\":{}}}",
                t, tid, templateNameOf(tid), payloadAt<uint16_t>(p, 4), payloadAt<int32_t>(p, 6));
        }
        case 0x13: {  // MapEvent: one map-rotation entry
            if (size != 112) break;
            static const char* modes[] = { "", "ctf", "conquest", "tdm", "coop", "objective" };
            uint8_t mode = payloadAt<uint8_t>(p, 3);
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"mapList\",\"add\":{},\"map\":\"{}\",\"mode\":\"{}\",\"mod\":\"{}\"}}",
                t, payloadAt<uint8_t>(p, 0), payloadString(p, 4, 64), mode < 6 ? modes[mode] : "", payloadString(p, 68, 32));
        }
        case 0x14:  // ChallengeEvent: the join's hash challenge; the token is dropped
            if (size != 52) break;
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"challenge\",\"mod\":\"{}\",\"xpack\":{}}}",
                t, payloadString(p, 10, 16), payloadAt<int32_t>(p, 27));
        case 0x16:  // GameRulesEvent
            if (size != 31) break;
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"gameRules\",\"extViews\":{},\"noseCam\":{},\"soldierFF\":{:.3f},\"ticketRatio\":{:.3f},\"timeLimit\":{},\"worldTime\":{},\"crosshair\":{}}}",
                t, payloadAt<uint8_t>(p, 0), payloadAt<uint8_t>(p, 1), payloadAt<float>(p, 2), payloadAt<float>(p, 6),
                payloadAt<uint32_t>(p, 10), payloadAt<uint32_t>(p, 14), payloadAt<uint8_t>(p, 18));
        case 0x1A:  // ServerInfoEvent: the first event of a join
            if (size != 63) break;
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"serverInfo\",\"mapId\":\"{}\",\"mod\":\"{}\",\"gameId\":\"{}\"}}",
                t, payloadString(p, 0, 16), payloadString(p, 17, 16), payloadString(p, 34, 16));
        case 0x1B:  // ServerInfoEvent2: the server's name
            if (size != 45) break;
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"serverName\",\"name\":\"{}\"}}",
                t, payloadString(p, 0, 32));
        case 0x29:  // TimerSyncEvent, every 10 s
            if (size != 20) break;
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"clock\",\"timeLimit\":{},\"worldTime\":{}}}",
                t, payloadAt<uint32_t>(p, 0), payloadAt<uint32_t>(p, 4));
        case 0x30:  // StatsKillsEvent / StatsShotsEvent / StatsHitsEvent: one
        case 0x31:  // player's round tally by template, at the round's end
        case 0x32: {
            if (size != 74) break;
            static const char* stats[] = { "destroyed", "fired", "hit" };
            int rows = payloadAt<uint8_t>(p, 0) + 1;
            if (rows > 10) rows = 10;
            std::string out;
            for (int i = 0; i < rows; i++) {
                uint32_t tid = payloadAt<uint32_t>(p, 2 + 4 * i);
                if (!out.empty()) out += ',';
                out += std::format("{{\"tid\":{},\"tmpl\":\"{}\",\"n\":{}}}", tid, templateNameOf(tid), payloadAt<uint16_t>(p, 42 + 2 * i));
            }
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"roundStats\",\"stat\":\"{}\",\"pid\":{},\"rows\":[{}]}}",
                t, stats[type - 0x30], payloadAt<uint8_t>(p, 1), out);
        }
        case 0x37:  // SessionIdEvent: this client's connection id
            if (size != 76) break;
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"sessionId\",\"id\":\"{}\"}}",
                t, payloadString(p, 0, 64));
        case 0x3C:  // HitFromPosEvent: this client's soldier was hurt
            // lnxded GameServer::_giveDamage 0x0814b870 sends it to the
            // damaged player's client alone: `dir` the sector the damage came
            // from, 45 degrees each (0 within 22.5 of ahead, 4 behind; the
            // cos 22.5 threshold is 0x086c0b68), `strength` 255 x the damage
            // over the victim's own vtable+0x14 value, truncated (0x0814ba31):
            // the hit indicator the HUD draws.
            if (size != 14) break;
            return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"hitFrom\",\"dir\":{},\"strength\":{}}}",
                t, payloadAt<uint8_t>(p, 0), payloadAt<uint8_t>(p, 1));
        default:
            break;
    }
    return std::string();
}

std::string setLevelLine(GameEvent* event, double t)
{
    auto ev = reinterpret_cast<SetLevelEvent*>(event);
    size_t lvlLen = strnlen(ev->levelPath, sizeof(ev->levelPath));
    size_t modeLen = strnlen(ev->gameModeFile, sizeof(ev->gameModeFile));
    return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"setLevel\",\"level\":\"{}\",\"mode\":\"{}\"}}",
        t, jsonEscape(ev->levelPath, lvlLen), jsonEscape(ev->gameModeFile, modeLen));
}

// The template is looked up under SEH: this runs for every object whether or
// not recording is on (holdObjectEvent).
std::string createObjectLine(GameEvent* event, double t)
{
    auto ev = reinterpret_cast<CreateObjectEvent*>(event);
    auto tmpl = templateById(ev->templateId);
    std::string tmplName = tmpl ? jsonEscape(std::string(tmpl->getName())) : std::string();
    return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"createObject\",\"tid\":{},\"netId\":{},\"tmpl\":\"{}\",\"pos\":[{:.2f},{:.2f},{:.2f}],\"rot\":[{:.2f},{:.2f},{:.2f}]}}",
        t, ev->templateId, ev->objectNetId, tmplName, ev->position.x, ev->position.y, ev->position.z, ev->rotation.x, ev->rotation.y, ev->rotation.z);
}

std::string pickupKitLine(GameEvent* event, double t)
{
    auto ev = reinterpret_cast<PickupKitEvent*>(event);
    return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"pickupKit\",\"pid\":{},\"netId\":{}}}", t, ev->playerID, ev->kitNetId);
}

std::string createPlayerLine(GameEvent* event, double t)
{
    auto ev = reinterpret_cast<CreatePlayerEvent*>(event);
    size_t nameLen = strnlen(ev->name, sizeof(ev->name));
    return std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"createPlayer\",\"pid\":{},\"name\":\"{}\",\"team\":{},\"ai\":{},\"netId\":{},\"vehNetId\":{},\"camNetId\":{},\"kitNetId\":{}}}",
        t, ev->playerID, jsonEscape(ev->name, nameLen), ev->team, ev->isAI ? 1 : 0,
        ev->playerNetworkID, ev->vehicleNetworkID, ev->cameraNetworkID, ev->kitNetworkID);
}

void recordEvent(GameEvent* event)
{
    double t = replayNow();
    int type = event->getType();
    // Payload starts after vtable, sequenceNumber, nextEvent.
    const uint8_t* payload = (const uint8_t*)event + 12;

    switch (type) {
        case BF_CreatePlayerEvent:
            writeLine(createPlayerLine(event, t));
            break;
        case BF_DestroyPlayerEvent: {
            auto ev = reinterpret_cast<DestroyPlayerEvent*>(event);
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"destroyPlayer\",\"pid\":{}}}", t, ev->playerid));
            break;
        }
        case BF_SetTeamEvent: {
            auto ev = reinterpret_cast<SetTeamEvent*>(event);
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"setTeam\",\"pid\":{},\"team\":{}}}", t, ev->playerid, ev->teamid));
            break;
        }
        case BF_ScoreMsgEvent: {
            auto ev = reinterpret_cast<ScoreMsgEvent*>(event);
            // A kill's weapon is the killing object's template id (the weapon
            // on foot, the hull when mounted); other kinds leave it unset.
            bool kill = ev->eventid == SE_KILL || ev->eventid == SE_TK;
            std::string weaponName = kill && ev->weapon > 0 ? templateNameOf((uint32_t)ev->weapon) : std::string();
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"score\",\"kind\":{},\"pid\":{},\"victim\":{},\"weapon\":{},\"bodypart\":{}{}}}",
                t, (uint32_t)ev->eventid, ev->playerid, ev->victimpid, ev->weapon, ev->bodypart,
                weaponName.empty() ? std::string() : std::format(",\"weaponName\":\"{}\"", weaponName)));
            break;
        }
        case BF_RadioMessageEvent: {
            auto ev = reinterpret_cast<RadioMessageEvent*>(event);
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"radio\",\"pid\":{},\"msg\":{},\"global\":{}}}",
                t, ev->playerid, ev->messageID, ev->broadcast ? 1 : 0));
            break;
        }
        case BF_GameStatusEvent: {
            auto ev = reinterpret_cast<GameStatusEvent*>(event);
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"gameStatus\",\"status\":{}}}", t, (int)ev->newStatus));
            break;
        }
        case BF_VoteEvent: {
            auto ev = reinterpret_cast<VoteEvent*>(event);
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"vote\",\"action\":{},\"type\":{},\"pid\":{},\"target\":{},\"yes\":{},\"required\":{}}}",
                t, (uint32_t)ev->action, (uint32_t)ev->type, ev->playerID, ev->target, ev->yesCount, ev->votesRequired));
            break;
        }
        case BF_WelcomeMsgEvent: {
            auto ev = reinterpret_cast<WelcomeMsgEvent*>(event);
            size_t len = ev->length > sizeof(ev->message) ? sizeof(ev->message) : ev->length;
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"welcome\",\"id\":{},\"text\":\"{}\"}}",
                t, ev->id, jsonEscape(ev->message, len)));
            break;
        }
        case BF_SpecialGameEvent: {
            auto ev = reinterpret_cast<SpecialGameEvent*>(event);
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"special\",\"action\":{}}}", t, ev->action));
            break;
        }
        case BF_DataBaseCompleteEvent:
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"dbComplete\"}}", t));
            break;
        case BF_DestroyObjectEvent: {
            auto ev = reinterpret_cast<DestroyObjectEvent*>(event);
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"destroyObject\",\"netId\":{}}}", t, ev->objectNetId));
            break;
        }
        case BF_PlayerControlObjectEvent: {
            auto ev = reinterpret_cast<PlayerControlObjectEvent*>(event);
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"control\",\"pid\":{},\"netId\":{}}}", t, ev->playerID, ev->objectNetId));
            break;
        }
        case BF_EnterVehicleEvent: {
            auto ev = reinterpret_cast<EnterVehicleEvent*>(event);
            writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"enterVehicle\",\"pid\":{},\"netId\":{}}}", t, ev->playerID, ev->vehicleNetId));
            break;
        }
        case BF_ExitVehicleEvent: {
            auto ev = reinterpret_cast<ExitVehicleEvent*>(event);
            if (!ev->flag) {
                writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"exitVehicle\",\"pid\":{}}}", t, ev->playerID));
            }
            break;
        }
        case BF_PickupKitEvent:
            writeLine(pickupKitLine(event, t));
            break;
        case BF_CreateObjectEvent:
            writeLine(createObjectLine(event, t));
            break;
        case BF_SetLevelEvent:
            writeLine(setLevelLine(event, t));
            break;
        case BF_HUDTextEvent:
        case BF_CreateStaticObjectEvent:
        case BF_UpdateStaticObjectEvent:
            // bf42plus extensions and HUD text, not replay material.
            break;
        case BF_ChatFragmentEvent:
            // Chat comes from the chat box instead (replay_onChat): it has every
            // line these pieces make, whole, plus the recording player's own.
            break;
        default: {
            size_t size = eventSize(type);
            std::string decoded = size ? decodeEvent(type, payload, size, t) : std::string();
            if (!decoded.empty()) {
                writeLine(decoded);
                break;
            }
            logUnknownEvent(type, payload, size, t);
            if (size) {
                writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"raw\",\"type\":{},\"size\":{}{},\"raw\":\"{}\"}}",
                    t, type, size, rawEventExtraFields(type, payload, size), hexDump(payload, size - 12)));
            }
            else {
                writeLine(std::format("{{\"k\":\"e\",\"t\":{:.3f},\"e\":\"raw\",\"type\":{},\"size\":null,\"raw\":\"{}\"}}",
                    t, type, hexDump(payload, 48)));
            }
            break;
        }
    }
}
