
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_IP "127.0.0.1"
#define PORT 8080
#define BUFFER_SIZE 4096
#define MAX_FILE_SIZE (1024 * 1024)
#define HEADER_SIZE 512

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

static int recv_line(int fd, char *line, size_t capacity)
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
            line[used] = '\0';
            return 0;
        }

        line[used++] = c;
    }

    return -1;
}

static int connect_server(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr = {0};

    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, SERVER_IP, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server IP address\n");
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr,
                sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}

static int send_request(int fd, const char *request)
{
    return send_all(fd, request, strlen(request));
}

static int server_error(const char *line)
{
    if (strncmp(line, "ERR ", 4) == 0) {
        fprintf(stderr, "Atlas: %s\n", line + 4);
        return 1;
    }

    return 0;
}

static int command_list(void)
{
    int fd = connect_server();

    if (fd < 0)
        return 1;

    if (send_request(fd, "LIST\n") < 0) {
        perror("send");
        close(fd);
        return 1;
    }

    char line[HEADER_SIZE];

    for (;;) {
        if (recv_line(fd, line, sizeof(line)) < 0) {
            fprintf(stderr, "Connection closed during LIST\n");
            close(fd);
            return 1;
        }

        if (strcmp(line, "END") == 0)
            break;

        if (server_error(line)) {
            close(fd);
            return 1;
        }

        puts(line);
    }

    close(fd);
    return 0;
}

static int command_read(const char *remote_name,
                        const char *local_path)
{
    char request[HEADER_SIZE];

    int n = snprintf(request, sizeof(request),
                     "READ %s\n", remote_name);

    if (n < 0 || (size_t)n >= sizeof(request)) {
        fprintf(stderr, "Request too long\n");
        return 1;
    }

    int fd = connect_server();

    if (fd < 0)
        return 1;

    if (send_request(fd, request) < 0) {
        perror("send");
        close(fd);
        return 1;
    }

    char response[HEADER_SIZE];

    if (recv_line(fd, response, sizeof(response)) < 0) {
        fprintf(stderr, "No response from server\n");
        close(fd);
        return 1;
    }

    if (server_error(response)) {
        close(fd);
        return 1;
    }

    size_t length;
    char extra;

    if (sscanf(response, "OK %zu %c", &length, &extra) != 1 ||
        length > MAX_FILE_SIZE) {
        fprintf(stderr, "Invalid server response: %s\n", response);
        close(fd);
        return 1;
    }

    /*
     * O_EXCL prevents accidentally overwriting an existing
     * local file. Remove it yourself if you want to download
     * to that name again.
     */
    int out = open(local_path,
                   O_WRONLY | O_CREAT | O_EXCL,
                   0644);

    if (out < 0) {
        perror("creating local file");
        close(fd);
        return 1;
    }

    char buffer[BUFFER_SIZE];
    size_t remaining = length;
    int failed = 0;

    while (remaining > 0) {
        size_t chunk = remaining < sizeof(buffer)
                     ? remaining : sizeof(buffer);

        if (recv_exact(fd, buffer, chunk) < 0) {
            fprintf(stderr, "Incomplete download\n");
            failed = 1;
            break;
        }

        size_t written = 0;

        while (written < chunk) {
            ssize_t w = write(out, buffer + written,
                              chunk - written);

            if (w < 0) {
                if (errno == EINTR)
                    continue;

                perror("writing local file");
                failed = 1;
                break;
            }

            if (w == 0) {
                fprintf(stderr, "Local write failed\n");
                failed = 1;
                break;
            }

            written += (size_t)w;
        }

        if (failed)
            break;

        remaining -= chunk;
    }

    if (close(out) < 0) {
        perror("closing local file");
        failed = 1;
    }

    close(fd);

    if (failed) {
        unlink(local_path);
        return 1;
    }

    printf("Downloaded %s to %s (%zu bytes)\n",
           remote_name, local_path, length);

    return 0;
}

static int command_write(const char *remote_name,
                         const char *local_path)
{
    int input = open(local_path, O_RDONLY);

    if (input < 0) {
        perror("opening local file");
        return 1;
    }

    struct stat st;

    if (fstat(input, &st) < 0 ||
        !S_ISREG(st.st_mode) ||
        st.st_size < 0 ||
        (uintmax_t)st.st_size > MAX_FILE_SIZE) {
        fprintf(stderr, "Source must be a regular file "
                        "of at most %d bytes\n", MAX_FILE_SIZE);
        close(input);
        return 1;
    }

    size_t length = (size_t)st.st_size;
    char request[HEADER_SIZE];

    int n = snprintf(request, sizeof(request),
                     "WRITE %s %zu\n", remote_name, length);

    if (n < 0 || (size_t)n >= sizeof(request)) {
        fprintf(stderr, "Request too long\n");
        close(input);
        return 1;
    }

    int fd = connect_server();

    if (fd < 0) {
        close(input);
        return 1;
    }

    if (send_request(fd, request) < 0) {
        perror("sending request");
        close(input);
        close(fd);
        return 1;
    }

    char buffer[BUFFER_SIZE];
    size_t remaining = length;

    while (remaining > 0) {
        size_t wanted = remaining < sizeof(buffer)
                      ? remaining : sizeof(buffer);

        ssize_t r = read(input, buffer, wanted);

        if (r < 0) {
            if (errno == EINTR)
                continue;

            perror("reading local file");
            close(input);
            close(fd);
            return 1;
        }

        if (r == 0) {
            fprintf(stderr, "Local file changed or ended early\n");
            close(input);
            close(fd);
            return 1;
        }

        if (send_all(fd, buffer, (size_t)r) < 0) {
            perror("sending file data");
            close(input);
            close(fd);
            return 1;
        }

        remaining -= (size_t)r;
    }

    close(input);

    char response[HEADER_SIZE];

    if (recv_line(fd, response, sizeof(response)) < 0) {
        fprintf(stderr, "No final response; write status unknown\n");
        close(fd);
        return 1;
    }

    close(fd);

    if (server_error(response))
        return 1;

    if (strcmp(response, "OK") != 0) {
        fprintf(stderr, "Unexpected server response: %s\n",
                response);
        return 1;
    }

    printf("Uploaded %s as %s (%zu bytes)\n",
           local_path, remote_name, length);

    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s list\n"
        "  %s read  REMOTE_NAME LOCAL_PATH\n"
        "  %s write REMOTE_NAME LOCAL_PATH\n",
        program, program, program);
}

int main(int argc, char *argv[])
{
    if (argc == 2 && strcmp(argv[1], "list") == 0)
        return command_list();

    if (argc == 4 && strcmp(argv[1], "read") == 0)
        return command_read(argv[2], argv[3]);

    if (argc == 4 && strcmp(argv[1], "write") == 0)
        return command_write(argv[2], argv[3]);

    usage(argv[0]);
    return 1;
}
