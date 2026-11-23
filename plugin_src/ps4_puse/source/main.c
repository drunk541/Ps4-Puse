#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <orbis/libkernel.h>
#define MINIMP3_IMPLEMENTATION
#include "minimp3/minimp3_ex.h"
#include "plugin_common.h"

attr_public const char *g_pluginName = "PS4-Puse";
attr_public const char *g_pluginDesc = "PS4 Music Companion";
attr_public const char *g_pluginAuth = "PS4-Puse";
attr_public uint32_t g_pluginVersion = 0x00000300;

#define PORT 9025
#define AUDIO_MODULE "/system/common/lib/libSceAudioOut.sprx"
#define USER_MODULE  "/system/common/lib/libSceUserService.sprx"

typedef int32_t (*AudioInitFn)(void);
typedef int32_t (*AudioOpenFn)(int32_t, int, int32_t, uint32_t, uint32_t, uint32_t);
typedef int32_t (*AudioOutputFn)(int32_t, const void *);
typedef int32_t (*AudioCloseFn)(int32_t);
typedef int32_t (*UserInitFn)(void *);

static AudioInitFn audioInit;
static AudioOpenFn audioOpen;
static AudioOutputFn audioOutput;
static AudioCloseFn audioClose;
static UserInitFn userInit;

static int audioReady;
static OrbisPthread serverThread;
static OrbisPthread playThread;
static volatile int running;
static volatile int listenFd = -1;
static volatile int playStop;
static int threadStarted;
static int playThreadStarted;

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
"button.active{background:#247d71}.track{padding:12px;"
"border-bottom:1px solid #293751;display:flex;justify-content:space-between;"
"align-items:center;gap:8px}.track button{flex-shrink:0}"
"#status{color:#6fffc5}small{color:#91a0bb}"
"</style></head><body><div class='app'>"
"<h1>PS4-PUSE</h1><p class='muted'>Your music companion</p>"
"<div class='card'><b id='status'>PS4 connected</b><p class='muted'>"
"Choose music source</p>"
"<button onclick=\"load('internal',this)\" class='active'>Internal</button>"
"<button onclick=\"load('usb0',this)\">USB 0</button>"
"<button onclick=\"load('usb1',this)\">USB 1</button>"
"<button onclick=\"load('usb2',this)\">USB 2</button>"
"<button onclick=\"load('usb3',this)\">USB 3</button>"
"<p><button onclick='stopMusic()'>STOP</button></p></div>"
"<div class='card'><h3>Music library</h3><div id='files' class='muted'>"
"Choose a source</div></div>"
"<small>PS4-Puse v3.0 · MP3 playback</small></div><script>"
"let source='internal';"
"async function load(src,btn){source=src;"
"document.querySelectorAll('.card button').forEach(x=>x.classList.remove('active'));"
"if(btn)btn.classList.add('active');"
"let box=document.getElementById('files');box.textContent='Loading...';"
"try{let r=await fetch('/api/files?source='+src);let j=await r.json();"
"box.innerHTML='';if(!j.files.length){box.textContent=j.error||'No audio files found';return;}"
"j.files.forEach(f=>{let d=document.createElement('div');d.className='track';"
"let name=document.createElement('span');name.textContent=f;"
"let b=document.createElement('button');b.textContent='PLAY';"
"b.onclick=()=>playFile(f);d.appendChild(name);d.appendChild(b);box.appendChild(d)});"
"}catch(e){box.textContent='Could not load files';}}"
"async function playFile(f){let s=document.getElementById('status');"
"s.textContent='Starting: '+f;try{let r=await fetch('/api/play?source='+source+'&file='+encodeURIComponent(f));"
"let j=await r.json();s.textContent=j.message||j.error||'Request sent';}"
"catch(e){s.textContent='Connection error';}}"
"async function stopMusic(){try{let r=await fetch('/api/stop');let j=await r.json();"
"document.getElementById('status').textContent=j.message||j.error||'Stopped';}"
"catch(e){document.getElementById('status').textContent='Stop failed';}}"
"load('internal',document.querySelector('.card button'));"
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
    return ext && !strcasecmp(ext, ".mp3");
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

static int valid_name(const char *s) {
    if (!s || !*s || strstr(s, "..")) return 0;
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == ' ' || c == '_' || c == '-' ||
              c == '(' || c == ')' || c == '.'))
            return 0;
    }
    return 1;
}

static int make_root(char *root, size_t cap, const char *source) {
    if (!strcmp(source, "internal")) {
        snprintf(root, cap, "/data/PS4-Puse/music");
        mkdir("/data/PS4-Puse", 0777);
        mkdir(root, 0777);
        return 1;
    }
    if (!strcmp(source, "usb0") || !strcmp(source, "usb1") ||
        !strcmp(source, "usb2") || !strcmp(source, "usb3")) {
        snprintf(root, cap, "/mnt/%s", source);
        return 1;
    }
    return 0;
}

static void list_files(int fd, const char *source) {
    char root[128];
    if (!make_root(root, sizeof(root), source)) {
        respond(fd, "application/json",
                "{\"files\":[],\"error\":\"Invalid source\"}");
        return;
    }

    DIR *dir = opendir(root);
    if (!dir) {
        respond(fd, "application/json",
                "{\"files\":[],\"error\":\"Folder unavailable\"}");
        return;
    }

    char body[8192];
    size_t used = 0;
    used += snprintf(body, sizeof(body), "{\"files\":[");
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

static uint16_t le16(const unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int load_audio_functions(void) {
    if (audioReady) return 0;

    int32_t ah = sceKernelLoadStartModule(
        AUDIO_MODULE, 0, NULL, 0, NULL, NULL);
    if (ah < 0) return -1;

    int32_t uh = sceKernelLoadStartModule(
        USER_MODULE, 0, NULL, 0, NULL, NULL);
    if (uh < 0) return -2;

    if (sceKernelDlsym(ah, "sceAudioOutInit", (void **)&audioInit) != 0 ||
        sceKernelDlsym(ah, "sceAudioOutOpen", (void **)&audioOpen) != 0 ||
        sceKernelDlsym(ah, "sceAudioOutOutput", (void **)&audioOutput) != 0 ||
        sceKernelDlsym(ah, "sceAudioOutClose", (void **)&audioClose) != 0 ||
        sceKernelDlsym(uh, "sceUserServiceInitialize", (void **)&userInit) != 0)
        return -3;

    audioReady = 1;
    return 0;
}


static void *play_worker(void *arg) {
    char path[512];
    snprintf(path, sizeof(path), "%s", (const char *)arg);
    free(arg);

    mp3dec_ex_t dec;
    memset(&dec, 0, sizeof(dec));

    if (mp3dec_ex_open(&dec, path, MP3D_SEEK_TO_BYTE) != 0)
        return NULL;

    int channels = dec.info.channels;
    int rate = dec.info.hz;

    if ((channels != 1 && channels != 2) ||
        (rate != 44100 && rate != 48000)) {
        mp3dec_ex_close(&dec);
        return NULL;
    }

    if (load_audio_functions() != 0) {
        mp3dec_ex_close(&dec);
        return NULL;
    }

    userInit(NULL);
    if (audioInit() < 0) {
        mp3dec_ex_close(&dec);
        return NULL;
    }

    int32_t handle = audioOpen(
        0xFF, 0, 0, 256, (uint32_t)rate,
        channels == 1 ? 0 : 1
    );

    if (handle <= 0) {
        mp3dec_ex_close(&dec);
        return NULL;
    }

    mp3d_sample_t buffer[256 * 2];
    const size_t wanted = 256 * (size_t)channels;

    while (!playStop) {
        size_t got = mp3dec_ex_read(&dec, buffer, wanted);
        if (got == 0)
            break;

        if (got < wanted)
            memset(buffer + got, 0,
                   (wanted - got) * sizeof(mp3d_sample_t));

        if (audioOutput(handle, NULL) < 0)
            break;
        if (audioOutput(handle, buffer) < 0)
            break;
    }

    audioClose(handle);
    mp3dec_ex_close(&dec);
    return NULL;
}

static void stop_playback(void) {
    if (playThreadStarted) {
        playStop = 1;
        scePthreadJoin(playThread, NULL);
        playThreadStarted = 0;
    }
}

static void start_playback(int fd, const char *source, const char *name) {
    char root[128], path[512];

    if (!make_root(root, sizeof(root), source) ||
        !valid_name(name) || !audio_file(name)) {
        respond(fd, "application/json",
                "{\"error\":\"Invalid file or source\"}");
        return;
    }

    stop_playback();
    snprintf(path, sizeof(path), "%s/%s", root, name);

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        respond(fd, "application/json",
                "{\"error\":\"File not found\"}");
        return;
    }

    char *copy = malloc(strlen(path) + 1);
    if (!copy) {
        respond(fd, "application/json", "{\"error\":\"Out of memory\"}");
        return;
    }
    strcpy(copy, path);

    playStop = 0;
    int r = scePthreadCreate(&playThread, NULL, play_worker,
                             copy, "PS4PuseAudio");
    if (r != 0) {
        free(copy);
        respond(fd, "application/json",
                "{\"error\":\"Could not start player thread\"}");
        return;
    }

    playThreadStarted = 1;
    respond(fd, "application/json",
            "{\"message\":\"Playback starting (WAV)\"}");
}

static void decode_url(char *s) {
    char *src = s, *dst = s;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            char *end = NULL;
            long v = strtol(hex, &end, 16);
            if (end && *end == 0) {
                *dst++ = (char)v;
                src += 3;
                continue;
            }
        }
        if (*src == '+') *dst++ = ' ';
        else *dst++ = *src;
        src++;
    }
    *dst = 0;
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

            } else if (strstr(req, "GET /api/stop")) {
                stop_playback();
                respond(c, "application/json",
                        "{\"message\":\"Playback stopped\"}");

            } else if (strstr(req, "GET /api/play")) {
                char source[32] = "internal";
                char name[256] = "";
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
                q = strstr(req, "file=");
                if (q) {
                    q += 5;
                    int i = 0;
                    while (q[i] && q[i] != ' ' && q[i] != '&' &&
                           i < (int)sizeof(name) - 1) {
                        name[i] = q[i];
                        i++;
                    }
                    name[i] = 0;
                    decode_url(name);
                }
                start_playback(c, source, name);

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
    (void)argc;
    (void)argv;
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
    (void)argc;
    (void)argv;

    stop_playback();
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
