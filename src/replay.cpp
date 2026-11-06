// Round replay recorder: the file, its clock, and the entry points replay.h
// declares. What goes into the file is written by the files replay_internal.h
// lists.

#include "replay.h"
#include "replay_internal.h"

#include "bf/gameevent.h"
#include "bf/ui.h"
#include "debug.h"
#include "settings.h"
#include "util.h"
#include "version.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <format>
#include <string>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static const int SAMPLE_HZ = 10;
static const int FLUSH_INTERVAL_MS = 2000;
// The file format, written as "v" in the header. docs/replay-format.md
// describes every record. A reader skips record kinds and fields it does not
// know, so adding one does not need a new version; changing or removing one does.
static const int FORMAT_VERSION = 5;

static FILE* g_file = nullptr;
static LARGE_INTEGER g_qpcFreq = {};
static LARGE_INTEGER g_qpcStart = {};
static double g_lastSampleTime = -1.0;
static DWORD g_lastFlushTick = 0;
static bool g_fileHasContent = false;   // anything written after the header

// ---------------------------------------------------------------------------
// Shared with the recorder's other files
// ---------------------------------------------------------------------------

double replayNow()
{
    if (g_qpcFreq.QuadPart == 0) return 0.0;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_qpcStart.QuadPart) / (double)g_qpcFreq.QuadPart;
}

// Escape an ISO-8859-1 byte string into a JSON string literal (without quotes).
std::string jsonEscape(const char* s, size_t len)
{
    std::string out;
    out.reserve(len + 8);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c >= 0x7F) {
                    char buf[8];
                    _snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else out += (char)c;
        }
    }
    return out;
}

std::string jsonEscape(const std::string& s) { return jsonEscape(s.data(), s.size()); }

// Copy bytes out of a game-owned struct without trusting its size. Used to dump
// the payload of events whose layout we have not mapped yet.
size_t safeCopy(void* dst, const void* src, size_t len)
{
    __try {
        memcpy(dst, src, len);
        return len;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool replayFileOpen()
{
    return g_file != nullptr;
}

void writeLine(const std::string& line)
{
    if (!g_file) return;
    fwrite(line.data(), 1, line.size(), g_file);
    fputc('\n', g_file);
    g_fileHasContent = true;

    DWORD tick = GetTickCount();
    if (tick - g_lastFlushTick > FLUSH_INTERVAL_MS) {
        fflush(g_file);
        g_lastFlushTick = tick;
    }
}

// ---------------------------------------------------------------------------
// File lifecycle
// ---------------------------------------------------------------------------

static bool replay_start()
{
    if (g_file) return true;
    if (!g_settings.recordReplays) return false;

    CreateDirectoryA("replays", nullptr);

    time_t t = time(nullptr);
    struct tm lt;
    localtime_s(&lt, &t);
    char name[128];
    strftime(name, sizeof(name), "replays\\replay_%Y%m%d-%H%M%S.ndjson", &lt);

    g_file = fopen(name, "wb");
    if (!g_file) {
        debuglogt("replay: failed to open %s\n", name);
        return false;
    }
    setvbuf(g_file, nullptr, _IOFBF, 1 << 16);

    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpcStart);
    g_lastSampleTime = -1.0;
    g_lastFlushTick = GetTickCount();
    resetObjectSampler();
    resetPartSampler();

    char iso[64];
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &lt);
    // `plus` is the marketing version out of version.h, bumped by hand and
    // neither stamped by CI nor tied to a commit, so it cannot answer "which
    // recorder wrote this". `build` can -- the compiler stamps it per build, so
    // a file says whether it predates a given recorder change.
    writeLine(std::format("{{\"k\":\"h\",\"v\":{},\"plus\":\"{}\",\"build\":\"{}\",\"start\":\"{}\",\"hz\":{}}}",
        FORMAT_VERSION, jsonEscape(WideStringToISO88591(build_version)),
        jsonEscape(__DATE__ " " __TIME__), iso, SAMPLE_HZ));
    g_fileHasContent = false;

    debuglogt("replay: recording to %s\n", name);
    BfMenu::getSingleton()->outputConsole(std::string("plus: recording replay to ") + name);

    // A file that begins after the join opens with what the join said, the
    // players, the objects standing, their projectile pools and the players'
    // kits, and who is playing. One that begins with the join holds nothing
    // yet (its 0x1A let the last join's go) and records the join as it comes.
    writeHeldJoin();
    return true;
}

void replay_stop()
{
    if (!g_file) return;
    writeLine(std::format("{{\"k\":\"end\",\"t\":{:.3f}}}", replayNow()));
    fclose(g_file);
    g_file = nullptr;
    debuglogt("replay: stopped\n");
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

// Begin recording now, if the setting is on and no file is open. Called from
// the console when plus.recordReplays is set, so the file opens the moment the
// user types plus.recordReplays 1 instead of waiting for the next event/frame.
void replay_begin()
{
    replay_start();
}

void replay_onEvent(GameEvent* event)
{
    const int type = event->getType();
    // A new join: what the last one said, and what it made, no longer hold.
    // Let go first, so a file this event starts does not open with the last
    // server's level, players or objects.
    if (type == 0x1A) forgetJoin();
    if (g_settings.recordReplays) {
        // A join sequence starts a new file; every join recorded so far opens with
        // event 0x1A. Map end is deliberately not the boundary: the server keeps
        // sending that map's teardown after GameStatus(ENDMAP), and the client
        // relaunches between maps anyway, closing the file on its way out.
        if (g_file && g_fileHasContent && type == 0x1A) replay_stop();
        if (g_file || replay_start()) recordEvent(event);
    }
    holdJoinEvent(event);
    holdObjectEvent(event);
}

void replay_onFrame()
{
    if (!g_settings.recordReplays) {
        if (g_file) replay_stop();
        return;
    }
    if (!g_file && !replay_start()) return;

    double t = replayNow();
    if (g_lastSampleTime >= 0.0 && t - g_lastSampleTime < 1.0 / SAMPLE_HZ) return;
    g_lastSampleTime = t;

    sampleObjects(t);
    sampleParts(t);
    sampleTickets(t);
    samplePlayers(t);
    sampleControlPoints(t);
}

// Chat as the client displays it. The server never sends players their own
// chat back, so ChatFragment events miss everything the recording player says;
// this catches every line on its way to the chat box instead.
void replay_onChat(const wchar_t* text, size_t length, int playerId, int team)
{
    if (!g_settings.recordReplays || !text) return;
    if (!g_file && !replay_start()) return;
    std::string iso = WideStringToISO88591(std::wstring(text, length));
    writeLine(std::format("{{\"k\":\"chat\",\"t\":{:.3f},\"pid\":{},\"team\":{},\"text\":\"{}\"}}",
        replayNow(), playerId, team, jsonEscape(iso)));
}
