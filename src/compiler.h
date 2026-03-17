#ifndef COMPILER_H
#define COMPILER_H

#include <stdio.h>

#include "shl/shl-str.h"

bool compile(Str code, Str file_path, FILE *output_file);

#endif // COMPILER_H
