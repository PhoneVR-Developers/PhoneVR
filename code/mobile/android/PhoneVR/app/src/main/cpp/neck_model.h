#ifndef PHONEVR_NECK_MODEL_H
#define PHONEVR_NECK_MODEL_H

// Neck model for seated 3DoF: fakes the small eye movement you get when the head pivots on the
// neck, so looking down at a dashboard or tilting towards a mirror gives parallax even though
// there is no positional tracking. It is not tracking: leaning the body or ducking does nothing.
//
// The eyes sit at e = (0, up, -forward) from the neck pivot, in the head frame (x right, y up,
// forward = -z). With head orientation R (world_from_head) the eyes are at R*e. The returned
// offset is that minus where the eyes would be with the same heading and no pitch or roll:
//     offset = R*e - Ry(yaw)*e
// so it is exactly zero when you look straight ahead, and ignores turning the head left and right
// (yaw has no fixed meaning in this app: it starts wherever the app launched and can be nudged
// by the ARCore yaw correction, so including it would make the view jump when that happens).
//
// Frame: y up, x/z horizontal (same as ALVR / OpenXR). Result in metres.

#include <cmath>

namespace phonevr {

// q = world_from_head orientation (x, y, z, w). lastYawRad is state kept by the caller: the
// heading is undefined when looking straight up or down, so it is held until the head levels out.
inline void neckModelOffset(float qx, float qy, float qz, float qw, float eyeUpM,
                            float eyeForwardM, float &lastYawRad, float out[3]) {
    // forward = -(third column of R); heading is its direction on the horizontal plane
    float fx = -2.f * (qx * qz + qw * qy);
    float fz = -(1.f - 2.f * (qx * qx + qy * qy));
    if (std::hypot(fx, fz) >= 0.25f) {
        lastYawRad = std::atan2(-fx, -fz);
    }
    float s = std::sin(lastYawRad), c = std::cos(lastYawRad);

    // R*e = up * (second column of R) - forward * (third column of R)
    float re[3];
    re[0] = eyeUpM * 2.f * (qx * qy - qw * qz) - eyeForwardM * 2.f * (qx * qz + qw * qy);
    re[1] = eyeUpM * (1.f - 2.f * (qx * qx + qz * qz)) - eyeForwardM * 2.f * (qy * qz - qw * qx);
    re[2] = eyeUpM * 2.f * (qy * qz + qw * qx) - eyeForwardM * (1.f - 2.f * (qx * qx + qy * qy));

    // Ry(yaw)*e for e = (0, up, -forward)
    float neutral[3] = {-eyeForwardM * s, eyeUpM, -eyeForwardM * c};
    for (int i = 0; i < 3; i++) {
        out[i] = re[i] - neutral[i];
    }
}

}  // namespace phonevr

#endif  // PHONEVR_NECK_MODEL_H
