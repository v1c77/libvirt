/*
 * logging.c: internal logging and debugging
 *
 * Copyright (C) 2008 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 */

#include <config.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#if HAVE_SYSLOG_H
#include <syslog.h>
#endif

#include "logging.h"
#include "memory.h"
#include "util.h"

#ifdef ENABLE_DEBUG
int debugFlag = 0;

/*
 * Macro used to format the message as a string in virLogMessage
 * and borrowed from libxml2 (also used in virRaiseError)
 */
#define VIR_GET_VAR_STR(msg, str) {				\
    int       size, prev_size = -1;				\
    int       chars;						\
    char      *larger;						\
    va_list   ap;						\
                                                                \
    str = (char *) malloc(150);					\
    if (str != NULL) {						\
                                                                \
    size = 150;							\
                                                                \
    while (1) {							\
        va_start(ap, msg);					\
        chars = vsnprintf(str, size, msg, ap);			\
        va_end(ap);						\
        if ((chars > -1) && (chars < size)) {			\
            if (prev_size == chars) {				\
                break;						\
            } else {						\
                prev_size = chars;				\
            }							\
        }							\
        if (chars > -1)						\
            size += chars + 1;					\
        else							\
            size += 100;					\
        if ((larger = (char *) realloc(str, size)) == NULL) {	\
            break;						\
        }							\
        str = larger;						\
    }}								\
}

/*
 * A logging buffer to keep some history over logs
 */
#define LOG_BUFFER_SIZE 64000

static char virLogBuffer[LOG_BUFFER_SIZE + 1];
static int virLogLen = 0;
static int virLogStart = 0;
static int virLogEnd = 0;

/*
 * Filters are used to refine the rules on what to keep or drop
 * based on a matching pattern (currently a substring)
 */
struct _virLogFilter {
    const char *match;
    int priority;
};
typedef struct _virLogFilter virLogFilter;
typedef virLogFilter *virLogFilterPtr;

static virLogFilterPtr virLogFilters = NULL;
static int virLogNbFilters = 0;

/*
 * Outputs are used to emit the messages retained
 * after filtering, multiple output can be used simultaneously
 */
struct _virLogOutput {
    void *data;
    virLogOutputFunc f;
    virLogCloseFunc c;
    int priority;
};
typedef struct _virLogOutput virLogOutput;
typedef virLogOutput *virLogOutputPtr;

static virLogOutputPtr virLogOutputs = NULL;
static int virLogNbOutputs = 0;

/*
 * Default priorities
 */
static virLogPriority virLogDefaultPriority = VIR_LOG_WARN;

static int virLogResetFilters(void);
static int virLogResetOutputs(void);

/*
 * Logs accesses must be serialized though a mutex
 */
PTHREAD_MUTEX_T(virLogMutex);

static void virLogLock(void)
{
    pthread_mutex_lock(&virLogMutex);
}
static void virLogUnlock(void)
{
    pthread_mutex_unlock(&virLogMutex);
}


static const char *virLogPriorityString(virLogPriority lvl) {
    switch (lvl) {
        case VIR_LOG_DEBUG:
            return("debug");
        case VIR_LOG_INFO:
            return("info");
        case VIR_LOG_WARN:
            return("warning");
        case VIR_LOG_ERROR:
            return("error");
    }
    return("unknown");
}

static int virLogInitialized = 0;

/**
 * virLogStartup:
 *
 * Initialize the logging module
 *
 * Returns 0 if successful, and -1 in case or error
 */
int virLogStartup(void) {
    if (virLogInitialized)
        return(-1);
    virLogInitialized = 1;
    pthread_mutex_init(&virLogMutex, NULL);
    virLogLock();
    virLogLen = 0;
    virLogStart = 0;
    virLogEnd = 0;
    virLogDefaultPriority = VIR_LOG_WARN;
    virLogUnlock();
    return(0);
}

/**
 * virLogReset:
 *
 * Reset the logging module to its default initial state
 *
 * Returns 0 if successful, and -1 in case or error
 */
int virLogReset(void) {
    if (!virLogInitialized)
        return(virLogStartup());

    virLogLock();
    virLogResetFilters();
    virLogResetOutputs();
    virLogLen = 0;
    virLogStart = 0;
    virLogEnd = 0;
    virLogDefaultPriority = VIR_LOG_WARN;
    virLogUnlock();
    return(0);
}
/**
 * virLogShutdown:
 *
 * Shutdown the logging module
 */
void virLogShutdown(void) {
    if (!virLogInitialized)
        return;
    virLogLock();
    virLogResetFilters();
    virLogResetOutputs();
    virLogLen = 0;
    virLogStart = 0;
    virLogEnd = 0;
    virLogUnlock();
    pthread_mutex_destroy(&virLogMutex);
    virLogInitialized = 0;
}

/*
 * Store a string in the ring buffer
 */
static void virLogStr(const char *str, int len) {
    int tmp;

    if (str == NULL)
        return;
    if (len <= 0)
        len = strlen(str);
    if (len > LOG_BUFFER_SIZE)
        return;
    virLogLock();

    /*
     * copy the data and reset the end, we cycle over the end of the buffer
     */
    if (virLogEnd + len >= LOG_BUFFER_SIZE) {
        tmp = LOG_BUFFER_SIZE - virLogEnd;
        memcpy(&virLogBuffer[virLogEnd], str, tmp);
        virLogBuffer[LOG_BUFFER_SIZE] = 0;
        memcpy(&virLogBuffer[0], &str[len], len - tmp);
        virLogEnd = len - tmp;
    } else {
        memcpy(&virLogBuffer[virLogEnd], str, len);
        virLogEnd += len;
    }
    /*
     * Update the log length, and if full move the start index
     */
    virLogLen += len;
    if (virLogLen > LOG_BUFFER_SIZE) {
        tmp = virLogLen - LOG_BUFFER_SIZE;
        virLogLen = LOG_BUFFER_SIZE;
        virLogStart += tmp;
    }
    virLogUnlock();
}

#if 0
/*
 * Output the ring buffer
 */
static int virLogDump(void *data, virLogOutputFunc f) {
    int ret = 0, tmp;

    if ((virLogLen == 0) || (f == NULL))
        return(0);
    virLogLock();
    if (virLogStart + virLogLen < LOG_BUFFER_SIZE) {
push_end:
        virLogBuffer[virLogStart + virLogLen] = 0;
        tmp = f(data, &virLogBuffer[virLogStart], virLogLen);
        if (tmp < 0) {
            ret = -1;
            goto error;
        }
        ret += tmp;
        virLogStart += tmp;
        virLogLen -= tmp;
    } else {
        tmp = LOG_BUFFER_SIZE - virLogStart;
        ret = f(data, &virLogBuffer[virLogStart], tmp);
        if (ret < 0) {
            ret = -1;
            goto error;
        }
        if (ret < tmp) {
            virLogStart += ret;
            virLogLen -= ret;
        } else {
            virLogStart = 0;
            virLogLen -= tmp;
            /* dump the second part */
            if (virLogLen > 0)
                goto push_end;
        }
    }
error:
    virLogUnlock();
    return(ret);
}
#endif

/**
 * virLogSetDefaultPriority:
 * @priority: the default priority level
 *
 * Set the default priority level, i.e. any logged data of a priority
 * equal or superior to this level will be logged, unless a specific rule
 * was defined for the log category of the message.
 *
 * Returns 0 if successful, -1 in case of error.
 */
int virLogSetDefaultPriority(int priority) {
    if ((priority < VIR_LOG_DEBUG) || (priority > VIR_LOG_ERROR))
        return(-1);
    if (!virLogInitialized)
        virLogStartup();
    virLogDefaultPriority = priority;
    return(0);
}

/**
 * virLogResetFilters:
 *
 * Removes the set of logging filters defined.
 *
 * Returns the number of filters removed
 */
static int virLogResetFilters(void) {
    int i;

    for (i = 0; i < virLogNbFilters;i++)
        VIR_FREE(virLogFilters[i].match);
    VIR_FREE(virLogFilters);
    virLogNbFilters = 0;
    return(i);
}

/**
 * virLogDefineFilter:
 * @match: the pattern to match
 * @priority: the priority to give to messages matching the pattern
 * @flags: extra flag, currently unused
 *
 * Defines a pattern used for log filtering, it allow to select or
 * reject messages independently of the default priority.
 * The filter defines a rules that will apply only to messages matching
 * the pattern (currently if @match is a substring of the message category)
 *
 * Returns -1 in case of failure or the filter number if successful
 */
int virLogDefineFilter(const char *match, int priority,
                       int flags ATTRIBUTE_UNUSED) {
    int i;
    char *mdup = NULL;

    if ((match == NULL) || (priority < VIR_LOG_DEBUG) ||
        (priority > VIR_LOG_ERROR))
        return(-1);

    virLogLock();
    for (i = 0;i < virLogNbFilters;i++) {
        if (STREQ(virLogFilters[i].match, match)) {
            virLogFilters[i].priority = priority;
            goto cleanup;
        }
    }

    mdup = strdup(match);
    if (dup == NULL) {
        i = -1;
        goto cleanup;
    }
    i = virLogNbFilters;
    if (VIR_REALLOC_N(virLogFilters, virLogNbFilters + 1)) {
        i = -1;
        VIR_FREE(mdup);
        goto cleanup;
    }
    virLogFilters[i].match = mdup;
    virLogFilters[i].priority = priority;
    virLogNbFilters++;
cleanup:
    virLogUnlock();
    return(i);
}

/**
 * virLogFiltersCheck:
 * @input: the input string
 *
 * Check the input of the message against the existing filters. Currently
 * the match is just a substring check of the category used as the input
 * string, a more subtle approach could be used instead
 *
 * Returns 0 if not matched or the new priority if found.
 */
static int virLogFiltersCheck(const char *input) {
    int ret = 0;
    int i;

    virLogLock();
    for (i = 0;i < virLogNbFilters;i++) {
        if (strstr(input, virLogFilters[i].match)) {
            ret = virLogFilters[i].priority;
            break;
        }
    }
    virLogUnlock();
    return(ret);
}

/**
 * virLogResetOutputs:
 *
 * Removes the set of logging output defined.
 *
 * Returns the number of output removed
 */
static int virLogResetOutputs(void) {
    int i;

    for (i = 0;i < virLogNbOutputs;i++) {
        if (virLogOutputs[i].c != NULL)
            virLogOutputs[i].c(virLogOutputs[i].data);
    }
    VIR_FREE(virLogOutputs);
    i = virLogNbOutputs;
    virLogNbOutputs = 0;
    return(i);
}

/**
 * virLogDefineOutput:
 * @f: the function to call to output a message
 * @f: the function to call to close the output (or NULL)
 * @data: extra data passed as first arg to the function
 * @priority: minimal priority for this filter, use 0 for none
 * @flags: extra flag, currently unused
 *
 * Defines an output function for log messages. Each message once
 * gone though filtering is emitted through each registered output.
 *
 * Returns -1 in case of failure or the output number if successful
 */
int virLogDefineOutput(virLogOutputFunc f, virLogCloseFunc c, void *data,
                       int priority, int flags ATTRIBUTE_UNUSED) {
    int ret = -1;

    if (f == NULL)
        return(-1);

    virLogLock();
    if (VIR_REALLOC_N(virLogOutputs, virLogNbOutputs + 1)) {
        goto cleanup;
    }
    ret = virLogNbOutputs++;
    virLogOutputs[ret].f = f;
    virLogOutputs[ret].c = c;
    virLogOutputs[ret].data = data;
    virLogOutputs[ret].priority = priority;
cleanup:
    virLogUnlock();
    return(ret);
}

/**
 * virLogMessage:
 * @category: where is that message coming from
 * @priority: the priority level
 * @funcname: the function emitting the (debug) message
 * @linenr: line where the message was emitted
 * @flags: extra flags, 1 if coming from the error handler
 * @fmt: the string format
 * @...: the arguments
 *
 * Call the libvirt logger with some informations. Based on the configuration
 * the message may be stored, sent to output or just discarded
 */
void virLogMessage(const char *category, int priority, const char *funcname,
                   long long linenr, int flags, const char *fmt, ...) {
    char *str = NULL;
    char *msg;
    struct timeval cur_time;
    struct tm time_info;
    int len, fprio, i, ret;

    if (!virLogInitialized)
        virLogStartup();

    if (fmt == NULL)
       return;

    /*
     * check against list of specific logging patterns
     */
    fprio = virLogFiltersCheck(category);
    if (fprio == 0) {
        if (priority < virLogDefaultPriority)
            return;
    } else if (priority < fprio)
        return;

    /*
     * serialize the error message, add level and timestamp
     */
    VIR_GET_VAR_STR(fmt, str);
    if (str == NULL)
        return;
    gettimeofday(&cur_time, NULL);
    localtime_r(&cur_time.tv_sec, &time_info);

    if ((funcname != NULL) && (priority == VIR_LOG_DEBUG)) {
        ret = virAsprintf(&msg, "%02d:%02d:%02d.%03d: %s : %s:%lld : %s\n",
                          time_info.tm_hour, time_info.tm_min,
                          time_info.tm_sec, (int) cur_time.tv_usec / 1000,
                          virLogPriorityString(priority), funcname, linenr, str);
    } else {
        ret = virAsprintf(&msg, "%02d:%02d:%02d.%03d: %s : %s\n",
                          time_info.tm_hour, time_info.tm_min,
                          time_info.tm_sec, (int) cur_time.tv_usec / 1000,
                          virLogPriorityString(priority), str);
    }
    VIR_FREE(str);
    if (ret < 0) {
        /* apparently we're running out of memory */
        return;
    }

    /*
     * Log based on defaults, first store in the history buffer
     * then push the message on the outputs defined, if none
     * use stderr.
     * NOTE: the locking is a single point of contention for multiple
     *       threads, but avoid intermixing. Maybe set up locks per output
     *       to improve paralellism.
     */
    len = strlen(msg);
    virLogStr(msg, len);
    virLogLock();
    for (i = 0; i < virLogNbOutputs;i++) {
        if (priority >= virLogOutputs[i].priority)
            virLogOutputs[i].f(category, priority, funcname, linenr,
                               msg, len, virLogOutputs[i].data);
    }
    if ((virLogNbOutputs == 0) && (flags != 1))
        safewrite(2, msg, len);
    virLogUnlock();

    VIR_FREE(msg);
}

static int virLogOutputToFd(const char *category ATTRIBUTE_UNUSED,
                            int priority ATTRIBUTE_UNUSED,
                            const char *funcname ATTRIBUTE_UNUSED,
                            long long linenr ATTRIBUTE_UNUSED,
                            const char *str, int len, void *data) {
    int fd = (long) data;
    int ret;

    if (fd < 0)
        return(-1);
    ret = safewrite(fd, str, len);
    return(ret);
}

static void virLogCloseFd(void *data) {
    int fd = (long) data;

    if (fd >= 0)
        close(fd);
}

static int virLogAddOutputToStderr(int priority) {
    if (virLogDefineOutput(virLogOutputToFd, NULL, (void *)2L, priority, 0) < 0)
        return(-1);
    return(0);
}

static int virLogAddOutputToFile(int priority, const char *file) {
    int fd;

    fd = open(file, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
    if (fd < 0)
        return(-1);
    if (virLogDefineOutput(virLogOutputToFd, virLogCloseFd, (void *)(long)fd,
                           priority, 0) < 0) {
        close(fd);
        return(-1);
    }
    return(0);
}

#if HAVE_SYSLOG_H
static int virLogOutputToSyslog(const char *category ATTRIBUTE_UNUSED,
                                int priority,
                                const char *funcname ATTRIBUTE_UNUSED,
                                long long linenr ATTRIBUTE_UNUSED,
                                const char *str, int len ATTRIBUTE_UNUSED,
                                void *data ATTRIBUTE_UNUSED) {
    int prio;

    switch (priority) {
        case VIR_LOG_DEBUG:
            prio = LOG_DEBUG;
            break;
        case VIR_LOG_INFO:
            prio = LOG_INFO;
            break;
        case VIR_LOG_WARN:
            prio = LOG_WARNING;
            break;
        case VIR_LOG_ERROR:
            prio = LOG_ERR;
            break;
        default:
            prio = LOG_ERR;
    }
    syslog(prio, "%s", str);
    return(len);
}

static void virLogCloseSyslog(void *data ATTRIBUTE_UNUSED) {
    closelog();
}

static int virLogAddOutputToSyslog(int priority, const char *ident) {
    openlog(ident, 0, 0);
    if (virLogDefineOutput(virLogOutputToSyslog, virLogCloseSyslog, NULL,
                           priority, 0) < 0) {
        closelog();
        return(-1);
    }
    return(0);
}
#endif /* HAVE_SYSLOG_H */

#define IS_SPACE(cur)                                                   \
    ((*cur == ' ') || (*cur == '\t') || (*cur == '\n') ||               \
     (*cur == '\r') || (*cur == '\\'))

/**
 * virLogParseOutputs:
 * @outputs: string defining a (set of) output(s)
 *
 * The format for an output can be:
 *    x:stderr
 *       output goes to stderr
 *    x:syslog:name
 *       use syslog for the output and use the given name as the ident
 *    x:file:file_path
 *       output to a file, with the given filepath
 * In all case the x prefix is the minimal level, acting as a filter
 *    0: everything
 *    1: DEBUG
 *    2: INFO
 *    3: WARNING
 *    4: ERROR
 *
 * Multiple output can be defined in a single @output, they just need to be
 * separated by spaces.
 *
 * Returns the number of output parsed and installed or -1 in case of error
 */
int virLogParseOutputs(const char *outputs) {
    const char *cur = outputs, *str;
    char *name;
    int prio;
    int ret = 0;

    if (cur == NULL)
        return(-1);

    virSkipSpaces(&cur);
    while (*cur != 0) {
        prio= virParseNumber(&cur);
        if ((prio < 0) || (prio > 4))
            return(-1);
        if (*cur != ':')
            return(-1);
        cur++;
        if (STREQLEN(cur, "stderr", 6)) {
            cur += 6;
            if (virLogAddOutputToStderr(prio) == 0)
                ret++;
        } else if (STREQLEN(cur, "syslog", 6)) {
            cur += 6;
            if (*cur != ':')
                return(-1);
            cur++;
            str = cur;
            while ((*cur != 0) && (!IS_SPACE(cur)))
                cur++;
            if (str == cur)
                return(-1);
#if HAVE_SYSLOG_H
            name = strndup(str, cur - str);
            if (name == NULL)
                return(-1);
            if (virLogAddOutputToSyslog(prio, name) == 0)
                ret++;
            VIR_FREE(name);
#endif /* HAVE_SYSLOG_H */
        } else if (STREQLEN(cur, "file", 4)) {
            cur += 4;
            if (*cur != ':')
                return(-1);
            cur++;
            str = cur;
            while ((*cur != 0) && (!IS_SPACE(cur)))
                cur++;
            if (str == cur)
                return(-1);
            name = strndup(str, cur - str);
            if (name == NULL)
                return(-1);
            if (virLogAddOutputToFile(prio, name) == 0)
                ret++;
            VIR_FREE(name);
        } else {
            return(-1);
        }
        virSkipSpaces(&cur);
    }
    return(ret);
}

/**
 * virLogParseFilters:
 * @filters: string defining a (set of) filter(s)
 *
 * The format for a filter is:
 *    x:name
 *       where name is a match string
 * the x prefix is the minimal level where the messages should be logged
 *    1: DEBUG
 *    2: INFO
 *    3: WARNING
 *    4: ERROR
 *
 * Multiple filter can be defined in a single @filters, they just need to be
 * separated by spaces.
 *
 * Returns the number of filter parsed and installed or -1 in case of error
 */
int virLogParseFilters(const char *filters) {
    const char *cur = filters, *str;
    char *name;
    int prio;
    int ret = 0;

    if (cur == NULL)
        return(-1);

    virSkipSpaces(&cur);
    while (*cur != 0) {
        prio= virParseNumber(&cur);
        if ((prio < 0) || (prio > 4))
            return(-1);
        if (*cur != ':')
            return(-1);
        cur++;
        str = cur;
        while ((*cur != 0) && (!IS_SPACE(cur)))
            cur++;
        if (str == cur)
            return(-1);
        name = strndup(str, cur - str);
        if (name == NULL)
            return(-1);
        if (virLogDefineFilter(name, prio, 0) >= 0)
            ret++;
        VIR_FREE(name);
        virSkipSpaces(&cur);
    }
    return(ret);
}
#endif /* ENABLE_DEBUG */

