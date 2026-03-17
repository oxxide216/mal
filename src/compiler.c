#include <stdio.h>

#include "shl/shl-log.h"
#include "lexgen/runtime.h"
#include "grammar.h"
#include "compiler.h"
#include "lexer.h"

#define MASK(id) (1lu << (id))

typedef struct {
  Str    code;
  u16    pos;
  Tokens tokens;
  Str    file_path;
  bool   has_error;
} Parser;

typedef struct {
  Str name;
  u32 size;
  u32 offset;
} Var;

typedef Da(Var) Vars;

typedef Da(Str) Strs;

typedef struct {
  Str name;
} Proc;

typedef Da(Proc) Procs;

typedef struct {
  Str    name;
  Token *token;
} RequiredProc;

typedef Da(RequiredProc) RequiredProcs;

typedef struct {
  Vars           vars;
  Strs           strs;
  Procs          procs;
  RequiredProcs  required_procs;
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

static char *get_dest_loc(Dest dest, u32 size) {
  if (dest == DestReturn) {
    switch (size) {
    case 1: return "al";
    case 2: return "ax";
    case 4: return "eax";
    case 8: return "rax";
    }
  }

  return "";
}

static u32 get_size_on_stack(u32 size) {
  switch (size) {
  case 1:  return 2;
  case 4:  return 8;
  default: return size;
  }
}

static u32 compile_expr(Parser *parser, Compiler *compiler, Dest dest) {
  Token *token = expect_token(parser, "identifier, integer or string",
                              MASK(TT_INT) | MASK(TT_STR) | MASK(TT_IDENT));
  if (parser->has_error)
    return 0;

  if (token->id == TT_INT) {
    char *loc = get_dest_loc(dest, 4);
    fprintf(compiler->output_file, "  mov %s, "STR_FMT"\n", loc, STR_ARG(token->lexeme));

    return 4;
  } else if (token->id == TT_STR) {
    char *loc = get_dest_loc(dest, 8);
    fprintf(compiler->output_file, "  lea %s, [str@%u]\n", loc, compiler->strs.len);

    sb_push(&compiler->temp_sb, "str@");
    sb_push_u32(&compiler->temp_sb, compiler->strs.len);

    Str str_loc = sb_to_str(compiler->temp_sb);
    DA_APPEND(compiler->strs, str_loc);


    compiler->temp_sb.len = 0;

    return 8;
  } else if (token->id == TT_IDENT) {
    Token *next = peek_token(parser);
    if (next && next->id == TT_OPAREN) {
      expect_token(parser, "`)`", MASK(TT_CPAREN));

      RequiredProc new_required = { token->lexeme, token };
      DA_APPEND(compiler->required_procs, new_required);

      fprintf(compiler->output_file, "  call $"STR_FMT"\n", STR_ARG(token->lexeme));
    } else {
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
        return 0;
      }

      char *loc = get_dest_loc(dest, var->size);
      fprintf(compiler->output_file, "  mov %s,[rbp-%u]\n", loc, var->offset);

      return var->size;
    }
  }

  return 0;
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

      u32 size = get_size_on_stack(compile_expr(parser, compiler, DestReturn));
      if (parser->has_error)
        return;

      char *loc = get_dest_loc(DestReturn, size);
      fprintf(compiler->output_file, "  push %s\n", loc);

      compiler->stack_size += size;

      Var new_var = {
        name->lexeme,
        size,
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

        u32 size = compile_expr(parser, compiler, DestReturn);
        if (parser->has_error)
          return;

        char *loc = get_dest_loc(DestReturn, size);
        fprintf(compiler->output_file, "  mov [rbp-%u],%s\n", var->offset, loc);
      } else if (next && next->id == TT_OPAREN) {
        expect_token(parser, "`)`", MASK(TT_CPAREN));

        RequiredProc new_required = { token->lexeme, token };
        DA_APPEND(compiler->required_procs, new_required);

        fprintf(compiler->output_file, "  call $"STR_FMT"\n", STR_ARG(token->lexeme));
      }
    }
  }
}

bool compile(Str code, Str file_path, FILE *output_file) {
  StringBuilder temp_sb = {0};
  Tokens tokens = lex(code, file_path, &temp_sb);
  Parser parser = { code, 0, tokens, file_path, false };
  Compiler compiler = {0};
  compiler.output_file = output_file;
  compiler.temp_sb = temp_sb;

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
      Token *name = expect_token(&parser, "identifier", MASK(TT_IDENT));
      if (parser.has_error)
        return false;

      expect_token(&parser, "`(`", MASK(TT_OPAREN));
      if (parser.has_error)
        return false;

      expect_token(&parser, "`)`", MASK(TT_CPAREN));
      if (parser.has_error)
        return false;

      fprintf(output_file, "global $"STR_FMT"\n", STR_ARG(name->lexeme));
      fprintf(output_file, "$"STR_FMT":\n", STR_ARG(name->lexeme));
      fprintf(output_file, "  push rbp\n");
      fprintf(output_file, "  mov rbp,rsp\n");

      Proc new_proc = { name->lexeme };
      DA_APPEND(compiler.procs, new_proc);

      compile_instrs(&parser, &compiler);
      if (parser.has_error)
        return false;

      fprintf(output_file, ".end:\n");
      fprintf(output_file, "  leave\n");
      fprintf(output_file, "  ret\n");

      expect_token(&parser, "end procedure", MASK(TT_END));
      if (parser.has_error)
        return false;
    } else if (token->id == TT_EXTERN) {
      expect_token(&parser, "`proc`", MASK(TT_PROC));
      if (parser.has_error)
        return false;

      Token *name = expect_token(&parser, "identifier", MASK(TT_IDENT));
      if (parser.has_error)
        return false;

      expect_token(&parser, "`(`", MASK(TT_OPAREN));
      if (parser.has_error)
        return false;

      expect_token(&parser, "`)`", MASK(TT_CPAREN));
      if (parser.has_error)
        return false;

      Proc new_proc = { name->lexeme };
      DA_APPEND(compiler.procs, new_proc);

      fprintf(output_file, "extern $"STR_FMT"\n", STR_ARG(name->lexeme));
    }
  }

  if (compiler.strs.len > 0) {
    fprintf(output_file, "section '.data'\n");
  }

  for (u32 i = 0; i < compiler.strs.len; ++i) {
    Str *str = compiler.strs.items + i;

    fprintf(output_file, "str@%u: db ", i);
    for (u32 j = 0; j < str->len; ++j) {
      if (j > 0)
        fprintf(output_file, ",");
      fprintf(output_file, "%u", str->ptr[j]);
    }
    fprintf(output_file, ",0\n");
  }

  for (u32 i = 0; i < compiler.required_procs.len; ++i) {
    RequiredProc *required = compiler.required_procs.items + i;
    bool found = false;

    for (u32 j = 0; j < compiler.procs.len; ++j) {
      if (str_eq(compiler.procs.items[j].name, required->name)) {
        found = true;
        break;
      }
    }

    if (!found) {
      parser.has_error = true;
      PERROR(STR_FMT":%u:%u: ", "Undeclared procedure `"STR_FMT"`\n",
             STR_ARG(parser.file_path),
             required->token->row + 1,
             required->token->col + 1,
             STR_ARG(required->name));
    }
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
  if (compiler.required_procs.items)
    free(compiler.required_procs.items);
  if (temp_sb.buffer)
    free(temp_sb.buffer);

  return true;
}
