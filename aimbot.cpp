#include "aimbot.hpp"
#include "cheat.hpp"
#include "unity.hpp"
#include "fnv1a.hpp"
#include <random>
#include <chrono>

// ── RNG ──
static std::mt19937 g_rng(std::random_device{}());
static std::uniform_real_distribution<float> g_jitter_close(-0.005f, 0.005f);
static std::uniform_real_distribution<float> g_jitter_far(-0.015f, 0.015f);
static std::uniform_real_distribution<float> g_overshoot(0.995f, 1.005f);

// ── Prediction state ──
static uintptr_t g_pred_target = 0;
static Vector3 g_pred_vel = {};
static Vector3 g_pred_prev = {};
static ULONGLONG g_pred_tick = 0;
static bool g_pred_has_prev = false;

// ── Smoothing state ──
static Vector3 g_smooth_pos = {};
static uintptr_t g_smooth_inst = 0;

namespace data::aimbot {
	uintptr_t m_pTarget;
	bool m_bFound;
	bool m_bActive;
	entity_type m_eType;
	int m_iHitbox;
	Vector3 m_vecPosition;
	float m_flFOV;
	std::vector<Vector3> m_vecBulletSimulation;
	time_t m_tShot;
	bool m_bLocked;
};

float aim_fov;
Vector2 center{};

// ── Helpers ──
static void norm2(Vector2& a) {
	while (a.x > 180.f) a.x -= 360.f;
	while (a.x < -180.f) a.x += 360.f;
	while (a.y > 180.f) a.y -= 360.f;
	while (a.y < -180.f) a.y -= 360.f;
	if (a.x < -89.f) a.x = -89.f;
	if (a.x > 89.f) a.x = 89.f;
}

static Vector2 calc_angle(const Vector3& src, const Vector3& dst) {
	Vector3 dir = src - dst;
	const auto sqrtss = [](float in) {
		__m128 reg = _mm_load_ss(&in);
		return _mm_mul_ss(reg, _mm_rsqrt_ss(reg)).m128_f32[0];
	};
	float hyp = sqrtss(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
	if (isnan(hyp)) hyp = sqrtss(dir.x*dir.x + roundf(dir.y)*roundf(dir.y));
	float pitch = (float)(asin(dir.y / hyp) * (180.0 / M_PI));
	float yaw = (float)(-atan2(dir.x, -dir.z) * (180.0 / M_PI));
	return { pitch, yaw };
}

static Vector2 angle_delta(Vector2 src, Vector2 dst) {
	Vector2 d = { dst.x - src.x, dst.y - src.y };
	while (d.x > 180.f) d.x -= 360.f;
	while (d.x < -180.f) d.x += 360.f;
	while (d.y > 180.f) d.y -= 360.f;
	while (d.y < -180.f) d.y += 360.f;
	return d;
}

static bool aim_key_active() {
	bool held = (GetAsyncKeyState(var::aim::aim_key) & 0x8000) != 0;
	if (var::aim::aim_key_mode == 0) return held;
	if (var::aim::aim_key_mode == 2) return true;
	static bool tog = false, was = false;
	if (held && !was) tog = !tog;
	was = held;
	return tog;
}

static bool is_npc_type(entity_type t) {
	return t == entity_type::Scientist;
}

// ── Target selection (screen-space FOV) ──
static bool find_best_target() {
	float best = FLT_MAX;
	uintptr_t best_ent = 0;
	int best_bone = BoneList::head;

	Matrix4x4 vm = rust::get_view_matrix(mist::aim.get());
	Vector3 local_pos = rust::get_camera_position(mist::aim.get());
	if (local_pos.empty()) return false;

	float cx = monitor::width / 2.f;
	float cy = monitor::height / 2.f;

	// FOV in pixels (percentage of screen half-width)
	float fov_pixels = (float)var::aim::aim_fov / 180.f * cx;
	float npc_fov_pixels = (float)(var::aim::npc_fov > 0 ? var::aim::npc_fov : var::aim::aim_fov) / 180.f * cx;

	int bone_sel = var::aim::hitbox;
	static const int bone_map[] = { BoneList::head, BoneList::neck, BoneList::spine2 };
	static const int closest_bones[] = { BoneList::head, BoneList::neck, BoneList::spine2, BoneList::pelvis,
		BoneList::l_forearm, BoneList::l_knee, BoneList::r_forearm, BoneList::r_knee };

	float player_dist = var::aim::max_distance > 0 ? (float)var::aim::max_distance : 350.f;
	float npc_dist = var::aim::npc_max_distance > 0 ? (float)var::aim::npc_max_distance : player_dist;

	for (const auto& entry : game::players) {
		auto& entity = entry.first;
		bool is_npc = is_npc_type(entry.second);

		if (is_npc && !var::aim::target_npcs) continue;
		if (!is_npc && var::aim::npc_only) continue;

		BasePlayer* player = (BasePlayer*)rust::get_base_entity(mist::aim.get(), entity);
		if (!player) continue;
		if (player->is_sleeping(mist::aim.get())) continue;
		if (var::aim::ignore_friends && game::cache[player->get_uid(mist::aim.get())].m_bFriend) continue;

		// Sticky target
		if (data::aimbot::m_bLocked && data::aimbot::m_pTarget == entity) {
			best_ent = entity;
			break;
		}
		if (data::aimbot::m_bLocked) continue;

		float fov_lim = is_npc ? npc_fov_pixels : fov_pixels;
		float dist_lim = is_npc ? npc_dist : player_dist;
		int eff_bone = is_npc ? var::aim::npc_bone : bone_sel;

		auto check_bone = [&](int bi) {
			Vector3 bp = player->get_bone_position(mist::aim.get(), bi);
			if (bp.empty()) return;
			float wd = local_pos.distance(bp);
			if (wd > dist_lim || wd < 0.5f) return; // skip self / too close
			// Skip FOV check for now — pure distance scoring
			float score = wd / dist_lim;
			if (bi == BoneList::head) score *= 0.85f;
			if (var::aim::npc_priority && is_npc) score *= 0.5f;
			if (score < best) { best = score; best_ent = entity; best_bone = bi; }
		};

		if (eff_bone == 3) { // closest bone
			for (int bi : closest_bones) check_bone(bi);
		} else {
			int bone = (eff_bone < 3) ? bone_map[eff_bone] : BoneList::head;
			check_bone(bone);
		}
	}

	if (!best_ent) {
		data::aimbot::m_pTarget = 0;
		data::aimbot::m_bLocked = false;
		data::aimbot::m_bFound = false;
		return false;
	}
	data::aimbot::m_pTarget = best_ent;
	data::aimbot::m_iHitbox = best_bone;
	data::aimbot::m_bFound = true;
	data::aimbot::m_bLocked = true;
	return true;
}

// ── Prediction ──
static float get_bullet_drop(float dist, float speed, float grav) {
	if (dist < 0.001f) return -1;
	float t = dist / std::fabs(speed);
	return 0.5f * 9.81f * grav * t * t;
}

static void do_prediction(const Vector3& from, BasePlayer* who, unsigned int bone) {
	Vector3 aimpoint = who->get_bone_position(mist::aim.get(), bone);
	float distance = from.distance(aimpoint);
	if (distance < 0.1f || !game::m_rLocalInfo.m_vInfo.m_bPrediction) {
		data::aimbot::m_vecPosition = {};
		return;
	}

	auto wpn = game::m_rLocalInfo.m_pWeapon;
	float speed = game::m_rLocalInfo.m_vInfo.m_flSpeed;
	float gravity = game::m_rLocalInfo.m_vInfo.m_flGravity;
	if (speed < 10.f) speed = 375.f;

	uintptr_t bp = wpn ? (uintptr_t)wpn->get_base_projectile(mist::aim.get()) : 0;
	float velScale = bp ? mist::aim->read<float>(bp + 0x26C) : 1.f;
	float bulletSpeed = speed * velScale;

	// Iterative bullet simulation
	const float dt = 0.015625f;
	float yTravel = 0, ySpeed = 0, bTime = 0;
	float simSpeed = bulletSpeed;
	float drag = rust::get_bullet_drag();

	for (float traveled = 0; traveled < distance;) {
		float mod = 1.f - dt * drag;
		simSpeed *= mod;
		if (simSpeed <= 0 || simSpeed >= 10000 || bTime > 8.f) break;
		ySpeed += 9.81f * gravity * dt;
		ySpeed *= mod;
		traveled += simSpeed * dt;
		yTravel += ySpeed * dt;
		bTime += dt;
	}

	Vector3 vel = who->get_velocity(mist::aim.get()) * 0.75f;
	if (vel.y > 0) vel.y /= 3.25f;

	aimpoint.y += yTravel;
	aimpoint += vel * bTime;

	data::aimbot::m_vecPosition = aimpoint;
	data::aimbot::m_bLocked = true;
}

// ── Process aim (smooth / silent) ──
static bool process_aim() {
	BasePlayer* target = (BasePlayer*)rust::get_base_entity(mist::aim.get(), data::aimbot::m_pTarget);
	BasePlayer* local = game::m_pLocalplayer;
	if (!target || !local) { printf("[pa] FAIL: target=%p local=%p\n",(void*)target,(void*)local); return false; }

	Vector3 local_pos = rust::get_camera_position(mist::aim.get());
	if (local_pos.empty()) { printf("[pa] FAIL: local_pos empty\n"); return false; }

	// Skip prediction for now — just use raw bone position
	data::aimbot::m_vecPosition = target->get_bone_position(mist::aim.get(), data::aimbot::m_iHitbox);

	if (data::aimbot::m_vecPosition.empty()) {
		printf("[pa] FAIL: bone position empty (hitbox=%d)\n", data::aimbot::m_iHitbox);
		data::aimbot::m_bLocked = false;
		return false;
	}

	// Head offset
	float dist = local_pos.distance(data::aimbot::m_vecPosition);
	if (data::aimbot::m_iHitbox == BoneList::head) {
		float off = 0.12f + std::clamp(dist / 300.f, 0.f, 1.f) * 0.06f;
		data::aimbot::m_vecPosition.y += off;
	}

	// Bone smoothing
	uintptr_t ci = data::aimbot::m_pTarget;
	if (ci != g_smooth_inst || g_smooth_pos.empty()) {
		g_smooth_pos = data::aimbot::m_vecPosition;
		g_smooth_inst = ci;
	} else {
		float lp = 0.75f - std::clamp(dist / 200.f, 0.f, 1.f) * 0.40f;
		g_smooth_pos.x += (data::aimbot::m_vecPosition.x - g_smooth_pos.x) * lp;
		g_smooth_pos.y += (data::aimbot::m_vecPosition.y - g_smooth_pos.y) * lp;
		g_smooth_pos.z += (data::aimbot::m_vecPosition.z - g_smooth_pos.z) * lp;
	}
	data::aimbot::m_vecPosition = g_smooth_pos;

	// body_angles offset is broken on this build — skip the check
	// Always use silent aim via PlayerEyes.bodyRotation (quaternion)
	auto eyes = local->get_player_eyes(mist::aim.get());
	if (eyes) {
		auto dir = data::aimbot::m_vecPosition - local_pos;
		dir = dir.normalize();
		Vector4 quat = Vector4::QuaternionLookRotation(dir, {0,1,0});
		eyes->set_body_rotation(mist::aim.get(), quat);

		static ULONGLONG pa_last = 0;
		ULONGLONG pa_now = GetTickCount64();
		if (pa_now - pa_last > 3000) {
			pa_last = pa_now;
			printf("[aim-diag] WROTE silent aim: eyes=%p target=(%.1f,%.1f,%.1f) dir=(%.2f,%.2f,%.2f) quat=(%.3f,%.3f,%.3f,%.3f)\n",
				(void*)eyes, data::aimbot::m_vecPosition.x, data::aimbot::m_vecPosition.y, data::aimbot::m_vecPosition.z,
				dir.x, dir.y, dir.z, quat.x, quat.y, quat.z, quat.w);
		}
	}
	return true;
}

// ── Triggerbot ──
static void do_triggerbot() {
	if (!var::weapon::triggerbot || !game::m_pLocalplayer) return;

	Vector3 local_pos = rust::get_camera_position(mist::aim.get());
	Vector2 view = game::m_pLocalplayer->get_body_angles(mist::aim.get());
	norm2(view);
	float radius = (float)var::weapon::trigger_radius * 0.015f;
	bool on_target = false;

	for (const auto& entry : game::players) {
		BasePlayer* p = (BasePlayer*)rust::get_base_entity(mist::aim.get(), entry.first);
		if (!p || p->is_sleeping(mist::aim.get())) continue;
		Vector3 head = p->get_bone_position(mist::aim.get(), BoneList::head);
		if (head.empty()) continue;
		if (var::aim::max_distance > 0 && local_pos.distance(head) > (float)var::aim::max_distance) continue;
		Vector2 ang = calc_angle(local_pos, head);
		norm2(ang);
		Vector2 diff = angle_delta(view, ang);
		float ad = sqrtf(diff.x*diff.x + diff.y*diff.y);
		if (ad <= radius) { on_target = true; break; }
	}

	static auto last_fire = std::chrono::steady_clock::now();
	static auto last_on = std::chrono::steady_clock::now();
	static bool holding = false;
	auto now = std::chrono::steady_clock::now();

	if (on_target) {
		last_on = now;
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_fire).count();
		if (!holding && elapsed >= var::weapon::trigger_delay) {
			INPUT inp{}; inp.type = INPUT_MOUSE; inp.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
			SendInput(1, &inp, sizeof(INPUT));
			holding = true; last_fire = now;
		} else if (holding && var::weapon::trigger_rapid && elapsed >= 25) {
			INPUT inp{}; inp.type = INPUT_MOUSE; inp.mi.dwFlags = MOUSEEVENTF_LEFTUP;
			SendInput(1, &inp, sizeof(INPUT));
			holding = false;
		}
	} else {
		auto off = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_on).count();
		if (holding && off > 150) {
			INPUT inp{}; inp.type = INPUT_MOUSE; inp.mi.dwFlags = MOUSEEVENTF_LEFTUP;
			SendInput(1, &inp, sizeof(INPUT));
			holding = false; last_fire = now;
		}
	}
}

// ── Main thread ──
void aimbot::thread() {
	while (monitor::width == 0);

	auto compute_fov = [](float deg) -> float {
		float game_fov = 90.f;
		uintptr_t gfx = mist::misc->read<uintptr_t>(addr::gameassembly + offsets::fov_offset);
		uintptr_t gi = gfx ? mist::misc->read<uintptr_t>(gfx + 0xB8) : 0;
		if (gi >= 0x10000ULL && gi <= 0x7FFFFFFFFFFFULL) {
			float c = mist::misc->read<float>(gi + 0x18);
			if (c >= 30.f && c <= 130.f) game_fov = c;
		}
		float s1 = std::abs(std::tan((deg * (float)M_PI / 180.f) / 2.f));
		float s2 = std::abs(std::tan((game_fov * (float)M_PI / 180.f) / 2.f));
		if (s2 < 1e-6f) s2 = 1.f;
		return (s1 / s2) / 2.f * (monitor::width / 2.f);
	};

	aim_fov = compute_fov((float)var::aim::aim_fov);
	data::aimbot::m_flFOV = aim_fov;
	center = { monitor::width / 2.f, monitor::height / 2.f };
	static int old_fov = var::aim::aim_fov;

	// Hitchance
	static bool hc_active = false, hc_decided = false;
	// Diagnostics
	static ULONGLONG diag_last = 0;

	while (true) {
	 try {
		ULONGLONG now_diag = GetTickCount64();
		if (now_diag - diag_last > 5000) {
			diag_last = now_diag;
			printf("[aim-diag] enabled=%d local=%p players=%zu key_active=%d aim_fov=%.1f key=0x%X mode=%d\n",
				(int)var::aim::enabled, (void*)game::m_pLocalplayer, game::players.size(),
				(int)aim_key_active(), aim_fov, var::aim::aim_key, var::aim::aim_key_mode);
		}
		if (!game::m_pLocalplayer || !var::aim::enabled) {
			data::aimbot::m_bActive = false;
			data::aimbot::m_bFound = false;
			data::aimbot::m_bLocked = false;
			data::aimbot::m_pTarget = 0;
			data::aimbot::m_vecPosition = {};
			hc_decided = false;
			sleep_for(milliseconds(200));
			continue;
		}

		if (var::aim::aim_fov != old_fov) {
			aim_fov = compute_fov((float)var::aim::aim_fov);
			data::aimbot::m_flFOV = aim_fov;
			old_fov = var::aim::aim_fov;
		}

		do_triggerbot();

		if (!aim_key_active()) {
			hc_decided = false;
			data::aimbot::m_bLocked = false;
			sleep_for(milliseconds(var::weapon::triggerbot ? 8 : 100));
			continue;
		}

		// Hitchance roll
		if (!hc_decided) {
			hc_decided = true;
			if (var::aim::hitchance >= 100) hc_active = true;
			else if (var::aim::hitchance <= 0) hc_active = false;
			else { std::uniform_int_distribution<int> d(1,100); hc_active = d(g_rng) <= var::aim::hitchance; }
		}
		if (!hc_active) { sleep_for(milliseconds(3)); continue; }

		game::aim_mutex.lock();

		if (var::misc::omnisprint)
			game::m_rLocalInfo.m_pModelState->set_sprinting(mist::aim.get(), true);

		// Quick diagnostic: show local position
		{
			static ULONGLONG lp_last = 0;
			if (now_diag - lp_last > 3000) {
				lp_last = now_diag;
				Vector3 lp = rust::get_camera_position(mist::aim.get());
				Vector2 va = game::m_pLocalplayer->get_body_angles(mist::aim.get());
				printf("[aim-diag] local_pos=(%.1f,%.1f,%.1f) view_ang=(%.1f,%.1f) max_dist=%d\n",
					lp.x, lp.y, lp.z, va.x, va.y, var::aim::max_distance);
			}
		}

		if (!find_best_target()) {
			if (now_diag - diag_last < 500) {
				static ULONGLONG tgt_last = 0;
				if (now_diag - tgt_last > 3000) {
					tgt_last = now_diag;
					printf("[aim-diag] find_best_target FAILED: players=%zu fov_lim=%.1f\n",
						game::players.size(), (float)var::aim::aim_fov);
					// Check first player's bone
					if (!game::players.empty()) {
						auto& e = game::players[0];
						BasePlayer* p = (BasePlayer*)rust::get_base_entity(mist::aim.get(), e.first);
						if (p) {
							Vector3 bp = p->get_bone_position(mist::aim.get(), BoneList::head);
							bool sleeping = p->is_sleeping(mist::aim.get());
							printf("[aim-diag] player0: ent=0x%llX base=%p bone=(%.1f,%.1f,%.1f) sleeping=%d\n",
								(unsigned long long)e.first, (void*)p, bp.x, bp.y, bp.z, (int)sleeping);
						}
					}
				}
			}
			game::aim_mutex.unlock();
			sleep_for(milliseconds(50));
			continue;
		}

		if (!process_aim()) {
			game::aim_mutex.unlock();
			sleep_for(milliseconds(3));
			continue;
		}

		game::aim_mutex.unlock();
		sleep_for(milliseconds(1));

	 } catch (...) {
		sleep_for(milliseconds(200));
	 }
	}
}