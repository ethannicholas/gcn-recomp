// GL back end for the GX pixel pipeline. Runs on the main thread.
//
// One source serves desktop OpenGL 3.3 core and OpenGL ES 3.2; where the two profiles
// differ, the difference is confined to the small block of helpers below.
#include "../runtime.h"
#include "render.h"
#include "render_gl.h"
#include "shadergen.h"
#include "gl.h"
#include <chrono>
#include <unordered_map>

bool write_png(const char* path, const uint8_t* rgba, int w, int h);

namespace gx {

static const int EFB_W = 640, EFB_H = 528;

// The renderer's half of GCN_FRAMETIME (see xf.cpp): one line per batch with the time
// spent uploading textures, uploading the vertex buffer, and issuing the frame, plus how
// many times the full pixel state had to be re-applied. CPU time only: the GPU runs
// behind, so a long frame here is driver and submission cost, not fill.
static const bool g_frametime = getenv("GCN_FRAMETIME") != nullptr;
static inline double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// Logic-op blending does not exist in GL ES: there is no GL_COLOR_LOGIC_OP, no
// glLogicOp, and none of the GL_CLEAR..GL_SET enums. The game uses GX logic ops for
// only a few effects, so on ES they are skipped -- the draw writes through with
// blending off, which matches the common GX_LO_COPY case. Reproducing the rest would
// mean reading the framebuffer in the generated TEV shader via
// GL_EXT_shader_framebuffer_fetch.
#ifdef GCN_GL_ES
static inline void set_logic_op_off() {}
static inline void set_logic_op(uint32_t) {}
// ES spells this with the float suffix; desktop GL 3.3 core only has the double form.
static inline void clear_depth(float d) { glClearDepthf(d); }
// ES has no sampler LOD bias. GX uses it to nudge mip selection, so skipping it can
// pick a slightly different mip level than hardware would.
static inline void set_lod_bias(GLuint, float) {}
#else
static const GLenum kLogicOp[16] = {GL_CLEAR, GL_AND, GL_AND_REVERSE, GL_COPY, GL_AND_INVERTED, GL_NOOP, GL_XOR, GL_OR,
                                    GL_NOR, GL_EQUIV, GL_INVERT, GL_OR_REVERSE, GL_COPY_INVERTED, GL_OR_INVERTED, GL_NAND, GL_SET};
static inline void set_logic_op_off() { glDisable(GL_COLOR_LOGIC_OP); }
static inline void set_logic_op(uint32_t mode) {
    glEnable(GL_COLOR_LOGIC_OP);
    glLogicOp(kLogicOp[mode & 15]);
}
static inline void clear_depth(float d) { glClearDepth(d); }
static inline void set_lod_bias(GLuint s, float bias) {
    glSamplerParameterf(s, GL_TEXTURE_LOD_BIAS, bias);
}
#endif

// The inputs a program's uniforms were last computed from; see AppliedState. `gen` and
// `view_gen` say whether the record is current at all (g_uniform_gen, g_view_gen).
struct UniformShadow {
    uint32_t gen = 0, view_gen = 0;
    float proj[7];
    uint8_t view_space, background, foreground, fg_layer;
    float viewport[6];
    uint32_t scissor_off;      // bp[0x59]
    uint32_t psize_reg;        // bp[0x22]
    uint32_t tev_reg[4][2], tev_konst[4][2];
    uint32_t aref;             // bp[0xF3]
    uint32_t ind[9];           // bp[0x06..0x0E]
    uint32_t indcs[2];         // bp[0x25..0x26]
    uint32_t fog[5];           // bp[0xEE..0xF2]
    uint32_t screen_uv;
    float ripple[2];
    float tsz[16];
};

struct Program {
    GLuint prog;
    GLint u_proj, u_vp_a, u_vp_b, u_point_size, u_tex, u_reg, u_konst, u_texsize, u_indmtx, u_indscale,
        u_alpharef, u_fog, u_fogcolor, u_indcoordscale;
    GLint u_vr, u_view, u_crop, u_zproj, u_screen_uv, u_screen_px, u_screen_ripple;
    mutable UniformShadow shadow;
};

// Samples kept per hardware pixel, in each axis. See render_set_internal_scale.
static int g_scale = 2;

// VR eye state. When active, perspective batches are re-projected for the eye and the
// orthographic ones (the 2D HUD) are painted on a frame standing out in front of the
// game's camera -- see render_hud_frame.
static bool g_vr_active = false;
static float g_vr_proj[16], g_vr_view[16];
// The eye's view with the world's pitch taken out, for world geometry. The HUD frame uses
// g_vr_view untouched -- it is placed in the headset's space, not the game's.
static float g_vr_view_world[16];
// Depth layers in stereo (render_set_depth_layers). Both are off unless a game turns them
// on, because where a game puts its layers in the depth buffer -- if it uses them at all --
// is the game's own convention.
//
// Background: a perspective draw whose viewport confines it to depths from g_bg_from to
// 1 is drawn at infinity, through g_vr_view_sky, g_vr_view_world with its translation
// taken out. 0 is off.
static float g_vr_view_sky[16];
static float g_bg_from = 0.0f;
// Foreground: a perspective draw whose viewport confines it to depths no further than
// g_fg_to is scaled towards the camera by g_fg_scale -- the same angular size in each eye,
// but that much nearer and that much smaller. 0 is off.
static float g_fg_to = 0.0f;
static float g_fg_scale = 1.0f;
static float g_vr_view_fg[16], g_vr_view_world_fg[16];
static float g_world_pitch = 0.0f;
static float g_vr_hud[16];
// What the HUD frame goes through before the eye's view: the identity, unless the game's
// eye hook put something there -- an eye that tilts with a vehicle wants the HUD turned
// back to its level frame. g_vr_hud_eff is this times g_vr_hud, and is what the HUD is
// drawn with.
static float g_hud_xform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
static float g_vr_hud_eff[16];
// What world geometry goes through before the eye's own view: the chase camera's pitch
// taken back out (world_pitch_matrix), or whatever the game's eye hook put there.
// g_vr_view_world is g_vr_view times this.
static float g_world_xform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
// First person (render_set_first_person): while on, the game's eye hook is asked per frame
// where the eye stands, and g_eye_overridden says it answered for the frame being drawn.
static EyeHook g_eye_hook = nullptr;
static bool g_fp_on = false;
static bool g_eye_overridden = false;
// Per command of the batch being drawn: 1 for a draw the game's eye leaves out (the
// player's own model, say). Empty when nothing is hidden.
static std::vector<uint8_t> g_hide;
// The morph between theater and stereo; see render_set_vr_morph. 1 is plain stereo.
static float g_morph = 1.0f;
// A flat frame drawn as one of a stereo pair (render_execute_stereo_pair): where the eye
// stands beside the game's camera, 0 for the plain flat frame, and half the width of the
// panel the pair is shown on, both in game units.
static float g_flat_eye = 0.0f, g_flat_halfw = 1.0f;
static float g_panel_band = 0.0f;   // render_set_panel_band
static float g_panel[16];
// Whether the vertex shader can write clip distances, which is what crops the morph.
static bool g_clip_ok = false;
// What a draw is to the morph: where it ends up in stereo decides what it is folded from.
enum class MorphKind { World, CameraHeld, Hud };
static void morph_chain(const float P[16], MorphKind kind, float chain[16], float fog[16],
                        float crop[16]);
// What the eye has drawn so far, standing in for the game's copy of the finished frame.
// There are two because the two users want different moments: the water refracts the
// scene as it stood *before* the water was drawn, and the spray refracts it after, water
// included. One texture serving both would quietly re-point the water's lookup -- and the
// game's own final composite, which samples the same whole-frame copy -- at whatever the
// spray grabbed later. See grab_eye().
struct EyeGrab { GLuint tex; int w, h; };
static EyeGrab g_eye_grab, g_spray_grab;
static int g_eye_w, g_eye_h;
static bool is_fullscreen_tex(uint32_t id);
static GLuint g_efb_fbo, g_efb_color, g_efb_depth;
// Copy of the EFB as it looked at the last present. The display copy is immediately
// followed by an EFB clear, so repainting has to come from here, not the live EFB.

// The vertex and index buffers a batch is drawn from, and the VAO holding both. There are
// several, used in turn, because refilling one the GPU is still drawing from makes the
// driver wait for it. At the headset's own eye size that wait is rare, but the eyes' GPU
// time is what decides it: at 1.4x with 4x MSAA, one set left the flat pass at 27 ms a
// race frame on a Quest 3 (p50), nearly all of it in glBufferData waiting for the last
// frame's eyes; three sets took it to 4.9 ms, the same as at the old size. The GPU runs
// at most a frame or two behind, so three is enough. g_vao, g_vbo and g_ebo are the set
// in use; execute_batch moves them on.
static constexpr int kVertexRing = 3;
//
// Each set's vertex buffer holds the batch's packed vertices (Batch::packed), a region per
// texture coordinate count, so each set has a vertex array per count over the same two
// buffers: g_vaos[set][n] reads n coordinates at packed_stride(n). g_vao is the one bound.
static GLuint g_vaos[kVertexRing][9], g_vbos[kVertexRing], g_ebos[kVertexRing];
static int g_vertex_set = 0;
static GLuint g_vao, g_vbo, g_ebo;

// Binds the vertex layout for a draw with `ntex` texture coordinates.
static void bind_vertex_format(uint32_t ntex) {
    const GLuint vao = g_vaos[g_vertex_set][ntex];
    if (vao == g_vao) return;
    g_vao = vao;
    glBindVertexArray(vao);
}

// One draw command, from the packed region its texture coordinate count selects.
static void draw_cmd(const Batch& b, const Cmd& c) {
    static const GLenum mode[3] = {GL_TRIANGLES, GL_LINES, GL_POINTS};
    bind_vertex_format(draw_ntex(b.states[c.state]));
    glDrawElementsBaseVertex(mode[c.prim], c.count, GL_UNSIGNED_INT, (const void*)(uintptr_t)(c.first * sizeof(uint32_t)),
                             c.base_vertex);
}
static GLuint g_copy_prog, g_copy_vao;
static GLint g_copy_u_src, g_copy_u_rect, g_copy_u_mode, g_copy_u_depth;
static GLuint g_blit_prog;
static GLint g_blit_u_src, g_blit_u_rect;
static GLuint g_copy_fbo;
// The morph's background: see draw_morph_background.
static GLuint g_bg_prog;
static GLint g_bg_u_mvp, g_bg_u_crop, g_bg_u_quad, g_bg_u_col;
static GLuint g_vs;
static std::unordered_map<ShaderKey, Program, ShaderKeyHash> g_programs;
// `grab` marks one of the spray's screen-space grabs; see is_grab_copy().
// `depth`: an EFB copy of the depth buffer, whose texels are a depth packed into bytes. A
// filter would average neighbouring depths a byte at a time, which at an edge is nonsense
// -- a fog volume showed it as bright specks along a waterline -- so it is always sampled
// nearest. (At the console's own resolution a full-screen quad lands on texel centres and
// a filter changes nothing; at a higher internal scale it does not.)
struct GlTex { GLuint tex; uint32_t w, h, levels; bool efb; bool grab; bool depth; uint32_t last_used; };
static std::unordered_map<uint32_t, GlTex> g_textures;
// Frames counted here rather than reusing the GX frame counter, so eviction works the
// same for any frontend. Textures the game stops using are released: a race streams
// them continuously, and without this both the GL objects and the decoded copies grow
// without bound until the device runs out of memory and crawls.
static uint32_t g_render_frame;
static constexpr uint32_t kTexIdleFrames = 240;  // ~8 s at 30 fps
static std::unordered_map<uint32_t, GLuint> g_samplers;
static int g_win_w, g_win_h;
bool g_cull_swap = false;

static GLuint compile(GLenum type, const std::string& src) {
    GLuint sh = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    GLint ok;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        fprintf(stderr, "%s\n", src.c_str());
        fatal("shader compile failed: %s", log);
    }
    return sh;
}

static GLuint link(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        fatal("program link failed: %s", log);
    }
    return p;
}

// ---------------------------------------------------------------------------
// Shader cache
//
// A TEV configuration is compiled the first time a draw uses it, on the render thread, in
// the middle of a frame. The race start brings in nine at once (spray, wake, the speed
// effects) and later stretches of a course add more: on this machine's compiler that
// frame took 60 ms against a 4 ms norm, and a mobile driver takes tens of milliseconds
// per program, so a burst of them is a visible hitch at a fixed spot in the course.
//
// So every key that gets compiled is written to a file, and the next run builds all of
// them in render_init before the game starts. Where the driver can hand back program
// binaries (GL ES 3.0 can; macOS reports no binary formats) those are stored too, which
// turns the second run's startup from compiling into loading. A binary is only trusted
// with the same driver and the same generated source: the file carries the GL strings
// and each record a hash of the GLSL its key generates, and either changing drops back
// to compiling that record and rewriting the file.
// ---------------------------------------------------------------------------
#if defined(GCN_GL_ES) || defined(__APPLE__)
#define GCN_HAVE_PROGRAM_BINARY 1
#else
// The desktop glad loader stops at 3.3; program binaries are 4.1.
#define GCN_HAVE_PROGRAM_BINARY 0
#endif

static std::string g_shader_cache_path;
static bool g_binaries_supported = false;
static constexpr uint32_t kCacheMagic = 0x43535257;  // "WRSC"
static constexpr uint32_t kCacheVersion = 1;

static uint64_t hash_str(const char* s, uint64_t h = 1469598103934665603ull) {
    for (; s && *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull;
    return h;
}

// A stored binary is a link of one fragment shader against the one vertex shader, and
// nothing in the key says which vertex shader that was -- so its source goes in beside
// the fragment's. Changing it then retires every record in the file rather than quietly
// restoring binaries that predate the change.
static uint64_t g_vs_hash = 1469598103934665603ull;
static uint64_t program_src_hash(const char* fs) { return hash_str(fs, g_vs_hash); }

// Identifies the driver whose binaries the file holds.
static uint64_t driver_id() {
    uint64_t h = hash_str((const char*)glGetString(GL_VENDOR));
    h = hash_str((const char*)glGetString(GL_RENDERER), h);
    return hash_str((const char*)glGetString(GL_VERSION), h);
}

static void shader_cache_write_record(FILE* f, const ShaderKey& k, uint64_t src_hash, GLuint prog) {
    fwrite(&k, sizeof(k), 1, f);
    fwrite(&src_hash, sizeof(src_hash), 1, f);
    uint32_t fmt = 0, len = 0;
#if GCN_HAVE_PROGRAM_BINARY
    std::vector<uint8_t> bin;
    if (g_binaries_supported) {
        GLint n = 0;
        glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &n);
        if (n > 0) {
            bin.resize((size_t)n);
            GLenum e = 0;
            GLsizei got = 0;
            glGetProgramBinary(prog, n, &got, &e, bin.data());
            if (got > 0) { fmt = (uint32_t)e; len = (uint32_t)got; }
        }
    }
    fwrite(&fmt, sizeof(fmt), 1, f);
    fwrite(&len, sizeof(len), 1, f);
    if (len) fwrite(bin.data(), len, 1, f);
#else
    (void)prog;
    fwrite(&fmt, sizeof(fmt), 1, f);
    fwrite(&len, sizeof(len), 1, f);
#endif
}

static void shader_cache_write_header(FILE* f) {
    const uint32_t magic = kCacheMagic, ver = kCacheVersion;
    const uint64_t drv = driver_id();
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&ver, sizeof(ver), 1, f);
    fwrite(&drv, sizeof(drv), 1, f);
}

// Compiles a program from source, or restores it from a binary when one is given and
// the driver accepts it. Returns 0 if the binary was refused.
static GLuint build_program(const ShaderKey& k, const std::string& src, const uint8_t* bin, uint32_t fmt, uint32_t len) {
#if GCN_HAVE_PROGRAM_BINARY
    if (bin && len && g_binaries_supported) {
        GLuint p = glCreateProgram();
        glProgramBinary(p, (GLenum)fmt, bin, (GLsizei)len);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (ok) return p;
        glDeleteProgram(p);
        return 0;
    }
#else
    (void)bin; (void)fmt; (void)len;
#endif
    if (getenv("GCN_DUMP_SHADERS")) fprintf(stderr, "---- shader %zu ----\n%s\n", g_programs.size(), src.c_str());
    GLuint fs = compile(GL_FRAGMENT_SHADER, src);
    GLuint p = glCreateProgram();
    glAttachShader(p, g_vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
#if GCN_HAVE_PROGRAM_BINARY
    if (g_binaries_supported) glProgramParameteri(p, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
#endif
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        fatal("program link failed: %s", log);
    }
    glDeleteShader(fs);
    (void)k;
    return p;
}

static const Program& register_program(const ShaderKey& k, GLuint p) {
    Program pr{};
    pr.prog = p;
    pr.u_proj = glGetUniformLocation(p, "u_proj");
    pr.u_vp_a = glGetUniformLocation(p, "u_vp_a");
    pr.u_vp_b = glGetUniformLocation(p, "u_vp_b");
    pr.u_point_size = glGetUniformLocation(p, "u_point_size");
    pr.u_vr = glGetUniformLocation(p, "u_vr");
    pr.u_view = glGetUniformLocation(p, "u_view");
    pr.u_crop = glGetUniformLocation(p, "u_crop");
    pr.u_zproj = glGetUniformLocation(p, "u_zproj");
    pr.u_screen_uv = glGetUniformLocation(p, "u_screen_uv");
    pr.u_screen_px = glGetUniformLocation(p, "u_screen_px");
    pr.u_screen_ripple = glGetUniformLocation(p, "u_screen_ripple");
    pr.u_tex = glGetUniformLocation(p, "u_tex");
    pr.u_reg = glGetUniformLocation(p, "u_reg");
    pr.u_konst = glGetUniformLocation(p, "u_konst");
    pr.u_texsize = glGetUniformLocation(p, "u_texsize");
    pr.u_indmtx = glGetUniformLocation(p, "u_indmtx");
    pr.u_indscale = glGetUniformLocation(p, "u_indscale");
    pr.u_alpharef = glGetUniformLocation(p, "u_alpharef");
    pr.u_fog = glGetUniformLocation(p, "u_fog");
    pr.u_fogcolor = glGetUniformLocation(p, "u_fogcolor");
    pr.u_indcoordscale = glGetUniformLocation(p, "u_indcoordscale");
    glUseProgram(p);
    GLint units[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    glUniform1iv(pr.u_tex, 8, units);
    return g_programs.emplace(k, pr).first->second;
}

void render_set_shader_cache(const char* path) { g_shader_cache_path = path ? path : ""; }

// Building every program the file remembers: shader_cache_open reads the file, then
// shader_cache_step builds its records in order -- all at once from render_init, or a few
// milliseconds at a time for a frontend that asked for that
// (render_set_shader_cache_deferred) -- and the last step rewrites the file if it has to.
struct CacheLoad {
    std::vector<uint8_t> data;
    size_t pos = 0;
    bool active = false, same_driver = false, stale = false;
    int total = 0, done = 0, from_binary = 0, compiled = 0;
    std::chrono::steady_clock::time_point t0;
};
static CacheLoad g_cache_load;
static bool g_cache_deferred = false;

void render_set_shader_cache_deferred(bool on) { g_cache_deferred = on; }

// One record's fields, read from `p`; false at the end or at a truncated record.
struct CacheRecord {
    ShaderKey k;
    uint64_t src_hash;
    uint32_t fmt, len;
    const uint8_t* bin;
};
static bool cache_next(const uint8_t*& p, const uint8_t* end, CacheRecord& r) {
    auto take = [&](void* dst, size_t n) {
        if ((size_t)(end - p) < n) return false;
        memcpy(dst, p, n);
        p += n;
        return true;
    };
    if (!take(&r.k, sizeof(r.k)) || !take(&r.src_hash, 8) || !take(&r.fmt, 4) || !take(&r.len, 4)) return false;
    if ((size_t)(end - p) < r.len) return false;
    r.bin = p;
    p += r.len;
    return true;
}

static void shader_cache_open() {
    CacheLoad& L = g_cache_load;
    L = CacheLoad{};
    if (g_shader_cache_path.empty()) return;
    L.t0 = std::chrono::steady_clock::now();
    if (FILE* f = fopen(g_shader_cache_path.c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0) {
            L.data.resize((size_t)n);
            if (fread(L.data.data(), 1, (size_t)n, f) != (size_t)n) L.data.clear();
        }
        fclose(f);
    }
    if (L.data.empty()) return;
    uint32_t magic = 0, ver = 0;
    uint64_t drv = 0;
    if (L.data.size() < 16) return;
    memcpy(&magic, L.data.data(), 4);
    memcpy(&ver, L.data.data() + 4, 4);
    memcpy(&drv, L.data.data() + 8, 8);
    if (magic != kCacheMagic || ver != kCacheVersion) {
        fprintf(stderr, "[shaders] ignoring unrecognised cache %s\n", g_shader_cache_path.c_str());
        return;
    }
    L.same_driver = drv == driver_id();
    L.stale = !L.same_driver;
    L.pos = 16;
    // Counted first, for the progress a frontend shows.
    const uint8_t* p = L.data.data() + L.pos;
    const uint8_t* end = L.data.data() + L.data.size();
    CacheRecord r;
    while (cache_next(p, end, r)) L.total++;
    L.active = true;
}

bool render_shader_cache_step(double budget_ms, int* done, int* total) {
    CacheLoad& L = g_cache_load;
    if (L.active) {
        const auto t_step = std::chrono::steady_clock::now();
        const uint8_t* p = L.data.data() + L.pos;
        const uint8_t* end = L.data.data() + L.data.size();
        while (p < end) {
            CacheRecord r;
            if (!cache_next(p, end, r)) {
                L.stale = true;
                p = end;
                break;
            }
            L.done++;
            if (g_programs.count(r.k)) {
                L.stale = true;  // a duplicate, written by a crash mid-append
            } else {
                std::string src = gen_pixel_shader(r.k);
                const bool bin_ok = L.same_driver && r.len && program_src_hash(src.c_str()) == r.src_hash;
                GLuint prog = bin_ok ? build_program(r.k, src, r.bin, r.fmt, r.len) : 0;
                if (prog) {
                    L.from_binary++;
                } else {
                    prog = build_program(r.k, src, nullptr, 0, 0);
                    L.compiled++;
                    L.stale = true;
                }
                register_program(r.k, prog);
            }
            if (ms_since(t_step) >= budget_ms) break;
        }
        L.pos = (size_t)(p - L.data.data());
        if (p >= end) {
            // Anything that could not be used as stored is replaced: the whole file is
            // rewritten from the programs now in hand, with fresh binaries where the
            // driver gives them.
            if (L.stale || (g_binaries_supported && L.from_binary == 0 && L.compiled > 0)) {
                if (FILE* f = fopen(g_shader_cache_path.c_str(), "wb")) {
                    shader_cache_write_header(f);
                    for (auto& kv : g_programs)
                        shader_cache_write_record(f, kv.first, program_src_hash(gen_pixel_shader(kv.first).c_str()),
                                                  kv.second.prog);
                    fclose(f);
                }
            }
            fprintf(stderr, "[shaders] %d programs from cache (%d from binaries, %d compiled) in %.0f ms\n",
                    L.from_binary + L.compiled, L.from_binary, L.compiled, ms_since(L.t0));
            L.active = false;
            L.data.clear();
            L.data.shrink_to_fit();
        }
    }
    if (done) *done = L.done;
    if (total) *total = L.total;
    return !L.active;
}

// Called once from render_init.
static void shader_cache_load() {
    shader_cache_open();
    if (!g_cache_deferred) render_shader_cache_step(1e30, nullptr, nullptr);
}

static const Program& get_program(const ShaderKey& k) {
    auto it = g_programs.find(k);
    if (it != g_programs.end()) return it->second;
    const auto t0 = std::chrono::steady_clock::now();
    std::string src = gen_pixel_shader(k);
    GLuint p = build_program(k, src, nullptr, 0, 0);
    const Program& pr = register_program(k, p);
    if (g_frametime) fprintf(stderr, "[shaders] compiled program %zu in %.1f ms\n", g_programs.size(), ms_since(t0));
    if (!g_shader_cache_path.empty()) {
        // Appended rather than rewritten, so a crash later in the run keeps what was
        // learned so far. A missing or truncated file gets a header first.
        FILE* f = fopen(g_shader_cache_path.c_str(), "ab");
        if (f) {
            fseek(f, 0, SEEK_END);
            if (ftell(f) == 0) shader_cache_write_header(f);
            shader_cache_write_record(f, k, program_src_hash(src.c_str()), p);
            fclose(f);
        }
    }
    return pr;
}

static const char* kCopyVS = GCN_GLSL_VERSION R"(
out vec2 v_uv;
uniform vec4 u_rect;  // source rect in normalized EFB coords (x0,y0,x1,y1), y down
void main() {
    vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    v_uv = mix(u_rect.xy, u_rect.zw, p);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Converts EFB color (or depth) to the requested GX copy format. Output row 0 = top.
static const char* kCopyFS = GCN_GLSL_VERSION R"(
in vec2 v_uv;
uniform sampler2D u_src;
uniform sampler2D u_depth;
uniform int u_mode;   // copy texture format (+16 intensity, +32 depth)
out vec4 o;
void main() {
    vec2 uv = vec2(v_uv.x, 1.0 - v_uv.y);
    vec4 c = texture(u_src, uv);
    if ((u_mode & 32) != 0) {
        float z = texture(u_depth, uv).r;
        uint zi = uint(z * 16777215.0 + 0.5);
        c = vec4(float((zi >> 16) & 255u), float((zi >> 8) & 255u), float(zi & 255u), 255.0) / 255.0;
        c = vec4(c.r, c.g, c.b, c.r);
    }
    int fmt = u_mode & 15;
    bool intensity = (u_mode & 16) != 0;
    if (intensity) {
        float y = clamp(dot(c.rgb, vec3(0.257, 0.504, 0.098)) + 16.0 / 255.0, 0.0, 1.0);
        c.rgb = vec3(y);
    }
    // Single-channel formats replicate into all components like the texture decoder does.
    if (fmt == 7) c = vec4(c.a);              // A8
    else if (fmt == 8) c = vec4(c.r);         // R8
    else if (fmt == 9) c = vec4(c.g);         // G8
    else if (fmt == 10) c = vec4(c.b);        // B8
    else if (fmt == 1 && !intensity) c = vec4(c.r);
    // The two-channel formats are read back as IA8: the first byte is the alpha and the
    // second the intensity. A 16-bit depth copy is RG8 -- the depth's top byte, then its
    // middle -- and a fog volume subtracts two of them a byte at a time; handed the colour
    // channels as they were, it borrowed from the wrong byte and drew a line wherever
    // the lower byte wrapped.
    else if (fmt == 11) c = vec4(c.g, c.g, c.g, c.r);   // RG8
    else if (fmt == 12) c = vec4(c.b, c.b, c.b, c.g);   // GB8
    if (fmt == 4) c.a = 1.0;                  // RGB565
    o = c;
}
)";

static const char* kBlitFS = GCN_GLSL_VERSION R"(
in vec2 v_uv;
uniform sampler2D u_src;
out vec4 o;
void main() { o = vec4(texture(u_src, vec2(v_uv.x, 1.0 - v_uv.y)).rgb, 1.0); }
)";

void render_init(int internal_scale) {
    g_scale = internal_scale < 1 ? 1
                                 : (internal_scale > kMaxInternalScale ? kMaxInternalScale
                                                                       : internal_scale);
    const std::string vs_src = gen_vertex_shader();
#ifdef GCN_GL_ES
    {
        GLint n = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        for (GLint i = 0; i < n && !g_clip_ok; i++) {
            const char* e = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
            g_clip_ok = e && !strcmp(e, "GL_EXT_clip_cull_distance");
        }
        if (!g_clip_ok)
            fprintf(stderr, "no GL_EXT_clip_cull_distance: the theater/stereo morph is uncropped\n");
    }
#else
    g_clip_ok = true;
#endif
    g_vs_hash = hash_str(vs_src.c_str());
    g_vs = compile(GL_VERTEX_SHADER, vs_src);
#if GCN_HAVE_PROGRAM_BINARY
    {
        GLint n = 0;
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &n);
        g_binaries_supported = n > 0;
    }
#endif
    shader_cache_load();

    glGenFramebuffers(1, &g_efb_fbo);
    glGenTextures(1, &g_efb_color);
    glBindTexture(GL_TEXTURE_2D, g_efb_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, EFB_W * g_scale, EFB_H * g_scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &g_efb_depth);
    glBindTexture(GL_TEXTURE_2D, g_efb_depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, EFB_W * g_scale, EFB_H * g_scale, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_efb_color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_efb_depth, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("EFB framebuffer incomplete");
    glClearColor(0, 0, 0, 1);
    clear_depth(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glGenVertexArrays(kVertexRing * 9, &g_vaos[0][0]);
    glGenBuffers(kVertexRing, g_vbos);
    glGenBuffers(kVertexRing, g_ebos);
    for (int r = 0; r < kVertexRing; r++)
        for (uint32_t n = 0; n < 9; n++) {
            glBindVertexArray(g_vaos[r][n]);
            glBindBuffer(GL_ARRAY_BUFFER, g_vbos[r]);
            // The element buffer binding is part of the VAO's state, so binding it once here
            // keeps it bound whenever this VAO is.
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ebos[r]);
            const GLsizei stride = (GLsizei)packed_stride(n);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(GpuVertex, pos));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)offsetof(GpuVertex, col[0]));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)offsetof(GpuVertex, col[1]));
            for (uint32_t i = 0; i < n; i++) {
                glEnableVertexAttribArray(3 + i);
                glVertexAttribPointer(3 + i, 3, GL_FLOAT, GL_FALSE, stride, (void*)(offsetof(GpuVertex, tex) + i * 12));
            }
        }
    // A coordinate past a draw's count reads the attribute's constant value instead, which
    // is what the transform writes into a GpuVertex's unused ones.
    for (int i = 0; i < 8; i++) glVertexAttrib4f(3 + i, 0.0f, 0.0f, 1.0f, 1.0f);
    g_vao = g_vaos[0][0];
    g_vbo = g_vbos[0];
    g_ebo = g_ebos[0];
    glBindVertexArray(g_vao);

    GLuint cvs = compile(GL_VERTEX_SHADER, kCopyVS);
    g_copy_prog = link(cvs, compile(GL_FRAGMENT_SHADER, kCopyFS));
    g_copy_u_src = glGetUniformLocation(g_copy_prog, "u_src");
    g_copy_u_depth = glGetUniformLocation(g_copy_prog, "u_depth");
    g_copy_u_rect = glGetUniformLocation(g_copy_prog, "u_rect");
    g_copy_u_mode = glGetUniformLocation(g_copy_prog, "u_mode");
    g_blit_prog = link(cvs, compile(GL_FRAGMENT_SHADER, kBlitFS));
    g_blit_u_src = glGetUniformLocation(g_blit_prog, "u_src");
    g_blit_u_rect = glGetUniformLocation(g_blit_prog, "u_rect");
    glGenVertexArrays(1, &g_copy_vao);
    glGenFramebuffers(1, &g_copy_fbo);
    {
        const std::string bvs = glsl_header_with_clip() + R"(
uniform mat4 u_mvp;
uniform mat4 u_crop;
uniform vec4 u_quad;   // half-width, half-height, view-space z, unused
void main() {
    vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1) * 2.0 - 1.0;
    vec4 a = vec4(c * u_quad.xy, u_quad.z, 1.0);
    gl_Position = u_mvp * a;
#ifdef GCN_CLIP
    vec4 cd = u_crop * a;
    gl_ClipDistance[0] = cd.x;
    gl_ClipDistance[1] = cd.y;
    gl_ClipDistance[2] = cd.z;
    gl_ClipDistance[3] = cd.w;
#endif
}
)";
        const std::string bfs = std::string(GCN_GLSL_VERSION) + R"(
uniform vec3 u_col;
out vec4 o;
void main() { o = vec4(u_col, 1.0); }
)";
        g_bg_prog = link(compile(GL_VERTEX_SHADER, bvs), compile(GL_FRAGMENT_SHADER, bfs));
        g_bg_u_mvp = glGetUniformLocation(g_bg_prog, "u_mvp");
        g_bg_u_crop = glGetUniformLocation(g_bg_prog, "u_crop");
        g_bg_u_quad = glGetUniformLocation(g_bg_prog, "u_quad");
        g_bg_u_col = glGetUniformLocation(g_bg_prog, "u_col");
    }
#ifndef GCN_GL_ES
    // Desktop core profile needs this to honour gl_PointSize; ES always does.
    glEnable(GL_PROGRAM_POINT_SIZE);
#endif
}

void render_set_window_size(int w, int h) { g_win_w = w; g_win_h = h; }

// Change how many samples the EFB keeps per hardware pixel, between frames.
//
// The two VR views want different answers. Theater shows the EFB itself, blown up to fill
// a panel wider than the frame was ever drawn for, so every extra sample is detail the
// viewer sees. Stereo never shows it: there it holds only what the eye passes sample out
// of it -- the water reflection, the sheet the spray is cut from -- and the pass that
// fills it is a whole extra scene render on top of the two eyes. So the scale follows the
// view, which means changing it while the game runs.
//
// Nothing in the EFB has to survive the change: it is cleared and redrawn every frame.
// The textures EFB copies land in do not get off so lightly -- they are allocated at
// dst * scale but matched for reuse by their logical size alone, so a survivor would be
// reused at the old resolution and quietly sample wrong. They go with the EFB.
static void gl_state_invalidate();
static void uniforms_invalidate();
void render_set_internal_scale(int scale) {
    if (scale < 1) scale = 1;
    if (scale > kMaxInternalScale) scale = kMaxInternalScale;
    if (scale == g_scale) return;
    g_scale = scale;
    gl_state_invalidate();
    uniforms_invalidate();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_efb_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, EFB_W * g_scale, EFB_H * g_scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, g_efb_depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, EFB_W * g_scale, EFB_H * g_scale, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    for (auto it = g_textures.begin(); it != g_textures.end();) {
        if (it->second.efb) {
            glDeleteTextures(1, &it->second.tex);
            it = g_textures.erase(it);
        } else {
            ++it;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fatal("EFB framebuffer incomplete at internal scale %d", g_scale);
}

// ---------------------------------------------------------------------------
static void upload_texture(const TexData& t) {
    GlTex g{};
    glGenTextures(1, &g.tex);
    glBindTexture(GL_TEXTURE_2D, g.tex);
    g.w = t.width; g.h = t.height;
    uint32_t w = t.width, h = t.height;
    for (size_t l = 0; l < t.levels.size(); l++) {
        glTexImage2D(GL_TEXTURE_2D, (GLint)l, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.levels[l].data());
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)t.levels.size() - 1);
    g.levels = (uint32_t)t.levels.size();
    g.last_used = g_render_frame;
    g_textures[t.id] = g;
    // GCN_TEXDUMP=<dir> writes every texture's top level as <dir>/tex_<id>.png, alpha
    // forced opaque, so a decoded texture can be looked at apart from the draw using it.
    static const char* dump = getenv("GCN_TEXDUMP");
    if (dump) {
        std::vector<uint8_t> px((size_t)t.width * t.height * 4);
        memcpy(px.data(), t.levels[0].data(), px.size());
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        char path[512];
        snprintf(path, sizeof(path), "%s/tex_%u.png", dump, t.id);
        write_png(path, px.data(), (int)t.width, (int)t.height);
    }
}

static GLuint get_sampler(uint32_t mode0, uint32_t mode1, uint32_t levels) {
    uint32_t key = (mode0 & 0x3FFFFF) ^ (mode1 << 22) ^ (levels << 30);
    auto it = g_samplers.find(key);
    if (it != g_samplers.end()) return it->second;
    GLuint s;
    glGenSamplers(1, &s);
    static const GLenum wrap[4] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_REPEAT};
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, wrap[mode0 & 3]);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, wrap[(mode0 >> 2) & 3]);
    bool mag_lin = (mode0 >> 4) & 1;
    uint32_t minf = (mode0 >> 5) & 7;
    bool min_lin = minf & 4;
    uint32_t mip = minf & 3;
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, mag_lin ? GL_LINEAR : GL_NEAREST);
    GLenum mf;
    if (!mip || levels <= 1) mf = min_lin ? GL_LINEAR : GL_NEAREST;
    else if (mip == 1) mf = min_lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    else mf = min_lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, mf);
    int8_t bias = (int8_t)((mode0 >> 9) & 0xFF);
    set_lod_bias(s, bias / 32.0f);
    glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, (mode1 & 0xFF) / 16.0f);
    glSamplerParameterf(s, GL_TEXTURE_MAX_LOD, ((mode1 >> 8) & 0xFF) / 16.0f);
    g_samplers[key] = s;
    return s;
}

static inline int32_t sx11(uint32_t v) { return (int32_t)(v << 21) >> 21; }

static float fog_float(uint32_t v) {
    uint32_t mant = v & 0x7FF, exp = (v >> 11) & 0xFF, sign = (v >> 19) & 1;
    uint32_t bits = (sign << 31) | (exp << 23) | (mant << 12);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static const GLenum kBlendSrc[8] = {GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
static const GLenum kBlendDst[8] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
static const GLenum kDepthFunc[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
static bool samples_fullscreen_copy(const PixelState& st);

// The origin that the scissor box and the viewport are both measured from, in EFB pixels.
//
// GX stores it halved and biased by 342, so GXSetScissorBoxOffset(0, 0) reads back as 342.
// This game sets (-2, -2) -- the register reads 340 -- and moves every viewport by the same
// (-2, -2), so on hardware the two cancel and the picture sits at the EFB's origin.
// Assuming 342 instead left every pass two pixels up and left of where the game believes
// it is. That shows wherever the game samples a copy of the EFB at screen coordinates it
// worked out itself: the water looked its refraction up two pixels out of register, which
// stood a water-tinted second copy of the racer beside the real one, and the last two
// columns and rows of every pass were never drawn.
static void scissor_offset(const uint32_t* bp, int& xoff, int& yoff) {
    xoff = (int)(bp[0x59] & 0x3FF) * 2;
    yoff = (int)((bp[0x59] >> 10) & 0x3FF) * 2;
}

// c = a * b, column-major, element (row r, column k) at m[k * 4 + r].
static void mat4_mul(const float* a, const float* b, float* c) {
    for (int k = 0; k < 4; k++)
        for (int r = 0; r < 4; r++) {
            float sum = 0;
            for (int i = 0; i < 4; i++) sum += a[i * 4 + r] * b[k * 4 + i];
            c[k * 4 + r] = sum;
        }
}

// The water surface refracts by looking the finished frame up at the screen position the
// game computed for each of its vertices. In an eye those positions are the flat view's,
// and the copy is the flat view's too, so water the eye can see beyond the game's own
// 60-degree frustum samples off the edge of the copy and clamps -- the scene smeared down
// the sea in streaks, the racer among it.
//
// Nothing about that is fixable by moving the lookup around, because the data is not in
// the copy. So the eye grabs what it has drawn itself, which covers exactly what the eye
// can see, and the fragment samples it at its own position rather than at the flat view's.
// The indirect stage that ripples the lookup still applies on top, so the water keeps its
// wobble. Costs one full-target copy per eye, taken only on a frame that has such a
// draw -- and the spray wants the same thing at a later moment, so a frame throwing
// spray pays for two. See is_grab_tex().
static void grab_eye(EyeGrab& g, int w, int h) {
    if (!g.tex || g.w != w || g.h != h) {
        if (!g.tex) glGenTextures(1, &g.tex);
        glBindTexture(GL_TEXTURE_2D, g.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g.w = w;
        g.h = h;
    } else {
        glBindTexture(GL_TEXTURE_2D, g.tex);
    }
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
}

// The spray is composited from screen-space grabs -- the same mechanism as the water
// surface, one size down. At speed it lifts some fifty small rects out of the live scene
// and redraws each as a billboard sampling its own rect, so on hardware a droplet
// refracts exactly the pixels it covers and its edges cannot be seen. An eye re-projects
// the billboard but not the rect, which belongs to the flat view, so the patch agrees
// with nothing behind it and reads as a square with a piece of scene in it.
//
// So these lookups are taken over the way the water's are: the texture becomes the eye's
// own grab and the coordinate becomes the fragment's own position, which is the one place
// the pixels the droplet wants actually are. What survives of the game's own lookup is
// the indirect offset that distorts it -- the droplet's whole visible character, since
// the undistorted part is by construction the background it sits on.
//
// Unlike the whole-frame copies these go to addresses that rotate, so a fresh texture id
// appears for every sprite of every frame; a set of ids would grow without bound and be
// scanned per draw. The flag rides on the texture instead.
static bool is_grab_tex(uint32_t id) {
    // GCN_EYE_SPRAY=0 restores the stale lookup, which is the only way to see from inside
    // the headset what this changed.
    static const bool off = getenv("GCN_EYE_SPRAY") && atoi(getenv("GCN_EYE_SPRAY")) == 0;
    if (off || !id) return false;
    auto it = g_textures.find(id);
    return it != g_textures.end() && it->second.grab;
}

// Whether an eye takes this texture's lookups over at all: a copy of the whole frame
// (the water surface samples one) or one of the spray's grabs.
static bool is_eye_screen_tex(uint32_t id) {
    return (g_eye_grab.tex && is_fullscreen_tex(id)) || (g_spray_grab.tex && is_grab_tex(id));
}

// What apply_state last put into GL, so that it can put in only what differs.
//
// A race frame applies around a thousand states, and measured over a million consecutive
// pairs, 94% of them differ in a texture -- 1.1 texmaps on average -- while the projection,
// viewport, scissor, fog, indirect matrices and point size are the same more than 99% of
// the time, the program 81%, the TEV registers 76%, blend and depth 90%. Re-issuing the
// forty-odd GL calls of a full application for every one of those was most of the
// renderer's CPU time; the typical application is now a texture bind and its size.
//
// Uniforms belong to a program, so they are only skipped while the program is the one they
// were last uploaded to. Bindings and fixed-function state are global and skipped on their
// own terms. The shadow holds within one pass: an EFB copy, a present or an eye's grab
// changes GL state behind it, and so does the start of a new pass, whose eye matrices
// differ -- each of those invalidates it.
// What apply_state last put into GL, so that it can put in only what differs.
//
// A race frame applies around a thousand states, and measured over a million consecutive
// pairs, 94% of them differ in a texture -- 1.1 texmaps on average -- while the projection,
// viewport, scissor, fog, indirect matrices and point size are the same more than 99% of
// the time, the program 81%, the TEV registers 76%, blend and depth 90%. Re-issuing the
// forty-odd GL calls of a full application for every one of those was most of the
// renderer's CPU time; the typical application is now a texture bind and its size.
//
// This holds the bindings and the fixed-function state, which are global to the context.
// The uniforms are kept with their program instead (UniformShadow): a uniform belongs to
// its program and keeps its value across switches, so a program switched back to still
// holds what it was last given, and a group whose inputs match is left alone whether or not
// the program changed. Before that, every switch re-uploaded every group -- a dozen and more
// calls -- and a frame here switched programs one draw in six. The shadow holds within one
// pass: an EFB copy, a present or an eye's grab changes GL state behind it, and so does the
// start of a new pass, whose eye matrices differ -- each of those invalidates it.
struct AppliedState {
    bool valid = false;
    ShaderKey key;
    const Program* pr = nullptr;
    uint32_t bp[256];
    uint32_t tex_id[8];
    uint8_t tex_sub[8];       // the unit holds an eye grab, not the game's texture
    GLuint sampler[8];
    int prim;
    bool hud_depth;
};
static AppliedState g_applied;
// What GL's face culling is set to: -1 unknown, 0 off, 1 front, 2 back, 3 both. Kept apart
// from g_applied because GL culls polygons only: a line or a point leaves it as it is, so a
// game alternating filled faces with outlines -- a 3D map, thousands of draws a frame -- need
// not turn culling off and on again around each outline. Those toggles were a quarter of
// the render thread's time on such a screen on a Quest 3.
static int g_gl_cull = -1;
static void gl_state_invalidate() {
    g_applied.valid = false;
    g_gl_cull = -1;
}

// A program's uniform record is current while its `gen` matches g_uniform_gen, and the
// groups that depend on the pass -- the eye's matrices, the morph, the eye target's size,
// whether this is the flat view at all -- while `view_gen` matches g_view_gen as well.
// Nothing inside the renderer touches a program's uniforms except apply_state, so the
// first counter moves only for the EFB scale (the point size is in EFB pixels), and the
// second at the start of every pass and whenever the eye is placed.
static uint32_t g_uniform_gen = 1, g_view_gen = 1;
static void uniforms_invalidate() { g_uniform_gen++; }
static void view_invalidate() { g_view_gen++; }

// GCN_APPLYSTATS=1: which groups of state apply_state actually had to send, summed over a
// pass, printed every 64th frame. What a game changes between draws decides what a state
// application costs, and this is how to see it.
static const bool g_applystats = getenv("GCN_APPLYSTATS") != nullptr;
static struct ApplyStats {
    uint32_t applies, program, proj, viewport, psize, tev, aref, ind, indcs, fog, screen, ripple,
        texbind, sampbind, tsz, blend, depth, cull, scissor;
} g_astat;
static void apply_stats_flush(const char* pass, uint32_t frame) {
    if (!g_applystats) return;
    if ((frame & 63) == 0) {
        const ApplyStats& a = g_astat;
        fprintf(stderr, "[apply] f%u %s applies %u: prog %u proj %u vp %u psz %u tev %u aref %u ind %u "
                "indcs %u fog %u scr %u rip %u tex %u samp %u tsz %u blend %u depth %u cull %u sciss %u\n",
                frame, pass, a.applies, a.program, a.proj, a.viewport, a.psize, a.tev, a.aref, a.ind,
                a.indcs, a.fog, a.screen, a.ripple, a.texbind, a.sampbind, a.tsz, a.blend, a.depth, a.cull,
                a.scissor);
    }
    memset(&g_astat, 0, sizeof(g_astat));
}

static void apply_state(const PixelState& st, int prim) {
    int xoff, yoff;
    scissor_offset(st.bp, xoff, yoff);
    const uint32_t* bp = st.bp;
    const uint32_t* abp = g_applied.bp;
    ShaderKey key = make_shader_key(st);
    g_astat.applies++;
    // `same` means the previous application is still in GL and the program is unchanged.
    const bool same = g_applied.valid && key == g_applied.key;
    const Program& pr = same ? *g_applied.pr : get_program(key);
    if (!same) {
        g_astat.program++;
        glUseProgram(pr.prog);
        g_applied.key = key;
        g_applied.pr = &pr;
    }
    // What this program was last given; see UniformShadow. A group is uploaded when its
    // inputs differ from the record, or the record is not current.
    UniformShadow& u = pr.shadow;
    const bool held = u.gen == g_uniform_gen;
    const bool view_held = held && u.view_gen == g_view_gen;
    auto held_words = [&](const uint32_t* a, const uint32_t* b, size_t n) {
        return held && memcmp(a, b, n * sizeof(uint32_t)) == 0;
    };

    // Projection. In VR a perspective batch is world geometry and gets the eye's
    // projection instead of the game's; an orthographic one is a 2D element and goes on
    // the HUD frame -- and so does a perspective batch the game placed in view space
    // itself, which is 3D but is no more part of the course than the lap counter is.
    // That is the countdown light rig: left in the world it stands in the water between
    // the viewer and the racer, because the game means it to hang in front of the camera.
    float P[16] = {0};
    const float* p = st.proj;
    const bool perspective = (int)p[6] == 0;
    // Something the game placed in view space but drew with a perspective frustum is a 3D
    // object held in front of the camera, not a 2D overlay. Painting it on the HUD frame
    // flattens it -- that frame's Z column is zero, so every vertex lands on one plane --
    // which throws away the object's own depth. The countdown rig lost the occlusion that
    // hides the lamp behind its lens, and the lamps showed through as white squares.
    //
    // So it keeps the eye's projection and stays 3D. It takes g_vr_view rather than
    // g_vr_view_world: the world pitch is taken out of the *world* to level the sea, and
    // applying it to something attached to the camera is what tilted the rig by 23
    // degrees. Without the correction it hangs in front of the viewer the way the game
    // means it to, and the head still moves within that.
    //
    // GCN_EYE_VS3D=0 puts it back on the HUD frame, which is where it was.
    static const bool vs3d = !(getenv("GCN_EYE_VS3D") && atoi(getenv("GCN_EYE_VS3D")) == 0);
    const bool view_space_3d = perspective && st.view_space && vs3d;
    const bool on_hud_frame = (!perspective || st.view_space) && !view_space_3d;
    if (perspective) {
        P[0] = p[0]; P[8] = p[1]; P[5] = p[2]; P[9] = p[3]; P[10] = p[4]; P[14] = p[5]; P[11] = -1.0f;
    } else {
        P[0] = p[0]; P[12] = p[1]; P[5] = p[2]; P[13] = p[3]; P[10] = p[4]; P[14] = p[5]; P[15] = 1.0f;
    }
    // Depth layers. GX has no depth-range call, but a viewport carries a z range, and a game
    // can confine a draw to a band of the depth buffer with it -- a sky kept behind the
    // world whatever its geometry's real distance, a weapon kept in front of it. An eye
    // keeps those bands (see the vertex shader). What it cannot keep is the illusion: a sky
    // modelled a short way out around the camera has the disparity of something near, and
    // a weapon modelled large and far off reads as large in two eyes. So a game can name
    // its background and foreground bands (render_set_depth_layers): the background is
    // drawn at infinity, turned with the head and the same in both eyes, and the
    // foreground is scaled towards the camera. GCN_EYE_SKY=0 turns the background off.
    static const bool sky_at_infinity = !(getenv("GCN_EYE_SKY") && atoi(getenv("GCN_EYE_SKY")) == 0);
    const float band_lo = (st.viewport[5] - fabsf(st.viewport[2])) / 16777215.0f;
    const float band_hi = st.viewport[5] / 16777215.0f;
    const bool background = sky_at_infinity && g_bg_from > 0.0f && perspective && !st.view_space &&
                            band_lo >= g_bg_from;
    const bool foreground = g_fg_to > 0.0f && g_fg_scale != 1.0f && perspective && band_hi <= g_fg_to;
    // A foreground layer also keeps the game's own depth within its band (u_vr 4 in the
    // vertex shader), whether or not it is scaled.
    const bool fg_layer = g_fg_to > 0.0f && perspective && band_hi <= g_fg_to;
    const bool same_proj = view_held && memcmp(st.proj, u.proj, sizeof(st.proj)) == 0 &&
                           st.view_space == u.view_space && background == u.background &&
                           foreground == u.foreground && fg_layer == u.fg_layer;
    // Note: a draw sampling a copy of the whole frame (the water surface is one) must
    // stay in the world, however tempting its screen-space origin makes the overlay path
    // look. Sending the water through it put the water, and the racer baked into the
    // copy, on a flat panel hanging in front of the camera while the real racer went on
    // moving in the world. The seam at the billboard's edge is the lesser problem.
    if (!same_proj) {
        g_astat.proj++;
        if (g_vr_active && g_morph < 1.0f) {
            // Part way between theater and stereo: see morph_chain.
            float M[16], F[16], C[16];
            morph_chain(P, on_hud_frame ? MorphKind::Hud
                           : view_space_3d ? MorphKind::CameraHeld : MorphKind::World,
                        M, F, C);
            glUniformMatrix4fv(pr.u_proj, 1, GL_FALSE, M);
            glUniformMatrix4fv(pr.u_view, 1, GL_FALSE, F);
            glUniformMatrix4fv(pr.u_crop, 1, GL_FALSE, C);
            glUniform1i(pr.u_vr, 3);
        } else if (g_vr_active && !on_hud_frame) {
            // A camera-placed 3D object is viewed with the head transform but without the
            // world's pitch correction; see view_space_3d above.
            const float* view = background ? g_vr_view_sky
                              : view_space_3d ? (foreground ? g_vr_view_fg : g_vr_view)
                              : (foreground ? g_vr_view_world_fg : g_vr_view_world);
            // GCN_EYE_GAMEPROJ keeps the game's own frustum and applies only the head
            // transform, which tells apart "the eye sees less than it should" from "the game
            // never drew anything out there".
            static const bool game_proj = getenv("GCN_EYE_GAMEPROJ") != nullptr;
            static bool logged = false;
            if (!logged && getenv("GCN_EYELOG")) {
                logged = true;
                fprintf(stderr, "[eye] game fov: x=%.1fdeg y=%.1fdeg (p0=%f p2=%f)\n",
                        2.0f * atanf(1.0f / p[0]) * 57.2958f, 2.0f * atanf(1.0f / p[2]) * 57.2958f,
                        p[0], p[2]);
            }
            glUniformMatrix4fv(pr.u_proj, 1, GL_FALSE, game_proj ? P : g_vr_proj);
            glUniformMatrix4fv(pr.u_view, 1, GL_FALSE, view);
            static const bool game_depth = !(getenv("GCN_EYE_FGDEPTH") && atoi(getenv("GCN_EYE_FGDEPTH")) == 0);
            glUniform1i(pr.u_vr, fg_layer && game_depth ? 4 : 1);
        } else if (g_vr_active) {
            // A HUD element in an eye. The game's own projection already puts its frame in
            // [-1,1], so that is where the chain picks up -- for the perspective rig too,
            // since the frame's matrix carries clip w through and the divide lands it on
            // the plane just the same.
            //
            // GX's viewport transform is deliberately not in the chain: it places the frame
            // within the 640x528 EFB, and an eye's render target is not the EFB -- the same
            // reason the scissor rect is dropped below. Including it would map the EFB
            // rather than the 480 lines the game displays, leaving the HUD a few per cent
            // small and off centre.
            //
            // Every term is constant for the draw, so the whole chain folds into one matrix
            // here and the shader is left with a single multiply.
            float a[16], b[16], M[16];
            mat4_mul(g_vr_hud_eff, P, a);   // the game's 2D frame, placed in view space
            mat4_mul(g_vr_view, a, b);  // that frame seen from this eye
            mat4_mul(g_vr_proj, b, M);
            glUniformMatrix4fv(pr.u_proj, 1, GL_FALSE, M);
            glUniform1i(pr.u_vr, 2);
        } else if (g_flat_eye != 0.0f && perspective &&
                   !(g_panel_band > 0.0f && band_hi > band_lo && band_hi <= g_panel_band)) {
            // One of a stereo pair on the panel (a draw in the panel band falls through to
            // the plain projection, and so sits on the panel). The view is moved g_flat_eye to the side,
            // which shifts every point by -P00*e in clip x, and the frustum is sheared back
            // by e/halfw per unit of depth, which cancels that at the depth where the
            // frustum is as wide as the panel (D = P00 * halfw): a point there sits on the
            // panel in both eyes, and the disparity of one at depth z is e * (1 - D/z) of
            // the panel's half-width -- the eyes' own separation at infinity. In the
            // vertex shader clip x is P00*x + P[8]*z + P[12], so both terms are in P.
            float Q[16];
            memcpy(Q, P, sizeof(Q));
            Q[12] = -P[0] * g_flat_eye;
            Q[8] -= g_flat_eye / g_flat_halfw;
            glUniformMatrix4fv(pr.u_proj, 1, GL_FALSE, Q);
            glUniform1i(pr.u_vr, 0);
        } else {
            glUniformMatrix4fv(pr.u_proj, 1, GL_FALSE, P);
            glUniform1i(pr.u_vr, 0);
        }
        // Fog runs off the depth GX would have written, which an eye does not write, so
        // the shader rebuilds it from the game's own projection -- the z row is all that
        // takes, plus how w is formed: -z under a frustum, 1 under an ortho batch.
        const float zp[4] = {p[4], p[5], perspective ? -1.0f : 0.0f, perspective ? 0.0f : 1.0f};
        glUniform4fv(pr.u_zproj, 1, zp);
        memcpy(u.proj, st.proj, sizeof(st.proj));
        u.view_space = st.view_space;
        u.background = background;
        u.foreground = foreground;
        u.fg_layer = fg_layer;
    }
    const float* vp = st.viewport;  // sx, sy, sz, ox, oy, oz
    if (!(held && bp[0x59] == u.scissor_off && memcmp(vp, u.viewport, sizeof(st.viewport)) == 0)) {
        g_astat.viewport++;
        float vpa[4] = {2.0f * (vp[3] - xoff) / EFB_W - 1.0f, 2.0f * vp[0] / EFB_W, 2.0f * (vp[4] - yoff) / EFB_H - 1.0f, 2.0f * vp[1] / EFB_H};
        float vpb[4] = {2.0f * vp[5] / 16777215.0f - 1.0f, 2.0f * vp[2] / 16777215.0f, 0, 0};
        glUniform4fv(pr.u_vp_a, 1, vpa);
        glUniform4fv(pr.u_vp_b, 1, vpb);
        memcpy(u.viewport, vp, sizeof(st.viewport));
        u.scissor_off = bp[0x59];
    }
    // Only points read the point size, and a game that sets a line width per draw changes
    // this register with every line: so it is sent when a draw of points needs it, and a
    // record that is not current forgets what it held rather than keep a value it never sent.
    if (prim == 2) {
        if (!(held && bp[0x22] == u.psize_reg)) {
            g_astat.psize++;
            float psize = ((bp[0x22] >> 8) & 0xFF) / 6.0f * g_scale;
            glUniform1f(pr.u_point_size, psize < 1 ? 1 : psize);
            u.psize_reg = bp[0x22];
        }
    } else if (!held) {
        u.psize_reg = ~0u;
    }

    // TEV registers
    if (!(held && memcmp(st.tev_reg, u.tev_reg, sizeof(st.tev_reg)) == 0 &&
          memcmp(st.tev_konst, u.tev_konst, sizeof(st.tev_konst)) == 0)) {
        g_astat.tev++;
        GLint regs[16], kon[16];
        for (int r = 0; r < 4; r++) {
            uint32_t ra = st.tev_reg[r][0], bg = st.tev_reg[r][1];
            regs[r * 4 + 0] = sx11(ra); regs[r * 4 + 3] = sx11(ra >> 12);
            regs[r * 4 + 2] = sx11(bg); regs[r * 4 + 1] = sx11(bg >> 12);
            uint32_t ka = st.tev_konst[r][0], kb = st.tev_konst[r][1];
            kon[r * 4 + 0] = ka & 0xFF; kon[r * 4 + 3] = (ka >> 12) & 0xFF;
            kon[r * 4 + 2] = kb & 0xFF; kon[r * 4 + 1] = (kb >> 12) & 0xFF;
        }
        glUniform4iv(pr.u_reg, 4, regs);
        glUniform4iv(pr.u_konst, 4, kon);
        memcpy(u.tev_reg, st.tev_reg, sizeof(st.tev_reg));
        memcpy(u.tev_konst, st.tev_konst, sizeof(st.tev_konst));
    }
    if (!(held && bp[0xF3] == u.aref)) {
        g_astat.aref++;
        GLint aref[2] = {(GLint)(bp[0xF3] & 0xFF), (GLint)((bp[0xF3] >> 8) & 0xFF)};
        glUniform2iv(pr.u_alpharef, 1, aref);
        u.aref = bp[0xF3];
    }
    // Indirect matrices
    if (!held_words(bp + 0x06, u.ind, 9)) {
        g_astat.ind++;
        GLint im[18];
        GLint isc[3];
        for (int m = 0; m < 3; m++) {
            uint32_t r0 = bp[0x06 + 3 * m], r1 = bp[0x07 + 3 * m], r2 = bp[0x08 + 3 * m];
            im[m * 6 + 0] = sx11(r0); im[m * 6 + 1] = sx11(r1); im[m * 6 + 2] = sx11(r2);
            im[m * 6 + 3] = sx11(r0 >> 11); im[m * 6 + 4] = sx11(r1 >> 11); im[m * 6 + 5] = sx11(r2 >> 11);
            isc[m] = (int)(((r0 >> 22) & 3) | (((r1 >> 22) & 3) << 2) | (((r2 >> 22) & 3) << 4)) - 17;
        }
        glUniform3iv(pr.u_indmtx, 6, im);
        glUniform1iv(pr.u_indscale, 3, isc);
        memcpy(u.ind, bp + 0x06, sizeof(u.ind));
    }
    if (!held_words(bp + 0x25, u.indcs, 2)) {
        g_astat.indcs++;
        float ics[8];
        for (int i = 0; i < 4; i++) {
            uint32_t ss = bp[0x25 + i / 2] >> ((i & 1) * 8);
            ics[i * 2] = 1.0f / (float)(1u << (ss & 15));
            ics[i * 2 + 1] = 1.0f / (float)(1u << ((ss >> 4) & 15));
        }
        glUniform2fv(pr.u_indcoordscale, 4, ics);
        memcpy(u.indcs, bp + 0x25, sizeof(u.indcs));
    }
    // Fog
    if (!held_words(bp + 0xEE, u.fog, 5)) {
        g_astat.fog++;
        float fog[4] = {fog_float(bp[0xEE]), fog_float(bp[0xF1]), (float)(bp[0xEF] & 0xFFFFFF), (float)(bp[0xF0] & 0x1F)};
        glUniform4fv(pr.u_fog, 1, fog);
        float fogc[3] = {((bp[0xF2] >> 16) & 0xFF) / 255.0f, ((bp[0xF2] >> 8) & 0xFF) / 255.0f, (bp[0xF2] & 0xFF) / 255.0f};
        glUniform3fv(pr.u_fogcolor, 1, fogc);
        memcpy(u.fog, bp + 0xEE, sizeof(u.fog));
    }

    // Which texgens feed a copy of the whole frame. In an eye those lookups are taken
    // over: the texture becomes the eye's own grab and the coordinate becomes the
    // fragment's own position, since the game's coordinate belongs to a view this eye is
    // not looking from. See grab_eye().
    uint32_t screen_uv = 0;
    if (g_vr_active) {
        const uint32_t nstg = ((bp[0x00] >> 10) & 15) + 1;
        for (uint32_t s = 0; s < nstg; s++) {
            const uint32_t order = bp[0x28 + s / 2] >> ((s & 1) * 12);
            const uint32_t map = order & 7;
            if ((order & 0x40) && st.tex_is_efb[map] && st.tex_id[map] &&
                is_eye_screen_tex(st.tex_id[map]))
                screen_uv |= 1u << ((order >> 3) & 7);
        }
    }
    if (!(view_held && screen_uv == u.screen_uv)) {
        g_astat.screen++;
        glUniform1i(pr.u_screen_uv, (GLint)screen_uv);
        glUniform2f(pr.u_screen_px, g_eye_w ? 1.0f / (float)g_eye_w : 0.0f,
                    g_eye_h ? 1.0f / (float)g_eye_h : 0.0f);
        u.screen_uv = screen_uv;
    }

    // The indirect offset that distorts a substituted lookup is an absolute displacement
    // in the copy's texels, and the copy's texels are EFB pixels. The eye grab's are not:
    // the eye sees a wider field across more pixels, so one of its pixels covers a
    // different angle. Both grids are pixels over an angle, so the conversion is the
    // ratio of their pixels per unit of frustum tangent -- the game's from its own
    // viewport and projection, the eye's from the target size and the headset's.
    //
    // Without it the droplets keep a displacement of a few pixels on a target several
    // times the EFB's width, which is a fraction of the distortion the game asked for:
    // each one degenerates into an almost exact copy of its own background and the spray
    // disappears rather than reading wrongly.
    //
    // The y term is negated because the two grids run opposite ways. An EFB copy's row 0
    // is the top of its source rect, so +t walks down the screen; the eye grab comes
    // straight off the render target, so its row 0 is the bottom and +t walks up -- which
    // is also the direction gl_FragCoord.y counts, and why the undistorted part of the
    // lookup needs no flip of its own.
    float ripple[2] = {1.0f, 1.0f};
    // GCN_EYE_RIPPLE=0 keeps the substitution but leaves the offset unconverted, which is
    // what the water surface shipped with and separates "the lookup is in the wrong
    // space" from "the distortion is the wrong size".
    static const bool no_ripple = getenv("GCN_EYE_RIPPLE") && atoi(getenv("GCN_EYE_RIPPLE")) == 0;
    if (screen_uv && perspective && !no_ripple) {
        const float gx = fabsf(vp[0]) * p[0], gy = fabsf(vp[1]) * p[2];
        if (gx > 0.0f) ripple[0] = 0.5f * (float)g_eye_w * g_vr_proj[0] / gx;
        if (gy > 0.0f) ripple[1] = -0.5f * (float)g_eye_h * g_vr_proj[5] / gy;
        // Mid-morph the scene is partly on the panel, where a unit of the game's frustum
        // tangent spans the panel's (half-width / distance) * p rather than one -- so the
        // eye's pixels cover a different share of it.
        if (g_morph < 1.0f) {
            const float D = -g_panel[14], k = g_morph;
            ripple[0] *= (1.0f - k) * p[0] * g_panel[0] / D + k;
            ripple[1] *= (1.0f - k) * p[2] * g_panel[5] / D + k;
        }
    }
    if (!(view_held && ripple[0] == u.ripple[0] && ripple[1] == u.ripple[1])) {
        g_astat.ripple++;
        glUniform2fv(pr.u_screen_ripple, 1, ripple);
        u.ripple[0] = ripple[0];
        u.ripple[1] = ripple[1];
    }

    // Textures. The bindings are global, so a unit already holding this texture under
    // this sampler is left alone whatever the program; the sizes are a uniform. The
    // texture and the sampler are bound separately: a frame here changed the texture on
    // a unit at nearly every draw and the sampler almost never.
    float tsz[16];
    for (int m = 0; m < 8; m++) {
        tsz[m * 2] = tsz[m * 2 + 1] = 1.0f;
        uint32_t id = st.tex_id[m];
        const bool sub = screen_uv && st.tex_is_efb[m] && id && is_eye_screen_tex(id);
        GLuint sampler = 0;
        const GlTex* gt = nullptr;
        if (sub) {
            const EyeGrab& g = is_grab_tex(id) ? g_spray_grab : g_eye_grab;
            tsz[m * 2] = (float)g.w;
            tsz[m * 2 + 1] = (float)g.h;
        } else if (id) {
            auto it = g_textures.find(id);
            if (it != g_textures.end()) {
                gt = &it->second;
                it->second.last_used = g_render_frame;
                uint32_t base = m < 4 ? 0x80 + m : 0xA0 + (m - 4);
                uint32_t img0 = bp[base + 8];
                tsz[m * 2] = (float)((img0 & 0x3FF) + 1);
                tsz[m * 2 + 1] = (float)(((img0 >> 10) & 0x3FF) + 1);
                const uint32_t mode0 = gt->depth ? bp[base] & ~0xF0u : bp[base];  // see GlTex::depth
                sampler = get_sampler(mode0, bp[base + 4], gt->efb ? 1 : gt->levels);
            } else {
                id = 0;
            }
        }
        const bool tex_held = g_applied.valid && g_applied.tex_id[m] == id && g_applied.tex_sub[m] == sub;
        const bool samp_held = g_applied.valid && g_applied.sampler[m] == sampler;
        if (tex_held && samp_held) continue;
        glActiveTexture(GL_TEXTURE0 + m);
        if (!tex_held) {
            g_astat.texbind++;
            if (sub) glBindTexture(GL_TEXTURE_2D, (is_grab_tex(id) ? g_spray_grab : g_eye_grab).tex);
            else glBindTexture(GL_TEXTURE_2D, gt ? gt->tex : 0);
            g_applied.tex_id[m] = id;
            g_applied.tex_sub[m] = sub;
        }
        if (!samp_held) {
            // The grab carries its own filtering; a sampler object would override it, so
            // a substituted unit gets none (0), as does an empty one.
            g_astat.sampbind++;
            glBindSampler(m, sampler);
            g_applied.sampler[m] = sampler;
        }
    }
    if (!(held && memcmp(tsz, u.tsz, sizeof(tsz)) == 0)) {
        g_astat.tsz++;
        glUniform2fv(pr.u_texsize, 8, tsz);
        memcpy(u.tsz, tsz, sizeof(tsz));
    }
    u.gen = g_uniform_gen;
    u.view_gen = g_view_gen;
    // Blend / logic op, colour mask
    const bool same_blend = g_applied.valid && bp[0x41] == abp[0x41] && bp[0x43] == abp[0x43];
    uint32_t bm = bp[0x41];
    if (!same_blend) {
        g_astat.blend++;
        bool blend = bm & 1, logic = (bm >> 1) & 1, sub = (bm >> 11) & 1;
        if (sub) {
            glEnable(GL_BLEND);
            set_logic_op_off();
            glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
            glBlendFunc(GL_ONE, GL_ONE);
        } else if (blend) {
            glEnable(GL_BLEND);
            set_logic_op_off();
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(kBlendSrc[(bm >> 8) & 7], kBlendDst[(bm >> 5) & 7]);
        } else if (logic) {
            glDisable(GL_BLEND);
            set_logic_op(bm >> 12);
        } else {
            glDisable(GL_BLEND);
            set_logic_op_off();
        }
        bool has_alpha = (bp[0x43] & 7) == 1;
        GLboolean cw = (bm >> 3) & 1, aw = ((bm >> 4) & 1) && has_alpha;
        glColorMask(cw, cw, cw, aw);
    }
    // Depth. In an eye the HUD is a quad out in the world rather than something laid over
    // the finished image, so the game's depth state no longer places it: the scene it is
    // meant to sit over is mostly nearer than the frame, and every element of the HUD is
    // on one plane. It is submitted last, so submission order is the layering.
    const bool hud_depth = g_vr_active && on_hud_frame;
    if (!(g_applied.valid && bp[0x40] == abp[0x40] && hud_depth == g_applied.hud_depth)) {
        g_astat.depth++;
        uint32_t zm = bp[0x40];
        if (zm & 1) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(kDepthFunc[(zm >> 1) & 7]);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
        glDepthMask((zm >> 4) & 1);
        if (hud_depth) {
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
        }
        g_applied.hud_depth = hud_depth;
    }
    // Cull
    uint32_t cull = (bp[0x00] >> 14) & 3;
    if (prim == 0) {
        static bool nocull = getenv("GCN_NOCULL") != nullptr;
        // GX: 1 = cull front, 2 = cull back. GX front faces are clockwise with y down,
        // which is counter-clockwise after our y flip (GL's default front face).
        const int want = (cull == 0 || nocull) ? 0 : cull == 3 ? 3 : ((cull == 2) != g_cull_swap ? 2 : 1);
        if (want != g_gl_cull) {
            g_astat.cull++;
            if (!want) {
                glDisable(GL_CULL_FACE);
            } else {
                glEnable(GL_CULL_FACE);
                glCullFace(want == 3 ? GL_FRONT_AND_BACK : want == 2 ? GL_BACK : GL_FRONT);
            }
            g_gl_cull = want;
        }
    }
    g_applied.prim = prim;
    // Scissor (EFB coords, y down), measured from the same origin as the viewport.
    const bool same_scissor = g_applied.valid && bp[0x20] == abp[0x20] && bp[0x21] == abp[0x21] && bp[0x59] == abp[0x59];
    if (!same_scissor) {
        g_astat.scissor++;
        int x0 = (int)(bp[0x20] >> 12 & 0x7FF) - xoff, y0 = (int)(bp[0x20] & 0x7FF) - yoff;
        int x1 = (int)(bp[0x21] >> 12 & 0x7FF) - xoff + 1, y1 = (int)(bp[0x21] & 0x7FF) - yoff + 1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > EFB_W) x1 = EFB_W;
        if (y1 > EFB_H) y1 = EFB_H;
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;
        if (g_vr_active) {
            // The scissor rect is in EFB coordinates, which say nothing about an eye's
            // render target.
            //
            // GCN_EYE_SCISSORLOG reports the draws whose rect is not the whole frame, which
            // are the ones this is throwing away clipping for. Anything the game relies on
            // the scissor to hide is drawn in full in an eye.
            static const bool slog = getenv("GCN_EYE_SCISSORLOG") != nullptr;
            if (slog && (x0 > 0 || y0 > 0 || x1 < EFB_W || y1 < EFB_H)) {
                static int shown;
                if (shown++ < 40)
                    fprintf(stderr, "[scissor] f%u rect=%d,%d..%d,%d (frame is 0,0..%d,%d)\n",
                            g_render_frame, x0, y0, x1, y1, EFB_W, EFB_H);
            }
            glDisable(GL_SCISSOR_TEST);
        } else {
            glEnable(GL_SCISSOR_TEST);
            glScissor(x0 * g_scale, (EFB_H - y1) * g_scale, (x1 - x0) * g_scale, (y1 - y0) * g_scale);
        }
    }
    // Everything above has now been compared against bp, so it becomes the reference.
    memcpy(g_applied.bp, bp, sizeof(g_applied.bp));
    g_applied.valid = true;
}

// A copy the spray composites from. It neither scans out nor clears -- a clearing copy
// ends an off-screen pass, and a grab deliberately leaves the scene it lifted from intact
// -- and it is small: at speed the spray takes 32x32 and 64x64 rects, while the other
// partial copies in this game (the water reflection, the rect the submerged tint samples)
// are far larger, which is what keeps them out of this. See is_grab_tex().
static constexpr uint32_t kGrabMax = 64;
static bool is_grab_copy(const EfbCopyCmd& c) {
    return !c.to_xfb && !c.clear && c.dst_w <= kGrabMax && c.dst_h <= kGrabMax;
}

static void do_efb_copy(const EfbCopyCmd& c) {
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    set_logic_op_off();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    g_gl_cull = 0;
    glColorMask(1, 1, 1, 1);
    if (!c.to_xfb && c.tex_id) {
        // The same target redrawn each frame keeps its id, so reuse the texture it
        // already has rather than allocating and freeing one per copy per frame.
        GlTex t{};
        auto old = g_textures.find(c.tex_id);
        const bool reuse = old != g_textures.end() && old->second.efb &&
                           old->second.w == c.dst_w && old->second.h == c.dst_h;
        if (reuse) {
            t = old->second;
            glBindTexture(GL_TEXTURE_2D, t.tex);
        } else {
            glGenTextures(1, &t.tex);
            t.w = c.dst_w; t.h = c.dst_h; t.levels = 1; t.efb = true;
            glBindTexture(GL_TEXTURE_2D, t.tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, c.dst_w * g_scale, c.dst_h * g_scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        }
        t.last_used = g_render_frame;
        // Set after the reuse path, which carries the previous copy's flags in.
        t.grab = is_grab_copy(c);
        t.depth = c.depth;
        glBindFramebuffer(GL_FRAMEBUFFER, g_copy_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
        glViewport(0, 0, c.dst_w * g_scale, c.dst_h * g_scale);
        glUseProgram(g_copy_prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_efb_color);
        glBindSampler(0, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_efb_depth);
        glBindSampler(1, 0);
        glUniform1i(g_copy_u_src, 0);
        glUniform1i(g_copy_u_depth, 1);
        // Destination texel row 0 = top of the source rect.
        float rect[4] = {(float)c.src_x / EFB_W, (float)c.src_y / EFB_H,
                         (float)(c.src_x + c.src_w) / EFB_W, (float)(c.src_y + c.src_h) / EFB_H};
        glUniform4fv(g_copy_u_rect, 1, rect);
        glUniform1i(g_copy_u_mode, (int)c.format | (c.depth ? 32 : 0));
        glBindVertexArray(g_copy_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (!reuse && old != g_textures.end()) glDeleteTextures(1, &old->second.tex);
        g_textures[c.tex_id] = t;
        glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
        glBindVertexArray(g_vao);
    }
    if (c.clear) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
        glEnable(GL_SCISSOR_TEST);
        glScissor(c.src_x * g_scale, (EFB_H - c.src_y - c.src_h) * g_scale, c.src_w * g_scale, c.src_h * g_scale);
        GLbitfield bits = 0;
        glColorMask(c.clear_color, c.clear_color, c.clear_color, c.clear_alpha);
        if (c.clear_color || c.clear_alpha) bits |= GL_COLOR_BUFFER_BIT;
        if (c.clear_z) { bits |= GL_DEPTH_BUFFER_BIT; glDepthMask(GL_TRUE); }
        glClearColor(((c.clear_rgba >> 24) & 0xFF) / 255.0f, ((c.clear_rgba >> 16) & 0xFF) / 255.0f,
                     ((c.clear_rgba >> 8) & 0xFF) / 255.0f, (c.clear_rgba & 0xFF) / 255.0f);
        clear_depth(c.clear_z_value / 16777215.0f);
        if (bits) glClear(bits);
        glColorMask(1, 1, 1, 1);
    }
}

const char* g_dump_dir = nullptr;
int g_dump_every = 0;
static uint32_t g_present_count;
// The second pass of a stereo pair (render_execute_stereo_pair) presents the same frame
// again: it is not counted twice, and its dump is told apart by a suffix.
static bool g_pair_second = false;
static const char* g_dump_suffix = "";

static void dump_efb(const EfbCopyCmd& c) {
    int w = c.src_w * g_scale, h = c.src_h * g_scale;
    std::vector<uint8_t> px((size_t)w * h * 4), flipped((size_t)w * h * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_efb_fbo);
    glReadPixels(c.src_x * g_scale, (EFB_H - c.src_y - c.src_h) * g_scale, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < h; y++) {
        memcpy(&flipped[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
        for (int x = 0; x < w; x++) flipped[((size_t)y * w + x) * 4 + 3] = 255;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/frame_%05u%s.png", g_dump_dir, g_present_count, g_dump_suffix);
    write_png(path, flipped.data(), w, h);
}

// Where the finished frame is blitted. 0 is the window's framebuffer; a VR frontend
// points this at one of its swapchain images instead.
static GLuint g_output_fbo = 0;
// The most recent presented rect, so the output can be refreshed without the game
// having produced a new frame (a VR compositor wants one every display frame, which is
// far more often than this game renders).
static void blit_to_output(const EfbCopyCmd& c, GLuint src_tex) {
    glBindFramebuffer(GL_FRAMEBUFFER, g_output_fbo);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    set_logic_op_off();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    g_gl_cull = 0;
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, g_win_w, g_win_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    // Letterbox to 4:3
    int w = g_win_w, h = g_win_h;
    int vw = w, vh = w * 3 / 4;
    if (vh > h) { vh = h; vw = h * 4 / 3; }
    glViewport((w - vw) / 2, (h - vh) / 2, vw, vh);
    glUseProgram(g_blit_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glBindSampler(0, 0);
    glUniform1i(g_blit_u_src, 0);
    float rect[4] = {(float)c.src_x / EFB_W, (float)(c.src_y + c.src_h) / EFB_H, (float)(c.src_x + c.src_w) / EFB_W, (float)c.src_y / EFB_H};
    glUniform4fv(g_blit_u_rect, 1, rect);
    glBindVertexArray(g_copy_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(g_vao);
}

static void present(const EfbCopyCmd& c) {
    if (!g_pair_second) g_present_count++;
    // GCN_DUMP_RANGE=a-b restricts dumping to a window of presented frames, so a short
    // stretch can be captured every single frame. Flicker is only visible frame by frame.
    static int range_lo = -1, range_hi = -1;
    static bool range_parsed = false;
    if (!range_parsed) {
        range_parsed = true;
        if (const char* s = getenv("GCN_DUMP_RANGE")) {
            range_lo = atoi(s);
            const char* dash = strchr(s, '-');
            range_hi = dash ? atoi(dash + 1) : range_lo;
        }
    }
    const bool in_range = range_lo < 0 || ((int)g_present_count >= range_lo &&
                                           (int)g_present_count <= range_hi);
    if (g_dump_dir && g_dump_every && in_range && g_present_count % g_dump_every == 0) dump_efb(c);
    blit_to_output(c, g_efb_color);
}

void render_set_output_fbo(unsigned fbo) { g_output_fbo = (GLuint)fbo; }
uint32_t present_count() { return g_present_count; }


// Executes a batch. Returns true if it contained a Present.
// The camera looks down by `pitch`, so the world's up arrives at (0, cos, sin) in the
// vertices' own frame. Rotating about X by -pitch takes it back to (0, 1, 0), which is the
// headset's up, and the sea with it.
static void world_pitch_matrix(float R[16]) {
    const float c = cosf(g_world_pitch), s = sinf(g_world_pitch);
    memset(R, 0, 16 * sizeof(float));
    R[0] = 1.0f;
    R[5] = c;  R[6] = -s;
    R[9] = s;  R[10] = c;
    R[15] = 1.0f;
}

static void view_invalidate();
static void compose_world_view() {
    view_invalidate();
    mat4_mul(g_vr_view, g_world_xform, g_vr_view_world);
    memcpy(g_vr_view_sky, g_vr_view_world, sizeof(g_vr_view_sky));
    g_vr_view_sky[12] = g_vr_view_sky[13] = g_vr_view_sky[14] = 0.0f;
    const float S[16] = {g_fg_scale, 0, 0, 0, 0, g_fg_scale, 0, 0, 0, 0, g_fg_scale, 0, 0, 0, 0, 1};
    mat4_mul(g_vr_view, S, g_vr_view_fg);
    mat4_mul(g_vr_view_world, S, g_vr_view_world_fg);
    mat4_mul(g_hud_xform, g_vr_hud, g_vr_hud_eff);
}

static void hud_upright() {
    memset(g_hud_xform, 0, sizeof(g_hud_xform));
    g_hud_xform[0] = g_hud_xform[5] = g_hud_xform[10] = g_hud_xform[15] = 1.0f;
}

void render_set_world_pitch(float pitch_rad) {
    g_world_pitch = pitch_rad;
    if (!g_eye_overridden) world_pitch_matrix(g_world_xform);
    compose_world_view();
}

void render_set_depth_layers(float background_from, float foreground_to, float foreground_scale) {
    g_bg_from = background_from;
    g_fg_to = foreground_to;
    g_fg_scale = foreground_scale > 0.0f ? foreground_scale : 1.0f;
    compose_world_view();
}

void render_set_eye_hook(EyeHook hook) { g_eye_hook = hook; }

void render_set_first_person(bool on) {
    g_fp_on = on;
    if (!on) {
        g_eye_overridden = false;
        hud_upright();
        world_pitch_matrix(g_world_xform);
        compose_world_view();
    }
}

void render_set_vr_eye(const float proj[16], const float view[16], const float hud[16]) {
    memcpy(g_vr_proj, proj, sizeof(g_vr_proj));
    memcpy(g_vr_view, view, sizeof(g_vr_view));
    memcpy(g_vr_hud, hud, sizeof(g_vr_hud));
    compose_world_view();
}

// Where the eye stands and what it leaves out, for the batch about to be drawn: the chase
// camera with its pitch taken out, unless first person is on and the game's eye hook has
// somewhere better to put it (render_set_eye_hook). `skip` marks the off-screen passes, as
// mark_offscreen_passes leaves it.
static void eye_prepare(const Batch& b, const std::vector<uint8_t>& skip) {
    g_hide.clear();
    g_eye_overridden = false;
    hud_upright();
    EyeOverride o;
    if (g_fp_on && g_eye_hook && g_eye_hook(b, skip, g_render_frame, o)) {
        memcpy(g_world_xform, o.view_to_eye, sizeof(g_world_xform));
        memcpy(g_hud_xform, o.hud_to_eye, sizeof(g_hud_xform));
        g_hide.swap(o.hide);
        g_eye_overridden = true;
        return;
    }
    world_pitch_matrix(g_world_xform);
}

void render_set_vr_morph(float t, const float panel[16]) {
    // Exactly 0 puts every vertex on the panel's plane, where the depth test has nothing
    // to sort by: the sea drew over the racer. Any step off it restores the order, since
    // depth along each line of sight stays monotonic -- 0.002 already matches the flat
    // frame on the device, and puts the scene within a few millimetres of the panel.
    constexpr float kFloor = 0.002f;
    g_morph = t >= 1.0f ? 1.0f : kFloor + (1.0f - kFloor) * (t < 0.0f ? 0.0f : t);
    if (panel) memcpy(g_panel, panel, sizeof(g_panel));
    view_invalidate();
}

// One draw's matrices part way between theater and stereo.
//
// Theater is the game's frame on a panel: a vertex goes through the game's projection P and
// lands on the panel at the frame position that gives -- g_panel * P, the same thing the
// HUD path does with its own frame. Stereo is wherever the eye path would put it. The two
// are blended as homogeneous points, which keeps the whole chain one matrix per draw and
// keeps the GPU's clipping correct all the way through: a vertex behind the game's camera
// still has w < 0 on the panel side and is still cut, which a per-vertex divide would lose.
//
// Blending homogeneous points weights each by its w. The panel side's w is the vertex's
// depth from the game's camera; the stereo side's is 1, so it is scaled by the panel's own
// distance D. The weights then come out as (1-k)*z and k*D, which is exactly interpolation
// in 1/depth: every vertex's disparity grows linearly with k, rather than the far scenery
// sitting on the panel until the last moment and then leaving all at once. When the stereo
// side was itself built from P (the HUD frame), the two share a w and no scaling is wanted.
//
// For the same reason, a vertex slides along a line between two points that the game's
// camera sees in nearly the same direction; from where that camera stands the picture
// barely moves, and only depth comes in. It is not exact, because the panel is not exactly
// the game's field of view and the world is pitched in stereo but not on the panel.
//
// `crop` gives four clip distances that cut the scene to a window of the game's frustum
// widened by s in each axis: s = 1 is the frame the panel shows, and s grows without bound
// as k reaches 1. In stereo the eye sees far more than the game's frustum, and on the panel
// none of that may show -- it would spill round the edges of the frame.
static void morph_chain(const float P[16], MorphKind kind, float chain[16], float fog[16],
                        float crop[16]) {
    const float k = g_morph;
    const float D = -g_panel[14];
    float T[16], S[16], sigma = D;
    mat4_mul(g_panel, P, T);
    memset(fog, 0, 16 * sizeof(float));
    fog[0] = fog[5] = fog[10] = fog[15] = 1.0f;
    switch (kind) {
    case MorphKind::World:
        memcpy(S, g_world_xform, sizeof(S));
        for (int i = 0; i < 16; i++) fog[i] = (1.0f - k) * fog[i] + k * g_vr_view_world[i];
        break;
    case MorphKind::CameraHeld:
        memset(S, 0, sizeof(S));
        S[0] = S[5] = S[10] = S[15] = 1.0f;
        for (int i = 0; i < 16; i++) fog[i] = (1.0f - k) * fog[i] + k * g_vr_view[i];
        break;
    case MorphKind::Hud:
        mat4_mul(g_vr_hud_eff, P, S);
        sigma = 1.0f;
        break;
    }
    float M[16], VM[16];
    for (int i = 0; i < 16; i++) M[i] = (1.0f - k) * T[i] + k * sigma * S[i];
    mat4_mul(g_vr_view, M, VM);
    mat4_mul(g_vr_proj, VM, chain);

    memset(crop, 0, 16 * sizeof(float));
    if (kind == MorphKind::Hud) {
        // The HUD maps the game's frame onto its own at both ends; there is nothing outside
        // it to hide.
        for (int i = 0; i < 4; i++) crop[12 + i] = 1.0f;
        return;
    }
    const float s = 1.0f / fmaxf(1.0f - k, 1e-4f);
    // Row r of P is (P[r], P[4+r], P[8+r], P[12+r]); distance i is crop's row i.
    for (int c = 0; c < 4; c++) {
        const float w = s * P[c * 4 + 3], x = P[c * 4 + 0], y = P[c * 4 + 1];
        crop[c * 4 + 0] = w - x;
        crop[c * 4 + 1] = w + x;
        crop[c * 4 + 2] = w - y;
        crop[c * 4 + 3] = w + y;
    }
    // However wide s opens them, planes through the game's camera still hide everything
    // behind its image plane -- and that camera looks down, so with the head turned the
    // sky off to the side is behind it, and stayed black until the morph finished and it
    // popped in. So the planes also back away from the camera, by an amount that is
    // nothing at the start and passes the far end of the world well before the end.
    const float back = D * (s * s - 1.0f);
    for (int i = 0; i < 4; i++) crop[12 + i] += back;
}

// Paints what the panel shows where the game drew nothing: the EFB's clear colour, inside
// the window the crop leaves open and black outside it, as theater is black around its
// panel. A quad well out in the main camera's frustum, morphed like the world -- so it is
// the panel's own rectangle at the start and opens out with the crop. Drawn first, with no
// depth, so the scene simply covers it.
static void draw_morph_background(const float P[16], const float rgb[3]) {
    float chain[16], fog[16], crop[16];
    morph_chain(P, MorphKind::World, chain, fog, crop);
    const float s = 1.0f / fmaxf(1.0f - g_morph, 1e-4f);
    const float z = 4000.0f;   // game units; anywhere inside both frustums does
    glUseProgram(g_bg_prog);
    glUniformMatrix4fv(g_bg_u_mvp, 1, GL_FALSE, chain);
    glUniformMatrix4fv(g_bg_u_crop, 1, GL_FALSE, crop);
    // Half again past the crop on each side, so the crop and not the quad is the edge.
    glUniform4f(g_bg_u_quad, 1.5f * s * z / P[0], 1.5f * s * z / P[5], -z, 0.0f);
    glUniform3f(g_bg_u_col, rgb[0], rgb[1], rgb[2]);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    set_logic_op_off();
    glDisable(GL_CULL_FACE);
    g_gl_cull = 0;
    glColorMask(1, 1, 1, 1);
    glBindVertexArray(g_copy_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(g_vao);
    glDepthMask(GL_TRUE);
}

// The frame the HUD is painted on in stereo: a quad `dist` game units ahead of the game's
// camera, `scale` of the vertical field of view tall and 4:3 wide, with the game's own 2D
// frame mapped onto it corner to corner. It is anchored to the camera rather than to the
// head, so it frames the race while the viewer looks forward and stays where it is when
// they turn to look at something else.
//
// `height` lifts its centre off the forward axis and `pitch_rad` leans the top away, both
// because where a panel wants to hang is a question about a person and not about geometry.
// At 0 and 0 the frame stands vertical and centred on the axis, which is where the eye was
// at the moment the runtime fixed its LOCAL space.
//
// Writing NDC straight into the eye -- what this replaced -- cannot work in stereo. The
// headset's per-eye frustums are asymmetric, so one NDC position is a different direction
// in each eye; there is no depth at which the two images agree, and they never fuse.
void render_hud_frame(float dist, float tan_half_fovy, float scale, float height,
                      float pitch_rad, float out[16]) {
    const float half_h = dist * tan_half_fovy * scale;
    const float half_w = half_h * 4.0f / 3.0f;   // the game's frame is 4:3
    const float c = cosf(pitch_rad), s = sinf(pitch_rad);
    memset(out, 0, 16 * sizeof(float));
    // X: the frame's own right, which the tilt leaves alone.
    out[0] = half_w;
    // Y: its up, leaned back by the pitch. GX clip space and view space both point Y up.
    out[5] = half_h * c;
    out[6] = -half_h * s;
    // W: the centre, `height` up from the forward axis and `dist` down it (-Z forward).
    out[13] = height;
    out[14] = -dist;
    out[15] = 1.0f;
    // The Z column stays zero: every element lands on the plane of the frame. What that
    // costs is the overlay's own depth ordering, which is why the eye path drops the
    // depth test for these draws and lets submission order do the layering.
}

static bool execute_batch(Batch& b, bool do_present, const std::vector<uint8_t>* leave_out = nullptr,
                          bool again = false);

// The size of the frame the game actually scans out, which is the display copy's. It is
// not the EFB's size: the EFB is 640x528 and the game displays 640x480 of it.
static void display_size(const Batch& b, uint32_t& w, uint32_t& h) {
    w = EFB_W;
    h = EFB_H;
    for (auto& c : b.cmds)
        if (c.type == CmdType::EfbCopy && c.copy.to_xfb) { w = c.copy.dst_w; h = c.copy.dst_h; }
}

// A copy that takes the whole displayed frame is the game compositing its finished
// image: a post pass reads the EFB out and draws it straight back as a screen-filling
// quad. Smaller copies are real scene content -- the water reflection, the sprite sheet
// the spray uses -- and the eye pass needs them.
static bool is_fullscreen_copy(const EfbCopyCmd& c, uint32_t dw, uint32_t dh) {
    return !c.to_xfb && c.dst_w >= dw && c.dst_h >= dh;
}

// Marks the commands an eye must not replay.
//
// An off-screen pass draws into a target and then copies it out -- a reflection, the
// sheet the spray uses. Its draws are in that target's space, so re-aiming them at an
// eye is meaningless, and the first eye has already produced all of them against the
// EFB. Everything else -- the main scene, the composite over it, the HUD -- is replayed.
//
// What marks such a pass is that its copy *clears* the EFB, since the next pass needs it
// empty. A copy that does not clear is a grab: the game lifting a piece of the live
// scene to texture with, which is how the spray is done -- at speed it takes some fifty
// 32x32 and 64x64 rects from scattered screen positions, each preceded by no draws of
// its own. Treating every copy as ending a pass instead throws away whatever draws
// happen to sit in front of the first grab, and the water surface is among them: the
// ocean disappeared the moment the racer was fast enough to throw spray.
static void mark_offscreen_passes(const Batch& b, std::vector<uint8_t>& skip) {
    uint32_t dw, dh;
    display_size(b, dw, dh);
    skip.assign(b.cmds.size(), 0);
    size_t pass_start = 0;
    for (size_t i = 0; i < b.cmds.size(); i++) {
        if (b.cmds[i].type != CmdType::EfbCopy) continue;
        const EfbCopyCmd& c = b.cmds[i].copy;
        if (!c.to_xfb && c.clear && !is_fullscreen_copy(c, dw, dh))
            for (size_t j = pass_start; j <= i; j++) skip[j] = 1;
        pass_start = i + 1;
    }
}

// Texture ids holding a copy of a whole frame.
//
// A draw sampling one is screen-space, so in an eye it is a flat billboard rather than
// something in the world -- it leaves a faint rectangular seam where its edges fall. It
// is tempting to drop those draws, but in this game the water surface is one of them:
// dropping it leaves the seabed showing through bare sand instead of blue-green water,
// which is far worse than the seam. GCN_EYE_SKIPCOMP drops them anyway, for comparing.
static std::vector<uint32_t> g_fullscreen_tex;

// These ids are stable across frames -- an EFB copy keeps the id its destination address
// was registered under -- so the set only ever needs adding to.
static void note_fullscreen_copies(const Batch& b) {
    uint32_t dw, dh;
    display_size(b, dw, dh);
    for (auto& c : b.cmds) {
        if (c.type != CmdType::EfbCopy || !is_fullscreen_copy(c.copy, dw, dh) || !c.copy.tex_id)
            continue;
        bool known = false;
        for (uint32_t id : g_fullscreen_tex) known |= (id == c.copy.tex_id);
        if (!known) g_fullscreen_tex.push_back(c.copy.tex_id);
    }
}

static bool is_fullscreen_tex(uint32_t id) {
    for (uint32_t t : g_fullscreen_tex)
        if (t == id) return true;
    return false;
}

static bool samples_fullscreen_copy(const PixelState& st) {
    for (int i = 0; i < 8; i++)
        if (st.tex_is_efb[i] && st.tex_id[i] && is_fullscreen_tex(st.tex_id[i])) return true;
    return false;
}

static bool samples_grab_copy(const PixelState& st) {
    for (int i = 0; i < 8; i++)
        if (st.tex_is_efb[i] && st.tex_id[i] && is_grab_tex(st.tex_id[i])) return true;
    return false;
}

// Which draws the flat pass can leave out when it runs only to feed the eyes.
//
// In stereo the flat pass exists for the render-to-texture copies: the eyes sample them,
// and an eye never draws into the EFB. The picture it paints is never shown. So it need not
// draw past the last copy whose contents something will read -- the main scene after that
// point is drawn by each eye anyway, and here it was a thousand draw calls a frame, a third
// of the render thread, spent on pixels that went straight to the next clear.
//
// What is read: every copy an off-screen pass samples (those draws run only here), and
// every copy an eye samples as copied. An eye takes over the lookups of a whole-frame copy
// and of the small grabs the spray uses, once it has a grab of its own to substitute
// (is_eye_screen_tex), and those need no flat scene behind them. A copy with no reader in
// this frame is kept all the same: a game can copy the scene once and sample it for the
// frames that follow, so an unread copy is not an unneeded one. Only a whole-frame copy or a
// grab the eye would substitute for is taken as unread. GCN_EYE_FULLFLAT=1 draws the whole
// flat pass as before.
//
// `skip` is mark_offscreen_passes' answer; `out` gets one byte per command, 1 to leave out.
// Empty when nothing is to be left out.
static void flat_pass_trim(const Batch& b, const std::vector<uint8_t>& skip, std::vector<uint8_t>& out) {
    out.clear();
    static const bool full = getenv("GCN_EYE_FULLFLAT") != nullptr;
    if (full) return;
    uint32_t dw, dh;
    display_size(b, dw, dh);
    // Copy textures read as copied this frame. A frame has a handful of copies, so a list.
    std::vector<uint32_t> read;
    auto note_read = [&](uint32_t id) {
        for (uint32_t r : read) if (r == id) return;
        read.push_back(id);
    };
    for (size_t i = 0; i < b.cmds.size(); i++) {
        const Cmd& c = b.cmds[i];
        if (c.type != CmdType::Draw) continue;
        const PixelState& st = b.states[c.state];
        // Whether an eye substitutes its grab in this draw: the same test apply_state makes
        // for screen_uv, which then takes over every eye-screen texture the draw binds.
        bool subst = false;
        if (!skip[i]) {
            const uint32_t nstg = ((st.bp[0x00] >> 10) & 15) + 1;
            for (uint32_t s = 0; s < nstg && !subst; s++) {
                const uint32_t order = st.bp[0x28 + s / 2] >> ((s & 1) * 12);
                const uint32_t map = order & 7;
                subst = (order & 0x40) && st.tex_is_efb[map] && st.tex_id[map] &&
                        is_eye_screen_tex(st.tex_id[map]);
            }
        }
        for (int m = 0; m < 8; m++) {
            if (!st.tex_is_efb[m] || !st.tex_id[m]) continue;
            if (subst && is_eye_screen_tex(st.tex_id[m])) continue;
            note_read(st.tex_id[m]);
        }
    }
    size_t last = 0;
    bool any = false;
    for (size_t i = 0; i < b.cmds.size(); i++) {
        const Cmd& c = b.cmds[i];
        if (c.type != CmdType::EfbCopy || c.copy.to_xfb) continue;
        const EfbCopyCmd& cc = c.copy;
        bool needed = true;
        if (cc.tex_id && !cc.depth &&
            ((g_eye_grab.tex && is_fullscreen_copy(cc, dw, dh)) || (g_spray_grab.tex && is_grab_copy(cc)))) {
            needed = false;
            for (uint32_t r : read) needed |= r == cc.tex_id;
        }
        if (needed) { last = i; any = true; }
    }
    const size_t from = any ? last + 1 : 0;
    bool trimmed = false;
    for (size_t i = from; i < b.cmds.size(); i++)
        if (b.cmds[i].type == CmdType::Draw && !skip[i]) trimmed = true;
    if (!trimmed) return;
    out.assign(b.cmds.size(), 0);
    for (size_t i = from; i < b.cmds.size(); i++)
        if (b.cmds[i].type == CmdType::Draw && !skip[i]) out[i] = 1;
}

// Draw one eye's view of a batch into `fbo`.
//
// The vertex buffer, the CPU-side transform in xf.cpp and any render-to-texture results
// are all shared between the eyes -- only the uniforms and the draw calls are repeated,
// which is what makes stereo affordable here. Pass do_copies for the first eye only;
// the second reuses what it produced.
bool render_execute_eye(Batch& b, unsigned fbo, int w, int h, bool do_copies) {
    // The scene samples textures the game produces by copying them back out of the EFB:
    // the water reflection, the sprite sheet the spray uses. An eye pass never draws into
    // the EFB, so on its own it would copy out an empty one -- which once left a model
    // untextured and put a black quad on the water. So the first eye runs the whole frame
    // flat into the EFB exactly as the hardware would, minus the scanout, and the eyes
    // then re-project only the main scene on top of correct textures.
    static std::vector<uint8_t> skip, trim;
    mark_offscreen_passes(b, skip);
    const auto t_start = std::chrono::steady_clock::now();
    double ms_flat = 0.0;
    size_t n_trimmed = 0;
    if (do_copies) {
        // Where the eye stands and what it leaves out are read off the batch, so they
        // are settled before anything is drawn from it -- the flat pass included, which
        // leaves the hidden draws out of the EFB too.
        eye_prepare(b, skip);
        compose_world_view();
        g_vr_active = false;
        flat_pass_trim(b, skip, trim);
        for (uint8_t t : trim) n_trimmed += t;
        execute_batch(b, false, trim.empty() ? nullptr : &trim);  // which also notes this batch's whole-frame copies
        if (g_frametime) ms_flat = ms_since(t_start);
    }
    // GCN_EYELOG=1 reports how a frame was split, which is the only way to tell a scene
    // rendered at the wrong field of view from a composite quad standing in for one.
    static const bool eyelog = getenv("GCN_EYELOG") != nullptr;
    int n_drawn = 0, n_skipped = 0, n_spray = 0;
    if (eyelog && do_copies) {
        uint32_t dw, dh;
        display_size(b, dw, dh);
        fprintf(stderr, "[eye] f%u cmds=%zu display=%ux%u\n", g_render_frame, b.cmds.size(),
                dw, dh);
        size_t pass_start = 0;
        for (size_t i = 0; i < b.cmds.size(); i++) {
            if (b.cmds[i].type != CmdType::EfbCopy) continue;
            int nd = 0;
            for (size_t j = pass_start; j < i; j++) nd += b.cmds[j].type == CmdType::Draw;
            const EfbCopyCmd& cc = b.cmds[i].copy;
            fprintf(stderr, "[eye]   copy %ux%u xfb=%d tex=%u fmt=%02X%s full=%d draws=%d clr=%d%d src=%u,%u+%ux%u %s\n",
                    cc.dst_w, cc.dst_h, (int)cc.to_xfb, cc.tex_id, cc.format, cc.depth ? " depth" : "",
                    (int)is_fullscreen_copy(cc, dw, dh), nd, (int)cc.clear,
                    (int)cc.clear_color, cc.src_x, cc.src_y, cc.src_w, cc.src_h,
                    skip[i] ? "SKIP" : "replay");
            pass_start = i + 1;
        }
    }

    g_vr_active = true;
    g_eye_w = w;
    g_eye_h = h;
    view_invalidate();   // this eye's matrices and target
    bool grabbed = false, grabbed_spray = false;
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glDepthMask(GL_TRUE);
    // The eye's background has to be the EFB's. Where the scene draws nothing behind the
    // water -- open sea, past where the course gives the sea a bottom -- the water
    // refracts whatever the buffer was cleared to, and an eye cleared to black turned
    // that into a black sea in stereo while the flat view, cleared to the game's own
    // colour, showed open water. So take the clear the game set for the main scene: the
    // last copy to clear the colour buffer ahead of the first draw an eye replays.
    float bg[3] = {0, 0, 0};
    for (size_t i = 0; i < b.cmds.size(); i++) {
        if (b.cmds[i].type == CmdType::Draw) {
            if (!skip[i]) break;
            continue;
        }
        if (b.cmds[i].type != CmdType::EfbCopy) continue;
        const EfbCopyCmd& c = b.cmds[i].copy;
        if (c.clear && c.clear_color) {
            bg[0] = ((c.clear_rgba >> 24) & 0xFF) / 255.0f;
            bg[1] = ((c.clear_rgba >> 16) & 0xFF) / 255.0f;
            bg[2] = ((c.clear_rgba >> 8) & 0xFF) / 255.0f;
        }
    }
    if (eyelog && do_copies)
        fprintf(stderr, "[eye]   background %.3f %.3f %.3f\n", bg[0], bg[1], bg[2]);
    // Mid-morph, theater's black surround comes up to the scene's clear colour as the
    // window opens, so the edge of the background quad is gone by the time it would show.
    const bool morph = g_morph < 1.0f;
    const float surround = morph ? g_morph * g_morph : 1.0f;
    glClearColor(bg[0] * surround, bg[1] * surround, bg[2] * surround, 1);
    clear_depth(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    // GL_CLIP_DISTANCE0, which ES knows only by its extension's name.
    constexpr GLenum kClipDistance0 = 0x3000;
    if (morph) {
        // The main camera's projection, from the first world draw an eye replays: the
        // background is a piece of its frustum. A frame with no world in it -- a menu --
        // gets a stand-in the shape of the panel, since only where it lands matters.
        float P[16] = {0};
        P[0] = 2.5f / 1.6f; P[5] = 2.5f / 1.2f; P[10] = -1.0f; P[11] = -1.0f; P[14] = -1.0f;
        for (size_t i = 0; i < b.cmds.size(); i++) {
            if (b.cmds[i].type != CmdType::Draw || skip[i]) continue;
            const PixelState& st = b.states[b.cmds[i].state];
            const float* p = st.proj;
            if ((int)p[6] != 0 || st.view_space) continue;
            memset(P, 0, sizeof(P));
            P[0] = p[0]; P[8] = p[1]; P[5] = p[2]; P[9] = p[3]; P[10] = p[4]; P[14] = p[5];
            P[11] = -1.0f;
            break;
        }
        if (g_clip_ok)
            for (GLenum i = 0; i < 4; i++) glEnable(kClipDistance0 + i);
        draw_morph_background(P, bg);
    }

    uint32_t cur_state = UINT32_MAX;
    gl_state_invalidate();
    int cur_prim = -1;
    for (size_t i = 0; i < b.cmds.size(); i++) {
        Cmd& c = b.cmds[i];
        // Copies and the present are the first eye's business, done against the EFB.
        if (c.type != CmdType::Draw) {
            cur_state = UINT32_MAX;
            gl_state_invalidate();
            continue;
        }
        if (skip[i]) { n_skipped++; continue; }
        if (!g_hide.empty() && g_hide[i]) continue;
        // GCN_EYE_SKIPCOMP drops the screen-space passes entirely, for comparing against
        // drawing them flat across the eye. Dropping the water one leaves bare seabed.
        static const bool skipcomp = getenv("GCN_EYE_SKIPCOMP") != nullptr;
        if (skipcomp && samples_fullscreen_copy(b.states[c.state])) { n_skipped++; continue; }
        // The first draw that wants the finished frame is the moment to take it: the
        // scene behind the water is in the target by now and the water is not yet.
        if (!grabbed && samples_fullscreen_copy(b.states[c.state])) {
            grabbed = true;
            grab_eye(g_eye_grab, w, h);
            cur_state = UINT32_MAX;   // the grab left its own texture bound
            gl_state_invalidate();
        }
        // The spray composites itself over the finished scene, the water included, so the
        // grab above -- taken deliberately before the water, which is what the water
        // itself needs -- is a layer short by the time the droplets draw. Re-take it at
        // the first of them, once, and every droplet in the frame then refracts the scene
        // as it actually stands. Costs a second full-target copy per eye on any frame
        // that throws spray, which at speed is every frame.
        if (samples_grab_copy(b.states[c.state])) {
            // How a droplet actually addresses its grab, which decides what the
            // substitution has to carry over: the indirect matrix id per stage (1..3 is
            // a static offset in texels, 5..11 scales the coordinate itself) and the
            // wrap, which folds the coordinate into the copy's own size.
            if (eyelog && do_copies && !grabbed_spray) {
                const PixelState& st = b.states[c.state];
                const uint32_t nstg = ((st.bp[0x00] >> 10) & 15) + 1;
                fprintf(stderr, "[eye]   spray draw: stages=%u nind=%u", nstg,
                        (st.bp[0x00] >> 16) & 7);
                for (uint32_t t = 0; t < nstg; t++) {
                    const uint32_t ic = st.bp[0x10 + t];
                    fprintf(stderr, " s%u[mid=%u sw=%u tw=%u bt=%u]", t, (ic >> 9) & 15,
                            (ic >> 13) & 7, (ic >> 16) & 7, ic & 3);
                }
                fprintf(stderr, "\n");
            }
            if (!grabbed_spray) {
                grabbed_spray = true;
                grab_eye(g_spray_grab, w, h);
                cur_state = UINT32_MAX;
                gl_state_invalidate();
            }
            n_spray++;
        }
        n_drawn++;
        if (c.state != cur_state || c.prim != cur_prim) {
            apply_state(b.states[c.state], c.prim);
            // apply_state binds the EFB's scissor and viewport expectations; the eye
            // target overrides both.
            glViewport(0, 0, w, h);
            glDisable(GL_SCISSOR_TEST);
            cur_state = c.state;
            cur_prim = c.prim;
        }
        draw_cmd(b, c);
    }
    if (eyelog && do_copies)
        fprintf(stderr, "[eye]   drawn=%d skipped_composite=%d spray=%d flat_trimmed=%zu\n", n_drawn,
                n_skipped, n_spray, n_trimmed);
    // The renderer's half of GCN_FRAMETIME for an eye: the flat pass (first eye only) and
    // this eye's own draws, CPU time.
    if (g_frametime)
        fprintf(stderr, "[eye-rt] f%u %s flat %6.2fms eye %6.2fms  draws %d\n", g_render_frame,
                do_copies ? "first" : "second", ms_flat, ms_since(t_start) - ms_flat, n_drawn);
    apply_stats_flush(do_copies ? "eye0" : "eye1", g_render_frame);
    if (morph && g_clip_ok)
        for (GLenum i = 0; i < 4; i++) glDisable(kClipDistance0 + i);
#ifdef GCN_GL_ES
    // The eye's depth buffer is finished with: nothing reads it, and the next pass clears
    // it. On a tiled GPU, which every standalone headset has, a depth attachment left
    // valid at the end of a pass is written out of tile memory to RAM tile by tile, and
    // at the eye's size with multisampling that is a store of several tens of megabytes
    // per eye per frame, for nothing. Saying so lets the driver drop it. Desktop GL 3.3
    // has no glInvalidateFramebuffer, and an immediate-mode GPU would gain nothing.
    // GCN_EYE_KEEPDEPTH=1 leaves it in, for comparing.
    static const bool keep_depth = getenv("GCN_EYE_KEEPDEPTH") != nullptr;
    if (!keep_depth) {
        const GLenum attachment = GL_DEPTH_ATTACHMENT;
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &attachment);
    }
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_vr_active = false;
    return true;
}

// Marks a draw's textures as in use without drawing it. Eviction goes by what was bound,
// and the guest-side cache, which is what would send a texture again, goes by what the
// game referenced; a draw left out on purpose is the one case where those differ.
static void touch_textures(const PixelState& st) {
    for (int m = 0; m < 8; m++) {
        if (!st.tex_id[m]) continue;
        auto it = g_textures.find(st.tex_id[m]);
        if (it != g_textures.end()) it->second.last_used = g_render_frame;
    }
}

// Release GL textures the game has stopped using.
//
// EFB copies are included. A copy to an address it already holds reuses its texture, so
// the fixed targets never come through here, but the spray copies to rotating addresses
// and would otherwise strand a texture per sprite per frame. They are given a longer
// idle period than the guest-side cache so that an address is always forgotten there
// first: a draw can then never reach an id whose texture has already gone.
// Decoded textures go when the front end says (Batch::dead_textures), a batch late, so that
// nothing still drawing from one loses it: the ids `b` names are deleted at the next batch.
// The renderer ages only the EFB copies' textures, which it makes itself.
static std::vector<uint32_t> g_dead_pending;
static void evict_textures(const Batch& b) {
    for (uint32_t id : g_dead_pending) {
        auto it = g_textures.find(id);
        if (it != g_textures.end() && !it->second.efb) {
            glDeleteTextures(1, &it->second.tex);
            g_textures.erase(it);
        }
    }
    g_dead_pending = b.dead_textures;
    if ((g_render_frame & 63) != 0) return;
    for (auto it = g_textures.begin(); it != g_textures.end();) {
        if (it->second.efb && g_render_frame - it->second.last_used > 4 * kTexIdleFrames) {
            glDeleteTextures(1, &it->second.tex);
            it = g_textures.erase(it);
        } else {
            ++it;
        }
    }
}

// Runs a batch into the EFB the way the hardware would. With do_present false the final
// scanout is skipped but everything else -- including every render-to-texture copy -- still
// happens, which is how the stereo path obtains the textures its eye passes sample.
// `again` draws a batch this function has just drawn: the textures and vertices are
// already up, and it is the same frame to the texture cache.
static bool execute_batch(Batch& b, bool do_present, const std::vector<uint8_t>* leave_out, bool again) {
    const auto t_start = std::chrono::steady_clock::now();
    double ms_tex = 0.0, ms_vbo = 0.0;
    if (!again) {
        g_render_frame++;
        evict_textures(b);
        for (auto& t : b.new_textures) upload_texture(*t);
        ms_tex = g_frametime ? ms_since(t_start) : 0.0;
        // A set the GPU has finished with; see g_vaos. The eye passes after this draw from
        // the same one, so a batch is still uploaded once however many views it is drawn
        // into.
        g_vertex_set = (g_vertex_set + 1) % kVertexRing;
        g_vao = g_vaos[g_vertex_set][0];
        g_vbo = g_vbos[g_vertex_set];
        g_ebo = g_ebos[g_vertex_set];
        glBindVertexArray(g_vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        glBufferData(GL_ARRAY_BUFFER, b.packed.size(), b.packed.data(), GL_STREAM_DRAW);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, b.indices.size() * sizeof(uint32_t), b.indices.data(), GL_STREAM_DRAW);
        ms_vbo = g_frametime ? ms_since(t_start) - ms_tex : 0.0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    glViewport(0, 0, EFB_W * g_scale, EFB_H * g_scale);
    glBindVertexArray(g_vao);
    uint32_t n_apply = 0;
    note_fullscreen_copies(b);
    static const bool batchlog = getenv("GCN_EYELOG") != nullptr;
    if (batchlog) {
        int nd = 0, nc = 0, np = 0;
        for (auto& c : b.cmds) {
            nd += c.type == CmdType::Draw;
            nc += c.type == CmdType::EfbCopy;
            np += c.type == CmdType::Present;
        }
        fprintf(stderr, "[batch] f%u cmds=%zu draws=%d copies=%d present=%d verts=%zu indices=%zu\n",
                g_render_frame, b.cmds.size(), nd, nc, np, b.verts.size(), b.indices.size());
    }
    bool presented = false;
    uint32_t cur_state = UINT32_MAX;
    gl_state_invalidate();
    view_invalidate();   // the flat view, after an eye
    int cur_prim = -1;
    // GCN_NO_COMP drops draws that sample a copy of the whole frame, in the flat path too.
    // The duplicate racer on the water is visible without any of the VR code, so this is
    // how to tell whether that draw is responsible for it.
    static const bool no_comp = getenv("GCN_NO_COMP") != nullptr;
    static const bool only_comp = getenv("GCN_ONLY_COMP") != nullptr;
    static const bool complog = getenv("GCN_COMPLOG") != nullptr;
    int draw_index = 0;
    for (size_t ci = 0; ci < b.cmds.size(); ci++) {
        Cmd& c = b.cmds[ci];
        switch (c.type) {
        case CmdType::Draw: {
            if (!g_hide.empty() && g_hide[ci]) {
                // Not drawn, but still the game's: its textures must not age out. The game
                // goes on referencing them, so it never sends them again, and a hidden
                // model came back from first person with parts of it untextured.
                touch_textures(b.states[c.state]);
                break;
            }
            // A draw the eyes will make themselves, whose only product here would be a
            // picture nobody sees; see flat_pass_trim.
            if (leave_out && (*leave_out)[ci]) break;
            const bool comp = (no_comp || complog || only_comp) &&
                              samples_fullscreen_copy(b.states[c.state]);
            if (only_comp && !comp) break;
            // GCN_DRAWLOG=<frame> lists every draw in one frame with its index, so a
            // specific piece of geometry can be found and then skipped by index.
            static const uint32_t drawlog = getenv("GCN_DRAWLOG") ? atoi(getenv("GCN_DRAWLOG")) : 0;
            // GCN_DRAW_SKIP=a-b drops a range of draw indices, to attribute a piece of the
            // image to the draws that made it.
            static int skip_lo = -1, skip_hi = -1;
            static bool skip_parsed = false;
            if (!skip_parsed) {
                skip_parsed = true;
                if (const char* s = getenv("GCN_DRAW_SKIP")) {
                    skip_lo = atoi(s);
                    const char* dash = strchr(s, '-');
                    skip_hi = dash ? atoi(dash + 1) : skip_lo;
                }
            }
            if (drawlog && g_render_frame == drawlog) {
                const PixelState& st = b.states[c.state];
                // The projection type and the view-space depth are what decide a draw's
                // fate in an eye: a perspective batch is re-projected as world geometry,
                // anything else goes on the HUD frame. And a draw a few tens of units
                // from the camera is in front of the viewer's face either way.
                float zlo = 1e30f, zhi = -1e30f, xlo = 1e30f, xhi = -1e30f, ylo = 1e30f, yhi = -1e30f;
                for (uint32_t v = 0; v < c.count; v++) {
                    const float* q = b.verts[b.indices[c.first + v]].pos;
                    zlo = std::min(zlo, q[2]); zhi = std::max(zhi, q[2]);
                    xlo = std::min(xlo, q[0]); xhi = std::max(xhi, q[0]);
                    ylo = std::min(ylo, q[1]); yhi = std::max(yhi, q[1]);
                }
                // Vertices within a unit of the view-space origin sit on top of the camera,
                // and a polygon that uses one is drawn as a sliver radiating from the
                // screen's centre; counted so such draws can be found.
                uint32_t at_origin = 0;
                for (uint32_t v = 0; v < c.count; v++) {
                    const float* p = b.verts[b.indices[c.first + v]].pos;
                    if (fabsf(p[0]) < 1.0f && fabsf(p[1]) < 1.0f && fabsf(p[2]) < 1.0f) at_origin++;
                }
                // Sliver triangles: longer than 40 units in view space with a height under
                // a thousandth of their length. Geometry that is right never looks like
                // that; a vertex that is wrong makes exactly that, drawn as a hairline
                // across the screen. The first few are printed with their vertices.
                uint32_t slivers = 0;
                std::string sliver_text;
                if (c.prim == 0) {
                    for (uint32_t t = 0; t + 2 < c.count; t += 3) {
                        const float* a = b.verts[b.indices[c.first + t]].pos;
                        const float* bb = b.verts[b.indices[c.first + t + 1]].pos;
                        const float* cc = b.verts[b.indices[c.first + t + 2]].pos;
                        auto len = [](const float* p, const float* q) {
                            return sqrtf((p[0]-q[0])*(p[0]-q[0]) + (p[1]-q[1])*(p[1]-q[1]) + (p[2]-q[2])*(p[2]-q[2]));
                        };
                        const float e0 = len(a, bb), e1 = len(bb, cc), e2 = len(cc, a);
                        const float longest = std::max(e0, std::max(e1, e2));
                        if (longest < 40.0f) continue;
                        // Heron's formula for the area; height = 2A / longest.
                        const float sp = (e0 + e1 + e2) * 0.5f;
                        const float area2 = std::max(0.0f, sp * (sp - e0) * (sp - e1) * (sp - e2));
                        const float height = 2.0f * sqrtf(area2) / longest;
                        static const float ratio = getenv("GCN_SLIVER_RATIO") ? (float)atof(getenv("GCN_SLIVER_RATIO")) : 0.001f;
                        if (height < longest * ratio) {
                            if (slivers < 3) {
                                char buf[200];
                                const GpuVertex& va = b.verts[b.indices[c.first + t]];
                                const GpuVertex& vb = b.verts[b.indices[c.first + t + 1]];
                                const GpuVertex& vc = b.verts[b.indices[c.first + t + 2]];
                                snprintf(buf, sizeof(buf), "\n        sliver src d%u:%d d%u:%d d%u:%d (%.3f,%.3f,%.3f) (%.3f,%.3f,%.3f) (%.3f,%.3f,%.3f) len=%.0f",
                                         va.src_draw, (int)va.src_idx, vb.src_draw, (int)vb.src_idx, vc.src_draw, (int)vc.src_idx,
                                         a[0], a[1], a[2], bb[0], bb[1], bb[2], cc[0], cc[1], cc[2], longest);
                                sliver_text += buf;
                            }
                            slivers++;
                        }
                    }
                }
                // The viewport's depth band, and the view-space box, to two decimals: a
                // weapon held in front of the camera is a unit or two across.
                const float band_hi = st.viewport[5] / 16777215.0f;
                const float band_lo = band_hi - fabsf(st.viewport[2]) / 16777215.0f;
                fprintf(stderr, "[draw] %d verts=%u st=%u texgens=%u proj=%c z=%.2f..%.2f x=%.2f..%.2f y=%.2f..%.2f band=%.5f..%.5f origin=%u slivers=%u%s",
                        draw_index, c.count, c.state, st.num_texgens,
                        (int)st.proj[6] == 0 ? 'p' : 'o', zlo, zhi, xlo, xhi, ylo, yhi, band_lo, band_hi,
                        at_origin, slivers, sliver_text.c_str());
                if (slivers) fprintf(stderr, "\n       ");
                for (int i = 0; i < 8; i++)
                    if (st.tex_id[i]) fprintf(stderr, " t%d=%u%s", i, st.tex_id[i],
                                              st.tex_is_efb[i] ? "*" : "");
                fprintf(stderr, "\n");
                // GCN_DRAWLOG_VERBOSE=1 adds the TEV setup -- stage count, each stage's
                // order, colour and alpha combiners and konst selectors, the colour
                // registers and konsts -- and the first few vertices, so a draw's shading
                // can be read off without a shader dump.
                static const bool verbose = getenv("GCN_DRAWLOG_VERBOSE") != nullptr;
                if (verbose) {
                    const uint32_t* bp = st.bp;
                    const uint32_t nstages = ((bp[0] >> 10) & 15) + 1;
                    fprintf(stderr, "        genmode=%06X stages=%u colors=%u", bp[0], nstages, st.num_colors);
                    for (uint32_t i = 0; i < nstages; i++)
                        fprintf(stderr, "\n        st%u order=%06X cenv=%06X aenv=%06X ksel=%06X", i,
                                bp[0x28 + i / 2], bp[0xC0 + 2 * i], bp[0xC1 + 2 * i], bp[0xF6 + i / 2]);
                    fprintf(stderr, "\n        alphafunc=%06X zmode=%06X blend=%06X", bp[0xF3], bp[0x40], bp[0x41]);
                    for (int r = 0; r < 4; r++)
                        fprintf(stderr, "\n        reg%d=%08X %08X  konst%d=%08X %08X", r, st.tev_reg[r][0], st.tev_reg[r][1],
                                r, st.tev_konst[r][0], st.tev_konst[r][1]);
                    for (uint32_t v = 0; v < c.count && v < 24; v++) {
                        const uint32_t ix = b.indices[c.first + v];
                        const GpuVertex& gv = b.verts[ix];
                        fprintf(stderr, "\n        i%u=%u pos=%.1f,%.1f,%.1f col0=%02X%02X%02X%02X col1=%02X%02X%02X%02X uv0=%.3f,%.3f", v, ix,
                                gv.pos[0], gv.pos[1], gv.pos[2], gv.col[0][0], gv.col[0][1], gv.col[0][2], gv.col[0][3],
                                gv.col[1][0], gv.col[1][1], gv.col[1][2], gv.col[1][3], gv.tex[0][0], gv.tex[0][1]);
                    }
                    fprintf(stderr, "\n");
                }
            }
            const int this_draw = draw_index++;
            if (skip_lo >= 0 && this_draw >= skip_lo && this_draw <= skip_hi) break;
            // GCN_NO_EFBTEX drops draws that sample a partial EFB copy -- here, the copy of
            // the water surface that the game tints submerged geometry with. Unlike a draw
            // index this is stable from frame to frame, which matters because the game's
            // timebase is wall-clock driven and frame N is not the same moment twice.
            static const bool no_efbtex = getenv("GCN_NO_EFBTEX") != nullptr;
            if (no_efbtex) {
                const PixelState& st = b.states[c.state];
                bool partial = false;
                for (int i = 0; i < 8; i++)
                    if (st.tex_is_efb[i] && st.tex_id[i]) partial = true;
                if (partial && !samples_fullscreen_copy(st)) break;
            }
            if (complog) {
                // Every draw that samples a render-to-texture result, not just the
                // whole-frame ones: the duplicate racer is in one of these layers and
                // the whole-frame one turned out to be innocent.
                const PixelState& st = b.states[c.state];
                bool any_efb = false;
                for (int i = 0; i < 8; i++) any_efb |= st.tex_is_efb[i] && st.tex_id[i];
                if (any_efb) {
                    fprintf(stderr, "[efb] f%u verts=%u texgens=%u cols=%u full=%d", g_render_frame,
                            c.count, st.num_texgens, st.num_colors, (int)comp);
                    for (int i = 0; i < 8; i++)
                        if (st.tex_id[i]) fprintf(stderr, " t%d=%u%s", i, st.tex_id[i],
                                                  st.tex_is_efb[i] ? "*" : "");
                    fprintf(stderr, "\n");
                }
            }
            if (comp && no_comp) break;
            if (c.state != cur_state || c.prim != cur_prim) {
                apply_state(b.states[c.state], c.prim);
                cur_state = c.state;
                cur_prim = c.prim;
                n_apply++;
            }
            draw_cmd(b, c);
            break;
        }
        case CmdType::EfbCopy:
            do_efb_copy(c.copy);
            glViewport(0, 0, EFB_W * g_scale, EFB_H * g_scale);
            cur_state = UINT32_MAX;
            gl_state_invalidate();
            break;
        case CmdType::Present:
            if (do_present) present(c.copy);
            presented = true;
            glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
            glViewport(0, 0, EFB_W * g_scale, EFB_H * g_scale);
            cur_state = UINT32_MAX;
            gl_state_invalidate();
            break;
        }
    }
    if (g_frametime)
        fprintf(stderr, "[rt] f%u render %6.2fms (tex %5.2f vbo %5.2f)  applies %4u  programs %zu\n",
                g_render_frame, ms_since(t_start), ms_tex, ms_vbo, n_apply, g_programs.size());
    apply_stats_flush("flat", g_render_frame);
    return presented;
}

bool render_execute(Batch& b) {
    g_hide.clear();   // the flat view shows everything, whatever the eyes leave out
    return execute_batch(b, true);
}

void render_set_panel_band(float band) { g_panel_band = band; }

bool render_execute_stereo_pair(Batch& b, unsigned fbo_l, unsigned fbo_r, float eye_sep,
                                float panel_width) {
    g_hide.clear();
    const GLuint out = g_output_fbo;
    g_flat_halfw = panel_width > 0.0f ? 0.5f * panel_width : 1.0f;
    g_flat_eye = -0.5f * eye_sep;
    g_output_fbo = (GLuint)fbo_l;
    g_dump_suffix = "_l";
    const bool presented = execute_batch(b, true);
    g_flat_eye = 0.5f * eye_sep;
    g_output_fbo = (GLuint)fbo_r;
    g_dump_suffix = "_r";
    g_pair_second = true;
    execute_batch(b, true, nullptr, true);
    g_pair_second = false;
    g_dump_suffix = "";
    g_flat_eye = 0.0f;
    g_output_fbo = out;
    return presented;
}

}  // namespace gx
