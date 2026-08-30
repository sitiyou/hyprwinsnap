CXX ?= g++
CXXFLAGS ?= -shared -fPIC -Wall -Wextra -O2 -std=c++23
# hyprland headers as system headers: their own warnings are not our concern
INCLUDES = $(shell pkg-config --cflags hyprland | sed 's/-I/-isystem /g')

SRC = src/main.cpp
TARGET = hyprwinsnap.so

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@

clean:
	rm -f $(TARGET)

.PHONY: all clean