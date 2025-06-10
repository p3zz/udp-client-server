# Compiler
CC = gcc

# Compiler flags
CFLAGS = -Wall -Wextra -O2

# Output directory
SOURCES_DIR = src

# Source files
SOURCES = $(SOURCES_DIR)/server.c $(SOURCES_DIR)/client.c

# Output directory
BUILD_DIR = build

# Executable targets
TARGETS = $(BUILD_DIR)/server $(BUILD_DIR)/client

# Default rule
all: $(TARGETS)

# Create build directory if it doesn't exist
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Rule to build each target
$(BUILD_DIR)/server: $(SOURCES_DIR)/server.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/client: $(SOURCES_DIR)/client.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $<

# Clean rule
clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
