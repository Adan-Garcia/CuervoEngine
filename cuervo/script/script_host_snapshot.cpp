// Simulation snapshots of a running game (DESIGN.md 4.8, milestone 11.7a): ScriptHost::save,
// restore and snapshot_hash. The Pluma heap and VM go through pluma/snapshot.hpp; the host adds
// its scalars, the node tree roots and the owner references of the physics world to the same
// stream. Particle pools, RNG streams and the Box2D world are copied natively.

#include <format>

#include "cuervo/core/hash.hpp"
#include "cuervo/script/script_host.hpp"
#include "pluma/heap.hpp"
#include "pluma/snapshot.hpp"
#include "pluma/vm.hpp"

namespace cuervo::script {

using pluma::Value;

namespace {

constexpr std::uint32_t host_format = 2;

void write_frame(pluma::SnapshotWriter& w, const InputFrame& f) {
    w.u64(f.held);
    for (std::int16_t a : f.axes) w.u32(static_cast<std::uint32_t>(static_cast<std::uint16_t>(a)));
    for (std::int16_t p : f.pointer) w.u32(static_cast<std::uint32_t>(static_cast<std::uint16_t>(p)));
    w.u32(f.text);
}

InputFrame read_frame(pluma::SnapshotReader& r) {
    InputFrame f;
    f.held = r.u64();
    for (std::int16_t& a : f.axes) a = static_cast<std::int16_t>(static_cast<std::uint16_t>(r.u32()));
    for (std::int16_t& p : f.pointer) p = static_cast<std::int16_t>(static_cast<std::uint16_t>(r.u32()));
    f.text = r.u32();
    return f;
}

} // namespace

bool ScriptHost::save(SimSnapshot& out, std::string& error) const {
    if (session_ == nullptr) {
        error = "no game is loaded";
        return false;
    }
    if (panicked_) {
        error = "the game has panicked";
        return false;
    }
#if CUERVO_FEATURE_COLLISION
    if (!hitboxes_.empty()) {
        error = "a snapshot can only be taken between ticks";
        return false;
    }
#endif
    pluma::SnapshotWriter w(session_->vm());
    w.native_writer = [this](pluma::SnapshotWriter& sw, const pluma::InstanceObj& o) {
        if (o.native == static_cast<const void*>(this)) {
            sw.u8(1);
            return true;
        }
#if CUERVO_FEATURE_PARTICLES
        for (const pluma::RecordRT* t = o.type; t != nullptr; t = t->base) {
            if (t->name == "Emitter") {
                sw.u8(2);
                sw.u32(static_cast<const EmitterData*>(o.native)->id);
                return true;
            }
        }
#endif
        return false;
    };
    w.u32(host_format);
    w.u64(tick_);
    w.u64(anim_digest_);
    w.u64(idle_ticks_);
    w.boolean(quit_requested_);
    w.boolean(text_wanted_);
    const Input::State input = input_.save_state();
    std::vector<Input::State> snap_players;
    write_frame(w, input.current);
    write_frame(w, input.previous);
    w.u32(static_cast<std::uint32_t>(extra_inputs_.size()));
    for (const Input& p : extra_inputs_) {
        const Input::State st = p.save_state();
        write_frame(w, st.current);
        write_frame(w, st.previous);
        snap_players.push_back(st);
    }
#if CUERVO_FEATURE_PARTICLES
    w.u32(static_cast<std::uint32_t>(emitters_.size()));
    for (const EmitterMeta& m : emitters_) {
        w.boolean(m.used);
        w.str(m.tag);
    }
    w.u32(static_cast<std::uint32_t>(free_emitters_.size()));
    for (particles::EmitterId id : free_emitters_) w.u32(id);
#else
    w.u64(particle_ticks_);
#endif
    w.value(root_);
    w.value(canvas_);
    SimSnapshot snap;
    snap.tick = tick_;
    snap.input = input;
    snap.extra_players = std::move(snap_players);
#if CUERVO_FEATURE_PARTICLES
    snap.particles = particles_.save_state();
#else
    snap.rng = rng_;
#endif
#if CUERVO_FEATURE_PHYSICS
    if (physics_) {
        snap.physics = physics_->save_state(w);
        if (!snap.physics) {
            error = w.error();
            return false;
        }
    }
#endif
    w.vm_state();
    if (!w.finish(snap.image)) {
        error = w.error();
        return false;
    }
    out = std::move(snap);
    return true;
}

bool ScriptHost::restore(const SimSnapshot& snap, std::string& error) {
    if (session_ == nullptr) {
        error = "no game is loaded";
        return false;
    }
    pluma::VM& vm = session_->vm();
    auto native_reader = [this](pluma::SnapshotReader& r, pluma::InstanceObj&) -> void* {
        switch (r.u8()) {
            case 1: return this;
#if CUERVO_FEATURE_PARTICLES
            case 2: return new EmitterData{this, r.u32()};
#endif
            default: r.fail("the snapshot holds native data this build cannot restore"); return nullptr;
        }
    };
    // Building the objects first: a snapshot of other code is refused before anything is touched.
    pluma::SnapshotReader r(vm, snap.image, native_reader);
    if (!r.ok()) {
        error = r.error();
        return false;
    }
#if CUERVO_FEATURE_PHYSICS
    if (physics_ && !snap.physics) {
        error = "the snapshot has no physics world";
        return false;
    }
#endif

    // Drop everything the old simulation holds, then read the new one in the order it was written.
#if CUERVO_FEATURE_COLLISION
    for (Hitbox& h : hitboxes_) pluma::release(h.owner);
    hitboxes_.clear();
#endif
#if CUERVO_FEATURE_PHYSICS
    if (physics_) physics_->drop_owners();
#endif
    {
        const Value old_root = root_;
        const Value old_canvas = canvas_;
        root_ = canvas_ = Value{};
        pluma::release(old_root);
        pluma::release(old_canvas);
    }
    pluma::snapshot_reset(vm);

    bool ok = r.u32() == host_format;
    tick_ = r.u64();
    anim_digest_ = r.u64();
    idle_ticks_ = r.u64();
    quit_requested_ = r.boolean();
    text_wanted_ = r.boolean();
    const InputFrame current = read_frame(r);
    const InputFrame previous = read_frame(r);
    input_.restore_state({current, previous});
    const std::uint32_t player_count = r.u32();
    extra_inputs_.clear();
    for (std::uint32_t i = 0; i < player_count && r.ok(); ++i) {
        const InputFrame c = read_frame(r);
        const InputFrame p = read_frame(r);
        extra_inputs_.emplace_back(*input_map_);
        extra_inputs_.back().restore_state({c, p});
    }
#if CUERVO_FEATURE_PARTICLES
    const std::uint32_t emitter_count = r.u32();
    emitters_.assign(emitter_count, EmitterMeta{});
    for (EmitterMeta& m : emitters_) {
        m.used = r.boolean();
        m.tag = r.str();
    }
    const std::uint32_t free_count = r.u32();
    free_emitters_.assign(free_count, 0);
    for (particles::EmitterId& id : free_emitters_) id = r.u32();
    particles_.restore_state(snap.particles);
#else
    particle_ticks_ = r.u64();
    rng_ = snap.rng;
#endif
    root_ = r.value();
    canvas_ = r.value();
#if CUERVO_FEATURE_PHYSICS
    if (physics_ && ok && r.ok()) ok = physics_->restore_state(*snap.physics, r);
#endif
    ok = ok && r.vm_state() && r.finish();
    if (!ok || !r.ok()) {
        error = r.error().empty() ? "the snapshot is damaged" : r.error();
        panicked_ = true;
        error_ = "restoring a snapshot failed: " + error;
        return false;
    }
    panicked_ = false;
    error_.clear();
    last_panic_.reset();
    return true;
}

std::uint64_t ScriptHost::snapshot_hash() const {
    SimSnapshot snap;
    std::string error;
    if (!save(snap, error)) return 0;
    StateHasher h;
    h.span(std::span<const std::uint8_t>(snap.image));
    h.value(state_hash());
    h.value(snap.input.current.held);
    h.value(snap.input.current.axes);
    h.value(snap.input.current.pointer);
    h.value(snap.input.current.text);
    h.value(snap.input.previous.held);
    h.value(snap.input.previous.axes);
    h.value(snap.input.previous.pointer);
    h.value(snap.input.previous.text);
#if CUERVO_FEATURE_PARTICLES
    particles_.hash_deep(h);
#else
    Pcg32 probe = rng_;
    h.value(probe.next_u32());
    h.value(probe.next_u32());
#endif
#if CUERVO_FEATURE_PHYSICS
    if (physics_) physics_->hash_deep(h);
#endif
    return h.digest();
}

} // namespace cuervo::script
