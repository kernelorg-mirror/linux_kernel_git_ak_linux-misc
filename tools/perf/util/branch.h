#ifndef _BRANCH_H
#define _BRANCH_H 1
struct option;
int parse_branch_stack(const struct option *opt, const char *str, int unset);
#endif
