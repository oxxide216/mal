#ifndef IO_H
#define IO_H

#include "shl/shl-str.h"

Str  read_file(char *path);
Str  read_file_str(Str path);
bool write_file(char *path, Str content);

#endif // IO_H
