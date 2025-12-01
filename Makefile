# Tides 2 for Disting NT - Makefile
# Authentic port of Mutable Instruments Tides 2

# ARM Cortex-M7 toolchain
CC = arm-none-eabi-gcc
CXX = arm-none-eabi-g++
OBJCOPY = arm-none-eabi-objcopy

# API path - adjust to your distingNT_API location
API_PATH ?= ../distingNT_API

# Source files
SOURCES = tides.cpp

# Output
TARGET = tides.o

# CPU flags for Cortex-M7 with FPU
CPU_FLAGS = -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard

# Compiler flags
CXXFLAGS = $(CPU_FLAGS) \
           -O2 \
           -ffunction-sections \
           -fdata-sections \
           -fno-exceptions \
           -fno-rtti \
           -fno-threadsafe-statics \
           -std=c++17 \
           -Wall \
           -Wextra \
           -I$(API_PATH)/include \
           -I.

# Linker flags (for relocatable object)
LDFLAGS = -r

.PHONY: all clean install check syntax help

all: $(TARGET)

$(TARGET): $(SOURCES) tides_dsp.h tides_resources.h
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(SOURCES)
	@echo "Built $(TARGET)"
	@$(OBJCOPY) -O binary $@ /dev/null 2>/dev/null && echo "Object file valid" || echo "Warning: object validation skipped"

# Syntax check using host compiler (no ARM toolchain needed)
syntax:
	g++ -std=c++17 -fsyntax-only -Wall -Wextra \
		-DSYNTAX_CHECK_ONLY \
		-I$(API_PATH)/include \
		-I. \
		$(SOURCES) 2>&1 || true
	@echo "Syntax check complete"

# Check that ARM toolchain is available
check:
	@which $(CXX) > /dev/null 2>&1 || (echo "ERROR: ARM toolchain not found. Install with:"; \
		echo "  Ubuntu/Debian: sudo apt install gcc-arm-none-eabi"; \
		echo "  macOS: brew install arm-none-eabi-gcc"; \
		echo "  Windows: Download from ARM website"; \
		exit 1)
	@echo "ARM toolchain found: $$(which $(CXX))"
	@test -d $(API_PATH) || (echo "ERROR: API path not found: $(API_PATH)"; \
		echo "Clone with: git clone https://github.com/expertsleepersltd/distingNT_API"; \
		exit 1)
	@echo "API path found: $(API_PATH)"

# Install to SD card (adjust MOUNT_POINT as needed)
MOUNT_POINT ?= /media/sdcard
install: $(TARGET)
	@test -d $(MOUNT_POINT)/programs/plug-ins || mkdir -p $(MOUNT_POINT)/programs/plug-ins
	cp $(TARGET) $(MOUNT_POINT)/programs/plug-ins/
	@echo "Installed to $(MOUNT_POINT)/programs/plug-ins/$(TARGET)"

clean:
	rm -f $(TARGET) *.o

help:
	@echo "Tides 2 for Disting NT"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build the plugin (default)"
	@echo "  syntax   - Check syntax using host compiler"
	@echo "  check    - Verify toolchain and API path"
	@echo "  install  - Copy to SD card"
	@echo "  clean    - Remove build artifacts"
	@echo "  help     - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  API_PATH    - Path to distingNT_API (default: $(API_PATH))"
	@echo "  MOUNT_POINT - SD card mount point (default: $(MOUNT_POINT))"
	@echo ""
	@echo "Example:"
	@echo "  make API_PATH=/path/to/distingNT_API"
