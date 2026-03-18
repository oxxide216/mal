#ifndef LEXER_H
#define LEXER_H

#include "shl/shl-defs.h"
#include "shl/shl-str.h"

typedef struct {
  u64 id;
  Str lexeme;
  Str file_path;
  u16 row, col;
} Token;

typedef Da(Token) Tokens;

bool lex(Tokens *tokens, Str code, Str file_path, StringBuilder *temp_sb);

#endif // LEXER_H
