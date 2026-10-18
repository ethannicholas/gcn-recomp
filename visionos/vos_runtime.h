// What the visionOS shell (visionos_main.mm) tells the OpenXR runtime in xr_runtime.mm, and
// what that runtime offers it besides OpenXR.
//
// There is no OpenXR on visionOS. xr_runtime.mm implements the part of it the frontend core
// (openxr/xr_frontend.cpp) calls, over CompositorServices for the frames, ARKit for the head
// and the tracked controllers, and GameController for buttons, so the core is the same one
// a Quest runs. Kept free of Objective-C so the C++ side can include it.
#pragma once

struct PadState;

namespace vos {

// Before xrCreateSession: the layer the immersive space handed the app (a
// cp_layer_renderer_t, as a bridged pointer). ANGLE's context (angle.h) must be current on
// the thread that will render by then.
void set_layer_renderer(void* layer_renderer);

// Starts watching for controllers: gamepads, and spatial controllers along with their
// tracking. Called once, early; their buttons are read from then on.
void start_controllers();

// A gamepad's state, merged into the pad the spatial controllers built
// (xrf::Platform::merge_input). A gamepad has a D-pad and a Z button of its own, which no
// OpenXR action in the core carries, so it is read here rather than through actions.
void merge_gamepad(PadState& p);

// False once the immersive space has gone: the viewer left it, or the system took it.
bool layer_alive();

}  // namespace vos
