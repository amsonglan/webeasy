#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <shlobj.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Shell32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define BACKLOG 16
#define READ_BUF 4096

#ifdef _WIN32
#define CLOSE_SOCKET closesocket
#define SOCK_ERR SOCKET_ERROR
typedef SOCKET socket_handle;
#else
#define CLOSE_SOCKET close
#define SOCK_ERR (-1)
typedef int socket_handle;
#endif

typedef struct {
#ifdef _WIN32
    HANDLE thread;
    CRITICAL_SECTION lock;
    socket_handle listen_fd;
#else
    pthread_t thread;
    pthread_mutex_t lock;
    socket_handle listen_fd;
#endif
    int running;
    int stop_requested;
    int port;
#ifdef _WIN32
    char root[MAX_PATH];
#else
    char root[PATH_MAX];
#endif
} server_state;

static void lock_state(server_state *state) {
#ifdef _WIN32
    EnterCriticalSection(&state->lock);
#else
    pthread_mutex_lock(&state->lock);
#endif
}

static void unlock_state(server_state *state) {
#ifdef _WIN32
    LeaveCriticalSection(&state->lock);
#else
    pthread_mutex_unlock(&state->lock);
#endif
}

static void set_running(server_state *state, int running) {
    lock_state(state);
    state->running = running;
    unlock_state(state);
}

static int is_running(server_state *state) {
    int running;
    lock_state(state);
    running = state->running;
    unlock_state(state);
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

static void send_response(socket_handle client_fd, int status, const char *status_text,
                          const char *content_type, const void *body, size_t body_len) {
    char header[512];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Content-Type: %s\r\n"
                              "Connection: close\r\n\r\n",
                              status, status_text, body_len, content_type);
    send(client_fd, header, (int)header_len, 0);
    if (body_len > 0 && body) {
        send(client_fd, (const char *)body, (int)body_len, 0);
    }
}

static void send_404(socket_handle client_fd) {
    const char *body = "404 Not Found\n";
    send_response(client_fd, 404, "Not Found", "text/plain", body, strlen(body));
}

static int contains_parent_traversal(const char *path) {
    return strstr(path, "..") != NULL;
}

static void handle_client(socket_handle client_fd, server_state *state) {
    char buffer[READ_BUF];
    int received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
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

#ifdef _WIN32
    char path[MAX_PATH];
#else
    char path[PATH_MAX];
#endif

    lock_state(state);
    const char *root = state->root;
    unlock_state(state);

    if (strcmp(url, "/") == 0) {
        snprintf(path, sizeof(path), "%s/index.html", root);
    } else {
        snprintf(path, sizeof(path), "%s%s", root, url);
    }

#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attr) ||
        (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        send_404(client_fd);
        return;
    }
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        send_404(client_fd);
        return;
    }
    LARGE_INTEGER size;
    size.QuadPart = 0;
    GetFileSizeEx(file, &size);
    const char *content_type = content_type_for_path(path);
    char header[512];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Length: %lld\r\n"
                              "Content-Type: %s\r\n"
                              "Connection: close\r\n\r\n",
                              (long long)size.QuadPart, content_type);
    send(client_fd, header, header_len, 0);

    DWORD bytes_read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &bytes_read, NULL) && bytes_read > 0) {
        send(client_fd, buffer, (int)bytes_read, 0);
    }
    CloseHandle(file);
#else
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
#endif
}

#ifdef _WIN32
static DWORD WINAPI server_thread(LPVOID arg) {
#else
static void *server_thread(void *arg) {
#endif
    server_state *state = (server_state *)arg;

    socket_handle listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == SOCK_ERR) {
        perror("socket");
        set_running(state, 0);
#ifdef _WIN32
        return 0;
#else
        return NULL;
#endif
    }

#ifdef _WIN32
    BOOL opt = TRUE;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#else
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    lock_state(state);
    int port = state->port;
    unlock_state(state);
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCK_ERR) {
        perror("bind");
        CLOSE_SOCKET(listen_fd);
        set_running(state, 0);
#ifdef _WIN32
        return 0;
#else
        return NULL;
#endif
    }

    if (listen(listen_fd, BACKLOG) == SOCK_ERR) {
        perror("listen");
        CLOSE_SOCKET(listen_fd);
        set_running(state, 0);
#ifdef _WIN32
        return 0;
#else
        return NULL;
#endif
    }

    lock_state(state);
    state->listen_fd = listen_fd;
    unlock_state(state);

    set_running(state, 1);

    while (1) {
        lock_state(state);
        int stop = state->stop_requested;
        unlock_state(state);
        if (stop) {
            break;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listen_fd, &fds);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ready = select((int)(listen_fd + 1), &fds, NULL, NULL, &tv);
        if (ready < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEINTR) {
                continue;
            }
#else
            if (errno == EINTR) {
                continue;
            }
#endif
            perror("select");
            break;
        }
        if (ready == 0) {
            continue;
        }

        if (FD_ISSET(listen_fd, &fds)) {
            socket_handle client_fd = accept(listen_fd, NULL, NULL);
            if (client_fd != SOCK_ERR) {
                handle_client(client_fd, state);
                CLOSE_SOCKET(client_fd);
            }
        }
    }

    CLOSE_SOCKET(listen_fd);
    set_running(state, 0);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void init_state(server_state *state) {
    memset(state, 0, sizeof(*state));
#ifdef _WIN32
    InitializeCriticalSection(&state->lock);
    state->listen_fd = INVALID_SOCKET;
    state->port = 8890;
    DWORD len = GetCurrentDirectoryA(MAX_PATH, state->root);
    if (len == 0 || len >= MAX_PATH) {
        strcpy(state->root, ".");
    }
#else
    pthread_mutex_init(&state->lock, NULL);
    state->port = 8890;
    if (!getcwd(state->root, sizeof(state->root))) {
        strncpy(state->root, ".", sizeof(state->root));
    }
#endif
}

static void request_stop(server_state *state) {
    lock_state(state);
    state->stop_requested = 1;
    socket_handle listen_fd = state->listen_fd;
    unlock_state(state);

    if (listen_fd) {
        shutdown(listen_fd, 2);
    }
}

static void start_server(server_state *state) {
    if (is_running(state)) {
        return;
    }

    lock_state(state);
    state->stop_requested = 0;
    unlock_state(state);

#ifdef _WIN32
    state->thread = CreateThread(NULL, 0, server_thread, state, 0, NULL);
    if (!state->thread) {
        perror("CreateThread");
    }
#else
    if (pthread_create(&state->thread, NULL, server_thread, state) != 0) {
        perror("pthread_create");
    }
#endif
}

static void stop_server(server_state *state) {
    if (!is_running(state)) {
        return;
    }
    request_stop(state);
#ifdef _WIN32
    WaitForSingleObject(state->thread, INFINITE);
    CloseHandle(state->thread);
    state->thread = NULL;
#else
    pthread_join(state->thread, NULL);
#endif
}

#ifdef _WIN32
static void set_directory(server_state *state, const char *path) {
    if (!path || path[0] == '\0') {
        return;
    }
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return;
    }
    lock_state(state);
    snprintf(state->root, sizeof(state->root), "%s", path);
    unlock_state(state);
}
#else
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

    lock_state(state);
    snprintf(state->root, sizeof(state->root), "%s", input);
    unlock_state(state);
    printf("Directory updated to: %s\n", input);
}
#endif

#ifdef _WIN32
static void set_port(server_state *state, int port) {
    if (port <= 0 || port > 65535) {
        return;
    }
    if (is_running(state)) {
        return;
    }
    lock_state(state);
    state->port = port;
    unlock_state(state);
}
#else
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
    lock_state(state);
    state->port = port;
    unlock_state(state);
    printf("Port updated to %d\n", port);
}
#endif

#ifndef _WIN32
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

static void print_status(server_state *state) {
    lock_state(state);
    int running = state->running;
    int port = state->port;
    char root[PATH_MAX];
    snprintf(root, sizeof(root), "%s", state->root);
    unlock_state(state);

    if (running) {
        printf("Status: RUNNING on port %d\n", port);
    } else {
        printf("Status: STOPPED\n");
    }
    printf("Serving directory: %s\n", root);
}
#endif

#ifdef _WIN32
#define ID_BUTTON_START 101
#define ID_BUTTON_STOP 102
#define ID_BUTTON_STATUS 103
#define ID_BUTTON_DIR 104
#define ID_EDIT_PORT 201
#define ID_STATIC_STATUS 301
#define ID_STATIC_DIR 302

static void update_status_text(HWND hwnd, server_state *state) {
    char status[128];
    char dir[MAX_PATH + 32];
    lock_state(state);
    int running = state->running;
    int port = state->port;
    snprintf(status, sizeof(status), "Status: %s on port %d", running ? "RUNNING" : "STOPPED", port);
    snprintf(dir, sizeof(dir), "Directory: %s", state->root);
    unlock_state(state);

    SetDlgItemTextA(hwnd, ID_STATIC_STATUS, status);
    SetDlgItemTextA(hwnd, ID_STATIC_DIR, dir);
}

static void browse_directory(HWND hwnd, server_state *state) {
    BROWSEINFOA bi;
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = hwnd;
    bi.lpszTitle = "Select directory to serve";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) {
        return;
    }
    char path[MAX_PATH];
    if (SHGetPathFromIDListA(pidl, path)) {
        set_directory(state, path);
        update_status_text(hwnd, state);
    }
    CoTaskMemFree(pidl);
}

static void apply_port_from_edit(HWND hwnd, server_state *state) {
    BOOL translated = FALSE;
    int port = GetDlgItemInt(hwnd, ID_EDIT_PORT, &translated, FALSE);
    if (translated) {
        set_port(state, port);
        update_status_text(hwnd, state);
    }
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    static server_state *state = NULL;

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCT *create = (CREATESTRUCT *)lparam;
            state = (server_state *)create->lpCreateParams;
            CreateWindowA("BUTTON", "Start", WS_TABSTOP | WS_VISIBLE | WS_CHILD,
                          20, 20, 100, 30, hwnd, (HMENU)ID_BUTTON_START, NULL, NULL);
            CreateWindowA("BUTTON", "Stop", WS_TABSTOP | WS_VISIBLE | WS_CHILD,
                          140, 20, 100, 30, hwnd, (HMENU)ID_BUTTON_STOP, NULL, NULL);
            CreateWindowA("BUTTON", "Status", WS_TABSTOP | WS_VISIBLE | WS_CHILD,
                          260, 20, 100, 30, hwnd, (HMENU)ID_BUTTON_STATUS, NULL, NULL);
            CreateWindowA("BUTTON", "Choose Dir", WS_TABSTOP | WS_VISIBLE | WS_CHILD,
                          20, 70, 100, 30, hwnd, (HMENU)ID_BUTTON_DIR, NULL, NULL);
            CreateWindowA("STATIC", "Port:", WS_VISIBLE | WS_CHILD,
                          140, 76, 40, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("EDIT", "8890", WS_TABSTOP | WS_VISIBLE | WS_CHILD | WS_BORDER,
                          190, 70, 80, 30, hwnd, (HMENU)ID_EDIT_PORT, NULL, NULL);
            CreateWindowA("STATIC", "Status: STOPPED", WS_VISIBLE | WS_CHILD,
                          20, 120, 340, 20, hwnd, (HMENU)ID_STATIC_STATUS, NULL, NULL);
            CreateWindowA("STATIC", "Directory:", WS_VISIBLE | WS_CHILD,
                          20, 150, 340, 20, hwnd, (HMENU)ID_STATIC_DIR, NULL, NULL);
            update_status_text(hwnd, state);
            return 0;
        }
        case WM_COMMAND: {
            if (!state) {
                return 0;
            }
            switch (LOWORD(wparam)) {
                case ID_BUTTON_START:
                    apply_port_from_edit(hwnd, state);
                    start_server(state);
                    update_status_text(hwnd, state);
                    break;
                case ID_BUTTON_STOP:
                    stop_server(state);
                    update_status_text(hwnd, state);
                    break;
                case ID_BUTTON_STATUS:
                    update_status_text(hwnd, state);
                    break;
                case ID_BUTTON_DIR:
                    browse_directory(hwnd, state);
                    break;
                default:
                    break;
            }
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProc(hwnd, msg, wparam, lparam);
    }
}
#endif

#ifndef _WIN32
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
#else
int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev_instance, LPSTR cmd_line, int show_cmd) {
    (void)prev_instance;
    (void)cmd_line;

    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        return 1;
    }

    server_state state;
    init_state(&state);

    const char *class_name = "SimpleHttpServerWindow";
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    if (!RegisterClassA(&wc)) {
        WSACleanup();
        return 1;
    }

    HWND hwnd = CreateWindowExA(0, class_name, "Simple HTTP Server", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 420, 240, NULL, NULL,
                                instance, &state);
    if (!hwnd) {
        WSACleanup();
        return 1;
    }

    ShowWindow(hwnd, show_cmd);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (is_running(&state)) {
        stop_server(&state);
    }

    DeleteCriticalSection(&state.lock);
    WSACleanup();
    return 0;
}
#endif
