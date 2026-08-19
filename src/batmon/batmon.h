#ifndef ADC_H__
#define ADC_H__

#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef PLATFORM_MIYOOMINI
#include "shmvar/shmvar.h"
#endif

#include "system/battery.h"
#include "system/system.h"
#include "utils/file.h"
#include "utils/log.h"

#define CHECK_BATTERY_TIMEOUT_MM_S 15
#define CHECK_BATTERY_TIMEOUT_MMP_S 60

// for reading battery
#define SARADC_IOC_MAGIC 'a'
#define IOCTL_SAR_INIT _IO(SARADC_IOC_MAGIC, 0)
#define IOCTL_SAR_SET_CHANNEL_READ_VALUE _IO(SARADC_IOC_MAGIC, 1)
typedef struct {
    int channel_value;
    int adc_value;
} SAR_ADC_CONFIG_READ;

static volatile sig_atomic_t quit = 0;
static int sar_fd = -1;
static int adc_value_g;

static void sigHandler(int sig);
void cleanup(void);
void saveFakeAxpResult(int current_percentage);
int updateADCValue(int);
bool getBatStatusMMP(int *percentage, int *voltage, int *charging);
int batteryPercentage(int);

#endif // ADC_H__
