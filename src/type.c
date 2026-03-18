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

Type *type_clone(Type *type) {
  Type *new_type = malloc(sizeof(Type));
  new_type->kind = type->kind;
  if (type->target)
    new_type->target = type_clone(type->target);
  else
    new_type->target = NULL;
  return new_type;
}

void type_free(Type *type) {
  if (type->target)
    type_free(type->target);
  free(type);
}

u32 type_get_size(Type *type) {
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

bool type_is_signed(Type *type) {
  return type->kind >= TypeKindS8 && type->kind <= TypeKindS64;
}

bool types_can_add(Type *a, Type *b) {
  return (a->kind >= TypeKindS8 && a->kind <= TypeKindS64 && a->kind == b->kind) ||
         (a->kind == TypeKindPtr && b->kind >= TypeKindS8 && b->kind <= TypeKindS64);
}

bool types_can_mul(Type *a, Type *b) {
  return a->kind >= TypeKindS8 && a->kind <= TypeKindS64 &&
         a->kind == b->kind;
}

bool types_can_cast(Type *a, Type *b) {
  return b->kind != TypeKindS64 || a->kind == TypeKindS32;
}
