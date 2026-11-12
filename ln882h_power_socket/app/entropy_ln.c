#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ln_misc.h"
#include "mbedtls/entropy_poll.h"

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    size_t done = 0;
    uint8_t random_bytes[16];

    (void)data;

    while (done < len) {
        size_t chunk = len - done;

        if (!ln_generate_true_random_16bytes(random_bytes)) {
            *olen = done;
            return -1;
        }

        if (chunk > sizeof(random_bytes)) {
            chunk = sizeof(random_bytes);
        }

        memcpy(output + done, random_bytes, chunk);
        done += chunk;
    }

    *olen = done;
    return 0;
}
