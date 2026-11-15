#include "lvgl.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
float atan2f(float y,float x) { return (float)atan2((double)y,(double)x); }
float sqrtf(float x) { return (float)sqrt((double)x); }
float cosf(float x) { return (float)cos((double)x); }
float sinf(float x) { return (float)sin((double)x); }
enum { DRAW_DOT_SIZE=10, WIDTH=480, HEIGHT=420 };
/* PRODUCTION_STATE */
/* PRODUCTION_HELPERS */
/* PRODUCTION_FLAGS */
static lv_obj_t *root,*button;
static lv_indev_t *mouse;
static lv_indev_data_t input;
static DrawpanelStroke stroke;
static unsigned checks,presses,releases,resets;
#define CHECK(expr) do { ++checks; if(!(expr)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#expr);abort();} } while(0)
static void read_input(lv_indev_t *d,lv_indev_data_t *data) {(void)d;*data=input;}
static void event(lv_event_t *e) {
    if(lv_event_get_target(e)!=root) return;
    if(lv_event_get_code(e)==LV_EVENT_PRESSED) presses++;
    if(lv_event_get_code(e)==LV_EVENT_RELEASED) releases++;
    if(lv_event_get_code(e)==LV_EVENT_INDEV_RESET) resets++;
    handleDrawEvent(&stroke,e);
}
static void clear_clicked(lv_event_t *e) { (void)e;clearDrawCanvas(&stroke); }
static void point(int x,int y,bool down) {
    input.point.x=x;input.point.y=y;
    input.state=down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
    lv_tick_inc(16);lv_indev_read(mouse);
}
static bool red(int x,int y) {
    const unsigned char *row=(const unsigned char *)lv_canvas_get_buf(stroke.canvas)+y*lv_draw_buf_width_to_stride(WIDTH,LV_COLOR_FORMAT_RGB565);
    return ((const uint16_t *)row)[x]==0xF800;
}
static bool white(int x,int y) {
    const unsigned char *row=(const unsigned char *)lv_canvas_get_buf(stroke.canvas)+y*lv_draw_buf_width_to_stride(WIDTH,LV_COLOR_FORMAT_RGB565);
    return ((const uint16_t *)row)[x]==0xFFFF;
}
int main(void) {
    lv_init();lv_display_t *display=lv_display_create(480,480);CHECK(display);
    lv_timer_pause(lv_display_get_refr_timer(display));
    root=lv_obj_create(lv_screen_active());lv_obj_remove_style_all(root);
    lv_obj_set_pos(root,0,0);lv_obj_set_size(root,WIDTH,HEIGHT);
    configureFixedDrawObject(root,true);
    const size_t bytes=lv_draw_buf_width_to_stride(WIDTH,LV_COLOR_FORMAT_RGB565)*HEIGHT;
    unsigned char *allocation=malloc(bytes+32);CHECK(allocation);memset(allocation,0xA5,bytes+32);
    void *pixels=allocation+16;
    stroke.canvas=lv_canvas_create(root);configureFixedDrawObject(stroke.canvas,false);
    lv_canvas_set_buffer(stroke.canvas,pixels,WIDTH,HEIGHT,LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(stroke.canvas,WIDTH,HEIGHT);lv_obj_set_pos(stroke.canvas,0,0);
    lv_obj_add_event_cb(root,event,LV_EVENT_ALL,NULL);
    button=lv_button_create(root);lv_obj_set_size(button,76,40);lv_obj_set_pos(button,364,12);
    lv_obj_add_event_cb(button,clear_clicked,LV_EVENT_CLICKED,NULL);
    mouse=lv_indev_create();lv_indev_set_type(mouse,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(mouse,read_input);lv_indev_set_disp(mouse,display);
    lv_timer_pause(lv_indev_get_read_timer(mouse));lv_obj_update_layout(root);
    CHECK(lv_obj_get_width(stroke.canvas)==WIDTH&&lv_obj_get_height(stroke.canvas)==HEIGHT);
    CHECK(!lv_obj_has_flag(root,LV_OBJ_FLAG_SCROLLABLE));
    CHECK(!lv_obj_has_flag(stroke.canvas,LV_OBJ_FLAG_CLICKABLE));
    clearDrawCanvas(&stroke);CHECK(!stroke.active&&white(30,30));
    /* Sparse real pointer samples must draw every centerline pixel between them. */
    point(30,60,true);point(230,60,true);point(350,60,true);point(350,60,false);
    CHECK(presses==1&&releases==1&&!stroke.active);
    for(int x=30;x<=350;x++) CHECK(red(x,60));
    CHECK(lv_obj_get_child_count(root)==2);
    /* A tap is visible; a later genuine press must not bridge the white gap. */
    clearDrawCanvas(&stroke);point(30,120,true);point(30,120,false);
    CHECK(red(30,120));point(230,120,true);point(230,120,false);
    CHECK(red(230,120)&&white(130,120)&&!stroke.active);
    /* Genuine reset/lost-touch terminate the stroke, even if pointer stays down. */
    clearDrawCanvas(&stroke);point(30,180,true);
    lv_obj_send_event(root,LV_EVENT_PRESS_LOST,NULL);CHECK(!stroke.active);
    point(230,180,true);CHECK(red(230,180)&&white(130,180));point(230,180,false);
    clearDrawCanvas(&stroke);point(30,220,true);lv_indev_reset(mouse,root);
    CHECK(resets>0&&!stroke.active);point(230,220,true);point(230,220,false);
    CHECK(red(230,220)&&white(130,220));
    /* Clear terminates a still-down stroke; the next sample starts a fresh dot. */
    clearDrawCanvas(&stroke);point(30,260,true);clearDrawCanvas(&stroke);
    CHECK(!stroke.active&&white(30,260));point(230,260,true);point(230,260,false);
    CHECK(red(230,260)&&white(130,260));
    /* The real clear button remains clickable and does not draw underneath. */
    point(390,30,true);point(390,30,false);CHECK(white(230,260)&&!stroke.active);
    /* Out-of-bounds held input breaks the stroke, without clamping a bridge. */
    point(30,300,true);point(-30,300,true);CHECK(!stroke.active);
    point(230,300,true);point(230,300,false);CHECK(red(230,300)&&white(130,300));
    /* Clipped wide corner strokes cannot touch the allocation guard bytes. */
    clearDrawCanvas(&stroke);lv_point_t p={0,0};drawCanvasPoint(&stroke,&p);
    p.x=WIDTH-1;p.y=HEIGHT-1;drawCanvasPoint(&stroke,&p);breakDrawStroke(&stroke);
    CHECK(red(0,0)&&red(WIDTH-1,HEIGHT-1));
    for(int i=0;i<16;i++) CHECK(allocation[i]==0xA5&&allocation[bytes+16+i]==0xA5);
    /* Long strokes use the same buffer and object count, not one widget/sample. */
    clearDrawCanvas(&stroke);const uint32_t children=lv_obj_get_child_count(root);
    point(50,350,true);
    for(int i=0;i<2000;i++) point(50+i%250,350+(i/250)%20,true);
    point(299,357,false);
    CHECK(lv_obj_get_child_count(root)==children&&children==2&&!stroke.active);
    CHECK(lv_canvas_get_buf(stroke.canvas)==pixels);
    for(int i=0;i<16;i++) CHECK(allocation[i]==0xA5&&allocation[bytes+16+i]==0xA5);
    /* Background/close use this same stroke break; deleting canvas precedes free. */
    point(30,390,true);breakDrawStroke(&stroke);CHECK(!stroke.active);
    lv_obj_delete(stroke.canvas);stroke.canvas=NULL;free(allocation);
    clearDrawCanvas(&stroke);CHECK(!stroke.active);
    printf("PINNED_LVGL_CANVAS_PASS checks=%u samples=2000 canvas_bytes=%u presses=%u releases=%u resets=%u\n",checks,(unsigned)bytes,presses,releases,resets);
    return 0;
}
