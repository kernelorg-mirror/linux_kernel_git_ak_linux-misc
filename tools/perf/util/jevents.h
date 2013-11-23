#ifndef JEVENTS_H
#define JEVENTS_H 1

int json_events(const char *fn,
		int (*func)(void *data, char *name, char *event, char *desc),
		void *data);

#endif
