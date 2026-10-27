#pragma once

namespace pluma {
class Bindings;
}

namespace cuervo::script {

class ScriptHost;

// Registers the native Box2D-backed physics functions (DESIGN.md 4.17, milestone 9.6)
// used by cuervo/script/prelude/physics/*.plm.
void bind_physics(pluma::Bindings& bindings, ScriptHost& host);

} // namespace cuervo::script
