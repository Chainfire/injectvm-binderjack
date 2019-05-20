# Primary is CMakeLists.txt, but this should still work, though injectvm doesn't automatically get
# renamed to libinjectvm.so, and the output obj/ and libs/ paths may be in the wrong place for
# inclusion in the app itself

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    payload_main.cpp

LOCAL_MODULE := payload
LOCAL_CFLAGS += -DDEBUG
LOCAL_CFLAGS += -std=c++11
LOCAL_LDLIBS := -lm -llog

include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    libinject/inject.cpp \
    inject_main.cpp

LOCAL_MODULE := injectvm
LOCAL_CFLAGS += -DDEBUG
LOCAL_LDLIBS := -ldl -llog

include $(BUILD_EXECUTABLE)
