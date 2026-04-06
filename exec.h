#ifndef EXEC_H_
#define EXEC_H_
#include "shell.h"
int execute_line(Line line, const char *cmdline);
void init_signals(void);
void reap_jobs(void);
#endif
