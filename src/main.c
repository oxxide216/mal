#include <stdio.h>

#include "shl/shl-defs.h"
#include "shl/shl-log.h"
#define SHL_STR_IMPLEMENTATION
#include "shl/shl-str.h"
#undef SHL_STR_IMPLEMENTATION
#include "io.h"
#include "compiler.h"

void print_usage(char *program_name) {
  printf("%s [OUTPUT FILE] [INPUT FILE]\n", program_name);
}

int main(i32 argc, char **argv) {
  if (argc < 3) {
    print_usage(argv[0]);
    ERROR("No input file\n");
    return 1;
  }

  Str code = read_file(argv[2]);
  if (code.ptr == NULL) {
    ERROR("Could not open %s\n", argv[2]);
    return 1;
  }

  FILE *output_file = fopen(argv[1], "w");
  if (output_file == NULL) {
    ERROR("Could not open %s\n", argv[1]);
    return 1;
  }

  Str input_path = {
    argv[2],
    strlen(argv[2]),
  };

  bool success = compile(code, input_path, output_file);
  fclose(output_file);
  if (!success)
    return 1;

  return 0;
}
