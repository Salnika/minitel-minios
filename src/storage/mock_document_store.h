#ifndef STORAGE_MOCK_DOCUMENT_STORE_H
#define STORAGE_MOCK_DOCUMENT_STORE_H

#include "document_store.h"

class MockDocumentStore : public DocumentStore {
public:
  MockDocumentStore();

  void begin() override;
  int list_folders(const char* parent_path, folder_info_t* out, int max_items) override;
  int list_documents(const char* folder_path,
                     const char* tag_filter,
                     document_summary_t* out,
                     int max_items) override;
  int list_tags(char out[][STORE_MAX_TAG_LENGTH + 1], int max_items) override;
  bool get_document(const char* id, document_record_t* out) override;
  bool save_document(const document_record_t& document) override;
  bool create_document(const char* folder_path,
                       const char* name_or_filename,
                       document_record_t* out) override;
  bool create_folder(const char* parent_path, const char* name) override;
  bool update_tags(const char* id, const char* csv_tags) override;

private:
  bool initialized_;
  folder_info_t folders_[STORE_MAX_FOLDERS];
  document_record_t documents_[STORE_MAX_DOCUMENTS];

  void load_seed_data();
  int next_document_slot() const;
  int next_folder_slot() const;
  document_record_t* find_document(const char* id);
  const document_record_t* find_document(const char* id) const;
  bool folder_exists(const char* path) const;
};

#endif
