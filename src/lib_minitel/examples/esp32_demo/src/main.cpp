#include <Arduino.h>
#include "../gfx/minigfx.h"
#include "../hal/bind.h"

static void wr(const uint8_t* d, size_t n){ Serial2.write(d, n); }
static void pr(const char* s){ while(*s) Serial2.write((uint8_t)*s++); }
static void cs(uint8_t r,uint8_t c){ char esc[4]={0x1B,'Y', (char)(0x20 + r - 1), (char)(0x20 + c - 1)}; Serial2.write((uint8_t*)esc,4); }
static void cl(){ Serial2.write(0x0C); }

void setup(){
  Serial.begin(115200);
  Serial2.begin(1200, SERIAL_7E1, 23, 22);
  lm_bind_io(wr, pr, cs, cl);
  lm_gfx_clear();
  lm_gfx_set_mode(true);
  lm_gfx_set_color(LM_RED);
  lm_fill_rect(2,8,16,12,true);
  lm_draw_line(2,8,17,19,true);
}
void loop(){}
