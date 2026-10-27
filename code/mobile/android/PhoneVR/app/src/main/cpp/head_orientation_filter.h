#ifndef PHONEVR_HEAD_ORIENTATION_FILTER_H
#define PHONEVR_HEAD_ORIENTATION_FILTER_H

// 3DoF head orientation: Mahony complementary filter plus a stillness-based gyro bias estimator.
//
// FRAMES (same as OpenXR / ALVR, which is what the rest of alvr_main.cpp works in)
//   Head frame:  x = right, y = up, z = towards the wearer's face, forward = -z.
//   World frame: y = up (gravity), x/z horizontal. Identity = looking along -z.
//   The orientation returned is world_from_head, as a quaternion (x, y, z, w).
//
// INPUT: gyro (rad/s) and accelerometer (m/s^2, gravity included), already in the head frame, and
// the gyro timestamp in ns. Why the bias estimator exists: a gyro reads a small constant offset
// when still; integrating it makes the yaw turn slowly forever. Gravity corrects pitch and roll,
// nothing corrects yaw, so the offset has to be measured: whenever the head has been still for
// about half a second, the mean gyro reading in that window IS the offset.
//
// Thread-safe: sensor thread calls onGyroscope/onAccelerometer, any thread reads orientationAt().

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

namespace phonevr {

struct Quatd {
    double x = 0, y = 0, z = 0, w = 1;
};

class HeadOrientationFilter {
public:
    struct Config {
        // Mahony (accelerometer -> pitch/roll)
        double kp = 1.0;               // proportional gain, 1/s
        double ki = 0.0;               // integral gain; 0 = off (the bias estimator handles drift)
        double accelTolerance = 3.0;   // m/s^2 away from g at which the accelerometer weight is 0

        // Stillness-based gyro bias
        int biasWindowSamples = 100;   // ~0.5 s at 200 Hz
        double stillGyroStdDps = 0.6;  // max gyro std-dev inside the window to count as still
        double stillAccelStd = 0.12;   // max std-dev of |accel| (m/s^2)
        double stillAccelMeanTol = 0.4;// max | mean|accel| - g | (m/s^2)
        double bootstrapMeanDps = 5.0; // before the first estimate: max |mean gyro|
        double meanDevDps = 1.5;       // afterwards: max distance of the window mean from the bias
        double biasGain = 0.1;         // blend per accepted (non-overlapping) window

        // Prediction: orientationAt() extrapolates with the latest angular velocity, at most this far
        double maxPredictionSec = 0.1;
    };

    struct Diagnostics {
        double biasDps[3];
        bool biasReady;
    };

    // (Two constructors rather than "cfg = Config()": a default argument can't use Config's
    // member initializers before the enclosing class is complete.)
    HeadOrientationFilter() : HeadOrientationFilter(Config()) {}
    explicit HeadOrientationFilter(const Config &cfg) : cfg_(cfg), bias_(cfg) {}

    void onAccelerometer(double x, double y, double z) {
        std::lock_guard<std::mutex> lock(m_);
        ax_ = x; ay_ = y; az_ = z;
        hasAccel_ = true;
    }

    // Main step: call for every gyro sample.
    void onGyroscope(double gx, double gy, double gz, int64_t timestampNs) {
        std::lock_guard<std::mutex> lock(m_);
        if (!hasAccel_) return;
        if (!initialised_) {
            initFromAccel();
            lastTs_ = timestampNs;
            initialised_ = true;
            recenterLocked();
            return;
        }
        double dt = (double) (timestampNs - lastTs_) * 1e-9;
        lastTs_ = timestampNs;
        if (dt <= 0.0 || dt > 0.1) return;  // clock hiccup or pause: don't integrate

        double aMag = std::sqrt(ax_ * ax_ + ay_ * ay_ + az_ * az_);
        bias_.push(gx, gy, gz, aMag);

        double wx = gx - bias_.b[0];
        double wy = gy - bias_.b[1];
        double wz = gz - bias_.b[2];
        rateX_ = wx; rateY_ = wy; rateZ_ = wz;  // for prediction (bias-corrected, no accel term)

        // "up" in the head frame = second row of the rotation matrix
        double vx = 2.0 * (qx_ * qy_ + qw_ * qz_);
        double vy = 1.0 - 2.0 * (qx_ * qx_ + qz_ * qz_);
        double vz = 2.0 * (qy_ * qz_ - qw_ * qx_);

        // accelerometer error: a_unit x v, weighted down while the head is accelerating
        if (aMag > 1e-3) {
            double weight = std::max(0.0, 1.0 - std::fabs(aMag - kG) / cfg_.accelTolerance);
            if (weight > 0.0) {
                double nx = ax_ / aMag, ny = ay_ / aMag, nz = az_ / aMag;
                double ex = (ny * vz - nz * vy) * weight;
                double ey = (nz * vx - nx * vz) * weight;
                double ez = (nx * vy - ny * vx) * weight;
                if (cfg_.ki > 0.0) {
                    eIntX_ += ex * cfg_.ki * dt; eIntY_ += ey * cfg_.ki * dt; eIntZ_ += ez * cfg_.ki * dt;
                    wx += eIntX_; wy += eIntY_; wz += eIntZ_;
                }
                wx += cfg_.kp * ex; wy += cfg_.kp * ey; wz += cfg_.kp * ez;
            }
        }

        // integrate q' = 0.5 * q (x) (0, w)
        double h = 0.5 * dt;
        double nw = qw_ + h * (-qx_ * wx - qy_ * wy - qz_ * wz);
        double nx = qx_ + h * (qw_ * wx + qy_ * wz - qz_ * wy);
        double ny = qy_ + h * (qw_ * wy - qx_ * wz + qz_ * wx);
        double nz = qz_ + h * (qw_ * wz + qx_ * wy - qy_ * wx);
        double inv = 1.0 / std::sqrt(nw * nw + nx * nx + ny * ny + nz * nz);
        qw_ = nw * inv; qx_ = nx * inv; qy_ = ny * inv; qz_ = nz * inv;
    }

    bool ready() const {
        std::lock_guard<std::mutex> lock(m_);
        return initialised_;
    }

    // Orientation at timestampNs (same clock as the gyro timestamps; CLOCK_BOOTTIME on Android),
    // extrapolated from the last gyro sample with the current angular velocity.
    Quatd orientationAt(int64_t timestampNs) const {
        std::lock_guard<std::mutex> lock(m_);
        double dt = std::clamp((double) (timestampNs - lastTs_) * 1e-9, 0.0, cfg_.maxPredictionSec);
        double w[4] = {qw_, qx_, qy_, qz_};
        double speed = std::sqrt(rateX_ * rateX_ + rateY_ * rateY_ + rateZ_ * rateZ_);
        if (dt > 0.0 && speed > 1e-6) {
            double half = 0.5 * speed * dt, s = std::sin(half) / speed, c = std::cos(half);
            double dw = c, dx = rateX_ * s, dy = rateY_ * s, dz = rateZ_ * s;
            double r[4];
            mul(w, dw, dx, dy, dz, r);
            for (int i = 0; i < 4; i++) w[i] = r[i];
        }
        double out[4];
        mul2(ow_, 0.0, oy_, 0.0, w, out);
        return {out[1], out[2], out[3], out[0]};
    }

    // Makes the direction you are looking now "straight ahead". Output only; filter untouched.
    void recenter() {
        std::lock_guard<std::mutex> lock(m_);
        recenterLocked();
    }

    // Start over (e.g. on resume). Keeps the gyro bias estimate.
    void reset() {
        std::lock_guard<std::mutex> lock(m_);
        initialised_ = false;
        hasAccel_ = false;
        qw_ = 1; qx_ = qy_ = qz_ = 0;
        ow_ = 1; oy_ = 0;
        eIntX_ = eIntY_ = eIntZ_ = 0;
        rateX_ = rateY_ = rateZ_ = 0;
    }

    // rad/s. Can be saved per device and restored to skip the warm-up.
    void getGyroBias(double out[3]) const {
        std::lock_guard<std::mutex> lock(m_);
        for (int i = 0; i < 3; i++) out[i] = bias_.b[i];
    }
    void setGyroBias(double x, double y, double z) {
        std::lock_guard<std::mutex> lock(m_);
        bias_.b[0] = x; bias_.b[1] = y; bias_.b[2] = z;
        bias_.ready = true;
    }

    Diagnostics diagnostics() const {
        std::lock_guard<std::mutex> lock(m_);
        Diagnostics d;
        for (int i = 0; i < 3; i++) d.biasDps[i] = bias_.b[i] * 180.0 / M_PI;
        d.biasReady = bias_.ready;
        return d;
    }

private:
    static constexpr double kG = 9.80665;

    // r = a (x) (w, x, y, z)   with a = {w,x,y,z}
    static void mul(const double a[4], double bw, double bx, double by, double bz, double r[4]) {
        r[0] = a[0] * bw - a[1] * bx - a[2] * by - a[3] * bz;
        r[1] = a[0] * bx + a[1] * bw + a[2] * bz - a[3] * by;
        r[2] = a[0] * by - a[1] * bz + a[2] * bw + a[3] * bx;
        r[3] = a[0] * bz + a[1] * by - a[2] * bx + a[3] * bw;
    }
    // r = (aw, ax, ay, az) (x) b   with b = {w,x,y,z}
    static void mul2(double aw, double ax, double ay, double az, const double b[4], double r[4]) {
        r[0] = aw * b[0] - ax * b[1] - ay * b[2] - az * b[3];
        r[1] = aw * b[1] + ax * b[0] + ay * b[3] - az * b[2];
        r[2] = aw * b[2] - ax * b[3] + ay * b[0] + az * b[1];
        r[3] = aw * b[3] + ax * b[2] - ay * b[1] + az * b[0];
    }

    void initFromAccel() {
        double n = std::sqrt(ax_ * ax_ + ay_ * ay_ + az_ * az_);
        double ux = ax_ / n, uy = ay_ / n, uz = az_ / n;
        // shortest rotation taking the measured "up" (head frame) onto world +y
        if (1.0 + uy < 1e-6) {
            qw_ = 0; qx_ = 1; qy_ = 0; qz_ = 0;
        } else {
            double w = 1.0 + uy, x = -uz, z = ux;
            double m = 1.0 / std::sqrt(w * w + x * x + z * z);
            qw_ = w * m; qx_ = x * m; qy_ = 0; qz_ = z * m;
        }
    }

    void recenterLocked() {
        // forward = -(third column of R)
        double fx = -2.0 * (qx_ * qz_ + qw_ * qy_);
        double fz = -(1.0 - 2.0 * (qx_ * qx_ + qy_ * qy_));
        if (std::hypot(fx, fz) < 0.2) return;  // looking straight up/down: yaw undefined
        double yaw = std::atan2(-fx, -fz);     // rotation about +y
        ow_ = std::cos(yaw / 2.0);
        oy_ = -std::sin(yaw / 2.0);            // Ry(-yaw)
    }

    // Estimates the gyro bias whenever the head is still. Works on raw gyro (before bias
    // subtraction). O(1) per sample: ring buffer with running sums.
    struct BiasEstimator {
        explicit BiasEstimator(const Config &c)
            : cfg(c), n(std::max(8, c.biasWindowSamples)), stride(std::max(1, n / 4)) {
            for (auto &v : buf) v.assign(n, 0.0);
            am.assign(n, 0.0);
        }

        double b[3] = {0, 0, 0};  // bias, rad/s
        bool ready = false;

        void push(double gx, double gy, double gz, double aMag) {
            double g[3] = {gx, gy, gz};
            if (count == n) {
                for (int i = 0; i < 3; i++) { double o = buf[i][idx]; sum[i] -= o; sumSq[i] -= o * o; }
                double o = am[idx]; aSum -= o; aSumSq -= o * o;
            } else {
                count++;
            }
            for (int i = 0; i < 3; i++) { buf[i][idx] = g[i]; sum[i] += g[i]; sumSq[i] += g[i] * g[i]; }
            am[idx] = aMag; aSum += aMag; aSumSq += aMag * aMag;
            if (++idx == n) { idx = 0; recompute(); }  // stop float drift in the running sums
            sinceEval++; sinceUsed++;
            if (count < n || sinceEval < stride) return;
            sinceEval = 0;
            evaluate();
        }

    private:
        Config cfg;
        int n, stride;
        std::vector<double> buf[3];
        std::vector<double> am;
        double sum[3] = {0, 0, 0}, sumSq[3] = {0, 0, 0};
        double aSum = 0, aSumSq = 0;
        int idx = 0, count = 0, sinceEval = 0, sinceUsed = 0, streak = 0;
        static constexpr int kRebaseStreak = 30;

        static double rad(double deg) { return deg * M_PI / 180.0; }

        void recompute() {
            for (int i = 0; i < 3; i++) {
                double s = 0, q = 0;
                for (double v : buf[i]) { s += v; q += v * v; }
                sum[i] = s; sumSq[i] = q;
            }
            double s = 0, q = 0;
            for (double v : am) { s += v; q += v * v; }
            aSum = s; aSumSq = q;
        }

        void evaluate() {
            double inv = 1.0 / n;
            double aMean = aSum * inv;
            double aVar = aSumSq * inv - aMean * aMean;
            if (std::fabs(aMean - kG) > cfg.stillAccelMeanTol || aVar > cfg.stillAccelStd * cfg.stillAccelStd) {
                streak = 0;
                return;
            }
            double stdTol = rad(cfg.stillGyroStdDps);
            double tol = rad(ready ? cfg.meanDevDps : cfg.bootstrapMeanDps);
            bool meanOk = true;
            for (int i = 0; i < 3; i++) {
                double mean = sum[i] * inv;
                if (sumSq[i] * inv - mean * mean > stdTol * stdTol) { streak = 0; return; }
                if (std::fabs(mean - (ready ? b[i] : 0.0)) > tol) meanOk = false;
            }
            bool force = false;
            if (!meanOk) {
                // Still and steady but the mean moved: after ~5 s of that, believe it.
                if (!ready || ++streak < kRebaseStreak) return;
                force = true;
            }
            if (ready && !force && sinceUsed < n) return;  // don't reuse the same samples
            double alpha = (!ready || force) ? 1.0 : cfg.biasGain;
            for (int i = 0; i < 3; i++) b[i] += alpha * (sum[i] * inv - b[i]);
            ready = true;
            sinceUsed = 0;
            streak = 0;
        }
    };

    Config cfg_;
    BiasEstimator bias_;
    mutable std::mutex m_;

    double qw_ = 1, qx_ = 0, qy_ = 0, qz_ = 0;
    double ow_ = 1, oy_ = 0;  // recenter offset (rotation about world up), output only
    bool initialised_ = false;
    int64_t lastTs_ = 0;
    bool hasAccel_ = false;
    double ax_ = 0, ay_ = 0, az_ = 0;
    double eIntX_ = 0, eIntY_ = 0, eIntZ_ = 0;
    double rateX_ = 0, rateY_ = 0, rateZ_ = 0;
};

}  // namespace phonevr

#endif  // PHONEVR_HEAD_ORIENTATION_FILTER_H
