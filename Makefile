PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

TARGET := bin/singleDPI.elf
SOURCES := source/main.cpp source/cache.cpp third_party/tiny-json/tiny-json.cpp

CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wpedantic \
	-Iinclude -Ithird_party/tiny-json
LDADD := -lSceNet -lSceSystemService -lSceAppInstUtil -lkernel_sys

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SOURCES) include/appinst.hpp include/cache.hpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(LDADD) -o $@ $(SOURCES)
	$(STRIP) --strip-unneeded $@

clean:
	rm -f $(TARGET)

test: $(TARGET)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(TARGET)
