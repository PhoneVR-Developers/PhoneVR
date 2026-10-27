#ifndef PHONEVR_HEAD_SENSOR_SOURCE_H
#define PHONEVR_HEAD_SENSOR_SOURCE_H

// Reads the phone's gyroscope and accelerometer through the NDK sensor API on its own thread and
// feeds a HeadOrientationFilter. start() when the app resumes, stop() when it pauses.
//
// AXES: the filter wants the head frame (x right, y up, z towards the wearer's face). The phone
// sits in the headset in landscape, with gravity along its x axis. Which way round it sits
// (the two landscape directions) is read from the accelerometer, so nothing here depends on
// what Cardboard calls "landscape left":
//     device +x points up   ->  head = (-y,  x, z)
//     device +x points down ->  head = ( y, -x, z)
// If the phone is later turned 180 degrees in the headset, the mapping flips (after the new
// direction has been steady for about a second) and the filter restarts, keeping the gyro bias.
//
// SENSORS: TYPE_GYROSCOPE_UNCALIBRATED (raw gyro; the OS's own bias guess is ignored because the
// filter estimates the bias itself), TYPE_ACCELEROMETER. Both at 200 Hz (5000 us), the most
// Android 12+ allows without the HIGH_SAMPLING_RATE_SENSORS permission.

#include <android/log.h>
#include <android/looper.h>
#include <android/sensor.h>

#include <atomic>
#include <cmath>
#include <thread>

#include "head_orientation_filter.h"

namespace phonevr {

class HeadSensorSource {
public:
    ~HeadSensorSource() { stop(); }

    void start(HeadOrientationFilter *filter, int rateUs = 5000) {
        if (running_.exchange(true)) return;
        filter_ = filter;
        rateUs_ = rateUs;
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
    }

private:
    static constexpr int kLooperId = 3;
    static constexpr int kTypeGyroUncalibrated = 16;  // ASENSOR_TYPE_GYROSCOPE_UNCALIBRATED (API 26+)
    static constexpr int kFlipSamples = 200;           // ~1 s at 200 Hz

    std::thread thread_;
    std::atomic<bool> running_{false};
    HeadOrientationFilter *filter_ = nullptr;
    int rateUs_ = 5000;

    // Mapping state (sensor thread only)
    bool mapKnown_ = false;
    int sign_ = 1;       // +1: device +x points up; -1: points down
    int flipCount_ = 0;  // consecutive accelerometer samples that disagree with sign_

    static void logi(const char *msg, double a = 0, double b = 0) {
        __android_log_print(ANDROID_LOG_INFO, "PhoneVR_HeadSensors", msg, a, b);
    }

    void remap(double x, double y, double z, double out[3]) const {
        if (sign_ > 0) { out[0] = -y; out[1] = x; out[2] = z; }
        else           { out[0] = y;  out[1] = -x; out[2] = z; }
    }

    void handleAccelerometer(const ASensorEvent &e) {
        double x = e.data[0], y = e.data[1], z = e.data[2];
        bool upright = std::fabs(x) > 5.0 && std::fabs(x) > std::fabs(z);  // headset-like pose
        if (!mapKnown_) {
            if (!upright) return;
            sign_ = x > 0 ? 1 : -1;
            mapKnown_ = true;
            logi("phone in headset, device +x points %.0f (1 = up, -1 = down)", sign_);
        } else if (upright && std::fabs(x) > 7.0 && (x > 0 ? 1 : -1) != sign_) {
            if (++flipCount_ >= kFlipSamples) {
                // Turned 180 degrees in the headset. A fixed device-frame gyro bias changes sign
                // in head x and y, so carry it across.
                double b[3];
                filter_->getGyroBias(b);
                sign_ = -sign_;
                flipCount_ = 0;
                filter_->reset();
                filter_->setGyroBias(-b[0], -b[1], b[2]);
                logi("phone turned round, device +x now points %.0f", sign_);
            }
        } else {
            flipCount_ = 0;
        }
        double h[3];
        remap(x, y, z, h);
        filter_->onAccelerometer(h[0], h[1], h[2]);
    }

    void handleGyroscope(const ASensorEvent &e) {
        if (!mapKnown_) return;
        double h[3];
        remap(e.data[0], e.data[1], e.data[2], h);
        filter_->onGyroscope(h[0], h[1], h[2], e.timestamp);  // CLOCK_BOOTTIME, like GetBootTimeNano()
    }

    void run() {
        ALooper *looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
        ASensorManager *mgr = ASensorManager_getInstance();  // minSdk 24: the package-name variant needs 26
        int gyroType = kTypeGyroUncalibrated;
        const ASensor *gyro = ASensorManager_getDefaultSensor(mgr, gyroType);
        if (!gyro) {
            gyroType = ASENSOR_TYPE_GYROSCOPE;
            gyro = ASensorManager_getDefaultSensor(mgr, gyroType);
        }
        const ASensor *accel = ASensorManager_getDefaultSensor(mgr, ASENSOR_TYPE_ACCELEROMETER);
        if (!gyro || !accel) {
            logi("no gyroscope or accelerometer; own head filter disabled");
            return;
        }
        ASensorEventQueue *queue =
            ASensorManager_createEventQueue(mgr, looper, kLooperId, nullptr, nullptr);
        if (!queue) {
            logi("could not create sensor event queue");
            return;
        }
        ASensorEventQueue_enableSensor(queue, gyro);
        ASensorEventQueue_setEventRate(queue, gyro, rateUs_);
        ASensorEventQueue_enableSensor(queue, accel);
        ASensorEventQueue_setEventRate(queue, accel, rateUs_);

        mapKnown_ = false;
        flipCount_ = 0;
        while (running_) {
            int ident = ALooper_pollAll(100, nullptr, nullptr, nullptr);
            if (ident != kLooperId) continue;
            ASensorEvent ev[32];
            ssize_t n;
            while ((n = ASensorEventQueue_getEvents(queue, ev, 32)) > 0) {
                for (ssize_t i = 0; i < n; i++) {
                    if (ev[i].type == ASENSOR_TYPE_ACCELEROMETER) handleAccelerometer(ev[i]);
                    else if (ev[i].type == gyroType) handleGyroscope(ev[i]);
                }
            }
        }
        ASensorEventQueue_disableSensor(queue, gyro);
        ASensorEventQueue_disableSensor(queue, accel);
        ASensorManager_destroyEventQueue(mgr, queue);
    }
};

}  // namespace phonevr

#endif  // PHONEVR_HEAD_SENSOR_SOURCE_H
