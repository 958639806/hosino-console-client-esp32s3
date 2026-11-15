#include "stub.h"
#define CHECK(c) do { ++checks; if(!(c)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);exit(1);} }while(0)
static int checks,allocs,frees,critical_depth,read_calls,get_calls,log_calls;
static int io_calls,register_calls,unregister_calls;
static bool fail_alloc,fail_register,reenter_unregister;
static int read_result,get_result,unregister_result,report_count=1;
static int64_t now,read_duration=2000;
static esp_lcd_panel_io_i2c_config_t io_seen;
static esp_lv_adapter_touch_config_t cfg_seen[12];
static lv_indev_t devices[12];
static uint32_t last_log_reads,last_log_errors,last_log_slow,last_log_read_max;
#include "hosino_touch_override.c"

void *heap_caps_calloc(size_t n,size_t size,unsigned caps){
 CHECK(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));CHECK(critical_depth==0);
 if(fail_alloc)return NULL;++allocs;return calloc(n,size);
}
void heap_caps_free(void*p){CHECK(critical_depth==0);if(p){++frees;free(p);}}
int64_t esp_timer_get_time(void){return now;}
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t p){++read_calls;now+=read_duration;return read_result;}
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t p,esp_lcd_touch_point_data_t*pts,uint8_t*n,uint8_t max){
 ++get_calls;*n=report_count;if(max){pts[0].x=100;pts[0].y=200;pts[0].track_id=3;}return get_result;
}
void fake_log(const char*tag,const char*fmt,...){
 CHECK(critical_depth==0);
 if(!strstr(fmt,"read_n="))return;
 ++log_calls;va_list a;va_start(a,fmt);
 last_log_reads=va_arg(a,unsigned);last_log_errors=va_arg(a,unsigned);last_log_slow=va_arg(a,unsigned);last_log_read_max=va_arg(a,unsigned);
 va_end(a);
}
esp_err_t __real_esp_lcd_new_panel_io_i2c_v2(i2c_master_bus_handle_t bus,const esp_lcd_panel_io_i2c_config_t*cfg,esp_lcd_panel_io_handle_t*out){
 ++io_calls;if(cfg)io_seen=*cfg;return 17;
}
lv_indev_t *__real_esp_lv_adapter_register_touch(const esp_lv_adapter_touch_config_t*cfg){
 ++register_calls;if(fail_register||!cfg)return NULL;
 int i=register_calls-1;cfg_seen[i]=*cfg;devices[i].id=i;return &devices[i];
}
esp_err_t __real_esp_lv_adapter_unregister_touch(lv_indev_t*p){
 ++unregister_calls;if(reenter_unregister){reenter_unregister=false;CHECK(__wrap_esp_lv_adapter_unregister_touch(p)==ESP_ERR_INVALID_STATE);}return unregister_result;
}
static void original_irq(esp_lcd_touch_handle_t p,void*ctx){
 CHECK(critical_depth==0);
}
static esp_err_t original_read(esp_lcd_touch_handle_t p,esp_lcd_touch_point_data_t*q,uint8_t*n,uint8_t max,void*ctx){return 31;}
static esp_err_t read_cfg(esp_lv_adapter_touch_config_t*c,esp_lcd_touch_point_data_t*q,uint8_t*n){return c->callbacks.custom_touch_read(c->handle,q,n,2,c->callbacks.user_ctx);}
int main(void){
 esp_lcd_panel_io_i2c_config_t io={.dev_addr=0x34,.transaction_timeout_ms=0,.scl_speed_hz=400000,.flags=23,.user_ctx=(void*)7};
 CHECK(__wrap_esp_lcd_new_panel_io_i2c_v2(NULL,&io,NULL)==17);CHECK(io_seen.transaction_timeout_ms==0);
 io.dev_addr=0x5a;CHECK(__wrap_esp_lcd_new_panel_io_i2c_v2(NULL,&io,NULL)==17);CHECK(io_seen.transaction_timeout_ms==20);CHECK(io.transaction_timeout_ms==0);CHECK(io_seen.scl_speed_hz==400000&&io_seen.flags==23&&io_seen.user_ctx==(void*)7);
 io.transaction_timeout_ms=-1;__wrap_esp_lcd_new_panel_io_i2c_v2(NULL,&io,NULL);CHECK(io_seen.transaction_timeout_ms==20);
 io.transaction_timeout_ms=100;__wrap_esp_lcd_new_panel_io_i2c_v2(NULL,&io,NULL);CHECK(io_seen.transaction_timeout_ms==20);
 io.transaction_timeout_ms=5;__wrap_esp_lcd_new_panel_io_i2c_v2(NULL,&io,NULL);CHECK(io_seen.transaction_timeout_ms==5);
 CHECK(__wrap_esp_lcd_new_panel_io_i2c_v2(NULL,NULL,NULL)==17);
 struct esp_lcd_touch_s hw={.config={11}},other={.config={12}};
 int ctx=99;
 esp_lv_adapter_touch_config_t cfg={.disp=(void*)5,.handle=&hw,.scale={1.0f,2.0f},.multi_touch={0,1},.callbacks={NULL,NULL,&ctx}};
 esp_lv_adapter_touch_config_t copy=cfg;
 fail_alloc=true;int r=register_calls;CHECK(__wrap_esp_lv_adapter_register_touch(&cfg)==NULL);CHECK(register_calls==r);fail_alloc=false;
 fail_register=true;CHECK(__wrap_esp_lv_adapter_register_touch(&cfg)==NULL);CHECK(allocs==frees);fail_register=false;
 lv_indev_t*one=__wrap_esp_lv_adapter_register_touch(&cfg);CHECK(one!=NULL);int a=register_calls-1;CHECK(!memcmp(&cfg,&copy,sizeof(cfg)));CHECK(cfg_seen[a].callbacks.user_ctx!=&ctx);CHECK(cfg_seen[a].multi_touch.mode==0&&cfg_seen[a].multi_touch.pointers==1);CHECK(cfg_seen[a].scale.y==2.0f&&cfg_seen[a].disp==(void*)5);
 touch_guard_t*g=cfg_seen[a].callbacks.user_ctx;CHECK(g->indev==one);
 CHECK(cfg_seen[a].callbacks.on_interrupt==NULL);
 esp_lcd_touch_point_data_t points[2]={{999,999,1,9},{999,999,1,9}};uint8_t n=2;
 now=4000;CHECK(read_cfg(&cfg_seen[a],points,&n)==ESP_OK);CHECK(n==1&&points[0].x==100&&points[1].x==0);CHECK(log_calls==0);
 read_result=ESP_FAIL;int gc=get_calls;n=2;points[0].x=999;CHECK(read_cfg(&cfg_seen[a],points,&n)==ESP_FAIL);CHECK(n==0&&points[0].x==0&&points[1].x==0);CHECK(get_calls==gc);CHECK(g->stats.read_errors==1);
 read_result=ESP_OK;get_result=ESP_FAIL;n=2;CHECK(read_cfg(&cfg_seen[a],points,&n)==ESP_FAIL);CHECK(n==0&&points[0].x==0);CHECK(g->stats.read_errors==2);
 get_result=ESP_OK;report_count=0;CHECK(read_cfg(&cfg_seen[a],points,&n)==ESP_OK);CHECK(n==0);report_count=1;
 read_duration=12000;now=5000000;CHECK(read_cfg(&cfg_seen[a],points,&n)==ESP_OK);CHECK(log_calls==1);CHECK(last_log_reads==5&&last_log_errors==2&&last_log_slow==1&&last_log_read_max==12000);CHECK(g->stats.reads==0);
 CHECK(read_cfg(&cfg_seen[a],points,&n)==ESP_OK);CHECK(log_calls==1);
 n=8;CHECK(touch_read(&other,points,&n,2,g)==ESP_ERR_INVALID_ARG);CHECK(n==0);
 lv_indev_t*two=__wrap_esp_lv_adapter_register_touch(&cfg);int b=register_calls-1;CHECK(two!=one);CHECK(cfg_seen[b].callbacks.user_ctx!=g);CHECK(s_guards!=NULL);
 cfg.callbacks.custom_touch_read=original_read;r=allocs;lv_indev_t*custom=__wrap_esp_lv_adapter_register_touch(&cfg);CHECK(allocs==r);CHECK(cfg_seen[register_calls-1].callbacks.custom_touch_read==original_read);CHECK(cfg_seen[register_calls-1].callbacks.user_ctx==&ctx);
 cfg.callbacks.custom_touch_read=NULL;cfg.callbacks.on_interrupt=original_irq;r=allocs;lv_indev_t*irqdev=__wrap_esp_lv_adapter_register_touch(&cfg);CHECK(allocs==r);CHECK(cfg_seen[register_calls-1].callbacks.on_interrupt==original_irq);CHECK(cfg_seen[register_calls-1].callbacks.user_ctx==&ctx);CHECK(cfg_seen[register_calls-1].callbacks.custom_touch_read==NULL);
 cfg.callbacks.on_interrupt=NULL;cfg.handle=&other;r=allocs;lv_indev_t*otherdev=__wrap_esp_lv_adapter_register_touch(&cfg);CHECK(allocs==r);CHECK(cfg_seen[register_calls-1].callbacks.on_interrupt==NULL);
 unregister_result=ESP_FAIL;int fr=frees;CHECK(__wrap_esp_lv_adapter_unregister_touch(one)==ESP_FAIL);CHECK(frees==fr);CHECK(g->unregistering==false);CHECK(read_cfg(&cfg_seen[a],points,&n)==ESP_OK);
 unregister_result=ESP_OK;reenter_unregister=true;r=unregister_calls;CHECK(__wrap_esp_lv_adapter_unregister_touch(one)==ESP_OK);CHECK(unregister_calls==r+1);CHECK(frees==fr+1);
 CHECK(__wrap_esp_lv_adapter_unregister_touch(two)==ESP_OK);CHECK(s_guards==NULL);CHECK(allocs==frees);
 CHECK(__wrap_esp_lv_adapter_unregister_touch(custom)==ESP_OK);CHECK(__wrap_esp_lv_adapter_unregister_touch(irqdev)==ESP_OK);CHECK(__wrap_esp_lv_adapter_unregister_touch(otherdev)==ESP_OK);CHECK(__wrap_esp_lv_adapter_unregister_touch(NULL)==ESP_OK);CHECK(critical_depth==0);
 printf("PASS touch guard: %d assertions; bounded address-specific IO, error/no-stale-data, unchanged IRQ path and callback passthrough, log rate, isolated registrations and lifecycle failures\n",checks);
 return 0;
}
