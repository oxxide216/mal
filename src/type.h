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
Type *type_clone(Type *type);
void  type_free(Type *type);
u32   type_get_size(Type *type);

bool type_is_signed(Type *type);

bool types_can_add(Type *a, Type *b);
bool types_can_mul(Type *a, Type *b);

#endif // TYPE_H
