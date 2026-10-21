LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE     := aoqfreeflip
LOCAL_LDLIBS     := -llog -ldl
LOCAL_CFLAGS     := -std=gnu17 -fvisibility=hidden
LOCAL_SRC_FILES  := main.c \
    ../shared/utils/utils.c \
    ../shared/inline-hook/inlineHook.c \
    ../shared/inline-hook/relocate.c \
    ../modmanager/modconfig.c \
    ../modmanager/cJSON.c
LOCAL_C_INCLUDES := $(LOCAL_PATH)/../modmanager
include $(BUILD_SHARED_LIBRARY)
