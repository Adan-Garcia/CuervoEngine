#pragma once

// Simulation snapshots of the Pluma heap and VM (DESIGN.md 4.8, milestone 11.7a).
//
// A snapshot is a byte image of everything a running game's scripts own: the
// objects reachable from the VM (globals, registers, coroutines, arenas) and
// from whatever roots the host adds, the VM's counters and the cycle
// collector's suspect buffer. Restoring it into a VM that runs the same code
// makes the game continue bit for bit (the collector included: it frees the
// same objects on the same tick).
//
// The host (cuervo/script) writes its own roots through the same stream: its
// scalars with u8/u32/..., references to objects with value()/object(). The
// writer numbers objects in first-seen order, so the image does not depend on
// addresses (it can be hashed). Every counted reference must be reported:
// finish() checks each object's reference count against the references it was
// told about, which finds a root the host forgot.
//
// Limits: a snapshot is only valid between ticks (no script frame open), for
// the same loaded code (a fingerprint is checked), and objects with native
// data need a host hook (plugin natives have none: the snapshot fails).

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pluma/value.hpp"

namespace pluma {

class VM;
struct SnapshotAccess;

class SnapshotWriter {
public:
    explicit SnapshotWriter(const VM& vm);

    // Writes the data a native object (InstanceObj::native) needs; false = unsupported.
    std::function<bool(SnapshotWriter&, const InstanceObj&)> native_writer;

    void u8(std::uint8_t v) { cur_->push_back(v); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void u32(std::uint32_t v) { raw(&v, 4); }
    void u64(std::uint64_t v) { raw(&v, 8); }
    void i64(std::int64_t v) { raw(&v, 8); }
    void f32(float v) { raw(&v, 4); }
    void str(std::string_view s) {
        u32(static_cast<std::uint32_t>(s.size()));
        raw(s.data(), s.size());
    }
    void raw(const void* data, std::size_t n);

    // A value; objects it names are written to the image. `owned`: the holder counts the
    // reference in the object's rc (almost always).
    void value(const Value& v, bool owned = true);
    // A reference to an object (null allowed).
    void object(const Object* o, bool owned = true);

    // Writes the VM's own state (call once, after the host's data).
    void vm_state();

    void fail(std::string message) {
        if (error_.empty()) error_ = std::move(message);
    }
    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }

    // Appends the suspect buffer and the object table, checks reference counts and returns the image.
    bool finish(std::vector<std::uint8_t>& image);

private:
    friend struct SnapshotAccess;
    std::uint32_t id_of(const Object* o);
    void write_object(std::uint32_t id);
    void record_ref(const RecordRT* rt);
    void enum_ref(const EnumRT* rt);
    void proto_ref(const Proto* p);
    bool foreign(const Object* root);
    void find_alien();

    const VM& vm_;
    std::vector<std::uint8_t> stream_, header_, body_;
    std::vector<std::uint8_t>* cur_ = nullptr;
    std::vector<const Object*> order_;
    std::unordered_map<const Object*, std::uint32_t> ids_;
    std::vector<std::uint32_t> counted_;
    std::unordered_map<const void*, std::uint32_t> record_ids_, enum_ids_;
    std::unordered_map<const Object*, std::pair<std::uint32_t, std::uint32_t>> consts_;
    std::unordered_map<const Object*, char> alien_; // suspects some other VM still holds
    std::string error_;
};

class SnapshotReader {
public:
    // Parses the image's object table and builds the objects. Their references are counted by
    // what the host then reads. `native_reader` returns InstanceObj::native for the data
    // native_writer wrote.
    SnapshotReader(VM& vm, std::span<const std::uint8_t> image,
                   std::function<void*(SnapshotReader&, InstanceObj&)> native_reader = {});
    SnapshotReader(const SnapshotReader&) = delete;
    SnapshotReader& operator=(const SnapshotReader&) = delete;

    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }
    void fail(std::string message) {
        if (error_.empty()) error_ = std::move(message);
    }

    std::uint8_t u8() { return take<std::uint8_t>(); }
    bool boolean() { return u8() != 0; }
    std::uint32_t u32() { return take<std::uint32_t>(); }
    std::uint64_t u64() { return take<std::uint64_t>(); }
    std::int64_t i64() { return take<std::int64_t>(); }
    float f32() { return take<float>(); }
    std::string str();
    void raw(void* out, std::size_t n);

    // The counterparts of SnapshotWriter::value/object. An owned reference is counted for the
    // caller's slot, which must not be released afterwards.
    Value value(bool owned = true);
    Object* object(bool owned = true);

    // Reads the VM's own state (the counterpart of SnapshotWriter::vm_state; call after the host's data).
    bool vm_state();

    // After the host read everything: restores the suspect buffer and the weak map, checks the
    // reference counts. False with error() set on a mismatch.
    bool finish();

private:
    friend struct SnapshotAccess;
    struct Cursor {
        const std::uint8_t* p = nullptr;
        const std::uint8_t* end = nullptr;
    };
    template <class T>
    T take() {
        T v{};
        raw(&v, sizeof(T));
        return v;
    }
    Object* resolve(std::uint32_t id, bool owned);
    const RecordRT* record_ref();
    const EnumRT* enum_ref();
    const Proto* proto_ref();
    void read_object(std::uint32_t id);
    void build_object(std::uint32_t id);

    VM& vm_;
    Cursor stream_, header_, body_;
    Cursor* cur_ = nullptr;
    std::function<void*(SnapshotReader&, InstanceObj&)> native_reader_;
    std::vector<Object*> objs_;
    std::vector<std::uint32_t> counted_, saved_rc_;
    std::vector<std::uint32_t> suspects_;
    std::string error_;
};

// Tears down the VM's state ahead of a restore: registers, globals, coroutines and arenas.
// Call after the host dropped its own references, then collect_all_cycles().
void snapshot_reset(VM& vm);

} // namespace pluma
