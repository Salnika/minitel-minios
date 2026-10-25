#ifndef APP_INPUT_H
#define APP_INPUT_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
  INPUT_EVENT_NONE = 0,
  INPUT_EVENT_PRINTABLE,
  INPUT_EVENT_ENTER,
  INPUT_EVENT_BACKSPACE,
  INPUT_EVENT_ESCAPE,
  INPUT_EVENT_CANCEL,
  INPUT_EVENT_SAVE,
  INPUT_EVENT_HOME,
  INPUT_EVENT_END,
  INPUT_EVENT_PAGE_UP,
  INPUT_EVENT_PAGE_DOWN,
  INPUT_EVENT_UP,
  INPUT_EVENT_DOWN,
  INPUT_EVENT_LEFT,
  INPUT_EVENT_RIGHT
} input_event_type_t;

typedef struct {
  input_event_type_t type;
  char ch;
} input_event_t;

typedef struct {
  uint8_t sequence_state;
  uint8_t last_special_code;
  uint32_t last_special_ms;
} input_parser_t;

void input_parser_init(input_parser_t* parser);
bool input_parser_feed(input_parser_t* parser, uint8_t byte, input_event_t* event);

#endif
