#include "https_client.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "utils/debug/log.h"

static int parse_https_url(const char *url,
                           char *host, size_t host_len,
                           char *port, size_t port_len,
                           char *path, size_t path_len)
{
    const char *cursor;
    const char *host_end;
    const char *slash;
    const char *colon = NULL;
    size_t copy_len;

    if (strncmp(url, "https://", 8) != 0) {
        return -1;
    }

    cursor = url + 8;
    slash = strchr(cursor, '/');
    host_end = (slash != NULL) ? slash : (cursor + strlen(cursor));

    for (const char *p = cursor; p < host_end; p++) {
        if (*p == ':') {
            colon = p;
            break;
        }
    }

    copy_len = (size_t)((colon != NULL ? colon : host_end) - cursor);
    if ((copy_len == 0) || (copy_len >= host_len)) {
        return -1;
    }
    memcpy(host, cursor, copy_len);
    host[copy_len] = '\0';

    if (colon != NULL) {
        copy_len = (size_t)(host_end - colon - 1);
        if ((copy_len == 0) || (copy_len >= port_len)) {
            return -1;
        }
        memcpy(port, colon + 1, copy_len);
        port[copy_len] = '\0';
    } else {
        snprintf(port, port_len, "443");
    }

    if (slash != NULL) {
        copy_len = strlen(slash);
        if (copy_len >= path_len) {
            return -1;
        }
        memcpy(path, slash, copy_len + 1);
    } else {
        snprintf(path, path_len, "/");
    }

    return 0;
}

static int http_status_is_2xx(const char *response)
{
    const char *space;
    int status = 0;

    if (strncmp(response, "HTTP/", 5) != 0) {
        return 0;
    }

    space = strchr(response, ' ');
    if (space == NULL) {
        return 0;
    }

    if (sscanf(space + 1, "%d", &status) != 1) {
        return 0;
    }

    return (status >= 200) && (status <= 299);
}

int https_post_json(const char *url, const char *json)
{
    char host[128];
    char port[8];
    char path[192];
    char host_header[160];
    char request[768];
    char response[384];
    int request_len;
    int rc;
    int result = -1;
    int ssl_ready = 0;
    size_t written;
    size_t total;
    const unsigned char personalization[] = "ln882h-power-socket";
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;

    if ((url == NULL) || (json == NULL)) {
        return -1;
    }

    if (parse_https_url(url, host, sizeof(host), port, sizeof(port),
                        path, sizeof(path)) != 0) {
        LOG(LOG_LVL_ERROR, "HTTPS: invalid URL\r\n");
        return -1;
    }

    if (strcmp(port, "443") == 0) {
        snprintf(host_header, sizeof(host_header), "%s", host);
    } else {
        snprintf(host_header, sizeof(host_header), "%s:%s", host, port);
    }

    request_len = snprintf(request, sizeof(request),
                           "POST %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %u\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "%s",
                           path, host_header, (unsigned)strlen(json), json);
    if ((request_len < 0) || ((size_t)request_len >= sizeof(request))) {
        LOG(LOG_LVL_ERROR, "HTTPS: request too large\r\n");
        return -1;
    }

    mbedtls_net_init(&net);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    rc = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                               personalization, sizeof(personalization) - 1);
    if (rc != 0) {
        LOG(LOG_LVL_ERROR, "HTTPS: ctr_drbg_seed failed %d\r\n", rc);
        goto cleanup;
    }

    rc = mbedtls_ssl_config_defaults(&conf,
                                     MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        LOG(LOG_LVL_ERROR, "HTTPS: config_defaults failed %d\r\n", rc);
        goto cleanup;
    }

    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
    mbedtls_ssl_conf_read_timeout(&conf, POWER_SOCKET_HTTP_TIMEOUT_MS);

    rc = mbedtls_ssl_setup(&ssl, &conf);
    if (rc != 0) {
        LOG(LOG_LVL_ERROR, "HTTPS: ssl_setup failed %d\r\n", rc);
        goto cleanup;
    }

    rc = mbedtls_ssl_set_hostname(&ssl, host);
    if (rc != 0) {
        LOG(LOG_LVL_ERROR, "HTTPS: set_hostname failed %d\r\n", rc);
        goto cleanup;
    }

    LOG(LOG_LVL_INFO, "HTTPS: connecting to %s:%s\r\n", host, port);
    rc = mbedtls_net_connect(&net, host, port, MBEDTLS_NET_PROTO_TCP);
    if (rc != 0) {
        LOG(LOG_LVL_ERROR, "HTTPS: connect failed %d\r\n", rc);
        goto cleanup;
    }

    mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send,
                        mbedtls_net_recv, mbedtls_net_recv_timeout);

    while ((rc = mbedtls_ssl_handshake(&ssl)) != 0) {
        if ((rc != MBEDTLS_ERR_SSL_WANT_READ) &&
            (rc != MBEDTLS_ERR_SSL_WANT_WRITE)) {
            LOG(LOG_LVL_ERROR, "HTTPS: handshake failed %d\r\n", rc);
            goto cleanup;
        }
    }
    ssl_ready = 1;

    written = 0;
    while (written < (size_t)request_len) {
        rc = mbedtls_ssl_write(&ssl,
                               (const unsigned char *)request + written,
                               (size_t)request_len - written);
        if (rc > 0) {
            written += (size_t)rc;
            continue;
        }
        if ((rc == MBEDTLS_ERR_SSL_WANT_READ) ||
            (rc == MBEDTLS_ERR_SSL_WANT_WRITE)) {
            continue;
        }
        LOG(LOG_LVL_ERROR, "HTTPS: write failed %d\r\n", rc);
        goto cleanup;
    }

    total = 0;
    while (total < (sizeof(response) - 1)) {
        rc = mbedtls_ssl_read(&ssl,
                              (unsigned char *)response + total,
                              sizeof(response) - 1 - total);
        if (rc > 0) {
            total += (size_t)rc;
            response[total] = '\0';
            if (strstr(response, "\r\n\r\n") != NULL) {
                break;
            }
            continue;
        }
        if ((rc == MBEDTLS_ERR_SSL_WANT_READ) ||
            (rc == MBEDTLS_ERR_SSL_WANT_WRITE)) {
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_TIMEOUT) {
            break;
        }
        if (rc == 0) {
            break;
        }
        LOG(LOG_LVL_ERROR, "HTTPS: read failed %d\r\n", rc);
        break;
    }
    response[total] = '\0';

    if (total > 0) {
        LOG(LOG_LVL_INFO, "HTTPS response: %s\r\n", response);
        if (http_status_is_2xx(response)) {
            result = 0;
        } else {
            LOG(LOG_LVL_ERROR, "HTTPS: non-2xx response\r\n");
        }
    } else {
        LOG(LOG_LVL_ERROR, "HTTPS: empty response\r\n");
    }

cleanup:
    if (ssl_ready) {
        mbedtls_ssl_close_notify(&ssl);
    }
    mbedtls_net_free(&net);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    return result;
}
