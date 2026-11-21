/*
 * FrameLab Detect: a tiny Windows program that reads the processor, graphics card, RAM and screen resolution
 * and hands them to the FrameLab page that is waiting for them. Nothing to click: it scans, sends, shows the
 * result and closes by itself.
 *
 * How the values reach the page:
 *  - The site names the download FrameLab-Detect-<locale>-<tool>-<code>.exe, where <code> is a random one-time
 *    code the page created (16 characters a-z, 2-7). The program sends the values to
 *    https://framelab-fps.com/api/detect/<code>; the page asks the same address for them and fills them in.
 *    The server keeps them in memory only, for a few minutes, and forgets them once the page has read them.
 *  - Without a code (file renamed, or no internet), it opens the site in the browser with the values in the
 *    address fragment (#...), which browsers never send to the server.
 *
 * Only model names and sizes are read: no serial numbers, MAC addresses, user or computer names, installed
 * software. Nothing is written to disk or the registry.
 *
 * --print      writes the fallback address to standard output instead (testing, support)
 * --site URL   talks to another FrameLab address, e.g. a local development server
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <dxgi.h>
#include <winhttp.h>
#include <wchar.h>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

#define DEFAULT_SITE L"https://framelab-fps.com"
#define MAX_GPUS 3
#define CODE_LEN 16
#define CLOSE_AFTER_MS 8000

typedef enum { L_EN, L_UK, L_DE, L_RU } Lang;

typedef struct {
    const wchar_t *title, *scanning, *sending, *done, *doneBrowser, *failed, *cpu, *gpu, *ram, *res, *unknown,
        *closing, *footer, *privacy;
} Text;

static const Text TEXT[] = {
    [L_EN] = {
        L"FrameLab Detect", L"Scanning your PC…", L"Sending to FrameLab…",
        L"Done! Your results are on the FrameLab page.", L"Done! FrameLab opened in your browser with your PC.",
        L"Could not reach FrameLab. Please check your internet connection and run the program again.",
        L"Processor", L"Graphics card", L"RAM", L"Screen", L"not detected",
        L"This window closes in %d s.",
        L"Only model names and sizes are sent, no personal data. Nothing is installed or stored.",
        L"Privacy policy",
    },
    [L_UK] = {
        L"FrameLab Detect", L"Скануємо ваш ПК…", L"Передаємо на FrameLab…",
        L"Готово! Результат уже на сторінці FrameLab.", L"Готово! FrameLab відкрито у браузері з вашим ПК.",
        L"Не вдалося з'єднатися з FrameLab. Перевірте інтернет і запустіть програму ще раз.",
        L"Процесор", L"Відеокарта", L"Оперативна пам'ять", L"Екран", L"не визначено",
        L"Вікно закриється через %d с.",
        L"Передаються лише назви моделей і обсяги, без особистих даних. Нічого не встановлюється й не зберігається.",
        L"Політика конфіденційності",
    },
    [L_DE] = {
        L"FrameLab Detect", L"Dein PC wird gescannt…", L"Wird an FrameLab gesendet…",
        L"Fertig! Das Ergebnis steht auf der FrameLab-Seite.", L"Fertig! FrameLab wurde mit deinem PC im Browser geöffnet.",
        L"FrameLab ist nicht erreichbar. Bitte prüfe die Internetverbindung und starte das Programm erneut.",
        L"Prozessor", L"Grafikkarte", L"Arbeitsspeicher", L"Bildschirm", L"nicht erkannt",
        L"Dieses Fenster schließt sich in %d s.",
        L"Gesendet werden nur Modellnamen und Größen, keine persönlichen Daten. Nichts wird installiert oder gespeichert.",
        L"Datenschutzerklärung",
    },
    [L_RU] = {
        L"FrameLab Detect", L"Сканируем ваш ПК…", L"Передаём на FrameLab…",
        L"Готово! Результат уже на странице FrameLab.", L"Готово! FrameLab открыт в браузере с вашим ПК.",
        L"Не удалось связаться с FrameLab. Проверьте интернет и запустите программу ещё раз.",
        L"Процессор", L"Видеокарта", L"Оперативная память", L"Экран", L"не определено",
        L"Окно закроется через %d с.",
        L"Передаются только названия моделей и объёмы, без личных данных. Ничего не устанавливается и не сохраняется.",
        L"Политика конфиденциальности",
    },
};

static const wchar_t *LOCALE_CODE[] = { [L_EN] = L"en", [L_UK] = L"uk", [L_DE] = L"de", [L_RU] = L"ru" };

typedef struct {
    wchar_t cpu[128];
    wchar_t gpu[MAX_GPUS][128];
    int gpuVramMB[MAX_GPUS];
    int gpuCount;
    int ramGB;
    int width, height;
} Specs;

/* ---------- detection ---------- */

static void trim(wchar_t *s) {
    wchar_t *p = s;
    while (*p == L' ' || *p == L'\t') p++;
    if (p != s) memmove(s, p, (wcslen(p) + 1) * sizeof(wchar_t));
    size_t n = wcslen(s);
    while (n > 0 && (s[n - 1] == L' ' || s[n - 1] == L'\t' || s[n - 1] == L'\0')) s[--n] = L'\0';
    /* Collapse runs of spaces (older Intel names pad with them). */
    wchar_t *r = s, *w = s;
    for (; *r; r++) if (!(*r == L' ' && w > s && w[-1] == L' ')) *w++ = *r;
    *w = L'\0';
}

static void detectCpu(Specs *s) {
    DWORD size = sizeof(s->cpu);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                     RRF_RT_REG_SZ, NULL, s->cpu, &size) != ERROR_SUCCESS)
        s->cpu[0] = L'\0';
    trim(s->cpu);
}

/* Real graphics adapters, the one with the most dedicated memory first (the discrete card in gaming laptops). */
static void detectGpus(Specs *s) {
    IDXGIFactory1 *factory = NULL;
    SIZE_T mem[MAX_GPUS] = { 0 };
    if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory))) {
        IDXGIAdapter1 *adapter = NULL;
        for (UINT i = 0; IDXGIFactory1_EnumAdapters1(factory, i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                d.VendorId != 0x1414 /* Microsoft Basic Render Driver */) {
                int dup = 0;
                for (int k = 0; k < s->gpuCount; k++) if (wcscmp(s->gpu[k], d.Description) == 0) dup = 1;
                if (!dup) {
                    /* Insert sorted by dedicated memory, keeping at most MAX_GPUS. */
                    int pos = s->gpuCount;
                    while (pos > 0 && mem[pos - 1] < d.DedicatedVideoMemory) pos--;
                    if (pos < MAX_GPUS) {
                        int last = s->gpuCount < MAX_GPUS ? s->gpuCount : MAX_GPUS - 1;
                        for (int k = last; k > pos; k--) {
                            mem[k] = mem[k - 1];
                            s->gpuVramMB[k] = s->gpuVramMB[k - 1];
                            wcscpy(s->gpu[k], s->gpu[k - 1]);
                        }
                        mem[pos] = d.DedicatedVideoMemory;
                        s->gpuVramMB[pos] = (int)(d.DedicatedVideoMemory >> 20);
                        lstrcpynW(s->gpu[pos], d.Description, 128);
                        trim(s->gpu[pos]);
                        if (s->gpuCount < MAX_GPUS) s->gpuCount++;
                    }
                }
            }
            IDXGIAdapter1_Release(adapter);
        }
        IDXGIFactory1_Release(factory);
    }
    if (s->gpuCount == 0) {
        /* Fallback without DXGI: the display device the desktop runs on. */
        DISPLAY_DEVICEW dd = { .cb = sizeof(dd) };
        for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); i++) {
            if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
                lstrcpynW(s->gpu[0], dd.DeviceString, 128);
                trim(s->gpu[0]);
                s->gpuCount = 1;
                break;
            }
        }
    }
}

static void detectRam(Specs *s) {
    ULONGLONG kb = 0;
    /* Installed memory (what the user bought), not the slightly smaller amount Windows can use. */
    if (GetPhysicallyInstalledSystemMemory(&kb) && kb > 0) {
        s->ramGB = (int)((kb + 512 * 1024) / (1024 * 1024));
        return;
    }
    MEMORYSTATUSEX m = { .dwLength = sizeof(m) };
    if (GlobalMemoryStatusEx(&m)) s->ramGB = (int)((m.ullTotalPhys + (512ull << 20)) >> 30);
}

static void detectResolution(Specs *s) {
    DEVMODEW dm = { .dmSize = sizeof(dm) };
    /* The display mode of the primary monitor, independent of Windows scaling. */
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        s->width = (int)dm.dmPelsWidth;
        s->height = (int)dm.dmPelsHeight;
    }
}

/* ---------- what the site asked for, from the file name ---------- */

typedef struct {
    Lang lang;
    const wchar_t *path;  /* tool to open in the browser fallback */
    wchar_t code[CODE_LEN + 1];
    wchar_t site[256];
    BOOL print;
} Request;

static Lang langFromSystem(void) {
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_UKRAINIAN: return L_UK;
    case LANG_GERMAN: return L_DE;
    case LANG_RUSSIAN: return L_RU;
    default: return L_EN;
    }
}

static BOOL isCode(const wchar_t *t) {
    if (wcslen(t) != CODE_LEN) return FALSE;
    for (; *t; t++) if (!((*t >= L'a' && *t <= L'z') || (*t >= L'2' && *t <= L'7'))) return FALSE;
    return TRUE;
}

static void readRequest(Request *r, PWSTR args) {
    wchar_t self[MAX_PATH];
    r->lang = langFromSystem();
    r->path = L"/fps-calculator";
    r->code[0] = L'\0';
    wcscpy(r->site, DEFAULT_SITE);
    r->print = args && wcsstr(args, L"--print") != NULL;
    const wchar_t *site = args ? wcsstr(args, L"--site ") : NULL;
    if (site) {
        site += 7;
        size_t n = wcscspn(site, L" ");
        if (n > 0 && n < ARRAYSIZE(r->site)) {
            wcsncpy(r->site, site, n);
            r->site[n] = L'\0';
            while (n > 0 && r->site[n - 1] == L'/') r->site[--n] = L'\0';
        }
    }

    if (!GetModuleFileNameW(NULL, self, MAX_PATH)) return;
    wchar_t *name = wcsrchr(self, L'\\');
    name = name ? name + 1 : self;
    CharLowerW(name);
    /* Tokens are separated by '-', '_', ' ', '.', '(' or ')'; browsers may add " (1)". */
    wchar_t *end = name + wcslen(name);
    for (wchar_t *p = name; p < end; p++) if (wcschr(L"-_ .()", *p)) *p = L'\0';
    for (wchar_t *t = name; t < end; t += wcslen(t) + 1) {
        if (!*t) continue;
        if (!wcscmp(t, L"uk")) r->lang = L_UK;
        else if (!wcscmp(t, L"en")) r->lang = L_EN;
        else if (!wcscmp(t, L"de")) r->lang = L_DE;
        else if (!wcscmp(t, L"ru")) r->lang = L_RU;
        else if (!wcscmp(t, L"fps")) r->path = L"/fps-calculator";
        else if (!wcscmp(t, L"run")) r->path = L"/what-can-i-run";
        else if (!wcscmp(t, L"psu")) r->path = L"/psu-calculator";
        else if (isCode(t)) wcscpy(r->code, t);
    }
}

/* ---------- the values as "fl-pc&v=1&cpu=…" (address fragment and request body alike) ---------- */

typedef struct { wchar_t buf[2048]; size_t len; } Buf;

static void put(Buf *u, const wchar_t *s) {
    while (*s && u->len + 1 < ARRAYSIZE(u->buf)) u->buf[u->len++] = *s++;
    u->buf[u->len] = L'\0';
}

/* Percent-encodes UTF-8, keeping only unreserved characters as they are. */
static void putEncoded(Buf *u, const wchar_t *s) {
    char utf8[512];
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, utf8, sizeof(utf8), NULL, NULL);
    static const wchar_t HEX[] = L"0123456789ABCDEF";
    for (int i = 0; i < n - 1; i++) {
        unsigned char c = (unsigned char)utf8[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~') {
            wchar_t one[2] = { (wchar_t)c, 0 };
            put(u, one);
        } else {
            wchar_t three[4] = { L'%', HEX[c >> 4], HEX[c & 15], 0 };
            put(u, three);
        }
    }
}

static void putSpecs(Buf *u, const Specs *s) {
    wchar_t num[32];
    put(u, L"fl-pc&v=1");
    if (s->cpu[0]) { put(u, L"&cpu="); putEncoded(u, s->cpu); }
    for (int i = 0; i < s->gpuCount; i++) {
        put(u, L"&gpu=");
        putEncoded(u, s->gpu[i]);
        /* Dedicated video memory in MB, same order: tells apart variants such as Arc A770 8GB / 16GB. */
        swprintf(num, 32, L"&vram=%d", s->gpuVramMB[i]);
        put(u, num);
    }
    if (s->ramGB > 0) { swprintf(num, 32, L"&ram=%d", s->ramGB); put(u, num); }
    if (s->width > 0 && s->height > 0) { swprintf(num, 32, L"&res=%dx%d", s->width, s->height); put(u, num); }
}

static void browserUrl(Buf *u, const Specs *s, const Request *r) {
    u->len = 0;
    put(u, r->site);
    put(u, L"/");
    put(u, LOCALE_CODE[r->lang]);
    put(u, r->path);
    put(u, L"#");
    putSpecs(u, s);
}

/* ---------- sending to the waiting page ---------- */

/* POST <site>/api/detect/<code> with a form-encoded body; TRUE on a 2xx answer. */
static BOOL postToSite(const Request *r, const wchar_t *body) {
    wchar_t host[256] = L"";
    URL_COMPONENTS uc = { .dwStructSize = sizeof(uc), .lpszHostName = host, .dwHostNameLength = ARRAYSIZE(host) };
    if (!WinHttpCrackUrl(r->site, 0, 0, &uc)) return FALSE;
    BOOL secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

    wchar_t path[64];
    swprintf(path, ARRAYSIZE(path), L"/api/detect/%ls", r->code);
    char data[2048];
    int len = WideCharToMultiByte(CP_UTF8, 0, body, -1, data, sizeof(data), NULL, NULL) - 1;
    if (len <= 0) return FALSE;

    BOOL ok = FALSE;
    HINTERNET session = WinHttpOpen(L"FrameLab-Detect/1.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) /* before Windows 8.1 */
        session = WinHttpOpen(L"FrameLab-Detect/1.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return FALSE;
    WinHttpSetTimeouts(session, 5000, 8000, 8000, 8000);
    HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    HINTERNET req = connect ? WinHttpOpenRequest(connect, L"POST", path, NULL, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
                            : NULL;
    if (req && WinHttpSendRequest(req, L"Content-Type: application/x-www-form-urlencoded\r\n", (DWORD)-1L, data,
                                  (DWORD)len, (DWORD)len, 0) &&
        WinHttpReceiveResponse(req, NULL)) {
        DWORD status = 0, size = sizeof(status);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &size, WINHTTP_NO_HEADER_INDEX))
            ok = status >= 200 && status < 300;
    }
    if (req) WinHttpCloseHandle(req);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

static BOOL postWithRetry(const Request *r, const wchar_t *body) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (postToSite(r, body)) return TRUE;
        Sleep(1000u << attempt);
    }
    return FALSE;
}

/* ---------- window: a task dialog with a progress bar, updated by a worker thread ---------- */

typedef struct {
    Request req;
    const Text *t;
    Specs specs;
    HWND dialog;
    volatile LONG finishedAt; /* dialog tick count when the work ended, 0 while running */
    volatile LONG failed;
    wchar_t content[2048];
    wchar_t heading[256];
    wchar_t privacyLink[512]; /* <a href="…/privacy">…</a>, shown in the footer */
} App;

static App app;

static void line(wchar_t *out, size_t cap, const wchar_t *label, const wchar_t *value) {
    size_t n = wcslen(out);
    swprintf(out + n, cap - n, L"%ls: %ls\n", label, value);
}

static void describe(wchar_t *out, size_t cap, const Specs *s, const Text *t) {
    wchar_t value[64];
    out[0] = L'\0';
    line(out, cap, t->cpu, s->cpu[0] ? s->cpu : t->unknown);
    if (s->gpuCount == 0) line(out, cap, t->gpu, t->unknown);
    for (int i = 0; i < s->gpuCount; i++) line(out, cap, t->gpu, s->gpu[i]);
    if (s->ramGB > 0) swprintf(value, 64, L"%d GB", s->ramGB); else wcscpy(value, t->unknown);
    line(out, cap, t->ram, value);
    if (s->width > 0) swprintf(value, 64, L"%d × %d", s->width, s->height); else wcscpy(value, t->unknown);
    line(out, cap, t->res, value);
}

static void setText(int element, const wchar_t *text) {
    if (app.dialog) SendMessageW(app.dialog, TDM_SET_ELEMENT_TEXT, (WPARAM)element, (LPARAM)text);
}

static DWORD WINAPI work(LPVOID unused) {
    (void)unused;
    const Text *t = app.t;
    BOOL online = app.req.code[0] != L'\0';
    /* The page switches to "scanning" as soon as it hears from the program. */
    if (online) online = postToSite(&app.req, L"fl-pc&v=1&status=scanning");

    detectCpu(&app.specs);
    detectGpus(&app.specs);
    detectRam(&app.specs);
    detectResolution(&app.specs);
    Sleep(700); /* long enough to read the window */
    describe(app.content, ARRAYSIZE(app.content), &app.specs, t);
    setText(TDE_CONTENT, app.content);

    Buf body = { .len = 0 };
    putSpecs(&body, &app.specs);
    put(&body, L"&status=done");
    BOOL sent = FALSE;
    if (online) {
        setText(TDE_MAIN_INSTRUCTION, t->sending);
        sent = postWithRetry(&app.req, body.buf);
    }
    if (!sent) {
        /* No waiting page (or it cannot be reached): open FrameLab with the values in the address fragment. */
        Buf url = { .len = 0 };
        browserUrl(&url, &app.specs, &app.req);
        if ((INT_PTR)ShellExecuteW(NULL, L"open", url.buf, NULL, NULL, SW_SHOWNORMAL) <= 32) {
            InterlockedExchange(&app.failed, 1);
        }
    }

    if (app.dialog) {
        SendMessageW(app.dialog, TDM_SET_MARQUEE_PROGRESS_BAR, FALSE, 0);
        SendMessageW(app.dialog, TDM_SET_PROGRESS_BAR_POS, 100, 0);
        if (app.failed) SendMessageW(app.dialog, TDM_SET_PROGRESS_BAR_STATE, PBST_ERROR, 0);
    }
    setText(TDE_MAIN_INSTRUCTION, app.failed ? t->failed : sent ? t->done : t->doneBrowser);
    InterlockedExchange(&app.finishedAt, (LONG)GetTickCount());
    return 0;
}

static HRESULT CALLBACK onDialog(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LONG_PTR data) {
    (void)lParam; (void)data;
    if (msg == TDN_CREATED) {
        app.dialog = hwnd;
        SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30);
        HANDLE thread = CreateThread(NULL, 0, work, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
    } else if (msg == TDN_TIMER) {
        (void)wParam;
        LONG finished = app.finishedAt;
        if (finished && !app.failed) {
            DWORD elapsed = GetTickCount() - (DWORD)finished;
            if (elapsed >= CLOSE_AFTER_MS) {
                SendMessageW(hwnd, TDM_CLICK_BUTTON, IDCLOSE, 0);
            } else {
                wchar_t closing[128], footer[768];
                swprintf(closing, ARRAYSIZE(closing), app.t->closing, (int)((CLOSE_AFTER_MS - elapsed + 999) / 1000));
                swprintf(footer, ARRAYSIZE(footer), L"%ls %ls", closing, app.privacyLink);
                SendMessageW(hwnd, TDM_UPDATE_ELEMENT_TEXT, TDE_FOOTER, (LPARAM)footer);
            }
        }
    } else if (msg == TDN_HYPERLINK_CLICKED) {
        /* The only link is our own privacy policy; open web addresses only. */
        const wchar_t *href = (const wchar_t *)lParam;
        if (href && (wcsncmp(href, L"https://", 8) == 0 || wcsncmp(href, L"http://", 7) == 0))
            ShellExecuteW(NULL, L"open", href, NULL, NULL, SW_SHOWNORMAL);
    } else if (msg == TDN_DESTROYED) {
        app.dialog = NULL;
    }
    return S_OK;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR args, int show) {
    (void)prev; (void)show;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    readRequest(&app.req, args);
    app.t = &TEXT[app.req.lang];
    const Text *t = app.t;

    if (app.req.print) {
        detectCpu(&app.specs);
        detectGpus(&app.specs);
        detectRam(&app.specs);
        detectResolution(&app.specs);
        Buf url = { .len = 0 };
        browserUrl(&url, &app.specs, &app.req);
        char out[4096];
        int n = WideCharToMultiByte(CP_UTF8, 0, url.buf, -1, out, sizeof(out) - 1, NULL, NULL);
        if (n > 0) {
            out[n - 1] = '\n';
            DWORD written;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out, (DWORD)n, &written, NULL);
        }
        CoUninitialize();
        return 0;
    }

    /* Placeholder lines keep the window the same size when the values arrive. */
    Specs empty = { 0 };
    describe(app.content, ARRAYSIZE(app.content), &empty, t);
    for (wchar_t *p = app.content; (p = wcsstr(p, t->unknown)) != NULL; ) {
        size_t n = wcslen(t->unknown);
        memmove(p + 1, p + n, (wcslen(p + n) + 1) * sizeof(wchar_t));
        *p = L'…';
    }

    swprintf(app.privacyLink, ARRAYSIZE(app.privacyLink), L"<a href=\"%ls/%ls/privacy\">%ls</a>",
             app.req.site, LOCALE_CODE[app.req.lang], t->privacy);
    wchar_t footer[1024];
    swprintf(footer, ARRAYSIZE(footer), L"%ls %ls", t->footer, app.privacyLink);

    TASKDIALOGCONFIG cfg = {
        .cbSize = sizeof(cfg),
        .hInstance = inst,
        .dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_CALLBACK_TIMER |
                   TDF_SIZE_TO_CONTENT | TDF_USE_HICON_MAIN | TDF_ENABLE_HYPERLINKS,
        .dwCommonButtons = TDCBF_CLOSE_BUTTON,
        .pszWindowTitle = t->title,
        .pszMainInstruction = t->scanning,
        .pszContent = app.content,
        .pszFooter = footer,
        .pszFooterIcon = TD_INFORMATION_ICON,
        .pfCallback = onDialog,
    };
    cfg.hMainIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    if (!cfg.hMainIcon) {
        cfg.dwFlags &= ~TDF_USE_HICON_MAIN;
        cfg.pszMainIcon = TD_INFORMATION_ICON;
    }

    if (FAILED(TaskDialogIndirect(&cfg, NULL, NULL, NULL))) {
        /* Very old or stripped-down systems without the task dialog: do the work, then a plain message box. */
        work(NULL);
        wchar_t text[2600];
        swprintf(text, ARRAYSIZE(text), L"%ls\n\n%ls\n%ls: %ls/%ls/privacy", app.content, t->footer, t->privacy,
                 app.req.site, LOCALE_CODE[app.req.lang]);
        MessageBoxW(NULL, text, app.failed ? t->failed : t->title, MB_OK | (app.failed ? MB_ICONWARNING : MB_ICONINFORMATION));
    }
    CoUninitialize();
    return 0;
}
