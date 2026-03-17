.RECIPEPREFIX = >

CC = cc
CFLAGS = -Wall -Wextra -Ilibs
LDFLAGS =
BUILD_DIR = build

SRC = $(wildcard src/*.c)

OBJ = $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRC))

ifdef GDB
  CFLAGS += -ggdb
endif

mal: $(OBJ)
> $(CC) -o mal $(OBJ) $(LDFLAGS)

$(BUILD_DIR)/%.o: src/%.c
> mkdir -p $(dir $@)
> $(CC) $(CFLAGS) -c -o $@ $^

clean:
> rm -rf $(BUILD_DIR) mal
