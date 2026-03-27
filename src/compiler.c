#include <stdio.h>

#include "shl/shl-log.h"
#include "lexgen/runtime.h"
#include "grammar.h"
#include "compiler.h"
#include "lexer.h"
#include "type.h"
#include "io.h"

#define MASK(id) (1lu << (id))

#define STACK_FRAME_BEGIN_SIZE     16
#define CALLEE_PRESERVED_REGS_SIZE 40

typedef struct {
  Str    code;
  u16    pos;
  Tokens tokens;
  bool   has_error;
} Parser;

typedef struct {
  Str   name;
  Type *type;
  u32   offset;
  bool  is_param;
  bool  is_global;
} Var;

typedef Da(Var) Vars;

typedef Da(Str) Strs;

typedef struct {
  Str   name;
  Type *type;
} Param;

typedef Da(Param) Params;

typedef struct {
  Str     name;
  Type   *return_type;
  Params  params;
} Proc;

typedef Da(Proc) Procs;

typedef struct {
  Str   name;
  Type *type;
} NamedType;

typedef Da(NamedType) NamedTypes;

typedef Da(Type *) Types;

typedef struct {
  Vars           vars;
  Vars           global_vars;
  Strs           strs;
  Procs          procs;
  NamedTypes     named_types;
  u32            stack_size;
  u32            labels_count;
  FILE          *output_file;
  Type          *current_proc_return_type;
  StringBuilder  temp_sb;
} Compiler;

typedef enum {
  DestReturn = 0,
  DestTemp0,
  DestTemp1,
  DestTemp2,
  DestTemp3,
  DestTemp4,
  DestRem,
} Dest;

static Token *peek_token(Parser *parser) {
  if (parser->pos >= parser->tokens.len)
    return NULL;
  return parser->tokens.items + parser->pos;
}

static Token *next_token(Parser *parser) {
  if (parser->pos >= parser->tokens.len)
    return NULL;
  return parser->tokens.items + parser->pos++;
}

static Token *expect_token(Parser *parser, char *expected, u64 id_mask) {
  Token *token = next_token(parser);

  if (MASK(token->id) & id_mask)
    return token;

  parser->has_error = true;

  PERROR(STR_FMT":%u:%u: ", "Expected %s, got `"STR_FMT"`\n",
         STR_ARG(token->file_path),
         token->row + 1, token->col + 1,
         expected, STR_ARG(token->lexeme));

  return NULL;
}

static char *get_dest_loc(Dest dest, Type *type) {
  if (dest == DestReturn) {
    switch (type->kind) {
    case TypeKindUnit: return NULL;
    case TypeKindS8:   return "al";
    case TypeKindS16:  return "ax";
    case TypeKindS32:  return "eax";
    case TypeKindS64:  return "rax";
    case TypeKindPtr:  return "rax";
    }
  } else if (dest == DestTemp0) {
    switch (type->kind) {
    case TypeKindUnit: return NULL;
    case TypeKindS8:   return "bl";
    case TypeKindS16:  return "bx";
    case TypeKindS32:  return "ebx";
    case TypeKindS64:  return "rbx";
    case TypeKindPtr:  return "rbx";
    }
  } else if (dest == DestTemp1) {
    switch (type->kind) {
    case TypeKindUnit: return NULL;
    case TypeKindS8:   return "r12b";
    case TypeKindS16:  return "r12w";
    case TypeKindS32:  return "r12d";
    case TypeKindS64:  return "r12";
    case TypeKindPtr:  return "r12";
    }
  } else if (dest == DestTemp2) {
    switch (type->kind) {
    case TypeKindUnit: return NULL;
    case TypeKindS8:   return "r13b";
    case TypeKindS16:  return "r13w";
    case TypeKindS32:  return "r13d";
    case TypeKindS64:  return "r13";
    case TypeKindPtr:  return "r13";
    }
  } else if (dest == DestTemp3) {
    switch (type->kind) {
    case TypeKindUnit: return NULL;
    case TypeKindS8:   return "r14b";
    case TypeKindS16:  return "r14w";
    case TypeKindS32:  return "r14d";
    case TypeKindS64:  return "r14";
    case TypeKindPtr:  return "r14";
    }
  } else if (dest == DestTemp4) {
    switch (type->kind) {
    case TypeKindUnit: return NULL;
    case TypeKindS8:   return "r15b";
    case TypeKindS16:  return "r15w";
    case TypeKindS32:  return "r15d";
    case TypeKindS64:  return "r15";
    case TypeKindPtr:  return "r15";
    }
  } else if (dest == DestRem) {
    switch (type->kind) {
    case TypeKindUnit: return NULL;
    case TypeKindS8:   return "dl";
    case TypeKindS16:  return "dx";
    case TypeKindS32:  return "edx";
    case TypeKindS64:  return "rdx";
    case TypeKindPtr:  return "rdx";
    }
  }

  return NULL;
}

static Type *get_type_on_stack(Type *type) {
  switch (type->kind) {
  case TypeKindS8:  return type_new(TypeKindS16, NULL);
  case TypeKindS32: return type_new(TypeKindS64, NULL);
  default:          return type_clone(type);
  }
}

static Type *compile_type(Parser *parser, Compiler *compiler) {
  Token *token = expect_token(parser, "`&` or identifier",
                              MASK(TT_AMP) | MASK(TT_IDENT));
  if (parser->has_error)
    return NULL;

  if (token->id == TT_AMP)
    return type_new(TypeKindPtr, compile_type(parser, compiler));

  for (u32 i = 0; i < compiler->named_types.len; ++i)
    if (str_eq(compiler->named_types.items[i].name, token->lexeme))
      return type_clone(compiler->named_types.items[i].type);

  parser->has_error = true;
  PERROR(STR_FMT":%u:%u: ", "Undeclared type `"STR_FMT"`\n",
         STR_ARG(token->file_path),
         token->row + 1, token->col + 1,
         STR_ARG(token->lexeme));

  return NULL;
}

static Type *compile_expr(Parser *parser, Compiler *compiler, Dest dest);

static Type *compile_proc_call(Parser *parser, Compiler *compiler, Token *name) {
  Types param_types = {0};

  u32 params_size = 0;

  Token *token = peek_token(parser);
  bool first = true;

  while (token && token->id != TT_CPAREN) {
    if (first) {
      first = false;
    } else {
      expect_token(parser, "`,`", MASK(TT_COMMA));
      if (parser->has_error)
        return NULL;
    }

    Type *type = compile_expr(parser, compiler, DestTemp0);
    if (parser->has_error)
      return NULL;

    Type *stack_type = get_type_on_stack(type);

    char *loc = get_dest_loc(DestTemp0, stack_type);
    fprintf(compiler->output_file, "  push %s\n", loc);

    token = peek_token(parser);
    DA_APPEND(param_types, type);
    params_size += type_get_size(stack_type);

    type_free(stack_type);
  }

  expect_token(parser, "`)`", MASK(TT_CPAREN));
  if (parser->has_error)
    return NULL;

  for (u32 i = 0; i < compiler->procs.len; ++i) {
    Proc *proc = compiler->procs.items + i;

    if (!str_eq(proc->name, name->lexeme))
      continue;

    if (proc->params.len != param_types.len)
      continue;

    bool found = true;

    for (u32 j = 0; j < proc->params.len; ++j) {
      Param *param = proc->params.items + j;

      if (!type_eq(param->type, param_types.items[j])) {
        found = false;
        break;
      }
    }

    if (!found)
      continue;

    fprintf(compiler->output_file, "  call $"STR_FMT"\n", STR_ARG(name->lexeme));
    if (params_size > 0)
      fprintf(compiler->output_file, "  add rsp,%u\n", params_size);

    for (u32 i = 0; i < param_types.len; ++i)
      type_free(param_types.items[i]);

    return type_clone(proc->return_type);
  }

  parser->has_error = true;
  PERROR(STR_FMT":%u:%u: ", "Undeclared procedure `"STR_FMT"(",
         STR_ARG(name->file_path),
         name->row + 1, name->col + 1,
         STR_ARG(name->lexeme));
  for (u32 i = 0; i < param_types.len; ++i) {
    if (i > 0)
      fprintf(stderr, ", ");
    type_print(stderr, param_types.items[i]);
  }
  fprintf(stderr, ")`\n");

  INFO("Available procedures with this name are:\n");
  for (u32 i = 0; i < compiler->procs.len; ++i) {
    Proc *proc = compiler->procs.items + i;

    if (str_eq(proc->name, name->lexeme)) {
      printf("        "STR_FMT"(", STR_ARG(proc->name));
      for (u32 i = 0; i < proc->params.len; ++i) {
        if (i > 0)
          fprintf(stdout, ", ");
        type_print(stdout, proc->params.items[i].type);
      }
      printf(")\n");
    }
  }

  return NULL;
}

static Var *get_var(Parser *parser, Compiler *compiler, Token *name) {
  for (u32 i = compiler->vars.len; i > 0; --i)
    if (str_eq(name->lexeme, compiler->vars.items[i - 1].name))
      return compiler->vars.items + i - 1;

  for (u32 i = compiler->global_vars.len; i > 0; --i)
    if (str_eq(name->lexeme, compiler->global_vars.items[i - 1].name))
      return compiler->global_vars.items + i - 1;

  parser->has_error = true;
  PERROR(STR_FMT":%u:%u: ", "Undeclared variable `"STR_FMT"`\n",
         STR_ARG(name->file_path),
         name->row + 1, name->col + 1,
         STR_ARG(name->lexeme));
  return NULL;
}

static void print_var_loc(FILE *output_file, Var *var) {
  if (var->is_param)
    fprintf(output_file, "[rbp+%u]", var->offset + STACK_FRAME_BEGIN_SIZE);
  else if (var->is_global)
    fprintf(output_file, "[$"STR_FMT"]", STR_ARG(var->name));
  else
    fprintf(output_file, "[rbp-%u]", var->offset + CALLEE_PRESERVED_REGS_SIZE);
}

static Type *compile_cmp_expr(Parser *parser, Compiler *compiler,
                              Dest dest, u32 label_index,
                              bool *found_comparison);

static Type *_compile_primary_expr(Parser *parser, Compiler *compiler, Dest dest) {
  Token *token = peek_token(parser);
  if (token->id == TT_AMP || token->id == TT_STAR) {
    next_token(parser);

    if (token->id == TT_AMP) {
      token = expect_token(parser, "identifier", MASK(TT_IDENT));

      Var *var = get_var(parser, compiler, token);
      if (parser->has_error)
        return NULL;

      Type *ptr_type = type_new(TypeKindPtr, type_clone(var->type));;

      char *loc = get_dest_loc(dest, ptr_type);
      fprintf(compiler->output_file, "  lea %s,", loc);
      print_var_loc(compiler->output_file, var);
      fprintf(compiler->output_file, "\n");

      return ptr_type;
    } else if (token->id == TT_STAR) {
      Type *type = _compile_primary_expr(parser, compiler, dest);
      if (parser->has_error)
        return NULL;

      if (type->kind != TypeKindPtr) {
        parser->has_error = true;
        PERROR(STR_FMT":%u:%u: ",
               "Attempt to dereference a non-pointer value\n",
               STR_ARG(token->file_path),
               token->row + 1, token->col + 1);
        return NULL;
      }

      char *loc0 = get_dest_loc(dest, type);
      char *loc1 = get_dest_loc(dest, type->target);
      fprintf(compiler->output_file, "  mov %s,[%s]\n", loc1, loc0);

      return type_clone(type->target);
    }

    token = peek_token(parser);
  }

  token = expect_token(parser, "identifier, integer, string, `sizeof`, `(`, `&` or `*`",
                       MASK(TT_INT) | MASK(TT_STR) | MASK(TT_IDENT) |
                       MASK(TT_OPAREN) | MASK(TT_SIZEOF));
  if (parser->has_error)
    return NULL;

  if (token->id == TT_INT) {
    Type *type = type_new(TypeKindS64, NULL);
    char *loc = get_dest_loc(dest, type);
    fprintf(compiler->output_file, "  mov %s,"STR_FMT"\n", loc, STR_ARG(token->lexeme));

    return type;
  } else if (token->id == TT_STR) {
    Type *type = type_new(TypeKindPtr, type_new(TypeKindS8, NULL));
    char *loc = get_dest_loc(dest, type);
    fprintf(compiler->output_file, "  lea %s, [str@%u]\n", loc, compiler->strs.len);

    DA_APPEND(compiler->strs, token->lexeme);

    compiler->temp_sb.len = 0;

    return type;
  } else if (token->id == TT_IDENT) {
    Token *next = peek_token(parser);
    if (next && next->id == TT_OPAREN) {
      next_token(parser);

      Type *type = compile_proc_call(parser, compiler, token);
      if (parser->has_error)
        return NULL;

      char *loc0 = get_dest_loc(DestReturn, type);
      char *loc1 = get_dest_loc(dest, type);
      if (dest != DestReturn)
        fprintf(compiler->output_file, "  mov %s,%s\n", loc1, loc0);

      return type;
    } else {
      Var *var = get_var(parser, compiler, token);
      if (parser->has_error)
        return NULL;

      char *loc = get_dest_loc(dest, var->type);
      fprintf(compiler->output_file, "  mov %s,", loc);
      print_var_loc(compiler->output_file, var);
      fprintf(compiler->output_file, "\n");

      return type_clone(var->type);
    }
  } else if (token->id == TT_OPAREN) {
    Type *type = compile_cmp_expr(parser, compiler, dest, (u32) -1, NULL);
    if (parser->has_error)
      return NULL;

    expect_token(parser, "`)`", MASK(TT_CPAREN));
    if (parser->has_error)
      return NULL;

    return type;
  } else if (token->id == TT_SIZEOF) {
    Type *target = compile_type(parser, compiler);
    if (parser->has_error)
      return NULL;

    u64 size = type_get_size(target);
    Type *type = type_new(TypeKindS64, NULL);
    char *loc = get_dest_loc(dest, type);
    fprintf(compiler->output_file, "  mov %s,%lu\n", loc, size);

    return type;
  }

  return NULL;
}

static Type *compile_primary_expr(Parser *parser, Compiler *compiler, Dest dest) {
  Type *type = _compile_primary_expr(parser, compiler, dest);
  if (parser->has_error)
    return NULL;

  Token *token = peek_token(parser);
  while (token && (token->id == TT_AS || token->id == TT_OBRACKET)) {
    next_token(parser);

    if (token->id == TT_AS) {
      Type *new_type = compile_type(parser, compiler);
      if (parser->has_error)
        return NULL;

      if (!types_can_cast(type, new_type)) {
        parser->has_error = true;
        PERROR(STR_FMT":%u:%u: ", "Cannot cast ",
               STR_ARG(token->file_path),
               token->row + 1, token->col + 1);
        type_print(stderr, type);
        fprintf(stderr, " -> ");
        type_print(stderr, new_type);
        fprintf(stderr, "\n");
        return NULL;
      }

      char *loc0 = get_dest_loc(dest, type);
      char *loc1 = get_dest_loc(dest, new_type);

      if (type->kind == TypeKindS8 &&
          new_type->kind == TypeKindS32)
        fprintf(compiler->output_file, "  movsx %s,%s\n", loc1, loc0);
      if (type->kind == TypeKindS32 &&
          new_type->kind == TypeKindS64)
        fprintf(compiler->output_file, "  movsxd %s,%s\n", loc1, loc0);

      type_free(type);
      type = new_type;
    } else if (token->id == TT_OBRACKET) {
      Type *index = compile_cmp_expr(parser, compiler, DestReturn, (u32) -1, NULL);
      if (parser->has_error)
        return NULL;

      expect_token(parser, "`]`", MASK(TT_CBRACKET));
      if (parser->has_error)
        return NULL;

      if (type->kind != TypeKindPtr) {
        parser->has_error = true;
        PERROR(STR_FMT":%u:%u: ",
               "Attempt to dereference a non-pointer value\n",
               STR_ARG(token->file_path),
               token->row + 1, token->col + 1);
        return NULL;
      }

      if (!type_is_int(index)) {
        parser->has_error = true;
        PERROR(STR_FMT":%u:%u: ",
               "Only integer can be used as an index\n",
               STR_ARG(token->file_path),
               token->row + 1, token->col + 1);
        return NULL;
      }

      char *loc0 = get_dest_loc(dest, type);
      char *loc1 = get_dest_loc(dest, type->target);
      char *loc2 = get_dest_loc(DestReturn, type);
      u32 target_size = type_get_size(type->target);

      fprintf(compiler->output_file, "  mov %s,[%s+%s*%u]\n",
              loc1, loc0, loc2, target_size);

      Type *target = type_clone(type->target);
      type_free(type);
      type = target;
    }

    token = peek_token(parser);
  }

  return type;
}

// mul, div, mod
static Type *compile_mul_expr(Parser *parser, Compiler *compiler, Dest dest) {
  Type *lhs = compile_primary_expr(parser, compiler, dest);
  if (parser->has_error)
    return NULL;

  Token *token = peek_token(parser);
  if (!token)
    return lhs;

  while (token->id == TT_STAR || token->id == TT_SLASH || token->id == TT_PERC) {
    next_token(parser);

    Type *rhs = compile_primary_expr(parser, compiler, DestTemp2);
    if (parser->has_error)
      return NULL;

    if (!types_can_add(lhs, rhs)) {
      parser->has_error = true;
      PERROR(STR_FMT":%u:%u: ", "Cannot do ",
             STR_ARG(token->file_path),
             token->row + 1, token->col + 1);
      type_print(stderr, lhs);
      if (token->id == TT_STAR)
        fprintf(stderr, " * ");
      else if (token->id == TT_SLASH)
        fprintf(stderr, " / ");
      else
        fprintf(stderr, " %% ");
      type_print(stderr, rhs);
      fprintf(stderr, "\n");
      return NULL;
    }

    char *loc0 = get_dest_loc(dest, lhs);
    char *loc1 = get_dest_loc(DestTemp2, rhs);
    char *loc2 = get_dest_loc(DestReturn, lhs);
    char *loc3 = get_dest_loc(DestRem, lhs);

    type_free(rhs);

    if (dest != DestReturn)
      fprintf(compiler->output_file, "  mov %s,%s\n", loc2, loc0);

    if (token->id == TT_STAR) {
      if (type_is_signed(lhs))
        fprintf(compiler->output_file, "  imul %s\n", loc1);
      else
        fprintf(compiler->output_file, "  mul %s\n", loc1);
    } else {
      switch (lhs->kind) {
      case TypeKindS16: fprintf(compiler->output_file, "  cwd\n"); break;
      case TypeKindS32: fprintf(compiler->output_file, "  cdq\n"); break;
      case TypeKindS64: fprintf(compiler->output_file, "  cqo\n"); break;
      default:                                                     break;
      }

      if (type_is_signed(lhs))
        fprintf(compiler->output_file, "  idiv %s\n", loc1);
      else
        fprintf(compiler->output_file, "  div %s\n", loc1);
    }

    if (token->id == TT_PERC)
      fprintf(compiler->output_file, "  mov %s,%s\n", loc0, loc3);
    else if (dest != DestReturn)
      fprintf(compiler->output_file, "  mov %s,%s\n", loc0, loc2);

    token = peek_token(parser);
    if (!token)
      return lhs;
  }

  return lhs;
}

// add, sub
static Type *compile_add_expr(Parser *parser, Compiler *compiler, Dest dest) {
  Type *lhs = compile_mul_expr(parser, compiler, dest);
  if (parser->has_error)
    return NULL;

  Token *token = peek_token(parser);
  if (!token)
    return lhs;

  while (token->id == TT_PLUS || token->id == TT_MINUS) {
    next_token(parser);

    Type *rhs = compile_mul_expr(parser, compiler, DestTemp1);
    if (parser->has_error)
      return NULL;

    if (!types_can_add(lhs, rhs)) {
      parser->has_error = true;
      PERROR(STR_FMT":%u:%u: ", "Cannot do ",
             STR_ARG(token->file_path),
             token->row + 1, token->col + 1);
      type_print(stderr, lhs);
      if (token->id == TT_PLUS)
        fprintf(stderr, " + ");
      else
        fprintf(stderr, " - ");
      type_print(stderr, rhs);
      fprintf(stderr, "\n");
      return NULL;
    }

    char *loc0 = get_dest_loc(dest, lhs);
    char *loc1 = get_dest_loc(DestTemp1, rhs);
    char *loc2 = get_dest_loc(DestReturn, rhs);
    u32 target_size = 0;
    if (lhs->kind == TypeKindPtr)
      target_size = type_get_size(lhs->target);

    type_free(rhs);

    if (target_size <= 0) {
      if (token->id == TT_PLUS)
        fprintf(compiler->output_file, "  add %s,%s\n", loc0, loc1);
      else
        fprintf(compiler->output_file, "  sub %s,%s\n", loc0, loc1);
    } else {
      fprintf(compiler->output_file, "  mov %s,%u\n", loc2, target_size);
      fprintf(compiler->output_file, "  mul %s\n", loc1);
      if (token->id == TT_PLUS)
        fprintf(compiler->output_file, "  add %s,%s\n", loc0, loc2);
      else
        fprintf(compiler->output_file, "  sub %s,%s\n", loc0, loc2);
    }

    token = peek_token(parser);
    if (!token)
      return lhs;
  }

  return lhs;
}

// |, &. ^
static Type *compile_bit_expr(Parser *parser, Compiler *compiler, Dest dest) {
  Type *lhs = compile_add_expr(parser, compiler, dest);
  if (parser->has_error)
    return NULL;

  Token *token = peek_token(parser);
  if (!token)
    return lhs;

  while (token->id == TT_OR || token->id == TT_AMP || token->id == TT_XOR) {
    next_token(parser);

    Type *rhs = compile_add_expr(parser, compiler, DestTemp4);
    if (parser->has_error)
      return NULL;

    if (!type_eq(lhs, rhs)) {
      parser->has_error = true;
      PERROR(STR_FMT":%u:%u: ", "Cannot do ",
             STR_ARG(token->file_path),
             token->row + 1, token->col + 1);
      type_print(stderr, lhs);
      if (token->id == TT_OR)
        fprintf(stderr, " | ");
      else if (token->id == TT_AMP)
        fprintf(stderr, " & ");
      else
        fprintf(stderr, " ^ ");
      type_print(stderr, rhs);
      fprintf(stderr, "\n");
      return NULL;
    }

    char *loc0 = get_dest_loc(dest, lhs);
    char *loc1 = get_dest_loc(DestTemp4, rhs);

    type_free(rhs);

    if (token->id == TT_OR)
      fprintf(compiler->output_file, "  or %s,%s\n", loc0, loc1);
    else if (token->id == TT_AMP)
      fprintf(compiler->output_file, "  and %s,%s\n", loc0, loc1);
    else
      fprintf(compiler->output_file, "  xor %s,%s\n", loc0, loc1);

    token = peek_token(parser);
    if (!token)
      return lhs;
  }

  return lhs;
}

// ==, !=, <, >, <=, >=
static Type *compile_cmp_expr(Parser *parser, Compiler *compiler,
                              Dest dest, u32 label_index,
                              bool *found_comparison) {
  Type *lhs = compile_bit_expr(parser, compiler, dest);
  if (parser->has_error)
    return NULL;

  Token *token = peek_token(parser);
  if (!token)
    return lhs;

  Type *result = type_new(TypeKindS8, NULL);
  bool found_cmp = false;

  while (token->id == TT_EQ || token->id == TT_NE || token->id == TT_LS ||
         token->id == TT_LE || token->id == TT_GT || token->id == TT_GE) {
    next_token(parser);

    found_cmp = true;
    if (found_comparison)
      *found_comparison = true;

    Type *rhs = compile_bit_expr(parser, compiler, DestTemp3);
    if (parser->has_error)
      return NULL;

    if (!type_eq(lhs, rhs)) {
      parser->has_error = true;
      PERROR(STR_FMT":%u:%u: ", "Cannot do ",
             STR_ARG(token->file_path),
             token->row + 1, token->col + 1);
      type_print(stderr, lhs);
      if (token->id == TT_EQ)
        fprintf(stderr, " == ");
      else if (token->id == TT_NE)
        fprintf(stderr, " != ");
      else if (token->id == TT_LS)
        fprintf(stderr, " < ");
      else if (token->id == TT_LE)
        fprintf(stderr, " <= ");
      else if (token->id == TT_GT)
        fprintf(stderr, " > ");
      else if (token->id == TT_GE)
        fprintf(stderr, " >= ");
      type_print(stderr, rhs);
      fprintf(stderr, "\n");
      return NULL;
    }

    Type *temp_lhs = lhs;
    if (temp_lhs->kind == TypeKindS8)
      temp_lhs = type_new(TypeKindS16, NULL);

    Type *temp_rhs = rhs;
    if (temp_rhs->kind == TypeKindS8)
      temp_rhs = type_new(TypeKindS16, NULL);

    char *loc0 = get_dest_loc(dest, temp_lhs);
    char *loc1 = get_dest_loc(DestTemp3, temp_rhs);
    char *loc2 = get_dest_loc(DestReturn, temp_lhs);

    if (lhs->kind == TypeKindS8) {
      char *temp_loc0 = get_dest_loc(dest, lhs);
      fprintf(compiler->output_file, "  movzx %s,%s\n", loc0, temp_loc0);
      type_free(temp_lhs);
    }

    if (rhs->kind == TypeKindS8) {
      char *temp_loc1 = get_dest_loc(DestTemp3, rhs);
      char *temp_loc2 = get_dest_loc(DestReturn, rhs);
      fprintf(compiler->output_file, "  movzx %s,%s\n", loc1, temp_loc1);
      fprintf(compiler->output_file, "  movzx %s,%s\n", loc2, temp_loc2);
      type_free(temp_rhs);
    }

    fprintf(compiler->output_file, "  cmp %s,%s\n", loc0, loc1);

    if (label_index != (u32) -1) {
      if (type_is_signed(lhs)) {
        if (token->id == TT_EQ)
          fprintf(compiler->output_file, "  jne .l%u\n", label_index);
        else if (token->id == TT_NE)
          fprintf(compiler->output_file, "  je .l%u\n", label_index);
        else if (token->id == TT_LS)
          fprintf(compiler->output_file, "  jge .l%u\n", label_index);
        else if (token->id == TT_LE)
          fprintf(compiler->output_file, "  jg .l%u\n", label_index);
        else if (token->id == TT_GT)
          fprintf(compiler->output_file, "  jle .l%u\n", label_index);
        else if (token->id == TT_GE)
          fprintf(compiler->output_file, "  jl .l%u\n", label_index);
      } else {
        if (token->id == TT_EQ)
          fprintf(compiler->output_file, "  jne .l%u\n", label_index);
        else if (token->id == TT_NE)
          fprintf(compiler->output_file, "  je .l%u\n", label_index);
        else if (token->id == TT_LS)
          fprintf(compiler->output_file, "  jae .l%u\n", label_index);
        else if (token->id == TT_LE)
          fprintf(compiler->output_file, "  ja .l%u\n", label_index);
        else if (token->id == TT_GT)
          fprintf(compiler->output_file, "  jbe .l%u\n", label_index);
        else if (token->id == TT_GE)
          fprintf(compiler->output_file, "  jb .l%u\n", label_index);
      }
    } else {
      fprintf(compiler->output_file, "  mov %s,0\n", loc0);
      fprintf(compiler->output_file, "  mov %s,1\n", loc2);
      if (type_is_signed(lhs)) {
        if (token->id == TT_EQ)
          fprintf(compiler->output_file, "  cmove %s,%s\n", loc0, loc2);
        else if (token->id == TT_NE)
          fprintf(compiler->output_file, "  cmovne %s,%s\n", loc0, loc2);
        else if (token->id == TT_LS)
          fprintf(compiler->output_file, "  cmovl %s,%s\n", loc0, loc2);
        else if (token->id == TT_LE)
          fprintf(compiler->output_file, "  cmovle %s,%s\n", loc0, loc2);
        else if (token->id == TT_GT)
          fprintf(compiler->output_file, "  cmovg %s,%s\n", loc0, loc2);
        else if (token->id == TT_GE)
          fprintf(compiler->output_file, "  cmovge %s,%s\n", loc0, loc2);
      } else {
        if (token->id == TT_EQ)
          fprintf(compiler->output_file, "  cmove %s,%s\n", loc0, loc2);
        else if (token->id == TT_NE)
          fprintf(compiler->output_file, "  cmovne %s,%s\n", loc0, loc2);
        else if (token->id == TT_LS)
          fprintf(compiler->output_file, "  cmovb %s,%s\n", loc0, loc2);
        else if (token->id == TT_LE)
          fprintf(compiler->output_file, "  cmovbe %s,%s\n", loc0, loc2);
        else if (token->id == TT_GT)
          fprintf(compiler->output_file, "  cmova %s,%s\n", loc0, loc2);
        else if (token->id == TT_GE)
          fprintf(compiler->output_file, "  cmovae %s,%s\n", loc0, loc2);
      }
    }

    type_free(rhs);

    token = peek_token(parser);
    if (!token)
      return result;
  }

  if (found_cmp) {
    type_free(lhs);
    return result;
  }

  return lhs;
}

static Type *compile_expr(Parser *parser, Compiler *compiler, Dest dest) {
  bool was_return = dest == DestReturn;

  if (was_return)
    dest = DestTemp0;

  Type *type = compile_cmp_expr(parser, compiler, dest, (u32) -1, NULL);
  if (parser->has_error)
    return NULL;

  if (was_return) {
    char *loc0 = get_dest_loc(dest, type);
    char *loc1 = get_dest_loc(DestReturn, type);
    fprintf(compiler->output_file, "  mov %s,%s\n", loc1, loc0);
  }

  return type;
}

static void compile_instrs(Parser *parser, Compiler *compiler) {
  Token *token = NULL;
  while ((token = peek_token(parser)) &&
         token->id != TT_END &&
         token->id != TT_ELIF &&
         token->id != TT_ELSE) {
    expect_token(parser, "`let`, `ret`, `retval`, `if`, `while`, identifier or string",
                 MASK(TT_LET) | MASK(TT_RET) | MASK(TT_RETVAL) |
                 MASK(TT_IF) | MASK(TT_WHILE) | MASK(TT_IDENT) |
                 MASK(TT_STR));
    if (parser->has_error)
      return;

    if (token->id == TT_LET) {
      Token *name = expect_token(parser, "identifier", MASK(TT_IDENT));
      if (parser->has_error)
        return;

      expect_token(parser, "`=`", MASK(TT_ASSIGN));
      if (parser->has_error)
        return;

      Type *type = compile_expr(parser, compiler, DestTemp0);
      if (parser->has_error)
        return;

      Type *stack_type = get_type_on_stack(type);

      char *loc = get_dest_loc(DestTemp0, stack_type);
      fprintf(compiler->output_file, "  push %s\n", loc);

      compiler->stack_size += type_get_size(stack_type);

      type_free(stack_type);

      Var new_var = {
        name->lexeme,
        type,
        compiler->stack_size,
        false,
        false,
      };
      DA_APPEND(compiler->vars, new_var);
    } else if (token->id == TT_RET) {
      fprintf(compiler->output_file, "  jmp .end\n");
    } else if (token->id == TT_RETVAL) {
      Type *type = compile_expr(parser, compiler, DestReturn);
      if (parser->has_error)
        return;

      if (!type_eq(type, compiler->current_proc_return_type)) {
        parser->has_error = true;
        PERROR(STR_FMT":%u:%u: ", "Unexpected return type: ",
               STR_ARG(token->file_path),
               token->row + 1, token->col + 1);
        type_print(stderr, type);
        fprintf(stderr, "\n");
        return;
      }

      type_free(type);

      fprintf(compiler->output_file, "  jmp .end\n");
    } else if (token->id == TT_IDENT) {
      Token *next = expect_token(parser, "`=`, `(`, `:=` or `[`",
                                 MASK(TT_ASSIGN) | MASK(TT_OPAREN) |
                                 MASK(TT_CASSIGN) | MASK(TT_OBRACKET));
      if (parser->has_error)
        return;

      if (next->id == TT_ASSIGN) {
        Var *var = NULL;
        for (u32 i = compiler->vars.len; i > 0; --i) {
          if (str_eq(token->lexeme, compiler->vars.items[i - 1].name)) {
            var = compiler->vars.items + i - 1;
            break;
          }
        }

        if (!var) {
          for (u32 i = compiler->global_vars.len; i > 0; --i) {
            if (str_eq(token->lexeme, compiler->global_vars.items[i - 1].name)) {
              var = compiler->global_vars.items + i - 1;
              break;
            }
          }
        }

        if (!var) {
          parser->has_error = true;
          PERROR(STR_FMT":%u:%u: ", "Undeclared variable `"STR_FMT"`\n",
                 STR_ARG(token->file_path),
                 token->row + 1, token->col + 1,
                 STR_ARG(token->lexeme));
          return;
        }

        Type *type = compile_expr(parser, compiler, DestTemp0);
        if (parser->has_error)
          return;

        Type *stack_type = get_type_on_stack(type);

        char *loc = get_dest_loc(DestTemp0, stack_type);
        fprintf(compiler->output_file, "  mov ");
        print_var_loc(compiler->output_file, var);
        fprintf(compiler->output_file, ",%s\n", loc);

        type_free(type);
        type_free(stack_type);
      } else if (next->id == TT_OPAREN) {
        Type *type = compile_proc_call(parser, compiler, token);
        if (parser->has_error)
          return;

        type_free(type);
      } else if (next->id == TT_CASSIGN) {
        Var *var = get_var(parser, compiler, token);
        if (parser->has_error)
          return;

        if (var->type->kind != TypeKindPtr) {
          parser->has_error = true;
          PERROR(STR_FMT":%u:%u: ",
                 "Attempt to deref-assign to a non-pointer variable `"STR_FMT"`\n",
                 STR_ARG(token->file_path),
                 token->row + 1, token->col + 1,
                 STR_ARG(token->lexeme));
          return;
        }

        Type *type = compile_expr(parser, compiler, DestTemp0);
        if (parser->has_error)
          return;

        Type *stack_type = get_type_on_stack(type);

        char *loc = get_dest_loc(DestTemp0, stack_type);

        type_free(stack_type);

        fprintf(compiler->output_file, "  mov rax,");
        print_var_loc(compiler->output_file, var);
        fprintf(compiler->output_file, "\n");
        fprintf(compiler->output_file, "  mov [rax],%s\n", loc);
      } else if (next->id == TT_OBRACKET) {
        Type *index = compile_cmp_expr(parser, compiler, DestReturn,
                                     (u32) -1, NULL);
        if (parser->has_error)
          return;

        expect_token(parser, "`]`", MASK(TT_CBRACKET));
        if (parser->has_error)
          return;

        expect_token(parser, "`=`", MASK(TT_ASSIGN));
        if (parser->has_error)
          return;

        Type *type = compile_expr(parser, compiler, DestTemp0);
        if (parser->has_error)
          return;

        Var *var = get_var(parser, compiler, token);
        if (parser->has_error)
          return;

        if (var->type->kind != TypeKindPtr) {
          parser->has_error = true;
          PERROR(STR_FMT":%u:%u: ",
                 "Attempt to dereference a non-pointer value\n",
                 STR_ARG(token->file_path),
                 token->row + 1, token->col + 1);
          return;
        }

        if (!type_is_int(index)) {
          parser->has_error = true;
          PERROR(STR_FMT":%u:%u: ",
                 "Only integer can be used as an index\n",
                 STR_ARG(token->file_path),
                 token->row + 1, token->col + 1);
          return;
        }

        char *loc0 = get_dest_loc(DestTemp1, var->type);
        char *loc1 = get_dest_loc(DestReturn, index);
        char *loc2 = get_dest_loc(DestTemp0, type);
        u32 target_size = type_get_size(type);

        fprintf(compiler->output_file, "  mov %s,", loc0);
        print_var_loc(compiler->output_file, var);
        fprintf(compiler->output_file, "\n");
        fprintf(compiler->output_file, "  mov [%s+%s*%u],%s\n",
                loc0, loc1, target_size, loc2);
      }
    } else if (token->id == TT_STR) {
      fprintf(compiler->output_file, "  "STR_FMT,
              STR_ARG(STR(token->lexeme.ptr + 1, token->lexeme.len - 2)));

      Token *next = peek_token(parser);

      while (next && next->id == TT_COMMA) {
        next_token(parser);

        next = expect_token(parser, "string or identifier",
                            MASK(TT_STR) | MASK(TT_IDENT));
        if (parser->has_error)
          return;

        if (next->id == TT_STR) {
          fprintf(compiler->output_file, STR_FMT,
                  STR_ARG(STR(token->lexeme.ptr + 1, token->lexeme.len - 2)));
        } else {
          Var *var = get_var(parser, compiler, next);
          if (parser->has_error)
            return;

          print_var_loc(compiler->output_file, var);
        }

        next = peek_token(parser);
      }

      fprintf(compiler->output_file, "\n");
    } else if (token->id == TT_IF) {
      u32 label_index = compiler->labels_count++;
      u32 end_label_index = (u32) -1;
      bool found_comparison = false;

      Type *type = compile_cmp_expr(parser, compiler, DestTemp0,
                                    label_index, &found_comparison);
      if (parser->has_error)
        return;

      if (!found_comparison) {
        char *loc = get_dest_loc(DestTemp0, type);
        fprintf(compiler->output_file, "  cmp %s,0\n", loc);
        fprintf(compiler->output_file, "  je .l%u\n", label_index);
        found_comparison = false;
      }

      compile_instrs(parser, compiler);
      if (parser->has_error)
        return;

      Token *next = expect_token(parser, "`elif`, `else` or `end`",
                                 MASK(TT_ELIF) | MASK(TT_ELSE) | MASK(TT_END));
      if (parser->has_error)
        return;

      if (next->id != TT_END) {
        end_label_index = compiler->labels_count++;
        fprintf(compiler->output_file, "  jmp .l%u\n", end_label_index);
      }
      fprintf(compiler->output_file, ".l%u\n", label_index);

      while (next->id == TT_ELIF) {
        label_index = compiler->labels_count++;
        type = compile_cmp_expr(parser, compiler, DestTemp0,
                                label_index, &found_comparison);

        if (!found_comparison) {
          char *loc = get_dest_loc(DestTemp0, type);
          fprintf(compiler->output_file, "  cmp %s,0\n", loc);
          fprintf(compiler->output_file, "  je .l%u\n", label_index);
        }

        compile_instrs(parser, compiler);
        if (parser->has_error)
          return;

        next = expect_token(parser, "`elif`, `else` or `end`",
                            MASK(TT_ELIF) | MASK(TT_ELSE) | MASK(TT_END));
        if (parser->has_error)
          return;

        if (next->id != TT_END) {
          if (end_label_index == (u32) -1)
            end_label_index = compiler->labels_count++;
          fprintf(compiler->output_file, "  jmp .l%u\n", end_label_index);
        }
        fprintf(compiler->output_file, ".l%u\n", label_index);
      }

      if (next->id == TT_ELSE) {
        compile_instrs(parser, compiler);
        if (parser->has_error)
          return;

        if (end_label_index != (u32) -1)
          fprintf(compiler->output_file, ".l%u\n", end_label_index);

        next = expect_token(parser, "`end`", MASK(TT_END));
        if (parser->has_error)
          return;
      }
    } else if (token->id == TT_WHILE) {
      u32 begin_label_index = compiler->labels_count++;
      u32 end_label_index = compiler->labels_count++;
      u32 prev_stack_size = compiler->stack_size;
      bool found_comparison = false;

      fprintf(compiler->output_file, ".l%u\n", begin_label_index);

      Type *type = compile_cmp_expr(parser, compiler, DestTemp0,
                                    end_label_index, &found_comparison);
      if (parser->has_error)
        return;

      if (!found_comparison) {
        char *loc = get_dest_loc(DestTemp0, type);
        fprintf(compiler->output_file, "  cmp %s,0\n", loc);
        fprintf(compiler->output_file, "  je .l%u\n", end_label_index);
      }

      compile_instrs(parser, compiler);
      if (parser->has_error)
        return;

      expect_token(parser, "`end`", MASK(TT_END));
      if (parser->has_error)
        return;

      u32 diff = compiler->stack_size - prev_stack_size;
      if (diff > 0)
        fprintf(compiler->output_file, "  add rsp,%u\n", diff);

      fprintf(compiler->output_file, "  jmp .l%u\n", begin_label_index);
      fprintf(compiler->output_file, ".l%u\n", end_label_index);
    }
  }
}

static Proc compile_proc_decl(Parser *parser, Compiler *compiler) {
  Proc proc = {0};

  Token *name = expect_token(parser, "identifier", MASK(TT_IDENT));
  if (parser->has_error)
    return proc;

  proc.name = name->lexeme;

  expect_token(parser, "`(`", MASK(TT_OPAREN));
  if (parser->has_error)
    return proc;

  Token *token = peek_token(parser);
  bool first = true;

  while (token && token->id != TT_CPAREN) {
    if (first) {
      first = false;
    } else {
      expect_token(parser, "`,`", MASK(TT_COMMA));
      if (parser->has_error)
        break;
    }

    Token *param_name = expect_token(parser, "identifier", MASK(TT_IDENT));
    if (parser->has_error)
      break;

    expect_token(parser, "`:`", MASK(TT_COLON));
    if (parser->has_error)
      break;

    Type *param_type = compile_type(parser, compiler);
    if (parser->has_error)
      break;

    Param new_param = {
      param_name->lexeme,
      param_type,
    };
    DA_APPEND(proc.params, new_param);

    token = peek_token(parser);
  }

  if (parser->has_error) {
    if (proc.params.items)
      free(proc.params.items);
    return proc;
  }

  expect_token(parser, "`)`", MASK(TT_CPAREN));
  if (parser->has_error) {
    if (proc.params.items)
      free(proc.params.items);
    return proc;
  }

  token = peek_token(parser);
  if (token && token->id == TT_ARROW) {
    next_token(parser);
    proc.return_type = compile_type(parser, compiler);
  } else {
    proc.return_type = type_new(TypeKindUnit, NULL);
  }

  return proc;
}

static void cleanup(Parser *parser, Compiler *compiler, Strs *included_files) {
  for (u32 i = 0; i < parser->tokens.len; ++i)
    free(parser->tokens.items[i].lexeme.ptr);

  for (u32 i = 0; i < compiler->global_vars.len; ++i)
    type_free(compiler->global_vars.items[i].type);

  for (u32 i = 0; i < compiler->procs.len; ++i) {
    Proc *proc = compiler->procs.items + i;

    for (u32 j = 0; j < proc->params.len; ++j)
      type_free(proc->params.items[j].type);
    if (proc->params.items)
      free(proc->params.items);
    type_free(proc->return_type);
  }

  for (u32 i = 0; i < compiler->named_types.len; ++i)
    type_free(compiler->named_types.items[i].type);

  for (u32 i = 0; i < included_files->len; ++i)
    free(included_files->items[i].ptr);

  if (parser->tokens.items)
    free(parser->tokens.items);
  if (compiler->vars.items)
    free(compiler->vars.items);
  if (compiler->strs.items)
    free(compiler->strs.items);
  if (compiler->procs.items)
    free(compiler->procs.items);
  if (compiler->named_types.items)
    free(compiler->named_types.items);
  if (compiler->temp_sb.buffer)
    free(compiler->temp_sb.buffer);
  if (included_files->items)
    free(included_files->items);
}

Str get_file_dir(Str path) {
  while (path.len > 0 && path.ptr[path.len - 1] != '/')
    --path.len;

  return path;
}

bool compile(Str code, Str file_path, FILE *output_file) {
  StringBuilder temp_sb = {0};
  Strs included_files = {0};

  Parser parser = { code, 0, {}, false };
  bool success = lex(&parser.tokens, code, file_path, &temp_sb);
  if (!success)
    return false;

  Compiler compiler = {0};
  compiler.output_file = output_file;
  compiler.temp_sb = temp_sb;

  NamedType new_named_type = {
    STR_LIT("s8"),
    type_new(TypeKindS8, NULL),
  };
  DA_APPEND(compiler.named_types, new_named_type);
  new_named_type = (NamedType) {
    STR_LIT("s16"),
    type_new(TypeKindS16, NULL),
  };
  DA_APPEND(compiler.named_types, new_named_type);
  new_named_type = (NamedType) {
    STR_LIT("s32"),
    type_new(TypeKindS32, NULL),
  };
  DA_APPEND(compiler.named_types, new_named_type);
  new_named_type = (NamedType) {
    STR_LIT("s64"),
    type_new(TypeKindS64, NULL),
  };
  DA_APPEND(compiler.named_types, new_named_type);

  fprintf(output_file, "section '.text'\n");
  fprintf(output_file, "global _start\n");
  fprintf(output_file, "_start:\n");
  fprintf(output_file, "  mov rbx,qword[rsp]\n");
  fprintf(output_file, "  push rbx\n");
  fprintf(output_file, "  lea rbx,qword[rsp+16]\n");
  fprintf(output_file, "  push rbx\n");
  fprintf(output_file, "  call $main\n");
  fprintf(output_file, "  mov rdi,rax\n");
  fprintf(output_file, "  mov rax,60\n");
  fprintf(output_file, "  syscall\n");

  Token *token = NULL;
  while ((token = peek_token(&parser))) {
    expect_token(&parser, "`proc`, `extern`, `let` or `include`",
                 MASK(TT_PROC) | MASK(TT_EXTERN) |
                 MASK(TT_LET) | MASK(TT_INCLUDE));
    if (parser.has_error) {
      cleanup(&parser, &compiler, &included_files);
      return false;
    }

    if (token->id == TT_PROC) {
      Proc new_proc = compile_proc_decl(&parser, &compiler);
      if (parser.has_error) {
        cleanup(&parser, &compiler, &included_files);
        return false;
      }

      DA_APPEND(compiler.procs, new_proc);

      fprintf(output_file, "global $"STR_FMT"\n", STR_ARG(new_proc.name));
      fprintf(output_file, "$"STR_FMT":\n", STR_ARG(new_proc.name));
      fprintf(output_file, "  push rbp\n");
      fprintf(output_file, "  mov rbp,rsp\n");
      fprintf(output_file, "  push rbx\n");
      fprintf(output_file, "  push r12\n");
      fprintf(output_file, "  push r13\n");
      fprintf(output_file, "  push r14\n");
      fprintf(output_file, "  push r15\n");

      compiler.stack_size = 0;
      compiler.labels_count = 0;
      compiler.current_proc_return_type = new_proc.return_type;

      u32 params_stack_size = 0;

      for (u32 i = new_proc.params.len; i > 0; --i) {
        Param *param = new_proc.params.items + i - 1;

        Var new_var = {
          param->name,
          type_clone(param->type),
          params_stack_size,
          true,
          false,
        };
        DA_APPEND(compiler.vars, new_var);

        params_stack_size += type_get_size(get_type_on_stack(param->type));
      }

      compile_instrs(&parser, &compiler);

      for (u32 i = 0; i < compiler.vars.len; ++i)
        type_free(compiler.vars.items[i].type);
      compiler.vars.len = 0;

      if (compiler.stack_size > 0)
        fprintf(output_file, "  add rsp,%u\n", compiler.stack_size);

      if (parser.has_error) {
        cleanup(&parser, &compiler, &included_files);
        return false;
      }

      fprintf(output_file, ".end:\n");
      fprintf(output_file, "  pop r15\n");
      fprintf(output_file, "  pop r14\n");
      fprintf(output_file, "  pop r13\n");
      fprintf(output_file, "  pop r12\n");
      fprintf(output_file, "  pop rbx\n");
      fprintf(output_file, "  leave\n");
      fprintf(output_file, "  ret\n");

      expect_token(&parser, "`end`", MASK(TT_END));
      if (parser.has_error) {
        cleanup(&parser, &compiler, &included_files);
        return false;
      }
    } else if (token->id == TT_EXTERN) {
      expect_token(&parser, "`proc`", MASK(TT_PROC));
      if (parser.has_error) {
        cleanup(&parser, &compiler, &included_files);
        return false;
      }

      Proc new_proc = compile_proc_decl(&parser, &compiler);
      if (parser.has_error) {
        cleanup(&parser, &compiler, &included_files);
        return false;
      }

      DA_APPEND(compiler.procs, new_proc);

      fprintf(output_file, "extern $"STR_FMT"\n", STR_ARG(new_proc.name));
    } else if (token->id == TT_LET) {
      Token *name = expect_token(&parser, "identifier", MASK(TT_IDENT));
      if (parser.has_error) {
        cleanup(&parser, &compiler, &included_files);
        return false;
      }

      expect_token(&parser, "`:`", MASK(TT_COLON));
      if (parser.has_error) {
        cleanup(&parser, &compiler, &included_files);
        return false;
      }

      Type *type = compile_type(&parser, &compiler);
      if (parser.has_error) {
        cleanup(&parser, &compiler, &included_files);
        return false;
      }

      for (u32 i = 0; i < compiler.global_vars.len; ++i) {
        if (str_eq(compiler.global_vars.items[i].name, name->lexeme)) {
          parser.has_error = true;
          PERROR(STR_FMT":%u:%u: ",
                 "Global variable `"STR_FMT"` redefined\n",
                 STR_ARG(token->file_path),
                 token->row + 1, token->col + 1,
                 STR_ARG(name->lexeme));
          cleanup(&parser, &compiler, &included_files);
          return false;
        }
      }

      Var new_var = {
        name->lexeme,
        type,
        0,
        false,
        true,
      };
      DA_APPEND(compiler.global_vars, new_var);
    } else if (token->id == TT_INCLUDE) {
      Token *path = expect_token(&parser, "string", MASK(TT_STR));
      if (parser.has_error) {
        cleanup(&parser, &compiler, &included_files);
        return false;
      }

      Str current_dir = get_file_dir(token->file_path);
      Str path_str;
      path_str.len = current_dir.len + path->lexeme.len - 2;
      path_str.ptr = malloc(path_str.len);
      memcpy(path_str.ptr, current_dir.ptr, current_dir.len);
      memcpy(path_str.ptr + current_dir.len,
             path->lexeme.ptr + 1,
             path->lexeme.len - 2);

      bool already_included = false;

      for (u32 i = 0; i < included_files.len; ++i) {
        if (str_eq(included_files.items[i], path_str)) {
          already_included = true;
          break;
        }
      }

      if (already_included) {
        free(path_str.ptr);
        continue;
      }

      Str temp_code = read_file_str(path_str);
      if (temp_code.ptr == NULL) {
        ERROR("Could not open "STR_FMT"\n", STR_ARG(path_str));
        cleanup(&parser, &compiler, &included_files);
        return false;
      }

      Tokens temp_tokens = {0};
      bool success = lex(&temp_tokens, temp_code, path_str, &temp_sb);
      if (!success) {
        cleanup(&parser, &compiler, &included_files);
        return false;
      }

      if (parser.tokens.cap < parser.tokens.len + temp_tokens.len) {
        parser.tokens.cap = parser.tokens.len + temp_tokens.len;
        parser.tokens.items =
          realloc(parser.tokens.items, parser.tokens.cap * sizeof(Token));
      }
      memmove(parser.tokens.items + parser.pos + temp_tokens.len,
              parser.tokens.items + parser.pos,
              (parser.tokens.len - parser.pos) * sizeof(Token));
      memcpy(parser.tokens.items + parser.pos,
             temp_tokens.items,
             temp_tokens.len * sizeof(Token));

      parser.tokens.len += temp_tokens.len;

      DA_APPEND(included_files, path_str);

      free(temp_code.ptr);
      if (temp_tokens.items)
        free(temp_tokens.items);
    }
  }

  if (compiler.strs.len > 0)
    fprintf(output_file, "section '.data'\n");

  for (u32 i = 0; i < compiler.strs.len; ++i) {
    Str *str = compiler.strs.items + i;
    bool is_escaped = false;
    u32 chars_emitted = 0;

    fprintf(output_file, "str@%u: db ", i);
    for (u32 j = 1; j + 1 < str->len; ++j) {
      char ch = str->ptr[j];
      if (is_escaped) {
        is_escaped = false;
        switch (ch) {
        case 'n': ch = '\n'; break;
        case 'r': ch = '\r'; break;
        case 't': ch = '\t'; break;
        case 'v': ch = '\v'; break;
        case 'b': ch = '\b'; break;
        case 'e': ch = '\e'; break;
        case '0': ch = '\0'; break;
        }
      } else if (ch == '\\') {
        is_escaped = true;
        continue;
      }

      if (chars_emitted > 0)
        fprintf(output_file, ",");
      fprintf(output_file, "%u", ch);
      ++chars_emitted;
    }
    if (str->len > 2)
      fprintf(output_file, ",");
    fprintf(output_file, "0\n");
  }

  if (compiler.global_vars.len > 0)
    fprintf(output_file, "section '.bss'\n");

  for (u32 i = 0; i < compiler.global_vars.len; ++i) {
    Var *var = compiler.global_vars.items + i;

    fprintf(output_file, "$"STR_FMT": resb %u\n",
            STR_ARG(var->name),
            type_get_size(var->type));
  }

  cleanup(&parser, &compiler, &included_files);

  return true;
}
