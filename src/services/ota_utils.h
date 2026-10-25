#ifndef SERVICES_OTA_UTILS_H
#define SERVICES_OTA_UTILS_H

#include <stddef.h>
#include <stdint.h>

namespace ota_utils {

constexpr size_t OTA_MAX_VERSION_LENGTH = 23;
constexpr size_t OTA_MD5_LENGTH = 32;

// What the server publishes at /firmware/manifest.
struct ota_manifest_t {
  char version[OTA_MAX_VERSION_LENGTH + 1];
  char md5[OTA_MD5_LENGTH + 1];  // lowercase hex, empty when absent
  uint32_t size;
};

// Compares dotted numeric versions ("1.10.0" > "1.9.2"); missing parts count as 0.
// Returns <0, 0 or >0 like strcmp.
int compare_versions(const char* left, const char* right);

// Parses {"version": "...", "size": n, "md5": "..."}. The version is required; an md5 that
// is not 32 hex digits is ignored.
bool parse_manifest(const char* json, ota_manifest_t* out);

// Host of a server URL, port included: "https://docs.example.fr:8443/api" gives
// "docs.example.fr:8443". Shows where updates are looked for.
void url_host(const char* url, char* out, size_t out_size);

// `base_url` as saved in Reglages, then `path`. Surrounding spaces and trailing '/' are
// dropped as the notes client does: "https://host/" + "/firmware/manifest" must not ask
// "//firmware/manifest", which the server does not know. False when empty or too long.
bool server_url(const char* base_url, const char* path, char* out, size_t out_size);

// A 404 on /firmware/manifest only means "nothing published yet" when Bibliotek says so; any
// other 404 comes from a server without the update routes.
bool nothing_published(const char* body);

// Status line when the server has nothing newer than `running`: "Deja a jour (1.2.1)", plus
// the server version when it holds an older build, or "rien de publie" when it holds none.
void up_to_date_status(const char* running, const char* server_version, char* out, size_t out_size);

}  // namespace ota_utils

#endif
