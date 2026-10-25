#include "lib_minitel/gfx/minigfx.h"
#include "lib_minitel/gfx/graphics_engine.h"
#include "lib_minitel/gfx/mosaic_map.h"

static inline ge_color_t to_ge(lm_color_t c){
    switch(c){
        case LM_RED: return GE_RED; case LM_GREEN: return GE_GREEN; case LM_BLUE: return GE_BLUE;
        case LM_YELLOW: return GE_YELLOW; case LM_MAGENTA: return GE_MAGENTA; case LM_CYAN: return GE_CYAN;
        case LM_WHITE: default: return GE_WHITE;
    }
}

void lm_gfx_clear(void){ ge_clear(); }
void lm_gfx_enter(bool contiguous){ ge_enter_graphics(contiguous); }
void lm_gfx_exit(void){ ge_exit_graphics(); }
void lm_gfx_set_color(lm_color_t c){ ge_color_t color = to_ge(c); mosaic_set_color(color); ge_set_color(color); }
void lm_gfx_set_contiguity(bool contiguous){ ge_set_mosaic_contiguous(contiguous); }
void lm_gfx_set_mode(bool contiguous){ mosaic_set_mode(contiguous); }
void lm_gfx_pixel(uint8_t x,uint8_t y,bool on){ mosaic_set_pixel(x,y,on); }
void lm_gfx_set_cell_mask(uint8_t r,uint8_t c,uint8_t m){ mosaic_set_cell_mask(r,c,m); }
uint8_t lm_gfx_get_cell_mask(uint8_t r,uint8_t c){ return mosaic_get_cell_mask(r,c); }
void lm_gfx_clear_pixels(void){ mosaic_clear(); }

static inline int abs_i(int v){ return v<0?-v:v; }
static inline void plot(int x,int y,bool on){ if(x>=0&&x<80&&y>=0&&y<75) lm_gfx_pixel((uint8_t)x,(uint8_t)y,on); }

void lm_draw_line(int x0,int y0,int x1,int y1,bool on){
    int dx = abs_i(x1-x0), sx = x0<x1?1:-1;
    int dy = -abs_i(y1-y0), sy = y0<y1?1:-1;
    int err = dx+dy;
    while (1){ plot(x0,y0,on); if (x0==x1 && y0==y1) break; int e2 = 2*err; if (e2>=dy){ err+=dy; x0+=sx; } if (e2<=dx){ err+=dx; y0+=sy; } }
}

void lm_draw_rect(int x,int y,int w,int h,bool on){
    if (w<=0||h<=0) return; lm_draw_line(x,y,x+w-1,y,on); lm_draw_line(x,y+h-1,x+w-1,y+h-1,on); lm_draw_line(x,y,x,y+h-1,on); lm_draw_line(x+w-1,y,x+w-1,y+h-1,on);
}

void lm_fill_rect(int x,int y,int w,int h,bool on){
    if (w<=0||h<=0) return; for (int yy=0; yy<h; ++yy) for (int xx=0; xx<w; ++xx) plot(x+xx,y+yy,on);
}

void lm_draw_circle(int cx,int cy,int r,bool on){
    int x=-r, y=0, err=2-2*r; do { plot(cx-x,cy+y,on); plot(cx-y,cy-x,on); plot(cx+x,cy-y,on); plot(cx+y,cy+x,on); r=err; if (r<=y) err+=++y*2+1; if (r> x || err>y) err+=++x*2+1; } while (x<0);
}
