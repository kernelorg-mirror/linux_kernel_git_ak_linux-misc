/* Simple expression parser */
%{
#include "util.h"
#include "util/debug.h"
#define IN_EXPR_Y 1
#include "expr.h"
#include <string.h>

#define MAXIDLEN 256
%}

%define api.pure full
%parse-param { double *final_val }
%parse-param { struct parse_ctx *ctx }
%parse-param { const char **pp }
%lex-param { const char **pp }

%union {
	double num;
	char id[MAXIDLEN+1];
}

%token <num> NUMBER
%token <id> ID
%left '|'
%left '^'
%left '&'
%left '-' '+'
%left '*' '/' '%'
%left NEG NOT
%type <num> expr

%{
static int expr_lex(YYSTYPE *res, const char **pp);

static void expr_error(double *final_val __maybe_unused,
		       struct parse_ctx *ctx __maybe_unused,
		       const char **pp __maybe_unused,
		       const char *s)
{
	pr_debug("%s", s);
}

static int lookup_id(struct parse_ctx *ctx, char *id, double *val)
{
	int i;

	for (i = 0; i < ctx->num_ids; i++) {
		if (!strcasecmp(ctx->ids[i].name, id)) {
			*val = ctx->ids[i].val;
			return 0;
		}
	}
	return -1;
}

%}
%%

all_expr: expr			{ *final_val = $1; }
	;

expr:	  NUMBER
	| ID			{ if (lookup_id(ctx, $1, &$$) < 0) {
					pr_debug("%s not found", $1);
					YYABORT;
				  }
				}
	| expr '+' expr		{ $$ = $1 + $3; }
	| expr '-' expr		{ $$ = $1 - $3; }
	| expr '*' expr		{ $$ = $1 * $3; }
	| expr '/' expr		{ if ($3 == 0) YYABORT; $$ = $1 / $3; }
	| expr '%' expr		{ if ((long)$3 == 0) YYABORT; $$ = (long)$1 % (long)$3; }
	| '-' expr %prec NEG	{ $$ = -$2; }
	| '(' expr ')'		{ $$ = $2; }
	;

%%

static int expr_symbol(YYSTYPE *res, const char *p, const char **pp)
{
	char *dst = res->id;
	const char *s = p;

	while (isalnum(*p) || *p == '_' || *p == '.') {
		if (p - s >= MAXIDLEN)
			return -1;
		*dst++ = *p++;
	}
	*dst = 0;
	*pp = p;
	return ID;
}

static int expr_lex(YYSTYPE *res, const char **pp)
{
	int tok;
	const char *s;
	const char *p = *pp;

	while (isspace(*p))
		p++;
	s = p;
	switch (*p++) {
	case 'a' ... 'z':
	case 'A' ... 'Z':
		return expr_symbol(res, p - 1, pp);
	case '0' ... '9': case '.':
		res->num = strtod(s, (char **)&p);
		tok = NUMBER;
		break;
	default:
		tok = *s;
		break;
	}
	*pp = p;
	return tok;
}

/* Caller must make sure id is allocated */
void expr_add_id(struct parse_ctx *ctx, const char *name, double val)
{
	int idx;
	assert(ctx->num_ids < MAX_PARSE_ID);
	idx = ctx->num_ids++;
	ctx->ids[idx].name = name;
	ctx->ids[idx].val = val;
}

void expr_ctx_init(struct parse_ctx *ctx)
{
	ctx->num_ids = 0;
}

int expr_find_other(const char *p, const char *one, const char **other)
{
	const char *orig = p;

	*other = NULL;
	for (;;) {
		YYSTYPE val;
		int tok = expr_lex(&val, &p);
		if (tok == 0)
			break;
		if (tok == ID && strcasecmp(one, val.id)) {
			if (*other) {
				pr_debug("More than one extra identifier in %s\n", orig);
				return -1;
			}
			*other = strdup(val.id);
			if (!*other)
				return -1;
		}
	}
	return 0;
}
