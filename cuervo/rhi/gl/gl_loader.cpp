#include "cuervo/rhi/gl/gl_loader.hpp"

#include <SDL3/SDL_video.h>

#include "cuervo/core/log.hpp"

namespace cuervo::gl {

bool load_functions(Functions& out) {
    bool ok = true;
#define CUERVO_GL_LOAD(ret, name, params)                                          \
    out.name = reinterpret_cast<ret(CUERVO_GL_APIENTRY*) params>(SDL_GL_GetProcAddress(#name)); \
    if (!out.name) {                                                               \
        LOG_ERROR("Missing GL function: {}", #name);                               \
        ok = false;                                                                \
    }
    CUERVO_GL_FUNCTIONS(CUERVO_GL_LOAD)
#undef CUERVO_GL_LOAD
    return ok;
}

} // namespace cuervo::gl
