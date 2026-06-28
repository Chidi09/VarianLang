#ifndef _SYS_WAIT_H_
#define _SYS_WAIT_H_

#define WNOHANG 1
#define WUNTRACED 2

#define WIFEXITED(s) (((s) & 0xFF) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s) (((s) & 0xFF) != 0 && ((s) & 0xFF) != 0x7F)
#define WTERMSIG(s) ((s) & 0x7F)
#define WIFSTOPPED(s) (((s) & 0xFF) == 0x7F)
#define WSTOPSIG(s) (((s) >> 8) & 0xFF)

#endif
