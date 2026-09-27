CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
PKG_CONFIG ?= pkg-config
SQLITE_PREFIX ?= $(shell brew --prefix sqlite 2>/dev/null)

CFLAGS += $(shell $(PKG_CONFIG) --cflags libimobiledevice-1.0 2>/dev/null)
LDLIBS += $(shell $(PKG_CONFIG) --libs libimobiledevice-1.0 2>/dev/null)
ifneq ($(strip $(SQLITE_PREFIX)),)
CFLAGS += -I$(SQLITE_PREFIX)/include
LDFLAGS += -L$(SQLITE_PREFIX)/lib
endif
LDLIBS += -lsqlite3

TARGET = recover_iphone_audio

.PHONY: all clean

all: $(TARGET)

$(TARGET): src/recover_iphone_audio.c
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

clean:
	rm -f $(TARGET)
