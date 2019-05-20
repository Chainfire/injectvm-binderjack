/* Excerpts:
   Copyright (c) 2008, The Android Open Source Project

   Modifications:
   Copyright (c) 2015, Jorrit 'Chainfire' Jongma

   See LICENSE file for details */

// Slightly modified cutils/log.h for use in NDK
//
// define LOG_TAG prior to inclusion
//
// if you define DEBUG or LOG_DEBUG before inclusion, VDIWE are logged
// if LOG_SILENT is defined, nothing is logged (overrides (LOG_)DEBUG)
// if none of the above are defined, VD are ignored and IWE are logged
//
// if LOG_TO_STDOUT is defined, logging is to STDOUT

#pragma GCC diagnostic ignored "-Wwrite-strings"

#ifndef _NDK_LOG_H
#define _NDK_LOG_H

#include <android/log.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <unistd.h>

#ifndef LOG_TAG
#define LOG_TAG "NDK_LOG"
#endif

#ifdef DEBUG
    #define LOG_DEBUG
#endif

#ifndef LOG_SILENT
    #ifndef LOG_TO_STDOUT
        #define LOG(...) ((void)__android_log_print(__VA_ARGS__))
    #else
        static void __stdout_log_print(int level, char* tag, char* fmt, ...) {
            char* lvl = "?";
            switch (level) {
            case ANDROID_LOG_VERBOSE: lvl = "V"; break;
            case ANDROID_LOG_DEBUG: lvl = "D"; break;
            case ANDROID_LOG_INFO: lvl = "I"; break;
            case ANDROID_LOG_WARN: lvl = "W"; break;
            case ANDROID_LOG_ERROR: lvl = "E"; break;
            }
            printf("%s/%s: ", lvl, tag);
            va_list args;
            va_start(args, fmt);
            vprintf(fmt, args);
            va_end(args);
            printf("\n");
            sync();
        }

        #define LOG(level, tag, ...) __stdout_log_print(level, tag, __VA_ARGS__)
    #endif
#else
    #define LOG(...) ((void)0)
#endif

#ifdef LOG_DEBUG
    #define LOGV(...) ((void)LOG(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__))
    #define LOGD(...) ((void)LOG(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__))
#else
    #define LOGV(...) ((void)0)
    #define LOGD(...) ((void)0)
#endif
#define LOGI(...) ((void)LOG(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define LOGW(...) ((void)LOG(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__))
#define LOGE(...) ((void)LOG(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

#define CONDITION(cond) (__builtin_expect((cond)!=0, 0))

#define LOGV_IF(cond, ...) ((CONDITION(cond)) ? ((void)LOGV(__VA_ARGS__)) : (void)0)
#define LOGD_IF(cond, ...) ((CONDITION(cond)) ? ((void)LOGD(__VA_ARGS__)) : (void)0)
#define LOGI_IF(cond, ...) ((CONDITION(cond)) ? ((void)LOGI(__VA_ARGS__)) : (void)0)
#define LOGW_IF(cond, ...) ((CONDITION(cond)) ? ((void)LOGW(__VA_ARGS__)) : (void)0)
#define LOGE_IF(cond, ...) ((CONDITION(cond)) ? ((void)LOGE(__VA_ARGS__)) : (void)0)

#ifdef __cplusplus
}
#endif

#endif // _NDK_LOG_H
