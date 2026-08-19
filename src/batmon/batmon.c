#include "batmon.h"
#include "system/device_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[])
{
    getDeviceModel();
    FILE *fp;
    int old_percentage = -1;
    bool was_charging = false;

    atexit(cleanup);
    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);

    while (!quit) {
        int current_percentage = old_percentage;
        bool is_charging = false;
        bool sample_valid = true;

        if (DEVICE_ID == MIYOO354) {
            int voltage = 0, charging = 0;
            sample_valid = getBatStatusMMP(&current_percentage, &voltage, &charging);
            is_charging = sample_valid && charging == 3;
        } else {
            is_charging = battery_isCharging();
            if (is_charging) {
                current_percentage = 500;
                saveFakeAxpResult(current_percentage);
            } else {
                if (was_charging) {
                    // Do not smooth from the synthetic charging value.
                    adc_value_g = updateADCValue(0);
                } else {
                    adc_value_g = updateADCValue(adc_value_g);
                }
                current_percentage = batteryPercentage(adc_value_g);
                saveFakeAxpResult(current_percentage);
            }
        }

        if (sample_valid) {
            printf_debug(
                "battery check: perc = %d, charging = %d\n",
                current_percentage, is_charging);

            if (current_percentage != old_percentage) {
                old_percentage = current_percentage;
                file_put_sync(fp, "/tmp/percBat", "%d", current_percentage);
            }
            was_charging = is_charging;
        }

        // GPIO/ADC sampling is cheap on the Mini. On the Mini Plus, one sample
        // launches the vendor axp_test process, so a minute is ample for an
        // integer battery gauge and avoids 180 extra launches per hour.
        sleep(DEVICE_ID == MIYOO354
                  ? CHECK_BATTERY_TIMEOUT_MMP_S
                  : CHECK_BATTERY_TIMEOUT_MM_S);
    }

    return EXIT_SUCCESS;
}

static void sigHandler(int sig)
{
    switch (sig) {
    case SIGINT:
    case SIGTERM:
        quit = true;
        break;
    default:
        break;
    }
}

void cleanup(void)
{
    remove("/tmp/percBat");
    if (sar_fd >= 0) {
        close(sar_fd);
    }
}

void saveFakeAxpResult(int current_percentage)
{
    FILE *fp;
    if ((fp = fopen("/tmp/.axp_result", "w+"))) {
        fprintf(fp, "{\"battery\":%d, \"voltage\":%d, \"charging\":%d}", current_percentage, adc_value_g, current_percentage == 500 ? 3 : 0);
        fclose(fp);
    }
}

int updateADCValue(int value)
{
    if (battery_isCharging())
        return 100;

    if (sar_fd < 0) {
        sar_fd = open("/dev/sar", O_WRONLY);
        if (sar_fd < 0) {
            return value;
        }
        ioctl(sar_fd, IOCTL_SAR_INIT, NULL);
    }

    static SAR_ADC_CONFIG_READ adcConfig;
    ioctl(sar_fd, IOCTL_SAR_SET_CHANNEL_READ_VALUE, &adcConfig);

    if (value <= 100)
        value = adcConfig.adc_value;
    else if (adcConfig.adc_value > value)
        value++;
    else if (adcConfig.adc_value < value)
        value--;

    return value;
}

bool getBatStatusMMP(int *percentage, int *voltage, int *charging)
{
    char buf[128] = "";
    FILE *command = popen("cd /customer/app/ && ./axp_test", "r");
    if (command == NULL) {
        return false;
    }

    bool valid = fgets(buf, sizeof(buf), command) != NULL &&
                 sscanf(buf, "{\"battery\":%d, \"voltage\":%d, \"charging\":%d}",
                        percentage, voltage, charging) == 3;
    pclose(command);

    if (valid) {
        // Keep the same RAM cache consumed by the UI and optional telemetry,
        // without launching axp_test a second time.
        FILE *fp = fopen("/tmp/.axp_result", "w+");
        if (fp != NULL) {
            fprintf(fp, "{\"battery\":%d, \"voltage\":%d, \"charging\":%d}",
                    *percentage, *voltage, *charging);
            fclose(fp);
        }
    }

    return valid;
}

int batteryPercentage(int value)
{
    if (value == 100)
        return 500;
    if (value >= 578)
        return 100;
    if (value >= 528)
        return value - 478;
    if (value >= 512)
        return (int)(value * 2.125 - 1068);
    if (value >= 480)
        return (int)(value * 0.51613 - 243.742);
    return 0;
}
