// client.c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/socket.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 9090
#define LINE_SIZE 1024
#define BUF_SIZE 4096

ssize_t safe_send(int fd, const void *buf, size_t len) {
    size_t sent = 0;
    const char *p = buf;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += n;
    }
    return sent;
}

// Thread: read from stdin, send to server
void* stdin_to_server(void* arg) {
    int sock = *(int*)arg;
    char line[LINE_SIZE];
    while (fgets(line, sizeof(line), stdin)) {
        if (strcmp(line, "/quit\n") == 0 || strcmp(line, "/quit\r\n") == 0) {
            // send /quit to server then close
            safe_send(sock, "/quit\n", 6);
            shutdown(sock, SHUT_RDWR);
            break;
        }
        // If user types a name without NICK but it's first message, allow direct nick
        // But we keep simple: user should type "NICK <name>" or first prompt will be shown by server.
        safe_send(sock, line, strlen(line));
    }
    return NULL;
}

// Thread: read from server, print to stdout
void* server_to_stdout(void* arg) {
    int sock = *(int*)arg;
    char buf[BUF_SIZE];
    ssize_t n;
    while ((n = read(sock, buf, sizeof(buf))) > 0) {
        ssize_t w = write(STDOUT_FILENO, buf, n);
        (void)w;
    }
    // server closed or error
    return NULL;
}

int main(int argc, char *argv[]) {
    int sock;
    struct sockaddr_in serv_addr;
    pthread_t t1, t2;
    const char *server_ip = SERVER_IP;
    int port = SERVER_PORT;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); exit(EXIT_FAILURE); }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &serv_addr.sin_addr) <= 0) {
        perror("inet_pton"); exit(EXIT_FAILURE);
    }

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect"); exit(EXIT_FAILURE);
    }

    // print server greeting (non-blocking read could be used, but simple sleep/read is fine)
    // Launch I/O threads
    if (pthread_create(&t1, NULL, stdin_to_server, &sock) != 0) {
        perror("pthread_create");
        close(sock); exit(EXIT_FAILURE);
    }
    if (pthread_create(&t2, NULL, server_to_stdout, &sock) != 0) {
        perror("pthread_create");
        close(sock); exit(EXIT_FAILURE);
    }

    // Wait for threads to finish
    pthread_join(t1, NULL);
    // After input thread exits, shutdown and wait for server thread
    shutdown(sock, SHUT_RDWR);
    pthread_join(t2, NULL);

    close(sock);
    return 0;
}
