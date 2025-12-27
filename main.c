#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BACKLOG 16
#define READ_BUF 4096

typedef struct {
    pthread_t thread;
    int running;
    int stop_requested;
    int listen_fd;
    int port;
    char root[PATH_MAX];
    pthread_mutex_t lock;
} server_state;

static void set_running(server_state *state, int running) {
    pthread_mutex_lock(&state->lock);
    state->running = running;
    pthread_mutex_unlock(&state->lock);
}

static int is_running(server_state *state) {
    int running;
    pthread_mutex_lock(&state->lock);
    running = state->running;
    pthread_mutex_unlock(&state->lock);
    return running;
}

static const char *content_type_for_path(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) {
        return "application/octet-stream";
    }
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) {
        return "text/html";
    }
    if (strcmp(ext, ".css") == 0) {
        return "text/css";
    }
    if (strcmp(ext, ".js") == 0) {
        return "application/javascript";
    }
    if (strcmp(ext, ".png") == 0) {
        return "image/png";
    }
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcmp(ext, ".gif") == 0) {
        return "image/gif";
    }
    if (strcmp(ext, ".txt") == 0) {
        return "text/plain";
    }
    return "application/octet-stream";
}

static void send_response(int client_fd, int status, const char *status_text, const char *content_type,
                          const void *body, size_t body_len) {
    char header[512];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Content-Type: %s\r\n"
                              "Connection: close\r\n\r\n",
                              status, status_text, body_len, content_type);
    send(client_fd, header, (size_t)header_len, 0);
    if (body_len > 0 && body) {
        send(client_fd, body, body_len, 0);
    }
}

static void send_404(int client_fd) {
    const char *body = "404 Not Found\n";
    send_response(client_fd, 404, "Not Found", "text/plain", body, strlen(body));
}

static int contains_parent_traversal(const char *path) {
    return strstr(path, "..") != NULL;
}

static void handle_client(int client_fd, server_state *state) {
    char buffer[READ_BUF];
    ssize_t received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (received <= 0) {
        return;
    }
    buffer[received] = '\0';

    char method[8];
    char url[1024];
    if (sscanf(buffer, "%7s %1023s", method, url) != 2) {
        send_response(client_fd, 400, "Bad Request", "text/plain", "Bad Request\n", 12);
        return;
    }

    if (strcmp(method, "GET") != 0) {
        send_response(client_fd, 405, "Method Not Allowed", "text/plain", "Method Not Allowed\n", 20);
        return;
    }

    if (contains_parent_traversal(url)) {
        send_response(client_fd, 403, "Forbidden", "text/plain", "Forbidden\n", 10);
        return;
    }

    char path[PATH_MAX];
    pthread_mutex_lock(&state->lock);
    const char *root = state->root;
    pthread_mutex_unlock(&state->lock);

    if (strcmp(url, "/") == 0) {
        snprintf(path, sizeof(path), "%s/index.html", root);
    } else {
        snprintf(path, sizeof(path), "%s%s", root, url);
    }

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        send_404(client_fd);
        return;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        send_404(client_fd);
        return;
    }

    const char *content_type = content_type_for_path(path);
    char header[512];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Length: %zu\r\n"
                              "Content-Type: %s\r\n"
                              "Connection: close\r\n\r\n",
                              (size_t)st.st_size, content_type);
    send(client_fd, header, (size_t)header_len, 0);

    ssize_t bytes_read;
    while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
        send(client_fd, buffer, (size_t)bytes_read, 0);
    }
    close(fd);
}

static void *server_thread(void *arg) {
    server_state *state = (server_state *)arg;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        set_running(state, 0);
        return NULL;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    pthread_mutex_lock(&state->lock);
    int port = state->port;
    pthread_mutex_unlock(&state->lock);
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        set_running(state, 0);
        return NULL;
    }

    if (listen(listen_fd, BACKLOG) < 0) {
        perror("listen");
        close(listen_fd);
        set_running(state, 0);
        return NULL;
    }

    pthread_mutex_lock(&state->lock);
    state->listen_fd = listen_fd;
    pthread_mutex_unlock(&state->lock);

    set_running(state, 1);

    while (1) {
        pthread_mutex_lock(&state->lock);
        int stop = state->stop_requested;
        pthread_mutex_unlock(&state->lock);
        if (stop) {
            break;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listen_fd, &fds);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ready = select(listen_fd + 1, &fds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            break;
        }
        if (ready == 0) {
            continue;
        }

        if (FD_ISSET(listen_fd, &fds)) {
            int client_fd = accept(listen_fd, NULL, NULL);
            if (client_fd >= 0) {
                handle_client(client_fd, state);
                close(client_fd);
            }
        }
    }

    close(listen_fd);
    set_running(state, 0);
    return NULL;
}

static void init_state(server_state *state) {
    memset(state, 0, sizeof(*state));
    pthread_mutex_init(&state->lock, NULL);
    state->port = 8890;
    if (!getcwd(state->root, sizeof(state->root))) {
        strncpy(state->root, ".", sizeof(state->root));
    }
}

static void request_stop(server_state *state) {
    pthread_mutex_lock(&state->lock);
    state->stop_requested = 1;
    int listen_fd = state->listen_fd;
    pthread_mutex_unlock(&state->lock);

    if (listen_fd > 0) {
        shutdown(listen_fd, SHUT_RDWR);
    }
}

static void start_server(server_state *state) {
    if (is_running(state)) {
        printf("Server is already running.\n");
        return;
    }

    pthread_mutex_lock(&state->lock);
    state->stop_requested = 0;
    pthread_mutex_unlock(&state->lock);

    if (pthread_create(&state->thread, NULL, server_thread, state) != 0) {
        perror("pthread_create");
    } else {
        printf("Starting server on port %d...\n", state->port);
    }
}

static void stop_server(server_state *state) {
    if (!is_running(state)) {
        printf("Server is not running.\n");
        return;
    }
    request_stop(state);
    pthread_join(state->thread, NULL);
    printf("Server stopped.\n");
}

static void print_status(server_state *state) {
    pthread_mutex_lock(&state->lock);
    int running = state->running;
    int port = state->port;
    char root[PATH_MAX];
    snprintf(root, sizeof(root), "%s", state->root);
    pthread_mutex_unlock(&state->lock);

    if (running) {
        printf("Status: RUNNING on port %d\n", port);
    } else {
        printf("Status: STOPPED\n");
    }
    printf("Serving directory: %s\n", root);
}

static void set_directory(server_state *state) {
    char input[PATH_MAX];
    printf("Enter directory to serve: ");
    if (!fgets(input, sizeof(input), stdin)) {
        return;
    }
    input[strcspn(input, "\n")] = '\0';
    if (input[0] == '\0') {
        printf("Directory not changed.\n");
        return;
    }

    struct stat st;
    if (stat(input, &st) != 0 || !S_ISDIR(st.st_mode)) {
        printf("Directory does not exist.\n");
        return;
    }

    pthread_mutex_lock(&state->lock);
    snprintf(state->root, sizeof(state->root), "%s", input);
    pthread_mutex_unlock(&state->lock);
    printf("Directory updated to: %s\n", input);
}

static void set_port(server_state *state) {
    char input[64];
    printf("Enter port (current %d): ", state->port);
    if (!fgets(input, sizeof(input), stdin)) {
        return;
    }
    int port = atoi(input);
    if (port <= 0 || port > 65535) {
        printf("Invalid port.\n");
        return;
    }
    if (is_running(state)) {
        printf("Stop the server before changing the port.\n");
        return;
    }
    pthread_mutex_lock(&state->lock);
    state->port = port;
    pthread_mutex_unlock(&state->lock);
    printf("Port updated to %d\n", port);
}

static void print_menu(void) {
    printf("\n==== Simple HTTP Server ===\n");
    printf("[1] Start server\n");
    printf("[2] Stop server\n");
    printf("[3] Status\n");
    printf("[4] Set directory\n");
    printf("[5] Set port\n");
    printf("[0] Quit\n");
    printf("Select: ");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    server_state state;
    init_state(&state);

    char choice[16];
    int running = 1;
    while (running) {
        print_menu();
        if (!fgets(choice, sizeof(choice), stdin)) {
            break;
        }
        switch (choice[0]) {
            case '1':
                start_server(&state);
                break;
            case '2':
                stop_server(&state);
                break;
            case '3':
                print_status(&state);
                break;
            case '4':
                set_directory(&state);
                break;
            case '5':
                set_port(&state);
                break;
            case '0':
                running = 0;
                break;
            default:
                printf("Unknown option.\n");
                break;
        }
    }

    if (is_running(&state)) {
        stop_server(&state);
    }

    pthread_mutex_destroy(&state.lock);
    return 0;
}
