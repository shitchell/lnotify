#include "config.h"
#include "test_util.h"
#include <string.h>
#include <stdio.h>

void test_config_suite(void) {
    // Test: defaults populated without any file
    {
        lnotify_config cfg;
        config_defaults(&cfg);
        ASSERT_EQ(cfg.default_timeout, 5000, "default timeout is 5000");
        ASSERT_STR_EQ(cfg.position, "top-right", "default position");
        ASSERT_EQ(cfg.border_width, 2, "default border width");
        ASSERT_EQ(cfg.border_radius, 12, "default border radius");
        ASSERT_EQ(cfg.padding, 20, "default padding");
        ASSERT_EQ(cfg.margin, 30, "default margin");
        ASSERT_EQ(cfg.font_size, 14, "default font size");

        // Default colors
        ASSERT_EQ(cfg.bg_color.r, 0x28, "default bg red");
        ASSERT_EQ(cfg.bg_color.g, 0x28, "default bg green");
        ASSERT_EQ(cfg.bg_color.b, 0x28, "default bg blue");
        ASSERT_EQ(cfg.bg_color.a, 0xE6, "default bg alpha");

        ASSERT_EQ(cfg.fg_color.r, 0xFF, "default fg red");
        ASSERT_EQ(cfg.fg_color.g, 0xFF, "default fg green");
        ASSERT_EQ(cfg.fg_color.b, 0xFF, "default fg blue");
        ASSERT_EQ(cfg.fg_color.a, 0xFF, "default fg alpha");

        ASSERT_EQ(cfg.border_color.r, 0x64, "default border red");
        ASSERT_EQ(cfg.border_color.g, 0xA0, "default border green");
        ASSERT_EQ(cfg.border_color.b, 0xFF, "default border blue");
        ASSERT_EQ(cfg.border_color.a, 0xFF, "default border alpha");

        // Default font path
        ASSERT_STR_EQ(cfg.font_path,
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "default font path");

        // Default SSH settings
        ASSERT_STR_EQ(cfg.ssh_modes, "osc,overlay,text", "default ssh_modes");
        ASSERT_STR_EQ(cfg.ssh_fullscreen_apps,
            "vim nvim less man htop top nano emacs",
            "default ssh_fullscreen_apps");
        ASSERT_FALSE(cfg.ssh_notify_over_fullscreen,
            "default ssh_notify_over_fullscreen is false");
        ASSERT_STR_EQ(cfg.ssh_groups, "", "default ssh_groups empty");
        ASSERT_STR_EQ(cfg.ssh_users, "", "default ssh_users empty");
    }

    // Test: parse a config file
    {
        const char *path = "/tmp/lnotify_test.conf";
        FILE *f = fopen(path, "w");
        fprintf(f, "# comment line\n");
        fprintf(f, "default_timeout = 3000\n");
        fprintf(f, "position = bottom-left\n");
        fprintf(f, "bg_color = #112233FF\n");
        fprintf(f, "border_width = 4\n");
        fprintf(f, "ssh_users = alice bob\n");
        fprintf(f, "ssh_modes = overlay,text\n");
        fclose(f);

        lnotify_config cfg;
        config_defaults(&cfg);
        int rc = config_load(&cfg, path);
        ASSERT_EQ(rc, 0, "config_load succeeds");
        ASSERT_EQ(cfg.default_timeout, 3000, "timeout overridden");
        ASSERT_STR_EQ(cfg.position, "bottom-left", "position overridden");
        ASSERT_EQ(cfg.border_width, 4, "border_width overridden");
        ASSERT_STR_EQ(cfg.ssh_users, "alice bob", "ssh_users parsed");
        ASSERT_STR_EQ(cfg.ssh_modes, "overlay,text", "ssh_modes parsed");

        // Check color parsing (RRGGBBAA -> separate r,g,b,a)
        ASSERT_EQ(cfg.bg_color.r, 0x11, "bg red");
        ASSERT_EQ(cfg.bg_color.g, 0x22, "bg green");
        ASSERT_EQ(cfg.bg_color.b, 0x33, "bg blue");
        ASSERT_EQ(cfg.bg_color.a, 0xFF, "bg alpha");

        // Unchanged fields keep defaults
        ASSERT_EQ(cfg.font_size, 14, "font_size unchanged");
        ASSERT_EQ(cfg.border_radius, 12, "border_radius unchanged");

        config_free(&cfg);
        remove(path);
    }

    // Test: missing file returns error, defaults preserved
    {
        lnotify_config cfg;
        config_defaults(&cfg);
        int rc = config_load(&cfg, "/tmp/nonexistent_lnotify.conf");
        ASSERT_TRUE(rc != 0, "config_load fails on missing file");
        ASSERT_EQ(cfg.default_timeout, 5000, "defaults preserved");
        config_free(&cfg);
    }

    // Test: malformed lines are skipped
    {
        const char *path = "/tmp/lnotify_test_bad.conf";
        FILE *f = fopen(path, "w");
        fprintf(f, "no_equals_sign\n");
        fprintf(f, "= no_key\n");
        fprintf(f, "default_timeout = 9999\n");
        fprintf(f, "unknown_key = ignored\n");
        fclose(f);

        lnotify_config cfg;
        config_defaults(&cfg);
        int rc = config_load(&cfg, path);
        ASSERT_EQ(rc, 0, "config_load succeeds despite bad lines");
        ASSERT_EQ(cfg.default_timeout, 9999, "valid line still parsed");

        config_free(&cfg);
        remove(path);
    }

    // Test: color parsing edge cases
    {
        lnotify_color c;
        ASSERT_EQ(config_parse_color("#AABBCCDD", &c), 0, "parse 8-char hex");
        ASSERT_EQ(c.r, 0xAA, "red");
        ASSERT_EQ(c.g, 0xBB, "green");
        ASSERT_EQ(c.b, 0xCC, "blue");
        ASSERT_EQ(c.a, 0xDD, "alpha");

        // Lowercase hex
        ASSERT_EQ(config_parse_color("#aabbccdd", &c), 0, "parse lowercase hex");
        ASSERT_EQ(c.r, 0xAA, "lowercase red");
        ASSERT_EQ(c.a, 0xDD, "lowercase alpha");

        ASSERT_TRUE(config_parse_color("AABBCCDD", &c) != 0, "reject missing #");
        ASSERT_TRUE(config_parse_color("#AABB", &c) != 0, "reject short hex");
        ASSERT_TRUE(config_parse_color("#GGHHIIJJ", &c) != 0, "reject non-hex chars");
        ASSERT_TRUE(config_parse_color("", &c) != 0, "reject empty string");
        ASSERT_TRUE(config_parse_color("#", &c) != 0, "reject lone hash");
    }

    // Test: whitespace handling
    {
        const char *path = "/tmp/lnotify_test_ws.conf";
        FILE *f = fopen(path, "w");
        fprintf(f, "  default_timeout  =  7777  \n");
        fprintf(f, "position=center\n");
        fprintf(f, "\n");
        fprintf(f, "   \n");
        fprintf(f, "  # indented comment\n");
        fprintf(f, "border_width=5\n");
        fclose(f);

        lnotify_config cfg;
        config_defaults(&cfg);
        int rc = config_load(&cfg, path);
        ASSERT_EQ(rc, 0, "config_load with whitespace");
        ASSERT_EQ(cfg.default_timeout, 7777, "timeout with extra whitespace");
        ASSERT_STR_EQ(cfg.position, "center", "position no spaces around =");
        ASSERT_EQ(cfg.border_width, 5, "border_width no spaces");

        config_free(&cfg);
        remove(path);
    }

    // Test: boolean parsing
    {
        const char *path = "/tmp/lnotify_test_bool.conf";
        FILE *f = fopen(path, "w");
        fprintf(f, "ssh_notify_over_fullscreen = true\n");
        fclose(f);

        lnotify_config cfg;
        config_defaults(&cfg);
        int rc = config_load(&cfg, path);
        ASSERT_EQ(rc, 0, "config_load bool");
        ASSERT_TRUE(cfg.ssh_notify_over_fullscreen, "bool true parsed");

        // Now test false
        f = fopen(path, "w");
        fprintf(f, "ssh_notify_over_fullscreen = false\n");
        fclose(f);

        config_defaults(&cfg);
        // Set it to true first to verify false overrides
        cfg.ssh_notify_over_fullscreen = true;
        rc = config_load(&cfg, path);
        ASSERT_EQ(rc, 0, "config_load bool false");
        ASSERT_FALSE(cfg.ssh_notify_over_fullscreen, "bool false parsed");

        config_free(&cfg);
        remove(path);
    }
}
