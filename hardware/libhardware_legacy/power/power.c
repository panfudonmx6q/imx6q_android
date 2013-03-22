/*
 * Copyright (C) 2008 The Android Open Source Project
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
#include <hardware_legacy/power.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>

#define LOG_TAG "power"
#include <utils/Log.h>

#include "qemu.h"
#ifdef QEMU_POWER
#include "power_qemu.h"
#endif

#ifdef CHECK_MX5X_HARDWARE
#include "mxc_ipu_hl_lib.h"
#define MAX_WAIT_HARDWARE_TIME (3000000) /*in us*/
#endif

enum {
    ACQUIRE_PARTIAL_WAKE_LOCK = 0,
    RELEASE_WAKE_LOCK,
    REQUEST_STATE,
    OUR_FD_COUNT
};

const char * const OLD_PATHS[] = {
    "/sys/android_power/acquire_partial_wake_lock",
    "/sys/android_power/release_wake_lock",
    "/sys/android_power/request_state"
};

const char * const NEW_PATHS[] = {
    "/sys/power/wake_lock",
    "/sys/power/wake_unlock",
    "/sys/power/state"
};

const char * const V4L_STREAM_STATUS[] = {
	"/sys/class/video4linux/video16/fsl_v4l2_output_property",
//	"/sys/class/video4linux/video0/fsl_v4l2_capture_property",
	"/sys/class/video4linux/video0/fsl_v4l2_overlay_property"
};

const char * const DVFS_CORE_EN_PATH = "/sys/devices/platform/imx_dvfscore.0/enable";
const char * const BUSFREQ_EN_PATH = "/sys/devices/platform/imx_busfreq.0/enable";

const char * const AUTO_OFF_TIMEOUT_DEV = "/sys/android_power/auto_off_timeout";

//XXX static pthread_once_t g_initialized = THREAD_ONCE_INIT;
static int g_initialized = 0;
static int g_fds[OUR_FD_COUNT];
static int g_error = 1;

static const char *off_state = "mem";
static const char *on_state = "on";
static const char *eink_state = "standby";

static int64_t systemTime()
{
    struct timespec t;
    t.tv_sec = t.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec*1000000000LL + t.tv_nsec;
}

static int
open_file_descriptors(const char * const paths[])
{
    int i;
    for (i=0; i<OUR_FD_COUNT; i++) {
        int fd = open(paths[i], O_RDWR);
        if (fd < 0) {
            fprintf(stderr, "fatal error opening \"%s\"\n", paths[i]);
            g_error = errno;
            return -1;
        }
        g_fds[i] = fd;
    }

    g_error = 0;
    return 0;
}

static inline void
initialize_fds(void)
{
    // XXX: should be this:
    //pthread_once(&g_initialized, open_file_descriptors);
    // XXX: not this:
    if (g_initialized == 0) {
        if(open_file_descriptors(NEW_PATHS) < 0) {
            open_file_descriptors(OLD_PATHS);
            on_state = "wake";
            off_state = "standby";
        }
        g_initialized = 1;
    }
}

int
acquire_wake_lock(int lock, const char* id)
{
    initialize_fds();

//    LOGI("acquire_wake_lock lock=%d id='%s'\n", lock, id);

    if (g_error) return g_error;

    int fd;

    if (lock == PARTIAL_WAKE_LOCK) {
        fd = g_fds[ACQUIRE_PARTIAL_WAKE_LOCK];
    }
    else {
        return EINVAL;
    }

    return write(fd, id, strlen(id));
}

int
release_wake_lock(const char* id)
{
    initialize_fds();

//    LOGI("release_wake_lock id='%s'\n", id);

    if (g_error) return g_error;

    ssize_t len = write(g_fds[RELEASE_WAKE_LOCK], id, strlen(id));
    return len >= 0;
}

int
set_last_user_activity_timeout(int64_t delay)
{
//    LOGI("set_last_user_activity_timeout delay=%d\n", ((int)(delay)));

    int fd = open(AUTO_OFF_TIMEOUT_DEV, O_RDWR);
    if (fd >= 0) {
        char buf[32];
        ssize_t len;
        len = snprintf(buf, sizeof(buf), "%d", ((int)(delay)));
        buf[sizeof(buf) - 1] = '\0';
        len = write(fd, buf, len);
        close(fd);
        return 0;
    } else {
        return errno;
    }
}

#ifdef CHECK_MX5X_HARDWARE
int is_safe_suspend()
{
    int i;
	char buf[20];
	int readinbytes;
	ipu_lib_ctl_task_t task;

    for (i = 0; i < MAX_TASK_NUM; i++) {
		task.index = i;
		mxc_ipu_lib_task_control(IPU_CTL_TASK_QUERY, (void *)(&task), NULL);
		if (task.task_pid) {
			LOGI("task %d:", i);
			LOGI("pid: %d", task.task_pid);
			LOGI("mode:");
			if (task.task_mode & IC_ENC)
				LOGI("\t\tIC_ENC");
			if (task.task_mode & IC_VF)
				LOGI("\t\tIC_VF");
			if (task.task_mode & IC_PP)
				LOGI("\t\tIC_PP");
			if (task.task_mode & ROT_ENC)
				LOGI("\t\tROT_ENC");
			if (task.task_mode & ROT_VF)
				LOGI("\t\tROT_VF");
			if (task.task_mode & ROT_PP)
				LOGI("\t\tROT_PP");
			if (task.task_mode & VDI_IC_VF)
				LOGI("\t\tVDI_IC_VF");

            /*Not safe too suspend*/
            return 0;
		}
	}

	for (i = 0; i < (sizeof(V4L_STREAM_STATUS) / sizeof(V4L_STREAM_STATUS[0])); i++)
	{
		FILE *fp = fopen(V4L_STREAM_STATUS[i], "r");
		if (fp != NULL)
		{
			memset(buf, 0, 20);
			readinbytes = fread(buf, 1, 20, fp);
			if (readinbytes <= 20)
				buf[readinbytes] = '\0';
			else
				return 0;

			LOGV("for the port %s, the stream state is %s", V4L_STREAM_STATUS[i], buf );
			if (NULL == strstr(buf, "off"))
			{
				LOGD("@@the V4L output is still on@@");
				return 0;
			}
			fclose(fp);
		}
	}


    return 1;
}

void *set_state_off_sync(void *arg)
{
    char buf[32];
    int len;
    int wait_time = 0;
    LOGI("*****set_state_off_sync****");
    do {
        if (is_safe_suspend()) {
            /*It is safe now*/
            break;
        } else{
            LOGI("...Have to wait...");
            usleep(100000);
            wait_time += 100000;
        }
    } while (wait_time < MAX_WAIT_HARDWARE_TIME);

    len = sprintf(buf, "%s", off_state);
    LOGI("*****do change the sate****");
    len = write(g_fds[REQUEST_STATE], buf, len);
    return NULL;
}

void *set_state_eink_sync(void *arg)
{
    char buf[32];
    int len;
    int wait_time = 0;
    LOGI("*****set_state_off_sync****");
    do {
        if (is_safe_suspend()) {
            /*It is safe now*/
            break;
        } else{
            LOGI("...Have to wait...");
            usleep(100000);
            wait_time += 100000;
        }
    } while (wait_time < MAX_WAIT_HARDWARE_TIME);

    len = sprintf(buf, "%s", eink_state);
    LOGI("*****do change the sate****");
    len = write(g_fds[REQUEST_STATE], buf, len);
    return NULL;
}

#else
int is_safe_suspend()
{
    return 1;
}

void *set_state_off_sync(void *arg)
{
    char buf[32];
    int len;
    len = sprintf(buf, "%s", off_state);
    len = write(g_fds[REQUEST_STATE], buf, len);
    return NULL;
}

void *set_state_eink_sync(void *arg)
{
    char buf[32];
    int len;
    len = sprintf(buf, "%s", eink_state);
    len = write(g_fds[REQUEST_STATE], buf, len);
    return NULL;
}
#endif

int
set_screen_state(int on)
{
    QEMU_FALLBACK(set_screen_state(on));

    LOGI("*** set_screen_state %d", on);

    initialize_fds();

    //LOGI("go_to_sleep eventTime=%lld now=%lld g_error=%s\n", eventTime,
      //      systemTime(), strerror(g_error));

    if (g_error) {
        LOGE("Failed setting last user activity: g_error=%d\n", g_error);
        return 0;
    }

    char buf[32];
    int len;
    if (on == 1) {
        len = sprintf(buf, "%s", on_state);
        len = write(g_fds[REQUEST_STATE], buf, len);
        if (len < 0)
            LOGE("Failed setting last user activity: g_error=%d\n", g_error);
    } else if(on == 0){
        /*Check it is safe to enter suspend*/
        if (is_safe_suspend()) {
            len = sprintf(buf, "%s", off_state);
            len = write(g_fds[REQUEST_STATE], buf, len);
            if (len < 0)
                LOGE("Failed setting last user activity: g_error=%d\n", g_error);
        } else{
           pthread_t threadId;
           pthread_create(&threadId, NULL, set_state_off_sync, NULL);
        }
    }else if(on == 2){
        /*Check it is safe to enter suspend*/
        if (is_safe_suspend()) {
            len = sprintf(buf, "%s", eink_state);
            len = write(g_fds[REQUEST_STATE], buf, len);
            if (len < 0)
                LOGE("Failed setting last user activity: g_error=%d\n", g_error);
        } else{
           pthread_t threadId;
           pthread_create(&threadId, NULL, set_state_eink_sync, NULL);
        }
    }

    return 0;
}

#ifdef CHECK_MX5X_HARDWARE
void
enable_dvfs_core(int on)
{
    int fd, len, i;
    char buf[2];

    len = sprintf(buf, "%s", on? "1":"0");

    /* enable dvfs core */
    fd = open(DVFS_CORE_EN_PATH, O_RDWR);
    if (fd <= 0) {
        LOGE("Failed to open file: %s\n", DVFS_CORE_EN_PATH);
        return;
    }
    write(fd, buf, len);
    close(fd);
    LOGD("DVFS core has been %s!\n", on? "enabled":"disabled");

    /* enable bus freq */
    fd = open(BUSFREQ_EN_PATH, O_RDWR);
    if (fd <= 0) {
        LOGE("Failed to open file: %s\n", BUSFREQ_EN_PATH);
        return;
    }
    write(fd, buf, len);
    close(fd);
    LOGD("Bus Frequency has been %s!\n", on? "enabled":"disabled");

}
#else
void enable_dvfs_core(int on)
{

}
#endif
