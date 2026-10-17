#include "json.h"
#include <ctype.h>
#include <stdlib.h>

/*
 * Nodes are bump-allocated out of a chain of fixed blocks so that a whole
 * document costs a handful of allocations rather than one per value. The PSP
 * has ~32 MB of usable heap, and Invidious search payloads are large enough
 * that per-node malloc would be both slow and fragmenting.
 */
#define JSON_BLOCK_NODES 64

typedef struct json_block {
	struct json_block *next;
	json_value_t nodes[JSON_BLOCK_NODES];
	int used;
} json_block_t;

typedef struct {
	char          *text;
	int            len;
	int            pos;
	json_block_t  *blocks;
	int            depth;
	int            failed;
} json_parser_t;

static json_value_t *block_alloc(json_parser_t *p)
{
	if (!p->blocks || p->blocks->used == JSON_BLOCK_NODES) {
		json_block_t *b = calloc(1, sizeof(json_block_t));
		if (!b) {
			p->failed = 1;
			return NULL;
		}
		b->next = p->blocks;
		p->blocks = b;
	}
	return &p->blocks->nodes[p->blocks->used++];
}

static void skip_ws(json_parser_t *p)
{
	while (p->pos < p->len) {
		char c = p->text[p->pos];
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
			p->pos++;
		else
			break;
	}
}

static int peek(json_parser_t *p)
{
	return (p->pos < p->len) ? (unsigned char)p->text[p->pos] : -1;
}

static json_value_t *parse_value(json_parser_t *p);

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/*
 * Reads a string starting at the opening quote. The result is unescaped in
 * place: escapes collapse to one byte, so the decoded text is always shorter
 * than the source and the buffer can be reused directly.
 */
static const char *parse_string_raw(json_parser_t *p, int *out_len)
{
	if (peek(p) != '"') {
		p->failed = 1;
		return NULL;
	}
	p->pos++;

	char *dst = p->text + p->pos;   /* decode in place, never grows */
	char *start = dst;

	while (p->pos < p->len) {
		char c = p->text[p->pos];

		if (c == '"') {
			p->pos++;
			*dst = 0;
			*out_len = (int)(dst - start);
			return start;
		}

		if (c == '\\') {
			p->pos++;
			if (p->pos >= p->len)
				break;
			char e = p->text[p->pos++];
			switch (e) {
			case 'n': *dst++ = '\n'; break;
			case 't': *dst++ = '\t'; break;
			case 'r': *dst++ = '\r'; break;
			case 'b': *dst++ = '\b'; break;
			case 'f': *dst++ = '\f'; break;
			case '/': *dst++ = '/';  break;
			case '"': *dst++ = '"';  break;
			case '\\': *dst++ = '\\'; break;
			case 'u': {
				if (p->pos + 4 > p->len) {
					p->failed = 1;
					return NULL;
				}
				int cp = 0;
				for (int i = 0; i < 4; i++) {
					int h = hexval((unsigned char)p->text[p->pos + i]);
					if (h < 0) {
						p->failed = 1;
						return NULL;
					}
					cp = (cp << 4) | h;
				}
				p->pos += 4;
				/* Encode as UTF-8. Surrogate halves are passed through
				 * as replacement characters, which is adequate for the
				 * display-only strings this player handles. */
				if (cp < 0x80) {
					*dst++ = (char)cp;
				} else if (cp < 0x800) {
					*dst++ = (char)(0xC0 | (cp >> 6));
					*dst++ = (char)(0x80 | (cp & 0x3F));
				} else {
					*dst++ = (char)(0xE0 | (cp >> 12));
					*dst++ = (char)(0x80 | ((cp >> 6) & 0x3F));
					*dst++ = (char)(0x80 | (cp & 0x3F));
				}
				break;
			}
			default:
				*dst++ = e;
				break;
			}
			continue;
		}

		*dst++ = c;
		p->pos++;
	}

	p->failed = 1;
	return NULL;
}

static json_value_t *parse_object(json_parser_t *p)
{
	json_value_t *v = block_alloc(p);
	if (!v)
		return NULL;
	v->type = JSON_OBJECT;

	p->pos++;   /* consume '{' */
	skip_ws(p);

	if (peek(p) == '}') {
		p->pos++;
		return v;
	}

	json_value_t **tail = &v->first;
	for (;;) {
		skip_ws(p);

		int klen = 0;
		const char *key = parse_string_raw(p, &klen);
		if (!key)
			return NULL;

		skip_ws(p);
		if (peek(p) != ':') {
			p->failed = 1;
			return NULL;
		}
		p->pos++;

		json_value_t *child = parse_value(p);
		if (!child)
			return NULL;
		child->key = key;

		*tail = child;
		tail = &child->next;

		skip_ws(p);
		int c = peek(p);
		if (c == ',') {
			p->pos++;
			continue;
		}
		if (c == '}') {
			p->pos++;
			return v;
		}
		p->failed = 1;
		return NULL;
	}
}

static json_value_t *parse_array(json_parser_t *p)
{
	json_value_t *v = block_alloc(p);
	if (!v)
		return NULL;
	v->type = JSON_ARRAY;

	p->pos++;   /* consume '[' */
	skip_ws(p);

	if (peek(p) == ']') {
		p->pos++;
		return v;
	}

	json_value_t **tail = &v->first;
	for (;;) {
		json_value_t *child = parse_value(p);
		if (!child)
			return NULL;

		*tail = child;
		tail = &child->next;

		skip_ws(p);
		int c = peek(p);
		if (c == ',') {
			p->pos++;
			continue;
		}
		if (c == ']') {
			p->pos++;
			return v;
		}
		p->failed = 1;
		return NULL;
	}
}

static json_value_t *parse_value(json_parser_t *p)
{
	if (p->failed)
		return NULL;
	if (++p->depth > 64) {          /* guard against hostile nesting */
		p->failed = 1;
		return NULL;
	}

	skip_ws(p);
	json_value_t *v = NULL;
	int c = peek(p);

	if (c == '{') {
		v = parse_object(p);
	} else if (c == '[') {
		v = parse_array(p);
	} else if (c == '"') {
		int slen = 0;
		const char *s = parse_string_raw(p, &slen);
		if (s) {
			v = block_alloc(p);
			if (v) {
				v->type = JSON_STRING;
				v->string = s;
				v->string_len = slen;
			}
		}
	} else if (c == 't' || c == 'f') {
		bool is_true = (c == 't');
		const char *lit = is_true ? "true" : "false";
		int n = is_true ? 4 : 5;
		if (p->pos + n <= p->len && !strncmp(p->text + p->pos, lit, n)) {
			p->pos += n;
			v = block_alloc(p);
			if (v) {
				v->type = JSON_BOOL;
				v->boolean = is_true;
			}
		} else {
			p->failed = 1;
		}
	} else if (c == 'n') {
		if (p->pos + 4 <= p->len && !strncmp(p->text + p->pos, "null", 4)) {
			p->pos += 4;
			v = block_alloc(p);
			if (v)
				v->type = JSON_NULL;
		} else {
			p->failed = 1;
		}
	} else if (c == '-' || (c >= '0' && c <= '9')) {
		char *end = NULL;
		double d = strtod(p->text + p->pos, &end);
		if (end && end != p->text + p->pos) {
			p->pos = (int)(end - p->text);
			v = block_alloc(p);
			if (v) {
				v->type = JSON_NUMBER;
				v->number = d;
			}
		} else {
			p->failed = 1;
		}
	} else {
		p->failed = 1;
	}

	p->depth--;
	return v;
}

struct json_doc {
	char          *text;
	json_block_t  *blocks;
	json_value_t  *root;
};

static void free_blocks(json_block_t *blocks)
{
	while (blocks) {
		json_block_t *n = blocks->next;
		free(blocks);
		blocks = n;
	}
}

json_doc_t *json_parse(char *text, int len)
{
	if (!text || len <= 0)
		return NULL;

	json_parser_t p;
	memset(&p, 0, sizeof(p));
	p.text = text;
	p.len = len;

	json_value_t *root = parse_value(&p);
	if (!root || p.failed) {
		free_blocks(p.blocks);
		return NULL;
	}

	json_doc_t *doc = calloc(1, sizeof(json_doc_t));
	if (!doc) {
		free_blocks(p.blocks);
		return NULL;
	}
	doc->text = text;
	doc->blocks = p.blocks;
	doc->root = root;
	return doc;
}

void json_doc_free(json_doc_t *doc)
{
	if (!doc)
		return;
	free_blocks(doc->blocks);
	free(doc->text);
	free(doc);
}

json_value_t *json_root(const json_doc_t *doc)
{
	return doc ? doc->root : NULL;
}

json_value_t *json_get(const json_value_t *obj, const char *key)
{
	if (!obj || obj->type != JSON_OBJECT || !key)
		return NULL;
	for (const json_value_t *c = obj->first; c; c = c->next)
		if (c->key && !strcmp(c->key, key))
			return (json_value_t *)c;
	return NULL;
}

int json_count(const json_value_t *arr)
{
	if (!arr || arr->type != JSON_ARRAY)
		return 0;
	int n = 0;
	for (const json_value_t *c = arr->first; c; c = c->next)
		n++;
	return n;
}

json_value_t *json_at(const json_value_t *arr, int index)
{
	if (!arr || arr->type != JSON_ARRAY || index < 0)
		return NULL;
	for (json_value_t *c = arr->first; c; c = c->next, index--)
		if (index == 0)
			return c;
	return NULL;
}

const char *json_str(const json_value_t *v, const char *fallback)
{
	return (v && v->type == JSON_STRING && v->string) ? v->string : fallback;
}

double json_num(const json_value_t *v, double fallback)
{
	return (v && v->type == JSON_NUMBER) ? v->number : fallback;
}

int json_int(const json_value_t *v, int fallback)
{
	return (v && v->type == JSON_NUMBER) ? (int)v->number : fallback;
}
