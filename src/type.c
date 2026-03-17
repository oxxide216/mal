#include "shl/shl-defs.h"
#include "type.h"

Type *type_new(TypeKind kind, Type *target) {
  Type *type = malloc(sizeof(Type));
  type->kind = kind;
  type->target = target;
  return type;
}

void type_print(FILE *stream, Type *type) {
  switch (type->kind) {
  case TypeKindUnit: fprintf(stream, "unit"); break;
  case TypeKindS8: fprintf(stream, "s8"); break;
  case TypeKindS16: fprintf(stream, "s16"); break;
  case TypeKindS32: fprintf(stream, "s32"); break;
  case TypeKindS64: fprintf(stream, "s64"); break;
  case TypeKindPtr: {
    fprintf(stream, "&");
    type_print(stream, type->target);
  } break;
  }
}

bool type_eq(Type *a, Type *b) {
  if (a->kind != b->kind)
    return false;

  return (!a->target && !b->target) ||
         (a->target && b->target &&
           type_eq(a->target, b->target));
}

u32 get_type_size(Type *type) {
  switch (type->kind) {
  case TypeKindUnit: return 0;
  case TypeKindS8:   return 1;
  case TypeKindS16:  return 2;
  case TypeKindS32:  return 4;
  case TypeKindS64:  return 8;
  case TypeKindPtr:  return 8;
  default:           return 0;
  }
}
