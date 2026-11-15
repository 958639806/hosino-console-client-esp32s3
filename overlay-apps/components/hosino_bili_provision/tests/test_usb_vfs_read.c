/* Compile the pinned IDF read function unchanged with deterministic FIFO/ring
 * boundaries. No serial success or ACK is fabricated by this host test. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif
#define NONE -1
#define USJ_LOCAL_FD 0
#define ESP_LINE_ENDINGS_CR 1
#define ESP_LINE_ENDINGS_CRLF 2
#define MIN(a,b) ((a)<(b)?(a):(b))
struct {bool non_blocking;int peek_char,rx_mode,read_lock;} s_ctx;
static int fifo_count,driver;
static bool usb_serial_jtag_is_connected(void) {return true;}
static int usb_serial_jtag_read_char(int fd) {(void)fd;if(fifo_count){--fifo_count;return '\n';}return NONE;}
static void usb_serial_jtag_return_char(int fd,int c) {(void)fd;s_ctx.peek_char=c;}
static size_t usb_serial_jtag_get_read_bytes_available(void) {return driver?(size_t)fifo_count:0;}
static void _lock_acquire_recursive(int *p) {(void)p;}
static void _lock_release_recursive(int *p) {(void)p;}
#include "idf_usb_read.c"
int main(void) {
    char received=0;s_ctx.peek_char=NONE;
    driver=0;fifo_count=1;s_ctx.non_blocking=true;
    assert(usb_serial_jtag_read(0,&received,1)==-1&&fifo_count==1);
    s_ctx.non_blocking=false;
    assert(usb_serial_jtag_read(0,&received,1)==1&&received=='\n'&&fifo_count==0);
    assert(usb_serial_jtag_read(0,&received,1)==-1);
    driver=1;fifo_count=1;s_ctx.non_blocking=true;
    assert(usb_serial_jtag_read(0,&received,1)==1&&fifo_count==0);
    puts("PINNED_IDF_USB_READ_REPRODUCED_NONDRIVER_FLAG_STARVATION_AND_FIX_PASS");return 0;
}
