#ifndef STORAGE_DOCUMENT_STORE_H
#define STORAGE_DOCUMENT_STORE_H

#include <stdint.h>

constexpr int STORE_MAX_DOCUMENTS = 16;
constexpr int STORE_MAX_FOLDERS = 12;
constexpr int STORE_MAX_TAGS_PER_DOC = 4;
constexpr int STORE_MAX_TAG_LENGTH = 16;
constexpr int STORE_MAX_TITLE_LENGTH = 32;
constexpr int STORE_MAX_EXTENSION_LENGTH = 8;
constexpr int STORE_MAX_FOLDER_PATH = 48;
constexpr int STORE_MAX_FILE_NAME_LENGTH = STORE_MAX_TITLE_LENGTH + STORE_MAX_EXTENSION_LENGTH + 1;
constexpr int STORE_MAX_BODY_LENGTH = 2048;
constexpr int STORE_MAX_TIMESTAMP_LENGTH = 20;
constexpr int STORE_MAX_DOCUMENT_ID = STORE_MAX_FOLDER_PATH + STORE_MAX_FILE_NAME_LENGTH + 2;
// All tags joined with ',' (used to pre-fill tag editing, so it must never be clipped).
constexpr int STORE_MAX_TAGS_SUMMARY_LENGTH = STORE_MAX_TAGS_PER_DOC * (STORE_MAX_TAG_LENGTH + 1) - 1;

struct folder_info_t {
  char path[STORE_MAX_FOLDER_PATH + 1];
  char name[STORE_MAX_TITLE_LENGTH + 1];
};

struct document_summary_t {
  char id[STORE_MAX_DOCUMENT_ID + 1];
  char title[STORE_MAX_TITLE_LENGTH + 1];
  char extension[STORE_MAX_EXTENSION_LENGTH + 1];
  char folder_path[STORE_MAX_FOLDER_PATH + 1];
  char tags_summary[STORE_MAX_TAGS_SUMMARY_LENGTH + 1];
  uint16_t version;
};

struct document_record_t {
  char id[STORE_MAX_DOCUMENT_ID + 1];
  char title[STORE_MAX_TITLE_LENGTH + 1];
  char extension[STORE_MAX_EXTENSION_LENGTH + 1];
  char folder_path[STORE_MAX_FOLDER_PATH + 1];
  char tags[STORE_MAX_TAGS_PER_DOC][STORE_MAX_TAG_LENGTH + 1];
  uint8_t tag_count;
  char body[STORE_MAX_BODY_LENGTH + 1];
  char updated_at[STORE_MAX_TIMESTAMP_LENGTH + 1];
  uint16_t version;
  bool in_use;
  // Body, title or tags did not fit the buffers: saving it back would lose data, so the
  // record is read-only.
  bool truncated;
};

class DocumentStore {
public:
  virtual ~DocumentStore() {}
  virtual void begin() = 0;
  virtual int list_folders(const char* parent_path, folder_info_t* out, int max_items) = 0;
  virtual int list_documents(const char* folder_path,
                             const char* tag_filter,
                             document_summary_t* out,
                             int max_items) = 0;
  virtual int list_tags(char out[][STORE_MAX_TAG_LENGTH + 1], int max_items) = 0;
  virtual bool get_document(const char* id, document_record_t* out) = 0;
  virtual bool save_document(const document_record_t& document) = 0;
  virtual bool create_document(const char* folder_path,
                               const char* name_or_filename,
                               document_record_t* out) = 0;
  virtual bool create_folder(const char* parent_path, const char* name) = 0;
  virtual bool update_tags(const char* id, const char* csv_tags) = 0;
  virtual bool is_available() const { return true; }
  virtual const char* last_error() const { return ""; }
};

#endif
