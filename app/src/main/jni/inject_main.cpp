/* Copyright (c) 2015, Simone 'evilsocket' Margaritelli
   Copyright (c) 2015-2019, Jorrit 'Chainfire' Jongma
   See LICENSE file for details */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include "libinject/inject.h"

int main( int argc, char **argv ) {
#ifndef DEBUG
    libinject_log(NULL);
#endif

    printf("injectvm - Copyright (C) 2015 - Chainfire, evilsocket\n");

    pid_t pid     = atoi(argv[1]);
    char* library = argv[2];
    char* param = argv[3];

    if (libinject_injectvm(pid, library, param) == 0) {
        printf("-ok\n");
        return 0;
    } else {
        printf("-fail\n");
        return 1;
    }
}
