#include "json.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int reader_get(JsonReader *reader)
{
  int ch = fgetc(reader->file);
  if (ch == EOF && ferror(reader->file)) reader->error = 1;
  return ch;
}

static void token_append(JsonToken *token, unsigned char byte, size_t *length)
{
  if (*length + 1U < sizeof(token->text)) {
    token->text[(*length)++] = (char)byte;
  } else {
    token->truncated = 1;
  }
}

static int read_hex4(JsonReader *reader, uint32_t *out)
{
  uint32_t value = 0;
  int i;
  for (i = 0; i < 4; i++) {
    int ch = reader_get(reader);
    int digit;
    if (ch >= '0' && ch <= '9') digit = ch - '0';
    else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
    else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
    else return -1;
    value = value * 16U + (uint32_t)digit;
  }
  *out = value;
  return 0;
}

static int append_codepoint(JsonToken *token, uint32_t cp, size_t *length)
{
  if (cp <= 0x7fU) {
    token_append(token, (unsigned char)cp, length);
  } else if (cp <= 0x7ffU) {
    token_append(token, (unsigned char)(0xc0U | (cp >> 6)), length);
    token_append(token, (unsigned char)(0x80U | (cp & 0x3fU)), length);
  } else if (cp <= 0xffffU) {
    token_append(token, (unsigned char)(0xe0U | (cp >> 12)), length);
    token_append(token, (unsigned char)(0x80U | ((cp >> 6) & 0x3fU)), length);
    token_append(token, (unsigned char)(0x80U | (cp & 0x3fU)), length);
  } else if (cp <= 0x10ffffU) {
    token_append(token, (unsigned char)(0xf0U | (cp >> 18)), length);
    token_append(token, (unsigned char)(0x80U | ((cp >> 12) & 0x3fU)), length);
    token_append(token, (unsigned char)(0x80U | ((cp >> 6) & 0x3fU)), length);
    token_append(token, (unsigned char)(0x80U | (cp & 0x3fU)), length);
  } else {
    return -1;
  }
  return 0;
}

static JsonTokenType read_string(JsonReader *reader, JsonToken *token)
{
  size_t length = 0U;
  int ch;
  while ((ch = reader_get(reader)) != EOF) {
    if (ch == '"') {
      token->text[length] = '\0';
      return JSON_TOKEN_STRING;
    }
    if ((unsigned int)ch < 0x20U) return JSON_TOKEN_ERROR;
    if (ch != '\\') {
      token_append(token, (unsigned char)ch, &length);
      continue;
    }
    ch = reader_get(reader);
    if (ch == EOF) return JSON_TOKEN_ERROR;
    switch (ch) {
    case '"': case '\\': case '/': token_append(token, (unsigned char)ch, &length); break;
    case 'b': token_append(token, '\b', &length); break;
    case 'f': token_append(token, '\f', &length); break;
    case 'n': token_append(token, '\n', &length); break;
    case 'r': token_append(token, '\r', &length); break;
    case 't': token_append(token, '\t', &length); break;
    case 'u': {
      uint32_t cp;
      if (read_hex4(reader, &cp) != 0) return JSON_TOKEN_ERROR;
      if (cp >= 0xd800U && cp <= 0xdbffU) {
        uint32_t low;
        if (reader_get(reader) != '\\' || reader_get(reader) != 'u' ||
            read_hex4(reader, &low) != 0 || low < 0xdc00U || low > 0xdfffU)
          return JSON_TOKEN_ERROR;
        cp = 0x10000U + ((cp - 0xd800U) << 10) + (low - 0xdc00U);
      } else if (cp >= 0xdc00U && cp <= 0xdfffU) {
        return JSON_TOKEN_ERROR;
      }
      if (append_codepoint(token, cp, &length) != 0) return JSON_TOKEN_ERROR;
      break;
    }
    default: return JSON_TOKEN_ERROR;
    }
  }
  return JSON_TOKEN_ERROR;
}

static JsonTokenType read_word(JsonReader *reader, JsonToken *token, int first)
{
  size_t length = 0U;
  int ch = first;
  do {
    token_append(token, (unsigned char)ch, &length);
    ch = reader_get(reader);
  } while (ch != EOF && (isalnum((unsigned char)ch) || ch == '+' || ch == '-' ||
                         ch == '.' || ch == '_'));
  if (ch != EOF) ungetc(ch, reader->file);
  token->text[length] = '\0';
  if (token->truncated) return JSON_TOKEN_ERROR;
  if (strcmp(token->text, "true") == 0) return JSON_TOKEN_TRUE;
  if (strcmp(token->text, "false") == 0) return JSON_TOKEN_FALSE;
  if (strcmp(token->text, "null") == 0) return JSON_TOKEN_NULL;
  if (first == '-' || isdigit((unsigned char)first)) return JSON_TOKEN_NUMBER;
  return JSON_TOKEN_ERROR;
}

void json_reader_init(JsonReader *reader, FILE *file)
{
  memset(reader, 0, sizeof(*reader));
  reader->file = file;
}

JsonTokenType json_reader_next(JsonReader *reader, JsonToken *token)
{
  int ch;
  memset(token, 0, sizeof(*token));
  if (!reader || !reader->file || reader->error) return JSON_TOKEN_ERROR;
  do {
    ch = reader_get(reader);
  } while (ch != EOF && isspace((unsigned char)ch));
  if (ch == EOF) return reader->error ? JSON_TOKEN_ERROR : JSON_TOKEN_EOF;
  switch (ch) {
  case '{': token->type = JSON_TOKEN_OBJECT_BEGIN; break;
  case '}': token->type = JSON_TOKEN_OBJECT_END; break;
  case '[': token->type = JSON_TOKEN_ARRAY_BEGIN; break;
  case ']': token->type = JSON_TOKEN_ARRAY_END; break;
  case ':': token->type = JSON_TOKEN_COLON; break;
  case ',': token->type = JSON_TOKEN_COMMA; break;
  case '"': token->type = read_string(reader, token); break;
  default: token->type = read_word(reader, token, ch); break;
  }
  if (token->type == JSON_TOKEN_ERROR) reader->error = 1;
  return token->type;
}

int json_token_to_long(const JsonToken *token, long *out)
{
  char *end = NULL;
  long value;
  if (!token || token->type != JSON_TOKEN_NUMBER || token->truncated || !out) return -1;
  errno = 0;
  value = strtol(token->text, &end, 10);
  if (errno != 0 || end == token->text || *end != '\0') return -1;
  *out = value;
  return 0;
}

static int writer_putc(JsonWriter *writer, int ch)
{
  if (!writer->error && fputc(ch, writer->file) == EOF) writer->error = 1;
  return writer->error ? -1 : 0;
}

static int writer_puts(JsonWriter *writer, const char *text)
{
  if (!writer->error && fputs(text, writer->file) == EOF) writer->error = 1;
  return writer->error ? -1 : 0;
}

static int writer_before_value(JsonWriter *writer)
{
  if (writer->depth == 0U) {
    if (writer->root_written) return -1;
    writer->root_written = 1;
    return 0;
  }
  if (writer->stack[writer->depth - 1U].type == JSON_WRITER_OBJECT) {
    if (!writer->stack[writer->depth - 1U].expecting_value) return -1;
    writer->stack[writer->depth - 1U].expecting_value = 0;
    return 0;
  }
  if (writer->stack[writer->depth - 1U].count++ > 0U) return writer_putc(writer, ',');
  return 0;
}

static int writer_string_raw(JsonWriter *writer, const char *value)
{
  const unsigned char *p = (const unsigned char *)(value ? value : "");
  if (writer_putc(writer, '"') != 0) return -1;
  for (; *p; p++) {
    switch (*p) {
    case '"': if (writer_puts(writer, "\\\"") != 0) return -1; break;
    case '\\': if (writer_puts(writer, "\\\\") != 0) return -1; break;
    case '\b': if (writer_puts(writer, "\\b") != 0) return -1; break;
    case '\f': if (writer_puts(writer, "\\f") != 0) return -1; break;
    case '\n': if (writer_puts(writer, "\\n") != 0) return -1; break;
    case '\r': if (writer_puts(writer, "\\r") != 0) return -1; break;
    case '\t': if (writer_puts(writer, "\\t") != 0) return -1; break;
    default:
      if (*p < 0x20U) {
        char escape[7];
        snprintf(escape, sizeof(escape), "\\u%04x", *p);
        if (writer_puts(writer, escape) != 0) return -1;
      } else if (writer_putc(writer, *p) != 0) return -1;
    }
  }
  return writer_putc(writer, '"');
}

void json_writer_init(JsonWriter *writer, FILE *file)
{
  memset(writer, 0, sizeof(*writer));
  writer->file = file;
}

static int writer_begin(JsonWriter *writer, JsonWriterContainer type, int ch)
{
  if (!writer || !writer->file || writer->depth >= JSON_WRITER_MAX_DEPTH ||
      writer_before_value(writer) != 0 || writer_putc(writer, ch) != 0) return -1;
  writer->stack[writer->depth].type = type;
  writer->stack[writer->depth].count = 0U;
  writer->stack[writer->depth].expecting_value = 0;
  writer->depth++;
  return 0;
}

int json_writer_begin_object(JsonWriter *writer) { return writer_begin(writer, JSON_WRITER_OBJECT, '{'); }
int json_writer_begin_array(JsonWriter *writer) { return writer_begin(writer, JSON_WRITER_ARRAY, '['); }

static int writer_end(JsonWriter *writer, JsonWriterContainer type, int ch)
{
  if (!writer || writer->depth == 0U ||
      writer->stack[writer->depth - 1U].type != type ||
      writer->stack[writer->depth - 1U].expecting_value) return -1;
  writer->depth--;
  return writer_putc(writer, ch);
}

int json_writer_end_object(JsonWriter *writer) { return writer_end(writer, JSON_WRITER_OBJECT, '}'); }
int json_writer_end_array(JsonWriter *writer) { return writer_end(writer, JSON_WRITER_ARRAY, ']'); }

int json_writer_key(JsonWriter *writer, const char *key)
{
  if (!writer || writer->depth == 0U ||
      writer->stack[writer->depth - 1U].type != JSON_WRITER_OBJECT ||
      writer->stack[writer->depth - 1U].expecting_value) return -1;
  if (writer->stack[writer->depth - 1U].count++ > 0U && writer_putc(writer, ',') != 0)
    return -1;
  if (writer_string_raw(writer, key) != 0 || writer_putc(writer, ':') != 0) return -1;
  writer->stack[writer->depth - 1U].expecting_value = 1;
  return 0;
}

int json_writer_string(JsonWriter *writer, const char *value)
{
  if (writer_before_value(writer) != 0) return -1;
  return writer_string_raw(writer, value);
}

int json_writer_long(JsonWriter *writer, long value)
{
  char number[64];
  if (writer_before_value(writer) != 0) return -1;
  snprintf(number, sizeof(number), "%ld", value);
  return writer_puts(writer, number);
}

int json_writer_bool(JsonWriter *writer, int value)
{
  if (writer_before_value(writer) != 0) return -1;
  return writer_puts(writer, value ? "true" : "false");
}

int json_writer_finish(JsonWriter *writer)
{
  if (!writer || writer->depth != 0U || !writer->root_written || writer->error)
    return -1;
  return fflush(writer->file) == 0 ? 0 : -1;
}
