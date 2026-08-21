#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <assert.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "system/battery.h"
#include "system/device_model.h"
#include "system/display.h"
#include "system/keymap_hw.h"
#include "system/rumble.h"
#include "system/system.h"
#include "utils/file.h"
#include "utils/log.h"
#include "utils/msleep.h"

#define SYSTEM_RESOURCES "/mnt/SDCARD/.tmp_update/res/"
#define FONT_BOLD SYSTEM_RESOURCES "Exo2-Bold.ttf"
#define FONT_REGULAR SYSTEM_RESOURCES "Exo2-Regular.ttf"

#define RELEASED 0
#define PRESSED 1
#define REPEATING 2

#define DISPLAY_TIMEOUT 10000

// Linear full-charge model: about three hours end to end (seconds per percent),
// used until the session has measured enough of a climb for a real slope.
#define CHARGE_MODEL_S_PER_PCT 108

static volatile sig_atomic_t quit = 0;
static bool suspended = false;
static int input_fd;
static struct input_event ev;
static struct pollfd fds[1];

SDL_Window *window = NULL;
SDL_Renderer *renderer = NULL;
SDL_Surface *screen = NULL;
SDL_Texture *texture = NULL;

void applyScreen(void) {
    SDL_RenderClear(renderer);
    SDL_UpdateTexture(texture, NULL, screen->pixels, screen->pitch);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

void suspend(bool enabled) {
    suspended = enabled;
    if (suspended) {
        SDL_FillRect(screen, NULL, 0);
        applyScreen();
    }
    // system_powersave(suspended);
    display_setScreen(!suspended);
}

static void sigHandler(int sig) {
    switch (sig) {
        case SIGINT:
        case SIGTERM:
            quit = true;
            break;
        default:
            break;
    }
}

// One axp_test launch gives charge state AND battery level (Mini Plus only) —
// same cost as the battery_isCharging() poll it replaces. On a failed sample
// both outputs are left untouched, so a transient hiccup of the vendor tool
// cannot fake an "unplugged" state and shut the device down.
static bool mmp_battery_status(bool *is_charging, int *percentage) {
    char buf[128] = "";
    int perc = 0, volt = 0, charging = 0;
    FILE *fp = popen("cd /customer/app/ && ./axp_test", "r");
    if (fp == NULL)
        return false;
    bool valid = fgets(buf, sizeof(buf), fp) != NULL &&
                 sscanf(buf, "{\"battery\":%d, \"voltage\":%d, \"charging\":%d}",
                        &perc, &volt, &charging) == 3;
    pclose(fp);
    if (valid) {
        *is_charging = charging == 3;
        if (perc >= 0 && perc <= 100)
            *percentage = perc;
    }
    return valid;
}

// Minutes until full: measured slope once the level climbed at least two
// points this session, otherwise the linear model. 0 = nothing to show.
static int eta_minutes(int perc, int start_perc, uint32_t start_ticks, uint32_t now) {
    if (perc < 1 || perc >= 99)
        return 0;
    if (start_perc >= 0 && perc >= start_perc + 2 && now > start_ticks) {
        uint32_t ms_per_pct = (now - start_ticks) / (uint32_t)(perc - start_perc);
        return (int)((uint32_t)(100 - perc) * (ms_per_pct / 1000u) / 60u);
    }
    return (100 - perc) * CHARGE_MODEL_S_PER_PCT / 60;
}

// "~2h05" / "~45min". Round to the nearest 5 minutes first, then pick the
// format, so the hour boundary cannot flip between "~1h00" and "~60min".
static void format_eta(char *out, size_t n, int minutes) {
    if (minutes < 1) {
        out[0] = '\0';
        return;
    }
    int rounded = (minutes + 2) / 5 * 5;
    if (rounded < 5)
        rounded = 5;
    if (rounded >= 60)
        snprintf(out, n, "~%dh%02d", rounded / 60, rounded % 60);
    else
        snprintf(out, n, "~%dmin", rounded);
}

int main(void) {
    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);

    bool turn_off = false;

    getDeviceModel();
    display_init();
    display_getResolution();

    SDL_Init(SDL_INIT_VIDEO);
    IMG_Init(IMG_INIT_PNG);
    TTF_Init();

    // Text is Mini Plus only: the Mini cannot report a level while charging.
    TTF_Font *font_perc = NULL, *font_eta = NULL;
    if (DEVICE_ID == MIYOO354) {
        font_perc = TTF_OpenFont(FONT_BOLD, 56);
        font_eta = TTF_OpenFont(FONT_REGULAR, 26);
    }

    window = SDL_CreateWindow("main", 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, SDL_WINDOW_SHOWN);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    screen = SDL_CreateRGBSurface(0, 640, 480, 32, 0, 0, 0, 0);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, screen->w, screen->h);

    int min_delay = 15;
    int frame_delay = 80;
    int frame_count = 0;

    SDL_Surface *frames[24];
    SDL_Surface *image;

    for (int i = 0; i < 24; i++) {
        char image_path[STR_MAX + 50];
        snprintf(image_path, STR_MAX + 49, "%schargingState%d.png", SYSTEM_RESOURCES, i);
        if ((image = IMG_Load(image_path)))
            frames[frame_count++] = image;
    }

    char json_path[STR_MAX + 20];
    snprintf(json_path, STR_MAX + 19, "%schargingState.json", SYSTEM_RESOURCES);
    if (is_file(json_path)) {
        int value;
        char json_value[STR_MAX];
        if (file_parseKeyValue(json_path, "frame_delay", json_value, ':', 0) != NULL) {
            value = atoi(json_value);
            // accept both microseconds and milliseconds
            frame_delay = value >= 10000 ? value / 1000 : value;
        }
    }

    // Prepare for Poll button input
    input_fd = open("/dev/input/event0", O_RDONLY);
    memset(&fds, 0, sizeof(fds));
    fds[0].fd = input_fd;
    fds[0].events = POLLIN;

    if (frame_delay < min_delay)
        frame_delay = min_delay;

    printf_debug("Frame count: %d\n", frame_count);
    printf_debug("Frame delay: %d ms\n", frame_delay);

    bool power_pressed = false;
    int repeat_power = 0;

    int current_frame = 0;

    // Set the CPU to powersave (charges faster?)
    system_powersave_on();

    uint32_t acc_ticks = 0, last_ticks = SDL_GetTicks(),
            display_timer = last_ticks;
    uint32_t charging_check_timer = last_ticks;
    bool is_charging = true;

    int battery_perc = -1;
    int start_perc = -1;
    int axp_fail_streak = 0;
    uint32_t start_ticks = 0;
    SDL_Surface *perc_text = NULL, *eta_text = NULL;
    char perc_str[16] = "", eta_str[32] = "";

    if (DEVICE_ID == MIYOO354 &&
        mmp_battery_status(&is_charging, &battery_perc) && battery_perc >= 0) {
        start_perc = battery_perc;
        start_ticks = last_ticks;
    }

    while (!quit) {
        while (poll(fds, 1, suspended ? 1000 - min_delay : 0)) {
            read(input_fd, &ev, sizeof(ev));

            if (ev.type != EV_KEY || ev.value > REPEATING)
                continue;

            if (ev.code == HW_BTN_POWER) {
                if (ev.value == PRESSED) {
                    power_pressed = true;
                    repeat_power = 0;
                } else if (ev.value == RELEASED && power_pressed) {
                    if (suspended) {
                        acc_ticks = 0;
                        last_ticks = SDL_GetTicks();
                    }
                    suspend(!suspended);
                    power_pressed = false;
                } else if (ev.value == REPEATING) {
                    if (repeat_power >= 5) {
                        quit = true; // power on
                        break;
                    }
                    repeat_power++;
                }
            }

            display_timer = SDL_GetTicks();
        }

        uint32_t now = SDL_GetTicks();
        if (now - charging_check_timer >= 1000) {
            if (DEVICE_ID == MIYOO354) {
                if (mmp_battery_status(&is_charging, &battery_perc)) {
                    axp_fail_streak = 0;
                    if (start_perc < 0 && battery_perc >= 0) {
                        start_perc = battery_perc;
                        start_ticks = now;
                    }
                }
                // Fail safe: while suspended the idle-timeout shutdown does not
                // run, so a permanently broken axp_test must not keep the loop
                // alive forever. After 30 s of unreadable samples assume the
                // charger is gone, matching the old behavior.
                else if (++axp_fail_streak >= 30) {
                    is_charging = false;
                }
            } else {
                is_charging = battery_isCharging();
            }
            charging_check_timer = now;
        }
        if (!is_charging) {
            quit = true;
            turn_off = true;
            break;
        }

        if (quit)
            break;

        uint32_t ticks = SDL_GetTicks();

        if (!suspended) {
            if (ticks - display_timer >= DISPLAY_TIMEOUT) {
                if (DEVICE_ID == MIYOO354) {
                    quit = true;
                    turn_off = true;
                    break;
                } else {
                    suspend(true);
                    continue;
                }
            }

            acc_ticks += ticks - last_ticks;
            last_ticks = ticks;

            if (acc_ticks >= frame_delay) {
                // Clear screen
                SDL_FillRect(screen, NULL, 0);

                if (current_frame < frame_count) {
                    SDL_Surface *frame = frames[current_frame];
                    SDL_Rect frame_rect = {320 - frame->w / 2,
                                           240 - frame->h / 2};
                    SDL_BlitSurface(frame, NULL, screen, &frame_rect);
                    current_frame = (current_frame + 1) % frame_count;
                }

                // Live battery level + time-to-full over the animation. Text
                // surfaces are re-rendered only when their string changes.
                if (battery_perc >= 0 && font_perc != NULL) {
                    char s[16];
                    snprintf(s, sizeof(s), "%d%%", battery_perc);
                    if (strcmp(s, perc_str) != 0) {
                        strcpy(perc_str, s);
                        if (perc_text != NULL)
                            SDL_FreeSurface(perc_text);
                        SDL_Color white = {255, 255, 255, 255};
                        perc_text = TTF_RenderUTF8_Blended(font_perc, perc_str, white);
                    }

                    char e[32] = "";
                    format_eta(e, sizeof(e),
                               eta_minutes(battery_perc, start_perc, start_ticks, ticks));
                    if (strcmp(e, eta_str) != 0) {
                        strcpy(eta_str, e);
                        if (eta_text != NULL) {
                            SDL_FreeSurface(eta_text);
                            eta_text = NULL;
                        }
                        if (e[0] != '\0' && font_eta != NULL) {
                            SDL_Color gray = {170, 170, 180, 255};
                            eta_text = TTF_RenderUTF8_Blended(font_eta, eta_str, gray);
                        }
                    }
                }
                if (perc_text != NULL) {
                    SDL_Rect r = {320 - perc_text->w / 2, 366};
                    SDL_BlitSurface(perc_text, NULL, screen, &r);
                }
                if (eta_text != NULL) {
                    SDL_Rect r = {320 - eta_text->w / 2, 436};
                    SDL_BlitSurface(eta_text, NULL, screen, &r);
                }

                applyScreen();

                acc_ticks -= frame_delay;
            }
        }

        msleep(min_delay);
    }

#ifndef PLATFORM_MIYOOMINI
    msleep(100);
#endif

    for (int i = 0; i < frame_count; i++)
        SDL_FreeSurface(frames[i]);
    if (perc_text != NULL)
        SDL_FreeSurface(perc_text);
    if (eta_text != NULL)
        SDL_FreeSurface(eta_text);
    if (font_perc != NULL)
        TTF_CloseFont(font_perc);
    if (font_eta != NULL)
        TTF_CloseFont(font_eta);
    TTF_Quit();
    SDL_FreeSurface(screen);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    if (turn_off) {
#ifdef PLATFORM_MIYOOMINI
        display_setScreen(false);
        system("shutdown; sleep 10");
#endif
    } else {
#ifdef PLATFORM_MIYOOMINI
        display_setScreen(true);
        short_pulse();
#endif
    }

    // restore CPU performance mode
    system_powersave_off();
    display_free();

    return EXIT_SUCCESS;
}
