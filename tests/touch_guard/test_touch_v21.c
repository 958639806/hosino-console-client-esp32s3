#include "stub.h"
#define CHECK(c) do { ++checks; if(!(c)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);exit(1);} }while(0)
static int checks,allocs,frees,get_calls,log_calls,diag_log_calls,tx_calls,rx_calls,delay_calls;
int critical_depth;
static int io_calls,register_calls,unregister_calls,lock_calls,unlock_calls,threshold_calls;
static bool fail_alloc,fail_register,reenter_unregister;
static int fail_tx,rx_result,get_result,unregister_result,lock_result;
static int64_t now,read_duration=1000;
static uint8_t wire[15];
static esp_lcd_panel_io_i2c_config_t io_seen;
static esp_lv_adapter_touch_config_t cfg_seen[32];
static lv_indev_t devices[32];
static bool native_mode;
static unsigned notify_calls;
static uint8_t adapter_count;
static uint32_t last_log_reads,last_log_errors,last_log_slow,last_log_read_max,last_log_two,last_log_max;
static uint32_t last_diag[24];
#include "hosino_touch_override.c"

void *heap_caps_calloc(size_t n,size_t size,unsigned caps){
 CHECK(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));CHECK(critical_depth==0);
 if(fail_alloc)return NULL;++allocs;return calloc(n,size);
}
void heap_caps_free(void*p){CHECK(critical_depth==0);if(p){++frees;free(p);}}
int64_t esp_timer_get_time(void){return now;}
void vTaskDelay(unsigned ticks){CHECK(ticks==2);++delay_calls;now+=ticks*1000;}
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io,int cmd,const void*data,size_t len){
 ++tx_calls;CHECK(cmd==0xd0);CHECK(data!=NULL);CHECK(critical_depth==0);
 const uint8_t*p=data;
 if(tx_calls==1){CHECK(len==1&&p[0]==0);}
 else {CHECK(tx_calls==2);CHECK(len==2&&p[0]==0&&p[1]==0xab);}
 return fail_tx==tx_calls?ESP_FAIL:ESP_OK;
}
esp_err_t esp_lcd_panel_io_rx_param(esp_lcd_panel_io_handle_t io,int cmd,void*data,size_t len){
 ++rx_calls;CHECK(cmd==-1&&len==15);CHECK(tx_calls==1);CHECK(critical_depth==0);
 now+=read_duration;memcpy(data,wire,15);return rx_result;
}
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t p,esp_lcd_touch_point_data_t*pts,uint8_t*n,uint8_t max){
 ++get_calls;*n=p->data.points<max?p->data.points:max;
 for(unsigned i=0;i<*n;i++){
  uint16_t x=p->data.coords[i].x,y=p->data.coords[i].y;
  if(p->config.flags.mirror_x)x=p->config.x_max-x;
  if(p->config.flags.mirror_y)y=p->config.y_max-y;
  if(p->config.flags.swap_xy){uint16_t t=x;x=y;y=t;}
  pts[i].x=x;pts[i].y=y;pts[i].track_id=99;pts[i].strength=1;
 }
 return get_result;
}
esp_err_t esp_lv_adapter_lock(int timeout){CHECK(timeout==-1);++lock_calls;return lock_result;}
void esp_lv_adapter_unlock(void){++unlock_calls;}
lv_timer_t*lv_indev_get_read_timer(lv_indev_t*p){return &p->timer;}
void lv_timer_set_period(lv_timer_t*p,uint32_t n){p->period=n;}
static void fake_adapter_read(lv_indev_t *dev,lv_indev_data_t *out){
 if(dev->pending){
  dev->pending=false;esp_lv_adapter_touch_config_t *cfg=&cfg_seen[dev->id];
  esp_lcd_touch_point_data_t points[2];uint8_t count=0;
  esp_err_t ret=cfg->callbacks.custom_touch_read(cfg->handle,points,&count,2,cfg->callbacks.user_ctx);
  adapter_count=count;dev->last.state=ret==ESP_OK&&count?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
  if(count){dev->last.point.x=points[0].x;dev->last.point.y=points[0].y;}
 }
 *out=dev->last;
}
lv_indev_read_cb_t lv_indev_get_read_cb(lv_indev_t*p){(void)p;return native_mode?fake_adapter_read:NULL;}
void lv_indev_set_read_cb(lv_indev_t*p,lv_indev_read_cb_t cb){p->read_cb=cb;}
bool esp_lv_adapter_touch_notify_interrupt(lv_indev_t*p){++notify_calls;bool was=p->pending;p->pending=true;return !was;}
void lv_indev_set_pinch_up_threshold(lv_indev_t*p,float f){CHECK(f>1.14f&&f<1.16f);++threshold_calls;}
void lv_indev_set_pinch_down_threshold(lv_indev_t*p,float f){CHECK(f>0.84f&&f<0.86f);++threshold_calls;}
void fake_log(const char*tag,const char*fmt,...){
 CHECK(critical_depth==0);
 if(strstr(fmt,"raw0=")) {
  ++diag_log_calls;va_list a;va_start(a,fmt);
  for(unsigned i=0;i<24;i++)last_diag[i]=va_arg(a,unsigned);
  va_end(a);return;
 }
 if(!strstr(fmt,"read_n="))return;
 ++log_calls;va_list a;va_start(a,fmt);
 last_log_reads=va_arg(a,unsigned);last_log_errors=va_arg(a,unsigned);last_log_slow=va_arg(a,unsigned);last_log_read_max=va_arg(a,unsigned);
 last_log_two=va_arg(a,unsigned);last_log_max=va_arg(a,unsigned);va_end(a);
}
esp_err_t __real_esp_lcd_new_panel_io_i2c_v2(i2c_master_bus_handle_t bus,const esp_lcd_panel_io_i2c_config_t*cfg,esp_lcd_panel_io_handle_t*out){++io_calls;if(cfg)io_seen=*cfg;return 17;}
lv_indev_t *__real_esp_lv_adapter_register_touch(const esp_lv_adapter_touch_config_t*cfg){
 ++register_calls;if(fail_register||!cfg)return NULL;
 int i=register_calls-1;cfg_seen[i]=*cfg;devices[i].id=i;devices[i].timer.period=15;devices[i].read_cb=native_mode?fake_adapter_read:NULL;return &devices[i];
}
esp_err_t __real_esp_lv_adapter_unregister_touch(lv_indev_t*p){
 ++unregister_calls;if(reenter_unregister){reenter_unregister=false;CHECK(__wrap_esp_lv_adapter_unregister_touch(p)==ESP_ERR_INVALID_STATE);}return unregister_result;
}
static void original_irq(esp_lcd_touch_handle_t p,void*ctx){}
static esp_err_t original_read(esp_lcd_touch_handle_t p,esp_lcd_touch_point_data_t*q,uint8_t*n,uint8_t max,void*ctx){return 31;}
static void empty(void){memset(wire,0,sizeof(wire));wire[6]=0xab;}
static void put(unsigned index,unsigned id,unsigned status,unsigned x,unsigned y){
 unsigned o=index?7:0;wire[o]=(id<<4)|status;wire[o+1]=x>>4;wire[o+2]=y>>4;wire[o+3]=((x&15)<<4)|(y&15);
}
static esp_err_t read_cfg(esp_lv_adapter_touch_config_t*c,esp_lcd_touch_point_data_t*q,uint8_t*n){
 tx_calls=rx_calls=delay_calls=0;return c->callbacks.custom_touch_read(c->handle,q,n,2,c->callbacks.user_ctx);
}
static void up(esp_lv_adapter_touch_config_t*c){esp_lcd_touch_point_data_t p[2];uint8_t n=2;empty();CHECK(read_cfg(c,p,&n)==ESP_OK);CHECK(n==0);}
int main(void){
 /* Telemetry preserves distinct valid empty reports, errors and pointer
  * transitions. State must continue across five-second counter windows. */
 touch_stats_t ds={0};touch_diag_state_t dt={0};touch_read_diag_t dd={.raw_valid=true};
 touch_diag_note(&ds,&dt,&dd,1000);
 dd.raw_count=1;dd.reported_pressed=dd.primary_valid=true;
 touch_diag_note(&ds,&dt,&dd,2000);
 touch_diag_note(&ds,&dt,&dd,62000);
 CHECK(ds.raw_zero_to_nonzero==1&&ds.reported_down==1&&ds.pressed_report_gaps==1&&ds.pressed_report_gap_max_us==60000);
 dd.raw_count=0;dd.reported_pressed=dd.primary_valid=false;
 touch_diag_note(&ds,&dt,&dd,63000);
 CHECK(ds.raw_nonzero_to_zero==1&&ds.reported_up==1&&ds.zero_after_pressed==1&&ds.error_after_pressed==0);
 dd.raw_count=1;dd.reported_pressed=dd.primary_valid=true;touch_diag_note(&ds,&dt,&dd,64000);
 memset(&ds,0,sizeof(ds));touch_diag_note(&ds,&dt,&dd,65000);CHECK(ds.reported_down==0);
 dd.raw_valid=false;dd.raw_count=0;dd.reported_pressed=dd.primary_valid=false;dd.blocked=true;dd.fault=TOUCH_FAULT_ACK;
 touch_diag_note(&ds,&dt,&dd,66000);
 CHECK(ds.reported_up==1&&ds.error_after_pressed==1&&ds.zero_after_pressed==0&&ds.raw_nonzero_to_zero==0&&ds.faults[TOUCH_FAULT_ACK]==1&&ds.block_enters==1);
 esp_lcd_panel_io_i2c_config_t io={.dev_addr=0x34,.transaction_timeout_ms=0,.scl_speed_hz=400000,.flags=23,.user_ctx=(void*)7};
 CHECK(__wrap_esp_lcd_new_panel_io_i2c_v2(NULL,&io,NULL)==17);CHECK(io_seen.transaction_timeout_ms==0);
 io.dev_addr=0x5a;CHECK(__wrap_esp_lcd_new_panel_io_i2c_v2(NULL,&io,NULL)==17);CHECK(io_seen.transaction_timeout_ms==20);CHECK(io.transaction_timeout_ms==0);CHECK(io_seen.scl_speed_hz==400000&&io_seen.flags==23&&io_seen.user_ctx==(void*)7);
 io.transaction_timeout_ms=-1;__wrap_esp_lcd_new_panel_io_i2c_v2(NULL,&io,NULL);CHECK(io_seen.transaction_timeout_ms==20);
 io.transaction_timeout_ms=100;__wrap_esp_lcd_new_panel_io_i2c_v2(NULL,&io,NULL);CHECK(io_seen.transaction_timeout_ms==20);
 io.transaction_timeout_ms=5;__wrap_esp_lcd_new_panel_io_i2c_v2(NULL,&io,NULL);CHECK(io_seen.transaction_timeout_ms==5);
 CHECK(__wrap_esp_lcd_new_panel_io_i2c_v2(NULL,NULL,NULL)==17);
 struct esp_lcd_touch_s hw={.config={.int_gpio_num=11,.x_max=480,.y_max=480,.flags={.mirror_y=1,.swap_xy=1}}},other={.config={.int_gpio_num=12}};
 int ctx=99;
 esp_lv_adapter_touch_config_t cfg={.disp=(void*)5,.handle=&hw,.scale={1.0f,2.0f},.multi_touch={0,1},.callbacks={NULL,NULL,&ctx}},copy=cfg;
 fail_alloc=true;int r=register_calls;CHECK(__wrap_esp_lv_adapter_register_touch(&cfg)==NULL);CHECK(register_calls==r);fail_alloc=false;
 fail_register=true;CHECK(__wrap_esp_lv_adapter_register_touch(&cfg)==NULL);CHECK(allocs==frees);fail_register=false;
 lv_indev_t*one=__wrap_esp_lv_adapter_register_touch(&cfg);CHECK(one!=NULL);int a=register_calls-1;
 CHECK(!memcmp(&cfg,&copy,sizeof(cfg)));CHECK(cfg_seen[a].callbacks.user_ctx!=&ctx);CHECK(cfg_seen[a].multi_touch.mode==0&&cfg_seen[a].multi_touch.pointers==1);CHECK(cfg_seen[a].scale.y==2.0f&&cfg_seen[a].disp==(void*)5);CHECK(cfg_seen[a].callbacks.on_interrupt==observe_touch_irq);
 touch_guard_t*g=cfg_seen[a].callbacks.user_ctx;CHECK(g->indev==one);CHECK(one->timer.period==8);CHECK(lock_calls==1&&unlock_calls==1&&threshold_calls==2);
 esp_lcd_touch_point_data_t p[2]={{.x=999},{.x=999}};uint8_t n=2;
 empty();wire[5]=1;put(0,0,6,100,200);CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_OK);CHECK(tx_calls==2&&rx_calls==1&&delay_calls==1);CHECK(n==1&&p[0].x==280&&p[0].y==100&&p[0].track_id==0&&p[1].x==0);CHECK(log_calls==0&&diag_log_calls==0);CHECK(g->stats.raw_nonzero==1&&g->stats.reported_down==1&&g->stats.reported_up==0);
 empty();wire[5]=2;put(0,0,6,110,210);put(1,1,6,300,400);CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_OK);CHECK(n==2&&p[0].x==270&&p[1].x==80&&p[0].track_id==0&&p[1].track_id==1);CHECK(g->stats.two_point_reads==1&&g->stats.max_points==2);
 empty();wire[5]=2;put(0,1,6,301,401);put(1,0,6,111,211);CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_OK);CHECK(n==2&&p[0].track_id==0&&p[0].y==111&&p[1].track_id==1);CHECK(g->primary.primary_id==0);
 empty();wire[5]=1;put(0,1,6,302,402);CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_OK);CHECK(n==0&&g->primary.block_until_all_up&&hw.data.points==0);CHECK(g->stats.primary_missing==1&&g->stats.reported_up==1&&g->stats.block_enters==1&&g->stats.primary_id_changes==0&&g->stats.raw_id_changes==1);
 CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_OK);CHECK(n==0);up(&cfg_seen[a]);CHECK(!g->primary.block_until_all_up);
 empty();wire[5]=1;put(0,1,6,200,201);CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_OK);CHECK(n==1&&p[0].track_id==1);up(&cfg_seen[a]);
 empty();wire[5]=2;put(0,1,0,30,40);put(1,0,6,130,140);CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_OK);CHECK(n==1&&p[0].track_id==0&&p[0].y==130);up(&cfg_seen[a]);
 empty();wire[5]=1;put(0,0,6,100,101);CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_OK);wire[6]=0;int gc=get_calls;CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_ERR_INVALID_RESPONSE);CHECK(tx_calls==2&&get_calls==gc&&n==0&&p[0].x==0&&hw.data.points==0&&g->primary.block_until_all_up);up(&cfg_seen[a]);
 empty();wire[5]=3;CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_ERR_INVALID_RESPONSE);CHECK(n==0);empty();wire[5]=2;put(0,0,6,100,100);put(1,0,6,200,200);CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_ERR_INVALID_RESPONSE);CHECK(n==0);
 empty();wire[5]=1;put(0,2,6,100,100);CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_ERR_INVALID_RESPONSE);CHECK(n==0);put(0,0,6,481,100);CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_ERR_INVALID_RESPONSE);CHECK(n==0);
 put(0,0,6,100,100);fail_tx=1;gc=get_calls;CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_FAIL);CHECK(tx_calls==1&&rx_calls==0&&delay_calls==0&&get_calls==gc&&n==0);fail_tx=0;
 rx_result=ESP_FAIL;CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_FAIL);CHECK(tx_calls==1&&rx_calls==1&&get_calls==gc&&n==0);rx_result=ESP_OK;
 fail_tx=2;CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_FAIL);CHECK(tx_calls==2&&get_calls==gc&&n==0);fail_tx=0;
 get_result=ESP_FAIL;CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_FAIL);CHECK(n==0&&p[0].x==0&&hw.data.points==0);get_result=ESP_OK;up(&cfg_seen[a]);
 empty();wire[5]=1;put(0,0,6,0,0);CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_OK);CHECK(n==1&&p[0].x==480&&g->stats.exact_edge_output==1&&g->stats.offscreen_output==1);up(&cfg_seen[a]);
 CHECK(g->stats.faults[TOUCH_FAULT_ADDRESS]==1&&g->stats.faults[TOUCH_FAULT_READ]==1&&g->stats.faults[TOUCH_FAULT_ACK]==1&&g->stats.faults[TOUCH_FAULT_MARKER]==1&&g->stats.faults[TOUCH_FAULT_FORMAT]==3&&g->stats.faults[TOUCH_FAULT_BOUNDS]==1&&g->stats.faults[TOUCH_FAULT_OUTPUT]==1);
 read_duration=12000;now=5000000;CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_OK);CHECK(log_calls==1&&diag_log_calls==1&&last_log_reads>10&&last_log_errors>=8&&last_log_slow==1&&last_log_read_max==14000&&last_log_two>=2&&last_log_max==2);CHECK(g->stats.reads==0&&g->stats.raw_nonzero==0&&last_diag[0]>0&&last_diag[1]>0);CHECK(last_diag[18]==1&&last_diag[19]==3&&last_diag[20]==1&&last_diag[21]==1&&last_diag[22]==1&&last_diag[23]==1);CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_OK);CHECK(log_calls==1&&diag_log_calls==1);
 n=8;CHECK(touch_read(&other,p,&n,2,g)==ESP_ERR_INVALID_ARG);CHECK(n==0);CHECK(touch_read(&hw,NULL,&n,2,g)==ESP_ERR_INVALID_ARG);CHECK(touch_read(&hw,p,&n,0,g)==ESP_ERR_INVALID_ARG);
 lock_result=ESP_FAIL;lv_indev_t*two=__wrap_esp_lv_adapter_register_touch(&cfg);int b=register_calls-1;CHECK(two!=one&&two->timer.period==15);CHECK(cfg_seen[b].callbacks.user_ctx!=g);lock_result=ESP_OK;
 touch_guard_t*g2=cfg_seen[b].callbacks.user_ctx;CHECK(!g2->primary.primary_valid&&!g2->primary.block_until_all_up);
 empty();wire[5]=1;put(0,0,6,150,150);fail_tx=1;CHECK(read_cfg(&cfg_seen[b],p,&n)==ESP_FAIL);CHECK(n==0&&g2->primary.block_until_all_up);fail_tx=0;
 CHECK(read_cfg(&cfg_seen[b],p,&n)==ESP_OK);CHECK(n==0&&!g2->primary.primary_valid&&g2->primary.block_until_all_up);
 up(&cfg_seen[b]);CHECK(!g2->primary.block_until_all_up);empty();wire[5]=1;put(0,1,6,150,150);CHECK(read_cfg(&cfg_seen[b],p,&n)==ESP_OK);CHECK(n==1&&p[0].track_id==1);up(&cfg_seen[b]);
 cfg.callbacks.custom_touch_read=original_read;r=allocs;lv_indev_t*custom=__wrap_esp_lv_adapter_register_touch(&cfg);CHECK(allocs==r&&cfg_seen[register_calls-1].callbacks.custom_touch_read==original_read&&cfg_seen[register_calls-1].callbacks.user_ctx==&ctx);
 cfg.callbacks.custom_touch_read=NULL;cfg.callbacks.on_interrupt=original_irq;r=allocs;lv_indev_t*irqdev=__wrap_esp_lv_adapter_register_touch(&cfg);CHECK(allocs==r&&cfg_seen[register_calls-1].callbacks.on_interrupt==original_irq&&cfg_seen[register_calls-1].callbacks.user_ctx==&ctx&&cfg_seen[register_calls-1].callbacks.custom_touch_read==NULL);
 cfg.callbacks.on_interrupt=NULL;cfg.multi_touch.mode=1;r=allocs;lv_indev_t*multi=__wrap_esp_lv_adapter_register_touch(&cfg);CHECK(allocs==r);cfg.multi_touch.mode=0;cfg.handle=&other;r=allocs;lv_indev_t*otherdev=__wrap_esp_lv_adapter_register_touch(&cfg);CHECK(allocs==r);
 unregister_result=ESP_FAIL;int fr=frees;CHECK(__wrap_esp_lv_adapter_unregister_touch(one)==ESP_FAIL);CHECK(frees==fr&&!g->unregistering);CHECK(read_cfg(&cfg_seen[a],p,&n)==ESP_OK);
 unregister_result=ESP_OK;reenter_unregister=true;r=unregister_calls;CHECK(__wrap_esp_lv_adapter_unregister_touch(one)==ESP_OK);CHECK(unregister_calls==r+1&&frees==fr+1);CHECK(__wrap_esp_lv_adapter_unregister_touch(two)==ESP_OK);CHECK(s_guards==NULL&&allocs==frees);
 CHECK(__wrap_esp_lv_adapter_unregister_touch(custom)==ESP_OK);CHECK(__wrap_esp_lv_adapter_unregister_touch(irqdev)==ESP_OK);CHECK(__wrap_esp_lv_adapter_unregister_touch(multi)==ESP_OK);CHECK(__wrap_esp_lv_adapter_unregister_touch(otherdev)==ESP_OK);CHECK(critical_depth==0);
 hosino_touch_packet_t parsed;CHECK(!hosino_touch_parse_packet(NULL,&parsed));CHECK(!hosino_touch_parse_packet(wire,NULL));
 printf("PASS two-contact touch: %d assertions; wire15+ACK, compact IDs, reorder/primary release barrier, invalid reports/IO failures, transforms, 8ms input timer, callback and lifecycle isolation\n",checks);
 return 0;
}
