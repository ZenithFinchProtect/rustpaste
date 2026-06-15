#ifndef _INCLUDE_GUARD
#define _INCLUDE_GUARD

#include "stdafx.hpp"

#include "communication.hpp"
#include "crypto.hpp"
#include "lazy_importer.hpp"
#include "math.hpp"
#include "mutex.hpp"
#include "syscall.hpp"
#include "xorfloat.hpp"
#include "xorstr.hpp"

using namespace std::chrono;
using namespace std::this_thread;

namespace offsets {
// ---- GameAssembly.dll static fields (RVAs) - 2026-06-10 dump ----
constexpr uint64_t gom_offset = 0x17C1F18;
constexpr uint64_t bn_offset =
    0xE2D5ED8; // base_networkable klass (was 0xE20FB70)
constexpr uint64_t main_camera_c =
    0xE2C22B0; // main_camera_c klass    (was 0xE210720)
constexpr uint64_t game_manager_c =
    0xE2C1A48; // game_manager klass     (was 0xE21F220)
constexpr uint64_t effect_network_c =
    0xE248118; // effect_network klass (was 0xE1BB7E0)
constexpr uint64_t player_eyes_c =
    0xE2834A0; // player_eyes_c klass  (was 0xE216C98)
constexpr uint64_t fov_offset = 0x2FFC2B8;
constexpr uint64_t global_offset = 0x2FFDF68;
constexpr uint64_t admin_offset = 0x2FF8D70;
constexpr uint64_t timescale = 0x17C1CD0;
constexpr uint64_t effect = 0x1D98DF4;

// ---- BaseNetworkable / entity walk (2026-06-10 dump) ----
constexpr uint64_t cl_ent = 0xB8;    // base_networkable.static_fields
constexpr uint64_t ent_realm = 0x10; // base_networkable.parent_static_fields
constexpr uint64_t buf_list = 0x18;  // base_networkable.entities (was 0x10)
// List<T> layout: +0x10 = T[] array ptr, +0x18 = int32 count
constexpr uint64_t obj_list = 0x10;
constexpr uint64_t obj_size = 0x18;
constexpr uint64_t base_ply = 0x28;
constexpr uint64_t class_name = 0x10;
constexpr uint64_t player_visual = 0x08;
constexpr uint64_t obj_state = 0x38;
constexpr uint64_t obj_pos = 0x90;
constexpr uint64_t element =
    0x30; // base_networkable.wrapper_class_ptr (was 0x08)

// ---- Camera (2026-06-10 dump) ----
constexpr uint64_t camera_static = 0xB8; // main_camera::static_fields
constexpr uint64_t camera_object = 0x88; // main_camera::camera_object (was 0x70)
constexpr uint64_t camera_entity = 0x10; // Object::m_CachedPtr

// ---- BaseCombatEntity / BaseEntity (verified from rust-dumper) ----
constexpr uint64_t model = 0x1A8;             // BaseEntity.model
constexpr uint64_t base_entity_flags = 0x1B0; // BaseEntity.flags
constexpr uint64_t lifestate = 0x290;         // BaseCombatEntity.lifestate
constexpr uint64_t health = 0x29C;            // BaseCombatEntity._health
constexpr uint64_t maxHealth = 0x2A0;         // BaseCombatEntity._maxHealth

// ---- BasePlayer (2026-06-10 alt dump) ----
constexpr uint64_t player_model =
    0x2C8; // base_player::playerModel
constexpr uint64_t base_movement_field = 0x308; // base_player::base_movement
constexpr uint64_t modelState = 0x378;          // base_player::model_state
constexpr uint64_t steam_id =
    0x408;                      // base_player::userID
constexpr uint64_t uid = 0x408; // alias for userID
constexpr uint64_t basemovement = 0x308; // alias for base_movement_field
constexpr uint64_t team = 0x4E0; // base_player::currentTeam
constexpr uint64_t active_item =
    0x510; // base_player::clActiveItem
constexpr uint64_t player_input =
    0x628; // base_player::player_input
constexpr uint64_t display_name =
    0x670;                       // base_player::_displayName
constexpr uint64_t eyes = 0x3E0; // base_player::eyes
constexpr uint64_t inventory =
    0x3A8; // base_player::inventory
constexpr uint64_t PlayerFlags = 0x670;    // base_player::playerFlags
constexpr uint64_t currentGesture = 0x500; // base_player::currentGesture

// Fields not in the latest dump - kept from prior reverse (UNVERIFIED on this
// build):
constexpr uint64_t user_id_string = 0x2C8;             // legacy
constexpr uint64_t mounted = 0x578;                    // legacy
constexpr uint64_t looking_at_entity = 0x5C8;          // legacy
constexpr uint64_t weaponMoveSpeedScale = 0x750;       // legacy
constexpr uint64_t clothingBlocksAiming = 0x754;       // legacy
constexpr uint64_t clothingMoveSpeedReduction = 0x758; // legacy
constexpr uint64_t clothingWaterSpeedBonus = 0x75C;    // legacy
constexpr uint64_t equippingBlocked = 0x764;           // legacy

// ---- LocalPlayer static-slot fast path (NeoRed v6) ----
// Direct, no entity-walk required:
//   klass = read(GA + local_player_slot_klass)
//   sf    = read(klass + camera_static = 0xB8)
//   local = read(sf + local_player_self_static_off)
constexpr uint64_t local_player_slot_klass = 0xE270F98;
constexpr uint64_t local_player_self_static_off = 0x230;

// ---- ModelState (latest dump) ----
constexpr uint64_t flags = 0x48;      // model_state.flags (was 0x14)
constexpr uint64_t waterLevel = 0x58; // kept (not in latest dump)
constexpr uint64_t lookDir = 0x20;    // kept
constexpr uint64_t swimming = 0x136;  // kept (bitflag)
constexpr uint64_t climbing = 0x132;  // kept (bitflag)

// ---- PlayerWalkMovement ----
constexpr uint64_t target_movement = 0x34;
constexpr uint64_t groundangle = 0x70;
constexpr uint64_t groundangle_new = 0x78;
constexpr uint64_t maxangle_walk = 0x80;
constexpr uint64_t maxangle_climb = 0x80;
constexpr uint64_t gravity = 0x88;
constexpr uint64_t maxVelocity_walk = 0x90;
constexpr uint64_t groundtime = 0x98;
constexpr uint64_t walk_body = 0xE8;
constexpr uint64_t jumping = 0x1A8;
constexpr uint64_t grounded = 0x1BC;
constexpr uint64_t flying = 0x1C4;

// ---- PlayerInput / PlayerEyes ----
constexpr uint64_t input_state = 0x28;
constexpr uint64_t body_angles =
    0x44; // PlayerInput.bodyAngles (relative to player_input)
constexpr uint64_t recoil_angles =
    0x6C; // not in latest dump, kept from previous

constexpr uint64_t head_angles = 0x28; // PlayerEyes.headAngles
constexpr uint64_t viewOffset = 0x40;
constexpr uint64_t bodyRotation = 0x50;

// ---- PlayerModel (verified from rust-dumper) ----
constexpr uint64_t pm_position = 0x2F0;     // PlayerModel.position
constexpr uint64_t velocity = 0x314;        // PlayerModel.newVelocity
constexpr uint64_t oldvelocity = 0x314;     // alias - same field
constexpr uint64_t multi_mesh = 0x408;      // PlayerModel._multiMesh
constexpr uint64_t pm_isLocalPlayer = 0x34; // kept from prior reverse
constexpr uint64_t pm_visible = 0xBC;       // kept from prior reverse
constexpr uint64_t pm_rotation = 0x168;     // kept from prior reverse
constexpr uint64_t pm_skinColor = 0x274;    // kept from prior reverse

// ---- Model (skeleton) ----
constexpr uint64_t rootBone = 0x28;
constexpr uint64_t headBone = 0x30;
constexpr uint64_t eyeBone = 0x38;
constexpr uint64_t bone_transforms =
    0x50; // Model.boneTransforms (was hardcoded 0x48)

// ---- Inventory / ItemContainer / Item (2026-06-10 dump) ----
constexpr uint64_t belt = 0x58;      // PlayerInventory.containerbelt   (was 0x60)
constexpr uint64_t wear = 0x78;      // PlayerInventory.containerwear   (was 0x58)
constexpr uint64_t item_list = 0x10; // ItemContainer.item_list         (was 0x58)
constexpr uint64_t item_amount = 0x18;
constexpr uint64_t item_condition = 0x20;
constexpr uint64_t item_heldEntity = 0x1C;
constexpr uint64_t item_position = 0x24;
constexpr uint64_t item_maxCondition = 0x38;
constexpr uint64_t item_uid = 0xB8;
constexpr uint64_t item_info = 0xD8;

// ---- ItemDefinition ----
constexpr uint64_t item_id = 0x20;
constexpr uint64_t shortname = 0x28;
constexpr uint64_t item_displayName = 0x40;
constexpr uint64_t item_category = 0x58;
constexpr uint64_t item_stackable = 0x78;
constexpr uint64_t item_rarity = 0x90;

// ---- HeldEntity / BaseProjectile ----
constexpr uint64_t heldEnt_viewModel = 0x1E8;
constexpr uint64_t heldEnt_isDeployed = 0x220;
constexpr uint64_t heldEnt_hostileAnimation = 0x230;
constexpr uint64_t heldEnt_holsterInfo = 0x278;

constexpr uint64_t base_projectile = 0x98;
constexpr uint64_t projectileVelocityScale = 0x35C;
constexpr uint64_t automatic = 0x360;
constexpr uint64_t needsCycle = 0x368;
constexpr uint64_t magazine = 0x3A8;
constexpr uint64_t isCycling = 0x3A4;
constexpr uint64_t aiming = 0x3B0;
constexpr uint64_t sway = 0x3C8;
constexpr uint64_t aimSwaySpeed = 0x3CC;
constexpr uint64_t recoil_properties = 0x3D0;
constexpr uint64_t aimcone = 0x3E0;
constexpr uint64_t hipAimCone = 0x3E4;
constexpr uint64_t aimconePenaltyPerShot = 0x3E8;
constexpr uint64_t aimConePenaltyMax = 0x3EC;
constexpr uint64_t stancePenaltyScale = 0x3F8;
constexpr uint64_t noAimingWhileCycling = 0x3FD;
constexpr uint64_t manualCycle = 0x3FE;

// ---- Magazine ----
constexpr uint64_t magazine_capacity = 0x18;
constexpr uint64_t magazine_contents = 0x1C;
constexpr uint64_t ammotype = 0x20; // Magazine.definition

// ---- RecoilProperties ----
constexpr uint64_t recoilYawMin = 0x18;
constexpr uint64_t recoilYawMax = 0x1C;
constexpr uint64_t recoilPitchMin = 0x20;
constexpr uint64_t recoilPitchMax = 0x24;
constexpr uint64_t recoil_timeToTakeMin = 0x28;
constexpr uint64_t recoil_timeToTakeMax = 0x2C;
constexpr uint64_t recoil_ADSScale = 0x30;
constexpr uint64_t recoil_movementPenalty = 0x34;
constexpr uint64_t recoil_clampPitch = 0x38;
constexpr uint64_t recoil_pitchCurve = 0x40;
constexpr uint64_t recoil_yawCurve = 0x48;
constexpr uint64_t newRecoilOverride = 0x80;

// ---- Bow / Compound bow / FlintStrike ----
constexpr uint64_t successFraction = 0x488;
constexpr uint64_t bow_attackReady = 0x488;
constexpr uint64_t strikeRecoil = 0x490;
constexpr uint64_t bow_wasAiming = 0x498;
constexpr uint64_t stringHoldDurationMax = 0x4A0;
constexpr uint64_t stringBonusVelocity = 0x4AC;
}; // namespace offsets

namespace addr {
extern uintptr_t unityplayer;
extern uintptr_t gameassembly;

extern uintptr_t base_networkable;
extern uintptr_t client_entities;
extern uintptr_t entity_realm;
extern uintptr_t buffer_list;
extern uintptr_t object_list;
extern uintptr_t camera_instance;
extern uintptr_t tod_sky;
extern std::size_t object_size;
}; // namespace addr

namespace monitor {
extern HWND game_window;
extern float width, height;
}; // namespace monitor

namespace security {
extern bool unload;
extern bool eject;
}; // namespace security

namespace syscalls {
HWND find_window(const char *szClassName, const char *szWindowName);
SHORT get_async_key_state(SHORT vk);
HWND get_foreground_window();
BOOL show_window(HWND hWnd, int nCmdShow);
BOOL set_window_pos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx,
                    int cy, UINT uFlags);
HWND get_window(HWND hWnd, UINT uCmd);
int set_window_rgn(HWND hWnd, HRGN hRgn, BOOL bRedraw);
}; // namespace syscalls
enum : char { TYPE_LOG, TYPE_DEBUG, TYPE_ERROR, TYPE_WARNING };
constexpr inline const char *GET_PREFIX(const char type) {
  switch (type) {
  case TYPE_LOG:
    return "\u001b[37m[-] ";
    break;
  case TYPE_DEBUG:
    return "\u001b[36m[+] ";
    break;
  case TYPE_ERROR:
    return "\u001b[31m[x] ";
    break;
  case TYPE_WARNING:
    return "\u001b[33m[!] ";
    break;
  default:
    return "\u001b[37m[?] ";
    break;
  }
}

// Always-on logging (was previously gated by _DEBUG). Output goes to the
// console allocated in main(). Safe to call even before AllocConsole - printf
// will just fail silently if there's no console attached yet.
namespace logging {
inline void print(const char type, const char *format, ...) {
  char buffer[512];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf_s(buffer, 512, format, arguments);
  va_end(arguments);

  const char *PREFIX = GET_PREFIX(type);
  printf("%s%s\n", PREFIX, buffer);
  fflush(stdout);
}
}; // namespace logging

#define VASSERT(x)                                                             \
  if (!x)                                                                      \
    return;

#define CASSERT(x)                                                             \
  if (!x)                                                                      \
    continue;

#define ASSERT(x)                                                              \
  if (!x)                                                                      \
    return false;

#define BASSERT(x)                                                             \
  if (!x)                                                                      \
    break;
#endif