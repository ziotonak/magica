#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define OS_WINDOWS 1
#define OS_LINUX 0
#elif defined(__linux__)
#define OS_WINDOWS 0
#define OS_LINUX 1
#else
#error "unsupported platform"
#endif
#if OS_LINUX
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef float f32;
typedef double f64;
typedef size_t usize;
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define KiB (1024ULL)
#define MiB (1024ULL * KiB)
typedef struct {
  const char *data;
  usize len;
} Str8;
#define S(lit) str8((lit), sizeof(lit) - 1)
#define STR_FMT "%.*s"
#define STR_ARG(s) (int)(s).len, (s).data

static Str8 str8(const char *data, usize len) { return (Str8){data, len}; }

static Str8 str8_cstr(const char *s) {
  usize n = 0;
  if (s)
    while (s[n])
      n++;
  return str8(s, n);
}

static Str8 str8_range(Str8 s, usize start, usize end) {
  if (start >= s.len || start >= end)
    return (Str8){0};
  return str8(s.data + start, MIN(end, s.len) - start);
}

static bool str8_eq(Str8 a, Str8 b) {
  if (a.len != b.len)
    return false;
  for (usize i = 0; i < a.len; i++)
    if (a.data[i] != b.data[i])
      return false;
  return true;
}
typedef struct {
  u8 *base;
  usize size, used, committed;
} Arena;

static void *os_reserve(usize size);

static bool os_commit(void *addr, usize size);

static void os_release(void *addr, usize size);

static Str8 os_read_file(Arena *arena, const char *path);

static Arena *arena_new(usize size) {
  void *mem = os_reserve(size);
  if (!mem)
    return NULL;
  usize init = ALIGN_UP(sizeof(Arena), 4 * KiB);
  if (!os_commit(mem, init)) {
    os_release(mem, size);
    return NULL;
  }
  Arena *a = mem;
  *a = (Arena){.base = mem,
               .size = size,
               .used = ALIGN_UP(sizeof(Arena), 16),
               .committed = init};
  return a;
}

static void arena_free(Arena *a) {
  if (a)
    os_release(a->base, a->size);
}

static void *arena_alloc(Arena *a, usize size) {
  usize aligned = ALIGN_UP(a->used, 16);
  usize next = aligned + size;
  if (next > a->size)
    return NULL;
  if (next > a->committed) {
    usize new_commit = ALIGN_UP(next, 64 * KiB);
    if (new_commit > a->size)
      new_commit = a->size;
    if (!os_commit(a->base + a->committed, new_commit - a->committed))
      return NULL;
    a->committed = new_commit;
  }
  a->used = next;
  void *p = a->base + aligned;
  memset(p, 0, size);
  return p;
}
#define NEW(a, T) ((T *)arena_alloc((a), sizeof(T)))
#define NEW_ARRAY(a, T, n) ((T *)arena_alloc((a), sizeof(T) * (n)))

static char *arena_dup_cstr(Arena *a, Str8 s) {
  char *p = arena_alloc(a, s.len + 1);
  if (p) {
    memcpy(p, s.data, s.len);
    p[s.len] = 0;
  }
  return p;
}
#if OS_LINUX

static void *os_reserve(usize size) {
  void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? NULL : p;
}

static bool os_commit(void *addr, usize size) {
  return mprotect(addr, size, PROT_READ | PROT_WRITE) == 0;
}

static void os_release(void *addr, usize size) { munmap(addr, size); }

static Str8 os_read_file(Arena *arena, const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return (Str8){0};
  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return (Str8){0};
  }
  usize size = st.st_size;
  char *buf = arena_alloc(arena, size + 1);
  if (!buf) {
    close(fd);
    return (Str8){0};
  }
  usize n = 0;
  while (n < size) {
    ssize_t r = read(fd, buf + n, size - n);
    if (r <= 0)
      break;
    n += r;
  }
  close(fd);
  buf[n] = 0;
  return str8(buf, n);
}
#else

static void *os_reserve(usize size) {
  return VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
}

static bool os_commit(void *addr, usize size) {
  return VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

static void os_release(void *addr, usize size) {
  (void)size;
  VirtualFree(addr, 0, MEM_RELEASE);
}

static Str8 os_read_file(Arena *arena, const char *path) {
  HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, 0, NULL);
  if (f == INVALID_HANDLE_VALUE)
    return (Str8){0};
  LARGE_INTEGER sz;
  if (!GetFileSizeEx(f, &sz)) {
    CloseHandle(f);
    return (Str8){0};
  }
  usize size = sz.QuadPart;
  char *buf = arena_alloc(arena, size + 1);
  if (!buf) {
    CloseHandle(f);
    return (Str8){0};
  }
  DWORD n;
  ReadFile(f, buf, (DWORD)size, &n, NULL);
  CloseHandle(f);
  buf[n] = 0;
  return str8(buf, n);
}
#endif

static struct {
  Str8 source, filename;
  bool had_error;
} g_err;

static void get_line_col(usize pos, u32 *line, u32 *col) {
  *line = 1;
  *col = 1;
  for (usize i = 0; i < pos && i < g_err.source.len; i++) {
    if (g_err.source.data[i] == '\n') {
      (*line)++;
      *col = 1;
    } else {
      (*col)++;
    }
  }
}

static void error_at(usize pos, usize len, const char *fmt, ...) {
  g_err.had_error = true;
  u32 line, col;
  get_line_col(pos, &line, &col);
  fprintf(stderr, STR_FMT ":%u:%u: error: ", STR_ARG(g_err.filename), line,
          col);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  usize ls = pos, le = pos;
  while (ls > 0 && g_err.source.data[ls - 1] != '\n')
    ls--;
  while (le < g_err.source.len && g_err.source.data[le] != '\n')
    le++;
  fprintf(stderr, "  " STR_FMT "\n  ",
          STR_ARG(str8_range(g_err.source, ls, le)));
  for (usize i = ls; i < pos; i++)
    fputc(' ', stderr);
  for (usize i = 0; i < MAX(len, 1); i++)
    fputc('^', stderr);
  fputc('\n', stderr);
}

static void fatal(const char *fmt, ...) {
  fprintf(stderr, "fatal: ");
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  exit(1);
}
typedef enum {
  TK_EOF = 0,
  TK_ERROR,
  TK_INT,
  TK_FLOAT,
  TK_CHAR,
  TK_STRING,
  TK_IDENT,
  TK_FOREIGN,
  TK_STRUCT,
  TK_UNION,
  TK_ENUM,
  TK_FUNC,
  TK_IF,
  TK_ELSE,
  TK_FOR,
  TK_IN,
  TK_MATCH,
  TK_BREAK,
  TK_CONTINUE,
  TK_RETURN,
  TK_DEFER,
  TK_TRUE,
  TK_FALSE,
  TK_NULL,
  TK_CAST,
  TK_SIZEOF,
  TK_ALIGNOF,
  TK_IMPORT,
  TK_LPAREN,
  TK_RPAREN,
  TK_LBRACKET,
  TK_RBRACKET,
  TK_LBRACE,
  TK_RBRACE,
  TK_COMMA,
  TK_SEMI,
  TK_DOT3,
  TK_NEWLINE,
  TK_COLON,
  TK_COLON2,
  TK_COLONEQ,
  TK_ARROW,
  TK_DOT,
  TK_DOT2,
  TK_DOT2EQ,
  TK_DOTSTAR,
  TK_PLUS,
  TK_MINUS,
  TK_STAR,
  TK_SLASH,
  TK_PERCENT,
  TK_CARET,
  TK_AMP,
  TK_PIPE,
  TK_TILDE,
  TK_SHL,
  TK_SHR,
  TK_EQ,
  TK_NE,
  TK_LT,
  TK_GT,
  TK_LE,
  TK_GE,
  TK_BANG,
  TK_AND,
  TK_OR,
  TK_ASSIGN,
  TK_PLUS_EQ,
  TK_MINUS_EQ,
  TK_STAR_EQ,
  TK_SLASH_EQ,
  TK_PERCENT_EQ,
  TK_AMP_EQ,
  TK_PIPE_EQ,
  TK_CARET_EQ,
  TK_SHL_EQ,
  TK_SHR_EQ,
  TK_FATARROW,
} TokenKind;
typedef struct {
  TokenKind kind;
  u32 pos;
  u16 len;
} Token;
typedef struct {
  Str8 src;
  usize pos;
} Lexer;

static Str8 tok_text(Token t) {
  return str8_range(g_err.source, t.pos, t.pos + t.len);
}

static inline char lex_peek(Lexer *l, usize ahead) {
  usize i = l->pos + ahead;
  return (i < l->src.len) ? l->src.data[i] : '\0';
}

static inline char lex_advance(Lexer *l) {
  return (l->pos < l->src.len) ? l->src.data[l->pos++] : '\0';
}

static inline bool lex_match(Lexer *l, char c) {
  if (lex_peek(l, 0) == c) {
    l->pos++;
    return true;
  }
  return false;
}

static inline Token lex_make(TokenKind kind, usize start, usize end) {
  return (Token){.kind = kind, .pos = (u32)start, .len = (u16)(end - start)};
}

static inline bool is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

static inline bool is_hex(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static inline bool is_binary(char c) { return c == '0' || c == '1'; }

static inline bool is_octal(char c) { return c >= '0' && c <= '7'; }

static inline bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }

static TokenKind lex_keyword(Str8 s) {

  static const struct {
    const char *s;
    usize len;
    TokenKind k;
  } kws[] = {
      {"if", 2, TK_IF},
      {"in", 2, TK_IN},
      {"for", 3, TK_FOR},
      {"func", 4, TK_FUNC},
      {"cast", 4, TK_CAST},
      {"null", 4, TK_NULL},
      {"else", 4, TK_ELSE},
      {"true", 4, TK_TRUE},
      {"enum", 4, TK_ENUM},
      {"false", 5, TK_FALSE},
      {"break", 5, TK_BREAK},
      {"match", 5, TK_MATCH},
      {"defer", 5, TK_DEFER},
      {"union", 5, TK_UNION},
      {"struct", 6, TK_STRUCT},
      {"return", 6, TK_RETURN},
      {"sizeof", 6, TK_SIZEOF},
      {"import", 6, TK_IMPORT},
      {"alignof", 7, TK_ALIGNOF},
      {"foreign", 7, TK_FOREIGN},
      {"continue", 8, TK_CONTINUE},
  };
  for (usize i = 0; i < ARRAY_COUNT(kws); i++) {
    if (s.len == kws[i].len && str8_eq(s, str8(kws[i].s, kws[i].len))) {
      return kws[i].k;
    }
  }
  return TK_IDENT;
}

static void lex_skip_ws(Lexer *l) {
  while (l->pos < l->src.len) {
    char c = lex_peek(l, 0);
    if (c == ' ' || c == '\t' || c == '\r') {
      l->pos++;
    } else if (c == '/' && lex_peek(l, 1) == '/') {
      l->pos += 2;
      while (l->pos < l->src.len && lex_peek(l, 0) != '\n')
        l->pos++;
    } else {
      break;
    }
  }
}

static Token lex_number(Lexer *l, usize start) {
  char c = lex_peek(l, 0);
  if (l->src.data[start] == '0' && l->pos == start + 1) {
    if (c == 'x' || c == 'X') {
      l->pos++;
      while (is_hex(lex_peek(l, 0)) || lex_peek(l, 0) == '_')
        l->pos++;
      return lex_make(TK_INT, start, l->pos);
    }
    if (c == 'b' || c == 'B') {
      l->pos++;
      while (is_binary(lex_peek(l, 0)) || lex_peek(l, 0) == '_')
        l->pos++;
      return lex_make(TK_INT, start, l->pos);
    }
    if (c == 'o' || c == 'O') {
      l->pos++;
      while (is_octal(lex_peek(l, 0)) || lex_peek(l, 0) == '_')
        l->pos++;
      return lex_make(TK_INT, start, l->pos);
    }
  }
  while (is_digit(lex_peek(l, 0)) || lex_peek(l, 0) == '_')
    l->pos++;
  bool is_float = false;
  if (lex_peek(l, 0) == '.' && lex_peek(l, 1) != '.') {
    char next = lex_peek(l, 1);
    if (is_digit(next)) {
      l->pos++;
      while (is_digit(lex_peek(l, 0)) || lex_peek(l, 0) == '_')
        l->pos++;
      is_float = true;
    }
  }
  c = lex_peek(l, 0);
  if (c == 'e' || c == 'E') {
    char next = lex_peek(l, 1);
    if (is_digit(next) || next == '+' || next == '-') {
      l->pos++;
      if (lex_peek(l, 0) == '+' || lex_peek(l, 0) == '-')
        l->pos++;
      while (is_digit(lex_peek(l, 0)) || lex_peek(l, 0) == '_')
        l->pos++;
      is_float = true;
    }
  }
  return lex_make(is_float ? TK_FLOAT : TK_INT, start, l->pos);
}

static Token lex_string(Lexer *l, usize start) {
  while (l->pos < l->src.len) {
    char c = lex_peek(l, 0);
    if (c == '"') {
      l->pos++;
      return lex_make(TK_STRING, start, l->pos);
    }
    if (c == '\n') {
      error_at(start, l->pos - start, "unterminated string literal");
      return lex_make(TK_ERROR, start, l->pos);
    }
    if (c == '\\' && l->pos + 1 < l->src.len) {
      l->pos += 2;
    } else {
      l->pos++;
    }
  }
  error_at(start, l->pos - start, "unterminated string literal");
  return lex_make(TK_ERROR, start, l->pos);
}

static Token lex_char(Lexer *l, usize start) {
  if (lex_peek(l, 0) == '\\' && l->pos + 1 < l->src.len) {
    l->pos += 2;
  } else if (l->pos < l->src.len) {
    l->pos++;
  }
  if (lex_peek(l, 0) != '\'') {
    error_at(start, l->pos - start, "unterminated character literal");
    return lex_make(TK_ERROR, start, l->pos);
  }
  l->pos++;
  return lex_make(TK_CHAR, start, l->pos);
}

static Token lex_next(Lexer *l) {
  lex_skip_ws(l);
  usize start = l->pos;
  if (l->pos >= l->src.len) {
    return lex_make(TK_EOF, start, start);
  }
  char c = lex_advance(l);
  if (c == '\n')
    return lex_make(TK_NEWLINE, start, l->pos);
  if (is_digit(c))
    return lex_number(l, start);
  if (c == '"')
    return lex_string(l, start);
  if (c == '\'')
    return lex_char(l, start);
  if (is_alpha(c)) {
    while (is_alnum(lex_peek(l, 0)))
      l->pos++;
    Str8 text = str8_range(l->src, start, l->pos);
    return (Token){.kind = lex_keyword(text),
                   .pos = (u32)start,
                   .len = (u16)(l->pos - start)};
  }
  switch (c) {
  case '(':
    return lex_make(TK_LPAREN, start, l->pos);
  case ')':
    return lex_make(TK_RPAREN, start, l->pos);
  case '[':
    return lex_make(TK_LBRACKET, start, l->pos);
  case ']':
    return lex_make(TK_RBRACKET, start, l->pos);
  case '{':
    return lex_make(TK_LBRACE, start, l->pos);
  case '}':
    return lex_make(TK_RBRACE, start, l->pos);
  case ',':
    return lex_make(TK_COMMA, start, l->pos);
  case ';':
    return lex_make(TK_SEMI, start, l->pos);
  case '~':
    return lex_make(TK_TILDE, start, l->pos);
  case '^':
    if (lex_match(l, '='))
      return lex_make(TK_CARET_EQ, start, l->pos);
    return lex_make(TK_CARET, start, l->pos);
  case ':':
    if (lex_match(l, ':'))
      return lex_make(TK_COLON2, start, l->pos);
    if (lex_match(l, '='))
      return lex_make(TK_COLONEQ, start, l->pos);
    return lex_make(TK_COLON, start, l->pos);
  case '.':
    if (lex_match(l, '.')) {
      if (lex_match(l, '.'))
        return lex_make(TK_DOT3, start, l->pos);
      if (lex_match(l, '='))
        return lex_make(TK_DOT2EQ, start, l->pos);
      return lex_make(TK_DOT2, start, l->pos);
    }
    if (lex_match(l, '*'))
      return lex_make(TK_DOTSTAR, start, l->pos);
    return lex_make(TK_DOT, start, l->pos);
  case '+':
    if (lex_match(l, '='))
      return lex_make(TK_PLUS_EQ, start, l->pos);
    return lex_make(TK_PLUS, start, l->pos);
  case '-':
    if (lex_match(l, '>'))
      return lex_make(TK_ARROW, start, l->pos);
    if (lex_match(l, '='))
      return lex_make(TK_MINUS_EQ, start, l->pos);
    return lex_make(TK_MINUS, start, l->pos);
  case '*':
    if (lex_match(l, '='))
      return lex_make(TK_STAR_EQ, start, l->pos);
    return lex_make(TK_STAR, start, l->pos);
  case '/':
    if (lex_match(l, '='))
      return lex_make(TK_SLASH_EQ, start, l->pos);
    return lex_make(TK_SLASH, start, l->pos);
  case '%':
    if (lex_match(l, '='))
      return lex_make(TK_PERCENT_EQ, start, l->pos);
    return lex_make(TK_PERCENT, start, l->pos);
  case '=':
    if (lex_match(l, '='))
      return lex_make(TK_EQ, start, l->pos);
    if (lex_match(l, '>'))
      return lex_make(TK_FATARROW, start, l->pos);
    return lex_make(TK_ASSIGN, start, l->pos);
  case '!':
    if (lex_match(l, '='))
      return lex_make(TK_NE, start, l->pos);
    return lex_make(TK_BANG, start, l->pos);
  case '<':
    if (lex_match(l, '<')) {
      if (lex_match(l, '='))
        return lex_make(TK_SHL_EQ, start, l->pos);
      return lex_make(TK_SHL, start, l->pos);
    }
    if (lex_match(l, '='))
      return lex_make(TK_LE, start, l->pos);
    return lex_make(TK_LT, start, l->pos);
  case '>':
    if (lex_match(l, '>')) {
      if (lex_match(l, '='))
        return lex_make(TK_SHR_EQ, start, l->pos);
      return lex_make(TK_SHR, start, l->pos);
    }
    if (lex_match(l, '='))
      return lex_make(TK_GE, start, l->pos);
    return lex_make(TK_GT, start, l->pos);
  case '&':
    if (lex_match(l, '&'))
      return lex_make(TK_AND, start, l->pos);
    if (lex_match(l, '='))
      return lex_make(TK_AMP_EQ, start, l->pos);
    return lex_make(TK_AMP, start, l->pos);
  case '|':
    if (lex_match(l, '|'))
      return lex_make(TK_OR, start, l->pos);
    if (lex_match(l, '='))
      return lex_make(TK_PIPE_EQ, start, l->pos);
    return lex_make(TK_PIPE, start, l->pos);
  }
  error_at(start, 1, "unexpected character '%c' (0x%02X)", c, (unsigned char)c);
  return lex_make(TK_ERROR, start, l->pos);
}
typedef struct Node Node;
typedef enum {
  NODE_INVALID = 0,
  NODE_TYPE_VOID,
  NODE_TYPE_BOOL,
  NODE_TYPE_U8,
  NODE_TYPE_U16,
  NODE_TYPE_U32,
  NODE_TYPE_U64,
  NODE_TYPE_I8,
  NODE_TYPE_I16,
  NODE_TYPE_I32,
  NODE_TYPE_I64,
  NODE_TYPE_F32,
  NODE_TYPE_F64,
  NODE_TYPE_PTR,
  NODE_TYPE_ARRAY,
  NODE_TYPE_SLICE,
  NODE_TYPE_TUPLE,
  NODE_TYPE_STRING,
  NODE_TYPE_NAMED,
  NODE_TYPE_GENERIC,
  NODE_TYPE_UNTYPED_INT,
  NODE_TYPE_UNTYPED_FLOAT,
  NODE_INT,
  NODE_FLOAT,
  NODE_CHAR,
  NODE_STRING,
  NODE_BOOL,
  NODE_NULL,
  NODE_IDENT,
  NODE_UNDERSCORE,
  NODE_TUPLE,
  NODE_ARRAY_LIT,
  NODE_STRUCT_LIT,
  NODE_FIELD_INIT,
  NODE_UNARY,
  NODE_BINARY,
  NODE_CALL,
  NODE_INDEX,
  NODE_SLICE,
  NODE_FIELD,
  NODE_CAST,
  NODE_SIZEOF,
  NODE_ALIGNOF,
  NODE_BLOCK,
  NODE_IF,
  NODE_FOR,
  NODE_MATCH,
  NODE_MATCH_ARM,
  NODE_RANGE,
  NODE_RETURN,
  NODE_BREAK,
  NODE_CONTINUE,
  NODE_DEFER,
  NODE_IMPORT,
  NODE_VAR,
  NODE_CONST,
  NODE_FUNC,
  NODE_FOREIGN,
  NODE_PARAM,
  NODE_VARARGS,
  NODE_TYPE_PARAM,
  NODE_STRUCT,
  NODE_UNION,
  NODE_ENUM,
  NODE_STRUCT_FIELD,
  NODE_ENUM_VARIANT,
} NodeKind;
struct Node {
  NodeKind kind;
  Token token;
  TokenKind op;
  Node *type;
  Node *left;
  Node *right;
  Node *ptr;
  Node *next;
  Node *decl;
};

static const char *node_kind_str(NodeKind k) {
  switch (k) {
  case NODE_INVALID:
    return "invalid";
  case NODE_TYPE_VOID:
    return "type_void";
  case NODE_TYPE_BOOL:
    return "type_bool";
  case NODE_TYPE_U8:
    return "type_u8";
  case NODE_TYPE_U16:
    return "type_u16";
  case NODE_TYPE_U32:
    return "type_u32";
  case NODE_TYPE_U64:
    return "type_u64";
  case NODE_TYPE_I8:
    return "type_i8";
  case NODE_TYPE_I16:
    return "type_i16";
  case NODE_TYPE_I32:
    return "type_i32";
  case NODE_TYPE_I64:
    return "type_i64";
  case NODE_TYPE_F32:
    return "type_f32";
  case NODE_TYPE_F64:
    return "type_f64";
  case NODE_TYPE_PTR:
    return "type_ptr";
  case NODE_TYPE_ARRAY:
    return "type_array";
  case NODE_TYPE_SLICE:
    return "type_slice";
  case NODE_TYPE_STRING:
    return "type_string";
  case NODE_TYPE_TUPLE:
    return "type_tuple";
  case NODE_TYPE_NAMED:
    return "type_named";
  case NODE_TYPE_GENERIC:
    return "type_generic";
  case NODE_TYPE_UNTYPED_INT:
    return "type_untyped_int";
  case NODE_TYPE_UNTYPED_FLOAT:
    return "type_untyped_float";
  case NODE_INT:
    return "int";
  case NODE_FLOAT:
    return "float";
  case NODE_CHAR:
    return "char";
  case NODE_STRING:
    return "string";
  case NODE_BOOL:
    return "bool";
  case NODE_NULL:
    return "null";
  case NODE_IDENT:
    return "ident";
  case NODE_TUPLE:
    return "tuple";
  case NODE_UNDERSCORE:
    return "underscore";
  case NODE_ARRAY_LIT:
    return "array_lit";
  case NODE_STRUCT_LIT:
    return "struct_lit";
  case NODE_FIELD_INIT:
    return "field_init";
  case NODE_UNARY:
    return "unary";
  case NODE_BINARY:
    return "binary";
  case NODE_CALL:
    return "call";
  case NODE_INDEX:
    return "index";
  case NODE_SLICE:
    return "slice";
  case NODE_FIELD:
    return "field";
  case NODE_CAST:
    return "cast";
  case NODE_SIZEOF:
    return "sizeof";
  case NODE_ALIGNOF:
    return "alignof";
  case NODE_BLOCK:
    return "block";
  case NODE_IF:
    return "if";
  case NODE_FOR:
    return "for";
  case NODE_MATCH:
    return "match";
  case NODE_MATCH_ARM:
    return "match_arm";
  case NODE_RANGE:
    return "range";
  case NODE_RETURN:
    return "return";
  case NODE_BREAK:
    return "break";
  case NODE_CONTINUE:
    return "continue";
  case NODE_DEFER:
    return "defer";
  case NODE_IMPORT:
    return "import";
  case NODE_VAR:
    return "var";
  case NODE_CONST:
    return "const";
  case NODE_FUNC:
    return "func";
  case NODE_FOREIGN:
    return "foreign";
  case NODE_PARAM:
    return "param";
  case NODE_VARARGS:
    return "varargs";
  case NODE_TYPE_PARAM:
    return "type_param";
  case NODE_STRUCT:
    return "struct";
  case NODE_UNION:
    return "union";
  case NODE_ENUM:
    return "enum";
  case NODE_STRUCT_FIELD:
    return "struct_field";
  case NODE_ENUM_VARIANT:
    return "enum_variant";
  default:
    return "?";
  }
}
typedef struct {
  Lexer lexer;
  Token current;
  Token previous;
} Parser;

static void advance(Parser *p) {
  p->previous = p->current;
  for (;;) {
    p->current = lex_next(&p->lexer);
    if (p->current.kind != TK_ERROR)
      break;
  }
}

static bool check(Parser *p, TokenKind kind) { return p->current.kind == kind; }

static TokenKind peek_next(Parser *p) {
  Lexer saved = p->lexer;
  Token t = lex_next(&saved);
  return t.kind;
}

static bool match_tok(Parser *p, TokenKind kind) {
  if (!check(p, kind))
    return false;
  advance(p);
  return true;
}

static void skip_newlines(Parser *p) {
  while (match_tok(p, TK_NEWLINE))
    ;
}

static void expect(Parser *p, TokenKind kind, const char *msg) {
  if (match_tok(p, kind))
    return;
  error_at(p->current.pos, p->current.len, "%s", msg);
}

static Node *parse_decl(Arena *a, Parser *p);

static Node *parse_expr(Arena *a, Parser *p);

static Node *parse_stmt(Arena *a, Parser *p);

static Node *parse_type(Arena *a, Parser *p) {
  if (match_tok(p, TK_CARET)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_TYPE_PTR;
    n->token = p->previous;
    n->ptr = parse_type(a, p);
    return n;
  }
  if (match_tok(p, TK_LBRACKET)) {
    Node *n = NEW(a, Node);
    n->token = p->previous;
    if (match_tok(p, TK_RBRACKET)) {
      n->kind = NODE_TYPE_SLICE;
      n->ptr = parse_type(a, p);
      return n;
    }
    n->kind = NODE_TYPE_ARRAY;
    n->left = parse_expr(a, p);
    expect(p, TK_RBRACKET, "expected ']' after array size");
    n->ptr = parse_type(a, p);
    return n;
  }
  if (check(p, TK_IDENT)) {
    Str8 name = tok_text(p->current);
    Node *n = NEW(a, Node);
    n->token = p->current;
    advance(p);
    if (str8_eq(name, S("void")))
      n->kind = NODE_TYPE_VOID;
    else if (str8_eq(name, S("bool")))
      n->kind = NODE_TYPE_BOOL;
    else if (str8_eq(name, S("u8")))
      n->kind = NODE_TYPE_U8;
    else if (str8_eq(name, S("u16")))
      n->kind = NODE_TYPE_U16;
    else if (str8_eq(name, S("u32")))
      n->kind = NODE_TYPE_U32;
    else if (str8_eq(name, S("u64")))
      n->kind = NODE_TYPE_U64;
    else if (str8_eq(name, S("i8")))
      n->kind = NODE_TYPE_I8;
    else if (str8_eq(name, S("i16")))
      n->kind = NODE_TYPE_I16;
    else if (str8_eq(name, S("i32")))
      n->kind = NODE_TYPE_I32;
    else if (str8_eq(name, S("i64")))
      n->kind = NODE_TYPE_I64;
    else if (str8_eq(name, S("f32")))
      n->kind = NODE_TYPE_F32;
    else if (str8_eq(name, S("f64")))
      n->kind = NODE_TYPE_F64;
    else if (str8_eq(name, S("string")))
      n->kind = NODE_TYPE_STRING;
    else {
      if (match_tok(p, TK_LPAREN)) {
        n->kind = NODE_TYPE_GENERIC;
        Node *first = NULL, *last = NULL;
        while (!check(p, TK_RPAREN) && !check(p, TK_EOF)) {
          Node *arg = parse_type(a, p);
          if (last)
            last->next = arg;
          else
            first = arg;
          last = arg;
          if (!match_tok(p, TK_COMMA))
            break;
        }
        expect(p, TK_RPAREN, "expected ')' after type arguments");
        n->ptr = first;
      } else {
        n->kind = NODE_TYPE_NAMED;
      }
    }
    return n;
  }
  error_at(p->current.pos, p->current.len, "expected type");
  return NULL;
}

static Node *parse_params(Arena *a, Parser *p) {
  Node *first = NULL, *last = NULL;
  while (!check(p, TK_RPAREN) && !check(p, TK_EOF)) {
    skip_newlines(p);
    if (check(p, TK_RPAREN))
      break;
    if (match_tok(p, TK_DOT3)) {
      Node *varargs = NEW(a, Node);
      varargs->kind = NODE_VARARGS;
      varargs->token = p->previous;
      if (last)
        last->next = varargs;
      else
        first = varargs;
      break;
    }
    Node *param = NEW(a, Node);
    param->kind = NODE_PARAM;
    expect(p, TK_IDENT, "expected parameter name");
    param->token = p->previous;
    expect(p, TK_COLON, "expected ':' after parameter name");
    param->type = parse_type(a, p);
    if (last)
      last->next = param;
    else
      first = param;
    last = param;
    if (!match_tok(p, TK_COMMA))
      break;
  }
  return first;
}

static Node *parse_type_params(Arena *a, Parser *p) {
  if (!match_tok(p, TK_LPAREN))
    return NULL;
  Node *first = NULL, *last = NULL;
  while (!check(p, TK_RPAREN) && !check(p, TK_EOF)) {
    Node *tp = NEW(a, Node);
    tp->kind = NODE_TYPE_PARAM;
    expect(p, TK_IDENT, "expected type parameter name");
    tp->token = p->previous;
    if (last)
      last->next = tp;
    else
      first = tp;
    last = tp;
    if (!match_tok(p, TK_COMMA))
      break;
  }
  expect(p, TK_RPAREN, "expected ')' after type parameters");
  return first;
}

static Node *parse_primary(Arena *a, Parser *p);

static Node *parse_postfix(Arena *a, Parser *p);

static Node *parse_binary(Arena *a, Parser *p, int min_prec);

static Node *parse_primary(Arena *a, Parser *p) {
  if (match_tok(p, TK_LPAREN)) {
    Node *e = parse_expr(a, p);
    expect(p, TK_RPAREN, "expected ')'");
    return e;
  }
  if (match_tok(p, TK_LBRACE)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_BLOCK;
    n->token = p->previous;
    Node *first = NULL, *last = NULL;
    skip_newlines(p);
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
      Node *s = parse_stmt(a, p);
      if (last)
        last->next = s;
      else
        first = s;
      last = s;
      skip_newlines(p);
    }
    expect(p, TK_RBRACE, "expected '}'");
    n->ptr = first;
    return n;
  }
  if (match_tok(p, TK_LBRACKET)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_ARRAY_LIT;
    n->token = p->previous;
    Node *first = NULL, *last = NULL;
    skip_newlines(p);
    while (!check(p, TK_RBRACKET) && !check(p, TK_EOF)) {
      skip_newlines(p);
      if (check(p, TK_RBRACKET))
        break;
      Node *elem = parse_expr(a, p);
      if (last)
        last->next = elem;
      else
        first = elem;
      last = elem;
      if (!match_tok(p, TK_COMMA))
        break;
      skip_newlines(p);
    }
    expect(p, TK_RBRACKET, "expected ']' after array literal");
    n->ptr = first;
    return n;
  }
  if (match_tok(p, TK_IF)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_IF;
    n->token = p->previous;
    n->left = parse_expr(a, p);
    n->right = parse_expr(a, p);
    if (match_tok(p, TK_ELSE)) {
      n->ptr = parse_expr(a, p);
    }
    return n;
  }
  if (match_tok(p, TK_FOR)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_FOR;
    n->token = p->previous;
    if (check(p, TK_IDENT)) {
      Lexer saved = p->lexer;
      Token saved_cur = p->current;
      advance(p);
      if (check(p, TK_IN)) {
        advance(p);
        Node *iter_var = NEW(a, Node);
        iter_var->kind = NODE_VAR;
        iter_var->token = saved_cur;
        iter_var->left = parse_expr(a, p);
        n->ptr = iter_var;
        n->right = parse_expr(a, p);
        return n;
      }
      p->lexer = saved;
      p->current = saved_cur;
    }
    n->left = parse_expr(a, p);
    n->right = parse_expr(a, p);
    return n;
  }
  if (match_tok(p, TK_MATCH)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_MATCH;
    n->token = p->previous;
    n->left = parse_expr(a, p);
    expect(p, TK_LBRACE, "expected '{' after match expression");
    skip_newlines(p);
    Node *first = NULL, *last = NULL;
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
      skip_newlines(p);
      if (check(p, TK_RBRACE))
        break;
      Node *arm = NEW(a, Node);
      arm->kind = NODE_MATCH_ARM;
      arm->token = p->current;
      arm->left = parse_expr(a, p);
      expect(p, TK_FATARROW, "expected '=>' after match pattern");
      arm->right = parse_expr(a, p);
      if (last)
        last->next = arm;
      else
        first = arm;
      last = arm;
      match_tok(p, TK_COMMA);
      skip_newlines(p);
    }
    expect(p, TK_RBRACE, "expected '}' after match arms");
    n->ptr = first;
    return n;
  }
  if (match_tok(p, TK_NULL)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_NULL;
    n->token = p->previous;
    return n;
  }
  if (match_tok(p, TK_CAST)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_CAST;
    n->token = p->previous;
    expect(p, TK_LPAREN, "expected '(' after 'cast'");
    n->type = parse_type(a, p);
    expect(p, TK_COMMA, "expected ',' after type in cast");
    n->left = parse_expr(a, p);
    expect(p, TK_RPAREN, "expected ')' after cast expression");
    return n;
  }
  if (match_tok(p, TK_SIZEOF)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_SIZEOF;
    n->token = p->previous;
    expect(p, TK_LPAREN, "expected '(' after 'sizeof'");
    n->left = parse_type(a, p);
    expect(p, TK_RPAREN, "expected ')' after sizeof type");
    return n;
  }
  if (match_tok(p, TK_ALIGNOF)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_ALIGNOF;
    n->token = p->previous;
    expect(p, TK_LPAREN, "expected '(' after 'alignof'");
    n->left = parse_type(a, p);
    expect(p, TK_RPAREN, "expected ')' after alignof type");
    return n;
  }
  if (check(p, TK_MINUS) || check(p, TK_BANG) || check(p, TK_CARET) ||
      check(p, TK_STAR) || check(p, TK_TILDE)) {
    Token op = p->current;
    advance(p);
    Node *n = NEW(a, Node);
    n->kind = NODE_UNARY;
    n->token = op;
    n->op = op.kind;
    n->right = parse_binary(a, p, 100);
    return n;
  }
  if (match_tok(p, TK_INT)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_INT;
    n->token = p->previous;
    return n;
  }
  if (match_tok(p, TK_FLOAT)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_FLOAT;
    n->token = p->previous;
    return n;
  }
  if (match_tok(p, TK_CHAR)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_CHAR;
    n->token = p->previous;
    return n;
  }
  if (match_tok(p, TK_STRING)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_STRING;
    n->token = p->previous;
    return n;
  }
  if (match_tok(p, TK_TRUE) || match_tok(p, TK_FALSE)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_BOOL;
    n->token = p->previous;
    return n;
  }
  if (match_tok(p, TK_IDENT)) {
    Node *n = NEW(a, Node);
    n->token = p->previous;
    Str8 ident_text = tok_text(n->token);
    if (ident_text.len == 1 && ident_text.data[0] == '_') {
      n->kind = NODE_UNDERSCORE;
      return n;
    }
    n->kind = NODE_IDENT;
    return n;
  }
  error_at(p->current.pos, p->current.len, "expected expression");
  advance(p);
  return NULL;
}

static Node *parse_postfix(Arena *a, Parser *p) {
  Node *n = parse_primary(a, p);
  if (!n)
    return NULL;
  for (;;) {
    if (match_tok(p, TK_LPAREN)) {
      Node *call = NEW(a, Node);
      call->kind = NODE_CALL;
      call->token = n->token;
      call->left = n;
      Node *first = NULL, *last = NULL;
      while (!check(p, TK_RPAREN) && !check(p, TK_EOF)) {
        Node *arg = parse_expr(a, p);
        if (last)
          last->next = arg;
        else
          first = arg;
        last = arg;
        if (!match_tok(p, TK_COMMA))
          break;
      }
      expect(p, TK_RPAREN, "expected ')' after arguments");
      call->ptr = first;
      n = call;
      continue;
    }
    if (match_tok(p, TK_LBRACKET)) {
      Node *idx = NEW(a, Node);
      idx->token = n->token;
      idx->left = n;
      if (match_tok(p, TK_DOT2) || match_tok(p, TK_DOT2EQ)) {
        idx->kind = NODE_SLICE;
        idx->op = p->previous.kind;
        if (!check(p, TK_RBRACKET)) {
          idx->right = parse_binary(a, p, 11);
        }
        expect(p, TK_RBRACKET, "expected ']' after slice");
        n = idx;
        continue;
      }
      Node *first_expr = parse_binary(a, p, 11);
      if (match_tok(p, TK_DOT2) || match_tok(p, TK_DOT2EQ)) {
        idx->kind = NODE_SLICE;
        idx->op = p->previous.kind;
        idx->ptr = first_expr;
        if (!check(p, TK_RBRACKET)) {
          idx->right = parse_binary(a, p, 11);
        }
        expect(p, TK_RBRACKET, "expected ']' after slice");
        n = idx;
        continue;
      }
      idx->kind = NODE_INDEX;
      idx->right = first_expr;
      expect(p, TK_RBRACKET, "expected ']' after index");
      n = idx;
      continue;
    }
    if (match_tok(p, TK_DOT)) {
      if (match_tok(p, TK_LBRACE)) {
        Node *lit = NEW(a, Node);
        lit->kind = NODE_STRUCT_LIT;
        lit->token = n->token;
        Node *type_node;
        if (n->kind == NODE_IDENT) {
          type_node = NEW(a, Node);
          type_node->kind = NODE_TYPE_NAMED;
          type_node->token = n->token;
        } else if (n->kind == NODE_CALL && n->left &&
                   n->left->kind == NODE_IDENT) {
          type_node = NEW(a, Node);
          type_node->kind = NODE_TYPE_GENERIC;
          type_node->token = n->left->token;
          type_node->ptr = n->ptr;
        } else {
          type_node = NEW(a, Node);
          type_node->kind = NODE_TYPE_NAMED;
          type_node->token = n->token;
        }
        lit->type = type_node;
        skip_newlines(p);
        Node *first = NULL, *last = NULL;
        while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
          skip_newlines(p);
          if (check(p, TK_RBRACE))
            break;
          Node *field_init = NEW(a, Node);
          field_init->kind = NODE_FIELD_INIT;
          expect(p, TK_IDENT, "expected field name");
          field_init->token = p->previous;
          expect(p, TK_ASSIGN, "expected '=' after field name");
          field_init->left = parse_expr(a, p);
          if (last)
            last->next = field_init;
          else
            first = field_init;
          last = field_init;
          match_tok(p, TK_COMMA);
          skip_newlines(p);
        }
        expect(p, TK_RBRACE, "expected '}' after struct literal");
        lit->ptr = first;
        n = lit;
        continue;
      }
      Node *field = NEW(a, Node);
      field->kind = NODE_FIELD;
      field->left = n;
      expect(p, TK_IDENT, "expected field name after '.'");
      field->token = p->previous;
      n = field;
      continue;
    }
    break;
  }
  return n;
}

static int get_precedence(TokenKind kind) {
  switch (kind) {
  case TK_ASSIGN:
  case TK_PLUS_EQ:
  case TK_MINUS_EQ:
  case TK_STAR_EQ:
  case TK_SLASH_EQ:
  case TK_PERCENT_EQ:
  case TK_AMP_EQ:
  case TK_PIPE_EQ:
  case TK_CARET_EQ:
  case TK_SHL_EQ:
  case TK_SHR_EQ:
    return 1;
  case TK_OR:
    return 2;
  case TK_AND:
    return 3;
  case TK_EQ:
  case TK_NE:
  case TK_LT:
  case TK_GT:
  case TK_LE:
  case TK_GE:
    return 4;
  case TK_PIPE:
    return 5;
  case TK_CARET:
    return 6;
  case TK_AMP:
    return 7;
  case TK_SHL:
  case TK_SHR:
    return 8;
  case TK_PLUS:
  case TK_MINUS:
    return 9;
  case TK_STAR:
  case TK_SLASH:
  case TK_PERCENT:
    return 10;
  case TK_DOT2:
  case TK_DOT2EQ:
    return 11;
  default:
    return 0;
  }
}

static bool is_right_assoc(TokenKind kind) {
  switch (kind) {
  case TK_ASSIGN:
  case TK_PLUS_EQ:
  case TK_MINUS_EQ:
  case TK_STAR_EQ:
  case TK_SLASH_EQ:
  case TK_PERCENT_EQ:
  case TK_AMP_EQ:
  case TK_PIPE_EQ:
  case TK_CARET_EQ:
  case TK_SHL_EQ:
  case TK_SHR_EQ:
    return true;
  default:
    return false;
  }
}

static Node *parse_binary(Arena *a, Parser *p, int min_prec) {
  Node *left = parse_postfix(a, p);
  while (left) {
    int prec = get_precedence(p->current.kind);
    if (prec == 0 || prec <= min_prec)
      break;
    Token op = p->current;
    advance(p);
    int next_min = is_right_assoc(op.kind) ? prec - 1 : prec;
    Node *right = parse_binary(a, p, next_min);
    NodeKind kind = NODE_BINARY;
    if (op.kind == TK_DOT2 || op.kind == TK_DOT2EQ) {
      kind = NODE_RANGE;
    }
    Node *bin = NEW(a, Node);
    bin->kind = kind;
    bin->token = op;
    bin->op = op.kind;
    bin->left = left;
    bin->right = right;
    left = bin;
  }
  return left;
}

static Node *parse_expr(Arena *a, Parser *p) { return parse_binary(a, p, 0); }

static Node *parse_expr_list(Arena *a, Parser *p) {
  Node *first = parse_expr(a, p);
  if (!first)
    return NULL;
  if (!check(p, TK_COMMA)) {
    return first;
  }
  Node *tuple = NEW(a, Node);
  tuple->kind = NODE_TUPLE;
  tuple->token = first->token;
  Node *last = first;
  while (match_tok(p, TK_COMMA)) {
    Node *e = parse_expr(a, p);
    if (e) {
      last->next = e;
      last = e;
    }
  }
  tuple->ptr = first;
  return tuple;
}

static Node *parse_stmt(Arena *a, Parser *p) {
  if (match_tok(p, TK_RETURN)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_RETURN;
    n->token = p->previous;
    if (!check(p, TK_NEWLINE) && !check(p, TK_RBRACE) && !check(p, TK_EOF)) {
      n->left = parse_expr_list(a, p);
    }
    return n;
  }
  if (match_tok(p, TK_BREAK)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_BREAK;
    n->token = p->previous;
    return n;
  }
  if (match_tok(p, TK_CONTINUE)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_CONTINUE;
    n->token = p->previous;
    return n;
  }
  if (match_tok(p, TK_DEFER)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_DEFER;
    n->token = p->previous;
    n->left = parse_expr(a, p);
    return n;
  }
  if (check(p, TK_IDENT)) {
    Lexer saved = p->lexer;
    Token saved_cur = p->current;
    advance(p);
    if (check(p, TK_COLON) || check(p, TK_COLON2) || check(p, TK_COLONEQ)) {
      p->lexer = saved;
      p->current = saved_cur;
      return parse_decl(a, p);
    }
    if (check(p, TK_COMMA)) {
      p->lexer = saved;
      p->current = saved_cur;
      return parse_decl(a, p);
    }
    p->lexer = saved;
    p->current = saved_cur;
  }
  return parse_expr(a, p);
}

static Node *parse_struct_fields(Arena *a, Parser *p) {
  Node *first = NULL, *last = NULL;
  while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
    skip_newlines(p);
    if (check(p, TK_RBRACE))
      break;
    Node *field = NEW(a, Node);
    field->kind = NODE_STRUCT_FIELD;
    expect(p, TK_IDENT, "expected field name");
    field->token = p->previous;
    expect(p, TK_COLON, "expected ':' after field name");
    field->type = parse_type(a, p);
    if (last)
      last->next = field;
    else
      first = field;
    last = field;
    match_tok(p, TK_COMMA);
    skip_newlines(p);
  }
  return first;
}

static Node *parse_enum_variants(Arena *a, Parser *p) {
  Node *first = NULL, *last = NULL;
  while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
    skip_newlines(p);
    if (check(p, TK_RBRACE))
      break;
    Node *variant = NEW(a, Node);
    variant->kind = NODE_ENUM_VARIANT;
    expect(p, TK_IDENT, "expected variant name");
    variant->token = p->previous;
    if (match_tok(p, TK_ASSIGN)) {
      variant->left = parse_expr(a, p);
    }
    if (last)
      last->next = variant;
    else
      first = variant;
    last = variant;
    match_tok(p, TK_COMMA);
    skip_newlines(p);
  }
  return first;
}

static Node *parse_return_type(Arena *a, Parser *p) {
  Node *first = parse_type(a, p);
  if (!first)
    return NULL;
  if (!check(p, TK_COMMA)) {
    return first;
  }
  Node *tuple = NEW(a, Node);
  tuple->kind = NODE_TYPE_TUPLE;
  tuple->token = first->token;
  Node *last = first;
  while (match_tok(p, TK_COMMA)) {
    Node *t = parse_type(a, p);
    if (t) {
      last->next = t;
      last = t;
    }
  }
  tuple->ptr = first;
  return tuple;
}

static Node *parse_decl(Arena *a, Parser *p) {
  skip_newlines(p);
  if (match_tok(p, TK_IMPORT)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_IMPORT;
    expect(p, TK_STRING, "expected string path after 'import'");
    n->token = p->previous;
    return n;
  }
  if (match_tok(p, TK_FOREIGN)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_FOREIGN;
    if (match_tok(p, TK_STRING)) {
      Node *lib = NEW(a, Node);
      lib->kind = NODE_STRING;
      lib->token = p->previous;
      n->left = lib;
    }
    expect(p, TK_IDENT, "expected function name after 'foreign'");
    n->token = p->previous;
    expect(p, TK_COLON2, "expected '::' after function name");
    expect(p, TK_LPAREN, "expected '(' for parameter list");
    n->ptr = parse_params(a, p);
    expect(p, TK_RPAREN, "expected ')' after parameters");
    if (match_tok(p, TK_ARROW)) {
      n->type = parse_return_type(a, p);
    }
    return n;
  }
  expect(p, TK_IDENT, "expected declaration name");
  Token first_name = p->previous;
  Node *names = NULL;
  if (check(p, TK_COMMA)) {
    Node *first = NEW(a, Node);
    first->kind = NODE_IDENT;
    first->token = first_name;
    names = first;
    Node *last = first;
    while (match_tok(p, TK_COMMA)) {
      expect(p, TK_IDENT, "expected variable name");
      Node *n = NEW(a, Node);
      n->kind = NODE_IDENT;
      n->token = p->previous;
      last->next = n;
      last = n;
    }
    expect(p, TK_COLONEQ, "expected ':=' for multi-variable declaration");
    Node *n = NEW(a, Node);
    n->kind = NODE_VAR;
    n->token = first_name;
    n->ptr = names;
    n->left = parse_expr(a, p);
    return n;
  }
  Token name = first_name;
  if (match_tok(p, TK_COLON2)) {
    if (match_tok(p, TK_STRUCT)) {
      Node *n = NEW(a, Node);
      n->kind = NODE_STRUCT;
      n->token = name;
      n->right = parse_type_params(a, p);
      expect(p, TK_LBRACE, "expected '{' after 'struct'");
      skip_newlines(p);
      n->ptr = parse_struct_fields(a, p);
      expect(p, TK_RBRACE, "expected '}'");
      return n;
    }
    if (match_tok(p, TK_UNION)) {
      Node *n = NEW(a, Node);
      n->kind = NODE_UNION;
      n->token = name;
      n->right = parse_type_params(a, p);
      expect(p, TK_LBRACE, "expected '{' after 'union'");
      skip_newlines(p);
      n->ptr = parse_struct_fields(a, p);
      expect(p, TK_RBRACE, "expected '}'");
      return n;
    }
    if (match_tok(p, TK_ENUM)) {
      Node *n = NEW(a, Node);
      n->kind = NODE_ENUM;
      n->token = name;
      expect(p, TK_LBRACE, "expected '{' after 'enum'");
      skip_newlines(p);
      n->ptr = parse_enum_variants(a, p);
      expect(p, TK_RBRACE, "expected '}'");
      return n;
    }
    if (match_tok(p, TK_LPAREN)) {
      Node *n = NEW(a, Node);
      n->kind = NODE_FUNC;
      n->token = name;
      bool is_type_params = check(p, TK_IDENT) && (peek_next(p) == TK_COMMA ||
                                                   peek_next(p) == TK_RPAREN);
      if (is_type_params) {
        Node *first = NULL, *last = NULL;
        while (!check(p, TK_RPAREN) && !check(p, TK_EOF)) {
          Node *tp = NEW(a, Node);
          tp->kind = NODE_TYPE_PARAM;
          expect(p, TK_IDENT, "expected type parameter name");
          tp->token = p->previous;
          if (last)
            last->next = tp;
          else
            first = tp;
          last = tp;
          if (!match_tok(p, TK_COMMA))
            break;
        }
        expect(p, TK_RPAREN, "expected ')' after type parameters");
        n->right = first;
        expect(p, TK_LPAREN,
               "expected '(' for parameters after type parameters");
        n->ptr = parse_params(a, p);
        expect(p, TK_RPAREN, "expected ')' after parameters");
      } else {
        n->ptr = parse_params(a, p);
        expect(p, TK_RPAREN, "expected ')' after parameters");
      }
      if (match_tok(p, TK_ARROW)) {
        n->type = parse_return_type(a, p);
      }
      n->left = parse_expr(a, p);
      return n;
    }
    Node *n = NEW(a, Node);
    n->kind = NODE_CONST;
    n->token = name;
    n->left = parse_expr(a, p);
    return n;
  }
  if (match_tok(p, TK_COLONEQ)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_VAR;
    n->token = name;
    n->left = parse_expr(a, p);
    return n;
  }
  if (match_tok(p, TK_COLON)) {
    Node *n = NEW(a, Node);
    n->kind = NODE_VAR;
    n->token = name;
    n->type = parse_type(a, p);
    if (match_tok(p, TK_ASSIGN)) {
      n->left = parse_expr(a, p);
    }
    return n;
  }
  error_at(p->current.pos, p->current.len,
           "expected ':', '::', or ':=' after name");
  return NULL;
}
typedef struct Symbol Symbol;
struct Symbol {
  Str8 name;
  Node *decl;
  Symbol *next;
};
typedef struct Scope Scope;
struct Scope {
  Symbol *symbols;
  Scope *parent;
};
typedef struct {
  Arena *arena;
  Scope *global;
  Scope *current;
} Resolver;

static void scope_push(Resolver *r) {
  Scope *s = NEW(r->arena, Scope);
  s->parent = r->current;
  r->current = s;
}

static void scope_pop(Resolver *r) {
  if (r->current && r->current != r->global) {
    r->current = r->current->parent;
  }
}

static void scope_add(Resolver *r, Str8 name, Node *decl) {
  Symbol *sym = NEW(r->arena, Symbol);
  sym->name = name;
  sym->decl = decl;
  sym->next = r->current->symbols;
  r->current->symbols = sym;
}

static Node *scope_lookup(Resolver *r, Str8 name) {
  for (Scope *s = r->current; s; s = s->parent) {
    for (Symbol *sym = s->symbols; sym; sym = sym->next) {
      if (str8_eq(sym->name, name)) {
        return sym->decl;
      }
    }
  }
  return NULL;
}

static void resolve_node(Resolver *r, Node *n);

static void resolve_type(Resolver *r, Node *type);

static void resolve_type(Resolver *r, Node *type) {
  if (!type)
    return;
  switch (type->kind) {
  case NODE_TYPE_NAMED: {
    Str8 name = tok_text(type->token);
    Node *decl = scope_lookup(r, name);
    if (!decl) {
      error_at(type->token.pos, type->token.len, "unknown type '" STR_FMT "'",
               STR_ARG(name));
    } else {
      type->decl = decl;
    }
    break;
  }
  case NODE_TYPE_GENERIC: {
    Str8 name = tok_text(type->token);
    Node *decl = scope_lookup(r, name);
    if (!decl) {
      error_at(type->token.pos, type->token.len, "unknown type '" STR_FMT "'",
               STR_ARG(name));
    } else {
      type->decl = decl;
      if (decl->kind == NODE_STRUCT || decl->kind == NODE_UNION) {
        int expected = 0, got = 0;
        for (Node *tp = decl->right; tp; tp = tp->next)
          expected++;
        for (Node *arg = type->ptr; arg; arg = arg->next)
          got++;
        if (expected != got) {
          error_at(type->token.pos, type->token.len,
                   "type '" STR_FMT "' expects %d type argument(s), got %d",
                   STR_ARG(name), expected, got);
        }
      }
    }
    for (Node *arg = type->ptr; arg; arg = arg->next) {
      resolve_type(r, arg);
    }
    break;
  }
  case NODE_TYPE_PTR:
  case NODE_TYPE_SLICE:
    resolve_type(r, type->ptr);
    break;
  case NODE_TYPE_ARRAY:
    resolve_node(r, type->left);
    resolve_type(r, type->ptr);
    break;
  case NODE_TYPE_TUPLE:
    for (Node *t = type->ptr; t; t = t->next) {
      resolve_type(r, t);
    }
    break;
  default:
    break;
  }
}

static void resolve_node(Resolver *r, Node *n) {
  if (!n)
    return;
  switch (n->kind) {
  case NODE_VAR: {
    resolve_type(r, n->type);
    resolve_node(r, n->left);
    if (r->current != r->global) {
      if (n->ptr) {
        for (Node *name = n->ptr; name; name = name->next) {
          scope_add(r, tok_text(name->token), n);
        }
      } else {
        scope_add(r, tok_text(n->token), n);
      }
    }
    break;
  }
  case NODE_CONST: {
    resolve_type(r, n->type);
    resolve_node(r, n->left);
    if (r->current != r->global) {
      scope_add(r, tok_text(n->token), n);
    }
    break;
  }
  case NODE_FUNC:
  case NODE_FOREIGN: {
    scope_push(r);
    for (Node *tp = n->right; tp; tp = tp->next) {
      scope_add(r, tok_text(tp->token), tp);
    }
    for (Node *param = n->ptr; param; param = param->next) {
      if (param->kind == NODE_PARAM) {
        resolve_type(r, param->type);
        scope_add(r, tok_text(param->token), param);
      }
    }
    resolve_type(r, n->type);
    if (n->kind == NODE_FUNC) {
      resolve_node(r, n->left);
    }
    scope_pop(r);
    break;
  }
  case NODE_STRUCT:
  case NODE_UNION: {
    scope_push(r);
    for (Node *tp = n->right; tp; tp = tp->next) {
      scope_add(r, tok_text(tp->token), tp);
    }
    for (Node *field = n->ptr; field; field = field->next) {
      resolve_type(r, field->type);
    }
    scope_pop(r);
    break;
  }
  case NODE_ENUM: {
    for (Node *variant = n->ptr; variant; variant = variant->next) {
      resolve_node(r, variant->left);
    }
    break;
  }
  case NODE_IDENT: {
    Str8 name = tok_text(n->token);
    Node *decl = scope_lookup(r, name);
    if (!decl) {
      error_at(n->token.pos, n->token.len, "undefined identifier '" STR_FMT "'",
               STR_ARG(name));
    } else {
      n->decl = decl;
    }
    break;
  }
  case NODE_BINARY:
  case NODE_RANGE:
    resolve_node(r, n->left);
    resolve_node(r, n->right);
    break;
  case NODE_UNARY:
    resolve_node(r, n->right);
    break;
  case NODE_CALL:
    resolve_node(r, n->left);
    for (Node *arg = n->ptr; arg; arg = arg->next) {
      resolve_node(r, arg);
    }
    break;
  case NODE_INDEX:
    resolve_node(r, n->left);
    resolve_node(r, n->right);
    break;
  case NODE_SLICE:
    resolve_node(r, n->left);
    resolve_node(r, n->ptr);
    resolve_node(r, n->right);
    break;
  case NODE_FIELD:
    resolve_node(r, n->left);
    break;
  case NODE_CAST:
    resolve_type(r, n->type);
    resolve_node(r, n->left);
    break;
  case NODE_SIZEOF:
  case NODE_ALIGNOF:
    resolve_type(r, n->left);
    break;
  case NODE_STRUCT_LIT:
    resolve_type(r, n->type);
    for (Node *init = n->ptr; init; init = init->next) {
      resolve_node(r, init->left);
    }
    break;
  case NODE_ARRAY_LIT:
    for (Node *elem = n->ptr; elem; elem = elem->next) {
      resolve_node(r, elem);
    }
    break;
  case NODE_TUPLE:
    for (Node *elem = n->ptr; elem; elem = elem->next) {
      resolve_node(r, elem);
    }
    break;
  case NODE_BLOCK: {
    scope_push(r);
    for (Node *stmt = n->ptr; stmt; stmt = stmt->next) {
      resolve_node(r, stmt);
    }
    scope_pop(r);
    break;
  }
  case NODE_IF:
    resolve_node(r, n->left);
    resolve_node(r, n->right);
    resolve_node(r, n->ptr);
    break;
  case NODE_FOR: {
    scope_push(r);
    if (n->ptr) {
      Node *iter_var = n->ptr;
      resolve_node(r, iter_var->left);
      scope_add(r, tok_text(iter_var->token), iter_var);
    } else {
      resolve_node(r, n->left);
    }
    resolve_node(r, n->right);
    scope_pop(r);
    break;
  }
  case NODE_MATCH:
    resolve_node(r, n->left);
    for (Node *arm = n->ptr; arm; arm = arm->next) {
      resolve_node(r, arm->left);
      resolve_node(r, arm->right);
    }
    break;
  case NODE_RETURN:
    resolve_node(r, n->left);
    break;
  case NODE_DEFER:
    resolve_node(r, n->left);
    break;
  case NODE_INT:
  case NODE_FLOAT:
  case NODE_CHAR:
  case NODE_STRING:
  case NODE_BOOL:
  case NODE_NULL:
  case NODE_UNDERSCORE:
  case NODE_BREAK:
  case NODE_CONTINUE:
  case NODE_IMPORT:
  case NODE_PARAM:
  case NODE_VARARGS:
  case NODE_TYPE_PARAM:
  case NODE_STRUCT_FIELD:
  case NODE_ENUM_VARIANT:
  case NODE_FIELD_INIT:
  case NODE_MATCH_ARM:
    break;
  default:
    break;
  }
}

static void resolve(Arena *arena, Node **decls, usize count) {
  Resolver r = {0};
  r.arena = arena;
  r.global = NEW(arena, Scope);
  r.current = r.global;
  for (usize i = 0; i < count; i++) {
    Node *n = decls[i];
    if (!n)
      continue;
    switch (n->kind) {
    case NODE_VAR:
    case NODE_CONST:
    case NODE_FUNC:
    case NODE_FOREIGN:
    case NODE_STRUCT:
    case NODE_UNION:
      scope_add(&r, tok_text(n->token), n);
      break;
    case NODE_ENUM: {
      scope_add(&r, tok_text(n->token), n);
      Node *enum_type = NEW(arena, Node);
      enum_type->kind = NODE_TYPE_NAMED;
      enum_type->token = n->token;
      enum_type->decl = n;
      for (Node *variant = n->ptr; variant; variant = variant->next) {
        scope_add(&r, tok_text(variant->token), variant);
        variant->type = enum_type;
      }
      break;
    }
    case NODE_IMPORT:
      break;
    default:
      break;
    }
  }
  for (usize i = 0; i < count; i++) {
    resolve_node(&r, decls[i]);
  }
}

static Node *type_void, *type_bool, *type_string;

static Node *type_i8, *type_i16, *type_i32, *type_i64;

static Node *type_u8, *type_u16, *type_u32, *type_u64;

static Node *type_f32, *type_f64;

static Node *type_null;

static Node *type_range;

static void init_builtin_types(Arena *a) {
  type_void = NEW(a, Node);
  type_void->kind = NODE_TYPE_VOID;
  type_bool = NEW(a, Node);
  type_bool->kind = NODE_TYPE_BOOL;
  type_string = NEW(a, Node);
  type_string->kind = NODE_TYPE_STRING;
  type_i8 = NEW(a, Node);
  type_i8->kind = NODE_TYPE_I8;
  type_i16 = NEW(a, Node);
  type_i16->kind = NODE_TYPE_I16;
  type_i32 = NEW(a, Node);
  type_i32->kind = NODE_TYPE_I32;
  type_i64 = NEW(a, Node);
  type_i64->kind = NODE_TYPE_I64;
  type_u8 = NEW(a, Node);
  type_u8->kind = NODE_TYPE_U8;
  type_u16 = NEW(a, Node);
  type_u16->kind = NODE_TYPE_U16;
  type_u32 = NEW(a, Node);
  type_u32->kind = NODE_TYPE_U32;
  type_u64 = NEW(a, Node);
  type_u64->kind = NODE_TYPE_U64;
  type_f32 = NEW(a, Node);
  type_f32->kind = NODE_TYPE_F32;
  type_f64 = NEW(a, Node);
  type_f64->kind = NODE_TYPE_F64;
  type_null = NEW(a, Node);
  type_null->kind = NODE_TYPE_PTR;
  type_null->ptr = type_void;
  type_range = NEW(a, Node);
  type_range->kind = NODE_TYPE_NAMED;
}
typedef struct {
  Arena *arena;
  Node *current_func;
  int loop_depth;
} TypeChecker;

static bool type_is_integer(Node *t) {
  if (!t)
    return false;
  switch (t->kind) {
  case NODE_TYPE_I8:
  case NODE_TYPE_I16:
  case NODE_TYPE_I32:
  case NODE_TYPE_I64:
  case NODE_TYPE_U8:
  case NODE_TYPE_U16:
  case NODE_TYPE_U32:
  case NODE_TYPE_U64:
    return true;
  default:
    return false;
  }
}

static bool type_is_signed(Node *t) {
  if (!t)
    return false;
  switch (t->kind) {
  case NODE_TYPE_I8:
  case NODE_TYPE_I16:
  case NODE_TYPE_I32:
  case NODE_TYPE_I64:
    return true;
  default:
    return false;
  }
}

static bool type_is_unsigned(Node *t) {
  if (!t)
    return false;
  switch (t->kind) {
  case NODE_TYPE_U8:
  case NODE_TYPE_U16:
  case NODE_TYPE_U32:
  case NODE_TYPE_U64:
    return true;
  default:
    return false;
  }
}

static bool type_is_float(Node *t) {
  if (!t)
    return false;
  return t->kind == NODE_TYPE_F32 || t->kind == NODE_TYPE_F64;
}

static bool type_is_numeric(Node *t) {
  return type_is_integer(t) || type_is_float(t);
}

static bool type_is_pointer(Node *t) { return t && t->kind == NODE_TYPE_PTR; }

static bool type_is_array(Node *t) { return t && t->kind == NODE_TYPE_ARRAY; }

static bool type_is_slice(Node *t) { return t && t->kind == NODE_TYPE_SLICE; }

static int type_integer_rank(Node *t) {
  if (!t)
    return 0;
  switch (t->kind) {
  case NODE_TYPE_I8:
  case NODE_TYPE_U8:
    return 1;
  case NODE_TYPE_I16:
  case NODE_TYPE_U16:
    return 2;
  case NODE_TYPE_I32:
  case NODE_TYPE_U32:
    return 3;
  case NODE_TYPE_I64:
  case NODE_TYPE_U64:
    return 4;
  default:
    return 0;
  }
}

static bool types_equal(Node *a, Node *b) {
  if (a == b)
    return true;
  if (!a || !b)
    return false;
  if (a->kind != b->kind)
    return false;
  switch (a->kind) {
  case NODE_TYPE_PTR:
    return types_equal(a->ptr, b->ptr);
  case NODE_TYPE_ARRAY:
    return types_equal(a->ptr, b->ptr);
  case NODE_TYPE_SLICE:
    return types_equal(a->ptr, b->ptr);
  case NODE_TYPE_TUPLE: {
    Node *ta = a->ptr, *tb = b->ptr;
    while (ta && tb) {
      if (!types_equal(ta, tb))
        return false;
      ta = ta->next;
      tb = tb->next;
    }
    return !ta && !tb;
  }
  case NODE_TYPE_NAMED:
    return a->decl == b->decl;
  case NODE_TYPE_GENERIC: {
    if (a->decl != b->decl)
      return false;
    Node *arg_a = a->ptr, *arg_b = b->ptr;
    while (arg_a && arg_b) {
      if (!types_equal(arg_a, arg_b))
        return false;
      arg_a = arg_a->next;
      arg_b = arg_b->next;
    }
    return !arg_a && !arg_b;
  }
  default:
    return true;
  }
}

static bool type_has_type_param(Node *t) {
  if (!t)
    return false;
  if (t->kind == NODE_TYPE_NAMED && t->decl &&
      t->decl->kind == NODE_TYPE_PARAM) {
    return true;
  }
  if (t->kind == NODE_TYPE_PTR || t->kind == NODE_TYPE_SLICE ||
      t->kind == NODE_TYPE_ARRAY) {
    return type_has_type_param(t->ptr);
  }
  if (t->kind == NODE_TYPE_TUPLE) {
    for (Node *elem = t->ptr; elem; elem = elem->next) {
      if (type_has_type_param(elem))
        return true;
    }
  }
  return false;
}

static bool types_assignable(Node *dst, Node *src) {
  if (!dst || !src)
    return false;
  if (types_equal(dst, src))
    return true;
  if (type_has_type_param(dst) || type_has_type_param(src)) {
    if (dst->kind == src->kind)
      return true;
    if (dst->kind == NODE_TYPE_SLICE && src->kind == NODE_TYPE_SLICE)
      return true;
  }
  if (dst->kind == NODE_TYPE_PTR && src == type_null) {
    return true;
  }
  if (type_is_integer(dst) && type_is_integer(src)) {
    bool dst_signed = type_is_signed(dst);
    bool src_signed = type_is_signed(src);
    if (dst_signed == src_signed) {
      return type_integer_rank(dst) >= type_integer_rank(src);
    }
    if (dst_signed && !src_signed) {
      return type_integer_rank(dst) > type_integer_rank(src);
    }
    return false;
  }
  if (type_is_float(dst) && type_is_float(src)) {
    return true;
  }
  if (dst->kind == NODE_TYPE_SLICE && src->kind == NODE_TYPE_ARRAY) {
    return types_equal(dst->ptr, src->ptr);
  }
  if (dst->kind == NODE_TYPE_SLICE && src->kind == NODE_TYPE_SLICE) {
    return types_assignable(dst->ptr, src->ptr);
  }
  if (dst->kind == NODE_TYPE_PTR && src->kind == NODE_TYPE_STRING) {
    return true;
  }
  return false;
}

static Node *common_numeric_type(Node *a, Node *b) {
  if (!a || !b)
    return NULL;
  if (type_is_float(a) || type_is_float(b)) {
    if (a->kind == NODE_TYPE_F64 || b->kind == NODE_TYPE_F64)
      return type_f64;
    return type_f32;
  }
  if (type_is_integer(a) && type_is_integer(b)) {
    int rank_a = type_integer_rank(a);
    int rank_b = type_integer_rank(b);
    if (rank_a >= rank_b)
      return a;
    return b;
  }
  return NULL;
}

static bool is_lvalue(Node *n) {
  if (!n)
    return false;
  switch (n->kind) {
  case NODE_IDENT:
    if (n->decl && n->decl->kind == NODE_CONST)
      return false;
    return true;
  case NODE_INDEX:
    return true;
  case NODE_FIELD:
    return true;
  case NODE_UNARY:
    if (n->op == TK_STAR)
      return true;
    return false;
  default:
    return false;
  }
}

static Node *typecheck_expr(TypeChecker *tc, Node *n);

static void typecheck_stmt(TypeChecker *tc, Node *n);

static void typecheck_decl(TypeChecker *tc, Node *n);

static void type_error(Node *n, const char *fmt, ...) {
  if (!n)
    return;
  g_err.had_error = true;
  u32 line, col;
  get_line_col(n->token.pos, &line, &col);
  fprintf(stderr, STR_FMT ":%u:%u: error: ", STR_ARG(g_err.filename), line,
          col);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
}

static const char *type_name(Node *t) {
  if (!t)
    return "<unknown>";
  if (t == type_range)
    return "range";
  if (t == type_null)
    return "null";
  switch (t->kind) {
  case NODE_TYPE_VOID:
    return "void";
  case NODE_TYPE_BOOL:
    return "bool";
  case NODE_TYPE_I8:
    return "i8";
  case NODE_TYPE_I16:
    return "i16";
  case NODE_TYPE_I32:
    return "i32";
  case NODE_TYPE_I64:
    return "i64";
  case NODE_TYPE_U8:
    return "u8";
  case NODE_TYPE_U16:
    return "u16";
  case NODE_TYPE_U32:
    return "u32";
  case NODE_TYPE_U64:
    return "u64";
  case NODE_TYPE_F32:
    return "f32";
  case NODE_TYPE_F64:
    return "f64";
  case NODE_TYPE_STRING:
    return "string";
  case NODE_TYPE_PTR:
    return "^...";
  case NODE_TYPE_ARRAY:
    return "[N]...";
  case NODE_TYPE_SLICE:
    return "[]...";
  case NODE_TYPE_TUPLE:
    return "(...)";
  case NODE_TYPE_NAMED:
  case NODE_TYPE_GENERIC:
    return "named";
  default:
    return "<invalid>";
  }
}

static Node *make_ptr_type(Arena *a, Node *base) {
  Node *ptr = NEW(a, Node);
  ptr->kind = NODE_TYPE_PTR;
  ptr->ptr = base;
  return ptr;
}

static Node *make_slice_type(Arena *a, Node *elem) {
  Node *slice = NEW(a, Node);
  slice->kind = NODE_TYPE_SLICE;
  slice->ptr = elem;
  return slice;
}

static Node *get_element_type(Node *t) {
  if (!t)
    return NULL;
  if (t->kind == NODE_TYPE_ARRAY || t->kind == NODE_TYPE_SLICE) {
    return t->ptr;
  }
  return NULL;
}

static Node *substitute_type(Arena *a, Node *type, Node *type_params,
                             Node *type_args) {
  if (!type)
    return NULL;
  switch (type->kind) {
  case NODE_TYPE_NAMED: {
    if (type->decl && type->decl->kind == NODE_TYPE_PARAM) {
      Node *param = type_params;
      Node *arg = type_args;
      while (param && arg) {
        if (param == type->decl) {
          return arg;
        }
        param = param->next;
        arg = arg->next;
      }
    }
    return type;
  }
  case NODE_TYPE_PTR: {
    Node *sub = substitute_type(a, type->ptr, type_params, type_args);
    if (sub == type->ptr)
      return type;
    return make_ptr_type(a, sub);
  }
  case NODE_TYPE_SLICE: {
    Node *sub = substitute_type(a, type->ptr, type_params, type_args);
    if (sub == type->ptr)
      return type;
    return make_slice_type(a, sub);
  }
  case NODE_TYPE_ARRAY: {
    Node *sub = substitute_type(a, type->ptr, type_params, type_args);
    if (sub == type->ptr)
      return type;
    Node *arr = NEW(a, Node);
    arr->kind = NODE_TYPE_ARRAY;
    arr->left = type->left;
    arr->ptr = sub;
    return arr;
  }
  default:
    return type;
  }
}

static Node *lookup_field(TypeChecker *tc, Node *struct_type, Str8 field_name,
                          Node **out_type) {
  if (!struct_type)
    return NULL;
  Node *decl = NULL;
  Node *type_params = NULL;
  Node *type_args = NULL;
  if (struct_type->kind == NODE_TYPE_NAMED) {
    decl = struct_type->decl;
  } else if (struct_type->kind == NODE_TYPE_GENERIC) {
    decl = struct_type->decl;
    type_args = struct_type->ptr;
  }
  if (!decl || (decl->kind != NODE_STRUCT && decl->kind != NODE_UNION)) {
    return NULL;
  }
  type_params = decl->right;
  for (Node *field = decl->ptr; field; field = field->next) {
    if (field->kind == NODE_STRUCT_FIELD) {
      Str8 fname = tok_text(field->token);
      if (str8_eq(fname, field_name)) {
        Node *field_type = field->type;
        if (type_params && type_args) {
          field_type =
              substitute_type(tc->arena, field_type, type_params, type_args);
        }
        if (out_type)
          *out_type = field_type;
        return field;
      }
    }
  }
  return NULL;
}

static Node *typecheck_expr(TypeChecker *tc, Node *n) {
  if (!n)
    return NULL;
  switch (n->kind) {
  case NODE_INT:
    n->type = type_i32;
    return type_i32;
  case NODE_FLOAT:
    n->type = type_f64;
    return type_f64;
  case NODE_CHAR:
    n->type = type_u8;
    return type_u8;
  case NODE_STRING:
    n->type = type_string;
    return type_string;
  case NODE_BOOL:
    n->type = type_bool;
    return type_bool;
  case NODE_NULL:
    n->type = type_null;
    return type_null;
  case NODE_IDENT: {
    if (!n->decl) {
      return NULL;
    }
    Node *decl = n->decl;
    switch (decl->kind) {
    case NODE_VAR:
      if (decl->ptr) {
        for (Node *name = decl->ptr; name; name = name->next) {
          if (str8_eq(tok_text(name->token), tok_text(n->token))) {
            if (name->type) {
              n->type = name->type;
              return name->type;
            }
            break;
          }
        }
      }
      n->type = decl->type;
      return decl->type;
    case NODE_CONST:
    case NODE_PARAM:
      n->type = decl->type;
      return decl->type;
    case NODE_FUNC:
    case NODE_FOREIGN:
      n->type = decl->type;
      return decl->type;
    case NODE_ENUM_VARIANT:
      n->type = decl->type;
      return decl->type;
    default:
      return NULL;
    }
  }
  case NODE_UNDERSCORE:
    return NULL;
  case NODE_BINARY: {
    Node *left_type = typecheck_expr(tc, n->left);
    Node *right_type = typecheck_expr(tc, n->right);
    switch (n->op) {
    case TK_PLUS:
    case TK_MINUS:
    case TK_STAR:
    case TK_SLASH:
    case TK_PERCENT:
    case TK_PLUS_EQ:
    case TK_MINUS_EQ:
    case TK_STAR_EQ:
    case TK_SLASH_EQ:
    case TK_PERCENT_EQ: {
      if (!type_is_numeric(left_type) || !type_is_numeric(right_type)) {
        type_error(n, "arithmetic operators require numeric operands");
        return NULL;
      }
      Node *result = common_numeric_type(left_type, right_type);
      n->type = result;
      if (n->op >= TK_PLUS_EQ && n->op <= TK_PERCENT_EQ) {
        if (!is_lvalue(n->left)) {
          type_error(n->left, "cannot assign to this expression");
        }
        return left_type;
      }
      return result;
    }
    case TK_AMP:
    case TK_PIPE:
    case TK_CARET:
    case TK_SHL:
    case TK_SHR:
    case TK_AMP_EQ:
    case TK_PIPE_EQ:
    case TK_CARET_EQ:
    case TK_SHL_EQ:
    case TK_SHR_EQ: {
      if (!type_is_integer(left_type) || !type_is_integer(right_type)) {
        type_error(n, "bitwise operators require integer operands");
        return NULL;
      }
      Node *result = (n->op == TK_SHL || n->op == TK_SHR ||
                      n->op == TK_SHL_EQ || n->op == TK_SHR_EQ)
                         ? left_type
                         : common_numeric_type(left_type, right_type);
      n->type = result;
      if (n->op >= TK_AMP_EQ && n->op <= TK_SHR_EQ) {
        if (!is_lvalue(n->left)) {
          type_error(n->left, "cannot assign to this expression");
        }
        return left_type;
      }
      return result;
    }
    case TK_EQ:
    case TK_NE:
    case TK_LT:
    case TK_GT:
    case TK_LE:
    case TK_GE: {
      if (type_is_numeric(left_type) && type_is_numeric(right_type)) {
      } else if (types_equal(left_type, right_type)) {
      } else if (type_is_pointer(left_type) && right_type == type_null) {
      } else if (type_is_pointer(right_type) && left_type == type_null) {
      } else if (type_is_pointer(left_type) && type_is_pointer(right_type)) {
        Node *base_left = left_type->ptr;
        Node *base_right = right_type->ptr;
        if (!types_equal(base_left, base_right) &&
            base_left->kind != NODE_TYPE_VOID &&
            base_right->kind != NODE_TYPE_VOID) {
          type_error(n, "cannot compare pointers to incompatible types");
        }
      } else {
        type_error(n, "cannot compare incompatible types");
      }
      n->type = type_bool;
      return type_bool;
    }
    case TK_AND:
    case TK_OR: {
      if (left_type != type_bool || right_type != type_bool) {
        type_error(n, "logical operators require boolean operands");
      }
      n->type = type_bool;
      return type_bool;
    }
    case TK_ASSIGN: {
      if (!is_lvalue(n->left)) {
        type_error(n->left, "cannot assign to this expression");
      }
      if (!types_assignable(left_type, right_type)) {
        type_error(n, "cannot assign '%s' to '%s'", type_name(right_type),
                   type_name(left_type));
      }
      n->type = left_type;
      return left_type;
    }
    default:
      type_error(n, "unknown binary operator");
      return NULL;
    }
  }
  case NODE_RANGE: {
    Node *left_type = typecheck_expr(tc, n->left);
    Node *right_type = typecheck_expr(tc, n->right);
    if (!type_is_integer(left_type) || !type_is_integer(right_type)) {
      type_error(n, "range bounds must be integers");
    }
    n->type = type_range;
    return type_range;
  }
  case NODE_UNARY: {
    Node *operand_type = typecheck_expr(tc, n->right);
    switch (n->op) {
    case TK_MINUS:
      if (!type_is_numeric(operand_type)) {
        type_error(n, "negation requires numeric operand");
        return NULL;
      }
      n->type = operand_type;
      return operand_type;
    case TK_BANG:
      if (operand_type != type_bool) {
        type_error(n, "logical not requires boolean operand");
        return NULL;
      }
      n->type = type_bool;
      return type_bool;
    case TK_TILDE:
      if (!type_is_integer(operand_type)) {
        type_error(n, "bitwise not requires integer operand");
        return NULL;
      }
      n->type = operand_type;
      return operand_type;
    case TK_CARET:
      if (!is_lvalue(n->right)) {
        type_error(n->right, "cannot take address of non-lvalue");
        return NULL;
      }
      n->type = make_ptr_type(tc->arena, operand_type);
      return n->type;
    case TK_STAR:
      if (!type_is_pointer(operand_type)) {
        type_error(n, "cannot dereference non-pointer type");
        return NULL;
      }
      n->type = operand_type->ptr;
      return n->type;
    default:
      type_error(n, "unknown unary operator");
      return NULL;
    }
  }
  case NODE_CALL: {
    typecheck_expr(tc, n->left);
    Node *func_decl = NULL;
    if (n->left && n->left->kind == NODE_IDENT) {
      func_decl = n->left->decl;
    }
    if (!func_decl ||
        (func_decl->kind != NODE_FUNC && func_decl->kind != NODE_FOREIGN)) {
      type_error(n, "cannot call non-function");
      return NULL;
    }
    Node *type_params = func_decl->right;
    Node *inferred_args = NULL;
    if (type_params) {
      Node *first_inferred = NULL, *last_inferred = NULL;
      int num_type_params = 0;
      for (Node *tp = type_params; tp; tp = tp->next)
        num_type_params++;
      Node **inferred = NEW_ARRAY(tc->arena, Node *, num_type_params);
      Node *param = func_decl->ptr;
      Node *arg = n->ptr;
      while (param && arg && param->kind == NODE_PARAM) {
        Node *arg_type = typecheck_expr(tc, arg);
        if (param->type && param->type->kind == NODE_TYPE_NAMED &&
            param->type->decl && param->type->decl->kind == NODE_TYPE_PARAM) {
          int idx = 0;
          for (Node *tp = type_params; tp; tp = tp->next, idx++) {
            if (tp == param->type->decl) {
              if (!inferred[idx]) {
                inferred[idx] = arg_type;
              }
              break;
            }
          }
        } else if (param->type && param->type->kind == NODE_TYPE_PTR &&
                   param->type->ptr &&
                   param->type->ptr->kind == NODE_TYPE_NAMED &&
                   param->type->ptr->decl &&
                   param->type->ptr->decl->kind == NODE_TYPE_PARAM) {
          if (arg_type && arg_type->kind == NODE_TYPE_PTR) {
            int idx = 0;
            for (Node *tp = type_params; tp; tp = tp->next, idx++) {
              if (tp == param->type->ptr->decl) {
                if (!inferred[idx]) {
                  inferred[idx] = arg_type->ptr;
                }
                break;
              }
            }
          }
        } else if (param->type && param->type->kind == NODE_TYPE_SLICE &&
                   param->type->ptr &&
                   param->type->ptr->kind == NODE_TYPE_NAMED &&
                   param->type->ptr->decl &&
                   param->type->ptr->decl->kind == NODE_TYPE_PARAM) {
          if (arg_type && arg_type->kind == NODE_TYPE_SLICE) {
            int idx = 0;
            for (Node *tp = type_params; tp; tp = tp->next, idx++) {
              if (tp == param->type->ptr->decl) {
                if (!inferred[idx]) {
                  inferred[idx] = arg_type->ptr;
                }
                break;
              }
            }
          }
        }
        param = param->next;
        arg = arg->next;
      }
      for (int i = 0; i < num_type_params; i++) {
        if (!inferred[i]) {
          type_error(n, "could not infer type parameter %d", i + 1);
          inferred[i] = type_void;
        }
        Node *type_arg = NEW(tc->arena, Node);
        type_arg->kind = inferred[i]->kind;
        type_arg->token = inferred[i]->token;
        type_arg->ptr = inferred[i]->ptr;
        type_arg->decl = inferred[i]->decl;
        type_arg->next = NULL;
        if (last_inferred)
          last_inferred->next = type_arg;
        else
          first_inferred = type_arg;
        last_inferred = type_arg;
      }
      inferred_args = first_inferred;
    }
    Node *param = func_decl->ptr;
    Node *arg = n->ptr;
    int arg_num = 0;
    while (param && arg) {
      if (param->kind == NODE_VARARGS) {
        while (arg) {
          typecheck_expr(tc, arg);
          arg = arg->next;
        }
        break;
      }
      arg_num++;
      Node *arg_type = typecheck_expr(tc, arg);
      Node *expected_type = param->type;
      if (type_params && inferred_args) {
        expected_type =
            substitute_type(tc->arena, param->type, type_params, inferred_args);
      }
      if (!types_assignable(expected_type, arg_type)) {
        type_error(arg, "argument %d: cannot convert '%s' to '%s'", arg_num,
                   type_name(arg_type), type_name(expected_type));
      }
      param = param->next;
      arg = arg->next;
    }
    if (param && param->kind != NODE_VARARGS) {
      type_error(n, "not enough arguments in function call");
    }
    if (arg && !(param && param->kind == NODE_VARARGS)) {
      type_error(n, "too many arguments in function call");
    }
    Node *return_type = func_decl->type;
    if (type_params && inferred_args && return_type) {
      return_type =
          substitute_type(tc->arena, return_type, type_params, inferred_args);
    }
    n->type = return_type;
    return return_type;
  }
  case NODE_INDEX: {
    Node *left_type = typecheck_expr(tc, n->left);
    Node *index_type = typecheck_expr(tc, n->right);
    if (!type_is_array(left_type) && !type_is_slice(left_type) &&
        !type_is_pointer(left_type)) {
      type_error(n, "cannot index into non-array/slice/pointer type");
      return NULL;
    }
    if (!type_is_integer(index_type)) {
      type_error(n->right, "array index must be an integer");
    }
    Node *elem_type = get_element_type(left_type);
    if (!elem_type && type_is_pointer(left_type)) {
      elem_type = left_type->ptr;
    }
    n->type = elem_type;
    return elem_type;
  }
  case NODE_SLICE: {
    Node *left_type = typecheck_expr(tc, n->left);
    if (!type_is_array(left_type) && !type_is_slice(left_type)) {
      type_error(n, "cannot slice non-array/slice type");
      return NULL;
    }
    if (n->ptr) {
      Node *lo_type = typecheck_expr(tc, n->ptr);
      if (!type_is_integer(lo_type)) {
        type_error(n->ptr, "slice bound must be an integer");
      }
    }
    if (n->right) {
      Node *hi_type = typecheck_expr(tc, n->right);
      if (!type_is_integer(hi_type)) {
        type_error(n->right, "slice bound must be an integer");
      }
    }
    Node *elem_type = get_element_type(left_type);
    n->type = make_slice_type(tc->arena, elem_type);
    return n->type;
  }
  case NODE_FIELD: {
    Node *left_type = typecheck_expr(tc, n->left);
    while (type_is_pointer(left_type)) {
      left_type = left_type->ptr;
    }
    if (!left_type || (left_type->kind != NODE_TYPE_NAMED &&
                       left_type->kind != NODE_TYPE_GENERIC)) {
      if (left_type == type_range) {
        Str8 fname = tok_text(n->token);
        if (str8_eq(fname, S("start")) || str8_eq(fname, S("end"))) {
          n->type = type_i64;
          return type_i64;
        }
        type_error(n, "range has no field '%.*s'", (int)fname.len, fname.data);
        return NULL;
      }
      type_error(n, "cannot access field of non-struct type");
      return NULL;
    }
    Str8 field_name = tok_text(n->token);
    Node *field_type = NULL;
    Node *field = lookup_field(tc, left_type, field_name, &field_type);
    if (!field) {
      type_error(n, "type has no field '%.*s'", (int)field_name.len,
                 field_name.data);
      return NULL;
    }
    n->decl = field;
    n->type = field_type;
    return field_type;
  }
  case NODE_CAST: {
    Node *src_type = typecheck_expr(tc, n->left);
    Node *dst_type = n->type;
    bool valid = false;
    if (type_is_numeric(src_type) && type_is_numeric(dst_type))
      valid = true;
    if (type_is_pointer(src_type) && type_is_pointer(dst_type))
      valid = true;
    if (type_is_pointer(src_type) && type_is_integer(dst_type))
      valid = true;
    if (type_is_integer(src_type) && type_is_pointer(dst_type))
      valid = true;
    if (!valid) {
      type_error(n, "invalid cast from '%s' to '%s'", type_name(src_type),
                 type_name(dst_type));
    }
    return dst_type;
  }
  case NODE_SIZEOF:
  case NODE_ALIGNOF:
    n->type = type_u64;
    return type_u64;
  case NODE_BLOCK: {
    Node *last_stmt = NULL;
    for (Node *stmt = n->ptr; stmt; stmt = stmt->next) {
      typecheck_stmt(tc, stmt);
      last_stmt = stmt;
    }
    if (last_stmt) {
      switch (last_stmt->kind) {
      case NODE_RETURN:
      case NODE_BREAK:
      case NODE_CONTINUE:
      case NODE_VAR:
      case NODE_CONST:
        n->type = type_void;
        break;
      default:
        n->type = last_stmt->type ? last_stmt->type : type_void;
        break;
      }
    } else {
      n->type = type_void;
    }
    return n->type;
  }
  case NODE_IF: {
    Node *cond_type = typecheck_expr(tc, n->left);
    if (cond_type != type_bool) {
      type_error(n->left, "condition must be boolean");
    }
    Node *then_type = typecheck_expr(tc, n->right);
    if (n->ptr) {
      Node *else_type = typecheck_expr(tc, n->ptr);
      if (then_type && else_type) {
        if (types_equal(then_type, else_type)) {
          n->type = then_type;
        } else if (types_assignable(then_type, else_type)) {
          n->type = then_type;
        } else if (types_assignable(else_type, then_type)) {
          n->type = else_type;
        } else if (type_is_numeric(then_type) && type_is_numeric(else_type)) {
          n->type = common_numeric_type(then_type, else_type);
        } else {
          n->type = type_void;
        }
      } else {
        n->type = then_type ? then_type : else_type;
      }
    } else {
      n->type = type_void;
    }
    return n->type;
  }
  case NODE_FOR: {
    tc->loop_depth++;
    if (n->ptr) {
      Node *iter_var = n->ptr;
      Node *range_type = typecheck_expr(tc, iter_var->left);
      if (range_type == type_range) {
        iter_var->type = type_i64;
      } else if (type_is_slice(range_type)) {
        iter_var->type = get_element_type(range_type);
      } else if (type_is_array(range_type)) {
        iter_var->type = get_element_type(range_type);
      } else {
        type_error(iter_var->left, "cannot iterate over this type");
        iter_var->type = type_i64;
      }
    } else {
      Node *cond_type = typecheck_expr(tc, n->left);
      if (cond_type != type_bool) {
        type_error(n->left, "loop condition must be boolean");
      }
    }
    typecheck_expr(tc, n->right);
    tc->loop_depth--;
    n->type = type_void;
    return type_void;
  }
  case NODE_MATCH: {
    Node *match_type = typecheck_expr(tc, n->left);
    Node *result_type = NULL;
    int arm_num = 0;
    if (!n->ptr) {
      type_error(n, "match expression must have at least one arm");
      n->type = type_void;
      return type_void;
    }
    for (Node *arm = n->ptr; arm; arm = arm->next) {
      arm_num++;
      Node *pattern_type = typecheck_expr(tc, arm->left);
      Node *arm_result_type = typecheck_expr(tc, arm->right);
      if (pattern_type && match_type &&
          !types_equal(pattern_type, match_type)) {
        if (arm->left->kind != NODE_UNDERSCORE) {
          type_error(arm->left, "pattern type doesn't match");
        }
      }
      if (!result_type) {
        result_type = arm_result_type;
      } else if (arm_result_type) {
        if (types_equal(result_type, arm_result_type)) {
        } else if (types_assignable(result_type, arm_result_type)) {
        } else if (types_assignable(arm_result_type, result_type)) {
          result_type = arm_result_type;
        } else if (type_is_numeric(result_type) &&
                   type_is_numeric(arm_result_type)) {
          result_type = common_numeric_type(result_type, arm_result_type);
        } else {
          type_error(arm->right,
                     "match arm %d has incompatible type '%s', expected '%s'",
                     arm_num, type_name(arm_result_type),
                     type_name(result_type));
        }
      }
    }
    n->type = result_type;
    return result_type;
  }
  case NODE_STRUCT_LIT: {
    Node *struct_type = n->type;
    if (!struct_type) {
      type_error(n, "struct literal has no type");
      return NULL;
    }
    Node *decl = struct_type->decl;
    if (!decl || (decl->kind != NODE_STRUCT && decl->kind != NODE_UNION)) {
      type_error(n, "not a struct or union type");
      return NULL;
    }
    bool is_union = (decl->kind == NODE_UNION);
    int field_count = 0;
    for (Node *init = n->ptr; init; init = init->next) {
      field_count++;
      Str8 field_name = tok_text(init->token);
      for (Node *prev = n->ptr; prev != init; prev = prev->next) {
        if (str8_eq(tok_text(prev->token), field_name)) {
          type_error(init, "duplicate field '%.*s' in %s literal",
                     (int)field_name.len, field_name.data,
                     is_union ? "union" : "struct");
          break;
        }
      }
      Node *field_type = NULL;
      Node *field = lookup_field(tc, struct_type, field_name, &field_type);
      if (!field) {
        type_error(init, "%s has no field '%.*s'",
                   is_union ? "union" : "struct", (int)field_name.len,
                   field_name.data);
        continue;
      }
      Node *init_type = typecheck_expr(tc, init->left);
      if (!types_assignable(field_type, init_type)) {
        type_error(init, "cannot assign '%s' to field '%.*s' of type '%s'",
                   type_name(init_type), (int)field_name.len, field_name.data,
                   type_name(field_type));
      }
      init->decl = field;
    }
    if (is_union && field_count != 1) {
      type_error(n, "union literal must initialize exactly one field, got %d",
                 field_count);
    }
    if (!is_union) {
      for (Node *field = decl->ptr; field; field = field->next) {
        if (field->kind != NODE_STRUCT_FIELD)
          continue;
        Str8 field_name = tok_text(field->token);
        bool found = false;
        for (Node *init = n->ptr; init; init = init->next) {
          if (str8_eq(tok_text(init->token), field_name)) {
            found = true;
            break;
          }
        }
        if (!found) {
          type_error(n, "missing field '%.*s' in struct literal",
                     (int)field_name.len, field_name.data);
        }
      }
    }
    return struct_type;
  }
  case NODE_ARRAY_LIT: {
    Node *elem_type = NULL;
    int count = 0;
    for (Node *elem = n->ptr; elem; elem = elem->next) {
      Node *et = typecheck_expr(tc, elem);
      if (!elem_type) {
        elem_type = et;
      } else if (!types_equal(elem_type, et)) {
        type_error(elem, "array element type mismatch");
      }
      count++;
    }
    if (!elem_type)
      elem_type = type_i32;
    Node *arr_type = NEW(tc->arena, Node);
    arr_type->kind = NODE_TYPE_ARRAY;
    arr_type->ptr = elem_type;
    Node *size_node = NEW(tc->arena, Node);
    size_node->kind = NODE_INT;
    size_node->type = type_i32;
    size_node->token.pos = (u32)count;
    size_node->token.len = 0;
    arr_type->left = size_node;
    n->type = arr_type;
    return arr_type;
  }
  case NODE_TUPLE: {
    Node *first_type = NULL, *last_type = NULL;
    for (Node *elem = n->ptr; elem; elem = elem->next) {
      Node *elem_type = typecheck_expr(tc, elem);
      Node *type_node = NEW(tc->arena, Node);
      if (elem_type) {
        type_node->kind = elem_type->kind;
        type_node->token = elem_type->token;
        type_node->ptr = elem_type->ptr;
        type_node->left = elem_type->left;
        type_node->decl = elem_type->decl;
      } else {
        type_node->kind = NODE_INVALID;
      }
      type_node->next = NULL;
      if (last_type)
        last_type->next = type_node;
      else
        first_type = type_node;
      last_type = type_node;
    }
    Node *tuple_type = NEW(tc->arena, Node);
    tuple_type->kind = NODE_TYPE_TUPLE;
    tuple_type->ptr = first_type;
    n->type = tuple_type;
    return tuple_type;
  }
  default:
    return NULL;
  }
}

static bool definitely_returns(Node *n) {
  if (!n)
    return false;
  switch (n->kind) {
  case NODE_RETURN:
    return true;
  case NODE_BLOCK: {
    for (Node *stmt = n->ptr; stmt; stmt = stmt->next) {
      if (definitely_returns(stmt))
        return true;
    }
    return false;
  }
  case NODE_IF:
    if (n->ptr) {
      return definitely_returns(n->right) && definitely_returns(n->ptr);
    }
    return false;
  case NODE_MATCH: {
    bool has_wildcard = false;
    for (Node *arm = n->ptr; arm; arm = arm->next) {
      if (!definitely_returns(arm->right))
        return false;
      if (arm->left && arm->left->kind == NODE_UNDERSCORE)
        has_wildcard = true;
    }
    return has_wildcard;
  }
  case NODE_FOR:
    return false;
  default:
    return false;
  }
}

static void typecheck_stmt(TypeChecker *tc, Node *n) {
  if (!n)
    return;
  switch (n->kind) {
  case NODE_RETURN: {
    Node *expected = tc->current_func ? tc->current_func->type : NULL;
    if (n->left) {
      Node *actual = typecheck_expr(tc, n->left);
      if (expected && !types_assignable(expected, actual)) {
        type_error(n, "return type mismatch: expected '%s', got '%s'",
                   type_name(expected), type_name(actual));
      }
    } else {
      if (expected && expected->kind != NODE_TYPE_VOID) {
        type_error(n, "function requires a return value");
      }
    }
    break;
  }
  case NODE_BREAK:
  case NODE_CONTINUE:
    if (tc->loop_depth == 0) {
      type_error(n, "%s outside of loop",
                 n->kind == NODE_BREAK ? "break" : "continue");
    }
    break;
  case NODE_DEFER:
    typecheck_expr(tc, n->left);
    break;
  case NODE_VAR:
  case NODE_CONST:
    typecheck_decl(tc, n);
    break;
  default:
    typecheck_expr(tc, n);
    break;
  }
}

static void typecheck_decl(TypeChecker *tc, Node *n) {
  if (!n)
    return;
  switch (n->kind) {
  case NODE_VAR: {
    if (n->left) {
      Node *init_type = typecheck_expr(tc, n->left);
      if (n->left->kind == NODE_IF && !n->type) {
        Node *then_t = n->left->right ? n->left->right->type : NULL;
        Node *else_t = n->left->ptr ? n->left->ptr->type : NULL;
        if (then_t && else_t && !types_equal(then_t, else_t)) {
          type_error(
              n,
              "if expression branches have incompatible types: '%s' and '%s'",
              then_t ? type_name(then_t) : "?",
              else_t ? type_name(else_t) : "?");
        }
      }
      if (n->type) {
        if (!types_assignable(n->type, init_type)) {
          type_error(n, "cannot initialize '%s' with '%s'", type_name(n->type),
                     type_name(init_type));
        }
      } else {
        n->type = init_type;
      }
      if (n->ptr && init_type && init_type->kind == NODE_TYPE_TUPLE) {
        Node *name = n->ptr;
        Node *elem_type = init_type->ptr;
        int name_count = 0, type_count = 0;
        for (Node *nm = n->ptr; nm; nm = nm->next)
          name_count++;
        for (Node *et = init_type->ptr; et; et = et->next)
          type_count++;
        if (name_count != type_count) {
          type_error(n, "assignment count mismatch: %d variables but %d values",
                     name_count, type_count);
        }
        while (name && elem_type) {
          name->type = elem_type;
          name = name->next;
          elem_type = elem_type->next;
        }
      }
    } else if (!n->type) {
      type_error(n, "variable must have type annotation or initializer");
    }
    break;
  }
  case NODE_CONST: {
    Node *init_type = typecheck_expr(tc, n->left);
    if (!n->type) {
      n->type = init_type;
    } else if (!types_assignable(n->type, init_type)) {
      type_error(n, "constant type mismatch");
    }
    break;
  }
  case NODE_FUNC: {
    Node *saved_func = tc->current_func;
    tc->current_func = n;
    if (!n->type) {
      n->type = type_void;
    }
    typecheck_expr(tc, n->left);
    if (n->type && n->type->kind != NODE_TYPE_VOID) {
      if (!definitely_returns(n->left)) {
        type_error(n, "function '%.*s' must return a value on all paths",
                   (int)tok_text(n->token).len, tok_text(n->token).data);
      }
    }
    tc->current_func = saved_func;
    break;
  }
  case NODE_FOREIGN:
    break;
  case NODE_STRUCT:
  case NODE_UNION:
    break;
  case NODE_ENUM: {
    for (Node *variant = n->ptr; variant; variant = variant->next) {
      if (variant->left) {
        Node *val_type = typecheck_expr(tc, variant->left);
        if (!type_is_integer(val_type)) {
          type_error(variant, "enum value must be an integer");
        }
      }
    }
    break;
  }
  case NODE_IMPORT:
    break;
  default:
    break;
  }
}

static void typecheck(Arena *arena, Node **decls, usize count) {
  init_builtin_types(arena);
  TypeChecker tc = {0};
  tc.arena = arena;
  for (usize i = 0; i < count; i++) {
    typecheck_decl(&tc, decls[i]);
  }
}
typedef struct {
  Arena *arena;
  LLVMContextRef ctx;
  LLVMModuleRef mod;
  LLVMBuilderRef builder;
  LLVMValueRef current_func;
  LLVMBasicBlockRef break_target;
  LLVMBasicBlockRef continue_target;
  struct {
    Node *node;
    LLVMValueRef value;
  } values[4096];
  int value_count;
  struct {
    Node *node;
    LLVMTypeRef type;
  } types[256];
  int type_count;
} Codegen;

static void cg_set_value(Codegen *cg, Node *node, LLVMValueRef value) {
  if (cg->value_count < 4096) {
    cg->values[cg->value_count].node = node;
    cg->values[cg->value_count].value = value;
    cg->value_count++;
  }
}

static LLVMValueRef cg_get_value(Codegen *cg, Node *node) {
  for (int i = cg->value_count - 1; i >= 0; i--) {
    if (cg->values[i].node == node) {
      return cg->values[i].value;
    }
  }
  return NULL;
}

static void cg_set_type(Codegen *cg, Node *node, LLVMTypeRef type) {
  if (cg->type_count < 256) {
    cg->types[cg->type_count].node = node;
    cg->types[cg->type_count].type = type;
    cg->type_count++;
  }
}

static LLVMTypeRef cg_get_type(Codegen *cg, Node *node) {
  for (int i = 0; i < cg->type_count; i++) {
    if (cg->types[i].node == node) {
      return cg->types[i].type;
    }
  }
  return NULL;
}

static LLVMTypeRef cg_type(Codegen *cg, Node *type);

static LLVMTypeRef cg_type_subst(Codegen *cg, Node *type, Node *params,
                                 Node *args);

static LLVMValueRef cg_expr(Codegen *cg, Node *n);

static void cg_stmt(Codegen *cg, Node *n);

static void cg_decl(Codegen *cg, Node *n);

static LLVMTypeRef cg_type_subst(Codegen *cg, Node *type, Node *params,
                                 Node *args) {
  if (!type)
    return LLVMVoidTypeInContext(cg->ctx);
  if (!params || !args)
    return cg_type(cg, type);
  if (type->kind == NODE_TYPE_NAMED && type->decl &&
      type->decl->kind == NODE_TYPE_PARAM) {
    Node *p = params;
    Node *a = args;
    while (p && a) {
      if (p == type->decl) {
        return cg_type(cg, a);
      }
      p = p->next;
      a = a->next;
    }
    return LLVMVoidTypeInContext(cg->ctx);
  }
  switch (type->kind) {
  case NODE_TYPE_PTR: {
    return LLVMPointerTypeInContext(cg->ctx, 0);
  }
  case NODE_TYPE_SLICE: {
    LLVMTypeRef fields[2];
    fields[0] = LLVMPointerTypeInContext(cg->ctx, 0);
    fields[1] = LLVMInt64TypeInContext(cg->ctx);
    return LLVMStructTypeInContext(cg->ctx, fields, 2, false);
  }
  case NODE_TYPE_ARRAY: {
    LLVMTypeRef elem = cg_type_subst(cg, type->ptr, params, args);
    u64 size = 0;
    if (type->left && type->left->kind == NODE_INT) {
      if (type->left->token.len == 0) {
        size = type->left->token.pos;
      } else {
        Str8 text = tok_text(type->left->token);
        for (usize i = 0; i < text.len; i++) {
          if (text.data[i] >= '0' && text.data[i] <= '9') {
            size = size * 10 + (text.data[i] - '0');
          }
        }
      }
    }
    return LLVMArrayType2(elem, size);
  }
  case NODE_TYPE_TUPLE: {
    int count = 0;
    for (Node *t = type->ptr; t; t = t->next)
      count++;
    LLVMTypeRef *elem_types = NEW_ARRAY(cg->arena, LLVMTypeRef, count);
    int i = 0;
    for (Node *t = type->ptr; t; t = t->next) {
      elem_types[i++] = cg_type_subst(cg, t, params, args);
    }
    return LLVMStructTypeInContext(cg->ctx, elem_types, count, false);
  }
  default:
    return cg_type(cg, type);
  }
}

static LLVMTypeRef cg_type(Codegen *cg, Node *type) {
  if (!type)
    return LLVMVoidTypeInContext(cg->ctx);
  switch (type->kind) {
  case NODE_TYPE_VOID:
    return LLVMVoidTypeInContext(cg->ctx);
  case NODE_TYPE_BOOL:
    return LLVMInt1TypeInContext(cg->ctx);
  case NODE_TYPE_I8:
  case NODE_TYPE_U8:
    return LLVMInt8TypeInContext(cg->ctx);
  case NODE_TYPE_I16:
  case NODE_TYPE_U16:
    return LLVMInt16TypeInContext(cg->ctx);
  case NODE_TYPE_I32:
  case NODE_TYPE_U32:
    return LLVMInt32TypeInContext(cg->ctx);
  case NODE_TYPE_I64:
  case NODE_TYPE_U64:
    return LLVMInt64TypeInContext(cg->ctx);
  case NODE_TYPE_F32:
    return LLVMFloatTypeInContext(cg->ctx);
  case NODE_TYPE_F64:
    return LLVMDoubleTypeInContext(cg->ctx);
  case NODE_TYPE_PTR:
    return LLVMPointerTypeInContext(cg->ctx, 0);
  case NODE_TYPE_STRING:
    return LLVMPointerTypeInContext(cg->ctx, 0);
  case NODE_TYPE_ARRAY: {
    LLVMTypeRef elem = cg_type(cg, type->ptr);
    u64 size = 0;
    if (type->left && type->left->kind == NODE_INT) {
      if (type->left->token.len == 0) {
        size = type->left->token.pos;
      } else {
        Str8 text = tok_text(type->left->token);
        for (usize i = 0; i < text.len; i++) {
          if (text.data[i] >= '0' && text.data[i] <= '9') {
            size = size * 10 + (text.data[i] - '0');
          }
        }
      }
    }
    return LLVMArrayType2(elem, size);
  }
  case NODE_TYPE_SLICE: {
    LLVMTypeRef fields[2];
    fields[0] = LLVMPointerTypeInContext(cg->ctx, 0);
    fields[1] = LLVMInt64TypeInContext(cg->ctx);
    return LLVMStructTypeInContext(cg->ctx, fields, 2, false);
  }
  case NODE_TYPE_NAMED: {
    Node *decl = type->decl;
    if (!decl)
      return LLVMVoidTypeInContext(cg->ctx);
    if (decl->kind == NODE_STRUCT || decl->kind == NODE_UNION) {
      if (decl->right) {
        return LLVMPointerTypeInContext(cg->ctx, 0);
      }
      LLVMTypeRef t = cg_get_type(cg, decl);
      if (t)
        return t;
      Str8 name = tok_text(decl->token);
      char name_buf[256];
      snprintf(name_buf, sizeof(name_buf), "%.*s", (int)name.len, name.data);
      LLVMTypeRef struct_type = LLVMStructCreateNamed(cg->ctx, name_buf);
      cg_set_type(cg, decl, struct_type);
      int field_count = 0;
      for (Node *f = decl->ptr; f; f = f->next) {
        if (f->kind == NODE_STRUCT_FIELD)
          field_count++;
      }
      LLVMTypeRef *field_types = NEW_ARRAY(cg->arena, LLVMTypeRef, field_count);
      int i = 0;
      for (Node *f = decl->ptr; f; f = f->next) {
        if (f->kind == NODE_STRUCT_FIELD) {
          field_types[i++] = cg_type(cg, f->type);
        }
      }
      LLVMStructSetBody(struct_type, field_types, field_count, false);
      return struct_type;
    }
    if (decl->kind == NODE_ENUM) {
      return LLVMInt32TypeInContext(cg->ctx);
    }
    if (decl->kind == NODE_TYPE_PARAM) {
      return LLVMVoidTypeInContext(cg->ctx);
    }
    return LLVMVoidTypeInContext(cg->ctx);
  }
  case NODE_TYPE_GENERIC: {
    Node *decl = type->decl;
    if (!decl)
      return LLVMVoidTypeInContext(cg->ctx);
    if (decl->kind == NODE_STRUCT || decl->kind == NODE_UNION) {
      LLVMTypeRef t = cg_get_type(cg, type);
      if (t)
        return t;
      Str8 base_name = tok_text(decl->token);
      char name_buf[256];
      int pos = snprintf(name_buf, sizeof(name_buf), "%.*s", (int)base_name.len,
                         base_name.data);
      for (Node *arg = type->ptr; arg && pos < 250; arg = arg->next) {
        if (arg->token.len > 0) {
          Str8 arg_name = tok_text(arg->token);
          pos += snprintf(name_buf + pos, sizeof(name_buf) - pos, "_%.*s",
                          (int)arg_name.len, arg_name.data);
        }
      }
      LLVMTypeRef struct_type = LLVMStructCreateNamed(cg->ctx, name_buf);
      cg_set_type(cg, type, struct_type);
      int field_count = 0;
      for (Node *f = decl->ptr; f; f = f->next) {
        if (f->kind == NODE_STRUCT_FIELD)
          field_count++;
      }
      LLVMTypeRef *field_types = NEW_ARRAY(cg->arena, LLVMTypeRef, field_count);
      int i = 0;
      for (Node *f = decl->ptr; f; f = f->next) {
        if (f->kind == NODE_STRUCT_FIELD) {
          field_types[i++] = cg_type_subst(cg, f->type, decl->right, type->ptr);
        }
      }
      LLVMStructSetBody(struct_type, field_types, field_count, false);
      return struct_type;
    }
    if (decl->kind == NODE_ENUM) {
      return LLVMInt32TypeInContext(cg->ctx);
    }
    return LLVMVoidTypeInContext(cg->ctx);
  }
  case NODE_TYPE_TUPLE: {
    int count = 0;
    for (Node *t = type->ptr; t; t = t->next)
      count++;
    LLVMTypeRef *elem_types = NEW_ARRAY(cg->arena, LLVMTypeRef, count);
    int i = 0;
    for (Node *t = type->ptr; t; t = t->next) {
      elem_types[i++] = cg_type(cg, t);
    }
    return LLVMStructTypeInContext(cg->ctx, elem_types, count, false);
  }
  default:
    return LLVMVoidTypeInContext(cg->ctx);
  }
}

static i64 parse_int_literal(Str8 text) {
  i64 value = 0;
  usize i = 0;
  if (text.len > 2 && text.data[0] == '0') {
    if (text.data[1] == 'x' || text.data[1] == 'X') {
      for (i = 2; i < text.len; i++) {
        char c = text.data[i];
        if (c == '_')
          continue;
        if (c >= '0' && c <= '9')
          value = value * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f')
          value = value * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
          value = value * 16 + (c - 'A' + 10);
      }
      return value;
    }
    if (text.data[1] == 'b' || text.data[1] == 'B') {
      for (i = 2; i < text.len; i++) {
        char c = text.data[i];
        if (c == '_')
          continue;
        value = value * 2 + (c - '0');
      }
      return value;
    }
    if (text.data[1] == 'o' || text.data[1] == 'O') {
      for (i = 2; i < text.len; i++) {
        char c = text.data[i];
        if (c == '_')
          continue;
        value = value * 8 + (c - '0');
      }
      return value;
    }
  }
  for (i = 0; i < text.len; i++) {
    char c = text.data[i];
    if (c == '_')
      continue;
    if (c >= '0' && c <= '9')
      value = value * 10 + (c - '0');
  }
  return value;
}

static double parse_float_literal(Str8 text) {
  char buf[64];
  usize j = 0;
  for (usize i = 0; i < text.len && j < 63; i++) {
    if (text.data[i] != '_')
      buf[j++] = text.data[i];
  }
  buf[j] = '\0';
  return strtod(buf, NULL);
}

static LLVMValueRef cg_expr(Codegen *cg, Node *n);

static LLVMValueRef cg_lvalue(Codegen *cg, Node *n) {
  if (!n)
    return NULL;
  switch (n->kind) {
  case NODE_IDENT:
    if (n->decl) {
      return cg_get_value(cg, n->decl);
    }
    return NULL;
  case NODE_INDEX: {
    Node *base_type = n->left->type;
    LLVMValueRef index = cg_expr(cg, n->right);
    if (!index)
      return NULL;
    if (base_type->kind == NODE_TYPE_ARRAY) {
      LLVMValueRef array_ptr = cg_lvalue(cg, n->left);
      if (!array_ptr)
        return NULL;
      LLVMValueRef indices[2];
      indices[0] = LLVMConstInt(LLVMInt64TypeInContext(cg->ctx), 0, false);
      indices[1] = index;
      LLVMTypeRef arr_type = cg_type(cg, base_type);
      return LLVMBuildGEP2(cg->builder, arr_type, array_ptr, indices, 2, "");
    } else if (base_type->kind == NODE_TYPE_PTR) {
      LLVMValueRef base = cg_expr(cg, n->left);
      if (!base)
        return NULL;
      LLVMTypeRef elem_type = cg_type(cg, n->type);
      return LLVMBuildGEP2(cg->builder, elem_type, base, &index, 1, "");
    } else if (base_type->kind == NODE_TYPE_SLICE) {
      LLVMValueRef base = cg_expr(cg, n->left);
      if (!base)
        return NULL;
      LLVMTypeRef elem_type = cg_type(cg, n->type);
      LLVMValueRef data_ptr = LLVMBuildExtractValue(cg->builder, base, 0, "");
      return LLVMBuildGEP2(cg->builder, elem_type, data_ptr, &index, 1, "");
    }
    return NULL;
  }
  case NODE_FIELD: {
    Node *obj_type = n->left->type;
    if (!obj_type)
      return NULL;
    Node *decl_type = obj_type;
    if (decl_type->kind == NODE_TYPE_PTR)
      decl_type = decl_type->ptr;
    Node *struct_decl = NULL;
    if (decl_type->kind == NODE_TYPE_NAMED ||
        decl_type->kind == NODE_TYPE_GENERIC) {
      struct_decl = decl_type->decl;
    }
    if (!struct_decl)
      return NULL;
    Str8 field_name = tok_text(n->token);
    int field_idx = 0;
    for (Node *f = struct_decl->ptr; f; f = f->next) {
      if (f->kind == NODE_STRUCT_FIELD) {
        Str8 fname = tok_text(f->token);
        if (str8_eq(fname, field_name))
          break;
        field_idx++;
      }
    }
    LLVMValueRef struct_ptr = cg_lvalue(cg, n->left);
    if (!struct_ptr)
      return NULL;
    if (n->left->type && n->left->type->kind == NODE_TYPE_PTR) {
      LLVMTypeRef ptr_val_type = cg_type(cg, n->left->type);
      struct_ptr =
          LLVMBuildLoad2(cg->builder, ptr_val_type, struct_ptr, "ptr.val");
    }
    Node *elem_node = obj_type;
    if (elem_node->kind == NODE_TYPE_PTR)
      elem_node = elem_node->ptr;
    LLVMTypeRef struct_llvm_type = cg_type(cg, elem_node);
    LLVMValueRef indices[2];
    indices[0] = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, false);
    indices[1] =
        LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), field_idx, false);
    return LLVMBuildGEP2(cg->builder, struct_llvm_type, struct_ptr, indices, 2,
                         "");
  }
  case NODE_UNARY:
    if (n->op == TK_STAR) {
      return cg_expr(cg, n->right);
    }
    return NULL;
  default:
    return NULL;
  }
}

static LLVMValueRef cg_convert(Codegen *cg, LLVMValueRef val, Node *src_type,
                               Node *dst_type) {
  if (!val || !src_type || !dst_type)
    return val;
  if (types_equal(src_type, dst_type))
    return val;
  LLVMTypeRef llvm_dst = cg_type(cg, dst_type);
  if (type_is_integer(src_type) && type_is_integer(dst_type)) {
    int src_bits = type_integer_rank(src_type);
    int dst_bits = type_integer_rank(dst_type);
    if (dst_bits > src_bits) {
      return type_is_signed(src_type)
                 ? LLVMBuildSExt(cg->builder, val, llvm_dst, "")
                 : LLVMBuildZExt(cg->builder, val, llvm_dst, "");
    } else if (dst_bits < src_bits) {
      return LLVMBuildTrunc(cg->builder, val, llvm_dst, "");
    }
    return val;
  }
  if (type_is_float(src_type) && type_is_float(dst_type)) {
    if (dst_type->kind == NODE_TYPE_F64 && src_type->kind == NODE_TYPE_F32) {
      return LLVMBuildFPExt(cg->builder, val, llvm_dst, "");
    } else if (dst_type->kind == NODE_TYPE_F32 &&
               src_type->kind == NODE_TYPE_F64) {
      return LLVMBuildFPTrunc(cg->builder, val, llvm_dst, "");
    }
    return val;
  }
  if (type_is_integer(src_type) && type_is_float(dst_type)) {
    return type_is_signed(src_type)
               ? LLVMBuildSIToFP(cg->builder, val, llvm_dst, "")
               : LLVMBuildUIToFP(cg->builder, val, llvm_dst, "");
  }
  if (type_is_float(src_type) && type_is_integer(dst_type)) {
    return type_is_signed(dst_type)
               ? LLVMBuildFPToSI(cg->builder, val, llvm_dst, "")
               : LLVMBuildFPToUI(cg->builder, val, llvm_dst, "");
  }
  return val;
}

static LLVMValueRef cg_expr(Codegen *cg, Node *n) {
  if (!n)
    return NULL;
  switch (n->kind) {
  case NODE_INT: {
    LLVMTypeRef t = cg_type(cg, n->type);
    i64 value = parse_int_literal(tok_text(n->token));
    return LLVMConstInt(t, (unsigned long long)value, true);
  }
  case NODE_FLOAT: {
    LLVMTypeRef t = cg_type(cg, n->type);
    double value = parse_float_literal(tok_text(n->token));
    return LLVMConstReal(t, value);
  }
  case NODE_BOOL: {
    Str8 text = tok_text(n->token);
    int value = str8_eq(text, S("true")) ? 1 : 0;
    return LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), value, false);
  }
  case NODE_CHAR: {
    Str8 text = tok_text(n->token);
    u8 value = 0;
    if (text.len >= 3) {
      if (text.data[1] == '\\' && text.len >= 4) {
        switch (text.data[2]) {
        case 'n':
          value = '\n';
          break;
        case 't':
          value = '\t';
          break;
        case 'r':
          value = '\r';
          break;
        case '0':
          value = '\0';
          break;
        case '\\':
          value = '\\';
          break;
        case '\'':
          value = '\'';
          break;
        default:
          value = text.data[2];
          break;
        }
      } else {
        value = text.data[1];
      }
    }
    return LLVMConstInt(LLVMInt8TypeInContext(cg->ctx), value, false);
  }
  case NODE_STRING: {
    Str8 text = tok_text(n->token);
    if (text.len >= 2) {
      text.data++;
      text.len -= 2;
    }
    char buf[4096];
    usize j = 0;
    for (usize i = 0; i < text.len && j < sizeof(buf) - 1; i++) {
      if (text.data[i] == '\\' && i + 1 < text.len) {
        switch (text.data[i + 1]) {
        case 'n':
          buf[j++] = '\n';
          i++;
          break;
        case 't':
          buf[j++] = '\t';
          i++;
          break;
        case 'r':
          buf[j++] = '\r';
          i++;
          break;
        case '0':
          buf[j++] = '\0';
          i++;
          break;
        case '\\':
          buf[j++] = '\\';
          i++;
          break;
        case '"':
          buf[j++] = '"';
          i++;
          break;
        default:
          buf[j++] = text.data[i];
          break;
        }
      } else {
        buf[j++] = text.data[i];
      }
    }
    buf[j] = '\0';
    return LLVMBuildGlobalStringPtr(cg->builder, buf, "str");
  }
  case NODE_NULL:
    return LLVMConstNull(LLVMPointerTypeInContext(cg->ctx, 0));
  case NODE_IDENT: {
    if (!n->decl)
      return NULL;
    LLVMValueRef ptr = cg_get_value(cg, n->decl);
    if (!ptr)
      return NULL;
    if (n->decl->kind == NODE_PARAM) {
      return ptr;
    }
    if (n->decl->kind == NODE_CONST) {
      return ptr;
    }
    if (n->decl->kind == NODE_FUNC || n->decl->kind == NODE_FOREIGN) {
      return ptr;
    }
    Node *actual_type = n->decl->type;
    LLVMTypeRef load_type = cg_type(cg, actual_type);
    return LLVMBuildLoad2(cg->builder, load_type, ptr, "");
  }
  case NODE_BINARY: {
    if (n->op == TK_ASSIGN) {
      LLVMValueRef rhs = cg_expr(cg, n->right);
      LLVMValueRef ptr = cg_lvalue(cg, n->left);
      if (ptr && rhs) {
        rhs = cg_convert(cg, rhs, n->right->type, n->left->type);
        LLVMBuildStore(cg->builder, rhs, ptr);
      }
      return rhs;
    }
    LLVMValueRef lhs = cg_expr(cg, n->left);
    LLVMValueRef rhs = cg_expr(cg, n->right);
    if (!lhs || !rhs)
      return NULL;
    Node *result_type = n->type;
    bool is_comparison = (n->op == TK_EQ || n->op == TK_NE || n->op == TK_LT ||
                          n->op == TK_LE || n->op == TK_GT || n->op == TK_GE);
    Node *operand_type = is_comparison ? n->left->type : result_type;
    bool is_float = type_is_float(operand_type);
    bool is_signed = type_is_signed(operand_type);
    if (is_comparison) {
      rhs = cg_convert(cg, rhs, n->right->type, n->left->type);
    } else if (type_is_numeric(result_type)) {
      lhs = cg_convert(cg, lhs, n->left->type, result_type);
      rhs = cg_convert(cg, rhs, n->right->type, result_type);
    }
    switch (n->op) {
    case TK_PLUS:
      return is_float ? LLVMBuildFAdd(cg->builder, lhs, rhs, "")
                      : LLVMBuildAdd(cg->builder, lhs, rhs, "");
    case TK_MINUS:
      return is_float ? LLVMBuildFSub(cg->builder, lhs, rhs, "")
                      : LLVMBuildSub(cg->builder, lhs, rhs, "");
    case TK_STAR:
      return is_float ? LLVMBuildFMul(cg->builder, lhs, rhs, "")
                      : LLVMBuildMul(cg->builder, lhs, rhs, "");
    case TK_SLASH:
      if (is_float)
        return LLVMBuildFDiv(cg->builder, lhs, rhs, "");
      return is_signed ? LLVMBuildSDiv(cg->builder, lhs, rhs, "")
                       : LLVMBuildUDiv(cg->builder, lhs, rhs, "");
    case TK_PERCENT:
      if (is_float)
        return LLVMBuildFRem(cg->builder, lhs, rhs, "");
      return is_signed ? LLVMBuildSRem(cg->builder, lhs, rhs, "")
                       : LLVMBuildURem(cg->builder, lhs, rhs, "");
    case TK_AMP:
      return LLVMBuildAnd(cg->builder, lhs, rhs, "");
    case TK_PIPE:
      return LLVMBuildOr(cg->builder, lhs, rhs, "");
    case TK_CARET:
      return LLVMBuildXor(cg->builder, lhs, rhs, "");
    case TK_SHL:
      return LLVMBuildShl(cg->builder, lhs, rhs, "");
    case TK_SHR:
      return is_signed ? LLVMBuildAShr(cg->builder, lhs, rhs, "")
                       : LLVMBuildLShr(cg->builder, lhs, rhs, "");
    case TK_EQ:
      return is_float ? LLVMBuildFCmp(cg->builder, LLVMRealOEQ, lhs, rhs, "")
                      : LLVMBuildICmp(cg->builder, LLVMIntEQ, lhs, rhs, "");
    case TK_NE:
      return is_float ? LLVMBuildFCmp(cg->builder, LLVMRealONE, lhs, rhs, "")
                      : LLVMBuildICmp(cg->builder, LLVMIntNE, lhs, rhs, "");
    case TK_LT:
      if (is_float)
        return LLVMBuildFCmp(cg->builder, LLVMRealOLT, lhs, rhs, "");
      return is_signed ? LLVMBuildICmp(cg->builder, LLVMIntSLT, lhs, rhs, "")
                       : LLVMBuildICmp(cg->builder, LLVMIntULT, lhs, rhs, "");
    case TK_LE:
      if (is_float)
        return LLVMBuildFCmp(cg->builder, LLVMRealOLE, lhs, rhs, "");
      return is_signed ? LLVMBuildICmp(cg->builder, LLVMIntSLE, lhs, rhs, "")
                       : LLVMBuildICmp(cg->builder, LLVMIntULE, lhs, rhs, "");
    case TK_GT:
      if (is_float)
        return LLVMBuildFCmp(cg->builder, LLVMRealOGT, lhs, rhs, "");
      return is_signed ? LLVMBuildICmp(cg->builder, LLVMIntSGT, lhs, rhs, "")
                       : LLVMBuildICmp(cg->builder, LLVMIntUGT, lhs, rhs, "");
    case TK_GE:
      if (is_float)
        return LLVMBuildFCmp(cg->builder, LLVMRealOGE, lhs, rhs, "");
      return is_signed ? LLVMBuildICmp(cg->builder, LLVMIntSGE, lhs, rhs, "")
                       : LLVMBuildICmp(cg->builder, LLVMIntUGE, lhs, rhs, "");
    case TK_AND:
      return LLVMBuildAnd(cg->builder, lhs, rhs, "");
    case TK_OR:
      return LLVMBuildOr(cg->builder, lhs, rhs, "");
    default:
      (void)result_type;
      return NULL;
    }
  }
  case NODE_UNARY: {
    LLVMValueRef operand = cg_expr(cg, n->right);
    if (!operand)
      return NULL;
    switch (n->op) {
    case TK_MINUS:
      if (type_is_float(n->right->type)) {
        return LLVMBuildFNeg(cg->builder, operand, "");
      }
      return LLVMBuildNeg(cg->builder, operand, "");
    case TK_BANG:
      return LLVMBuildXor(
          cg->builder, operand,
          LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 1, false), "");
    case TK_TILDE:
      return LLVMBuildNot(cg->builder, operand, "");
    case TK_CARET:
      return cg_lvalue(cg, n->right);
    case TK_STAR: {
      LLVMTypeRef elem_type = cg_type(cg, n->type);
      return LLVMBuildLoad2(cg->builder, elem_type, operand, "");
    }
    default:
      return NULL;
    }
  }
  case NODE_CALL: {
    LLVMValueRef callee = cg_expr(cg, n->left);
    if (!callee)
      return NULL;
    int arg_count = 0;
    for (Node *arg = n->ptr; arg; arg = arg->next)
      arg_count++;
    LLVMValueRef *args = NEW_ARRAY(cg->arena, LLVMValueRef, arg_count);
    int i = 0;
    for (Node *arg = n->ptr; arg; arg = arg->next) {
      args[i++] = cg_expr(cg, arg);
    }
    Node *func_decl = n->left->decl;
    LLVMTypeRef ret_type = cg_type(cg, n->type);
    int param_count = 0;
    bool is_vararg = false;
    for (Node *p = func_decl->ptr; p; p = p->next) {
      if (p->kind == NODE_VARARGS) {
        is_vararg = true;
        break;
      }
      param_count++;
    }
    LLVMTypeRef *param_types = NEW_ARRAY(cg->arena, LLVMTypeRef, param_count);
    i = 0;
    for (Node *p = func_decl->ptr; p && p->kind == NODE_PARAM; p = p->next) {
      param_types[i++] = cg_type(cg, p->type);
    }
    LLVMTypeRef func_type =
        LLVMFunctionType(ret_type, param_types, param_count, is_vararg);
    return LLVMBuildCall2(cg->builder, func_type, callee, args, arg_count, "");
  }
  case NODE_BLOCK: {
    LLVMValueRef last_value = NULL;
    for (Node *stmt = n->ptr; stmt; stmt = stmt->next) {
      if (!stmt->next) {
        switch (stmt->kind) {
        case NODE_RETURN:
        case NODE_BREAK:
        case NODE_CONTINUE:
        case NODE_VAR:
        case NODE_CONST:
          cg_stmt(cg, stmt);
          break;
        default:
          last_value = cg_expr(cg, stmt);
          break;
        }
      } else {
        cg_stmt(cg, stmt);
      }
    }
    return last_value;
  }
  case NODE_IF: {
    LLVMValueRef cond = cg_expr(cg, n->left);
    if (!cond)
      return NULL;
    LLVMBasicBlockRef then_bb =
        LLVMAppendBasicBlockInContext(cg->ctx, cg->current_func, "then");
    LLVMBasicBlockRef else_bb =
        n->ptr
            ? LLVMAppendBasicBlockInContext(cg->ctx, cg->current_func, "else")
            : NULL;
    LLVMBasicBlockRef merge_bb =
        LLVMAppendBasicBlockInContext(cg->ctx, cg->current_func, "ifcont");
    LLVMBuildCondBr(cg->builder, cond, then_bb, else_bb ? else_bb : merge_bb);
    LLVMPositionBuilderAtEnd(cg->builder, then_bb);
    LLVMValueRef then_val = cg_expr(cg, n->right);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      LLVMBuildBr(cg->builder, merge_bb);
    }
    LLVMBasicBlockRef then_end = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef else_val = NULL;
    LLVMBasicBlockRef else_end = NULL;
    if (n->ptr) {
      LLVMPositionBuilderAtEnd(cg->builder, else_bb);
      else_val = cg_expr(cg, n->ptr);
      if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
        LLVMBuildBr(cg->builder, merge_bb);
      }
      else_end = LLVMGetInsertBlock(cg->builder);
    }
    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
    if (then_val && else_val && n->type && n->type->kind != NODE_TYPE_VOID) {
      then_val = cg_convert(cg, then_val, n->right->type, n->type);
      else_val = cg_convert(cg, else_val, n->ptr->type, n->type);
      LLVMTypeRef phi_type = cg_type(cg, n->type);
      LLVMValueRef phi = LLVMBuildPhi(cg->builder, phi_type, "iftmp");
      LLVMAddIncoming(phi, &then_val, &then_end, 1);
      LLVMAddIncoming(phi, &else_val, &else_end, 1);
      return phi;
    }
    return NULL;
  }
  case NODE_CAST: {
    LLVMValueRef val = cg_expr(cg, n->left);
    if (!val)
      return NULL;
    Node *src_type = n->left->type;
    Node *dst_type = n->type;
    LLVMTypeRef llvm_dst = cg_type(cg, dst_type);
    if (type_is_integer(src_type) && type_is_integer(dst_type)) {
      int src_bits = type_integer_rank(src_type);
      int dst_bits = type_integer_rank(dst_type);
      if (dst_bits > src_bits) {
        return type_is_signed(src_type)
                   ? LLVMBuildSExt(cg->builder, val, llvm_dst, "")
                   : LLVMBuildZExt(cg->builder, val, llvm_dst, "");
      } else if (dst_bits < src_bits) {
        return LLVMBuildTrunc(cg->builder, val, llvm_dst, "");
      }
      return val;
    }
    if (type_is_float(src_type) && type_is_float(dst_type)) {
      if (dst_type->kind == NODE_TYPE_F64 && src_type->kind == NODE_TYPE_F32) {
        return LLVMBuildFPExt(cg->builder, val, llvm_dst, "");
      } else if (dst_type->kind == NODE_TYPE_F32 &&
                 src_type->kind == NODE_TYPE_F64) {
        return LLVMBuildFPTrunc(cg->builder, val, llvm_dst, "");
      }
      return val;
    }
    if (type_is_integer(src_type) && type_is_float(dst_type)) {
      return type_is_signed(src_type)
                 ? LLVMBuildSIToFP(cg->builder, val, llvm_dst, "")
                 : LLVMBuildUIToFP(cg->builder, val, llvm_dst, "");
    }
    if (type_is_float(src_type) && type_is_integer(dst_type)) {
      return type_is_signed(dst_type)
                 ? LLVMBuildFPToSI(cg->builder, val, llvm_dst, "")
                 : LLVMBuildFPToUI(cg->builder, val, llvm_dst, "");
    }
    if (type_is_pointer(src_type) && type_is_pointer(dst_type)) {
      return val;
    }
    if (type_is_integer(src_type) && type_is_pointer(dst_type)) {
      return LLVMBuildIntToPtr(cg->builder, val, llvm_dst, "");
    }
    if (type_is_pointer(src_type) && type_is_integer(dst_type)) {
      return LLVMBuildPtrToInt(cg->builder, val, llvm_dst, "");
    }
    return val;
  }
  case NODE_SIZEOF: {
    LLVMTypeRef t = cg_type(cg, n->left);
    LLVMValueRef size = LLVMSizeOf(t);
    return LLVMBuildIntCast2(cg->builder, size, LLVMInt64TypeInContext(cg->ctx),
                             false, "");
  }
  case NODE_ALIGNOF: {
    LLVMTypeRef t = cg_type(cg, n->left);
    LLVMValueRef align = LLVMAlignOf(t);
    return LLVMBuildIntCast2(cg->builder, align,
                             LLVMInt64TypeInContext(cg->ctx), false, "");
  }
  case NODE_INDEX: {
    LLVMValueRef base = cg_expr(cg, n->left);
    LLVMValueRef index = cg_expr(cg, n->right);
    if (!base || !index)
      return NULL;
    Node *base_type = n->left->type;
    LLVMTypeRef elem_type = cg_type(cg, n->type);
    if (base_type->kind == NODE_TYPE_ARRAY) {
      LLVMValueRef indices[2];
      indices[0] = LLVMConstInt(LLVMInt64TypeInContext(cg->ctx), 0, false);
      indices[1] = index;
      LLVMValueRef array_ptr = NULL;
      if (n->left->kind == NODE_IDENT && n->left->decl) {
        array_ptr = cg_get_value(cg, n->left->decl);
      } else {
        LLVMTypeRef arr_type = cg_type(cg, base_type);
        LLVMValueRef tmp = LLVMBuildAlloca(cg->builder, arr_type, "arr.tmp");
        LLVMBuildStore(cg->builder, base, tmp);
        array_ptr = tmp;
      }
      LLVMTypeRef arr_type = cg_type(cg, base_type);
      LLVMValueRef elem_ptr =
          LLVMBuildGEP2(cg->builder, arr_type, array_ptr, indices, 2, "");
      return LLVMBuildLoad2(cg->builder, elem_type, elem_ptr, "");
    } else if (base_type->kind == NODE_TYPE_PTR) {
      LLVMValueRef elem_ptr =
          LLVMBuildGEP2(cg->builder, elem_type, base, &index, 1, "");
      return LLVMBuildLoad2(cg->builder, elem_type, elem_ptr, "");
    } else if (base_type->kind == NODE_TYPE_SLICE) {
      LLVMTypeRef slice_type = cg_type(cg, base_type);
      LLVMValueRef data_ptr =
          LLVMBuildExtractValue(cg->builder, base, 0, "slice.data");
      LLVMValueRef elem_ptr =
          LLVMBuildGEP2(cg->builder, elem_type, data_ptr, &index, 1, "");
      (void)slice_type;
      return LLVMBuildLoad2(cg->builder, elem_type, elem_ptr, "");
    }
    return NULL;
  }
  case NODE_FIELD: {
    Node *obj_type = n->left->type;
    if (!obj_type)
      return NULL;
    Node *struct_decl = NULL;
    Node *search_type = obj_type;
    if (search_type->kind == NODE_TYPE_PTR)
      search_type = search_type->ptr;
    if (search_type->kind == NODE_TYPE_NAMED ||
        search_type->kind == NODE_TYPE_GENERIC) {
      struct_decl = search_type->decl;
    }
    if (!struct_decl ||
        (struct_decl->kind != NODE_STRUCT && struct_decl->kind != NODE_UNION)) {
      return NULL;
    }
    Str8 field_name = tok_text(n->token);
    int field_idx = 0;
    for (Node *f = struct_decl->ptr; f; f = f->next) {
      if (f->kind == NODE_STRUCT_FIELD) {
        Str8 fname = tok_text(f->token);
        if (str8_eq(fname, field_name))
          break;
        field_idx++;
      }
    }
    LLVMValueRef struct_ptr = NULL;
    if (n->left->kind == NODE_IDENT && n->left->decl) {
      struct_ptr = cg_get_value(cg, n->left->decl);
      if (n->left->type && n->left->type->kind == NODE_TYPE_PTR) {
        LLVMTypeRef ptr_val_type = cg_type(cg, n->left->type);
        struct_ptr =
            LLVMBuildLoad2(cg->builder, ptr_val_type, struct_ptr, "ptr.val");
      }
    } else if (n->left->kind == NODE_UNARY && n->left->op == TK_STAR) {
      struct_ptr = cg_expr(cg, n->left->right);
    } else {
      LLVMValueRef obj = cg_expr(cg, n->left);
      LLVMTypeRef obj_llvm_type = cg_type(cg, obj_type);
      LLVMValueRef tmp =
          LLVMBuildAlloca(cg->builder, obj_llvm_type, "struct.tmp");
      LLVMBuildStore(cg->builder, obj, tmp);
      struct_ptr = tmp;
    }
    Node *elem_node = obj_type;
    if (elem_node->kind == NODE_TYPE_PTR)
      elem_node = elem_node->ptr;
    LLVMTypeRef struct_llvm_type = cg_type(cg, elem_node);
    LLVMValueRef indices[2];
    indices[0] = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, false);
    indices[1] =
        LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), field_idx, false);
    LLVMValueRef field_ptr = LLVMBuildGEP2(cg->builder, struct_llvm_type,
                                           struct_ptr, indices, 2, "");
    LLVMTypeRef field_type = cg_type(cg, n->type);
    return LLVMBuildLoad2(cg->builder, field_type, field_ptr, "");
  }
  case NODE_ARRAY_LIT: {
    int count = 0;
    for (Node *elem = n->ptr; elem; elem = elem->next)
      count++;
    LLVMTypeRef elem_type = cg_type(cg, n->type->ptr);
    LLVMTypeRef arr_type = LLVMArrayType2(elem_type, count);
    bool all_const = true;
    for (Node *elem = n->ptr; elem; elem = elem->next) {
      if (elem->kind != NODE_INT && elem->kind != NODE_FLOAT &&
          elem->kind != NODE_BOOL && elem->kind != NODE_CHAR) {
        all_const = false;
        break;
      }
    }
    if (all_const) {
      LLVMValueRef *values = NEW_ARRAY(cg->arena, LLVMValueRef, count);
      int i = 0;
      for (Node *elem = n->ptr; elem; elem = elem->next) {
        values[i++] = cg_expr(cg, elem);
      }
      return LLVMConstArray2(elem_type, values, count);
    } else {
      LLVMValueRef arr = LLVMBuildAlloca(cg->builder, arr_type, "arr.lit");
      int i = 0;
      for (Node *elem = n->ptr; elem; elem = elem->next) {
        LLVMValueRef indices[2];
        indices[0] = LLVMConstInt(LLVMInt64TypeInContext(cg->ctx), 0, false);
        indices[1] = LLVMConstInt(LLVMInt64TypeInContext(cg->ctx), i, false);
        LLVMValueRef elem_ptr =
            LLVMBuildGEP2(cg->builder, arr_type, arr, indices, 2, "");
        LLVMValueRef val = cg_expr(cg, elem);
        LLVMBuildStore(cg->builder, val, elem_ptr);
        i++;
      }
      return LLVMBuildLoad2(cg->builder, arr_type, arr, "");
    }
  }
  case NODE_STRUCT_LIT: {
    Node *struct_type = n->type;
    if (!struct_type)
      return NULL;
    Node *struct_decl = NULL;
    if (struct_type->kind == NODE_TYPE_NAMED ||
        struct_type->kind == NODE_TYPE_GENERIC) {
      struct_decl = struct_type->decl;
    }
    if (!struct_decl)
      return NULL;
    LLVMTypeRef llvm_struct_type = cg_type(cg, struct_type);
    LLVMValueRef struct_ptr =
        LLVMBuildAlloca(cg->builder, llvm_struct_type, "struct.lit");
    LLVMValueRef zero = LLVMConstNull(llvm_struct_type);
    LLVMBuildStore(cg->builder, zero, struct_ptr);
    for (Node *init = n->ptr; init; init = init->next) {
      if (init->kind != NODE_FIELD_INIT)
        continue;
      Str8 field_name = tok_text(init->token);
      int field_idx = 0;
      Node *field_type = NULL;
      for (Node *f = struct_decl->ptr; f; f = f->next) {
        if (f->kind == NODE_STRUCT_FIELD) {
          Str8 fname = tok_text(f->token);
          if (str8_eq(fname, field_name)) {
            field_type = f->type;
            break;
          }
          field_idx++;
        }
      }
      LLVMValueRef indices[2];
      indices[0] = LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, false);
      indices[1] =
          LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), field_idx, false);
      LLVMValueRef field_ptr = LLVMBuildGEP2(cg->builder, llvm_struct_type,
                                             struct_ptr, indices, 2, "");
      LLVMValueRef val = cg_expr(cg, init->left);
      if (val && field_type) {
        val = cg_convert(cg, val, init->left->type, field_type);
        LLVMBuildStore(cg->builder, val, field_ptr);
      }
    }
    return LLVMBuildLoad2(cg->builder, llvm_struct_type, struct_ptr, "");
  }
  case NODE_FOR: {
    Node *body = n->right;
    if (n->ptr && n->ptr->kind == NODE_VAR) {
      Node *range = n->ptr->left;
      if (range && range->kind == NODE_RANGE) {
        LLVMTypeRef iter_type = cg_type(cg, n->ptr->type);
        LLVMValueRef start = cg_expr(cg, range->left);
        LLVMValueRef end = cg_expr(cg, range->right);
        if (!start || !end)
          return NULL;
        start = cg_convert(cg, start, range->left->type, n->ptr->type);
        end = cg_convert(cg, end, range->right->type, n->ptr->type);
        LLVMValueRef iter_ptr = LLVMBuildAlloca(cg->builder, iter_type, "i");
        LLVMBuildStore(cg->builder, start, iter_ptr);
        cg_set_value(cg, n->ptr, iter_ptr);
        LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(
            cg->ctx, cg->current_func, "for.cond");
        LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(
            cg->ctx, cg->current_func, "for.body");
        LLVMBasicBlockRef incr_bb = LLVMAppendBasicBlockInContext(
            cg->ctx, cg->current_func, "for.incr");
        LLVMBasicBlockRef end_bb =
            LLVMAppendBasicBlockInContext(cg->ctx, cg->current_func, "for.end");
        LLVMBasicBlockRef saved_break = cg->break_target;
        LLVMBasicBlockRef saved_continue = cg->continue_target;
        cg->break_target = end_bb;
        cg->continue_target = incr_bb;
        LLVMBuildBr(cg->builder, cond_bb);
        LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
        LLVMValueRef iter_val =
            LLVMBuildLoad2(cg->builder, iter_type, iter_ptr, "i.val");
        LLVMValueRef cond;
        if (range->op == TK_DOT2EQ) {
          cond = LLVMBuildICmp(cg->builder, LLVMIntSLE, iter_val, end, "");
        } else {
          cond = LLVMBuildICmp(cg->builder, LLVMIntSLT, iter_val, end, "");
        }
        LLVMBuildCondBr(cg->builder, cond, body_bb, end_bb);
        LLVMPositionBuilderAtEnd(cg->builder, body_bb);
        cg_expr(cg, body);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
          LLVMBuildBr(cg->builder, incr_bb);
        }
        LLVMPositionBuilderAtEnd(cg->builder, incr_bb);
        LLVMValueRef curr =
            LLVMBuildLoad2(cg->builder, iter_type, iter_ptr, "");
        LLVMValueRef next = LLVMBuildAdd(
            cg->builder, curr, LLVMConstInt(iter_type, 1, false), "i.next");
        LLVMBuildStore(cg->builder, next, iter_ptr);
        LLVMBuildBr(cg->builder, cond_bb);
        LLVMPositionBuilderAtEnd(cg->builder, end_bb);
        cg->break_target = saved_break;
        cg->continue_target = saved_continue;
        return NULL;
      }
    }
    if (n->left) {
      LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(
          cg->ctx, cg->current_func, "while.cond");
      LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(
          cg->ctx, cg->current_func, "while.body");
      LLVMBasicBlockRef end_bb =
          LLVMAppendBasicBlockInContext(cg->ctx, cg->current_func, "while.end");
      LLVMBasicBlockRef saved_break = cg->break_target;
      LLVMBasicBlockRef saved_continue = cg->continue_target;
      cg->break_target = end_bb;
      cg->continue_target = cond_bb;
      LLVMBuildBr(cg->builder, cond_bb);
      LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
      LLVMValueRef cond = cg_expr(cg, n->left);
      LLVMBuildCondBr(cg->builder, cond, body_bb, end_bb);
      LLVMPositionBuilderAtEnd(cg->builder, body_bb);
      cg_expr(cg, body);
      if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
        LLVMBuildBr(cg->builder, cond_bb);
      }
      LLVMPositionBuilderAtEnd(cg->builder, end_bb);
      cg->break_target = saved_break;
      cg->continue_target = saved_continue;
      return NULL;
    }
    return NULL;
  }
  case NODE_MATCH: {
    LLVMValueRef scrutinee = cg_expr(cg, n->left);
    if (!scrutinee)
      return NULL;
    int arm_count = 0;
    for (Node *arm = n->ptr; arm; arm = arm->next)
      arm_count++;
    bool has_value = n->type && n->type->kind != NODE_TYPE_VOID;
    LLVMBasicBlockRef merge_bb =
        LLVMAppendBasicBlockInContext(cg->ctx, cg->current_func, "match.end");
    LLVMValueRef *incoming_vals = NULL;
    LLVMBasicBlockRef *incoming_bbs = NULL;
    int incoming_count = 0;
    if (has_value) {
      incoming_vals = NEW_ARRAY(cg->arena, LLVMValueRef, arm_count);
      incoming_bbs = NEW_ARRAY(cg->arena, LLVMBasicBlockRef, arm_count);
    }
    Node *arm = n->ptr;
    LLVMBasicBlockRef default_bb = NULL;
    while (arm) {
      Node *pattern = arm->left;
      Node *result = arm->right;
      bool is_wildcard = (pattern->kind == NODE_UNDERSCORE);
      if (is_wildcard) {
        default_bb = LLVMAppendBasicBlockInContext(cg->ctx, cg->current_func,
                                                   "match.default");
        LLVMBuildBr(cg->builder, default_bb);
        LLVMPositionBuilderAtEnd(cg->builder, default_bb);
        LLVMValueRef val = cg_expr(cg, result);
        if (has_value && val) {
          incoming_vals[incoming_count] = val;
          incoming_bbs[incoming_count] = LLVMGetInsertBlock(cg->builder);
          incoming_count++;
        }
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
          LLVMBuildBr(cg->builder, merge_bb);
        }
        break;
      } else {
        LLVMValueRef pattern_val = cg_expr(cg, pattern);
        LLVMValueRef cond =
            LLVMBuildICmp(cg->builder, LLVMIntEQ, scrutinee, pattern_val, "");
        LLVMBasicBlockRef then_bb = LLVMAppendBasicBlockInContext(
            cg->ctx, cg->current_func, "match.arm");
        LLVMBasicBlockRef else_bb = LLVMAppendBasicBlockInContext(
            cg->ctx, cg->current_func, "match.next");
        LLVMBuildCondBr(cg->builder, cond, then_bb, else_bb);
        LLVMPositionBuilderAtEnd(cg->builder, then_bb);
        LLVMValueRef val = cg_expr(cg, result);
        if (has_value && val) {
          incoming_vals[incoming_count] = val;
          incoming_bbs[incoming_count] = LLVMGetInsertBlock(cg->builder);
          incoming_count++;
        }
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
          LLVMBuildBr(cg->builder, merge_bb);
        }
        LLVMPositionBuilderAtEnd(cg->builder, else_bb);
      }
      arm = arm->next;
    }
    if (!default_bb &&
        !LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      LLVMBuildBr(cg->builder, merge_bb);
    }
    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
    if (has_value && incoming_count > 0) {
      LLVMTypeRef result_type = cg_type(cg, n->type);
      LLVMValueRef phi = LLVMBuildPhi(cg->builder, result_type, "match.result");
      LLVMAddIncoming(phi, incoming_vals, incoming_bbs, incoming_count);
      return phi;
    }
    return NULL;
  }
  case NODE_SLICE: {
    LLVMValueRef base = cg_expr(cg, n->left);
    if (!base)
      return NULL;
    Node *base_type = n->left->type;
    Node *range = n->right;
    LLVMValueRef lo = NULL;
    LLVMValueRef hi = NULL;
    if (range && range->kind == NODE_RANGE) {
      if (range->left)
        lo = cg_expr(cg, range->left);
      if (range->right)
        hi = cg_expr(cg, range->right);
    }
    if (!lo)
      lo = LLVMConstInt(LLVMInt64TypeInContext(cg->ctx), 0, false);
    LLVMTypeRef elem_type = NULL;
    LLVMValueRef data_ptr = NULL;
    if (base_type->kind == NODE_TYPE_ARRAY) {
      elem_type = cg_type(cg, base_type->ptr);
      LLVMValueRef array_ptr = NULL;
      if (n->left->kind == NODE_IDENT && n->left->decl) {
        array_ptr = cg_get_value(cg, n->left->decl);
      } else {
        LLVMTypeRef arr_type = cg_type(cg, base_type);
        LLVMValueRef tmp = LLVMBuildAlloca(cg->builder, arr_type, "");
        LLVMBuildStore(cg->builder, base, tmp);
        array_ptr = tmp;
      }
      LLVMValueRef indices[2];
      indices[0] = LLVMConstInt(LLVMInt64TypeInContext(cg->ctx), 0, false);
      indices[1] = lo;
      LLVMTypeRef arr_type = cg_type(cg, base_type);
      data_ptr =
          LLVMBuildGEP2(cg->builder, arr_type, array_ptr, indices, 2, "");
      if (!hi) {
        i64 arr_len = 0;
        if (base_type->left && base_type->left->kind == NODE_INT) {
          arr_len = parse_int_literal(tok_text(base_type->left->token));
        }
        hi = LLVMConstInt(LLVMInt64TypeInContext(cg->ctx), arr_len, false);
      }
    } else if (base_type->kind == NODE_TYPE_SLICE) {
      elem_type = cg_type(cg, base_type->ptr);
      LLVMValueRef old_data = LLVMBuildExtractValue(cg->builder, base, 0, "");
      data_ptr = LLVMBuildGEP2(cg->builder, elem_type, old_data, &lo, 1, "");
      if (!hi) {
        hi = LLVMBuildExtractValue(cg->builder, base, 1, "");
      }
    } else {
      return NULL;
    }
    LLVMValueRef len = LLVMBuildSub(cg->builder, hi, lo, "slice.len");
    LLVMTypeRef slice_type = cg_type(cg, n->type);
    LLVMValueRef slice = LLVMGetUndef(slice_type);
    slice = LLVMBuildInsertValue(cg->builder, slice, data_ptr, 0, "");
    slice = LLVMBuildInsertValue(cg->builder, slice, len, 1, "");
    return slice;
  }
  case NODE_RANGE: {
    return cg_expr(cg, n->left);
  }
  case NODE_TUPLE: {
    LLVMTypeRef tuple_type = cg_type(cg, n->type);
    LLVMValueRef tuple = LLVMGetUndef(tuple_type);
    int i = 0;
    for (Node *elem = n->ptr; elem; elem = elem->next, i++) {
      LLVMValueRef elem_val = cg_expr(cg, elem);
      tuple = LLVMBuildInsertValue(cg->builder, tuple, elem_val, i, "");
    }
    return tuple;
  }
  default:
    return NULL;
  }
}

static void cg_stmt(Codegen *cg, Node *n) {
  if (!n)
    return;
  switch (n->kind) {
  case NODE_RETURN: {
    if (n->left) {
      LLVMValueRef val = cg_expr(cg, n->left);
      LLVMBuildRet(cg->builder, val);
    } else {
      LLVMBuildRetVoid(cg->builder);
    }
    break;
  }
  case NODE_VAR: {
    if (n->ptr && n->left && n->left->type &&
        n->left->type->kind == NODE_TYPE_TUPLE) {
      LLVMValueRef init_val = cg_expr(cg, n->left);
      int i = 0;
      for (Node *name = n->ptr; name; name = name->next, i++) {
        LLVMTypeRef var_type = cg_type(cg, name->type);
        Str8 var_name = tok_text(name->token);
        char name_buf[256];
        snprintf(name_buf, sizeof(name_buf), "%.*s", (int)var_name.len,
                 var_name.data);
        LLVMValueRef alloca = LLVMBuildAlloca(cg->builder, var_type, name_buf);
        cg_set_value(cg, name, alloca);
        if (var_name.len == 1 && var_name.data[0] == '_') {
          continue;
        }
        LLVMValueRef elem = LLVMBuildExtractValue(cg->builder, init_val, i, "");
        LLVMBuildStore(cg->builder, elem, alloca);
      }
    } else {
      LLVMTypeRef var_type = cg_type(cg, n->type);
      Str8 name = tok_text(n->token);
      char name_buf[256];
      snprintf(name_buf, sizeof(name_buf), "%.*s", (int)name.len, name.data);
      LLVMValueRef alloca = LLVMBuildAlloca(cg->builder, var_type, name_buf);
      cg_set_value(cg, n, alloca);
      if (n->left) {
        LLVMValueRef init_val = cg_expr(cg, n->left);
        if (init_val) {
          init_val = cg_convert(cg, init_val, n->left->type, n->type);
          LLVMBuildStore(cg->builder, init_val, alloca);
        }
      }
    }
    break;
  }
  case NODE_IF:
  case NODE_BINARY:
  case NODE_CALL:
  case NODE_BLOCK:
  case NODE_FOR:
  case NODE_MATCH:
    cg_expr(cg, n);
    break;
  case NODE_BREAK:
    if (cg->break_target) {
      LLVMBuildBr(cg->builder, cg->break_target);
    }
    break;
  case NODE_CONTINUE:
    if (cg->continue_target) {
      LLVMBuildBr(cg->builder, cg->continue_target);
    }
    break;
  default:
    cg_expr(cg, n);
    break;
  }
}

static void cg_decl(Codegen *cg, Node *n) {
  if (!n)
    return;
  switch (n->kind) {
  case NODE_FUNC: {
    if (n->right) {
      break;
    }
    LLVMTypeRef ret_type = cg_type(cg, n->type);
    int param_count = 0;
    for (Node *p = n->ptr; p; p = p->next) {
      if (p->kind == NODE_PARAM)
        param_count++;
    }
    LLVMTypeRef *param_types = NEW_ARRAY(cg->arena, LLVMTypeRef, param_count);
    int i = 0;
    for (Node *p = n->ptr; p; p = p->next) {
      if (p->kind == NODE_PARAM) {
        param_types[i++] = cg_type(cg, p->type);
      }
    }
    LLVMTypeRef func_type =
        LLVMFunctionType(ret_type, param_types, param_count, false);
    Str8 name = tok_text(n->token);
    char name_buf[256];
    snprintf(name_buf, sizeof(name_buf), "%.*s", (int)name.len, name.data);
    LLVMValueRef func = LLVMAddFunction(cg->mod, name_buf, func_type);
    cg_set_value(cg, n, func);
    cg->current_func = func;
    LLVMBasicBlockRef entry =
        LLVMAppendBasicBlockInContext(cg->ctx, func, "entry");
    LLVMPositionBuilderAtEnd(cg->builder, entry);
    i = 0;
    for (Node *p = n->ptr; p; p = p->next) {
      if (p->kind == NODE_PARAM) {
        LLVMValueRef param_val = LLVMGetParam(func, i);
        cg_set_value(cg, p, param_val);
        i++;
      }
    }
    if (n->left) {
      cg_expr(cg, n->left);
    }
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      if (ret_type == LLVMVoidTypeInContext(cg->ctx)) {
        LLVMBuildRetVoid(cg->builder);
      } else {
        LLVMBuildRet(cg->builder, LLVMConstNull(ret_type));
      }
    }
    cg->current_func = NULL;
    break;
  }
  case NODE_FOREIGN: {
    LLVMTypeRef ret_type = cg_type(cg, n->type);
    int param_count = 0;
    bool is_vararg = false;
    for (Node *p = n->ptr; p; p = p->next) {
      if (p->kind == NODE_VARARGS) {
        is_vararg = true;
        break;
      }
      if (p->kind == NODE_PARAM)
        param_count++;
    }
    LLVMTypeRef *param_types = NEW_ARRAY(cg->arena, LLVMTypeRef, param_count);
    int i = 0;
    for (Node *p = n->ptr; p; p = p->next) {
      if (p->kind == NODE_PARAM) {
        param_types[i++] = cg_type(cg, p->type);
      }
    }
    LLVMTypeRef func_type =
        LLVMFunctionType(ret_type, param_types, param_count, is_vararg);
    Str8 name = tok_text(n->token);
    char name_buf[256];
    snprintf(name_buf, sizeof(name_buf), "%.*s", (int)name.len, name.data);
    LLVMValueRef func = LLVMAddFunction(cg->mod, name_buf, func_type);
    LLVMSetLinkage(func, LLVMExternalLinkage);
    cg_set_value(cg, n, func);
    break;
  }
  case NODE_CONST: {
    LLVMValueRef val = cg_expr(cg, n->left);
    cg_set_value(cg, n, val);
    break;
  }
  case NODE_VAR: {
    LLVMTypeRef var_type = cg_type(cg, n->type);
    Str8 name = tok_text(n->token);
    char name_buf[256];
    snprintf(name_buf, sizeof(name_buf), "%.*s", (int)name.len, name.data);
    LLVMValueRef global = LLVMAddGlobal(cg->mod, var_type, name_buf);
    if (n->left) {
      LLVMValueRef init_val = cg_expr(cg, n->left);
      if (init_val && LLVMIsConstant(init_val)) {
        init_val = cg_convert(cg, init_val, n->left->type, n->type);
        LLVMSetInitializer(global, init_val);
      } else {
        LLVMSetInitializer(global, LLVMConstNull(var_type));
      }
    } else {
      LLVMSetInitializer(global, LLVMConstNull(var_type));
    }
    cg_set_value(cg, n, global);
    break;
  }
  case NODE_STRUCT:
  case NODE_UNION:
    break;
  case NODE_ENUM:
    break;
  case NODE_IMPORT:
    break;
  default:
    break;
  }
}

static void codegen(Arena *arena, Node **decls, usize count,
                    const char *bin_name, const char *dump_ir_path) {
  Codegen cg = {0};
  cg.arena = arena;
  cg.ctx = LLVMContextCreate();
  cg.mod = LLVMModuleCreateWithNameInContext("magica", cg.ctx);
  cg.builder = LLVMCreateBuilderInContext(cg.ctx);
  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();
  LLVMInitializeNativeAsmParser();
  const char *triple = LLVMGetDefaultTargetTriple();
  LLVMSetTarget(cg.mod, triple);
  char *err_msg = NULL;
  LLVMTargetRef target;
  if (LLVMGetTargetFromTriple(triple, &target, &err_msg)) {
    fatal("target lookup failed: %s\n", err_msg);
    LLVMDisposeMessage(err_msg);
    goto cleanup_context;
  }
  char *host_cpu = LLVMGetHostCPUName();
  char *host_features = LLVMGetHostCPUFeatures();
  LLVMTargetMachineRef machine = LLVMCreateTargetMachine(
      target, triple, host_cpu, host_features, LLVMCodeGenLevelDefault,
      LLVMRelocPIC, LLVMCodeModelDefault);
  LLVMTargetDataRef layout = LLVMCreateTargetDataLayout(machine);
  LLVMSetModuleDataLayout(cg.mod, layout);
  for (usize i = 0; i < count; i++) {
    cg_decl(&cg, decls[i]);
  }
  if (dump_ir_path) {
    char *err_msg = NULL;
    if (LLVMPrintModuleToFile(cg.mod, dump_ir_path, &err_msg) != 0) {
      fatal("failed to write IR to %s: %s\n", dump_ir_path,
            err_msg ? err_msg : "unknown");
      if (err_msg) {
        LLVMDisposeMessage(err_msg);
      }
    }
  }
  if (LLVMVerifyModule(cg.mod, LLVMReturnStatusAction, &err_msg)) {
    fatal("llvm verification: %s\n", err_msg);
    LLVMDisposeMessage(err_msg);
    goto cleanup_machine;
  }
  char obj_name[256];
  snprintf(obj_name, sizeof(obj_name), "%s.o", bin_name);
  if (LLVMTargetMachineEmitToFile(machine, cg.mod, obj_name, LLVMObjectFile,
                                  &err_msg)) {
    fatal("failed to emit .o: %s\n", err_msg);
    LLVMDisposeMessage(err_msg);
  } else {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "clang %s -o %s", obj_name, bin_name);
    if (system(cmd) != 0) {
      fatal("linker command failed: %s\n", cmd);
    }
    remove(obj_name);
  }
cleanup_machine:
  LLVMDisposeTargetData(layout);
  LLVMDisposeTargetMachine(machine);
  LLVMDisposeMessage(host_cpu);
  LLVMDisposeMessage(host_features);
cleanup_context:
  LLVMDisposeBuilder(cg.builder);
  LLVMDisposeModule(cg.mod);
  LLVMContextDispose(cg.ctx);
}
int main(int argc, char **argv) {
  if (argc < 2) {
    fatal("usage: %s <file.magi> [output_path] [+dump-ir <path>]\n", argv[0]);
  }
  Arena *arena = arena_new(64 * MiB);
  if (!arena)
    fatal("out of memory");
  const char *src_path = NULL;
  char bin_name[256];
  const char *dump_ir_path = NULL;
  int nonflag = 0;
  strcpy(bin_name, "out");
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "+dump-ir") == 0) {
      if (i + 1 >= argc) {
        fatal("flag %s requires a path", argv[i]);
      }
      dump_ir_path = argv[i + 1];
      i++;
      continue;
    }
    if (argv[i][0] == '-') {
      continue;
    }
    if (nonflag == 0) {
      src_path = argv[i];
      nonflag++;
      continue;
    }
    if (nonflag == 1) {
      strncpy(bin_name, argv[i], sizeof(bin_name) - 1);
      bin_name[sizeof(bin_name) - 1] = '\0';
      nonflag++;
      continue;
    }
  }
  if (!src_path) {
    fatal("no input file specified");
  }
  if (strcmp(bin_name, "out") == 0) {
    const char *dot = strrchr(src_path, '.');
    if (dot && strcmp(dot, ".magi") == 0) {
      usize len = dot - src_path;
      if (len >= sizeof(bin_name))
        len = sizeof(bin_name) - 1;
      memcpy(bin_name, src_path, len);
      bin_name[len] = '\0';
    }
  }
  g_err.filename = str8_cstr(src_path);
  g_err.source = os_read_file(arena, src_path);
  if (g_err.source.len == 0)
    fatal("could not read: %s", src_path);
  Parser p = {.lexer = {.src = g_err.source}};
  advance(&p);
  Node *decls[1024];
  usize count = 0;
  while (!check(&p, TK_EOF)) {
    skip_newlines(&p);
    if (check(&p, TK_EOF))
      break;
    Node *n = parse_decl(arena, &p);
    if (n && count < ARRAY_COUNT(decls)) {
      decls[count++] = n;
    } else if (!n) {
      while (!check(&p, TK_NEWLINE) && !check(&p, TK_EOF))
        advance(&p);
    }
    skip_newlines(&p);
  }
  if (!g_err.had_error)
    resolve(arena, decls, count);
  if (!g_err.had_error)
    typecheck(arena, decls, count);
  if (!g_err.had_error) {
    codegen(arena, decls, count, bin_name, dump_ir_path);
  }
  arena_free(arena);
  return g_err.had_error ? 1 : 0;
}
