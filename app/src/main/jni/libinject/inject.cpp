/* Copyright (c) 2015, Simone 'evilsocket' Margaritelli
   Copyright (c) 2015-2019, Jorrit 'Chainfire' Jongma
   See LICENSE file for details */

// interesting info re:debug http://stackoverflow.com/questions/18577956/how-to-use-ptrace-to-get-a-consistent-view-of-multiple-threads

#include <asm/ptrace.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <dirent.h>
#include <elf.h>
#include <stdbool.h>
#include <time.h>

#include "inject.h"

#if defined(__arm__)
#define CPSR_T_MASK ( 1u << 5 )
#define PARAMS_IN_REGS 4
#elif defined(__aarch64__)
#define CPSR_T_MASK ( 1u << 5 )
#define PARAMS_IN_REGS 8
#define pt_regs user_pt_regs
#define uregs regs
#define ARM_pc pc
#define ARM_sp sp
#define ARM_cpsr pstate
#define ARM_lr regs[30]
#define ARM_r0 regs[0]
#endif

#if defined(__LP64__)
#define PATH_LINKER_BIONIC "/bionic/bin/linker64"
#define PATH_LIBDL_BIONIC "/bionic/lib64/libdl.so"
#define PATH_LIBC_BIONIC "/bionic/lib64/libc.so"
#define PATH_LINKER "/system/bin/linker64"
#define PATH_LIBDL "/system/lib64/libdl.so"
#define PATH_LIBC "/system/lib64/libc.so"
#define PATH_LIBANDROID_RUNTIME "/system/lib64/libandroid_runtime.so"
#else
#define PATH_LINKER_BIONIC "/bionic/bin/linker"
#define PATH_LIBDL_BIONIC "/bionic/lib/libdl.so"
#define PATH_LIBC_BIONIC "/bionic/lib/libc.so"
#define PATH_LINKER "/system/bin/linker"
#define PATH_LIBDL "/system/lib/libdl.so"
#define PATH_LIBC "/system/lib/libc.so"
#define PATH_LIBANDROID_RUNTIME "/system/lib/libandroid_runtime.so"
#endif

const char* _libinject_log_tag = "InjectVM/Injector";
int _libinject_log = 1;

void libinject_log(const char* log_tag) {
    _libinject_log_tag = log_tag;
    _libinject_log = log_tag == NULL ? 0 : 1;
}

pid_t _pid;
void *_dlopen;
void *_dlerror;
void *_calloc;
void *_free;

typedef void (*remote_stop_t)();
remote_stop_t remote_stop_ptr = NULL;

// ptrace wrapper with some error checking.
static long trace(const char* debug, int request, void *addr = NULL, size_t data = 0) {
    errno = 0;
    long ret = 0;
    for (int i = 0; i < 10; i++) {
        ret = ptrace(request, _pid, (caddr_t) addr, (void *) data);
        if (ret == -1 && (errno == EBUSY || errno == EFAULT || errno == ESRCH)) {
            char eb[16];
            char rb[16];

            const char* e = NULL;
            const char* r = NULL;

            switch (errno) {
            case ESRCH: e = "ESRCH"; break;
            default: snprintf(eb, sizeof(eb), "%d", errno); e = eb;
            }

            switch (request) {
            case PTRACE_PEEKTEXT: r = "PTRACE_PEEKTEXT"; break;
            case PTRACE_PEEKDATA: r = "PTRACE_PEEKDATA"; break;
            case PTRACE_POKETEXT: r = "PTRACE_POKETEXT"; break;
            case PTRACE_POKEDATA: r = "PTRACE_POKEDATA"; break;
            case PTRACE_CONT: r = "PTRACE_CONT"; break;
            case PTRACE_KILL: r = "PTRACE_KILL"; break;
            case PTRACE_SINGLESTEP: r = "PTRACE_SINGLESTEP"; break;
#if defined(PTRACE_GETREGS)
            case PTRACE_GETREGS: r = "PTRACE_GETREGS"; break;
#endif
#if defined(PTRACE_SETREGS)
            case PTRACE_SETREGS: r = "PTRACE_SETREGS"; break;
#endif
#if defined(PTRACE_GETFPREGS)
            case PTRACE_GETFPREGS: r = "PTRACE_GETFPREGS"; break;
#endif
#if defined(PTRACE_SETFPREGS)
            case PTRACE_SETFPREGS: r = "PTRACE_SETFPREGS"; break;
#endif
            case PTRACE_ATTACH: r = "PTRACE_ATTACH"; break;
            case PTRACE_DETACH: r = "PTRACE_DETACH"; break;
            case PTRACE_SYSCALL: r = "PTRACE_SYSCALL"; break;
            case PTRACE_SETOPTIONS: r = "PTRACE_SETOPTIONS"; break;
            case PTRACE_GETEVENTMSG: r = "PTRACE_GETEVENTMSG"; break;
            case PTRACE_GETSIGINFO: r = "PTRACE_GETSIGINFO"; break;
            case PTRACE_SETSIGINFO: r = "PTRACE_SETSIGINFO"; break;
#if defined(PTRACE_GETREGSET)
            case PTRACE_GETREGSET: r = "PTRACE_GETREGSET"; break;
#endif
#if defined(PTRACE_SETREGSET)
            case PTRACE_SETREGSET: r = "PTRACE_SETREGSET"; break;
#endif
            default: snprintf(rb, sizeof(rb), "%d", request); r = rb;
            }

            INJECTLOG("ptrace [%s] error [%s] on request [%s]", debug, e, r);
        }
        if (ret == -1 && (errno == ESRCH)) {
            INJECTLOG("ptrace remote_stop/retry");
            if (remote_stop_ptr != NULL) {
                remote_stop_ptr();
            }
        } else {
            break;
        }
    }
    return ret;
}

/*
 * This method will open /proc/<pid>/maps and search for the specified
 * library base address.
 */
static uintptr_t findLibrary(const char *library, pid_t pid) {
    char filename[0xFF] = { 0 }, buffer[1024] = { 0 };
    FILE *fp = NULL;
    uintptr_t address = 0;

    sprintf(filename, "/proc/%d/maps", pid == -1 ? _pid : pid);

    fp = fopen(filename, "rt");
    if (fp == NULL) {
        INJECTLOG("fopen error");
        goto done;
    }

    while (fgets(buffer, sizeof(buffer), fp)) {
        if (strstr(buffer, library)) {
            address = (uintptr_t) strtoul(buffer, NULL, 16);
            goto done;
        }
    }

    done:

    if (fp) {
        fclose(fp);
    }

    return address;
}

/*
 * Compute the delta of the local and the remote modules and apply it to
 * the local address of the symbol ... BOOM, remote symbol address!
 */
static void* remote_findFunction(const char* library, void* local_addr) {
    uintptr_t local_handle = findLibrary( library, getpid() );
    uintptr_t remote_handle = findLibrary( library, -1 );
    uintptr_t remote_addr = (uintptr_t)local_addr + remote_handle - local_handle;
    return (void*)remote_addr;
}

static uint64_t ms() {
    struct timespec spec;
    clock_gettime(CLOCK_MONOTONIC, &spec);
    return (spec.tv_sec * 1000) + (spec.tv_nsec / 1.0e6);
}

/*
 * Make sure the remote process is stopped, or we get ESRCH errors
 */
static void remote_stop() {
    INJECTLOG( "remote_stop" );
    kill( _pid, SIGSTOP );

    int status;
    int ret;
    uint64_t start = ms();
    while ( (ret = waitpid( _pid, &status, WUNTRACED || WNOHANG )) != -1 ) {
        if (ret == _pid) {
            if (WIFSIGNALED(status)) {
                trace ( "remote_stop", PTRACE_CONT, NULL, WTERMSIG(status));
            } else if (WIFSTOPPED(status)) {
                break;
            } else if (WIFEXITED(status)) {
                break;
            }
        } else if (ms() - start > 128) {
            // assume stopped before remote_stop() was called, 128ms is long
            break;
        }
        usleep(1);
    }
    INJECTLOG( "/remote_stop" );
}

/*
* Read 'blen' bytes from the remote process at 'addr' address.
*/
static bool remote_read(const char* debug, size_t addr, unsigned char *buf, size_t blen){
    remote_stop();

    size_t i = 0;
    long ret = 0;

    for( i = 0; i < blen; i += sizeof(size_t) ){
       ret = trace( debug, PTRACE_PEEKTEXT, (void *)(addr + i) );
       if( ret == -1 ) {
           return false;
       }

       memcpy( &buf[i], &ret, sizeof(ret) );
    }

    return true;
}

/*
 * Write 'blen' bytes to the remote process at 'addr' address.
 */
static bool remote_write(const char* debug, size_t addr, unsigned char *buf, size_t blen) {
    remote_stop();

    size_t i = 0;
    long ret;

    // make sure the buffer is word aligned
    char *ptr = (char *) malloc(blen + blen % sizeof(size_t));
    memcpy(ptr, buf, blen);

    for (i = 0; i < blen; i += sizeof(size_t)) {
        ret = trace( debug, PTRACE_POKETEXT, (void *) (addr + i), *(size_t *) &ptr[i] );
        if (ret == -1) {
            free(ptr);
            return false;
        }
    }

    free(ptr);

    return true;
}

// Get remote registers
static void trace_getregs(const char* debug, struct pt_regs * regs) {
#if defined (__aarch64__) || defined(__x86_64__)
    uintptr_t regset = NT_PRSTATUS;
    struct iovec ioVec;

    ioVec.iov_base = regs;
    ioVec.iov_len = sizeof(*regs);
    trace( debug, PTRACE_GETREGSET, (void*)regset, (size_t)&ioVec );
#else
    trace( debug, PTRACE_GETREGS, 0, (size_t)regs );
#endif
}

// Set remote registers
static void trace_setregs(const char* debug, struct pt_regs * regs) {
#if defined (__aarch64__) || defined(__x86_64__)
    uintptr_t regset = NT_PRSTATUS;
    struct iovec ioVec;

    ioVec.iov_base = regs;
    ioVec.iov_len = sizeof(*regs);
    trace( debug, PTRACE_SETREGSET, (void*)regset, (size_t)&ioVec );
#else
    trace( debug, PTRACE_SETREGS, 0, (size_t)regs );
#endif
}

/*
 * Remotely call the remote function given its address, the number of
 * arguments and the arguments themselves.
 */
static uintptr_t remote_call(void *function, int nargs, ...) {
#if defined(__arm__) || defined(__aarch64__) || defined(__i386__) || defined(__x86_64__)
    remote_stop();

    struct pt_regs regs, rbackup;

    // get registers and backup them
    trace_getregs( "backup", &regs );
    memcpy( &rbackup, &regs, sizeof(struct pt_regs) );

    // start copying parameters
    va_list vl;
    va_start(vl,nargs);

    // push parameters into registers and stacks, setup registers to perform the call

#if defined(__arm__) || defined(__aarch64__)
    // fill R0-Rx with the first 4 (32-bit) or 8 (64-bit) parameters
    for ( int i = 0; ( i < nargs ) && ( i < PARAMS_IN_REGS ); ++i ) {
        regs.uregs[i] = va_arg( vl, uintptr_t );
    }

    // push remaining parameters onto stack
    if (nargs > PARAMS_IN_REGS) {
        regs.ARM_sp -= sizeof(uintptr_t) * (nargs - PARAMS_IN_REGS);
        uintptr_t stack = regs.ARM_sp;
        for ( int i = PARAMS_IN_REGS; i < nargs; ++i ) {
            uintptr_t arg = va_arg( vl, uintptr_t );
            remote_write( "params", (size_t)stack, (uint8_t *)&arg, sizeof(uintptr_t) );
            stack += sizeof(uintptr_t);
        }
    }

    // return address to catch
    regs.ARM_lr = 0;

    // function address to call
    regs.ARM_pc = (uintptr_t)function;

    // setup the current processor status register
    if ( regs.ARM_pc & 1 ) {
        // thumb
        regs.ARM_pc &= (~1u);
        regs.ARM_cpsr |= CPSR_T_MASK;
    } else {
        // arm
        regs.ARM_cpsr &= ~CPSR_T_MASK;
    }
#elif defined(__i386__)
    // push all params onto stack
    regs.esp -= sizeof(uintptr_t) * nargs;
    uintptr_t stack = regs.esp;
    for( int i = 0; i < nargs; ++i ) {
        uintptr_t arg = va_arg( vl, uintptr_t );
        remote_write( "params", (size_t)stack, (uint8_t *)&arg, sizeof(uintptr_t) );
        stack += sizeof(uintptr_t);
    }

    // return address to catch
    uintptr_t tmp_addr = 0;
    regs.esp -= sizeof(uintptr_t);
    remote_write( "return", (size_t)regs.esp, (uint8_t *)&tmp_addr, sizeof(uintptr_t) );

    // function address to call
    regs.eip = (uintptr_t)function;
#elif defined(__x86_64__)
    // align, rsp - 8 must be a multiple of 16 at function entry point
    {
        uintptr_t space = sizeof(uintptr_t);
        if (nargs > 6) space += sizeof(uintptr_t) * (nargs - 6);
        while (((regs.rsp - space - 8) & 0xF) != 0) regs.rsp--;
    }

    // fill [RDI, RSI, RDX, RCX, R8, R9] with the first 6 parameters
    for ( int i = 0; ( i < nargs ) && ( i < 6 ); ++i ) {
        uintptr_t arg = va_arg( vl, uintptr_t );
        switch (i) {
        case 0: regs.rdi = arg; break;
        case 1: regs.rsi = arg; break;
        case 2: regs.rdx = arg; break;
        case 3: regs.rcx = arg; break;
        case 4: regs.r8 = arg; break;
        case 5: regs.r9 = arg; break;
        }
    }

    // push remaining parameters onto stack
    if (nargs > 6) {
        regs.rsp -= sizeof(uintptr_t) * (nargs - 6);
        uintptr_t stack = regs.rsp;
        for( int i = 6; i < nargs; ++i ) {
            uintptr_t arg = va_arg( vl, uintptr_t );
            remote_write( "params", (size_t)stack, (uint8_t *)&arg, sizeof(uintptr_t) );
            stack += sizeof(uintptr_t);
        }
    }

    // return address to catch
    uintptr_t tmp_addr = 0;
    regs.rsp -= sizeof(uintptr_t);
    remote_write( "return", (size_t)regs.rsp, (uint8_t *)&tmp_addr, sizeof(uintptr_t) );

    // function address to call
    regs.rip = (uintptr_t)function;

    // may be needed
    regs.rax = 0;
    regs.orig_rax = 0;
#endif

    // end of parameters
    va_end(vl);

    // do the call
    trace_setregs( "call", &regs );
    trace( "call", PTRACE_CONT );

    // catch the SIGSEGV caused by the 0 return address
    int status;
    while ( waitpid( _pid, &status, WUNTRACED ) == _pid ) {
        if ( WIFSTOPPED(status) && (WSTOPSIG(status) == SIGSEGV) ) {
            break;
        }
        trace( "waitpid", PTRACE_CONT );
    }

    // get registers again for return value
    trace_getregs( "return", &regs );

    // restore original registers state
    trace_setregs( "restore", &rbackup );

    // continue execution
    trace( "continue", PTRACE_CONT );

#if defined(__arm__) || defined(__aarch64__)
    return regs.ARM_r0;
#elif defined(__i386__)
    return regs.eax;
#elif defined(__x86_64__)
    return regs.rax;
#endif
    return 0;
#else
#error ARCHITECTURE NOT SUPPORTED
#endif
}

// Allocate memory in remote process
static uintptr_t remote_calloc(size_t nmemb, size_t size) {
    return remote_call(_calloc, 2, nmemb, size);
}

// Free remotely allocated memory.
static void remote_free(uintptr_t p) {
    remote_call(_free, 1, p);
}

// Copy a given string into the remote process memory.
static uintptr_t remote_string(const char *s) {
    uintptr_t mem = remote_calloc(strlen(s) + 1, 1);

    remote_write( "string", mem, (unsigned char *) s, strlen(s) + 1);

    return mem;
}

// Remotely force the target process to dlopen a library.
static uintptr_t remote_dlopen(const char *libname) {
    uintptr_t pmem = remote_string(libname);

    uintptr_t plib = remote_call(_dlopen, 2, pmem, 0);

    remote_free(pmem);

    return plib;
}

// Get remote dlerror
static void remote_dlerror(char* error, int size) {
    uintptr_t e = remote_call(_dlerror, 0);
    remote_read("dlerror", e, (unsigned char*)error, size - 1);
}

// Find pid for process
pid_t libinject_find_pid_of(const char* process) {
    int id;
    pid_t pid = -1;
    DIR* dir;
    FILE *fp;
    char filename[32];
    char cmdline[256];

    struct dirent * entry;

    if (process == NULL)
        return -1;

    dir = opendir("/proc");
    if (dir == NULL)
        return -1;

    while ((entry = readdir(dir)) != NULL) {
        id = atoi(entry->d_name);
        if (id != 0) {
            sprintf(filename, "/proc/%d/cmdline", id);
            fp = fopen(filename, "r");
            if (fp) {
                fgets(cmdline, sizeof(cmdline), fp);
                fclose(fp);

                if (strcmp(process, cmdline) == 0) {
                    /* process found */
                    pid = id;
                    break;
                }
            }
        }
    }

    closedir(dir);
    return pid;
}

// Load library in process pid, resolves JavaVM and passes it and param to loaded library, returns 0 on success
int libinject_injectvm(pid_t pid, char* library, char* param) {
    remote_stop_ptr = remote_stop;

    int ret = 1;
    _pid = pid;

    // attach to target process
    if ( trace( "attach", PTRACE_ATTACH ) != -1) {
        // stop entire process, including non-main threads
        kill( _pid, SIGSTOP);

        // wait until we're stopped
        remote_stop();

        /* First thing first, we need to search these functions into the target
         * process address space.
         */

        /* We can resolve the references to LIBC easily, but dl* is tricky. On older Android
         * versions, libdl.so is commonly not loaded by the linker, and our dl* functions
         * come directly from the linker.
         *
         * On newer Android versions, libdl.so is directly loaded and dl* come from there.
         *
         * On even newer Android versions, the linker/libc/libdl have moved from /system to /bionic
         */
        const char* libc = access( PATH_LIBC_BIONIC, R_OK ) == 0 ? PATH_LIBC_BIONIC : PATH_LIBC;
        const char* libdl = access( PATH_LIBDL_BIONIC, R_OK ) == 0 ? PATH_LIBDL_BIONIC : PATH_LIBDL;
        const char* linker = access( PATH_LINKER_BIONIC, R_OK ) == 0 ? PATH_LINKER_BIONIC : PATH_LINKER;
        
        _calloc = remote_findFunction( libc, (void *) calloc );
        _free = remote_findFunction( libc, (void *) free );
        if ((findLibrary( libdl, -1 ) != 0) && (findLibrary( libdl, _pid ) != 0)) {
            void* handle = dlopen( libdl, RTLD_LAZY );
            _dlopen = remote_findFunction( libdl, dlsym( handle, "dlopen" ) );
            _dlerror = remote_findFunction( libdl, dlsym( handle, "dlerror" ) );
            dlclose( handle );
        } else {
            _dlopen = remote_findFunction( linker, (void *) dlopen );
            _dlerror = remote_findFunction( linker, (void *) dlerror );
        }

        // Resolve android::AndroidRuntime::mJavaVM, this is tricky from the payload because
        // of linker namespaces (you can't load the lib, dlsym doesn't work right, and the location
        // of the variable in memory is different between Android versions), but no such issue
        // exists from this injector.
        void* runtime = dlopen( PATH_LIBANDROID_RUNTIME, RTLD_LAZY );
        void* javavm = dlsym( runtime, "_ZN7android14AndroidRuntime7mJavaVME" );
        void* _javavm = remote_findFunction( PATH_LIBANDROID_RUNTIME, javavm );
        dlclose(runtime);

        INJECTLOG( "calloc:%p free:%p dlopen:%p dlerror:%p javavm:%p", _calloc, _free, _dlopen, _dlerror, _javavm );

        // once we have the addresses, we can proceed to inject
        if ( remote_dlopen(library) != 0 ) {

            // call OnInject(_javavm, param)
            void* payload = dlopen( library, RTLD_LAZY );
            void* oninject = dlsym( payload, "OnInject" );
            void* _oninject = remote_findFunction( library, oninject );
            INJECTLOG( "oninject:%p", _oninject );

            if ((oninject != NULL) && (_oninject != NULL)) {
                uintptr_t pmem = remote_string(param);
                remote_call(_oninject, 2, _javavm, pmem);
                remote_free(pmem);
                ret = 0;
            }
        } else {
            char error[1024] = { 0 };
            remote_dlerror(error, 1024);
            INJECTLOG( "dlopen failed: %s", error );
        }

        // detach from target process
        remote_stop();
        trace( "detach", PTRACE_DETACH );

        // let all threads in the target process continue
        kill( _pid, SIGCONT );
    } else {
        INJECTLOG( "Failed to attach to process %d", _pid);
    }

    return ret;
}
