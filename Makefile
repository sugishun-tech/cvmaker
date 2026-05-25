CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
PKGS = cairo pangocairo pango gdk-pixbuf-2.0
TARGET = cvmaker
SRC = cvmaker.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(shell pkg-config --cflags --libs $(PKGS))

clean:
	rm -f $(TARGET) output.pdf
