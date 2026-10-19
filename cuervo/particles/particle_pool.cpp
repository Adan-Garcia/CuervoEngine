#include "cuervo/particles/particle_pool.hpp"

namespace cuervo::particles {

void ParticlePool::reserve(std::size_t count) {
    for_each_float_column([&](std::vector<float>& column) { column.reserve(count); });
    color.reserve(count);
    tag.reserve(count);
    dead.reserve(count);
    slot_of_.reserve(count);
}

ParticleHandle ParticlePool::spawn(const ParticleSpawn& p) {
    if (x.size() >= max_particles) {
        return {};
    }
    std::uint32_t slot;
    if (!free_slots_.empty()) {
        slot = free_slots_.back();
        free_slots_.pop_back();
    } else {
        slot = static_cast<std::uint32_t>(slot_index_.size());
        slot_index_.push_back(0);
        slot_generation_.push_back(0);
    }
    const auto index = static_cast<std::uint32_t>(x.size());
    slot_index_[slot] = index;
    slot_of_.push_back(slot);

    x.push_back(p.position.x());
    y.push_back(p.position.y());
    prev_x.push_back(p.position.x());
    prev_y.push_back(p.position.y());
    vx.push_back(p.velocity.x());
    vy.push_back(p.velocity.y());
    age.push_back(0.0f);
    life.push_back(p.life);
    size.push_back(p.size);
    base_size.push_back(p.size);
    rotation.push_back(p.rotation);
    spin.push_back(p.spin);
    color.push_back(p.color);
    tag.push_back(0);
    dead.push_back(0);
    return {slot, slot_generation_[slot]};
}

std::optional<std::size_t> ParticlePool::index_of(ParticleHandle handle) const {
    if (handle.slot >= slot_index_.size() || slot_generation_[handle.slot] != handle.generation) {
        return std::nullopt;
    }
    const std::uint32_t index = slot_index_[handle.slot];
    if (index >= slot_of_.size() || slot_of_[index] != handle.slot) {
        return std::nullopt;
    }
    return index;
}

bool ParticlePool::kill(ParticleHandle handle) {
    if (const auto index = index_of(handle)) {
        kill(*index);
        return true;
    }
    return false;
}

void ParticlePool::kill_all() {
    for (auto& d : dead) d = 1;
}

void ParticlePool::remove_at(std::size_t index) {
    const std::size_t last = x.size() - 1;
    const std::uint32_t slot = slot_of_[index];
    ++slot_generation_[slot]; // outstanding handles to this particle go stale
    free_slots_.push_back(slot);

    if (index != last) {
        for_each_float_column([&](std::vector<float>& column) { column[index] = column[last]; });
        color[index] = color[last];
        tag[index] = tag[last];
        dead[index] = dead[last];
        slot_of_[index] = slot_of_[last];
        slot_index_[slot_of_[index]] = static_cast<std::uint32_t>(index);
    }
    for_each_float_column([](std::vector<float>& column) { column.pop_back(); });
    color.pop_back();
    tag.pop_back();
    dead.pop_back();
    slot_of_.pop_back();
}

} // namespace cuervo::particles
