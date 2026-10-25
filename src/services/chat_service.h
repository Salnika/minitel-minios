#ifndef SERVICES_CHAT_SERVICE_H
#define SERVICES_CHAT_SERVICE_H

#include <stddef.h>

#include "chat_service_utils.h"

class String;

enum chat_stage_t {
  CHAT_STAGE_CONNECTING = 0,
  CHAT_STAGE_THINKING,  // reasoning deltas, nothing to show yet
  CHAT_STAGE_ACTING,    // agent mode: the assistant works on the notes
  CHAT_STAGE_WRITING,
};

// Callbacks of a streamed reply, called on the task running stream_message().
struct chat_stream_hooks_t {
  // Polled between reads: true stops the reply where it is.
  bool (*cancelled)();
  // The reply grew to `length` characters and the actions to `actions_length` (written to
  // the caller's buffers, never changed afterwards), or the stage changed.
  void (*progress)(chat_stage_t stage, size_t length, size_t actions_length);
};

// Where an agent mode reply goes. The reply and the actions go on after what the buffers
// already hold: empty for a new question, the reply so far after a confirmation.
struct chat_agent_reply_t {
  char* text;
  size_t text_size;
  char* actions;
  size_t actions_size;
  chat_confirm_t* confirm;  // id set when the assistant waits for the user's approval
};

class ChatService {
public:
  ChatService();

  void configure(const char* base_url, const char* api_key, const char* model);
  // Bibliotek, which runs the agent mode.
  void configure_assistant(const char* api_url, const char* api_token);
  bool is_configured() const;
  // Sends the conversation with "stream": true and writes the reply into `out_reply` as it
  // arrives. On failure, a reply cut midway stays in `out_reply`.
  bool stream_message(const chat_history_t& history,
                      const char* prompt,
                      char* out_reply,
                      size_t out_reply_size,
                      const chat_stream_hooks_t& hooks,
                      bool* out_truncated);
  // Agent mode: the question goes to Bibliotek's assistant (POST /assistant), which runs the
  // model with tools on the notes and streams its reply and the actions it runs. `now` is
  // the local "YYYY-MM-DD HH:MM" ("" when unknown). When an action needs the user's
  // approval, the reply stops there with `reply.confirm->id` set: answer_confirmation()
  // resumes it. On failure, what arrived stays in the buffers.
  bool stream_agent(const chat_history_t& history,
                    const char* prompt,
                    const char* now,
                    const chat_agent_reply_t& reply,
                    const chat_stream_hooks_t& hooks,
                    bool* out_truncated);
  bool answer_confirmation(const char* id,
                           bool accept,
                           const chat_agent_reply_t& reply,
                           const chat_stream_hooks_t& hooks,
                           bool* out_truncated);
  // Chat models offered by the provider (DigitalOcean-owned only). Returns the count, or
  // -1 on error (see last_error()).
  int list_models(char out[][CHAT_MAX_MODEL_ID_LENGTH + 1], int max_items);
  const char* last_error() const;

private:
  char base_url_[129];
  char api_key_[193];
  char model_[49];
  char api_url_[97];
  char api_token_[97];
  char last_error_[96];

  void clear_error();
  void set_error(const char* message);
  void set_http_error(int status_code, const char* response_body);
  void set_assistant_http_error(bool confirmation, int status_code, const char* response_body);
  bool check_ai_settings();
  bool check_assistant_settings();
  bool stream_attempt(const chat_history_t& history,
                      const char* prompt,
                      bool retry_mode,
                      char* out_reply,
                      size_t out_reply_size,
                      const chat_stream_hooks_t& hooks);
  bool agent_attempt(bool confirmation,
                     String* payload,
                     const chat_agent_reply_t& reply,
                     const chat_stream_hooks_t& hooks,
                     bool* out_truncated);
};

#endif
