#pragma once

#include "cuervo/audio/audio_system.hpp"
#include "pluma/bindings.hpp"

// Natives of cuervo/script/prelude/audio.plm (DESIGN.md 4.9). Kept apart from
// script_host.cpp so the audio API grows without touching the host. `audio`
// must outlive the bindings' sessions.
namespace cuervo::script {

void bind_audio(pluma::Bindings& bindings, audio::AudioSystem& audio);

} // namespace cuervo::script
