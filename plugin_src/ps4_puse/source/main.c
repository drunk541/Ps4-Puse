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
#include <orbis/Sysmodule.h>
#define MINIMP3_IMPLEMENTATION
#include "minimp3/minimp3.h"
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
static volatile int playStatus;
static volatile int playError;
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
"async function refreshStatus(){try{let r=await fetch('/api/status');"
"let j=await r.json();if(j.status!==0){let e=document.getElementById('status');"
"e.textContent=j.message+(j.error?' (code '+j.error+')':'');}}catch(e){}}"
"setInterval(refreshStatus,1000);"
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



static int find_module_handle(const char *name, int32_t *handle_out) {
    OrbisKernelModule handles[256] = {0};
    size_t count = 0;

    int ret = sceKernelGetModuleList(handles, sizeof(handles), &count);
    if (ret != 0) return -1;

    if (count > 256) count = 256;

    for (size_t i = 0; i < count; i++) {
        OrbisKernelModuleInfo info = {0};
        info.size = sizeof(info);

        if (sceKernelGetModuleInfo(handles[i], &info) != 0)
            continue;

        if (strstr(info.name, name)) {
            *handle_out = (int32_t)handles[i];
            return 0;
        }
    }

    return -2;
}

static int load_audio_functions(void) {
    if (audioReady) return 0;

    int32_t ret = (int32_t)sceSysmoduleLoadModuleInternal(
        ORBIS_SYSMODULE_INTERNAL_AUDIOOUT
    );
    if (ret < 0) return -1;

    ret = (int32_t)sceSysmoduleLoadModuleInternal(
        ORBIS_SYSMODULE_INTERNAL_USER_SERVICE
    );
    if (ret < 0) return -2;

    int32_t audioHandle = 0;
    int32_t userHandle = 0;

    if (find_module_handle("libSceAudioOut", &audioHandle) != 0)
        return -3;

    if (find_module_handle("libSceUserService", &userHandle) != 0)
        return -4;

    if (sceKernelDlsym(audioHandle, "sceAudioOutInit",
                       (void **)&audioInit) != 0)
        return -5;

    if (sceKernelDlsym(audioHandle, "sceAudioOutOpen",
                       (void **)&audioOpen) != 0)
        return -6;

    if (sceKernelDlsym(audioHandle, "sceAudioOutOutput",
                       (void **)&audioOutput) != 0)
        return -7;

    if (sceKernelDlsym(audioHandle, "sceAudioOutClose",
                       (void **)&audioClose) != 0)
        return -8;

    if (sceKernelDlsym(userHandle, "sceUserServiceInitialize",
                       (void **)&userInit) != 0)
        return -9;

    audioReady = 1;
    return 0;
}





static void *play_worker(void *arg) {
    char path[512];
    snprintf(path, sizeof(path), "%s", (const char *)arg);
    free(arg);

    playStatus = 1;
    playError = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        playStatus = -1;
        return NULL;
    }

    mp3dec_t dec;
    mp3dec_init(&dec);

    unsigned char input[16384];
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t bytes = 0;
    int eof = 0, decodedFrames = 0;
    int32_t handle = -1;
    int channels = 0, rate = 0;

    while (!playStop) {
        if (!eof && bytes < sizeof(input)) {
            size_t n = fread(input + bytes, 1,
                             sizeof(input) - bytes, fp);
            bytes += n;
            if (n == 0) eof = 1;
        }

        size_t offset = 0;
        while (!playStop && offset < bytes) {
            mp3dec_frame_info_t info = {0};
            int samples = mp3dec_decode_frame(
                &dec, input + offset, (int)(bytes - offset),
                pcm, &info
            );

            if (info.frame_bytes <= 0) break;
            offset += (size_t)info.frame_bytes;
            if (samples <= 0) continue;

            decodedFrames++;
            if (handle < 0) {
                channels = info.channels;
                rate = info.hz;

                if ((channels != 1 && channels != 2) ||
                    (rate != 32000 && rate != 44100 && rate != 48000)) {
                    playStatus = -2;
                    goto cleanup;
                }

                playStatus = 3;
                int r = load_audio_functions();
                if (r != 0) {
                    playError = r;
                    playStatus = -3;
                    goto cleanup;
                }

                playStatus = 4;
                r = userInit(NULL);
                if (r < 0) {
                    playError = r;
                    playStatus = -4;
                    goto cleanup;
                }

                r = audioInit();
                if (r < 0) {
                    playError = r;
                    playStatus = -5;
                    goto cleanup;
                }

                playStatus = 5;
                handle = audioOpen(
                    0xFF, 0, 0, 256, (uint32_t)rate,
                    channels == 1 ? 0 : 1
                );

                if (handle <= 0) {
                    playError = handle;
                    playStatus = -6;
                    goto cleanup;
                }
            }

            if (info.channels != channels || info.hz != rate) {
                playStatus = -2;
                goto cleanup;
            }

            int pos = 0;
            while (pos < samples && !playStop) {
                int count = samples - pos;
                if (count > 256) count = 256;

                mp3d_sample_t out[512] = {0};
                memcpy(out, pcm + pos * channels,
                    (size_t)count * channels * sizeof(mp3d_sample_t));

                int r = audioOutput(handle, NULL);
                if (r < 0) {
                    playError = r;
                    playStatus = -7;
                    goto cleanup;
                }

                r = audioOutput(handle, out);
                if (r < 0) {
                    playError = r;
                    playStatus = -7;
                    goto cleanup;
                }

                playStatus = 6;
                pos += count;
            }
        }

        if (offset > 0) {
            memmove(input, input + offset, bytes - offset);
            bytes -= offset;
        }

        if (eof && bytes == 0) break;

        if (bytes == sizeof(input) && offset == 0)
            memmove(input, input + 1, --bytes);

        if (eof && offset == 0) break;
    }

    if (playStop) playStatus = 8;
    else if (decodedFrames > 0) playStatus = 7;
    else playStatus = -8;

cleanup:
    if (handle > 0) audioClose(handle);
    fclose(fp);
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
    playStatus = 1;
    playError = 0;
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


static const char *play_status_message(int status) {
    switch (status) {
        case 0: return "Idle";
        case 1: return "Starting player";
        case 3: return "Loading audio modules";
        case 4: return "Initializing audio service";
        case 5: return "Opening audio port";
        case 6: return "Playing";
        case 7: return "Playback finished";
        case 8: return "Stopped";
        case -1: return "File open failed";
        case -2: return "Unsupported MP3 format";
        case -3: return "Audio module loading failed";
        case -4: return "UserService initialization failed";
        case -5: return "AudioOut initialization failed";
        case -6: return "Audio port opening failed";
        case -7: return "Audio output failed";
        case -8: return "No decodable MP3 frames";
        case -9: return "Player thread creation failed";
        default: return "Unknown playback state";
    }
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

            if (strstr(req, "GET /api/status")) {
                char json[256];
                snprintf(json, sizeof(json),
                    "{\"status\":%d,\"error\":%d,\"message\":\"%s\"}",
                    (int)playStatus, (int)playError,
                    play_status_message(playStatus));
                respond(c, "application/json", json);

            } else if (strstr(req, "GET /api/files")) {
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
