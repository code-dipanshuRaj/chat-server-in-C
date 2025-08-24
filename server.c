// server.c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <netinet/in.h>

#define LISTEN_PORT 9090
#define MAX_EVENTS  128
#define BUF_SIZE    4096
#define MAX_FDS     10000
#define MAX_NAME    32

typedef struct client {
    int fd;
    char name[MAX_NAME];
    char buf[BUF_SIZE];
    int buf_len;
    struct client *next;
} client_t;

static client_t *clients_by_fd[MAX_FDS];

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

ssize_t safe_send(int fd, const void *buf, size_t len) {
    size_t sent = 0;
    const char *p = buf;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return sent;
            return -1;
        }
        sent += n;
    }
    return sent;
}

void remove_client(int epfd, client_t *c) {
    if (!c) return;
    int fd = c->fd;
    if (fd >= 0 && fd < MAX_FDS) clients_by_fd[fd] = NULL;
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    printf("Removed client fd=%d name=%s\n", fd, c->name[0] ? c->name : "<unnamed>");
    free(c);
}

client_t* add_client(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    client_t *c = calloc(1, sizeof(client_t));
    if (!c) return NULL;
    c->fd = fd;
    c->buf_len = 0;
    c->name[0] = '\0';
    clients_by_fd[fd] = c;
    return c;
}

client_t* find_by_name(const char *name) {
    for (int i = 0; i < MAX_FDS; ++i) {
        client_t *c = clients_by_fd[i];
        if (c && c->name[0] && strcmp(c->name, name) == 0) return c;
    }
    return NULL;
}

void broadcast_except(int epfd, int except_fd, const char *msg, size_t len) {
    for (int i = 0; i < MAX_FDS; ++i) {
        client_t *c = clients_by_fd[i];
        if (!c) continue;
        if (c->fd == except_fd) continue;
        safe_send(c->fd, msg, len);
    }
}

void send_to_client(client_t *c, const char *msg, size_t len) {
    if (!c) return;
    safe_send(c->fd, msg, len);
}

void process_line(int epfd, client_t *c, const char *line) {
    // Remove trailing \r or \n
    size_t L = strlen(line);
    while (L > 0 && (line[L-1] == '\n' || line[L-1] == '\r')) L--;
    char tmp[BUF_SIZE+1];
    if (L >= sizeof(tmp)) L = sizeof(tmp)-1;
    memcpy(tmp, line, L);
    tmp[L] = '\0';

    // If client has no name yet, expect "NICK <name>" as first message
    if (c->name[0] == '\0') {
        if (strncmp(tmp, "NICK ", 5) == 0) {
            const char *nick = tmp + 5;
            if (strlen(nick) == 0) {
                send_to_client(c, "SERVER: invalid nickname\n", 24);
                return;
            }
            if (find_by_name(nick) != NULL) {
                send_to_client(c, "SERVER: username taken, disconnecting\n", 38);
                // will be removed by caller on next loop
                return;
            }
            strncpy(c->name, nick, MAX_NAME-1);
            char welcome[128];
            int n = snprintf(welcome, sizeof(welcome), "SERVER: welcome %s\n", c->name);
            send_to_client(c, welcome, n);
            // notify others
            char joinmsg[128];
            int m = snprintf(joinmsg, sizeof(joinmsg), "SERVER: %s has joined\n", c->name);
            broadcast_except(epfd, c->fd, joinmsg, m);
            printf("Client fd=%d set name=%s\n", c->fd, c->name);
            return;
        } else {
            // If no NICK, treat as anonymous or ask for nickname
            const char *msg = "SERVER: please set nickname with: NICK <name>\n";
            send_to_client(c, msg, strlen(msg));
            return;
        }
    }

    // Commands start with '/'
    if (tmp[0] == '/') {
        if (strncmp(tmp, "/quit", 5) == 0) {
            char out[128];
            int n = snprintf(out, sizeof(out), "SERVER: %s disconnected\n", c->name);
            broadcast_except(epfd, c->fd, out, n);
            // caller will close the connection after processing
            return;
        } else if (strncmp(tmp, "/list", 5) == 0) {
            // build list
            char listbuf[4096];
            int pos = 0;
            pos += snprintf(listbuf + pos, sizeof(listbuf)-pos, "SERVER: active users:\n");
            for (int i = 0; i < MAX_FDS; ++i) {
                client_t *cc = clients_by_fd[i];
                if (!cc) continue;
                if (cc->name[0]) pos += snprintf(listbuf + pos, sizeof(listbuf)-pos, " - %s\n", cc->name);
            }
            send_to_client(c, listbuf, pos);
            return;
        } else if (strncmp(tmp, "/msg ", 5) == 0) {
            // format: /msg recipient text...
            const char *p = tmp + 5;
            // extract recipient
            char recipient[MAX_NAME];
            int ri = 0;
            while (*p && *p != ' ' && ri < (MAX_NAME-1)) recipient[ri++] = *p++;
            recipient[ri] = '\0';
            if (ri == 0) {
                send_to_client(c, "SERVER: usage: /msg <user> <text>\n", 33);
                return;
            }
            while (*p == ' ') p++;
            if (*p == '\0') {
                send_to_client(c, "SERVER: empty message\n", 22);
                return;
            }
            client_t *dest = find_by_name(recipient);
            if (!dest) {
                send_to_client(c, "SERVER: user not found\n", 23);
                return;
            }
            char pm[BUF_SIZE];
            int n = snprintf(pm, sizeof(pm), "[Private from %s]: %s\n", c->name, p);
            send_to_client(dest, pm, n);
            // optionally notify sender
            char ack[128];
            int m = snprintf(ack, sizeof(ack), "[Private to %s]: %s\n", recipient, p);
            send_to_client(c, ack, m);
            return;
        } else {
            send_to_client(c, "SERVER: unknown command\n", 23);
            return;
        }
    }

    // Normal broadcast message
    char out[BUF_SIZE + 64];
    int n = snprintf(out, sizeof(out), "[%s]: %s\n", c->name, tmp);
    broadcast_except(epfd, c->fd, out, n);
}

int main() {
    int listen_fd, epfd;
    struct sockaddr_in serv_addr;
    struct epoll_event ev, events[MAX_EVENTS];

    memset(clients_by_fd, 0, sizeof(clients_by_fd));

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(LISTEN_PORT);

    if (bind(listen_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind"); exit(EXIT_FAILURE);
    }

    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen"); exit(EXIT_FAILURE);
    }

    if (set_nonblocking(listen_fd) < 0) {
        perror("fcntl"); exit(EXIT_FAILURE);
    }

    epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); exit(EXIT_FAILURE); }

    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) { perror("epoll_ctl add listen"); exit(EXIT_FAILURE); }

    printf("Server listening on port %d\n", LISTEN_PORT);

    while (1) {
        int nready = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nready < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait"); break;
        }

        for (int i = 0; i < nready; i++) {
            int fd = events[i].data.fd;

            // New connection
            if (fd == listen_fd) {
                while (1) {
                    struct sockaddr_in cli_addr;
                    socklen_t addrlen = sizeof(cli_addr);
                    int client_fd = accept(listen_fd, (struct sockaddr*)&cli_addr, &addrlen);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept"); break;
                    }
                    if (client_fd >= MAX_FDS) {
                        close(client_fd);
                        continue;
                    }
                    if (set_nonblocking(client_fd) < 0) perror("set_nonblocking");
                    client_t *c = add_client(client_fd);
                    if (!c) { close(client_fd); continue; }
                    ev.events = EPOLLIN;
                    ev.data.fd = client_fd;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
                        perror("epoll_ctl add client");
                        remove_client(epfd, c);
                        continue;
                    }
                    char addrbuf[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &cli_addr.sin_addr, addrbuf, sizeof(addrbuf));
                    printf("New client fd=%d from %s:%d\n", client_fd, addrbuf, ntohs(cli_addr.sin_port));
                    // prompt for nickname
                    send_to_client(c, "SERVER: please set nickname with: NICK <name>\n", 44);
                }
            } else {
                // Existing client data
                if (fd < 0 || fd >= MAX_FDS) continue;
                client_t *c = clients_by_fd[fd];
                if (!c) {
                    // shouldn't happen
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                    continue;
                }

                // Read loop (non-blocking)
                while (1) {
                    ssize_t count = read(fd, c->buf + c->buf_len, sizeof(c->buf) - c->buf_len - 1);
                    if (count == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break; // no more data
                        perror("read");
                        remove_client(epfd, c);
                        break;
                    } else if (count == 0) {
                        // disconnected
                        char msg[128];
                        if (c->name[0]) {
                            int n = snprintf(msg, sizeof(msg), "SERVER: %s disconnected\n", c->name);
                            broadcast_except(epfd, c->fd, msg, n);
                        }
                        remove_client(epfd, c);
                        break;
                    } else {
                        c->buf_len += count;
                        c->buf[c->buf_len] = '\0';
                        // Process full lines
                        char *start = c->buf;
                        char *newline;
                        while ((newline = strchr(start, '\n')) != NULL) {
                            size_t linelen = newline - start + 1;
                            char line[BUF_SIZE+1];
                            if (linelen > BUF_SIZE) linelen = BUF_SIZE;
                            memcpy(line, start, linelen);
                            line[linelen] = '\0';
                            process_line(epfd, c, line);
                            // check if client sent /quit as a command => if so, remove client
                            if (strncmp(line, "/quit", 5) == 0) {
                                remove_client(epfd, c);
                                goto cont_outer; // break out to outer loop safely
                            }
                            start = newline + 1;
                        }
                        // move leftover to beginning
                        int leftover = c->buf + c->buf_len - start;
                        if (leftover > 0 && start != c->buf) {
                            memmove(c->buf, start, leftover);
                        }
                        c->buf_len = leftover;
                        c->buf[c->buf_len] = '\0';
                    }
                }
            }
        cont_outer:
            ;
        }
    }

    close(listen_fd);
    return 0;
}
