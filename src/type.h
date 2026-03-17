#ifndef TYPE_H
#define TYPE_H

#include <stdio.h>

typedef enum {
  TypeKindUnit = 0,
  TypeKindS8,
  TypeKindS16,
  TypeKindS32,
  TypeKindS64,
  TypeKindPtr,
} TypeKind;

typedef struct Type Type;

struct Type {
  TypeKind  kind;
  // For pointers
  Type     *target;
};

Type *type_new(TypeKind kind, Type *target);
void  type_print(FILE *stream, Type *type);
bool  type_eq(Type *a, Type *b);
u32   get_type_size(Type *type);

#endif // TYPE_H
