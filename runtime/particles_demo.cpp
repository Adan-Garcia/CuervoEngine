#include "particles_demo.hpp"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cuervo/collision/collision_world.hpp"
#include "cuervo/core/fixed_timestep.hpp"
#include "cuervo/core/log.hpp"
#include "cuervo/core/name.hpp"
#include "cuervo/input/input_map.hpp"
#include "cuervo/math/dmath.hpp"
#include "cuervo/particles/particle_render.hpp"
#include "cuervo/particles/particle_system.hpp"
#include "cuervo/render/font_import.hpp"
#include "cuervo/render/sdf_batcher.hpp"
#include "cuervo/render/text_renderer.hpp"

using namespace cuervo;
using namespace cuervo::particles;
using collision::layer_bit;

namespace {

constexpr ivec2 virtual_size{1280, 720};
constexpr vec2 virtual_extent{1280.0f, 720.0f};
constexpr float tick_dt = 1.0f / 60.0f;
constexpr rhi::Color background{0.035f, 0.035f, 0.075f, 1.0f};
constexpr rhi::Color letterbox{0.0f, 0.0f, 0.0f, 1.0f};

// Collision layers (DESIGN.md 4.7 layers/masks).
constexpr std::uint32_t layer_player = 0;
constexpr std::uint32_t layer_enemy_shots = 1;
constexpr std::uint32_t layer_boss = 2;
constexpr std::uint32_t layer_player_shots = 3;
// `user` values of the game's own proxies (bit 63 clear: not particles).
constexpr std::uint64_t user_player = 1;
constexpr std::uint64_t user_boss = 2;

constexpr float player_hitbox = 3.0f;
constexpr float graze_radius = 26.0f;
constexpr std::uint32_t tag_grazing = 1u;   // inside the graze radius this tick
constexpr std::uint32_t tag_grazed = 2u;    // was ever grazed (counted once)

double now_seconds() {
    return static_cast<double>(SDL_GetPerformanceCounter()) / static_cast<double>(SDL_GetPerformanceFrequency());
}

std::uint32_t scaled(std::uint32_t count, float intensity) {
    return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(static_cast<float>(count) * intensity + 0.5f));
}

ParticleTemplate shot(float speed, float size, rhi::Color color, float life = 0.0f) {
    ParticleTemplate t;
    t.speed = speed;
    t.size = size;
    t.color = color;
    t.life = life;
    return t;
}

Spawner spawner(Pattern pattern, ParticleTemplate particle, std::uint32_t interval, std::uint32_t delay = 0) {
    Spawner s;
    s.pattern = pattern;
    s.particle = particle;
    s.interval = interval;
    s.delay = delay;
    return s;
}

// Timing of the simulation phases, wall clock (measurement only: never fed
// back into the simulation).
struct SimTimes {
    double particles = 0.0; // simulate + spawn
    double colliders = 0.0; // register + build hash
    double queries = 0.0;   // graze query + hit resolution
};

// --- simulation (deterministic: fixed tick, dmath, one seeded RNG) --------------

class BulletHell {
public:
    BulletHell(std::uint64_t seed, float intensity) : system_(seed, 32.0f) {
        const KillOutside offscreen{{-40.0f, -40.0f}, {virtual_extent.x() + 40.0f, virtual_extent.y() + 40.0f}};
        CollisionConfig enemy_shots;
        enemy_shots.enabled = true;
        enemy_shots.layer = layer_enemy_shots;
        enemy_shots.hits = layer_bit(layer_player);

        // Spiral: many arms, a little faster every shot.
        {
            EmitterConfig c;
            c.collision = enemy_shots;
            c.modules = {offscreen};
            c.render.outline_width = 1.0f;
            c.render.outline_color = {1.0f, 0.85f, 0.9f, 0.9f};
            c.spawners.push_back(spawner(Spiral{scaled(18, intensity), 0.0f, 0.105f},
                                         shot(105.0f, 4.0f, {1.0f, 0.3f, 0.5f, 1.0f}), 1));
            bullet_emitters_.push_back(system_.add_emitter(std::move(c)));
        }
        // Flower rings: two counter-turning emitters, capsules that ramp up.
        for (float turn : {0.35f, -0.35f}) {
            EmitterConfig c;
            c.collision = enemy_shots;
            c.collision.half_length = 4.0f; // capsule along the rotation
            c.collision.radius = 2.5f;
            c.modules = {SpeedRamp{40.0f, 0.0f, 170.0f}, Turn{turn}, FaceVelocity{}, offscreen};
            c.render.shape = render::SdfShape::Capsule;
            c.render.half_length = 1.6f;
            c.spawners.push_back(spawner(Ring{scaled(80, intensity), turn > 0.0f ? 0.0f : 0.06f},
                                         shot(50.0f, 2.5f, turn > 0.0f ? rhi::Color{0.4f, 0.8f, 1.0f, 1.0f}
                                                                       : rhi::Color{0.55f, 0.6f, 1.0f, 1.0f}),
                                         20, turn > 0.0f ? 0u : 10u));
            bullet_emitters_.push_back(system_.add_emitter(std::move(c)));
        }
        // Wavy curtains from both top corners.
        for (float side : {0.0f, 1.0f}) {
            EmitterConfig c;
            c.collision = enemy_shots;
            c.modules = {Wave{12.0f, 0.7f, side * dmath::pi}, offscreen};
            c.render.shape = render::SdfShape::Ring;
            c.render.thickness = 0.45f;
            c.spawners.push_back(spawner(Arc{scaled(22, intensity), dmath::half_pi + (side - 0.5f) * -0.5f, 1.3f},
                                         shot(80.0f, 4.5f, {1.0f, 0.8f, 0.3f, 1.0f}), 4, side > 0.0f ? 2u : 0u));
            curtain_emitters_.push_back(system_.add_emitter(std::move(c)));
            bullet_emitters_.push_back(curtain_emitters_.back());
        }
        // Aimed fans at the player.
        {
            EmitterConfig c;
            c.collision = enemy_shots;
            c.modules = {FaceVelocity{}, offscreen};
            c.render.shape = render::SdfShape::RoundedRect;
            c.render.corner = 0.45f;
            c.render.outline_width = 1.0f;
            c.render.outline_color = {1.0f, 1.0f, 1.0f, 0.8f};
            c.spawners.push_back(spawner(Aimed{7, 0.6f}, shot(230.0f, 4.0f, {0.6f, 1.0f, 0.5f, 1.0f}), 24));
            bullet_emitters_.push_back(system_.add_emitter(std::move(c)));
        }
        // Homing missiles: fly out, home for a while, then go straight.
        {
            EmitterConfig c;
            c.collision = enemy_shots;
            c.collision.half_length = 5.0f;
            c.collision.radius = 3.0f;
            c.modules = {Homing{1.8f, 0.6f, 2.2f}, FaceVelocity{},
                         ColorOverLife{{1.0f, 0.55f, 0.2f, 1.0f}, {1.0f, 0.15f, 0.1f, 0.6f}}, offscreen};
            c.render.shape = render::SdfShape::Capsule;
            c.render.half_length = 2.0f;
            c.spawners.push_back(spawner(Ring{8, 0.2f}, shot(140.0f, 3.0f, {1.0f, 0.5f, 0.2f, 1.0f}, 7.0f), 80, 30));
            bullet_emitters_.push_back(system_.add_emitter(std::move(c)));
        }
        // Orbiters circling the boss while spiralling outwards, then fading.
        {
            EmitterConfig c;
            c.collision = enemy_shots;
            c.modules = {Orbit{0.9f, 55.0f}, SizeOverLife{1.0f, 0.4f, Ease::EaseIn},
                         ColorOverLife{{0.85f, 0.45f, 1.0f, 1.0f}, {0.85f, 0.45f, 1.0f, 0.3f}}};
            c.spawners.push_back(spawner(Ring{scaled(16, intensity), 0.0f}, shot(1.0f, 6.0f, {0.85f, 0.45f, 1.0f, 1.0f}, 9.0f),
                                         45));
            bullet_emitters_.push_back(system_.add_emitter(std::move(c)));
        }
        // Player shots: a second layer pair (player shots vs. the boss).
        {
            EmitterConfig c;
            c.collision.enabled = true;
            c.collision.layer = layer_player_shots;
            c.collision.hits = layer_bit(layer_boss);
            c.collision.half_length = 6.0f;
            c.modules = {offscreen};
            c.render.shape = render::SdfShape::Capsule;
            c.render.half_length = 3.0f;
            c.spawners.push_back(spawner(Arc{3, -dmath::half_pi, 0.12f}, shot(900.0f, 2.0f, {0.7f, 1.0f, 1.0f, 0.9f}), 5));
            player_shots_ = system_.add_emitter(std::move(c));
        }
        // Visual-mode sparks on hits (own RNG, never collide).
        {
            EmitterConfig c;
            c.mode = Mode::Visual;
            c.modules = {Drag{3.0f}, SizeOverLife{1.0f, 0.0f, Ease::EaseOut},
                         ColorOverLife{{1.0f, 1.0f, 0.8f, 1.0f}, {1.0f, 0.3f, 0.2f, 0.0f}}};
            sparks_ = system_.add_emitter(std::move(c));
        }
    }

    // One fixed tick. `move` in [-1, 1]^2; autopilot ignores it.
    void tick(vec2 move, bool focus, bool autopilot, SimTimes& times) {
        const auto t = static_cast<float>(system_.ticks());
        if (autopilot) {
            player_ = {640.0f + 420.0f * dmath::sin(t * 0.011f), 590.0f + 70.0f * dmath::sin(t * 0.027f)};
        } else {
            const float speed = (focus ? 120.0f : 300.0f) * tick_dt;
            player_ += move * speed;
        }
        player_ = cuervo::max(vec2{16.0f, 16.0f}, cuervo::min(player_, virtual_extent - vec2{16.0f, 16.0f}));
        boss_ = {640.0f + 260.0f * dmath::sin(t * 0.007f), 170.0f + 40.0f * dmath::sin(t * 0.013f)};

        for (EmitterId id : bullet_emitters_) {
            system_.emitter(id).position = boss_;
            system_.emitter(id).target = player_;
        }
        system_.emitter(curtain_emitters_[0]).position = {60.0f, 40.0f};
        system_.emitter(curtain_emitters_[1]).position = {1220.0f, 40.0f};
        system_.emitter(player_shots_).position = player_ - vec2{0.0f, 12.0f};
        system_.emitter(sparks_).position = player_;

        double t0 = now_seconds();
        system_.simulate(tick_dt);
        system_.spawn();
        double t1 = now_seconds();
        system_.collect_colliders([&](collision::CollisionWorld& world) {
            world.add_circle({player_, player_hitbox}, layer_player, user_player);
            world.add_capsule({boss_ - vec2{34.0f, 0.0f}, boss_ + vec2{34.0f, 0.0f}, 20.0f}, layer_boss, user_boss);
        });
        double t2 = now_seconds();

        // Graze: enemy shots near the player, queried before hits are resolved
        // so the proxies' pool indices are still valid.
        for (EmitterId id : bullet_emitters_) {
            for (std::uint32_t& tag : system_.emitter(id).pool().tag) tag &= ~tag_grazing;
        }
        grazing_.clear();
        system_.collision().within(player_, graze_radius, layer_bit(layer_enemy_shots), grazing_);
        for (collision::ProxyId proxy : grazing_) {
            const std::uint64_t user = system_.collision().user(proxy);
            std::uint32_t& tag = system_.emitter(ParticleSystem::user_emitter(user))
                                     .pool()
                                     .tag[ParticleSystem::user_index(user)];
            if (!(tag & tag_grazed)) ++grazes_;
            tag |= tag_grazing | tag_grazed;
        }

        system_.resolve_hits();
        for (const Hit& hit : system_.hits()) {
            Emitter& sparks = system_.emitter(sparks_);
            if (hit.target_user == user_player) {
                ++hits_;
                last_hit_tick_ = system_.ticks();
                sparks.emit(Burst{24, 0.0f, dmath::tau}, hit.position, shot(260.0f, 3.0f, {1, 1, 1, 1}, 0.6f));
            } else if (hit.target_user == user_boss) {
                ++boss_hits_;
                sparks.emit(Burst{3, -dmath::half_pi, 2.0f}, hit.position, shot(160.0f, 2.0f, {1, 1, 1, 1}, 0.3f));
            }
        }
        double t3 = now_seconds();
        times.particles += t1 - t0;
        times.colliders += t2 - t1;
        times.queries += t3 - t2;
        ++ticks_;
    }

    const ParticleSystem& system() const { return system_; }
    std::size_t bullets() const {
        std::size_t n = 0;
        for (EmitterId id : bullet_emitters_) n += system_.emitter(id).size();
        return n;
    }
    std::span<const EmitterId> bullet_emitters() const { return bullet_emitters_; }
    vec2 player() const { return player_; }
    vec2 boss() const { return boss_; }
    std::uint32_t hits() const { return hits_; }
    std::uint32_t grazes() const { return grazes_; }
    std::uint32_t boss_hits() const { return boss_hits_; }
    std::uint64_t ticks() const { return ticks_; }
    // Ticks since the player was last hit (large if never).
    std::uint64_t since_hit() const { return last_hit_tick_ ? system_.ticks() - last_hit_tick_ : 1u << 30; }

private:
    ParticleSystem system_;
    std::vector<EmitterId> bullet_emitters_;
    std::vector<EmitterId> curtain_emitters_;
    EmitterId player_shots_ = 0;
    EmitterId sparks_ = 0;
    std::vector<collision::ProxyId> grazing_;
    vec2 player_{640.0f, 600.0f};
    vec2 boss_{640.0f, 170.0f};
    std::uint32_t hits_ = 0;
    std::uint32_t grazes_ = 0;
    std::uint32_t boss_hits_ = 0;
    std::uint64_t last_hit_tick_ = 0;
    std::uint64_t ticks_ = 0;
};

// --- rendering (reads the simulation, never writes it) ----------------------------

void build_world(render::SdfBatcher& batch, const BulletHell& game, vec2 player_drawn, float alpha) {
    batch.clear();
    const ParticleSystem& system = game.system();

    // Boss: a capsule body (its hitbox) with a ring.
    batch.capsule(game.boss(), 34.0f, 20.0f, 0.0f, {0.35f, 0.12f, 0.3f, 1.0f}, {2.0f, {1.0f, 0.5f, 0.8f, 1.0f}});
    batch.ring(game.boss(), 12.0f, 3.0f, {1.0f, 0.6f, 0.85f, 1.0f});

    draw_emitters(system, batch, alpha, system.jobs()); // packed in parallel (DESIGN.md 4.15)

    // Collision highlights: bullets inside the graze radius get a bright ring.
    for (EmitterId id : game.bullet_emitters()) {
        const ParticlePool& p = system.emitter(id).pool();
        for (std::size_t i = 0; i < p.size_live(); ++i) {
            if (!(p.tag[i] & tag_grazing)) continue;
            const vec2 pos{p.prev_x[i] + (p.x[i] - p.prev_x[i]) * alpha, p.prev_y[i] + (p.y[i] - p.prev_y[i]) * alpha};
            batch.ring(pos, p.size[i] + 4.0f, 1.5f, {1.0f, 1.0f, 0.3f, 1.0f});
        }
    }

    // Player: ship, graze radius, hitbox (the only part that can be hit).
    const std::uint64_t since_hit = game.since_hit();
    batch.ring(player_drawn, graze_radius, 1.0f, {0.6f, 0.9f, 1.0f, 0.25f});
    batch.rounded_rect(player_drawn + vec2{0.0f, 4.0f}, {9.0f, 13.0f}, 6.0f, 0.0f, {0.2f, 0.45f, 0.8f, 0.9f});
    if (since_hit < 30) {
        const float k = static_cast<float>(since_hit) / 30.0f;
        batch.ring(player_drawn, 8.0f + 60.0f * k, 3.0f, {1.0f, 0.2f, 0.2f, 1.0f - k});
    }
    const rhi::Color hitbox_fill = since_hit < 20 ? rhi::Color{1.0f, 0.2f, 0.2f, 1.0f} : rhi::Color{1.0f, 1.0f, 1.0f, 1.0f};
    batch.circle(player_drawn, player_hitbox, hitbox_fill, {1.5f, {1.0f, 0.2f, 0.3f, 1.0f}});
}

struct FrameStats {
    std::vector<double> frame_ms;
    double sim_ms_sum = 0.0;
    double sim_ms_max = 0.0;
    SimTimes phases;
    double render_ms_sum = 0.0;
    double bullets_sum = 0.0;
    std::uint64_t ticks = 0;
    std::uint64_t frames = 0;

    static double percentile(std::vector<double> v, double p) {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5))];
    }
};

} // namespace

int run_particles_demo(Window& window, rhi::Device& device, const ParticlesDemoOptions& options) {
    render::SdfBatcher world;
    render::SdfBatcher hud_panels;
    if (!world.init(device, 16384) || !hud_panels.init(device, 16)) {
        return EXIT_FAILURE;
    }
    render::TextRenderer text;
    render::FontHandle font;
    if (!options.font_path.empty() && text.init(device)) {
        if (auto atlas = render::load_font_file(options.font_path)) {
            font = text.add_font(std::move(*atlas));
        }
    }
    if (!font) {
        LOG_WARN("No HUD font ('{}'); running without text", options.font_path);
    }

    const Name move_x("move_x");
    const Name move_y("move_y");
    const Name focus("focus");
    const Name toggle("toggle_autopilot");
    const Name quit("quit");
    InputMap input_map;
    input_map.set_axis(move_x, {AxisSource::digital(Binding::key(Key::Left), Binding::key(Key::Right)),
                                AxisSource::stick(PadAxis::LeftX)});
    input_map.set_axis(move_y, {AxisSource::digital(Binding::key(Key::Up), Binding::key(Key::Down)),
                                AxisSource::stick(PadAxis::LeftY)});
    input_map.set_action(focus, {Binding::key(Key::LeftShift), Binding::key(Key::RightShift),
                                 Binding::button(PadButton::RightShoulder)});
    input_map.set_action(toggle, {Binding::key(Key::Space), Binding::button(PadButton::South)});
    input_map.set_action(quit, {Binding::key(Key::Escape), Binding::button(PadButton::Back)});
    Input input(input_map);
    DeviceState devices;

    BulletHell game(options.seed, options.intensity);
    FixedTimestep timestep(60.0);
    const render::Camera2D camera = render::Camera2D::screen(virtual_extent);
    bool autopilot = true;

    LOG_INFO("Particle demo: seed {:#x}, intensity {:.2f}{}", options.seed, options.intensity,
             options.bench_seconds > 0.0 ? std::format(", benchmark {:.1f} s", options.bench_seconds) : "");

    enum class BenchPhase { Warmup, Measuring, Done };
    BenchPhase bench_phase = BenchPhase::Warmup;
    FrameStats bench;
    FrameStats window_stats; // rolling numbers for the HUD / periodic log
    const double start = now_seconds();
    double previous = start;
    double bench_start = start;
    double stats_start = start;
    vec2 player_prev = game.player();
    std::string hud_line;
    bool running = true;

    while (running) {
        const double now = now_seconds();
        const double frame_ms = (now - previous) * 1000.0;
        running = window.poll_events(devices) && running;
        FixedTimestep::Step step = timestep.advance(now - previous);
        previous = now;
        if (!options.screenshot.empty()) {
            step = {game.ticks() < options.screenshot_tick ? 1 : 0, 1.0f, false};
        }

        // --- simulation -------------------------------------------------------
        SimTimes phases;
        double tick_ms_max = 0.0;
        for (int i = 0; i < step.ticks; ++i) {
            input.push(input_map.sample(devices));
            if (input.pressed(quit)) running = false;
            if (input.pressed(toggle)) autopilot = !autopilot;
            const vec2 move{input.axis(move_x), input.axis(move_y)};
            if (move.x() != 0.0f || move.y() != 0.0f) autopilot = false;
            player_prev = game.player();
            const double tick_start = now_seconds();
            game.tick(move, input.held(focus), autopilot, phases);
            tick_ms_max = std::max(tick_ms_max, (now_seconds() - tick_start) * 1000.0);
        }
        const double sim_ms = (phases.particles + phases.colliders + phases.queries) * 1000.0;

        // --- render -----------------------------------------------------------
        const bool capture = !options.screenshot.empty() && game.ticks() >= options.screenshot_tick;
        device.begin_frame(window.pixel_size());
        const double render_start = now_seconds();
        const ivec2 fb = device.framebuffer_size();
        const render::ViewportFit fit = render::fit_viewport(options.scale, virtual_size, fb);
        device.clear(letterbox);
        device.set_viewport(fit.viewport);
        device.set_scissor(fit.viewport);
        device.clear(background);
        const float alpha = step.alpha;
        const vec2 player_drawn = player_prev + (game.player() - player_prev) * alpha;
        build_world(world, game, player_drawn, alpha);
        const float pixels = std::min(fit.scale.x(), fit.scale.y()) * camera.zoom;
        const Affine2 view = camera.view_projection(virtual_extent);
        world.flush(view, pixels > 0.0f ? 1.0f / pixels : 1.0f);

        if (font) {
            const bool refresh = hud_line.empty() || !options.screenshot.empty() || now - stats_start >= 0.5;
            if (refresh) {
                if (!options.screenshot.empty()) {
                    // Screenshots show only deterministic numbers, so backends compare equal.
                    hud_line = std::format("{} bullets   tick {}\nhits {}   grazes {}   boss hits {}", game.bullets(),
                                           game.ticks(), game.hits(), game.grazes(), game.boss_hits());
                } else {
                    const double frames = static_cast<double>(std::max<std::uint64_t>(window_stats.frames, 1));
                    const double ticks = static_cast<double>(std::max<std::uint64_t>(window_stats.ticks, 1));
                    hud_line = std::format(
                        "{} bullets ({} particles)   {:.0f} FPS   frame {:.2f} ms\n"
                        "sim {:.3f} ms/tick (particles {:.3f}, hash {:.3f}, queries {:.3f})   render CPU {:.2f} ms\n"
                        "hits {}   grazes {}   boss hits {}   {}   {}",
                        game.bullets(), game.system().particle_count(), frames / std::max(now - stats_start, 1e-6),
                        (now - stats_start) * 1000.0 / frames, window_stats.sim_ms_sum / ticks,
                        window_stats.phases.particles * 1000.0 / ticks, window_stats.phases.colliders * 1000.0 / ticks,
                        window_stats.phases.queries * 1000.0 / ticks, window_stats.render_ms_sum / frames, game.hits(),
                        game.grazes(), game.boss_hits(), autopilot ? "autopilot" : "manual",
                        rhi::backend_name(device.info().backend));
                    window_stats = {};
                    stats_start = now;
                }
            }
            render::TextStyle style;
            style.size = 15.0f;
            const render::TextLayout layout = render::layout_text(*text.font(font), hud_line, style);
            const vec2 origin{18.0f, 14.0f};
            hud_panels.clear();
            hud_panels.rounded_rect(origin + layout.size() * 0.5f, layout.size() * 0.5f + vec2{10.0f, 7.0f}, 7.0f, 0.0f,
                                    {0.0f, 0.0f, 0.0f, 0.6f}, {1.0f, {0.6f, 0.7f, 1.0f, 0.5f}});
            hud_panels.flush(view, pixels > 0.0f ? 1.0f / pixels : 1.0f);
            text.clear();
            text.draw_layout(font, layout, Affine2::translate(origin), {1.0f, 1.0f, 1.0f, 1.0f});
            render::TextStyle help;
            help.size = 13.0f;
            help.baseline = render::TextBaseline::Bottom;
            text.draw(font, "arrows: move   shift: focus   space: autopilot   esc: quit", vec2{18.0f, 706.0f}, help,
                      {0.8f, 0.85f, 1.0f, 0.6f});
            text.flush(view);
        }
        if (capture) {
            device.capture_frame(options.screenshot);
        }
        const double render_ms = (now_seconds() - render_start) * 1000.0;
        device.end_frame();

        window_stats.frames += 1;
        window_stats.ticks += static_cast<std::uint64_t>(step.ticks);
        window_stats.sim_ms_sum += sim_ms;
        window_stats.phases.particles += phases.particles;
        window_stats.phases.colliders += phases.colliders;
        window_stats.phases.queries += phases.queries;
        window_stats.render_ms_sum += render_ms;

        if (capture) {
            LOG_INFO("Screenshot written to '{}' at tick {} ({} bullets, {} hits, {} grazes, state {:#018x})",
                     options.screenshot, game.ticks(), game.bullets(), game.hits(), game.grazes(),
                     game.system().state_hash());
            running = false;
        }

        if (options.bench_seconds > 0.0) {
            if (bench_phase == BenchPhase::Warmup && now - bench_start >= 6.0) {
                // The pattern reaches its steady bullet count in ~5 s.
                bench_phase = BenchPhase::Measuring;
                bench_start = now;
            } else if (bench_phase == BenchPhase::Measuring) {
                bench.frame_ms.push_back(frame_ms);
                bench.sim_ms_sum += sim_ms;
                bench.sim_ms_max = std::max(bench.sim_ms_max, tick_ms_max);
                bench.phases.particles += phases.particles;
                bench.phases.colliders += phases.colliders;
                bench.phases.queries += phases.queries;
                bench.render_ms_sum += render_ms;
                bench.bullets_sum += static_cast<double>(game.bullets());
                bench.ticks += static_cast<std::uint64_t>(step.ticks);
                bench.frames += 1;
                if (now - bench_start >= options.bench_seconds) {
                    const double frames = static_cast<double>(bench.frames);
                    const double ticks = static_cast<double>(std::max<std::uint64_t>(bench.ticks, 1));
                    double total = 0.0;
                    for (double ms : bench.frame_ms) total += ms;
                    LOG_INFO("bench particles | {} | {}x{} | {:.0f} avg bullets | {} frames, {:.1f} FPS | frame ms avg "
                             "{:.3f} p50 {:.3f} p99 {:.3f} | sim {:.3f} ms/tick avg, {:.3f} max (particles {:.3f}, "
                             "hash build {:.3f}, queries+hits {:.3f}) | render CPU {:.3f} ms/frame",
                             rhi::backend_name(device.info().backend), fb.x(), fb.y(), bench.bullets_sum / frames,
                             bench.frames, frames * 1000.0 / total, total / frames,
                             FrameStats::percentile(bench.frame_ms, 0.5), FrameStats::percentile(bench.frame_ms, 0.99),
                             bench.sim_ms_sum / ticks, bench.sim_ms_max, bench.phases.particles * 1000.0 / ticks,
                             bench.phases.colliders * 1000.0 / ticks, bench.phases.queries * 1000.0 / ticks,
                             bench.render_ms_sum / frames);
                    bench_phase = BenchPhase::Done;
                    running = false;
                }
            }
        }
        if (options.quit_after > 0.0 && now - start >= options.quit_after) {
            LOG_INFO("--quit-after reached ({} ticks, {} bullets, {} particles, {} hits, {} grazes, {} boss hits)",
                     game.ticks(), game.bullets(), game.system().particle_count(), game.hits(), game.grazes(),
                     game.boss_hits());
            running = false;
        }
    }

    text.shutdown();
    hud_panels.shutdown();
    world.shutdown();
    return EXIT_SUCCESS;
}
