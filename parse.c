#include "parse.h"
#include "shell.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum {
  TOK_WORD,
  TOK_PIPE,
  TOK_REDIR_IN,
  TOK_REDIR_OUT,
  TOK_REDIR_ERR,
  TOK_AMP,
  TOK_EOF,
} TokenType;

typedef struct {
  TokenType type;
  char *text;
} Token;

typedef struct {
  const char *cur;
  Token lookahead;
  bool has_lookahead;
} Parser;

static bool is_redir(Token token);
static Token next_token(Parser *parser);
static Token peek_token(Parser *parser);
static bool parse_command(Parser *parser, Command *command);
static bool parse_pipeline(Parser *parser, Pipeline *pipeline);
static void free_redir(Redir *redir);
static void free_command(Command *command);
static void free_pipeline(Pipeline *pipeline);

bool parse(const char **cpp, Line *line) {
  Parser parser = {.cur = *cpp, .has_lookahead = false};
  return parse_pipeline(&parser, line);
}

void free_line(Line *line) { free_pipeline(line); }

static bool is_redir(Token token) {
  return token.type == TOK_REDIR_IN || token.type == TOK_REDIR_OUT ||
         token.type == TOK_REDIR_ERR;
}

static Token next_token(Parser *parser) {
  Token token = {.text = NULL};
  size_t len;

  if (parser->has_lookahead) {
    parser->has_lookahead = false;
    return parser->lookahead;
  }

  parser->cur += strspn(parser->cur, " \t\n");
  if (*parser->cur == '\0') {
    token.type = TOK_EOF;
    return token;
  }
  if (memcmp(parser->cur, "2>", 2) == 0) {
    token.type = TOK_REDIR_ERR;
    parser->cur += 2;
    return token;
  }
  if (*parser->cur == '<') {
    token.type = TOK_REDIR_IN;
    parser->cur++;
    return token;
  }
  if (*parser->cur == '>') {
    token.type = TOK_REDIR_OUT;
    parser->cur++;
    return token;
  }
  if (*parser->cur == '|') {
    token.type = TOK_PIPE;
    parser->cur++;
    return token;
  }
  if (*parser->cur == '&') {
    token.type = TOK_AMP;
    parser->cur++;
    return token;
  }

  token.type = TOK_WORD;
  len = strcspn(parser->cur, " \t\n<>|&");
  token.text = malloc(sizeof(*parser->cur) * (len + 1));
  memcpy(token.text, parser->cur, len);
  token.text[len] = '\0';
  parser->cur += len;
  return token;
}

static Token peek_token(Parser *parser) {
  if (!parser->has_lookahead) {
    parser->lookahead = next_token(parser);
    parser->has_lookahead = true;
  }
  return parser->lookahead;
}

static bool parse_command(Parser *parser, Command *command) {
  Token token;
  int argv_size = 0, redirv_size = 0;

  command->argc = 0;
  command->argv = NULL;
  command->redirc = 0;
  command->redirv = NULL;

  for (token = next_token(parser); token.type == TOK_WORD;
       token = next_token(parser)) {
    if (command->argc == 0) {
      argv_size = 2;
      if ((command->argv = malloc(sizeof(*command->argv) * argv_size)) ==
          NULL) {
        perror("malloc");
        exit(1);
      }
    } else if (command->argc + 1 >= argv_size) {
      argv_size *= 2;
      if ((command->argv = realloc(command->argv, sizeof(*command->argv) *
                                                      argv_size)) == NULL) {
        perror("realloc");
        exit(1);
      }
    }
    command->argv[command->argc] = token.text;
    command->argc++;
  }

  if (command->argc == 0) {
    fprintf(stderr, "syntax error: command must start with word\n");
    if (token.text != NULL)
      free(token.text);
    free(command->argv);
    parser->lookahead = token;
    parser->has_lookahead = true;
    return false;
  }
  command->argv[command->argc] = NULL;

  for (; is_redir(token); token = next_token(parser)) {
    Redir redir;

    switch (token.type) {
    case TOK_REDIR_IN: {
      redir.fd = STDIN_FILENO;
      break;
    }
    case TOK_REDIR_OUT: {
      redir.fd = STDOUT_FILENO;
      break;
    }
    case TOK_REDIR_ERR: {
      redir.fd = STDERR_FILENO;
      break;
    }
    default: {
      assert(false);
    }
    }

    if ((token = next_token(parser)).type != TOK_WORD) {
      fprintf(stderr, "syntax error: redirection requires a path\n");
      if (token.text != NULL)
        free(token.text);
      parser->lookahead = token;
      parser->has_lookahead = true;
      return false;
    }
    redir.path = token.text;

    if (command->redirc == 0) {
      redirv_size = 2;
      if ((command->redirv = malloc(sizeof(redir) * redirv_size)) == NULL) {
        perror("malloc");
        exit(1);
      }
    } else if (command->redirc >= redirv_size) {
      redirv_size *= 2;
      if ((command->redirv =
               realloc(command->redirv, sizeof(redir) * redirv_size)) == NULL) {
        perror("realloc");
        exit(1);
      }
    }

    command->redirv[command->redirc] = redir;
    command->redirc++;
  }

  parser->lookahead = token;
  parser->has_lookahead = true;
  return true;
}

static bool parse_pipeline(Parser *parser, Pipeline *pipeline) {
  Command cmd;
  int commandv_size = 0;

  pipeline->commandc = 0;
  pipeline->commandv = NULL;
  pipeline->is_background = false;

  if (peek_token(parser).type == TOK_EOF) {
    return true;
  }

  while (parse_command(parser, &cmd)) {
    Token lookahead;

    if (pipeline->commandc == 0) {
      commandv_size = 2;
      if ((pipeline->commandv = malloc(sizeof(cmd) * commandv_size)) == NULL) {
        perror("malloc");
        exit(1);
      }
    } else if (pipeline->commandc >= commandv_size) {
      commandv_size *= 2;
      if ((pipeline->commandv = realloc(pipeline->commandv,
                                        sizeof(cmd) * commandv_size)) == NULL) {
        perror("realloc");
        exit(1);
      }
    }
    pipeline->commandv[pipeline->commandc] = cmd;
    pipeline->commandc++;

    lookahead = peek_token(parser);
    if (lookahead.type == TOK_EOF)
      break;
    if (lookahead.type == TOK_AMP) {
      pipeline->is_background = true;
      next_token(parser);
      if (peek_token(parser).type == TOK_EOF)
        break;
      fprintf(stderr, "syntax error: cannot put token next to '&'\n");
      return false;
    }
    if (next_token(parser).type != TOK_PIPE) {
      fprintf(stderr, "syntax error: '|' or '&' must be next to command\n");
      return false;
    }
  }

  if (peek_token(parser).type == TOK_EOF)
    return true;

  fprintf(stderr, "syntax error: cannot put token next to '|'\n");
  return false;
}

static void free_redir(Redir *redir) { free(redir->path); }

static void free_command(Command *command) {
  int i;
  for (i = 0; i < command->argc; i++) {
    free(command->argv[i]);
  }
  free(command->argv);
  for (i = 0; i < command->redirc; i++) {
    free_redir(&command->redirv[i]);
  }
  free(command->redirv);
}

static void free_pipeline(Pipeline *pipeline) {
  int i;
  for (i = 0; i < pipeline->commandc; i++) {
    free_command(&pipeline->commandv[i]);
  }
  free(pipeline->commandv);
}
