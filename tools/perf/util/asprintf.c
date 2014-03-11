/* Replacement for asprintf as it's buggy in older glibc versions */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

int vasprintf(char **str, const char *fmt, va_list ap)
{
	char buf[1024];
	int len = vsnprintf(buf, sizeof buf, fmt, ap);

	*str = malloc(len + 1);
	if (!*str)
		return -1;
	strcpy(*str, buf);
	return len;
}

int asprintf(char **str, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = vasprintf(str, fmt, ap);
	va_end(ap);
	return ret;
}
