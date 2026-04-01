CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c99 \
           $(shell pkg-config --cflags zvbi)
LDFLAGS = $(shell pkg-config --libs zvbi)

TARGET  = ttxd
SRC     = ttxd.c

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)

clean:
	rm -f $(TARGET)
