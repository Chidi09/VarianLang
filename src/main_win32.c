/* Windows-only LSP entry shim. On POSIX this whole file is empty so it does
 * not collide with the real main() in main.c. */
#ifdef _WIN32
#include "varian.h"
#include "lsp.h"

#include <stdio.h>
#include <string.h>

#include <fcntl.h>
#include <io.h>

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    /* Set stdin/stdout to binary mode so \r\n isn't doubled */
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    return lsp_main();
}
#endif
