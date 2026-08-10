# If RACK_DIR is not defined when calling the Makefile, default to two directories above
RACK_DIR ?= ../..

# FLAGS will be passed to both the C and C++ compiler
FLAGS +=
CFLAGS +=
CXXFLAGS +=

# Careful about linking to shared libraries, since you can't assume much about the user's environment and library search path.
# Static libraries are fine, but they should be added to this plugin's build system.
LDFLAGS +=

# The driver needs the PipeWire headers at compile time, but never links
# against libpipewire: the library is opened with dlopen() at runtime so the
# plugin still loads on machines and platforms without PipeWire. Build hosts
# without the headers produce a plugin that registers no driver.
include $(RACK_DIR)/arch.mk
ifdef ARCH_LIN
PIPEWIRE_CFLAGS := $(shell pkg-config --cflags libpipewire-0.3 2>/dev/null)
ifneq ($(PIPEWIRE_CFLAGS),)
FLAGS += $(PIPEWIRE_CFLAGS) -DHAVE_PIPEWIRE
else
$(warning PipeWire headers not found: install libpipewire-0.3 development files, or this plugin will do nothing)
endif
endif

# Add .cpp files to the build
SOURCES += $(wildcard src/*.cpp)

# Add files to the ZIP package when running `make dist`
# The compiled plugin and "plugin.json" are automatically added.
DISTRIBUTABLES += $(wildcard LICENSE*)

# Include the Rack plugin Makefile framework
include $(RACK_DIR)/plugin.mk
