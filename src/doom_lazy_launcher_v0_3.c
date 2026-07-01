#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define APP_NAME "Doom Lazy Launcher v0.3"
#define CONFIG_DIR_NAME ".config/doom-lazy-launcher"
#define CONFIG_FILE_NAME "config.txt"

typedef struct {
    char **items;
    int count;
    int cap;
} StringList;

typedef struct {
    char *name;
    char *engine;
    char *iwad;
    StringList mods;
    char *args;
} Profile;

typedef struct {
    Profile *items;
    int count;
    int cap;
} ProfileList;

typedef struct {
    char *engine_dir;
    char *iwad_dir;
    char *mod_dir;
    ProfileList profiles;
} Config;

static char *xstrdup(const char *s) {
    if (!s) return xstrdup("");
    char *p = strdup(s);
    if (!p) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
    }
    return p;
}

static void *xcalloc(size_t n, size_t s) {
    void *p = calloc(n, s);
    if (!p) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
    }
    return p;
}

static void *xrealloc(void *ptr, size_t s) {
    void *p = realloc(ptr, s);
    if (!p) {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
    }
    return p;
}

static void clean_exit_signal(int sig) {
    (void)sig;
    _exit(0);
}


static char *trim_in_place(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *end = s + strlen(s) - 1;
    while (end >= s && isspace((unsigned char)*end)) {
        *end = 0;
        end--;
    }
    return s;
}

static bool equals_ignore_case(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static bool ends_with_ignore_case(const char *s, const char *suffix) {
    size_t ls = strlen(s);
    size_t lf = strlen(suffix);
    if (lf > ls) return false;
    return equals_ignore_case(s + ls - lf, suffix);
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool path_exists(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0;
}

static bool is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_regular_or_symlink_target_file(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void sl_init(StringList *sl) {
    sl->items = NULL;
    sl->count = 0;
    sl->cap = 0;
}

static void sl_add_owned(StringList *sl, char *item) {
    if (sl->count == sl->cap) {
        sl->cap = sl->cap ? sl->cap * 2 : 8;
        sl->items = xrealloc(sl->items, sizeof(char *) * (size_t)sl->cap);
    }
    sl->items[sl->count++] = item;
}

static void sl_add(StringList *sl, const char *item) {
    sl_add_owned(sl, xstrdup(item));
}

static void sl_free(StringList *sl) {
    for (int i = 0; i < sl->count; i++) free(sl->items[i]);
    free(sl->items);
    sl->items = NULL;
    sl->count = 0;
    sl->cap = 0;
}

static int cmp_strings(const void *a, const void *b) {
    const char *sa = *(const char * const *)a;
    const char *sb = *(const char * const *)b;
    return strcasecmp(base_name(sa), base_name(sb));
}

static void sl_sort(StringList *sl) {
    if (sl->count > 1) qsort(sl->items, (size_t)sl->count, sizeof(char *), cmp_strings);
}

static void profile_init(Profile *p) {
    p->name = xstrdup("");
    p->engine = xstrdup("");
    p->iwad = xstrdup("");
    sl_init(&p->mods);
    p->args = xstrdup("");
}

static void profile_free(Profile *p) {
    free(p->name);
    free(p->engine);
    free(p->iwad);
    sl_free(&p->mods);
    free(p->args);
    p->name = p->engine = p->iwad = p->args = NULL;
}

static void pl_init(ProfileList *pl) {
    pl->items = NULL;
    pl->count = 0;
    pl->cap = 0;
}

static void pl_add_owned(ProfileList *pl, Profile p) {
    if (pl->count == pl->cap) {
        pl->cap = pl->cap ? pl->cap * 2 : 8;
        pl->items = xrealloc(pl->items, sizeof(Profile) * (size_t)pl->cap);
    }
    pl->items[pl->count++] = p;
}

static void pl_remove(ProfileList *pl, int idx) {
    if (idx < 0 || idx >= pl->count) return;
    profile_free(&pl->items[idx]);
    for (int i = idx; i < pl->count - 1; i++) pl->items[i] = pl->items[i + 1];
    pl->count--;
}

static void pl_free(ProfileList *pl) {
    for (int i = 0; i < pl->count; i++) profile_free(&pl->items[i]);
    free(pl->items);
    pl->items = NULL;
    pl->count = 0;
    pl->cap = 0;
}

static void config_init(Config *cfg) {
    cfg->engine_dir = xstrdup("");
    cfg->iwad_dir = xstrdup("");
    cfg->mod_dir = xstrdup("");
    pl_init(&cfg->profiles);
}

static void config_free(Config *cfg) {
    free(cfg->engine_dir);
    free(cfg->iwad_dir);
    free(cfg->mod_dir);
    pl_free(&cfg->profiles);
}

static void set_string(char **dst, const char *src) {
    free(*dst);
    *dst = xstrdup(src ? src : "");
}

static void ensure_config_dir(char *out_path, size_t out_size) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = ".";

    char dot_config[PATH_MAX];
    snprintf(dot_config, sizeof(dot_config), "%s/.config", home);
    if (!is_dir(dot_config)) {
        if (mkdir(dot_config, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "Could not create config dir '%s': %s\n", dot_config, strerror(errno));
        }
    }

    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/doom-lazy-launcher", dot_config);
    if (!is_dir(dir)) {
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "Could not create config dir '%s': %s\n", dir, strerror(errno));
        }
    }
    snprintf(out_path, out_size, "%s/%s", dir, CONFIG_FILE_NAME);
}

static void save_config(const Config *cfg, const char *config_path) {
    FILE *f = fopen(config_path, "w");
    if (!f) {
        fprintf(stderr, "Could not save config '%s': %s\n", config_path, strerror(errno));
        return;
    }
    fprintf(f, "# %s config\n", APP_NAME);
    fprintf(f, "version=1\n");
    fprintf(f, "engine_dir=%s\n", cfg->engine_dir ? cfg->engine_dir : "");
    fprintf(f, "iwad_dir=%s\n", cfg->iwad_dir ? cfg->iwad_dir : "");
    fprintf(f, "mod_dir=%s\n", cfg->mod_dir ? cfg->mod_dir : "");
    for (int i = 0; i < cfg->profiles.count; i++) {
        const Profile *p = &cfg->profiles.items[i];
        fprintf(f, "[profile]\n");
        fprintf(f, "name=%s\n", p->name ? p->name : "");
        fprintf(f, "engine=%s\n", p->engine ? p->engine : "");
        fprintf(f, "iwad=%s\n", p->iwad ? p->iwad : "");
        fprintf(f, "args=%s\n", p->args ? p->args : "");
        for (int m = 0; m < p->mods.count; m++) fprintf(f, "mod=%s\n", p->mods.items[m]);
        fprintf(f, "[/profile]\n");
    }
    fclose(f);
}

static void load_config(Config *cfg, const char *config_path) {
    FILE *f = fopen(config_path, "r");
    if (!f) return;

    char *line = NULL;
    size_t n = 0;
    Profile current;
    bool in_profile = false;
    profile_init(&current);

    while (getline(&line, &n, f) != -1) {
        char *s = trim_in_place(line);
        if (!s[0] || s[0] == '#') continue;
        if (strcmp(s, "[profile]") == 0) {
            if (in_profile) profile_free(&current);
            profile_init(&current);
            in_profile = true;
            continue;
        }
        if (strcmp(s, "[/profile]") == 0) {
            if (in_profile) {
                if (current.name && current.name[0] && current.engine && current.engine[0] && current.iwad && current.iwad[0]) {
                    pl_add_owned(&cfg->profiles, current);
                    profile_init(&current);
                }
                in_profile = false;
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim_in_place(s);
        char *val = trim_in_place(eq + 1);
        if (in_profile) {
            if (strcmp(key, "name") == 0) set_string(&current.name, val);
            else if (strcmp(key, "engine") == 0) set_string(&current.engine, val);
            else if (strcmp(key, "iwad") == 0) set_string(&current.iwad, val);
            else if (strcmp(key, "args") == 0) set_string(&current.args, val);
            else if (strcmp(key, "mod") == 0 && val[0]) sl_add(&current.mods, val);
        } else {
            if (strcmp(key, "engine_dir") == 0) set_string(&cfg->engine_dir, val);
            else if (strcmp(key, "iwad_dir") == 0) set_string(&cfg->iwad_dir, val);
            else if (strcmp(key, "mod_dir") == 0) set_string(&cfg->mod_dir, val);
        }
    }

    if (in_profile) profile_free(&current);
    else profile_free(&current);
    free(line);
    fclose(f);
}

static char *read_line_prompt(const char *prompt) {
    printf("%s", prompt);
    fflush(stdout);
    char *line = NULL;
    size_t n = 0;
    ssize_t got = getline(&line, &n, stdin);
    if (got < 0) {
        free(line);
        printf("\nInput closed. Exiting.\n");
        fflush(stdout);
        exit(0);
    }
    if (got > 0 && line[got - 1] == '\n') line[got - 1] = 0;
    return line;
}

static void wait_enter(void) {
    char *line = read_line_prompt("\nPress Enter to continue...");
    free(line);
}

static bool ask_yes_no(const char *prompt, bool def) {
    char full[512];
    snprintf(full, sizeof(full), "%s [%s/%s]: ", prompt, def ? "Y" : "y", def ? "n" : "N");
    char *line = read_line_prompt(full);
    char *s = trim_in_place(line);
    bool result = def;
    if (s[0]) {
        if (tolower((unsigned char)s[0]) == 'y') result = true;
        else if (tolower((unsigned char)s[0]) == 'n') result = false;
    }
    free(line);
    return result;
}

static int ask_int_range(const char *prompt, int min, int max, int def) {
    for (;;) {
        char full[256];
        if (def >= min && def <= max) snprintf(full, sizeof(full), "%s [%d]: ", prompt, def);
        else snprintf(full, sizeof(full), "%s: ", prompt);
        char *line = read_line_prompt(full);
        char *s = trim_in_place(line);
        if (!s[0] && def >= min && def <= max) {
            free(line);
            return def;
        }
        char *end = NULL;
        long v = strtol(s, &end, 10);
        if (s[0] && end && *trim_in_place(end) == 0 && v >= min && v <= max) {
            free(line);
            return (int)v;
        }
        printf("Enter a number from %d to %d.\n", min, max);
        free(line);
    }
}

static bool file_matches_engine(const char *name, const char *path) {
    (void)name;
    return is_regular_or_symlink_target_file(path);
}

static bool file_matches_iwad(const char *name, const char *path) {
    (void)path;
    return ends_with_ignore_case(name, ".wad");
}

static bool file_matches_mod(const char *name, const char *path) {
    if (is_dir(path)) return true;
    if (!is_regular_or_symlink_target_file(path)) return false;
    return ends_with_ignore_case(name, ".wad") ||
           ends_with_ignore_case(name, ".pk3") ||
           ends_with_ignore_case(name, ".pk7") ||
           ends_with_ignore_case(name, ".deh") ||
           ends_with_ignore_case(name, ".bex") ||
           ends_with_ignore_case(name, ".zip");
}

typedef enum { SCAN_ENGINE, SCAN_IWAD, SCAN_MOD } ScanType;

static StringList scan_dir(const char *dir, ScanType type) {
    StringList out;
    sl_init(&out);
    if (!dir || !dir[0] || !is_dir(dir)) return out;
    DIR *d = opendir(dir);
    if (!d) return out;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (name[0] == '.') continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        bool ok = false;
        if (type == SCAN_ENGINE) ok = file_matches_engine(name, path);
        else if (type == SCAN_IWAD) ok = file_matches_iwad(name, path);
        else ok = file_matches_mod(name, path);
        if (ok) sl_add(&out, path);
    }
    closedir(d);
    sl_sort(&out);
    return out;
}

static void print_path_status(const char *label, const char *path) {
    printf("%-12s %s", label, path && path[0] ? path : "(not set)");
    if (path && path[0] && !is_dir(path)) printf("  [missing]");
    printf("\n");
}

static void print_header(const Config *cfg) {
    printf("\n=== %s ===\n", APP_NAME);
    print_path_status("Engines:", cfg->engine_dir);
    print_path_status("IWADs:", cfg->iwad_dir);
    print_path_status("Mods:", cfg->mod_dir);
    printf("Profiles:    %d\n", cfg->profiles.count);
}

static void configure_dirs(Config *cfg, const char *config_path) {
    printf("\nSet folders. Press Enter to keep the current value.\n");
    printf("Tip: use full paths, like /home/you/Games/Doom/engines\n\n");

    printf("Current engine folder: %s\n", cfg->engine_dir[0] ? cfg->engine_dir : "(not set)");
    char *s = read_line_prompt("New engine folder: ");
    char *t = trim_in_place(s);
    if (t[0]) set_string(&cfg->engine_dir, t);
    free(s);

    printf("Current IWAD folder: %s\n", cfg->iwad_dir[0] ? cfg->iwad_dir : "(not set)");
    s = read_line_prompt("New IWAD folder: ");
    t = trim_in_place(s);
    if (t[0]) set_string(&cfg->iwad_dir, t);
    free(s);

    printf("Current mod folder: %s\n", cfg->mod_dir[0] ? cfg->mod_dir : "(not set)");
    s = read_line_prompt("New mod folder: ");
    t = trim_in_place(s);
    if (t[0]) set_string(&cfg->mod_dir, t);
    free(s);

    save_config(cfg, config_path);
    printf("Saved.\n");
    wait_enter();
}

static int choose_from_list(const StringList *list, const char *title) {
    if (!list || list->count == 0) return -1;
    printf("\n%s\n", title);
    for (int i = 0; i < list->count; i++) {
        const char *path = list->items[i];
        printf("%3d. %s", i + 1, base_name(path));
        if (is_dir(path)) printf("/ [folder]");
        printf("\n");
    }
    printf("  0. Cancel\n");
    int choice = ask_int_range("Choose", 0, list->count, -1);
    if (choice == 0) return -1;
    return choice - 1;
}

static bool sl_contains_string(const StringList *sl, const char *item) {
    if (!sl || !item) return false;
    for (int i = 0; i < sl->count; i++) {
        if (strcmp(sl->items[i], item) == 0) return true;
    }
    return false;
}

static void sl_remove_index(StringList *sl, int idx) {
    if (!sl || idx < 0 || idx >= sl->count) return;
    free(sl->items[idx]);
    for (int i = idx; i < sl->count - 1; i++) sl->items[i] = sl->items[i + 1];
    sl->count--;
}

static void sl_swap_items(StringList *sl, int a, int b) {
    if (!sl || a < 0 || b < 0 || a >= sl->count || b >= sl->count) return;
    char *tmp = sl->items[a];
    sl->items[a] = sl->items[b];
    sl->items[b] = tmp;
}

static void print_selected_mods(const StringList *selected) {
    printf("\nCurrent mod load order:\n");
    if (!selected || selected->count == 0) {
        printf("  (no mods selected)\n");
        return;
    }
    for (int i = 0; i < selected->count; i++) {
        printf("%3d. %s", i + 1, base_name(selected->items[i]));
        if (is_dir(selected->items[i])) printf("/ [folder]");
        printf("\n");
    }
}

static void print_available_mods(const StringList *mods, const StringList *selected) {
    printf("\nAvailable mods:\n");
    for (int i = 0; i < mods->count; i++) {
        const char *path = mods->items[i];
        printf("%3d. [%c] %s", i + 1, sl_contains_string(selected, path) ? 'x' : ' ', base_name(path));
        if (is_dir(path)) printf("/ [folder]");
        printf("\n");
    }
}

static void add_mods_by_number(StringList *selected, const StringList *mods) {
    print_available_mods(mods, selected);
    printf("\nEnter numbers separated by spaces or commas. Example: 2 5 1\n");
    printf("Press Enter to cancel. Already-selected mods are skipped.\n");
    char *line = read_line_prompt("Add mods: ");
    char *p = line;
    int added = 0;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) {
            while (*p && !isspace((unsigned char)*p) && *p != ',') p++;
            continue;
        }
        if (v >= 1 && v <= mods->count) {
            const char *chosen = mods->items[v - 1];
            if (!sl_contains_string(selected, chosen)) {
                sl_add(selected, chosen);
                added++;
            }
        }
        p = end;
    }
    printf("Added %d mod%s.\n", added, added == 1 ? "" : "s");
    free(line);
}

static StringList choose_mods(const StringList *mods) {
    StringList selected;
    sl_init(&selected);
    if (!mods || mods->count == 0) {
        printf("\nNo mods found. Continuing with no mods.\n");
        return selected;
    }

    printf("\nBuild the mod load order.\n");
    printf("Tip: load order matters. Add gameplay mods, map packs, patches, and music in the order you want them passed to -file.\n");

    for (;;) {
        print_selected_mods(&selected);
        printf("\nMod options:\n");
        printf("1. Add mod(s)\n");
        printf("2. Move mod up\n");
        printf("3. Move mod down\n");
        printf("4. Remove mod\n");
        printf("5. Clear selected mods\n");
        printf("6. Done\n");
        int choice = ask_int_range("Choose", 1, 6, 6);
        if (choice == 1) {
            add_mods_by_number(&selected, mods);
        } else if (choice == 2) {
            if (selected.count < 2) {
                printf("Need at least two selected mods to reorder.\n");
                continue;
            }
            int idx = ask_int_range("Move which mod up", 1, selected.count, -1) - 1;
            if (idx > 0) sl_swap_items(&selected, idx, idx - 1);
            else printf("That mod is already first.\n");
        } else if (choice == 3) {
            if (selected.count < 2) {
                printf("Need at least two selected mods to reorder.\n");
                continue;
            }
            int idx = ask_int_range("Move which mod down", 1, selected.count, -1) - 1;
            if (idx >= 0 && idx < selected.count - 1) sl_swap_items(&selected, idx, idx + 1);
            else printf("That mod is already last.\n");
        } else if (choice == 4) {
            if (selected.count == 0) {
                printf("No selected mods to remove.\n");
                continue;
            }
            int idx = ask_int_range("Remove which mod", 1, selected.count, -1) - 1;
            if (idx >= 0) sl_remove_index(&selected, idx);
        } else if (choice == 5) {
            if (selected.count > 0 && ask_yes_no("Clear all selected mods", false)) sl_free(&selected), sl_init(&selected);
        } else if (choice == 6) {
            return selected;
        }
    }
}

static void print_shell_quoted(const char *s) {
    if (!s || !s[0]) {
        printf("''");
        return;
    }
    bool simple = true;
    for (const char *p = s; *p; p++) {
        if (!(isalnum((unsigned char)*p) || strchr("/_:.,+=@%~-", *p))) {
            simple = false;
            break;
        }
    }
    if (simple) {
        printf("%s", s);
        return;
    }
    putchar('\'');
    for (const char *p = s; *p; p++) {
        if (*p == '\'') printf("'\\''");
        else putchar(*p);
    }
    putchar('\'');
}

static char **parse_extra_args(const char *input, int *out_count) {
    *out_count = 0;
    StringList toks;
    sl_init(&toks);
    const char *p = input ? input : "";
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        size_t cap = 64, len = 0;
        char *buf = xcalloc(cap, 1);
        char quote = 0;
        while (*p) {
            char c = *p;
            if (quote) {
                if (c == quote) {
                    quote = 0;
                    p++;
                    continue;
                }
                if (c == '\\' && quote == '"' && p[1]) {
                    p++;
                    c = *p;
                }
            } else {
                if (isspace((unsigned char)c)) break;
                if (c == '\'' || c == '"') {
                    quote = c;
                    p++;
                    continue;
                }
                if (c == '\\' && p[1]) {
                    p++;
                    c = *p;
                }
            }
            if (len + 2 >= cap) {
                cap *= 2;
                buf = xrealloc(buf, cap);
            }
            buf[len++] = c;
            buf[len] = 0;
            p++;
        }
        sl_add_owned(&toks, buf);
        while (*p && isspace((unsigned char)*p)) p++;
    }
    char **arr = xcalloc((size_t)toks.count + 1, sizeof(char *));
    for (int i = 0; i < toks.count; i++) arr[i] = toks.items[i];
    *out_count = toks.count;
    free(toks.items);
    return arr;
}

static void free_args(char **args, int count) {
    if (!args) return;
    for (int i = 0; i < count; i++) free(args[i]);
    free(args);
}

static void print_command(const Profile *p) {
    printf("\nCommand:\n  ");
    print_shell_quoted(p->engine);
    printf(" -iwad ");
    print_shell_quoted(p->iwad);
    if (p->mods.count > 0) {
        printf(" -file");
        for (int i = 0; i < p->mods.count; i++) {
            putchar(' ');
            print_shell_quoted(p->mods.items[i]);
        }
    }
    int argc = 0;
    char **extra = parse_extra_args(p->args, &argc);
    for (int i = 0; i < argc; i++) {
        putchar(' ');
        print_shell_quoted(extra[i]);
    }
    free_args(extra, argc);
    printf("\n");
}

static bool ensure_engine_executable(const char *engine) {
    struct stat st;
    if (stat(engine, &st) != 0) {
        printf("Engine not found: %s\n", engine);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        printf("Engine is not a regular executable file: %s\n", engine);
        return false;
    }
    if (access(engine, X_OK) == 0) return true;
    printf("\nEngine is not marked executable:\n  %s\n", engine);
    if (!ask_yes_no("Run chmod +x on it", true)) return false;
    if (chmod(engine, st.st_mode | S_IXUSR) != 0) {
        printf("chmod failed: %s\n", strerror(errno));
        return false;
    }
    return access(engine, X_OK) == 0;
}

static int launch_profile(const Profile *p) {
    if (!path_exists(p->engine)) {
        printf("Missing engine: %s\n", p->engine);
        return -1;
    }
    if (!path_exists(p->iwad)) {
        printf("Missing IWAD: %s\n", p->iwad);
        return -1;
    }
    for (int i = 0; i < p->mods.count; i++) {
        if (!path_exists(p->mods.items[i])) printf("Warning: missing mod: %s\n", p->mods.items[i]);
    }
    if (!ensure_engine_executable(p->engine)) return -1;

    int extra_count = 0;
    char **extra = parse_extra_args(p->args, &extra_count);
    int argc = 1 + 2 + (p->mods.count > 0 ? 1 + p->mods.count : 0) + extra_count;
    char **argv = xcalloc((size_t)argc + 1, sizeof(char *));
    int n = 0;
    argv[n++] = p->engine;
    argv[n++] = "-iwad";
    argv[n++] = p->iwad;
    if (p->mods.count > 0) {
        argv[n++] = "-file";
        for (int i = 0; i < p->mods.count; i++) argv[n++] = p->mods.items[i];
    }
    for (int i = 0; i < extra_count; i++) argv[n++] = extra[i];
    argv[n] = NULL;

    printf("\nLaunching...\n");
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        execv(p->engine, argv);
        fprintf(stderr, "Launch failed: %s\n", strerror(errno));
        _exit(127);
    }
    if (pid < 0) {
        printf("fork failed: %s\n", strerror(errno));
        free(argv);
        free_args(extra, extra_count);
        return -1;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) printf("Doom exited with status %d.\n", WEXITSTATUS(status));
    else if (WIFSIGNALED(status)) printf("Doom was terminated by signal %d.\n", WTERMSIG(status));
    else printf("Doom process finished.\n");
    free(argv);
    free_args(extra, extra_count);
    return status;
}

static Profile make_profile_from_choices(Config *cfg) {
    Profile p;
    profile_init(&p);

    StringList engines = scan_dir(cfg->engine_dir, SCAN_ENGINE);
    StringList iwads = scan_dir(cfg->iwad_dir, SCAN_IWAD);
    StringList mods = scan_dir(cfg->mod_dir, SCAN_MOD);

    if (engines.count == 0) printf("\nNo engines found in: %s\n", cfg->engine_dir[0] ? cfg->engine_dir : "(not set)");
    if (iwads.count == 0) printf("\nNo IWADs found in: %s\n", cfg->iwad_dir[0] ? cfg->iwad_dir : "(not set)");
    if (engines.count == 0 || iwads.count == 0) {
        sl_free(&engines);
        sl_free(&iwads);
        sl_free(&mods);
        return p;
    }

    int e = choose_from_list(&engines, "Choose engine");
    if (e < 0) goto done;
    int w = choose_from_list(&iwads, "Choose base IWAD");
    if (w < 0) goto done;

    set_string(&p.engine, engines.items[e]);
    set_string(&p.iwad, iwads.items[w]);

    sl_free(&p.mods);
    p.mods = choose_mods(&mods);

    char *args = read_line_prompt("Extra args, or Enter for none: ");
    char *ta = trim_in_place(args);
    set_string(&p.args, ta);
    free(args);

done:
    sl_free(&engines);
    sl_free(&iwads);
    sl_free(&mods);
    return p;
}

static bool profile_ready(const Profile *p) {
    return p && p->engine && p->engine[0] && p->iwad && p->iwad[0];
}

static void save_profile_prompt(Config *cfg, const char *config_path, Profile *p) {
    if (!ask_yes_no("Save this as a profile", true)) return;
    char *name = read_line_prompt("Profile name: ");
    char *tn = trim_in_place(name);
    if (!tn[0]) {
        printf("Not saved: empty name.\n");
        free(name);
        return;
    }
    set_string(&p->name, tn);
    free(name);
    pl_add_owned(&cfg->profiles, *p);
    profile_init(p);
    save_config(cfg, config_path);
    printf("Profile saved.\n");
}

static void new_launch(Config *cfg, const char *config_path) {
    if (!cfg->engine_dir[0] || !cfg->iwad_dir[0] || !cfg->mod_dir[0]) {
        printf("\nFolders are not fully set yet.\n");
        configure_dirs(cfg, config_path);
    }
    Profile p = make_profile_from_choices(cfg);
    if (!profile_ready(&p)) {
        profile_free(&p);
        wait_enter();
        return;
    }
    print_command(&p);
    if (ask_yes_no("Launch now", true)) launch_profile(&p);
    save_profile_prompt(cfg, config_path, &p);
    profile_free(&p);
    wait_enter();
}

static void launch_saved_profile(Config *cfg) {
    if (cfg->profiles.count == 0) {
        printf("\nNo profiles yet. Create one with New launch.\n");
        wait_enter();
        return;
    }
    printf("\nSaved profiles\n");
    for (int i = 0; i < cfg->profiles.count; i++) {
        Profile *p = &cfg->profiles.items[i];
        printf("%3d. %s  [%s + %d mod%s]\n", i + 1, p->name, base_name(p->iwad), p->mods.count, p->mods.count == 1 ? "" : "s");
    }
    printf("  0. Cancel\n");
    int choice = ask_int_range("Choose", 0, cfg->profiles.count, -1);
    if (choice == 0) return;
    Profile *p = &cfg->profiles.items[choice - 1];
    print_command(p);
    if (ask_yes_no("Launch this profile", true)) launch_profile(p);
    wait_enter();
}

static void delete_profile(Config *cfg, const char *config_path) {
    if (cfg->profiles.count == 0) {
        printf("\nNo profiles to delete.\n");
        wait_enter();
        return;
    }
    printf("\nDelete profile\n");
    for (int i = 0; i < cfg->profiles.count; i++) printf("%3d. %s\n", i + 1, cfg->profiles.items[i].name);
    printf("  0. Cancel\n");
    int choice = ask_int_range("Choose", 0, cfg->profiles.count, -1);
    if (choice == 0) return;
    if (ask_yes_no("Delete it", false)) {
        pl_remove(&cfg->profiles, choice - 1);
        save_config(cfg, config_path);
        printf("Deleted.\n");
    }
    wait_enter();
}

static void show_config_info(const Config *cfg, const char *config_path) {
    printf("\nConfig file:\n  %s\n\n", config_path);
    printf("Recognized mods: .wad, .pk3, .pk7, .deh, .bex, .zip, and folders.\n");
    printf("Recognized IWADs: .wad files in the IWAD folder.\n");
    printf("Engines: regular files in the engine folder. AppImages are fine if executable.\n");
    printf("\nCurrent folders:\n");
    print_path_status("Engines:", cfg->engine_dir);
    print_path_status("IWADs:", cfg->iwad_dir);
    print_path_status("Mods:", cfg->mod_dir);
    wait_enter();
}

static void main_menu(Config *cfg, const char *config_path) {
    for (;;) {
        print_header(cfg);
        printf("\n");
        printf("1. Launch saved profile\n");
        printf("2. New launch\n");
        printf("3. Set folders\n");
        printf("4. Delete profile\n");
        printf("5. Show config/help\n");
        printf("6. Quit\n");
        int choice = ask_int_range("Choose", 1, 6, -1);
        switch (choice) {
            case 1: launch_saved_profile(cfg); break;
            case 2: new_launch(cfg, config_path); break;
            case 3: configure_dirs(cfg, config_path); break;
            case 4: delete_profile(cfg, config_path); break;
            case 5: show_config_info(cfg, config_path); break;
            case 6: return;
            default: break;
        }
    }
}

static void print_cli_help(void) {
    printf("%s\n", APP_NAME);
    printf("Usage:\n");
    printf("  DoomLazyLauncher            interactive mode\n");
    printf("  DoomLazyLauncher --help     show this help\n");
    printf("\nConfig is stored at ~/.config/doom-lazy-launcher/config.txt\n");
}

int main(int argc, char **argv) {
    signal(SIGHUP, clean_exit_signal);
    signal(SIGTERM, clean_exit_signal);

    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_cli_help();
        return 0;
    }
    char config_path[PATH_MAX];
    ensure_config_dir(config_path, sizeof(config_path));
    Config cfg;
    config_init(&cfg);
    load_config(&cfg, config_path);
    main_menu(&cfg, config_path);
    save_config(&cfg, config_path);
    config_free(&cfg);
    return 0;
}
