.RECIPEPREFIX = >

CC = cc
override CFLAGS += -Wall -Wextra -Ilibs -Ilibs/lexgen/include
override LDFLAGS +=
BUILD_DIR = build

SRC = $(wildcard src/*.c)
LIBS_SRC = $(wildcard libs/lexgen/src/runtime/*.c) libs/lexgen/src/common/wstr.c

OBJ = $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRC))
LIBS_OBJ = $(patsubst %.c,$(BUILD_DIR)/%.o,$(LIBS_SRC))

mal: $(OBJ) $(LIBS_OBJ)
> $(CC) -o mal $(OBJ) $(LIBS_OBJ) $(LDFLAGS)

malc:
> ./mal malc.s mal-src/main.mal
> yasm -f elf64 malc.s
> ld -T scripts/link.ld -o malc malc.o

src/grammar.h: libs/lexgen/lexgen grammar.lg
> libs/lexgen/lexgen src/grammar.h grammar.lg

libs/lexgen/lexgen:
> cd libs/lexgen && ./build.sh

$(BUILD_DIR)/%.o: src/%.c src/grammar.h
> mkdir -p $(dir $@)
> $(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/libs/%.o: libs/%.c src/grammar.h
> mkdir -p $(dir $@)
> $(CC) $(CFLAGS) -c -o $@ $<

clean:
> rm -rf $(BUILD_DIR) mal
