#include "profiling.h"
#include "hooks.h"

#include <unordered_map>

#include <string.h>

#include "settings.h"

#define ALLOC_CALLSTACK_DEPTH 4


static const char* MallocPoolName = "malloc";
static const char* MemoryPoolName = "MemoryPool";
static const char* MemoryPoolIndexableName = "MemoryPoolIndexable";

void profile_malloc()
{
    BEGIN_ASM_CODE(a)
        push ecx
        mov eax, 0x00D3F4A0 // malloc
        call [eax]
        pop ecx

        test eax, eax
        jz alloc_failed

        push eax
        push MallocPoolName // name
        push 0 // secure
        push ALLOC_CALLSTACK_DEPTH // depth
        push ecx // size
        push eax // ptr
        mov eax, ___tracy_emit_memory_alloc_callstack_named
        call eax
        pop eax
        pop eax
        pop eax
        pop eax
        pop eax
        pop eax

    alloc_failed:
    MOVE_CODE_AND_ADD_CODE(a, 0x00616DCB, 8, HOOK_DISCARD_ORIGINAL);


    static const char* leakMessage = "MemoryPool::alloc leak";

    BEGIN_ASM_CODE(b)

        mov eax, 0x00616DCB
        call eax

        push eax

        push 0 // depth
        push leakMessage // txt
        mov eax, ___tracy_emit_messageL
        call eax
        pop eax
        pop eax

        pop eax

    MOVE_CODE_AND_ADD_CODE(b, 0x0069217F, 5, HOOK_DISCARD_ORIGINAL);
}

void profile_free()
{
    BEGIN_ASM_CODE(a)
        push ecx

        test ecx, ecx
        jz invalid_ptr

        push ecx
        push MallocPoolName // name
        push 0 // secure
        push ALLOC_CALLSTACK_DEPTH // depth
        push ecx // ptr
        mov eax, ___tracy_emit_memory_free_callstack_named
        call eax
        pop ecx
        pop ecx
        pop ecx
        pop ecx
        pop ecx

    invalid_ptr:
        mov eax, 0x00D3F468 // free
        call [eax]
        pop ecx

    MOVE_CODE_AND_ADD_CODE(a, 0x00616DD4, 8, HOOK_DISCARD_ORIGINAL);
}

void profile_operator_new()
{
//    operator new
    BEGIN_ASM_CODE(a)

        push eax

        push MemoryPoolName // name
        push 0 // secure
        push ALLOC_CALLSTACK_DEPTH // depth
        push ecx // size
        push eax // ptr
        mov eax, ___tracy_emit_memory_alloc_callstack_named
        call eax
        pop eax
        pop eax
        pop eax
        pop eax
        pop eax

        pop eax

    MOVE_CODE_AND_ADD_CODE(a, 0x00494EB8, 5, HOOK_ADD_ORIGINAL_BEFORE);


//    operator new []
    BEGIN_ASM_CODE(b)

        push eax

        push MemoryPoolName // name
        push 0 // secure
        push ALLOC_CALLSTACK_DEPTH // depth
        push ecx // size
        push eax // ptr
        mov eax, ___tracy_emit_memory_alloc_callstack_named
        call eax
        pop eax
        pop eax
        pop eax
        pop eax
        pop eax

        pop eax

    MOVE_CODE_AND_ADD_CODE(b, 0x00494F1F, 5, HOOK_ADD_ORIGINAL_BEFORE);
}

void profile_operator_delete()
{
//    MemoryPool::tryFree
    BEGIN_ASM_CODE(a)

        push ecx
        push edx

        push MemoryPoolName // name
        push 0 // secure
        push edx // ptr
        mov eax, ___tracy_emit_memory_free_callstack_named
        call eax
        pop edx
        pop edx
        pop edx

        pop edx
        pop ecx

    MOVE_CODE_AND_ADD_CODE(a, 0x00691F43, 5, HOOK_ADD_ORIGINAL_AFTER);
}

void profile_memoryPoolIndexable()
{
//    MemoryPoolIndexable::alloc
    BEGIN_ASM_CODE(a)

        mov [esi+0x10], ebx
        push eax

        push MemoryPoolIndexableName // name
        push 0 // secure
        push ALLOC_CALLSTACK_DEPTH // depth
        push edi // size
        push eax // ptr
        mov eax, ___tracy_emit_memory_alloc_callstack_named
        call eax
        pop eax
        pop eax
        pop eax
        pop eax
        pop eax

        pop eax
        mov ebx, 0x006C4DE7
        jmp ebx

    MOVE_CODE_AND_ADD_CODE(a, 0x006C4DBB, 5, HOOK_DISCARD_ORIGINAL);


//    MemoryPoolIndexable::free
    BEGIN_ASM_CODE(b)

        mov eax, 0x006C4966
        call eax

        push MemoryPoolIndexableName // name
        push 0 // secure
        push [ebp+0x8] // ptr
        mov eax, ___tracy_emit_memory_free_callstack_named
        call eax
        pop edx
        pop edx
        pop edx

    MOVE_CODE_AND_ADD_CODE(b, 0x006C4A33, 5, HOOK_DISCARD_ORIGINAL);
}


thread_local TracyCZoneCtx zonesStack[64] {};
thread_local size_t zoneIdx {};


___tracy_source_location_data* __fastcall registerZone( const char* zoneName )
{
    thread_local std::unordered_map <const char*, ___tracy_source_location_data> locations;

    ___tracy_source_location_data& location = locations[zoneName];
    location.name = zoneName;
    return &location;
}

void __fastcall zonePush( uint32_t id, int32_t active )
{
  auto& zone = zonesStack[zoneIdx++];
  zone.id = id;
  zone.active = active;
}

TracyCZoneCtx __cdecl zonePop()
{
    return zonesStack[--zoneIdx];
}

TracyCZoneCtx __cdecl zoneGet()
{
    return zonesStack[zoneIdx - 1];
}


void profile_startTimer()
{
    BEGIN_ASM_CODE(a)

        mov ecx, [ebp+0xC] // timerName
        mov eax, registerZone
        call eax

        push 1 // active
        push TRACY_CALLSTACK // depth
        push eax // srcloc
        mov eax, ___tracy_emit_zone_begin_callstack // id: eax, active: edx
        call eax
        pop ecx
        pop ecx
        pop ecx

        mov ecx, eax
        mov eax, zonePush
        call eax

    MOVE_CODE_AND_ADD_CODE(a, 0x00617950, 6, HOOK_ADD_ORIGINAL_AFTER);


    static decltype(strlen)* p_strlen = &strlen;

//    Win32FileStream::open
    BEGIN_ASM_CODE(b)

        mov ecx, [ebp+0x8] // path
        mov eax, 0x00D3EE08 // std::string::c_str()
        call [eax]

        push eax
        mov eax, p_strlen
        call eax
        pop ecx

        push eax // size
        push ecx // text

        mov eax, zoneGet
        call eax

        push edx
        push eax // ctx
        mov eax, ___tracy_emit_zone_text
        call eax

        pop eax
        pop eax
        pop eax
        pop eax

    MOVE_CODE_AND_ADD_CODE(b, 0x006E5AE2, 5, HOOK_ADD_ORIGINAL_AFTER);


//    Win32FileSystem::fileExists
    BEGIN_ASM_CODE(c)

        mov ecx, [ebp+0x8] // path
        mov eax, 0x00D3EE08 // std::string::c_str()
        call [eax]

        push eax
        mov eax, p_strlen
        call eax
        pop ecx

        push eax // size
        push ecx // text

        mov eax, zoneGet
        call eax

        push edx
        push eax // ctx
        mov eax, ___tracy_emit_zone_text
        call eax

        pop eax
        pop eax
        pop eax
        pop eax

    MOVE_CODE_AND_ADD_CODE(c, 0x006E4E5B, 6, HOOK_ADD_ORIGINAL_AFTER);


//    OldConsole::run
    BEGIN_ASM_CODE(d)

        lea ecx, [ebp+0x10] // scriptPath
        mov eax, 0x00D3EE08 // std::string::c_str()
        call [eax]

        push eax
        mov eax, p_strlen
        call eax
        pop ecx

        push eax // size
        push ecx // text

        mov eax, zoneGet
        call eax

        push edx
        push eax // ctx
        mov eax, ___tracy_emit_zone_text
        call eax

        pop eax
        pop eax
        pop eax
        pop eax

    MOVE_CODE_AND_ADD_CODE(d, 0x00628790, 5, HOOK_ADD_ORIGINAL_AFTER);


//    OldConsole::handleCommand
    BEGIN_ASM_CODE(e)

        mov ecx, [ebp+0x74]

        mov eax, 0x00D3EE08 // std::string::c_str()
        call [eax]

        push eax
        mov eax, p_strlen
        call eax
        pop ecx

        push eax // size
        push ecx // text

        mov eax, zoneGet
        call eax

        push edx
        push eax // ctx
        mov eax, ___tracy_emit_zone_text
        call eax

        pop eax
        pop eax
        pop eax
        pop eax

    MOVE_CODE_AND_ADD_CODE(e, 0x00626A49, 7, HOOK_ADD_ORIGINAL_BEFORE);
}

void profile_stopTimer()
{
    BEGIN_ASM_CODE(a)

        mov eax, zonePop
        call eax

        push edx
        push eax // ctx
        mov eax, ___tracy_emit_zone_end
        call eax
        pop ecx
        pop ecx

    MOVE_CODE_AND_ADD_CODE(a, 0x00618545, 6, HOOK_ADD_ORIGINAL_AFTER);
}

void profile_startGlobalTimer()
{
    BEGIN_ASM_CODE(a)

        push 0
        mov eax, ___tracy_emit_frame_mark
        call eax
        pop eax
        mov ecx, esi

    MOVE_CODE_AND_ADD_CODE(a, 0x0061A55C, 6, HOOK_ADD_ORIGINAL_AFTER);
}


void profile_main_loop()
{
    static const char* frameName = "Main Loop";


    BEGIN_ASM_CODE(a)

        push eax
        push eax
        fst qword ptr [esp] // save curent time

        push frameName
        mov eax, ___tracy_emit_frame_mark_start
        call eax
        pop eax

        fld qword ptr [esp]
        pop eax
        pop eax

    MOVE_CODE_AND_ADD_CODE(a, 0x0047EECC, 6, HOOK_ADD_ORIGINAL_AFTER);


    BEGIN_ASM_CODE(b)

        push eax

        push frameName
        mov eax, ___tracy_emit_frame_mark_end
        call eax
        pop eax
        pop eax

    MOVE_CODE_AND_ADD_CODE(b, 0x0047F36C, 5, HOOK_ADD_ORIGINAL_AFTER);
}

void profile_game_load()
{
    static const char* frameName = "Game Load";

    BEGIN_ASM_CODE(a)

        push frameName
        mov eax, ___tracy_emit_frame_mark_start
        call eax
        pop eax

    MOVE_CODE_AND_ADD_CODE(a, 0x0042EAE4, 6, HOOK_ADD_ORIGINAL_AFTER);


    BEGIN_ASM_CODE(b)

        push frameName
        mov eax, ___tracy_emit_frame_mark_end
        call eax
        pop eax

    MOVE_CODE_AND_ADD_CODE(b, 0x0042F876, 5, HOOK_ADD_ORIGINAL_AFTER);
}


void profile_voiceManagerThread()
{
    static const char* threadName = "VoiceManager_update_thread";
    static const char* zoneName = "VoiceManager::updateStreaming";


    BEGIN_ASM_CODE(a)

        push threadName
        mov eax, ___tracy_set_thread_name
        call eax
        pop eax

    MOVE_CODE_AND_ADD_CODE(a, 0x00491515, 5, HOOK_ADD_ORIGINAL_AFTER);


    BEGIN_ASM_CODE(b)

        mov ecx, zoneName
        mov eax, registerZone
        call eax

        push 1 // active
        push eax // srcloc
        mov eax, ___tracy_emit_zone_begin // id: eax, active: edx
        call eax
        pop ecx
        pop ecx

        mov ecx, eax
        mov eax, zonePush
        call eax


        mov eax, 0x0090F079 // VoiceManager::updateStreaming
        call eax


        mov eax, zonePop
        call eax

        push edx
        push eax // ctx
        mov eax, ___tracy_emit_zone_end
        call eax
        pop ecx
        pop ecx

    MOVE_CODE_AND_ADD_CODE(b, 0x00491540, 5, HOOK_DISCARD_ORIGINAL);
}

void profile_netClientThread()
{
    static const char* threadName = "NetClientThread";

    static const char* zoneName = "NetClientThread::run";


    BEGIN_ASM_CODE(a)

        push threadName
        mov eax, ___tracy_set_thread_name
        call eax
        pop eax

    MOVE_CODE_AND_ADD_CODE(a, 0x006AC329, 6, HOOK_ADD_ORIGINAL_AFTER);


    BEGIN_ASM_CODE(b)

        mov ecx, zoneName
        mov eax, registerZone
        call eax

        push 1 // active
        push eax // srcloc
        mov eax, ___tracy_emit_zone_begin // id: eax, active: edx
        call eax
        pop ecx
        pop ecx

        mov ecx, eax
        mov eax, zonePush
        call eax


        mov eax, [esi]
        mov ecx, esi
        call [eax+0x34]
        push eax


        mov eax, zonePop
        call eax

        push edx
        push eax // ctx
        mov eax, ___tracy_emit_zone_end
        call eax
        pop ecx
        pop ecx

        pop eax

    MOVE_CODE_AND_ADD_CODE(b, 0x006AC37D, 7, HOOK_DISCARD_ORIGINAL);
}

void profile_netServerThread()
{
    static const char* threadName = "NetServerThread";

    static const char* zoneNameRun = "NetServerThread::run";
    static const char* zoneNameUpdateConnections = "NetServer::_updateConnections";
    static const char* zoneNameUpdate = "NetServer::_update";


    BEGIN_ASM_CODE(a)

        push threadName
        mov eax, ___tracy_set_thread_name
        call eax
        pop eax

    MOVE_CODE_AND_ADD_CODE(a, 0x00733B30, 5, HOOK_ADD_ORIGINAL_AFTER);


    BEGIN_ASM_CODE(b)

        push ecx // save ecx for _updateConnections call

        mov ecx, zoneNameRun
        mov eax, registerZone
        call eax

        push 1 // active
        push eax // srcloc
        mov eax, ___tracy_emit_zone_begin // id: eax, active: edx
        call eax
        pop ecx
        pop ecx

        mov ecx, eax
        mov eax, zonePush
        call eax


        mov ecx, zoneNameUpdateConnections
        mov eax, registerZone
        call eax

        push 1 // active
        push eax // srcloc
        mov eax, ___tracy_emit_zone_begin // id: eax, active: edx
        call eax
        pop ecx
        pop ecx

        mov ecx, eax
        mov eax, zonePush
        call eax


        pop ecx
        mov eax, 0x006A9A73 // NetServer::_updateConnections
        call eax


        mov eax, zonePop
        call eax

        push edx
        push eax // ctx
        mov eax, ___tracy_emit_zone_end
        call eax
        pop ecx
        pop ecx

    MOVE_CODE_AND_ADD_CODE(b, 0x00733C16, 5, HOOK_DISCARD_ORIGINAL);


    BEGIN_ASM_CODE(c)

        push ecx // save ecx for _update call

        mov ecx, zoneNameUpdate
        mov eax, registerZone
        call eax

        push 1 // active
        push eax // srcloc
        mov eax, ___tracy_emit_zone_begin // id: eax, active: edx
        call eax
        pop ecx
        pop ecx

        mov ecx, eax
        mov eax, zonePush
        call eax


        pop ecx
        mov eax, 0x006A86D4 // NetServer::_update
        call eax
        push eax


        mov eax, zonePop
        call eax

        push edx
        push eax // ctx
        mov eax, ___tracy_emit_zone_end
        call eax
        pop ecx
        pop ecx

        pop eax

    MOVE_CODE_AND_ADD_CODE(c, 0x00733C22, 5, HOOK_DISCARD_ORIGINAL);


    BEGIN_ASM_CODE(d)

        mov eax, 0x00642E15 // Win32Mutex::unlock
        call eax

        mov eax, zonePop
        call eax

        push edx
        push eax // ctx
        mov eax, ___tracy_emit_zone_end
        call eax
        pop ecx
        pop ecx

    MOVE_CODE_AND_ADD_CODE(d, 0x00733C4A, 5, HOOK_DISCARD_ORIGINAL);
}

void profile_autostart()
{
    patchBytes(0x00470457, {1}); // Setup::initModules force-enable Profiler

    patchBytes(0x0061A57D, {0x90, 0xE9}); // 0F 84 -> 90 E9 // Profiler::startGlobalTimer skip
    patchBytes(0x00619508, {0x90, 0xE9}); // 0F 84 -> 90 E9 // Profiler::stopGlobalTimer skip
}

decltype(std::malloc)* p_malloc_orig = nullptr;
decltype(std::free)* p_free_orig = nullptr;

void* __cdecl override_malloc(size_t size)
{
    void* ptr = p_malloc_orig(size);

    if (ptr)
      TracySecureAllocNS(ptr, size, ALLOC_CALLSTACK_DEPTH, MallocPoolName);

    return ptr;
}

void __cdecl override_free(void* ptr)
{
    if (ptr)
      TracySecureFreeNS(ptr, ALLOC_CALLSTACK_DEPTH, MallocPoolName);

    p_free_orig(ptr);
}


constexpr auto p_malloc = &override_malloc;
constexpr auto p_free = &override_free;

void attach_profiler()
{
    tracy::SetThreadName("MainThread");


//    overriding malloc/free breaks Tracy
//    since some static vars may allocate before
//    this function is called
    if (false)
    {
        std::memcpy(&p_malloc_orig, (void*) 0x00D3F4A0, sizeof(p_malloc_orig));
        std::memcpy(&p_free_orig, (void*) 0x00D3F468, sizeof(p_free_orig));

        patchBytes(0x00D3F4A0, p_malloc);
        patchBytes(0x00D3F468, p_free);
    }


//    these report allocations on the
//    same address, which breaks Tracy
    if (false)
    {
        profile_operator_new();
        profile_operator_delete();
        profile_memoryPoolIndexable();
    }

    profile_malloc();
    profile_free();

    profile_startTimer();
    profile_stopTimer();
    profile_startGlobalTimer();
    profile_stopGlobalTimer();

    profile_game_load();
    profile_main_loop();

    profile_voiceManagerThread();
    profile_netClientThread();
    profile_netServerThread();

    if (g_settings.profilerAutoStart.value)
        profile_autostart();
}
