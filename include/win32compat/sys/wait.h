#pragma once

#ifdef _WIN32
#define WNOHANG 1
#define WUNTRACED 2

#define WIFEXITED(s) (((s) & 0xFF) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s) (((s) & 0xFF) != 0 && ((s) & 0xFF) != 0x7F)
#define WTERMSIG(s) ((s) & 0x7F)
#define WIFSTOPPED(s) (((s) & 0xFF) == 0x7F)
#define WSTOPSIG(s) (((s) >> 8) & 0xFF)
#else
/* This compatibility header is found first because Varian's include directory
 * precedes the system paths. Unix builds must use the platform declaration and
 * status macros, including the real waitpid prototype. */
#include_next <sys/wait.h>
#endif
