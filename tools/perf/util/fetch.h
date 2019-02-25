#ifndef FETCH_H
#define FETCH_H 1

int fetch_exe(u64 ip, struct thread *thread, struct machine *machine,
	      char *buf, int len, bool *is64bit);

#endif
