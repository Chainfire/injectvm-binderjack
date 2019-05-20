/* Copyright (c) 2015, Simone 'evilsocket' Margaritelli
   Copyright (c) 2015-2019, Jorrit 'Chainfire' Jongma
   See LICENSE file for details */

#ifndef INJECT_H
#define INJECT_H

#include <android/log.h>

// No need to reference manually, use HOOKLOG
extern const char* _libinject_log_tag;
extern int _libinject_log;

// Pass NULL to disable logging
void libinject_log(const char* log_tag);

// INJECTLOG( "some message %d %d %d", 1, 2, 3 );
#define INJECTLOG(F,...) \
    if (_libinject_log) __android_log_print( ANDROID_LOG_DEBUG, _libinject_log_tag, F, ##__VA_ARGS__ )

// Find pid for process
pid_t libinject_find_pid_of(const char* process);

// Load library in process pid, returns 0 on success
int libinject_injectvm(pid_t pid, char* library, char* param);


#endif
