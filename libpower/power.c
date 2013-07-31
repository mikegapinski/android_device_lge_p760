/*
 * Copyright (C) 2012 The Android Open Source Project
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
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define LOG_TAG "P760 PowerHAL"
#include <utils/Log.h>

#include <hardware/hardware.h>
#include <hardware/power.h>

#define SCALINGMAXFREQ_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq"
#define BOOSTPULSE_PATH "/sys/devices/system/cpu/cpufreq/interactive/boostpulse"

#define TIMER_RATE_SCREEN_ON "20000"
#define TIMER_RATE_SCREEN_OFF "500000"

#define MAX_BUF_SZ  10

/* initialize freqs*/
static char screen_off_max_freq[MAX_BUF_SZ] = "600000";
static char scaling_max_freq[MAX_BUF_SZ] = "1008000";

struct p760_power_module {
    struct power_module base;
    pthread_mutex_t lock;
    int boostpulse_fd;
    int boostpulse_warned;
};

static void sysfs_write(char *path, char *s)
{
    char buf[80];
    int len;
    int fd = open(path, O_WRONLY);

    if (fd < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error opening %s: %s\n", path, buf);
        return;
    }

    len = write(fd, s, strlen(s));
    if (len < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error writing to %s: %s\n", path, buf);
    }

    close(fd);
}

int sysfs_read(const char *path, char *buf, size_t size)
{
  int fd, len;

  fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;

  do {
    len = read(fd, buf, size);
  } while (len < 0 && errno == EINTR);

  close(fd);

  return len;
}

static void p760_power_init(struct power_module *module)
{
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/timer_rate",
                TIMER_RATE_SCREEN_ON);
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/min_sample_time",
                "60000");
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/hispeed_freq",
                "600000");
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/target_loads",
                "70 800000:80 1008000:90");
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/go_hispeed_load",
                "90");
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/above_hispeed_delay",
                "80000");
}

static int boostpulse_open(struct p760_power_module *p760)
{
    char buf[80];

    pthread_mutex_lock(&p760->lock);

    if (p760->boostpulse_fd < 0) {
        p760->boostpulse_fd = open(BOOSTPULSE_PATH, O_WRONLY);

        if (p760->boostpulse_fd < 0) {
            if (!p760->boostpulse_warned) {
                strerror_r(errno, buf, sizeof(buf));
                ALOGE("Error opening %s: %s\n", BOOSTPULSE_PATH, buf);
                p760->boostpulse_warned = 1;
            }
        }
    }

    pthread_mutex_unlock(&p760->lock);
    return p760->boostpulse_fd;
}

static void p760_power_set_interactive(struct power_module *module, int on)
{
    int len;

    char buf[MAX_BUF_SZ];

    /*
     * Lower maximum frequency when screen is off.  CPU 0 and 1 share a
     * cpufreq policy.
     */
    if (!on) {
        /* read the current scaling max freq and save it before updating */
        len = sysfs_read(SCALINGMAXFREQ_PATH, buf, sizeof(buf));

        if (len != -1)
            memcpy(scaling_max_freq, buf, sizeof(buf));
    }

    sysfs_write(SCALINGMAXFREQ_PATH,
                on ? scaling_max_freq : screen_off_max_freq);
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/timer_rate",
                on ? TIMER_RATE_SCREEN_ON : TIMER_RATE_SCREEN_OFF);
}

static void p760_power_hint(struct power_module *module, power_hint_t hint,
                            void *data)
{
    struct p760_power_module *p760 = (struct p760_power_module *) module;
    char buf[80];
    int len;
    int duration = 1;

    switch (hint) {
    case POWER_HINT_INTERACTION:
    case POWER_HINT_CPU_BOOST:
        if (data != NULL)
            duration = (int) data;

        if (boostpulse_open(p760) >= 0) {
            snprintf(buf, sizeof(buf), "%d", duration);
            len = write(p760->boostpulse_fd, buf, strlen(buf));

            if (len < 0) {
                strerror_r(errno, buf, sizeof(buf));
                ALOGE("Error writing to %s: %s\n", BOOSTPULSE_PATH, buf);
            }
        }
        break;

    case POWER_HINT_VSYNC:
        break;

    default:
        break;
    }
}

static struct hw_module_methods_t power_module_methods = {
    .open = NULL,
};

struct p760_power_module HAL_MODULE_INFO_SYM = {
    base: {
        common: {
            tag: HARDWARE_MODULE_TAG,
            module_api_version: POWER_MODULE_API_VERSION_0_2,
            hal_api_version: HARDWARE_HAL_API_VERSION,
            id: POWER_HARDWARE_MODULE_ID,
            name: "p760 Power HAL",
            author: "The Android Open Source Project",
            methods: &power_module_methods,
        },

       init: p760_power_init,
       setInteractive: p760_power_set_interactive,
       powerHint: p760_power_hint,
    },

    lock: PTHREAD_MUTEX_INITIALIZER,
    boostpulse_fd: -1,
    boostpulse_warned: 0,
};
