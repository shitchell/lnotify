#include "lnotify.h"
#include "log.h"
#include "protocol.h"
#include "socket.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_MSG_SIZE 65536

static void print_usage(const char *progname) {
    fprintf(stderr,
        "Usage: %s [OPTIONS] BODY\n"
        "\n"
        "Send a notification to lnotifyd.\n"
        "\n"
        "Options:\n"
        "  -t, --title TITLE        Notification title\n"
        "  -p, --priority PRIORITY  low, normal (default), critical\n"
        "      --app APP            Application name\n"
        "      --group GROUP_ID     Group ID for dedup\n"
        "      --timeout MS         Timeout in milliseconds\n"
        "      --dry-run            Show what would happen, don't send\n"
        "      --socket PATH        Override socket path\n"
        "      --system             Use system socket (/run/lnotify.sock)\n"
        "      --version            Show version\n"
        "  -h, --help               Show help\n",
        progname);
}

// Parse priority string to uint8_t. Returns 0/1/2 or 255 on error.
static uint8_t parse_priority(const char *str) {
    if (strcmp(str, "low") == 0)      return 0;
    if (strcmp(str, "normal") == 0)   return 1;
    if (strcmp(str, "critical") == 0) return 2;
    return 255;
}

int main(int argc, char *argv[]) {
    const char *title = NULL;
    const char *app = NULL;
    const char *group_id = NULL;
    const char *socket_path = NULL;
    uint8_t priority = 1;  // normal
    int32_t timeout_ms = -1;  // use config default
    bool system_mode = false;
    bool dry_run = false;

    // Long options with unique identifiers for options without short forms
    enum {
        OPT_APP = 256,
        OPT_GROUP,
        OPT_TIMEOUT,
        OPT_DRY_RUN,
        OPT_SOCKET,
        OPT_SYSTEM,
        OPT_VERSION,
    };

    static struct option long_opts[] = {
        {"title",    required_argument, NULL, 't'},
        {"priority", required_argument, NULL, 'p'},
        {"app",      required_argument, NULL, OPT_APP},
        {"group",    required_argument, NULL, OPT_GROUP},
        {"timeout",  required_argument, NULL, OPT_TIMEOUT},
        {"dry-run",  no_argument,       NULL, OPT_DRY_RUN},
        {"socket",   required_argument, NULL, OPT_SOCKET},
        {"system",   no_argument,       NULL, OPT_SYSTEM},
        {"version",  no_argument,       NULL, OPT_VERSION},
        {"help",     no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:p:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 't':
            title = optarg;
            break;
        case 'p':
            priority = parse_priority(optarg);
            if (priority == 255) {
                fprintf(stderr, "error: invalid priority '%s' (use: low, normal, critical)\n", optarg);
                return 1;
            }
            break;
        case OPT_APP:
            app = optarg;
            break;
        case OPT_GROUP:
            group_id = optarg;
            break;
        case OPT_TIMEOUT: {
            char *endp;
            long val = strtol(optarg, &endp, 10);
            if (*endp != '\0' || val < 0) {
                fprintf(stderr, "error: invalid timeout '%s' (must be a non-negative integer)\n", optarg);
                return 1;
            }
            timeout_ms = (int32_t)val;
            break;
        }
        case OPT_DRY_RUN:
            dry_run = true;
            break;
        case OPT_SOCKET:
            socket_path = optarg;
            break;
        case OPT_SYSTEM:
            system_mode = true;
            break;
        case OPT_VERSION:
            printf("lnotify %s\n", LNOTIFY_VERSION);
            return 0;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    // BODY is the first non-option argument
    if (optind >= argc) {
        fprintf(stderr, "error: no notification body provided\n\n");
        print_usage(argv[0]);
        return 1;
    }
    const char *body = argv[optind];

    // Resolve socket path
    if (!socket_path && system_mode) {
        socket_path = socket_default_path(true);
    }
    // If no explicit path, we'll try user socket then fall back to system
    // (handled below at connect time)

    // Build notification
    notification notif;
    if (notification_init(&notif, title, body) < 0) {
        fprintf(stderr, "error: notification allocation failed\n");
        return 1;
    }
    notif.priority = priority;
    notif.timeout_ms = timeout_ms;
    notif.app = app ? strdup(app) : NULL;
    notif.group_id = group_id ? strdup(group_id) : NULL;

    // Serialize
    uint8_t buf[MAX_MSG_SIZE];
    ssize_t msg_len = protocol_serialize(&notif, buf, sizeof(buf));
    if (msg_len < 0) {
        fprintf(stderr, "error: failed to serialize notification\n");
        notification_free(&notif);
        return 1;
    }

    // If dry-run, set the FIELD_DRY_RUN bit in the serialized field_mask.
    // field_mask is at offset 4 (after uint32 total_len), little-endian uint16.
    if (dry_run && msg_len >= PROTOCOL_HEADER_SIZE) {
        uint16_t field_mask;
        memcpy(&field_mask, buf + 4, 2);
        field_mask |= FIELD_DRY_RUN;
        memcpy(buf + 4, &field_mask, 2);
    }

    // Connect to daemon socket
    // If no explicit path, try user socket first, then system socket
    const char *try_paths[2] = { NULL, NULL };
    char user_path_buf[256];
    int try_count = 0;

    if (socket_path) {
        // Explicit path (--socket or --system) — only try that one
        try_paths[0] = socket_path;
        try_count = 1;
    } else {
        // Copy user path since socket_default_path uses a static buffer
        snprintf(user_path_buf, sizeof(user_path_buf), "%s",
                 socket_default_path(false));
        try_paths[0] = user_path_buf;
        try_paths[1] = socket_default_path(true);  // "/run/lnotify.sock"
        try_count = 2;
        // If both resolve to the same path, only try once
        if (strcmp(try_paths[0], try_paths[1]) == 0) {
            try_count = 1;
        }
    }

    int fd = -1;
    const char *connected_path = NULL;
    for (int i = 0; i < try_count; i++) {
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            fprintf(stderr, "error: socket(): %s\n", strerror(errno));
            notification_free(&notif);
            return 1;
        }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (strlen(try_paths[i]) >= sizeof(addr.sun_path)) {
            fprintf(stderr, "error: socket path too long: %s\n", try_paths[i]);
            close(fd);
            fd = -1;
            continue;
        }
        strncpy(addr.sun_path, try_paths[i], sizeof(addr.sun_path) - 1);

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            connected_path = try_paths[i];
            break;
        }

        // Connection failed — if more paths to try, continue silently
        close(fd);
        fd = -1;
    }

    if (fd < 0) {
        // All paths failed
        fprintf(stderr, "error: cannot connect to lnotifyd — not running?\n");
        fprintf(stderr, "  tried:");
        for (int i = 0; i < try_count; i++) {
            fprintf(stderr, " %s", try_paths[i]);
        }
        fprintf(stderr, "\n");
        notification_free(&notif);
        return 1;
    }
    (void)connected_path;

    // Write serialized data
    ssize_t total_written = 0;
    while (total_written < msg_len) {
        ssize_t n = write(fd, buf + total_written, (size_t)(msg_len - total_written));
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "error: write(): %s\n", strerror(errno));
            close(fd);
            notification_free(&notif);
            return 1;
        }
        total_written += n;
    }

    if (dry_run) {
        // Signal end of request so daemon reads EOF, but keep socket open
        // for reading the response
        shutdown(fd, SHUT_WR);

        // Read and print the daemon's diagnostic response
        char rbuf[4096];
        ssize_t total_read = 0;
        for (;;) {
            ssize_t n = read(fd, rbuf + total_read,
                             sizeof(rbuf) - 1 - (size_t)total_read);
            if (n < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "error: read(): %s\n", strerror(errno));
                break;
            }
            if (n == 0) break;
            total_read += n;
            if ((size_t)total_read >= sizeof(rbuf) - 1) break;
        }
        if (total_read > 0) {
            rbuf[total_read] = '\0';
            printf("%s", rbuf);
        }
    }

    close(fd);
    notification_free(&notif);
    return 0;
}
