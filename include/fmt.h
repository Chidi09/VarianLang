#ifndef FMT_H
#define FMT_H

#include <stddef.h>

char *fmt_format_source(const char *source, size_t size, int *out_pos_ret);
int fmt_format_file(const char *path);

#endif /* FMT_H */
