
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
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

static int valid_filename(const char *name)
{
    size_t len = strlen(name);

    if (len == 0 || len > 200)
        return 0;

    for (const char *p = name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') ||
              (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') ||
              *p == '.' || *p == '_' || *p == '-'))
            return 0;
    }

    return strcmp(name, ".") != 0 &&
           strcmp(name, "..") != 0;
}

static void reply_error(int fd, const char *message)
{
    char response[256];
    int n = snprintf(response, sizeof(response),
                     "ERR %s\n", message);

    if (n > 0 && (size_t)n < sizeof(response))
        (void)send_all(fd, response, (size_t)n);
}

static int write_all(int fd, const void *data, size_t length)
{
    const char *p = data;
    size_t written = 0;

    while (written < length) {
        ssize_t n = write(fd, p + written, length - written);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (n == 0)
            return -1;

        written += (size_t)n;
    }

    return 0;
}

static int make_path(char *path, size_t capacity,
                     const char *filename)
{
    int n = snprintf(path, capacity, "%s%s",
                     STORAGE_DIR, filename);

    return n >= 0 && (size_t)n < capacity ? 0 : -1;
}

/* LIST: return stored regular filenames, followed by END. */
static void handle_list(int client_fd)
{
    DIR *dir = opendir(STORAGE_DIR);

    if (dir == NULL) {
        reply_error(client_fd, "cannot open storage directory");
        return;
    }

    struct dirent *entry;
    int failed = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;

        if (!valid_filename(entry->d_name))
            continue;

        char path[sizeof(STORAGE_DIR) + 201];
        struct stat st;

        if (make_path(path, sizeof(path), entry->d_name) < 0)
            continue;

        if (lstat(path, &st) < 0 || !S_ISREG(st.st_mode))
            continue;

        if (send_all(client_fd, entry->d_name,
                     strlen(entry->d_name)) < 0 ||
            send_all(client_fd, "\n", 1) < 0) {
            failed = 1;
            break;
        }
    }

    closedir(dir);

    if (!failed)
        (void)send_all(client_fd, "END\n", 4);
}

static void handle_read(int client_fd, const char *filename)
{
    char path[sizeof(STORAGE_DIR) + 201];
    char buffer[BUFFER_SIZE];

    if (!valid_filename(filename) ||
        make_path(path, sizeof(path), filename) < 0) {
        reply_error(client_fd, "invalid filename");
        return;
    }

    int file_fd = open(path, O_RDONLY);

    if (file_fd < 0) {
        reply_error(client_fd, "file not found");
        return;
    }

    struct stat st;

    if (fstat(file_fd, &st) < 0 ||
        !S_ISREG(st.st_mode) ||
        st.st_size < 0 ||
        (uintmax_t)st.st_size > MAX_FILE_SIZE) {
        close(file_fd);
        reply_error(client_fd, "invalid or oversized file");
        return;
    }

    size_t length = (size_t)st.st_size;
    char response[64];

    int n = snprintf(response, sizeof(response),
                     "OK %zu\n", length);

    if (n < 0 || (size_t)n >= sizeof(response) ||
        send_all(client_fd, response, (size_t)n) < 0) {
        close(file_fd);
        return;
    }

    size_t remaining = length;

    while (remaining > 0) {
        size_t wanted = remaining < sizeof(buffer)
                      ? remaining : sizeof(buffer);

        ssize_t r = read(file_fd, buffer, wanted);

        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (r == 0)
            break;

        if (send_all(client_fd, buffer, (size_t)r) < 0)
            break;

        remaining -= (size_t)r;
    }

    close(file_fd);
}

static void handle_write(int client_fd, const char *filename,
                         size_t length)
{
    char path[sizeof(STORAGE_DIR) + 201];
    char temp_path[sizeof(STORAGE_DIR) + 64];
    char buffer[BUFFER_SIZE];

    if (!valid_filename(filename) ||
        make_path(path, sizeof(path), filename) < 0) {
        reply_error(client_fd, "invalid filename");
        return;
    }

    if (length > MAX_FILE_SIZE) {
        reply_error(client_fd, "file too large");
        return;
    }

    int n = snprintf(temp_path, sizeof(temp_path),
                     "%s.atlas-tmp-XXXXXX", STORAGE_DIR);

    if (n < 0 || (size_t)n >= sizeof(temp_path)) {
        reply_error(client_fd, "temporary path too long");
        return;
    }

    int file_fd = mkstemp(temp_path);

    if (file_fd < 0) {
        reply_error(client_fd, "cannot create temporary file");
        return;
    }

    size_t remaining = length;
    int failed = 0;

    while (remaining > 0) {
        size_t chunk = remaining < sizeof(buffer)
                     ? remaining : sizeof(buffer);

        if (recv_exact(client_fd, buffer, chunk) < 0 ||
            write_all(file_fd, buffer, chunk) < 0) {
            failed = 1;
            break;
        }

        remaining -= chunk;
    }

    if (!failed && fsync(file_fd) < 0)
        failed = 1;

    if (close(file_fd) < 0)
        failed = 1;

    if (failed) {
        unlink(temp_path);
        reply_error(client_fd, "write failed or incomplete");
        return;
    }

    if (rename(temp_path, path) < 0) {
        unlink(temp_path);
        reply_error(client_fd, "cannot commit file");
        return;
    }

    int dir_fd = open(STORAGE_DIR, O_RDONLY | O_DIRECTORY);

    if (dir_fd < 0) {
        reply_error(client_fd,
                    "file committed; directory sync failed");
        return;
    }

    if (fsync(dir_fd) < 0) {
        close(dir_fd);
        reply_error(client_fd,
                    "file committed; directory sync failed");
        return;
    }

    close(dir_fd);
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

    close(server_fd);
    return 0;
}
