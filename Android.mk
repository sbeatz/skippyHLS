LOCAL_PATH_HLS := $(call my-dir)
MY_GSTREAMER_HLS_SOURCE_PATH := $(LOCAL_PATH_HLS)/src
MY_GSTREAMER_HLS_INCLUDE_PATH := $(LOCAL_PATH_HLS)/include
MY_GSTREAMER_HLS_INCLUDE_PATH += $(LOCAL_PATH_HLS)/src
include $(CLEAR_VARS)

ifeq ($(TARGET_ARCH_ABI),armeabi)
GSTREAMER_ROOT        := $(GSTREAMER_ROOT_ARM)
else ifeq ($(TARGET_ARCH_ABI),armeabi-v7a)
GSTREAMER_ROOT        := $(GSTREAMER_ROOT_ARMV7)
else ifeq ($(TARGET_ARCH_ABI),x86)
GSTREAMER_ROOT        := $(GSTREAMER_ROOT_X86)
else
$(error Target arch ABI not supported)
endif

LOCAL_C_INCLUDES := $(MY_GSTREAMER_HLS_INCLUDE_PATH)
LOCAL_C_INCLUDES += $(MY_GSTREAMER_HLS_INCLUDE_PATH)
LOCAL_EXPORT_C_INCLUDES := $(MY_GSTREAMER_HLS_INCLUDE_PATH)
LOCAL_MODULE    := skippyHLS
LOCAL_SRC_FILES += $(MY_GSTREAMER_HLS_SOURCE_PATH)/skippy_fragment.c $(MY_GSTREAMER_HLS_SOURCE_PATH)/skippy_hlsdemux.c $(MY_GSTREAMER_HLS_SOURCE_PATH)/skippy_m3u8.cpp $(MY_GSTREAMER_HLS_SOURCE_PATH)/skippy_uridownloader.c $(MY_GSTREAMER_HLS_SOURCE_PATH)/skippy_m3u8_parser.cpp $(MY_GSTREAMER_HLS_SOURCE_PATH)/oggOpusdec.cpp
LOCAL_SHARED_LIBRARIES := gstreamer_android
LOCAL_LDLIBS := -llog -landroid -lstdc++
include $(BUILD_SHARED_LIBRARY)
