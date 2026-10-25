#ifndef STORAGE_API_DOCUMENT_STORE_UTILS_H
#define STORAGE_API_DOCUMENT_STORE_UTILS_H

#include <stddef.h>
#include <stdint.h>

#include <ArduinoJson.h>

#include "document_store.h"

namespace api_document_store_utils {

void safe_copy(char* dest, size_t dest_size, const char* src);
void trim_in_place(char* text);

bool is_root(const char* path);
void basename_from_path(const char* path, char* out, size_t out_size);
void normalize_local_folder_path(const char* input, char* out, size_t out_size);
void local_folder_to_remote(const char* local, char* out, size_t out_size);
void join_local_path(const char* parent, const char* name, char* out, size_t out_size);
bool is_immediate_child(const char* parent, const char* candidate);

void split_file_name(const char* file_name,
                     char* title,
                     size_t title_size,
                     char* extension,
                     size_t extension_size);
void split_name_or_filename(const char* input,
                            char* file_name,
                            size_t file_name_size,
                            char* title,
                            size_t title_size,
                            char* extension,
                            size_t extension_size);
bool build_document_id(const char* folder_path, const char* file_name, char* out, size_t out_size);
bool parse_document_id(const char* id,
                       char* folder_path,
                       size_t folder_path_size,
                       char* file_name,
                       size_t file_name_size);

void build_tags_summary(const document_record_t& doc, char* out, size_t out_size);
void set_tags_from_csv(document_record_t* doc, const char* csv_tags);
bool document_has_tag(const document_record_t& doc, const char* tag_filter);

bool parse_document_record(const JsonVariantConst& item, document_record_t* out, bool include_body);
int parse_document_summaries(const JsonVariantConst& root,
                             const char* folder_path,
                             const char* tag_filter,
                             document_summary_t* out,
                             int max_items);
int parse_folder_items(const JsonVariantConst& root,
                       const char* parent_path,
                       folder_info_t* out,
                       int max_items);
int collect_unique_tags(const JsonVariantConst& root,
                        char out[][STORE_MAX_TAG_LENGTH + 1],
                        int max_items);

}  // namespace api_document_store_utils

#endif
