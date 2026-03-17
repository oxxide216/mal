#include <stdio.h>

#include "shl/shl-log.h"
#include "lexgen/runtime.h"
#include "grammar.h"
#include "compiler.h"
#include "lexer.h"
#include "type.h"

#define MASK(id) (1lu << (id))

#define CALLE_PRESERVED_REGS_SIZE 24

typedef struct {
  Str    code;
  u16    pos;
  Tokens tokens;
  Str    file_path;
  bool   has_error;
} Parser;

typedef struct {
  Str   name;
  Type *type;
  u32   offset;
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
  Strs           strs;
  Procs          procs;
  NamedTypes     named_types;
  Types          param_types;
  u32            stack_size;
  FILE          *output_file;
  Type          *current_proc_return_type;
  StringBuilder  temp_sb;
} Compiler;

typedef enum {
  DestReturn = 0,
  DestTemp0,
  DestTemp1,
  DestTemp2,
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
         STR_ARG(parser->file_path),
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
         STR_ARG(parser->file_path),
         token->row + 1, token->col + 1,
         STR_ARG(token->lexeme));

  return NULL;
}

static Type *compile_expr(Parser *parser, Compiler *compiler, Dest dest);

static Type *compile_proc_call(Parser *parser, Compiler *compiler, Token *name) {
  for (u32 i = 0; i < compiler->param_types.len; ++i)
    type_free(compiler->param_types.items[i]);

  compiler->param_types.len = 0;
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
    DA_APPEND(compiler->param_types, type);
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

    if (proc->params.len != compiler->param_types.len)
      continue;

    bool found = true;

    for (u32 j = 0; j < proc->params.len; ++j) {
      Param *param = proc->params.items + j;

      if (!type_eq(param->type, compiler->param_types.items[j])) {
        found = false;
        break;
      }
    }

    if (!found)
      continue;

    fprintf(compiler->output_file, "  call $"STR_FMT"\n", STR_ARG(name->lexeme));
    if (params_size > 0)
      fprintf(compiler->output_file, "  add rsp,%u\n", params_size);

    return proc->return_type;
  }

  parser->has_error = true;
  PERROR(STR_FMT":%u:%u: ", "Undeclared procedure `"STR_FMT"(",
         STR_ARG(parser->file_path),
         name->row + 1, name->col + 1,
         STR_ARG(name->lexeme));
  for (u32 i = 0; i < compiler->param_types.len; ++i) {
    if (i > 0)
      fprintf(stderr, ", ");
    type_print(stderr, compiler->param_types.items[i]);
  }
  fprintf(stderr, ")`\n");

  INFO("Available procedures with this name are:\n");
  for (u32 i = 0; i < compiler->procs.len; ++i) {
    Proc *proc = compiler->procs.items + i;

    if (str_eq(proc->name, name->lexeme)) {
      printf("        "STR_FMT"(", STR_ARG(proc->name));
      for (u32 i = 0; i < proc->params.len; ++i) {
        if (i > 0)
          fprintf(stderr, ", ");
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

  parser->has_error = true;
  PERROR(STR_FMT":%u:%u: ", "Undeclared variable `"STR_FMT"`\n",
         STR_ARG(parser->file_path),
         name->row + 1, name->col + 1,
         STR_ARG(name->lexeme));
  return NULL;
}

static Type *compile_add_expr(Parser *parser, Compiler *compiler, Dest dest);

static Type *_compile_primary_expr(Parser *parser, Compiler *compiler, Dest dest) {
  Token *token = peek_token(parser);
  if (token->id == TT_AMP || token->id == TT_STAR) {
    next_token(parser);

    if (token->id == TT_AMP) {
      token = expect_token(parser, "identifier", MASK(TT_IDENT));

      Var *var = get_var(parser, compiler, token);
      if (parser->has_error)
        return NULL;

      Type *ptr_type = type_new(TypeKindPtr, type_clone(var->type));

      char *loc = get_dest_loc(dest, ptr_type);
      fprintf(compiler->output_file, "  lea %s,[rbp-%u]\n",
              loc, var->offset + CALLE_PRESERVED_REGS_SIZE);

      return ptr_type;
    } else if (token->id == TT_STAR) {
      token = expect_token(parser, "identifier", MASK(TT_IDENT));

      Var *var = get_var(parser, compiler, token);
      if (parser->has_error)
        return NULL;

      if (var->type->kind != TypeKindPtr) {
        parser->has_error = true;
        PERROR(STR_FMT":%u:%u: ",
               "Attempt to dereference a non-pointer variable `"STR_FMT"`\n",
               STR_ARG(parser->file_path),
               token->row + 1, token->col + 1,
               STR_ARG(token->lexeme));
        return NULL;
      }

      fprintf(compiler->output_file, "  mov rax,[rbp-%u]\n",
              var->offset + CALLE_PRESERVED_REGS_SIZE);
      fprintf(compiler->output_file, "  mov rax,[rax]\n");

      if (dest != DestReturn) {
        char *loc = get_dest_loc(dest, var->type);
        fprintf(compiler->output_file, "  mov %s,rax\n", loc);
      }

      return type_clone(var->type->target);
    }

    token = peek_token(parser);
  }

  token = expect_token(parser, "identifier, integer, string, `(`, `&` or `*`",
                       MASK(TT_INT) | MASK(TT_STR) |
                       MASK(TT_IDENT) | MASK(TT_OPAREN));
  if (parser->has_error)
    return NULL;

  if (token->id == TT_INT) {
    Type *type = type_new(TypeKindS32, NULL);
    char *loc = get_dest_loc(dest, type);
    fprintf(compiler->output_file, "  mov %s, "STR_FMT"\n", loc, STR_ARG(token->lexeme));

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
      return compile_proc_call(parser, compiler, token);
    } else {
      Var *var = get_var(parser, compiler, token);
      if (parser->has_error)
        return NULL;

      char *loc = get_dest_loc(dest, var->type);
      fprintf(compiler->output_file, "  mov %s,[rbp-%u]\n",
              loc, var->offset + CALLE_PRESERVED_REGS_SIZE);

      return type_clone(var->type);
    }
  } else if (token->id == TT_OPAREN) {
    Type *type = compile_add_expr(parser, compiler, dest);
    expect_token(parser, "`)`", MASK(TT_CPAREN));
    return type;
  }

  return NULL;
}

static Type *compile_primary_expr(Parser *parser, Compiler *compiler, Dest dest) {
  Type *type = _compile_primary_expr(parser, compiler, dest);

  Token *token = peek_token(parser);
  while (token && token->id == TT_AS) {
    next_token(parser);

    Type *new_type = compile_type(parser, compiler);

    if (!types_can_cast(type, new_type)) {
      parser->has_error = true;
      PERROR(STR_FMT":%u:%u: ", "Cannot cast ",
             STR_ARG(parser->file_path),
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
             STR_ARG(parser->file_path),
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
             STR_ARG(parser->file_path),
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

    if (token->id == TT_PLUS)
      fprintf(compiler->output_file, "  add %s,%s\n", loc0, loc1);
    else
      fprintf(compiler->output_file, "  sub %s,%s\n", loc0, loc1);

    token = peek_token(parser);
    if (!token)
      return lhs;
  }

  return lhs;
}

static Type *compile_expr(Parser *parser, Compiler *compiler, Dest dest) {
  bool was_return = dest == DestReturn;

  if (was_return)
    dest = DestTemp0;

  Type *type = compile_add_expr(parser, compiler, dest);

  if (was_return) {
    char *loc0 = get_dest_loc(dest, type);
    char *loc1 = get_dest_loc(DestReturn, type);
    fprintf(compiler->output_file, "  mov %s,%s\n", loc1, loc0);
  }

  return type;
}

static void compile_instrs(Parser *parser, Compiler *compiler) {
  Token *token = NULL;
  while ((token = peek_token(parser)) && token->id != TT_END) {
    expect_token(parser, "`let`, `ret`, `retval`, identifier or string",
                 MASK(TT_LET) | MASK(TT_RET) | MASK(TT_RETVAL) |
                 MASK(TT_IDENT) | MASK(TT_STR));
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
      };
      DA_APPEND(compiler->vars, new_var);
    } else if (token->id == TT_RET) {
      if ((token = peek_token(parser)) && token->id != TT_END)
        fprintf(compiler->output_file, "  jmp .end\n");
    } else if (token->id == TT_RETVAL) {
      Type *type = compile_expr(parser, compiler, DestReturn);

      if (!type_eq(type, compiler->current_proc_return_type)) {
        parser->has_error = true;
        PERROR(STR_FMT":%u:%u: ", "Unexpected return type: ",
               STR_ARG(parser->file_path),
               token->row + 1, token->col + 1);
        type_print(stderr, type);
        fprintf(stderr, "\n");
        return;
      }

      type_free(type);

      if ((token = peek_token(parser)) && token->id != TT_END)
        fprintf(compiler->output_file, "  jmp .end\n");
    } else if (token->id == TT_IDENT) {
      Token *next = expect_token(parser, "`=`, `(` or `:=`",
                                 MASK(TT_ASSIGN) | MASK(TT_OPAREN) |
                                 MASK(TT_CASSIGN));
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
          parser->has_error = true;
          PERROR(STR_FMT":%u:%u: ", "Undeclared variable `"STR_FMT"`\n",
                 STR_ARG(parser->file_path),
                 token->row + 1, token->col + 1,
                 STR_ARG(token->lexeme));
          return;
        }

        Type *type = compile_expr(parser, compiler, DestTemp0);
        if (parser->has_error)
          return;

        Type *stack_type = get_type_on_stack(type);

        char *loc = get_dest_loc(DestTemp0, stack_type);
        fprintf(compiler->output_file, "  mov [rbp-%u],%s\n",
                var->offset + CALLE_PRESERVED_REGS_SIZE, loc);

        type_free(type);
        type_free(stack_type);
      } else if (next->id == TT_OPAREN) {
        type_free(compile_proc_call(parser, compiler, token));
        if (parser->has_error)
          return;
      } else if (next->id == TT_CASSIGN) {
        Var *var = get_var(parser, compiler, token);
        if (parser->has_error)
          return;

        if (var->type->kind != TypeKindPtr) {
          parser->has_error = true;
          PERROR(STR_FMT":%u:%u: ",
                 "Attempt to deref-assign to a non-pointer variable `"STR_FMT"`\n",
                 STR_ARG(parser->file_path),
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

        fprintf(compiler->output_file, "  mov rax,[rbp-%u]\n",
                var->offset + CALLE_PRESERVED_REGS_SIZE);
        fprintf(compiler->output_file, "  mov [rax],%s\n", loc);
      }
    } else if (token->id == TT_STR) {
      fprintf(compiler->output_file, "  "STR_FMT"\n",
              STR_ARG(STR(token->lexeme.ptr + 1, token->lexeme.len - 2)));
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

static void cleanup(Parser *parser, Compiler *compiler) {
  for (u32 i = 0; i < parser->tokens.len; ++i)
    free(parser->tokens.items[i].lexeme.ptr);

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

  for (u32 i = 0; i < compiler->param_types.len; ++i)
    type_free(compiler->param_types.items[i]);

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
  if (compiler->param_types.items)
    free(compiler->param_types.items);
  if (compiler->temp_sb.buffer)
    free(compiler->temp_sb.buffer);
}

bool compile(Str code, Str file_path, FILE *output_file) {
  StringBuilder temp_sb = {0};
  Tokens tokens = lex(code, file_path, &temp_sb);
  Parser parser = { code, 0, tokens, file_path, false };
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
  fprintf(output_file, "  lea rbx,qword[rsp+8]\n");
  fprintf(output_file, "  push rbx\n");
  fprintf(output_file, "  call $main\n");
  fprintf(output_file, "  mov rdi,rax\n");
  fprintf(output_file, "  mov rax,60\n");
  fprintf(output_file, "  syscall\n");

  Token *token = NULL;
  while ((token = peek_token(&parser))) {
    expect_token(&parser, "procedure", MASK(TT_PROC) | MASK(TT_EXTERN));
    if (parser.has_error) {
      cleanup(&parser, &compiler);
      return false;
    }

    if (token->id == TT_PROC) {
      Proc new_proc = compile_proc_decl(&parser, &compiler);
      if (parser.has_error) {
        cleanup(&parser, &compiler);
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

      compiler.current_proc_return_type = new_proc.return_type;

      compile_instrs(&parser, &compiler);

      for (u32 i = 0; i < compiler.vars.len; ++i)
        type_free(compiler.vars.items[i].type);
      compiler.vars.len = 0;

      if (compiler.stack_size > 0)
        fprintf(output_file, "  add rsp,%u\n", compiler.stack_size);

      if (parser.has_error) {
        cleanup(&parser, &compiler);
        return false;
      }

      fprintf(output_file, ".end:\n");
      fprintf(output_file, "  pop r13\n");
      fprintf(output_file, "  pop r12\n");
      fprintf(output_file, "  pop rbx\n");
      fprintf(output_file, "  leave\n");
      fprintf(output_file, "  ret\n");

      expect_token(&parser, "`end`", MASK(TT_END));
      if (parser.has_error) {
        cleanup(&parser, &compiler);
        return false;
      }
    } else if (token->id == TT_EXTERN) {
      expect_token(&parser, "`proc`", MASK(TT_PROC));
      if (parser.has_error) {
        cleanup(&parser, &compiler);
        return false;
      }

      Proc new_proc = compile_proc_decl(&parser, &compiler);
      if (parser.has_error) {
        cleanup(&parser, &compiler);
        return false;
      }

      DA_APPEND(compiler.procs, new_proc);

      fprintf(output_file, "extern $"STR_FMT"\n", STR_ARG(new_proc.name));
    }
  }

  if (compiler.strs.len > 0)
    fprintf(output_file, "section '.data'\n");

  for (u32 i = 0; i < compiler.strs.len; ++i) {
    Str *str = compiler.strs.items + i;
    bool is_escaped = false;

    fprintf(output_file, "str@%u: db ", i);
    for (u32 j = 1; j + 1 < str->len; ++j) {
      char ch = str->ptr[j];
      if (is_escaped) {
        switch (ch) {
        case 'n': ch = '\n'; break;
        case 'r': ch = '\r'; break;
        case 't': ch = '\t'; break;
        case 'v': ch = '\v'; break;
        case 'b': ch = '\b'; break;
        case '0': ch = '\0'; break;
        }
      } else if (ch == '\\') {
        is_escaped = true;
        continue;
      }

      if (j > 1)
        fprintf(output_file, ",");
      fprintf(output_file, "%u", ch);
    }
    fprintf(output_file, ",0\n");
  }

  cleanup(&parser, &compiler);

  return true;
}
