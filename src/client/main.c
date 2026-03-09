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

    // Handle --dry-run (stub for Task 17)
    if (dry_run) {
        fprintf(stderr, "dry-run mode not yet implemented\n");
        return 0;
    }

    // Resolve socket path
    if (!socket_path) {
        socket_path = socket_default_path(system_mode);
    }

    // Build notification
    notification notif;
    notification_init(&notif, title, body);
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

    // Connect to daemon socket
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "error: socket(): %s\n", strerror(errno));
        notification_free(&notif);
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "error: socket path too long: %s\n", socket_path);
        close(fd);
        notification_free(&notif);
        return 1;
    }
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "error: cannot connect to socket at %s — lnotifyd not running?\n", socket_path);
        } else if (errno == ECONNREFUSED) {
            fprintf(stderr, "error: connection refused at %s — lnotifyd not running?\n", socket_path);
        } else {
            fprintf(stderr, "error: connect(%s): %s\n", socket_path, strerror(errno));
        }
        close(fd);
        notification_free(&notif);
        return 1;
    }

    // Write serialized data (fire-and-forget)
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

    close(fd);
    notification_free(&notif);
    return 0;
}
