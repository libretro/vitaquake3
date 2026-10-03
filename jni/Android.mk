LOCAL_PATH := $(call my-dir)

CORE_DIR := $(LOCAL_PATH)/..

# Android has OpenGL ES only, so the core is built with ioquake3's OpenGL 2
# renderer, which runs on OpenGL ES 2/3 (see Makefile.common).
HAVE_GLES := 1

include $(CORE_DIR)/Makefile.common

ifeq ($(TARGET_ARCH_ABI),arm64-v8a)
  COMPILE_ARCH := aarch64
else ifeq ($(TARGET_ARCH_ABI),armeabi-v7a)
  COMPILE_ARCH := arm
else ifeq ($(TARGET_ARCH_ABI),x86_64)
  COMPILE_ARCH := x86_64
else
  COMPILE_ARCH := x86
endif

COREFLAGS := -D__LIBRETRO__ -DANDROID -DARCH_STRING=\"$(COMPILE_ARCH)\" -DNO_VM_COMPILED -DBOTLIB \
             -DPRODUCT_VERSION=\"1.36_GIT_ba68b99c-2018-01-23\" -DUSE_INTERNAL_JPEG \
             $(COREDEFINES) $(INCFLAGS)

include $(CLEAR_VARS)
LOCAL_MODULE    := retro
LOCAL_SRC_FILES := $(SOURCES_C)
LOCAL_CFLAGS    := $(COREFLAGS) -std=gnu99 -Wno-error=implicit-function-declaration
# The engine passes formatted strings on to printf-style functions in places
# (qcommon/common.c); the NDK makes that an error by default.
LOCAL_DISABLE_FORMAT_STRING_CHECKS := true
LOCAL_LDFLAGS   := -Wl,-version-script=$(CORE_DIR)/link.T
LOCAL_LDLIBS    := -llog -lm
include $(BUILD_SHARED_LIBRARY)
