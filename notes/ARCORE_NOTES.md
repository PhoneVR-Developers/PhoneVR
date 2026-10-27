# ARCore 6DoF on the v21 client

Last updated: 2026-10-07

## Status

ARCore position tracking and drift-free orientation work on the ALVR v21 client (branch
`arcore-on-v21`). After the world-yaw correction (step 6 below), a play session of roughly half an
hour showed no drift and no swimming. Everything was tested on one phone only.

Orientation comes from the Cardboard head tracker (fast, but its yaw drifts). ARCore supplies the
position and a slow yaw correction that cancels the drift. Everything lives in
`app/src/main/cpp/alvr_main.cpp`.

## How it fits together

- **Startup:** `initializeNative(..., enableARCore)` creates and configures the ARCore session
  (depth, light estimation and cloud anchors off; horizontal and vertical plane finding on; update
  mode "latest camera image", so `ArSession_update` does not block). If creation fails, the app
  continues as plain 3DoF.
- **Lifecycle:** the session is resumed in `resumeNative`, paused in `pauseNative` and destroyed in
  `destroyNative`. Every ARCore call is guarded by `CTX.arMutex`.
- **Camera texture:** ARCore needs a GL texture to write camera frames into. One is created right
  after `alvr_resume_opengl`. It is a plain `GL_TEXTURE_2D`; ARCore's documentation asks for an
  external-OES texture, but tracking worked this way on v20 and v21, so it was left alone.
- **Once per render frame:** `updateARCoreTracking()` runs on the render thread after the event
  loop. `ArSession_update` must run there, with the app's EGL context current. It reads ARCore's
  camera pose, publishes the position and updates the yaw correction.
- **From any thread:** `getPose(timestampNs)` returns Cardboard's orientation at that time, turned
  about the vertical by the current yaw correction, plus ARCore's last known position. The input
  thread, the render thread and the lobby all use it, so everything sees the same orientation.

## The yaw correction (complementary filter)

Each frame the code computes the world-space rotation that takes Cardboard's orientation onto
ARCore's. Both sensors know which way is down, so this is (nearly) a pure rotation about the
vertical, which is the yaw drift. The yaw part is extracted and a slow filter follows it:

- a sample is used only if it disagrees with the yaw currently being applied by less than
  `ARCORE_MAX_RESIDUAL_DEG`
- the correction follows ARCore with time constant `ARCORE_CORRECTION_TIME_CONSTANT_S`, using a
  gain that depends on elapsed time, so it does not depend on the frame rate
- if samples keep disagreeing for `ARCORE_REACQUIRE_AFTER_S`, ARCore has probably moved its own
  world frame (tracking lost and regained, or a relocalization), so the filter follows the new
  value with the faster `ARCORE_REACQUIRE_TIME_CONSTANT_S` until they agree again
- gaps longer than `ARCORE_MAX_CORRECTION_GAP_NS` (pause, debugger, lifecycle change) are skipped

### Why it is built this way

The first port copied the v20 filter, which had two flaws that only show up in long sessions:

1. **The disagreement check compared ARCore with raw Cardboard**, so it measured the total drift
   built up, not the disagreement left after correction. Once Cardboard had drifted about 15 degrees
   the filter rejected every sample and froze, and yaw drift appeared after five to six minutes.
   The check now compares against the orientation being shown.
2. **The correction was a rotation in the phone's own frame.** Gyro drift is a turn about the
   vertical, so a phone-frame correction is only right while the head keeps the pose it was
   measured in. Once the correction was large, looking up and down made the view roll and swim.
   A 21-minute log showed the disagreement growing with the size of the correction: about 1 degree
   below 20 degrees of correction, 3 degrees at 20 to 55, and about 8 degrees (with 12 percent of
   samples rejected) above 55. The correction is now a single angle about the vertical, which does
   not depend on where the head points.

## Tuning

All values are constants near the top of `alvr_main.cpp`. They were tuned on one phone.

| Constant | Meaning |
|---|---|
| `ARCORE_USE_POSITION` | Use ARCore's position as the head position. |
| `ARCORE_USE_ORIENTATION_CORRECTION` | Apply the yaw correction. |
| `ARCORE_CORRECTION_TIME_CONSTANT_S` | How fast the correction follows ARCore. Larger is smoother. |
| `ARCORE_MAX_RESIDUAL_DEG` | Disagreement above which a sample is ignored. |
| `ARCORE_REACQUIRE_AFTER_S`, `ARCORE_REACQUIRE_TIME_CONSTANT_S` | Re-acquire behaviour described above. |
| `ARCORE_MAX_CORRECTION_GAP_NS` | Longest time gap that is still filtered. |
| `ARCORE_TO_CARDBOARD` | Fixed rotation between ARCore's display-oriented camera frame and Cardboard's frame (landscape-left). Found by calibration on one phone. **Other phones may need a different value.** |
| `ARCORE_WORLD_UP_AXIS` | Which axis is up in the orientation the code uses (default y). |

## Height

`ARCORE_HEIGHT_MODE` selects how the vertical position is derived:

- **0:** relative to where ARCore started.
- **1:** height above the detected floor (the v20 behaviour). Until a floor is found it is the same
  as 0, so the height jumps when one is first detected.
- **2 (default):** assumes `ARCORE_ASSUMED_START_HEIGHT_M` above the floor until one is found, then
  eases toward the measured height (`ARCORE_FLOOR_BLEND_TIME_CONSTANT_S`).

`ARCORE_HEIGHT_OFFSET_M` is added in every mode, for tuning by hand. The floor is the lowest
horizontal, upward-facing plane ARCore reports, held by an anchor so it follows ARCore's frame.
**In a 21-minute session with the phone in the headset, no floor was ever found**, because the
camera rarely sees the floor. Do not rely on floor detection. Which mode and offset suit a given
setup is not settled.

## Related display fix (stuttering)

When the decoder has no new frame on a display refresh, the client draws the previous frame again
with the current head rotation, instead of returning without drawing. The saved buffer is cleared
whenever a stream starts or stops or the decoder is created or destroyed, because the buffer
belongs to the decoder.

## Build and device requirements

- ARCore 1.46.0 from Maven; Gradle also unpacks its native libraries (`natives` configuration).
- The ARCore C headers are downloaded by `prepare-alvr-deps.sh` from the `main` branch of
  `google-ar/arcore-android-sdk`. **They should be pinned to a tag that matches the library
  version.**
- The camera permission is already in the manifest. The manifest has no `com.google.ar.core`
  `meta-data` entry (`optional` or `required`); add it before releasing, so other phones can
  install or update ARCore.
- The phone needs Google Play Services for AR.

## Reading the log

`updateARCoreTracking()` prints one line a second: position, floor status, yaw offset from ARCore,
residual (and the largest residual that second), the applied yaw correction, the tilt disagreement
(the part of the difference that is not yaw; it should stay small, and if it is consistently large
the up axis is probably wrong), samples accepted and rejected, and `RE-ACQUIRING` when the filter is
following a new ARCore yaw. Filter with
`adb logcat -d | grep -E "ARCore: (tracking|not tracking|floor|yaw)"`.

## Not done / ideas

- Per-phone calibration of `ARCORE_TO_CARDBOARD` and the other constants, as a setting.
- Try a newer ARCore as one isolated change.
- A speed gate (ignore samples during fast head turns, when timing errors are largest).
- Snap the correction at the first tracked frame instead of easing in from zero.
- Attribution: the first ARCore plumbing came from serialuart's experimental PhoneVR 6DoF branch
  https://github.com/serialuart/PhoneVR/tree/arcore-workingtree.
- ARCore plane finding is enabled for both horizontal and vertical planes; only horizontal is used.
