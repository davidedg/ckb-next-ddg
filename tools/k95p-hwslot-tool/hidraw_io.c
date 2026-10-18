// The K95 RGB Platinum through Linux hidraw (hidraw_io.h).
#define _GNU_SOURCE
#include "hidraw_io.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REPLY_WAIT_S 5

void hidraw_log(FILE* f, const char* dir, const uint8_t pkt[CAPE_PKT_SIZE]){
    if(!f)
        return;
    fprintf(f, "%s", dir);
    for(int i = 0; i < CAPE_PKT_SIZE; i++)
        fprintf(f, " %02x", pkt[i]);
    fprintf(f, "\n");
    fflush(f);
}

static void sleep_ms(void* ctx, unsigned ms){
    (void)ctx;
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000 };
    while(nanosleep(&t, &t) && errno == EINTR){
    }
}

static int put(hidraw_io* h, const uint8_t* p, unsigned pause){
    if(pause)
        sleep_ms(h, pause);
    hidraw_log(h->transcript, "TX", p);
    h->packets++;
    return write(h->out, p, CAPE_PKT_SIZE) == CAPE_PKT_SIZE ? 0 : -1;
}

static int send_packet(void* v, const uint8_t* p){
    hidraw_io* h = v;
    return put(h, p, h->pause_ms);
}

static int send_cont(void* v, const uint8_t* p){
    return put(v, p, 0);
}

static int exchange(void* v, const uint8_t* p, uint8_t* reply){
    hidraw_io* h = v;
    if(send_packet(v, p))
        return -1;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const time_t deadline = now.tv_sec + REPLY_WAIT_S;
    do {
        if(poll(h->in, h->count, 100) > 0){
            for(nfds_t i = 0; i < h->count; ++i){
                if(!(h->in[i].revents & POLLIN))
                    continue;
                uint8_t b[128];
                const ssize_t n = read(h->in[i].fd, b, sizeof(b));
                if(n == CAPE_PKT_SIZE && b[0] == 0x0e){
                    memcpy(reply, b, CAPE_PKT_SIZE);
                    hidraw_log(h->transcript, "RX", b);
                    if(p[0] == 0x0e && p[1] == 0x01){
                        cape_fwinfo fw;
                        h->tested = cape_fwinfo_from_ident(b, CAPE_PKT_SIZE, &fw) == CAPE_OK && cape_fw_is_tested(&fw);
                        h->layout = b[23] + 1;   // as the daemon reads it (firmware.c)
                    }
                    return 0;
                }
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while(now.tv_sec < deadline);
    return -1;
}

static void phase(void* v, cape_phase p){
    hidraw_io* h = v;
    h->pause_ms = p == CAPE_PHASE_WRITE ? 11 : 1;
}

int hidraw_send(hidraw_io* h, const uint8_t pkt[CAPE_PKT_SIZE]){
    return send_packet(h, pkt);
}

cape_io hidraw_cape_io(hidraw_io* h){
    cape_io io = { .ctx = h, .send = send_packet, .xfer = exchange, .sleep_ms = sleep_ms, .send_cont = send_cont, .phase = phase };
    return io;
}

// A ckb-next daemon running: its process, or the pid file of its root node
static int daemon_running(char* why, size_t cap){
    DIR* d = opendir("/proc");
    struct dirent* e;
    int found = 0;
    while(d && (e = readdir(d)) && !found){
        if(e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        char path[PATH_MAX], comm[64] = "";
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        FILE* f = fopen(path, "r");
        if(!f)
            continue;
        if(fgets(comm, sizeof(comm), f) && !strncmp(comm, "ckb-next-daemon", 15)){
            snprintf(why, cap, "a ckb-next daemon is running (pid %s)", e->d_name);
            found = 1;
        }
        fclose(f);
    }
    if(d)
        closedir(d);
    if(!found && access("/dev/input/ckb0/pid", F_OK) == 0){
        snprintf(why, cap, "/dev/input/ckb0/pid exists: a ckb-next daemon is running or did not end cleanly");
        found = 1;
    }
    return found;
}

int hidraw_open(hidraw_io* h, char* why, size_t cap){
    memset(h, 0, sizeof(*h));
    h->out = -1;
    h->pause_ms = 1;
    if(daemon_running(why, cap))
        return -1;
    glob_t found = { 0 };
    if(glob("/sys/class/hidraw/hidraw*/device", 0, NULL, &found)){
        snprintf(why, cap, "no hidraw device");
        return -1;
    }
    char device[PATH_MAX] = "";
    for(size_t i = 0; i < found.gl_pathc; ++i){
        char resolved[PATH_MAX], uevent[PATH_MAX], contents[2048], node[PATH_MAX];
        if(!realpath(found.gl_pathv[i], resolved))
            continue;
        if((size_t)snprintf(uevent, sizeof(uevent), "%s/uevent", resolved) >= sizeof(uevent))
            continue;
        FILE* f = fopen(uevent, "r");
        if(!f)
            continue;
        const size_t n = fread(contents, 1, sizeof(contents) - 1, f);
        contents[n] = 0;
        fclose(f);
        if(!strstr(contents, "HID_ID=0003:00001B1C:00001B2D"))
            continue;
        char* slash = strrchr(resolved, '/');
        if(!slash)
            continue;
        *slash = 0;
        char* dot = strrchr(resolved, '.');
        if(!dot)
            continue;
        const int iface = atoi(dot + 1);
        // the USB device the interface belongs to: one keyboard only
        char dev[PATH_MAX];
        snprintf(dev, sizeof(dev), "%.*s", (int)(dot - resolved), resolved);
        char* colon = strrchr(dev, ':');
        if(colon)
            *colon = 0;
        if(!device[0])
            snprintf(device, sizeof(device), "%s", dev);
        else if(strcmp(device, dev)){
            snprintf(why, cap, "more than one K95 RGB Platinum attached");
            globfree(&found);
            hidraw_close(h);
            return -1;
        }
        char name[64];
        if(sscanf(found.gl_pathv[i], "/sys/class/hidraw/%63[^/]/device", name) != 1)
            continue;
        snprintf(node, sizeof(node), "/dev/%s", name);
        if(h->count >= HIDRAW_MAX_NODES)
            continue;
        const int fd = open(node, O_RDONLY | O_NONBLOCK);
        if(fd < 0){
            snprintf(why, cap, "%s: %s (run as root)", node, strerror(errno));
            globfree(&found);
            hidraw_close(h);
            return -1;
        }
        h->in[h->count++] = (struct pollfd){ fd, POLLIN, 0 };
        if(iface == 1)
            h->out = open(node, O_WRONLY);
    }
    globfree(&found);
    if(h->out < 0 || !h->count){
        snprintf(why, cap, "no K95 RGB Platinum (1b1c:1b2d) found, or its interface 1 cannot be written");
        hidraw_close(h);
        return -1;
    }
    // Old input reports go: nothing has been sent yet
    uint8_t stale[128];
    for(nfds_t i = 0; i < h->count; ++i)
        while(read(h->in[i].fd, stale, sizeof(stale)) > 0){
        }
    return 0;
}

void hidraw_close(hidraw_io* h){
    for(nfds_t i = 0; i < h->count; ++i)
        close(h->in[i].fd);
    if(h->out >= 0)
        close(h->out);
    h->count = 0;
    h->out = -1;
}
