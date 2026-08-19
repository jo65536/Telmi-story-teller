#ifndef DISPLAY_H__
#define DISPLAY_H__

#include <linux/fb.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/mman.h>

#include "system.h"
#include "utils/file.h"
#include "utils/log.h"

#define display_on() display_setScreen(true)
#define display_off() display_setScreen(false)

static int DISPLAY_WIDTH = 640;
static int DISPLAY_HEIGHT = 480;

static uint32_t *fb_addr;
static int fb_fd;
static uint8_t *fbofs;
static struct fb_fix_screeninfo finfo;
static struct fb_var_screeninfo vinfo;
static uint32_t stride, bpp;
static uint8_t *savebuf;
static bool display_enabled = true;

static void display_writeState(void) {
    FILE *state = fopen("/tmp/telmi-display-state", "w");
    if (state != NULL) {
        fputc(display_enabled ? '1' : '0', state);
        fclose(state);
    }
}

void display_init(void) {
    // Open and mmap FB
    fb_fd = open("/dev/fb0", O_RDWR);

    ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo);
    fb_addr = (uint32_t *) mmap(0, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);

    ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
    DISPLAY_WIDTH = vinfo.xres;
    DISPLAY_HEIGHT = vinfo.yres;
    display_writeState();
}

//
//    Get physical screen resolution
//
void display_getResolution(void) {
    FILE *file = fopen("/tmp/screen_resolution", "r");
    if (file == NULL) {
        return;
    }
    fscanf(file, "%dx%d", &DISPLAY_WIDTH, &DISPLAY_HEIGHT);
    fclose(file);
}

//
//    Save/Clear Display area
//
void display_save(void) {
    // Already saved: the framebuffer has been cleared since, saving it again
    // would leak the previous buffer and overwrite the content to restore.
    if (savebuf) {
        return;
    }

    stride = finfo.line_length;
    ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
    bpp = vinfo.bits_per_pixel / 8; // byte per pixel
    fbofs = (uint8_t *) fb_addr + (vinfo.yoffset * stride);

    // Save display area and clear
    if ((savebuf = (uint8_t *) malloc(DISPLAY_WIDTH * bpp * DISPLAY_HEIGHT))) {
        uint32_t i, ofss, ofsd;
        ofss = ofsd = 0;
        for (i = DISPLAY_HEIGHT; i > 0;
             i--, ofss += stride, ofsd += DISPLAY_WIDTH * bpp) {
            memcpy(savebuf + ofsd, fbofs + ofss, DISPLAY_WIDTH * bpp);
            memset(fbofs + ofss, 0, DISPLAY_WIDTH * bpp);
        }
    }
}

//
//    Restore Display area
//
void display_restore(void) {
    // Restore display area
    if (savebuf) {
        uint32_t i, ofss, ofsd;
        ofss = ofsd = 0;
        for (i = DISPLAY_HEIGHT; i > 0;
             i--, ofsd += stride, ofss += DISPLAY_WIDTH * bpp) {
            memcpy(fbofs + ofsd, savebuf + ofss, DISPLAY_WIDTH * bpp);
        }
        free(savebuf);
        savebuf = NULL;
    }
}

void display_reset(void) {
    ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
    vinfo.yoffset = 0;
    memset(fb_addr, 0, finfo.smem_len);
    ioctl(fb_fd, FBIOPUT_VSCREENINFO, &vinfo);
}

//
//    Screen On/Off
//
void display_setScreen(bool enabled) {
    // Avoid repeated sysfs operations and framebuffer copies when callers ask
    // for the state that is already active.
    if (display_enabled == enabled) {
        return;
    }

    if (!enabled) {
        display_save();
        // Stop the PWM clock as well as cutting the panel/backlight GPIO.
        // It is explicitly re-enabled on wake below.
        if (!exists(PWM_DIR "pwm0/enable")) {
            file_write(PWM_DIR "export", "0", 1);
        }
        file_write(PWM_DIR "pwm0/enable", "0", 1);
    } else {
        // Restore while the panel and backlight are still off to avoid exposing
        // the cleared framebuffer for a frame during wake-up.
        display_restore();
    }

    // export gpio4, direction: out
    file_write(GPIO_DIR1 "export", "4", 1);
    file_write(GPIO_DIR2 "gpio4/direction", "out", 3);

    // screen on/off
    bool gpio_ok = file_write(GPIO_DIR2 "gpio4/value", enabled ? "1" : "0", 1);

    // unexport gpio4
    file_write(GPIO_DIR1 "unexport", "4", 1);

    if (enabled) {
        // re-enable brightness control
        if (!exists(PWM_DIR "pwm0/enable")) {
            file_write(PWM_DIR "export", "0", 1);
        }
        file_write(PWM_DIR "pwm0/enable", "0", 1);
        file_write(PWM_DIR "pwm0/enable", "1", 1);
    }

    // The GPIO cuts the panel and the backlight, so it is the source of truth
    // for the visible state. Tracking it (rather than GPIO && PWM) keeps
    // display_enabled in sync with what the user sees: a failed PWM write can
    // waste a little power, but can no longer strand the toggle on a dark
    // panel that the state machine believes is lit.
    if (gpio_ok) {
        display_enabled = enabled;
        display_writeState();
    }
}

void display_toggle(void) { display_setScreen(!display_enabled); }

//
//    Set Brightness (Raw)
//
void display_setBrightnessRaw(uint32_t value) {
    FILE *fp;
    file_put_sync(fp, PWM_DIR "pwm0/duty_cycle", "%u", value);
    printf_debug("Raw brightness: %d\n", value);
}

// Set display brightness (0 - 10)
void display_setBrightness(uint32_t value) {
    // Linear curve
    // int value_raw = (value == 0) ? 3 : (value * 10);

    // Exponential curve
    int value_raw = round(3.0 * exp(0.350656 * value));

    display_setBrightnessRaw(value_raw);
}

//
//    Draw frame, fixed 640x480x32bpp for now
//
void display_drawFrame(uint32_t color) {
    uint32_t *ofs = fb_addr;
    uint32_t i;
    for (i = 0; i < 640; i++) {
        ofs[i] = color;
    }
    ofs += 640 * 479;
    for (i = 0; i < 640 * 2; i++) {
        ofs[i] = color;
    }
    ofs += 640 * 480;
    for (i = 0; i < 640 * 2; i++) {
        ofs[i] = color;
    }
    ofs += 640 * 480;
    for (i = 0; i < 640; i++) {
        ofs[i] = color;
    }
    ofs = fb_addr + 639;
    for (i = 0; i < 480 * 3 - 1; i++, ofs += 640) {
        ofs[0] = color;
        ofs[1] = color;
    }
}

//
//    Draw a battery icon
//
void display_drawBatteryIcon(uint32_t color, int x, int y, int level,
                             uint32_t fillColor) {
    uint32_t *ofs = fb_addr;
    int i, j;

    // Draw battery body wireframe
    for (i = x; i < x + 30; i++) {
        ofs[i + y * 640] = color;        // Top border
        ofs[i + (y + 14) * 640] = color; // Bottom border
    }
    for (j = y; j < y + 15; j++) {
        ofs[x + j * 640] = color;      // Left border
        ofs[x + 29 + j * 640] = color; // Right border
    }

    // Draw battery charge level
    int levelWidth = (level * 26) / 100;
    for (i = x + 3 + 26 - levelWidth; i < x + 1 + 26; i++) {
        for (j = y + 3; j < y + 12; j++) {
            ofs[i + j * 640] = fillColor;
        }
    }

    // Draw battery head wireframe
    for (i = x - 4; i < x; i++) {
        for (j = y + 2; j < y + 13; j++) {
            ofs[i + j * 640] = color;
        }
    }
}

void display_free(void) {
    if (savebuf)
        free(savebuf);
    if (fb_addr)
        munmap(fb_addr, finfo.smem_len);
    if (fb_fd > 0)
        close(fb_fd);
}

#endif // DISPLAY_H__
