#include "util/debug.h"
#include "util/expr.h"
#include "tests.h"

static int test(struct parse_ctx *ctx, const char *e, double val2)
{
	double val;

	if (expr_parse(&val, ctx, &e))
		TEST_ASSERT_VAL("parse test failed", 0);
	TEST_ASSERT_VAL("unexpected value", val == val2);
	return 0;
}

int test__expr(int subtest __maybe_unused)
{
	const char *p;
	double val;
	int ret;
	struct parse_ctx ctx;

	expr_ctx_init(&ctx);
	expr_add_id(&ctx, "FOO", 1);
	expr_add_id(&ctx, "BAR", 2);

	ret = test(&ctx, "1+1", 2);
	ret |= test(&ctx, "FOO+BAR", 3);
	ret |= test(&ctx, "(BAR/2)%2", 1);
	ret |= test(&ctx, "1 - -4",  5);
	ret |= test(&ctx, "(FOO-1)*2 + (BAR/2)%2 - -4",  5);

	if (ret)
		return ret;

	p = "FOO/0";
	ret = expr_parse(&val, &ctx, &p);
	TEST_ASSERT_VAL("division by zero", ret == 1);

	p = "BAR/";
	ret = expr_parse(&val, &ctx, &p);
	TEST_ASSERT_VAL("missing operand", ret == 1);

	TEST_ASSERT_VAL("find other", expr_find_other("FOO + BAR", "FOO", &p) == 0);
	TEST_ASSERT_VAL("find other", !strcmp(p, "BAR"));
	free((void *)p);

	return 0;
}
