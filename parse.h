#ifndef PARSE_H____
#define PARSE_H____
#include "shell.h"

bool parse(const char **cpp, Line *line);
void free_line(Line *line);

#endif
