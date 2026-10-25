#include "mock_document_store.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "platform/app_log.h"

namespace {
const char* ROOT_FOLDER = "/";

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
  while (text[start] && isspace(static_cast<unsigned char>(text[start]))) {
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

bool string_equals(const char* a, const char* b) {
  if (!a || !b) {
    return false;
  }
  return strcmp(a, b) == 0;
}

bool is_root(const char* path) {
  return !path || path[0] == '\0' || strcmp(path, ROOT_FOLDER) == 0;
}

void split_filename(const char* input,
                    char* title,
                    size_t title_size,
                    char* extension,
                    size_t extension_size) {
  safe_copy(title, title_size, input);
  trim_in_place(title);
  extension[0] = '\0';

  char* dot = strrchr(title, '.');
  if (dot && dot != title) {
    safe_copy(extension, extension_size, dot + 1);
    *dot = '\0';
    trim_in_place(title);
  }

  if (title[0] == '\0') {
    safe_copy(title, title_size, "untitled");
  }
  if (extension[0] == '\0') {
    safe_copy(extension, extension_size, "md");
  }
}

void join_path(const char* parent, const char* name, char* out, size_t out_size) {
  if (is_root(parent)) {
    snprintf(out, out_size, "/%s", name);
  } else {
    snprintf(out, out_size, "%s/%s", parent, name);
  }
}

bool is_immediate_child(const char* parent, const char* candidate) {
  if (!candidate || candidate[0] != '/') {
    return false;
  }

  if (is_root(parent)) {
    const char* slash = strchr(candidate + 1, '/');
    return slash == nullptr;
  }

  size_t parent_len = strlen(parent);
  if (strncmp(candidate, parent, parent_len) != 0 || candidate[parent_len] != '/') {
    return false;
  }

  const char* rest = candidate + parent_len + 1;
  return strchr(rest, '/') == nullptr;
}

void basename_from_path(const char* path, char* out, size_t out_size) {
  const char* slash = strrchr(path, '/');
  safe_copy(out, out_size, slash ? slash + 1 : path);
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
}  // namespace

MockDocumentStore::MockDocumentStore() : initialized_(false) {
  memset(folders_, 0, sizeof(folders_));
  memset(documents_, 0, sizeof(documents_));
}

void MockDocumentStore::begin() {
  if (!initialized_) {
    load_seed_data();
    initialized_ = true;
    app_log_info("store", "mock store initialized");
  }
}

int MockDocumentStore::list_folders(const char* parent_path, folder_info_t* out, int max_items) {
  if (!out || max_items <= 0) {
    return 0;
  }

  int count = 0;
  for (int i = 0; i < STORE_MAX_FOLDERS && count < max_items; ++i) {
    if (folders_[i].path[0] == '\0' || is_root(folders_[i].path)) {
      continue;
    }
    if (is_immediate_child(parent_path, folders_[i].path)) {
      out[count++] = folders_[i];
    }
  }
  app_log_debug("store", "list_folders parent=\"%s\" count=%d", parent_path ? parent_path : "<null>", count);
  return count;
}

int MockDocumentStore::list_documents(const char* folder_path,
                                      const char* tag_filter,
                                      document_summary_t* out,
                                      int max_items) {
  if (!out || max_items <= 0) {
    return 0;
  }

  int count = 0;
  for (int i = 0; i < STORE_MAX_DOCUMENTS && count < max_items; ++i) {
    if (!documents_[i].in_use) {
      continue;
    }
    if (!string_equals(documents_[i].folder_path, folder_path)) {
      continue;
    }
    if (!document_has_tag(documents_[i], tag_filter)) {
      continue;
    }

    safe_copy(out[count].id, sizeof(out[count].id), documents_[i].id);
    safe_copy(out[count].title, sizeof(out[count].title), documents_[i].title);
    safe_copy(out[count].extension, sizeof(out[count].extension), documents_[i].extension);
    safe_copy(out[count].folder_path, sizeof(out[count].folder_path), documents_[i].folder_path);
    build_tags_summary(documents_[i], out[count].tags_summary, sizeof(out[count].tags_summary));
    out[count].version = documents_[i].version;
    ++count;
  }
  app_log_debug("store",
                "list_documents folder=\"%s\" tag=\"%s\" count=%d",
                folder_path ? folder_path : "<null>",
                (tag_filter && tag_filter[0]) ? tag_filter : "<none>",
                count);
  return count;
}

int MockDocumentStore::list_tags(char out[][STORE_MAX_TAG_LENGTH + 1], int max_items) {
  if (!out || max_items <= 0) {
    return 0;
  }

  int count = 0;
  for (int i = 0; i < STORE_MAX_DOCUMENTS && count < max_items; ++i) {
    if (!documents_[i].in_use) {
      continue;
    }
    for (uint8_t tag_index = 0; tag_index < documents_[i].tag_count && count < max_items; ++tag_index) {
      bool already_seen = false;
      for (int existing = 0; existing < count; ++existing) {
        if (strcmp(out[existing], documents_[i].tags[tag_index]) == 0) {
          already_seen = true;
          break;
        }
      }
      if (!already_seen) {
        safe_copy(out[count], STORE_MAX_TAG_LENGTH + 1, documents_[i].tags[tag_index]);
        ++count;
      }
    }
  }
  return count;
}

bool MockDocumentStore::get_document(const char* id, document_record_t* out) {
  if (!out) {
    return false;
  }

  const document_record_t* doc = find_document(id);
  if (!doc) {
    app_log_warn("store", "get_document id=\"%s\" result=missing", id ? id : "<null>");
    return false;
  }

  *out = *doc;
  app_log_info("store", "get_document id=\"%s\" title=\"%s\"", out->id, out->title);
  return true;
}

bool MockDocumentStore::save_document(const document_record_t& document) {
  document_record_t* target = find_document(document.id);
  if (!target) {
    app_log_warn("store", "save_document id=\"%s\" result=missing", document.id);
    return false;
  }
  *target = document;
  target->version = document.version + 1;
  safe_copy(target->updated_at, sizeof(target->updated_at), "local-save");
  app_log_info("store",
               "save_document id=\"%s\" title=\"%s\" version=%u",
               target->id,
               target->title,
               static_cast<unsigned>(target->version));
  return true;
}

bool MockDocumentStore::create_document(const char* folder_path,
                                        const char* name_or_filename,
                                        document_record_t* out) {
  const int slot = next_document_slot();
  if (slot < 0 || !folder_exists(folder_path) || !name_or_filename) {
    app_log_warn("store",
                 "create_document folder=\"%s\" name=\"%s\" result=ko",
                 folder_path ? folder_path : "<null>",
                 name_or_filename ? name_or_filename : "<null>");
    return false;
  }

  document_record_t doc{};
  doc.in_use = true;
  snprintf(doc.id, sizeof(doc.id), "doc%02d", slot + 1);
  split_filename(name_or_filename, doc.title, sizeof(doc.title), doc.extension, sizeof(doc.extension));
  safe_copy(doc.folder_path, sizeof(doc.folder_path), folder_path);
  doc.version = 1;
  safe_copy(doc.updated_at, sizeof(doc.updated_at), "local-create");

  if (strcmp(doc.extension, "md") == 0) {
    snprintf(doc.body, sizeof(doc.body), "# %s\n\n", doc.title);
  } else {
    doc.body[0] = '\0';
  }

  documents_[slot] = doc;
  if (out) {
    *out = documents_[slot];
  }
  app_log_info("store",
               "create_document id=\"%s\" title=\"%s\" extension=\"%s\" folder=\"%s\"",
               documents_[slot].id,
               documents_[slot].title,
               documents_[slot].extension,
               documents_[slot].folder_path);
  return true;
}

bool MockDocumentStore::create_folder(const char* parent_path, const char* name) {
  const int slot = next_folder_slot();
  if (slot < 0 || !folder_exists(parent_path) || !name) {
    app_log_warn("store",
                 "create_folder parent=\"%s\" name=\"%s\" result=ko",
                 parent_path ? parent_path : "<null>",
                 name ? name : "<null>");
    return false;
  }

  char trimmed_name[STORE_MAX_TITLE_LENGTH + 1];
  safe_copy(trimmed_name, sizeof(trimmed_name), name);
  trim_in_place(trimmed_name);
  if (trimmed_name[0] == '\0') {
    return false;
  }

  char new_path[STORE_MAX_FOLDER_PATH + 1];
  join_path(parent_path, trimmed_name, new_path, sizeof(new_path));
  if (folder_exists(new_path)) {
    app_log_warn("store", "create_folder path=\"%s\" result=exists", new_path);
    return false;
  }

  safe_copy(folders_[slot].path, sizeof(folders_[slot].path), new_path);
  basename_from_path(new_path, folders_[slot].name, sizeof(folders_[slot].name));
  app_log_info("store", "create_folder path=\"%s\"", folders_[slot].path);
  return true;
}

bool MockDocumentStore::update_tags(const char* id, const char* csv_tags) {
  document_record_t* doc = find_document(id);
  if (!doc) {
    app_log_warn("store", "update_tags id=\"%s\" result=missing", id ? id : "<null>");
    return false;
  }

  set_tags_from_csv(doc, csv_tags);
  safe_copy(doc->updated_at, sizeof(doc->updated_at), "local-tags");
  app_log_info("store", "update_tags id=\"%s\" tags=\"%s\"", doc->id, csv_tags ? csv_tags : "");
  return true;
}

void MockDocumentStore::load_seed_data() {
  safe_copy(folders_[0].path, sizeof(folders_[0].path), ROOT_FOLDER);
  safe_copy(folders_[0].name, sizeof(folders_[0].name), ROOT_FOLDER);
  safe_copy(folders_[1].path, sizeof(folders_[1].path), "/notes");
  safe_copy(folders_[1].name, sizeof(folders_[1].name), "notes");
  safe_copy(folders_[2].path, sizeof(folders_[2].path), "/notes/ideas");
  safe_copy(folders_[2].name, sizeof(folders_[2].name), "ideas");
  safe_copy(folders_[3].path, sizeof(folders_[3].path), "/projects");
  safe_copy(folders_[3].name, sizeof(folders_[3].name), "projects");
  safe_copy(folders_[4].path, sizeof(folders_[4].path), "/projects/minios");
  safe_copy(folders_[4].name, sizeof(folders_[4].name), "minios");

  documents_[0].in_use = true;
  safe_copy(documents_[0].id, sizeof(documents_[0].id), "doc01");
  safe_copy(documents_[0].title, sizeof(documents_[0].title), "welcome");
  safe_copy(documents_[0].extension, sizeof(documents_[0].extension), "md");
  safe_copy(documents_[0].folder_path, sizeof(documents_[0].folder_path), ROOT_FOLDER);
  documents_[0].tag_count = 2;
  safe_copy(documents_[0].tags[0], sizeof(documents_[0].tags[0]), "start");
  safe_copy(documents_[0].tags[1], sizeof(documents_[0].tags[1]), "docs");
  safe_copy(documents_[0].updated_at, sizeof(documents_[0].updated_at), "2026-03-22");
  documents_[0].version = 1;
  safe_copy(documents_[0].body, sizeof(documents_[0].body),
            "# Welcome\n\nThis is the mock document store.\nUse N to create files and D to create folders.\n");

  documents_[1].in_use = true;
  safe_copy(documents_[1].id, sizeof(documents_[1].id), "doc02");
  safe_copy(documents_[1].title, sizeof(documents_[1].title), "wifi_notes");
  safe_copy(documents_[1].extension, sizeof(documents_[1].extension), "txt");
  safe_copy(documents_[1].folder_path, sizeof(documents_[1].folder_path), "/notes");
  documents_[1].tag_count = 2;
  safe_copy(documents_[1].tags[0], sizeof(documents_[1].tags[0]), "wifi");
  safe_copy(documents_[1].tags[1], sizeof(documents_[1].tags[1]), "ops");
  safe_copy(documents_[1].updated_at, sizeof(documents_[1].updated_at), "2026-03-22");
  documents_[1].version = 1;
  safe_copy(documents_[1].body, sizeof(documents_[1].body),
            "Fallback network comes from local secrets.\nChange it in the WiFi screen when needed.\n");

  documents_[2].in_use = true;
  safe_copy(documents_[2].id, sizeof(documents_[2].id), "doc03");
  safe_copy(documents_[2].title, sizeof(documents_[2].title), "roadmap");
  safe_copy(documents_[2].extension, sizeof(documents_[2].extension), "md");
  safe_copy(documents_[2].folder_path, sizeof(documents_[2].folder_path), "/projects/minios");
  documents_[2].tag_count = 2;
  safe_copy(documents_[2].tags[0], sizeof(documents_[2].tags[0]), "plan");
  safe_copy(documents_[2].tags[1], sizeof(documents_[2].tags[1]), "v1");
  safe_copy(documents_[2].updated_at, sizeof(documents_[2].updated_at), "2026-03-22");
  documents_[2].version = 3;
  safe_copy(documents_[2].body, sizeof(documents_[2].body),
            "# MiniOS v1\n\n- WiFi boot\n- Top bar\n- Files mock\n- Full-screen editor\n");

  documents_[3].in_use = true;
  safe_copy(documents_[3].id, sizeof(documents_[3].id), "doc04");
  safe_copy(documents_[3].title, sizeof(documents_[3].title), "api_contract");
  safe_copy(documents_[3].extension, sizeof(documents_[3].extension), "ts");
  safe_copy(documents_[3].folder_path, sizeof(documents_[3].folder_path), "/projects/minios");
  documents_[3].tag_count = 2;
  safe_copy(documents_[3].tags[0], sizeof(documents_[3].tags[0]), "api");
  safe_copy(documents_[3].tags[1], sizeof(documents_[3].tags[1]), "server");
  safe_copy(documents_[3].updated_at, sizeof(documents_[3].updated_at), "2026-03-22");
  documents_[3].version = 1;
  safe_copy(documents_[3].body, sizeof(documents_[3].body),
            "export type DocumentDto = {\n  id: string;\n  title: string;\n  extension: string;\n};\n");

  documents_[4].in_use = true;
  safe_copy(documents_[4].id, sizeof(documents_[4].id), "doc05");
  safe_copy(documents_[4].title, sizeof(documents_[4].title), "landing");
  safe_copy(documents_[4].extension, sizeof(documents_[4].extension), "html");
  safe_copy(documents_[4].folder_path, sizeof(documents_[4].folder_path), "/projects");
  documents_[4].tag_count = 1;
  safe_copy(documents_[4].tags[0], sizeof(documents_[4].tags[0]), "web");
  safe_copy(documents_[4].updated_at, sizeof(documents_[4].updated_at), "2026-03-22");
  documents_[4].version = 2;
  safe_copy(documents_[4].body, sizeof(documents_[4].body),
            "<main>\n  <h1>Minitel miniOS</h1>\n  <p>Mock content from the device.</p>\n</main>\n");
}

int MockDocumentStore::next_document_slot() const {
  for (int i = 0; i < STORE_MAX_DOCUMENTS; ++i) {
    if (!documents_[i].in_use) {
      return i;
    }
  }
  return -1;
}

int MockDocumentStore::next_folder_slot() const {
  for (int i = 0; i < STORE_MAX_FOLDERS; ++i) {
    if (folders_[i].path[0] == '\0') {
      return i;
    }
  }
  return -1;
}

document_record_t* MockDocumentStore::find_document(const char* id) {
  if (!id) {
    return nullptr;
  }
  for (int i = 0; i < STORE_MAX_DOCUMENTS; ++i) {
    if (documents_[i].in_use && strcmp(documents_[i].id, id) == 0) {
      return &documents_[i];
    }
  }
  return nullptr;
}

const document_record_t* MockDocumentStore::find_document(const char* id) const {
  if (!id) {
    return nullptr;
  }
  for (int i = 0; i < STORE_MAX_DOCUMENTS; ++i) {
    if (documents_[i].in_use && strcmp(documents_[i].id, id) == 0) {
      return &documents_[i];
    }
  }
  return nullptr;
}

bool MockDocumentStore::folder_exists(const char* path) const {
  if (!path) {
    return false;
  }
  for (int i = 0; i < STORE_MAX_FOLDERS; ++i) {
    if (folders_[i].path[0] != '\0' && strcmp(folders_[i].path, path) == 0) {
      return true;
    }
  }
  return false;
}
