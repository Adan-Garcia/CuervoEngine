#pragma once

#include <map>
#include <memory>
#include <string>

#include "cuervo/audio/audio_system.hpp"
#include "cuervo/save/save_store.hpp"
#include "cuervo/save/settings.hpp"
#include "pluma/bindings.hpp"

// Natives of cuervo/script/prelude/save.plm (DESIGN.md 7.2 "Save data"). The
// state lives beside the VM, never in it, so saving cannot reach the
// simulation hash.
namespace cuervo::script {

class SaveContext {
public:
    // `store` null: the per-user directory of `game`, opened on first use (a
    // host that never touches Save never creates the directory).
    void configure(std::string game, std::shared_ptr<save::SaveStore> store) {
        game_ = std::move(game);
        store_ = std::move(store);
    }

    save::SaveStore& store();

    // In-memory slots (Save.load / set_* / commit) and the version each had on disk.
    struct Open {
        save::SlotData data;
        std::uint32_t version = 0;
    };
    std::map<std::string, Open, std::less<>> open;

    save::Settings settings;
    bool settings_loaded = false;
    // Window / input hooks from the runner or editor; may be null.
    save::SettingsTarget* target = nullptr;
    audio::AudioSystem* audio = nullptr; // may be null

    // Pushes `settings` to the audio system and `target`.
    int apply();

private:
    std::string game_ = "Game";
    std::shared_ptr<save::SaveStore> store_;
};

void bind_save(pluma::Bindings& bindings, SaveContext& context);

} // namespace cuervo::script
