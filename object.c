// object.c — Content-addressable object store

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTATION ─────────────────────────────────────────────────────────

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    if ((!data && len > 0) || !id_out) return -1;

    const char *type_str;
    switch (type) {
        case OBJ_BLOB: type_str = "blob"; break;
        case OBJ_TREE: type_str = "tree"; break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) return -1;
    header_len++;

    size_t total_len = header_len + len;
    if (total_len < len) return -1;

    unsigned char *buffer = malloc(total_len ? total_len : 1);
    if (!buffer) return -1;

    memcpy(buffer, header, header_len);
    if (len > 0) memcpy(buffer + header_len, data, len);

    compute_hash(buffer, total_len, id_out);

    if (object_exists(id_out)) {
        free(buffer);
        return 0;
    }

    char path[512];
    object_path(id_out, path, sizeof(path));

    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/%.2s", OBJECTS_DIR, hex);

    if (mkdir(PES_DIR, 0755) != 0 && errno != EEXIST) {
        free(buffer);
        return -1;
    }
    if (mkdir(OBJECTS_DIR, 0755) != 0 && errno != EEXIST) {
        free(buffer);
        return -1;
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        free(buffer);
        return -1;
    }

    char temp_path[sizeof(path) + 5];
    int temp_len = snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    if (temp_len < 0 || (size_t)temp_len >= sizeof(temp_path)) {
        free(buffer);
        return -1;
    }

    int fd = open(temp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(buffer);
        return -1;
    }

    size_t offset = 0;
    while (offset < total_len) {
        ssize_t written = write(fd, buffer + offset, total_len - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(temp_path);
            free(buffer);
            return -1;
        }
        if (written == 0) {
            close(fd);
            unlink(temp_path);
            free(buffer);
            return -1;
        }
        offset += (size_t)written;
    }

    if (fsync(fd) != 0) {
        close(fd);
        unlink(temp_path);
        free(buffer);
        return -1;
    }

    if (close(fd) != 0) {
        unlink(temp_path);
        free(buffer);
        return -1;
    }

    if (rename(temp_path, path) != 0) {
        unlink(temp_path);
        free(buffer);
        return -1;
    }

    int dir_fd = open(dir, O_DIRECTORY);
    if (dir_fd >= 0) {
        if (fsync(dir_fd) != 0) {
            close(dir_fd);
            free(buffer);
            return -1;
        }
        close(dir_fd);
    }

    free(buffer);
    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    if (!id || !type_out || !data_out || !len_out) return -1;

    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }

    long file_size = ftell(f);
    if (file_size < 0) {
        fclose(f);
        return -1;
    }

    size_t size = (size_t)file_size;
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    unsigned char *buffer = malloc(size ? size : 1);
    if (!buffer) {
        fclose(f);
        return -1;
    }

    if (fread(buffer, 1, size, f) != size) {
        free(buffer);
        fclose(f);
        return -1;
    }
    fclose(f);

    ObjectID check;
    compute_hash(buffer, size, &check);
    if (memcmp(check.hash, id->hash, HASH_SIZE) != 0) {
        free(buffer);
        return -1;
    }

    char *null_pos = memchr(buffer, '\0', size);
    if (!null_pos) {
        free(buffer);
        return -1;
    }

    size_t header_len = (size_t)(null_pos - (char *)buffer);
    if (header_len >= 128) {
        free(buffer);
        return -1;
    }

    char header[128];
    memcpy(header, buffer, header_len);
    header[header_len] = '\0';

    char type_str[16];
    size_t data_len;
    if (sscanf(header, "%15s %zu", type_str, &data_len) != 2) {
        free(buffer);
        return -1;
    }

    if (strcmp(type_str, "blob") == 0) *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree") == 0) *type_out = OBJ_TREE;
    else if (strcmp(type_str, "commit") == 0) *type_out = OBJ_COMMIT;
    else {
        free(buffer);
        return -1;
    }

    size_t payload_offset = header_len + 1;
    if (payload_offset > size || data_len != size - payload_offset) {
        free(buffer);
        return -1;
    }

    *len_out = data_len;
    *data_out = malloc(data_len ? data_len : 1);
    if (!*data_out) {
        free(buffer);
        return -1;
    }

    if (data_len > 0) memcpy(*data_out, null_pos + 1, data_len);

    free(buffer);
    return 0;
}
