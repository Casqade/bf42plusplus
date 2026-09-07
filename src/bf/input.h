#pragma once

#include "generic.h"
#include "stl.h"

#include <cstddef>
#include <cstdint>


// ref2::io::ControlMap internals.
//
// Offsets below are taken from Engine/Io/ControlMap.hpp in the reversed source
// tree and verified against dice::ref2::io::ControlMap::update (0x0061BEF0) in
// BF1942 v1.61b, which touches nearly all of them:
//   [ebp+2Ch]/[ebp+30h]  axisRemaps head/size
//   [ebp+38h]/[ebp+3Ch]  triggerRemaps head/size
//   [esi+00h]/[esi+04h]  AxisRemap.first.device / .mapType
//   [esi+58h]/[esi+5Ch]  AxisRemap.second.device / .mapType
//   [esi+B0h]/[esi+B4h]  AxisRemap.isAxisValueAnalogue / .axisValue
//   [esi+60h]            TriggerRemap.triggerState

enum PlayerInputMap {
    PIYaw = 0,
    PIPitch = 1,
    PIRoll = 2,
    PIThrottle = 3,
    PIMouseLookX = 4,
    PIMouseLookY = 5,
    PICameraX = 6,   // resolved by ControlMap but never read into the frame
    PICameraY = 7,   // and never serialised by PlayerAction
    PIMouseLook = 11,
};

enum InputDeviceFlags : uint32_t {
    IDFNone = 0,
    IDFMouse = 1 << 0,
    IDFKeyboard = 1 << 1,
    IDFGameController0 = 1 << 2,

    // Not a real device. ControlMap::resolveAxisMapping looks the flags up via
    // IInputDeviceManager::getInputDevice() and returns 0.0f when that misses,
    // but ControlMap::update only tests the field for nonzero before deciding
    // whether to resolve a mapping at all. A nonzero value that matches no
    // device therefore makes a mapping resolve to a harmless zero instead of
    // being skipped -- which is exactly what we want, because skipping the
    // first mapping would leave axisValue holding last frame's value.
    IDFUnavailable = 0x40000000,
};

enum ControlMapType : uint32_t {
    MTNone = 0,
    MTFromAxis = 1,          // analogue axis: mouse or game controller stick
    MTFromButton = 2,
    MTFromKey = 3,
    MTFromPOV = 4,
    MTFromButtonAndKey = 5,
};

struct AxisMapping {
    uint32_t device;        // 0x00  InputDeviceFlags
    uint32_t mapType;       // 0x04  ControlMapType
    uint8_t  unk08[0x38];   // 0x08  options, axis, button/key pairs, POV, ...
    float    value;         // 0x40  button rise/fall ramp accumulator
    int      unk44;         // 0x44
    double   unk48;         // 0x48  button rise/fall ramp accumulator
    bool     inverted;      // 0x50
    uint8_t  unk51[3];
    int      unk54;         // 0x54
};
static_assert(sizeof(AxisMapping) == 0x58);

struct AxisRemap {
    AxisMapping first;              // 0x00
    AxisMapping second;             // 0x58
    bool        isAxisValueAnalogue;// 0xB0
    uint8_t     unkB1[3];
    float       axisValue;          // 0xB4  resolved value for this tick
};
static_assert(sizeof(AxisRemap) == 0xB8);

struct TriggerRemap {
    uint8_t first[0x30];    // 0x00  TriggerMapping
    uint8_t second[0x30];   // 0x30  TriggerMapping
    bool    triggerState;   // 0x60
    uint8_t unk61[3];
    int     unk64;
};
static_assert(sizeof(TriggerRemap) == 0x68);

class ControlMap {
public:
    uint8_t unk00[0x28];                            // 0x00  vtable, name, id, ...
    bfs::map<int, AxisRemap*>    axisRemaps;        // 0x28
    bfs::map<int, TriggerRemap*> triggerRemaps;     // 0x34

    AxisRemap* findAxisRemap(int input) const;
    bool findTrigger(int input, bool& held) const;

    void update_orig(float deltaTime) noexcept;
    void update_hook(float deltaTime);
};
static_assert(offsetof(ControlMap, axisRemaps) == 0x28);
static_assert(offsetof(ControlMap, triggerRemaps) == 0x34);


// dice::bf::PlayerAction -- the 24-byte wire form of one input frame, produced by
// PlayerAction::set and consumed by PlayerAction::get. Offsets verified against
// the disassembly of PlayerAction::set (community debug build, 0x004D45BE):
//   mov [esi],ax / [esi+2] / [esi+4] / [esi+6] / [esi+8] / [esi+0Ah]  -- six axes
//   and dword ptr [esi+0Ch], 0 / and dword ptr [esi+10h], 0           -- button mask
//   or  dword ptr [esi+0Ch], 8   after testInput(frame, PIMouseLook)  -- bit 3
//
// The mask bits are a dense packing in field order, NOT the PlayerInputMap ids:
// Fire=0, Action=1, Use=2, MouseLook=3, Walk=4, Run=5, ... Communication=40
// (which matches the "input out of range ... bit:40" message in that function).
struct PlayerAction {
    int16_t  yaw;         // 0x00  PIYaw
    int16_t  pitch;       // 0x02  PIPitch
    int16_t  roll;        // 0x04  PIRoll
    int16_t  throttle;    // 0x06  PIThrottle
    int16_t  mouseLookX;  // 0x08  PIMouseLookX
    int16_t  mouseLookY;  // 0x0A  PIMouseLookY
    uint32_t buttons0;    // 0x0C  bits 0..31
    uint32_t buttons1;    // 0x10  bits 32..63
    uint32_t unk14;       // 0x14
};
static_assert(sizeof(PlayerAction) == 0x18);

enum : uint32_t { PA_BUTTON_MOUSELOOK = 1u << 3 };

// The 3-slot redundancy ring that feeds the outgoing packet. Distinct from the
// ActionBuffer history used for correction replay -- see the comment on
// PlayerActionSendRing::add_hook for why that distinction is load-bearing.
class PlayerActionSendRing {
public:
    char add_orig(const PlayerAction* action) noexcept;
    char add_hook(const PlayerAction* action);
};


// Field offsets for the camera-recentring workaround. All MSVC/retail, derived
// from dice::ref2::world::Camera::handlePlayerInput and confirmed against
// NetworkableRotationalBundle::init and ::setNetUpdate. Note gcc lays this class
// out 8 bytes tighter, so the lnxded offsets do NOT transfer.
enum : size_t {
    // dice::ref2::world::RotationalBundle (base of Camera)
    RB_VIEW_ROTATION = 284,     // 3 floats, accumulated view angles. Networked.
                                // 296..304 rotation delta (networked),
                                // 308..316 rotation input (not networked).

    // dice::ref2::world::IObject -- matches bf42++'s own IObject layout, and
    // +76 is independently confirmed by Camera::handlePlayerInput reading the
    // template through it.
    OBJ_TEMPLATE = 76,
    OBJ_PARENT = 80,

    // ObjectTemplateCamera. Byte, default 0, written by "ObjectTemplate.toggleMouseLook".
    TMPL_TOGGLE_MOUSELOOK = 626,

    // NetworkableRotationalBundle -> the RotationalBundle it belongs to.
    NRB_OBJECT = 60,
};

class NetworkableRotationalBundle {
public:
    void setNetUpdate_orig(void* bs, float time, bool applyNow,
                           void* baseLine, bool b) noexcept;
    void setNetUpdate_hook(void* bs, float time, bool applyNow,
                           void* baseLine, bool b);
};


// Called from Game::addPlayerInput_hook, on the main thread, once per tick with
// the local player's finished input frame. Publishes whether the local player is
// in an aircraft (consumed by the ControlMap hook on the input thread) and puts
// the keyboard/controller flight axes back into the frame after the engine's
// client-side freelook suppression has zeroed them.
void input_onLocalPlayerInput(PlayerInput* input, bool localPlayerInAircraft,
                              void* localVehicle);

void input_hook_init();
