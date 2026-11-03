/*
 * mx-ratchetd: keep the scroll wheel of a Logitech MX Master 3 / 3S (and other
 * SmartShift mice) in permanent ratchet mode while it is connected over Bluetooth.
 *
 * - Talks HID++ 2.0 straight to /dev/hidraw*; only the ratchet setting is touched.
 * - Watches BlueZ (D-Bus) for connection changes and re-applies the setting after
 *   every (re)connect, e.g. after the mouse wakes from sleep.
 * - Only dependencies: libc and libsystemd (both always present on Arch Linux).
 *
 * Build:  gcc -O2 -Wall -Wextra -o mx-ratchetd mx-ratchetd.c -lsystemd
 *
 * Usage:
 *   mx-ratchetd                 run as daemon (follows the connection)
 *   mx-ratchetd --once          apply once and exit
 *   mx-ratchetd --status        only show the current wheel mode
 *   mx-ratchetd --free          switch to free-spin once (for testing)
 *   mx-ratchetd --device PATH   use this hidraw node (with --once/--status/--free)
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#define LOGITECH_VID      0x046D
#define BUS_BLUETOOTH     0x0005

#define FEAT_SMART_SHIFT      0x2110   /* older mice */
#define FEAT_SMART_SHIFT_ENH  0x2111   /* MX Master 3 / 3S */

#define SW_ID          0x07            /* arbitrary software id to recognise our replies */
#define LONG_REPORT    0x11
#define REPORT_LEN     20

#define MODE_FREE      1
#define MODE_RATCHET   2
#define ALWAYS_RATCHET 255             /* auto-disengage threshold: never go free-spin */

#define ATTEMPTS       20              /* tries (1 s apart) after each connect event */

static const uint8_t dev_indices[] = { 0xFF, 0x01 };  /* 0xFF = BLE direct connection */

enum result { R_DONE, R_UNSUPPORTED, R_RETRY };

/* ------------------------------------------------------------------ helpers */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    va_list ap;
    fputs("mx-ratchetd: ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

/* ------------------------------------------------------------------- HID++ */

static void drain(int fd)
{
    uint8_t buf[64];
    struct pollfd p = { .fd = fd, .events = POLLIN };

    while (poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
        if (read(fd, buf, sizeof buf) <= 0)
            break;
    }
}

/*
 * Send one HID++ 2.0 long request and wait for the matching reply.
 * Returns 0 on success (payload in out[16]), -1 on timeout/IO error,
 * -2 when the device answered with a HID++ error.
 */
static int hidpp_request(int fd, uint8_t idx, uint8_t feat, uint8_t func,
                         const uint8_t *params, size_t nparams, uint8_t out[16])
{
    uint8_t msg[REPORT_LEN] = { 0 };
    uint8_t func_sw = (uint8_t)((func << 4) | SW_ID);

    msg[0] = LONG_REPORT;
    msg[1] = idx;
    msg[2] = feat;
    msg[3] = func_sw;
    if (params && nparams) {
        if (nparams > 16)
            nparams = 16;
        memcpy(msg + 4, params, nparams);
    }

    memset(out, 0, 16);
    drain(fd);
    if (write(fd, msg, sizeof msg) != (ssize_t)sizeof msg)
        return -1;

    uint64_t deadline = now_ms() + 1000;
    for (;;) {
        uint64_t t = now_ms();
        if (t >= deadline)
            return -1;

        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r = poll(&p, 1, (int)(deadline - t));
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;

        uint8_t buf[64];
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            return -1;
        }
        if (n < 7 || (buf[0] != 0x10 && buf[0] != 0x11) || buf[1] != idx)
            continue;
        if (buf[2] == 0xFF && buf[3] == feat && buf[4] == func_sw)
            return -2;                               /* HID++ 2.0 error */
        if (buf[2] == feat && buf[3] == func_sw) {
            size_t len = (size_t)n - 4;
            memcpy(out, buf + 4, len > 16 ? 16 : len);
            return 0;
        }
    }
}

/* IRoot.getFeature: returns feature index (>0), 0 if unsupported, <0 on failure. */
static int feature_index(int fd, uint8_t idx, uint16_t feature_id)
{
    uint8_t p[2] = { (uint8_t)(feature_id >> 8), (uint8_t)(feature_id & 0xFF) };
    uint8_t out[16];
    int r = hidpp_request(fd, idx, 0x00, 0, p, sizeof p, out);

    return r < 0 ? r : out[0];
}

static enum result handle_node(const char *path, int mode, int status_only)
{
    static const uint16_t feats[] = { FEAT_SMART_SHIFT_ENH, FEAT_SMART_SHIFT };
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    int answered = 0;

    if (fd < 0) {
        if (errno == EACCES || errno == EPERM)
            logf_("no permission for %s (run as root)", path);
        return R_RETRY;
    }

    for (size_t i = 0; i < sizeof dev_indices; i++) {
        uint8_t idx = dev_indices[i];

        for (size_t j = 0; j < sizeof feats / sizeof feats[0]; j++) {
            int fidx = feature_index(fd, idx, feats[j]);
            if (fidx < 0)
                break;                               /* wrong index / not ready */
            answered = 1;
            if (fidx == 0)
                continue;                            /* feature not supported */

            int enhanced = feats[j] == FEAT_SMART_SHIFT_ENH;
            uint8_t get_fn = enhanced ? 1 : 0;
            uint8_t set_fn = enhanced ? 2 : 1;
            uint8_t out[16];

            if (!status_only) {
                uint8_t p[3] = { (uint8_t)mode,
                                 mode == MODE_RATCHET ? ALWAYS_RATCHET : 0, 0 };
                if (hidpp_request(fd, idx, (uint8_t)fidx, set_fn, p, sizeof p, out) < 0) {
                    close(fd);
                    return R_RETRY;
                }
            }
            if (hidpp_request(fd, idx, (uint8_t)fidx, get_fn, NULL, 0, out) < 0) {
                close(fd);
                return R_RETRY;
            }

            logf_("%s: wheel mode = %s (auto-disengage = %u)", path,
                  out[0] == MODE_RATCHET ? "ratchet" :
                  out[0] == MODE_FREE ? "free-spin" : "unknown", out[1]);
            close(fd);
            return (!status_only && out[0] != mode) ? R_RETRY : R_DONE;
        }
    }

    close(fd);
    return answered ? R_UNSUPPORTED : R_RETRY;
}

/* Try every Logitech Bluetooth hidraw node. Returns 1 when a mouse was handled. */
static int apply_all(int mode, int status_only)
{
    DIR *d = opendir("/sys/class/hidraw");
    struct dirent *e;
    int done = 0;

    if (!d)
        return 0;

    while ((e = readdir(d))) {
        char sys[512], path[300], *line = NULL;
        size_t cap = 0;
        unsigned bus = 0, vid = 0, pid = 0;
        int match = 0;

        if (strncmp(e->d_name, "hidraw", 6) != 0)
            continue;

        snprintf(sys, sizeof sys, "/sys/class/hidraw/%s/device/uevent", e->d_name);
        FILE *f = fopen(sys, "re");
        if (!f)
            continue;
        while (getline(&line, &cap, f) > 0) {
            if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &vid, &pid) == 3) {
                match = bus == BUS_BLUETOOTH && vid == LOGITECH_VID;
                break;
            }
        }
        free(line);
        fclose(f);
        if (!match)
            continue;

        snprintf(path, sizeof path, "/dev/%s", e->d_name);
        if (handle_node(path, mode, status_only) == R_DONE)
            done = 1;
    }
    closedir(d);
    return done;
}

/* ----------------------------------------------------------- daemon logic */

static int attempts_left;
static uint64_t next_attempt;

static void schedule(void)
{
    attempts_left = ATTEMPTS;
    next_attempt = now_ms() + 500;
}

/* org.freedesktop.DBus.Properties.PropertiesChanged on org.bluez.Device1 */
static int on_props_changed(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    const char *iface, *name;
    int connected;

    (void)userdata;
    (void)err;

    if (sd_bus_message_read(m, "s", &iface) < 0 || strcmp(iface, "org.bluez.Device1") != 0)
        return 0;
    if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0)
        return 0;

    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
        if (sd_bus_message_read(m, "s", &name) < 0)
            return 0;
        if (strcmp(name, "Connected") == 0) {
            if (sd_bus_message_read(m, "v", "b", &connected) < 0)
                return 0;
            if (connected) {
                logf_("Bluetooth device connected, applying ratchet mode");
                schedule();
            }
        } else if (sd_bus_message_skip(m, "v") < 0) {
            return 0;
        }
        if (sd_bus_message_exit_container(m) < 0)
            return 0;
    }
    return 0;
}

static int run_daemon(void)
{
    sd_bus *bus = NULL;
    int r = sd_bus_open_system(&bus);

    if (r < 0) {
        logf_("cannot connect to system bus: %s", strerror(-r));
        return 1;
    }

    r = sd_bus_add_match(bus, NULL,
        "type='signal',sender='org.bluez',"
        "interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged',arg0='org.bluez.Device1'",
        on_props_changed, NULL);
    if (r < 0) {
        logf_("cannot subscribe to BlueZ signals: %s", strerror(-r));
        return 1;
    }

    logf_("watching Bluetooth connections");
    schedule();                                   /* mouse may already be connected */

    for (;;) {
        r = sd_bus_process(bus, NULL);
        if (r < 0) {
            logf_("D-Bus error: %s", strerror(-r));
            return 1;                             /* systemd restarts us */
        }
        if (r > 0)
            continue;

        uint64_t timeout = UINT64_MAX;
        if (attempts_left > 0) {
            uint64_t t = now_ms();
            if (t >= next_attempt) {
                if (apply_all(MODE_RATCHET, 0)) {
                    attempts_left = 0;
                } else {
                    if (--attempts_left == 0)
                        logf_("no ratchet-capable mouse answered (gave up until next connect)");
                    next_attempt = now_ms() + 1000;
                }
                continue;
            }
            timeout = (next_attempt - t) * 1000ULL;
        }

        r = sd_bus_wait(bus, timeout);
        if (r < 0 && r != -EINTR) {
            logf_("D-Bus wait error: %s", strerror(-r));
            return 1;
        }
    }
}

/* -------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    int once = 0, status_only = 0, mode = MODE_RATCHET;
    const char *device = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--once")) {
            once = 1;
        } else if (!strcmp(argv[i], "--status")) {
            once = status_only = 1;
        } else if (!strcmp(argv[i], "--free")) {
            once = 1;
            mode = MODE_FREE;
        } else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            once = 1;
            device = argv[++i];
        } else {
            fprintf(stderr,
                "usage: %s [--once | --status | --free] [--device /dev/hidrawN]\n"
                "  no option: run as daemon and follow the Bluetooth connection\n", argv[0]);
            return 2;
        }
    }

    if (!once)
        return run_daemon();

    for (int attempt = 0; attempt < 5; attempt++) {
        int ok = device ? handle_node(device, mode, status_only) == R_DONE
                        : apply_all(mode, status_only);
        if (ok)
            return 0;
        sleep(1);
    }
    logf_("no Logitech Bluetooth mouse with ratchet support answered (is it connected?)");
    return 1;
}
