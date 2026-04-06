#ifndef SHELL_H_
#define SHELL_H_
#include <stdbool.h>

typedef struct {
  int fd;
  char *path;
} Redir;

typedef struct {
  char **argv;
  int argc;
  Redir *redirv;
  int redirc;
} Command;

typedef struct {
  Command *commandv;
  int commandc;
  bool is_background;
} Pipeline;

typedef Pipeline Line;

#endif
