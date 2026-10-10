
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>

#define PORT 8080
#define HEADER_SIZE 512
#define BUFFER_SIZE 4096
#define MAX_FILE_SIZE (1024 * 1024)
#define STORAGE_DIR "storage/"

static int send_all(int fd, const void *data, size_t length)
{
    const char *p = data;
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(fd, p + sent, length - sent, 0);

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
        ssize_t n = recv(fd, p + received, length - received, 0);

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

/* Read a newline-terminated header, one byte at a time. */
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

static int valid_filename(const char *name)
{
    if (*name == '\0' || strlen(name) > 200)
        return 0;

    for (const char *p = name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') ||
              (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') ||
              *p == '.' || *p == '_' || *p == '-'))
            return 0;
    }

    if (strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0)
        return 0;

    return 1;
}

static void reply_error(int fd, const char *message)
{
    char response[256];
    int n = snprintf(response, sizeof(response),
                     "ERR %s\n", message);

    if (n > 0 && (size_t)n < sizeof(response))
        send_all(fd, response, (size_t)n);
}

static void handle_client(int client_fd)
{
    char header[HEADER_SIZE];
    char filename[201];
    char path[sizeof(STORAGE_DIR) + sizeof(filename)];
    char buffer[BUFFER_SIZE];

    if (recv_header(client_fd, header, sizeof(header)) < 0) {
        reply_error(client_fd, "invalid header");
        return;
    }

    size_t length;
    int fields;

    if (strncmp(header, "READ ", 5) == 0) {
        fields = sscanf(header + 5, "%200s", filename);

        if (fields != 1 || !valid_filename(filename)) {
            reply_error(client_fd, "invalid filename");
            return;
        }

        snprintf(path, sizeof(path), "%s%s",
                 STORAGE_DIR, filename);

        int file_fd = open(path, O_RDONLY);
        if (file_fd < 0) {
            reply_error(client_fd, "file not found");
            return;
        }

        struct stat st;
        if (fstat(file_fd, &st) < 0 || !S_ISREG(st.st_mode) ||
            st.st_size < 0 ||
            (uintmax_t)st.st_size > MAX_FILE_SIZE) {
            close(file_fd);
            reply_error(client_fd, "invalid or oversized file");
            return;
        }

        length = (size_t)st.st_size;

        char response[64];
        int n = snprintf(response, sizeof(response),
                         "OK %zu\n", length);

        if (n < 0 || (size_t)n >= sizeof(response) ||
            send_all(client_fd, response, (size_t)n) < 0) {
            close(file_fd);
            return;
        }

        for (;;) {
            ssize_t r = read(file_fd, buffer, sizeof(buffer));

            if (r < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }

            if (r == 0)
                break;

            if (send_all(client_fd, buffer, (size_t)r) < 0)
                break;
        }

        close(file_fd);
        return;
    }

    if (strncmp(header, "WRITE ", 6) == 0) {
        char extra;
        fields = sscanf(header + 6, "%200s %zu %c",
                        filename, &length, &extra);

        if (fields != 2 || !valid_filename(filename)) {
            reply_error(client_fd, "invalid WRITE header");
            return;
        }

        if (length > MAX_FILE_SIZE) {
            reply_error(client_fd, "file too large");
            return;
        }

        snprintf(path, sizeof(path), "%s%s",
                 STORAGE_DIR, filename);

        /*
         * O_NOFOLLOW prevents opening a symbolic link as the
         * destination on systems that support this flag.
         */
        int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif

        int file_fd = open(path, flags, 0644);
        if (file_fd < 0) {
            reply_error(client_fd, "cannot open destination");
            return;
        }

        size_t remaining = length;
        while (remaining > 0) {
            size_t chunk = remaining < sizeof(buffer)
                         ? remaining : sizeof(buffer);

            if (recv_exact(client_fd, buffer, chunk) < 0) {
                close(file_fd);
                reply_error(client_fd, "incomplete file data");
                return;
            }

            size_t written = 0;
            while (written < chunk) {
                ssize_t w = write(file_fd, buffer + written,
                                  chunk - written);

                if (w < 0) {
                    if (errno == EINTR)
                        continue;
                    close(file_fd);
                    reply_error(client_fd, "write failed");
                    return;
                }

                if (w == 0) {
                    close(file_fd);
                    reply_error(client_fd, "write failed");
                    return;
                }

                written += (size_t)w;
            }

            remaining -= chunk;
        }

        if (close(file_fd) < 0) {
            reply_error(client_fd, "close failed");
            return;
        }

        send_all(client_fd, "OK\n", 3);
        return;
    }

    reply_error(client_fd, "unknown operation");
}

int main(void)
{
    if (mkdir("storage", 0755) < 0 && errno != EEXIST) {
        perror("mkdir storage");
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
