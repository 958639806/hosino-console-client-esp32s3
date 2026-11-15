#include "lvgl.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
/* Windows TCC's bundled CRT exports double math, so desktop float wrappers
 * preserve normal math semantics for LVGL's native recognizer implementation. */
float atan2f(float y,float x) { return (float)atan2((double)y,(double)x); }
float sqrtf(float x) { return (float)sqrt((double)x); }
float cosf(float x) { return (float)cos((double)x); }
float sinf(float x) { return (float)sin((double)x); }
/* PRODUCTION_PRESENTATION_GUARDS */

static lv_indev_t *mouse;
static lv_indev_data_t input;
static lv_obj_t *root,*body,*card;
static unsigned clicked,resets,renders,scroll_ends;
static unsigned native_presses,native_releases;
static int model_identity=101,worker_identity=101,command_identity;
static bool dirty,legacy;

static void read_input(lv_indev_t *d,lv_indev_data_t *data) {(void)d;*data=input;}
static void events(lv_event_t *e) {
    if(lv_event_get_code(e)==LV_EVENT_CLICKED&&lv_event_get_target(e)==lv_event_get_current_target(e)) {++clicked;command_identity=model_identity;}
    if(lv_event_get_code(e)==LV_EVENT_INDEV_RESET) ++resets;
    if(lv_event_get_code(e)==LV_EVENT_SCROLL_END) ++scroll_ends;
}
static void indev_events(lv_event_t *e) {
    if(lv_event_get_code(e)==LV_EVENT_INDEV_RESET) ++resets;
    if(lv_event_get_code(e)==LV_EVENT_PRESSED) ++native_presses;
    if(lv_event_get_code(e)==LV_EVENT_RELEASED) ++native_releases;
}
static void render(void) {
    int y=body?lv_obj_get_scroll_y(body):0;
    lv_obj_clean(root);++renders;
    body=lv_obj_create(root);lv_obj_remove_style_all(body);lv_obj_set_size(body,400,360);lv_obj_set_pos(body,0,0);
    lv_obj_set_scroll_dir(body,LV_DIR_VER);
    for(int i=0;i<10;i++) {
        lv_obj_t *button=lv_button_create(body);lv_obj_set_pos(button,8,i*90);lv_obj_set_size(button,370,72);
        lv_obj_add_event_cb(button,events,LV_EVENT_ALL,NULL);if(!i)card=button;
    }
    lv_obj_add_event_cb(body,events,LV_EVENT_ALL,NULL);
    lv_obj_update_layout(body);lv_obj_scroll_to_y(body,y,LV_ANIM_OFF);dirty=false;
}
static void tick(void) {
    bool busy=presentationBusy(body);
    if(legacy||mayApplySnapshot(busy,false)) model_identity=worker_identity;
    if(dirty&&(legacy||mayRebuild(busy))) render();
}
static void point(int x,int y,bool down) {
    input.point.x=x;input.point.y=y;input.state=down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
    lv_tick_inc(16);lv_indev_read(mouse);
}
static void restart(bool old) {
    input.state=LV_INDEV_STATE_RELEASED;lv_indev_read(mouse);
    lv_indev_reset(mouse,NULL);lv_anim_delete_all();lv_obj_clean(root);
    body=NULL;model_identity=worker_identity=101;clicked=resets=renders=scroll_ends=0;command_identity=0;legacy=old;dirty=true;tick();
}
static void revision(void) {worker_identity=202;dirty=true;tick();}

int main(int argc,char **argv) {
    lv_init();lv_display_t *display=lv_display_create(480,480);assert(display);
    lv_timer_pause(lv_display_get_refr_timer(display));
    root=lv_obj_create(lv_screen_active());lv_obj_remove_style_all(root);lv_obj_set_size(root,420,420);lv_obj_set_pos(root,0,0);
    mouse=lv_indev_create();lv_indev_set_type(mouse,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(mouse,read_input);
    lv_indev_add_event_cb(mouse,indev_events,LV_EVENT_ALL,NULL);
    lv_indev_set_scroll_limit(mouse,10);lv_indev_set_disp(mouse,display);lv_timer_pause(lv_indev_get_read_timer(mouse));
    /* Reproduce original cancellation: async revision deletes active widget. */
    restart(true);point(80,40,true);revision();assert(resets>0 && renders==2);
    point(80,100,true);point(80,100,false);assert(clicked==0);
    /* Keep real object identity and old row mapping throughout an async update. */
    restart(false);lv_obj_t *old_card=card;point(80,40,true);revision();revision();
    assert(card==old_card&&renders==1&&model_identity==101&&resets==0);
    point(80,40,false);assert(clicked==1&&command_identity==101);
    tick();assert(renders==2&&model_identity==202);
    point(80,40,true);point(80,40,false);assert(clicked==2&&command_identity==202);
    /* Same stable mapping covers the row-index shell used for folders and CID. */
    for(int kind=0;kind<2;kind++) {
        restart(false);model_identity=worker_identity=500+kind;
        point(80,40,true);worker_identity=900+kind;dirty=true;tick();point(80,40,false);
        assert(command_identity==500+kind);tick();assert(model_identity==900+kind);
    }
    /* Real pointer drag + repeated revisions: no synthetic click/reset, keep
     * throwing inertia alive after release, then coalesce to one rebuild. */
    restart(false);point(80,250,true);revision();point(80,210,true);revision();
    point(80,160,true);revision();point(80,100,true);revision();
    assert(lv_obj_get_scroll_y(body)>0&&clicked==0&&resets==0&&renders==1);
    point(80,80,false);int scroll_before=lv_obj_get_scroll_y(body);
    if(presentationBusy(body)) {revision();assert(renders==1);}
    for(int i=0;i<500&&presentationBusy(body);i++){lv_tick_inc(10);lv_timer_handler();tick();}
    assert(!presentationBusy(body));int final_scroll=lv_obj_get_scroll_y(body);
    assert(final_scroll>=scroll_before);tick();assert(renders==2&&clicked==0);
    if(lv_obj_get_scroll_y(body)!=final_scroll) fprintf(stderr,"scroll restore expected=%d actual=%d before=%d\n",final_scroll,lv_obj_get_scroll_y(body),scroll_before);
    assert(lv_obj_get_scroll_y(body)==final_scroll);
    /* Player progress remains applicable; page replacement still waits. */
    point(80,40,true);assert(mayApplySnapshot(pointerBusy(),true)&&!mayRebuild(pointerBusy()));
    point(80,40,false);
    /* Our guard never consumes or suppresses the input device's gesture stream. */
    assert(lv_indev_get_state(mouse)==LV_INDEV_STATE_RELEASED);
    if(argc==2) {
        /* Feed the actual production touch-wrapper host run's output through
         * real LVGL. The originating raw stream contained repeated zero-point
         * chatter; the final no-IRQ deadline produced a single true release. */
        restart(false);native_presses=native_releases=0;
        FILE *file=fopen(argv[1],"r");assert(file);long long time,previous=-1;int state,x,y,n=0;
        while(fscanf(file,"%lld %d %d %d",&time,&state,&x,&y)==4) {
            if(previous>=0)lv_tick_inc((uint32_t)((time-previous)/1000));
            previous=time;input.point.x=x;input.point.y=y;input.state=state;
            lv_indev_read(mouse);++n;
        }
        fclose(file);assert(n>=10&&native_presses==1&&native_releases==1&&clicked==0);
        assert(lv_indev_get_state(mouse)==LV_INDEV_STATE_RELEASED);
        puts("PRODUCTION_TOUCH_FILTER_TRACE_TO_PINNED_LVGL_SINGLE_DRAG_SINGLE_RELEASE_PASS");
    }
    puts("PINNED_LVGL_POINTER_DRAG_ASYNC_IDENTITY_INERTIA_RELEASE_AND_PLAYER_PROGRESS_PASS");
    return 0;
}
