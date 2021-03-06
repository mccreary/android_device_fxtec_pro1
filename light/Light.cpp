/*
 * Copyright (C) 2018-2020 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "LightService"

#include "Light.h"

#include <fcntl.h>
#include <sys/inotify.h>

#include <android-base/logging.h>
#include <linux/input.h>

namespace {

using android::hardware::light::V2_0::LightState;

static constexpr int RAMP_SIZE = 8;
static constexpr int RAMP_STEP_DURATION = 50;

static constexpr int BRIGHTNESS_RAMP[RAMP_SIZE] = {0, 12, 25, 37, 50, 72, 85, 100};
static constexpr int DEFAULT_MAX_BRIGHTNESS = 255;

static uint32_t rgbToBrightness(const LightState& state) {
    uint32_t color = state.color & 0x00ffffff;
    return ((77 * ((color >> 16) & 0xff)) + (150 * ((color >> 8) & 0xff)) +
            (29 * (color & 0xff))) >> 8;
}

static bool isLit(const LightState& state) {
    return (state.color & 0x00ffffff);
}

static std::string getScaledDutyPcts(int brightness) {
    std::string buf, pad;

    for (auto i : BRIGHTNESS_RAMP) {
        buf += pad;
        buf += std::to_string(i * brightness / 255);
        pad = ",";
    }

    return buf;
}

static int openInputDeviceByName(const char* name) {
    int fd;
    unsigned int n;
    char pathname[PATH_MAX];
    char devname[256];

    fd = -1;
    n = 0;
    while (fd < 0) {
        sprintf(pathname, "/dev/input/event%u", n++);
        fd = TEMP_FAILURE_RETRY(open(pathname, O_RDONLY | O_CLOEXEC));
        if (fd < 0) {
            // Fail: out of devices to try
            break;
        }
        memset(devname, 0, sizeof(devname));
        if (ioctl(fd, EVIOCGNAME(sizeof(devname) - 1), &devname) > 0) {
            if (!strcmp(devname, name)) {
                // Success
                break;
            }
        }
        close(fd);
        fd = -1;
    }

    return fd;
}

static bool readEvent(int fd, input_event& ev) {
    ssize_t len = TEMP_FAILURE_RETRY(read(fd, &ev, sizeof(ev)));
    return (len == sizeof(ev));
}

}  // anonymous namespace

namespace android {
namespace hardware {
namespace light {
namespace V2_0 {
namespace implementation {

static void* sliderMonitorThread(void* arg) {
    Light* owner = (Light*)arg;
    int fd;

restart:
    fd = openInputDeviceByName("gpio-keys");
    if (fd < 0) {
        LOG(ERROR) << "Cannot open slider input device";
        sleep(1);
        goto restart;
    }

    LOG(INFO) << "sliderMonitorThread enter loop";
    while (1) {
        struct input_event ev;
        if (!readEvent(fd, ev)) {
            LOG(ERROR) << "Failed to read slider event";
            close(fd);
            sleep(1);
            goto restart;
        }
        // Ignore unwanted event types and codes (eg. EV_SYN)
        if (ev.type != EV_SW || ev.code != SW_LID) {
            continue;
        }
        bool open = (ev.value == 0);
        owner->onSliderChanged(open);
    }
    close(fd);

    return nullptr;
}

Light::Light(std::pair<std::ofstream, uint32_t>&& lcd_backlight,
             std::pair<std::ofstream, uint32_t>&& keyboard_backlight,
             std::vector<std::ofstream>&& button_backlight,
             std::ofstream&& red_led, std::ofstream&& green_led, std::ofstream&& blue_led,
             std::ofstream&& red_duty_pcts, std::ofstream&& green_duty_pcts, std::ofstream&& blue_duty_pcts,
             std::ofstream&& red_start_idx, std::ofstream&& green_start_idx, std::ofstream&& blue_start_idx,
             std::ofstream&& red_pause_lo, std::ofstream&& green_pause_lo, std::ofstream&& blue_pause_lo,
             std::ofstream&& red_pause_hi, std::ofstream&& green_pause_hi, std::ofstream&& blue_pause_hi,
             std::ofstream&& red_ramp_step_ms, std::ofstream&& green_ramp_step_ms, std::ofstream&& blue_ramp_step_ms,
             std::ofstream&& red_blink, std::ofstream&& green_blink, std::ofstream&& blue_blink)
    : mLcdBacklight(std::move(lcd_backlight)),
      mKeyboardBacklight(std::move(keyboard_backlight)),
      mButtonBacklight(std::move(button_backlight)),
      mRedLed(std::move(red_led)),
      mGreenLed(std::move(green_led)),
      mBlueLed(std::move(blue_led)),
      mRedDutyPcts(std::move(red_duty_pcts)),
      mGreenDutyPcts(std::move(green_duty_pcts)),
      mBlueDutyPcts(std::move(blue_duty_pcts)),
      mRedStartIdx(std::move(red_start_idx)),
      mGreenStartIdx(std::move(green_start_idx)),
      mBlueStartIdx(std::move(blue_start_idx)),
      mRedPauseLo(std::move(red_pause_lo)),
      mGreenPauseLo(std::move(green_pause_lo)),
      mBluePauseLo(std::move(blue_pause_lo)),
      mRedPauseHi(std::move(red_pause_hi)),
      mGreenPauseHi(std::move(green_pause_hi)),
      mBluePauseHi(std::move(blue_pause_hi)),
      mRedRampStepMs(std::move(red_ramp_step_ms)),
      mGreenRampStepMs(std::move(green_ramp_step_ms)),
      mBlueRampStepMs(std::move(blue_ramp_step_ms)),
      mRedBlink(std::move(red_blink)),
      mGreenBlink(std::move(green_blink)),
      mBlueBlink(std::move(blue_blink)),
      mLcdBacklightOn(false),
      mSliderOpen(false) {
    auto attnFn(std::bind(&Light::setAttentionLight, this, std::placeholders::_1));
    auto backlightFn(std::bind(&Light::setLcdBacklight, this, std::placeholders::_1));
    auto batteryFn(std::bind(&Light::setBatteryLight, this, std::placeholders::_1));
    auto buttonsFn(std::bind(&Light::setButtonsBacklight, this, std::placeholders::_1));
    auto notifFn(std::bind(&Light::setNotificationLight, this, std::placeholders::_1));
    mLights.emplace(std::make_pair(Type::ATTENTION, attnFn));
    mLights.emplace(std::make_pair(Type::BACKLIGHT, backlightFn));
    mLights.emplace(std::make_pair(Type::BATTERY, batteryFn));
    mLights.emplace(std::make_pair(Type::BUTTONS, buttonsFn));
    mLights.emplace(std::make_pair(Type::NOTIFICATIONS, notifFn));

    pthread_t th;
    (void)pthread_create(&th, nullptr, sliderMonitorThread, this);
}

// Methods from ::android::hardware::light::V2_0::ILight follow.
Return<Status> Light::setLight(Type type, const LightState& state) {
    auto it = mLights.find(type);

    if (it == mLights.end()) {
        return Status::LIGHT_NOT_SUPPORTED;
    }

    it->second(state);

    return Status::SUCCESS;
}

Return<void> Light::getSupportedTypes(getSupportedTypes_cb _hidl_cb) {
    std::vector<Type> types;

    for (auto const& light : mLights) {
        types.push_back(light.first);
    }

    _hidl_cb(types);

    return Void();
}

void Light::onSliderChanged(bool open) {
    std::lock_guard<std::mutex> lock(mLock);
    mSliderOpen = open;
    setKeyboardBacklightLocked();
}

void Light::setAttentionLight(const LightState& state) {
    std::lock_guard<std::mutex> lock(mLock);
    mAttentionState = state;
    setSpeakerBatteryLightLocked();
}

void Light::setLcdBacklight(const LightState& state) {
    std::lock_guard<std::mutex> lock(mLock);

    mLcdBacklightOn = (state.color != 0);

    uint32_t brightness = rgbToBrightness(state);

    // If max panel brightness is not the default (255),
    // apply linear scaling across the accepted range.
    if (mLcdBacklight.second != DEFAULT_MAX_BRIGHTNESS) {
        int old_brightness = brightness;
        brightness = brightness * mLcdBacklight.second / DEFAULT_MAX_BRIGHTNESS;
        LOG(VERBOSE) << "scaling brightness " << old_brightness << " => " << brightness;
    }

    mLcdBacklight.first << brightness << std::endl;

    setKeyboardBacklightLocked();
}

void Light::setKeyboardBacklightLocked() {
    mKeyboardBacklight.second = (mSliderOpen && mLcdBacklightOn)
                                ? DEFAULT_MAX_BRIGHTNESS
                                : 0;
    LOG(INFO) << "setKeyboardBacklightLocked:" <<
                 " mSliderOpen=" << (mSliderOpen ? "true" : "false") <<
                 " mLcdBacklightOn=" << (mLcdBacklightOn ? "true" : "false");
    mKeyboardBacklight.first << mKeyboardBacklight.second << std::endl;
}

void Light::setButtonsBacklight(const LightState& state) {
    std::lock_guard<std::mutex> lock(mLock);

    uint32_t brightness = rgbToBrightness(state);

    for (auto& button : mButtonBacklight) {
        button << brightness << std::endl;
    }
}

void Light::setBatteryLight(const LightState& state) {
    std::lock_guard<std::mutex> lock(mLock);
    mBatteryState = state;
    setSpeakerBatteryLightLocked();
}

void Light::setNotificationLight(const LightState& state) {
    std::lock_guard<std::mutex> lock(mLock);

    uint32_t brightness, color, rgb[3];
    LightState localState = state;

    // If a brightness has been applied by the user
    brightness = (localState.color & 0xff000000) >> 24;
    if (brightness > 0 && brightness < 255) {
        // Retrieve each of the RGB colors
        color = localState.color & 0x00ffffff;
        rgb[0] = (color >> 16) & 0xff;
        rgb[1] = (color >> 8) & 0xff;
        rgb[2] = color & 0xff;

        // Apply the brightness level
        if (rgb[0] > 0) {
            rgb[0] = (rgb[0] * brightness) / 0xff;
        }
        if (rgb[1] > 0) {
            rgb[1] = (rgb[1] * brightness) / 0xff;
        }
        if (rgb[2] > 0) {
            rgb[2] = (rgb[2] * brightness) / 0xff;
        }

        // Update with the new color
        localState.color = (rgb[0] << 16) + (rgb[1] << 8) + rgb[2];
    }

    mNotificationState = localState;
    setSpeakerBatteryLightLocked();
}

void Light::setSpeakerBatteryLightLocked() {
    if (isLit(mNotificationState)) {
        setSpeakerLightLocked(mNotificationState);
    } else if (isLit(mAttentionState)) {
        setSpeakerLightLocked(mAttentionState);
    } else if (isLit(mBatteryState)) {
        setSpeakerLightLocked(mBatteryState);
    } else {
        // Lights off
        mRedLed << 0 << std::endl;
        mGreenLed << 0 << std::endl;
        mBlueLed << 0 << std::endl;
        mRedBlink << 0 << std::endl;
        mGreenBlink << 0 << std::endl;
        mBlueBlink << 0 << std::endl;
    }
}

void Light::setSpeakerLightLocked(const LightState& state) {
    int red, green, blue, blink;
    int onMs, offMs, stepDuration, pauseHi;
    uint32_t colorRGB = state.color;

    switch (state.flashMode) {
        case Flash::TIMED:
            onMs = state.flashOnMs;
            offMs = state.flashOffMs;
            break;
        case Flash::NONE:
        default:
            onMs = 0;
            offMs = 0;
            break;
    }

    red = (colorRGB >> 16) & 0xff;
    green = (colorRGB >> 8) & 0xff;
    blue = colorRGB & 0xff;
    blink = onMs > 0 && offMs > 0;

    if (blink) {
        stepDuration = RAMP_STEP_DURATION;
        pauseHi = onMs - (stepDuration * RAMP_SIZE * 2);

        if (stepDuration * RAMP_SIZE * 2 > onMs) {
            stepDuration = onMs / (RAMP_SIZE * 2);
            pauseHi = 0;
        }

        // Red
        mRedStartIdx << 0 << std::endl;
        mRedDutyPcts << getScaledDutyPcts(red) << std::endl;
        mRedPauseLo << offMs << std::endl;
        mRedPauseHi << pauseHi << std::endl;
        mRedRampStepMs << stepDuration << std::endl;

        // Green
        mGreenStartIdx << RAMP_SIZE << std::endl;
        mGreenDutyPcts << getScaledDutyPcts(green) << std::endl;
        mGreenPauseLo << offMs << std::endl;
        mGreenPauseHi << pauseHi << std::endl;
        mGreenRampStepMs << stepDuration << std::endl;

        // Blue
        mBlueStartIdx << RAMP_SIZE * 2 << std::endl;
        mBlueDutyPcts << getScaledDutyPcts(blue) << std::endl;
        mBluePauseLo << offMs << std::endl;
        mBluePauseHi << pauseHi << std::endl;
        mBlueRampStepMs << stepDuration << std::endl;
    } else {
        if (red == 0 && green == 0 && blue == 0) {
            mRedBlink << 0 << std::endl;
            mGreenBlink << 0 << std::endl;
            mBlueBlink << 0 << std::endl;
        }
        mRedLed << red << std::endl;
        mGreenLed << green << std::endl;
        mBlueLed << blue << std::endl;
    }
}

}  // namespace implementation
}  // namespace V2_0
}  // namespace light
}  // namespace hardware
}  // namespace android
