/* mqtt_broker_esp Berry port glue.
 *
 * Trimmed copy of berry/default/be_port.c — POSIX stdio path only, no
 * FatFs / MSVC branches. Directory functions are omitted because the
 * OS module is disabled (BE_USE_OS_MODULE=0); if a future feature ever
 * turns OS back on, restore the dir block from upstream default/be_port.c.
 *
 * On ESP-IDF, stdio fopen() succeeds against any mounted VFS path. We do
 * not mount one yet, so be_fopen() will simply return NULL until a real
 * filesystem partition is added — which is fine because no current code
 * path calls open() from Berry.
 */
#include "berry.h"
#include "be_mem.h"
#include "be_sys.h"
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

/* Forward declaration of the runtime-side stdout sink (port/berry_runtime.c).
 * be_writebuffer() routes script output to the runtime ring buffer + WS
 * stream defined in main/berry_runtime.c. If the runtime hasn't been
 * initialized yet (e.g. early boot logging), we fall back to ESP_LOGI.
 */
extern void berry_port_stdout_write(const char *buffer, size_t length);

BERRY_API void be_writebuffer(const char *buffer, size_t length)
{
    berry_port_stdout_write(buffer, length);
}

BERRY_API char* be_readstring(char *buffer, size_t size)
{
    /* No interactive stdin on the device. Return NULL to signal EOF;
     * the REPL is driven via /berry/eval HTTP/WS, not stdin. */
    (void)buffer;
    (void)size;
    return NULL;
}

/* --- File system glue (stdio over VFS) --- */

void* be_fopen(const char *filename, const char *modes)
{
    return fopen(filename, modes);
}

int be_fclose(void *hfile)
{
    return fclose((FILE *)hfile);
}

size_t be_fwrite(void *hfile, const void *buffer, size_t length)
{
    return fwrite(buffer, 1, length, (FILE *)hfile);
}

size_t be_fread(void *hfile, void *buffer, size_t length)
{
    return fread(buffer, 1, length, (FILE *)hfile);
}

char* be_fgets(void *hfile, void *buffer, int size)
{
    return fgets(buffer, size, (FILE *)hfile);
}

int be_fseek(void *hfile, long offset)
{
    return fseek((FILE *)hfile, offset, SEEK_SET);
}

long int be_ftell(void *hfile)
{
    return ftell((FILE *)hfile);
}

long int be_fflush(void *hfile)
{
    return fflush((FILE *)hfile);
}

size_t be_fsize(void *hfile)
{
    long int size, offset = be_ftell(hfile);
    fseek((FILE *)hfile, 0L, SEEK_END);
    size = ftell((FILE *)hfile);
    fseek((FILE *)hfile, offset, SEEK_SET);
    return (size_t)size;
}
