/*
 * tag_logger.c
 *
 * Scans NFC / RFID / iCLASS tags and appends one line per scan to a fixed
 * log file on the SD card:
 *
 *   /ext/tag_logger/log.csv
 *
 * Format:  YYYY-MM-DD HH:MM:SS,XX XX XX XX [...]
 *
 * Screens:
 *   Scan   – waiting for a tag; shows scan count and mode
 *   Result – shows UID + timestamp of the tag just scanned
 *            OK  → back to scanning
 *            Up  → open log viewer
 *   Log    – scrollable view of log.csv
 *            OK (long-press) → wipe confirmation
 *            Back → back to scanning
 *   Wipe   – confirm wipe
 *            OK  → delete log file, back to scanning
 *            Back → back to log viewer
 */

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <furi_hal_rtc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Pull in observation types from the host app. */
#include "core/observation.h"
#include "core/observation_provider.h"

#define TIMESTAMP_BUF_SIZE 32

#define LOG_PATH        "/ext/tag_logger/log.csv"
#define LOG_DIR         "/ext/tag_logger"
#define LOG_LINE_MAX    64   /* "YYYY-MM-DD HH:MM:SS,XX XX XX XX XX XX XX\n" */
#define LOG_VIEW_LINES  4    /* lines visible at once on the 64-px display */
#define LOG_LINES_MAX   256  /* max lines we load into memory for the viewer */

/* ── Types ──────────────────────────────────────────────────── */

typedef enum {
    ScreenScan,
    ScreenResult,
    ScreenLog,
    ScreenWipeConfirm,
} AppScreen;

typedef enum {
    EventTypeInput,
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
} AppEvent;

typedef struct {
    ViewPort*            view_port;
    FuriMessageQueue*    event_queue;
    ObservationProvider* nfc_provider;
    NotificationApp*     notifications;

    /* Last scanned tag */
    AccessObservation    obs;
    char                 last_uid[32];   /* "XX XX XX …" */
    char                 last_time[TIMESTAMP_BUF_SIZE];  /* "YYYY-MM-DD HH:MM:SS" */
    uint32_t             scan_count;

    AppScreen            screen;

    /* Log viewer */
    char*                log_lines[LOG_LINES_MAX];
    size_t               log_line_count;
    size_t               log_scroll;     /* index of first visible line */
} App;

/* ── Helpers ────────────────────────────────────────────────── */

static void format_uid(const AccessObservation* obs, char* out, size_t out_size) {
    if(!obs->uid_present || obs->uid_len == 0) {
        snprintf(out, out_size, "unavailable");
        return;
    }
    size_t pos = 0;
    for(size_t i = 0; i < obs->uid_len && pos + 3 < out_size; i++) {
        if(i > 0) out[pos++] = ' ';
        snprintf(out + pos, out_size - pos, "%02X", obs->uid[i]);
        pos += 2;
    }
    out[pos] = '\0';
}

static void format_timestamp(char* out, size_t out_size) {
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    snprintf(
        out, out_size,
        "%04u-%02u-%02u %02u:%02u:%02u",
        (unsigned)dt.year, (unsigned)dt.month,  (unsigned)dt.day,
        (unsigned)dt.hour, (unsigned)dt.minute, (unsigned)dt.second);
}

/* Append one line to the log.  Creates the directory / file as needed. */
static void log_append(const char* uid_str, const char* time_str) {
    Storage* storage = furi_record_open(RECORD_STORAGE);

    /* Ensure directory exists */
    storage_simply_mkdir(storage, LOG_DIR);

    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, LOG_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        char line[LOG_LINE_MAX];
        int  len = snprintf(line, sizeof(line), "%s,%s\n", time_str, uid_str);
        if(len > 0) {
            storage_file_write(f, line, (uint16_t)len);
        }
        storage_file_close(f);
    }
    storage_file_free(f);

    furi_record_close(RECORD_STORAGE);
}

/* Load log lines into memory for the viewer.  Frees any previous load. */
static void log_load(App* app) {
    /* Free old lines */
    for(size_t i = 0; i < app->log_line_count; i++) {
        free(app->log_lines[i]);
        app->log_lines[i] = NULL;
    }
    app->log_line_count = 0;
    app->log_scroll     = 0;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File*    f       = storage_file_alloc(storage);

    if(!storage_file_open(f, LOG_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(f);
        furi_record_close(RECORD_STORAGE);
        return;
    }

    char   buf[LOG_LINE_MAX];
    size_t pos = 0;

    while(app->log_line_count < LOG_LINES_MAX) {
        uint16_t read = storage_file_read(f, buf + pos, 1);
        if(read == 0) {
            /* EOF – flush any remaining chars as a line */
            if(pos > 0) {
                buf[pos] = '\0';
                app->log_lines[app->log_line_count++] = strdup(buf);
            }
            break;
        }
        if(buf[pos] == '\n') {
            buf[pos] = '\0';
            if(pos > 0) {
                app->log_lines[app->log_line_count++] = strdup(buf);
            }
            pos = 0;
        } else {
            if(pos + 1 < sizeof(buf) - 1) pos++;
            /* else silently truncate the overlong line */
        }
    }

    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);

    /* Scroll to the bottom so the most recent entry is visible */
    if(app->log_line_count > LOG_VIEW_LINES) {
        app->log_scroll = app->log_line_count - LOG_VIEW_LINES;
    }
}

static void log_wipe(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_remove(storage, LOG_PATH);
    furi_record_close(RECORD_STORAGE);
}

/* ── Scanning control ───────────────────────────────────────── */

static void app_start_scanning(App* app) {
    observation_provider_start(app->nfc_provider);
}

static void app_stop_scanning(App* app) {
    observation_provider_stop(app->nfc_provider);
}

/* ── Draw callback ──────────────────────────────────────────── */

static void draw_callback(Canvas* canvas, void* context) {
    App* app = context;
    canvas_clear(canvas);

    /* ── Scan screen ── */
    if(app->screen == ScreenScan) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 10, "Access Audit");

        canvas_draw_line(canvas, 0, 13, 127, 13);

        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, "[NFC]");

        if(app->scan_count > 0) {
            char cbuf[24];
            snprintf(cbuf, sizeof(cbuf), "logged: %u", (unsigned)app->scan_count);
            canvas_draw_str(canvas, 2, 24, cbuf);
        }

        canvas_draw_str(canvas, 2, 36, "Tap card to reader...");
        canvas_draw_str(canvas, 2, 50, "Up: view log");
        canvas_draw_str(canvas, 2, 62, "Back: exit");
        return;
    }

    /* ── Result screen ── */
    if(app->screen == ScreenResult) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 10, "Tag Logged");
        canvas_draw_line(canvas, 0, 13, 127, 13);

        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 24, app->last_time);
        canvas_draw_str(canvas, 2, 36, app->last_uid);

        char cbuf[16];
        snprintf(cbuf, sizeof(cbuf), "#%u", (unsigned)app->scan_count);
        canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, cbuf);

        canvas_draw_str(canvas, 2, 54, "OK: scan next");
        canvas_draw_str(canvas, 2, 62, "Up: view log");
        return;
    }

    /* ── Log viewer ── */
    if(app->screen == ScreenLog) {
        canvas_set_font(canvas, FontSecondary);

        if(app->log_line_count == 0) {
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str(canvas, 2, 10, "Log");
            canvas_draw_line(canvas, 0, 13, 127, 13);
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(canvas, 2, 36, "Log is empty");
        } else {
            char header[24];
            snprintf(header, sizeof(header), "Log [%u]", (unsigned)app->log_line_count);
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str(canvas, 2, 10, header);
            canvas_draw_line(canvas, 0, 13, 127, 13);
            canvas_set_font(canvas, FontSecondary);

            for(size_t i = 0; i < LOG_VIEW_LINES; i++) {
                size_t idx = app->log_scroll + i;
                if(idx >= app->log_line_count) break;
                canvas_draw_str(canvas, 2, 22 + (int)i * 11, app->log_lines[idx]);
            }
        }

        canvas_draw_str(canvas, 2, 62, "Back:exit  Hold OK:wipe");
        return;
    }

    /* ── Wipe confirm ── */
    if(app->screen == ScreenWipeConfirm) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 10, "Wipe log?");
        canvas_draw_line(canvas, 0, 13, 127, 13);

        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 28, "This cannot be undone.");
        canvas_draw_str(canvas, 2, 46, "OK: wipe");
        canvas_draw_str(canvas, 2, 58, "Back: cancel");
        return;
    }
}

/* ── Input callback ─────────────────────────────────────────── */

static void input_callback(InputEvent* input_event, void* context) {
    App* app = context;
    AppEvent event = {.type = EventTypeInput, .input = *input_event};
    furi_message_queue_put(app->event_queue, &event, FuriWaitForever);
}

/* ── Entry point ────────────────────────────────────────────── */

int32_t access_audit_app(void* p) {
    UNUSED(p);

    App* app = malloc(sizeof(App));
    if(!app) return -1;
    memset(app, 0, sizeof(App));

    app->event_queue = furi_message_queue_alloc(8, sizeof(AppEvent));
    if(!app->event_queue) { free(app); return -1; }

    app->nfc_provider = observation_provider_alloc();
    if(!app->nfc_provider) {
        furi_message_queue_free(app->event_queue);
        free(app);
        return -1;
    }

    app->screen = ScreenScan;

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->view_port, GuiLayerFullscreen);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    view_port_update(app->view_port);

    app_start_scanning(app);

    bool     running = true;
    AppEvent event;

    while(running) {

        /* ── Poll for a new tag while on the scan screen ── */
        if(app->screen == ScreenScan) {
            AccessObservation candidate;
            bool got = observation_provider_poll(app->nfc_provider, &candidate);

            if(got) {
                app->obs = candidate;
                format_uid(&app->obs, app->last_uid, sizeof(app->last_uid));
                format_timestamp(app->last_time, sizeof(app->last_time));
                log_append(app->last_uid, app->last_time);
                app->scan_count++;
                notification_message(app->notifications, &sequence_success);
                app->screen = ScreenResult;
                view_port_update(app->view_port);
            }
        }

        /* ── Process input ── */
        if(furi_message_queue_get(app->event_queue, &event, 100) != FuriStatusOk)
            continue;

        if(event.type != EventTypeInput) continue;

        /* Short presses */
        if(event.input.type == InputTypeShort) {
            switch(app->screen) {

            case ScreenScan:
                if(event.input.key == InputKeyBack) {
                    running = false;
                } else if(event.input.key == InputKeyUp) {
                    app_stop_scanning(app);
                    log_load(app);
                    app->screen = ScreenLog;
                    view_port_update(app->view_port);
                }
                break;

            case ScreenResult:
                if(event.input.key == InputKeyOk) {
                    app->screen = ScreenScan;
                    app_start_scanning(app);
                    view_port_update(app->view_port);
                } else if(event.input.key == InputKeyUp) {
                    app_stop_scanning(app);
                    log_load(app);
                    app->screen = ScreenLog;
                    view_port_update(app->view_port);
                }
                break;

            case ScreenLog:
                if(event.input.key == InputKeyBack) {
                    app->screen = ScreenScan;
                    app_start_scanning(app);
                    view_port_update(app->view_port);
                } else if(event.input.key == InputKeyUp) {
                    if(app->log_scroll > 0) {
                        app->log_scroll--;
                        view_port_update(app->view_port);
                    }
                } else if(event.input.key == InputKeyDown) {
                    if(app->log_line_count > LOG_VIEW_LINES &&
                       app->log_scroll + LOG_VIEW_LINES < app->log_line_count) {
                        app->log_scroll++;
                        view_port_update(app->view_port);
                    }
                }
                break;

            case ScreenWipeConfirm:
                if(event.input.key == InputKeyOk) {
                    log_wipe();
                    /* Free viewer lines */
                    for(size_t i = 0; i < app->log_line_count; i++) {
                        free(app->log_lines[i]);
                        app->log_lines[i] = NULL;
                    }
                    app->log_line_count = 0;
                    app->scan_count     = 0;
                    app->screen         = ScreenScan;
                    app_start_scanning(app);
                    view_port_update(app->view_port);
                } else if(event.input.key == InputKeyBack) {
                    app->screen = ScreenLog;
                    view_port_update(app->view_port);
                }
                break;
            }
        }

        /* Long-press OK on the log screen → wipe confirm */
        if(event.input.type == InputTypeLong && event.input.key == InputKeyOk) {
            if(app->screen == ScreenLog) {
                app->screen = ScreenWipeConfirm;
                view_port_update(app->view_port);
            }
        }
    }

    /* ── Teardown ── */
    for(size_t i = 0; i < app->log_line_count; i++) free(app->log_lines[i]);
    observation_provider_free(app->nfc_provider);

    view_port_enabled_set(app->view_port, false);
    gui_remove_view_port(gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);

    furi_message_queue_free(app->event_queue);
    free(app);

    return 0;
}
