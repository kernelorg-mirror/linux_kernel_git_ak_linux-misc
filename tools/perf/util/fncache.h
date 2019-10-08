#ifndef _FCACHE_H
#define _FCACHE_H 1

unsigned shash(const unsigned char *s);
void update_fncache(const char *name, bool res);
bool lookup_fncache(const char *name, bool *res);

#endif
