#include "cuervo/physics/physics_world.hpp"

#include <algorithm>
#include <cmath>

#include "cuervo/core/hash.hpp"
#include "cuervo/core/log.hpp"
#include "cuervo/physics/physics_internal.hpp"
#include "pluma/vm.hpp"

namespace cuervo::physics {

using namespace detail;

SlideResult PhysicsWorld::move_and_slide(uint32_t body_handle, vec2 velocity, float dt,
                                        vec2 up_direction, float floor_max_angle, int max_slides) {
    SlideResult result;
    auto bit = bodies_.find(body_handle);
    if (bit == bodies_.end() || !b2Body_IsValid(bit->second.id)) {
        result.position = {0.0f, 0.0f};
        result.velocity = velocity;
        return result;
    }
    if (!is_finite(velocity) || !is_finite(dt) || dt <= 0.0f || !is_finite(up_direction)) {
        const b2Vec2 p = b2Body_GetPosition(bit->second.id);
        result.position = {p.x, p.y};
        result.velocity = is_finite(velocity) ? velocity : vec2{0.0f, 0.0f};
        return result;
    }

    const b2BodyId body_id = bit->second.id;
    b2Vec2 current_pos = b2Body_GetPosition(body_id);
    const b2Rot body_rot = b2Body_GetRotation(body_id);

    // Normalise the inputs the slide math assumes: a unit up vector, a sane floor angle and a bounded
    // slide count (script-provided values are not trusted).
    const float up_len = std::sqrt(dot(up_direction, up_direction));
    up_direction = up_len > 1e-6f ? up_direction * (1.0f / up_len) : vec2{0.0f, -1.0f};
    floor_max_angle = is_finite(floor_max_angle) ? std::clamp(floor_max_angle, 0.0f, dmath::pi) : 0.785398f;
    max_slides = std::clamp(max_slides, 1, 16);
    const float floor_cos = dmath::cos(floor_max_angle);

    bit->second.is_character = true;
    bit->second.up_direction = up_direction;
    bit->second.floor_max_angle = floor_max_angle;
    bit->second.floor_cos = floor_cos;

    // Build the shape proxy from the primary shape of the body.
    b2ShapeProxy proxy;
    b2ShapeId self_shape_id = b2_nullShapeId;
    bool has_proxy = false;
    for (uint32_t s_handle : bit->second.shapes) {
        auto sit = shapes_.find(s_handle);
        if (sit != shapes_.end() && !sit->second.config.is_sensor && !sit->second.disabled) {
            proxy = make_proxy_for_shape(sit->second.config, body_rot);
            self_shape_id = sit->second.id;
            has_proxy = true;
            break;
        }
    }
    if (!has_proxy) {
        const b2Vec2 pts[4] = {to_b2(-16.0f, -16.0f), to_b2(16.0f, -16.0f), to_b2(16.0f, 16.0f), to_b2(-16.0f, 16.0f)};
        proxy = b2MakeProxy(pts, 4, 0.0f);
    }

    const b2QueryFilter filter = {.categoryBits = bit->second.layer, .maskBits = bit->second.mask};
    constexpr float slop = character_slop;

    auto get_surface_velocity = [&](b2BodyId fb, b2Vec2 pt) -> b2Vec2 {
        b2Vec2 fv = b2Body_GetWorldPointVelocity(fb, pt);
        const auto fb_handle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(b2Body_GetUserData(fb)));
        auto fbit = bodies_.find(fb_handle);
        if (fbit != bodies_.end() && (fbit->second.constant_velocity.x() != 0.0f || fbit->second.constant_velocity.y() != 0.0f)) {
            fv.x += fbit->second.constant_velocity.x();
            fv.y += fbit->second.constant_velocity.y();
        }
        return fv;
    };

    // The character presses on a dynamic floor with its weight. Box2D clears forces only at step(), so
    // only the first move_and_slide() of a physics step may add it; further calls in the same step
    // (e.g. a script moving the character in several passes) would otherwise stack the weight.
    const bool weigh_floor = bit->second.weight_step != step_count_;
    bit->second.weight_step = step_count_;
    auto apply_weight = [&](b2BodyId floor_body, b2Vec2 at) {
        if (!weigh_floor) return;
        const float mass = bit->second.mass > 0.0f ? bit->second.mass : default_character_mass;
        b2Body_ApplyForce(floor_body, to_b2(mass * gravity_.x(), mass * gravity_.y()), at, true);
    };

    vec2 char_vel = velocity;
    b2Vec2 plat_vel = {0.0f, 0.0f};

    // 0. Depenetrate initial overlaps (e.g. from upward-moving platforms or dynamic floors).
    if (b2Shape_IsValid(self_shape_id)) {
        for (int dep_iter = 0; dep_iter < 4; ++dep_iter) {
            struct DepenOverlapCtx {
                b2BodyId self_body;
                uint32_t mask;
                std::vector<b2ShapeId> shapes;
            } ov_ctx{body_id, bit->second.mask, {}};

            auto ov_cb = [](b2ShapeId sid, void* ptr) -> bool {
                auto* c = static_cast<DepenOverlapCtx*>(ptr);
                if (b2Shape_IsSensor(sid)) return true;
                const b2BodyId bid = b2Shape_GetBody(sid);
                if (B2_ID_EQUALS(bid, c->self_body)) return true;
                const b2Filter f = b2Shape_GetFilter(sid);
                if ((f.categoryBits & c->mask) == 0) return true;
                c->shapes.push_back(sid);
                return true;
            };

            b2World_OverlapShape(world_id_, current_pos, &proxy, filter, ov_cb, &ov_ctx);
            if (ov_ctx.shapes.empty()) break;

            b2ShapeId best_shape = b2_nullShapeId;
            b2Vec2 best_norm = {0.0f, 0.0f};
            float best_depth = 0.0f;

            const b2Transform self_xf = {current_pos, body_rot};
            const b2AABB self_aabb = get_shifted_shape_aabb(self_shape_id, body_id, current_pos);
            for (b2ShapeId osid : ov_ctx.shapes) {
                const b2BodyId obid = b2Shape_GetBody(osid);
                if (!b2Body_IsValid(obid)) continue;
                b2Vec2 norm = {0.0f, 0.0f};
                float depth = 0.0f;
                if (compute_shape_penetration(self_shape_id, self_xf, osid, b2Body_GetTransform(obid), norm, depth)) {
                    if (dot(to_v2(norm), up_direction) < floor_cos && up_direction.y() < -0.5f) {
                        const b2AABB other_aabb = b2Shape_GetBounds(osid);
                        const float pen_up = self_aabb.upperBound.y - other_aabb.lowerBound.y;
                        if (pen_up > 0.0f && pen_up <= character_max_step_height && self_aabb.lowerBound.y < other_aabb.lowerBound.y) {
                            const b2Vec2 rot_up = b2RotateVector(b2Body_GetRotation(obid), to_b2(up_direction));
                            norm = (dot(to_v2(rot_up), up_direction) >= floor_cos) ? rot_up : to_b2(up_direction);
                            depth = pen_up;
                        }
                    }
                    if (depth > best_depth) {
                        best_depth = depth;
                        best_norm = norm;
                        best_shape = osid;
                    }
                }
            }

            if (!b2Shape_IsValid(best_shape) || best_depth <= 1e-4f) break;

            const vec2 norm_v = to_v2(best_norm);
            const float dot_up = dot(norm_v, up_direction);
            // For floor surfaces, push strictly along up_direction so tilted planks/slopes do not cause horizontal drift.
            const b2Vec2 push_motion = (dot_up >= floor_cos)
                                           ? b2MulSV((best_depth + slop) / std::max(0.5f, dot_up), to_b2(up_direction))
                                           : b2MulSV(best_depth + slop, best_norm);

            // Sweep along push_motion so depenetrating upward from a rising floor never pushes through a ceiling.
            ShapeCastContext push_ctx = sweep_shape(world_id_, proxy, filter, body_id, self_shape_id, current_pos, body_rot, bit->second.mask, push_motion);

            if (b2Shape_IsValid(push_ctx.hit_shape) && push_ctx.min_fraction < 1.0f) {
                const float frac = std::max(0.0f, push_ctx.min_fraction);
                current_pos = b2Add(current_pos, b2MulSV(frac, push_motion));
                const vec2 block_norm = to_v2(push_ctx.hit_normal);
                current_pos = b2Add(current_pos, to_b2(block_norm.x() * slop, block_norm.y() * slop));
                result.collided = true;
                if (dot(block_norm, up_direction) <= -floor_cos) {
                    result.on_ceiling = true;
                }
                break;
            } else {
                current_pos = b2Add(current_pos, push_motion);
            }

            result.collided = true;
            if (dot_up >= floor_cos) {
                if (dot(char_vel, up_direction) <= floor_leave_speed) {
                    result.on_floor = true;
                    result.floor_normal = norm_v;
                    const b2BodyId fb = b2Shape_GetBody(best_shape);
                    if (b2Body_IsValid(fb)) {
                        plat_vel = get_surface_velocity(fb, current_pos);
                        result.floor_velocity = {plat_vel.x, plat_vel.y};
                    }
                    const float down_v = dot(char_vel, -up_direction);
                    if (down_v > 0.0f) {
                        char_vel = char_vel + up_direction * down_v;
                    }
                }
            } else if (dot_up <= -floor_cos) {
                result.on_ceiling = true;
                const float vd = dot(char_vel, norm_v);
                if (vd < 0.0f) {
                    char_vel = char_vel - norm_v * vd;
                }
            } else {
                result.on_wall = true;
                result.wall_normal = norm_v;
                const float vd = dot(char_vel, norm_v);
                if (vd < 0.0f) {
                    char_vel = char_vel - norm_v * vd;
                }
            }
        }
    }

    // Detect platform velocity beneath feet if resting on floor and not moving upward away from it.
    const float up_vel_start = dot(char_vel, up_direction);
    if (up_vel_start <= floor_leave_speed) {
        float probe_dist = floor_probe_dist;
        if (bit->second.on_floor) {
            const float down_speed = dot(bit->second.floor_velocity, -up_direction);
            if (down_speed > 0.0f) {
                probe_dist = std::clamp(floor_probe_dist + down_speed * dt * 1.25f, floor_probe_dist, floor_probe_max_dist);
            }
        }
        const b2Vec2 probe_motion = to_b2(-up_direction.x() * probe_dist, -up_direction.y() * probe_dist);
        ShapeCastContext probe_ctx = sweep_shape(world_id_, proxy, filter, body_id, self_shape_id, current_pos, body_rot, bit->second.mask, probe_motion);
        if (b2Shape_IsValid(probe_ctx.hit_shape) && probe_ctx.min_fraction < 1.0f) {
            const vec2 norm = {probe_ctx.hit_normal.x, probe_ctx.hit_normal.y};
            if (dot(norm, up_direction) >= floor_cos) {
                const b2BodyId floor_body = b2Shape_GetBody(probe_ctx.hit_shape);
                if (b2Body_IsValid(floor_body)) {
                    const auto ftype = b2Body_GetType(floor_body);
                    const float hit_dist = probe_dist * probe_ctx.min_fraction;
                    // For dynamic bodies, only inherit floor velocity if in close resting contact (<= 4.0f).
                    // Kinematic moving platforms can be tracked across their descent travel up to probe_dist.
                    if (ftype != b2_dynamicBody || hit_dist <= floor_probe_dist) {
                        plat_vel = get_surface_velocity(floor_body, current_pos);
                        result.floor_velocity = {plat_vel.x, plat_vel.y};
                        if (ftype == b2_dynamicBody) {
                            apply_weight(floor_body, current_pos);
                        }
                    }
                }
            }
        }
    }

    vec2 carry_vel = {plat_vel.x, plat_vel.y};
    const float plat_up_speed = dot(carry_vel, up_direction);
    if (plat_up_speed > 0.0f) {
        // Upward platform motion is resolved by floor contact and depenetration;
        // do not double-apply upward platform velocity in the slide cast.
        carry_vel = carry_vel - up_direction * plat_up_speed;
    }

    const vec2 total_vel = char_vel + carry_vel;
    b2Vec2 motion = {total_vel.x() * dt, total_vel.y() * dt};
    vec2 current_vel = total_vel;
    vec2 prev_slide_normal = {0.0f, 0.0f};
    bool has_prev_slide = false;

    for (int slide = 0; slide < max_slides; ++slide) {
        const float motion_len_sq = motion.x * motion.x + motion.y * motion.y;
        if (motion_len_sq < 1e-6f) break;

        ShapeCastContext ctx = sweep_shape(world_id_, proxy, filter, body_id, self_shape_id, current_pos, body_rot, bit->second.mask, motion);

        if (b2Shape_IsValid(ctx.hit_shape) && ctx.min_fraction < 1.0f) {
            vec2 normal = {ctx.hit_normal.x, ctx.hit_normal.y};
            const vec2 motion_v = {motion.x, motion.y};

            // If the motion is moving away from or parallel to the contact surface, this contact does not impede motion.
            if (dot(normal, motion_v) >= 0.0f) {
                current_pos = b2Add(current_pos, motion);
                motion = to_b2(0.0f, 0.0f);
                break;
            }

            const float frac = std::max(0.0f, ctx.min_fraction);
            const b2BodyId hit_body = b2Shape_GetBody(ctx.hit_shape);
            const auto hit_type = b2Body_GetType(hit_body);
            const float dot_up = dot(normal, up_direction);
            const bool hit_is_floor = dot_up >= floor_cos;

            // Step-up over small lips/seams on jointed dynamic floor planks (e.g. suspension bridge planks).
            if (!hit_is_floor && dot_up > -floor_cos &&
                (result.on_floor || bit->second.on_floor) &&
                hit_type == b2_dynamicBody && b2Body_GetJointCount(hit_body) > 0 &&
                dot(to_v2(current_pos) - to_v2(b2Body_GetWorldCenter(hit_body)), up_direction) > 0.0f) {
                const b2Vec2 step_base = b2Add(current_pos, to_b2(motion.x * frac, motion.y * frac));
                const b2Vec2 rem_orig = to_b2(motion.x * (1.0f - frac), motion.y * (1.0f - frac));
                if (b2Length(rem_orig) > 1e-4f) {
                    constexpr float max_step_height = character_max_step_height;
                    const b2Vec2 up_motion = to_b2(up_direction.x() * max_step_height, up_direction.y() * max_step_height);
                    ShapeCastContext up_ctx = sweep_shape(world_id_, proxy, filter, body_id, self_shape_id, step_base, body_rot, bit->second.mask, up_motion);
                    const float up_frac = b2Shape_IsValid(up_ctx.hit_shape) ? std::max(0.0f, up_ctx.min_fraction - 0.02f) : 1.0f;
                    if (up_frac > 0.1f) {
                        const b2Vec2 raised_pos = b2Add(step_base, b2MulSV(up_frac, up_motion));
                        ShapeCastContext fwd_ctx = sweep_shape(world_id_, proxy, filter, body_id, self_shape_id, raised_pos, body_rot, bit->second.mask, rem_orig);
                        const float fwd_frac = b2Shape_IsValid(fwd_ctx.hit_shape) ? std::max(0.0f, fwd_ctx.min_fraction) : 1.0f;
                        if (fwd_frac > 0.1f) {
                            const b2Vec2 fwd_pos = b2Add(raised_pos, b2MulSV(fwd_frac, rem_orig));
                            const b2Vec2 down_motion = b2MulSV(-(up_frac * max_step_height + 2.0f), to_b2(up_direction));
                            ShapeCastContext down_ctx = sweep_shape(world_id_, proxy, filter, body_id, self_shape_id, fwd_pos, body_rot, bit->second.mask, down_motion);
                            if (b2Shape_IsValid(down_ctx.hit_shape) && down_ctx.min_fraction < 1.0f) {
                                const vec2 down_norm = to_v2(down_ctx.hit_normal);
                                if (dot(down_norm, up_direction) >= floor_cos) {
                                    const float dfrac = std::max(0.0f, down_ctx.min_fraction);
                                    current_pos = b2Add(fwd_pos, b2MulSV(dfrac, down_motion));
                                    current_pos = b2Add(current_pos, to_b2(down_norm.x() * slop, down_norm.y() * slop));
                                    result.collided = true;
                                    result.on_floor = true;
                                    result.floor_normal = down_norm;
                                    const b2BodyId step_body = b2Shape_GetBody(down_ctx.hit_shape);
                                    if (b2Body_IsValid(step_body)) {
                                        const b2Vec2 fv = get_surface_velocity(step_body, current_pos);
                                        result.floor_velocity = {fv.x, fv.y};
                                        if (b2Body_GetType(step_body) == b2_dynamicBody) {
                                            apply_weight(step_body, current_pos);
                                        }
                                    }
                                    const float down_v = dot(char_vel, -up_direction);
                                    if (down_v > 0.0f) {
                                        char_vel = char_vel + up_direction * down_v;
                                    }
                                    motion = b2MulSV(1.0f - fwd_frac, rem_orig);
                                    continue;
                                }
                            }
                        }
                    }
                }
            }

            result.collided = true;
            current_pos = b2Add(current_pos, to_b2(motion.x * frac, motion.y * frac));

            // Slop offset along normal to prevent snagging (for vertical landings on sloped floors, offset along up_direction to avoid slope drift).
            if (hit_is_floor && std::abs(motion.x) < 1e-4f) {
                const float up_slop = slop / std::max(0.5f, dot_up);
                current_pos = b2Add(current_pos, to_b2(up_direction.x() * up_slop, up_direction.y() * up_slop));
            } else {
                current_pos = b2Add(current_pos, to_b2(normal.x() * slop, normal.y() * slop));
            }

            const vec2 hit_pt = {ctx.hit_point.x, ctx.hit_point.y};

            SlideCollision col;
            col.point = hit_pt;
            col.normal = normal;
            col.travel = {motion.x * frac, motion.y * frac};
            col.remainder = {motion.x * (1.0f - frac), motion.y * (1.0f - frac)};

            col.collider_body = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(b2Body_GetUserData(hit_body)));
            auto hbit = bodies_.find(col.collider_body);
            if (hbit != bodies_.end()) col.collider = hbit->second.owner;
            result.collisions.push_back(std::move(col));

            // Classify contact angle.
            if (hit_is_floor) {
                result.on_floor = true;
                result.floor_normal = normal;
                if (b2Body_IsValid(hit_body)) {
                    const b2Vec2 fv = get_surface_velocity(hit_body, to_b2(hit_pt));
                    result.floor_velocity = {fv.x, fv.y};
                    if (hit_type == b2_dynamicBody) {
                        apply_weight(hit_body, to_b2(hit_pt));
                    }
                }
            } else if (dot_up <= -floor_cos) {
                result.on_ceiling = true;
            } else {
                result.on_wall = true;
                result.wall_normal = normal;
            }

            // Push dynamic bodies (lateral/wall pushing only).
            if (hit_type == b2_dynamicBody && !hit_is_floor) {
                const bool is_hinged = (b2Body_GetJointCount(hit_body) > 0 && !b2Body_GetMotionLocks(hit_body).angularZ);
                const bool is_floor_plank = is_hinged && (result.on_floor || bit->second.on_floor) &&
                                            dot(to_v2(current_pos) - to_v2(b2Body_GetWorldCenter(hit_body)), up_direction) > 0.0f;
                const float normal_vel = dot(current_vel, -normal);
                if (normal_vel > 0.0f && !is_floor_plank) {
                    const b2MassData target_md = b2Body_GetMassData(hit_body);
                    const float target_mass = target_md.mass > 0.0f ? target_md.mass : 1.0f;
                    const float char_mass = bit->second.mass > 1.0f ? bit->second.mass : default_character_mass;
                    const float push_factor = std::min(1.0f, char_mass / target_mass);
                    const b2Vec2 body_vel = is_hinged ? b2Body_GetWorldPointVelocity(hit_body, to_b2(hit_pt))
                                                      : b2Body_GetLinearVelocity(hit_body);
                    const float target_vel_along_push = dot(to_v2(body_vel), -normal);
                    const float desired_vel = normal_vel * push_factor;
                    if (desired_vel > target_vel_along_push) {
                        const float delta_v = desired_vel - std::max(0.0f, target_vel_along_push);
                        const float impulse_mag = delta_v * target_mass;
                        const b2Vec2 impulse = to_b2(-normal.x() * impulse_mag, -normal.y() * impulse_mag);
                        if (is_hinged && b2DistanceSquared(b2Body_GetWorldCenter(hit_body), to_b2(hit_pt)) > 1.0f) {
                            b2Body_ApplyLinearImpulse(hit_body, impulse, to_b2(hit_pt), true);
                        } else {
                            b2Body_ApplyLinearImpulseToCenter(hit_body, impulse, true);
                        }
                    }
                }
            }

            // Deflect velocity and remaining motion along tangent.
            const float rem_frac = 1.0f - frac;
            vec2 rem_motion = {motion.x * rem_frac, motion.y * rem_frac};
            const float motion_dot = dot(rem_motion, normal);
            rem_motion = rem_motion - normal * motion_dot;

            const float vel_dot = dot(current_vel, normal);
            current_vel = current_vel - normal * vel_dot;

            if (hit_is_floor) {
                const float down_v = dot(char_vel, -up_direction);
                if (down_v > 0.0f) {
                    char_vel = char_vel + up_direction * down_v;
                }
            } else {
                const float char_vel_dot = dot(char_vel, normal);
                if (char_vel_dot < 0.0f) {
                    char_vel = char_vel - normal * char_vel_dot;
                }
            }

            // If deflecting along a second plane pushes us back into the previous plane (acute corner / wedge),
            // stop motion into the corner to prevent oscillation and corner-creeping.
            if (has_prev_slide && dot(normal, prev_slide_normal) < -1e-4f && dot(rem_motion, prev_slide_normal) < -1e-4f) {
                rem_motion = {0.0f, 0.0f};
                if (dot(current_vel, prev_slide_normal) < -1e-4f) {
                    current_vel = {0.0f, 0.0f};
                }
                if (dot(char_vel, prev_slide_normal) < -1e-4f) {
                    char_vel = {0.0f, 0.0f};
                }
            }
            prev_slide_normal = normal;
            has_prev_slide = true;

            motion = to_b2(rem_motion.x(), rem_motion.y());
        } else {
            current_pos = b2Add(current_pos, motion);
            motion = to_b2(0.0f, 0.0f);
            break;
        }
    }

    // Floor snap: if not already on a floor and not moving upward away from the floor,
    // probe directly downward by a small distance to detect resting contact or descending slopes.
    const float up_vel = dot(char_vel, up_direction);
    if (!result.on_floor && up_vel <= floor_leave_speed) {
        const float snap_dist = bit->second.on_floor ? floor_snap_dist_grounded : floor_probe_dist;
        const b2Vec2 snap_motion = to_b2(-up_direction.x() * snap_dist, -up_direction.y() * snap_dist);

        ShapeCastContext snap_ctx = sweep_shape(world_id_, proxy, filter, body_id, self_shape_id, current_pos, body_rot, bit->second.mask, snap_motion);

        if (b2Shape_IsValid(snap_ctx.hit_shape) && snap_ctx.min_fraction < 1.0f) {
            vec2 normal = {snap_ctx.hit_normal.x, snap_ctx.hit_normal.y};
            const float dot_up = dot(normal, up_direction);
            if (dot_up >= floor_cos) {
                result.on_floor = true;
                result.floor_normal = normal;

                const b2BodyId snap_body = b2Shape_GetBody(snap_ctx.hit_shape);
                if (b2Body_IsValid(snap_body)) {
                    const b2Vec2 fv = get_surface_velocity(snap_body, current_pos);
                    result.floor_velocity = {fv.x, fv.y};
                    if (b2Body_GetType(snap_body) == b2_dynamicBody) {
                        apply_weight(snap_body, current_pos);
                    }
                }

                const float frac = std::max(0.0f, snap_ctx.min_fraction);
                current_pos = b2Add(current_pos, to_b2(snap_motion.x * frac, snap_motion.y * frac));
                const float up_slop = slop / std::max(0.5f, dot_up);
                current_pos = b2Add(current_pos, to_b2(up_direction.x() * up_slop, up_direction.y() * up_slop));

                const float down_v = dot(char_vel, -up_direction);
                if (down_v > 0.0f) {
                    char_vel = char_vel + up_direction * down_v;
                }
            }
        }
    }

    if (result.on_floor) {
        bit->second.floor_velocity = result.floor_velocity;
        bit->second.on_floor = true;
    } else {
        result.floor_velocity = {0.0f, 0.0f};
        bit->second.floor_velocity = {0.0f, 0.0f};
        bit->second.on_floor = false;
    }

    b2Body_SetTransform(body_id, current_pos, body_rot);
    result.position = {current_pos.x, current_pos.y};
    result.velocity = char_vel;
    return result;
}

} // namespace cuervo::physics
