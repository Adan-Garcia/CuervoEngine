#pragma once

// Hand-written loader for the handful of GL functions the engine uses. Only
// functions in both OpenGL 3.3 core and OpenGL ES 3.0 / WebGL2 belong here,
// so the same backend code serves desktop, web and legacy hardware.
//
// To add a function: add its typedef + pointer here and one line in
// GL_FUNCTIONS (gl_loader.cpp's load list uses the same macro).

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define CUERVO_GL_APIENTRY __stdcall
#else
#define CUERVO_GL_APIENTRY
#endif

namespace cuervo::gl {

using GLenum = unsigned int;
using GLbitfield = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLfloat = float;
using GLubyte = unsigned char;
using GLboolean = unsigned char;
using GLchar = char;
using GLintptr = std::ptrdiff_t;
using GLsizeiptr = std::ptrdiff_t;

inline constexpr GLenum GL_NO_ERROR = 0;
inline constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x00004000;
inline constexpr GLbitfield GL_DEPTH_BUFFER_BIT = 0x00000100;
inline constexpr GLenum GL_VENDOR = 0x1F00;
inline constexpr GLenum GL_RENDERER = 0x1F01;
inline constexpr GLenum GL_VERSION = 0x1F02;
inline constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
inline constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
inline constexpr GLenum GL_UNIFORM_BUFFER = 0x8A11;
inline constexpr GLenum GL_STATIC_DRAW = 0x88E4;
inline constexpr GLenum GL_DYNAMIC_DRAW = 0x88E8;
inline constexpr GLenum GL_STREAM_DRAW = 0x88E0;
inline constexpr GLboolean GL_FALSE = 0;
inline constexpr GLboolean GL_TRUE = 1;
inline constexpr GLuint GL_INVALID_INDEX = 0xFFFFFFFFu;
inline constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
inline constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
inline constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
inline constexpr GLenum GL_LINK_STATUS = 0x8B82;
inline constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
inline constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
inline constexpr GLenum GL_UNSIGNED_SHORT = 0x1403;
inline constexpr GLenum GL_UNSIGNED_INT = 0x1405;
inline constexpr GLenum GL_FLOAT = 0x1406;
inline constexpr GLenum GL_RGBA = 0x1908;
inline constexpr GLenum GL_POINTS = 0x0000;
inline constexpr GLenum GL_LINES = 0x0001;
inline constexpr GLenum GL_LINE_STRIP = 0x0003;
inline constexpr GLenum GL_TRIANGLES = 0x0004;
inline constexpr GLenum GL_TRIANGLE_STRIP = 0x0005;
inline constexpr GLenum GL_BLEND = 0x0BE2;
inline constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
inline constexpr GLenum GL_ZERO = 0;
inline constexpr GLenum GL_ONE = 1;
inline constexpr GLenum GL_SRC_ALPHA = 0x0302;
inline constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
inline constexpr GLenum GL_FUNC_ADD = 0x8006;
inline constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05;
inline constexpr GLenum GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT = 0x8A34;
inline constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
inline constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8;
inline constexpr GLenum GL_DRAW_FRAMEBUFFER = 0x8CA9;
inline constexpr GLenum GL_RENDERBUFFER = 0x8D41;
inline constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
inline constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
inline constexpr GLenum GL_RGBA8 = 0x8058;
inline constexpr GLenum GL_RGB8 = 0x8051;
inline constexpr GLenum GL_MAX_SAMPLES = 0x8D57;
inline constexpr GLenum GL_NEAREST = 0x2600;
inline constexpr GLenum GL_BACK = 0x0405;
inline constexpr GLenum GL_BACK_LEFT = 0x0402;
inline constexpr GLenum GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE = 0x8215;
inline constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
inline constexpr GLenum GL_TEXTURE0 = 0x84C0;
inline constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
inline constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
inline constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
inline constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
inline constexpr GLenum GL_TEXTURE_MAX_LEVEL = 0x813D;
inline constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
inline constexpr GLenum GL_LINEAR = 0x2601;
inline constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
inline constexpr GLenum GL_MAX_TEXTURE_SIZE = 0x0D33;
inline constexpr GLenum GL_R8 = 0x8229;
inline constexpr GLenum GL_RED = 0x1903;
inline constexpr GLenum GL_MAX_UNIFORM_BLOCK_SIZE = 0x8A30;
inline constexpr GLenum GL_MAX_VERTEX_ATTRIBS = 0x8869;
// Desktop GL only (always on in GLES): never enable it on an ES context.
inline constexpr GLenum GL_PROGRAM_POINT_SIZE = 0x8642;
inline constexpr GLenum GL_FRAMEBUFFER_SRGB = 0x8DB9;

// X(return type, name, parameter list)
#define CUERVO_GL_FUNCTIONS(X)                                                        \
    X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))              \
    X(void, glClear, (GLbitfield mask))                                              \
    X(void, glViewport, (GLint x, GLint y, GLsizei width, GLsizei height))           \
    X(const GLubyte*, glGetString, (GLenum name))                                    \
    X(GLenum, glGetError, ())                                                        \
    X(void, glGenBuffers, (GLsizei n, GLuint* buffers))                              \
    X(void, glDeleteBuffers, (GLsizei n, const GLuint* buffers))                     \
    X(void, glBindBuffer, (GLenum target, GLuint buffer))                            \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage)) \
    X(void, glBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void* data)) \
    X(void, glBindBufferRange, (GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size)) \
    X(void, glBindBufferBase, (GLenum target, GLuint index, GLuint buffer))         \
    X(void, glEnable, (GLenum cap))                                                  \
    X(void, glDisable, (GLenum cap))                                                 \
    X(void, glScissor, (GLint x, GLint y, GLsizei width, GLsizei height))            \
    X(void, glBlendFuncSeparate, (GLenum src_rgb, GLenum dst_rgb, GLenum src_a, GLenum dst_a)) \
    X(void, glBlendEquation, (GLenum mode))                                          \
    X(void, glGetIntegerv, (GLenum pname, GLint* data))                              \
    X(void, glPixelStorei, (GLenum pname, GLint param))                              \
    X(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels)) \
    X(GLuint, glCreateShader, (GLenum type))                                         \
    X(void, glDeleteShader, (GLuint shader))                                         \
    X(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths)) \
    X(void, glCompileShader, (GLuint shader))                                        \
    X(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint* params))             \
    X(void, glGetShaderInfoLog, (GLuint shader, GLsizei size, GLsizei* length, GLchar* log)) \
    X(GLuint, glCreateProgram, ())                                                   \
    X(void, glDeleteProgram, (GLuint program))                                       \
    X(void, glAttachShader, (GLuint program, GLuint shader))                         \
    X(void, glDetachShader, (GLuint program, GLuint shader))                         \
    X(void, glLinkProgram, (GLuint program))                                         \
    X(void, glGetProgramiv, (GLuint program, GLenum pname, GLint* params))           \
    X(void, glGetProgramInfoLog, (GLuint program, GLsizei size, GLsizei* length, GLchar* log)) \
    X(void, glUseProgram, (GLuint program))                                          \
    X(GLuint, glGetUniformBlockIndex, (GLuint program, const GLchar* name))          \
    X(void, glUniformBlockBinding, (GLuint program, GLuint block, GLuint binding))   \
    X(GLint, glGetUniformLocation, (GLuint program, const GLchar* name))             \
    X(void, glUniform1i, (GLint location, GLint value))                              \
    X(void, glGenTextures, (GLsizei n, GLuint* textures))                            \
    X(void, glDeleteTextures, (GLsizei n, const GLuint* textures))                   \
    X(void, glBindTexture, (GLenum target, GLuint texture))                          \
    X(void, glActiveTexture, (GLenum unit))                                          \
    X(void, glTexParameteri, (GLenum target, GLenum pname, GLint param))             \
    X(void, glTexImage2D, (GLenum target, GLint level, GLint internal_format, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void* pixels)) \
    X(void, glTexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* pixels)) \
    X(void, glGenVertexArrays, (GLsizei n, GLuint* arrays))                          \
    X(void, glDeleteVertexArrays, (GLsizei n, const GLuint* arrays))                 \
    X(void, glBindVertexArray, (GLuint array))                                       \
    X(void, glEnableVertexAttribArray, (GLuint index))                               \
    X(void, glDisableVertexAttribArray, (GLuint index))                              \
    X(void, glVertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer)) \
    X(void, glVertexAttribIPointer, (GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer)) \
    X(void, glVertexAttribDivisor, (GLuint index, GLuint divisor))                   \
    X(void, glDrawArraysInstanced, (GLenum mode, GLint first, GLsizei count, GLsizei instances)) \
    X(void, glDrawElementsInstanced, (GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances)) \
    X(void, glGenFramebuffers, (GLsizei n, GLuint* framebuffers))                    \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint* framebuffers))           \
    X(void, glBindFramebuffer, (GLenum target, GLuint framebuffer))                  \
    X(GLenum, glCheckFramebufferStatus, (GLenum target))                             \
    X(void, glFramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum rb_target, GLuint renderbuffer)) \
    X(void, glGetFramebufferAttachmentParameteriv, (GLenum target, GLenum attachment, GLenum pname, GLint* params)) \
    X(void, glGenRenderbuffers, (GLsizei n, GLuint* renderbuffers))                  \
    X(void, glDeleteRenderbuffers, (GLsizei n, const GLuint* renderbuffers))         \
    X(void, glBindRenderbuffer, (GLenum target, GLuint renderbuffer))                \
    X(void, glRenderbufferStorageMultisample, (GLenum target, GLsizei samples, GLenum format, GLsizei width, GLsizei height)) \
    X(void, glBlitFramebuffer, (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter))

struct Functions {
#define CUERVO_GL_DECLARE(ret, name, params) ret(CUERVO_GL_APIENTRY* name) params = nullptr;
    CUERVO_GL_FUNCTIONS(CUERVO_GL_DECLARE)
#undef CUERVO_GL_DECLARE
};

// Resolve every function with SDL_GL_GetProcAddress. A GL context must be
// current. Returns false (and logs the missing name) if any is unavailable.
bool load_functions(Functions& out);

} // namespace cuervo::gl
