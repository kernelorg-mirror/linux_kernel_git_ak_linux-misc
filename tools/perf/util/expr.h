#ifndef PARSE_CTX_H
#define PARSE_CTX_H 1

#define MAX_PARSE_ID 2

struct parse_id {
	const char *name;
	double val;
};

struct parse_ctx {
	int num_ids;
	struct parse_id ids[MAX_PARSE_ID];
};

void expr_ctx_init(struct parse_ctx *ctx);
void expr_add_id(struct parse_ctx *ctx, const char *id, double val);
#ifndef IN_EXPR_Y
int expr_parse(double *final_val, struct parse_ctx *ctx, const char **pp);
#endif
int expr_find_other(const char *p, const char *one, const char **other);

#endif
