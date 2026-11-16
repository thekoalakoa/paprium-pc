/* Copyright (C) 2026 Marsupial Solutions LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See pc_filter.h for the looks and what each renderer does with them.
 *
 * HOW A SHADER LOOK REACHES THE GPU. SDL's OpenGL renderer draws with an OpenGL context of its own, and SDL_RenderFlush makes it finish what it has queued, so a program may then draw in
 * the same context itself. pc_window.c clears the window through SDL, flushes, and calls pc_filter_gl_draw, which:
 *   - copies the game's picture into a texture of its own, one texel a game pixel, sampled nearest, black outside, as RetroArch hands a shader its input;
 *   - draws one rectangle over the picture's place with the look's shader program, with the inputs RetroArch's GLSL shaders read: MVPMatrix, FrameCount, FrameDirection, OutputSize,
 *     TextureSize and InputSize, the vertex attributes VertexCoord, TexCoord and COLOR, and each "#pragma parameter" of the shader set to the look's value;
 *   - puts back every piece of OpenGL state it changed, as SDL's renderer keeps its own copy of that state and would otherwise draw the next thing wrongly.
 * Each shader is compiled the way RetroArch does: once with VERTEX defined and once with FRAGMENT, PARAMETER_UNIFORM defined so that parameters are uniforms, after the file's own
 * "#version" line, or after "#version 130" when the file has none.
 *
 * A PLAYER'S PRESET. librashader.dll is loaded from beside the program by its full path, never from the search path, only when the configuration names a preset. Its OpenGL runtime reads
 * the picture texture and draws into a texture of the picture's size on the screen, which is then copied into place. It is asked to keep no cache of compiled shaders, so it writes no
 * files. Its calls change much of OpenGL's state, sampler objects included; all of it is put back before SDL draws again.
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_opengl.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_config.h"
#include "pc_filter.h"

/* The shaders folder's files, made into C text by pc_build.py at build time. */
extern const char *const pc_shader_files[];
extern const char *const pc_shader_texts[];
extern const int pc_shader_count;

/* ---- the looks */

typedef struct Look {
    const char *name, *title, *about;
    const char *shader;         /* a file of the shaders folder, or NULL for off and sharp */
    const char *settings;       /* "NAME=value NAME=value": the shader's parameters for this look. Any the shader has and this leaves out keep the shader's own default */
    const char *settings_large; /* applied after settings when the picture is 1440 screen lines tall or more, where a finer mask would be too small to see */
} Look;

static const Look looks[PC_LOOKS] = {
    {"off", "Off", "the picture as before: sharp pixels, a whole number of screen lines for each game line", NULL, NULL, NULL},
    {"sharp", "Sharp pixels", "sharp pixels that fill the window's height, with at most one soft screen pixel at each edge", NULL, NULL, NULL},
    {"scanlines", "Scanlines", "the dark lines between the picture's lines on a flat RGB monitor, no colour mask (crt-hyllian, by Hyllian)", "crt-hyllian.glsl",
     "BEAM_PROFILE=0 BEAM_MIN_WIDTH=0.86 BEAM_MAX_WIDTH=1.0 SCANLINES_STRENGTH=0.58 COLOR_BOOST=1.25 HFILTER_SHARPNESS=1.0 PHOSPHOR_LAYOUT=0 MASK_INTENSITY=0.0 "
     "CRT_ANTI_RINGING=1.0 InputGamma=2.4 OutputGamma=2.2 VSCANLINES=0.0", NULL},
    {"crt", "Consumer TV", "a home television: soft beam, a little glow, a slot mask (crt-lottes, by Timothy Lottes)", "crt-lottes.glsl",
     "hardScan=-8.0 hardPix=-3.0 warpX=0.0 warpY=0.0 maskDark=0.5 maskLight=1.5 scaleInLinearGamma=1.0 shadowMask=1.0 brightBoost=1.0 hardBloomPix=-1.5 hardBloomScan=-2.0 "
     "bloomAmount=0.15 shape=2.0", NULL},
    {"pvm", "PVM / BVM monitor", "a Sony PVM or BVM broadcast monitor: sharp, clear dark scanlines, an aperture grille (crt-hyllian, by Hyllian)", "crt-hyllian.glsl",
     "BEAM_PROFILE=0 BEAM_MIN_WIDTH=0.72 BEAM_MAX_WIDTH=1.0 SCANLINES_STRENGTH=0.72 COLOR_BOOST=1.40 HFILTER_SHARPNESS=1.0 PHOSPHOR_LAYOUT=4 MASK_INTENSITY=0.6 "
     "CRT_ANTI_RINGING=1.0 InputGamma=2.4 OutputGamma=2.2 VSCANLINES=0.0", "PHOSPHOR_LAYOUT=9"},
    {"curved", "Curved TV", "an older television with curved glass, rounded corners and a fine dot mask (crt-geom, by cgwg, Themaister and DOLLS)", "crt-geom.glsl",
     "CRTgamma=2.4 INV=0.0 monitorgamma=2.2 d=1.6 CURVATURE=1.0 R=2.0 cornersize=0.03 cornersmooth=1000.0 x_tilt=0.0 y_tilt=0.0 overscan_x=100.0 overscan_y=100.0 DOTMASK=0.3 "
     "SHARPER=1.0 scanline_weight=0.3 lum=0.0 interlace_detect=1.0 SATURATION=1.0", NULL},
};

#define LARGE_LINES 1440

const char *pc_filter_name(int look) { return look >= 0 && look < PC_LOOKS ? looks[look].name : look == PC_LOOK_PRESET ? "preset" : ""; }
const char *pc_filter_title(int look) { return look >= 0 && look < PC_LOOKS ? looks[look].title : look == PC_LOOK_PRESET ? "Your preset" : ""; }
const char *pc_filter_about(int look) { return look >= 0 && look < PC_LOOKS ? looks[look].about : look == PC_LOOK_PRESET ? "a player's own RetroArch preset, through librashader" : ""; }
int pc_filter_needs_gl(int look) { return look == PC_LOOK_PRESET || (look >= 0 && look < PC_LOOKS && looks[look].shader != NULL); }

int pc_filter_find(const char *name)
{
    int i;
    for (i = 0; i < PC_LOOKS; i++)
        if (!strcmp(looks[i].name, name))
            return i;
    return -1;
}

/* ---- OpenGL, through the renderer's context. Nothing here links OpenGL: SDL has loaded it, and every function comes from SDL_GL_GetProcAddress. */

#define GL_FUNCTIONS(X) \
    X(const GLubyte *, GetString, (GLenum)) \
    X(GLenum, GetError, (void)) \
    X(void, GetIntegerv, (GLenum, GLint *)) \
    X(void, GetFloatv, (GLenum, GLfloat *)) \
    X(GLboolean, IsEnabled, (GLenum)) \
    X(void, Enable, (GLenum)) \
    X(void, Disable, (GLenum)) \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, Scissor, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(void, GenTextures, (GLsizei, GLuint *)) \
    X(void, DeleteTextures, (GLsizei, const GLuint *)) \
    X(void, BindTexture, (GLenum, GLuint)) \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, TexParameteri, (GLenum, GLenum, GLint)) \
    X(void, TexParameterfv, (GLenum, GLenum, const GLfloat *)) \
    X(void, PixelStorei, (GLenum, GLint)) \
    X(void, DrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, Flush, (void)) \
    X(void, Finish, (void)) \
    X(void, ActiveTexture, (GLenum)) \
    X(GLuint, CreateShader, (GLenum)) \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, CompileShader, (GLuint)) \
    X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, DeleteShader, (GLuint)) \
    X(GLuint, CreateProgram, (void)) \
    X(void, AttachShader, (GLuint, GLuint)) \
    X(void, BindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, LinkProgram, (GLuint)) \
    X(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, DeleteProgram, (GLuint)) \
    X(void, UseProgram, (GLuint)) \
    X(GLint, GetUniformLocation, (GLuint, const GLchar *)) \
    X(GLint, GetAttribLocation, (GLuint, const GLchar *)) \
    X(void, Uniform1f, (GLint, GLfloat)) \
    X(void, Uniform1i, (GLint, GLint)) \
    X(void, Uniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, GenBuffers, (GLsizei, GLuint *)) \
    X(void, DeleteBuffers, (GLsizei, const GLuint *)) \
    X(void, BindBuffer, (GLenum, GLuint)) \
    X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, EnableVertexAttribArray, (GLuint)) \
    X(void, DisableVertexAttribArray, (GLuint)) \
    X(void, GetVertexAttribiv, (GLuint, GLenum, GLint *)) \
    X(void, VertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, BindVertexArray, (GLuint)) \
    X(void, GenFramebuffers, (GLsizei, GLuint *)) \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void, BindFramebuffer, (GLenum, GLuint)) \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, CheckFramebufferStatus, (GLenum)) \
    X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))

/* Only for the timer queries and for putting back sampler objects after a preset: OpenGL 3.3, and missing ones are not a failure. */
#define GL_FUNCTIONS_33(X) \
    X(void, GenQueries, (GLsizei, GLuint *)) \
    X(void, DeleteQueries, (GLsizei, const GLuint *)) \
    X(void, BeginQuery, (GLenum, GLuint)) \
    X(void, EndQuery, (GLenum)) \
    X(void, GetQueryObjectui64v, (GLuint, GLenum, GLuint64 *)) \
    X(void, BindSampler, (GLuint, GLuint))

#define X(ret, name, args) typedef ret(APIENTRY *gl_##name##_f) args;
GL_FUNCTIONS(X)
GL_FUNCTIONS_33(X)
#undef X

static struct {
#define X(ret, name, args) gl_##name##_f name;
    GL_FUNCTIONS(X)
    GL_FUNCTIONS_33(X)
#undef X
} gl;

static int gl_ready, gl_has_33;

typedef struct Program {
    GLuint id;
    int built, failed;
    char failure[400];
    GLint mvp, frame_count, frame_direction, output_size, texture_size, input_size, texture, color;
    int large;          /* which settings the uniforms hold now: -1 none yet, 0 the normal ones, 1 the large ones */
} Program;

static Program programs[PC_LOOKS];
static GLuint picture_texture, vertex_buffer, preset_texture, preset_framebuffer;
static int picture_w, picture_h, preset_w, preset_h;
static uint32_t picture_rgba[240 * 320];

int pc_filter_gl_start(char *why, size_t size)
{
    const char *version;
    if (gl_ready)
        return 0;
#define X(ret, name, args) gl.name = (gl_##name##_f)SDL_GL_GetProcAddress("gl" #name);
    GL_FUNCTIONS(X)
    GL_FUNCTIONS_33(X)
#undef X
#define X(ret, name, args) if (!gl.name) { snprintf(why, size, "this OpenGL has no gl%s", #name); return -1; }
    GL_FUNCTIONS(X)
#undef X
    version = (const char *)gl.GetString(GL_VERSION);
    if (!version || atoi(version) < 3) {
        snprintf(why, size, "the looks need OpenGL 3.0 or later, and this one is %s", version ? version : "unknown");
        return -1;
    }
    gl_has_33 = gl.GenQueries && gl.DeleteQueries && gl.BeginQuery && gl.EndQuery && gl.GetQueryObjectui64v && gl.BindSampler && (atoi(version) > 3 || strncmp(version, "3.3", 3) >= 0);
    memset(programs, 0, sizeof(programs));
    gl_ready = 1;
    return 0;
}

/* ---- OpenGL state: everything this file changes, read before and put back after. */
#define SAVED_UNITS 3       /* SDL's renderer uses texture units 0 to 2 */
#define SAVED_ATTRIBS 3

typedef struct Saved {
    GLint program, active_texture, textures[SAVED_UNITS], array_buffer, vertex_array, draw_fb, read_fb, viewport[4], scissor_box[4];
    GLint unpack_row_length, unpack_alignment, unpack_skip_rows, unpack_skip_pixels, pack_alignment;
    GLboolean scissor, blend, srgb, depth, cull, stencil;
    GLint attrib_on[SAVED_ATTRIBS];
    GLint samplers[16];
    int with_samplers;
} Saved;

static const GLuint attrib_slot[SAVED_ATTRIBS] = {0, 1, 7};
/* Where the shaders' vertex inputs go. On some drivers the old fixed inputs share the low numbers (2 the normal, 3 the colour, 8 and up the texture coordinates), and SDL's renderer uses
   the colour and the first texture coordinate. 0 is the position, which every draw sets anew; 1 and 7 belong to nothing SDL uses. */
#define ATTRIB_VERTEX 0
#define ATTRIB_TEXCOORD 1
#define ATTRIB_COLOR 7

static void save(Saved *s, int with_samplers)
{
    int i;
    memset(s, 0, sizeof(*s));
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &s->program);
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &s->active_texture);
    for (i = 0; i < SAVED_UNITS; i++) {
        gl.ActiveTexture(GL_TEXTURE0 + (GLenum)i);
        gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &s->textures[i]);
    }
    gl.ActiveTexture((GLenum)s->active_texture);
    gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->array_buffer);
    gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vertex_array);
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->draw_fb);
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s->read_fb);
    gl.GetIntegerv(GL_VIEWPORT, s->viewport);
    gl.GetIntegerv(GL_SCISSOR_BOX, s->scissor_box);
    gl.GetIntegerv(GL_UNPACK_ROW_LENGTH, &s->unpack_row_length);
    gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &s->unpack_alignment);
    gl.GetIntegerv(GL_UNPACK_SKIP_ROWS, &s->unpack_skip_rows);
    gl.GetIntegerv(GL_UNPACK_SKIP_PIXELS, &s->unpack_skip_pixels);
    gl.GetIntegerv(GL_PACK_ALIGNMENT, &s->pack_alignment);
    s->scissor = gl.IsEnabled(GL_SCISSOR_TEST);
    s->blend = gl.IsEnabled(GL_BLEND);
    s->srgb = gl.IsEnabled(GL_FRAMEBUFFER_SRGB);
    s->depth = gl.IsEnabled(GL_DEPTH_TEST);
    s->cull = gl.IsEnabled(GL_CULL_FACE);
    s->stencil = gl.IsEnabled(GL_STENCIL_TEST);
    for (i = 0; i < SAVED_ATTRIBS; i++)
        gl.GetVertexAttribiv(attrib_slot[i], GL_VERTEX_ATTRIB_ARRAY_ENABLED, &s->attrib_on[i]);
    s->with_samplers = with_samplers && gl_has_33;
    for (i = 0; s->with_samplers && i < 16; i++) {
        gl.ActiveTexture(GL_TEXTURE0 + (GLenum)i);
        gl.GetIntegerv(GL_SAMPLER_BINDING, &s->samplers[i]);
    }
    gl.ActiveTexture((GLenum)s->active_texture);
    gl.GetError();      /* a state SDL's OpenGL lacks must not be taken for an error of ours below */
}

static void set_enabled(GLenum what, GLboolean on)
{
    if (on)
        gl.Enable(what);
    else
        gl.Disable(what);
}

static void restore(const Saved *s)
{
    int i;
    for (i = 0; s->with_samplers && i < 16; i++)
        gl.BindSampler((GLuint)i, (GLuint)s->samplers[i]);
    gl.BindVertexArray((GLuint)s->vertex_array);
    for (i = 0; i < SAVED_ATTRIBS; i++) {
        if (s->attrib_on[i])
            gl.EnableVertexAttribArray(attrib_slot[i]);
        else
            gl.DisableVertexAttribArray(attrib_slot[i]);
    }
    gl.UseProgram((GLuint)s->program);
    for (i = 0; i < SAVED_UNITS; i++) {
        gl.ActiveTexture(GL_TEXTURE0 + (GLenum)i);
        gl.BindTexture(GL_TEXTURE_2D, (GLuint)s->textures[i]);
    }
    gl.ActiveTexture((GLenum)s->active_texture);
    gl.BindBuffer(GL_ARRAY_BUFFER, (GLuint)s->array_buffer);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)s->draw_fb);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)s->read_fb);
    gl.Viewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    gl.Scissor(s->scissor_box[0], s->scissor_box[1], s->scissor_box[2], s->scissor_box[3]);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, s->unpack_row_length);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, s->unpack_alignment);
    gl.PixelStorei(GL_UNPACK_SKIP_ROWS, s->unpack_skip_rows);
    gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, s->unpack_skip_pixels);
    gl.PixelStorei(GL_PACK_ALIGNMENT, s->pack_alignment);
    set_enabled(GL_SCISSOR_TEST, s->scissor);
    set_enabled(GL_BLEND, s->blend);
    set_enabled(GL_FRAMEBUFFER_SRGB, s->srgb);
    set_enabled(GL_DEPTH_TEST, s->depth);
    set_enabled(GL_CULL_FACE, s->cull);
    set_enabled(GL_STENCIL_TEST, s->stencil);
    gl.GetError();
}

/* ---- building a look's program */

static const char *shader_text(const char *file)
{
    int i;
    for (i = 0; i < pc_shader_count; i++)
        if (!strcmp(pc_shader_files[i], file))
            return pc_shader_texts[i];
    return NULL;
}

/* One stage: the file's "#version" line (or 130), the stage's name and PARAMETER_UNIFORM, then the rest of the file. The version line is blanked where it stood, so the compiler's line
   numbers in a message stay one off from the file's. */
static GLuint compile(GLenum stage, const char *text, char *why, size_t size)
{
    static char head[200];
    const char *version = NULL, *at = text, *parts[3];
    char *body = NULL;
    GLint ok = 0, length;
    GLuint shader;
    while ((at = strstr(at, "#version")) != NULL) {
        if (at == text || at[-1] == '\n') {
            version = at;
            break;
        }
        at++;
    }
    if (version) {
        size_t n = strcspn(version, "\r\n");
        body = (char *)malloc(strlen(text) + 1);
        if (!body) {
            snprintf(why, size, "out of memory");
            return 0;
        }
        strcpy(body, text);
        snprintf(head, sizeof(head), "%.*s\n", (int)(n < 60 ? n : 60), version);
        memset(body + (version - text), ' ', n);
    } else {
        snprintf(head, sizeof(head), "#version 130\n");
    }
    parts[0] = head;
    parts[1] = stage == GL_VERTEX_SHADER ? "#define VERTEX\n#define PARAMETER_UNIFORM\n" : "#define FRAGMENT\n#define PARAMETER_UNIFORM\n";
    parts[2] = body ? body : text;
    shader = gl.CreateShader(stage);
    gl.ShaderSource(shader, 3, parts, NULL);
    gl.CompileShader(shader);
    free(body);
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[300] = "";
        gl.GetShaderInfoLog(shader, (GLsizei)sizeof(log), &length, log);
        log[strcspn(log, "\r\n")] = 0;
        snprintf(why, size, "its %s shader would not compile: %s", stage == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}

/* The settings into the program's uniforms, by name. A name the program does not have is skipped: the compiler drops parameters a shader never reads. */
static void apply(GLuint program, const char *settings)
{
    char name[64];
    const char *at = settings;
    while (at && *at) {
        size_t n;
        const char *equals;
        while (*at == ' ')
            at++;
        n = strcspn(at, " ");
        equals = memchr(at, '=', n);
        if (equals && (size_t)(equals - at) < sizeof(name)) {
            GLint where;
            memcpy(name, at, (size_t)(equals - at));
            name[equals - at] = 0;
            where = gl.GetUniformLocation(program, name);
            if (where >= 0)
                gl.Uniform1f(where, (GLfloat)atof(equals + 1));
        }
        at += n;
    }
}

static int build(int look, char *why, size_t size)
{
    Program *p = &programs[look];
    const char *text;
    GLuint vs, fs;
    GLint ok = 0, length;
    if (p->built)
        return 0;
    if (p->failed) {
        snprintf(why, size, "%s", p->failure);
        return -1;
    }
    text = shader_text(looks[look].shader);
    p->failed = 1;
    if (!text) {
        snprintf(p->failure, sizeof(p->failure), "the look %s has no shader %s in this program", looks[look].name, looks[look].shader);
        snprintf(why, size, "%s", p->failure);
        return -1;
    }
    vs = compile(GL_VERTEX_SHADER, text, p->failure, sizeof(p->failure));
    fs = vs ? compile(GL_FRAGMENT_SHADER, text, p->failure, sizeof(p->failure)) : 0;
    if (!vs || !fs) {
        if (vs)
            gl.DeleteShader(vs);
        snprintf(why, size, "%s", p->failure);
        return -1;
    }
    p->id = gl.CreateProgram();
    gl.AttachShader(p->id, vs);
    gl.AttachShader(p->id, fs);
    gl.BindAttribLocation(p->id, ATTRIB_VERTEX, "VertexCoord");
    gl.BindAttribLocation(p->id, ATTRIB_TEXCOORD, "TexCoord");
    gl.BindAttribLocation(p->id, ATTRIB_COLOR, "COLOR");
    gl.BindAttribLocation(p->id, ATTRIB_COLOR, "Color");
    gl.LinkProgram(p->id);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    gl.GetProgramiv(p->id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[300] = "";
        gl.GetProgramInfoLog(p->id, (GLsizei)sizeof(log), &length, log);
        log[strcspn(log, "\r\n")] = 0;
        snprintf(p->failure, sizeof(p->failure), "its shader would not link: %s", log);
        gl.DeleteProgram(p->id);
        p->id = 0;
        snprintf(why, size, "%s", p->failure);
        return -1;
    }
    p->mvp = gl.GetUniformLocation(p->id, "MVPMatrix");
    p->frame_count = gl.GetUniformLocation(p->id, "FrameCount");
    p->frame_direction = gl.GetUniformLocation(p->id, "FrameDirection");
    p->output_size = gl.GetUniformLocation(p->id, "OutputSize");
    p->texture_size = gl.GetUniformLocation(p->id, "TextureSize");
    p->input_size = gl.GetUniformLocation(p->id, "InputSize");
    p->texture = gl.GetUniformLocation(p->id, "Texture");
    p->color = gl.GetAttribLocation(p->id, "COLOR");
    if (p->color < 0)
        p->color = gl.GetAttribLocation(p->id, "Color");
    p->large = -1;
    p->failed = 0;
    p->built = 1;
    return 0;
}

int pc_filter_gl_build(int look, char *why, size_t size)
{
    Saved s;
    int result;
    if (look < 0 || look >= PC_LOOKS || !looks[look].shader)
        return 0;
    if (!gl_ready) {
        snprintf(why, size, "OpenGL is not ready");
        return -1;
    }
    save(&s, 0);
    result = build(look, why, size);
    restore(&s);
    return result;
}

int pc_filter_gl_build_all(char *why, size_t size)
{
    Saved s;
    int look, result = 0;
    if (!gl_ready)
        return -1;
    save(&s, 0);
    for (look = 0; look < PC_LOOKS; look++)
        if (looks[look].shader && build(look, why, size) != 0 && result == 0)
            result = -1;
    restore(&s);
    return result;
}

/* ---- drawing */

/* The picture into its texture, with the alpha set, as a preset's shaders may read it. */
static void upload(const uint32_t *rgb, int stride, int pw, int ph)
{
    int x, y;
    for (y = 0; y < ph; y++)
        for (x = 0; x < pw; x++)
            picture_rgba[y * pw + x] = rgb[y * stride + x] | 0xFF000000u;
    gl.ActiveTexture(GL_TEXTURE0);
    if (!picture_texture) {
        static const GLfloat black[4] = {0, 0, 0, 1};
        gl.GenTextures(1, &picture_texture);
        gl.BindTexture(GL_TEXTURE_2D, picture_texture);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        gl.TexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, black);
        picture_w = picture_h = 0;
    }
    gl.BindTexture(GL_TEXTURE_2D, picture_texture);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (pw != picture_w || ph != picture_h) {
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, picture_rgba);
        picture_w = pw;
        picture_h = ph;
    } else {
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pw, ph, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, picture_rgba);
    }
}

/* Four corners, each the position the shaders call VertexCoord (0 to 1 across the place) and the picture's TexCoord: the bottom left corner is the picture's last line. */
static const GLfloat corners[4][6] = {{0, 0, 0, 1, 0, 1}, {1, 0, 0, 1, 1, 1}, {0, 1, 0, 1, 0, 0}, {1, 1, 0, 1, 1, 0}};

static void draw_shader(int look, int pw, int ph, int w, int h, int top_down, uint32_t frame)
{
    Program *p = &programs[look];
    /* VertexCoord's 0 to 1 onto the place. A framebuffer that keeps its top line first gets the picture upside down in OpenGL's terms, so that it is the right way up in its texture. */
    const GLfloat mvp[16] = {2, 0, 0, 0, 0, top_down ? -2.0f : 2.0f, 0, 0, 0, 0, -1, 0, -1, top_down ? 1.0f : -1.0f, 0, 1};
    int large = h >= LARGE_LINES && looks[look].settings_large != NULL;
    gl.UseProgram(p->id);
    if (p->large != large) {
        apply(p->id, looks[look].settings);
        if (large)
            apply(p->id, looks[look].settings_large);
        p->large = large;
    }
    if (p->mvp >= 0) gl.UniformMatrix4fv(p->mvp, 1, GL_FALSE, mvp);
    if (p->frame_count >= 0) gl.Uniform1i(p->frame_count, (GLint)frame);
    if (p->frame_direction >= 0) gl.Uniform1i(p->frame_direction, 1);
    if (p->output_size >= 0) gl.Uniform2f(p->output_size, (GLfloat)w, (GLfloat)h);
    if (p->texture_size >= 0) gl.Uniform2f(p->texture_size, (GLfloat)pw, (GLfloat)ph);
    if (p->input_size >= 0) gl.Uniform2f(p->input_size, (GLfloat)pw, (GLfloat)ph);
    if (p->texture >= 0) gl.Uniform1i(p->texture, 0);
    if (!vertex_buffer) {
        gl.GenBuffers(1, &vertex_buffer);
        gl.BindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
        gl.BufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
    }
    gl.BindVertexArray(0);
    gl.BindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
    gl.VertexAttribPointer(ATTRIB_VERTEX, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat), (const void *)0);
    gl.VertexAttribPointer(ATTRIB_TEXCOORD, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat), (const void *)(4 * sizeof(GLfloat)));
    gl.EnableVertexAttribArray(ATTRIB_VERTEX);
    gl.EnableVertexAttribArray(ATTRIB_TEXCOORD);
    gl.DisableVertexAttribArray(ATTRIB_COLOR);
    if (p->color >= 0)
        gl.VertexAttrib4f((GLuint)p->color, 1, 1, 1, 1);
    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* ---- a player's preset, through librashader. Only the few calls used are declared, as librashader.h (MIT licence) lays them out for its ABI version 2, API version 5. */
typedef void *libra_error_t;
typedef void *libra_shader_preset_t;
typedef void *libra_gl_filter_chain_t;
typedef struct { size_t version; uint16_t glsl_version; uint8_t use_dsa, force_no_mipmaps, disable_cache; } LibraChainOptions;
typedef struct { uint32_t handle, format, width, height; } LibraImage;
typedef struct { float x, y; uint32_t width, height; } LibraViewport;
typedef struct {
    size_t version;
    uint8_t clear_history;
    int32_t frame_direction;
    uint32_t rotation, total_subframes, current_subframe;
    float aspect_ratio, frames_per_second;
    uint32_t frametime_delta;
    int32_t color_space;
    float brightness_nits;
    uint32_t expand_gamut;
    float gyroscope[3], accelerometer[3], accelerometer_rest[3];
} LibraFrameOptions;
#define LIBRA_ABI 2
#define LIBRA_API 5

static struct {
    HMODULE dll;
    size_t (*abi_version)(void);
    size_t (*api_version)(void);
    libra_error_t (*preset_create)(const char *, libra_shader_preset_t *);
    libra_error_t (*preset_free)(libra_shader_preset_t *);
    libra_error_t (*chain_create)(libra_shader_preset_t *, const void *(*)(const char *), const LibraChainOptions *, libra_gl_filter_chain_t *);
    libra_error_t (*chain_frame)(libra_gl_filter_chain_t *, size_t, LibraImage, LibraImage, const LibraViewport *, const float *, const LibraFrameOptions *);
    libra_error_t (*chain_free)(libra_gl_filter_chain_t *);
    int32_t (*error_write)(libra_error_t, char **);
    int32_t (*error_free_string)(char **);
    int32_t (*error_free)(libra_error_t *);
    libra_gl_filter_chain_t chain;
} libra;

static void libra_why(libra_error_t e, const char *what, char *why, size_t size)
{
    char *text = NULL;
    if (e && libra.error_write && libra.error_write(e, &text) == 0 && text) {
        snprintf(why, size, "%s: %s", what, text);
        libra.error_free_string(&text);
    } else {
        snprintf(why, size, "%s", what);
    }
    if (e && libra.error_free)
        libra.error_free(&e);
}

static void libra_unload(void)
{
    if (libra.chain && libra.chain_free)
        libra.chain_free(&libra.chain);
    if (libra.dll)
        FreeLibrary(libra.dll);
    memset(&libra, 0, sizeof(libra));
}

static const void *gl_loader(const char *name) { return SDL_GL_GetProcAddress(name); }

int pc_filter_preset_loaded(void) { return libra.chain != NULL; }

/* ---- a look at a preset file before the library is loaded, so the usual faults get plain words of our own: the file is not there, it names no shaders, or a file it names
   (a shader, a texture, or another preset it builds on with #reference) is not there. A preset's paths start from its own folder. Paths with RetroArch's $ words in them are left to
   the library. Anything subtler, such as a shader that will not compile, is the library's to find. */
#define PRESET_LINE 1500
#define PRESET_DEPTH 8

static void trim(char *s)
{
    size_t n = strlen(s), a = 0;
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
    while (s[a] == ' ' || s[a] == '\t')
        a++;
    memmove(s, s + a, n - a + 1);
    n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = 0;
    }
}

/* key = value, both trimmed, or 0 when the line is not one. */
static int key_value(char *line, char **key, char **value)
{
    char *equals = strchr(line, '=');
    if (!equals || line[0] == '#')
        return 0;
    *equals = 0;
    *key = line;
    *value = equals + 1;
    trim(*key);
    trim(*value);
    return 1;
}

static int file_there(const char *folder, const char *name, char *full, size_t size)
{
    FILE *f;
    if (strchr(name, '$'))
        return 1;
    if ((name[0] && name[1] == ':') || name[0] == '/' || name[0] == '\\')
        snprintf(full, size, "%s", name);
    else
        snprintf(full, size, "%s\\%s", folder, name);
    f = fopen(full, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static int preset_check_depth(const char *path, int depth, char *why, size_t size)
{
    char line[PRESET_LINE], textures[PRESET_LINE], folder[1200], full[1400], *key, *value;
    int shaders = -1, references = 0, highest = -1, k;
    FILE *f;
    if (depth > PRESET_DEPTH) {
        snprintf(why, size, "the preset %s goes more than %d presets deep with #reference", path, PRESET_DEPTH);
        return -1;
    }
    f = fopen(path, "rb");
    if (!f) {
        snprintf(why, size, "there is no preset file %s", path);
        return -1;
    }
    snprintf(folder, sizeof(folder), "%s", path);
    for (k = (int)strlen(folder); k > 0 && folder[k - 1] != '\\' && folder[k - 1] != '/'; k--)
        ;
    if (k > 0)
        folder[k - 1] = 0;
    else
        snprintf(folder, sizeof(folder), ".");
    textures[0] = 0;
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!strncmp(line, "#reference", 10)) {
            char referenced[PRESET_LINE];
            snprintf(referenced, sizeof(referenced), "%s", line + 10);
            trim(referenced);
            if (!file_there(folder, referenced, full, sizeof(full))) {
                snprintf(why, size, "the preset %s builds on %s, and there is no such file (looked for %s)", path, referenced, full);
                fclose(f);
                return -1;
            }
            if (!strchr(referenced, '$') && preset_check_depth(full, depth + 1, why, size) != 0) {
                fclose(f);
                return -1;
            }
            references++;
            continue;
        }
        if (!key_value(line, &key, &value))
            continue;
        if (!strcmp(key, "shaders")) {
            shaders = atoi(value);
        } else if (!strncmp(key, "shader", 6) && key[6] >= '0' && key[6] <= '9') {
            int n = atoi(key + 6);
            if (n > highest)
                highest = n;
            if (!file_there(folder, value, full, sizeof(full))) {
                snprintf(why, size, "the preset %s names %s = %s, and there is no such file (looked for %s)", path, key, value, full);
                fclose(f);
                return -1;
            }
        } else if (!strcmp(key, "textures")) {
            snprintf(textures, sizeof(textures), "%s", value);
        }
    }
    if (!references && shaders < 1) {
        snprintf(why, size, "the preset %s has no line shaders = N with N of 1 or more, so it is not a preset", path);
        fclose(f);
        return -1;
    }
    if (shaders > 0 && highest + 1 < shaders && !references) {
        snprintf(why, size, "the preset %s says shaders = %d but names no shader%d", path, shaders, highest + 1);
        fclose(f);
        return -1;
    }
    /* Each texture the preset lists, by its own line. */
    if (textures[0]) {
        const char *name = textures;
        while (*name) {
            int found = 0;
            size_t n = strcspn(name, ";");
            char wanted[200];
            snprintf(wanted, sizeof(wanted), "%.*s", (int)(n < sizeof(wanted) - 1 ? n : sizeof(wanted) - 1), name);
            name += n + (name[n] == ';');
            trim(wanted);
            if (!wanted[0])
                continue;
            rewind(f);
            while (!found && fgets(line, sizeof(line), f)) {
                trim(line);
                if (key_value(line, &key, &value) && !strcmp(key, wanted)) {
                    found = 1;
                    if (!file_there(folder, value, full, sizeof(full))) {
                        snprintf(why, size, "the preset %s names the texture %s = %s, and there is no such file (looked for %s)", path, wanted, value, full);
                        fclose(f);
                        return -1;
                    }
                }
            }
            if (!found && !references) {
                snprintf(why, size, "the preset %s lists the texture %s but gives no file for it", path, wanted);
                fclose(f);
                return -1;
            }
        }
    }
    fclose(f);
    return 0;
}

int pc_filter_preset_check(const char *path, char *why, size_t size)
{
    return preset_check_depth(path, 0, why, size);
}

int pc_filter_preset_load(const char *path, const char *dll_path, char *why, size_t size)
{
    wchar_t wide[1200];
    libra_shader_preset_t preset = NULL;
    libra_error_t e;
    LibraChainOptions options;
    Saved s;
    if (!gl_ready) {
        snprintf(why, size, "a preset needs the OpenGL renderer");
        return -1;
    }
    libra_unload();
    if (pc_filter_preset_check(path, why, size) != 0)
        return -1;
    if (!MultiByteToWideChar(CP_UTF8, 0, dll_path, -1, wide, (int)(sizeof(wide) / sizeof(wide[0])))) {
        snprintf(why, size, "the program's folder has a name too long for this");
        return -1;
    }
    /* Only this file: LOAD_WITH_ALTERED_SEARCH_PATH makes its own needs come from its folder, and nothing is looked for elsewhere under that name.
     * Windows' own error box (a "bad image" box for a damaged file, for one) is turned off while it loads, as for the game DLL (pc_game_dll.c), so
     * the reason comes back here and the player reads it in our words. A file that is there but will not load is told apart from no file. */
    {
        /* SetErrorMode gives back the mode it replaces: that one is kept, with the two boxes turned off, and put back after the load. */
        UINT mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
        DWORD code, a;
        SetErrorMode(mode | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
        libra.dll = LoadLibraryExW(wide, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        code = GetLastError();
        SetErrorMode(mode);
        if (!libra.dll) {
            a = GetFileAttributesW(wide);
            if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) {
                snprintf(why, size, "a preset needs librashader.dll beside the program, and there is none at %s", dll_path);
            } else {
                char words[400] = "";
                int n = (int)FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, words, sizeof(words), NULL);
                while (n > 0 && (words[n - 1] == '\n' || words[n - 1] == '\r' || words[n - 1] == ' ' || words[n - 1] == '.'))
                    words[--n] = 0;
                /* 193 is what Windows gives for a file that is not a 64-bit Windows program: a damaged file, or the 32-bit build of the library. 126
                 * says a file the library itself needs is not there. */
                snprintf(why, size, "librashader.dll is there, at %s, but Windows would not load it (error %lu: %s)%s", dll_path, (unsigned long)code,
                         words[0] ? words : "no description",
                         code == 193 ? ". It is damaged, or it is not a 64-bit Windows build of the library, which this program needs" :
                         code == 126 ? ". A file it needs itself is not there" : "");
            }
            return -1;
        }
    }
#define FIND(field, name) *(FARPROC *)&libra.field = GetProcAddress(libra.dll, name); if (!libra.field) { snprintf(why, size, "librashader.dll has no %s; it may be another version", name); libra_unload(); return -1; }
    FIND(abi_version, "libra_instance_abi_version")
    FIND(api_version, "libra_instance_api_version")
    FIND(preset_create, "libra_preset_create")
    FIND(preset_free, "libra_preset_free")
    FIND(chain_create, "libra_gl_filter_chain_create")
    FIND(chain_frame, "libra_gl_filter_chain_frame")
    FIND(chain_free, "libra_gl_filter_chain_free")
    FIND(error_write, "libra_error_write")
    FIND(error_free_string, "libra_error_free_string")
    FIND(error_free, "libra_error_free")
#undef FIND
    if (libra.abi_version() != LIBRA_ABI || libra.api_version() < LIBRA_API) {
        snprintf(why, size, "librashader.dll is a version this program does not know (its ABI %u, API %u; this program wants ABI %d, API %d or later)",
                 (unsigned)libra.abi_version(), (unsigned)libra.api_version(), LIBRA_ABI, LIBRA_API);
        libra_unload();
        return -1;
    }
    e = libra.preset_create(path, &preset);
    if (e) {
        libra_why(e, "librashader could not read the preset", why, size);
        libra_unload();
        return -1;
    }
    memset(&options, 0, sizeof(options));
    options.version = LIBRA_API;
    options.glsl_version = 330;
    options.disable_cache = 1;
    save(&s, 1);
    e = libra.chain_create(&preset, gl_loader, &options, &libra.chain);
    restore(&s);
    if (preset)
        libra.preset_free(&preset);
    if (e) {
        libra.chain = NULL;
        libra_why(e, "librashader could not build the preset", why, size);
        libra_unload();
        return -1;
    }
    return 0;
}

static int draw_preset(int pw, int ph, int x, int y, int w, int h, int fb_h, int top_down, uint32_t frame, const Saved *s, char *why, size_t size)
{
    LibraImage in, out;
    LibraViewport viewport;
    LibraFrameOptions options;
    libra_error_t e;
    int dy0, dy1;
    if (!libra.chain) {
        snprintf(why, size, "no preset is loaded");
        return -1;
    }
    if (!preset_texture || preset_w != w || preset_h != h) {
        if (!preset_texture)
            gl.GenTextures(1, &preset_texture);
        gl.BindTexture(GL_TEXTURE_2D, preset_texture);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        preset_w = w;
        preset_h = h;
        if (!preset_framebuffer)
            gl.GenFramebuffers(1, &preset_framebuffer);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, preset_framebuffer);
        gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, preset_texture, 0);
        if (gl.CheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            snprintf(why, size, "OpenGL would not make a framebuffer of %dx%d for the preset", w, h);
            return -1;
        }
    }
    in.handle = picture_texture;
    in.format = GL_RGBA8;
    in.width = (uint32_t)pw;
    in.height = (uint32_t)ph;
    out.handle = preset_texture;
    out.format = GL_RGBA8;
    out.width = (uint32_t)w;
    out.height = (uint32_t)h;
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = (uint32_t)w;
    viewport.height = (uint32_t)h;
    memset(&options, 0, sizeof(options));
    options.version = LIBRA_API;
    options.frame_direction = 1;
    options.total_subframes = 1;
    options.current_subframe = 1;
    options.frames_per_second = 60.0f;
    options.frametime_delta = 17;
    options.brightness_nits = 200.0f;
    e = libra.chain_frame(&libra.chain, (size_t)frame, in, out, &viewport, NULL, &options);
    if (e) {
        libra_why(e, "librashader could not draw the preset", why, size);
        return -1;
    }
    /* The preset's picture is the right way up in OpenGL's terms, its top line last. Copied into the place, turned over for a framebuffer that keeps its top line first. */
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, preset_framebuffer);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)s->draw_fb);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Disable(GL_FRAMEBUFFER_SRGB);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    dy0 = top_down ? y + h : fb_h - y - h;
    dy1 = top_down ? y : fb_h - y;
    gl.BlitFramebuffer(0, 0, w, h, x, dy0, x + w, dy1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    return 0;
}

int pc_filter_gl_draw(int look, const uint32_t *rgb, int stride, int pw, int ph, int x, int y, int w, int h, int fb_h, int top_down, uint32_t frame, char *why, size_t size)
{
    Saved s;
    int result = 0;
    if (!gl_ready) {
        snprintf(why, size, "OpenGL is not ready");
        return -1;
    }
    if (pw < 1 || ph < 1 || pw > 320 || ph > 240 || w < 1 || h < 1) {
        snprintf(why, size, "a picture or place of a size the looks do not take");
        return -1;
    }
    save(&s, look == PC_LOOK_PRESET);
    if (look == PC_LOOK_PRESET) {
        upload(rgb, stride, pw, ph);
        result = draw_preset(pw, ph, x, y, w, h, fb_h, top_down, frame, &s, why, size);
    } else if (look >= 0 && look < PC_LOOKS && looks[look].shader) {
        if (build(look, why, size) == 0) {
            upload(rgb, stride, pw, ph);
            gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)s.draw_fb);
            gl.Viewport(x, top_down ? y : fb_h - y - h, w, h);
            gl.Disable(GL_SCISSOR_TEST);
            gl.Disable(GL_BLEND);
            gl.Disable(GL_FRAMEBUFFER_SRGB);
            gl.Disable(GL_DEPTH_TEST);
            gl.Disable(GL_CULL_FACE);
            gl.Disable(GL_STENCIL_TEST);
            draw_shader(look, pw, ph, w, h, top_down, frame);
        } else {
            result = -1;
        }
    } else {
        snprintf(why, size, "the look %s is not a shader", pc_filter_name(look));
        result = -1;
    }
    if (result == 0 && gl.GetError() != GL_NO_ERROR) {
        snprintf(why, size, "OpenGL said there was an error drawing the look %s", pc_filter_name(look));
        result = -1;
    }
    restore(&s);
    return result;
}

/* ---- the GPU's time, for the timing tool */
#define TIMERS 4096
static GLuint timers[TIMERS];
static int timers_used, timer_open;

int pc_filter_gl_timer_begin(void)
{
    if (!gl_ready || !gl_has_33 || timer_open)
        return -1;
    if (timers_used == TIMERS)
        timers_used = 0;
    if (!timers[timers_used])
        gl.GenQueries(1, &timers[timers_used]);
    gl.BeginQuery(GL_TIME_ELAPSED, timers[timers_used]);
    timer_open = 1;
    return timers_used++;
}

void pc_filter_gl_timer_end(void)
{
    if (timer_open)
        gl.EndQuery(GL_TIME_ELAPSED);
    timer_open = 0;
}

double pc_filter_gl_timer_ms(int which)
{
    GLuint64 ns = 0;
    if (which < 0 || which >= TIMERS || !timers[which])
        return -1;
    gl.GetQueryObjectui64v(timers[which], GL_QUERY_RESULT, &ns);
    return (double)ns / 1e6;
}

void pc_filter_gl_stop(void)
{
    int i;
    if (!gl_ready)
        return;
    libra_unload();
    for (i = 0; i < PC_LOOKS; i++)
        if (programs[i].id)
            gl.DeleteProgram(programs[i].id);
    memset(programs, 0, sizeof(programs));
    if (picture_texture) gl.DeleteTextures(1, &picture_texture);
    if (preset_texture) gl.DeleteTextures(1, &preset_texture);
    if (vertex_buffer) gl.DeleteBuffers(1, &vertex_buffer);
    if (preset_framebuffer) gl.DeleteFramebuffers(1, &preset_framebuffer);
    for (i = 0; i < TIMERS; i++)
        if (timers[i])
            gl.DeleteQueries(1, &timers[i]);
    memset(timers, 0, sizeof(timers));
    timers_used = timer_open = 0;
    picture_texture = preset_texture = vertex_buffer = preset_framebuffer = 0;
    picture_w = picture_h = preset_w = preset_h = 0;
    gl_ready = 0;
}
