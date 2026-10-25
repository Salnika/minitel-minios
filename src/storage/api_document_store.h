#ifndef STORAGE_API_DOCUMENT_STORE_H
#define STORAGE_API_DOCUMENT_STORE_H

#include <ArduinoJson.h>

#include "document_store.h"
#include "workspace_utils.h"

class ApiDocumentStore : public DocumentStore {
public:
  ApiDocumentStore();

  void configure(const char* base_url, const char* api_token);
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
  bool is_available() const override;
  const char* last_error() const override;

  // Project manager routes of Bibliotek. `today` is the device's local YYYY-MM-DD.
  int list_projects(const char* today, project_summary_t* out, int max_items);
  bool create_project(const char* title, char* out_name, size_t out_name_size);
  // `due_filter` is "today" (due today or late) or nullptr for every open task.
  int list_tasks(const char* project,
                 const char* due_filter,
                 const char* today,
                 task_item_t* out,
                 int max_items,
                 int* out_total);
  bool add_task(const char* project, const task_input_t& task);
  bool set_task_done(const char* file, uint16_t line, bool done);
  // Appends one line to a file (created with its folders when missing).
  bool append_line(const char* id, const char* text);
  int search(const char* query, search_hit_t* out, int max_items);

private:
  bool available_;
  char base_url_[128];
  char api_token_[128];
  char last_error_[96];

  void clear_error();
  void set_error(const char* message);
  void set_http_error(int status_code, const char* response_body);
  bool perform_request(const char* method,
                       const char* path,
                       const char* query,
                       const char* json_body,
                       bool authorize,
                       DynamicJsonDocument* response);
};

#endif
