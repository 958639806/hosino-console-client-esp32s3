/* Deterministic native/GMF boundary fixture. Production helpers are inserted by
 * the runner. The tiny cache-tail model below follows pinned Xtensa branches;
 * it is not execution of the proprietary native binary on this host. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_gmf_err_io_t;
typedef void *esp_gmf_element_handle_t;
typedef struct { bool _is_stop; void *input_handle,*sync_handle; bool hosino_managed_mjpeg_http; } esp_player_stream_t;
typedef struct { uint8_t *buf; uint32_t buf_length,valid_size; bool is_done; } esp_gmf_payload_t;
enum { ESP_GMF_ERR_OK=0, ESP_PLAYER_ERR_OK=0, ESP_GMF_IO_OK=0,
       ESP_GMF_IO_FAIL=-1, ESP_GMF_IO_ABORT=-2, ESP_GMF_IO_TIMEOUT=-3 };
#define pdMS_TO_TICKS(x) (x)
#define ESP_PLAYER_READ_WAIT_TIME_MS 10000

static uint8_t source[200000],borrowed[200000];
static uint32_t length,source_pos,io_pos,produced,step_bytes;
static int64_t now_us,next_at,step_us,eof_at,cancel_at,fail_at,startup_deadline;
static uint32_t input_bytes,input_limit;
static unsigned acquire_calls,release_calls,open_calls,seek_calls;
static bool use_borrowed,fail_release;
static esp_player_stream_t stream;

static int64_t esp_timer_get_time(void) { return now_us; }
static void update(void) {
    while (next_at>=0 && now_us>=next_at && produced<length) {
        uint32_t left=length-produced;
        produced+=step_bytes<left ? step_bytes:left;
        next_at+=step_us;
        if (!step_us) next_at=-1;
    }
    if (cancel_at>=0 && now_us>=cancel_at) stream._is_stop=true;
}
static void vTaskDelay(unsigned ticks) { now_us+=(int64_t)ticks*1000; update(); }
static int player_ensure_input_open(esp_player_stream_t *s) { (void)s; ++open_calls; return 0; }
static void *player_extractor_el(esp_player_stream_t *s) { return s; }
static uint32_t player_extractor_startup_read_limit(void *e,uint32_t n,uint32_t *wait) {
    (void)e;
    if (startup_deadline) {
        if (now_us>=startup_deadline || input_bytes>=input_limit) return 0;
        uint32_t left=(uint32_t)((startup_deadline-now_us)/1000);
        if (!left) return 0;
        if (*wait>left) *wait=left;
        if (n>input_limit-input_bytes) n=input_limit-input_bytes;
    }
    return n;
}
static void player_extractor_startup_note_read(void *e,uint32_t n) { (void)e;input_bytes+=n; }
static int esp_gmf_io_get_db_filled_size(void *h,uint32_t *n) { (void)h;update();*n=produced-source_pos;return 0; }
static int esp_gmf_io_get_pos(void *h,uint64_t *p) { (void)h;*p=io_pos;return 0; }
static int esp_gmf_io_acquire_read(void *h,esp_gmf_payload_t *p,uint32_t n,unsigned ticks) {
    (void)h; ++acquire_calls; assert(acquire_calls<20000); update();
    /* Avoid waiting for a larger native-cache request when bytes are available;
     * only ask available bytes, or one byte when the snapshot was empty. */
    assert(n<=produced-source_pos || n==1);
    while (produced-source_pos<n) {
        if (stream._is_stop) return ESP_GMF_IO_ABORT;
        if (fail_at>=0 && now_us>=fail_at) return ESP_GMF_IO_FAIL;
        if (source_pos==length && eof_at>=0 && now_us>=eof_at) {p->is_done=true;return 0;}
        int64_t end=now_us+(int64_t)ticks*1000;
        int64_t target=end;
        if (next_at>=0 && next_at<target) target=next_at;
        if (cancel_at>=0 && cancel_at<target) target=cancel_at;
        if (fail_at>=0 && fail_at<target) target=fail_at;
        if (eof_at>=0 && eof_at<target && source_pos==length) target=eof_at;
        if (target<=now_us) target=end;
        now_us=target;update();
        if (produced-source_pos<n && now_us>=end) return ESP_GMF_IO_TIMEOUT;
    }
    uint8_t *dest=use_borrowed ? borrowed:p->buf;
    memcpy(dest,source+source_pos,n);source_pos+=n;
    p->buf=dest;p->valid_size=n;return 0;
}
static int esp_gmf_io_release_read(void *h,esp_gmf_payload_t *p,unsigned ticks) {
    (void)h;(void)ticks;++release_calls;io_pos+=p->valid_size;
    return fail_release ? ESP_GMF_IO_FAIL:0;
}
static uint64_t player_sync_get_audio_decode_pts_ms(void *h) { (void)h;return 0; }
static uint64_t player_sync_get_audio_render_pts_ms(void *h) { (void)h;return 0; }
static void player_extractor_raw_read_pace(esp_player_stream_t *s,void *e,uint64_t p) { (void)s;(void)e;(void)p; }
static void player_extractor_get_last_pts(void *e,uint64_t *p) { (void)e;*p=0; }

/* PRODUCTION_MANAGED_INPUT */
/* PRODUCTION_EXTRACTOR_READ */

static void reset(uint32_t n,uint32_t first) {
    memset(&stream,0,sizeof(stream));stream.hosino_managed_mjpeg_http=true;stream.input_handle=&stream;
    length=n;produced=first;source_pos=io_pos=0;now_us=0;
    next_at=eof_at=cancel_at=fail_at=-1;step_bytes=0;step_us=0;
    startup_deadline=0;input_limit=131072;input_bytes=0;
    acquire_calls=release_calls=open_calls=seek_calls=0;use_borrowed=fail_release=false;
    for (uint32_t i=0;i<n;i++) source[i]=(uint8_t)(i*17+3);
}

/* Pinned data_cache_seek forward cache miss with NULL callback calls
 * data_cache_skip then returns its positive count unchanged; flv_read_frame
 * +0x6ce..0x6dc propagates it as its frame status. With a callback, +0x86..0x90
 * expects zero and only then sets cache_start_pos=target. */
static int native_tail_seek_model(bool fixed,uint32_t target) {
    ++seek_calls;
    if (fixed) return _extractor_managed_seek(target,&stream);
    uint8_t scratch[16];
    uint32_t delta=target-io_pos;
    assert(delta<=sizeof(scratch));
    return player_managed_read(scratch,delta,&stream,now_us+PLAYER_MANAGED_READ_US);
}

int main(int argc,char **argv) {
    uint8_t out[8192];
    /* Initial non-word-aligned batch, then a 300ms gap: no native short-read EOF. */
    reset(4096,2423);next_at=300000;step_bytes=1673;use_borrowed=true;
    assert(_extractor_read(out,4096,&stream)==4096 && now_us==300000);
    assert(!memcmp(out,source,4096) && io_pos==4096 && source_pos==4096);
    assert(acquire_calls>3 && open_calls==1);
    /* Many short arrivals, preserving every byte and one open. */
    reset(64,0);next_at=10000;step_bytes=1;step_us=10000;
    assert(_extractor_read(out,64,&stream)==64 && now_us==640000);
    assert(!memcmp(out,source,64) && open_calls==1);
    /* Errors/deadline after useful bytes are negative, never fake EOS/partial. */
    reset(64,2);fail_at=100000;
    assert(_extractor_read(out,64,&stream)==-1 && io_pos==2 && now_us<=150000);
    reset(64,2);
    assert(_extractor_read(out,64,&stream)==-1 && now_us==8000000 && io_pos==2);
    reset(64,0);next_at=0;step_bytes=1;step_us=2000000;
    assert(_extractor_read(out,64,&stream)==-1 && now_us==8000000);
    /* Startup's original shared 3s and input-byte caps retain precedence. */
    reset(64,2);startup_deadline=3000000;
    assert(_extractor_read(out,64,&stream)==-1 && now_us==3000000 && input_bytes==2);
    reset(64,64);startup_deadline=3000000;input_limit=7;
    assert(_extractor_read(out,64,&stream)==-1 && io_pos==7 && input_bytes==7);
    /* Cancellation is observed during bounded one-byte waits. */
    reset(64,2);cancel_at=125000;
    assert(_extractor_read(out,64,&stream)==-1 && now_us<=175000 && io_pos==2);
    /* Only true source completion permits a positive short read / zero EOF. */
    reset(17,17);eof_at=0;
    assert(_extractor_read(out,32,&stream)==17 && !memcmp(out,source,17));
    assert(_extractor_read(out,32,&stream)==0 && io_pos==17);
    reset(32,32);fail_release=true;
    assert(_extractor_read(out,32,&stream)==-1);
    /* Current position counts consumer bytes, not the HTTP-prefetched supply. */
    reset(8192,8192);io_pos=source_pos=4096;
    assert(_extractor_managed_seek(4100,&stream)==0 && io_pos==4100 && produced==8192);
    assert(_extractor_managed_seek(4100,&stream)==0);
    unsigned before=acquire_calls;
    assert(_extractor_managed_seek(4099,&stream)==-1 && acquire_calls==before);
    assert(_extractor_managed_seek(4100+131073,&stream)==-1 && acquire_calls==before);
    stream.hosino_managed_mjpeg_http=false;
    assert(_extractor_managed_seek(4101,&stream)==-1 && acquire_calls==before);
    /* One cumulative skip deadline across 512-byte scratch chunks. */
    reset(4096,512);next_at=2000000;step_bytes=512;step_us=2000000;
    assert(_extractor_managed_seek(4096,&stream)==-1 && now_us==8000000);
    assert(io_pos<4096 && open_calls==1);
    /* Exact native cache-boundary regression: a valid 4-byte PrevTagSize skip
     * used to bubble up as EOS=4. New callback returns0 and leaves next AUDIO. */
    reset(8192,8192);io_pos=source_pos=4096;source[4100]=8;
    assert(native_tail_seek_model(false,4100)==4);
    reset(8192,8192);io_pos=source_pos=4096;source[4100]=8;
    assert(native_tail_seek_model(true,4100)==0 && io_pos==4100);
    assert(_extractor_read(out,1,&stream)==1 && out[0]==8);
    /* Cached rewind happens inside native, never calls the source callback.
     * Its cache miss to earlier source bytes is deliberately rejected above. */
    assert(seek_calls==1);
    /* Actual public HTTPS capture: its first MJPEG packet exceeds native's
     * 4096-byte cache. Once the body is read directly, only its valid 4-byte
     * PrevTagSize remains before the first real PCM AUDIO tag. */
    assert(argc==2);
    FILE *file=fopen(argv[1],"rb");assert(file);
    uint8_t prefix[32768];size_t prefix_size=fread(prefix,1,sizeof(prefix),file);fclose(file);
    assert(prefix_size==sizeof(prefix));
    assert(!memcmp(prefix,"FLV",3) && prefix[436]==9 && prefix[10532]==8);
    assert(prefix[10528]==0 && prefix[10529]==0 && prefix[10530]==0x27 && prefix[10531]==0x6c);
    reset((uint32_t)prefix_size,(uint32_t)prefix_size);memcpy(source,prefix,prefix_size);
    io_pos=source_pos=10528;
    assert(native_tail_seek_model(false,10532)==4);
    reset((uint32_t)prefix_size,(uint32_t)prefix_size);memcpy(source,prefix,prefix_size);
    io_pos=source_pos=10528;
    assert(native_tail_seek_model(true,10532)==0);
    assert(_extractor_read(out,12,&stream)==12 && out[0]==8 && out[11]==0x3a);
    puts("PRODUCTION_MANAGED_EXACT_READ_FORWARD_SEEK_CACHE4_EOS_CANCEL_BOUNDS_PASS");
    return 0;
}
