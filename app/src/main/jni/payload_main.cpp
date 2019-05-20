/* Copyright (c) 2019, Jorrit 'Chainfire' Jongma
   See LICENSE file for details */

#define LOG_TAG "InjectVM/Payload"
#include "ndklog.h"

#include <jni.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

#include <thread>
#include <string>

#include <pthread.h>

static std::thread inject;

// Retrieves current Thread's ContextClassLoader
static jobject getContextClassLoader(JNIEnv* env) {
    LOGD("getContextClassLoader");

    // Thread.currentThread().getContextClassLoader()
    jclass Thread = env->FindClass("java/lang/Thread");
    jmethodID getCurrentThread = env->GetStaticMethodID(Thread, "currentThread", "()Ljava/lang/Thread;");
    jobject currentThread = env->CallStaticObjectMethod(Thread, getCurrentThread);
    jmethodID getContextClassLoader = env->GetMethodID(Thread, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
    return env->CallObjectMethod(currentThread, getContextClassLoader);
}

// Set current Thread's ContextClassLoader
static void setContextClassLoader(JNIEnv* env, jobject classLoader) {
    LOGD("setContextClassLoader");

    // Thread.currentThread().setContextClassLoader(classLoader)
    jclass Thread = env->FindClass("java/lang/Thread");
    jmethodID getCurrentThread = env->GetStaticMethodID(Thread, "currentThread", "()Ljava/lang/Thread;");
    jobject currentThread = env->CallStaticObjectMethod(Thread, getCurrentThread);
    jmethodID setContextClassLoader = env->GetMethodID(Thread, "setContextClassLoader", "(Ljava/lang/ClassLoader;)V");
    env->CallVoidMethod(currentThread, setContextClassLoader, classLoader);
}

// Load class from APK
static jclass loadClass(JNIEnv* env, jobject classLoader, const char* packagePath, const char* className) {
    LOGD("loadClass");

    // Create PathClassLoader: new PathClassLoader(packagePath, classLoader)
    jclass PathClassLoader = env->FindClass("dalvik/system/PathClassLoader");
    jmethodID pclCtor = env->GetMethodID(PathClassLoader, "<init>", "(Ljava/lang/String;Ljava/lang/ClassLoader;)V");

    jstring path = env->NewStringUTF(packagePath);
    jobject pcl = env->NewObject(PathClassLoader, pclCtor, path, classLoader);
    env->DeleteLocalRef(path);

    // Load class using PathClassLoader: pcl.loadClass(className);
    jmethodID loadClass = env->GetMethodID(PathClassLoader, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");

    jstring injectClass = env->NewStringUTF(className);
    jclass InjectClass = (jclass)env->CallObjectMethod(pcl, loadClass, injectClass);
    env->DeleteLocalRef(injectClass);

    return InjectClass;
}

static void hijackJavaBinder(JNIEnv* env, jobject thiz, jobject targetJavaBinder, jobject replacementJavaBinder) {
    LOGD("targetBinder:%p", targetJavaBinder);

    // Get targetJavaBinder::mObject, which is a pointer to a JavaBBinderHolder
    jclass Binder = env->FindClass("android/os/Binder");
    jfieldID mObject = env->GetFieldID(Binder, "mObject", "J");
    jlong targetJavaBBinderHolder = env->GetLongField(targetJavaBinder, mObject);

    LOGD("targetJavaBBinderHolder:%p", (void*)targetJavaBBinderHolder);
    if (targetJavaBBinderHolder == 0) {
        LOGE("targetJavaBBinderHolder == NULL");
        return;
    }

    // JavaBBinderHolder data is: Mutex(==pthread_mutex_t), wp<JavaBBinder>
    uintptr_t* targetJavaBBinder = (uintptr_t*)(targetJavaBBinderHolder + sizeof(pthread_mutex_t));
    if (*targetJavaBBinder == 0) {
        //TODO Fix finding JavaBBinder
        // Not really sure why this is on some Androids versions.
        // Seems on some versions JavaBBinderHolder is a subclass of RefBase, and on others not.
        // But that should only make 1 pointer offset difference, not 2? Probably reading something wrong.
        targetJavaBBinder += 2;
    }
    LOGD("targetJavaBBinder:%p --> %p", targetJavaBBinder, (void*)*targetJavaBBinder);

    if (*targetJavaBBinder == 0) {
        LOGE("*targetJavaBBinder == NULL");
        return;
    } else {
        // Find reference to JavaBinder inside JavaBBinder
        uintptr_t* ptr = (uintptr_t*)*targetJavaBBinder;
        for (int i = 0; i < 20; i++) {
            LOGD("%i %p %p", i, ptr, (void*)*ptr);
            if (env->GetObjectRefType((jobject)*ptr) == JNIGlobalRefType) {
                if (env->IsSameObject(targetJavaBinder, (jobject)*ptr)) {
                    *ptr = (uintptr_t)env->NewGlobalRef(replacementJavaBinder);
                    LOGD("^^^^ replaced with %p", (void*)*ptr);
                    return;
                }
            }
            ptr++;
        }
    }
}

static JNINativeMethod nativeMethods[] = {
        { "hijackJavaBinder", "(Landroid/os/Binder;Ljava/lang/Object;)V", (void*)hijackJavaBinder }
};

// Java injector thread
static void inject_thread(JavaVM* jvm, jobject classLoader, char* apk, char* clazz, char* method) {
    LOGD("Injector thread start");

    // give injector binary some time to finish
    usleep(1000000);

    JNIEnv* env;
    jint res;
    res = jvm->AttachCurrentThread(&env, NULL);
    if (res != JNI_OK) {
        LOGE("AttachCurrentThread (background) != JNI_OK");
    } else {
        // Set the default ClassLoader for this thread to the one that was set for the main thread
        setContextClassLoader(env, classLoader);

        // Load clazz from APK
        jclass InjectClass = loadClass(env, classLoader, apk, clazz);

        // Attach native methods
        env->RegisterNatives(InjectClass, nativeMethods, 1);

        // Execute injected class method: public static void clazz::method()
        LOGD("Calling %s::%s()", clazz, method);
        jmethodID main = env->GetStaticMethodID(InjectClass, method, "()V");
        env->CallStaticVoidMethod(InjectClass, main);

        // Cleanup
        env->DeleteGlobalRef(classLoader);
        jvm->DetachCurrentThread();
        free(apk);
        free(clazz);
        free(method);
    }

    LOGD("Injector thread exit");
}

static int loaded = 0;

// Startup
extern "C" {
    void __attribute__ ((visibility ("default"))) OnInject(JavaVM** JavaVMSymbol, char* param) {
        LOGD("OnInject %p [%s]", JavaVMSymbol, param);
        if (loaded) {
            LOGD("-- Already loaded");
            return;
        }
        loaded = 1;

        if (JavaVMSymbol == NULL) {
            LOGE("JavaVM** == NULL");
            return;
        }

        JavaVM* jvm = *JavaVMSymbol;
        if (jvm == NULL) {
            LOGE("JavaVM* == NULL");
            return;
        }
        
        JNIEnv* env;
        jint res;

        // Should already be attached
        res = jvm->AttachCurrentThread(&env, NULL);
        if (res != JNI_OK) {
            LOGE("AttachCurrentThread (main) != JNI_OK");
        } else {
            // Should be running in the main thread, needed for this to work to get the right
            // ClassLoader; with the wrong parent ClassLoader, Android may refuse to load our APK
            jobject classLoader = env->NewGlobalRef(getContextClassLoader(env));

            // Parse param as apk:class:method
            char* apk = (char*)malloc(256);
            char* clazz = (char*)malloc(256);
            char* method = (char*)malloc(256);
            memset(apk, 0, 256);
            memset(clazz, 0, 256);
            memset(method, 0, 256);
            for (unsigned int i = 0; i < strlen(param); i++) {
                if (param[i] == ':') param[i] = ' ';
            }
            if (sscanf(param, "%s %s %s", apk, clazz, method) != 3) {
                LOGE("Parsing [%s] failed [apk class method] (':'==' ')", param);
                return;
            }
            for (unsigned int i = 0; i < strlen(clazz); i++) {
                if (clazz[i] == '.') clazz[i] = '/';
            }
            LOGD("apk[%s] class[%s] method[%s]", apk, clazz, method);

            // Do the rest of our work in a different thread because the current thread is likely
            // interrupting the jvm at an unpredictable point
            inject = std::thread(inject_thread, jvm, classLoader, apk, clazz, method);
        }
    }
}
