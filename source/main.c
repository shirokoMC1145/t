// EhViewer 3DS - skeleton: front page list + tag blocklist (no login)
// Build: devkitPro 3ds template, LIBS := -lctru -lm
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>

#define BASE_URL   "https://e-hentai.org/"
#define DATA_DIR   "sdmc:/3ds/ehviewer"
#define BLOCKLIST  DATA_DIR "/blocklist.txt"
#define DUMP_PATH  DATA_DIR "/last.html"
#define MAX_ITEMS  100
#define MAX_BLOCK  256
#define MAX_PAGES  128
#define MAX_BODY   (3 * 1024 * 1024)

typedef struct { char title[192]; long gid; } Item;

static PrintConsole top, bot;
static Item items[MAX_ITEMS];
static int n_items, n_blocked, cursor;
static long last_gid;
static char block[MAX_BLOCK][64];
static int n_block;
static long page_next[MAX_PAGES]; // page_next[i]: ?next= value for page i (0 = front page)
static int page_idx;
static char *last_html;
static size_t last_len;
static char status[64] = "";

// ---------- HTTP ----------
static char *http_get(const char *url, size_t *out_len) {
    char cur[512];
    snprintf(cur, sizeof cur, "%s", url);
    for (int hop = 0; hop < 5; hop++) {
        httpcContext ctx;
        if (R_FAILED(httpcOpenContext(&ctx, HTTPC_METHOD_GET, cur, 1))) return NULL;
        httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify); // 3DS cert store is old
        httpcAddRequestHeaderField(&ctx, "User-Agent", "Mozilla/5.0 (Nintendo 3DS) ehviewer3ds");
        httpcAddRequestHeaderField(&ctx, "Cookie", "nw=1");
        if (R_FAILED(httpcBeginRequest(&ctx))) { httpcCloseContext(&ctx); return NULL; }

        u32 code = 0;
        httpcGetResponseStatusCode(&ctx, &code);
        if (code >= 300 && code < 400) {
            char loc[512];
            if (R_SUCCEEDED(httpcGetResponseHeader(&ctx, "Location", loc, sizeof loc))) {
                snprintf(cur, sizeof cur, "%s", loc);
                httpcCloseContext(&ctx);
                continue;
            }
        }
        if (code != 200) { httpcCloseContext(&ctx); return NULL; }

        size_t cap = 0x40000, len = 0;
        char *buf = malloc(cap + 1);
        Result r;
        do {
            if (cap - len < 0x8000) {
                if (cap >= MAX_BODY) break;
                cap *= 2;
                char *nb = realloc(buf, cap + 1);
                if (!nb) { free(buf); buf = NULL; break; }
                buf = nb;
            }
            u32 got = 0;
            r = httpcDownloadData(&ctx, (u8 *)buf + len, 0x8000, &got);
            len += got;
        } while (r == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING);
        httpcCloseContext(&ctx);
        if (!buf || R_FAILED(r)) { free(buf); return NULL; }
        buf[len] = 0;
        *out_len = len;
        return buf;
    }
    return NULL;
}

// ---------- blocklist ----------
static void load_blocklist(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(DATA_DIR, 0777);
    n_block = 0;
    FILE *f = fopen(BLOCKLIST, "r");
    if (!f) {
        f = fopen(BLOCKLIST, "w");
        if (f) {
            fputs("# one tag per line, e.g. female:ntr  (or just: ntr)\n", f);
            fclose(f);
        }
        return;
    }
    char line[128];
    while (n_block < MAX_BLOCK && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *s = line;
        while (*s == ' ') s++;
        if (!*s || *s == '#') continue;
        snprintf(block[n_block++], 64, "%s", s);
    }
    fclose(f);
}

static int tag_blocked(const char *tag) {
    const char *bare = strchr(tag, ':');
    bare = bare ? bare + 1 : tag;
    for (int i = 0; i < n_block; i++) {
        const char *b = block[i];
        if (!strcasecmp(tag, b)) return 1;
        if (!strchr(b, ':') && !strcasecmp(bare, b)) return 1;
    }
    return 0;
}

// ---------- parsing ----------
static void unescape(char *s) {
    static const struct { const char *e; char c; } T[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
        {"&quot;", '"'}, {"&#039;", '\''}, {"&#x27;", '\''}};
    char *w = s;
    while (*s) {
        int hit = 0;
        if (*s == '&') {
            for (int i = 0; i < 6; i++) {
                size_t l = strlen(T[i].e);
                if (!strncmp(s, T[i].e, l)) { *w++ = T[i].c; s += l; hit = 1; break; }
            }
        }
        if (!hit) *w++ = *s++;
    }
    *w = 0;
}

// find "/g/<digits>" (skips static URLs like ehgt.org/g/t.png)
static long find_gid(const char *s) {
    while ((s = strstr(s, "/g/"))) {
        if (isdigit((unsigned char)s[3])) return strtol(s + 3, NULL, 10);
        s += 3;
    }
    return 0;
}

static void parse_page(char *html) {
    n_items = n_blocked = 0;
    last_gid = 0;
    char *p = html;
    while (n_items < MAX_ITEMS && (p = strstr(p, "<tr"))) {
        char *end = strstr(p + 3, "<tr");
        char saved = 0;
        if (end) { saved = *end; *end = 0; }

        char *g = strstr(p, "class=\"glink\"");
        long gid = g ? find_gid(p) : 0;
        if (g && gid) {
            last_gid = gid;
            int blocked = 0;
            for (char *q = p; (q = strstr(q, "class=\"gt"));) {
                char *ti = strstr(q, "title=\"");
                if (!ti) break;
                ti += 7;
                char *te = strchr(ti, '"');
                if (!te) break;
                char tag[96];
                size_t n = te - ti;
                if (n >= sizeof tag) n = sizeof tag - 1;
                memcpy(tag, ti, n);
                tag[n] = 0;
                if (tag_blocked(tag)) blocked = 1;
                q = te;
            }
            if (blocked) {
                n_blocked++;
            } else {
                char *t = strchr(g, '>');
                char *te = t ? strstr(t, "</") : NULL;
                if (t && te) {
                    size_t n = te - (t + 1);
                    Item *it = &items[n_items++];
                    if (n >= sizeof it->title) n = sizeof it->title - 1;
                    memcpy(it->title, t + 1, n);
                    it->title[n] = 0;
                    unescape(it->title);
                    it->gid = gid;
                }
            }
        }
        if (end) *end = saved; else break;
        p = end;
    }
    if (cursor >= n_items) cursor = n_items ? n_items - 1 : 0;
}

// ---------- UI ----------
static void draw(void) {
    consoleSelect(&top);
    consoleClear();
    printf("EhViewer 3DS  page %d  shown %d  hidden %d\n", page_idx + 1, n_items, n_blocked);
    printf("--------------------------------------------------\n");
    int rows = 26, start = cursor - rows / 2;
    if (start > n_items - rows) start = n_items - rows;
    if (start < 0) start = 0;
    for (int i = start; i < n_items && i < start + rows; i++)
        printf("%c%.48s\n", i == cursor ? '>' : ' ', items[i].title);

    consoleSelect(&bot);
    consoleClear();
    printf("Up/Down  move\n");
    printf("L / R    prev / next page\n");
    printf("X        reload blocklist\n");
    printf("Y        dump html to SD\n");
    printf("START    exit\n\n");
    printf("blocklist: %d tags\n", n_block);
    printf("%s/blocklist.txt\n\n", DATA_DIR);
    printf("%s\n", status);

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

static int load_page(void) {
    char url[128];
    if (page_next[page_idx]) snprintf(url, sizeof url, BASE_URL "?next=%ld", page_next[page_idx]);
    else snprintf(url, sizeof url, "%s", BASE_URL);

    snprintf(status, sizeof status, "Loading...");
    draw();
    size_t len = 0;
    char *h = http_get(url, &len);
    if (!h) { snprintf(status, sizeof status, "Request failed"); return 0; }
    free(last_html);
    last_html = h;
    last_len = len;
    cursor = 0;
    parse_page(last_html);
    snprintf(status, sizeof status, "OK (%u KB)", (unsigned)(len / 1024));
    return 1;
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, &top);
    consoleInit(GFX_BOTTOM, &bot);
    httpcInit(0);
    load_blocklist();
    load_page();

    while (aptMainLoop()) {
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & KEY_START) break;
        if ((k & KEY_DOWN) && cursor < n_items - 1) cursor++;
        if ((k & KEY_UP) && cursor > 0) cursor--;
        if ((k & KEY_R) && last_gid && page_idx < MAX_PAGES - 1) {
            page_next[page_idx + 1] = last_gid;
            page_idx++;
            if (!load_page()) page_idx--;
        }
        if ((k & KEY_L) && page_idx > 0) {
            page_idx--;
            if (!load_page()) page_idx++;
        }
        if ((k & KEY_X) && last_html) {
            load_blocklist();
            parse_page(last_html);
        }
        if ((k & KEY_Y) && last_html) {
            FILE *f = fopen(DUMP_PATH, "wb");
            if (f) { fwrite(last_html, 1, last_len, f); fclose(f); }
            snprintf(status, sizeof status, f ? "Saved last.html" : "Save failed");
        }
        draw();
    }

    free(last_html);
    httpcExit();
    gfxExit();
    return 0;
}
