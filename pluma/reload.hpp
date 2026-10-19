#pragma once

// Hot reload (DESIGN.md 5.13): moves live state from the VM of the previous
// compile into the VM of a fresh compile of the edited sources.
//
// Values are rebuilt in the new VM by name: class and struct instances by
// type name, keeping fields whose name and type are unchanged (new fields get
// their initializers, removed fields are dropped, fields whose type changed
// are reset with a warning); enums by variant name; lists, strings and
// vectors element-wise; closures when a function with the same stable key and
// captures exists; host data (InstanceObj::native) moves to the new instance.
// Object identity and cycles are preserved. Running coroutines restart from
// the beginning with their original arguments; Task values follow them.
//
// Use: `Migrator m(old_vm, new_vm); m.begin(); roots = m.migrate(...);
// m.migrate_globals(); m.finish();`. Script code (field initializers,
// restarted coroutines) runs in the new VM during migration.

#include <map>
#include <string>
#include <vector>

#include "pluma/vm.hpp"

namespace pluma {

class Migrator {
public:
    Migrator(VM& from, VM& to) : from_(from), to_(to) {}

    // Creates the new VM's counterparts of the old VM's live coroutines
    // (not started yet), so Task values can be mapped.
    void begin();
    // The new VM's version of `v` (owned by the caller).
    Value migrate(const Value& v);
    // Globals with the same qualified name and type keep their values.
    void migrate_globals();
    // Gives the restarted coroutines their arguments and owners and runs
    // them to their first suspension.
    RunResult finish();

    const std::vector<std::string>& warnings() const { return warnings_; }
    std::size_t objects_migrated() const { return migrated_ != 0 ? migrated_ : memo_.size(); }
    std::size_t coroutines_restarted() const { return coros_.size(); }

private:
    Value migrate_instance(InstanceObj* o);
    Value migrate_enum(EnumObj* e);
    Value convert(const Value& v); // owned result
    void warn(std::string message);

    VM& from_;
    VM& to_;
    std::map<const Object*, Value> memo_; // old object -> new value (owned by the memo)
    std::vector<std::pair<CoroObj*, CoroObj*>> coros_;
    std::vector<std::string> warnings_;
    std::size_t migrated_ = 0;
};

} // namespace pluma
