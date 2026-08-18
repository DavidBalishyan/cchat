#define _POSIX_C_SOURCE 200809L

#include "loader.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

enum client_state {
    CLIENT_AWAITING_PROTOCOL,
    CLIENT_AWAITING_USERNAME,
    CLIENT_AWAITING_PASSWORD,
    CLIENT_READY,
    CLIENT_REJECTED
};

struct client {
    int fd;
    char host[INET_ADDRSTRLEN];
    uint16_t port;
    enum client_state state;
    char username[MAX_USERNAME_LENGTH + 1];
    unsigned char incoming[MAX_MESSAGE_LENGTH];
    size_t incoming_length;
    unsigned char outgoing[MAX_PENDING_BYTES];
    size_t outgoing_length;
    time_t authentication_deadline;
};

static volatile sig_atomic_t stop_requested = 0;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int install_signal_handlers(void)
{
    struct sigaction stop_action = {0};
    struct sigaction ignore_action = {0};

    stop_action.sa_handler = request_stop;
    ignore_action.sa_handler = SIG_IGN;
    if (sigemptyset(&stop_action.sa_mask) == -1 ||
        sigemptyset(&ignore_action.sa_mask) == -1 ||
        sigaction(SIGINT, &stop_action, NULL) == -1 ||
        sigaction(SIGTERM, &stop_action, NULL) == -1 ||
        sigaction(SIGPIPE, &ignore_action, NULL) == -1) {
        return -1;
    }
    return 0;
}

static int set_nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return -1;
    }
    return 0;
}

static bool parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || *text == '\0') {
        return false;
    }

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || value > UINT16_MAX) {
        return false;
    }

    *port = (uint16_t)value;
    return true;
}

static bool valid_server_password(const char *password)
{
    const size_t length = password == NULL ? 0 : strlen(password);

    return length > 0 && length <= MAX_PASSWORD_LENGTH &&
           strchr(password, '\n') == NULL && strchr(password, '\r') == NULL;
}

static void print_usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage: C_CHAT_PASSWORD=<password> %s [port]\n"
            "Start the C-Chat server (default port: %u).\n"
            "Port 0 asks the operating system to select an available port.\n",
            program, (unsigned int)DEFAULT_PORT);
}

static int create_listener(uint16_t requested_port, uint16_t *bound_port)
{
    const int enabled = 1;
    struct sockaddr_in address = {0};
    socklen_t address_length = (socklen_t)sizeof(address);
    int listener;

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1) {
        perror("socket");
        return -1;
    }

    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled,
                   (socklen_t)sizeof(enabled)) == -1) {
        perror("setsockopt(SO_REUSEADDR)");
        close(listener);
        return -1;
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(requested_port);

    if (bind(listener, (const struct sockaddr *)&address,
             (socklen_t)sizeof(address)) == -1) {
        perror("bind");
        close(listener);
        return -1;
    }
    if (listen(listener, LISTEN_BACKLOG) == -1) {
        perror("listen");
        close(listener);
        return -1;
    }
    if (set_nonblocking(listener) == -1) {
        perror("fcntl(listener)");
        close(listener);
        return -1;
    }
    if (getsockname(listener, (struct sockaddr *)&address, &address_length) == -1) {
        perror("getsockname");
        close(listener);
        return -1;
    }

    *bound_port = ntohs(address.sin_port);
    return listener;
}

static void initialize_clients(struct client clients[MAX_CLIENTS])
{
    size_t index;

    for (index = 0; index < MAX_CLIENTS; ++index) {
        clients[index].fd = -1;
        clients[index].incoming_length = 0;
        clients[index].outgoing_length = 0;
    }
}

static void disconnect_client(struct client *client, const char *reason)
{
    if (client->fd == -1) {
        return;
    }

    if (client->state == CLIENT_READY) {
        printf("Client %s (%s:%u) disconnected%s%s\n", client->username,
               client->host, (unsigned int)client->port,
               reason == NULL ? "" : ": ", reason == NULL ? "" : reason);
    } else {
        printf("Client %s:%u disconnected%s%s\n", client->host,
               (unsigned int)client->port, reason == NULL ? "" : ": ",
               reason == NULL ? "" : reason);
    }

    close(client->fd);
    client->fd = -1;
    client->incoming_length = 0;
    client->outgoing_length = 0;
}

static struct client *available_client(struct client clients[MAX_CLIENTS])
{
    size_t index;

    for (index = 0; index < MAX_CLIENTS; ++index) {
        if (clients[index].fd == -1) {
            return &clients[index];
        }
    }
    return NULL;
}

static void accept_clients(int listener, struct client clients[MAX_CLIENTS])
{
    for (;;) {
        struct sockaddr_in address = {0};
        socklen_t address_length = (socklen_t)sizeof(address);
        struct client *client;
        int client_fd;

        client_fd = accept(listener, (struct sockaddr *)&address, &address_length);
        if (client_fd == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("accept");
            }
            return;
        }

        client = available_client(clients);
        if (client == NULL) {
            fprintf(stderr, "Connection rejected: maximum of %d clients reached\n",
                    MAX_CLIENTS);
            close(client_fd);
            continue;
        }
        if (set_nonblocking(client_fd) == -1) {
            perror("fcntl(client)");
            close(client_fd);
            continue;
        }

        client->fd = client_fd;
        client->port = ntohs(address.sin_port);
        client->state = CLIENT_AWAITING_PROTOCOL;
        client->username[0] = '\0';
        client->incoming_length = 0;
        client->outgoing_length = 0;
        client->authentication_deadline =
            time(NULL) + AUTHENTICATION_TIMEOUT_SECONDS;
        if (inet_ntop(AF_INET, &address.sin_addr, client->host,
                      (socklen_t)sizeof(client->host)) == NULL) {
            (void)snprintf(client->host, sizeof(client->host), "unknown");
        }
        printf("Connection from %s:%u awaiting authentication\n", client->host,
               (unsigned int)client->port);
    }
}

static void queue_for_client(struct client *client, const unsigned char *data,
                             size_t length)
{
    if (length > sizeof(client->outgoing) - client->outgoing_length) {
        disconnect_client(client, "outgoing buffer limit reached");
        return;
    }

    memcpy(client->outgoing + client->outgoing_length, data, length);
    client->outgoing_length += length;
}

static void reject_client(struct client *client, const char *response,
                          const char *log_reason)
{
    printf("Authentication rejected for %s:%u: %s\n", client->host,
           (unsigned int)client->port, log_reason);
    client->state = CLIENT_REJECTED;
    client->incoming_length = 0;
    queue_for_client(client, (const unsigned char *)response, strlen(response));
}

static bool valid_username(const unsigned char *username, size_t length)
{
    size_t index;

    if (length == 0 || length > MAX_USERNAME_LENGTH) {
        return false;
    }
    for (index = 0; index < length; ++index) {
        const unsigned char character = username[index];
        if (!((character >= (unsigned char)'a' &&
               character <= (unsigned char)'z') ||
              (character >= (unsigned char)'A' &&
               character <= (unsigned char)'Z') ||
              (character >= (unsigned char)'0' &&
               character <= (unsigned char)'9') ||
              character == (unsigned char)'_' ||
              character == (unsigned char)'-')) {
            return false;
        }
    }
    return true;
}

static bool passwords_equal(const unsigned char *candidate,
                            size_t candidate_length, const char *password)
{
    const size_t password_length = strlen(password);
    unsigned int difference =
        (unsigned int)(candidate_length ^ password_length);
    size_t index;

    for (index = 0; index < MAX_PASSWORD_LENGTH; ++index) {
        const unsigned char candidate_byte =
            index < candidate_length ? candidate[index] : 0U;
        const unsigned char password_byte = index < password_length
                                                ? (unsigned char)password[index]
                                                : 0U;
        difference |= (unsigned int)(candidate_byte ^ password_byte);
    }
    return difference == 0U;
}

static bool username_in_use(const struct client clients[MAX_CLIENTS],
                            const struct client *current)
{
    size_t index;

    for (index = 0; index < MAX_CLIENTS; ++index) {
        if (&clients[index] != current && clients[index].fd != -1 &&
            clients[index].state == CLIENT_READY &&
            strcmp(clients[index].username, current->username) == 0) {
            return true;
        }
    }
    return false;
}

static void broadcast_message(struct client clients[MAX_CLIENTS],
                              const struct client *sender,
                              const unsigned char *message, size_t length)
{
    unsigned char formatted[MAX_USERNAME_LENGTH + 2 + MAX_MESSAGE_LENGTH + 1];
    const size_t username_length = strlen(sender->username);
    const size_t formatted_length = username_length + 2 + length + 1;
    size_t index;

    memcpy(formatted, sender->username, username_length);
    memcpy(formatted + username_length, ": ", 2);
    memcpy(formatted + username_length + 2, message, length);
    formatted[formatted_length - 1] = (unsigned char)'\n';

    for (index = 0; index < MAX_CLIENTS; ++index) {
        if (clients[index].fd != -1 && &clients[index] != sender &&
            clients[index].state == CLIENT_READY) {
            queue_for_client(&clients[index], formatted, formatted_length);
        }
    }
}

static bool process_client_line(struct client *client,
                                struct client clients[MAX_CLIENTS],
                                const char *password,
                                const unsigned char *line, size_t length)
{
    switch (client->state) {
    case CLIENT_AWAITING_PROTOCOL:
        if (length != strlen(PROTOCOL_VERSION) ||
            memcmp(line, PROTOCOL_VERSION, length) != 0) {
            reject_client(client, "ERROR unsupported protocol\n",
                          "unsupported protocol");
            return false;
        }
        client->state = CLIENT_AWAITING_USERNAME;
        return true;

    case CLIENT_AWAITING_USERNAME:
        if (!valid_username(line, length)) {
            reject_client(client, "ERROR invalid username\n", "invalid username");
            return false;
        }
        memcpy(client->username, line, length);
        client->username[length] = '\0';
        client->state = CLIENT_AWAITING_PASSWORD;
        return true;

    case CLIENT_AWAITING_PASSWORD:
        if (!passwords_equal(line, length, password)) {
            reject_client(client, "ERROR authentication failed\n",
                          "incorrect password");
            return false;
        }
        if (username_in_use(clients, client)) {
            reject_client(client, "ERROR username already in use\n",
                          "username already in use");
            return false;
        }
        client->state = CLIENT_READY;
        queue_for_client(client, (const unsigned char *)"OK\n", 3);
        printf("Client %s authenticated from %s:%u\n", client->username,
               client->host, (unsigned int)client->port);
        return true;

    case CLIENT_READY:
        if (length > 0) {
            broadcast_message(clients, client, line, length);
        }
        return true;

    case CLIENT_REJECTED:
        return false;
    }
    return false;
}

static bool consume_client_data(struct client *client,
                                struct client clients[MAX_CLIENTS],
                                const char *password,
                                const unsigned char *data, size_t length)
{
    size_t index;

    for (index = 0; index < length; ++index) {
        const unsigned char byte = data[index];

        if (byte == (unsigned char)'\n') {
            size_t line_length = client->incoming_length;
            if (line_length > 0 &&
                client->incoming[line_length - 1] == (unsigned char)'\r') {
                --line_length;
            }
            if (!process_client_line(client, clients, password, client->incoming,
                                     line_length)) {
                return false;
            }
            client->incoming_length = 0;
            continue;
        }

        if (client->incoming_length == sizeof(client->incoming)) {
            if (client->state == CLIENT_READY) {
                disconnect_client(client, "message exceeds size limit");
            } else {
                reject_client(client, "ERROR authentication line too long\n",
                              "authentication line too long");
            }
            return false;
        }
        client->incoming[client->incoming_length++] = byte;
    }
    return true;
}

static void receive_from_client(struct client *client,
                                struct client clients[MAX_CLIENTS],
                                const char *password)
{
    unsigned char buffer[READ_BUFFER_SIZE];

    for (;;) {
        const ssize_t received = recv(client->fd, buffer, sizeof(buffer), 0);

        if (received > 0) {
            if (!consume_client_data(client, clients, password, buffer,
                                     (size_t)received)) {
                return;
            }
            continue;
        }
        if (received == 0) {
            disconnect_client(client, NULL);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }

        {
            const int saved_errno = errno;
            disconnect_client(client, strerror(saved_errno));
            return;
        }
    }
}

static void flush_client(struct client *client)
{
    ssize_t sent;

    if (client->outgoing_length == 0) {
        return;
    }

    sent = send(client->fd, client->outgoing, client->outgoing_length, 0);
    if (sent > 0) {
        const size_t sent_length = (size_t)sent;
        const size_t remaining = client->outgoing_length - sent_length;

        if (remaining > 0) {
            memmove(client->outgoing, client->outgoing + sent_length, remaining);
        }
        client->outgoing_length = remaining;
        return;
    }
    if (sent == 0) {
        disconnect_client(client, "socket closed");
    } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        const int saved_errno = errno;
        disconnect_client(client, strerror(saved_errno));
    }
}

static void expire_authentication_attempts(struct client clients[MAX_CLIENTS])
{
    const time_t now = time(NULL);
    size_t index;

    for (index = 0; index < MAX_CLIENTS; ++index) {
        if (clients[index].fd != -1 &&
            clients[index].state != CLIENT_READY &&
            clients[index].state != CLIENT_REJECTED &&
            now >= clients[index].authentication_deadline) {
            reject_client(&clients[index], "ERROR authentication timeout\n",
                          "authentication timeout");
        }
    }
}

static int serve(int listener, const char *password)
{
    struct client clients[MAX_CLIENTS];
    struct pollfd descriptors[MAX_CLIENTS + 1];
    size_t index;

    initialize_clients(clients);

    while (!stop_requested) {
        int activity;

        descriptors[0].fd = listener;
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        for (index = 0; index < MAX_CLIENTS; ++index) {
            descriptors[index + 1].fd = clients[index].fd;
            descriptors[index + 1].events = 0;
            if (clients[index].state != CLIENT_REJECTED) {
                descriptors[index + 1].events |= POLLIN;
            }
            if (clients[index].outgoing_length > 0) {
                descriptors[index + 1].events |= POLLOUT;
            }
            descriptors[index + 1].revents = 0;
        }

        activity = poll(descriptors, MAX_CLIENTS + 1, POLL_INTERVAL_MILLISECONDS);
        if (activity == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }

        if ((descriptors[0].revents & POLLIN) != 0) {
            accept_clients(listener, clients);
        }
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            fprintf(stderr, "Listener socket failed\n");
            break;
        }

        for (index = 0; index < MAX_CLIENTS; ++index) {
            const short events = descriptors[index + 1].revents;

            if (clients[index].fd == -1) {
                continue;
            }
            if ((events & POLLIN) != 0) {
                receive_from_client(&clients[index], clients, password);
            }
            if (clients[index].fd != -1 && (events & POLLOUT) != 0) {
                flush_client(&clients[index]);
            }
            if (clients[index].fd != -1 &&
                (events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                disconnect_client(&clients[index], "socket closed");
            }
            if (clients[index].fd != -1 &&
                clients[index].state == CLIENT_REJECTED &&
                clients[index].outgoing_length == 0) {
                disconnect_client(&clients[index], NULL);
            }
        }
        expire_authentication_attempts(clients);
    }

    for (index = 0; index < MAX_CLIENTS; ++index) {
        if (clients[index].fd != -1) {
            close(clients[index].fd);
        }
    }
    return stop_requested ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char **argv)
{
    const char *password = getenv("C_CHAT_PASSWORD");
    uint16_t requested_port = DEFAULT_PORT;
    uint16_t bound_port;
    int listener;
    int status;

    if (argc == 2 && (strcmp(argv[1], "-h") == 0 ||
                      strcmp(argv[1], "--help") == 0)) {
        print_usage(stdout, argv[0]);
        return EXIT_SUCCESS;
    }
    if (argc > 2 || (argc == 2 && !parse_port(argv[1], &requested_port))) {
        print_usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }
    if (!valid_server_password(password)) {
        fprintf(stderr,
                "C_CHAT_PASSWORD must contain 1 to %d bytes and no newlines\n",
                MAX_PASSWORD_LENGTH);
        return EXIT_FAILURE;
    }
    if (install_signal_handlers() == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    if (setvbuf(stdout, NULL, _IOLBF, 0) != 0) {
        fprintf(stderr, "Unable to configure server output buffering\n");
        return EXIT_FAILURE;
    }

    listener = create_listener(requested_port, &bound_port);
    if (listener == -1) {
        return EXIT_FAILURE;
    }

    printf("C-Chat server listening on 0.0.0.0:%u (password required)\n",
           (unsigned int)bound_port);
    status = serve(listener, password);
    close(listener);
    printf("C-Chat server stopped\n");
    return status;
}
