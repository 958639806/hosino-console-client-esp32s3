"""Run the complete production two-contact wrapper with deterministic IDF stubs.
No hardware, serial, network, driver edits or staged-project changes occur.
"""
from pathlib import Path
import hashlib,json,subprocess,tempfile

ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'overlay-apps/main/hosino_touch_override.c'
TCC=ROOT/'tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe'
STUB=r'''
#ifndef TOUCH_TEST_STUB_H
#define TOUCH_TEST_STUB_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 258
#define ESP_ERR_INVALID_STATE 259
#define ESP_ERR_INVALID_RESPONSE 264
#define CONFIG_ESP_LCD_TOUCH_MAX_POINTS 5
#define _Static_assert(c,m) typedef char touch_static_assertion[(c)?1:-1]
#define LV_USE_GESTURE_RECOGNITION 1
#define ESP_LV_ADAPTER_TOUCH_MODE_SINGLE 0
#define pdMS_TO_TICKS(x) (x)
#define IRAM_ATTR
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
extern int critical_depth;
#define portENTER_CRITICAL(x) (++critical_depth)
#define portEXIT_CRITICAL(x) (--critical_depth)
#define portENTER_CRITICAL_ISR(x) (++critical_depth)
#define portEXIT_CRITICAL_ISR(x) (--critical_depth)
typedef void *i2c_master_bus_handle_t;
typedef void *esp_lcd_panel_io_handle_t;
typedef struct {unsigned dev_addr;int transaction_timeout_ms;unsigned scl_speed_hz;void *user_ctx;unsigned flags;} esp_lcd_panel_io_i2c_config_t;
typedef struct {uint8_t track_id;uint16_t x,y,strength;} esp_lcd_touch_point_data_t;
typedef struct esp_lcd_touch_s {struct {int int_gpio_num;uint16_t x_max,y_max;struct{unsigned mirror_x,mirror_y,swap_xy;}flags;} config;struct{uint8_t points;esp_lcd_touch_point_data_t coords[5];portMUX_TYPE lock;}data;esp_lcd_panel_io_handle_t io;} *esp_lcd_touch_handle_t;
typedef struct {uint32_t period;} lv_timer_t;
enum {LV_INDEV_STATE_RELEASED=0,LV_INDEV_STATE_PRESSED=1};
typedef struct {int state;struct {int x,y;} point;} lv_indev_data_t;
typedef struct lv_indev_t lv_indev_t;
typedef void (*lv_indev_read_cb_t)(lv_indev_t*,lv_indev_data_t*);
struct lv_indev_t {int id;lv_timer_t timer;lv_indev_read_cb_t read_cb;bool pending;lv_indev_data_t last;};
typedef struct {
 void (*on_interrupt)(esp_lcd_touch_handle_t,void*);
 esp_err_t (*custom_touch_read)(esp_lcd_touch_handle_t,esp_lcd_touch_point_data_t*,uint8_t*,uint8_t,void*);
 void *user_ctx;
} esp_lv_adapter_touch_callbacks_t;
typedef struct {void *disp;esp_lcd_touch_handle_t handle;struct{float x,y;}scale;struct{int mode;uint8_t pointers;}multi_touch;esp_lv_adapter_touch_callbacks_t callbacks;} esp_lv_adapter_touch_config_t;
void *heap_caps_calloc(size_t,size_t,unsigned);
void heap_caps_free(void*);
int64_t esp_timer_get_time(void);
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t,esp_lcd_touch_point_data_t*,uint8_t*,uint8_t);
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t,int,const void*,size_t);
esp_err_t esp_lcd_panel_io_rx_param(esp_lcd_panel_io_handle_t,int,void*,size_t);
void vTaskDelay(unsigned);
esp_err_t esp_lv_adapter_lock(int);
void esp_lv_adapter_unlock(void);
lv_timer_t *lv_indev_get_read_timer(lv_indev_t*);
void lv_timer_set_period(lv_timer_t*,uint32_t);
lv_indev_read_cb_t lv_indev_get_read_cb(lv_indev_t*);
void lv_indev_set_read_cb(lv_indev_t*,lv_indev_read_cb_t);
bool esp_lv_adapter_touch_notify_interrupt(lv_indev_t*);
void lv_indev_set_pinch_up_threshold(lv_indev_t*,float);
void lv_indev_set_pinch_down_threshold(lv_indev_t*,float);
void fake_log(const char*,const char*,...);
#define ESP_LOGI fake_log
#define ESP_LOGE fake_log
#define ESP_LOGW fake_log
#endif
'''

def main():
 with tempfile.TemporaryDirectory(prefix='touch-v21-',dir='C:/esp/hosino-qspi') as d:
  work=Path(d)
  (work/'stub.h').write_text(STUB,encoding='utf-8')
  headers=['freertos/FreeRTOS.h','freertos/task.h','esp_attr.h','esp_heap_caps.h','esp_lcd_io_i2c.h','esp_lcd_panel_io.h','esp_lcd_touch.h','esp_lv_adapter.h','esp_lv_adapter_input.h','esp_log.h','esp_timer.h']
  for name in headers:
   p=work/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#include "stub.h"\n',encoding='utf-8')
  (work/'hosino_touch_override.c').write_bytes(SOURCE.read_bytes())
  (work/'hosino_touch_packet.h').write_bytes(SOURCE.with_name('hosino_touch_packet.h').read_bytes())
  (work/'test_touch.c').write_bytes(Path(__file__).with_name('test_touch_v21.c').read_bytes())
  command=[str(TCC),'-I'+str(work),str(work/'test_touch.c'),'-o',str(work/'test_touch.exe')]
  build=subprocess.run(command,capture_output=True,text=True)
  assert build.returncode==0,build.stdout+build.stderr
  run=subprocess.run([str(work/'test_touch.exe')],capture_output=True,text=True)
  assert run.returncode==0,run.stdout+run.stderr
  result={'passed':True,'scope':'Complete production wrapper and parser under deterministic host IO/LVGL/clock mocks; excludes real I2C protocol acceptance, controller Hz, native gesture behavior, cross-core scheduling or board validation','source':'overlay-apps/main/hosino_touch_override.c','source_sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),'helper_sha256':hashlib.sha256(SOURCE.with_name('hosino_touch_packet.h').read_bytes()).hexdigest(),'test_source_sha256':hashlib.sha256(Path(__file__).with_name('test_touch_v21.c').read_bytes()).hexdigest(),'compile_exit_code':build.returncode,'test_exit_code':run.returncode,'output':run.stdout.strip()}
  (ROOT/'evidence/touch-v21-host-20261114.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
  print(run.stdout.strip())
if __name__=='__main__':main()
