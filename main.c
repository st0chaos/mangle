/*
 * A portable and small tangler for Markdown.
 *
 * Supported code block infostring forms include, but are not limited to:
 *
 *   ```c {name=main tangle=out.c}
 *   ```c name=main tangle=out.c
 *   ```{name=main tangle=out.c}
 *   ```c {#id .class name=main refer=part tangle="out file.c"}
 *
 * Recognized attributes:
 *   name    - globally unique chunk name. The block content becomes a
 *             contribution to this chunk.
 *   refer   - append this block's expanded content to the named chunk.
 *   tangle  - write this block's expanded content to the named file.
 *
 * Inside code block content, references have the form:
 *
 *   "<<chunk-name>>"
 *
 * References are expanded recursively. Circular references and undefined
 * references are reported as errors.
 *
 * The attribute parser is intentionally generic: all infostring attributes
 * are stored in an AttrList, and only name/refer/tangle are consumed here.
 * Adding support for more attributes later only requires reading another
 * key from the AttrList.
 *
 * This program is C90 oriented and uses only the standard library.
 */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define PRINTF_FORMAT(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define PRINTF_FORMAT(fmt, args)
#endif

#include <sys/stat.h>

/* ------------------------------------------------------------------------- */
/* Error reporting                                                           */
/* ------------------------------------------------------------------------- */

static void die(const char *fmt, ...) PRINTF_FORMAT(1, 2);
static void die(const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(1);
}

static void error_at(int line, const char *fmt, ...) PRINTF_FORMAT(2, 3);
static void error_at(int line, const char *fmt, ...) {
  va_list ap;

  fprintf(stderr, "line %d: ", line);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(1);
}

/* ------------------------------------------------------------------------- */
/* Memory helpers                                                            */
/* ------------------------------------------------------------------------- */

static void *xmalloc(size_t n) {
  void *p;

  p = malloc(n ? n : 1);
  if (!p)
    die("out of memory");
  return p;
}

static void *xrealloc(void *p, size_t n) {
  void *q;

  q = realloc(p, n ? n : 1);
  if (!q)
    die("out of memory");
  return q;
}

static char *xstrdup(const char *s) {
  size_t n;
  char *p;

  n = strlen(s);
  p = (char *)xmalloc(n + 1);
  memcpy(p, s, n + 1);
  return p;
}

static char *xstrndup(const char *s, size_t n) {
  char *p;

  p = (char *)xmalloc(n + 1);
  if (n > 0) {
    memcpy(p, s, n);
  }
  p[n] = '\0';
  return p;
}

/* ------------------------------------------------------------------------- */
/* Growable string buffer                                                    */
/* ------------------------------------------------------------------------- */

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} StrBuf;

static void sb_init(StrBuf *sb) {
  sb->data = NULL;
  sb->len = 0;
  sb->cap = 0;
}

static void sb_reserve(StrBuf *sb, size_t need) {
  size_t cap;

  if (need <= sb->cap) {
    return;
  }

  cap = sb->cap ? sb->cap : 64;
  while (cap < need) {
    if (cap > (size_t)-1 / 2) {
      die("string too large");
    }
    cap *= 2;
  }

  sb->data = (char *)xrealloc(sb->data, cap);
  sb->cap = cap;
}

static void sb_append_n(StrBuf *sb, const char *s, size_t n) {
  sb_reserve(sb, sb->len + n + 1);
  if (n > 0) {
    memcpy(sb->data + sb->len, s, n);
  }
  sb->len += n;
  sb->data[sb->len] = '\0';
}

static void sb_append_char(StrBuf *sb, char c) { sb_append_n(sb, &c, 1); }

static void sb_append_str(StrBuf *sb, const char *s) {
  sb_append_n(sb, s, strlen(s));
}

/*
 * Return the ownership of the string.
 * If the StrBuf is empty, it return a new allocated empty string.
 */
static char *sb_take(StrBuf *sb) {
  char *p;

  if (!sb->data) {
    sb->data = (char *)xmalloc(1);
    sb->data[0] = '\0';
  }

  p = sb->data;
  sb->data = NULL;
  sb->len = 0;
  sb->cap = 0;
  return p;
}

static void sb_free(StrBuf *sb) {
  free(sb->data);
  sb->data = NULL;
  sb->len = 0;
  sb->cap = 0;
}

/* ------------------------------------------------------------------------- */
/* Generic attribute list                                                    */
/* ------------------------------------------------------------------------- */

struct AttrItem {
  char *key;
  char *value;
  struct AttrItem *next;
};

typedef struct {
  struct AttrItem *head;
  struct AttrItem *tail;
} Attrs;

static void attr_add(Attrs *list, const char *key, const char *value) {
  struct AttrItem *a;

  a = (struct AttrItem *)xmalloc(sizeof(struct AttrItem));
  a->key = xstrdup(key);
  a->value = xstrdup(value ? value : "");
  a->next = NULL;

  if (list->tail) {
    list->tail->next = a;
  } else {
    list->head = a;
  }
  list->tail = a;
}

static void attr_free_all(Attrs *list) {
  struct AttrItem *a;
  struct AttrItem *next;

  a = list->head;
  while (a) {
    next = a->next;
    free(a->key);
    free(a->value);
    free(a);
    a = next;
  }

  list->head = NULL;
  list->tail = NULL;
}

/*
 * Return the first value for key. If the key occurs more than once, report
 * an error.
 */
static char *attr_get_unique(Attrs *list, const char *key, int line) {
  struct AttrItem *a;
  char *found;
  int count;

  found = NULL;
  count = 0;

  for (a = list->head; a; a = a->next) {
    if (strcmp(a->key, key) == 0) {
      count++;
      found = a->value;
    }
  }

  if (count > 1) {
    error_at(line, "duplicate attribute '%s'", key);
  }

  return found;
}

/* ------------------------------------------------------------------------- */
/* Pandoc-like infostring attribute parser                                   */
/* ------------------------------------------------------------------------- */

static void skip_space(const char **ps) {
  const char *s;

  s = *ps;
  while (*s && isspace((unsigned char)*s)) {
    s++;
  }
  *ps = s;
}

/*
 * Decide whether the first non-space token is a language word rather than
 * an attribute. A token containing '=' is an attribute. Tokens beginning
 * with '#', '.', '{' or '}' are also attributes/brace syntax.
 */
static int first_token_is_language(const char *s) {
  const char *p;

  p = s;
  while (*p && isspace((unsigned char)*p)) {
    p++;
  }

  if (!*p || *p == '{' || *p == '}' || *p == '#' || *p == '.') {
    return 0;
  }

  while (*p && !isspace((unsigned char)*p) && *p != '{' && *p != '}') {
    if (*p == '=') {
      return 0;
    }
    p++;
  }

  return 1;
}

/*
 * Read one attribute value. A value may be quoted with single or double
 * quotes, allowing spaces. Backslash escapes are supported for the active
 * quote character and for backslash itself.
 */
static char *read_attr_value(const char **ps) {
  const char *p;
  StrBuf sb;
  char quote;

  p = *ps;
  sb_init(&sb);

  if (*p == '"' || *p == '\'') {
    quote = *p++;
    while (*p) {
      if (*p == '\\' && (p[1] == quote || p[1] == '\\')) {
        p++;
        sb_append_char(&sb, *p++);
      } else if (*p == quote) {
        p++;
        break;
      } else {
        sb_append_char(&sb, *p++);
      }
    }
  } else {
    while (*p && !isspace((unsigned char)*p) && *p != '{' && *p != '}') {
      sb_append_char(&sb, *p++);
    }
  }

  *ps = p;
  return sb_take(&sb);
}

/*
 * Parse a Pandoc-like infostring.
 *
 * The parser accepts:
 *   - an optional leading language word
 *   - optional outer braces
 *   - #id, .class, key=value, and bare key tokens
 *
 * All attributes are stored. The caller can later query any key, which is
 * the extension point for future infostring attributes.
 */
static void parse_info_attrs(const char *info, Attrs *attrs) {
  const char *p;

  p = info;
  skip_space(&p);

  if (first_token_is_language(p)) {
    while (*p && !isspace((unsigned char)*p) && *p != '{' && *p != '}') {
      p++;
    }
  }

  for (;;) {
    const char *ks;
    char *key;
    char *value;

    skip_space(&p);
    while (*p == '{' || *p == '}') {
      p++;
      skip_space(&p);
    }

    if (!*p) {
      break;
    }

    key = NULL;
    value = NULL;

    if (*p == '#') {
      p++;
      key = xstrdup("id");
      value = read_attr_value(&p);
    } else if (*p == '.') {
      p++;
      key = xstrdup("class");
      value = read_attr_value(&p);
    } else {
      ks = p;
      while (*p && !isspace((unsigned char)*p) && *p != '=' && *p != '{' &&
             *p != '}') {
        p++;
      }

      if (p == ks) {
        p++;
        continue;
      }

      key = xstrndup(ks, (size_t)(p - ks));

      if (*p == '=') {
        p++;
        value = read_attr_value(&p);
      } else {
        value = xstrdup("");
      }
    }

    if (key && key[0]) {
      attr_add(attrs, key, value);
    }

    free(key);
    free(value);
  }
}

/* ------------------------------------------------------------------------- */
/* Blocks, chunks, and parser context                                       */
/* ------------------------------------------------------------------------- */

typedef struct {
  int line;
  char *info;
  char *content;
  Attrs attrs;

  /*
   * These point into attrs; they are borrowed pointers and must not be
   * freed separately.
   */
  char *name;
  char *refer;
  char *tangle;
} Block;

typedef struct {
  char *name;

  /* Indices into Ctx.blocks. Contributions are in document order. */
  int *blocks;
  int block_count;
  int block_cap;

  int name_defined; /* true if a block used name= for this chunk */

  /*
   * Expansion state:
   * - 0 = not expanded
   * - 1 = currently expanding (used for cycle detection)
   * - 2 = expanded and cached
   */
  int state;
  char *cache;
} Chunk;

typedef struct {
  Block *blocks;
  int block_count;
  int block_cap;

  Chunk *chunks;
  int chunk_count;
  int chunk_cap;
} Ctx;

static char *expand_chunk(Ctx *ctx, int ci);
static char *expand_block_content(Ctx *ctx, int bi);

static int ctx_add_block(Ctx *ctx, int line, char *info, char *content) {
  int idx;
  Block *b;

  if (ctx->block_count == ctx->block_cap) {
    ctx->block_cap = ctx->block_cap ? ctx->block_cap * 2 : 16;
    ctx->blocks =
        (Block *)xrealloc(ctx->blocks, (size_t)ctx->block_cap * sizeof(Block));
  }

  idx = ctx->block_count++;
  b = &ctx->blocks[idx];
  memset(b, 0, sizeof(*b));
  b->line = line;
  b->info = info;
  b->content = content;
  return idx;
}

static int find_chunk(Ctx *ctx, const char *name) {
  int i;

  for (i = 0; i < ctx->chunk_count; i++) {
    if (strcmp(ctx->chunks[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

static int find_or_add_chunk(Ctx *ctx, const char *name) {
  int i;
  Chunk *ch;

  i = find_chunk(ctx, name);
  if (i >= 0) {
    return i;
  }

  if (ctx->chunk_count == ctx->chunk_cap) {
    ctx->chunk_cap = ctx->chunk_cap ? ctx->chunk_cap * 2 : 16;
    ctx->chunks =
        (Chunk *)xrealloc(ctx->chunks, (size_t)ctx->chunk_cap * sizeof(Chunk));
  }

  i = ctx->chunk_count++;
  ch = &ctx->chunks[i];
  memset(ch, 0, sizeof(*ch));
  ch->name = xstrdup(name);
  return i;
}

static void chunk_add_block(Chunk *ch, int bi) {
  if (ch->block_count == ch->block_cap) {
    ch->block_cap = ch->block_cap ? ch->block_cap * 2 : 8;
    ch->blocks =
        (int *)xrealloc(ch->blocks, (size_t)ch->block_cap * sizeof(int));
  }

  ch->blocks[ch->block_count++] = bi;
}

/*
 * Register one code block's name/refer attributes in the chunk table.
 *
 * name= creates/defines a globally unique named chunk.
 * refer= appends this block as a contribution to another chunk.
 *
 * If a block has both name=foo and refer=foo, the block is added only once.
 */
static void register_block(Ctx *ctx, int bi) {
  Block *b;
  int ci;
  Chunk *ch;

  b = &ctx->blocks[bi];

  if (b->name) {
    ci = find_or_add_chunk(ctx, b->name);
    ch = &ctx->chunks[ci];

    if (ch->name_defined) {
      error_at(b->line, "duplicate name '%s'", b->name);
    }

    ch->name_defined = 1;
    chunk_add_block(ch, bi);
  }

  if (b->refer) {
    if (b->name && strcmp(b->name, b->refer) == 0) {
      /* Already added above. */
    } else {
      ci = find_or_add_chunk(ctx, b->refer);
      chunk_add_block(&ctx->chunks[ci], bi);
    }
  }
}

static void handle_new_block(Ctx *ctx, int line, char *info, char *content) {
  int bi;
  Block *b;

  bi = ctx_add_block(ctx, line, info, content);
  b = &ctx->blocks[bi];

  parse_info_attrs(b->info, &b->attrs);

  b->name = attr_get_unique(&b->attrs, "name", b->line);
  b->refer = attr_get_unique(&b->attrs, "refer", b->line);
  b->tangle = attr_get_unique(&b->attrs, "tangle", b->line);

  if (b->name && b->name[0] == '\0') {
    error_at(b->line, "attribute 'name' requires a value");
  }
  if (b->refer && b->refer[0] == '\0') {
    error_at(b->line, "attribute 'refer' requires a value");
  }
  if (b->tangle && b->tangle[0] == '\0') {
    error_at(b->line, "attribute 'tangle' requires a value");
  }

  register_block(ctx, bi);
}

static void ctx_free(Ctx *ctx) {
  int i;

  for (i = 0; i < ctx->block_count; i++) {
    Block *b;

    b = &ctx->blocks[i];
    free(b->info);
    free(b->content);
    attr_free_all(&b->attrs);
  }
  free(ctx->blocks);

  for (i = 0; i < ctx->chunk_count; i++) {
    Chunk *ch;

    ch = &ctx->chunks[i];
    free(ch->name);
    free(ch->blocks);
    free(ch->cache);
  }
  free(ctx->chunks);

  ctx->blocks = NULL;
  ctx->block_count = 0;
  ctx->block_cap = 0;

  ctx->chunks = NULL;
  ctx->chunk_count = 0;
  ctx->chunk_cap = 0;
}

/* ------------------------------------------------------------------------- */
/* Expansion                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Expand a chunk by concatenating the expanded content of all of its
 * contributions. The state field detects direct and indirect cycles.
 */
static char *expand_chunk(Ctx *ctx, int ci) {
  Chunk *ch;
  StrBuf sb;
  int k;

  ch = &ctx->chunks[ci];

  if (ch->state == 1) {
    die("circular reference involving '%s'", ch->name);
  }

  if (ch->state == 2) {
    return xstrdup(ch->cache);
  }

  ch->state = 1;
  sb_init(&sb);

  for (k = 0; k < ch->block_count; k++) {
    char *part;

    part = expand_block_content(ctx, ch->blocks[k]);
    sb_append_str(&sb, part);
    free(part);
  }

  ch->state = 2;
  free(ch->cache);
  ch->cache = sb_take(&sb);

  return xstrdup(ch->cache);
}

/*
 * Expand all "<<name>>" references in one block's raw content.
 */
static char *expand_block_content(Ctx *ctx, int bi) {
  Block *b;
  const char *s;
  size_t len;
  size_t i;
  StrBuf out;

  b = &ctx->blocks[bi];
  s = b->content;
  len = strlen(s);
  i = 0;
  sb_init(&out);

  while (i < len) {
    if (s[i] == '"' && i + 2 < len && s[i + 1] == '<' && s[i + 2] == '<') {
      size_t j;

      j = i + 3;
      while (j + 2 < len &&
             !(s[j] == '>' && s[j + 1] == '>' && s[j + 2] == '"')) {
        j++;
      }

      if (j + 2 < len) {
        size_t name_start;
        size_t name_end;

        name_start = i + 3;
        name_end = j;

        while (name_start < name_end && isspace((unsigned char)s[name_start])) {
          name_start++;
        }
        while (name_end > name_start &&
               isspace((unsigned char)s[name_end - 1])) {
          name_end--;
        }

        if (name_end > name_start) {
          char *name;
          int ci;
          char *exp;

          name = xstrndup(s + name_start, name_end - name_start);
          ci = find_chunk(ctx, name);

          if (ci < 0) {
            error_at(b->line, "undefined reference %s", name);
          }

          exp = expand_chunk(ctx, ci);
          sb_append_str(&out, exp);
          free(exp);
          free(name);

          i = j + 3;
          continue;
        }
      }
    }

    sb_append_char(&out, s[i]);
    i++;
  }

  return sb_take(&out);
}

/* ------------------------------------------------------------------------- */
/* CommonMark fenced code block parsing                                      */
/* ------------------------------------------------------------------------- */

/*
 * Recognize an opening fenced code block line.
 *
 * Rules implemented:
 *   - at most 3 leading spaces
 *   - fence is at least 3 backticks or at least 3 tildes
 *   - for backtick fences, the info string may not contain a backtick
 *   - info string is trimmed
 */
static int is_fence_open(const char *line, size_t len, int *indent,
                         char *fence_char, int *fence_len,
                         const char **info_start, size_t *info_len) {
  size_t i;
  size_t spaces;
  size_t start;
  char c;
  size_t is;
  size_t ie;
  size_t k;

  i = 0;
  spaces = 0;

  while (i < len && line[i] == ' ') {
    spaces++;
    i++;
  }

  if (spaces > 3) {
    return 0;
  }
  if (i >= len) {
    return 0;
  }

  c = line[i];
  if (c != '`' && c != '~') {
    return 0;
  }

  start = i;
  while (i < len && line[i] == c) {
    i++;
  }

  if (i - start < 3) {
    return 0;
  }

  is = i;
  ie = len;

  while (is < ie && (line[is] == ' ' || line[is] == '\t')) {
    is++;
  }
  while (ie > is && (line[ie - 1] == ' ' || line[ie - 1] == '\t')) {
    ie--;
  }

  if (c == '`') {
    for (k = is; k < ie; k++) {
      if (line[k] == '`') {
        return 0;
      }
    }
  }

  *indent = (int)spaces;
  *fence_char = c;
  *fence_len = (int)(i - start);
  *info_start = line + is;
  *info_len = ie - is;
  return 1;
}

/*
 * Recognize a closing fenced code block line.
 */
static int is_fence_close(const char *line, size_t len, char fence_char,
                          int fence_len) {
  size_t i;
  size_t spaces;
  size_t start;

  i = 0;
  spaces = 0;

  while (i < len && line[i] == ' ') {
    spaces++;
    i++;
  }

  if (spaces > 3) {
    return 0;
  }
  if (i >= len || line[i] != fence_char) {
    return 0;
  }

  start = i;
  while (i < len && line[i] == fence_char) {
    i++;
  }

  if ((int)(i - start) < fence_len) {
    return 0;
  }

  while (i < len) {
    if (line[i] != ' ' && line[i] != '\t') {
      return 0;
    }
    i++;
  }

  return 1;
}

/*
 * Append one code block content line, removing up to the opening fence's
 * indentation. A '\n' is appended for each source line.
 */
static void append_content_line(StrBuf *sb, const char *line, size_t len,
                                int indent) {
  size_t i;
  int removed;

  i = 0;
  removed = 0;

  while (i < len && removed < indent && line[i] == ' ') {
    i++;
    removed++;
  }

  sb_append_n(sb, line + i, len - i);
  sb_append_char(sb, '\n');
}

/*
 * Scan the already-split Markdown lines and extract fenced code blocks with
 * their infostrings and contents.
 */
static void parse_markdown(Ctx *ctx, char **lines, int line_count) {
  int i;

  i = 0;
  while (i < line_count) {
    char *line;
    size_t len;
    int indent;
    int fence_len;
    char fence_char;
    const char *info_start;
    size_t info_len;

    line = lines[i];
    len = strlen(line);

    if (is_fence_open(line, len, &indent, &fence_char, &fence_len, &info_start,
                      &info_len)) {
      int start_line;
      StrBuf content;
      int j;
      int closed;
      char *info_str;

      start_line = i + 1;
      sb_init(&content);
      j = i + 1;
      closed = 0;

      while (j < line_count) {
        size_t clen;

        clen = strlen(lines[j]);
        if (is_fence_close(lines[j], clen, fence_char, fence_len)) {
          closed = 1;
          break;
        }

        append_content_line(&content, lines[j], clen, indent);
        j++;
      }

      info_str = xstrndup(info_start, info_len);
      handle_new_block(ctx, start_line, info_str, sb_take(&content));

      if (closed) {
        i = j + 1;
      } else {
        i = line_count;
      }
      continue;
    }

    i++;
  }
}

/* ------------------------------------------------------------------------- */
/* File input and line splitting                                             */
/* ------------------------------------------------------------------------- */

static int create_parent_dirs(const char *path) {
  char *buf;
  char *p;
  size_t len;

  if (path == NULL)
    return -1;
  len = strlen(path);
  buf = xmalloc(len + 1);

  /* Remove the basename part of the path */
  strncpy(buf, path, len + 1);
  p = strrchr(buf, '/');
  if (p == NULL) {
    free(buf);
    return 0;
  }
  *p = '\0';

  for (p = buf; *p != '\0'; ++p) {
    if (*p == '/') {
      char ch = *p;
      *p = '\0';
      if (buf[0] != '\0' && mkdir(buf, 0777) != 0 && errno != EEXIST) {
        free(buf);
        return -1;
      }
      *p = ch;
    }
  }
  if (buf[0] != '\0' && mkdir(buf, 0777) != 0 && errno != EEXIST) {
    free(buf);
    return -1;
  }

  free(buf);
  return 0;
}

static char *read_file(const char *filename, size_t *out_len) {
  FILE *f;
  long size;
  char *buf;
  size_t n;

  f = fopen(filename, "rb");
  if (!f) {
    die("cannot open input file '%s'", filename);
  }

  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    die("cannot seek input file '%s'", filename);
  }

  size = ftell(f);
  if (size < 0) {
    fclose(f);
    die("cannot determine size of input file '%s'", filename);
  }

  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    die("cannot seek input file '%s'", filename);
  }

  buf = (char *)xmalloc((size_t)size + 1);
  n = fread(buf, 1, (size_t)size, f);
  if (n != (size_t)size) {
    fclose(f);
    die("cannot read input file '%s'", filename);
  }

  buf[n] = '\0';
  fclose(f);
  *out_len = n;
  return buf;
}

/*
 * Split a mutable buffer into lines. '\n' is replaced by '\0'. A trailing
 * '\r' is removed for CRLF compatibility.
 */
static void split_lines(char *buf, size_t len, char ***out_lines,
                        int *out_count) {
  int count;
  char **lines;
  int idx;
  char *p;
  char *end;

  count = 0;
  p = buf;
  end = buf + len;

  while (p < end) {
    count++;
    p = (char *)memchr(p, '\n', (size_t)(end - p));
    if (!p) {
      break;
    }
    p++;
  }

  if (count == 0) {
    *out_lines = NULL;
    *out_count = 0;
    return;
  }

  lines = (char **)xmalloc((size_t)count * sizeof(char *));
  idx = 0;
  p = buf;

  while (p < end) {
    char *nl;
    size_t l;

    lines[idx++] = p;
    nl = (char *)memchr(p, '\n', (size_t)(end - p));

    if (nl) {
      *nl = '\0';
      p = nl + 1;
    } else {
      p = end;
    }

    l = strlen(lines[idx - 1]);
    if (l > 0 && lines[idx - 1][l - 1] == '\r') {
      lines[idx - 1][l - 1] = '\0';
    }
  }

  *out_lines = lines;
  *out_count = idx;
}

/* ------------------------------------------------------------------------- */
/* Tangle outputs                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
  char *path;
  StrBuf data;
} Output;

typedef struct {
  Output *items;
  int count;
  int cap;
} OutputList;

/*
 * Append expanded content to a tangle output path. Multiple tangle blocks
 * targeting the same file are concatenated in document order.
 */
static void outputs_add(OutputList *ol, const char *path, const char *data) {
  int i;
  Output *o;

  for (i = 0; i < ol->count; i++) {
    if (strcmp(ol->items[i].path, path) == 0) {
      sb_append_str(&ol->items[i].data, data);
      return;
    }
  }

  if (ol->count == ol->cap) {
    ol->cap = ol->cap ? ol->cap * 2 : 8;
    ol->items = (Output *)xrealloc(ol->items, (size_t)ol->cap * sizeof(Output));
  }

  o = &ol->items[ol->count++];
  o->path = xstrdup(path);
  sb_init(&o->data);
  sb_append_str(&o->data, data);
}

static void outputs_write(OutputList *ol) {
  int i;
  FILE *f;

  for (i = 0; i < ol->count; i++) {
    if (create_parent_dirs(ol->items[i].path) != 0) {
      die("cannot create parent directories for %s", ol->items[i].path);
    }

    f = fopen(ol->items[i].path, "wb");
    if (!f) {
      die("cannot open output file '%s'", ol->items[i].path);
    }

    if (ol->items[i].data.len > 0) {
      if (fwrite(ol->items[i].data.data, 1, ol->items[i].data.len, f) !=
          ol->items[i].data.len) {
        fclose(f);
        die("cannot write output file '%s'", ol->items[i].path);
      }
    }

    if (fclose(f) != 0) {
      die("cannot close output file '%s'", ol->items[i].path);
    }
  }
}

static void outputs_free(OutputList *ol) {
  int i;

  for (i = 0; i < ol->count; i++) {
    free(ol->items[i].path);
    sb_free(&ol->items[i].data);
  }

  free(ol->items);
  ol->items = NULL;
  ol->count = 0;
  ol->cap = 0;
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

int main(int argc, char **argv) {
  Ctx ctx;
  char *buf;
  size_t len;
  char **lines;
  int line_count;
  OutputList outputs;
  int i;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <input.md>\n", argv[0]);
    return 2;
  }

  memset(&ctx, 0, sizeof(ctx));

  buf = read_file(argv[1], &len);
  split_lines(buf, len, &lines, &line_count);
  parse_markdown(&ctx, lines, line_count);

  free((void *)lines);
  free(buf);

  memset(&outputs, 0, sizeof(outputs));

  /*
   * Expand every block that has a tangle attribute. Do all expansion
   * before writing any file, so an expansion error does not leave partial
   * output files behind.
   */
  for (i = 0; i < ctx.block_count; i++) {
    Block *b;

    b = &ctx.blocks[i];
    if (b->tangle) {
      char *expanded;

      expanded = expand_block_content(&ctx, i);
      outputs_add(&outputs, b->tangle, expanded);
      free(expanded);
    }
  }

  outputs_write(&outputs);
  outputs_free(&outputs);
  ctx_free(&ctx);

  return 0;
}
