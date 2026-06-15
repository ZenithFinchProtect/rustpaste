#include "cheat.hpp"
#include "include.hpp"
#include "unity.hpp"
#include "math.hpp"
#include "sdk.hpp"

#include <cmath>
#include <cstring>

static Vector3 safe_unity_world_position ( CSharedMemComms* drv, uintptr_t transform_node );

// NeoRed v6 (Unity 6 / 2026-06-08) handle resolution scheme.
//
// Post-Unity-6 Rust uses a mixed-tag GCHandle scheme:
//   LSB(decrypted) == 1  -> legacy GCHandle index, would need il2cpp_gchandle_get_target
//                           (calling that from external is non-trivial; we return 0 and
//                           rely on the LSB=0 path being the common case in this build).
//   LSB(decrypted) == 0  -> direct slot ADDRESS in the GC table; dereference once to get
//                           the managed pointer.
inline uintptr_t resolve_tagged_handle(CSharedMemComms* drv, uint64_t decrypted) {
  if (!decrypted) return 0;
  if (decrypted & 1) {
    // Legacy GCHandle index path - external resolution not implemented.
    return 0;
  }
  // Direct-slot path. Slot is the full u64 treated as an address.
  if (decrypted < 0x10000ULL || decrypted > 0x7FFFFFFFFFFFULL)
    return 0;
  return drv->read<uintptr_t>((uintptr_t)decrypted);
}

// Updated 2026-06-10 decryptions::base_networkable
//   v += 0x9C7DA5DF
//   v = ROL(v, 29)   i.e. (v << 29) | (v >> 3)
//   v -= 0x1C287C47
//   v ^= 0x595DCF40
uintptr_t decrypt_client_entities(CSharedMemComms* drv, uintptr_t address) {
  uint64_t encrypted = drv->read<uint64_t>(address + 0x18);
  if (!encrypted) return 0;
  uint32_t* chunks = reinterpret_cast<uint32_t*>(&encrypted);
  for (int i = 0; i < 2; i++) {
    uint32_t v = chunks[i];
    v += 0x9C7DA5DFu;
    v = (v << 0x1D) | (v >> 0x03);  // ROL 29
    v -= 0x1C287C47u;
    v ^= 0x595DCF40u;
    chunks[i] = v;
  }
  return resolve_tagged_handle(drv, encrypted);
}

// Updated 2026-06-10 decrypt clActiveItem
//   v ^= 0x26C05F08
//   v += 0x0B3B38C5
//   v = ROL(v, 21)  i.e. (v << 21) | (v >> 11)
inline uint64_t decrypt_cl_active_data(uint64_t a1) {
  uint64_t encrypted = a1;
  uint32_t* chunks = reinterpret_cast<uint32_t*>(&encrypted);
  for (int i = 0; i < 2; i++) {
    uint32_t v = chunks[i];
    v ^= 0x26C05F08u;
    v += 0x0B3B38C5u;
    v = (v << 0x15) | (v >> 0x0B);  // ROL 21
    chunks[i] = v;
  }
  return encrypted;
}

// Updated 2026-06-10 decryptions::base_networkable_entity_list
//   v = ROL(v, 6)   i.e. (v << 6) | (v >> 26)
//   v += 0x718B2E6D
//   v ^= 0x594204AB
uintptr_t decrypt_entity_list(CSharedMemComms* drv, uintptr_t address) {
  uint64_t encrypted = drv->read<uint64_t>(address + 0x18);
  if (!encrypted) return 0;
  uint32_t* chunks = reinterpret_cast<uint32_t*>(&encrypted);
  for (int i = 0; i < 2; i++) {
    uint32_t v = chunks[i];
    v = (v << 0x06) | (v >> 0x1A);  // ROL 6
    v += 0x718B2E6Du;
    v ^= 0x594204ABu;
    chunks[i] = v;
  }
  return resolve_tagged_handle(drv, encrypted);
}

// Updated 2026-06-10 decryptions::player_inventory
//   v -= 0x0DD388D1
//   v ^= 0x715CB113
//   v += 0x26879A75
inline uint64_t decrypt_player_inventory(uint64_t a1) {
  uint64_t encrypted = a1;
  uint32_t* chunks = reinterpret_cast<uint32_t*>(&encrypted);
  for (int i = 0; i < 2; i++) {
    uint32_t v = chunks[i];
    v -= 0x0DD388D1u;
    v ^= 0x715CB113u;
    v += 0x26879A75u;
    chunks[i] = v;
  }
  return encrypted;
}

uintptr_t rust::get_base_networkable ( CSharedMemComms* drv )
{
	uintptr_t address = addr::gameassembly + offsets::bn_offset;
	uintptr_t basenetworkable = drv->read<uintptr_t> ( address );

	if ( !basenetworkable )
		return NULL;

	return basenetworkable;
}

uintptr_t rust::get_client_entities ( CSharedMemComms* drv )
{
	// klass + offsets::cl_ent (0xB8) -> static_fields struct ptr
	uintptr_t static_fields = drv->read<uintptr_t> ( addr::base_networkable + offsets::cl_ent );
	if ( !static_fields )
		return NULL;

	// static_fields + offsets::element (0x20 in current dump = wrapper_class_ptr)
	// -> HiddenValue<T> wrapper. The encrypted u64 lives at wrapper + 0x18.
	uintptr_t wrapper_ptr = drv->read<uintptr_t> ( static_fields + offsets::element );
	if ( !wrapper_ptr )
		return NULL;

	return decrypt_client_entities ( drv, wrapper_ptr );
}

// NeoRed v6 fast path - resolve the local BasePlayer from a static slot, no
// entity-walk required. Path: GameAssembly + slot_klass_rva -> klass +
// static_fields(0xB8) -> +self_static_off (0x230) -> local BasePlayer*.
// Returns 0 if the slot isn't populated yet (not in a server / not spawned).
uintptr_t rust::get_local_player_static ( CSharedMemComms* drv )
{
	if ( !addr::gameassembly )
		return 0;

	uintptr_t klass = drv->read<uintptr_t> ( addr::gameassembly + offsets::local_player_slot_klass );
	if ( !klass )
		return 0;

	uintptr_t static_fields = drv->read<uintptr_t> ( klass + offsets::cl_ent ); // klass_static_fields = 0xB8
	if ( !static_fields )
		return 0;

	uintptr_t local_bp = drv->read<uintptr_t> ( static_fields + offsets::local_player_self_static_off );
	if ( !local_bp )
		return 0;

	// Sanity check the pointer is in usermode range before handing it back.
	if ( local_bp < 0x10000ULL || local_bp > 0x7FFFFFFFFFFFULL )
		return 0;

	return local_bp;
}

uintptr_t rust::get_entity_realm ( CSharedMemComms* drv )
{
	uintptr_t address = addr::client_entities + offsets::ent_realm;
	uintptr_t parent_static_fields_ptr = drv->read<uintptr_t> ( address );
	if ( !parent_static_fields_ptr )
		return NULL;

	return decrypt_entity_list ( drv, parent_static_fields_ptr );
}

uintptr_t rust::get_buffer_list ( CSharedMemComms* drv )
{
	uintptr_t address = addr::entity_realm + offsets::buf_list;
	uintptr_t buffer_list = drv->read<uintptr_t> ( address );
	if ( !buffer_list )
		return NULL;

	return buffer_list;
}

uintptr_t rust::get_object_list ( CSharedMemComms* drv )
{
	uintptr_t address = addr::buffer_list + offsets::obj_list;
	uintptr_t object_list = drv->read<uintptr_t> ( address );
	if ( !object_list )
		return NULL;

	return object_list;
}

unsigned int rust::get_object_size ( CSharedMemComms* drv )
{
	// List<T>._size is a signed int32 at +0x18 (per RAVENS dumper check_buffer_list).
	int32_t object_size = drv->read<int32_t> ( addr::buffer_list + offsets::obj_size );
	if ( object_size <= 0 || object_size > 50000 )
		return 0;
	return static_cast<unsigned int>( object_size );
}

uintptr_t rust::get_base_entity ( CSharedMemComms* drv, uintptr_t element )
{
	// NeoRed v6: the legacy `element+0x18 -> +0x28` chain returns garbage on
	// Unity 6 - the field at +0x18 has been repurposed and the +0x28 read
	// dereferences a wild pointer (causing aim/ESP target lookup failures).
	//
	// In modern object_loop.cpp the entity stored in `game::players` IS already
	// the managed BasePlayer instance (objectClass = element). So just return
	// it. Keeping the function as a passthrough means every caller (aimbot,
	// ESP_player, item-info panel) starts working without site-by-site edits.
	(void)drv;
	if ( !element )
		return 0;
	return element;
}

uintptr_t rust::get_model_state ( CSharedMemComms* drv, uintptr_t baseplayer ) {
	return drv->read<uintptr_t> ( baseplayer + offsets::modelState );
}

std::string rust::get_class_name ( CSharedMemComms* drv, uintptr_t element )
{
	uintptr_t object = drv->read<uintptr_t> ( element );
	if ( !object )
		return {};

	uintptr_t pName = drv->read<uintptr_t> ( object + offsets::class_name );
	if ( !pName )
		return {};

	std::string nice = drv->read_ascii ( pName, 32 );

	return nice;
}

uintptr_t rust::get_player_visual ( CSharedMemComms* drv, uintptr_t element )
{
	return drv->read <uintptr_t> ( element + offsets::player_visual );
}

uintptr_t rust::get_object_pos_component ( CSharedMemComms* drv, uintptr_t element )
{
	uintptr_t visual = rust::get_player_visual ( drv, element );
	if ( !visual )
		return NULL;

	return drv->read<uintptr_t> ( visual + offsets::obj_state ); // state
}

Vector3 rust::get_object_position ( CSharedMemComms* drv, uintptr_t element )
{
	uintptr_t state = rust::get_object_pos_component ( drv, element );
	if ( !state )
		return { -1, -1, -1 };

	return drv->read<Vector3> ( state + offsets::obj_pos );
}

// NeoRed v6: the legacy `player_visual + obj_state + obj_pos` chain is dead
// on Unity 6 (the +0x90 read returns 0). Replace it with a two-path resolver:
//   Path A (players/NPCs): pelvis-bone walk - matches what ESP_player already
//                          uses for the skeleton, gives accurate body position.
//   Path B (everything else): GameObject -> Components[0] (= Transform) ->
//                             TransformData + root_pos_off (0x90). All offsets
//                             come straight out of the rust_sdk dump.
Vector3 rust::get_entity_world_position ( CSharedMemComms* drv, uintptr_t entity )
{
	if ( entity < 0x10000ULL || entity > 0x7FFFFFFFFFFFULL )
		return {};

	// ---------- Path A: pelvis bone (rigged entities) ----------
	{
		uintptr_t player_model = drv->read<uintptr_t> ( entity + offsets::model );
		if ( player_model >= 0x10000ULL && player_model <= 0x7FFFFFFFFFFFULL )
		{
			uintptr_t transform_arr = drv->read<uintptr_t> ( player_model + offsets::bone_transforms );
			if ( transform_arr >= 0x10000ULL && transform_arr <= 0x7FFFFFFFFFFFULL )
			{
				uintptr_t pelvis_t = drv->read<uintptr_t> ( transform_arr + 0x20 ); // bone 0
				if ( pelvis_t >= 0x10000ULL && pelvis_t <= 0x7FFFFFFFFFFFULL )
				{
					uintptr_t pelvis_native = drv->read<uintptr_t> ( pelvis_t + 0x10 );
					if ( pelvis_native >= 0x10000ULL && pelvis_native <= 0x7FFFFFFFFFFFULL )
					{
						Vector3 v = safe_unity_world_position ( drv, pelvis_native );
						if ( !v.empty ( ) )
							return v;
					}
				}
			}
		}
	}

	// ---------- Path B: GameObject -> Transform -> TransformData ----------
	// rust_sdk constants:
	//   unity_object::cached_ptr               = 0x10
	//   unity_component::game_object           = 0x20
	//   unity_game_object::components          = 0x20  (ptr to native components)
	//   unity_game_object::component_ptr_in_entry = 0x8 (Transform = entry[0])
	//   unity_transform::indirect_ptr_off      = 0x28
	//   unity_transform::world_pos_off         = 0x90
	uintptr_t native_comp = drv->read<uintptr_t> ( entity + 0x10 );
	if ( native_comp < 0x10000ULL || native_comp > 0x7FFFFFFFFFFFULL )
		return {};

	uintptr_t go = drv->read<uintptr_t> ( native_comp + 0x20 );
	if ( go < 0x10000ULL || go > 0x7FFFFFFFFFFFULL )
		return {};

	uintptr_t comps_arr = drv->read<uintptr_t> ( go + 0x20 );
	if ( comps_arr < 0x10000ULL || comps_arr > 0x7FFFFFFFFFFFULL )
		return {};

	// First component is Transform; ptr lives at entry +0x8 with stride 0x10.
	uintptr_t native_transform = drv->read<uintptr_t> ( comps_arr + 0x8 );
	if ( native_transform < 0x10000ULL || native_transform > 0x7FFFFFFFFFFFULL )
		return {};

	uintptr_t transform_data = drv->read<uintptr_t> ( native_transform + 0x28 );
	if ( transform_data < 0x10000ULL || transform_data > 0x7FFFFFFFFFFFULL )
		return {};

	Vector3 pos = drv->read<Vector3> ( transform_data + 0x90 );

	// Reject obvious garbage (NaN, far beyond Rust's 4km map bounds).
	if ( pos.empty ( ) )
		return {};
	const float kMax = 8192.f;
	if ( !(pos.x > -kMax && pos.x < kMax) || !(pos.y > -kMax && pos.y < kMax) || !(pos.z > -kMax && pos.z < kMax) )
		return {};

	return pos;
}

// Safe wrapper around unity::get_position_injected. The raw walker mallocs
// based on a memory-read index and dereferences SSE matrix offsets without
// bounds checks - if any field is garbage the cheat crashes. We sanity-check
// every value before letting the walker run.
static Vector3 safe_unity_world_position ( CSharedMemComms* drv, uintptr_t transform_node )
{
	if ( transform_node < 0x10000ULL || transform_node > 0x7FFFFFFFFFFFULL )
		return {};

	// Read TransformAccessReadOnly = { pTransformData, index } at the
	// NeoRed v6 offset 0x28 (was 0x38 on older Unity; the new dump probed
	// 0x28 as the only correct value for this build).
	struct TARO { uintptr_t pData; int index; };
	TARO taro { 0, 0 };
	if ( !drv->read ( transform_node + 0x28, &taro, sizeof ( taro ) ) )
		return {};

	if ( taro.pData < 0x10000ULL || taro.pData > 0x7FFFFFFFFFFFULL )
		return {};
	if ( taro.index < 0 || taro.index > 100000 )      // sanity bound
		return {};

	struct TData { uintptr_t pArray; uintptr_t pIndices; };
	TData td { 0, 0 };
	if ( !drv->read ( taro.pData + 0x18, &td, sizeof ( td ) ) )
		return {};
	if ( td.pArray < 0x10000ULL || td.pArray > 0x7FFFFFFFFFFFULL ) return {};
	if ( td.pIndices < 0x10000ULL || td.pIndices > 0x7FFFFFFFFFFFULL ) return {};

	// Now safe to call the SSE walker.
	return unity::get_position_injected ( drv, transform_node );
}

Vector3 rust::get_bone_position ( CSharedMemComms* drv, uintptr_t Entity, unsigned int bone )
{
	uintptr_t player_model = drv->read<uintptr_t> ( Entity + offsets::model );
	if ( !player_model ) return {};
	uintptr_t transform = drv->read<uintptr_t> ( player_model + offsets::bone_transforms );
	if ( !transform ) return {};
	uintptr_t ogbones = drv->read<uintptr_t> ( transform + ( 0x20 + ( ( bone ) * 0x8 ) ) );
	if ( !ogbones ) return {};
	uintptr_t bones = drv->read<uintptr_t> ( ogbones + 0x10 );
	if ( !bones ) return {};
	return safe_unity_world_position ( drv, bones );
}

Vector3 rust::get_bone_position_from_transform ( CSharedMemComms* drv, uintptr_t transform, uintptr_t Entity, unsigned int bone )
{
	if ( !transform ) return {};
	uintptr_t ogbones = drv->read<uintptr_t> ( transform + ( 0x20 + ( ( bone ) * 0x8 ) ) );
	if ( !ogbones ) return {};
	uintptr_t bones = drv->read<uintptr_t> ( ogbones + 0x10 );
	if ( !bones ) return {};
	return safe_unity_world_position ( drv, bones );
}

// --- Camera resolution -----------------------------------------------------
//
// Direct chain from the user-supplied offsets (no scanning, no brute-force):
//
//   GameAssembly + offsets::main_camera_c   -> MainCamera klass*
//   klass        + offsets::camera_static   -> static_fields struct
//   static_fields + offsets::camera_object  -> managed Camera ref
//   managed      + offsets::camera_entity   -> C++ Camera component (m_CachedPtr)
//   C++ Camera   + kViewMatrixOffset (0x2FC)-> view-projection matrix (16 floats)
//
// If any of these break with a future patch, update the offsets in include.hpp
// and the kViewMatrixOffset constant below.

namespace {
	constexpr uint32_t kViewMatrixOffset = 0x2FC;
	constexpr uint32_t kCamWorldPosFromVM = 0x148;  // empirical Unity gap

	bool ptr_looks_userland ( uintptr_t p )
	{
		return p > 0x10000ULL && p < 0x7FFFFFFFFFFFULL;
	}

}

uintptr_t rust::resolve_main_camera ( CSharedMemComms* drv )
{
	if ( !addr::gameassembly )
		return 0;

	uintptr_t klass = drv->read<uintptr_t> ( addr::gameassembly + offsets::main_camera_c );

	// One-shot diagnostic: trace the full camera chain to identify which step fails.
	static bool cam_diag_done = false;
	if ( !cam_diag_done )
	{
		uintptr_t dbg_sf      = ptr_looks_userland ( klass ) ? drv->read<uintptr_t> ( klass + offsets::camera_static ) : 0;
		uintptr_t dbg_managed = ptr_looks_userland ( dbg_sf ) ? drv->read<uintptr_t> ( dbg_sf + offsets::camera_object ) : 0;
		uintptr_t dbg_cpp     = ptr_looks_userland ( dbg_managed ) ? drv->read<uintptr_t> ( dbg_managed + offsets::camera_entity ) : 0;

		logging::print ( TYPE_DEBUG,
			"[cam] GA+0x%llX -> klass=0x%llX  klass+0x%llX(sf)=0x%llX  sf+0x%llX(inst)=0x%llX  inst+0x%llX(cpp)=0x%llX",
			(unsigned long long)offsets::main_camera_c, (unsigned long long)klass,
			(unsigned long long)offsets::camera_static, (unsigned long long)dbg_sf,
			(unsigned long long)offsets::camera_object, (unsigned long long)dbg_managed,
			(unsigned long long)offsets::camera_entity, (unsigned long long)dbg_cpp );

		// If managed is 0 at the expected offset, scan nearby offsets in static_fields
		// to find where the camera instance actually lives.
		if ( ptr_looks_userland ( dbg_sf ) && !ptr_looks_userland ( dbg_managed ) )
		{
			logging::print ( TYPE_WARNING, "[cam] Camera instance NOT found at sf+0x%llX. Scanning sf+0x00..0xF8:",
				(unsigned long long)offsets::camera_object );
			for ( uint64_t off = 0; off <= 0xF8; off += 8 )
			{
				uintptr_t probe = drv->read<uintptr_t> ( dbg_sf + off );
				if ( ptr_looks_userland ( probe ) )
				{
					// Try reading m_CachedPtr from it
					uintptr_t probe_cpp = drv->read<uintptr_t> ( probe + offsets::camera_entity );
					logging::print ( TYPE_DEBUG, "[cam]   sf+0x%02llX = 0x%llX  ->+0x10 = 0x%llX %s",
						(unsigned long long)off, (unsigned long long)probe, (unsigned long long)probe_cpp,
						ptr_looks_userland ( probe_cpp ) ? "<-- CANDIDATE" : "" );
				}
			}
		}

		// Only mark done if we got a valid klass - otherwise re-fire next call
		// so the diagnostic produces useful output once GA is populated.
		if ( ptr_looks_userland ( klass ) )
			cam_diag_done = true;
	}

	if ( !ptr_looks_userland ( klass ) )
		return 0;

	uintptr_t sf = drv->read<uintptr_t> ( klass + offsets::camera_static );
	if ( !ptr_looks_userland ( sf ) )
		return 0;

	uintptr_t managed = drv->read<uintptr_t> ( sf + offsets::camera_object );
	if ( !ptr_looks_userland ( managed ) )
		return 0;

	uintptr_t cpp = drv->read<uintptr_t> ( managed + offsets::camera_entity );
	if ( !ptr_looks_userland ( cpp ) )
		return 0;

	addr::camera_instance = cpp;
	return cpp;
}

Matrix4x4 rust::get_view_matrix ( CSharedMemComms* drv )
{
	uintptr_t cam = rust::resolve_main_camera ( drv );
	if ( !cam )
		return {};
	return drv->read<Matrix4x4> ( cam + kViewMatrixOffset );
}

Vector3 rust::get_camera_position ( CSharedMemComms* drv )
{
	uintptr_t cam = rust::resolve_main_camera ( drv );
	if ( !cam )
		return {};
	return drv->read<Vector3> ( cam + kViewMatrixOffset + kCamWorldPosFromVM );
}

bool rust::world_to_screen ( const Vector3& element_position, Vector2& screen_position, Matrix4x4 view_matrix, bool ignore_incorrect )
{
	if ( element_position.empty ( ) )
		return false;

	Vector3 trans_vec { view_matrix[0][3], view_matrix[1][3], view_matrix[2][3] };
	Vector3 right_vec { view_matrix[0][0], view_matrix[1][0], view_matrix[2][0] };
	Vector3 up_vec { view_matrix[0][1], view_matrix[1][1], view_matrix[2][1] };

	const float w = trans_vec.dot_product ( element_position ) + view_matrix[3][3];
	bool bad = false;

	if ( w < 1.f )
	{
		if ( !ignore_incorrect )
			return false;
		else
			bad = true;
	}

	float x = right_vec.dot_product ( element_position ) + view_matrix[3][0];
	float y = up_vec.dot_product ( element_position ) + view_matrix[3][1];

	Vector2 screen_pos = { ( monitor::width / 2 ) * ( 1.f + x / w ), ( monitor::height / 2 ) * ( 1.f - y / w ) };
	screen_position = screen_pos;

	if ( screen_pos.x >= monitor::width || screen_pos.y >= monitor::height || screen_pos.x <= 0 || screen_pos.y <= 0 || bad )
		return false;

	return true;
}

inline float bullet_gravity ( const char* bullet_name )
{
	if ( _stricmp ( bullet_name, _ ( "ammo.rifle" ) ) == NULL ) {
		return 1.f;
	}
	if ( _stricmp ( bullet_name, _ ( "ammo.rifle.hv" ) ) == NULL ) {
		return 1.f;
	}
	if ( _stricmp ( bullet_name, _ ( "ammo.rifle.explosive" ) ) == NULL ) {
		return 1.25f;
	}
	if ( _stricmp ( bullet_name, _ ( "ammo.rifle.incendiary" ) ) == NULL ) {
		return 1.f;
	}

	if ( _stricmp ( bullet_name, _ ( "ammo.pistol" ) ) == NULL ) {
		return 1.f;
	}
	if ( _stricmp ( bullet_name, _ ( "ammo.pistol.hv" ) ) == NULL ) {
		return 1.f;
	}
	if ( _stricmp ( bullet_name, _ ( "ammo.pistol.incendiary" ) ) == NULL ) {
		return 1.f;
	}

	if ( _stricmp ( bullet_name, _ ( "arrow.hv" ) ) == NULL ) {
		return 0.5f;
	}
	if ( _stricmp ( bullet_name, _ ( "arrow.bone" ) ) == NULL ) {
		return 0.75f;
	}
	if ( _stricmp ( bullet_name, _ ( "arrow.wooden" ) ) == NULL ) {
		return 0.75f;
	}
	if ( _stricmp ( bullet_name, _ ( "arrow.fire" ) ) == NULL ) {
		return 1.f;
	}

	if ( _stricmp ( bullet_name, _ ( "ammo.handmade.shell" ) ) == NULL ) {
		return 1.f;
	}
	if ( _stricmp ( bullet_name, _ ( "ammo.shotgun" ) ) == NULL ) {
		return 1.f;
	}
	if ( _stricmp ( bullet_name, _ ( "ammo.shotgun.fire" ) ) == NULL ) {
		return 1.f;
	}
	if ( _stricmp ( bullet_name, _ ( "ammo.shotgun.slug" ) ) == NULL ) {
		return 1.f;
	}

	if ( _stricmp ( bullet_name, _ ( "ammo.nailgun.nails" ) ) == NULL ) {
		return 0.75f;
	}

	return 1.f;
}

inline float get_bullet_dragg ( const char* bullet_name )
{
	if ( _stricmp ( bullet_name, _ ( "ammo.rifle" ) ) == NULL ) {
		return 0.6f;
	}
	if ( _stricmp ( bullet_name, _ ( "ammo.rifle.hv" ) ) == NULL ) {
		return 1.f;
	}
	if ( _stricmp ( bullet_name, _ ( "ammo.rifle.explosive" ) ) == NULL ) {
		return 0.6f;
	}
	if ( _stricmp ( bullet_name, _ ( "ammo.rifle.incendiary" ) ) == NULL ) {
		return 0.6f;
	}

	if ( _stricmp ( bullet_name, _ ( "ammo.pistol" ) ) == NULL ) {
		return 0.7f;
	}
	if ( _stricmp ( bullet_name, _ ( "ammo.pistol.hv" ) ) == NULL ) {
		return 1.f;
	}
	if ( _stricmp ( bullet_name, _ ( "ammo.pistol.incendiary" ) ) == NULL ) {
		return 0.7f;
	}

	if ( _stricmp ( bullet_name, _ ( "arrow.hv" ) ) == NULL ) {
		return 0.005f;
	}
	if ( _stricmp ( bullet_name, _ ( "arrow.bone" ) ) == NULL ) {
		return 0.01f;
	}
	if ( _stricmp ( bullet_name, _ ( "arrow.wooden" ) ) == NULL ) {
		return 0.005f;
	}
	if ( _stricmp ( bullet_name, _ ( "arrow.fire" ) ) == NULL ) {
		return 0.01f;
	}

	if ( _stricmp ( bullet_name, _ ( "ammo.handmade.shell" ) ) == NULL ) {
		return 1.f;
	}
	if ( _stricmp ( bullet_name, _ ( "ammo.shotgun" ) ) == NULL ) {
		return 1.f;
	}
	if ( _stricmp ( bullet_name, _ ( "ammo.shotgun.fire" ) ) == NULL ) {
		return 1.f;
	}
	if ( _stricmp ( bullet_name, _ ( "ammo.shotgun.slug" ) ) == NULL ) {
		return 0.6f;
	}

	if ( _stricmp ( bullet_name, _ ( "ammo.nailgun.nails" ) ) == NULL ) {
		return 0.005f;
	}

	return 1.f;
}

inline float get_bullet_velocity ( const char* weapon_name, const char* bullet_name )
{
	/* rifle ammo */

	if ( _stricmp ( weapon_name, _ ( "rifle.ak" ) ) == NULL
		|| _stricmp ( weapon_name, _ ( "rifle.lr300" ) ) == NULL ) {

		if ( _stricmp ( bullet_name, _ ( "ammo.rifle" ) ) == NULL ) {
			return 375.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle.hv" ) ) == NULL ) {
			return 450.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle.explosive" ) ) == NULL ) {
			return 225.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle.incendiary" ) ) == NULL ) {
			return 225.f;
		}
	}

	if ( _stricmp ( weapon_name, _ ( "rifle.bolt" ) ) == NULL ) {
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle" ) ) == NULL ) {
			return 656.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle.hv" ) ) == NULL ) {
			return 788.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle.explosive" ) ) == NULL ) {
			return 394.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle.incendiary" ) ) == NULL ) {
			return 394.f;
		}
	}

	if ( _stricmp ( weapon_name, _ ( "rifle.l96" ) ) == NULL ) {
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle" ) ) == NULL ) {
			return 1125.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle.hv" ) ) == NULL ) {
			return 1350.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle.explosive" ) ) == NULL ) {
			return 675.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle.incendiary" ) ) == NULL ) {
			return 675.f;
		}
	}

	if ( _stricmp ( weapon_name, _ ( "rifle.m39" ) ) == NULL ) {
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle" ) ) == NULL ) {
			return 469.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle.hv" ) ) == NULL ) {
			return 563.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle.explosive" ) ) == NULL ) {
			return 281.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle.incendiary" ) ) == NULL ) {
			return 281.f;
		}
	}

	if ( _stricmp ( weapon_name, _ ( "rifle.semiauto" ) ) == NULL ) {
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle" ) ) == NULL ) {
			return 375.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle.hv" ) ) == NULL ) {
			return 450.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle.explosive" ) ) == NULL ) {
			return 225.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle.incendiary" ) ) == NULL ) {
			return 225.f;
		}
	}

	if ( _stricmp ( weapon_name, _ ( "lmg.m249" ) ) == NULL ) {
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle" ) ) == NULL ) {
			return 487.5f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle.hv" ) ) == NULL ) {
			return 585.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle.explosive" ) ) == NULL ) {
			return 293.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.rifle.incendiary" ) ) == NULL ) {
			return 293.f;
		}
	}

	/* pistol ammo */

	if ( _stricmp ( weapon_name, _ ( "smg.2" ) ) == NULL
		|| _stricmp ( weapon_name, _ ( "smg.mp5" ) ) == NULL ) {

		if ( _stricmp ( bullet_name, _ ( "ammo.pistol" ) ) == NULL ) {
			return 240.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.pistol.hv" ) ) == NULL ) {
			return 320.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.pistol.incendiary" ) ) == NULL ) {
			return 180.f;
		}
	}

	if ( _stricmp ( weapon_name, _ ( "pistol.m92" ) ) == NULL
		|| _stricmp ( weapon_name, _ ( "pistol.python" ) ) == NULL
		|| _stricmp ( weapon_name, _ ( "pistol.revolver" ) ) == NULL
		|| _stricmp ( weapon_name, _ ( "pistol.semiauto" ) ) == NULL
		|| _stricmp ( weapon_name, _ ( "smg.thompson" ) ) == NULL ) {

		if ( _stricmp ( bullet_name, _ ( "ammo.pistol" ) ) == NULL ) {
			return 300.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.pistol.hv" ) ) == NULL ) {
			return 400.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.pistol.incendiary" ) ) == NULL ) {
			return 225.f;
		}
	}

	/* shotgun ammo */

	if ( _stricmp ( weapon_name, _ ( "shotgun.double" ) ) == NULL
		|| _stricmp ( weapon_name, _ ( "shotgun.pump" ) ) == NULL
		|| _stricmp ( weapon_name, _ ( "shotgun.waterpipe" ) ) == NULL
		|| _stricmp ( weapon_name, _ ( "shotgun.spas12" ) ) == NULL
		|| _stricmp ( weapon_name, _ ( "pistol.eoka" ) ) == NULL ) {

		if ( _stricmp ( bullet_name, _ ( "ammo.shotgun.slug" ) ) == NULL ) {
			return 225.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.shotgun.fire" ) ) == NULL ) {
			return 100.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.handmade.shell" ) ) == NULL ) {
			return 100.f;
		}
		if ( _stricmp ( bullet_name, _ ( "ammo.shotgun.slug" ) ) == NULL ) {
			return 225.f;
		}
	}

	/* monkey arrows and monkey guns */

	if ( _stricmp ( weapon_name, _ ( "bow.hunting" ) ) == NULL ) {
		if ( _stricmp ( bullet_name, _ ( "arrow.hv" ) ) == NULL ) {
			return 80.f;
		}
		if ( _stricmp ( bullet_name, _ ( "arrow.bone" ) ) == NULL ) {
			return 45.f;
		}
		if ( _stricmp ( bullet_name, _ ( "arrow.wooden" ) ) == NULL ) {
			return 50.f;
		}
		if ( _stricmp ( bullet_name, _ ( "arrow.fire" ) ) == NULL ) {
			return 40.f;
		}
	}

	if ( _stricmp ( weapon_name, _ ( "bow.compound" ) ) == NULL ) {
		if ( _stricmp ( bullet_name, _ ( "arrow.hv" ) ) == NULL ) {
			return 160.f;
		}
		if ( _stricmp ( bullet_name, _ ( "arrow.bone" ) ) == NULL ) {
			return 90.f;
		}
		if ( _stricmp ( bullet_name, _ ( "arrow.wooden" ) ) == NULL ) {
			return 100.f;
		}
		if ( _stricmp ( bullet_name, _ ( "arrow.fire" ) ) == NULL ) {
			return 80.f;
		}
	}

	if ( _stricmp ( weapon_name, _ ( "crossbow" ) ) == NULL ) {
		if ( _stricmp ( bullet_name, _ ( "arrow.hv" ) ) == NULL ) {
			return 120.f;
		}
		if ( _stricmp ( bullet_name, _ ( "arrow.bone" ) ) == NULL ) {
			return 68.f;
		}
		if ( _stricmp ( bullet_name, _ ( "arrow.wooden" ) ) == NULL ) {
			return 75.f;
		}
		if ( _stricmp ( bullet_name, _ ( "arrow.fire" ) ) == NULL ) {
			return 60.f;
		}
	}

	if ( _stricmp ( weapon_name, _ ( "pistol.nailgun" ) ) == NULL ) {
		return 50.f;
	}

	return 1337.f;
}

inline float is_prediction_available ( const char* weapon_name )
{

	if ( _stricmp ( weapon_name, _ ( "rifle.ak" ) ) == NULL
		|| _stricmp ( weapon_name, _ ( "rifle.lr300" ) ) == NULL ) {
		return true;
	}

	if ( _stricmp ( weapon_name, _ ( "rifle.bolt" ) ) == NULL ) {

		return true;
	}

	if ( _stricmp ( weapon_name, _ ( "rifle.l96" ) ) == NULL ) {

		return true;
	}

	if ( _stricmp ( weapon_name, _ ( "rifle.m39" ) ) == NULL ) {

		return true;
	}

	if ( _stricmp ( weapon_name, _ ( "rifle.semiauto" ) ) == NULL ) {

		return true;
	}

	if ( _stricmp ( weapon_name, _ ( "lmg.m249" ) ) == NULL ) {

		return true;
	}

	if ( _stricmp ( weapon_name, _ ( "smg.2" ) ) == NULL
		|| _stricmp ( weapon_name, _ ( "smg.mp5" ) ) == NULL ) {

		return true;
	}

	if ( _stricmp ( weapon_name, _ ( "pistol.m92" ) ) == NULL
		|| _stricmp ( weapon_name, _ ( "pistol.python" ) ) == NULL
		|| _stricmp ( weapon_name, _ ( "pistol.revolver" ) ) == NULL
		|| _stricmp ( weapon_name, _ ( "pistol.semiauto" ) ) == NULL
		|| _stricmp ( weapon_name, _ ( "smg.thompson" ) ) == NULL ) {

		return true;
	}

	if ( _stricmp ( weapon_name, _ ( "shotgun.double" ) ) == NULL
		|| _stricmp ( weapon_name, _ ( "shotgun.pump" ) ) == NULL
		|| _stricmp ( weapon_name, _ ( "shotgun.waterpipe" ) ) == NULL
		|| _stricmp ( weapon_name, _ ( "shotgun.spas12" ) ) == NULL
		|| _stricmp ( weapon_name, _ ( "pistol.eoka" ) ) == NULL ) {

		return true;
	}

	if ( _stricmp ( weapon_name, _ ( "bow.hunting" ) ) == NULL ) {

		return true;
	}

	if ( _stricmp ( weapon_name, _ ( "bow.compound" ) ) == NULL ) {

		return true;
	}

	if ( _stricmp ( weapon_name, _ ( "crossbow" ) ) == NULL ) {

		return true;
	}

	if ( _stricmp ( weapon_name, _ ( "pistol.nailgun" ) ) == NULL ) {
		return true;
	}

	return false;
}

float rust::get_bullet_speed ( )
{
	return get_bullet_velocity ( game::m_rLocalInfo.m_szWeaponName.c_str ( ), game::m_rLocalInfo.m_szBulletType.c_str ( ) );
}

float rust::get_bullet_gravity ( )
{
	return bullet_gravity ( game::m_rLocalInfo.m_szBulletType.c_str ( ) );
}

float rust::get_bullet_drag ( )
{
	return get_bullet_dragg ( game::m_rLocalInfo.m_szBulletType.c_str ( ) );
}

bool rust::use_weapon_prediction ( )
{
	return is_prediction_available ( game::m_rLocalInfo.m_szWeaponName.c_str ( ) );
}

uintptr_t rust::mono_field_static_get_value ( CSharedMemComms* drv, uintptr_t klass, uintptr_t offset )
{
	uintptr_t vTable = drv->read<uintptr_t> ( klass + 0xD0 );
	if ( vTable )
	{
		vTable = drv->read<uintptr_t> ( vTable + 0x8 );
		if ( vTable )
		{
			uintptr_t vTableSize = 0x40 + drv->read<uintptr_t> ( klass + 0x5C ) * 0x8;
			return drv->read<uintptr_t> ( vTable + vTableSize + offset );
		}
	}
	return 0;
}

float normalize_angle ( float angle )
{
	while ( angle > 360 )
		angle -= 360;
	while ( angle < 0 )
		angle += 360;
	return angle;
}

Vector3 normalize_angles ( Vector3 angles )
{
	angles.x = normalize_angle ( angles.x );
	angles.y = normalize_angle ( angles.y );
	angles.z = normalize_angle ( angles.z );
	return angles;
}

Vector3 rust::euler_angles ( Vector4 q1 )
{
	float sqw = q1.w * q1.w;
	float sqx = q1.x * q1.x;
	float sqy = q1.y * q1.y;
	float sqz = q1.z * q1.z;
	float unit = sqx + sqy + sqz + sqw; // if normalised is one, otherwise is correction factor
	float test = q1.x * q1.w - q1.y * q1.z;
	Vector3 v;
	if ( test > 0.4995f * unit ) { // singularity at north pole
		v.y = 2.0f * atan2 ( q1.y, q1.x );
		v.x = M_PI / 2.f;
		v.z = 0;
		return normalize_angles ( v * 57.2958f );
	}
	if ( test < -0.4995f * unit ) { // singularity at south pole
		v.y = -2.0f * atan2 ( q1.y, q1.x );
		v.x = -M_PI / 2.f;
		v.z = 0;
		return normalize_angles ( v * 57.2958f );
	}
	Vector4 q ( q1.w, q1.z, q1.x, q1.y );
	v.y = (float)atan2 ( 2.0f * q.x * q.w + 2.0f * q.y * q.z, 1 - 2.0f * ( q.z * q.z + q.w * q.w ) );     // Yaw
	v.x = (float)asin ( 2.0f * ( q.x * q.z - q.w * q.y ) );                             // Pitch
	v.z = (float)atan2 ( 2.0f * q.x * q.y + 2.0f * q.z * q.w, 1 - 2.0f * ( q.y * q.y + q.z * q.z ) );      // Roll
	return normalize_angles ( v * 57.2958f );
}
//stackoverflow
Vector3 rust::rotate_point ( Vector3 center, Vector3 origin, float angle )
{
	float rad = ( ( angle ) * static_cast<float>( ( M_PI / 180.0f ) ) );
	float s = -sin ( rad );
	float c = cos ( rad );
	origin.x -= center.x;
	origin.z -= center.z;
	float xnew = origin.x * c - origin.z * s;
	float znew = origin.x * s + origin.z * c;
	xnew += center.x;
	znew += center.z;
	return Vector3 ( xnew, origin.y, znew );
}

Vector2 rust::RotatePoint ( Vector2 pointToRotate, Vector2 centerPoint, float angle, bool angleInRadians = false )
{
	float rad = ( ( normalize_angle ( angle ) ) * static_cast<float>( ( M_PI / 180.0f ) ) );
	float s = -sin ( rad );
	float c = cos ( rad );
	pointToRotate.x -= centerPoint.x;
	pointToRotate.y -= centerPoint.y;
	float xnew = pointToRotate.x * c - pointToRotate.y * s;
	float znew = pointToRotate.x * s + pointToRotate.y * c;
	pointToRotate.x = xnew + centerPoint.x;
	pointToRotate.y = znew + centerPoint.y;
	return pointToRotate;
}