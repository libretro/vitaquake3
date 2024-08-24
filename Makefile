STATIC_LINKING := 0
AR             := ar

ifneq ($(V),1)
   Q := @
endif

ifneq ($(SANITIZER),)
   CFLAGS   := -fsanitize=$(SANITIZER) $(CFLAGS)
   CXXFLAGS := -fsanitize=$(SANITIZER) $(CXXFLAGS)
   LDFLAGS  := -fsanitize=$(SANITIZER) $(LDFLAGS)
endif

ifeq ($(platform),)
platform = unix
ifeq ($(shell uname -a),)
   platform = win
else ifneq ($(findstring MINGW,$(shell uname -a)),)
   platform = win
else ifneq ($(findstring Darwin,$(shell uname -a)),)
   platform = osx
else ifneq ($(findstring win,$(shell uname -a)),)
   platform = win
endif
endif

# system platform
system_platform = unix
ifeq ($(shell uname -a),)
	EXE_EXT = .exe
	system_platform = win
else ifneq ($(findstring Darwin,$(shell uname -a)),)
	system_platform = osx
	arch = intel
ifeq ($(shell uname -p),arm)
	arch = arm
endif
ifeq ($(shell uname -p),powerpc)
	arch = ppc
endif
else ifneq ($(findstring MINGW,$(shell uname -a)),)
	system_platform = win
endif

CORE_DIR    += .
TARGET_NAME := vitaquake3
LIBM		    = -lm

ifeq ($(STATIC_LINKING), 1)
EXT := a
endif

ifeq ($(platform), unix)
	EXT ?= so
   TARGET := $(TARGET_NAME)_libretro.$(EXT)
   fpic := -fPIC
   ifneq ($(findstring SunOS,$(shell uname -a)),)
      SHARED := -shared -z defs
   else
      SHARED := -shared -Wl,--version-script=$(CORE_DIR)/link.T -Wl,--no-undefined
   endif
   LDFLAGS += -lGL
else ifeq ($(platform), linux-portable)
   TARGET := $(TARGET_NAME)_libretro.$(EXT)
   fpic := -fPIC -nostdlib
   SHARED := -shared -Wl,--version-script=$(CORE_DIR)/link.T
	LIBM :=
else ifneq (,$(findstring osx,$(platform)))
   TARGET := $(TARGET_NAME)_libretro.dylib
   fpic := -fPIC
   SHARED := -dynamiclib

   ifeq ($(CROSS_COMPILE),1)
		TARGET_RULE   = -target $(LIBRETRO_APPLE_PLATFORM) -isysroot $(LIBRETRO_APPLE_ISYSROOT)
		CFLAGS   += $(TARGET_RULE)
		CPPFLAGS += $(TARGET_RULE)
		CXXFLAGS += $(TARGET_RULE)
		LDFLAGS  += $(TARGET_RULE)
   endif

ifeq ($(UNIVERSAL),1)
ifeq ($(ARCHFLAGS),)
   ARCHFLAGS = -arch i386 -arch x86_64
ifeq ($(archs),arm)
   ARCHFLAGS = -arch arm64
endif
ifeq ($(archs),ppc)
   ARCHFLAGS = -arch ppc -arch ppc64
endif
endif

   CXXFLAGS += $(ARCHFLAGS)
   LFLAGS += $(ARCHFLAGS)
endif


else ifneq (,$(findstring ios,$(platform)))
   TARGET := $(TARGET_NAME)_libretro_ios.dylib
	fpic := -fPIC
	SHARED := -dynamiclib

ifeq ($(IOSSDK),)
   IOSSDK := $(shell xcodebuild -version -sdk iphoneos Path)
endif

	DEFINES := -DIOS
	CC = cc -arch armv7 -isysroot $(IOSSDK)
ifeq ($(platform),ios9)
CC     += -miphoneos-version-min=8.0
CXXFLAGS += -miphoneos-version-min=8.0
else
CC     += -miphoneos-version-min=5.0
CXXFLAGS += -miphoneos-version-min=5.0
endif
else ifneq (,$(findstring qnx,$(platform)))
	TARGET := $(TARGET_NAME)_libretro_qnx.so
   fpic := -fPIC
   SHARED := -shared -Wl,--version-script=$(CORE_DIR)/link.T -Wl,--no-undefined
else ifeq ($(platform), emscripten)
   TARGET := $(TARGET_NAME)_libretro_emscripten.bc
   fpic := -fPIC
   SHARED := -shared -Wl,--version-script=$(CORE_DIR)/link.T -Wl,--no-undefined
else ifeq ($(platform), vita)
   TARGET := $(TARGET_NAME)_vita.a
   CC = arm-vita-eabi-gcc
   AR = arm-vita-eabi-ar
   CXXFLAGS += -Wl,-q -Wall -O3
   STATIC_LINKING = 1
# Nintendo Switch (libnx)
else ifeq ($(platform), libnx)
    include $(DEVKITPRO)/libnx/switch_rules
    EXT=a
    TARGET := $(TARGET_NAME)_libretro_$(platform).$(EXT)
    DEFINES := -DSWITCH=1 -D__SWITCH__=1 -fcommon -DHAVE_LIBNX=1
    CFLAGS	:= $(INCDIRS) $(DEFINES) -g -fPIE -I$(LIBNX)/include/ -I$(PORTLIBS)/include/ -ffunction-sections -fdata-sections -ftls-model=local-exec
    CFLAGS += -fvisibility=hidden
    CFLAGS	+= -march=armv8-a -mtune=cortex-a57 -mtp=soft -mcpu=cortex-a57+crc+fp+simd
    CXXFLAGS := $(ASFLAGS) $(CFLAGS) -std=gnu++11
    STATIC_LINKING = 1
else
   CC ?= gcc
   TARGET := $(TARGET_NAME)_libretro.dll
   SHARED := -shared -static-libgcc -static-libstdc++
   ifneq ($(DEBUG), 1)
   SHARED += -s
   endif
   SHARED += -Wl,--version-script=$(CORE_DIR)/link.T -Wl,--no-undefined
   LDFLAGS += -lwsock32 -lws2_32 -lopengl32
endif

LDFLAGS += $(LIBM)

ifeq ($(DEBUG), 1)
   CFLAGS += -O0 -g -DDEBUG
   CXXFLAGS += -O0 -g -DDEBUG
else
   CFLAGS += -O3 -DRELEASE
   CXXFLAGS += -O3
endif

include Makefile.common

OBJECTS := $(SOURCES_C:.c=.o)

COMPILE_PLATFORM=$(shell uname | sed -e 's/_.*//' | tr '[:upper:]' '[:lower:]' | sed -e 's/\//_/g')
COMPILE_ARCH=$(shell uname -m | sed -e 's/i.86/x86/' | sed -e 's/^arm.*/arm/')

ifeq ($(COMPILE_PLATFORM),cygwin)
  PLATFORM=mingw32
endif

ifndef PLATFORM
PLATFORM=$(COMPILE_PLATFORM)
endif
export PLATFORM

ifeq ($(PLATFORM),mingw32)
  MINGW=1
endif
ifeq ($(PLATFORM),mingw64)
  MINGW=1
endif

ifeq ($(COMPILE_ARCH),i86pc)
  COMPILE_ARCH=x86
endif

ifeq ($(COMPILE_ARCH),amd64)
  COMPILE_ARCH=x86_64
endif
ifeq ($(COMPILE_ARCH),x64)
  COMPILE_ARCH=x86_64
endif

ifeq ($(COMPILE_ARCH),powerpc)
  COMPILE_ARCH=ppc
endif
ifeq ($(COMPILE_ARCH),powerpc64)
  COMPILE_ARCH=ppc64
endif

ifeq ($(COMPILE_ARCH),axp)
  COMPILE_ARCH=alpha
endif
ifeq ($(platform), libnx)
  COMPILE_ARCH=aarch64
endif

CFLAGS   += -Wall -D__LIBRETRO__ $(fpic) -DUSE_ICON -DARCH_STRING=\"$(COMPILE_ARCH)\" -DNO_VM_COMPILED -DBOTLIB -DPRODUCT_VERSION=\"1.36_GIT_ba68b99c-2018-01-23\" -DUSE_INTERNAL_JPEG
CXXFLAGS += -Wall -D__LIBRETRO__ $(fpic) -fpermissive -fno-rtti -fno-exceptions -std=gnu++11

ifeq ($(platform), unix)
CFLAGS += -std=gnu99
else
CFLAGS += -std=c99
endif
CFLAGS     += $(INCFLAGS)
CXXFLAGS   += $(INCFLAGS)

all: $(TARGET)

$(TARGET): $(OBJECTS)
ifeq ($(STATIC_LINKING), 1)
	$(AR) rcs $@ $(OBJECTS)
else
	$(CC) $(fpic) $(SHARED) -o $@ $(OBJECTS) $(LDFLAGS)
endif

%.o: %.c
	$(CC) $(CFLAGS) $(fpic) -c -o $@ $<

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: clean

print-%:
	@echo '$*=$($*)'
