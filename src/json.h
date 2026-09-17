#ifndef MKDBG_JSON_H
#define MKDBG_JSON_H

#include <stddef.h>
#include <stdio.h>

#define JSON_TOKEN_TEXT_MAX 1024
#define JSON_WRITER_MAX_DEPTH 32

typedef enum {
  JSON_TOKEN_ERROR = -1,
  JSON_TOKEN_EOF = 0,
  JSON_TOKEN_OBJECT_BEGIN,
  JSON_TOKEN_OBJECT_END,
  JSON_TOKEN_ARRAY_BEGIN,
  JSON_TOKEN_ARRAY_END,
  JSON_TOKEN_COLON,
  JSON_TOKEN_COMMA,
  JSON_TOKEN_STRING,
  JSON_TOKEN_NUMBER,
  JSON_TOKEN_TRUE,
  JSON_TOKEN_FALSE,
  JSON_TOKEN_NULL
} JsonTokenType;

typedef struct {
  JsonTokenType type;
  char text[JSON_TOKEN_TEXT_MAX];
  int truncated;
} JsonToken;

typedef struct {
  FILE *file;
  int error;
} JsonReader;

typedef enum {
  JSON_WRITER_OBJECT,
  JSON_WRITER_ARRAY
} JsonWriterContainer;

typedef struct {
  FILE *file;
  int error;
  int root_written;
  size_t depth;
  struct {
    JsonWriterContainer type;
    size_t count;
    int expecting_value;
  } stack[JSON_WRITER_MAX_DEPTH];
} JsonWriter;

void json_reader_init(JsonReader *reader, FILE *file);
JsonTokenType json_reader_next(JsonReader *reader, JsonToken *token);
int json_token_to_long(const JsonToken *token, long *out);

void json_writer_init(JsonWriter *writer, FILE *file);
int json_writer_begin_object(JsonWriter *writer);
int json_writer_end_object(JsonWriter *writer);
int json_writer_begin_array(JsonWriter *writer);
int json_writer_end_array(JsonWriter *writer);
int json_writer_key(JsonWriter *writer, const char *key);
int json_writer_string(JsonWriter *writer, const char *value);
int json_writer_long(JsonWriter *writer, long value);
int json_writer_bool(JsonWriter *writer, int value);
int json_writer_finish(JsonWriter *writer);

#endif
