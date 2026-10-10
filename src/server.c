#define _POSIX_C_SOURCE 200809L

#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 8080
#define HEADER_SIZE 512
#define BUFFER_SIZE 4096

static int send_all(int fd, const void *data, size_t length)
{
    const char *p = data;
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(fd, p + sent,
                         length - sent, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (n == 0)
            return -1;

        sent += (size_t)n;
    }

    return 0;
}

static int recv_exact(int fd, void *data, size_t length)
{
    char *p = data;
    size_t received = 0;

    while (received < length) {
        ssize_t n = recv(fd, p + received,
                         length - received, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (n == 0)
            return -1;

        received += (size_t)n;
    }

    return 0;
}

static int recv_header(int fd, char *header, size_t capacity)
{
    size_t used = 0;

    while (used + 1 < capacity) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (n == 0)
            return -1;

        if (c == '\n') {
            header[used] = '\0';
            return 0;
        }

        header[used++] = c;
    }

    return -1;
}

static void reply_error(int fd, const char *message)
{
    char response[256];

    int n = snprintf(response, sizeof(response),
                     "ERR %s\n", message);

    if (n > 0 && (size_t)n < sizeof(response))
        (void)send_all(fd, response, (size_t)n);
}

/* LIST: delegate file discovery to the storage engine. */
static void handle_list(int client_fd)
{
    char **names = NULL;
    size_t count = 0;

    if (storage_list(&names, &count) < 0) {
        reply_error(client_fd, "cannot list storage");
        return;
    }

    int failed = 0;

    for (size_t i = 0; i < count; i++) {
        if (send_all(client_fd, names[i],
                     strlen(names[i])) < 0 ||
            send_all(client_fd, "\n", 1) < 0) {
            failed = 1;
            break;
        }
    }

    storage_free_list(names, count);

    if (!failed)
        (void)send_all(client_fd, "END\n", 4);
}

/* READ: storage engine loads the file; server sends it. */
static void handle_read(int client_fd, const char *filename)
{
    unsigned char *data = NULL;
    size_t length = 0;

    if (storage_read(filename, &data, &length) < 0) {
        reply_error(client_fd, "cannot read file");
        return;
    }

    char response[64];

    int n = snprintf(response, sizeof(response),
                     "OK %zu\n", length);

    if (n < 0 || (size_t)n >= sizeof(response) ||
        send_all(client_fd, response, (size_t)n) < 0) {
        free(data);
        return;
    }

    (void)send_all(client_fd, data, length);
    free(data);
}

/* WRITE: server receives bytes; storage engine saves them. */
static void handle_write(int client_fd, const char *filename,
                         size_t length)
{
    if (!storage_validate_name(filename)) {
        reply_error(client_fd, "invalid filename");
        return;
    }

    if (length > ATLAS_MAX_FILE_SIZE) {
        reply_error(client_fd, "file too large");
        return;
    }

    unsigned char *data = malloc(length ? length : 1);

    if (data == NULL) {
        reply_error(client_fd, "out of memory");
        return;
    }

    if (recv_exact(client_fd, data, length) < 0) {
        free(data);
        reply_error(client_fd, "incomplete file data");
        return;
    }

    if (storage_write(filename, data, length) < 0) {
        free(data);
        reply_error(client_fd, "cannot store file");
        return;
    }

    free(data);
    (void)send_all(client_fd, "OK\n", 3);
}

static void handle_client(int client_fd)
{
    char header[HEADER_SIZE];
    char filename[201];
    char extra;
    size_t length;
    int fields;

    if (recv_header(client_fd, header, sizeof(header)) < 0) {
        reply_error(client_fd, "invalid or incomplete header");
        return;
    }

    if (strcmp(header, "LIST") == 0) {
        handle_list(client_fd);
        return;
    }

    if (strncmp(header, "READ ", 5) == 0) {
        fields = sscanf(header + 5, "%200s %c",
                        filename, &extra);

        if (fields != 1) {
            reply_error(client_fd, "invalid READ header");
            return;
        }

        handle_read(client_fd, filename);
        return;
    }

    if (strncmp(header, "WRITE ", 6) == 0) {
        fields = sscanf(header + 6, "%200s %zu %c",
                        filename, &length, &extra);

        if (fields != 2) {
            reply_error(client_fd, "invalid WRITE header");
            return;
        }

        handle_write(client_fd, filename, length);
        return;
    }

    reply_error(client_fd, "unknown operation");
}

int main(void)
{
    if (storage_init() < 0) {
        perror("storage_init");
        return 1;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        return 1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&addr,
             sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 16) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("Atlas storage server listening on port %d\n", PORT);

    for (;;) {
        int client_fd = accept(server_fd, NULL, NULL);

        if (client_fd < 0) {
            if (errno == EINTR)
                continue;

            perror("accept");
            continue;
        }

        handle_client(client_fd);
        close(client_fd);
    }
}