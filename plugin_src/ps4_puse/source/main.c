#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <orbis/libkernel.h>
#include "plugin_common.h"

attr_public const char *g_pluginName = "PS4-Puse";
attr_public const char *g_pluginDesc = "PS4 Music Companion";
attr_public const char *g_pluginAuth = "PS4-Puse";
attr_public uint32_t g_pluginVersion = 0x00000200;

#define PORT 9025

static OrbisPthread serverThread;
static volatile int running = 0;
static volatile int listenFd = -1;
static int threadStarted = 0;

static const char page[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>PS4-Puse</title><style>"
"*{box-sizing:border-box}body{margin:0;background:#090d18;color:white;"
"font-family:Arial;padding:25px}.app{max-width:480px;margin:auto}"
"h1{letter-spacing:3px}.muted{color:#91a0bb}"
".card{background:#141e31;border:1px solid #293751;border-radius:20px;"
"padding:20px;margin:18px 0}button{background:#263c60;color:white;"
"border:0;border-radius:12px;padding:13px;margin:4px;cursor:pointer}"
"button.active{background:#247d71}.track{padding:13px;border-bottom:1px solid #293751}"
"#status{color:#6fffc5}small{color:#91a0bb}"
"</style></head><body><div class='app'>"
"<h1>PS4-PUSE</h1><p class='muted'>Your music companion</p>"
"<div class='card'><b id='status'>PS4 connected</b><p class='muted'>"
"Choose music source</p>"
"<button onclick=\"load('internal',this)\" class='active'>Internal</button>"
"<button onclick=\"load('usb0',this)\">USB 0</button>"
"<button onclick=\"load('usb1',this)\">USB 1</button>"
"<button onclick=\"load('usb2',this)\">USB 2</button>"
"<button onclick=\"load('usb3',this)\">USB 3</button></div>"
"<div class='card'><h3>Music library</h3><div id='files' class='muted'>"
"Choose a source</div></div>"
"<small>PS4-Puse v2.0 · Music playback is not enabled yet</small>"
"</div><script>"
"async function load(src,btn){"
"document.querySelectorAll('button').forEach(x=>x.classList.remove('active'));"
"if(btn)btn.classList.add('active');"
"let box=document.getElementById('files');box.textContent='Loading...';"
"try{let r=await fetch('/api/files?source='+src);let j=await r.json();"
"box.innerHTML='';if(!j.files.length){box.textContent=j.error||'No audio files found';return;}"
"j.files.forEach(f=>{let d=document.createElement('div');d.className='track';"
"d.textContent=f;box.appendChild(d)});"
"}catch(e){box.textContent='Could not load files';}}"
"load('internal',document.querySelector('button'));"
"</script></body></html>";

static int send_all(int fd, const char *data, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, data + done, len - done);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static void respond(int fd, const char *type, const char *body) {
    char header[256];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n",
        type, strlen(body));
    if (n > 0) {
        send_all(fd, header, (size_t)n);
        send_all(fd, body, strlen(body));
    }
}

static int audio_file(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext) return 0;
    return !strcasecmp(ext, ".mp3") ||
           !strcasecmp(ext, ".wav") ||
           !strcasecmp(ext, ".ogg") ||
           !strcasecmp(ext, ".flac") ||
           !strcasecmp(ext, ".m4a") ||
           !strcasecmp(ext, ".aac");
}

static void json_string(char *out, size_t cap, const char *s) {
    size_t p = 0;
    if (cap < 3) return;
    out[p++] = '"';
    while (*s && p + 3 < cap) {
        if (*s == '"' || *s == '\\') out[p++] = '\\';
        if ((unsigned char)*s >= 32) out[p++] = *s;
        s++;
    }
    out[p++] = '"';
    out[p] = 0;
}

static void list_files(int fd, const char *source) {
    char root[128];
    if (!strcmp(source, "internal")) {
        snprintf(root, sizeof(root), "/data/PS4-Puse/music");
        mkdir("/data/PS4-Puse", 0777);
        mkdir(root, 0777);
    } else if (!strcmp(source, "usb0") ||
               !strcmp(source, "usb1") ||
               !strcmp(source, "usb2") ||
               !strcmp(source, "usb3")) {
        snprintf(root, sizeof(root), "/mnt/%s", source);
    } else {
        respond(fd, "application/json", "{\"files\":[],\"error\":\"Invalid source\"}");
        return;
    }

    DIR *dir = opendir(root);
    if (!dir) {
        respond(fd, "application/json", "{\"files\":[],\"error\":\"Folder unavailable\"}");
        return;
    }

    char body[8192];
    size_t used = 0;
    used += snprintf(body + used, sizeof(body) - used, "{\"files\":[");
    struct dirent *entry;
    int first = 1;

    while ((entry = readdir(dir)) != NULL && used < sizeof(body) - 300) {
        if (entry->d_name[0] == '.' || !audio_file(entry->d_name))
            continue;

        char full[512];
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", root, entry->d_name);
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        char escaped[512];
        json_string(escaped, sizeof(escaped), entry->d_name);
        used += snprintf(body + used, sizeof(body) - used,
                         "%s%s", first ? "" : ",", escaped);
        first = 0;
    }

    closedir(dir);
    snprintf(body + used, sizeof(body) - used, "]}");
    respond(fd, "application/json", body);
}

static void *server_worker(void *arg) {
    (void)arg;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return NULL;
    }
    if (listen(fd, 5) != 0) {
        close(fd);
        return NULL;
    }

    listenFd = fd;
    final_printf("[PS4-Puse] HTTP listening on %d\n", PORT);

    while (running) {
        struct sockaddr_in client;
        socklen_t len = sizeof(client);
        int c = accept(fd, (struct sockaddr *)&client, &len);
        if (c < 0) {
            if (!running) break;
            continue;
        }

        char req[2048];
        ssize_t n = read(c, req, sizeof(req) - 1);
        if (n > 0) {
            req[n] = 0;
            if (strstr(req, "GET /api/files")) {
                char source[32] = "internal";
                char *q = strstr(req, "source=");
                if (q) {
                    q += 7;
                    int i = 0;
                    while (q[i] && q[i] != ' ' && q[i] != '&' &&
                           i < (int)sizeof(source) - 1) {
                        source[i] = q[i];
                        i++;
                    }
                    source[i] = 0;
                }
                list_files(c, source);
            } else {
                respond(c, "text/html; charset=utf-8", page);
            }
        }
        close(c);
    }

    listenFd = -1;
    close(fd);
    return NULL;
}

int32_t attr_public plugin_load(int32_t argc, const char *argv[]) {
    (void)argc; (void)argv;
    running = 1;

    int r = scePthreadCreate(&serverThread, NULL, server_worker,
                             NULL, "PS4PuseHTTP");
    if (r != 0) {
        running = 0;
        NotifyStatic(TEX_ICON_SYSTEM, "PS4-Puse thread error");
        return r;
    }

    threadStarted = 1;
    NotifyStatic(TEX_ICON_SYSTEM, "PS4-Puse HTTP starting");
    return 0;
}

int32_t attr_public plugin_unload(int32_t argc, const char *argv[]) {
    (void)argc; (void)argv;
    running = 0;
    if (listenFd >= 0) shutdown(listenFd, SHUT_RDWR);
    if (threadStarted) {
        scePthreadJoin(serverThread, NULL);
        threadStarted = 0;
    }
    return 0;
}

s32 attr_module_hidden module_start(s64 argc, const void *args) {
    return 0;
}
s32 attr_module_hidden module_stop(s64 argc, const void *args) {
    return 0;
}
