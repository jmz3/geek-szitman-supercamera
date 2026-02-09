MAKEFLAGS += --no-builtin-rules --no-builtin-variables
.SUFFIXES:

CXX := g++
CXXFLAGS := -std=c++23 -Wall -Wextra -O2 -MMD -g -Iinclude -pthread
OPENCVFLAGS := `pkg-config --cflags --libs opencv4`
LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0 2>/dev/null || pkg-config --cflags libusb 2>/dev/null)
LIBUSB_LIBS := $(shell pkg-config --libs libusb-1.0 2>/dev/null || pkg-config --libs libusb 2>/dev/null || echo -lusb-1.0)

VIEWER_BIN := out
SENDER_BIN := out_stream_sender
CORE_OBJ := src/supercamera_core.o

all: $(VIEWER_BIN) $(SENDER_BIN)

-include $(VIEWER_BIN).d $(SENDER_BIN).d src/supercamera_core.d

$(CORE_OBJ): src/supercamera_core.cpp include/supercamera_core.hpp Makefile
	$(CXX) $(CXXFLAGS) $(LIBUSB_CFLAGS) -c "$<" -o "$@"

$(VIEWER_BIN): src/supercamera_poc.cpp $(CORE_OBJ) include/supercamera_core.hpp Makefile
	$(CXX) $(CXXFLAGS) $(LIBUSB_CFLAGS) "$<" $(CORE_OBJ) $(OPENCVFLAGS) $(LIBUSB_LIBS) -o "$@"

$(SENDER_BIN): src/supercamera_stream_sender.cpp $(CORE_OBJ) include/supercamera_core.hpp Makefile
	$(CXX) $(CXXFLAGS) $(LIBUSB_CFLAGS) "$<" $(CORE_OBJ) $(LIBUSB_LIBS) -o "$@"

clean:
	rm -rf $(VIEWER_BIN) $(SENDER_BIN) $(VIEWER_BIN).d $(SENDER_BIN).d $(CORE_OBJ) src/supercamera_core.d
