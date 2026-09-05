#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "d_event.h"
#include "d_main.h"
#include "doomkeys.h"
#include "e1-dashboard.h"
#include "e1-key-event.h"
#include "e1-ptz-event.h"
#include "e1-ptz-input.h"
#include "e1-audio-transition.h"
#include "i_flickstick.h"
#include "i_gamepad.h"
#include "i_gyro.h"
#include "i_input.h"
#include "mn_menu.h"
#include "s_sound.h"
#include "sounds.h"

#ifndef E1_STANDALONE_CONTROLLER
int joy_device;
int last_joy_device;
joy_platform_t joy_platform;
boolean joy_invert_forward;
boolean joy_invert_strafe;
boolean joy_invert_turn;
boolean joy_invert_look;
float axes[NUM_AXES];
float gyro_axes[NUM_GYRO_AXES];
int trigger_threshold;

static int ptz_socket = -1;
static struct e1_ptz_input_state ptz_input;
#else
static int key_sender = -1;
static volatile sig_atomic_t controller_stop;
static volatile sig_atomic_t controller_release;
#endif

#define E1_HTTP_DEFAULT_PORT 666
#define E1_HTTP_MAX_CLIENTS 8
#define E1_HTTP_REQUEST_SIZE 1024
#define E1_HTTP_HELD_CODES 64
#define E1_HTTP_CODE_SIZE 24
#define E1_HTTP_DEADMAN_MS UINT64_C(2500)
#define E1_RUNTIME_STATE_DEFAULT "/mnt/tmp/e1-doom/state"

struct e1_http_client {
    int fd;
    char request[E1_HTTP_REQUEST_SIZE];
    size_t request_length;
    const char *response;
    char response_storage[320];
    size_t response_length;
    size_t response_sent;
};

struct e1_http_held_code {
    char code[E1_HTTP_CODE_SIZE];
    int key;
    boolean held;
};

static int http_listener = -1;
static int http_port = E1_HTTP_DEFAULT_PORT;
static struct e1_http_client http_clients[E1_HTTP_MAX_CLIENTS];
static struct e1_http_held_code http_held_codes[E1_HTTP_HELD_CODES];
static uint64_t http_last_activity_ms;
#ifndef E1_STANDALONE_CONTROLLER
static boolean input_initialized;
static boolean exit_audio_started;
static boolean demo_timer_active;
#endif

static void http_release_all(const char *reason);

static const char http_controller_response[] =
    "HTTP/1.1 200 OK\r\n"
    "Connection: close\r\n"
    "Cache-Control: no-store\r\n"
    "Content-Type: text/html; charset=utf-8\r\n\r\n"
    "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'>"
    "<title>DOOM REMOTE :666</title><style>"
    ":root{color-scheme:dark;--bg:#090a08;--panel:#151713;--ink:#b3aa82;"
    "--dim:#85816a;--line:#777b62;--key:#666a52;--hot:#9f201b;--live:#9aac75}"
    "*{box-sizing:border-box}body{margin:0;min-height:100vh;display:grid;"
    "place-items:center;padding:16px;background:var(--bg);color:var(--ink);"
    "font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,Liberation Mono,monospace}"
    "main{width:min(1120px,100%);background:var(--panel);border:1px solid var(--line)}"
    "header{display:flex;align-items:center;justify-content:space-between;gap:24px;"
    "padding:17px 20px;border-bottom:1px solid var(--line)}h1{margin:0;font-size:1.35rem;"
    "line-height:1;letter-spacing:.04em}.status{display:flex;align-items:center;gap:9px;"
    "white-space:nowrap;font-size:.78rem}.dot{width:8px;height:8px;background:var(--dim)}"
    ".online .dot{background:var(--live)}.groups{display:grid;grid-template-columns:repeat(3,1fr)}"
    ".group{min-width:0;padding:18px 20px}.group+.group{border-left:1px solid var(--line)}"
    "h2{margin:0 0 13px;color:var(--dim);font-size:.78rem;letter-spacing:.12em}"
    ".binding{display:flex;align-items:center;justify-content:space-between;gap:12px;"
    "min-height:31px;font-size:.72rem}.binding>span:first-child{color:var(--dim)}"
    ".caps{display:flex;justify-content:flex-end;gap:5px;flex-wrap:wrap}kbd{display:inline-block;"
    "min-width:2.1em;padding:.3em .48em;border:1px solid var(--line);background:var(--key);"
    "box-shadow:inset 1px 1px var(--ink),inset -1px -1px var(--bg);color:var(--bg);"
    "font-family:inherit;font-size:.72rem;font-weight:700;text-align:center}.livebar{display:grid;grid-template-columns:1fr auto;"
    "border-top:1px solid var(--line)}.last{display:flex;align-items:center;gap:18px;"
    "min-width:0;padding:14px 20px;font-size:.78rem}.last label{color:var(--dim)}"
    "output{color:var(--ink);font-weight:700;overflow-wrap:anywhere}.hint{margin-left:auto;"
    "color:var(--dim);font-size:.72rem}.actions{display:flex}button{appearance:none;border:0;"
    "border-left:1px solid var(--line);padding:14px 20px;background:var(--key);color:var(--bg);font-family:inherit;"
    "font-size:.76rem;font-weight:700;"
    "letter-spacing:.06em;cursor:pointer;white-space:nowrap}.start{background:var(--line)}"
    ".camera{background:#641512;color:var(--ink);border-left-color:var(--hot)}"
    "button:hover:not(:disabled){background:var(--ink);color:var(--bg)}.camera:hover:not(:disabled){background:var(--hot);color:var(--ink)}"
    "button:disabled{color:var(--dim);background:var(--panel);cursor:not-allowed}"
    "button:focus-visible{outline:2px solid var(--ink);outline-offset:-4px}"
    "@media(max-width:760px),(orientation:portrait){body{place-items:start center;padding:max(10px,env(safe-area-inset-top)) "
    "max(10px,env(safe-area-inset-right)) max(10px,env(safe-area-inset-bottom)) max(10px,env(safe-area-inset-left))}"
    "main{width:min(560px,100%)}"
    ".groups{grid-template-columns:minmax(0,1fr)}.binding{width:100%;min-width:0}.caps{min-width:0}"
    ".group+.group{border-left:0;border-top:1px solid var(--line)}header{align-items:flex-start;"
    "flex-direction:column;gap:10px}.status{white-space:normal;line-height:1.4}.livebar{grid-template-columns:1fr}.last{align-items:flex-start;"
    "flex-wrap:wrap}.hint{width:100%;margin-left:0}.actions{display:grid;grid-template-columns:1fr 1fr 1fr}"
    "button{min-height:48px;border-left:0;border-top:1px solid var(--line);padding-inline:10px;white-space:normal;line-height:1.15}"
    ".camera{border-top-color:var(--hot)}}"
    "</style></head><body><!-- THESIS: A compact 1995 setup strip makes the keyboard"
    " legible and refuses fake-console spectacle. OWN-WORLD: Flat soot, olive, aged-bone"
    " type, square one-pixel rules, and one blood-red recovery action. STORY: Confirm the"
    " link, learn the real controls, play, and see or clear held input. FIRST VIEWPORT:"
    " Status header, three equal binding columns, then one live-input and release row."
    " FORM: Deathmatch Setup, grounded position 4, seed 043d2efc; approved horizontal-strip"
    " composition. FINISH: unreviewed and undocumented is unfinished; this build ends with"
    " the finish review, the verdict, and DESIGN.md --><main><header><h1>DOOM REMOTE</h1>"
    "<div class='status' id='status' aria-live='polite'><span class='dot'></span>"
    "<span id='status-text'>CONNECTING &middot; PORT 666</span></div></header>"
    "<section class='groups' aria-label='Keyboard controls'><section class='group'><h2>MOVE</h2>"
    "<div class='binding'><span>FORWARD / BACK</span><span class='caps'><kbd>W</kbd><kbd>S</kbd></span></div>"
    "<div class='binding'><span>STRAFE</span><span class='caps'><kbd>A</kbd><kbd>D</kbd></span></div>"
    "<div class='binding'><span>TURN</span><span class='caps'><kbd>&larr;</kbd><kbd>&rarr;</kbd></span></div>"
    "<div class='binding'><span>RUN</span><span class='caps'><kbd>SHIFT</kbd></span></div></section>"
    "<section class='group'><h2>ACTION</h2>"
    "<div class='binding'><span>FIRE</span><span class='caps'><kbd>CTRL</kbd></span></div>"
    "<div class='binding'><span>USE</span><span class='caps'><kbd>SPACE</kbd></span></div>"
    "<div class='binding'><span>WEAPON</span><span class='caps'><kbd>1</kbd><span>&ndash;</span><kbd>9</kbd></span></div>"
    "<div class='binding'><span>FORWARD / BACK</span><span class='caps'><kbd>&uarr;</kbd><kbd>&darr;</kbd></span></div></section>"
    "<section class='group'><h2>MENU</h2>"
    "<div class='binding'><span>MENU / BACK</span><span class='caps'><kbd>ESC</kbd></span></div>"
    "<div class='binding'><span>SELECT</span><span class='caps'><kbd>ENTER</kbd></span></div>"
    "<div class='binding'><span>MAP</span><span class='caps'><kbd>TAB</kbd></span></div>"
    "<div class='binding'><span>SAVE / LOAD</span><span class='caps'><kbd>F2</kbd><kbd>F3</kbd></span></div></section>"
    "</section><footer class='livebar'><div class='last'><label for='pressed'>HELD INPUT</label>"
    "<output id='pressed'>NONE</output><span class='hint'>KEEP THIS TAB FOCUSED &mdash; BLUR RELEASES ALL KEYS</span>"
    "</div><div class='actions'><button type='button' class='start' id='start' disabled>START DOOM</button>"
    "<button type='button' class='camera' id='camera' disabled>RETURN TO CAMERA</button>"
    "<button type='button' id='release'>RELEASE KEYS</button></div></footer></main><script>"
    "const statusEl=document.querySelector('#status'),pressed=document.querySelector('#pressed');"
    "const statusText=document.querySelector('#status-text'),startButton=document.querySelector('#start'),"
    "cameraButton=document.querySelector('#camera'),held=new Set();let runtimeMode='unknown';"
    "const named=new Set(['ArrowUp','ArrowDown','ArrowLeft','ArrowRight','Space','Enter',"
    "'NumpadEnter','Escape','Tab','Backspace','ShiftLeft','ShiftRight','ControlLeft','ControlRight',"
    "'AltLeft','AltRight','CapsLock','Home','End','PageUp','PageDown','Insert','Delete','Pause',"
    "'Minus','Equal','BracketLeft','BracketRight','Backslash','Semicolon','Quote','Backquote',"
    "'Comma','Period','Slash']);"
    "function accepted(c){return named.has(c)||/^Key[A-Z]$/.test(c)||/^Digit[0-9]$/.test(c)||"
    "/^Numpad[0-9]$/.test(c)||/^F(?:[1-9]|1[0-2])$/.test(c)}"
    "function modeLabel(){return runtimeMode.replaceAll('-',' ').toUpperCase()}"
    "function show(ok){statusEl.classList.toggle('online',ok);"
    "statusText.textContent=(ok?'CONNECTED · '+modeLabel():'RECONNECTING')+' · PORT 666';"
    "startButton.disabled=!ok||runtimeMode!=='camera-armed';"
    "cameraButton.disabled=!ok||runtimeMode!=='doom'}"
    "async function send(path){try{const r=await fetch(path,{cache:'no-store',keepalive:true});"
    "show(r.ok);return r}catch(_){show(false);return null}}"
    "async function pollMode(){const r=await send('/mode');if(r&&r.ok){runtimeMode=(await r.text()).trim();show(true)}}"
    "const nice={ControlLeft:'CTRL',ControlRight:'CTRL',ShiftLeft:'SHIFT',ShiftRight:'SHIFT',"
    "AltLeft:'ALT',AltRight:'ALT',Space:'SPACE',Escape:'ESC',NumpadEnter:'ENTER'};"
    "function label(c){return nice[c]||(c.startsWith('Key')?c.slice(3):c.startsWith('Digit')?c.slice(5):"
    "c.startsWith('Arrow')?c.slice(5).toUpperCase():c.toUpperCase())}"
    "function summary(){pressed.textContent=held.size?[...held].map(label).join(' + '):'NONE'}"
    "function releaseAll(){held.clear();summary();send('/release-all')}"
    "async function lifecycle(path){releaseAll();startButton.disabled=true;cameraButton.disabled=true;"
    "const r=await send(path);if(r&&r.ok)setTimeout(pollMode,120)}"
    "addEventListener('keydown',e=>{if(!accepted(e.code))return;e.preventDefault();if(e.repeat||held.has(e.code))return;"
    "held.add(e.code);summary();send('/down/'+encodeURIComponent(e.code))});"
    "addEventListener('keyup',e=>{if(!accepted(e.code))return;e.preventDefault();held.delete(e.code);summary();"
    "send('/up/'+encodeURIComponent(e.code))});"
    "addEventListener('blur',releaseAll);addEventListener('pagehide',releaseAll);"
    "document.addEventListener('visibilitychange',()=>{if(document.hidden)releaseAll()});"
    "document.querySelector('#release').addEventListener('click',releaseAll);"
    "startButton.addEventListener('click',()=>lifecycle('/start'));"
    "cameraButton.addEventListener('click',()=>lifecycle('/camera'));"
    "setInterval(()=>send('/heartbeat'),800);setInterval(pollMode,500);pollMode();"
    "</script></body></html>";

static const char http_ok_response[] =
    "HTTP/1.1 200 OK\r\nConnection: close\r\nCache-Control: no-store\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n\r\nok\n";
static const char http_empty_response[] =
    "HTTP/1.1 204 No Content\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n";
static const char http_conflict_response[] =
    "HTTP/1.1 409 Conflict\r\nConnection: close\r\nCache-Control: no-store\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n\r\ntransition unavailable\n";
static const char http_not_found_response[] =
    "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\nnot found\n";

static uint64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000) +
           (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

static const char *ptz_socket_path(void)
{
    const char *override = getenv("E1_DOOM_PTZ_SOCKET");

    return override != NULL && override[0] == '/' ? override :
        E1_PTZ_SOCKET_PATH;
}

static const char *runtime_state_dir(void)
{
    const char *override = getenv("E1_DOOM_RUNTIME_STATE");

    return override != NULL && override[0] == '/' ? override :
        E1_RUNTIME_STATE_DEFAULT;
}

static boolean runtime_path(char *path, size_t capacity, const char *name)
{
    int length = snprintf(path, capacity, "%s/%s", runtime_state_dir(), name);

    return length > 0 && (size_t)length < capacity;
}

static boolean read_runtime_mode(char *mode, size_t capacity)
{
    char path[256];
    int descriptor;
    ssize_t length;

    if (capacity < 2U || !runtime_path(path, sizeof(path), "runtime.mode")) {
        return false;
    }
    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return false;
    }
    length = read(descriptor, mode, capacity - 1U);
    (void)close(descriptor);
    if (length <= 0 || (size_t)length >= capacity) {
        return false;
    }
    mode[length] = '\0';
    mode[strcspn(mode, "\r\n")] = '\0';
    return mode[0] != '\0';
}

static boolean write_runtime_request(const char *name)
{
    char path[256];
    int descriptor;
    static const char request[] = "1\n";
    ssize_t length;

    if (!runtime_path(path, sizeof(path), name)) {
        return false;
    }
    descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC |
                      O_NOFOLLOW, 0600);
    if (descriptor < 0) {
        return false;
    }
    length = write(descriptor, request, sizeof(request) - 1U);
    (void)close(descriptor);
    return length == (ssize_t)(sizeof(request) - 1U);
}

#ifndef E1_STANDALONE_CONTROLLER
void I_E1ReturnToCamera(void)
{
    char mode[32];

    http_release_all("doom-quit");
    if (read_runtime_mode(mode, sizeof(mode)) && !strcmp(mode, "doom") &&
        write_runtime_request("dashboard.camera")) {
        fprintf(stderr, "E1_DASHBOARD request=return-to-camera source=doom-quit\n");
    } else {
        fprintf(stderr, "E1_DASHBOARD request=return-to-camera rejected source=doom-quit\n");
    }
}
#endif

static void post_key(int key, evtype_t type)
{
#ifdef E1_STANDALONE_CONTROLLER
    struct e1_key_event event;
    struct sockaddr_un destination;

    if (key_sender < 0) {
        return;
    }
    memset(&event, 0, sizeof(event));
    event.magic = E1_KEY_EVENT_MAGIC;
    event.version = E1_KEY_EVENT_VERSION;
    event.key = key;
    event.action = type == ev_keydown ? E1_KEY_EVENT_DOWN : E1_KEY_EVENT_UP;
    memset(&destination, 0, sizeof(destination));
    destination.sun_family = AF_UNIX;
    (void)snprintf(destination.sun_path, sizeof(destination.sun_path), "%s",
                   ptz_socket_path());
    (void)sendto(key_sender, &event, sizeof(event), MSG_DONTWAIT,
                 (const struct sockaddr *)(const void *)&destination,
                 sizeof(destination));
#else
    event_t event;

    memset(&event, 0, sizeof(event));
    event.type = type;
    event.data1.i = key;
    if (type == ev_keydown && (key == KEY_ENTER || key == ' ')) {
        event.data2.i = key;
    }
    D_PostEvent(&event);
#endif
}

static int http_key_for_code(const char *code)
{
    static const struct {
        const char *code;
        int key;
    } special[] = {
        {"ArrowRight", KEY_RIGHTARROW}, {"ArrowLeft", KEY_LEFTARROW},
        {"ArrowUp", KEY_UPARROW}, {"ArrowDown", KEY_DOWNARROW},
        {"Escape", KEY_ESCAPE}, {"Enter", KEY_ENTER},
        {"NumpadEnter", KEY_ENTER}, {"Tab", KEY_TAB},
        {"Backspace", KEY_BACKSPACE}, {"Space", ' '},
        {"ShiftLeft", KEY_RSHIFT}, {"ShiftRight", KEY_RSHIFT},
        {"ControlLeft", KEY_RCTRL}, {"ControlRight", KEY_RCTRL},
        {"AltLeft", KEY_RALT}, {"AltRight", KEY_RALT},
        {"CapsLock", KEY_CAPSLOCK}, {"Home", KEY_HOME},
        {"End", KEY_END}, {"PageUp", KEY_PGUP},
        {"PageDown", KEY_PGDN}, {"Insert", KEY_INS},
        {"Delete", KEY_DEL}, {"Pause", KEY_PAUSE},
        {"Minus", '-'}, {"Equal", '='}, {"BracketLeft", '['},
        {"BracketRight", ']'}, {"Backslash", '\\'}, {"Semicolon", ';'},
        {"Quote", '\''}, {"Backquote", '`'}, {"Comma", ','},
        {"Period", '.'}, {"Slash", '/'},
    };
    size_t index;

    if (strlen(code) == 4U && !memcmp(code, "Key", 3) &&
        code[3] >= 'A' && code[3] <= 'Z') {
        return code[3] - 'A' + 'a';
    }
    if (strlen(code) == 6U && !memcmp(code, "Digit", 5) &&
        code[5] >= '0' && code[5] <= '9') {
        return code[5];
    }
    if (strlen(code) == 7U && !memcmp(code, "Numpad", 6) &&
        code[6] >= '0' && code[6] <= '9') {
        return code[6];
    }
    if (code[0] == 'F' && code[1] >= '1' && code[1] <= '9' &&
        (code[2] == '\0' || (code[1] == '1' && code[2] >= '0' &&
                             code[2] <= '2' && code[3] == '\0'))) {
        int number = atoi(code + 1);
        return KEY_F1 + number - 1;
    }
    for (index = 0; index < sizeof(special) / sizeof(special[0]); ++index) {
        if (!strcmp(code, special[index].code)) {
            return special[index].key;
        }
    }
    return -1;
}

static boolean http_key_is_held(int key)
{
    size_t index;

    for (index = 0; index < E1_HTTP_HELD_CODES; ++index) {
        if (http_held_codes[index].held && http_held_codes[index].key == key) {
            return true;
        }
    }
    return false;
}

static void http_release_all(const char *reason)
{
    boolean released[NUMKEYS];
    size_t index;

    memset(released, 0, sizeof(released));
    for (index = 0; index < E1_HTTP_HELD_CODES; ++index) {
        int key = http_held_codes[index].key;
        if (http_held_codes[index].held && key >= 0 && key < NUMKEYS) {
            released[key] = true;
            http_held_codes[index].held = false;
        }
    }
    for (index = 0; index < NUMKEYS; ++index) {
        if (released[index]) {
            post_key((int)index, ev_keyup);
        }
    }
    if (reason != NULL) {
        fprintf(stderr, "E1_HTTP release-all reason=%s\n", reason);
    }
}

static boolean http_apply_key(const char *code, boolean down)
{
    struct e1_http_held_code *binding = NULL;
    struct e1_http_held_code *free_binding = NULL;
    int key = http_key_for_code(code);
    size_t index;

    if (key < 0 || key >= NUMKEYS || strlen(code) >= E1_HTTP_CODE_SIZE) {
        return false;
    }
    for (index = 0; index < E1_HTTP_HELD_CODES; ++index) {
        if (!strcmp(http_held_codes[index].code, code)) {
            binding = &http_held_codes[index];
            break;
        }
        if (!http_held_codes[index].held && free_binding == NULL) {
            free_binding = &http_held_codes[index];
        }
    }
    if (binding == NULL) {
        binding = free_binding;
        if (binding == NULL) {
            return false;
        }
        memset(binding, 0, sizeof(*binding));
        (void)snprintf(binding->code, sizeof(binding->code), "%s", code);
        binding->key = key;
    }
    http_last_activity_ms = monotonic_ms();
    if (down && !binding->held) {
        boolean already_held = http_key_is_held(key);
        binding->held = true;
        if (!already_held) {
            post_key(key, ev_keydown);
        }
        fprintf(stderr, "E1_HTTP key=%s down doom_key=%#x\n", code, key);
    } else if (!down && binding->held) {
        binding->held = false;
        if (!http_key_is_held(key)) {
            post_key(key, ev_keyup);
        }
        fprintf(stderr, "E1_HTTP key=%s up doom_key=%#x\n", code, key);
    }
    return true;
}

static void close_http_client(struct e1_http_client *client)
{
    if (client->fd >= 0) {
        (void)close(client->fd);
    }
    memset(client, 0, sizeof(*client));
    client->fd = -1;
}

static void http_set_response(struct e1_http_client *client,
                              const char *response)
{
    client->response = response;
    client->response_length = strlen(response);
    client->response_sent = 0;
}

static void http_set_text_response(struct e1_http_client *client,
                                   const char *body)
{
    int length = snprintf(client->response_storage,
                          sizeof(client->response_storage),
                          "HTTP/1.1 200 OK\r\nConnection: close\r\n"
                          "Cache-Control: no-store\r\n"
                          "Content-Type: text/plain; charset=utf-8\r\n\r\n%s\n",
                          body);

    if (length <= 0 || (size_t)length >= sizeof(client->response_storage)) {
        http_set_response(client, http_not_found_response);
        return;
    }
    http_set_response(client, client->response_storage);
}

static void http_route_request(struct e1_http_client *client)
{
    char method[8];
    char path[256];
    char version[16];
    char mode[32];
    const char *code;

    if (sscanf(client->request, "%7s %255s %15s", method, path, version) != 3 ||
        strcmp(method, "GET")) {
        http_set_response(client, http_not_found_response);
        return;
    }
    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        http_set_response(client, http_controller_response);
    } else if (!strcmp(path, "/health")) {
        http_set_response(client, http_ok_response);
    } else if (!strcmp(path, "/mode")) {
        http_set_text_response(client,
            read_runtime_mode(mode, sizeof(mode)) ? mode : "unknown");
    } else if (!strcmp(path, "/start")) {
        if (read_runtime_mode(mode, sizeof(mode)) &&
            !strcmp(mode, "camera-armed") &&
            write_runtime_request(
#ifdef E1_STANDALONE_CONTROLLER
                "runtime.start"
#else
                "dashboard.start"
#endif
            )) {
#ifndef E1_STANDALONE_CONTROLLER
            MN_StartControlPanel();
#endif
            fprintf(stderr, "E1_DASHBOARD request=start source=http\n");
            http_set_response(client, http_empty_response);
        } else {
            http_set_response(client, http_conflict_response);
        }
    } else if (!strcmp(path, "/camera")) {
        http_release_all("return-to-camera");
        if (read_runtime_mode(mode, sizeof(mode)) && !strcmp(mode, "doom") &&
            write_runtime_request("dashboard.camera")) {
            fprintf(stderr, "E1_DASHBOARD request=return-to-camera source=http\n");
            http_set_response(client, http_empty_response);
        } else {
            http_set_response(client, http_conflict_response);
        }
    } else if (!strcmp(path, "/heartbeat")) {
        http_last_activity_ms = monotonic_ms();
        http_set_response(client, http_empty_response);
    } else if (!strcmp(path, "/release-all")) {
        http_release_all("browser");
        http_set_response(client, http_empty_response);
    } else if (!strncmp(path, "/down/", 6)) {
        code = path + 6;
        http_set_response(client,
                          read_runtime_mode(mode, sizeof(mode)) &&
                          !strcmp(mode, "doom") &&
                          http_apply_key(code, true) ?
                          http_empty_response : http_not_found_response);
    } else if (!strncmp(path, "/up/", 4)) {
        code = path + 4;
        http_set_response(client,
                          read_runtime_mode(mode, sizeof(mode)) &&
                          !strcmp(mode, "doom") &&
                          http_apply_key(code, false) ?
                          http_empty_response : http_not_found_response);
    } else {
        http_set_response(client, http_not_found_response);
    }
}

static void poll_http_clients(void)
{
    size_t index;

    for (index = 0; index < E1_HTTP_MAX_CLIENTS; ++index) {
        struct e1_http_client *client = &http_clients[index];
        ssize_t count;
        if (client->fd < 0) {
            continue;
        }
        if (client->response != NULL) {
#ifdef MSG_NOSIGNAL
            count = send(client->fd,
                         client->response + client->response_sent,
                         client->response_length - client->response_sent,
                         MSG_NOSIGNAL);
#else
            count = send(client->fd,
                         client->response + client->response_sent,
                         client->response_length - client->response_sent, 0);
#endif
            if (count > 0) {
                client->response_sent += (size_t)count;
                if (client->response_sent == client->response_length) {
                    close_http_client(client);
                }
            } else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                close_http_client(client);
            }
            continue;
        }
        count = recv(client->fd,
                     client->request + client->request_length,
                     sizeof(client->request) - client->request_length - 1U, 0);
        if (count > 0) {
            client->request_length += (size_t)count;
            client->request[client->request_length] = '\0';
            if (strchr(client->request, '\n') != NULL ||
                client->request_length + 1U == sizeof(client->request)) {
                http_route_request(client);
            }
        } else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            close_http_client(client);
        }
    }
}

static void poll_http_listener(void)
{
    unsigned int accepted;

    if (http_listener < 0) {
        return;
    }
    for (accepted = 0; accepted < E1_HTTP_MAX_CLIENTS; ++accepted) {
        int fd = accept(http_listener, NULL, NULL);
        size_t index;
        if (fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (fd < 0) {
            fprintf(stderr, "E1_HTTP accept-error=%d\n", errno);
            break;
        }
        if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0 ||
            fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
            (void)close(fd);
            continue;
        }
        for (index = 0; index < E1_HTTP_MAX_CLIENTS; ++index) {
            if (http_clients[index].fd < 0) {
                http_clients[index].fd = fd;
                break;
            }
        }
        if (index == E1_HTTP_MAX_CLIENTS) {
            (void)close(fd);
        }
    }
    poll_http_clients();
}

static void close_http_server(void)
{
    size_t index;

    if (http_listener >= 0) {
        (void)close(http_listener);
        http_listener = -1;
    }
    for (index = 0; index < E1_HTTP_MAX_CLIENTS; ++index) {
        close_http_client(&http_clients[index]);
    }
}

static void init_http_server(void)
{
    struct sockaddr_in address;
    const char *port_env = getenv("E1_DOOM_HTTP_PORT");
    int reuse = 1;
    size_t index;

    for (index = 0; index < E1_HTTP_MAX_CLIENTS; ++index) {
        http_clients[index].fd = -1;
    }
    if (port_env != NULL) {
        char *end = NULL;
        long parsed = strtol(port_env, &end, 10);
        if (end == port_env || *end != '\0' || parsed < 0 || parsed > 65535) {
            fprintf(stderr, "E1_HTTP invalid-port=%s\n", port_env);
            return;
        }
        http_port = (int)parsed;
    }
    if (http_port == 0) {
        fprintf(stderr, "E1_HTTP disabled\n");
        return;
    }
    http_listener = socket(AF_INET, SOCK_STREAM, 0);
    if (http_listener < 0) {
        fprintf(stderr, "E1_HTTP socket-error=%d\n", errno);
        return;
    }
    (void)setsockopt(http_listener, SOL_SOCKET, SO_REUSEADDR, &reuse,
                     sizeof(reuse));
    if (fcntl(http_listener, F_SETFL, O_NONBLOCK) != 0 ||
        fcntl(http_listener, F_SETFD, FD_CLOEXEC) != 0) {
        fprintf(stderr, "E1_HTTP socket-flags-error=%d\n", errno);
        close_http_server();
        return;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)http_port);
    if (bind(http_listener, (const struct sockaddr *)(const void *)&address,
             sizeof(address)) != 0 || listen(http_listener, 8) != 0) {
        fprintf(stderr, "E1_HTTP bind-error port=%d errno=%d\n", http_port,
                errno);
        close_http_server();
        return;
    }
    fprintf(stderr,
            "E1_HTTP ready address=0.0.0.0 port=%d deadman_ms=%llu\n",
            http_port, (unsigned long long)E1_HTTP_DEADMAN_MS);
}

#ifdef E1_STANDALONE_CONTROLLER
static void controller_request_stop(int signal_number)
{
    (void)signal_number;
    controller_stop = 1;
}

static void controller_request_release(int signal_number)
{
    (void)signal_number;
    controller_release = 1;
}

static void remove_runtime_marker(const char *name)
{
    char path[256];

    if (runtime_path(path, sizeof(path), name)) {
        (void)unlink(path);
    }
}

int main(void)
{
    struct timespec delay = {0, 20000000L};

    signal(SIGINT, controller_request_stop);
    signal(SIGTERM, controller_request_stop);
    signal(SIGHUP, controller_request_stop);
    signal(SIGUSR1, controller_request_release);
    key_sender = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (key_sender < 0 ||
        fcntl(key_sender, F_SETFL, O_NONBLOCK) != 0 ||
        fcntl(key_sender, F_SETFD, FD_CLOEXEC) != 0) {
        fprintf(stderr, "E1_CONTROLLER key-socket-error=%d\n", errno);
        return 1;
    }
    init_http_server();
    if (http_listener < 0 || !write_runtime_request("controller.ready")) {
        close_http_server();
        (void)close(key_sender);
        key_sender = -1;
        return 1;
    }
    fprintf(stderr, "E1_CONTROLLER ready\n");
    while (!controller_stop) {
        uint64_t now = monotonic_ms();

        poll_http_listener();
        if (controller_release) {
            controller_release = 0;
            http_release_all("runtime-transition");
            http_last_activity_ms = 0;
        }
        if (http_last_activity_ms != 0U &&
            now - http_last_activity_ms > E1_HTTP_DEADMAN_MS) {
            http_release_all("heartbeat-timeout");
            http_last_activity_ms = 0;
        }
        (void)nanosleep(&delay, NULL);
    }
    http_release_all("controller-stop");
    close_http_server();
    (void)close(key_sender);
    key_sender = -1;
    remove_runtime_marker("controller.ready");
    fprintf(stderr, "E1_CONTROLLER stopped\n");
    return 0;
}
#else
struct key_mapping {
    uint32_t mask;
    int key;
};

static const struct key_mapping key_mappings[] = {
    {E1_PTZ_KEY_UP, KEY_UPARROW},
    {E1_PTZ_KEY_DOWN, KEY_DOWNARROW},
    {E1_PTZ_KEY_LEFT, KEY_LEFTARROW},
    {E1_PTZ_KEY_RIGHT, KEY_RIGHTARROW},
    {E1_PTZ_KEY_FIRE, KEY_RCTRL},
    {E1_PTZ_KEY_USE, ' '},
    {E1_PTZ_KEY_ENTER, KEY_ENTER},
    {E1_PTZ_KEY_ESCAPE, KEY_ESCAPE},
};

static void post_delta(struct e1_ptz_input_delta delta, int32_t command)
{
    size_t index;

    for (index = 0; index < sizeof(key_mappings) / sizeof(key_mappings[0]);
         ++index) {
        if ((delta.release & key_mappings[index].mask) != 0U) {
            post_key(key_mappings[index].key, ev_keyup);
        }
    }
    for (index = 0; index < sizeof(key_mappings) / sizeof(key_mappings[0]);
         ++index) {
        if ((delta.press & key_mappings[index].mask) != 0U) {
            post_key(key_mappings[index].key, ev_keydown);
        }
    }
    if (delta.press != 0U || delta.release != 0U) {
        fprintf(stderr,
                "E1_INPUT command=%d press=%#x release=%#x held=%#x\n",
                command, delta.press, delta.release, ptz_input.held);
    }
}

static void close_ptz_socket(void)
{
    if (ptz_socket >= 0) {
        (void)close(ptz_socket);
        ptz_socket = -1;
    }
    (void)unlink(ptz_socket_path());
}

static void close_input(void)
{
    close_http_server();
    close_ptz_socket();
}

static void poll_exit_transition_audio(void)
{
    char mode[32];

    if (!read_runtime_mode(mode, sizeof(mode))) {
        demo_timer_active = false;
        return;
    }
    demo_timer_active = !strcmp(mode, "doom");
    if (!exit_audio_started && !strcmp(mode, "exit-melt")) {
        S_StopMusic();
        S_StopChannels();
        I_E1SetExitAudioOnly(true);
        S_StartSound(NULL, sfx_pldeth);
        exit_audio_started = true;
        fprintf(stderr, "E1_EXIT_AUDIO started sfx=pldeth\n");
    }
}

boolean I_E1DemoTimerActive(void)
{
    return demo_timer_active;
}

void I_StartFrame(void) {}
void I_StartTic(void)
{
    uint64_t now = monotonic_ms();

    poll_exit_transition_audio();
    poll_http_listener();
    if (http_last_activity_ms != 0 &&
        now - http_last_activity_ms > E1_HTTP_DEADMAN_MS) {
        http_release_all("heartbeat-timeout");
        http_last_activity_ms = 0;
    }
    if (ptz_socket >= 0) {
        unsigned int received;

        for (received = 0; received < 64U; ++received) {
            union {
                struct e1_ptz_event ptz;
                struct e1_key_event key;
            } packet;
            ssize_t length = recv(ptz_socket, &packet, sizeof(packet), 0);

            if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            if (length < 0) {
                fprintf(stderr, "E1_INPUT recv-error=%d\n", errno);
                break;
            }
            if (length == (ssize_t)sizeof(packet.key) &&
                packet.key.magic == E1_KEY_EVENT_MAGIC &&
                packet.key.version == E1_KEY_EVENT_VERSION &&
                packet.key.key >= 0 && packet.key.key < NUMKEYS &&
                (packet.key.action == E1_KEY_EVENT_UP ||
                 packet.key.action == E1_KEY_EVENT_DOWN)) {
                post_key(packet.key.key,
                         packet.key.action == E1_KEY_EVENT_DOWN
                             ? ev_keydown : ev_keyup);
                fprintf(stderr, "E1_INPUT http-key=%#x action=%d\n",
                        packet.key.key, packet.key.action);
                continue;
            }
            if (length != (ssize_t)sizeof(packet.ptz) ||
                packet.ptz.magic != E1_PTZ_EVENT_MAGIC ||
                packet.ptz.version != E1_PTZ_EVENT_VERSION) {
                fprintf(stderr, "E1_INPUT invalid-datagram length=%ld\n",
                        (long)length);
                continue;
            }
            post_delta(e1_ptz_apply_command(&ptz_input, packet.ptz.command,
                                            now),
                       packet.ptz.command);
        }
    }
    post_delta(e1_ptz_expire(&ptz_input, now), -1);
}
void I_StartDisplay(void) {}
int I_GetAxisState(int axis) { (void)axis; return 0; }
boolean I_UseGamepad(void) { return false; }
boolean I_GyroSupported(void) { return false; }
void I_GetFaceButtons(int *buttons) { memset(buttons, 0, 4 * sizeof(*buttons)); }
void I_FlushGamepadSensorEvents(void) {}
void I_FlushGamepadEvents(void) {}
void I_SetSensorEventState(boolean condition) { (void)condition; }
void I_SetSensorsEnabled(boolean condition) { (void)condition; }
void I_InitGamepad(void) {}
void I_OpenGamepad(int device_index) { (void)device_index; }
void I_CloseGamepad(SDL_JoystickID instance_id) { (void)instance_id; }
void I_UpdateGamepadDevice(boolean gamepad_input) { (void)gamepad_input; }
const char **I_GamepadDeviceList(void) { return NULL; }
boolean I_GamepadDevices(void) { return false; }
void I_ReadMouse(void) {}
void I_ReadGyro(void) {}
void I_UpdateGamepad(enum evtype_e type, boolean axis_buttons)
{ (void)type; (void)axis_buttons; }
void I_DelayEvent(void) {}
void I_HandleSensorEvent(SDL_Event *event) { (void)event; }
void I_HandleGamepadEvent(SDL_Event *event, boolean menu)
{ (void)event; (void)menu; }
void I_HandleKeyboardEvent(SDL_Event *event) { (void)event; }
void I_HandleMouseEvent(SDL_Event *event) { (void)event; }
void I_InitKeyboard(void)
{
    struct sockaddr_un address;
    const char *path = ptz_socket_path();
    if (input_initialized) {
        return;
    }
    input_initialized = true;
    init_http_server();
    ptz_socket = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (ptz_socket < 0) {
        fprintf(stderr, "E1_INPUT socket-error=%d\n", errno);
        (void)atexit(close_input);
        return;
    }
    if (fcntl(ptz_socket, F_SETFL, O_NONBLOCK) != 0 ||
        fcntl(ptz_socket, F_SETFD, FD_CLOEXEC) != 0) {
        fprintf(stderr, "E1_INPUT socket-flags-error=%d\n", errno);
        close_ptz_socket();
        return;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(address.sun_path)) {
        fprintf(stderr, "E1_INPUT socket-path-too-long\n");
        close_ptz_socket();
        return;
    }
    (void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    (void)unlink(path);
    if (bind(ptz_socket, (const struct sockaddr *)(const void *)&address,
             sizeof(address)) != 0) {
        fprintf(stderr, "E1_INPUT bind-error=%d\n", errno);
        close_ptz_socket();
        return;
    }
    (void)atexit(close_input);
    fprintf(stderr, "E1_INPUT ready path=%s timeout_ms=%llu\n", path,
            (unsigned long long)E1_PTZ_INPUT_TIMEOUT_MS);
}
void I_StartTextInput(void) {}
void I_StopTextInput(void) {}
void I_BindKeyboardVariables(void) {}

void I_GetRawAxesScaleMenu(boolean move, float *scale, float *limit)
{ (void)move; *scale = 1.0f; *limit = 1.0f; }
boolean I_GamepadEnabled(void) { return false; }
boolean I_UseStickLayout(void) { return false; }
boolean I_StandardLayout(void) { return true; }
boolean I_RampTimeEnabled(void) { return false; }
void I_CalcRadial(axes_t *axis, float *xaxis, float *yaxis)
{ (void)axis; *xaxis = 0.0f; *yaxis = 0.0f; }
void I_CalcGamepadAxes(boolean strafe) { (void)strafe; }
void I_UpdateAxesData(const struct event_s *event) { (void)event; }
void I_ResetGamepadAxes(void) { memset(axes, 0, sizeof(axes)); }
void I_ResetGamepadState(void) {}
void I_ResetGamepad(void) {}
void I_BindGamepadVariables(void) {}

gyro_calibration_state_t I_GetGyroCalibrationState(void)
{ return GYRO_CALIBRATION_INACTIVE; }
void I_LoadGyroCalibration(void) {}
void I_UpdateGyroCalibrationState(void) {}
boolean I_GyroEnabled(void) { return false; }
boolean I_GyroAcceleration(void) { return false; }
void I_SetStickMoving(boolean condition) { (void)condition; }
void I_GetRawGyroScaleMenu(float *scale, float *limit)
{ *scale = 1.0f; *limit = 1.0f; }
void I_CalcGyroAxes(boolean strafe) { (void)strafe; }
void I_UpdateGyroData(const struct event_s *event) { (void)event; }
void I_UpdateAccelData(const float *data) { (void)data; }
void I_ResetGyroAxes(void) { memset(gyro_axes, 0, sizeof(gyro_axes)); }
void I_ResetGyro(void) {}
void I_UpdateGyroSteadying(void) {}
void I_RefreshGyroSettings(void) {}
void I_BindGyroVaribales(void) {}

boolean I_FlickStickActive(void) { return false; }
boolean I_PendingFlickStickReset(void) { return false; }
void I_SetPendingFlickStickReset(boolean condition) { (void)condition; }
void I_CalcFlickStick(struct axes_s *axis, float *xaxis, float *yaxis)
{ (void)axis; *xaxis = 0.0f; *yaxis = 0.0f; }
void I_ResetFlickStick(void) {}
void I_RefreshFlickStickSettings(void) {}
void I_BindFlickStickVariables(void) {}
#endif
