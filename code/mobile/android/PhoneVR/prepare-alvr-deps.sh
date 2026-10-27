rustup target add \
    aarch64-linux-android \
    armv7-linux-androideabi \
    x86_64-linux-android \
    i686-linux-android

# # Download ALVR source
# rm -r "ALVR"
# curl -sLS "https://github.com/alvr-org/ALVR/archive/refs/heads/master.zip" > download.zip
# # curl -sLS "https://github.com/ShootingKing-AM/ALVR/archive/refs/heads/phonevr.zip" > download.zip
# unzip download.zip
# rm download.zip
# mv ALVR-master ALVR
# # mv ALVR-phonevr ALVR

bash ./prepare-wgpu-hal.sh

pushd ALVR
cargo update
cargo xtask prepare-deps --platform android $@
popd

rm -r cardboard

# Download sdk source$
CARB_REPO_NAME="cardboard-master"
rm -r "${CARB_REPO_NAME}"
# curl -sLS "https://github.com/googlevr/cardboard/archive/refs/heads/master.zip" > download.zip
curl -sLS "https://github.com/nift4/cardboard/archive/refs/heads/master.zip" > download.zip
unzip download.zip
rm download.zip

# Build sdk
pushd "${CARB_REPO_NAME}"
./gradlew sdk:assembleRelease -Parm64-v8a
popd

# Prepare files
mkdir cardboard
mv "${CARB_REPO_NAME}/sdk/build/outputs/aar/sdk-release.aar" cardboard/cardboard-sdk.aar
cp "${CARB_REPO_NAME}/sdk/include/cardboard.h" cardboard/cardboard.h
rm -r "${CARB_REPO_NAME}"

# ARCore C API headers and native library, pinned to a release so the build does not change when
# Google updates the main branch. The version must match com.google.ar:core in app/build.gradle.
# To try a newer ARCore, change the version here and the dependency there (tags: v1.46.0 ...).
ARCORE_VERSION="1.46.0"
rm -rf "arcore-android-sdk-main" "arcore-android-sdk-${ARCORE_VERSION}"
curl -fsSL "https://github.com/google-ar/arcore-android-sdk/archive/refs/tags/v${ARCORE_VERSION}.zip" > download.zip
unzip -q download.zip
rm download.zip
# The rest of the build looks for this folder name, so keep it even though it is a pinned release.
mv "arcore-android-sdk-${ARCORE_VERSION}" "arcore-android-sdk-main"
