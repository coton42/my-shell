#include "shell.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum { JOB_RUNNING, JOB_STOPPED, JOB_DONE } JobState;

typedef struct job {
  int id, pgid;
  int pidc;
  pid_t *pidv;
  JobState state;
  const char *cmd;
  struct job *prev, *next;
} Job;

extern char **environ;

static bool is_parent_builtin(const Command *command);
static int execute_builtin(const Command *command);
static bool apply_command_redirections(const Command *command);
static void close_pipe_fds(int (*pipes)[2], int count);
static int wait_for_children(const pid_t *pids, int count);
static int execute_pipeline(const Pipeline *pipeline, const char *cmdline);
static const char *resolve_command_path(const char *command_name);
static void append_job(Job *job);
static void remove_job(Job *job);
static Job *search_job_by_id(int id);
static int gen_job_id();
static Job *resolve_job_arg(const Command *command);
static void sigchld_handler(int sig);

static volatile sig_atomic_t sigchld_pending = 0;

Job jobs_head = {.id = 0,
                 .pgid = 0,
                 .pidc = 0,
                 .pidv = NULL,
                 .state = JOB_DONE,
                 .cmd = NULL,
                 .prev = &jobs_head,
                 .next = &jobs_head};

int execute_line(Line line, const char *cmdline) {
  const char *cmdlinedup;

  if (line.commandc == 0)
    return 0;
  if (line.commandc == 1 && is_parent_builtin(&line.commandv[0]))
    return execute_builtin(&line.commandv[0]);
  if ((cmdlinedup = strdup(cmdline)) == NULL) {
    perror("strdup");
    return 1;
  }
  return execute_pipeline(&line, cmdlinedup);
}

void init_signals(void) {
  struct sigaction sig = {.sa_handler = sigchld_handler, .sa_flags = 0};
  struct sigaction ign = {.sa_handler = SIG_IGN, .sa_flags = 0};
  sigemptyset(&sig.sa_mask);
  sigemptyset(&ign.sa_mask);
  sigaction(SIGCHLD, &sig, NULL);
  sigaction(SIGINT, &ign, NULL);
  sigaction(SIGTSTP, &ign, NULL);
  sigaction(SIGTTIN, &ign, NULL);
  sigaction(SIGTTOU, &ign, NULL);
}

void reap_jobs(void) {
  int i, pid, status;
  Job *job, *next;
  bool all_done, any_stopped;

  if (!sigchld_pending)
    return;

  for (job = jobs_head.next; job != &jobs_head; job = next) {
    next = job->next;
    any_stopped = false;

    while ((pid = waitpid(-job->pgid, &status, WNOHANG | WUNTRACED)) > 0) {
      if (WIFSTOPPED(status)) {
        any_stopped = true;
      } else {
        for (i = 0; i < job->pidc; i++) {
          if (job->pidv[i] == pid) {
            job->pidv[i] = 0;
            break;
          }
        }
      }
    }

    sigchld_pending = 0;

    if (any_stopped) {
      job->state = JOB_STOPPED;
      continue;
    }

    all_done = true;
    for (i = 0; i < job->pidc; i++) {
      if (job->pidv[i] > 0) {
        all_done = false;
        break;
      }
    }

    if (all_done) {
      fprintf(stderr, "Job %d '%s' has ended\n", job->id, job->cmd);
      remove_job(job);
    }
  }
}

static bool is_parent_builtin(const Command *command) {
  return command->argc > 0 && (strcmp(command->argv[0], "exit") == 0 ||
                               strcmp(command->argv[0], "cd") == 0 ||
                               strcmp(command->argv[0], "pwd") == 0 ||
                               strcmp(command->argv[0], "jobs") == 0 ||
                               strcmp(command->argv[0], "fg") == 0 ||
                               strcmp(command->argv[0], "bg") == 0);
}

static int execute_builtin(const Command *command) {
  if (strcmp(command->argv[0], "exit") == 0) {
    exit(0);
  }

  if (strcmp(command->argv[0], "cd") == 0) {
    const char *path;

    if (command->argc < 2) {
      if ((path = getenv("HOME")) == NULL)
        return 1;
    } else
      path = command->argv[1];

    if (chdir(path) < 0) {
      perror("chdir");
      return 1;
    }
    return 0;
  }

  if (strcmp(command->argv[0], "pwd") == 0) {
    char wd_path[PATH_MAX];

    if ((getcwd(wd_path, sizeof(wd_path))) == NULL) {
      perror("getcwd");
      return 1;
    }
    puts(wd_path);
    return 0;
  }

  if (strcmp(command->argv[0], "jobs") == 0) {
    Job *job;

    if (jobs_head.next == &jobs_head) {
      printf("jobs: There are no jobs\n");
      return 0;
    }

    printf("Job     Group   State   Command\n");
    for (job = jobs_head.next; job != &jobs_head; job = job->next) {
      const char *state;
      switch (job->state) {
      case JOB_RUNNING: {
        state = "running";
        break;
      }
      case JOB_STOPPED: {
        state = "stopped";
        break;
      }
      default:
        assert(false);
      }
      printf("%-8d%-8d%-8s%s\n", job->id, job->pgid, state, job->cmd);
    }
    return 0;
  }

  if (strcmp(command->argv[0], "bg") == 0) {
    Job *job;
    if ((job = resolve_job_arg(command)) == NULL)
      return 1;
    if (kill(-job->pgid, SIGCONT) < 0) {
      perror("kill");
      return 1;
    }
    job->state = JOB_RUNNING;
    printf("Send job %d '%s' to background\n", job->id, job->cmd);
    return 0;
  }

  if (strcmp(command->argv[0], "fg") == 0) {
    Job *job;
    int i, pid, status, fg_status;
    pid_t last_pid;
    bool all_done;
    sigset_t mask, old_mask;
    if ((job = resolve_job_arg(command)) == NULL)
      return 1;

    // フォアグラウンドタスクが SIGCHLD で wait されないように一時マスク
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, &old_mask);

    last_pid = job->pidv[job->pidc - 1];

    // ターミナルをコマンドへ委譲してから再開して待機
    tcsetpgrp(STDIN_FILENO, job->pgid);
    if (kill(-job->pgid, SIGCONT) < 0) {
      perror("kill");
      tcsetpgrp(STDIN_FILENO, getpgrp());
      sigprocmask(SIG_SETMASK, &old_mask, NULL);
      return 1;
    }

    // プロセスグループ全体を待つ。最終プロセス終了 or 任意プロセス停止で抜ける
    fg_status = 0;
    while (1) {
      pid = waitpid(-(job->pgid), &status, WUNTRACED);
      if (pid < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      if (WIFSTOPPED(status)) {
        fg_status = status;
        break;
      }
      for (i = 0; i < job->pidc; i++) {
        if (job->pidv[i] == pid) {
          job->pidv[i] = 0;
          break;
        }
      }
      if (pid == last_pid) {
        fg_status = status;
        break;
      }
    }

    if (WIFSTOPPED(fg_status)) {
      job->state = JOB_STOPPED;
    } else {
      // 中間プロセスがまだ残っている場合はジョブを残す（reap_jobs で回収）
      all_done = true;
      for (i = 0; i < job->pidc; i++) {
        if (job->pidv[i] > 0) {
          all_done = false;
          break;
        }
      }
      if (all_done)
        remove_job(job);
    }

    // ターミナルを取り戻す
    tcsetpgrp(STDIN_FILENO, getpgrp());

    // SIGCHLDをUNBLOCK
    sigprocmask(SIG_SETMASK, &old_mask, NULL);

    return WIFSTOPPED(fg_status)  ? 0
           : WIFEXITED(fg_status) ? WEXITSTATUS(fg_status)
                                  : 1;
  }

  assert(false);
}

static bool apply_command_redirections(const Command *command) {
  int i, fd;

  for (i = 0; i < command->redirc; i++) {
    const Redir *rp = &command->redirv[i];

    if (rp->fd == STDIN_FILENO) {
      if ((fd = open(rp->path, O_RDONLY)) < 0) {
        perror("open");
        return false;
      }
    } else {
      if ((fd = open(rp->path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
        perror("open");
        return false;
      }
    }
    dup2(fd, rp->fd);
    close(fd);
  }

  return true;
}

static void close_pipe_fds(int (*pipes)[2], int count) {
  int i;

  for (i = 0; i < count; i++) {
    close(pipes[i][0]);
    close(pipes[i][1]);
  }
}

static int wait_for_children(const pid_t *pids, int count) {
  int i, status, last_status;

  last_status = 1;
  for (i = 0; i < count; i++) {
    int ret;
    if (pids[i] <= 0)
      continue;
    do {
      ret = waitpid(pids[i], &status, WUNTRACED);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
      perror("waitpid");
      continue;
    }
    if (i == count - 1)
      last_status = status;
  }

  return last_status;
}

static int execute_pipeline(const Pipeline *pipeline, const char *cmdline) {
  pid_t *pids = NULL, pgid = 0;
  int i, (*pipes)[2], status;
  int command_count = pipeline->commandc, pipe_count = command_count - 1;
  int pids_size = 0;
  sigset_t mask, old_mask;
  Job *new_job_ptr;

  pipes = NULL;
  if (pipe_count > 0) {
    if ((pipes = malloc(sizeof(*pipes) * pipe_count)) == NULL) {
      perror("malloc");
      exit(1);
    }
  }

  for (i = 0; i < pipe_count; i++) {
    if (pipe(pipes[i]) < 0) {
      perror("pipe");
      free(pipes);
      exit(1);
    }
  }

  // フォアグラウンドタスクが SIGCHID で wait されないように一時マスク
  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);
  sigprocmask(SIG_BLOCK, &mask, &old_mask);

  for (i = 0; i < command_count; i++) {
    const Command *command = &pipeline->commandv[i];
    const char *path;
    int in_fd = i == 0 ? STDIN_FILENO : pipes[i - 1][0];
    int out_fd = i == command_count - 1 ? STDOUT_FILENO : pipes[i][1];

    if (i == 0) {
      pids_size = 2;
      if ((pids = malloc(sizeof(*pids) * pids_size)) == NULL) {
        perror("malloc");
        exit(1);
      }
    } else if (i >= pids_size) {
      pids_size *= 2;
      if ((pids = realloc(pids, sizeof(*pids) * pids_size)) == NULL) {
        perror("realloc");
        exit(1);
      }
    }

    path = resolve_command_path(command->argv[0]);
    if (path == NULL) {
      fprintf(stderr, "%s: command not found\n", command->argv[0]);
      pids[i] = -1;
      continue;
    } else if (access(path, X_OK) < 0) {
      fprintf(stderr, "%s: file not executable\n", path);
      free((void *)path);
      pids[i] = -1;
      continue;
    }

    pids[i] = fork();
    if (pids[i] == 0) {
      // 子プロセス
      if (setpgid(0, pgid) < 0)
        perror("setpgid");

      sigprocmask(SIG_SETMASK, &old_mask, NULL);
      signal(SIGINT, SIG_DFL);
      signal(SIGTSTP, SIG_DFL);
      signal(SIGTTIN, SIG_DFL);
      signal(SIGTTOU, SIG_DFL);

      if (in_fd != STDIN_FILENO && dup2(in_fd, STDIN_FILENO) < 0) {
        perror("dup2");
        exit(1);
      }
      if (out_fd != STDOUT_FILENO && dup2(out_fd, STDOUT_FILENO) < 0) {
        perror("dup2");
        exit(1);
      }

      close_pipe_fds(pipes, pipe_count);

      if (!apply_command_redirections(command))
        exit(1);

      execve(path, command->argv, environ);

      perror("execve");
      exit(1);
    } else if (pids[i] < 0) {
      perror("fork");
      free((void *)path);
      close_pipe_fds(pipes, pipe_count);
      free(pipes);
      free(pids);
      exit(1);
    }

    if (pgid <= 0)
      pgid = pids[i];
    if (setpgid(pids[i], pgid) < 0 && errno != EACCES)
      perror("setpgid");

    free((void *)path);
  }

  close_pipe_fds(pipes, pipe_count);

  if (pgid == 0) {
    free(pipes);
    free(pids);
    free((void *)cmdline);
    sigprocmask(SIG_SETMASK, &old_mask, NULL);
    return 1;
  }

  if ((new_job_ptr = malloc(sizeof(*new_job_ptr))) == NULL) {
    perror("malloc");
    exit(1);
  }

  new_job_ptr->id = gen_job_id();
  new_job_ptr->pgid = pgid;
  new_job_ptr->pidc = command_count;
  new_job_ptr->pidv = pids;
  new_job_ptr->cmd = cmdline;
  new_job_ptr->state = JOB_RUNNING;

  append_job(new_job_ptr);

  if (pipeline->is_background) {
    sigprocmask(SIG_SETMASK, &old_mask, NULL);
    free(pipes);
    return 0;
  }

  // ターミナルをコマンドへ委譲してから待機
  if (tcsetpgrp(STDIN_FILENO, pgid) < 0)
    perror("tcsetpgrp");
  status = wait_for_children(new_job_ptr->pidv, new_job_ptr->pidc);

  if (WIFSTOPPED(status))
    new_job_ptr->state = JOB_STOPPED;
  else
    remove_job(new_job_ptr);

  // ターミナルを取り戻す（これはbgから呼ばれるためSIGTTOUが飛ぶ）
  if (tcsetpgrp(STDIN_FILENO, getpgrp()) < 0)
    perror("tcsetpgrp");

  // SIGCHLDをUNBLOCK
  sigprocmask(SIG_SETMASK, &old_mask, NULL);
  free(pipes);

  return WIFSTOPPED(status) ? 0 : WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static const char *resolve_command_path(const char *command_name) {
  char *path_env, *path_env_cpy, *dir, *save_ptr;
  int command_name_len;

  if (strchr(command_name, '/') != NULL) {
    // コマンドにスラッシュが含まれている場合はサーチしない
    char *command_name_copy;

    if (access(command_name, F_OK) < 0)
      return NULL;
    if ((command_name_copy = strdup(command_name)) == NULL) {
      perror("strdup");
      exit(1);
    }
    return command_name_copy;
  }

  path_env = getenv("PATH");
  if (path_env == NULL || *path_env == '\0')
    return NULL;

  if ((path_env_cpy = strdup(path_env)) == NULL) {
    perror("strdup");
    exit(1);
  }

  command_name_len = strlen(command_name);
  for (dir = strtok_r(path_env_cpy, ":", &save_ptr); dir != NULL;
       dir = strtok_r(NULL, ":", &save_ptr)) {
    char *candidate;
    int dir_len;

    dir_len = strlen(dir);
    candidate = malloc(dir_len + 1 + command_name_len + 1);
    if (candidate == NULL) {
      perror("malloc");
      exit(1);
    }

    memcpy(candidate, dir, dir_len);
    candidate[dir_len] = '/';
    memcpy(candidate + dir_len + 1, command_name, command_name_len + 1);

    if (access(candidate, X_OK) == 0) {
      free(path_env_cpy);
      return candidate;
    }

    free(candidate);
  }

  free(path_env_cpy);
  return NULL;
}

static void append_job(Job *job) {
  job->prev = jobs_head.prev;
  job->next = &jobs_head;
  jobs_head.prev->next = job;
  jobs_head.prev = job;
}

static void remove_job(Job *job) {
  job->prev->next = job->next;
  job->next->prev = job->prev;
  free(job->pidv);
  free((void *)job->cmd);
  free(job);
}

static Job *search_job_by_id(int id) {
  Job *job;
  for (job = jobs_head.next; job != &jobs_head; job = job->next)
    if (job->id == id)
      return job;
  return NULL;
}

static int gen_job_id(void) {
  if (jobs_head.prev == &jobs_head)
    return 1;
  return jobs_head.prev->id + 1;
}

static Job *resolve_job_arg(const Command *command) {
  int id;
  const char *arg;
  Job *job;

  if (command->argc < 2) {
    if (jobs_head.next == &jobs_head) {
      fprintf(stderr, "%s: no current job\n", command->argv[0]);
      return NULL;
    }
    for (job = jobs_head.prev; job != &jobs_head; job = job->prev)
      if (job->state == JOB_STOPPED)
        return job;
    return jobs_head.prev;
  }

  arg = command->argv[1];
  id = arg[0] == '%' ? atoi(&arg[1]) : atoi(arg);

  if (id <= 0) {
    fprintf(stderr, "%s: %s: invalid argument\n", command->argv[0], arg);
    return NULL;
  }

  if ((job = search_job_by_id(id)) == NULL) {
    fprintf(stderr, "%s: %%%d: no such job\n", command->argv[0], id);
    return NULL;
  }

  return job;
}

static void sigchld_handler(int sig) {
  (void)sig;
  sigchld_pending = 1;
}
