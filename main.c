#include "exec.h"
#include "parse.h"
#include "shell.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

int main() {
  char *buf = NULL;
  size_t buf_size = 0;
  ssize_t read_len;

  init_signals();

  while (true) {
    Line line;

    printf("$ ");
    fflush(stdout);

    if ((read_len = getline(&buf, &buf_size, stdin)) < 0) {
      if (errno != EINTR)
        exit(0);
      clearerr(stdin);
      reap_jobs();
      printf("\n");
      continue;
    }

    if (buf[read_len - 1] == '\n')
      buf[read_len - 1] = '\0';

    if (parse((const char **)&buf, &line))
      execute_line(line, buf);

    free_line(&line);
    reap_jobs();
  }
}
