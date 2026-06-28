#include "varian.h"
#include "lsp.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
#ifdef _WIN32
    /* Set stdin/stdout to binary mode so \r\n isn't doubled */
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    return lsp_main();
}
