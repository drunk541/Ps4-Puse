#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <orbis/libkernel.h>
#include "plugin_common.h"

attr_public const char *g_pluginName = "PS4-Puse";
attr_public const char *g_pluginDesc = "PS4-Puse HTTP Server";
attr_public const char *g_pluginAuth = "PS4-Puse";
attr_public uint32_t g_pluginVersion = 0x00000100;

#define SERVER_PORT 9025

static OrbisPthread g_serverThread;
static volatile int g_running = 0;
static volatile int g_listenFd = -1;
static int g_threadStarted = 0;

static const char page[] =
"<!DOCTYPE html><html lang='en'><head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<meta charset='UTF-8'><title>PS4-Puse</title>"
"<style>"
"*{box-sizing:border-box}body{margin:0;background:#090d18;color:#fff;"
"font-family:Arial,sans-serif;display:grid;place-items:center;min-height:100vh}"
".panel{width:min(90%,420px);padding:30px;border:1px solid #26334d;"
"border-radius:24px;background:#111a2b;box-shadow:0 20px 70px #0008}"
".logo{font-size:34px;font-weight:800;letter-spacing:2px}"
".sub{color:#9aabc8;margin-top:8px}"
".status{margin-top:30px;padding:18px;border-radius:15px;"
"background:#102b29;color:#6fffd0}"
".dot{display:inline-block;width:9px;height:9px;border-radius:50%;"
"background:#35e6a0;margin-right:9px}"
".info{color:#9aabc8;font-size:13px;margin-top:24px;line-height:1.8}"
"</style></head><body><main class='panel'>"
"<div class='logo'>PS4-PUSE</div>"
"<div class='sub'>Your music companion</div>"
"<div class='status'><span class='dot'></span>PS4 connected successfully</div>"
"<div class='info'>HTTP server is running on your PlayStation 4."
"<br>Mobile control panel is under development.</div>"
"</main></body></html>";

static int send_all(int fd, const char *data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = write(fd, data + sent, length - sent);
        if (n < 0) {
                        return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static void *server_worker(void *arg) {
    (void)arg;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        final_printf("[PS4-Puse] socket failed: %s\n", "system call failed");
        return NULL;
    }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(SERVER_PORT);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        final_printf("[PS4-Puse] bind failed: %s\n", "system call failed");
        close(fd);
        return NULL;
    }

    if (listen(fd, 5) != 0) {
        final_printf("[PS4-Puse] listen failed: %s\n", "system call failed");
        close(fd);
        return NULL;
    }

    g_listenFd = fd;
    final_printf("[PS4-Puse] HTTP server listening on port %d\n", SERVER_PORT);

    while (g_running) {
        struct sockaddr_in client;
        socklen_t clientLen = sizeof(client);

        int clientFd = accept(fd, (struct sockaddr *)&client, &clientLen);
        if (clientFd < 0) {
            if (!g_running) break;
                        continue;
        }

        char request[1024];
        read(clientFd, request, sizeof(request) - 1);

        char header[256];
        int headerLen = snprintf(
            header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n",
            strlen(page)
        );

        if (headerLen > 0) {
            send_all(clientFd, header, (size_t)headerLen);
            send_all(clientFd, page, strlen(page));
        }

        close(clientFd);
    }

    g_listenFd = -1;
    close(fd);
    return NULL;
}

int32_t attr_public plugin_load(int32_t argc, const char *argv[]) {
    (void)argc;
    (void)argv;

    g_running = 1;

    int result = scePthreadCreate(
        &g_serverThread,
        NULL,
        server_worker,
        NULL,
        "PS4PuseHTTP"
    );

    if (result != 0) {
        g_running = 0;
        NotifyStatic(TEX_ICON_SYSTEM, "PS4-Puse: Thread creation failed");
        return result;
    }

    g_threadStarted = 1;
    NotifyStatic(TEX_ICON_SYSTEM, "PS4-Puse HTTP starting");
    return 0;
}

int32_t attr_public plugin_unload(int32_t argc, const char *argv[]) {
    (void)argc;
    (void)argv;

    g_running = 0;

    int fd = g_listenFd;
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
    }

    if (g_threadStarted) {
        scePthreadJoin(g_serverThread, NULL);
        g_threadStarted = 0;
    }

    NotifyStatic(TEX_ICON_SYSTEM, "PS4-Puse stopped");
    return 0;
}

s32 attr_module_hidden module_start(s64 argc, const void *args) {
    return 0;
}

s32 attr_module_hidden module_stop(s64 argc, const void *args) {
    return 0;
}
