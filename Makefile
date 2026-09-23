# ==============================================================================
# Generic C Makefile
# To add modules: drop .c files into SRC_DIR and run make.
# ==============================================================================

# --- Compiler & flags ---------------------------------------------------------
CC      := gcc
# Feature flags: must be defined here (build-wide), not with a local #define
# in a single .c file, since every .c file is its own translation unit and
# a per-file #define would leave other files disagreeing on struct layout.
FEATURES := #-DUSE_VP_LIST_QUEUE
CFLAGS  := -Wall -Wextra -g -MMD -MP -g -static $(FEATURES)
LDFLAGS :=
LIBS    :=

# --- Directories --------------------------------------------------------------
SRC_DIR := src
OBJ_DIR := obj
BIN_DIR := .

# --- Target binary ------------------------------------------------------------
TARGET := $(BIN_DIR)/flame

# --- Sources, objects & dependency files (auto-discovered) -------------------
SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))
DEPS := $(OBJS:.o=.d)

# ==============================================================================
# Rules
# ==============================================================================

.PHONY: all clean rebuild

all: $(TARGET)

# Link
$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@ $(LIBS)
	@echo "Built $@"

# Compile each .c -> .o
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Create dirs if missing
$(OBJ_DIR):
	mkdir -p $@

$(BIN_DIR):
	mkdir -p $@

# Pull in auto-generated header dependencies (silently ignored on first build)
-include $(DEPS)

clean:
	rm -rf $(OBJ_DIR) $(TARGET)
	@echo "Cleaned."

rebuild: clean all
