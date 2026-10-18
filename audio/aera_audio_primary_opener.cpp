/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal recovery-side HIDL client that opens the stock Qualcomm primary
 * audio HAL. Android normally does this from AudioFlinger/framework code.
 * Keeping the returned IPrimaryDevice alive also keeps the primary HAL open.
 */

#include <android/hardware/audio/7.0/types.h>
#include <android/hardware/audio/7.1/IDevicesFactory.h>
#include <android/hardware/audio/7.1/IPrimaryDevice.h>
#include <unistd.h>

#include <cstdio>

using ::android::sp;
using ::android::hardware::Return;
using ::android::hardware::audio::V7_0::Result;
using ::android::hardware::audio::V7_1::IDevicesFactory;
using ::android::hardware::audio::V7_1::IPrimaryDevice;

namespace {

constexpr char kReadyMarker[] = "/tmp/aera-audio-primary-opened";

}  // namespace

int main() {
    unlink(kReadyMarker);

    sp<IDevicesFactory> factory;
    for (int attempt = 0; attempt < 50 && factory == nullptr; ++attempt) {
        factory = IDevicesFactory::tryGetService("default");
        if (factory == nullptr) usleep(100000);
    }

    if (factory == nullptr) {
        std::fprintf(stderr,
                     "AERA audio primary: IDevicesFactory/default unavailable\n");
        return 69;
    }

    Result result = Result::NOT_INITIALIZED;
    sp<IPrimaryDevice> primary;

    Return<void> transaction = factory->openPrimaryDevice_7_1(
        [&](Result callback_result, const sp<IPrimaryDevice>& callback_device) {
            result = callback_result;
            primary = callback_device;
        });

    if (!transaction.isOk()) {
        std::fprintf(stderr,
                     "AERA audio primary: openPrimaryDevice_7_1 binder call failed\n");
        return 70;
    }

    if (result != Result::OK || primary == nullptr) {
        std::fprintf(stderr,
                     "AERA audio primary: primary HAL open failed, result=%d\n",
                     static_cast<int>(result));
        return 71;
    }

    FILE* marker = std::fopen(kReadyMarker, "w");
    if (marker == nullptr) {
        std::fprintf(stderr,
                     "AERA audio primary: could not create ready marker\n");
        return 72;
    }
    std::fprintf(marker, "ready\n");
    std::fclose(marker);

    std::fprintf(stderr, "AERA audio primary: primary Qualcomm HAL opened\n");

    for (;;) pause();
}
