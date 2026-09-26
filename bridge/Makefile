CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -pedantic
WAYLAND_FLAGS = $(shell pkg-config --cflags --libs wayland-client)

TARGET = ../bin/sc7-clipboard-bridge
SOURCES = sc7_clipboard_bridge.c wlr-data-control-unstable-v1-protocol.c

all: $(TARGET)

$(TARGET): $(SOURCES) wlr-data-control-unstable-v1-client-protocol.h
	mkdir -p ../bin
	$(CC) $(CFLAGS) $(SOURCES) $(WAYLAND_FLAGS) -o $(TARGET)

protocol: wlr-data-control-unstable-v1.xml
	wayland-scanner client-header wlr-data-control-unstable-v1.xml wlr-data-control-unstable-v1-client-protocol.h
	wayland-scanner private-code wlr-data-control-unstable-v1.xml wlr-data-control-unstable-v1-protocol.c

clean:
	rm -f $(TARGET)

.PHONY: all clean protocol
