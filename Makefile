CC = gcc
CFLAGS = -Wall -Wextra -Werror -pthread
TARGET = iot_monitor
SOURCE = src/main.c
LIBS = -lmosquitto -lcjson -lm -pthread

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) $(SOURCE) -o $(TARGET) $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
