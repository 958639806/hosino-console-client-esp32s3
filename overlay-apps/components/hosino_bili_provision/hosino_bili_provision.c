/* SPDX-License-Identifier: Apache-2.0 */
#include "hosino_bili_provision.h"
#include "provision_parser.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static bool s_started;
static void worker(void *argument)
{
    bili_provision_line_t *line=argument;
#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
    const char *path="/dev/secondary";
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    const char *path="/dev/usbserjtag";
#else
    const char *path=NULL;
#endif
    FILE *input=path?fopen(path,"r"):NULL;
    const int input_error=input?0:errno;
    FILE *output=path?fopen(path,"w"):NULL;
    const int output_error=output?0:errno;
    if(input&&output) {
        setvbuf(input,NULL,_IONBF,0);setvbuf(output,NULL,_IONBF,0);
        const bool driver=usb_serial_jtag_is_driver_installed();
        const int flags=fcntl(fileno(input),F_GETFL,0);
        /* IDF5.5.5 polling VFS: O_NONBLOCK skips the LL FIFO pre-read, but
         * get_read_bytes_available only counts an installed driver's ring.
         * Polling no-driver reads therefore need this flag cleared; its LL
         * read still returns immediately when empty. Driver reads need it set. */
        if(flags<0||fcntl(fileno(input),F_SETFL,driver?(flags|O_NONBLOCK):(flags&~O_NONBLOCK))<0) {
            ESP_LOGE("bili_usb_cfg","USB reader mode failed errno=%d",errno);
            goto cleanup;
        }
        ESP_LOGI("bili_usb_cfg","USB reader ready driver=%d",driver?1:0);
        uint32_t received=0;
        for(;;) {
            const int byte=fgetc(input);
            if(byte==EOF) { clearerr(input);vTaskDelay(pdMS_TO_TICKS(20));continue; }
            if(++received==1) ESP_LOGI("bili_usb_cfg","USB RX active count=1");
            const bili_line_result_t ready=bili_provision_feed(line,(unsigned char)byte);
            if(ready==BILI_LINE_NONE) continue;
            char request_id[17]={0};
            if(ready==BILI_LINE_READY&&bili_provision_sync_id(line->bytes,request_id)) {
                fprintf(output,"\nBILI_SYNC_OK %s\n",request_id);fflush(output);
                bili_provision_clear(line,sizeof(*line));continue;
            }
            const bili_provision_result_t result=ready==BILI_LINE_TOO_LONG?BILI_PROVISION_TOO_LONG:bili_provision_apply_request(line->bytes,request_id);
            const char *ack=bili_provision_ack(result);
            if(ack) { if(request_id[0]) fprintf(output,"\n%s %s\n",ack,request_id);else fprintf(output,"\n%s\n",ack);fflush(output); }
            bili_provision_clear(line,sizeof(*line));
            vTaskDelay(1);
        }
    }
    ESP_LOGE("bili_usb_cfg","USB channel open failed input_errno=%d output_errno=%d",input_error,output_error);
cleanup:
    if(input) fclose(input);
    if(output) fclose(output);
    bili_provision_clear(line,sizeof(*line));heap_caps_free(line);
    portENTER_CRITICAL(&s_lock);s_started=false;portEXIT_CRITICAL(&s_lock);
    vTaskDeleteWithCaps(NULL);
}
esp_err_t hosino_bili_provision_start(void)
{
#if !(CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    return ESP_ERR_NOT_SUPPORTED;
#endif
    portENTER_CRITICAL(&s_lock);
    const bool existing=s_started;s_started=true;
    portEXIT_CRITICAL(&s_lock);
    if(existing) return ESP_OK;
    bili_provision_line_t *line=heap_caps_calloc(1,sizeof(*line),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(line&&xTaskCreateWithCaps(worker,"bili_usb_cfg",4096,line,1,NULL,MALLOC_CAP_SPIRAM)==pdPASS) return ESP_OK;
    heap_caps_free(line);
    portENTER_CRITICAL(&s_lock);s_started=false;portEXIT_CRITICAL(&s_lock);
    return ESP_ERR_NO_MEM;
}
