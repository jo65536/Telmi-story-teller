#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "system/system.h"
#include "system/keymap_hw.h"
#include "system/settings.h"
#include "system/settings_sync.h"
#include "system/display.h"

#include "./logs_helper.h"
#include "./time_helper.h"
#include "./app_lock.h"
#include "./app_brightness.h"
#include "./app_volume.h"
#include "./app_autosleep.h"
#include "./sdl_helper.h"
#include "./app_selector.h"
#include "./app_parameters.h"

// for ev.value
#define RELEASED 0
#define PRESSED 1
#define REPEAT 2

// How long the main loop is allowed to block in poll() waiting for a key, in
// milliseconds. A key press always wakes it up immediately, so these values
// only bound the latency of the timers the loop drives by itself.
#define POLL_TIMEOUT_AUDIO 50   // chaining to the next track, must be inaudible
#define POLL_TIMEOUT_TIMER 100  // a button is being held down
#define POLL_TIMEOUT_SCREEN 250 // an on-screen counter refreshes every second
#define POLL_TIMEOUT_IDLE 1000  // nothing but second-grained timers left
#define POWER_LONG_PRESS_MS 2000

// Global Variables
static int input_fd;
static struct input_event ev;
static struct pollfd fds[2];

bool keyinput_isValid(void) {
    read(input_fd, &ev, sizeof(ev));

    if (ev.type != EV_KEY || ev.value > REPEAT) {
        return false;
    }

    return true;
}

//
//    Longest the loop may sleep before it has work to do again.
//    Ordered from the tightest deadline to the loosest, so the first match is
//    the smallest applicable timeout.
//
int keyinput_pollTimeout(bool isPowerPressed) {
    // The SDL_mixer finished hook normally wakes poll through a self-pipe.
    // Keep the 50 ms path only as a compatibility fallback if pipe setup failed.
    if (!audio_notifications_available() && app_isAudioChaining()) {
        return POLL_TIMEOUT_AUDIO;
    }
    if (isPowerPressed || applock_isTimerRunning()) {
        return POLL_TIMEOUT_TIMER;
    }
    if (app_isScreenAnimated() || app_volume_isShowed() || app_brightness_isShowed()) {
        return POLL_TIMEOUT_SCREEN;
    }
    return POLL_TIMEOUT_IDLE;
}

int main(int argc, char *argv[]) {
    srand(time(NULL));
    display_init();
    video_audio_init();
    audio_notifications_init();
    settings_init();
    parameters_init();
    settings_setVolume(parameters_getAudioVolumeStartup(), true);
    settings_setBrightness(parameters_getScreenBrightnessStartup(), true, false);

    autosleep_init(parameters_getScreenOnInactivityTime(), parameters_getScreenOffInactivityTime());
    app_init();

    input_fd = open("/dev/input/event0", O_RDONLY);
    memset(&fds, 0, sizeof(fds));
    fds[0].fd = input_fd;
    fds[0].events = POLLIN;
    fds[1].fd = audio_notifications_fd();
    fds[1].events = POLLIN;

    bool isMenuPressed = false;
    bool menuPreventDefault = false;
    bool startPowerPressed = false;
    Uint32 startPowerPressedTime = 0;

    while (1) {
        if (autosleep_isSleepingTime() ||
            (startPowerPressed && SDL_GetTicks() - startPowerPressedTime >= POWER_LONG_PRESS_MS)) {
            goto exit_loop;
        }

        bool forceRefreshScreen = applock_checkLock();
        forceRefreshScreen = app_volume_checkDisplay() || forceRefreshScreen;
        forceRefreshScreen = app_brightness_checkDisplay() || forceRefreshScreen;
        app_update();

        // A pending refresh is applied at the end of this iteration, so never
        // sleep on it: that would delay what is already on screen by a tick.
        int pollTimeout = forceRefreshScreen ? 0 : keyinput_pollTimeout(startPowerPressed);

        int pollResult = poll(fds, 2, pollTimeout);
        if (pollResult > 0 && (fds[1].revents & POLLIN)) {
            audio_notifications_drain();
        }

        if (pollResult > 0 && (fds[0].revents & POLLIN)) {
            if (!keyinput_isValid()) {
                continue;
            }

            switch (ev.value) {
                case PRESSED:
                    switch (ev.code) {
                        case HW_BTN_MENU :
                            isMenuPressed = true;
                            forceRefreshScreen = applock_startTimer() || forceRefreshScreen;
                            if (applock_isLocked()) {
                                menuPreventDefault = true;
                            }
                            break;
                        case HW_BTN_POWER :
                            if (!applock_isLocked()) {
                                startPowerPressedTime = SDL_GetTicks();
                                startPowerPressed = true;
                            }
                            break;
                    }
                    break;

                case RELEASED:
                    if (applock_isLocked()) {
                        if (ev.code == HW_BTN_MENU) {
                            forceRefreshScreen = applock_stopTimer() || forceRefreshScreen;
                        }
                        break;
                    }
                    autosleep_keepAwake();
                    switch (ev.code) {
                        case HW_BTN_POWER :
                            // Short press: toggle the panel without leaving playback.
                            // Re-check the duration here in case RELEASE woke poll
                            // exactly as the long-press deadline elapsed.
                            if (startPowerPressed) {
                                if (SDL_GetTicks() - startPowerPressedTime >= POWER_LONG_PRESS_MS) {
                                    goto exit_loop;
                                }
                                if (display_enabled) {
                                    app_screenSleep();
                                } else {
                                    app_screenWakeUp();
                                }
                                // The panel state drives which inactivity delay
                                // applies, so recompute it once it has changed.
                                autosleep_keepAwake();
                            }
                            startPowerPressed = false;
                            break;
                        case HW_BTN_MENU :
                            if (!menuPreventDefault) {
                                app_menu();
                            }
                            isMenuPressed = false;
                            menuPreventDefault = false;
                            forceRefreshScreen = applock_stopTimer() || forceRefreshScreen;
                            break;
                        case HW_BTN_LEFT :
                            app_previous();
                            break;
                        case HW_BTN_RIGHT :
                            app_next();
                            break;
                        case HW_BTN_UP :
                            app_up();
                            break;
                        case HW_BTN_DOWN :
                            app_down();
                            break;
                        case HW_BTN_START :
                        case HW_BTN_SELECT :
                            app_pause();
                            break;
                        case HW_BTN_A :
                        case HW_BTN_B :
                            app_ok();
                            break;
                        case HW_BTN_Y :
                        case HW_BTN_X :
                            app_home();
                            break;
                    }

                    if (isMenuPressed) {
                        switch (ev.code) {
                            case HW_BTN_L2 :
                            case HW_BTN_VOLUME_DOWN :
                                forceRefreshScreen = app_brightness_down();
                                applock_stopTimer();
                                menuPreventDefault = true;
                                break;
                            case HW_BTN_R2 :
                            case HW_BTN_VOLUME_UP :
                                forceRefreshScreen = app_brightness_up();
                                applock_stopTimer();
                                menuPreventDefault = true;
                                break;
                            default:
                                break;
                        }
                    } else {
                        switch (ev.code) {
                            case HW_BTN_VOLUME_DOWN :
                                forceRefreshScreen = app_volume_down();
                                break;
                            case HW_BTN_VOLUME_UP :
                                forceRefreshScreen = app_volume_up();
                                break;
                            default:
                                break;
                        }
                    }
                    break;

                default:
                    break;
            }
        }

        if(forceRefreshScreen) {
            app_forceRefreshScreen();
        }
    }

    exit_loop:
    app_save();
    display_setScreen(true);
    video_audio_quit();
    system_shutdown();
    return EXIT_SUCCESS;
}
