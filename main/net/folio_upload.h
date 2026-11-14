#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FOLIO_UPLOAD_NAME_CAP 256
#define FOLIO_UPLOAD_ENCODED_NAME_CAP (3 * FOLIO_UPLOAD_NAME_CAP)
#define FOLIO_UPLOAD_FORM_CAP (FOLIO_UPLOAD_ENCODED_NAME_CAP + 64)
#define FOLIO_UPLOAD_PATH_CAP 256
typedef enum { FOLIO_UPLOAD_TXT, FOLIO_UPLOAD_EPUB, FOLIO_UPLOAD_TTF,
               FOLIO_UPLOAD_PNG, FOLIO_UPLOAD_JPEG,
               FOLIO_UPLOAD_FIRMWARE, FOLIO_UPLOAD_SHA256,
               FOLIO_UPLOAD_PDF, FOLIO_UPLOAD_MOBI, FOLIO_UPLOAD_AZW3,
               FOLIO_UPLOAD_BMP, FOLIO_UPLOAD_GIF, FOLIO_UPLOAD_WEBP,
               FOLIO_UPLOAD_HEIC, FOLIO_UPLOAD_HEIF } folio_upload_kind_t;
typedef enum { FOLIO_UPLOAD_OK, FOLIO_UPLOAD_BAD_NAME, FOLIO_UPLOAD_TOO_LARGE,
               FOLIO_UPLOAD_CONFLICT, FOLIO_UPLOAD_IO, FOLIO_UPLOAD_INVALID,
               FOLIO_UPLOAD_TRUNCATED } folio_upload_result_t;
typedef struct folio_upload folio_upload_t;
typedef bool (*folio_upload_validator_t)(const char*, folio_upload_kind_t, char*, size_t);

// UTF-8 basename only; rejects FAT aliases, separators, controls and hidden names.
bool folio_upload_name(const char* name, folio_upload_kind_t* kind, size_t* limit);
bool folio_upload_is_image(folio_upload_kind_t kind);
bool folio_upload_saved_only(folio_upload_kind_t kind);
const char* folio_upload_directory(folio_upload_kind_t kind);
// Legacy API category is an alias, never a second disk directory.
const char* folio_upload_canonical_directory(const char* directory);
bool folio_upload_in_directory(const char* name,const char* directory,folio_upload_kind_t* kind);
// Remove only reserved .folio-upload-<32 lowercase hex>.part regular files.
// Caller verifies this fixed category directory and holds the shared lock.
bool folio_upload_clean_staging(const char* directory);
// Caller holds the shared storage lock from begin through finish/abort. root is
// a trusted mounted SD root, never an HTTP parameter. nonce is 32 lowercase hex.
folio_upload_result_t folio_upload_begin(const char* root, const char* name, size_t total,
    const char* nonce, folio_upload_validator_t validate, folio_upload_t** upload);
// Optional category override; picture/wallpapers accepts images only. books also
// accepts PNG/JPEG sidecar covers. The full destination must fit PATH_CAP.
folio_upload_result_t folio_upload_begin_in_directory(const char* root,const char* name,const char* directory,
    size_t total,const char* nonce,folio_upload_validator_t validate,folio_upload_t** upload);
folio_upload_result_t folio_upload_write(folio_upload_t* upload, const void* bytes, size_t count);
// Both finish and abort consume *upload and clean temporary files on failure.
folio_upload_result_t folio_upload_finish(folio_upload_t** upload, char* error, size_t cap);
void folio_upload_abort(folio_upload_t** upload);
const char* folio_upload_message(folio_upload_result_t result);

// Bounded transfer validation is separate from reading/rendering capability.
bool folio_transfer_validate(const char* path, folio_upload_kind_t kind, char* error, size_t cap);
// Inspect a validated, committed file while holding the storage lock.
bool folio_transfer_saved_only(const char* path, folio_upload_kind_t kind);
