CC = gcc
CFLAGS = $(shell pkg-config --cflags gtk+-3.0 vte-2.91 glib-2.0) -Wall -Wextra -Werror
LDFLAGS = $(shell pkg-config --libs gtk+-3.0 vte-2.91 glib-2.0)

TARGET = codews
SRCS = main.c
OBJS = $(SRCS:.c=.o)
INSTALL_DIR = /usr/local/bin

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	sudo cp $(TARGET) $(INSTALL_DIR)/$(TARGET)
	sudo chmod +x $(INSTALL_DIR)/$(TARGET)

uninstall:
	sudo rm -f $(INSTALL_DIR)/$(TARGET)

.PHONY: all clean install uninstall
