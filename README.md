<br/><br/>
[![PhonveVR](./.github/LogoPVR.png)](https://github.com/PhoneVR-Developers/PhoneVR/releases)<br/>
![LogoPVRv0 3](https://github.com/PhoneVR-Developers/PhoneVR/assets/4137788/b32a23f6-4264-45f9-99cc-bbdd593a8e94)

<br/>

<img src="./.github/rep1.png" width="50%"><img src="./.github/rep2.jpg" width="50%">
<sup>Pictures used for representational purposes</sup>

[![Build status](https://github.com/PhoneVR-Developers/PhoneVR/actions/workflows/build.yml/badge.svg)](https://github.com/PhoneVR-Developers/PhoneVR/actions)
![GitHub release (latest by date including pre-releases)](https://img.shields.io/github/v/release/PhoneVR-Developers/PhoneVR?color=orange&include_prereleases)
[![Discord](https://img.shields.io/discord/745688786410930296?label=Discuss&logo=Discord&logoColor=white)](https://discord.gg/pNYtBNk)

Use Steam VR-enabled applications with your phone as VR HMD (*Head-mounted display*). The only Open-Source solution to similar commercial packages like VRidge, Riftcat, Trinus etc etc. *Common-network* can be any type of network between desktop and android app, even **USB Tethering** and **Mobile Hotspots** are supported.
<br/>

| Tabs | Links |
| --- | --- |
| Roadmap/Plan/Future Todo | [**Project Kanban**](https://github.com/orgs/PhoneVR-Developers/projects/1/views/1) | 
| Latest Release | [`Github Release`](https://github.com/PhoneVR-Developers/PhoneVR/releases/latest) |
| Previous Builds | [`Github Actions`](https://github.com/PhoneVR-Developers/PhoneVR/actions) |
| Issues | [`Github Issue Tracker`](https://github.com/PhoneVR-Developers/PhoneVR/issues) |
| Discuss/Chat/Help/Real-Time Updates/Any - Discord | [`Discord Server Invite Link`](https://discord.com/invite/pNYtBNk) |

## ALVR v21 port with ARCore position tracking (this branch)

This branch ports the Android client from ALVR v20 to ALVR v21, because newer SteamVR releases
are breaking v20 servers, and adds 6DoF position tracking with ARCore.

* **ALVR v21:** the client talks to an ALVR **v21** streamer.
  This client was built against `21.0.0-dev14` (ALVR commit `9f118394`). **v20 servers are not
  compatible with this branch.**
* **Video:** Drame stutter present in the v20 client is
  fixed.
* **Position tracking (6DoF):** on phones with ARCore, head *position* is tracked. Cardboard's rotation drift (yaw) is corrected against it with a complementary filter. Phones
  without ARCore (or without Google Play Services for AR) run as 3DoF with the Cardboard tracker.
  See [`ARCORE_NOTES.md`](./ARCORE_NOTES.md).
* **Status:** works, but not released yet. There are no APKs for this branch; you build it from source
  (see [Building the v21 client](#building-the-v21-client)). The legacy PhoneVR server and the
  `gvr` build flavour are not part of this branch.
* **Technical details and the problems found on the way:** [`MIGRATION_NOTES.md`](./MIGRATION_NOTES.md).

Tested phones:

| Phone | Android | GPU | ARCore | Result |
| --- | --- | --- | --- | --- |
| Oppo Reno 9 A | 14 | Adreno 619 | yes | Video, 6DoF position and drift-free yaw |
| AQUOS sense4 basic | 12 | Adreno 618 | yes | Works (ARCore is started only after video is flowing) |
| UMIDIGI A3S | 10 | PowerVR Rogue GE8300 | no | Works as 3DoF |

Known limits: the yaw correction uses one fixed phone-to-ARCore alignment measured on a single phone,
so other phones may need a calibration that is not configurable yet; the floor height setting has not
been verified in a headset; and ARCore tracking is started again each time a stream starts, which
may cause a small position jump at that moment.

## Contents
* [ALVR v21 port with ARCore (this branch)](#alvr-v21-port-with-arcore-position-tracking-this-branch)
* [Requirements](#requirements)
* [Installation](#installation)
* [Advanced Configuration](#advanced-configuration)
* [Development](#development)
* [Building the v21 client](#building-the-v21-client)
* [Troubleshooting](#troubleshooting)
* [Issue / Bug Reporting](#issue--bug-reporting)

## Requirements

**For the ALVR v21 branch:** an Android 8.0+ phone with OpenGL ES 3.0+ and a hardware video decoder, and an ALVR v21 streamer on the PC. Position tracking additionally needs a phone [supported by ARCore](https://developers.google.com/ar/devices) with *Google Play Services for AR* installed; without it the app still runs, as 3DoF.

## Installation
The original PhoneVR(PVR) used with two servers ALVR and PVRServer. However, this fork uses ALVR Server only.

### A. Installation using ALVR Server
> **This branch (v21):** use an ALVR **v21** streamer, i.e. a build from ALVR's `master` branch (tested with `21.0.0-dev14`); 

* Use the install guide of ALVR Server from [here](https://github.com/alvr-org/ALVR#install).

### B. Installation using Legacy PhoneVR Server
> Not available on the ALVR v21 branch.

### Common Instructions for both Servers
* Install the Android Apk on your mobile from [`Releases`](https://github.com/PhoneVR-Developers/PhoneVR/releases/latest) page.
  * The ALVR v21 branch has no release APKs yet; build it as described in [Building the v21 client](#building-the-v21-client).
* Make sure that "Run in Background", "Auto Start"(Restart on Crash) permissions, if exists on your device(especially Xiaomi users), are given. Also make sure that any kind of 3rd party battery saver app dosen't kill PhoneVR when in background.

## Advanced Configuration 

### for ALVR Server
* This can be found [here](https://github.com/alvr-org/ALVR/wiki/Settings-guide) and [here](https://github.com/alvr-org/ALVR/wiki/Settings-tutorial).

## Development
This Project is presently under testing. But, pull requests are welcome. 

* Android App: App folder: `<root>/code/mobile/android/PhoneVR`
  * Compiled/Tested on `Android Studio 4.0.1`
  * For testing, this project has 2 Build Configs, Debug and Release. Debug has lots of debugging callouts to logcat from both JAVA and JNI.
  * (The ALVR v21 branch builds a single ALVR-based variant; the gvr/noGvr split described next applies to the v20 line.)
  * There are also two build flavours: gvr and noGvr. The gvr flavour is a superset of the noGvr flavour that adds support for legacy PhoneVR server with Google VR SDK (deprecated).
    * The source code of the gvr flavour is in app/src/gvr/ and app/src/main/. The code of the noGvr flavour is in app/src/noGvr/ and app/src/main/. Respectively, any shared code in app/src/main/ has to be tested with both flavours.
    * The prepare-alvr-deps.sh script, which needs to be run before building supports the optional "nogvr" argument. If you run the script without arguments, you can build both flavours. If you run the script with the "nogvr" argument, you can only build the noGvr flavour.
  
* External Vendor Libraries used (all Headers included in respective Projects):
  * Json v3.8.0 (https://github.com/nlohmann/json) (code\windows\libs\json)
  * Eigen v3.3.7 (https://gitlab.com/libeigen/eigen) (code\common\libs\eigen)
  * Asio v1.12.2 (https://sourceforge.net/projects/asio/files/asio/1.12.2%20%28Stable%29/) (code\common\libs\asio)
  * x264 0.161.r3015 MSVS15(2017) (https://github.com/ShiftMediaProject/x264)
  * GoogleVR 1.200 (https://github.com/googlevr/gvr-android-sdk) (code\mobile\android\libraries\jni)
  * ACRA v5.7 (https://github.com/ACRA/acra)
  * ALVR (https://github.com/alvr-org/ALVR) (code\mobile\android\PhoneVR\ALVR)
  * ARCore SDK 1.46.0 (https://github.com/google-ar/arcore-android-sdk) (ALVR v21 branch; C headers downloaded by `prepare-alvr-deps.sh`)
  * wgpu-hal 25.0.2, patched (https://github.com/gfx-rs/wgpu) (ALVR v21 branch; patch in `code/mobile/android/PhoneVR/patches/`)

* Code styling
  * Repo enforces clang-format v16.0.0 (.clang-format) for C/C++ and spotless (.editorconfig, google-java-format, ktfmt) for Java/Kotlin files
  * To autofix clang-format'ing issues devs can use `make format`
  * To autofix spotless format issues devs can goto PhoneVR android project folder and use `./gradlew spotlessApply` or use [spotless-gradle](https://plugins.jetbrains.com/plugin/18321-spotless-gradle) plugin for AndroidStudio/IDEs

## Building the v21 client

Tested on Ubuntu (a VirtualBox VM) with JDK 17, Rust 1.97.1 and Android NDK 25.2.9519653. These steps
are condensed from [`MIGRATION_NOTES.md`](./MIGRATION_NOTES.md), which has the details.

1. Install the Android SDK (a platform and build-tools must be installed) and NDK, and set
   `JAVA_HOME`, `ANDROID_HOME` and `ANDROID_NDK_HOME`.
2. In `code/mobile/android/PhoneVR`, run `prepare-alvr-deps.sh`. It prepares the ALVR dependencies
   (including `cargo xtask build-client-lib`) and downloads the ARCore C headers.
3. Patch `wgpu-hal` 25.0.2. Without this patch the lobby and video do not render, and some phones
   (for example one with a PowerVR GE8300 GPU) crash. Download the crate from
   `https://static.crates.io/crates/wgpu-hal/wgpu-hal-25.0.2.crate`, unpack it, and from inside the
   unpacked folder run
   `patch -p1 < <repo>/code/mobile/android/PhoneVR/patches/wgpu-hal-25.0.2-share-egl-context.patch`.
   It makes the Rust GL context share the app's EGL context and turns off GL debug output.
4. In ALVR's top-level `Cargo.toml`, point `[patch.crates-io]` at the patched folder:
   `wgpu-hal = { path = "<path to the patched wgpu-hal-25.0.2 folder>" }`.
   (The path used during development was absolute and machine-specific; use your own.)
5. Build the app with Gradle (Android Studio or `./gradlew assembleDebug`).

## Troubleshooting  
* **(ALVR v21 branch)** The lobby freezes as soon as the stream starts, while SteamVR still shows head movement
  - This happened on phones where the ARCore camera and the video decoder conflict. This branch starts ARCore only after video is flowing (`ARCORE_START_AFTER_VIDEO` in `alvr_main.cpp`). If you still see it, try turning ARCore off (last argument of `initializeNative` in `ALVRActivity.java`) and send the log.
* **(ALVR v21 branch)** The app crashes right after pressing the start button
  - Capture `adb logcat -d | grep -a -E "FATAL|UnsatisfiedLinkError|Fatal signal|panicked|dlopen"` and check that the `wgpu-hal` patch (see [Building the v21 client](#building-the-v21-client)) was applied.
* **(ALVR v21 branch)** No position tracking
  - Check the log for `ARCore: tracking`. If you see `could not create session`, the phone does not support ARCore or Google Play Services for AR is missing; the app then continues as 3DoF.


## Issue / Bug Reporting

Prior to reporting your Issue/Bug, please check out the ongoing issues ([here](https://github.com/PhoneVR-Developers/PhoneVR/issues) and [here](https://github.com/alvr-org/ALVR/issues)), If you have the same issue, you can join and watch that discussion(s).

You can use the [`Github Issues`](https://github.com/PhoneVR-Developers/PhoneVR/issues) to submit PhoneVR related issues/bugs related to working of this Project or for any query.
For reporting ALVRServer related issues please use [`ALVR Github Issues`](https://github.com/alvr-org/ALVR/issues).

For quick resolution you may want to add the following data along with your issue/bug report,

<ins>For Installation-time or SteamVR-and-PhoneVR-linking issues,</ins>
* `steamvr.vrsettings` file in default location `C:\Program Files (x86)\Steam\config\steamvr.vrsettings`
* `vrserver.txt` file in default location `C:\Program Files (x86)\Steam\logs\vrserver.txt`
* Open a `cmd` in the follow default directory and copy paste output of the `vrcmd` command. `C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\vrcmd.exe`

<ins>For networking issues,</ins>

* Network Capture of Desktop `.pcap` file. How-to-get-it-> [Here](https://github.com/PhoneVR-Developers/PhoneVR/issues/26#issuecomment-683905486)
* Network Capture of Android/Mobile `.pcap` file. How-to-get-it-> [Here](https://github.com/PhoneVR-Developers/PhoneVR/issues/26#issuecomment-687640757)

<ins>If Unexpected android app crashes occur(unexpected means, you did **NOT** get</ins> [this](https://user-images.githubusercontent.com/4137788/102040730-9c168180-3df3-11eb-985a-5d8d6798ea5a.jpg) <ins>screen when PhoneVR crashed),</ins>

1. Open `PhoneVR` android app and let it crash.
2. Collect `bugreport-xxx.zip` from android device (How-to under *"Capture a bug report from a device"* paragraph [here](https://developer.android.com/studio/debug/bug-report#bugreportdevice))
3. Attach the `bugreport-xxx.zip` file in Discord or GitHub or mail to *"phonevr.crash@gmail.com"*

<ins>**Common files required for all kinds issues,**</ins>
- If using ALVR
  * ALVR logs from its server, are available in the GUI (you may which to enable Setting -> Logging -> [Log to disk](https://github.com/alvr-org/ALVR/wiki/Settings-guide#log-to-disk) if you want to keep saving the logs to file, even after alvr is closed 
* Screenshots / Videos are much more helpful for developers to understand the issues faster.
* And ofcourse, how to reproduce the issue :)
