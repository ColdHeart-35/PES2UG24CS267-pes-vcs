// tree.c — Tree object serialization and construction

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define MODE_DIR 0040000

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

static int compare_tree_entries(const void *a, const void *b) {
    const TreeEntry *ea = a;
    const TreeEntry *eb = b;
    return strcmp(ea->name, eb->name);
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    if (!data || !tree_out) return -1;

    tree_out->count = 0;
    const unsigned char *ptr = data;
    const unsigned char *end = ptr + len;

    while (ptr < end) {
        if (tree_out->count >= MAX_TREE_ENTRIES) return -1;

        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const unsigned char *space = memchr(ptr, ' ', (size_t)(end - ptr));
        if (!space) return -1;

        size_t mode_len = (size_t)(space - ptr);
        if (mode_len == 0 || mode_len >= 16) return -1;

        char mode[16];
        memcpy(mode, ptr, mode_len);
        mode[mode_len] = '\0';
        entry->mode = (uint32_t)strtoul(mode, NULL, 8);

        ptr = space + 1;

        const unsigned char *nul = memchr(ptr, '\0', (size_t)(end - ptr));
        if (!nul) return -1;

        size_t name_len = (size_t)(nul - ptr);
        if (name_len == 0 || name_len >= sizeof(entry->name)) return -1;

        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = nul + 1;
        if ((size_t)(end - ptr) < HASH_SIZE) return -1;

        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }

    return 0;
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    if (!tree || !data_out || !len_out || tree->count < 0 || tree->count > MAX_TREE_ENTRIES) {
        return -1;
    }

    Tree sorted = *tree;
    qsort(sorted.entries, (size_t)sorted.count, sizeof(TreeEntry), compare_tree_entries);

    size_t total = 0;
    for (int i = 0; i < sorted.count; i++) {
        int mode_len = snprintf(NULL, 0, "%06o", sorted.entries[i].mode);
        if (mode_len < 0 || sorted.entries[i].name[0] == '\0') return -1;
        total += (size_t)mode_len + 1 + strlen(sorted.entries[i].name) + 1 + HASH_SIZE;
    }

    unsigned char *buffer = malloc(total ? total : 1);
    if (!buffer) return -1;

    size_t offset = 0;
    for (int i = 0; i < sorted.count; i++) {
        TreeEntry *entry = &sorted.entries[i];
        int written = snprintf((char *)buffer + offset, total - offset, "%06o %s",
                               entry->mode, entry->name);
        if (written < 0 || (size_t)written >= total - offset) {
            free(buffer);
            return -1;
        }

        offset += (size_t)written + 1;
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

static int prefix_matches(const char *path, const char *prefix) {
    size_t prefix_len = strlen(prefix);
    return strncmp(path, prefix, prefix_len) == 0;
}

static int component_seen(char seen[][256], int seen_count, const char *component) {
    for (int i = 0; i < seen_count; i++) {
        if (strcmp(seen[i], component) == 0) return 1;
    }
    return 0;
}

static int add_tree_entry(Tree *tree, uint32_t mode, const ObjectID *hash, const char *name) {
    if (tree->count >= MAX_TREE_ENTRIES || strlen(name) >= sizeof(tree->entries[0].name)) {
        return -1;
    }

    TreeEntry *entry = &tree->entries[tree->count++];
    entry->mode = mode;
    entry->hash = *hash;
    strcpy(entry->name, name);
    return 0;
}

static int build_tree(Index *index, const char *prefix, ObjectID *tree_id) {
    Tree tree;
    tree.count = 0;

    char seen_dirs[MAX_TREE_ENTRIES][256];
    int seen_count = 0;
    size_t prefix_len = strlen(prefix);

    for (int i = 0; i < index->count; i++) {
        IndexEntry *entry = &index->entries[i];
        if (!prefix_matches(entry->path, prefix)) continue;

        const char *rest = entry->path + prefix_len;
        if (rest[0] == '\0') continue;

        const char *slash = strchr(rest, '/');
        if (!slash) {
            if (add_tree_entry(&tree, entry->mode, &entry->hash, rest) != 0) return -1;
            continue;
        }

        size_t component_len = (size_t)(slash - rest);
        if (component_len == 0 || component_len >= sizeof(seen_dirs[0])) return -1;

        char component[256];
        memcpy(component, rest, component_len);
        component[component_len] = '\0';

        if (component_seen(seen_dirs, seen_count, component)) continue;
        if (seen_count >= MAX_TREE_ENTRIES) return -1;
        strcpy(seen_dirs[seen_count++], component);

        char child_prefix[512];
        int child_len = snprintf(child_prefix, sizeof(child_prefix), "%s%s/", prefix, component);
        if (child_len < 0 || (size_t)child_len >= sizeof(child_prefix)) return -1;

        ObjectID child_id;
        if (build_tree(index, child_prefix, &child_id) != 0) return -1;
        if (add_tree_entry(&tree, MODE_DIR, &child_id, component) != 0) return -1;
    }

    void *data;
    size_t len;
    if (tree_serialize(&tree, &data, &len) != 0) return -1;

    int rc = object_write(OBJ_TREE, data, len, tree_id);
    free(data);
    return rc;
}

static int load_index_for_tree(Index *index) {
    if (!index) return -1;

    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return errno == ENOENT ? 0 : -1;

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        if (index->count >= MAX_INDEX_ENTRIES) {
            fclose(f);
            return -1;
        }

        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;

        IndexEntry *entry = &index->entries[index->count];
        char hash_hex[HASH_HEX_SIZE + 1];
        char path[sizeof(entry->path)];
        unsigned int mode;
        unsigned long long mtime_sec;
        unsigned int size;

        int fields = sscanf(line, "%o %64s %llu %u %511[^\n]",
                            &mode, hash_hex, &mtime_sec, &size, path);
        if (fields != 5 || hex_to_hash(hash_hex, &entry->hash) != 0) {
            fclose(f);
            return -1;
        }

        entry->mode = mode;
        entry->mtime_sec = mtime_sec;
        entry->size = size;
        snprintf(entry->path, sizeof(entry->path), "%s", path);
        index->count++;
    }

    int rc = ferror(f) ? -1 : 0;
    fclose(f);
    return rc;
}

int tree_from_index(ObjectID *id_out) {
    if (!id_out) return -1;

    Index index;
    if (load_index_for_tree(&index) != 0) return -1;

    return build_tree(&index, "", id_out);
}
