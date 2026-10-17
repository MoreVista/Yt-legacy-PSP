#ifndef YT_NET_JSON_H
#define YT_NET_JSON_H

#include "../common.h"

/*
 * Minimal read-only JSON DOM. All nodes live in one arena so the whole tree
 * is released with a single json_free(). String values are unescaped in
 * place inside the source buffer, so they are only valid until the source
 * buffer is freed by the caller.
 */
typedef enum {
	JSON_NULL,
	JSON_BOOL,
	JSON_NUMBER,
	JSON_STRING,
	JSON_ARRAY,
	JSON_OBJECT
} json_type_t;

typedef struct json_value json_value_t;

struct json_value {
	json_type_t type;
	/* JSON_BOOL / JSON_NUMBER */
	double        number;
	int           boolean;
	/* JSON_STRING: unescaped, NUL-terminated, owned by the source buffer */
	const char   *string;
	int           string_len;
	/* JSON_ARRAY / JSON_OBJECT: children */
	json_value_t *first;
	json_value_t *next;
	const char   *key;      /* member name when the parent is an object */
};

typedef struct json_doc json_doc_t;

/*
 * Parses text. On success returns a document that owns both the text buffer
 * and the node pool; release it with json_doc_free(). Returns NULL on
 * malformed input or allocation failure.
 *
 * String values are unescaped in place inside the text buffer, so they stay
 * valid exactly as long as the document does.
 */
json_doc_t   *json_parse(char *text, int len);
void          json_doc_free(json_doc_t *doc);
json_value_t *json_root(const json_doc_t *doc);

/* Object member lookup; returns NULL when absent. */
json_value_t *json_get(const json_value_t *obj, const char *key);

/* Array element access. */
int           json_count(const json_value_t *arr);
json_value_t *json_at(const json_value_t *arr, int index);

/* Convenience accessors that tolerate NULL and type mismatches. */
const char   *json_str(const json_value_t *v, const char *fallback);
double        json_num(const json_value_t *v, double fallback);
int           json_int(const json_value_t *v, int fallback);

#endif
