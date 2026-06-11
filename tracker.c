/*
 * tracker.c — Baby/family event tracker CGI application
 * Requires: libsqlite3-dev
 * Build:    gcc -O2 -o tracker tracker.c -lsqlite3
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include "sqlite3.h"

/* ── Configuration ──────────────────────────────────────────────────────── */

#define DB_PATH "tracker.db"
#define CSS_PATH "/tracker.css"
#define MAX_BUF  4096
#define MAX_PARAM 512

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static sqlite3 *db;

static void open_db(void) {
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        exit(1);
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS people ("
        "  id    INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  emoji TEXT    NOT NULL,"
        "  label TEXT    NOT NULL DEFAULT ''"
        ");",
        NULL, NULL, NULL);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS event_types ("
        "  id   INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL UNIQUE"
        ");",
        NULL, NULL, NULL);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS events ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  person_id  INTEGER NOT NULL REFERENCES people(id),"
        "  event_type TEXT    NOT NULL,"
        "  ts         INTEGER NOT NULL"   /* unix timestamp */
        ");",
        NULL, NULL, NULL);

    /* Seed default event types if table is empty */
    int count = 0;
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM event_types;", -1, &st, NULL);
    if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (count == 0) {
        const char *defaults[] = {
            "Fed", "Diaper change", "Nap start", "Nap end",
            "Bath", "Medicine", "Tummy time", NULL
        };
        for (int i = 0; defaults[i]; i++) {
            sqlite3_prepare_v2(db,
                "INSERT OR IGNORE INTO event_types(name) VALUES(?);", -1, &st, NULL);
            sqlite3_bind_text(st, 1, defaults[i], -1, SQLITE_STATIC);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
}

/* URL-decode a string in-place. Returns pointer to dst. */
static char *url_decode(const char *src, char *dst, int max) {
    int i = 0;
    while (*src && i < max - 1) {
        if (*src == '%' && isxdigit(src[1]) && isxdigit(src[2])) {
            char hex[3] = { src[1], src[2], 0 };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
    return dst;
}

/* Parse a single key=value pair out of a query-string body.
   Writes into dst (max bytes). Returns 1 if found. */
static int get_param(const char *qs, const char *key, char *dst, int max) {
    if (!qs || !*qs) return 0;
    size_t klen = strlen(key);
    const char *p = qs;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            char raw[MAX_PARAM];
            int n = end ? (int)(end - v) : (int)strlen(v);
            if (n >= MAX_PARAM) n = MAX_PARAM - 1;
            strncpy(raw, v, n);
            raw[n] = '\0';
            url_decode(raw, dst, max);
            return 1;
        }
        p = strchr(p, '&');
        if (!p) break;
        p++;
    }
    return 0;
}

/* HTML-escape — rotates through 8 static buffers so multiple calls
   can coexist safely within a single printf argument list. */
#define HE_BUFS 8
static char he_pool[HE_BUFS][MAX_BUF];
static int  he_idx = 0;
static const char *he(const char *s) {
    char *buf = he_pool[he_idx++ % HE_BUFS];
    char *o = buf;
    while (*s && o < buf + MAX_BUF - 6) {
        switch (*s) {
            case '&':  memcpy(o, "&amp;",  5); o += 5; break;
            case '<':  memcpy(o, "&lt;",   4); o += 4; break;
            case '>':  memcpy(o, "&gt;",   4); o += 4; break;
            case '"':  memcpy(o, "&quot;", 6); o += 6; break;
            default:   *o++ = *s;
        }
        s++;
    }
    *o = '\0';
    return buf;
}

/* Read POST body from stdin */
static char post_body[MAX_BUF];
static void read_post(void) {
    const char *cl = getenv("CONTENT_LENGTH");
    int len = cl ? atoi(cl) : 0;
    if (len <= 0) { post_body[0] = '\0'; return; }
    if (len >= MAX_BUF) len = MAX_BUF - 1;
    { size_t _r = fread(post_body, 1, len, stdin); (void)_r; }
    post_body[len] = '\0';
}

/* Format a unix timestamp as "Mon DD, YYYY HH:MM" in local time */

/* ── HTTP helpers ────────────────────────────────────────────────────────── */

static void http_header(const char *ct) {
    printf("Content-Type: %s\r\n\r\n", ct);
}

static void redirect(const char *path) {
    printf("Status: 303 See Other\r\n");
    printf("Location: %s\r\n\r\n", path);
}

/* ── Page chrome ─────────────────────────────────────────────────────────── */

static void page_open(const char *title) {
    http_header("text/html; charset=utf-8");
    printf("<!doctype html>\n"
           "<html lang='en'>\n"
           "<head>\n"
           "  <meta charset='utf-8'>\n"
           "  <meta name='viewport' content='width=device-width, initial-scale=1'>\n"
           "  <title>%s — Tracker</title>\n"
           "  <link rel='stylesheet' href='%s'>\n"
           "  <script src='https://unpkg.com/htmx.org@1.9.12/dist/htmx.min.js'></script>\n"
           "</head>\n"
           "<body>\n"
           "  <header>\n"
           "    <a href='/cgi-bin/tracker' class='brand'>🏠 tracker</a>\n"
           "    <a href='/cgi-bin/tracker/new' class='btn-new'>+ new event</a>\n"
           "  </header>\n"
           "  <main>\n", he(title), CSS_PATH);
}

static void page_close(void) {
    printf("  </main>\n</body>\n</html>\n");
}

/* ── Route: GET / — event log ───────────────────────────────────────────── */

static void route_log(void) {
    page_open("Event log");

    /* Filter controls */
    const char *qs = getenv("QUERY_STRING");
    char filter_person[MAX_PARAM] = "";
    char filter_type[MAX_PARAM]   = "";
    get_param(qs, "person",  filter_person, sizeof(filter_person));
    get_param(qs, "etype",   filter_type,   sizeof(filter_type));

    /* Person filter pills */
    printf("<section class='filter-bar'>\n");
    printf("  <a href='/cgi-bin/tracker' class='pill%s'>All</a>\n",
           (!*filter_person && !*filter_type) ? " active" : "");

    sqlite3_stmt *st;
    sqlite3_prepare_v2(db,
        "SELECT id, emoji, label FROM people ORDER BY id;", -1, &st, NULL);
    while (sqlite3_step(st) == SQLITE_ROW) {
        int pid       = sqlite3_column_int(st, 0);
        const char *e = (const char *)sqlite3_column_text(st, 1);
        const char *l = (const char *)sqlite3_column_text(st, 2);
        char pid_str[16];
        snprintf(pid_str, sizeof(pid_str), "%d", pid);
        printf("  <a href='/cgi-bin/tracker?person=%d' class='pill%s'>%s%s%s</a>\n",
               pid,
               (strcmp(filter_person, pid_str) == 0) ? " active" : "",
               he(e),
               (*l ? " " : ""), he(l));
    }
    sqlite3_finalize(st);
    printf("</section>\n");

    /* Event list */
    printf("<section class='event-list'>\n");

    const char *sql;
    if (*filter_person) {
        sql = "SELECT e.id, p.emoji, p.label, e.event_type, e.ts "
              "FROM events e JOIN people p ON p.id=e.person_id "
              "WHERE e.person_id=? "
              "ORDER BY e.ts DESC LIMIT 200;";
    } else {
        sql = "SELECT e.id, p.emoji, p.label, e.event_type, e.ts "
              "FROM events e JOIN people p ON p.id=e.person_id "
              "ORDER BY e.ts DESC LIMIT 200;";
    }
    sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (*filter_person) sqlite3_bind_int(st, 1, atoi(filter_person));

    int any = 0;
    char prev_date[32] = "";
    while (sqlite3_step(st) == SQLITE_ROW) {
        any = 1;
        const char *emoji  = (const char *)sqlite3_column_text(st, 1);
        const char *label  = (const char *)sqlite3_column_text(st, 2);
        const char *etype  = (const char *)sqlite3_column_text(st, 3);
        time_t ts          = (time_t)sqlite3_column_int64(st, 4);

        struct tm *tm = localtime(&ts);
        char date_hdr[32], time_str[32];
        strftime(date_hdr, sizeof(date_hdr), "%A, %B %-e", tm);
        strftime(time_str, sizeof(time_str), "%I:%M %p", tm);
        /* strip leading zero from time */
        const char *tdisp = (time_str[0] == '0') ? time_str + 1 : time_str;

        if (strcmp(date_hdr, prev_date) != 0) {
            if (*prev_date) printf("</ul>\n");
            printf("<h2 class='date-hdr'>%s</h2>\n<ul>\n", he(date_hdr));
            snprintf(prev_date, sizeof(prev_date), "%s", date_hdr);
        }

        printf("<li class='event-row'>\n"
               "  <span class='who'>%s%s%s</span>\n"
               "  <span class='what'>%s</span>\n"
               "  <span class='when'>%s</span>\n"
               "</li>\n",
               he(emoji), (*label ? " " : ""), he(label),
               he(etype), he(tdisp));
    }
    sqlite3_finalize(st);

    if (*prev_date) printf("</ul>\n");
    if (!any) printf("<p class='empty'>No events yet. <a href='/cgi-bin/tracker/new'>Add one?</a></p>\n");

    printf("</section>\n");
    page_close();
}

/* ── Route: GET /new — step 1: pick person ──────────────────────────────── */

static void route_new(void) {
    page_open("New event");
    printf("<h1>New event</h1>\n");

    /* ── Step 1: Who? ── */
    printf("<section class='step' id='step-who'>\n"
           "  <h2>Who?</h2>\n"
           "  <div class='person-grid'>\n");

    sqlite3_stmt *st;
    sqlite3_prepare_v2(db,
        "SELECT id, emoji, label FROM people ORDER BY id;", -1, &st, NULL);
    while (sqlite3_step(st) == SQLITE_ROW) {
        int pid      = sqlite3_column_int(st, 0);
        const char *e = (const char *)sqlite3_column_text(st, 1);
        const char *l = (const char *)sqlite3_column_text(st, 2);
        if (*l) {
            char esc_e[MAX_PARAM], esc_l[MAX_PARAM];
            strncpy(esc_e, he(e), sizeof(esc_e)-1); esc_e[sizeof(esc_e)-1]='\0';
            strncpy(esc_l, he(l), sizeof(esc_l)-1); esc_l[sizeof(esc_l)-1]='\0';
            printf("    <a href='/cgi-bin/tracker/pick-event?person=%d' class='person-btn'>"
                   "<span class='emoji'>%s</span><small>%s</small></a>\n",
                   pid, esc_e, esc_l);
        } else {
            printf("    <a href='/cgi-bin/tracker/pick-event?person=%d' class='person-btn'>"
                   "<span class='emoji'>%s</span></a>\n",
                   pid, he(e));
        }
    }
    sqlite3_finalize(st);

    /* Add person button → HTMX inline form */
    printf("    <button class='person-btn add-btn'\n"
           "            hx-get='/cgi-bin/tracker/add-person-form'\n"
           "            hx-target='#add-person-slot'\n"
           "            hx-swap='innerHTML'>\n"
           "      <span class='emoji'>＋</span>\n"
           "    </button>\n");
    printf("  </div>\n"
           "  <div id='add-person-slot'></div>\n"
           "</section>\n");

    page_close();
}

/* ── Route: GET /add-person-form — HTMX partial ─────────────────────────── */

static void route_add_person_form(void) {
    http_header("text/html; charset=utf-8");
    printf("<form class='inline-form'\n"
           "      hx-post='/cgi-bin/tracker/add-person'\n"
           "      hx-target='#step-who'\n"
           "      hx-swap='outerHTML'>\n"
           "  <label>Emoji<br><input name='emoji' type='text' maxlength='8' "
           "         placeholder='👶' required style='width:4em;font-size:1.5rem'></label>\n"
           "  <label>Label (optional)<br>"
           "         <input name='label' type='text' maxlength='40' placeholder='e.g. Mia'></label>\n"
           "  <button type='submit'>Add person</button>\n"
           "  <button type='button' onclick=\"this.closest('.inline-form').remove()\">Cancel</button>\n"
           "</form>\n");
}

/* ── Route: POST /add-person ─────────────────────────────────────────────── */

static void route_add_person(void) {
    read_post();
    char emoji[MAX_PARAM] = "", label[MAX_PARAM] = "";
    get_param(post_body, "emoji", emoji, sizeof(emoji));
    get_param(post_body, "label", label, sizeof(label));

    if (*emoji) {
        sqlite3_stmt *st;
        sqlite3_prepare_v2(db,
            "INSERT INTO people(emoji, label) VALUES(?,?);", -1, &st, NULL);
        sqlite3_bind_text(st, 1, emoji, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, label, -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    /* Return refreshed step-who section as HTMX response */
    http_header("text/html; charset=utf-8");
    printf("<section class='step' id='step-who'>\n"
           "  <h2>Who?</h2>\n"
           "  <div class='person-grid'>\n");

    sqlite3_stmt *st;
    sqlite3_prepare_v2(db,
        "SELECT id, emoji, label FROM people ORDER BY id;", -1, &st, NULL);
    while (sqlite3_step(st) == SQLITE_ROW) {
        int pid       = sqlite3_column_int(st, 0);
        const char *e = (const char *)sqlite3_column_text(st, 1);
        const char *l = (const char *)sqlite3_column_text(st, 2);
        if (*l) {
            char esc_e[MAX_PARAM], esc_l[MAX_PARAM];
            strncpy(esc_e, he(e), sizeof(esc_e)-1); esc_e[sizeof(esc_e)-1]='\0';
            strncpy(esc_l, he(l), sizeof(esc_l)-1); esc_l[sizeof(esc_l)-1]='\0';
            printf("    <a href='/cgi-bin/tracker/pick-event?person=%d' class='person-btn'>"
                   "<span class='emoji'>%s</span><small>%s</small></a>\n",
                   pid, esc_e, esc_l);
        } else {
            printf("    <a href='/cgi-bin/tracker/pick-event?person=%d' class='person-btn'>"
                   "<span class='emoji'>%s</span></a>\n",
                   pid, he(e));
        }
    }
    sqlite3_finalize(st);

    printf("    <button class='person-btn add-btn'\n"
           "            hx-get='/cgi-bin/tracker/add-person-form'\n"
           "            hx-target='#add-person-slot'\n"
           "            hx-swap='innerHTML'>\n"
           "      <span class='emoji'>＋</span>\n"
           "    </button>\n"
           "  </div>\n"
           "  <div id='add-person-slot'></div>\n"
           "</section>\n");
}

/* ── Route: GET /pick-event?person=N — step 2 ───────────────────────────── */

static void route_pick_event(void) {
    const char *qs = getenv("QUERY_STRING");
    char pid_str[MAX_PARAM] = "";
    get_param(qs, "person", pid_str, sizeof(pid_str));
    int pid = atoi(pid_str);

    /* Look up person */
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db,
        "SELECT emoji, label FROM people WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, pid);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        redirect("/cgi-bin/tracker/new");
        return;
    }
    char person_emoji[MAX_PARAM], person_label[MAX_PARAM];
    strncpy(person_emoji, (const char *)sqlite3_column_text(st, 0), sizeof(person_emoji)-1);
    strncpy(person_label, (const char *)sqlite3_column_text(st, 1), sizeof(person_label)-1);
    sqlite3_finalize(st);

    page_open("New event — pick event");
    printf("<h1>New event</h1>\n");

    /* Show chosen person */
    printf("<div class='chosen-person'>%s%s%s</div>\n",
           he(person_emoji),
           (*person_label ? " " : ""), he(person_label));

    /* ── Step 2: What happened? ── */
    printf("<section class='step' id='step-what'>\n"
           "  <h2>What happened?</h2>\n"
           "  <ul class='event-type-list'>\n");

    sqlite3_prepare_v2(db,
        "SELECT name FROM event_types ORDER BY name;", -1, &st, NULL);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 0);
        printf("    <li><a href='/cgi-bin/tracker/confirm?person=%d&etype=%s' "
               "class='event-type-btn'>%s</a></li>\n",
               pid, he(name), he(name));
    }
    sqlite3_finalize(st);

    printf("  </ul>\n");

    /* Add new event type — HTMX inline */
    printf("  <button class='add-etype-btn'\n"
           "          hx-get='/cgi-bin/tracker/add-etype-form?person=%d'\n"
           "          hx-target='#add-etype-slot'\n"
           "          hx-swap='innerHTML'>\n"
           "    + add new event type\n"
           "  </button>\n"
           "  <div id='add-etype-slot'></div>\n"
           "</section>\n",
           pid);

    page_close();
}

/* ── Route: GET /add-etype-form — HTMX partial ───────────────────────────── */

static void route_add_etype_form(void) {
    const char *qs = getenv("QUERY_STRING");
    char pid_str[MAX_PARAM] = "";
    get_param(qs, "person", pid_str, sizeof(pid_str));

    http_header("text/html; charset=utf-8");
    printf("<form class='inline-form'\n"
           "      hx-post='/cgi-bin/tracker/add-etype'\n"
           "      hx-target='#step-what'\n"
           "      hx-swap='outerHTML'>\n"
           "  <input type='hidden' name='person' value='%s'>\n"
           "  <label>Event name<br>"
           "    <input name='etype' type='text' maxlength='60' "
           "           placeholder='e.g. Vaccinated' required></label>\n"
           "  <button type='submit'>Add &amp; select</button>\n"
           "  <button type='button' onclick=\"this.closest('.inline-form').remove()\">Cancel</button>\n"
           "</form>\n", he(pid_str));
}

/* ── Route: POST /add-etype ──────────────────────────────────────────────── */

static void route_add_etype(void) {
    read_post();
    char etype[MAX_PARAM] = "", pid_str[MAX_PARAM] = "";
    get_param(post_body, "etype",  etype,   sizeof(etype));
    get_param(post_body, "person", pid_str, sizeof(pid_str));
    int pid = atoi(pid_str);

    if (*etype) {
        sqlite3_stmt *st;
        sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO event_types(name) VALUES(?);", -1, &st, NULL);
        sqlite3_bind_text(st, 1, etype, -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    /* Return refreshed step-what section */
    http_header("text/html; charset=utf-8");
    printf("<section class='step' id='step-what'>\n"
           "  <h2>What happened?</h2>\n"
           "  <ul class='event-type-list'>\n");

    sqlite3_stmt *st;
    sqlite3_prepare_v2(db,
        "SELECT name FROM event_types ORDER BY name;", -1, &st, NULL);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 0);
        printf("    <li><a href='/cgi-bin/tracker/confirm?person=%d&etype=%s' "
               "class='event-type-btn'>%s</a></li>\n",
               pid, he(name), he(name));
    }
    sqlite3_finalize(st);

    printf("  </ul>\n"
           "  <button class='add-etype-btn'\n"
           "          hx-get='/cgi-bin/tracker/add-etype-form?person=%d'\n"
           "          hx-target='#add-etype-slot'\n"
           "          hx-swap='innerHTML'>\n"
           "    + add new event type\n"
           "  </button>\n"
           "  <div id='add-etype-slot'></div>\n"
           "</section>\n", pid);
}

/* ── Route: GET /confirm — step 3 ───────────────────────────────────────── */

static void route_confirm(void) {
    const char *qs = getenv("QUERY_STRING");
    char pid_str[MAX_PARAM] = "", etype[MAX_PARAM] = "";
    get_param(qs, "person", pid_str, sizeof(pid_str));
    get_param(qs, "etype",  etype,   sizeof(etype));
    int pid = atoi(pid_str);

    /* Look up person */
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db,
        "SELECT emoji, label FROM people WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, pid);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        redirect("/cgi-bin/tracker/new");
        return;
    }
    char person_emoji[MAX_PARAM], person_label[MAX_PARAM];
    strncpy(person_emoji, (const char *)sqlite3_column_text(st, 0), sizeof(person_emoji)-1);
    strncpy(person_label, (const char *)sqlite3_column_text(st, 1), sizeof(person_label)-1);
    sqlite3_finalize(st);

    /* Current time components for the pickers */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char date_val[16], time_val[8];
    strftime(date_val, sizeof(date_val), "%Y-%m-%d", tm);
    strftime(time_val, sizeof(time_val), "%H:%M", tm);

    page_open("Confirm event");
    printf("<h1>Confirm event</h1>\n");

    printf("<div class='confirm-card'>\n");

    /* Summary line */
    printf("  <div class='confirm-summary'>"
           "    <span class='who'>%s%s%s</span>"
           "    <span class='arrow'>→</span>"
           "    <span class='what'>%s</span>"
           "  </div>\n",
           he(person_emoji), (*person_label ? " " : ""), he(person_label),
           he(etype));

    /* Form */
    printf("  <form method='POST' action='/cgi-bin/tracker/create'>\n"
           "    <input type='hidden' name='person' value='%d'>\n"
           "    <input type='hidden' name='etype'  value='%s'>\n",
           pid, he(etype));

    printf("    <div class='time-row'>\n"
           "      <label>\n"
           "        <span>Time</span>\n"
           "        <input type='time' name='time' value='%s' required>\n"
           "      </label>\n"
           "      <label>\n"
           "        <span>Date</span>\n"
           "        <input type='date' name='date' value='%s' required>\n"
           "      </label>\n"
           "    </div>\n",
           time_val, date_val);

    printf("    <button type='submit' class='btn-create'>✓ Create event</button>\n"
           "  </form>\n"
           "</div>\n");

    /* Back link */
    printf("<p class='back'><a href='/cgi-bin/tracker/pick-event?person=%d'>"
           "← back</a></p>\n", pid);

    page_close();
}

/* ── Route: POST /create ─────────────────────────────────────────────────── */

static void route_create(void) {
    read_post();
    char pid_str[MAX_PARAM] = "", etype[MAX_PARAM] = "";
    char date_str[MAX_PARAM] = "", time_str[MAX_PARAM] = "";
    get_param(post_body, "person", pid_str,  sizeof(pid_str));
    get_param(post_body, "etype",  etype,    sizeof(etype));
    get_param(post_body, "date",   date_str, sizeof(date_str));
    get_param(post_body, "time",   time_str, sizeof(time_str));

    int pid = atoi(pid_str);

    /* Parse date + time into unix timestamp */
    struct tm tm = {0};
    int year, mon, day, hour, min;
    sscanf(date_str, "%d-%d-%d", &year, &mon, &day);
    sscanf(time_str, "%d:%d", &hour, &min);
    tm.tm_year  = year - 1900;
    tm.tm_mon   = mon - 1;
    tm.tm_mday  = day;
    tm.tm_hour  = hour;
    tm.tm_min   = min;
    tm.tm_isdst = -1;
    time_t ts = mktime(&tm);

    if (pid > 0 && *etype && ts != (time_t)-1) {
        sqlite3_stmt *st;
        sqlite3_prepare_v2(db,
            "INSERT INTO events(person_id, event_type, ts) VALUES(?,?,?);",
            -1, &st, NULL);
        sqlite3_bind_int(st,   1, pid);
        sqlite3_bind_text(st,  2, etype, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)ts);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    redirect("/cgi-bin/tracker");
}

/* ── Route: GET /seed — pre-populate demo people (run once) ─────────────── */

static void route_seed(void) {
    int count = 0;
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM people;", -1, &st, NULL);
    if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);

    if (count == 0) {
        struct { const char *emoji; const char *label; } people[] = {
            { "👶", "Baby" },
            { "🧑", "Partner" },
            { NULL, NULL }
        };
        for (int i = 0; people[i].emoji; i++) {
            sqlite3_prepare_v2(db,
                "INSERT INTO people(emoji, label) VALUES(?,?);", -1, &st, NULL);
            sqlite3_bind_text(st, 1, people[i].emoji, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, people[i].label, -1, SQLITE_STATIC);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    redirect("/cgi-bin/tracker");
}

/* ── Router ──────────────────────────────────────────────────────────────── */

int main(void) {
    open_db();

    const char *method   = getenv("REQUEST_METHOD") ?: "GET";
    const char *path     = getenv("PATH_INFO")      ?: "";

    /* Strip leading slash for matching */
    if (path[0] == '/') path++;

    if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "create")     == 0) route_create();
        else if (strcmp(path, "add-person") == 0) route_add_person();
        else if (strcmp(path, "add-etype")  == 0) route_add_etype();
        else redirect("/cgi-bin/tracker");
    } else {
        if      (*path == '\0')                      route_log();
        else if (strcmp(path, "new")             == 0) route_new();
        else if (strcmp(path, "pick-event")      == 0) route_pick_event();
        else if (strcmp(path, "confirm")         == 0) route_confirm();
        else if (strcmp(path, "add-person-form") == 0) route_add_person_form();
        else if (strcmp(path, "add-etype-form")  == 0) route_add_etype_form();
        else if (strcmp(path, "seed")            == 0) route_seed();
        else redirect("/cgi-bin/tracker");
    }

    sqlite3_close(db);
    return 0;
}
