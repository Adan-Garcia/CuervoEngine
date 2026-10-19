#include "cuervo/rhi/gl/gl_device.hpp"

#include <SDL3/SDL_video.h>

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "cuervo/core/log.hpp"
#include "cuervo/platform/window.hpp"
#include "cuervo/rhi/gl/gl_loader.hpp"
#include "cuervo/rhi/rhi_internal.hpp"

namespace cuervo::gl {

namespace {

struct BufferInfo {
    GLuint name = 0;
    GLenum target = 0;
    rhi::BufferUsage usage = rhi::BufferUsage::Static;
    std::size_t size = 0;
    std::uint64_t written_frame = 0; // Stream: frame of the last orphaning update
};

struct StreamInfo {
    std::uint32_t stride = 0;
    rhi::VertexRate rate = rhi::VertexRate::PerVertex;
    std::vector<rhi::VertexAttribute> attributes;
};

struct PipelineInfo {
    GLuint program = 0;
    std::vector<StreamInfo> streams;
    rhi::BlendMode blend = rhi::BlendMode::None;
    GLenum topology = GL_TRIANGLES;
};

struct VertexBinding {
    std::uint32_t buffer = 0;
    std::size_t offset = 0;
};

struct TextureInfo {
    GLuint name = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    rhi::TextureFormat format = rhi::TextureFormat::RGBA8;
};

struct GlTextureFormat {
    GLint internal_format;
    GLenum format;
};

// Sized internal formats + matching client formats: valid on GL 3.3 and GLES 3.0.
GlTextureFormat to_gl_texture_format(rhi::TextureFormat format) {
    switch (format) {
        case rhi::TextureFormat::RGBA8: return {static_cast<GLint>(GL_RGBA8), GL_RGBA};
        case rhi::TextureFormat::R8:    return {static_cast<GLint>(GL_R8), GL_RED};
    }
    return {static_cast<GLint>(GL_RGBA8), GL_RGBA};
}

// Texture unit N serves sampler binding N; uploads use the unit after them so
// they never disturb what draws have bound.
constexpr GLenum upload_texture_unit = GL_TEXTURE0 + rhi::max_texture_bindings;

GLenum to_gl_target(rhi::BufferKind kind) {
    switch (kind) {
        case rhi::BufferKind::Vertex:  return GL_ARRAY_BUFFER;
        case rhi::BufferKind::Index:   return GL_ELEMENT_ARRAY_BUFFER;
        case rhi::BufferKind::Uniform: return GL_UNIFORM_BUFFER;
    }
    return GL_ARRAY_BUFFER;
}

GLenum to_gl_usage(rhi::BufferUsage usage) {
    switch (usage) {
        case rhi::BufferUsage::Static:  return GL_STATIC_DRAW;
        case rhi::BufferUsage::Dynamic: return GL_DYNAMIC_DRAW;
        case rhi::BufferUsage::Stream:  return GL_STREAM_DRAW;
    }
    return GL_STATIC_DRAW;
}

GLenum to_gl_topology(rhi::Topology topology) {
    switch (topology) {
        case rhi::Topology::TriangleList:  return GL_TRIANGLES;
        case rhi::Topology::TriangleStrip: return GL_TRIANGLE_STRIP;
        case rhi::Topology::LineList:      return GL_LINES;
        case rhi::Topology::LineStrip:     return GL_LINE_STRIP;
        case rhi::Topology::PointList:     return GL_POINTS;
    }
    return GL_TRIANGLES;
}

struct GlFormat {
    GLint components;
    GLenum type;
    bool normalized;
    bool integer;
};

GlFormat to_gl_format(rhi::VertexFormat format) {
    switch (format) {
        case rhi::VertexFormat::Float1:     return {1, GL_FLOAT, false, false};
        case rhi::VertexFormat::Float2:     return {2, GL_FLOAT, false, false};
        case rhi::VertexFormat::Float3:     return {3, GL_FLOAT, false, false};
        case rhi::VertexFormat::Float4:     return {4, GL_FLOAT, false, false};
        case rhi::VertexFormat::UByte4Norm: return {4, GL_UNSIGNED_BYTE, true, false};
        case rhi::VertexFormat::UInt1:      return {1, GL_UNSIGNED_INT, false, true};
    }
    return {4, GL_FLOAT, false, false};
}

std::string gl_string(const Functions& f, GLenum name) {
    const GLubyte* text = f.glGetString(name);
    return text ? reinterpret_cast<const char*>(text) : "unknown";
}

const void* as_pointer(std::size_t offset) { return reinterpret_cast<const void*>(offset); }

class GlDevice final : public rhi::Device {
public:
    GlDevice(SDL_Window* window, SDL_GLContext context, const Functions& functions, const rhi::DeviceOptions& options,
             bool requested_es)
        : window_(window), context_(context), gl_(functions) {
        version_ = gl_string(gl_, GL_VERSION);
        renderer_ = gl_string(gl_, GL_RENDERER);
        // Desktop contexts report "4.6.0 NVIDIA ..."; ES ones "OpenGL ES 3.0 ...";
        // Emscripten reports WebGL2 as "OpenGL ES 3.0 (WebGL 2.0 ...)". The
        // requested profile decides too, in case a driver words it differently.
        es_ = requested_es || version_.starts_with("OpenGL ES") || version_.starts_with("WebGL");
        info_.backend = rhi::Backend::OpenGL;
        info_.max_tier = rhi::Tier::Compatible; // GLES 3.0 / WebGL2 subset only
        info_.api_version = version_;
        info_.renderer = renderer_;
        info_.gles = es_;
        auto get = [&](GLenum name, GLint fallback) {
            GLint value = fallback;
            gl_.glGetIntegerv(name, &value);
            return static_cast<std::uint32_t>(std::max(value, 1));
        };
        rhi::Limits& limits = info_.limits;
        limits.uniform_offset_alignment = get(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, 256);
        limits.max_texture_size = get(GL_MAX_TEXTURE_SIZE, 2048);
        limits.max_uniform_block_size = get(GL_MAX_UNIFORM_BLOCK_SIZE, 16384);
        limits.max_vertex_attributes = std::min<std::uint32_t>(get(GL_MAX_VERTEX_ATTRIBS, 16), rhi::max_vertex_attributes);
        limits.max_msaa_samples = get(GL_MAX_SAMPLES, 1);
        // Everything Compatible is core GLES 3.0; nothing Full exists in GL.
        using rhi::Feature;
        for (Feature f : {Feature::InstancedDraws, Feature::IndexUInt32, Feature::UniformBuffers, Feature::Textures,
                          Feature::IntegerAttributes, Feature::PointsAndLines, Feature::FrameCapture}) {
            info_.hardware_features.set(f);
        }
        info_.hardware_features.set(Feature::Msaa, limits.max_msaa_samples >= 2);
        rhi::detail::apply_tier_gate(info_, options.tier);
        if (!es_) {
            // Desktop core profiles ignore gl_PointSize unless asked; GLES
            // (and Vulkan) always honor it. Not a GLES enum, so desktop only.
            gl_.glEnable(GL_PROGRAM_POINT_SIZE);
        }

        // Core profiles have no default vertex array object. One VAO stays bound
        // for the device's lifetime; attribute state is re-specified per draw.
        gl_.glGenVertexArrays(1, &vao_);
        gl_.glBindVertexArray(vao_);

        // MSAA: an offscreen multisampled renderbuffer, resolved into the
        // default framebuffer with glBlitFramebuffer at end_frame (all GLES 3.0
        // / WebGL2). Explicit instead of a multisampled default framebuffer so
        // the sample count is ours to pick on every platform, like Vulkan's.
        samples_ = rhi::detail::msaa_sample_count(options.msaa_samples, info_.limits.max_msaa_samples);
        // GLES requires the multisampled source of a blit to match the
        // destination's format, so mirror whether the window has alpha.
        GLint alpha_bits = 8;
        gl_.glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, es_ ? GL_BACK : GL_BACK_LEFT,
                                                  GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE, &alpha_bits);
        while (gl_.glGetError() != GL_NO_ERROR) {
        }
        msaa_format_ = alpha_bits > 0 ? GL_RGBA8 : GL_RGB8;
        info_.msaa_samples = samples_;

        gl_.glDisable(GL_FRAMEBUFFER_SRGB);
        while (gl_.glGetError() != GL_NO_ERROR) {
        }
    }

    ~GlDevice() override {
        destroy_msaa_target();
        for (auto& [id, buffer] : buffers_) {
            gl_.glDeleteBuffers(1, &buffer.name);
        }
        for (auto& [id, pipeline] : pipelines_) {
            gl_.glDeleteProgram(pipeline.program);
        }
        for (auto& [id, texture] : textures_) {
            gl_.glDeleteTextures(1, &texture.name);
        }
        gl_.glBindVertexArray(0);
        gl_.glDeleteVertexArrays(1, &vao_);
        SDL_GL_DestroyContext(context_);
    }

    const rhi::DeviceInfo& info() const override { return info_; }

    void begin_frame(ivec2 framebuffer_size) override {
        ++frame_;
        framebuffer_size_ = framebuffer_size;
        ensure_msaa_target();
        gl_.glBindFramebuffer(GL_FRAMEBUFFER, msaa_fbo_);
        const rhi::Rect full{0, 0, framebuffer_size.x(), framebuffer_size.y()};
        set_viewport(full);
        gl_.glEnable(GL_SCISSOR_TEST); // so clear() honors the scissor like Vulkan's
        set_scissor(full);
        current_pipeline_ = 0;
        vertex_bindings_ = {};
        index_buffer_ = 0;
        gl_.glUseProgram(0);
        // Uniform ranges are context state that would otherwise leak into the
        // next frame; Vulkan starts every frame with none bound.
        for (std::uint32_t binding = 0; binding < rhi::max_uniform_bindings; ++binding) {
            if (uniform_bound_ & (1u << binding)) gl_.glBindBufferBase(GL_UNIFORM_BUFFER, binding, 0);
        }
        uniform_bound_ = 0;
        for (std::uint32_t unit = 0; unit < rhi::max_texture_bindings; ++unit) {
            if (bound_textures_[unit]) {
                gl_.glActiveTexture(GL_TEXTURE0 + unit);
                gl_.glBindTexture(GL_TEXTURE_2D, 0);
                bound_textures_[unit] = 0;
            }
        }
    }

    void clear(rhi::Color color) override {
        gl_.glClearColor(color.r, color.g, color.b, color.a);
        gl_.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    void end_frame() override {
        resolve_msaa();
        if (!capture_path_.empty()) {
            read_back(capture_path_);
            capture_path_.clear();
        }
#ifndef NDEBUG
        for (GLenum error = gl_.glGetError(); error != GL_NO_ERROR; error = gl_.glGetError()) {
            LOG_WARN("GL error 0x{:04X}", error);
        }
#endif
        SDL_GL_SwapWindow(window_);
    }

    ivec2 framebuffer_size() const override { return framebuffer_size_; }

    rhi::BufferHandle create_buffer(const rhi::BufferDesc& desc) override {
        BufferInfo buffer{0, to_gl_target(desc.kind), desc.usage, desc.size};
        gl_.glGenBuffers(1, &buffer.name);
        gl_.glBindBuffer(buffer.target, buffer.name);
        gl_.glBufferData(buffer.target, static_cast<GLsizeiptr>(desc.size), desc.initial_data,
                         to_gl_usage(desc.usage));
        gl_.glBindBuffer(buffer.target, 0);

        rhi::BufferHandle handle{next_id_++};
        buffers_.emplace(handle.id, buffer);
        return handle;
    }

    void update_buffer(rhi::BufferHandle handle, std::size_t offset, std::span<const std::byte> data) override {
        auto it = buffers_.find(handle.id);
        if (it == buffers_.end()) {
            LOG_ERROR("update_buffer: invalid buffer handle {}", handle.id);
            return;
        }
        BufferInfo& buffer = it->second;
        if (offset > buffer.size || data.size() > buffer.size - offset) {
            LOG_ERROR("update_buffer: {} bytes at offset {} overflows a {}-byte buffer", data.size(), offset,
                      buffer.size);
            return;
        }
        gl_.glBindBuffer(buffer.target, buffer.name);
        if (buffer.usage == rhi::BufferUsage::Stream && buffer.written_frame != frame_) {
            // Orphan on the frame's first update: the driver hands us fresh
            // storage instead of stalling on draws that still read the old
            // contents. Bytes not written this frame become undefined, which is
            // exactly the Stream contract (and what the Vulkan backend's rename does).
            gl_.glBufferData(buffer.target, static_cast<GLsizeiptr>(buffer.size), nullptr, GL_STREAM_DRAW);
            buffer.written_frame = frame_;
        }
        gl_.glBufferSubData(buffer.target, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(data.size()),
                            data.data());
        gl_.glBindBuffer(buffer.target, 0);
    }

    void destroy_buffer(rhi::BufferHandle handle) override {
        // GL keeps the storage alive until queued commands that use it finish.
        if (auto it = buffers_.find(handle.id); it != buffers_.end()) {
            gl_.glDeleteBuffers(1, &it->second.name);
            buffers_.erase(it);
        }
    }

    rhi::TextureHandle create_texture(const rhi::TextureDesc& desc) override {
        if (!rhi::detail::texture_size_allowed(info_, desc)) {
            return {};
        }
        TextureInfo texture{0, desc.width, desc.height, desc.format};
        const GlTextureFormat format = to_gl_texture_format(desc.format);
        const GLint filter = static_cast<GLint>(desc.filter == rhi::Filter::Nearest ? GL_NEAREST : GL_LINEAR);
        gl_.glActiveTexture(upload_texture_unit);
        gl_.glGenTextures(1, &texture.name);
        gl_.glBindTexture(GL_TEXTURE_2D, texture.name);
        gl_.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        gl_.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        gl_.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL_CLAMP_TO_EDGE));
        gl_.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL_CLAMP_TO_EDGE));
        gl_.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0); // one level: complete without mips
        gl_.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        // GL leaves storage without data undefined; the RHI promises zeros.
        std::vector<std::byte> zeros;
        const void* pixels = desc.initial_data;
        if (!pixels) {
            zeros.resize(static_cast<std::size_t>(desc.width) * static_cast<std::size_t>(desc.height) *
                         rhi::texture_format_size(desc.format));
            pixels = zeros.data();
        }
        gl_.glTexImage2D(GL_TEXTURE_2D, 0, format.internal_format, desc.width, desc.height, 0, format.format,
                         GL_UNSIGNED_BYTE, pixels);
        gl_.glBindTexture(GL_TEXTURE_2D, 0);

        rhi::TextureHandle handle{next_id_++};
        textures_.emplace(handle.id, texture);
        return handle;
    }

    void update_texture(rhi::TextureHandle handle, rhi::Rect region, std::span<const std::byte> data) override {
        auto it = textures_.find(handle.id);
        if (it == textures_.end()) {
            LOG_ERROR("update_texture: invalid texture handle {}", handle.id);
            return;
        }
        const TextureInfo& texture = it->second;
        if (!rhi::detail::valid_texture_update(texture.width, texture.height, texture.format, region, data.size())) {
            return;
        }
        if (region.width == 0 || region.height == 0) {
            return;
        }
        const GlTextureFormat format = to_gl_texture_format(texture.format);
        gl_.glActiveTexture(upload_texture_unit);
        gl_.glBindTexture(GL_TEXTURE_2D, texture.name);
        gl_.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl_.glTexSubImage2D(GL_TEXTURE_2D, 0, region.x, region.y, region.width, region.height, format.format,
                            GL_UNSIGNED_BYTE, data.data());
        gl_.glBindTexture(GL_TEXTURE_2D, 0);
    }

    void destroy_texture(rhi::TextureHandle handle) override {
        // Like buffers, GL keeps the storage alive until queued commands finish.
        if (auto it = textures_.find(handle.id); it != textures_.end()) {
            for (std::uint32_t& bound : bound_textures_) {
                if (bound == handle.id) {
                    bound = 0;
                }
            }
            gl_.glDeleteTextures(1, &it->second.name);
            textures_.erase(it);
        }
    }

    rhi::PipelineHandle create_pipeline(const rhi::PipelineDesc& desc) override {
        if (desc.streams.size() > rhi::max_vertex_streams) {
            LOG_ERROR("Pipeline '{}': {} vertex streams (max {})", desc.debug_name, desc.streams.size(),
                      rhi::max_vertex_streams);
            return {};
        }
        GLuint vs = compile(GL_VERTEX_SHADER, desc.vertex, desc.debug_name);
        GLuint fs = compile(GL_FRAGMENT_SHADER, desc.fragment, desc.debug_name);
        if (!vs || !fs) {
            gl_.glDeleteShader(vs);
            gl_.glDeleteShader(fs);
            return {};
        }
        GLuint program = gl_.glCreateProgram();
        gl_.glAttachShader(program, vs);
        gl_.glAttachShader(program, fs);
        gl_.glLinkProgram(program);
        gl_.glDetachShader(program, vs);
        gl_.glDetachShader(program, fs);
        gl_.glDeleteShader(vs);
        gl_.glDeleteShader(fs);

        GLint linked = 0;
        gl_.glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            GLint length = 0;
            gl_.glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
            std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
            gl_.glGetProgramInfoLog(program, length, nullptr, log.data());
            LOG_ERROR("Pipeline '{}': link failed: {}", desc.debug_name, log);
            gl_.glDeleteProgram(program);
            return {};
        }

        // GLSL 330 / ES 300 can't say `layout(binding = N)` on uniform blocks;
        // apply the bindings the shader tool reflected from the SPIR-V.
        for (const rhi::ShaderSource* stage : {&desc.vertex, &desc.fragment}) {
            for (const rhi::UniformBlockBinding& block : stage->uniform_blocks) {
                const std::string name(block.name);
                GLuint index = gl_.glGetUniformBlockIndex(program, name.c_str());
                if (index != GL_INVALID_INDEX) {
                    gl_.glUniformBlockBinding(program, index, block.binding);
                }
            }
        }
        // Likewise for samplers: sampler binding N reads texture unit N.
        bool has_textures = false;
        for (const rhi::ShaderSource* stage : {&desc.vertex, &desc.fragment}) {
            for (const rhi::TextureBinding& texture : stage->textures) {
                if (!has_textures) {
                    gl_.glUseProgram(program);
                    has_textures = true;
                }
                const std::string name(texture.name);
                if (const GLint location = gl_.glGetUniformLocation(program, name.c_str()); location >= 0) {
                    gl_.glUniform1i(location, static_cast<GLint>(texture.binding));
                }
            }
        }
        if (has_textures) {
            auto current = pipelines_.find(current_pipeline_);
            gl_.glUseProgram(current != pipelines_.end() ? current->second.program : 0);
        }

        PipelineInfo pipeline;
        pipeline.program = program;
        pipeline.blend = desc.blend;
        pipeline.topology = to_gl_topology(desc.topology);
        for (const rhi::VertexStream& stream : desc.streams) {
            pipeline.streams.push_back({stream.stride, stream.rate, {stream.attributes.begin(), stream.attributes.end()}});
        }
        rhi::PipelineHandle handle{next_id_++};
        pipelines_.emplace(handle.id, std::move(pipeline));
        return handle;
    }

    void destroy_pipeline(rhi::PipelineHandle handle) override {
        if (auto it = pipelines_.find(handle.id); it != pipelines_.end()) {
            if (current_pipeline_ == handle.id) {
                gl_.glUseProgram(0);
                current_pipeline_ = 0;
            }
            gl_.glDeleteProgram(it->second.program);
            pipelines_.erase(it);
        }
    }

    // GL's window origin is bottom-left; the RHI's is top-left.
    void set_viewport(rhi::Rect r) override {
        gl_.glViewport(r.x, framebuffer_size_.y() - r.y - r.height, r.width, r.height);
    }

    void set_scissor(rhi::Rect r) override {
        gl_.glScissor(r.x, framebuffer_size_.y() - r.y - r.height, std::max(r.width, 0), std::max(r.height, 0));
    }

    void bind_pipeline(rhi::PipelineHandle handle) override {
        auto it = pipelines_.find(handle.id);
        if (it == pipelines_.end()) {
            LOG_ERROR("bind_pipeline: invalid pipeline handle {}", handle.id);
            return;
        }
        if (current_pipeline_ == handle.id) {
            return;
        }
        current_pipeline_ = handle.id;
        const PipelineInfo& pipeline = it->second;
        gl_.glUseProgram(pipeline.program);
        switch (pipeline.blend) {
            case rhi::BlendMode::None:
                gl_.glDisable(GL_BLEND);
                break;
            case rhi::BlendMode::Alpha:
                gl_.glEnable(GL_BLEND);
                gl_.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                break;
            case rhi::BlendMode::Premultiplied:
                gl_.glEnable(GL_BLEND);
                gl_.glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                break;
            case rhi::BlendMode::Additive:
                gl_.glEnable(GL_BLEND);
                gl_.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE);
                break;
        }
        gl_.glBlendEquation(GL_FUNC_ADD);
    }

    void bind_vertex_buffer(std::uint32_t stream, rhi::BufferHandle buffer, std::size_t offset) override {
        if (stream >= rhi::max_vertex_streams) {
            LOG_ERROR("bind_vertex_buffer: stream {} out of range", stream);
            return;
        }
        vertex_bindings_[stream] = {buffer.id, offset};
    }

    void bind_index_buffer(rhi::BufferHandle buffer, rhi::IndexFormat format) override {
        index_buffer_ = buffer.id;
        index_format_ = format;
    }

    void bind_uniform_buffer(std::uint32_t binding, rhi::BufferHandle handle, std::size_t offset,
                             std::size_t size) override {
        auto it = buffers_.find(handle.id);
        if (it == buffers_.end() || binding >= rhi::max_uniform_bindings) {
            LOG_ERROR("bind_uniform_buffer: invalid buffer {} or binding {}", handle.id, binding);
            return;
        }
        const BufferInfo& buffer = it->second;
        const std::size_t range = size ? size : buffer.size - std::min(offset, buffer.size);
        gl_.glBindBufferRange(GL_UNIFORM_BUFFER, binding, buffer.name, static_cast<GLintptr>(offset),
                              static_cast<GLsizeiptr>(range));
        uniform_bound_ |= 1u << binding;
    }

    void bind_texture(std::uint32_t binding, rhi::TextureHandle handle) override {
        auto it = textures_.find(handle.id);
        if (it == textures_.end() || binding >= rhi::max_texture_bindings) {
            LOG_ERROR("bind_texture: invalid texture {} or binding {}", handle.id, binding);
            return;
        }
        if (bound_textures_[binding] == handle.id) {
            return;
        }
        gl_.glActiveTexture(GL_TEXTURE0 + binding);
        gl_.glBindTexture(GL_TEXTURE_2D, it->second.name);
        bound_textures_[binding] = handle.id;
    }

    void draw(std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t first_vertex,
              std::uint32_t first_instance) override {
        const PipelineInfo* pipeline = prepare_draw(first_instance);
        if (!pipeline || vertex_count == 0 || instance_count == 0) {
            return;
        }
        gl_.glDrawArraysInstanced(pipeline->topology, static_cast<GLint>(first_vertex),
                                  static_cast<GLsizei>(vertex_count), static_cast<GLsizei>(instance_count));
    }

    void draw_indexed(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index,
                      std::uint32_t first_instance) override {
        const PipelineInfo* pipeline = prepare_draw(first_instance);
        if (!pipeline || index_count == 0 || instance_count == 0) {
            return;
        }
        auto it = buffers_.find(index_buffer_);
        if (it == buffers_.end()) {
            LOG_ERROR("draw_indexed: no index buffer bound");
            return;
        }
        // The element binding is VAO state that buffer uploads may have changed.
        gl_.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, it->second.name);
        const bool u16 = index_format_ == rhi::IndexFormat::UInt16;
        gl_.glDrawElementsInstanced(pipeline->topology, static_cast<GLsizei>(index_count),
                                    u16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                                    as_pointer(first_index * (u16 ? 2u : 4u)), static_cast<GLsizei>(instance_count));
    }

    void draw_indexed_base_vertex(std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t,
                                  std::uint32_t) override {
        // GLES 3.0 / WebGL2 have no base-vertex draws (Tier::Full); this
        // always fails, after logging once.
        rhi::detail::feature_allowed(info_, rhi::Feature::BaseVertexDraws, base_vertex_reported_);
    }

    bool capture_frame(std::string path) override {
        capture_path_ = std::move(path);
        return true;
    }

private:
    GLuint compile(GLenum type, const rhi::ShaderSource& source, std::string_view debug_name) {
        const std::string_view text = es_ ? source.glsl_es300 : source.glsl330;
        if (text.empty()) {
            LOG_ERROR("Pipeline '{}': shader has no {} source", debug_name, es_ ? "GLSL ES 300" : "GLSL 330");
            return 0;
        }
        GLuint shader = gl_.glCreateShader(type);
        const GLchar* strings[] = {text.data()};
        const GLint lengths[] = {static_cast<GLint>(text.size())};
        gl_.glShaderSource(shader, 1, strings, lengths);
        gl_.glCompileShader(shader);
        GLint compiled = 0;
        gl_.glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            GLint length = 0;
            gl_.glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
            std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
            gl_.glGetShaderInfoLog(shader, length, nullptr, log.data());
            LOG_ERROR("Pipeline '{}': {} shader failed to compile: {}", debug_name,
                      type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
            gl_.glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    // Points every attribute of the bound pipeline at its stream's buffer.
    // Instance streams are offset by first_instance (GLES 3.0 has no base instance).
    const PipelineInfo* prepare_draw(std::uint32_t first_instance) {
        auto it = pipelines_.find(current_pipeline_);
        if (it == pipelines_.end()) {
            LOG_ERROR("draw: no pipeline bound");
            return nullptr;
        }
        const PipelineInfo& pipeline = it->second;
        std::uint32_t enabled = 0;
        for (std::size_t s = 0; s < pipeline.streams.size(); ++s) {
            const StreamInfo& stream = pipeline.streams[s];
            auto buffer = buffers_.find(vertex_bindings_[s].buffer);
            if (buffer == buffers_.end()) {
                LOG_ERROR("draw: no vertex buffer bound to stream {}", s);
                return nullptr;
            }
            gl_.glBindBuffer(GL_ARRAY_BUFFER, buffer->second.name);
            const bool per_instance = stream.rate == rhi::VertexRate::PerInstance;
            std::size_t base = vertex_bindings_[s].offset;
            if (per_instance) {
                base += static_cast<std::size_t>(first_instance) * stream.stride;
            }
            for (const rhi::VertexAttribute& attribute : stream.attributes) {
                const GlFormat format = to_gl_format(attribute.format);
                const GLuint location = attribute.location;
                if (format.integer) {
                    gl_.glVertexAttribIPointer(location, format.components, format.type,
                                               static_cast<GLsizei>(stream.stride), as_pointer(base + attribute.offset));
                } else {
                    gl_.glVertexAttribPointer(location, format.components, format.type,
                                              format.normalized ? GL_TRUE : GL_FALSE,
                                              static_cast<GLsizei>(stream.stride), as_pointer(base + attribute.offset));
                }
                gl_.glVertexAttribDivisor(location, per_instance ? 1 : 0);
                enabled |= 1u << location;
            }
        }
        gl_.glBindBuffer(GL_ARRAY_BUFFER, 0);
        for (GLuint location = 0; location < rhi::max_vertex_attributes; ++location) {
            const std::uint32_t bit = 1u << location;
            if ((enabled & bit) && !(enabled_attributes_ & bit)) {
                gl_.glEnableVertexAttribArray(location);
            } else if (!(enabled & bit) && (enabled_attributes_ & bit)) {
                gl_.glDisableVertexAttribArray(location);
            }
        }
        enabled_attributes_ = enabled;
        return &pipeline;
    }

    void destroy_msaa_target() {
        if (msaa_fbo_) {
            gl_.glDeleteFramebuffers(1, &msaa_fbo_);
            gl_.glDeleteRenderbuffers(1, &msaa_color_);
        }
        msaa_fbo_ = msaa_color_ = 0;
        msaa_size_ = {0, 0};
    }

    void ensure_msaa_target() {
        if (samples_ <= 1 || framebuffer_size_ == msaa_size_ || framebuffer_size_.x() <= 0 || framebuffer_size_.y() <= 0) {
            return;
        }
        destroy_msaa_target();
        gl_.glGenRenderbuffers(1, &msaa_color_);
        gl_.glBindRenderbuffer(GL_RENDERBUFFER, msaa_color_);
        gl_.glRenderbufferStorageMultisample(GL_RENDERBUFFER, static_cast<GLsizei>(samples_), msaa_format_,
                                             framebuffer_size_.x(), framebuffer_size_.y());
        gl_.glBindRenderbuffer(GL_RENDERBUFFER, 0);
        gl_.glGenFramebuffers(1, &msaa_fbo_);
        gl_.glBindFramebuffer(GL_FRAMEBUFFER, msaa_fbo_);
        gl_.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msaa_color_);
        if (gl_.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOG_ERROR("GL: {}x MSAA framebuffer incomplete; anti-aliasing disabled", samples_);
            gl_.glBindFramebuffer(GL_FRAMEBUFFER, 0);
            destroy_msaa_target();
            samples_ = 1;
            info_.msaa_samples = 1;
            return;
        }
        gl_.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        msaa_size_ = framebuffer_size_;
        LOG_DEBUG("GL: {}x MSAA target {}x{}", samples_, msaa_size_.x(), msaa_size_.y());
    }

    // Resolves the multisampled frame into the default framebuffer.
    void resolve_msaa() {
        if (!msaa_fbo_) {
            return;
        }
        const GLint w = msaa_size_.x(), h = msaa_size_.y();
        gl_.glBindFramebuffer(GL_READ_FRAMEBUFFER, msaa_fbo_);
        gl_.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        gl_.glDisable(GL_SCISSOR_TEST); // blits are scissored too
        gl_.glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        gl_.glEnable(GL_SCISSOR_TEST);
        gl_.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void read_back(const std::string& path) {
        const int w = framebuffer_size_.x();
        const int h = framebuffer_size_.y();
        if (w <= 0 || h <= 0) {
            return;
        }
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
        gl_.glPixelStorei(GL_PACK_ALIGNMENT, 1);
        gl_.glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3);
        for (int y = 0; y < h; ++y) {
            const std::uint8_t* src = rgba.data() + static_cast<std::size_t>(h - 1 - y) * static_cast<std::size_t>(w) * 4;
            std::uint8_t* dst = rgb.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 3;
            for (int x = 0; x < w; ++x) {
                dst[x * 3 + 0] = src[x * 4 + 0];
                dst[x * 3 + 1] = src[x * 4 + 1];
                dst[x * 3 + 2] = src[x * 4 + 2];
            }
        }
        rhi::detail::write_ppm(path, w, h, rgb);
    }

    SDL_Window* window_;
    SDL_GLContext context_;
    Functions gl_;
    rhi::DeviceInfo info_;
    std::string version_;
    std::string renderer_;
    bool es_ = false;
    GLuint vao_ = 0;
    std::uint32_t samples_ = 1;
    GLenum msaa_format_ = GL_RGBA8;
    GLuint msaa_fbo_ = 0;
    GLuint msaa_color_ = 0;
    ivec2 msaa_size_{0, 0};
    ivec2 framebuffer_size_{0, 0};
    std::uint64_t frame_ = 1; // > every BufferInfo::written_frame default

    std::unordered_map<std::uint32_t, BufferInfo> buffers_;
    std::unordered_map<std::uint32_t, PipelineInfo> pipelines_;
    std::unordered_map<std::uint32_t, TextureInfo> textures_;
    std::uint32_t next_id_ = 1;
    std::uint32_t uniform_bound_ = 0; // bit per uniform binding with a range bound
    bool base_vertex_reported_ = false;
    std::array<std::uint32_t, rhi::max_texture_bindings> bound_textures_{};

    std::uint32_t current_pipeline_ = 0;
    std::array<VertexBinding, rhi::max_vertex_streams> vertex_bindings_{};
    std::uint32_t index_buffer_ = 0;
    rhi::IndexFormat index_format_ = rhi::IndexFormat::UInt16;
    std::uint32_t enabled_attributes_ = 0;
    std::string capture_path_;
};

} // namespace

std::unique_ptr<rhi::Device> create_device(Window& window, const rhi::DeviceOptions& options) {
    if (window.graphics() != GraphicsApi::OpenGL) {
        LOG_ERROR("The window was not created for OpenGL");
        return nullptr;
    }

    SDL_GLContext context = SDL_GL_CreateContext(window.native());
    if (!context) {
        LOG_ERROR("SDL_GL_CreateContext failed: {}", SDL_GetError());
        return nullptr;
    }
    if (!SDL_GL_SetSwapInterval(options.vsync ? 1 : 0)) {
        LOG_WARN("Could not set swap interval {}: {}", options.vsync ? 1 : 0, SDL_GetError());
    }

    Functions functions;
    if (!load_functions(functions)) {
        SDL_GL_DestroyContext(context);
        return nullptr;
    }

    auto device = std::make_unique<GlDevice>(window.native(), context, functions, options, window.gl_es());
    LOG_INFO("OpenGL device: {} ({}), {}x MSAA, {} tier", device->info().api_version, device->info().renderer,
             device->info().msaa_samples, rhi::tier_name(device->info().tier));
    return device;
}

} // namespace cuervo::gl
