#include <stdio.h>

#include "shl/shl-log.h"
#include "lexgen/runtime.h"
#include "grammar.h"
#include "compiler.h"
#include "lexer.h"
#include "type.h"

#define MASK(id) (1lu << (id))

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

typedef struct {
  Vars           vars;
  Strs           strs;
  Procs          procs;
  NamedTypes     named_types;
  u32            stack_size;
  FILE          *output_file;
  StringBuilder  temp_sb;
} Compiler;

typedef enum {
  DestReturn = 0,
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
    case TypeKindS8:   return "ax";
    case TypeKindS16:  return "ax";
    case TypeKindS32:  return "rax";
    case TypeKindS64:  return "rax";
    case TypeKindPtr:  return "rax";
    }
  }

  return NULL;
}

static u32 get_size_on_stack(u32 size) {
  switch (size) {
  case 1:  return 2;
  case 4:  return 8;
  default: return size;
  }
}

static Type *compile_expr(Parser *parser, Compiler *compiler, Dest dest);

static Type *compile_proc_call(Parser *parser, Compiler *compiler, Token *name) {
  Da(Type *) param_types = {0};
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

    Type *type = compile_expr(parser, compiler, DestReturn);
    char *loc = get_dest_loc(DestReturn, type);
    fprintf(compiler->output_file, "  push %s\n", loc);

    token = peek_token(parser);
    DA_APPEND(param_types, type);
    params_size += get_size_on_stack(get_type_size(type));
  }

  expect_token(parser, "`)`", MASK(TT_CPAREN));
  if (parser->has_error)
    return NULL;

  for (u32 i = 0; i < compiler->procs.len; ++i) {
    Proc *proc = compiler->procs.items + i;

    if (str_eq(proc->name, name->lexeme)) {
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

      if (found) {
        fprintf(compiler->output_file, "  call $"STR_FMT"\n", STR_ARG(name->lexeme));
        if (params_size > 0)
          fprintf(compiler->output_file, "  add rsp,%u\n", params_size);
        return proc->return_type;
      }
    }
  }

  parser->has_error = true;
  PERROR(STR_FMT":%u:%u: ", "Undeclared procedure `"STR_FMT"(",
         STR_ARG(parser->file_path),
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
          fprintf(stderr, ", ");
        type_print(stdout, proc->params.items[i].type);
      }
      printf(")\n");
    }
  }

  return NULL;
}

static Type *compile_expr(Parser *parser, Compiler *compiler, Dest dest) {
  Token *token = expect_token(parser, "identifier, integer or string",
                              MASK(TT_INT) | MASK(TT_STR) | MASK(TT_IDENT));
  if (parser->has_error)
    return NULL;

  if (token->id == TT_INT) {
    Type *type = type_new(TypeKindS32, NULL);
    char *loc = get_dest_loc(dest, type);
    fprintf(compiler->output_file, "  mov %s, "STR_FMT"\n", loc, STR_ARG(token->lexeme));
    free(type);

    return type_new(TypeKindS32, NULL);
  } else if (token->id == TT_STR) {
    Type *type = type_new(TypeKindPtr, NULL);
    char *loc = get_dest_loc(dest, type);
    fprintf(compiler->output_file, "  lea %s, [str@%u]\n", loc, compiler->strs.len);
    free(type);

    sb_push(&compiler->temp_sb, "str@");
    sb_push_u32(&compiler->temp_sb, compiler->strs.len);

    Str str_loc = sb_to_str(compiler->temp_sb);
    DA_APPEND(compiler->strs, str_loc);

    compiler->temp_sb.len = 0;

    return type_new(TypeKindPtr, type_new(TypeKindS8, NULL));
  } else if (token->id == TT_IDENT) {
    Token *next = peek_token(parser);
    if (next && next->id == TT_OPAREN) {
      return compile_proc_call(parser, compiler, token);
    } else {
      for (u32 i = compiler->vars.len; i > 0; --i) {
        if (str_eq(token->lexeme, compiler->vars.items[i - 1].name)) {
          Var *var = compiler->vars.items + i - 1;
          char *loc = get_dest_loc(dest, var->type);
          fprintf(compiler->output_file, "  mov %s,[rbp-%u]\n", loc, var->offset);

          return var->type;
        }
      }

      parser->has_error = true;
      PERROR(STR_FMT":%u:%u: ", "Undeclared variable `"STR_FMT"`\n",
             STR_ARG(parser->file_path),
             token->row + 1, token->col + 1,
             STR_ARG(token->lexeme));
      return NULL;
    }
  }

  return NULL;
}

static void compile_instrs(Parser *parser, Compiler *compiler) {
  Token *token = NULL;
  while ((token = peek_token(parser)) && token->id != TT_END) {
    expect_token(parser, "`let`, `ret`, `retval` or identifier",
                 MASK(TT_LET) | MASK(TT_RET) | MASK(TT_RETVAL) | MASK(TT_IDENT));
    if (parser->has_error)
      return;

    if (token->id == TT_LET) {
      Token *name = expect_token(parser, "identifier", MASK(TT_IDENT));
      if (parser->has_error)
        return;

      expect_token(parser, "`=`", MASK(TT_ASSIGN));
      if (parser->has_error)
        return;

      Type *type = compile_expr(parser, compiler, DestReturn);
      if (parser->has_error)
        return;

      char *loc = get_dest_loc(DestReturn, type);
      fprintf(compiler->output_file, "  push %s\n", loc);

      compiler->stack_size += get_size_on_stack(get_type_size(type));

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
      compile_expr(parser, compiler, DestReturn);
      if ((token = peek_token(parser)) && token->id != TT_END)
        fprintf(compiler->output_file, "  jmp .end\n");
    } else if (token->id == TT_IDENT) {
      Token *next = expect_token(parser, "`=` or `(`", MASK(TT_ASSIGN) | MASK(TT_OPAREN));
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

        Type *type = compile_expr(parser, compiler, DestReturn);
        if (parser->has_error)
          return;

        char *loc = get_dest_loc(DestReturn, type);
        fprintf(compiler->output_file, "  mov [rbp-%u],%s\n", var->offset, loc);
      } else if (next && next->id == TT_OPAREN) {
        compile_proc_call(parser, compiler, token);
        if (parser->has_error)
          return;
      }
    }
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
      return compiler->named_types.items[i].type;

  parser->has_error = true;
  PERROR(STR_FMT":%u:%u: ", "Undeclared type `"STR_FMT"`\n",
         STR_ARG(parser->file_path),
         token->row + 1, token->col + 1,
         STR_ARG(token->lexeme));

  return NULL;
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
        return proc;
    }

    Token *param_name = expect_token(parser, "identifier", MASK(TT_IDENT));
    if (parser->has_error)
      return proc;

    expect_token(parser, "`:`", MASK(TT_COLON));
    if (parser->has_error)
      return proc;

    Type *param_type = compile_type(parser, compiler);
    if (parser->has_error)
      return proc;

    Param new_param = {
      param_name->lexeme,
      param_type,
    };
    DA_APPEND(proc.params, new_param);

    token = peek_token(parser);
  }

  expect_token(parser, "`)`", MASK(TT_CPAREN));
  if (parser->has_error)
    return proc;

  token = peek_token(parser);
  if (token && token->id == TT_ARROW) {
    next_token(parser);
    proc.return_type = compile_type(parser, compiler);
  } else {
    proc.return_type = type_new(TypeKindUnit, NULL);
  }

  return proc;
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
  fprintf(output_file, "  mov rdi,qword[rsp]\n");
  fprintf(output_file, "  lea rsi,qword[rsp+8]\n");
  fprintf(output_file, "  call main\n");
  fprintf(output_file, "  mov rdi,rax\n");
  fprintf(output_file, "  mov rax,60\n");
  fprintf(output_file, "  syscall\n");

  Token *token = NULL;
  while ((token = peek_token(&parser))) {
    expect_token(&parser, "procedure", MASK(TT_PROC) | MASK(TT_EXTERN));
    if (parser.has_error)
      return false;

    if (token->id == TT_PROC) {
      Proc new_proc = compile_proc_decl(&parser, &compiler);
      if (parser.has_error)
        return false;

      DA_APPEND(compiler.procs, new_proc);

      fprintf(output_file, "global $"STR_FMT"\n", STR_ARG(new_proc.name));
      fprintf(output_file, "$"STR_FMT":\n", STR_ARG(new_proc.name));
      fprintf(output_file, "  push rbp\n");
      fprintf(output_file, "  mov rbp,rsp\n");

      compile_instrs(&parser, &compiler);
      if (parser.has_error)
        return false;

      fprintf(output_file, ".end:\n");
      fprintf(output_file, "  leave\n");
      fprintf(output_file, "  ret\n");

      expect_token(&parser, "`end`", MASK(TT_END));
      if (parser.has_error)
        return false;
    } else if (token->id == TT_EXTERN) {
      expect_token(&parser, "`proc`", MASK(TT_PROC));
      if (parser.has_error)
        return false;

      Proc new_proc = compile_proc_decl(&parser, &compiler);
      if (parser.has_error)
        return false;

      DA_APPEND(compiler.procs, new_proc);

      fprintf(output_file, "extern $"STR_FMT"\n", STR_ARG(new_proc.name));
    }
  }

  if (compiler.strs.len > 0)
    fprintf(output_file, "section '.data'\n");

  for (u32 i = 0; i < compiler.strs.len; ++i) {
    Str *str = compiler.strs.items + i;

    fprintf(output_file, "str@%u: db ", i);
    for (u32 j = 1; j + 1 < str->len; ++j) {
      if (j > 1)
        fprintf(output_file, ",");
      fprintf(output_file, "%u", str->ptr[j]);
    }
    fprintf(output_file, ",0\n");
  }

  for (u32 i = 0; i < tokens.len; ++i)
    free(tokens.items[i].lexeme.ptr);

  if (tokens.items)
    free(tokens.items);
  if (compiler.vars.items)
    free(compiler.vars.items);
  if (compiler.strs.items)
    free(compiler.strs.items);
  if (compiler.procs.items)
    free(compiler.procs.items);
  if (compiler.named_types.items)
    free(compiler.named_types.items);
  if (temp_sb.buffer)
    free(temp_sb.buffer);

  return true;
}

// TODO: fix type memory leaks
