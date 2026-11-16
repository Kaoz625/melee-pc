/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/touch.h"
#include "pc/pc.h"
#include <dolphin/pad.h>
#include <string.h>

#if defined(__ANDROID__) || defined(__APPLE__) || defined(TARGET_OS_IPHONE)
#include <pthread.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

#if defined(__ANDROID__)
#include <jni.h>
#endif

static pthread_mutex_t s_touch_lock = PTHREAD_MUTEX_INITIALIZER;
static PADStatus s_touch_status;
static uint16_t s_latched_buttons;
static bool s_touch_active;

void pc_touch_set_pad(uint16_t buttons, int8_t stickX, int8_t stickY, int8_t cstickX,
    int8_t cstickY, uint8_t triggerL, uint8_t triggerR) {
    pthread_mutex_lock(&s_touch_lock);
    s_touch_status.button = buttons;
    s_latched_buttons |= buttons;
    s_touch_status.stickX = stickX;
    s_touch_status.stickY = stickY;
    s_touch_status.substickX = cstickX;
    s_touch_status.substickY = cstickY;
    s_touch_status.triggerLeft = triggerL;
    s_touch_status.triggerRight = triggerR;
    s_touch_active = true;
    pthread_mutex_unlock(&s_touch_lock);
}

void pc_touch_set_active(bool active) {
    pthread_mutex_lock(&s_touch_lock);
    s_touch_active = active;
    if (!active) {
        memset(&s_touch_status, 0, sizeof(s_touch_status));
        s_latched_buttons = 0;
        PADClearVirtualStatus(0);
    }
    pthread_mutex_unlock(&s_touch_lock);
}

bool pc_touch_get_status(PADStatus* out) {
    pthread_mutex_lock(&s_touch_lock);
    if (!s_touch_active) {
        pthread_mutex_unlock(&s_touch_lock);
        return false;
    }
    *out = s_touch_status;
    out->button |= s_latched_buttons;
    s_latched_buttons = 0;
    pthread_mutex_unlock(&s_touch_lock);
    return true;
}

void pc_touch_apply(void) {
    PADStatus st = {0};
    if (pc_touch_get_status(&st)) {
        PADSetVirtualStatus(0, &st);
    }
}

#if !defined(__ANDROID__)
/* blz (Markus 15 Nov 2026: "drawn touch controls"): fixed round buttons the overlay draws
 * (pc_touch_layout, drawn by launcher.cpp from the same table) plus a floating stick on the
 * left. x is a fraction of the width, y and r fractions of the height. Each finger keeps its
 * own button, so holding A while tapping B works. */
static const PcTouchButton s_layout[] = {
    {"L", PAD_TRIGGER_L, 0.07f, 0.13f, 0.075f},
    {"R", PAD_TRIGGER_R, 0.93f, 0.13f, 0.075f},
    {"Z", PAD_TRIGGER_Z, 0.80f, 0.13f, 0.065f},
    {"START", PAD_BUTTON_START, 0.50f, 0.09f, 0.055f},
    {"Y", PAD_BUTTON_Y, 0.82f, 0.42f, 0.075f},
    {"X", PAD_BUTTON_X, 0.94f, 0.55f, 0.075f},
    {"A", PAD_BUTTON_A, 0.86f, 0.76f, 0.105f},
    {"B", PAD_BUTTON_B, 0.73f, 0.86f, 0.075f},
};
#define TOUCH_FINGERS 10
static struct {
    SDL_FingerID id;
    uint16_t button; /* 0xFFFF = this finger drives the stick */
    float ox, oy;
} s_fingers[TOUCH_FINGERS];
static int s_finger_count;

int pc_touch_layout(const PcTouchButton** out) {
    *out = s_layout;
    return (int)(sizeof(s_layout) / sizeof(s_layout[0]));
}

uint16_t pc_touch_held_buttons(void) {
    uint16_t held = 0;
    for (int i = 0; i < s_finger_count; i++)
        if (s_fingers[i].button != 0xFFFF)
            held |= s_fingers[i].button;
    return held;
}

static float s_aspect = 2.16f;

static uint16_t hit(float x, float y) {
    for (size_t i = 0; i < sizeof(s_layout) / sizeof(s_layout[0]); i++) {
        const float dx = (x - s_layout[i].x) * s_aspect, dy = y - s_layout[i].y;
        const float r = s_layout[i].r * 1.25f; /* a little slack around the drawn circle */
        if (dx * dx + dy * dy <= r * r)
            return s_layout[i].button;
    }
    return 0;
}

static void publish(int8_t sx, int8_t sy, bool keep_stick) {
    const uint16_t held = pc_touch_held_buttons();
    pc_touch_set_pad(held, keep_stick ? s_touch_status.stickX : sx, keep_stick ? s_touch_status.stickY : sy,
        0, 0, (held & PAD_TRIGGER_L) ? 255 : 0, (held & PAD_TRIGGER_R) ? 255 : 0);
}

void pc_touch_event(const SDL_Event* e) {
    if (e == NULL)
        return;
    if (e->type != SDL_EVENT_FINGER_DOWN && e->type != SDL_EVENT_FINGER_MOTION &&
        e->type != SDL_EVENT_FINGER_UP)
    {
        return;
    }
    SDL_Window* w = SDL_GetWindowFromID(e->tfinger.windowID);
    if (w) {
        int ww = 0, wh = 0;
        SDL_GetWindowSize(w, &ww, &wh);
        if (ww > 0 && wh > 0)
            s_aspect = (float)ww / (float)wh;
    }
    const float x = e->tfinger.x;
    const float y = e->tfinger.y;
    const SDL_FingerID fid = e->tfinger.fingerID;
    int slot = -1;
    for (int i = 0; i < s_finger_count; i++)
        if (s_fingers[i].id == fid)
            slot = i;

    if (e->type == SDL_EVENT_FINGER_DOWN) {
        if (slot >= 0 || s_finger_count >= TOUCH_FINGERS)
            return;
        uint16_t b = hit(x, y);
        bool stick_taken = false;
        for (int i = 0; i < s_finger_count; i++)
            stick_taken |= s_fingers[i].button == 0xFFFF;
        if (b == 0 && (x >= 0.5f || stick_taken))
            return;
        s_fingers[s_finger_count].id = fid;
        s_fingers[s_finger_count].button = b ? b : 0xFFFF;
        s_fingers[s_finger_count].ox = x;
        s_fingers[s_finger_count].oy = y;
        s_finger_count++;
        publish(0, 0, true);
    } else if (e->type == SDL_EVENT_FINGER_MOTION) {
        if (slot < 0 || s_fingers[slot].button != 0xFFFF)
            return;
        const float radius = 0.12f; /* of the height */
        float sx = (x - s_fingers[slot].ox) * s_aspect / radius;
        float sy = -(y - s_fingers[slot].oy) / radius;
        if (sx > 1.0f) sx = 1.0f;
        if (sx < -1.0f) sx = -1.0f;
        if (sy > 1.0f) sy = 1.0f;
        if (sy < -1.0f) sy = -1.0f;
        publish((int8_t)(sx * 80.0f), (int8_t)(sy * 80.0f), false);
    } else {
        if (slot < 0)
            return;
        const bool was_stick = s_fingers[slot].button == 0xFFFF;
        s_fingers[slot] = s_fingers[--s_finger_count];
        publish(0, 0, !was_stick);
    }
}

void pc_touch_get_status_peek(PADStatus* out) {
    pthread_mutex_lock(&s_touch_lock);
    *out = s_touch_status;
    pthread_mutex_unlock(&s_touch_lock);
}

bool pc_touch_stick_origin(float* x, float* y) {
    for (int i = 0; i < s_finger_count; i++)
        if (s_fingers[i].button == 0xFFFF) {
            *x = s_fingers[i].ox;
            *y = s_fingers[i].oy;
            return true;
        }
    return false;
}
#else
void pc_touch_event(const SDL_Event* e) {
    /* The Java overlay owns gameplay touch; launcher taps still pass through SDL. */
    (void)e;
}
#endif

#if defined(__ANDROID__)
#include "pc/net.h"
#include "pc/net_match.h"
#include "pc/net_lan.h"

JNIEXPORT void JNICALL Java_dev_melee_MeleeActivity_nativeDisconnect(JNIEnv* env, jclass clazz) {
    (void)env;
    (void)clazz;
    pc_net_match_stop();
    pc_net_disconnect();
    pc_lan_stop();
}

JNIEXPORT void JNICALL Java_dev_melee_TouchControls_nativeSetTouchPad(JNIEnv* env, jclass clazz,
    jint buttons, jint stickX, jint stickY, jint cstickX, jint cstickY, jint triggerL,
    jint triggerR) {
    (void)env;
    (void)clazz;
    pc_touch_set_pad((uint16_t)buttons, (int8_t)stickX, (int8_t)stickY, (int8_t)cstickX,
        (int8_t)cstickY, (uint8_t)triggerL, (uint8_t)triggerR);
}

JNIEXPORT void JNICALL Java_dev_melee_TouchControls_nativeSetTouchActive(
    JNIEnv* env, jclass clazz, jboolean active) {
    (void)env;
    (void)clazz;
    pc_touch_set_active(active == JNI_TRUE);
}
#endif

#else

bool pc_touch_get_status(PADStatus* out) {
    (void)out;
    return false;
}

void pc_touch_apply(void) {}

void pc_touch_set_pad(uint16_t buttons, int8_t stickX, int8_t stickY, int8_t cstickX,
    int8_t cstickY, uint8_t triggerL, uint8_t triggerR) {
    (void)buttons;
    (void)stickX;
    (void)stickY;
    (void)cstickX;
    (void)cstickY;
    (void)triggerL;
    (void)triggerR;
}

void pc_touch_set_active(bool active) {
    (void)active;
}

void pc_touch_event(const SDL_Event* e) {
    (void)e;
}

#endif
