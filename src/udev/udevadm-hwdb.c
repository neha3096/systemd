/* SPDX-License-Identifier: LGPL-2.1+ */

#include <ctype.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>

#include "alloc-util.h"
#include "conf-files.h"
#include "fileio.h"
#include "fs-util.h"
#include "hwdb-internal.h"
#include "hwdb-util.h"
#include "label.h"
#include "mkdir.h"
#include "path-util.h"
#include "strbuf.h"
#include "string-util.h"
#include "udev.h"
#include "udevadm.h"
#include "util.h"

/*
 * Generic udev properties, key/value database based on modalias strings.
 * Uses a Patricia/radix trie to index all matches for efficient lookup.
 */

static const char *arg_test = NULL;
static const char *arg_root = NULL;
static const char *arg_hwdb_bin_dir = "/etc/udev";
static bool arg_update = false;
static bool arg_strict = false;

static const char * const conf_file_dirs[] = {
        "/etc/udev/hwdb.d",
        UDEVLIBEXECDIR "/hwdb.d",
        NULL
};

/* in-memory trie objects */
struct trie {
        struct trie_node *root;
        struct strbuf *strings;

        size_t nodes_count;
        size_t children_count;
        size_t values_count;
};

struct trie_node {
        /* prefix, common part for all children of this node */
        size_t prefix_off;

        /* sorted array of pointers to children nodes */
        struct trie_child_entry *children;
        uint8_t children_count;

        /* sorted array of key/value pairs */
        struct trie_value_entry *values;
        size_t values_count;
};

/* children array item with char (0-255) index */
struct trie_child_entry {
        uint8_t c;
        struct trie_node *child;
};

/* value array item with key/value pairs */
struct trie_value_entry {
        size_t key_off;
        size_t value_off;
};

static int trie_children_cmp(const void *v1, const void *v2) {
        const struct trie_child_entry *n1 = v1;
        const struct trie_child_entry *n2 = v2;

        return n1->c - n2->c;
}

static int node_add_child(struct trie *trie, struct trie_node *node, struct trie_node *node_child, uint8_t c) {
        struct trie_child_entry *child;

        /* extend array, add new entry, sort for bisection */
        child = reallocarray(node->children, node->children_count + 1, sizeof(struct trie_child_entry));
        if (!child)
                return -ENOMEM;

        node->children = child;
        trie->children_count++;
        node->children[node->children_count].c = c;
        node->children[node->children_count].child = node_child;
        node->children_count++;
        qsort(node->children, node->children_count, sizeof(struct trie_child_entry), trie_children_cmp);
        trie->nodes_count++;

        return 0;
}

static struct trie_node *node_lookup(const struct trie_node *node, uint8_t c) {
        struct trie_child_entry *child;
        struct trie_child_entry search;

        search.c = c;
        child = bsearch_safe(&search,
                             node->children, node->children_count, sizeof(struct trie_child_entry),
                             trie_children_cmp);
        if (child)
                return child->child;
        return NULL;
}

static void trie_node_cleanup(struct trie_node *node) {
        size_t i;

        if (!node)
                return;

        for (i = 0; i < node->children_count; i++)
                trie_node_cleanup(node->children[i].child);
        free(node->children);
        free(node->values);
        free(node);
}

static void trie_free(struct trie *trie) {
        if (!trie)
                return;

        trie_node_cleanup(trie->root);
        strbuf_cleanup(trie->strings);
        free(trie);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(struct trie*, trie_free);

static int trie_values_cmp(const void *v1, const void *v2, void *arg) {
        const struct trie_value_entry *val1 = v1;
        const struct trie_value_entry *val2 = v2;
        struct trie *trie = arg;

        return strcmp(trie->strings->buf + val1->key_off,
                      trie->strings->buf + val2->key_off);
}

static int trie_node_add_value(struct trie *trie, struct trie_node *node,
                          const char *key, const char *value) {
        ssize_t k, v;
        struct trie_value_entry *val;

        k = strbuf_add_string(trie->strings, key, strlen(key));
        if (k < 0)
                return k;
        v = strbuf_add_string(trie->strings, value, strlen(value));
        if (v < 0)
                return v;

        if (node->values_count) {
                struct trie_value_entry search = {
                        .key_off = k,
                        .value_off = v,
                };

                val = xbsearch_r(&search, node->values, node->values_count, sizeof(struct trie_value_entry), trie_values_cmp, trie);
                if (val) {
                        /* replace existing earlier key with new value */
                        val->value_off = v;
                        return 0;
                }
        }

        /* extend array, add new entry, sort for bisection */
        val = reallocarray(node->values, node->values_count + 1, sizeof(struct trie_value_entry));
        if (!val)
                return -ENOMEM;
        trie->values_count++;
        node->values = val;
        node->values[node->values_count].key_off = k;
        node->values[node->values_count].value_off = v;
        node->values_count++;
        qsort_r(node->values, node->values_count, sizeof(struct trie_value_entry), trie_values_cmp, trie);
        return 0;
}

static int trie_insert(struct trie *trie, struct trie_node *node, const char *search,
                       const char *key, const char *value) {
        size_t i = 0;
        int err = 0;

        for (;;) {
                size_t p;
                uint8_t c;
                struct trie_node *child;

                for (p = 0; (c = trie->strings->buf[node->prefix_off + p]); p++) {
                        _cleanup_free_ char *s = NULL;
                        ssize_t off;
                        _cleanup_free_ struct trie_node *new_child = NULL;

                        if (c == search[i + p])
                                continue;

                        /* split node */
                        new_child = new0(struct trie_node, 1);
                        if (!new_child)
                                return -ENOMEM;

                        /* move values from parent to child */
                        new_child->prefix_off = node->prefix_off + p+1;
                        new_child->children = node->children;
                        new_child->children_count = node->children_count;
                        new_child->values = node->values;
                        new_child->values_count = node->values_count;

                        /* update parent; use strdup() because the source gets realloc()d */
                        s = strndup(trie->strings->buf + node->prefix_off, p);
                        if (!s)
                                return -ENOMEM;

                        off = strbuf_add_string(trie->strings, s, p);
                        if (off < 0)
                                return off;

                        node->prefix_off = off;
                        node->children = NULL;
                        node->children_count = 0;
                        node->values = NULL;
                        node->values_count = 0;
                        err = node_add_child(trie, node, new_child, c);
                        if (err)
                                return err;

                        new_child = NULL; /* avoid cleanup */
                        break;
                }
                i += p;

                c = search[i];
                if (c == '\0')
                        return trie_node_add_value(trie, node, key, value);

                child = node_lookup(node, c);
                if (!child) {
                        ssize_t off;

                        /* new child */
                        child = new0(struct trie_node, 1);
                        if (!child)
                                return -ENOMEM;

                        off = strbuf_add_string(trie->strings, search + i+1, strlen(search + i+1));
                        if (off < 0) {
                                free(child);
                                return off;
                        }

                        child->prefix_off = off;
                        err = node_add_child(trie, node, child, c);
                        if (err) {
                                free(child);
                                return err;
                        }

                        return trie_node_add_value(trie, child, key, value);
                }

                node = child;
                i++;
        }
}

struct trie_f {
        FILE *f;
        struct trie *trie;
        uint64_t strings_off;

        uint64_t nodes_count;
        uint64_t children_count;
        uint64_t values_count;
};

/* calculate the storage space for the nodes, children arrays, value arrays */
static void trie_store_nodes_size(struct trie_f *trie, struct trie_node *node) {
        uint64_t i;

        for (i = 0; i < node->children_count; i++)
                trie_store_nodes_size(trie, node->children[i].child);

        trie->strings_off += sizeof(struct trie_node_f);
        for (i = 0; i < node->children_count; i++)
                trie->strings_off += sizeof(struct trie_child_entry_f);
        for (i = 0; i < node->values_count; i++)
                trie->strings_off += sizeof(struct trie_value_entry_f);
}

static int64_t trie_store_nodes(struct trie_f *trie, struct trie_node *node) {
        uint64_t i;
        struct trie_node_f n = {
                .prefix_off = htole64(trie->strings_off + node->prefix_off),
                .children_count = node->children_count,
                .values_count = htole64(node->values_count),
        };
        struct trie_child_entry_f *children = NULL;
        int64_t node_off;

        if (node->children_count) {
                children = new0(struct trie_child_entry_f, node->children_count);
                if (!children)
                        return -ENOMEM;
        }

        /* post-order recursion */
        for (i = 0; i < node->children_count; i++) {
                int64_t child_off;

                child_off = trie_store_nodes(trie, node->children[i].child);
                if (child_off < 0) {
                        free(children);
                        return child_off;
                }
                children[i].c = node->children[i].c;
                children[i].child_off = htole64(child_off);
        }

        /* write node */
        node_off = ftello(trie->f);
        fwrite(&n, sizeof(struct trie_node_f), 1, trie->f);
        trie->nodes_count++;

        /* append children array */
        if (node->children_count) {
                fwrite(children, sizeof(struct trie_child_entry_f), node->children_count, trie->f);
                trie->children_count += node->children_count;
                free(children);
        }

        /* append values array */
        for (i = 0; i < node->values_count; i++) {
                struct trie_value_entry_f v = {
                        .key_off = htole64(trie->strings_off + node->values[i].key_off),
                        .value_off = htole64(trie->strings_off + node->values[i].value_off),
                };

                fwrite(&v, sizeof(struct trie_value_entry_f), 1, trie->f);
                trie->values_count++;
        }

        return node_off;
}

static int trie_store(struct trie *trie, const char *filename) {
        struct trie_f t = {
                .trie = trie,
        };
        _cleanup_free_ char *filename_tmp = NULL;
        int64_t pos;
        int64_t root_off;
        int64_t size;
        struct trie_header_f h = {
                .signature = HWDB_SIG,
                .tool_version = htole64(atoi(PACKAGE_VERSION)),
                .header_size = htole64(sizeof(struct trie_header_f)),
                .node_size = htole64(sizeof(struct trie_node_f)),
                .child_entry_size = htole64(sizeof(struct trie_child_entry_f)),
                .value_entry_size = htole64(sizeof(struct trie_value_entry_f)),
        };
        int err;

        /* calculate size of header, nodes, children entries, value entries */
        t.strings_off = sizeof(struct trie_header_f);
        trie_store_nodes_size(&t, trie->root);

        err = fopen_temporary(filename, &t.f, &filename_tmp);
        if (err < 0)
                return err;
        fchmod(fileno(t.f), 0444);

        /* write nodes */
        if (fseeko(t.f, sizeof(struct trie_header_f), SEEK_SET) < 0)
                goto error_fclose;
        root_off = trie_store_nodes(&t, trie->root);
        h.nodes_root_off = htole64(root_off);
        pos = ftello(t.f);
        h.nodes_len = htole64(pos - sizeof(struct trie_header_f));

        /* write string buffer */
        fwrite(trie->strings->buf, trie->strings->len, 1, t.f);
        h.strings_len = htole64(trie->strings->len);

        /* write header */
        size = ftello(t.f);
        h.file_size = htole64(size);
        if (fseeko(t.f, 0, SEEK_SET < 0))
                goto error_fclose;
        fwrite(&h, sizeof(struct trie_header_f), 1, t.f);

        if (ferror(t.f))
                goto error_fclose;
        if (fflush(t.f) < 0)
                goto error_fclose;
        if (fsync(fileno(t.f)) < 0)
                goto error_fclose;
        if (rename(filename_tmp, filename) < 0)
                goto error_fclose;

        /* write succeeded */
        fclose(t.f);

        log_debug("=== trie on-disk ===");
        log_debug("size:             %8"PRIi64" bytes", size);
        log_debug("header:           %8zu bytes", sizeof(struct trie_header_f));
        log_debug("nodes:            %8"PRIu64" bytes (%8"PRIu64")",
                  t.nodes_count * sizeof(struct trie_node_f), t.nodes_count);
        log_debug("child pointers:   %8"PRIu64" bytes (%8"PRIu64")",
                  t.children_count * sizeof(struct trie_child_entry_f), t.children_count);
        log_debug("value pointers:   %8"PRIu64" bytes (%8"PRIu64")",
                  t.values_count * sizeof(struct trie_value_entry_f), t.values_count);
        log_debug("string store:     %8zu bytes", trie->strings->len);
        log_debug("strings start:    %8"PRIu64, t.strings_off);

        return 0;

 error_fclose:
        err = -errno;
        fclose(t.f);
        unlink(filename_tmp);
        return err;
}

static int insert_data(struct trie *trie, struct udev_list *match_list,
                       char *line, const char *filename) {
        char *value;
        struct udev_list_entry *entry;

        value = strchr(line, '=');
        if (!value) {
                log_error("Error, key/value pair expected but got '%s' in '%s':", line, filename);
                return -EINVAL;
        }

        value[0] = '\0';
        value++;

        /* libudev requires properties to start with a space */
        while (isblank(line[0]) && isblank(line[1]))
                line++;

        if (line[0] == '\0' || value[0] == '\0') {
                log_error("Error, empty key or value '%s' in '%s':", line, filename);
                return -EINVAL;
        }

        udev_list_entry_foreach(entry, udev_list_get_entry(match_list))
                trie_insert(trie, trie->root, udev_list_entry_get_name(entry), line, value);

        return 0;
}

static int import_file(struct trie *trie, const char *filename) {
        enum {
                HW_MATCH,
                HW_DATA,
                HW_NONE,
        } state = HW_NONE;
        FILE *f;
        char line[LINE_MAX];
        struct udev_list match_list;
        int r = 0, err;

        udev_list_init(NULL, &match_list, false);

        f = fopen(filename, "re");
        if (f == NULL)
                return -errno;

        while (fgets(line, sizeof(line), f)) {
                size_t len;
                char *pos;

                /* comment line */
                if (line[0] == '#')
                        continue;

                /* strip trailing comment */
                pos = strchr(line, '#');
                if (pos)
                        pos[0] = '\0';

                /* strip trailing whitespace */
                len = strlen(line);
                while (len > 0 && isspace(line[len-1]))
                        len--;
                line[len] = '\0';

                switch (state) {
                case HW_NONE:
                        if (len == 0)
                                break;

                        if (line[0] == ' ') {
                                log_error("Error, MATCH expected but got '%s' in '%s':", line, filename);
                                r = -EINVAL;
                                break;
                        }

                        /* start of record, first match */
                        state = HW_MATCH;
                        udev_list_entry_add(&match_list, line, NULL);
                        break;

                case HW_MATCH:
                        if (len == 0) {
                                log_error("Error, DATA expected but got empty line in '%s':", filename);
                                r = -EINVAL;
                                state = HW_NONE;
                                udev_list_cleanup(&match_list);
                                break;
                        }

                        /* another match */
                        if (line[0] != ' ') {
                                udev_list_entry_add(&match_list, line, NULL);
                                break;
                        }

                        /* first data */
                        state = HW_DATA;
                        err = insert_data(trie, &match_list, line, filename);
                        if (err < 0)
                                r = err;
                        break;

                case HW_DATA:
                        /* end of record */
                        if (len == 0) {
                                state = HW_NONE;
                                udev_list_cleanup(&match_list);
                                break;
                        }

                        if (line[0] != ' ') {
                                log_error("Error, DATA expected but got '%s' in '%s':", line, filename);
                                r = -EINVAL;
                                state = HW_NONE;
                                udev_list_cleanup(&match_list);
                                break;
                        }

                        err = insert_data(trie, &match_list, line, filename);
                        if (err < 0)
                                r = err;
                        break;
                };
        }

        fclose(f);
        udev_list_cleanup(&match_list);
        return r;
}

static int hwdb_update(void) {
        _cleanup_(trie_freep) struct trie *trie = NULL;
        _cleanup_strv_free_ char **files = NULL;
        _cleanup_free_ char *hwdb_bin = NULL;
        char **f;
        int r;

        trie = new0(struct trie, 1);
        if (!trie)
                return -ENOMEM;

        /* string store */
        trie->strings = strbuf_new();
        if (!trie->strings)
                return -ENOMEM;

        /* index */
        trie->root = new0(struct trie_node, 1);
        if (!trie->root)
                return -ENOMEM;

        trie->nodes_count++;

        r = conf_files_list_strv(&files, ".hwdb", arg_root, 0, conf_file_dirs);
        if (r < 0)
                return log_error_errno(r, "failed to enumerate hwdb files: %m");

        STRV_FOREACH(f, files) {
                log_debug("Reading file '%s'", *f);
                r = import_file(trie, *f);
                if (r < 0 && arg_strict)
                        return r;
        }

        strbuf_complete(trie->strings);

        log_debug("=== trie in-memory ===");
        log_debug("nodes:            %8zu bytes (%8zu)",
                  trie->nodes_count * sizeof(struct trie_node), trie->nodes_count);
        log_debug("children arrays:  %8zu bytes (%8zu)",
                  trie->children_count * sizeof(struct trie_child_entry), trie->children_count);
        log_debug("values arrays:    %8zu bytes (%8zu)",
                  trie->values_count * sizeof(struct trie_value_entry), trie->values_count);
        log_debug("strings:          %8zu bytes",
                  trie->strings->len);
        log_debug("strings incoming: %8zu bytes (%8zu)",
                  trie->strings->in_len, trie->strings->in_count);
        log_debug("strings dedup'ed: %8zu bytes (%8zu)",
                  trie->strings->dedup_len, trie->strings->dedup_count);

        hwdb_bin = path_join(arg_root, arg_hwdb_bin_dir, "/hwdb.bin");
        if (!hwdb_bin)
                return -ENOMEM;

        mkdir_parents_label(hwdb_bin, 0755);

        r = trie_store(trie, hwdb_bin);
        if (r < 0)
                log_error_errno(r, "Failed to write database %s: %m", hwdb_bin);

        (void) label_fix(hwdb_bin, 0);

        return r;
}

static int hwdb_test(void) {
        _cleanup_(sd_hwdb_unrefp) sd_hwdb *hwdb = NULL;
        const char *key, *value;
        int r;

        r = sd_hwdb_new(&hwdb);
        if (r < 0)
                return r;

        SD_HWDB_FOREACH_PROPERTY(hwdb, arg_test, key, value)
                printf("%s=%s\n", key, value);

        return 0;
}

static int help(void) {
        printf("%s hwdb [OPTIONS]\n\n"
               "  -h --help            Print this message\n"
               "  -V --version         Print version of the program\n"
               "  -u --update          Update the hardware database\n"
               "  -s --strict          When updating, return non-zero exit value on any parsing error\n"
               "     --usr             Generate in " UDEVLIBEXECDIR " instead of /etc/udev\n"
               "  -t --test=MODALIAS   Query database and print result\n"
               "  -r --root=PATH       Alternative root path in the filesystem\n\n"
               "NOTE:\n"
               "The sub-command 'hwdb' is deprecated, and is left for backwards compatibility.\n"
               "Please use systemd-hwdb instead.\n"
               , program_invocation_short_name);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_USR = 0x100,
        };

        static const struct option options[] = {
                { "update",  no_argument,       NULL, 'u'     },
                { "usr",     no_argument,       NULL, ARG_USR },
                { "strict",  no_argument,       NULL, 's'     },
                { "test",    required_argument, NULL, 't'     },
                { "root",    required_argument, NULL, 'r'     },
                { "version", no_argument,       NULL, 'V'     },
                { "help",    no_argument,       NULL, 'h'     },
                {}
        };

        int c;

        while ((c = getopt_long(argc, argv, "ust:r:Vh", options, NULL)) >= 0)
                switch(c) {
                case 'u':
                        arg_update = true;
                        break;
                case ARG_USR:
                        arg_hwdb_bin_dir = UDEVLIBEXECDIR;
                        break;
                case 's':
                        arg_strict = true;
                        break;
                case 't':
                        arg_test = optarg;
                        break;
                case 'r':
                        arg_root = optarg;
                        break;
                case 'V':
                        return version();
                case 'h':
                        return help();
                case '?':
                        return -EINVAL;
                default:
                        assert_not_reached("Unknown option");
                }

        return 1;
}

int hwdb_main(int argc, char *argv[], void *userdata) {
        int r;

        r = parse_argv(argc, argv);
        if (r < 0)
                return r;

        if (!arg_update && !arg_test) {
                log_error("Either --update or --test must be used.");
                return -EINVAL;
        }

        if (arg_update) {
                r = hwdb_update();
                if (r < 0)
                        return r;
        }

        if (arg_test)
                return hwdb_test();

        return 0;
}
