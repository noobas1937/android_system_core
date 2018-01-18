/*
 * Copyright (C) 2007-2014 The Android Open Source Project
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
#include <fcntl.h>
#ifdef HAVE_PTHREADS
#include <pthread.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#if (FAKE_LOG_DEVICE == 0)
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <time.h>
#include <unistd.h>

#ifdef __BIONIC__
#include <android/set_abort_message.h>
#endif

#if defined(MOTOROLA_LOG) || defined(MTK_HARDWARE)
#if HAVE_LIBC_SYSTEM_PROPERTIES
#include <sys/system_properties.h>
#endif
#endif

#include <log/logd.h>
#include <log/logger.h>
#include <log/log_read.h>
#include <private/android_filesystem_config.h>

#define LOG_BUF_SIZE 1024

#if FAKE_LOG_DEVICE
/* This will be defined when building for the host. */
#include "fake_log_device.h"
#endif

static int __write_to_log_init(log_id_t, struct iovec *vec, size_t nr);
static int (*write_to_log)(log_id_t, struct iovec *vec, size_t nr) = __write_to_log_init;
#ifdef HAVE_PTHREADS
static pthread_mutex_t log_init_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

#ifndef __unused
#define __unused  __attribute__((__unused__))
#endif

#if FAKE_LOG_DEVICE
static int log_fds[(int)LOG_ID_MAX] = { -1, -1, -1, -1, -1 };
#else
static int logd_fd = -1;
#endif

/*
 * This is used by the C++ code to decide if it should write logs through
 * the C code.  Basically, if /dev/socket/logd is available, we're running in
 * the simulator rather than a desktop tool and want to use the device.
 */
static enum {
    kLogUninitialized, kLogNotAvailable, kLogAvailable
} g_log_status = kLogUninitialized;
int __android_log_dev_available(void)
{
    if (g_log_status == kLogUninitialized) {
        if (access("/dev/socket/logdw", W_OK) == 0)
            g_log_status = kLogAvailable;
        else
            g_log_status = kLogNotAvailable;
    }

    return (g_log_status == kLogAvailable);
}

#ifdef MOTOROLA_LOG
/* Fallback when there is neither log.tag.<tag> nor log.tag.DEFAULT.
 * this is compile-time defaulted to "info". The log startup code
 * looks at the build tags to see about whether it should be DEBUG...
 * -- just as is done in frameworks/base/core/jni/android_util_Log.cpp
 */
static int prio_fallback = ANDROID_LOG_INFO;

/*
 * public interface so native code can see "should i log this"
 * and behave similar to java Log.isLoggable() calls.
 *
 * NB: we have (level,tag) here to match the other __android_log entries.
 * The Java side uses (tag,level) for its ordering.
 * since the args are (int,char*) vs (char*,char*) we won't get strange
 * swapped-the-strings errors.
 */

#define	LOGGING_PREFIX	"log.tag."
#define	LOGGING_DEFAULT	"log.tag.DEFAULT"

int __android_log_loggable(int prio, const char *tag)
{
    int nprio;

#if HAVE_LIBC_SYSTEM_PROPERTIES
    char keybuf[PROP_NAME_MAX];
    char results[PROP_VALUE_MAX];
    int n;

    /* we can NOT cache the log.tag.<tag> and log.tag.DEFAULT
     * values because either one can be changed dynamically.
     *
     * damn, says the performance compulsive.
     */

    n = 0;
    results[0] = '\0';
    if (tag) {
        memcpy (keybuf, LOGGING_PREFIX, strlen (LOGGING_PREFIX) + 1);
        /* watch out for buffer overflow */
        strncpy (keybuf + strlen (LOGGING_PREFIX), tag,
                sizeof (keybuf) - strlen (LOGGING_PREFIX));
        keybuf[sizeof (keybuf) - 1] = '\0';
        n = __system_property_get (keybuf, results);
    }
    if (n == 0) {
        /* nothing yet, look for the global */
        memcpy (keybuf, LOGGING_DEFAULT, sizeof (LOGGING_DEFAULT));
        n = __system_property_get (keybuf, results);
    }

    if (n == 0) {
        nprio = prio_fallback;
    } else {
        switch (results[0]) {
            case 'E':
                nprio = ANDROID_LOG_ERROR;
                break;
            case 'W':
                nprio = ANDROID_LOG_WARN;
                break;
            case 'I':
                nprio = ANDROID_LOG_INFO;
                break;
            case 'D':
                nprio = ANDROID_LOG_DEBUG;
                break;
            case 'V':
                nprio = ANDROID_LOG_VERBOSE;
                break;
            case 'S':
                nprio = ANDROID_LOG_SILENT;
                break;
            default:
                /* unspecified or invalid */
                nprio = prio_fallback;
                break;
        }
    }
#else
    /* no system property routines, fallback to a default */
    nprio = prio_fallback;
#endif

    return ((prio >= nprio) ? 1 : 0);
}
#endif

#if !FAKE_LOG_DEVICE
/* give up, resources too limited */
static int __write_to_log_null(log_id_t log_fd __unused, struct iovec *vec __unused,
                               size_t nr __unused)
{
    return -1;
}
#endif

/* log_init_lock assumed */
static int __write_to_log_initialize()
{
    int i, ret = 0;

#if FAKE_LOG_DEVICE
    for (i = 0; i < LOG_ID_MAX; i++) {
        char buf[sizeof("/dev/log_system")];
        snprintf(buf, sizeof(buf), "/dev/log_%s", android_log_id_to_name(i));
        log_fds[i] = fakeLogOpen(buf, O_WRONLY);
    }
#else
    if (logd_fd >= 0) {
        i = logd_fd;
        logd_fd = -1;
        close(i);
    }

    i = socket(PF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (i < 0) {
        ret = -errno;
        write_to_log = __write_to_log_null;
    } else if (fcntl(i, F_SETFL, O_NONBLOCK) < 0) {
        ret = -errno;
        close(i);
        i = -1;
        write_to_log = __write_to_log_null;
    } else {
        struct sockaddr_un un;
        memset(&un, 0, sizeof(struct sockaddr_un));
        un.sun_family = AF_UNIX;
        strcpy(un.sun_path, "/dev/socket/logdw");

        if (connect(i, (struct sockaddr *)&un, sizeof(struct sockaddr_un)) < 0) {
            ret = -errno;
            close(i);
            i = -1;
        }
    }
    logd_fd = i;
#endif

    return ret;
}

static int __write_to_log_kernel(log_id_t log_id, struct iovec *vec, size_t nr)
{
    ssize_t ret;
#if FAKE_LOG_DEVICE
    int log_fd;

    if (/*(int)log_id >= 0 &&*/ (int)log_id < (int)LOG_ID_MAX) {
        log_fd = log_fds[(int)log_id];
    } else {
        return -EBADF;
    }
    do {
        ret = fakeLogWritev(log_fd, vec, nr);
        if (ret < 0) {
            ret = -errno;
        }
    } while (ret == -EINTR);
#else
    static const unsigned header_length = 3;
    struct iovec newVec[nr + header_length];
    typeof_log_id_t log_id_buf;
    uint16_t tid;
    struct timespec ts;
    log_time realtime_ts;
    size_t i, payload_size;
    static uid_t last_uid = AID_ROOT; /* logd *always* starts up as AID_ROOT */

    if (last_uid == AID_ROOT) { /* have we called to get the UID yet? */
        last_uid = getuid();
    }
    if (last_uid == AID_LOGD) { /* logd, after initialization and priv drop */
        /*
         * ignore log messages we send to ourself (logd).
         * Such log messages are often generated by libraries we depend on
         * which use standard Android logging.
         */
        return 0;
    }

    if (logd_fd < 0) {
        return -EBADF;
    }

    /*
     *  struct {
     *      // what we provide
     *      typeof_log_id_t  log_id;
     *      u16              tid;
     *      log_time         realtime;
     *      // caller provides
     *      union {
     *          struct {
     *              char     prio;
     *              char     payload[];
     *          } string;
     *          struct {
     *              uint32_t tag
     *              char     payload[];
     *          } binary;
     *      };
     *  };
     */

    clock_gettime(CLOCK_REALTIME, &ts);
    realtime_ts.tv_sec = ts.tv_sec;
    realtime_ts.tv_nsec = ts.tv_nsec;

    log_id_buf = log_id;
    tid = gettid();

    newVec[0].iov_base   = (unsigned char *) &log_id_buf;
    newVec[0].iov_len    = sizeof_log_id_t;
    newVec[1].iov_base   = (unsigned char *) &tid;
    newVec[1].iov_len    = sizeof(tid);
    newVec[2].iov_base   = (unsigned char *) &realtime_ts;
    newVec[2].iov_len    = sizeof(log_time);

    for (payload_size = 0, i = header_length; i < nr + header_length; i++) {
        newVec[i].iov_base = vec[i - header_length].iov_base;
        payload_size += newVec[i].iov_len = vec[i - header_length].iov_len;

        if (payload_size > LOGGER_ENTRY_MAX_PAYLOAD) {
            newVec[i].iov_len -= payload_size - LOGGER_ENTRY_MAX_PAYLOAD;
            if (newVec[i].iov_len) {
                ++i;
            }
            break;
        }
    }

    /*
     * The write below could be lost, but will never block.
     *
     * ENOTCONN occurs if logd dies.
     * EAGAIN occurs if logd is overloaded.
     */
    ret = writev(logd_fd, newVec, i);
    if (ret < 0) {
        ret = -errno;
        if (ret == -ENOTCONN) {
#ifdef HAVE_PTHREADS
            pthread_mutex_lock(&log_init_lock);
#endif
            ret = __write_to_log_initialize();
#ifdef HAVE_PTHREADS
            pthread_mutex_unlock(&log_init_lock);
#endif

            if (ret < 0) {
                return ret;
            }

            ret = writev(logd_fd, newVec, nr + header_length);
            if (ret < 0) {
                ret = -errno;
            }
        }
    }

    if (ret > (ssize_t)(sizeof_log_id_t + sizeof(tid) + sizeof(log_time))) {
        ret -= sizeof_log_id_t + sizeof(tid) + sizeof(log_time);
    }
#endif

    return ret;
}

#if FAKE_LOG_DEVICE
static const char *LOG_NAME[LOG_ID_MAX] = {
    [LOG_ID_MAIN] = "main",
    [LOG_ID_RADIO] = "radio",
    [LOG_ID_EVENTS] = "events",
    [LOG_ID_SYSTEM] = "system",
    [LOG_ID_CRASH] = "crash"
};

const char *android_log_id_to_name(log_id_t log_id)
{
    if (log_id >= LOG_ID_MAX) {
        log_id = LOG_ID_MAIN;
    }
    return LOG_NAME[log_id];
}
#endif

/*
 * Release any logger resources. A new log write will immediately re-acquire.
 */
void __android_log_close()
{
#if FAKE_LOG_DEVICE
    int i;
#endif

#ifdef HAVE_PTHREADS
    pthread_mutex_lock(&log_init_lock);
#endif

    write_to_log = __write_to_log_init;

    /*
     * Threads that are actively writing at this point are not held back
     * by a lock and are at risk of dropping the messages with a return code
     * -EBADF. Prefer to return error code than add the overhead of a lock to
     * each log writing call to guarantee delivery. In addition, anyone
     * calling this is doing so to release the logging resources and shut down,
     * for them to do so with outstanding log requests in other threads is a
     * disengenuous use of this function.
     */
#if FAKE_LOG_DEVICE
    for (i = 0; i < LOG_ID_MAX; i++) {
        fakeLogClose(log_fds[i]);
        log_fds[i] = -1;
    }
#else
    close(logd_fd);
    logd_fd = -1;
#endif

#ifdef HAVE_PTHREADS
    pthread_mutex_unlock(&log_init_lock);
#endif
}

static int __write_to_log_init(log_id_t log_id, struct iovec *vec, size_t nr)
{
#ifdef HAVE_PTHREADS
    pthread_mutex_lock(&log_init_lock);
#endif

    if (write_to_log == __write_to_log_init) {
        int ret;

        ret = __write_to_log_initialize();
        if (ret < 0) {
#ifdef HAVE_PTHREADS
            pthread_mutex_unlock(&log_init_lock);
#endif
            return ret;
        }

        write_to_log = __write_to_log_kernel;
    }

#ifdef HAVE_PTHREADS
    pthread_mutex_unlock(&log_init_lock);
#endif

    return write_to_log(log_id, vec, nr);
}

#ifdef AMAZON_LOG
int lab126_log_write(int bufID, int prio, const char *tag, const char *fmt, ...)
{
	va_list ap;
	char buf[LOG_BUF_SIZE];
	int _a = bufID;
	int _b = prio;

	// skip flooding logs
	if (!tag)
	{
		tag = "";
	}
	if( strncmp(tag, "Sensors", 7) == 0
		||  strncmp(tag, "qcom_se", 7) == 0 )
	{
		return 0;
	}
	// skip flooding logs

	va_start(ap, fmt);
	vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);
	va_end(ap);

	char new_tag[128];
	snprintf(new_tag, sizeof(new_tag), "AMZ-%s", tag);

	return __android_log_buf_write(LOG_ID_MAIN, ANDROID_LOG_DEBUG, new_tag, buf);
}

int __vitals_log_print(int bufID, int prio, const char *tag, const char *fmt, ...)
{
	va_list ap;
	char buf[LOG_BUF_SIZE];
	int _a = bufID;
	int _b = prio;

	va_start(ap, fmt);
	va_end(ap);

	return __android_log_write(ANDROID_LOG_DEBUG, tag, "__vitals_log_print not implemented");
}
#endif

int __android_log_write(int prio, const char *tag, const char *msg)
{
    struct iovec vec[3];
    log_id_t log_id = LOG_ID_MAIN;
    char tmp_tag[32];

    if (!tag)
        tag = "";

    /* XXX: This needs to go! */
    if (!strcmp(tag, "HTC_RIL") ||
        !strncmp(tag, "RIL", 3) || /* Any log tag with "RIL" as the prefix */
        !strncmp(tag, "IMS", 3) || /* Any log tag with "IMS" as the prefix */
        !strcmp(tag, "AT") ||
        !strcmp(tag, "GSM") ||
        !strcmp(tag, "STK") ||
        !strcmp(tag, "CDMA") ||
        !strcmp(tag, "PHONE") ||
        !strcmp(tag, "SMS")) {
            log_id = LOG_ID_RADIO;
            /* Inform third party apps/ril/radio.. to use Rlog or RLOG */
            snprintf(tmp_tag, sizeof(tmp_tag), "use-Rlog/RLOG-%s", tag);
            tag = tmp_tag;
    }

#if __BIONIC__
    if (prio == ANDROID_LOG_FATAL) {
        android_set_abort_message(msg);
    }
#endif

    vec[0].iov_base   = (unsigned char *) &prio;
    vec[0].iov_len    = 1;
    vec[1].iov_base   = (void *) tag;
    vec[1].iov_len    = strlen(tag) + 1;
    vec[2].iov_base   = (void *) msg;
    vec[2].iov_len    = strlen(msg) + 1;

    return write_to_log(log_id, vec, 3);
}

int __android_log_buf_write(int bufID, int prio, const char *tag, const char *msg)
{
    struct iovec vec[3];
    char tmp_tag[32];

    if (!tag)
        tag = "";

    /* XXX: This needs to go! */
    if ((bufID != LOG_ID_RADIO) &&
         (!strcmp(tag, "HTC_RIL") ||
        !strncmp(tag, "RIL", 3) || /* Any log tag with "RIL" as the prefix */
        !strncmp(tag, "IMS", 3) || /* Any log tag with "IMS" as the prefix */
        !strcmp(tag, "AT") ||
        !strcmp(tag, "GSM") ||
        !strcmp(tag, "STK") ||
        !strcmp(tag, "CDMA") ||
        !strcmp(tag, "PHONE") ||
        !strcmp(tag, "SMS"))) {
            bufID = LOG_ID_RADIO;
            /* Inform third party apps/ril/radio.. to use Rlog or RLOG */
            snprintf(tmp_tag, sizeof(tmp_tag), "use-Rlog/RLOG-%s", tag);
            tag = tmp_tag;
    }

    vec[0].iov_base   = (unsigned char *) &prio;
    vec[0].iov_len    = 1;
    vec[1].iov_base   = (void *) tag;
    vec[1].iov_len    = strlen(tag) + 1;
    vec[2].iov_base   = (void *) msg;
    vec[2].iov_len    = strlen(msg) + 1;

    return write_to_log(bufID, vec, 3);
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
{
    char buf[LOG_BUF_SIZE];

    vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);

    return __android_log_write(prio, tag, buf);
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    char buf[LOG_BUF_SIZE];

    va_start(ap, fmt);
    vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);
    va_end(ap);

    return __android_log_write(prio, tag, buf);
}

int __android_log_buf_print(int bufID, int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    char buf[LOG_BUF_SIZE];

    va_start(ap, fmt);
    vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);
    va_end(ap);

    return __android_log_buf_write(bufID, prio, tag, buf);
}

void __android_log_assert(const char *cond, const char *tag,
                          const char *fmt, ...)
{
    char buf[LOG_BUF_SIZE];

    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);
        va_end(ap);
    } else {
        /* Msg not provided, log condition.  N.B. Do not use cond directly as
         * format string as it could contain spurious '%' syntax (e.g.
         * "%d" in "blocks%devs == 0").
         */
        if (cond)
            snprintf(buf, LOG_BUF_SIZE, "Assertion failed: %s", cond);
        else
            strcpy(buf, "Unspecified assertion failed");
    }

    __android_log_write(ANDROID_LOG_FATAL, tag, buf);
    __builtin_trap(); /* trap so we have a chance to debug the situation */
    /* NOTREACHED */
}

int __android_log_bwrite(int32_t tag, const void *payload, size_t len)
{
    struct iovec vec[2];

    vec[0].iov_base = &tag;
    vec[0].iov_len = sizeof(tag);
    vec[1].iov_base = (void*)payload;
    vec[1].iov_len = len;

    return write_to_log(LOG_ID_EVENTS, vec, 2);
}

/*
 * Like __android_log_bwrite, but takes the type as well.  Doesn't work
 * for the general case where we're generating lists of stuff, but very
 * handy if we just want to dump an integer into the log.
 */
int __android_log_btwrite(int32_t tag, char type, const void *payload,
                          size_t len)
{
    struct iovec vec[3];

    vec[0].iov_base = &tag;
    vec[0].iov_len = sizeof(tag);
    vec[1].iov_base = &type;
    vec[1].iov_len = sizeof(type);
    vec[2].iov_base = (void*)payload;
    vec[2].iov_len = len;

    return write_to_log(LOG_ID_EVENTS, vec, 3);
}

/*
 * Like __android_log_bwrite, but used for writing strings to the
 * event log.
 */
int __android_log_bswrite(int32_t tag, const char *payload)
{
    struct iovec vec[4];
    char type = EVENT_TYPE_STRING;
    uint32_t len = strlen(payload);

    vec[0].iov_base = &tag;
    vec[0].iov_len = sizeof(tag);
    vec[1].iov_base = &type;
    vec[1].iov_len = sizeof(type);
    vec[2].iov_base = &len;
    vec[2].iov_len = sizeof(len);
    vec[3].iov_base = (void*)payload;
    vec[3].iov_len = len;

    return write_to_log(LOG_ID_EVENTS, vec, 4);
}

#ifdef MTK_HARDWARE
struct xlog_record {
    const char *tag_str;
    const char *fmt_str;
    int prio;
};

void __attribute__((weak)) __xlog_buf_printf(int bufid, const struct xlog_record *xlog_record, ...) {
    va_list args;
    va_start(args, xlog_record);
#if    HAVE_LIBC_SYSTEM_PROPERTIES
    int len = 0;
    int do_xlog = 0;
    char results[PROP_VALUE_MAX];


    // MobileLog
    len = __system_property_get ("debug.MB.running", results);
    if (len && atoi(results))
        do_xlog = 1;

    // ModemLog
    len = __system_property_get ("debug.mdlogger.Running", results);
    if (len && atoi(results))
        do_xlog = 1;

    // Manual
    len = __system_property_get ("persist.debug.xlog.enable", results);
    if (len && atoi(results))
        do_xlog = 1;

    if (do_xlog > 0)
#endif
        __android_log_vprint(xlog_record->prio, xlog_record->tag_str, xlog_record->fmt_str, args);

    // get rid of "unused parameter 'bufid'"
    bufid = bufid;
    return;
}
#endif