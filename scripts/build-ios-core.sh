#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds build/ios/VelinCore.xcframework: Velin's engine, platform services,
# extension host and the VLNPlayer Objective-C API as one static library per
# platform (iPhone and the Apple-silicon simulator). libmpv is not inside; the
# app links it (MPVKit), so the engine is built with VELIN_MPV_LINKED.
#
# Usage: scripts/build-ios-core.sh [Debug|Release]
set -euo pipefail

configuration="${1:-Release}"
root="$(cd "$(dirname "$0")/.." && pwd)"
output="$root/build/ios"
deployment_target="26.0"  # Floating-point std::from_chars needs iOS 26.

build_platform() {
    local sdk="$1" directory="$output/$1"
    cmake -S "$root" -B "$directory" -G Ninja \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_SYSROOT="$sdk" \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment_target" \
        -DCMAKE_BUILD_TYPE="$configuration" \
        -DBUILD_TESTING=OFF \
        -DVELIN_MPV_LINKED=ON >/dev/null
    cmake --build "$directory" --parallel
    libtool -static -no_warning_for_no_symbols -o "$directory/libVelinCore.a" \
        "$directory"/libvelin_apple.a "$directory"/libvelin_engine.a "$directory"/libvelin_extensions.a \
        "$directory"/libvelin_system.a "$directory"/libvelin_core.a \
        "$directory"/libvelin_quickjs.a "$directory"/libvelin_miniz.a
}

build_platform iphoneos
build_platform iphonesimulator

# Swift imports the Objective-C API as the VelinCore module. The headers sit
# in a VelinCore folder so the module map cannot clash with other packages.
headers="$output/headers"
rm -rf "$headers"
mkdir -p "$headers/VelinCore"
cp "$root/include/velin/apple/VLNPlayer.h" "$headers/VelinCore/"
cat > "$headers/VelinCore/module.modulemap" <<'MODULEMAP'
module VelinCore {
    header "VLNPlayer.h"
    link "c++"
    export *
}
MODULEMAP

rm -rf "$output/VelinCore.xcframework"
xcodebuild -create-xcframework \
    -library "$output/iphoneos/libVelinCore.a" -headers "$headers" \
    -library "$output/iphonesimulator/libVelinCore.a" -headers "$headers" \
    -output "$output/VelinCore.xcframework" >/dev/null
echo "Built $output/VelinCore.xcframework ($configuration)"
