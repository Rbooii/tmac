ifeq ($(origin CC),default)
CC := clang
endif
CFLAGS ?= -std=c17 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS += -MMD -MP
FRAMEWORKS = -framework IOKit -framework CoreFoundation
PREFIX ?= /usr/local

SRC = src/main.c src/sensors.c src/ui.c
OBJ = $(SRC:.c=.o)
BIN = tmac

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(FRAMEWORKS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

install: $(BIN)
	install -d $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(PREFIX)/bin/$(BIN)

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) $(BIN)

-include $(OBJ:.o=.d)

.PHONY: all install uninstall clean
