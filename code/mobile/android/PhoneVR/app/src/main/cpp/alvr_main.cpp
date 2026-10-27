#include "alvr_client_core.h"
#include "arcore_c_api.h"
#include "cardboard.h"
#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include <algorithm>
#include <android/log.h>
#include <cmath>
#include <cstring>
#include <deque>
#include <jni.h>
#include <map>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

#include "head_sensor_source.h"
#include "neck_model.h"
#include "utils.h"


#ifndef LOG
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "ALVR_EGLCHK", __VA_ARGS__)
#endif
#define EGLCHK(where) LOG("EGLCHK " where " ctx=%p surf=%p", eglGetCurrentContext(), eglGetCurrentSurface(EGL_DRAW))

struct EglStateGuard {
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext ctx = eglGetCurrentContext();
    EGLSurface draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface read = eglGetCurrentSurface(EGL_READ);
    ~EglStateGuard() {
        if (ctx != EGL_NO_CONTEXT && eglGetCurrentContext() != ctx)
            eglMakeCurrent(dpy, draw, read, ctx);
    }
};

// TEMP DEBUG: burn texture names before creating the lobby swapchain, so the names handed to the
// ALVR library (17,18 instead of 1,2) can't coincide with names its own EGL context already uses.
// Remove once the context-sharing question is settled.
#define PVR_TEXTURE_NAME_TEST 1

// The ALVR graphics code aborts the process when it sees a GL error. glGetError() is sticky, so
// an error left behind by app/Cardboard code would be blamed on the library call. Drain (and
// log) any pending errors before each library call.
static void drainGlErrors(const char *where) {
    GLenum e;
    int n = 0;
    while ((e = glGetError()) != GL_NO_ERROR && n++ < 16)
        LOG("Pending GL error 0x%x before %s", e, where);
}

// One-time GL driver info, to check feature support (external images) when debugging.
static void logGlInfo() {
    const char *ver = (const char *) glGetString(GL_VERSION);
    const char *ren = (const char *) glGetString(GL_RENDERER);
    const char *ext = (const char *) glGetString(GL_EXTENSIONS);
    LOG("GLINFO version=%s renderer=%s", ver ? ver : "?", ren ? ren : "?");
    LOG("GLINFO image_external_essl3=%d image_external=%d",
        ext && strstr(ext, "GL_OES_EGL_image_external_essl3") != nullptr,
        ext && strstr(ext, "GL_OES_EGL_image_external") != nullptr);
}

// The header says later DecoderConfig events are ignored until reconnection.
static bool g_decoderCreated = false;

// alvr_*_opengl() calls in this ALVR version can leave the thread with no current EGL context.
// Run them under a guard so the app's context/surface is rebound right after each call.
#define ALVR_GL_CALL(call)                \
    do {                                  \
        drainGlErrors(#call);             \
        EglStateGuard _eglGuard;          \
        call;                             \
    } while (0)


uint64_t HEAD_ID = alvr_path_string_to_id("/user/head");

// v21 removed the alvr_get_head_prediction_offset_ns() C export (it's now
// ClientCoreContext::get_total_prediction_offset(), a Rust-only method used internally by
// client_openxr). There is currently no C-ABI replacement, so this is a fixed heuristic in the
// meantime.
// TODO(upstream): propose re-exposing get_total_prediction_offset() via c_api.rs so native
// clients don't have to guess this.
const uint64_t ESTIMATED_PREDICTION_OFFSET_NS = 30e6;

// Note: the Cardboard SDK cannot estimate display time and an heuristic is used instead.
// Tested on the Oppo Reno 9 A: 50 ms made the streamed picture overshoot when a fast head turn
// stopped (felt over-sensitive); 30 ms removes that. Candidate for per-phone calibration.
const uint64_t VSYNC_QUEUE_INTERVAL_NS = 30e6;

// Client-side upscaling (ALVR's Snapdragon Game Super Resolution pass). The server streams a
// smaller picture, the decoder works on that, and the client scales it up to (about) the phone's
// real per-eye size. Only worth turning on when the stream is smaller than the display, e.g. to
// save Wi-Fi bandwidth or decoder load: set the server's video resolution lower in the dashboard.
// The upscale factor is worked out from the stream size, so there is nothing else to tune.
constexpr bool ENABLE_UPSCALING = true;
constexpr float UPSCALING_MAX_FACTOR = 3.0f;  // the same limit as ALVR's own slider
// Same defaults as ALVR's own settings.
constexpr bool UPSCALING_EDGE_DIRECTION = true;
constexpr float UPSCALING_EDGE_THRESHOLD = 4.0f;
constexpr float UPSCALING_EDGE_SHARPNESS = 2.0f;

// Zoom of the streamed picture. The server renders with the field of view we report; reporting a
// narrower one than the Cardboard lens really shows gives a bigger, closer-looking picture with
// more pixels per degree (so text is easier to read). 1.0 = exactly what the lens geometry says.
// The value scales the tangent of the half-angles, so 0.8 = about 25% zoom. The lobby is not
// affected. Too small a value makes straight lines bend slightly toward the edges.
// Tested on the Oppo Reno 9 A against the native client, which looked about 25% more zoomed in.
// Default; the app's Settings screen can change it (see setStreamFovScaleNative).
constexpr float STREAM_FOV_SCALE = 0.8f;
const float FLOOR_HEIGHT = 1.5;
const int MAXIMUM_TRACKING_FRAMES = 360;

// ---- ARCore tuning (values carried over from the v20 branch; tuned on one phone only) ----
// Use ARCore's position as the head position (otherwise position stays at the origin: 3DoF).
// Some phones (seen on an AQUOS sense4 basic, Adreno 618, Android 12) stop delivering decoded
// video frames while the ARCore camera session is running. With this set, ARCore is only
// started once the video decoder has produced a few frames, and it is paused again when the
// stream stops. Set to false to start ARCore together with the app, as before.
constexpr bool ARCORE_START_AFTER_VIDEO = true;
constexpr int ARCORE_START_AFTER_VIDEO_FRAMES = 10;

constexpr bool ARCORE_USE_POSITION = true;
// Slowly pull Cardboard's drifting orientation toward ARCore's (complementary filter).
constexpr bool ARCORE_USE_ORIENTATION_CORRECTION = true;
// How fast the correction follows ARCore. Larger = slower and smoother.
constexpr float ARCORE_CORRECTION_TIME_CONSTANT_S = 3.0f;
// A sample is ignored as a bad measurement when it disagrees with the orientation we are
// currently showing (Cardboard plus the correction) by more than this. How much drift Cardboard
// has built up in total does not matter, only the disagreement left after the correction.
constexpr float ARCORE_MAX_RESIDUAL_DEG = 15.0f;
// If samples keep disagreeing for this long, ARCore has probably moved its world frame (tracking
// was lost and regained, or it relocalized), so the new value is accepted...
constexpr float ARCORE_REACQUIRE_AFTER_S = 1.5f;
// ...and the correction follows it with this faster time constant until the two agree again.
constexpr float ARCORE_REACQUIRE_TIME_CONSTANT_S = 1.0f;
// Ignore the update after a gap longer than this (pause, lifecycle change, debugger).
constexpr uint64_t ARCORE_MAX_CORRECTION_GAP_NS = 500000000ULL;
// Fixed rotation between ARCore's display-oriented camera frame and Cardboard's frame
// (landscape-left). Found by calibration on one phone; other phones may need other values.
const AlvrQuat ARCORE_TO_CARDBOARD = {-0.0057f, -0.0031f, 0.7076f, 0.7066f};

// Which axis is "up" in the orientation this file works with (0 = x, 1 = y, 2 = z). The yaw
// correction is a rotation about this axis. The log prints a "tilt disagreement" that should stay
// small (a few degrees); if it is large, this axis is probably wrong.
constexpr int ARCORE_WORLD_UP_AXIS = 1;

// ---- Own head orientation (replaces Cardboard's gyro tracker) ----
// Mahony filter plus a stillness-based gyro bias estimator (head_orientation_filter.h), fed by
// head_sensor_source.h. Cardboard's tracker drifts in yaw because it never measures the gyro's
// constant offset; this does. The ARCore yaw correction below works on top of it unchanged.
// Set to false to go back to Cardboard's tracker. Falls back to it until the first sensor data
// arrives (or if the phone has no gyroscope).
constexpr bool USE_OWN_HEAD_FILTER = true;
// Once a second, while streaming: compare the own filter with Cardboard's tracker in the log.
constexpr bool HEAD_FILTER_DEBUG_LOG = true;

// ---- Neck model (seated 3DoF) ----
// Without ARCore there is no position, so the view is glued to one point. With this on, pitching
// or rolling the head moves the eyes a few centimetres, as it does on a real neck (see
// neck_model.h). Only used when ARCore is off; ARCore's real position replaces it. Turning the
// head left or right is deliberately ignored.
constexpr bool NECK_MODEL_ENABLED = true;
// Where the eyes are relative to the neck pivot, in metres (rough adult average; per-user
// setting later).
constexpr float NECK_EYE_UP_M = 0.09f;
constexpr float NECK_EYE_FORWARD_M = 0.08f;

// ---- ARCore head height ----
// How the vertical position is derived:
//   0 = relative to where ARCore started (height starts at 0, as in the earlier builds).
//   1 = height above the detected floor (the v20 behaviour). Until a floor is found it is the
//       same as 0, so the height jumps the moment the floor is first detected.
//   2 = like 1, but until a floor is found it assumes the headset started
//       ARCORE_ASSUMED_START_HEIGHT_M above the floor, and it eases toward the measured floor
//       height instead of jumping.
// Default; the app's Settings screen can change it (see setFloorModeNative).
constexpr int ARCORE_HEIGHT_MODE = 2;
// Mode 2 only: assumed height above the floor at the start, before any floor has been detected.
constexpr float ARCORE_ASSUMED_START_HEIGHT_M = 1.5f;
// Mode 2 only: how slowly the height eases toward the measured floor. Larger = slower.
constexpr float ARCORE_FLOOR_BLEND_TIME_CONSTANT_S = 2.0f;
// Added to the height in every mode. Use it to tune the height by hand (negative = lower).
constexpr float ARCORE_HEIGHT_OFFSET_M = 0.0f;
// A newly detected plane has to be this much lower than the current floor to replace it.
constexpr float ARCORE_FLOOR_REANCHOR_MARGIN_M = 0.03f;

struct NativeContext {
    JavaVM *javaVm = nullptr;
    jobject javaContext = nullptr;

    CardboardHeadTracker *headTracker = nullptr;
    phonevr::HeadOrientationFilter headFilter;
    phonevr::HeadSensorSource headSensors;
    float neckLastYawRad = 0.f;  // heading held while looking straight up/down; under poseMutex
    CardboardLensDistortion *lensDistortion = nullptr;
    CardboardDistortionRenderer *distortionRenderer = nullptr;

    // ARCore (positional tracking). ARCore calls that touch the session must not run
    // concurrently, so every use of arSession is guarded by arMutex. ArSession_update() must
    // additionally run on the render thread, with the app's EGL context current.
    bool arcoreEnabled = false;
    // Settings chosen on the app's Settings screen (the constants above are the defaults).
    float streamFovScale = STREAM_FOV_SCALE;
    int heightMode = ARCORE_HEIGHT_MODE;
    bool arSessionRunning = false;  // ArSession_resume() done and not yet paused; under arMutex
    int videoFramesSeen = 0;        // decoded frames since the stream started (render thread)
    ArSession *arSession = nullptr;
    ArFrame *arFrame = nullptr;
    // Anchor on the lowest horizontal plane found so far (taken to be the floor). An anchor keeps
    // following the floor when ARCore adjusts its world frame. Render thread only (arMutex).
    ArAnchor *arFloorAnchor = nullptr;
    // Mode 2: how far above the floor ARCore's origin is, eased toward the measured value.
    float floorOffsetM = ARCORE_ASSUMED_START_HEIGHT_M;
    uint64_t lastFloorBlendNs = 0;
    GLuint arTexture = 0;
    std::mutex arMutex;

    // Pose state written by the render thread (updateARCoreTracking) and read by any thread
    // that calls getPose(), for example the input thread. Guarded by poseMutex.
    std::mutex poseMutex;
    bool arPositionValid = false;
    float arPosition[3] = {0.f, 0.f, 0.f};
    // Yaw (rotation about the world's up axis) added to Cardboard's orientation, in radians.
    float yawCorrectionRad = 0.f;
    uint64_t lastCorrectionNs = 0;
    uint64_t disagreeSinceNs = 0;  // when ARCore started disagreeing with the correction, or 0

    int screenWidth = 0;
    int screenHeight = 0;

    bool renderingParamsChanged = true;
    bool glContextRecreated = false;

    bool running = false;
    bool streaming = false;
    std::thread inputThread;

    // Use one texture per eye, no need for swapchains.
    GLuint lobbyTextures[2] = {0, 0};
    GLuint streamTextures[2] = {0, 0};

    float eyeOffsets[2] = {0.0, 0.0};
    AlvrFov fovArr[2] = {};
    AlvrDeviceMotion deviceMotion = {};

    // Cache of the most recently matched frame metadata, since alvr_report_compositor_start()
    // can legitimately return false (no match for this exact frame), in which case the caller
    // is expected to reuse the previous metadata.
    bool hasFrameMetadata = false;
    AlvrVideoFrameMetadata lastFrameMetadata = {};

    // The last decoded video frame, kept so it can be shown again (re-oriented to the current
    // head pose) on display refreshes where no new video frame has arrived. The buffer belongs
    // to the decoder, so this is cleared whenever the decoder is created or destroyed or a
    // stream starts or stops.
    void *lastStreamBuffer = nullptr;
    uint64_t lastStreamTimestampNs = 0;

    NativeContext() {
        memset(&fovArr, 0, sizeof(fovArr));
        memset(&deviceMotion, 0, sizeof(deviceMotion));
    }
};

NativeContext CTX = {};

int64_t GetBootTimeNano() {
    struct timespec res = {};
    clock_gettime(CLOCK_BOOTTIME, &res);
    return (res.tv_sec * 1e9) + res.tv_nsec;
}

// Inverse unit quaternion
AlvrQuat inverseQuat(AlvrQuat q) { return {-q.x, -q.y, -q.z, q.w}; }

void cross(float a[3], float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

void quatVecMultiply(AlvrQuat q, float v[3], float out[3]) {
    float rv[3], rrv[3];
    float r[3] = {q.x, q.y, q.z};
    cross(r, v, rv);
    cross(r, rv, rrv);
    for (int i = 0; i < 3; i++) {
        out[i] = v[i] + 2 * (q.w * rv[i] + rrv[i]);
    }
}

void offsetPosWithQuat(AlvrQuat q, float offset[3], float outPos[3]) {
    float rotatedOffset[3];
    quatVecMultiply(q, offset, rotatedOffset);

    outPos[0] -= rotatedOffset[0];
    outPos[1] -= rotatedOffset[1] - FLOOR_HEIGHT;
    outPos[2] -= rotatedOffset[2];
}

AlvrFov getFov(CardboardEye eye) {
    float f[4];
    CardboardLensDistortion_getFieldOfView(CTX.lensDistortion, eye, f);

    AlvrFov fov = {};
    fov.left = -f[0];
    fov.right = f[1];
    fov.up = f[3];
    fov.down = -f[2];

    return fov;
}

// The field of view reported to the server, i.e. the one the stream is rendered with. The lobby
// keeps using getFov() directly. See STREAM_FOV_SCALE.
AlvrFov getStreamFov(CardboardEye eye) {
    AlvrFov fov = getFov(eye);
    fov.left = atanf(tanf(fov.left) * CTX.streamFovScale);
    fov.right = atanf(tanf(fov.right) * CTX.streamFovScale);
    fov.up = atanf(tanf(fov.up) * CTX.streamFovScale);
    fov.down = atanf(tanf(fov.down) * CTX.streamFovScale);
    return fov;
}

// Quaternion helpers for the orientation correction (same maths as the v20 branch).
static AlvrQuat quatMultiply(const AlvrQuat &a, const AlvrQuat &b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// Inverse of a unit quaternion.
static AlvrQuat quatInverse(const AlvrQuat &q) { return {-q.x, -q.y, -q.z, q.w}; }

static AlvrQuat normalizeQuat(AlvrQuat q) {
    float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len < 1e-6f) {
        return {0.f, 0.f, 0.f, 1.f};
    }
    return {q.x / len, q.y / len, q.z / len, q.w / len};
}

// q and -q are the same rotation, so use abs(w).
static float quatAngleDegrees(const AlvrQuat &q) {
    float w = std::clamp(std::abs(q.w), 0.0f, 1.0f);
    return 2.0f * std::acos(w) * 180.0f / (float) M_PI;
}

// Wraps an angle to the range [-pi, pi].
static float wrapRad(float angle) {
    const float twoPi = 2.0f * (float) M_PI;
    while (angle > (float) M_PI) {
        angle -= twoPi;
    }
    while (angle < -(float) M_PI) {
        angle += twoPi;
    }
    return angle;
}

// A rotation about the world's up axis.
static AlvrQuat worldYawQuat(float angleRad) {
    float s = std::sin(angleRad * 0.5f);
    float c = std::cos(angleRad * 0.5f);
    float axis[3] = {0.f, 0.f, 0.f};
    axis[ARCORE_WORLD_UP_AXIS] = s;
    return {axis[0], axis[1], axis[2], c};
}

// Cardboard's own tracker, in the same convention the rest of this file uses.
static AlvrQuat cardboardTrackerOrientationAt(uint64_t timestampNs) {
    float pos[3];
    float q[4];
    CardboardHeadTracker_getPose(CTX.headTracker, (int64_t) timestampNs, kLandscapeLeft, pos, q);
    return normalizeQuat(inverseQuat(AlvrQuat{q[0], q[1], q[2], q[3]}));
}

// The gyro-based orientation at the given time (before any ARCore yaw correction): our own
// filter (USE_OWN_HEAD_FILTER), or Cardboard's tracker until the filter has data.
static AlvrQuat cardboardOrientationAt(uint64_t timestampNs) {
    if (USE_OWN_HEAD_FILTER && CTX.headFilter.ready()) {
        phonevr::Quatd o = CTX.headFilter.orientationAt((int64_t) timestampNs);
        return normalizeQuat(AlvrQuat{(float) o.x, (float) o.y, (float) o.z, (float) o.w});
    }
    return cardboardTrackerOrientationAt(timestampNs);
}

// Yaw (radians, about +y) of the direction the head faces (-z), for the log check below.
static float facingYawRad(const AlvrQuat &q) {
    float fx = -2.f * (q.x * q.z + q.w * q.y);
    float fz = -(1.f - 2.f * (q.x * q.x + q.y * q.y));
    return std::atan2(-fx, -fz);
}

// Angle (degrees) between the two orientations' "up" directions, as seen from the head. This is
// pitch/roll only, so it ignores yaw. Close to 0 when the own filter's axes match Cardboard's.
static float upVectorAngleDeg(const AlvrQuat &a, const AlvrQuat &b) {
    auto up = [](const AlvrQuat &q, float v[3]) {
        v[0] = 2.f * (q.x * q.y + q.w * q.z);
        v[1] = 1.f - 2.f * (q.x * q.x + q.z * q.z);
        v[2] = 2.f * (q.y * q.z - q.w * q.x);
    };
    float va[3], vb[3];
    up(a, va);
    up(b, vb);
    float dot = std::clamp(va[0] * vb[0] + va[1] * vb[1] + va[2] * vb[2], -1.f, 1.f);
    return std::acos(dot) * 180.f / (float) M_PI;
}

// Head pose, in world/tracking space (not head-relative). Safe to call from any thread.
//
// Orientation: the fast gyro orientation (own filter, or Cardboard's), turned about the vertical by a slowly changing
// yaw correction that the render thread computes from ARCore (updateARCoreTracking). Gyro drift
// is a turn about the vertical, so this is what removes it.
// Position: ARCore's last known position, or the origin until ARCore has tracked (3DoF).
AlvrPose getPose(uint64_t timestampNs) {
    AlvrPose pose = {};

    AlvrQuat orientation = cardboardOrientationAt(timestampNs);

    {
        std::lock_guard<std::mutex> lock(CTX.poseMutex);

        if (ARCORE_USE_ORIENTATION_CORRECTION) {
            orientation =
                normalizeQuat(quatMultiply(worldYawQuat(CTX.yawCorrectionRad), orientation));
        }

        if (ARCORE_USE_POSITION && CTX.arPositionValid) {
            for (int i = 0; i < 3; i++) {
                pose.position[i] = CTX.arPosition[i];
            }
        } else if (NECK_MODEL_ENABLED && !CTX.arcoreEnabled) {
            float offset[3];
            phonevr::neckModelOffset(orientation.x, orientation.y, orientation.z, orientation.w,
                                     NECK_EYE_UP_M, NECK_EYE_FORWARD_M, CTX.neckLastYawRad,
                                     offset);
            for (int i = 0; i < 3; i++) {
                pose.position[i] = offset[i];
            }
        }
    }

    pose.orientation = orientation;
    return pose;
}

// Height (y) of an anchor in ARCore's world frame.
static float anchorHeight(const ArAnchor *anchor) {
    ArPose *pose = nullptr;
    ArPose_create(CTX.arSession, nullptr, &pose);
    ArAnchor_getPose(CTX.arSession, anchor, pose);
    float raw[7] = {};
    ArPose_getPoseRaw(CTX.arSession, pose, raw);
    ArPose_destroy(pose);
    return raw[5];
}

// Looks for the floor: the lowest horizontal, upward-facing plane ARCore has found so far. When
// one is known, returns true and sets *outFloorY to the floor's height in ARCore's world frame
// (read from an anchor, so it follows any adjustment ARCore makes to that frame).
// Render thread only, with arMutex held and a successful ArSession_update behind it.
static bool updateFloorAnchor(float *outFloorY) {
    ArTrackableList *planes = nullptr;
    ArTrackableList_create(CTX.arSession, &planes);
    ArFrame_getUpdatedTrackables(CTX.arSession, CTX.arFrame, AR_TRACKABLE_PLANE, planes);
    int32_t planeCount = 0;
    ArTrackableList_getSize(CTX.arSession, planes, &planeCount);

    // Is the current floor anchor still usable?
    float floorY = 0.0f;
    bool haveFloor = false;
    if (CTX.arFloorAnchor != nullptr) {
        ArTrackingState anchorState;
        ArAnchor_getTrackingState(CTX.arSession, CTX.arFloorAnchor, &anchorState);
        if (anchorState == AR_TRACKING_STATE_STOPPED) {
            // ARCore has given up on this anchor, so forget it.
            ArAnchor_release(CTX.arFloorAnchor);
            CTX.arFloorAnchor = nullptr;
        } else if (anchorState == AR_TRACKING_STATE_TRACKING) {
            floorY = anchorHeight(CTX.arFloorAnchor);
            haveFloor = true;
        }
    }

    for (int32_t i = 0; i < planeCount; i++) {
        ArTrackable *trackable = nullptr;
        ArTrackableList_acquireItem(CTX.arSession, planes, i, &trackable);
        if (trackable == nullptr) {
            continue;
        }

        ArTrackingState planeState;
        ArTrackable_getTrackingState(CTX.arSession, trackable, &planeState);
        ArPlaneType planeType;
        ArPlane_getType(CTX.arSession, ArAsPlane(trackable), &planeType);

        if (planeState == AR_TRACKING_STATE_TRACKING &&
            planeType == AR_PLANE_HORIZONTAL_UPWARD_FACING) {
            ArPose *planePose = nullptr;
            ArPose_create(CTX.arSession, nullptr, &planePose);
            ArPlane_getCenterPose(CTX.arSession, ArAsPlane(trackable), planePose);
            float planeRaw[7] = {};
            ArPose_getPoseRaw(CTX.arSession, planePose, planeRaw);
            float planeY = planeRaw[5];

            // The floor is the lowest such plane (tables and shelves are higher).
            if (!haveFloor || planeY < floorY - ARCORE_FLOOR_REANCHOR_MARGIN_M) {
                // Anchor at the plane's position only, without its rotation. (Poses are created
                // from {qx, qy, qz, qw, tx, ty, tz}.)
                const float translationOnlyRaw[7] = {
                    0.f, 0.f, 0.f, 1.f, planeRaw[4], planeRaw[5], planeRaw[6]};
                ArPose *translationOnly = nullptr;
                ArPose_create(CTX.arSession, translationOnlyRaw, &translationOnly);
                ArAnchor *newAnchor = nullptr;
                if (ArTrackable_acquireNewAnchor(
                        CTX.arSession, trackable, translationOnly, &newAnchor) == AR_SUCCESS) {
                    if (CTX.arFloorAnchor != nullptr) {
                        ArAnchor_detach(CTX.arSession, CTX.arFloorAnchor);
                        ArAnchor_release(CTX.arFloorAnchor);
                    }
                    CTX.arFloorAnchor = newAnchor;
                    info("ARCore: floor %s at y=%.3f",
                         haveFloor ? "lowered" : "found",
                         planeY);
                    floorY = planeY;
                    haveFloor = true;
                }
                ArPose_destroy(translationOnly);
            }
            ArPose_destroy(planePose);
        }
        ArTrackable_release(trackable);
    }
    ArTrackableList_destroy(planes);

    if (haveFloor) {
        *outFloorY = floorY;
    }
    return haveFloor;
}

// Updates ARCore once per render frame and publishes what getPose() needs: the ARCore position
// and the orientation correction. Must be called from the render thread, with the app's EGL
// context current (ArSession_update requires it).
// Starts the ARCore camera session (no-op if it is already running or ARCore is off).
static void startARCoreSession(const char *why) {
    std::lock_guard<std::mutex> lock(CTX.arMutex);
    if (!CTX.arcoreEnabled || CTX.arSession == nullptr || CTX.arSessionRunning) {
        return;
    }
    ArStatus arStatus = ArSession_resume(CTX.arSession);
    if (arStatus != AR_SUCCESS) {
        info("ARCore ERROR: failed to resume session (status %d)", (int) arStatus);
        return;
    }
    CTX.arSessionRunning = true;
    info("ARCore: camera session started (%s)", why);
}

// Pauses the ARCore session, which releases the camera (no-op if it is not running).
static void stopARCoreSession(const char *why) {
    {
        std::lock_guard<std::mutex> lock(CTX.arMutex);
        if (!CTX.arcoreEnabled || CTX.arSession == nullptr || !CTX.arSessionRunning) {
            return;
        }
        ArSession_pause(CTX.arSession);
        CTX.arSessionRunning = false;
    }
    {
        std::lock_guard<std::mutex> lock(CTX.poseMutex);
        CTX.arPositionValid = false;
    }
    info("ARCore: camera session paused (%s)", why);
}

static void updateARCoreTracking() {
    if (!CTX.arcoreEnabled || CTX.arTexture == 0) {
        return;
    }
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
        info("ARCore ERROR: updateARCoreTracking called without an EGL context");
        return;
    }

    std::lock_guard<std::mutex> lock(CTX.arMutex);
    if (CTX.arSession == nullptr || CTX.arFrame == nullptr || !CTX.arSessionRunning) {
        return;
    }

    static uint64_t lastLogNs = 0;
    uint64_t nowNs = GetBootTimeNano();
    bool shouldLog = nowNs - lastLogNs >= 1000000000ULL;

    ArStatus status = ArSession_update(CTX.arSession, CTX.arFrame);
    if (status != AR_SUCCESS) {
        if (shouldLog) {
            lastLogNs = nowNs;
            info("ARCore ERROR: ArSession_update failed (status %d)", (int) status);
        }
        return;
    }

    ArCamera *arCamera = nullptr;
    ArFrame_acquireCamera(CTX.arSession, CTX.arFrame, &arCamera);

    ArTrackingState trackingState;
    ArCamera_getTrackingState(CTX.arSession, arCamera, &trackingState);

    if (trackingState != AR_TRACKING_STATE_TRACKING) {
        // Not tracking: keep the last known position and correction.
        if (shouldLog) {
            lastLogNs = nowNs;
            info("ARCore: not tracking (state %d)", (int) trackingState);
        }
        ArCamera_release(arCamera);
        return;
    }

    ArPose *arPose = nullptr;
    ArPose_create(CTX.arSession, nullptr, &arPose);
    ArCamera_getDisplayOrientedPose(CTX.arSession, arCamera, arPose);
    // {qx, qy, qz, qw, tx, ty, tz}
    float raw[7] = {};
    ArPose_getPoseRaw(CTX.arSession, arPose, raw);
    ArPose_destroy(arPose);
    ArCamera_release(arCamera);

    // Vertical position (see ARCORE_HEIGHT_MODE; the mode in use is CTX.heightMode).
    float headY = raw[5];
    float floorY = 0.0f;
    bool floorKnown = false;
    if (CTX.heightMode != 0) {
        floorKnown = updateFloorAnchor(&floorY);
    }
    if (CTX.heightMode == 1) {
        if (floorKnown) {
            headY = raw[5] - floorY;
        }
    } else if (CTX.heightMode == 2) {
        if (floorKnown) {
            uint64_t blendDtNs = CTX.lastFloorBlendNs != 0 ? nowNs - CTX.lastFloorBlendNs : 0;
            if (blendDtNs > 0 && blendDtNs < ARCORE_MAX_CORRECTION_GAP_NS) {
                float gain = 1.0f - std::exp(-((float) blendDtNs * 1e-9f) /
                                              ARCORE_FLOOR_BLEND_TIME_CONSTANT_S);
                // ARCore's origin is -floorY above the floor.
                CTX.floorOffsetM += (-floorY - CTX.floorOffsetM) * gain;
            }
            CTX.lastFloorBlendNs = nowNs;
        } else {
            CTX.lastFloorBlendNs = 0;
        }
        headY = raw[5] + CTX.floorOffsetM;
    }
    headY += ARCORE_HEIGHT_OFFSET_M;

    // How far apart are Cardboard and ARCore right now?
    AlvrQuat cardboardOrientation =
        cardboardOrientationAt(nowNs + ESTIMATED_PREDICTION_OFFSET_NS);
    AlvrQuat arcoreOrientation = normalizeQuat(AlvrQuat{raw[0], raw[1], raw[2], raw[3]});
    AlvrQuat arcoreInCardboard =
        normalizeQuat(quatMultiply(arcoreOrientation, quatInverse(ARCORE_TO_CARDBOARD)));

    // The rotation, in world space, that takes Cardboard's orientation onto ARCore's. Both sensors
    // know which way is down, so this is (nearly) a pure rotation about the vertical: the yaw
    // drift. Splitting off that yaw (and treating what is left as "tilt") makes the correction
    // independent of where the head is pointing. A correction expressed in the phone's own frame
    // is only right while the head keeps the pose it was measured in.
    AlvrQuat worldOffset =
        normalizeQuat(quatMultiply(arcoreInCardboard, quatInverse(cardboardOrientation)));
    if (worldOffset.w < 0.0f) {
        worldOffset = {-worldOffset.x, -worldOffset.y, -worldOffset.z, -worldOffset.w};
    }
    const float offsetComponents[3] = {worldOffset.x, worldOffset.y, worldOffset.z};
    float measuredYawRad =
        2.0f * std::atan2(offsetComponents[ARCORE_WORLD_UP_AXIS], worldOffset.w);
    AlvrQuat tilt = normalizeQuat(
        quatMultiply(quatInverse(worldYawQuat(measuredYawRad)), worldOffset));
    float tiltDeg = quatAngleDegrees(tilt);
    float yawOffsetDeg = measuredYawRad * 180.0f / (float) M_PI;

    static int acceptedCount = 0;
    static int rejectedCount = 0;
    static float maxResidualDeg = 0.0f;
    static float maxTiltDeg = 0.0f;
    static bool wasReacquiring = false;

    float residualDeg = 0.0f;
    bool accepted = false;
    bool reacquiring = false;
    float correctionDeg = 0.0f;

    {
        std::lock_guard<std::mutex> poseLock(CTX.poseMutex);

        CTX.arPosition[0] = raw[4];
        CTX.arPosition[1] = headY;
        CTX.arPosition[2] = raw[6];
        CTX.arPositionValid = true;

        // How much ARCore disagrees with the yaw being applied right now.
        float residualRad = wrapRad(measuredYawRad - CTX.yawCorrectionRad);
        residualDeg = std::abs(residualRad) * 180.0f / (float) M_PI;

        bool agrees = residualDeg < ARCORE_MAX_RESIDUAL_DEG;
        if (agrees) {
            CTX.disagreeSinceNs = 0;
        } else if (CTX.disagreeSinceNs == 0) {
            CTX.disagreeSinceNs = nowNs;
        }
        uint64_t reacquireAfterNs = (uint64_t) (ARCORE_REACQUIRE_AFTER_S * 1e9f);
        reacquiring = !agrees && (nowNs - CTX.disagreeSinceNs) >= reacquireAfterNs;
        accepted = agrees || reacquiring;

        if (accepted) {
            if (CTX.lastCorrectionNs == 0) {
                CTX.lastCorrectionNs = nowNs;
            }
            uint64_t deltaNs = nowNs - CTX.lastCorrectionNs;
            // Protect against an unexpectedly large gap (pause, debugger, lifecycle change).
            if (deltaNs > 0 && deltaNs < ARCORE_MAX_CORRECTION_GAP_NS) {
                float dt = (float) deltaNs * 1e-9f;
                float timeConstant =
                    agrees ? ARCORE_CORRECTION_TIME_CONSTANT_S : ARCORE_REACQUIRE_TIME_CONSTANT_S;
                // Time-based gain, so the result doesn't depend on the frame rate.
                float gain = 1.0f - std::exp(-dt / timeConstant);
                CTX.yawCorrectionRad = wrapRad(CTX.yawCorrectionRad + residualRad * gain);
            }
            CTX.lastCorrectionNs = nowNs;
        } else {
            // Don't let a transient mismatch pull the correction toward a bad measurement.
            CTX.lastCorrectionNs = nowNs;
        }

        correctionDeg = CTX.yawCorrectionRad * 180.0f / (float) M_PI;
    }

    if (accepted) {
        acceptedCount++;
    } else {
        rejectedCount++;
    }
    maxResidualDeg = std::max(maxResidualDeg, residualDeg);
    maxTiltDeg = std::max(maxTiltDeg, tiltDeg);
    if (reacquiring && !wasReacquiring) {
        info("ARCore: yaw disagreed for %.1f s (residual %.1f deg, offset %.1f deg); following "
             "ARCore's new yaw",
             ARCORE_REACQUIRE_AFTER_S, residualDeg, yawOffsetDeg);
    }
    wasReacquiring = reacquiring;

    if (shouldLog) {
        lastLogNs = nowNs;
        info("ARCore: tracking, position=(%.3f, %.3f, %.3f), floor %s (y=%.2f) | yaw offset from "
             "ARCore=%.2f deg, residual=%.2f deg (max %.2f), yaw correction=%.2f deg, tilt "
             "disagreement=%.2f deg (max %.2f), last second: %d accepted, %d rejected%s",
             raw[4], headY, raw[6], floorKnown ? "known" : "unknown", floorY, yawOffsetDeg,
             residualDeg, maxResidualDeg, correctionDeg, tiltDeg, maxTiltDeg, acceptedCount,
             rejectedCount, reacquiring ? ", RE-ACQUIRING" : "");
        acceptedCount = 0;
        rejectedCount = 0;
        maxResidualDeg = 0.0f;
        maxTiltDeg = 0.0f;
    }
}

// Local-space (head-at-origin) per-eye view parameters: just the eye offset and FOV. These rarely
// change (only when the lens distortion / viewer profile is recalculated), so this is sent once
// via alvr_send_view_params() rather than on every tracking update - unlike the v20 API, the
// moving head pose is no longer bundled into this call.
void sendViewParams() {
    AlvrViewParams viewParams[2] = {};

    for (int eye = 0; eye < 2; eye++) {
        viewParams[eye].pose.orientation = AlvrQuat{0.0, 0.0, 0.0, 1.0};
        viewParams[eye].pose.position[0] = CTX.eyeOffsets[eye];
        viewParams[eye].pose.position[1] = 0.0;
        viewParams[eye].pose.position[2] = 0.0;
        viewParams[eye].fov = CTX.fovArr[eye];
    }

    alvr_send_view_params(viewParams);
}

// Once a second: how the own head filter compares with Cardboard's tracker. "tilt diff" should
// stay under a degree or two (a big value means the sensor axes are mapped wrongly). "yaw diff"
// drifts slowly (that is the drift being removed); turn your head left and right: if it jumps
// around instead of staying put, the yaw direction is mirrored.
static void logHeadFilterCheck(uint64_t nowNs) {
    static uint64_t lastNs = 0;
    if (!USE_OWN_HEAD_FILTER || !HEAD_FILTER_DEBUG_LOG || !CTX.headFilter.ready()) {
        return;
    }
    if (nowNs - lastNs < 1000000000ULL) {
        return;
    }
    lastNs = nowNs;
    AlvrQuat own = cardboardOrientationAt(nowNs);
    AlvrQuat cardboard = cardboardTrackerOrientationAt(nowNs);
    float yawDiffDeg = wrapRad(facingYawRad(own) - facingYawRad(cardboard)) * 180.f / (float) M_PI;
    phonevr::HeadOrientationFilter::Diagnostics d = CTX.headFilter.diagnostics();
    info("HeadFilter: tilt diff vs Cardboard %.2f deg, yaw diff %.1f deg | gyro bias (deg/s) "
         "%.3f %.3f %.3f%s",
         upVectorAngleDeg(own, cardboard), yawDiffDeg, d.biasDps[0], d.biasDps[1], d.biasDps[2],
         d.biasReady ? "" : " (not measured yet: hold still)");
}

void inputThread() {
    auto deadline = std::chrono::steady_clock::now();

    info("inputThread: thread staring...");
    while (CTX.streaming) {
        auto targetTimestampNs = GetBootTimeNano() + ESTIMATED_PREDICTION_OFFSET_NS;
        logHeadFilterCheck(targetTimestampNs);

        CTX.deviceMotion.device_id = HEAD_ID;
        CTX.deviceMotion.pose = getPose(targetTimestampNs);

        alvr_send_tracking(targetTimestampNs, &CTX.deviceMotion, 1, nullptr, nullptr);

        deadline += std::chrono::nanoseconds((uint64_t) (1e9 / 60.f / 3));
        std::this_thread::sleep_until(deadline);
    }
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *) {
    CTX.javaVm = vm;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL Java_viritualisres_phonevr_ALVRActivity_initializeNative(
    JNIEnv *env, jobject obj, jint screenWidth, jint screenHeight, jfloat refreshRate,jboolean enableARCore) {
    CTX.javaContext = env->NewGlobalRef(obj);
    info("initializeNative: enableARCore=%d", (int) enableARCore);

    uint32_t viewWidth = (std::max(screenWidth, screenHeight) / 2) & ~15u;  // multiple of 16
    uint32_t viewHeight = std::min(screenWidth, screenHeight) & ~7u;        // multiple of 8

    // Route the Rust library's log output (including the reason for its aborts) to logcat.
    alvr_initialize_logging();

    alvr_initialize_android_context((void *) CTX.javaVm, (void *) CTX.javaContext);

    static float refreshRatesBuffer[1];
    refreshRatesBuffer[0] = refreshRate;

    AlvrClientCapabilities caps = {};
    caps.default_view_width = viewWidth;
    caps.default_view_height = viewHeight;
    caps.max_view_width = viewWidth;
    caps.max_view_height = viewHeight;
    caps.refresh_rates = refreshRatesBuffer;
    caps.refresh_rates_count = 1;
    caps.foveated_encoding = false;
    caps.encoder_high_profile = true;
    caps.encoder_10_bits = false;
    caps.encoder_av1 = false;
    caps.prefer_10bit = false;
    caps.preferred_encoding_gamma = 1.0;
    caps.prefer_hdr = false;

    alvr_initialize(caps);

    Cardboard_initializeAndroid(CTX.javaVm, CTX.javaContext);
    CTX.headTracker = CardboardHeadTracker_create();

    CTX.arcoreEnabled = (bool) enableARCore;
    if (CTX.arcoreEnabled) {
        if (ArSession_create(env, CTX.javaContext, &CTX.arSession) != AR_SUCCESS) {
            info("ARCore ERROR: could not create session (is Google Play Services for AR "
                 "installed?); continuing without ARCore");
            CTX.arSession = nullptr;
            CTX.arcoreEnabled = false;
        }
    }

    if (CTX.arcoreEnabled) {
        ArConfig *arConfig = nullptr;
        ArConfig_create(CTX.arSession, &arConfig);

        // Explicitly disable everything we don't need, to preserve CPU and battery.
        ArConfig_setDepthMode(CTX.arSession, arConfig, AR_DEPTH_MODE_DISABLED);
        ArConfig_setLightEstimationMode(
            CTX.arSession, arConfig, AR_LIGHT_ESTIMATION_MODE_DISABLED);
        ArConfig_setPlaneFindingMode(
            CTX.arSession, arConfig, AR_PLANE_FINDING_MODE_HORIZONTAL_AND_VERTICAL);
        ArConfig_setCloudAnchorMode(CTX.arSession, arConfig, AR_CLOUD_ANCHOR_MODE_DISABLED);
        // Don't block waiting for a new camera image.
        ArConfig_setUpdateMode(CTX.arSession, arConfig, AR_UPDATE_MODE_LATEST_CAMERA_IMAGE);

        ArStatus configureStatus = ArSession_configure(CTX.arSession, arConfig);
        ArConfig_destroy(arConfig);

        if (configureStatus != AR_SUCCESS) {
            info("ARCore ERROR: could not configure session (status %d); continuing without "
                 "ARCore",
                 (int) configureStatus);
            ArSession_destroy(CTX.arSession);
            CTX.arSession = nullptr;
            CTX.arcoreEnabled = false;
        } else {
            ArFrame_create(CTX.arSession, &CTX.arFrame);
            info("ARCore session created and configured");
        }
    }
}

// Settings from the app's Settings screen. Called right after initializeNative(), before the
// stream starts.
extern "C" JNIEXPORT void JNICALL
Java_viritualisres_phonevr_ALVRActivity_setStreamFovScaleNative(JNIEnv *, jobject, jfloat scale) {
    // Keep it in a sane range whatever was stored.
    CTX.streamFovScale = std::min(1.0f, std::max(0.5f, (float) scale));
    info("Setting: stream FOV scale %.2f", CTX.streamFovScale);
}

extern "C" JNIEXPORT void JNICALL
Java_viritualisres_phonevr_ALVRActivity_setFloorModeNative(JNIEnv *, jobject, jint mode) {
    CTX.heightMode = (mode >= 0 && mode <= 2) ? (int) mode : ARCORE_HEIGHT_MODE;
    info("Setting: ARCore floor/height mode %d", CTX.heightMode);
}

extern "C" JNIEXPORT void JNICALL Java_viritualisres_phonevr_ALVRActivity_destroyNative(JNIEnv *,
                                                                                        jobject) {
    alvr_destroy_opengl();
    alvr_destroy();

    CTX.headSensors.stop();
    CardboardHeadTracker_destroy(CTX.headTracker);
    CTX.headTracker = nullptr;
    CardboardLensDistortion_destroy(CTX.lensDistortion);
    CTX.lensDistortion = nullptr;
    CardboardDistortionRenderer_destroy(CTX.distortionRenderer);
    CTX.distortionRenderer = nullptr;

    {
        std::lock_guard<std::mutex> lock(CTX.arMutex);
        if (CTX.arFloorAnchor != nullptr) {
            ArAnchor_release(CTX.arFloorAnchor);
            CTX.arFloorAnchor = nullptr;
        }
        if (CTX.arFrame != nullptr) {
            ArFrame_destroy(CTX.arFrame);
            CTX.arFrame = nullptr;
        }
        if (CTX.arSession != nullptr) {
            ArSession_destroy(CTX.arSession);
            CTX.arSession = nullptr;
        }
        CTX.arcoreEnabled = false;
    }
}

extern "C" JNIEXPORT void JNICALL Java_viritualisres_phonevr_ALVRActivity_resumeNative(JNIEnv *,
                                                                                       jobject) {
    CardboardHeadTracker_resume(CTX.headTracker);
    if (USE_OWN_HEAD_FILTER) {
        CTX.headSensors.start(&CTX.headFilter);
    }

    if (!ARCORE_START_AFTER_VIDEO) {
        startARCoreSession("app resumed");
    }

    CTX.renderingParamsChanged = true;

    uint8_t *buffer;
    int size;
    CardboardQrCode_getSavedDeviceParams(&buffer, &size);
    if (size == 0) {
        CardboardQrCode_scanQrCodeAndSaveDeviceParams();
    }
    CardboardQrCode_destroy(buffer);

    CTX.running = true;
    alvr_resume();
    
    // at the start of renderNative, right after alvr_resume(...), and right before the final draw:
    LOG("EGLCHK ctx=%p surf=%p", eglGetCurrentContext(), eglGetCurrentSurface(EGL_DRAW));
    EGLCHK("after alvr_resume()");
}

extern "C" JNIEXPORT void JNICALL Java_viritualisres_phonevr_ALVRActivity_pauseNative(JNIEnv *,
                                                                                      jobject) {
    alvr_pause();

    if (CTX.running) {
        CTX.running = false;
    }

    CardboardHeadTracker_pause(CTX.headTracker);
    CTX.headSensors.stop();

    // Releases the camera. (The v20 code never paused the ARCore session.)
    stopARCoreSession("app paused");
}

extern "C" JNIEXPORT void JNICALL
Java_viritualisres_phonevr_ALVRActivity_surfaceCreatedNative(JNIEnv *, jobject) {
    logGlInfo();
    ALVR_GL_CALL(alvr_initialize_opengl());

    CTX.glContextRecreated = true;
}

extern "C" JNIEXPORT void JNICALL Java_viritualisres_phonevr_ALVRActivity_setScreenResolutionNative(
    JNIEnv *, jobject, jint width, jint height) {
    CTX.screenWidth = width;
    CTX.screenHeight = height;

    CTX.renderingParamsChanged = true;
}

extern "C" JNIEXPORT void JNICALL Java_viritualisres_phonevr_ALVRActivity_sendBatteryLevel(
    JNIEnv *, jobject, jfloat level, jboolean plugged) {
    alvr_send_battery(HEAD_ID, level, plugged);
}

extern "C" JNIEXPORT void JNICALL Java_viritualisres_phonevr_ALVRActivity_renderNative(JNIEnv *,
                                                                                       jobject) {
    // at the start of renderNative, right after alvr_resume(...), and right before the final draw:
    LOG("EGLCHK ctx=%p surf=%p", eglGetCurrentContext(), eglGetCurrentSurface(EGL_DRAW));
    EGLCHK("Start of renderNative()");

    if (CTX.renderingParamsChanged) {
        info("renderingParamsChanged, processing new params");
        uint8_t *buffer;
        int size;
        CardboardQrCode_getSavedDeviceParams(&buffer, &size);

        if (size == 0) {
            return;
        }

        info("renderingParamsChanged, sending new params to alvr");
        if (CTX.lensDistortion) {
            CardboardLensDistortion_destroy(CTX.lensDistortion);
            CTX.lensDistortion = nullptr;
        }
        info("renderingParamsChanged, destroyed distortion");
        CTX.lensDistortion =
            CardboardLensDistortion_create(buffer, size, CTX.screenWidth, CTX.screenHeight);

        CardboardQrCode_destroy(buffer);
        *buffer = 0;

        if (CTX.distortionRenderer) {
            CardboardDistortionRenderer_destroy(CTX.distortionRenderer);
            CTX.distortionRenderer = nullptr;
        }
        const CardboardOpenGlEsDistortionRendererConfig config{kGlTexture2D};
        CTX.distortionRenderer = CardboardOpenGlEs2DistortionRenderer_create(&config);

        for (int eye = 0; eye < 2; eye++) {
            CardboardMesh mesh;
            CardboardLensDistortion_getDistortionMesh(
                CTX.lensDistortion, (CardboardEye) eye, &mesh);
            CardboardDistortionRenderer_setMesh(
                CTX.distortionRenderer, &mesh, (CardboardEye) eye);

            float matrix[16] = {};
            CardboardLensDistortion_getEyeFromHeadMatrix(
                CTX.lensDistortion, (CardboardEye) eye, matrix);
            CTX.eyeOffsets[eye] = matrix[12];
        }

        CTX.fovArr[kLeft] = getStreamFov(kLeft);
        CTX.fovArr[kRight] = getStreamFov(kRight);

        info("renderingParamsChanged, sending new view configs (FOV) to alvr");
        sendViewParams();
    }

    // Note: if GL context is recreated, old resources are already freed.
    if (CTX.renderingParamsChanged && !CTX.glContextRecreated) {
        info("Pausing ALVR since glContext is not recreated, deleting textures");
        ALVR_GL_CALL(alvr_pause_opengl());

        GL(glDeleteTextures(2, CTX.lobbyTextures));
    }

    if (CTX.renderingParamsChanged || CTX.glContextRecreated) {
        info("Rebuilding, binding textures, Resuming ALVR since glContextRecreated %b, "
             "renderingParamsChanged %b",
             CTX.renderingParamsChanged,
             CTX.glContextRecreated);
#ifdef PVR_TEXTURE_NAME_TEST
        {
            static bool burned = false;
            if (!burned) {
                GLuint burn[16];
                glGenTextures(16, burn);
                burned = true;
            }
        }
#endif
        GL(glGenTextures(2, CTX.lobbyTextures));

        for (auto &lobbyTexture : CTX.lobbyTextures) {
            GL(glBindTexture(GL_TEXTURE_2D, lobbyTexture));
            GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
            GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
            GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
            GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
            GL(glTexImage2D(GL_TEXTURE_2D,
                            0,
                            GL_RGB,
                            CTX.screenWidth / 2,
                            CTX.screenHeight,
                            0,
                            GL_RGB,
                            GL_UNSIGNED_BYTE,
                            nullptr));
        }

        // The ALVR library renders in its own EGL context (now a share context of this one).
        // Object creation/changes in this context are only guaranteed visible to the other
        // context after a flush/finish, so finish before handing the texture names over.
        glFinish();
        LOG("LOBBYTEX names=%u,%u isTexture=%d,%d size=%dx%d",
            (unsigned) CTX.lobbyTextures[0],
            (unsigned) CTX.lobbyTextures[1],
            (int) glIsTexture(CTX.lobbyTextures[0]),
            (int) glIsTexture(CTX.lobbyTextures[1]),
            CTX.screenWidth / 2,
            CTX.screenHeight);

        const uint32_t *targetViews[2] = {(const uint32_t *) &CTX.lobbyTextures[0],
                                          (const uint32_t *) &CTX.lobbyTextures[1]};
        // v21 dropped the trailing bool (was "always direct") - resume takes 4 args now.
        // alvr_resume_opengl() leaves the thread with no current EGL context (observed:
        // ctx/surf go to 0x0), which makes every later GL call fail and GLSurfaceView stop
        // rendering. The guard captures the app's EGL state before the call and rebinds it
        // immediately afterwards.
        EGLCHK("before alvr_resume_opengl");
        {
            EglStateGuard guard;
            alvr_resume_opengl(CTX.screenWidth / 2, CTX.screenHeight, targetViews, 1);
        }
        EGLCHK("after alvr_resume_opengl (restored)");

        // ARCore needs a GL texture to write camera frames into. We never draw the camera
        // image, but ARCore requires one. If the GL context was recreated the old texture is
        // already gone; otherwise delete it so it doesn't leak.
        if (CTX.arcoreEnabled && CTX.arSession != nullptr) {
            if (CTX.arTexture != 0 && !CTX.glContextRecreated) {
                glDeleteTextures(1, &CTX.arTexture);
            }
            GLuint arTextureIds[1] = {0};
            glGenTextures(1, arTextureIds);
            GL(glBindTexture(GL_TEXTURE_2D, arTextureIds[0]));
            GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
            GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
            GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
            GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));

            {
                std::lock_guard<std::mutex> lock(CTX.arMutex);
                CTX.arTexture = arTextureIds[0];
                ArSession_setCameraTextureName(CTX.arSession, CTX.arTexture);
            }
            info("ARCore camera texture initialized: %u", (unsigned) CTX.arTexture);
        }

        CTX.renderingParamsChanged = false;
        CTX.glContextRecreated = false;
    }

    AlvrEvent event;
    while (alvr_poll_event(&event)) {
        if (event.tag == ALVR_EVENT_HUD_MESSAGE_UPDATED) {
            auto message_length = alvr_hud_message(nullptr);
            auto message_buffer = std::vector<char>(message_length);

            alvr_hud_message(&message_buffer[0]);
            info("ALVR Poll Event: HUD Message Update - %s", &message_buffer[0]);

            // 3) inside the HUD branch, right after the call
            if (message_length > 0)
                ALVR_GL_CALL(alvr_update_hud_message_opengl(&message_buffer[0]));
            EGLCHK("after update_hud_message");
        }
        if (event.tag == ALVR_EVENT_STREAMING_STARTED) {
            info("ALVR Poll Event: ALVR_EVENT_STREAMING_STARTED, generating and binding "
                 "textures...");
            auto &started = event.STREAMING_STARTED;

            info("Streaming started: %dx%d, refresh hint=%f, gamma=%f, hdr=%b, ffe=%b",
                 started.view_width,
                 started.view_height,
                 started.refresh_rate_hint,
                 started.encoding_gamma,
                 started.enable_hdr,
                 started.enable_foveated_encoding);

            // v21 delivers the negotiated foveation params directly in the event - no more
            // manual settings-JSON parsing needed here (settings.video.foveated_encoding, as
            // the v20 code did).
            CTX.hasFrameMetadata = false;
            CTX.lastStreamBuffer = nullptr;
            CTX.videoFramesSeen = 0;

            // Upscaling: the stream is started_view_width x height, but when upscaling is on ALVR
            // renders into textures that are upscale_factor times bigger (it truncates the
            // product to whole pixels, exactly as done here), so the textures we hand over have
            // to be that big too. A texture of the wrong size is what turned the screen black.
            float upscaleFactor = 1.0f;
            if (ENABLE_UPSCALING) {
                float wantW = (float) (CTX.screenWidth / 2) / (float) started.view_width;
                float wantH = (float) CTX.screenHeight / (float) started.view_height;
                float want = std::min(wantW, wantH);
                if (want >= 1.05f) {  // stream is clearly smaller than the display
                    upscaleFactor = std::min(want, UPSCALING_MAX_FACTOR);
                }
            }
            const bool upscaling = upscaleFactor > 1.0f;
            const uint32_t targetWidth = (uint32_t) ((float) started.view_width * upscaleFactor);
            const uint32_t targetHeight = (uint32_t) ((float) started.view_height * upscaleFactor);
            info("Upscaling %s: stream %dx%d, factor %.3f, textures %ux%u",
                 upscaling ? "ON" : "off",
                 started.view_width,
                 started.view_height,
                 upscaleFactor,
                 targetWidth,
                 targetHeight);

            GL(glGenTextures(2, CTX.streamTextures));

            for (auto &streamTexture : CTX.streamTextures) {
                GL(glBindTexture(GL_TEXTURE_2D, streamTexture));
                GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
                GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
                GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
                GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
                GL(glTexImage2D(GL_TEXTURE_2D,
                                0,
                                GL_RGB,
                                targetWidth,
                                targetHeight,
                                0,
                                GL_RGB,
                                GL_UNSIGNED_BYTE,
                                nullptr));
            }

            CTX.fovArr[0] = getStreamFov((CardboardEye) 0);
            CTX.fovArr[1] = getStreamFov((CardboardEye) 1);
            sendViewParams();

            info("ALVR Poll Event: ALVR_EVENT_STREAMING_STARTED, View configs updated...");

            auto leftIntHandle = (uint32_t) CTX.streamTextures[0];
            auto rightIntHandle = (uint32_t) CTX.streamTextures[1];
            const uint32_t *textureHandles[2] = {&leftIntHandle, &rightIntHandle};

            auto render_config = AlvrStreamConfig{};
            render_config.view_resolution_width = started.view_width;
            render_config.view_resolution_height = started.view_height;
            render_config.swapchain_textures = textureHandles;
            render_config.swapchain_length = 1;

            render_config.enable_foveation = started.enable_foveated_encoding;
            render_config.foveated_encoding = started.foveated_encoding;

            // view_resolution above stays the size of the decoded stream; the textures are the
            // upscaled size (see above).
            render_config.enable_upscaling = upscaling;
            if (upscaling) {
                render_config.upscaling_edge_direction = UPSCALING_EDGE_DIRECTION;
                render_config.upscaling_edge_threshold = UPSCALING_EDGE_THRESHOLD;
                render_config.upscaling_edge_sharpness = UPSCALING_EDGE_SHARPNESS;
                render_config.upscale_factor = upscaleFactor;
            }

            ALVR_GL_CALL(alvr_start_stream_opengl(render_config));

            info("ALVR Poll Event: ALVR_EVENT_STREAMING_STARTED, opengl stream started and "
                 "input Thread started...");
            CTX.streaming = true;
            CTX.inputThread = std::thread(inputThread);
        } else if (event.tag == ALVR_EVENT_DECODER_CONFIG) {

            /*
             * v21 no longer sends the decoder config in the ALVR_EVENT_STREAMING_STARTED event,
             * so we have to create the decoder here instead. The decoder config is sent in a
             * separate event, ALVR_EVENT_DECODER_CONFIG, which is guaranteed to arrive before
             * the first frame.
             */
            if (!g_decoderCreated) {
                auto configSize = alvr_get_decoder_config(nullptr);
                auto configBuffer = std::vector<char>(configSize);
                alvr_get_decoder_config(configBuffer.data());

                // false = use the phone's hardware decoder. true forces ALVR's software decoder,
                // which cannot be created on some phones (creating it then hangs), so leave it false.
                constexpr bool FORCE_SOFTWARE_DECODER = false;

                // Same options the stock ALVR client gets from the server's defaults.
                AlvrMediacodecOption decoderOptions[3] = {};
                decoderOptions[0].key = "operating-rate";
                decoderOptions[0].ty = ALVR_MEDIACODEC_PROP_TYPE_INT32;
                decoderOptions[0].value.int32 = INT32_MAX;
                decoderOptions[1].key = "priority";
                decoderOptions[1].ty = ALVR_MEDIACODEC_PROP_TYPE_INT32;
                decoderOptions[1].value.int32 = 0;
                decoderOptions[2].key = "vendor.qti-ext-dec-low-latency.enable";
                decoderOptions[2].ty = ALVR_MEDIACODEC_PROP_TYPE_INT32;
                decoderOptions[2].value.int32 = 1;

                AlvrDecoderConfig decoderConfig = {};
                decoderConfig.codec = event.DECODER_CONFIG.codec;
                decoderConfig.force_software_decoder = FORCE_SOFTWARE_DECODER;
                decoderConfig.max_buffering_frames = 1.5f;
                decoderConfig.buffering_history_weight = 0.9f;
                decoderConfig.options = decoderOptions;
                decoderConfig.options_count = 3;
                decoderConfig.config_buffer =
                    reinterpret_cast<const uint8_t *>(configBuffer.data());
                decoderConfig.config_buffer_size = configSize;

                info("ALVR Poll Event: ALVR_EVENT_DECODER_CONFIG, codec=%d, config bytes=%llu, "
                     "software=%d",
                     (int) decoderConfig.codec, (unsigned long long) configSize,
                     (int) FORCE_SOFTWARE_DECODER);
                // The saved frame belongs to the previous decoder, if there was one.
                CTX.lastStreamBuffer = nullptr;
                alvr_create_decoder(decoderConfig);
                g_decoderCreated = true;
            }

        } else if (event.tag == ALVR_EVENT_STREAMING_STOPPED) {
            info("ALVR Poll Event: ALVR_EVENT_STREAMING_STOPPED, Waiting for inputThread to "
                 "join...");
            CTX.streaming = false;
            CTX.inputThread.join();

            // The camera must be off before the next decoder is created.
            if (ARCORE_START_AFTER_VIDEO) {
                stopARCoreSession("stream stopped");
            }

            // The saved frame belongs to the decoder that is about to be destroyed.
            CTX.lastStreamBuffer = nullptr;
            if (g_decoderCreated) {
                alvr_destroy_decoder();
                g_decoderCreated = false;
            }

            GL(glDeleteTextures(2, CTX.streamTextures));
            info("ALVR Poll Event: ALVR_EVENT_STREAMING_STOPPED, Stream stopped deleted "
                 "textures.");
        }
    }
    // 4) right after the closing brace of the `while (alvr_poll_event(&event))` loop,
    //    before `CardboardEyeTextureDescription viewsDescs[2] = {};`
    EGLCHK("after event loop");

    // Update ARCore on the render thread; getPose() then uses its position and correction.
    updateARCoreTracking();

    CardboardEyeTextureDescription viewsDescs[2] = {};
    for (auto &viewsDesc : viewsDescs) {
        viewsDesc.left_u = 0.0;
        viewsDesc.right_u = 1.0;
        viewsDesc.top_v = 1.0;
        viewsDesc.bottom_v = 0.0;
    }

    if (CTX.streaming) {
        void *streamHardwareBuffer = nullptr;
        uint64_t timestampNs = 0;

        // A new decoded frame is not available on every display refresh: the video usually
        // arrives more slowly than the display runs. Returning here without drawing leaves the
        // screen showing stale or blank content, which looks like stuttering. Instead keep
        // showing the last frame, re-oriented to the current head pose by the reprojection
        // below.
        bool isNewFrame = alvr_get_frame(&timestampNs, &streamHardwareBuffer);

        AlvrVideoFrameMetadata metadata;
        if (isNewFrame) {
            if (ARCORE_START_AFTER_VIDEO && CTX.videoFramesSeen < ARCORE_START_AFTER_VIDEO_FRAMES &&
                ++CTX.videoFramesSeen >= ARCORE_START_AFTER_VIDEO_FRAMES) {
                startARCoreSession("video is flowing");
            }
            CTX.lastStreamBuffer = streamHardwareBuffer;
            CTX.lastStreamTimestampNs = timestampNs;

            // v21 no longer returns view params from alvr_get_frame() directly; fetch the
            // server-aligned metadata (view params + foveation centers) matched to this frame
            // here instead. If there's no match for this exact frame, fall back to the last
            // known-good metadata, per the API doc for this function.
            if (alvr_report_compositor_start(timestampNs, &metadata)) {
                CTX.lastFrameMetadata = metadata;
                CTX.hasFrameMetadata = true;
            } else if (CTX.hasFrameMetadata) {
                metadata = CTX.lastFrameMetadata;
            } else {
                // No metadata at all yet (first frame) - nothing sensible to reproject against.
                return;
            }
        } else if (CTX.lastStreamBuffer != nullptr && CTX.hasFrameMetadata) {
            // No new frame: show the previous one again, with the metadata that belongs to it.
            streamHardwareBuffer = CTX.lastStreamBuffer;
            timestampNs = CTX.lastStreamTimestampNs;
            metadata = CTX.lastFrameMetadata;
        } else {
            // Nothing has been decoded yet.
            return;
        }

        // Current best head-orientation estimate, for reprojecting away the latency between
        // when the server rendered this frame and now. The Cardboard SDK's own distortion
        // renderer has no reprojection support of its own; this reprojection_rotation is what
        // lets v21 do it properly even on this simple client.
        AlvrPose currentPose = getPose(GetBootTimeNano() + VSYNC_QUEUE_INTERVAL_NS);

        AlvrStreamViewParams streamViewParams[2] = {};
        for (int eye = 0; eye < 2; eye++) {
            streamViewParams[eye].swapchain_index = 0;
            streamViewParams[eye].reprojection_rotation =
                alvr_rotation_delta(metadata.view_params[eye].pose.orientation,
                                     currentPose.orientation);
            streamViewParams[eye].fov = metadata.view_params[eye].fov;
        }

        const float(*foveationCenters)[2][2] =
            metadata.has_foveation_centers ? &metadata.foveation_center_shifts : nullptr;

        EGLCHK("before alvr_render_stream_opengl()");
        ALVR_GL_CALL(
            alvr_render_stream_opengl(streamHardwareBuffer, streamViewParams, foveationCenters));
        EGLCHK("after alvr_render_stream_opengl()");

        // The statistics are per decoded frame, so only report frames that are new.
        if (isNewFrame) {
            alvr_report_submit(timestampNs, 0);
        }

        viewsDescs[0].texture = CTX.streamTextures[0];
        viewsDescs[1].texture = CTX.streamTextures[1];
    } else {
        AlvrPose pose = getPose(GetBootTimeNano() + VSYNC_QUEUE_INTERVAL_NS);

        AlvrLobbyViewParams viewInputs[2] = {};
        for (int eye = 0; eye < 2; eye++) {
            float headToEye[3] = {CTX.eyeOffsets[eye], 0.0, 0.0};
            // offset head pos to Eye Position
            offsetPosWithQuat(
                pose.orientation, headToEye, viewInputs[eye].view_params.pose.position);

            viewInputs[eye].view_params.pose.orientation = pose.orientation;
            viewInputs[eye].view_params.fov = getFov((CardboardEye) eye);
            viewInputs[eye].swapchain_index = 0;
        }
        static int lobbyTexLogs = 0;
        if (lobbyTexLogs++ < 3) {
            LOG("LOBBYTEX before lobby render: names=%u,%u isTexture=%d,%d",
                (unsigned) CTX.lobbyTextures[0],
                (unsigned) CTX.lobbyTextures[1],
                (int) glIsTexture(CTX.lobbyTextures[0]),
                (int) glIsTexture(CTX.lobbyTextures[1]));
        }
        EGLCHK("before alvr_render_lobby_opengl()");
        ALVR_GL_CALL(alvr_render_lobby_opengl(viewInputs, /*render_background=*/true));
        EGLCHK("after alvr_render_lobby_opengl()");

        viewsDescs[0].texture = CTX.lobbyTextures[0];
        viewsDescs[1].texture = CTX.lobbyTextures[1];
    }
    EGLCHK("before CardboardDistortionRenderer_renderEyeToDisplay()");
    CardboardDistortionRenderer_renderEyeToDisplay(CTX.distortionRenderer,
                                                   0,
                                                   0,
                                                   0,
                                                   CTX.screenWidth,
                                                   CTX.screenHeight,
                                                   &viewsDescs[0],
                                                   &viewsDescs[1]);
    EGLCHK("after CardboardDistortionRenderer_renderEyeToDisplay()");
}

extern "C" JNIEXPORT void JNICALL
Java_viritualisres_phonevr_ALVRActivity_switchViewerNative(JNIEnv *, jobject) {
    CardboardQrCode_scanQrCodeAndSaveDeviceParams();
}
