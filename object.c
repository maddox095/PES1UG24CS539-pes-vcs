// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
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

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
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

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
//   where <type> is "blob", "tree", or "commit"
//   and <size> is the decimal string of the data length
//
// Steps:
//   1. Build the full object: header ("blob 16\0") + data
//   2. Compute SHA-256 hash of the FULL object (header + data)
//   3. Check if object already exists (deduplication) — if so, just return success
//   4. Create shard directory (.pes/objects/XX/) if it doesn't exist
//   5. Write to a temporary file in the same shard directory
//   6. fsync() the temporary file to ensure data reaches disk
//   7. rename() the temp file to the final path (atomic on POSIX)
//   8. Open and fsync() the shard directory to persist the rename
//   9. Store the computed hash in *id_out

// HINTS - Useful syscalls and functions for this phase:
//   - sprintf / snprintf : formatting the header string
//   - compute_hash       : hashing the combined header + data
//   - object_exists      : checking for deduplication
//   - mkdir              : creating the shard directory (use mode 0755)
//   - open, write, close : creating and writing to the temp file
//                          (Use O_CREAT | O_WRONLY | O_TRUNC, mode 0644)
//   - fsync              : flushing the file descriptor to disk
//   - rename             : atomically moving the temp file to the final path
//

//
// Returns 0 on success, -1 on error.
char *object_write(ObjectType type, const uint8_t *data, size_t size) {
    // 1. Build the header string: "blob <size>\0" or "tree <size>\0" or "commit <size>\0"
    const char *type_str = (type == OBJ_BLOB) ? "blob" :
                           (type == OBJ_TREE) ? "tree" : "commit";
    char header[64];
    int hlen = snprintf(header, sizeof(header), "%s %zu", type_str, size) + 1;
    // +1 to include the null byte in hlen

    // 2. Combine header + data into one buffer
    size_t total = hlen + size;
    uint8_t *full = malloc(total);
    memcpy(full, header, hlen);
    memcpy(full + hlen, data, size);

    // 3. SHA-256 hash of the full buffer
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(full, total, hash);

    // 4. Convert hash to hex string (64 chars)
    char hex[65];
    for (int i = 0; i < 32; i++)
        sprintf(hex + i*2, "%02x", hash[i]);
    hex[64] = '\0';

    // 5. Build path: .pes/objects/XX/YYYY...
    char dir[256], path[256];
    snprintf(dir, sizeof(dir), ".pes/objects/%.2s", hex);
    snprintf(path, sizeof(path), "%s/%s", dir, hex + 2);

    // 6. Skip if already exists (deduplication)
    if (access(path, F_OK) == 0) {
        free(full);
        return strdup(hex);
    }

    // 7. Create shard directory if needed
    mkdir(dir, 0755);

    // 8. Write to a temp file, then rename (atomic write)
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    fwrite(full, 1, total, f);
    fclose(f);
    rename(tmp, path);

    free(full);
    return strdup(hex);  // caller must free
}

// Read an object from the store.
//
// Steps:
//   1. Build the file path from the hash using object_path()
//   2. Open and read the entire file
//   3. Parse the header to extract the type string and size
//   4. Verify integrity: recompute the SHA-256 of the file contents
//      and compare to the expected hash (from *id). Return -1 if mismatch.
//   5. Set *type_out to the parsed ObjectType
//   6. Allocate a buffer, copy the data portion (after the \0), set *data_out and *len_out
//
// HINTS - Useful syscalls and functions for this phase:
//   - object_path        : getting the target file path
//   - fopen, fread, fseek: reading the file into memory
//   - memchr             : safely finding the '\0' separating header and data
//   - strncmp            : parsing the type string ("blob", "tree", "commit")
//   - compute_hash       : re-hashing the read data for integrity verification
//   - memcmp             : comparing the computed hash against the requested hash
//   - malloc, memcpy     : allocating and returning the extracted data
//
// The caller is responsible for calling free(*data_out).
// Returns 0 on success, -1 on error (file not found, corrupt, etc.).
uint8_t *object_read(const char *hex, ObjectType *type_out, size_t *size_out) {
    // 1. Build path from hex
    char path[256];
    snprintf(path, sizeof(path), ".pes/objects/%.2s/%s", hex, hex + 2);

    // 2. Open and read file
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size_t fsize = ftell(f);
    rewind(f);
    uint8_t *full = malloc(fsize);
    fread(full, 1, fsize, f);
    fclose(f);

    // 3. Verify integrity: recompute SHA-256 and compare to filename
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(full, fsize, hash);
    char computed[65];
    for (int i = 0; i < 32; i++)
        sprintf(computed + i*2, "%02x", hash[i]);
    computed[64] = '\0';
    if (strcmp(computed, hex) != 0) {
        free(full);
        return NULL;  // corrupted
    }

    // 4. Parse header: "blob 16\0..."
    char *nul = memchr(full, '\0', fsize);
    if (!nul) { free(full); return NULL; }

    // Determine type
    if (strncmp((char*)full, "blob", 4) == 0)       *type_out = OBJ_BLOB;
    else if (strncmp((char*)full, "tree", 4) == 0)  *type_out = OBJ_TREE;
    else                                             *type_out = OBJ_COMMIT;

    // 5. Extract size from header
    *size_out = atoi(strchr((char*)full, ' ') + 1);

    // 6. Return data portion (after the \0)
    size_t header_len = (nul - (char*)full) + 1;
    uint8_t *data = malloc(*size_out);
    memcpy(data, full + header_len, *size_out);
    free(full);
    return data;
}
