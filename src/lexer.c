#include <wchar.h>

#include "shl/shl-str.h"
#include "shl/shl-log.h"
#include "lexgen/runtime.h"
#define LEXGEN_TRANSITION_TABLE_IMPLEMENTATION
#include "grammar.h"
#include "lexer.h"

bool lex(Tokens *tokens, Str code, Str file_path, StringBuilder *temp_sb) {
  TransitionTable *table = get_transition_table();
  u16 current_row = 0;
  u16 current_col = 0;

  while (code.len > 0) {
    u64 id = 0;
    u32 char_len;
    Str lexeme = table_matches(table, &code, &id, &char_len);
    u16 row = current_row;
    u16 col = current_col;

    if (id == TT_NEWLINE) {
      ++current_row;
      current_col = 0;

      continue;
    }

    if (id == TT_SLCOMMENT) {
      u32 next_len;
      wchar next;

      while ((next = get_next_wchar(code, 0, &next_len)) != U'\0' && next != U'\n') {
        code.ptr += next_len;
        code.len -= next_len;
      }

      continue;
    }

    if (id == TT_MLCOMMENT) {
      u32 next_len;
      wchar next;
      bool prev_is_comment = false;

      while ((next = get_next_wchar(code, 0, &next_len)) != U'\0' &&
             (next != U'#' || !prev_is_comment)) {
        prev_is_comment = next == U'#';

        if (next == U'\n') {
          ++current_row;
          current_col = 0;
        } else {
          ++current_col;
        }

        code.ptr += next_len;
        code.len -= next_len;
      }

      continue;
    }

    if (id == TT_WHITESPACE) {
      current_col += char_len;

      continue;
    }

    if (id == (u64) -1) {
      u32 wchar_len;
      wchar _wchar = get_next_wchar(code, 0, &wchar_len);

      PERROR(STR_FMT":%u:%u: ", "Unexpected `%lc`\n", STR_ARG(file_path),
             current_row + 1, current_col + 1, (wint_t) _wchar);

      for (u32 i = 0; i < tokens->len; ++i)
        free(tokens->items[i].lexeme.ptr);
      if (tokens->items)
        free(tokens->items);

      return false;
    }

    if (id == TT_STR) {
      sb_push_char(temp_sb, code.ptr[-1]);

      bool is_escaped = false;
      while (code.len > 0 &&
             (code.ptr[0] != temp_sb->buffer[0] ||
              is_escaped)) {
        u32 next_len;
        wchar next = get_next_wchar(code, 0, &next_len);

        for (u32 i = 0; i < next_len; ++i)
          if (code.ptr[i])
            sb_push_char(temp_sb, code.ptr[i]);

        if (is_escaped)
          is_escaped = false;
        else if (next == U'\\')
          is_escaped = true;

        code.ptr += next_len;
        code.len -= next_len;
        ++current_col;
      }

      if (code.len == 0) {
        PERROR(STR_FMT":%u:%u: ", "String literal was not closed\n",
               STR_ARG(file_path), row + 1, col + 1);

        for (u32 i = 0; i < tokens->len; ++i)
          free(tokens->items[i].lexeme.ptr);
        if (tokens->items)
          free(tokens->items);

        return false;
      }

      sb_push_char(temp_sb, code.ptr[0]);

      ++code.ptr;
      --code.len;
      ++current_col;

      lexeme = sb_to_str(*temp_sb);

      temp_sb->len = 0;
    } else if (id == TT_CHAR) {
      sb_push_char(temp_sb, code.ptr[-1]);

      if (code.len < 2 || (code.ptr[0] == '\\' && code.len < 3)) {
        PERROR(STR_FMT":%u:%u: ", "Character literal was not closed\n",
               STR_ARG(file_path), row + 1, col + 1);

        for (u32 i = 0; i < tokens->len; ++i)
          free(tokens->items[i].lexeme.ptr);
        if (tokens->items)
          free(tokens->items);

        return false;
      }

      u32 next_len;
      wchar next = get_next_wchar(code, 0, &next_len);

      for (u32 i = 0; i < next_len; ++i)
        if (code.ptr[i])
          sb_push_char(temp_sb, code.ptr[i]);

      code.ptr += next_len;
      code.len -= next_len;
      ++current_col;

      if (next == U'\\') {
        next = get_next_wchar(code, 0, &next_len);

        for (u32 i = 0; i < next_len; ++i)
          if (code.ptr[i])
            sb_push_char(temp_sb, code.ptr[i]);

        code.ptr += next_len;
        code.len -= next_len;
        ++current_col;
      }

      if (code.len == 0 || code.ptr[0] != '\'') {
        PERROR(STR_FMT":%u:%u: ", "Character literal was not closed\n",
               STR_ARG(file_path), row + 1, col + 1);

        for (u32 i = 0; i < tokens->len; ++i)
          free(tokens->items[i].lexeme.ptr);
        if (tokens->items)
          free(tokens->items);

        return false;
      }

      sb_push_char(temp_sb, code.ptr[0]);

      ++code.ptr;
      --code.len;
      ++current_col;

      lexeme = sb_to_str(*temp_sb);

      temp_sb->len = 0;
    } else {
      current_col += char_len;
    }

    char *new_ptr = malloc(lexeme.len);
    memcpy(new_ptr, lexeme.ptr, lexeme.len);
    lexeme.ptr = new_ptr;

    Token token = (Token) { id, lexeme, file_path, row, col };
    DA_APPEND(*tokens, token);
  }

  return true;
}
