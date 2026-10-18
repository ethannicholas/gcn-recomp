// A minimal OpenXR runtime for visionOS: exactly the calls the frontend core makes
// (openxr/xr_frontend.cpp), over the platform's own frameworks, so that a Vision Pro runs
// the same core a Quest does. See vos_runtime.h for why this exists at all.
//
//   frames       CompositorServices. xrWaitFrame walks a cp_frame up to its drawable;
//                xrEndFrame composites the submitted layers into it with Metal.
//   head         ARKit's world tracking: the device anchor at the drawable's anchor time.
//   controllers  ARKit's accessory tracking for where a spatial controller (the PS VR2
//                Sense) is, GameController for its buttons, laid out as the Touch
//                profile the core suggests first.
//   swapchains   Metal textures, each wrapped as a GL texture through ANGLE
//                (EGL_ANGLE_metal_texture_client_buffer) for the renderer to draw into. A
//                shared event, signalled by ANGLE once the renderer's work on them is done,
//                holds the compositing back until then.
//
// What the core leaves to an OpenXR runtime, this does the way a runtime would. A
// projection layer is the image as rendered for the pose it was rendered at: a display frame
// with no new game frame submits the last one again, and the drawable is given the device
// anchor that image was drawn for, so the compositor reprojects it rather than showing it
// stuck to the head. A quad layer -- the theater panel -- is drawn here into each eye, with
// its depth, at the drawable's own pose.
//
// The LOCAL reference space is where the head was when tracking first came up, turned to
// face where it faced and levelled, which is what OpenXR's LOCAL is. ARKit's own origin is
// on the floor wherever the immersive space opened.
#include "vos_runtime.h"
#include "angle.h"
#include "gx/gl.h"
#include "hw/pad.h"

#include <openxr/openxr.h>

#import <ARKit/ARKit.h>
#import <CompositorServices/CompositorServices.h>
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <Metal/Metal.h>
#include <simd/simd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define LOGI(fmt, ...) fprintf(stderr, "[vos] " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) fprintf(stderr, "[vos] error: " fmt "\n", ##__VA_ARGS__)

// The handles OpenXR declares opaque.
struct XrInstance_T { int unused; };
struct XrSession_T { int unused; };
struct XrActionSet_T { int unused; };

struct XrSpace_T {
    enum Kind { kLocal, kAim } kind;
    int hand;  // kAim: 0 left, 1 right
};

namespace {

constexpr int kImages = 3;

// ---------------------------------------------------------------------------
// Inputs: what each hand has, whichever device it came from.
// ---------------------------------------------------------------------------
enum Src { kSrcA, kSrcB, kSrcMenu, kSrcTrigger, kSrcGrip, kSrcStick, kSrcStickClick, kSrcAim, kSrcNone };

struct HandInput {
    bool a = false, b = false, menu = false, stick_click = false;
    float trigger = 0, grip = 0;
    float sx = 0, sy = 0;
    bool present = false;  // a spatial controller is in this hand
};

struct Binding {
    int hand;
    Src src;
};

}  // namespace

struct XrAction_T {
    XrActionType type;
    std::vector<XrPath> subactions;
    std::vector<Binding> binds;
};

struct XrSwapchain_T {
    int w = 0, h = 0;
    struct Image {
        id<MTLTexture> mtl = nil;   // what the renderer draws into
        id<MTLTexture> srgb = nil;  // the same texels, read as sRGB by the compositing
        GLuint tex = 0;
        // The device anchor current when the image was last released: the pose it holds.
        ar_device_anchor_t anchor = nil;
        // Command buffers still reading it.
        std::atomic<int> in_flight{0};
    };
    std::vector<std::unique_ptr<Image>> images;
    uint32_t next = 0;
    int acquired = -1;
    int released = -1;  // the image a layer naming this swapchain shows
};

namespace {

struct Runtime {
    // From the shell.
    cp_layer_renderer_t layer = nil;

    // The renderer's work reaching the GPU: ANGLE signals `rendered` to `rendered_value`
    // when what was submitted before a frame's compositing has finished.
    id<MTLSharedEvent> rendered = nil;
    uint64_t rendered_value = 0;

    // Metal.
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLRenderPipelineState> pipe = nil;
    id<MTLDepthStencilState> depth_always = nil, depth_greater = nil;
    id<MTLSamplerState> sampler = nil;

    // ARKit.
    ar_session_t session = nil;
    ar_world_tracking_provider_t world = nil;
    std::mutex acc_mutex;  // the accessory session is rebuilt on the main queue
    ar_session_t acc_session = nil;
    ar_accessory_tracking_provider_t acc_provider = nil;
    ar_accessories_t accessories = nil;

    // Frames.
    cp_frame_t frame = nullptr;
    std::vector<cp_drawable_t> drawables;
    ar_device_anchor_t anchor = nil;   // the head at this frame's anchor time
    bool anchor_valid = false;
    bool have_local = false;
    simd_float4x4 origin_from_local = matrix_identity_float4x4;
    simd_float4x4 local_from_origin = matrix_identity_float4x4;
    double last_presentation = 0;
    int view_w = 0, view_h = 0;  // a view's size in the drawable, for the recommendation

    // Session lifecycle.
    std::deque<XrEventDataBuffer> events;
    bool began = false;
    std::atomic<bool> alive{true};

    // Paths and actions.
    std::vector<std::string> paths;
    std::vector<XrAction_T*> actions;
    XrPath touch_profile = XR_NULL_PATH;
    HandInput hands[2];

    // Which hand each spatial controller is in, from its accessory's chirality once ARKit
    // has loaded it (GCController -> 0 or 1).
    NSMapTable<GCController*, NSNumber*>* hand_of = nil;
};
Runtime g;

XrTime to_xr_time(double seconds) { return (XrTime)(seconds * 1e9); }
double from_xr_time(XrTime t) { return (double)t * 1e-9; }

// ---------------------------------------------------------------------------
// Matrices.
// ---------------------------------------------------------------------------
XrPosef pose_from(simd_float4x4 m) {
    simd_float3x3 r = simd_matrix(m.columns[0].xyz, m.columns[1].xyz, m.columns[2].xyz);
    simd_quatf q = simd_quaternion(r);
    XrPosef p;
    p.orientation = {q.vector.x, q.vector.y, q.vector.z, q.vector.w};
    p.position = {m.columns[3].x, m.columns[3].y, m.columns[3].z};
    return p;
}

simd_float4x4 matrix_from(const XrPosef& p) {
    simd_quatf q = simd_quaternion(p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w);
    simd_float4x4 m = simd_matrix4x4(q);
    m.columns[3] = simd_make_float4(p.position.x, p.position.y, p.position.z, 1.0f);
    return m;
}

// LOCAL: the head's position, and its heading with the pitch and roll taken out.
void set_local_from(simd_float4x4 origin_from_head) {
    const simd_float3 fwd = -origin_from_head.columns[2].xyz;
    const float yaw = atan2f(-fwd.x, -fwd.z);
    simd_float4x4 m = simd_matrix4x4(simd_quaternion(yaw, simd_make_float3(0, 1, 0)));
    m.columns[3] = origin_from_head.columns[3];
    g.origin_from_local = m;
    g.local_from_origin = simd_inverse(m);
    g.have_local = true;
    LOGI("LOCAL space set at head (%.2f, %.2f, %.2f), yaw %.0f deg", m.columns[3].x, m.columns[3].y,
         m.columns[3].z, yaw * 57.2958f);
}

// The field of a view as the tangents of its edges, read off its projection: for a point
// one unit ahead at tangent t, ndc = (P00 t - P20 + P30) / (P33 - P23), set to +-1.
XrFovf fov_from(simd_float4x4 p) {
    const float w = p.columns[3].w - p.columns[2].w;
    const float l = (-w + p.columns[2].x - p.columns[3].x) / p.columns[0].x;
    const float r = (w + p.columns[2].x - p.columns[3].x) / p.columns[0].x;
    const float d = (-w + p.columns[2].y - p.columns[3].y) / p.columns[1].y;
    const float u = (w + p.columns[2].y - p.columns[3].y) / p.columns[1].y;
    return {atanf(l), atanf(r), atanf(u), atanf(d)};
}

// ---------------------------------------------------------------------------
// Metal: one pipeline draws a textured unit quad, as the projection layer's image across a
// whole view or as a quad layer in the room.
// ---------------------------------------------------------------------------
const char* kShaders = R"(
#include <metal_stdlib>
using namespace metal;
struct U { float4x4 mvp; };
struct V { float4 pos [[position]]; float2 uv; };
vertex V vs(uint vid [[vertex_id]], constant U& u [[buffer(0)]]) {
    float2 c = float2((vid & 1) ? 0.5 : -0.5, (vid & 2) ? 0.5 : -0.5);
    V o;
    o.pos = u.mvp * float4(c, 0.0, 1.0);
    // An image's first row in memory is GL's bottom row, which is Metal's v = 0.
    o.uv = float2(c.x + 0.5, c.y + 0.5);
    return o;
}
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    return float4(t.sample(s, in.uv).rgb, 1.0);
}
)";

bool metal_init() {
    g.device = cp_layer_renderer_get_device(g.layer);
    // ANGLE draws on the system's default device, and the textures are shared with it, so
    // the compositor's had better be the same one. A Vision Pro has one GPU.
    EGLAttrib egl_device = 0, angle_device = 0;
    if (vos::angle::QueryDisplayAttribEXT && vos::angle::QueryDeviceAttribEXT &&
        vos::angle::QueryDisplayAttribEXT(vos::angle::display(), EGL_DEVICE_EXT, &egl_device) &&
        vos::angle::QueryDeviceAttribEXT((EGLDeviceEXT)egl_device, EGL_METAL_DEVICE_ANGLE, &angle_device) &&
        (__bridge id<MTLDevice>)(void*)angle_device != g.device)
        LOGE("ANGLE and the compositor are on different Metal devices");
    g.queue = [g.device newCommandQueue];
    g.rendered = [g.device newSharedEvent];
    NSError* err = nil;
    id<MTLLibrary> lib = [g.device newLibraryWithSource:@(kShaders) options:nil error:&err];
    if (!lib) {
        LOGE("compositing shaders: %s", err.localizedDescription.UTF8String);
        return false;
    }
    cp_layer_renderer_configuration_t cfg = cp_layer_renderer_get_configuration(g.layer);
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
    pd.colorAttachments[0].pixelFormat = cp_layer_renderer_configuration_get_color_format(cfg);
    pd.depthAttachmentPixelFormat = cp_layer_renderer_configuration_get_depth_format(cfg);
    g.pipe = [g.device newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!g.pipe) {
        LOGE("compositing pipeline: %s", err.localizedDescription.UTF8String);
        return false;
    }
    // Reverse Z, as the compositor's projections are: 0 is infinitely far.
    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = YES;
    g.depth_always = [g.device newDepthStencilStateWithDescriptor:dd];
    dd.depthCompareFunction = MTLCompareFunctionGreaterEqual;
    g.depth_greater = [g.device newDepthStencilStateWithDescriptor:dd];
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    g.sampler = [g.device newSamplerStateWithDescriptor:sd];
    LOGI("compositing into %s color, %s depth",
         cp_layer_renderer_configuration_get_color_format(cfg) == MTLPixelFormatBGRA8Unorm_sRGB ? "BGRA8 sRGB" : "other",
         cp_layer_renderer_configuration_get_depth_format(cfg) == MTLPixelFormatDepth32Float ? "32F" : "other");
    return true;
}

// ---------------------------------------------------------------------------
// ARKit.
// ---------------------------------------------------------------------------
bool arkit_init() {
    if (!ar_world_tracking_provider_is_supported()) {
        LOGE("world tracking is not supported here");
        return false;
    }
    g.session = ar_session_create();
    ar_world_tracking_configuration_t wc = ar_world_tracking_configuration_create();
    g.world = ar_world_tracking_provider_create(wc);
    ar_session_set_data_provider_state_change_handler(g.session, nil,
        ^(ar_data_providers_t, ar_data_provider_state_t state, ar_error_t error, ar_data_provider_t) {
            LOGI("world tracking state %ld%s", (long)state, error ? " (error)" : "");
        });
    ar_session_run(g.session, ar_data_providers_create_with_data_providers(g.world, nil));
    return true;
}

// The accessory session, rebuilt whenever the set of controllers changes: a provider is
// configured once, at creation. A session of its own so the head's tracking never restarts.
void rebuild_accessory_tracking() {
    std::lock_guard<std::mutex> lk(g.acc_mutex);
    if (g.acc_session) ar_session_stop(g.acc_session);
    g.acc_session = nil;
    g.acc_provider = nil;
    if (!g.accessories || ar_accessories_get_count(g.accessories) == 0) return;
    if (!ar_accessory_tracking_provider_is_supported()) {
        LOGE("accessory tracking is not supported here");
        return;
    }
    ar_accessory_tracking_configuration_t c = ar_accessory_tracking_configuration_create();
    ar_accessory_tracking_configuration_set_accessories(c, g.accessories);
    g.acc_provider = ar_accessory_tracking_provider_create(c);
    g.acc_session = ar_session_create();
    ar_session_set_data_provider_state_change_handler(g.acc_session, nil,
        ^(ar_data_providers_t, ar_data_provider_state_t state, ar_error_t error, ar_data_provider_t) {
            LOGI("accessory tracking state %ld%s", (long)state, error ? " (error)" : "");
        });
    ar_session_run(g.acc_session, ar_data_providers_create_with_data_providers(g.acc_provider, nil));
    LOGI("tracking %zu controller(s)", ar_accessories_get_count(g.accessories));
}

// ---------------------------------------------------------------------------
// Controllers.
// ---------------------------------------------------------------------------
bool is_spatial(GCController* c) { return [c.productCategory isEqualToString:GCProductCategorySpatialController]; }

void log_elements(GCController* c) {
    NSMutableArray* names = [NSMutableArray array];
    for (NSString* n in c.physicalInputProfile.elements) [names addObject:n];
    LOGI("controller '%s' (%s): %s", c.vendorName.UTF8String ?: "?", c.productCategory.UTF8String,
         [[names componentsJoinedByString:@", "] UTF8String]);
}

void controller_connected(GCController* c) {
    log_elements(c);
    if (!is_spatial(c)) return;
    ar_accessory_load_from_device(c, ^(id<GCDevice>, bool ok, ar_error_t, ar_accessory_t acc) {
        if (!ok || !acc) {
            LOGE("could not load '%s' as a tracked accessory", c.vendorName.UTF8String ?: "?");
            return;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            const ar_accessory_chirality_t ch = ar_accessory_get_inherent_chirality(acc);
            const int hand = ch == ar_accessory_chirality_left ? 0 : 1;
            [g.hand_of setObject:@(hand) forKey:c];
            LOGI("'%s' is the %s controller", ar_accessory_get_name(acc), hand ? "right" : "left");
            if (!g.accessories) g.accessories = ar_accessories_create();
            ar_accessories_add_accessory(g.accessories, acc);
            rebuild_accessory_tracking();
        });
    });
}

void controller_disconnected(GCController* c) {
    LOGI("controller '%s' disconnected", c.vendorName.UTF8String ?: "?");
    if (!is_spatial(c)) return;
    [g.hand_of removeObjectForKey:c];
    if (!g.accessories) return;
    ar_accessories_t keep = ar_accessories_create();
    ar_accessories_enumerate_accessories(g.accessories, ^bool(ar_accessory_t acc) {
        if (ar_accessory_get_source_device(acc) != (id<GCDevice>)c) ar_accessories_add_accessory(keep, acc);
        return true;
    });
    g.accessories = keep;
    rebuild_accessory_tracking();
}

GCControllerButtonInput* button(GCPhysicalInputProfile* p, NSString* name) { return name ? p.buttons[name] : nil; }

// One spatial controller's buttons into its hand.
void read_spatial(GCController* c, HandInput& h) {
    GCPhysicalInputProfile* p = c.physicalInputProfile;
    auto pressed = [&](std::initializer_list<NSString*> names) {
        for (NSString* n : names)
            if (GCControllerButtonInput* b = button(p, n)) if (b.isPressed) return true;
        return false;
    };
    auto value = [&](std::initializer_list<NSString*> names) {
        float v = 0;
        for (NSString* n : names)
            if (GCControllerButtonInput* b = button(p, n)) v = fmaxf(v, b.value);
        return v;
    };
    NSString* grip = GCInputGripButton;
    h.present = true;
    h.a |= pressed({GCInputButtonA});
    h.b |= pressed({GCInputButtonB});
    h.menu |= pressed({GCInputButtonMenu, GCInputButtonOptions});
    h.stick_click |= pressed({GCInputThumbstickButton, GCInputLeftThumbstickButton, GCInputRightThumbstickButton});
    h.trigger = fmaxf(h.trigger, value({GCInputTrigger, GCInputLeftTrigger, GCInputRightTrigger}));
    h.grip = fmaxf(h.grip, value({grip, GCInputLeftShoulder, GCInputRightShoulder}));
    for (NSString* n : {GCInputThumbstick, GCInputLeftThumbstick, GCInputRightThumbstick}) {
        GCControllerDirectionPad* d = p.dpads[n];
        if (!d) continue;
        const float x = d.xAxis.value, y = d.yAxis.value;
        if (x * x + y * y > h.sx * h.sx + h.sy * h.sy) {
            h.sx = x;
            h.sy = y;
        }
    }
}

void read_hands() {
    for (auto& h : g.hands) h = HandInput{};
    for (GCController* c in GCController.controllers) {
        if (is_spatial(c)) {
            NSNumber* hand = [g.hand_of objectForKey:c];
            if (hand) read_spatial(c, g.hands[hand.intValue]);
        } else if (GCExtendedGamepad* gp = c.extendedGamepad) {
            // The gamepad's stick clicks are the core's: the right one switches views, the
            // left is the game's. The rest of it is merged as a pad (merge_gamepad).
            if (gp.leftThumbstickButton.isPressed) g.hands[0].stick_click = true;
            if (gp.rightThumbstickButton.isPressed) g.hands[1].stick_click = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Paths.
// ---------------------------------------------------------------------------
XrPath path_of(const char* s) {
    for (size_t i = 0; i < g.paths.size(); i++)
        if (g.paths[i] == s) return (XrPath)(i + 1);
    g.paths.emplace_back(s);
    return (XrPath)g.paths.size();
}

const std::string& path_str(XrPath p) {
    static const std::string none;
    return p >= 1 && p <= g.paths.size() ? g.paths[p - 1] : none;
}

// A Touch controller's binding path as one of the inputs above.
Binding binding_of(const std::string& path) {
    int hand;
    std::string rest;
    if (path.rfind("/user/hand/left/input/", 0) == 0) { hand = 0; rest = path.substr(22); }
    else if (path.rfind("/user/hand/right/input/", 0) == 0) { hand = 1; rest = path.substr(23); }
    else return {0, kSrcNone};
    // A Sense controller calls its two face buttons A and B on either hand; on the left
    // they stand where Touch has X and Y.
    if (rest == "a/click" || rest == "x/click") return {hand, kSrcA};
    if (rest == "b/click" || rest == "y/click") return {hand, kSrcB};
    if (rest == "menu/click") return {hand, kSrcMenu};
    if (rest == "trigger/value") return {hand, kSrcTrigger};
    if (rest == "squeeze/value") return {hand, kSrcGrip};
    if (rest == "thumbstick") return {hand, kSrcStick};
    if (rest == "thumbstick/click") return {hand, kSrcStickClick};
    if (rest == "aim/pose") return {hand, kSrcAim};
    return {hand, kSrcNone};
}

// ---------------------------------------------------------------------------
// Frames.
// ---------------------------------------------------------------------------
void push_state(XrSessionState s) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    auto* sc = (XrEventDataSessionStateChanged*)&ev;
    sc->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    sc->session = (XrSession)&g;
    sc->state = s;
    g.events.push_back(ev);
}

void lost() {
    if (g.alive.exchange(false)) {
        LOGI("the immersive space has gone");
        push_state(XR_SESSION_STATE_LOSS_PENDING);
    }
}

// Takes the next frame as far as its drawables, waiting for the moment the compositor wants
// input sampled. False if there is no frame to draw this time round.
bool next_frame() {
    g.frame = nullptr;
    g.drawables.clear();
    switch (cp_layer_renderer_get_state(g.layer)) {
    case cp_layer_renderer_state_invalidated: lost(); return false;
    case cp_layer_renderer_state_paused: cp_layer_renderer_wait_until_running(g.layer); return false;
    default: break;
    }
    cp_frame_t f = cp_layer_renderer_query_next_frame(g.layer);
    if (!f) return false;
    cp_frame_timing_t t = cp_frame_predict_timing(f);
    if (!t) return false;
    cp_frame_start_update(f);
    cp_frame_end_update(f);
    cp_time_wait_until(cp_frame_timing_get_optimal_input_time(t));
    cp_frame_start_submission(f);
    cp_drawable_array_t arr = cp_frame_query_drawables(f);
    for (size_t i = 0; arr && i < cp_drawable_array_get_count(arr); i++)
        g.drawables.push_back(cp_drawable_array_get_drawable(arr, i));
    g.frame = f;
    if (g.drawables.empty()) {
        cp_frame_end_submission(f);
        g.frame = nullptr;
        return false;
    }
    // The built-in display's drawable is the one the head is located for; a capture's
    // follows the same anchor.
    cp_drawable_t d = g.drawables[0];
    cp_frame_timing_t dt = cp_drawable_get_frame_timing(d);
    ar_device_anchor_t a = ar_device_anchor_create();
    const double anchor_time = cp_time_to_cf_time_interval(cp_frame_timing_get_trackable_anchor_time(dt));
    g.anchor_valid = ar_world_tracking_provider_query_device_anchor_at_timestamp(g.world, anchor_time, a) ==
                         ar_device_anchor_query_status_success &&
                     ar_device_anchor_is_tracked(a);
    if (g.anchor_valid) {
        g.anchor = a;
        if (!g.have_local) set_local_from(ar_device_anchor_get_origin_from_anchor_transform(a));
    }
    for (cp_drawable_t dr : g.drawables) cp_drawable_set_device_anchor(dr, g.anchor_valid ? g.anchor : nil);
    return true;
}

// Ends a frame that drew nothing: each drawable still has to be presented.
void present_black() {
    if (!g.frame) return;
    id<MTLCommandBuffer> cb = [g.queue commandBuffer];
    for (cp_drawable_t d : g.drawables) {
        for (size_t t = 0; t < cp_drawable_get_texture_count(d); t++) {
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor new];
            rp.colorAttachments[0].texture = cp_drawable_get_color_texture(d, t);
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.depthAttachment.texture = cp_drawable_get_depth_texture(d, t);
            rp.depthAttachment.loadAction = MTLLoadActionClear;
            rp.depthAttachment.clearDepth = 0.0;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            rp.renderTargetArrayLength = rp.colorAttachments[0].texture.arrayLength;
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        }
        cp_drawable_encode_present(d, cb);
    }
    [cb commit];
    cp_frame_end_submission(g.frame);
    g.frame = nullptr;
    g.drawables.clear();
}

simd_float4x4 mvp_for_quad(cp_drawable_t d, size_t view, simd_float4x4 origin_from_device,
                           const XrCompositionLayerQuad& q) {
    cp_view_t v = cp_drawable_get_view(d, view);
    const simd_float4x4 origin_from_view = simd_mul(origin_from_device, cp_view_get_transform(v));
    const simd_float4x4 view_from_local = simd_mul(simd_inverse(origin_from_view), g.origin_from_local);
    simd_float4x4 local_from_quad = matrix_from(q.pose);
    local_from_quad.columns[0] *= q.size.width;
    local_from_quad.columns[1] *= q.size.height;
    const simd_float4x4 proj = cp_drawable_compute_projection(d, cp_axis_direction_convention_right_up_back, view);
    return simd_mul(proj, simd_mul(view_from_local, local_from_quad));
}

}  // namespace

// ---------------------------------------------------------------------------
// The shell's side.
// ---------------------------------------------------------------------------
namespace vos {

void set_layer_renderer(void* layer) { g.layer = (__bridge cp_layer_renderer_t)layer; }
bool layer_alive() { return g.alive; }

void start_controllers() {
    g.hand_of = [NSMapTable weakToStrongObjectsMapTable];
    dispatch_async(dispatch_get_main_queue(), ^{
        NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
        [nc addObserverForName:GCControllerDidConnectNotification object:nil queue:nil
                    usingBlock:^(NSNotification* n) { controller_connected(n.object); }];
        [nc addObserverForName:GCControllerDidDisconnectNotification object:nil queue:nil
                    usingBlock:^(NSNotification* n) { controller_disconnected(n.object); }];
        for (GCController* c in GCController.controllers) controller_connected(c);
    });
}

void merge_gamepad(PadState& p) {
    for (GCController* c in GCController.controllers) {
        if (is_spatial(c)) continue;
        GCExtendedGamepad* gp = c.extendedGamepad;
        if (!gp) continue;
        // As the desktop lays a gamepad out: south is A, west B, east X, north Y, the right
        // shoulder Z, the triggers L and R.
        if (gp.buttonA.isPressed) p.buttons |= PAD_A;
        if (gp.buttonX.isPressed) p.buttons |= PAD_B;
        if (gp.buttonB.isPressed) p.buttons |= PAD_X;
        if (gp.buttonY.isPressed) p.buttons |= PAD_Y;
        if (gp.buttonMenu.isPressed) p.buttons |= PAD_START;
        if (gp.rightShoulder.isPressed) p.buttons |= PAD_Z;
        if (gp.dpad.up.isPressed) p.buttons |= PAD_UP;
        if (gp.dpad.down.isPressed) p.buttons |= PAD_DOWN;
        if (gp.dpad.left.isPressed) p.buttons |= PAD_LEFT;
        if (gp.dpad.right.isPressed) p.buttons |= PAD_RIGHT;
        auto to_u8 = [](float f) {
            int v = 128 + (int)(f * 100.0f);
            return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        };
        const float lx = gp.leftThumbstick.xAxis.value, ly = gp.leftThumbstick.yAxis.value;
        if (fabsf(lx) > 0.12f || fabsf(ly) > 0.12f) { p.stick_x = to_u8(lx); p.stick_y = to_u8(ly); }
        const float rx = gp.rightThumbstick.xAxis.value, ry = gp.rightThumbstick.yAxis.value;
        if (fabsf(rx) > 0.05f || fabsf(ry) > 0.05f) { p.cstick_x = to_u8(rx); p.cstick_y = to_u8(ry); }
        const float lt = gp.leftTrigger.value, rt = gp.rightTrigger.value;
        if (lt > 0.03f) { p.trig_l = std::max(p.trig_l, (uint8_t)(lt * 255.0f)); if (lt > 0.9f) p.buttons |= PAD_L; }
        if (rt > 0.03f) { p.trig_r = std::max(p.trig_r, (uint8_t)(rt * 255.0f)); if (rt > 0.9f) p.buttons |= PAD_R; }
    }
}

}  // namespace vos

// ---------------------------------------------------------------------------
// OpenXR.
// ---------------------------------------------------------------------------
extern "C" {

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(const char*, uint32_t cap, uint32_t* n,
                                                                      XrExtensionProperties* props) {
    static const char* exts[] = {"XR_KHR_opengl_es_enable"};
    *n = 1;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 1) return XR_ERROR_SIZE_INSUFFICIENT;
    snprintf(props[0].extensionName, sizeof(props[0].extensionName), "%s", exts[0]);
    props[0].extensionVersion = 1;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo*, XrInstance* instance) {
    static XrInstance_T inst;
    *instance = &inst;
    g.touch_profile = path_of("/interaction_profiles/oculus/touch_controller");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProperties(XrInstance, XrInstanceProperties* p) {
    snprintf(p->runtimeName, sizeof(p->runtimeName), "gcn-recomp visionOS");
    p->runtimeVersion = XR_MAKE_VERSION(1, 0, 0);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetSystem(XrInstance, const XrSystemGetInfo*, XrSystemId* id) {
    *id = 1;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* p) {
    snprintf(p->systemName, sizeof(p->systemName), "Apple Vision Pro");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance, const char*, PFN_xrVoidFunction* f) {
    *f = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

// The session: brings up Metal and ARKit and waits for the immersive space to be shown, then
// draws one empty frame to learn the size of a view.
XRAPI_ATTR XrResult XRAPI_CALL xrCreateSession(XrInstance, const XrSessionCreateInfo*, XrSession* s) {
    if (!g.layer || vos::angle::display() == EGL_NO_DISPLAY) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    if (!metal_init() || !arkit_init()) return XR_ERROR_INITIALIZATION_FAILED;
    LOGI("waiting for the immersive space");
    cp_layer_renderer_wait_until_running(g.layer);
    for (int tries = 0; tries < 600 && g.view_w == 0; tries++) {
        if (!next_frame()) {
            if (!g.alive) return XR_ERROR_SESSION_LOST;
            continue;
        }
        cp_drawable_t d = g.drawables[0];
        cp_view_texture_map_t m = cp_view_get_view_texture_map(cp_drawable_get_view(d, 0));
        const MTLViewport vp = cp_view_texture_map_get_viewport(m);
        g.view_w = (int)vp.width;
        g.view_h = (int)vp.height;
        LOGI("%zu views of %dx%d, in %zu texture(s)", cp_drawable_get_view_count(d), g.view_w, g.view_h,
             cp_drawable_get_texture_count(d));
        present_black();
    }
    if (g.view_w == 0) return XR_ERROR_INITIALIZATION_FAILED;
    *s = (XrSession)&g;
    push_state(XR_SESSION_STATE_IDLE);
    push_state(XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrBeginSession(XrSession, const XrSessionBeginInfo*) {
    g.began = true;
    push_state(XR_SESSION_STATE_SYNCHRONIZED);
    push_state(XR_SESSION_STATE_VISIBLE);
    push_state(XR_SESSION_STATE_FOCUSED);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEndSession(XrSession) {
    g.began = false;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrPollEvent(XrInstance, XrEventDataBuffer* ev) {
    if (g.events.empty()) return XR_EVENT_UNAVAILABLE;
    *ev = g.events.front();
    g.events.pop_front();
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo* ci, XrSpace* s) {
    if (ci->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL) return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    *s = new XrSpace_T{XrSpace_T::kLocal, 0};
    return XR_SUCCESS;
}

// Swapchains.
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession, uint32_t cap, uint32_t* n, int64_t* f) {
    *n = 1;
    if (cap == 0) return XR_SUCCESS;
    f[0] = GL_RGBA8;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType,
                                                                 uint32_t cap, uint32_t* n,
                                                                 XrViewConfigurationView* v) {
    *n = 2;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    for (int i = 0; i < 2; i++) {
        v[i].recommendedImageRectWidth = (uint32_t)g.view_w;
        v[i].recommendedImageRectHeight = (uint32_t)g.view_h;
        v[i].maxImageRectWidth = (uint32_t)g.view_w * 2;
        v[i].maxImageRectHeight = (uint32_t)g.view_h * 2;
        v[i].recommendedSwapchainSampleCount = 1;
        v[i].maxSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(XrSession, const XrSwapchainCreateInfo* ci, XrSwapchain* out) {
    auto* sc = new XrSwapchain_T;
    sc->w = (int)ci->width;
    sc->h = (int)ci->height;
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:sc->w
                                                                                 height:sc->h
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    td.storageMode = MTLStorageModePrivate;
    const EGLint no_attr[] = {EGL_NONE};
    for (int i = 0; i < kImages; i++) {
        auto img = std::make_unique<XrSwapchain_T::Image>();
        img->mtl = [g.device newTextureWithDescriptor:td];
        // Read as sRGB: the renderer's output is the game's gamma-encoded colour, so the
        // compositing decodes it as it samples and the sRGB drawable encodes it again,
        // storing what the renderer wrote.
        img->srgb = [img->mtl newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB];
        EGLImageKHR image = vos::angle::CreateImageKHR(vos::angle::display(), EGL_NO_CONTEXT, EGL_METAL_TEXTURE_ANGLE,
                                                       (__bridge EGLClientBuffer)img->mtl, no_attr);
        if (!img->mtl || !img->srgb || image == EGL_NO_IMAGE_KHR) {
            LOGE("swapchain image %dx%d: no texture (EGL 0x%x)", sc->w, sc->h, vos::angle::GetError());
            return XR_ERROR_RUNTIME_FAILURE;
        }
        glGenTextures(1, &img->tex);
        glBindTexture(GL_TEXTURE_2D, img->tex);
        vos::angle::EGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        sc->images.push_back(std::move(img));
    }
    *out = sc;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain sc, uint32_t cap, uint32_t* n,
                                                          XrSwapchainImageBaseHeader* images) {
    *n = (uint32_t)sc->images.size();
    if (cap == 0) return XR_SUCCESS;
    if (cap < *n) return XR_ERROR_SIZE_INSUFFICIENT;
    // XrSwapchainImageOpenGLESKHR: the header, then the texture name.
    struct GlImage { XrStructureType type; void* next; uint32_t image; };
    auto* out = (GlImage*)images;
    for (uint32_t i = 0; i < *n; i++) out[i].image = sc->images[i]->tex;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain sc, const XrSwapchainImageAcquireInfo*,
                                                       uint32_t* index) {
    if (sc->acquired >= 0) return XR_ERROR_CALL_ORDER_INVALID;
    sc->acquired = (int)sc->next;
    sc->next = (sc->next + 1) % sc->images.size();
    *index = (uint32_t)sc->acquired;
    return XR_SUCCESS;
}

// Waits until no compositing still reads the image.
XRAPI_ATTR XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain sc, const XrSwapchainImageWaitInfo*) {
    if (sc->acquired < 0) return XR_ERROR_CALL_ORDER_INVALID;
    auto& img = *sc->images[sc->acquired];
    for (int spins = 0; img.in_flight.load() > 0 && spins < 100000; spins++)
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain sc, const XrSwapchainImageReleaseInfo*) {
    if (sc->acquired < 0) return XR_ERROR_CALL_ORDER_INVALID;
    sc->images[sc->acquired]->anchor = g.anchor;
    sc->released = sc->acquired;
    sc->acquired = -1;
    return XR_SUCCESS;
}

// Frames.
XRAPI_ATTR XrResult XRAPI_CALL xrWaitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState* fs) {
    if (!g.alive) return XR_ERROR_SESSION_LOST;
    if (!next_frame()) {
        fs->shouldRender = XR_FALSE;
        fs->predictedDisplayTime = to_xr_time(g.last_presentation);
        return g.alive ? XR_SUCCESS : XR_ERROR_SESSION_LOST;
    }
    const double pt = cp_time_to_cf_time_interval(
        cp_frame_timing_get_presentation_time(cp_drawable_get_frame_timing(g.drawables[0])));
    fs->predictedDisplayPeriod = g.last_presentation > 0 && pt > g.last_presentation
                                     ? to_xr_time(pt - g.last_presentation)
                                     : to_xr_time(1.0 / 90.0);
    g.last_presentation = pt;
    fs->predictedDisplayTime = to_xr_time(pt);
    fs->shouldRender = g.anchor_valid && g.have_local ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrBeginFrame(XrSession, const XrFrameBeginInfo*) { return XR_SUCCESS; }

// Composites the layers into this frame's drawables and presents them.
XRAPI_ATTR XrResult XRAPI_CALL xrEndFrame(XrSession, const XrFrameEndInfo* fe) {
    if (!g.frame) return XR_SUCCESS;  // xrWaitFrame found nothing to draw into
    // What the renderer drew into the images has to be finished before Metal reads them:
    // ANGLE signals the event once the work submitted so far is done, and the compositing
    // waits for it on the GPU.
    const uint64_t rendered_value = ++g.rendered_value;
    const EGLAttrib sync_attr[] = {
        EGL_SYNC_METAL_SHARED_EVENT_OBJECT_ANGLE, (EGLAttrib)(__bridge void*)g.rendered,
        EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_ANGLE, (EGLAttrib)(rendered_value & 0xFFFFFFFFu),
        EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_HI_ANGLE, (EGLAttrib)(rendered_value >> 32),
        EGL_NONE,
    };
    EGLSync sync = vos::angle::CreateSync(vos::angle::display(), EGL_SYNC_METAL_SHARED_EVENT_ANGLE, sync_attr);
    glFlush();
    if (sync != EGL_NO_SYNC) vos::angle::DestroySync(vos::angle::display(), sync);

    const XrCompositionLayerProjection* proj = nullptr;
    std::vector<const XrCompositionLayerQuad*> quads;
    for (uint32_t i = 0; i < fe->layerCount; i++) {
        if (fe->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            proj = (const XrCompositionLayerProjection*)fe->layers[i];
        else if (fe->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_QUAD)
            quads.push_back((const XrCompositionLayerQuad*)fe->layers[i]);
    }
    // The pose everything in this frame is drawn for. A projection layer's images carry
    // the anchor they were rendered at, and the drawable takes that one, so the compositor
    // reprojects an old image from where it was drawn; a quad is drawn for the same pose.
    ar_device_anchor_t anchor = g.anchor_valid ? g.anchor : nil;
    if (proj && proj->viewCount >= 1) {
        XrSwapchain sc = proj->views[0].subImage.swapchain;
        if (sc && sc->released >= 0 && sc->images[sc->released]->anchor) anchor = sc->images[sc->released]->anchor;
    }
    const simd_float4x4 origin_from_device =
        anchor ? ar_device_anchor_get_origin_from_anchor_transform(anchor) : g.origin_from_local;

    id<MTLCommandBuffer> cb = [g.queue commandBuffer];
    if (sync != EGL_NO_SYNC) [cb encodeWaitForEvent:g.rendered value:rendered_value];
    std::vector<XrSwapchain_T::Image*> used;
    auto image_of = [&](XrSwapchain sc) -> XrSwapchain_T::Image* {
        if (!sc || sc->released < 0) return nullptr;
        XrSwapchain_T::Image* img = sc->images[sc->released].get();
        used.push_back(img);
        img->in_flight++;
        return img;
    };

    for (cp_drawable_t d : g.drawables) {
        cp_drawable_set_device_anchor(d, anchor);
        const size_t nviews = cp_drawable_get_view_count(d);
        for (size_t v = 0; v < nviews; v++) {
            cp_view_texture_map_t map = cp_view_get_view_texture_map(cp_drawable_get_view(d, v));
            const size_t ti = cp_view_texture_map_get_texture_index(map);
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor new];
            rp.colorAttachments[0].texture = cp_drawable_get_color_texture(d, ti);
            rp.colorAttachments[0].slice = cp_view_texture_map_get_slice_index(map);
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.depthAttachment.texture = cp_drawable_get_depth_texture(d, ti);
            rp.depthAttachment.slice = cp_view_texture_map_get_slice_index(map);
            rp.depthAttachment.loadAction = MTLLoadActionClear;
            rp.depthAttachment.clearDepth = 0.0;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            [enc setViewport:cp_view_texture_map_get_viewport(map)];
            [enc setRenderPipelineState:g.pipe];
            [enc setFragmentSamplerState:g.sampler atIndex:0];
            [enc setCullMode:MTLCullModeNone];

            // The projection layer's image for this eye, across the whole view, at
            // infinity: its depth is unknown, so the compositor reprojects it for rotation.
            if (proj && v < proj->viewCount) {
                if (XrSwapchain_T::Image* img = image_of(proj->views[v].subImage.swapchain)) {
                    simd_float4x4 full = matrix_identity_float4x4;
                    full.columns[0].x = 2.0f;
                    full.columns[1].y = 2.0f;
                    full.columns[2].z = 0.0f;
                    [enc setDepthStencilState:g.depth_always];
                    [enc setVertexBytes:&full length:sizeof(full) atIndex:0];
                    [enc setFragmentTexture:img->srgb atIndex:0];
                    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                }
            }
            // Quads, in the room, with their depth.
            for (const XrCompositionLayerQuad* q : quads) {
                if ((q->eyeVisibility == XR_EYE_VISIBILITY_LEFT && v != 0) ||
                    (q->eyeVisibility == XR_EYE_VISIBILITY_RIGHT && v != 1))
                    continue;
                XrSwapchain_T::Image* img = image_of(q->subImage.swapchain);
                if (!img) continue;
                const simd_float4x4 mvp = mvp_for_quad(d, v, origin_from_device, *q);
                [enc setDepthStencilState:g.depth_greater];
                [enc setVertexBytes:&mvp length:sizeof(mvp) atIndex:0];
                [enc setFragmentTexture:img->srgb atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            }
            [enc endEncoding];
        }
        cp_drawable_encode_present(d, cb);
    }
    [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
        for (XrSwapchain_T::Image* img : used) img->in_flight--;
    }];
    [cb commit];
    cp_frame_end_submission(g.frame);
    g.frame = nullptr;
    g.drawables.clear();
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrLocateViews(XrSession, const XrViewLocateInfo*, XrViewState* vs, uint32_t cap,
                                             uint32_t* n, XrView* views) {
    *n = 2;
    vs->viewStateFlags = 0;
    if (cap < 2) return cap == 0 ? XR_SUCCESS : XR_ERROR_SIZE_INSUFFICIENT;
    if (g.drawables.empty() || !g.anchor_valid || !g.have_local) return XR_SUCCESS;
    cp_drawable_t d = g.drawables[0];
    const simd_float4x4 local_from_device =
        simd_mul(g.local_from_origin, ar_device_anchor_get_origin_from_anchor_transform(g.anchor));
    for (size_t v = 0; v < 2 && v < cp_drawable_get_view_count(d); v++) {
        views[v].pose = pose_from(simd_mul(local_from_device, cp_view_get_transform(cp_drawable_get_view(d, v))));
        views[v].fov = fov_from(cp_drawable_compute_projection(d, cp_axis_direction_convention_right_up_back, v));
    }
    vs->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                         XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    return XR_SUCCESS;
}

// Paths.
XRAPI_ATTR XrResult XRAPI_CALL xrStringToPath(XrInstance, const char* s, XrPath* p) {
    *p = path_of(s);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrPathToString(XrInstance, XrPath p, uint32_t cap, uint32_t* n, char* buf) {
    const std::string& s = path_str(p);
    if (s.empty()) return XR_ERROR_PATH_INVALID;
    *n = (uint32_t)s.size() + 1;
    if (cap == 0) return XR_SUCCESS;
    if (cap < *n) return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(buf, s.c_str(), *n);
    return XR_SUCCESS;
}

// Actions.
XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSet(XrInstance, const XrActionSetCreateInfo*, XrActionSet* s) {
    static XrActionSet_T set;
    *s = &set;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateAction(XrActionSet, const XrActionCreateInfo* ci, XrAction* a) {
    auto* act = new XrAction_T;
    act->type = ci->actionType;
    act->subactions.assign(ci->subactionPaths, ci->subactionPaths + ci->countSubactionPaths);
    g.actions.push_back(act);
    *a = act;
    return XR_SUCCESS;
}

// Only the Touch profile's bindings are taken: a Sense controller has its layout.
XRAPI_ATTR XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(XrInstance,
                                                                   const XrInteractionProfileSuggestedBinding* sb) {
    if (sb->interactionProfile != g.touch_profile) return XR_ERROR_PATH_UNSUPPORTED;
    for (uint32_t i = 0; i < sb->countSuggestedBindings; i++) {
        const Binding b = binding_of(path_str(sb->suggestedBindings[i].binding));
        if (b.src != kSrcNone) sb->suggestedBindings[i].action->binds.push_back(b);
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrAttachSessionActionSets(XrSession, const XrSessionActionSetsAttachInfo*) {
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSpace(XrSession, const XrActionSpaceCreateInfo* ci, XrSpace* s) {
    const std::string& p = path_str(ci->subactionPath);
    *s = new XrSpace_T{XrSpace_T::kAim, p == "/user/hand/left" ? 0 : 1};
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrSyncActions(XrSession, const XrActionsSyncInfo*) {
    read_hands();
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateBoolean(XrSession, const XrActionStateGetInfo* gi,
                                                       XrActionStateBoolean* st) {
    st->isActive = XR_FALSE;
    st->currentState = XR_FALSE;
    for (const Binding& b : gi->action->binds) {
        const HandInput& h = g.hands[b.hand];
        bool v = false;
        switch (b.src) {
        case kSrcA: v = h.a; break;
        case kSrcB: v = h.b; break;
        // Either controller's menu button is Start: the Sense controllers have one each.
        case kSrcMenu: v = g.hands[0].menu || g.hands[1].menu; break;
        case kSrcStickClick: v = h.stick_click; break;
        default: continue;
        }
        st->isActive = XR_TRUE;
        if (v) st->currentState = XR_TRUE;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateFloat(XrSession, const XrActionStateGetInfo* gi,
                                                     XrActionStateFloat* st) {
    st->isActive = XR_FALSE;
    st->currentState = 0;
    for (const Binding& b : gi->action->binds) {
        const HandInput& h = g.hands[b.hand];
        float v;
        if (b.src == kSrcTrigger) v = h.trigger;
        else if (b.src == kSrcGrip) v = h.grip;
        else continue;
        st->isActive = XR_TRUE;
        st->currentState = fmaxf(st->currentState, v);
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateVector2f(XrSession, const XrActionStateGetInfo* gi,
                                                        XrActionStateVector2f* st) {
    st->isActive = XR_FALSE;
    st->currentState = {0, 0};
    for (const Binding& b : gi->action->binds) {
        if (b.src != kSrcStick) continue;
        const HandInput& h = g.hands[b.hand];
        st->isActive = XR_TRUE;
        if (h.sx * h.sx + h.sy * h.sy > st->currentState.x * st->currentState.x + st->currentState.y * st->currentState.y)
            st->currentState = {h.sx, h.sy};
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetCurrentInteractionProfile(XrSession, XrPath hand, XrInteractionProfileState* st) {
    const int h = path_str(hand) == "/user/hand/left" ? 0 : 1;
    st->interactionProfile = g.hands[h].present ? g.touch_profile : XR_NULL_PATH;
    return XR_SUCCESS;
}

// A controller's aim pose: ARKit's prediction of the accessory at `time`, at its aim
// location, in LOCAL.
XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace space, XrSpace, XrTime time, XrSpaceLocation* loc) {
    loc->locationFlags = 0;
    if (space->kind != XrSpace_T::kAim || !g.have_local) return XR_SUCCESS;
    std::lock_guard<std::mutex> lk(g.acc_mutex);
    if (!g.acc_provider) return XR_SUCCESS;
    ar_accessory_anchors_t anchors = ar_accessory_tracking_provider_get_latest_anchors(g.acc_provider);
    if (!anchors) return XR_SUCCESS;
    __block ar_accessory_anchor_t found = nil;
    const int want = space->hand;
    ar_accessory_anchors_enumerate_anchors(anchors, ^bool(ar_accessory_anchor_t a) {
        ar_accessory_t acc = ar_accessory_anchor_get_accessory(a);
        ar_accessory_chirality_t ch = ar_accessory_anchor_is_held(a) ? ar_accessory_anchor_get_held_chirality(a)
                                                                      : ar_accessory_get_inherent_chirality(acc);
        if ((ch == ar_accessory_chirality_left ? 0 : 1) == want) {
            found = a;
            return false;
        }
        return true;
    });
    if (!found || !ar_accessory_anchor_is_tracked(found)) return XR_SUCCESS;
    ar_accessory_anchor_t predicted = ar_accessory_anchor_create();
    if (!ar_accessory_tracking_provider_predict_anchor_at_timestamp(g.acc_provider, found, from_xr_time(time),
                                                                   predicted))
        predicted = found;
    const simd_float4x4 origin_from_anchor =
        ar_accessory_anchor_get_origin_from_anchor_transform_with_correction(predicted, ar_transform_correction_rendered);
    const simd_float4x4 anchor_from_aim = ar_accessory_anchor_get_anchor_from_location_transform_with_correction(
        predicted, ar_accessory_location_name_aim, ar_transform_correction_rendered);
    loc->pose = pose_from(simd_mul(g.local_from_origin, simd_mul(origin_from_anchor, anchor_from_aim)));
    loc->locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                         XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    return XR_SUCCESS;
}

}  // extern "C"
