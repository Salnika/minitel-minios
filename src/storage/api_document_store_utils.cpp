#include "api_document_store_utils.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace api_document_store_utils {
namespace {

bool copy_string_field(const JsonVariantConst& object,
                       const char* const* keys,
                       size_t key_count,
                       char* out,
                       size_t out_size,
                       bool* overflowed = nullptr) {
  if (!out || out_size == 0 || !object.is<JsonObjectConst>()) {
    return false;
  }

  out[0] = '\0';
  for (size_t i = 0; i < key_count; ++i) {
    JsonVariantConst value = object[keys[i]];
    if (value.is<const char*>()) {
      const char* source = value.as<const char*>();
      safe_copy(out, out_size, source);
      if (overflowed && strlen(source) >= out_size) {
        *overflowed = true;
      }
      return out[0] != '\0';
    }
  }
  return false;
}

void copy_span(char* dest, size_t dest_size, const char* src, size_t len) {
  if (!dest || dest_size == 0) {
    return;
  }
  if (len >= dest_size) {
    len = dest_size - 1;
  }
  memcpy(dest, src, len);
  dest[len] = '\0';
}

uint16_t first_uint16_field(const JsonVariantConst& object,
                            const char* const* keys,
                            size_t key_count) {
  if (!object.is<JsonObjectConst>()) {
    return 0;
  }

  for (size_t i = 0; i < key_count; ++i) {
    JsonVariantConst value = object[keys[i]];
    if (value.is<uint16_t>()) {
      return value.as<uint16_t>();
    }
    if (value.is<int>()) {
      const int raw = value.as<int>();
      if (raw >= 0 && raw <= 65535) {
        return static_cast<uint16_t>(raw);
      }
    }
  }
  return 0;
}

JsonVariantConst unwrap_document(const JsonVariantConst& root) {
  if (!root.is<JsonObjectConst>()) {
    return root;
  }

  const char* const keys[] = {"item", "file", "document", "data"};
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    JsonVariantConst nested = root[keys[i]];
    if (nested.is<JsonObjectConst>()) {
      return nested;
    }
  }
  return root;
}

JsonVariantConst resolve_collection_root(const JsonVariantConst& root,
                                         const char* primary_key) {
  if (root.is<JsonArrayConst>()) {
    return root;
  }
  if (!root.is<JsonObjectConst>()) {
    return JsonVariantConst();
  }

  if (primary_key) {
    JsonVariantConst primary = root[primary_key];
    if (primary.is<JsonArrayConst>()) {
      return primary;
    }
  }

  const char* const keys[] = {"items", "files", "folders", "data"};
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    JsonVariantConst nested = root[keys[i]];
    if (nested.is<JsonArrayConst>()) {
      return nested;
    }
  }
  return JsonVariantConst();
}

bool parse_file_path(const char* raw_path,
                     char* folder_path,
                     size_t folder_path_size,
                     char* file_name,
                     size_t file_name_size) {
  if (!raw_path || !file_name || file_name_size == 0 || !folder_path || folder_path_size == 0) {
    return false;
  }

  char buffer[STORE_MAX_DOCUMENT_ID + STORE_MAX_FILE_NAME_LENGTH + 8];
  safe_copy(buffer, sizeof(buffer), raw_path);
  trim_in_place(buffer);
  if (buffer[0] == '\0') {
    return false;
  }

  size_t length = strlen(buffer);
  while (length > 0 && (buffer[length - 1] == '/' || buffer[length - 1] == '\\')) {
    buffer[--length] = '\0';
  }
  for (size_t i = 0; buffer[i] != '\0'; ++i) {
    if (buffer[i] == '\\') {
      buffer[i] = '/';
    }
  }

  char* slash = strrchr(buffer, '/');
  if (!slash) {
    safe_copy(folder_path, folder_path_size, "/");
    safe_copy(file_name, file_name_size, buffer);
    return file_name[0] != '\0';
  }

  safe_copy(file_name, file_name_size, slash + 1);
  if (slash == buffer) {
    safe_copy(folder_path, folder_path_size, "/");
  } else {
    *slash = '\0';
    normalize_local_folder_path(buffer, folder_path, folder_path_size);
  }
  return file_name[0] != '\0';
}

bool parse_folder_path_from_item(const JsonVariantConst& item, char* out, size_t out_size) {
  if (!out || out_size == 0) {
    return false;
  }

  out[0] = '\0';
  if (item.is<const char*>()) {
    normalize_local_folder_path(item.as<const char*>(), out, out_size);
    return !is_root(out);
  }

  if (!item.is<JsonObjectConst>()) {
    return false;
  }

  const char* const direct_keys[] = {"path", "fullPath", "folder", "folderPath", "folder_path"};
  char buffer[STORE_MAX_FOLDER_PATH + 1];
  if (copy_string_field(item, direct_keys, sizeof(direct_keys) / sizeof(direct_keys[0]), buffer, sizeof(buffer))) {
    normalize_local_folder_path(buffer, out, out_size);
    return !is_root(out);
  }

  const char* const parent_keys[] = {"parentPath", "parent_path", "parent"};
  const char* const name_keys[] = {"name", "folderName"};
  char parent[STORE_MAX_FOLDER_PATH + 1];
  char name[STORE_MAX_TITLE_LENGTH + 1];
  parent[0] = '\0';
  name[0] = '\0';
  copy_string_field(item, parent_keys, sizeof(parent_keys) / sizeof(parent_keys[0]), parent, sizeof(parent));
  if (!copy_string_field(item, name_keys, sizeof(name_keys) / sizeof(name_keys[0]), name, sizeof(name))) {
    return false;
  }

  trim_in_place(name);
  if (name[0] == '\0') {
    return false;
  }

  if (parent[0] == '\0') {
    normalize_local_folder_path(name, out, out_size);
  } else {
    char normalized_parent[STORE_MAX_FOLDER_PATH + 1];
    normalize_local_folder_path(parent, normalized_parent, sizeof(normalized_parent));
    join_local_path(normalized_parent, name, out, out_size);
  }
  return !is_root(out);
}

bool folder_exists(const folder_info_t* items, int count, const char* path) {
  if (!items || !path) {
    return false;
  }
  for (int i = 0; i < count; ++i) {
    if (strcmp(items[i].path, path) == 0) {
      return true;
    }
  }
  return false;
}

bool tag_exists(char out[][STORE_MAX_TAG_LENGTH + 1], int count, const char* tag) {
  if (!tag || tag[0] == '\0') {
    return true;
  }
  for (int i = 0; i < count; ++i) {
    if (strcmp(out[i], tag) == 0) {
      return true;
    }
  }
  return false;
}

bool parse_tags_array(const JsonVariantConst& array, document_record_t* out) {
  if (!out || !array.is<JsonArrayConst>()) {
    return false;
  }

  bool any = false;
  for (JsonVariantConst tag_variant : array.as<JsonArrayConst>()) {
    if (!tag_variant.is<const char*>()) {
      continue;
    }
    const char* source = tag_variant.as<const char*>();
    char tag[STORE_MAX_TAG_LENGTH + 1];
    safe_copy(tag, sizeof(tag), source);
    if (strlen(source) >= sizeof(tag)) {
      out->truncated = true;
    }
    trim_in_place(tag);
    if (tag[0] == '\0') {
      continue;
    }
    if (out->tag_count >= STORE_MAX_TAGS_PER_DOC) {
      out->truncated = true;
      break;
    }
    safe_copy(out->tags[out->tag_count], sizeof(out->tags[out->tag_count]), tag);
    ++out->tag_count;
    any = true;
  }
  return any;
}

}  // namespace

void safe_copy(char* dest, size_t dest_size, const char* src) {
  if (!dest || dest_size == 0) {
    return;
  }
  if (!src) {
    dest[0] = '\0';
    return;
  }
  strncpy(dest, src, dest_size - 1);
  dest[dest_size - 1] = '\0';
}

void trim_in_place(char* text) {
  if (!text) {
    return;
  }

  size_t start = 0;
  while (text[start] != '\0' && isspace(static_cast<unsigned char>(text[start]))) {
    ++start;
  }

  size_t end = strlen(text);
  while (end > start && isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }

  if (start > 0) {
    memmove(text, text + start, end - start);
  }
  text[end - start] = '\0';
}

bool is_root(const char* path) {
  return !path || path[0] == '\0' || strcmp(path, "/") == 0;
}

void basename_from_path(const char* path, char* out, size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }

  if (!path || path[0] == '\0') {
    out[0] = '\0';
    return;
  }

  const char* slash = strrchr(path, '/');
  safe_copy(out, out_size, slash ? slash + 1 : path);
}

void normalize_local_folder_path(const char* input, char* out, size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }

  out[0] = '\0';
  if (!input) {
    safe_copy(out, out_size, "/");
    return;
  }

  size_t out_index = 0;
  const size_t input_len = strlen(input);
  size_t start = 0;
  size_t end = input_len;

  while (start < input_len &&
         (isspace(static_cast<unsigned char>(input[start])) || input[start] == '/' || input[start] == '\\')) {
    ++start;
  }
  while (end > start &&
         (isspace(static_cast<unsigned char>(input[end - 1])) || input[end - 1] == '/' || input[end - 1] == '\\')) {
    --end;
  }

  if (out_index + 1 < out_size) {
    out[out_index++] = '/';
  }

  bool previous_was_slash = true;
  for (size_t i = start; i < end && out_index + 1 < out_size; ++i) {
    char c = input[i];
    if (c == '\\') {
      c = '/';
    }
    if (c == '/') {
      if (previous_was_slash) {
        continue;
      }
      out[out_index++] = '/';
      previous_was_slash = true;
      continue;
    }

    out[out_index++] = c;
    previous_was_slash = false;
  }

  if (out_index > 1 && out[out_index - 1] == '/') {
    --out_index;
  }
  out[out_index] = '\0';
}

void local_folder_to_remote(const char* local, char* out, size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }

  char normalized[STORE_MAX_FOLDER_PATH + 1];
  normalize_local_folder_path(local, normalized, sizeof(normalized));
  if (is_root(normalized)) {
    out[0] = '\0';
    return;
  }
  safe_copy(out, out_size, normalized + 1);
}

void join_local_path(const char* parent, const char* name, char* out, size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }

  char normalized_parent[STORE_MAX_FOLDER_PATH + 1];
  char child[STORE_MAX_TITLE_LENGTH + 1];
  normalize_local_folder_path(parent, normalized_parent, sizeof(normalized_parent));
  safe_copy(child, sizeof(child), name);
  trim_in_place(child);
  while (child[0] == '/' || child[0] == '\\') {
    memmove(child, child + 1, strlen(child));
  }
  if (child[0] == '\0') {
    safe_copy(out, out_size, normalized_parent);
    return;
  }

  if (is_root(normalized_parent)) {
    snprintf(out, out_size, "/%s", child);
  } else {
    snprintf(out, out_size, "%s/%s", normalized_parent, child);
  }
}

bool is_immediate_child(const char* parent, const char* candidate) {
  if (!candidate || candidate[0] != '/') {
    return false;
  }

  char normalized_parent[STORE_MAX_FOLDER_PATH + 1];
  normalize_local_folder_path(parent, normalized_parent, sizeof(normalized_parent));

  if (is_root(normalized_parent)) {
    const char* slash = strchr(candidate + 1, '/');
    return slash == nullptr;
  }

  const size_t parent_len = strlen(normalized_parent);
  if (strncmp(candidate, normalized_parent, parent_len) != 0 || candidate[parent_len] != '/') {
    return false;
  }

  const char* rest = candidate + parent_len + 1;
  return strchr(rest, '/') == nullptr;
}

void split_file_name(const char* file_name,
                     char* title,
                     size_t title_size,
                     char* extension,
                     size_t extension_size) {
  if (extension && extension_size > 0) {
    extension[0] = '\0';
  }

  // Split the full leaf name before clipping anything, so a long title cannot swallow the
  // extension ("compte-rendu-reunion-hebdomadaire.txt" stays a .txt).
  const char* leaf = file_name ? file_name : "";
  const char* slash = strrchr(leaf, '/');
  if (slash) {
    leaf = slash + 1;
  }
  while (isspace(static_cast<unsigned char>(*leaf))) {
    ++leaf;
  }
  size_t leaf_len = strlen(leaf);
  while (leaf_len > 0 && isspace(static_cast<unsigned char>(leaf[leaf_len - 1]))) {
    --leaf_len;
  }

  size_t title_len = leaf_len;
  for (size_t i = leaf_len; i > 1; --i) {  // a leading dot belongs to the title
    if (leaf[i - 1] == '.') {
      title_len = i - 1;
      copy_span(extension, extension_size, leaf + i, leaf_len - i);
      break;
    }
  }
  copy_span(title, title_size, leaf, title_len);
  trim_in_place(title);

  if (title && title[0] == '\0') {
    safe_copy(title, title_size, "untitled");
  }
  if (extension && extension[0] == '\0') {
    safe_copy(extension, extension_size, "md");
  }
}

void split_name_or_filename(const char* input,
                            char* file_name,
                            size_t file_name_size,
                            char* title,
                            size_t title_size,
                            char* extension,
                            size_t extension_size) {
  split_file_name(input, title, title_size, extension, extension_size);
  if (file_name && file_name_size > 0) {
    snprintf(file_name, file_name_size, "%s.%s", title, extension);
  }
}

bool build_document_id(const char* folder_path, const char* file_name, char* out, size_t out_size) {
  if (!out || out_size == 0 || !file_name || file_name[0] == '\0') {
    return false;
  }

  char normalized_folder[STORE_MAX_FOLDER_PATH + 1];
  char leaf[STORE_MAX_FILE_NAME_LENGTH + 1];
  normalize_local_folder_path(folder_path, normalized_folder, sizeof(normalized_folder));
  basename_from_path(file_name, leaf, sizeof(leaf));
  trim_in_place(leaf);
  if (leaf[0] == '\0') {
    return false;
  }

  if (is_root(normalized_folder)) {
    snprintf(out, out_size, "/%s", leaf);
  } else {
    snprintf(out, out_size, "%s/%s", normalized_folder, leaf);
  }
  return out[0] != '\0';
}

bool parse_document_id(const char* id,
                       char* folder_path,
                       size_t folder_path_size,
                       char* file_name,
                       size_t file_name_size) {
  if (!id || !folder_path || !file_name || folder_path_size == 0 || file_name_size == 0) {
    return false;
  }

  char buffer[STORE_MAX_DOCUMENT_ID + 1];
  safe_copy(buffer, sizeof(buffer), id);
  trim_in_place(buffer);
  if (buffer[0] != '/') {
    return false;
  }

  char* slash = strrchr(buffer, '/');
  if (!slash || slash[1] == '\0') {
    return false;
  }

  safe_copy(file_name, file_name_size, slash + 1);
  if (slash == buffer) {
    safe_copy(folder_path, folder_path_size, "/");
  } else {
    *slash = '\0';
    normalize_local_folder_path(buffer, folder_path, folder_path_size);
  }
  return file_name[0] != '\0';
}

void build_tags_summary(const document_record_t& doc, char* out, size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }

  out[0] = '\0';
  for (uint8_t i = 0; i < doc.tag_count; ++i) {
    if (i > 0) {
      strncat(out, ",", out_size - strlen(out) - 1);
    }
    strncat(out, doc.tags[i], out_size - strlen(out) - 1);
  }
}

void set_tags_from_csv(document_record_t* doc, const char* csv_tags) {
  if (!doc) {
    return;
  }

  for (int i = 0; i < STORE_MAX_TAGS_PER_DOC; ++i) {
    doc->tags[i][0] = '\0';
  }
  doc->tag_count = 0;

  if (!csv_tags || csv_tags[0] == '\0') {
    return;
  }

  char buffer[96];
  safe_copy(buffer, sizeof(buffer), csv_tags);
  char* token = strtok(buffer, ",");
  while (token && doc->tag_count < STORE_MAX_TAGS_PER_DOC) {
    trim_in_place(token);
    if (token[0] != '\0') {
      safe_copy(doc->tags[doc->tag_count], sizeof(doc->tags[doc->tag_count]), token);
      ++doc->tag_count;
    }
    token = strtok(nullptr, ",");
  }
}

bool document_has_tag(const document_record_t& doc, const char* tag_filter) {
  if (!tag_filter || tag_filter[0] == '\0') {
    return true;
  }
  for (uint8_t i = 0; i < doc.tag_count; ++i) {
    if (strcmp(doc.tags[i], tag_filter) == 0) {
      return true;
    }
  }
  return false;
}

bool parse_document_record(const JsonVariantConst& item, document_record_t* out, bool include_body) {
  if (!out) {
    return false;
  }

  JsonVariantConst object = unwrap_document(item);
  if (!object.is<JsonObjectConst>()) {
    return false;
  }

  memset(out, 0, sizeof(*out));
  out->in_use = true;
  safe_copy(out->folder_path, sizeof(out->folder_path), "/");

  char file_name[STORE_MAX_FILE_NAME_LENGTH + 1];
  file_name[0] = '\0';

  JsonVariantConst folder_variant = object["folder"];
  if (folder_variant.is<const char*>()) {
    normalize_local_folder_path(folder_variant.as<const char*>(), out->folder_path, sizeof(out->folder_path));
  } else if (folder_variant.is<JsonObjectConst>()) {
    parse_folder_path_from_item(folder_variant, out->folder_path, sizeof(out->folder_path));
  } else {
    const char* const folder_keys[] = {"folderPath", "folder_path"};
    char folder_buffer[STORE_MAX_FOLDER_PATH + 1];
    if (copy_string_field(object,
                          folder_keys,
                          sizeof(folder_keys) / sizeof(folder_keys[0]),
                          folder_buffer,
                          sizeof(folder_buffer))) {
      normalize_local_folder_path(folder_buffer, out->folder_path, sizeof(out->folder_path));
    }
  }

  const char* const name_keys[] = {"name", "fileName", "filename"};
  copy_string_field(object, name_keys, sizeof(name_keys) / sizeof(name_keys[0]), file_name, sizeof(file_name));

  char path_buffer[STORE_MAX_DOCUMENT_ID + 1];
  path_buffer[0] = '\0';
  const char* const path_keys[] = {"path", "fullPath", "filePath"};
  copy_string_field(object, path_keys, sizeof(path_keys) / sizeof(path_keys[0]), path_buffer, sizeof(path_buffer));
  if (path_buffer[0] != '\0') {
    char parsed_folder[STORE_MAX_FOLDER_PATH + 1];
    char parsed_file[STORE_MAX_FILE_NAME_LENGTH + 1];
    if (parse_file_path(path_buffer, parsed_folder, sizeof(parsed_folder), parsed_file, sizeof(parsed_file))) {
      if (file_name[0] == '\0') {
        safe_copy(file_name, sizeof(file_name), parsed_file);
      }
      if (is_root(out->folder_path)) {
        safe_copy(out->folder_path, sizeof(out->folder_path), parsed_folder);
      }
    }
  }

  char derived_title[STORE_MAX_TITLE_LENGTH + 1];
  char derived_extension[STORE_MAX_EXTENSION_LENGTH + 1];
  derived_title[0] = '\0';
  derived_extension[0] = '\0';
  if (file_name[0] != '\0') {
    split_file_name(file_name, derived_title, sizeof(derived_title), derived_extension, sizeof(derived_extension));
  }

  const char* const title_keys[] = {"title"};
  if (!copy_string_field(object,
                         title_keys,
                         sizeof(title_keys) / sizeof(title_keys[0]),
                         out->title,
                         sizeof(out->title),
                         &out->truncated)) {
    safe_copy(out->title, sizeof(out->title), derived_title);
  } else {
    trim_in_place(out->title);
  }
  if (out->title[0] == '\0') {
    safe_copy(out->title, sizeof(out->title), "untitled");
  }

  const char* const extension_keys[] = {"extension", "ext"};
  if (!copy_string_field(object,
                         extension_keys,
                         sizeof(extension_keys) / sizeof(extension_keys[0]),
                         out->extension,
                         sizeof(out->extension))) {
    safe_copy(out->extension, sizeof(out->extension), derived_extension);
  } else {
    trim_in_place(out->extension);
  }
  if (out->extension[0] == '\0') {
    safe_copy(out->extension, sizeof(out->extension), "md");
  }

  if (file_name[0] == '\0') {
    snprintf(file_name, sizeof(file_name), "%s.%s", out->title, out->extension);
  }

  if (!build_document_id(out->folder_path, file_name, out->id, sizeof(out->id))) {
    return false;
  }

  if (include_body) {
    const char* const body_keys[] = {"content", "body"};
    copy_string_field(object,
                      body_keys,
                      sizeof(body_keys) / sizeof(body_keys[0]),
                      out->body,
                      sizeof(out->body),
                      &out->truncated);
  }

  const char* const time_keys[] = {"updatedAt", "updated_at", "modifiedAt", "createdAt"};
  copy_string_field(
      object, time_keys, sizeof(time_keys) / sizeof(time_keys[0]), out->updated_at, sizeof(out->updated_at));
  const char* const version_keys[] = {"sortOrder", "sort_order", "version"};
  out->version = first_uint16_field(object, version_keys, sizeof(version_keys) / sizeof(version_keys[0]));

  JsonVariantConst tags_variant = object["tags"];
  if (tags_variant.is<JsonArrayConst>()) {
    parse_tags_array(tags_variant, out);
  } else if (tags_variant.is<const char*>()) {
    set_tags_from_csv(out, tags_variant.as<const char*>());
  } else {
    JsonVariantConst labels_variant = object["labels"];
    if (labels_variant.is<JsonArrayConst>()) {
      parse_tags_array(labels_variant, out);
    }
  }

  return true;
}

int parse_document_summaries(const JsonVariantConst& root,
                             const char* folder_path,
                             const char* tag_filter,
                             document_summary_t* out,
                             int max_items) {
  if (!out || max_items <= 0) {
    return 0;
  }

  char normalized_folder[STORE_MAX_FOLDER_PATH + 1];
  normalize_local_folder_path(folder_path, normalized_folder, sizeof(normalized_folder));
  JsonVariantConst collection = resolve_collection_root(root, "items");
  if (!collection.is<JsonArrayConst>()) {
    return 0;
  }

  int count = 0;
  for (JsonVariantConst item : collection.as<JsonArrayConst>()) {
    if (count >= max_items) {
      break;
    }

    document_record_t record{};
    if (!parse_document_record(item, &record, false)) {
      continue;
    }
    if (strcmp(record.folder_path, normalized_folder) != 0 || !document_has_tag(record, tag_filter)) {
      continue;
    }

    safe_copy(out[count].id, sizeof(out[count].id), record.id);
    safe_copy(out[count].title, sizeof(out[count].title), record.title);
    safe_copy(out[count].extension, sizeof(out[count].extension), record.extension);
    safe_copy(out[count].folder_path, sizeof(out[count].folder_path), record.folder_path);
    build_tags_summary(record, out[count].tags_summary, sizeof(out[count].tags_summary));
    out[count].version = record.version;
    ++count;
  }
  return count;
}

int parse_folder_items(const JsonVariantConst& root,
                       const char* parent_path,
                       folder_info_t* out,
                       int max_items) {
  if (!out || max_items <= 0) {
    return 0;
  }

  JsonVariantConst collection = resolve_collection_root(root, "items");
  if (!collection.is<JsonArrayConst>()) {
    return 0;
  }

  int count = 0;
  for (JsonVariantConst item : collection.as<JsonArrayConst>()) {
    if (count >= max_items) {
      break;
    }

    char local_path[STORE_MAX_FOLDER_PATH + 1];
    if (!parse_folder_path_from_item(item, local_path, sizeof(local_path))) {
      continue;
    }
    if (!is_immediate_child(parent_path, local_path) || folder_exists(out, count, local_path)) {
      continue;
    }

    safe_copy(out[count].path, sizeof(out[count].path), local_path);
    basename_from_path(local_path, out[count].name, sizeof(out[count].name));
    ++count;
  }
  return count;
}

int collect_unique_tags(const JsonVariantConst& root,
                        char out[][STORE_MAX_TAG_LENGTH + 1],
                        int max_items) {
  if (!out || max_items <= 0) {
    return 0;
  }

  JsonVariantConst collection = resolve_collection_root(root, "items");
  if (!collection.is<JsonArrayConst>()) {
    return 0;
  }

  int count = 0;
  for (JsonVariantConst item : collection.as<JsonArrayConst>()) {
    document_record_t record{};
    if (!parse_document_record(item, &record, false)) {
      continue;
    }

    for (uint8_t i = 0; i < record.tag_count && count < max_items; ++i) {
      if (tag_exists(out, count, record.tags[i])) {
        continue;
      }
      safe_copy(out[count], STORE_MAX_TAG_LENGTH + 1, record.tags[i]);
      ++count;
    }
  }
  return count;
}

}  // namespace api_document_store_utils
