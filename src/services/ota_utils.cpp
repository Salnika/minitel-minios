#include "ota_utils.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace ota_utils {
namespace {

// Reads the next numeric part of a dotted version and moves `cursor` past its dot.
uint32_t next_version_part(const char** cursor) {
  uint32_t value = 0;
  const char* text = *cursor;
  while (isdigit(static_cast<unsigned char>(*text))) {
    value = value * 10 + static_cast<uint32_t>(*text - '0');
    ++text;
  }
  while (*text != '\0' && *text != '.') {
    ++text;  // ignore suffixes such as "-beta"
  }
  if (*text == '.') {
    ++text;
  }
  *cursor = text;
  return value;
}

bool is_md5_hex(const char* text) {
  if (!text || strlen(text) != OTA_MD5_LENGTH) {
    return false;
  }
  for (size_t i = 0; i < OTA_MD5_LENGTH; ++i) {
    if (!isxdigit(static_cast<unsigned char>(text[i]))) {
      return false;
    }
  }
  return true;
}

}  // namespace

int compare_versions(const char* left, const char* right) {
  const char* a = left ? left : "";
  const char* b = right ? right : "";
  while (*a != '\0' || *b != '\0') {
    const uint32_t part_a = next_version_part(&a);
    const uint32_t part_b = next_version_part(&b);
    if (part_a != part_b) {
      return part_a < part_b ? -1 : 1;
    }
  }
  return 0;
}

bool parse_manifest(const char* json, ota_manifest_t* out) {
  if (!json || !out) {
    return false;
  }
  memset(out, 0, sizeof(*out));

  StaticJsonDocument<384> doc;
  if (deserializeJson(doc, json)) {
    return false;
  }

  const char* version = doc["version"];
  if (!version || version[0] == '\0' || strlen(version) > OTA_MAX_VERSION_LENGTH) {
    return false;
  }
  strcpy(out->version, version);
  out->size = doc["size"].as<uint32_t>();

  const char* md5 = doc["md5"];
  if (is_md5_hex(md5)) {
    for (size_t i = 0; i < OTA_MD5_LENGTH; ++i) {
      out->md5[i] = static_cast<char>(tolower(static_cast<unsigned char>(md5[i])));
    }
    out->md5[OTA_MD5_LENGTH] = '\0';
  }
  return true;
}

void url_host(const char* url, char* out, size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  out[0] = '\0';
  if (!url) {
    return;
  }
  const char* scheme_end = strstr(url, "://");
  const char* host = scheme_end ? scheme_end + 3 : url;
  size_t length = 0;
  while (host[length] != '\0' && host[length] != '/' && length + 1 < out_size) {
    out[length] = host[length];
    ++length;
  }
  out[length] = '\0';
}

bool server_url(const char* base_url, const char* path, char* out, size_t out_size) {
  if (!out || out_size == 0) {
    return false;
  }
  out[0] = '\0';
  const char* start = base_url ? base_url : "";
  while (isspace(static_cast<unsigned char>(*start))) {
    ++start;
  }
  size_t length = strlen(start);
  while (length > 0 &&
         (isspace(static_cast<unsigned char>(start[length - 1])) || start[length - 1] == '/')) {
    --length;
  }
  const char* suffix = path ? path : "";
  const size_t suffix_length = strlen(suffix);
  if (length == 0 || length + suffix_length + 1 > out_size) {
    return false;
  }
  memcpy(out, start, length);
  memcpy(out + length, suffix, suffix_length + 1);
  return true;
}

bool nothing_published(const char* body) {
  return body && strstr(body, "No firmware published") != nullptr;  // server/src/firmware.ts
}

void up_to_date_status(const char* running, const char* server_version, char* out, size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  const char* version = running ? running : "";
  if (!server_version || server_version[0] == '\0') {
    snprintf(out, out_size, "Deja a jour (%s), rien de publie", version);
  } else if (compare_versions(server_version, version) < 0) {
    snprintf(out, out_size, "Deja a jour (%s), serveur %s", version, server_version);
  } else {
    snprintf(out, out_size, "Deja a jour (%s)", version);
  }
}

}  // namespace ota_utils
