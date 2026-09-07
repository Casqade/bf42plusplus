#include "input.h"
#include "object.h"

#include "../debug.h"
#include "../hooks.h"
#include "../settings.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <iterator>


// disable warnings about unreferenced parameters, uninitialized object variables, __asm blocks, ...
#pragma warning(push)
#pragma warning(disable: 26495 4100 4410 4409 4740)

// Let keyboard/joystick fly the plane while the freelook button is held.
//
// Vanilla makes the mouse either fly or look, never both, because Air.con binds
// the same physical mouse axis to two logical inputs:
//   addAxisMapping c_PIRoll  ... IDAxis_0 ... 0    -> AxisRemap.first
//   addAxisMapping c_PIMouseLookX ... IDAxis_0 ... -> the look axis
// (the trailing 0/1 argument is not a boolean -- 0 writes AxisRemap.first,
//  1 writes AxisRemap.second; see ControlMap::_addAxisMapping.)
//
// ControlMap::update resolves both mappings of an axis and keeps the larger
// magnitude, so moving the mouse wins over a held key. The engine then papers
// over the resulting double-control by zeroing one set or the other, in two
// places:
//   1. dice::bf::Setup::processPlayerInput  (gated on console game.mouseLook)
//   2. BFPlayer vtable slot 12, sub_00407EC0 (NOT gated on anything)
// Both zero Pitch, Roll and Yaw while PIMouseLook is held. (2) is the reason
// setting game.mouseLook 0 appears to do nothing, and it works on a stack copy,
// so the frame itself still looks correct to anything watching upstream.
//
// The fix has to keep the mouse out of the flight axes rather than just remove
// the suppression, otherwise the mouse flies the plane while you look around.
// So, while freelook is held, we hide the *mouse* mapping from ControlMap for
// Yaw/Pitch/Roll and let the other mapping win on its own. The mouse is never
// consulted for a flight axis, which excludes double control by construction
// instead of by timing.
//
// Only the mouse is excluded: a joystick axis is also MTFromAxis and must keep
// flying the plane, so the test is on `device`, not on `mapType`.

namespace {

// Written by the ControlMap hook (input thread), read by Game::addPlayerInput_hook
// (main thread). Plain relaxed atomics: worst case a consumer sees values one
// tick old, which is not observable at 60Hz.
std::atomic<uint32_t> g_restorable{ 0 };   // bitmask over kFlightAxes indices
std::atomic<float> g_flightAxis[3]{};   // PIYaw, PIPitch, PIRoll

// Whether to hold our view rotation against the server's. Set from the finished
// input frame on the main thread -- see input_onLocalPlayerInput.
std::atomic<bool> g_protectView{ false };

// The aircraft the local player is piloting, or null. Doubles as the "are we
// flying" flag -- a separate bool would just be a second copy of the same fact.
std::atomic<void*> g_localVehicle{ nullptr };

bool inAircraft()
{
    return g_localVehicle.load(std::memory_order_relaxed) != nullptr;
}

struct SuppressedMouse {
    AxisMapping* mapping;
    uint32_t     device;
};

constexpr int kFlightAxes[3] = { PIYaw, PIPitch, PIRoll };

// Finds the mouse mapping for one axis, and reports whether the axis has any
// non-mouse source to fall back on.
void classifyMappings(AxisRemap* remap, AxisMapping*& mouse, bool& hasOther)
{
    mouse = nullptr;
    hasOther = false;

    AxisMapping* slots[2] = { &remap->first, &remap->second };
    for (auto slot : slots) {
        if (slot->device == IDFNone) continue;
        if (slot->device & IDFMouse) mouse = slot;
        else hasOther = true;
    }
}

} // namespace


AxisRemap* ControlMap::findAxisRemap(int input) const
{
    if (!axisRemaps.head) return nullptr;
    for (auto node = axisRemaps.head->left; node != axisRemaps.head; node = node->next()) {
        if (node->pair.first == input) return node->pair.second;
    }
    return nullptr;
}

// Returns false when this map has no such trigger at all, which is how we tell
// the aircraft map apart from the soldier/land/sea/game maps -- update() is
// shared by all of them.
bool ControlMap::findTrigger(int input, bool& held) const
{
    held = false;
    if (!triggerRemaps.head) return false;
    for (auto node = triggerRemaps.head->left; node != triggerRemaps.head; node = node->next()) {
        if (node->pair.first == input) {
            auto remap = node->pair.second;
            if (!remap) return false;
            held = remap->triggerState;
            return true;
        }
    }
    return false;
}

static uintptr_t ControlMap_update_addr = 0x0061BEF0;
__declspec(naked) void ControlMap::update_orig(float deltaTime) noexcept
{
    _asm mov eax, ControlMap_update_addr
    _asm jmp eax
}

void ControlMap::update_hook(float deltaTime)
{
    // update() is shared by every control map -- soldier, land, sea, air, game --
    // and runs once per map per tick. Only a map that actually binds PIMouseLook
    // may publish state, otherwise the others overwrite it with their own answer
    // every tick and the published flags flicker. That flicker makes the half-4
    // view restore fire intermittently, which shows up as camera shake even when
    // freelook is never pressed.
    bool freelookHeld = false;
    if (!g_settings.dontBlockInputDuringFreeLook
        || !findTrigger(PIMouseLook, freelookHeld)) {
        update_orig(deltaTime);
        return;
    }

    SuppressedMouse saved[std::size(kFlightAxes)];
    AxisRemap* remaps[std::size(kFlightAxes)] = {};
    int numSaved = 0;

    // findTrigger reads the state update() computed last tick, since the trigger
    // loop runs after the axis loop we are about to enter. That leaves a single
    // tick of lag on pressing and releasing freelook, during which the vanilla
    // mapping applies. At the input rate this is not perceptible, and it avoids
    // resolving any mapping more than once.
    bool active = freelookHeld && inAircraft();

    // An axis is only safe to hand back to the main thread if no mouse mapping
    // can reach it: either we suppressed the one it had, or it never had one.
    // An axis bound to the mouse ALONE must be left suppressed -- restoring it
    // would put the mouse back on the stick, which is the whole thing we are
    // trying to avoid.
    uint32_t restorable = 0;

    if (active) {
        for (size_t i = 0; i < std::size(kFlightAxes); ++i) {
            auto remap = findAxisRemap(kFlightAxes[i]);
            if (!remap) continue;
            remaps[i] = remap;

            AxisMapping* mouse;
            bool hasOther;
            classifyMappings(remap, mouse, hasOther);

            if (!mouse) {
                restorable |= 1u << i;   // nothing to suppress, value is already clean
                continue;
            }
            if (!hasOther) continue;     // mouse-only axis: leave vanilla behaviour

            saved[numSaved].mapping = mouse;
            saved[numSaved].device = mouse->device;
            ++numSaved;
            restorable |= 1u << i;

            // Resolves to 0.0f instead of the mouse delta. Crucially the field
            // stays nonzero, so update() still resolves the mapping and still
            // writes axisValue -- see the IDFUnavailable comment in input.h.
            mouse->device = IDFUnavailable;
        }
    }

    update_orig(deltaTime);

    for (int i = 0; i < numSaved; ++i) {
        saved[i].mapping->device = saved[i].device;
    }

    // Publish the resolved flight axes so the main thread can undo the engine's
    // client-side suppression, which runs later in the same tick.
    for (size_t i = 0; i < std::size(kFlightAxes); ++i) {
        if (!(restorable & (1u << i))) continue;
        g_flightAxis[i].store(remaps[i] ? remaps[i]->axisValue : 0.0f,
                              std::memory_order_relaxed);
    }
    g_restorable.store(restorable, std::memory_order_relaxed);
}


// Keep the server from recentring our view while we lie about freelook.
//
// dice::ref2::world::Camera::handlePlayerInput multiplies the accumulated view
// rotation by 0.75 every tick whenever the frame says freelook is NOT held:
//
//   if (!template[toggleMouseLook] || testInput(frame, PIMouseLook))
//        return RotationalBundle::handlePlayerInput(...);
//   else { rotationDelta = 0; rotationInput = 0; viewRotation *= 0.75f; setState(); }
//
// Our own prediction is fine -- it runs with the true frame and takes the first
// branch. But the server runs it with the sanitised frame, decays its copy, and
// ships the recentred angles back in NetworkableRotationalBundle::setNetUpdate,
// which writes object+284 and calls setState. That is the "server rolls the
// camera back to centre" symptom.
//
// The engine already has a "don't correct the object I control" flag (computed at
// the top of GhostManager::readControlObjectState), but it only matches the
// control object's own network id -- a camera is a separate child bundle with its
// own id, so it isn't covered.
//
// So: let setNetUpdate run in full, then put the three floats back. Letting it run
// matters -- it consumes the bitstream, and skipping it would desync the reader
// (the function's own "Size differs bits read" check would fire). Restoring only
// the field is invisible to the stream.
//
// No setState call afterwards: the bundle integrates its rotation and calls
// setState itself every tick, so the restored value is picked up on the next
// integration. Worst case is one frame of visual lag.

static uintptr_t NRB_setNetUpdate_addr = 0x00559900;
__declspec(naked) void NetworkableRotationalBundle::setNetUpdate_orig(
    void* bs, float time, bool applyNow, void* baseLine, bool b) noexcept
{
    _asm mov eax, NRB_setNetUpdate_addr
    _asm jmp eax
}

// True only for the recentring camera of the aircraft we are currently piloting.
//
// The template test is the important one: a plane's turret is a RotationalBundle
// child of the same vehicle, and freezing ITS networked rotation would be a real
// bug. Only a camera with ObjectTemplate.toggleMouseLook set can recentre, so
// that flag is exactly the right discriminator.
static bool shouldProtectView(void* obj)
{
    if (!obj) return false;
    if (!g_protectView.load(std::memory_order_relaxed)) return false;

    void* vehicle = g_localVehicle.load(std::memory_order_relaxed);
    if (!vehicle) return false;

    auto tmpl = *reinterpret_cast<const uint8_t**>(
        reinterpret_cast<char*>(obj) + OBJ_TEMPLATE);
    if (!tmpl || tmpl[TMPL_TOGGLE_MOUSELOOK] == 0) return false;

    // Walk up to the vehicle. Bounded: a corrupt or unexpected parent chain must
    // not spin. Failing to find it just means we do not protect, which is safe.
    void* p = *reinterpret_cast<void**>(reinterpret_cast<char*>(obj) + OBJ_PARENT);
    for (int depth = 0; p && depth < 8; ++depth) {
        if (p == vehicle) return true;
        p = *reinterpret_cast<void**>(reinterpret_cast<char*>(p) + OBJ_PARENT);
    }
    return false;
}

void NetworkableRotationalBundle::setNetUpdate_hook(
    void* bs, float time, bool applyNow, void* baseLine, bool b)
{
    auto obj = *reinterpret_cast<char**>(reinterpret_cast<char*>(this) + NRB_OBJECT);

    float saved[3];
    bool protect = g_settings.dontBlockInputDuringFreeLook && shouldProtectView(obj);
    if (protect) {
        memcpy(saved, obj + RB_VIEW_ROTATION, sizeof(saved));
    }

    setNetUpdate_orig(bs, time, applyNow, baseLine, b);

    if (protect) {
        memcpy(obj + RB_VIEW_ROTATION, saved, sizeof(saved));
    }
}

void input_onLocalPlayerInput(PlayerInput* input, bool localPlayerInAircraft,
                              void* localVehicle)
{
    g_localVehicle.store(localPlayerInAircraft ? localVehicle : nullptr,
                         std::memory_order_relaxed);

    if (!g_settings.dontBlockInputDuringFreeLook) return;

    // Drive the view protection from THIS frame, on this thread. The ControlMap
    // hook could publish it instead, but its trigger state is computed at the end
    // of the previous update and it runs on the input thread, so the flag would
    // arrive a tick plus a cross-thread hop late. The frame here is the same one
    // the camera path consumes, so there is no skew to reason about.
    //
    // Releasing freelook drops protection immediately. The server has been
    // recentring its copy all along, so the ghost pulls the view to centre on the
    // next packet -- which is exactly what releasing freelook is supposed to do.
    bool freelookHeld =
        (input->mask & (1ull << PIMouseLook)) != 0
        && input->controls[PIMouseLook] > 0.5f;

    g_protectView.store(localPlayerInAircraft && freelookHeld,
                        std::memory_order_relaxed);

    // g_restorable is only recomputed while the air control map is being
    // updated. Leaving the plane switches the active map (Setup::setPlayerControlMap),
    // so update_hook stops running and the flag would stay frozen at its last
    // value -- stamping stale flight axes into the soldier's frame forever, which
    // on foot means controls[PIYaw] spins the player. Re-check the live,
    // main-thread answer before touching anything.
    if (!localPlayerInAircraft) {
        g_restorable.store(0, std::memory_order_relaxed);
        return;
    }

    uint32_t restorable = g_restorable.load(std::memory_order_relaxed);
    if (!restorable) return;

    // dice::bf::Setup::processPlayerInput has just zeroed Pitch/Roll/Yaw because
    // PIMouseLook is held (unless the player set game.mouseLook 0). Put the
    // keyboard/joystick values back. These carry no mouse contribution at all --
    // the ControlMap hook above kept the mouse mapping out of them.
    //
    // Writing controls[] directly is safe: bits 0..2 of the mapped mask are set
    // for any control map that binds these axes, which is the only case in which
    // the override arms.
    for (size_t i = 0; i < std::size(kFlightAxes); ++i) {
        if (!(restorable & (1u << i))) continue;
        input->controls[kFlightAxes[i]] = g_flightAxis[i].load(std::memory_order_relaxed);
    }
}

// Hide freelook from the server so a stock server flies the plane from our keys.
// Not separately configurable: without it the feature simply does not work in
// multiplayer, so it is part of what dontBlockInputDuringFreeLook means.
//
// The server runs the same suppression we patched out of BFPlayer::handleInput, so
// against an unpatched server the flight axes are discarded no matter what we send
// and the plane visibly rolls back. Rather than requiring every server operator to
// patch, just don't tell the server we are freelooking: clear the PIMouseLook bit
// and the look axes in the outgoing PlayerAction. The server then takes the
// `else` branch, which zeroes MouseLookX/Y (already zero) and passes Pitch, Roll
// and Yaw through untouched.
//
// This has to happen AFTER the client has round-tripped its own input. In
// GameClient::updatePlayers the sequence is:
//
//   PlayerAction::set(action, frame);     // frame -> wire form
//   PlayerAction::get(action, frame);     // wire form -> frame, OVERWRITING it
//   ...
//   BFPlayer::handleInput(player, frame); // local prediction and camera
//   GameClient::storeAndSendPlayerAction(player, action);
//
// The client deliberately quantises through the wire format so its prediction
// matches the server bit-for-bit -- which means the local camera comes from the
// reconstructed frame. Sanitising inside PlayerAction::set would therefore kill
// our own freelook. Doing it in the store/send path leaves the already-rebuilt
// frame alone, so the camera still looks around while the wire stays quiet.
//
// GameClient::storeAndSendPlayerAction (0x004904F0) splits into two steps, and the
// sanitisation must land on exactly one of them:
//
//   entry = ActionBuffer::add(action);   // 0x00484820 -- correction replay history
//   PlayerActionSendRing::add(entry);    // 0x004B7200 -- what goes on the wire
//
// Sanitise the SEND RING only. Sanitising the stored history instead causes a
// visible bug: GameClient::handleCorrection replays un-acked actions after every
// server correction, and replaying actions with the mouselook bit cleared makes
// the CLIENT run the recentring decay on its own camera. Several ticks replayed at
// once -- five is 0.75^5, about a quarter -- so the view snaps hard toward centre
// once per correction packet. That is the "recentring in bursts every 100-250ms"
// symptom; the interval is the ghost packet rate.
//
// Keeping the mouselook bit in the stored copy costs nothing in consistency,
// because it does not affect physics: with BFPlayer::handleInput patched the
// flight axes survive whether the bit is set or not. It only steers the camera.
//
// We pass a sanitised local copy rather than mutating the entry, so the history
// the engine keeps stays untouched.
//
// (Hooking 0x004904F0 directly is not an option anyway: bf42++ already owns that
// address via patch_drop_actions, and two hooks on one address do not compose.)

static uintptr_t PlayerActionSendRing_add_addr = 0x004B7200;
__declspec(naked) char PlayerActionSendRing::add_orig(const PlayerAction* action) noexcept
{
    _asm mov eax, PlayerActionSendRing_add_addr
    _asm jmp eax
}

char PlayerActionSendRing::add_hook(const PlayerAction* action)
{
    // Must be gated on being in an aircraft. On foot PIMouseLook can be bound to
    // something else entirely, and MouseLookX/Y are the soldier's primary look
    // axes -- zeroing those would stop the player looking around at all.
    if (action
        && g_settings.dontBlockInputDuringFreeLook
        && inAircraft()
        && (action->buttons0 & PA_BUTTON_MOUSELOOK))
    {
        PlayerAction sanitised = *action;
        sanitised.buttons0 &= ~PA_BUTTON_MOUSELOOK;
        sanitised.mouseLookX = 0;
        sanitised.mouseLookY = 0;
        return add_orig(&sanitised);
    }
    return add_orig(action);
}

void input_hook_init()
{
    if (!g_settings.dontBlockInputDuringFreeLook) return;

    // 6 bytes: push ebx / mov ebx,[esp+8] / push ebp
    ControlMap_update_addr = (uintptr_t)hook_function(
        ControlMap_update_addr, 6, method_to_voidptr(&ControlMap::update_hook));

    // 6 bytes: push esi / mov esi,ecx / mov edx,[esi+5Ch]
    PlayerActionSendRing_add_addr = (uintptr_t)hook_function(
        PlayerActionSendRing_add_addr, 6,
        method_to_voidptr(&PlayerActionSendRing::add_hook));

    // 6 bytes: sub esp,11Ch
    NRB_setNetUpdate_addr = (uintptr_t)hook_function(
        NRB_setNetUpdate_addr, 6,
        method_to_voidptr(&NetworkableRotationalBundle::setNetUpdate_hook));

    debuglogt("input: fly-with-keys-during-freelook enabled\n");
}

#pragma warning(pop)
