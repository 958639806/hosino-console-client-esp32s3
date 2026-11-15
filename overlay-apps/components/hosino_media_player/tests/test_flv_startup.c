/* Production helper tests. Native operations are a ref-counted boundary fake,
 * not execution of Espressif's closed Xtensa parser. */
#include <assert.h>
#include <stdio.h>
#include "hosino_flv_startup.h"

typedef struct {
    esp_extractor_frame_info_t packets[12];
    uint8_t storage[12];
    unsigned total,reads,live,released[12],enabled;
    uint16_t audio,video;
    int error_at,cancel_at;
    esp_extractor_err_t read_error;
    bool eos_buffer,bad_format,enable_error,count_error;
    esp_extractor_type_t type;
    int64_t now,read_cost;
} fixture_t;

static fixture_t fixture(const char *order)
{
    fixture_t f={0};
    f.error_at=f.cancel_at=-1; f.type=ESP_EXTRACTOR_TYPE_FLV;
    for (;order[f.total];f.total++) {
        unsigned i=f.total;
        f.packets[i].stream_type=order[i]=='A'?ESP_EXTRACTOR_STREAM_TYPE_AUDIO:ESP_EXTRACTOR_STREAM_TYPE_VIDEO;
        f.packets[i].frame_size=order[i]=='A'?980:5700;
        f.packets[i].pts=i*22;
        f.packets[i].frame_pos=i*6000;
    }
    /* Faithful relevant native behavior: parse_stream discovers first packet's
     * track, caches that packet, but has not read the following track yet. */
    if (f.total) {
        f.audio=f.packets[0].stream_type==ESP_EXTRACTOR_STREAM_TYPE_AUDIO;
        f.video=f.packets[0].stream_type==ESP_EXTRACTOR_STREAM_TYPE_VIDEO;
    }
    return f;
}
esp_extractor_err_t esp_extractor_get_extractor_type(esp_extractor_handle_t h,esp_extractor_type_t *type)
{*type=((fixture_t *)h)->type;return ESP_EXTRACTOR_ERR_OK;}
esp_extractor_err_t esp_extractor_get_stream_num(esp_extractor_handle_t h,esp_extractor_stream_type_t t,uint16_t *n)
{
    fixture_t *f=h;
    if (f->count_error) return ESP_EXTRACTOR_ERR_READ;
    *n=t==ESP_EXTRACTOR_STREAM_TYPE_AUDIO?f->audio:f->video;
    return *n?ESP_EXTRACTOR_ERR_OK:ESP_EXTRACTOR_ERR_NOT_FOUND;
}
esp_extractor_err_t esp_extractor_read_frame(esp_extractor_handle_t h,esp_extractor_frame_info_t *out)
{
    fixture_t *f=h;
    unsigned i=f->reads;
    if (i==(unsigned)f->error_at && !f->eos_buffer) return f->read_error;
    if (i>=f->total) return ESP_EXTRACTOR_ERR_EOS;
    *out=f->packets[i]; out->frame_buffer=&f->storage[i];
    f->live++; f->reads++; f->now+=f->read_cost;
    if (out->stream_type==ESP_EXTRACTOR_STREAM_TYPE_AUDIO) f->audio=1; else f->video=1;
    return i==(unsigned)f->error_at?f->read_error:ESP_EXTRACTOR_ERR_OK;
}
esp_extractor_err_t esp_extractor_release_frame(esp_extractor_handle_t h,esp_extractor_frame_info_t *frame)
{
    fixture_t *f=h;
    ptrdiff_t i=frame->frame_buffer-f->storage;
    assert(i>=0 && i<(ptrdiff_t)f->total);
    assert(f->released[i]++==0 && f->live>0);
    f->live--;return ESP_EXTRACTOR_ERR_OK;
}
esp_extractor_err_t esp_extractor_get_stream_info(esp_extractor_handle_t h,esp_extractor_stream_type_t t,uint16_t idx,esp_extractor_stream_info_t *info)
{
    fixture_t *f=h;assert(idx==0);memset(info,0,sizeof(*info)); info->stream_type=t;
    if (t==ESP_EXTRACTOR_STREAM_TYPE_AUDIO) {
        info->audio_info.format=f->bad_format?ESP_EXTRACTOR_AUDIO_FORMAT_AAC:ESP_EXTRACTOR_AUDIO_FORMAT_PCM;
        info->audio_info.channel=1;info->audio_info.bits_per_sample=16;info->audio_info.sample_rate=22050;
    } else info->video_info.format=ESP_EXTRACTOR_VIDEO_FORMAT_MJPEG;
    return ESP_EXTRACTOR_ERR_OK;
}
esp_extractor_err_t esp_extractor_enable_stream(esp_extractor_handle_t h,esp_extractor_stream_type_t t,uint16_t idx,bool enable)
{
    fixture_t *f=h;assert(idx==0&&enable);
    assert(t==ESP_EXTRACTOR_STREAM_TYPE_AUDIO?f->audio:f->video);
    f->enabled++;return f->enable_error?ESP_EXTRACTOR_ERR_FAIL:ESP_EXTRACTOR_ERR_OK;
}
static int64_t now(void *h){return ((fixture_t *)h)->now;}
static bool stop(void *h){fixture_t *f=h;return f->cancel_at>=0&&f->reads>=(unsigned)f->cancel_at;}
static esp_extractor_err_t probe(fixture_t *f,hosino_flv_startup_t *q)
{uint16_t a=0,v=0;return hosino_flv_startup_probe(q,f,now,stop,f,320,180,20,&a,&v);}

static void test_order(const char *order,unsigned expected)
{
    fixture_t f=fixture(order);hosino_flv_startup_t q={0};
    assert(probe(&f,&q)==ESP_EXTRACTOR_ERR_OK);
    assert(q.count==expected && f.reads==expected && f.live==expected && f.enabled==2);
    for(unsigned i=0;i<expected;i++) {
        esp_extractor_frame_info_t frame={0};assert(hosino_flv_startup_pop(&q,&frame));
        assert(frame.frame_buffer==&f.storage[i] && frame.pts==i*22 && frame.frame_pos==i*6000);
        assert(frame.stream_type==f.packets[i].stream_type);
        esp_extractor_release_frame(&f,&frame);
    }
    esp_extractor_frame_info_t unused;assert(!hosino_flv_startup_pop(&q,&unused));
    hosino_flv_startup_clear(&q,&f);assert(f.live==0);
}
static void expect_failure(fixture_t *f,const char *reason,unsigned live)
{
    hosino_flv_startup_t q={0};assert(probe(f,&q)!=ESP_EXTRACTOR_ERR_OK);
    assert(strcmp(q.reason,reason)==0 && f->live==live);
    hosino_flv_startup_clear(&q,f);assert(f->live==0);
    hosino_flv_startup_clear(&q,f);assert(f->live==0);
}
int main(void)
{
    test_order("VA",2);test_order("AAAV",4);test_order("VVVVVVVA",8);
    fixture_t f=fixture("VA");hosino_flv_startup_t q={0};
    assert(f.audio==0&&f.video==1);assert(probe(&f,&q)==0);
    esp_extractor_frame_info_t transferred={0};assert(hosino_flv_startup_pop(&q,&transferred));
    hosino_flv_startup_clear(&q,&f);assert(f.live==1&&f.released[1]==1&&f.released[0]==0);
    esp_extractor_release_frame(&f,&transferred);assert(f.live==0);
    /* Pop followed by disabled track / missing port / acquire FAIL or ABORT:
     * still local, so release once; clearing the remaining queue never repeats it. */
    for(unsigned failure=0;failure<4;failure++) {
        f=fixture("VA");q=(hosino_flv_startup_t){0};assert(probe(&f,&q)==0);
        bool owned=hosino_flv_startup_pop(&q,&transferred);assert(owned);
        hosino_flv_startup_release_local(&f,&transferred,&owned);assert(!owned);
        hosino_flv_startup_release_local(&f,&transferred,&owned);
        hosino_flv_startup_clear(&q,&f);assert(f.live==0&&f.released[0]==1&&f.released[1]==1);
    }
    /* Port release consumes on either success or push failure, so the caller
     * relinquishes ownership BEFORE that callback and cannot free it twice. */
    f=fixture("VA");q=(hosino_flv_startup_t){0};assert(probe(&f,&q)==0);
    bool owned=hosino_flv_startup_pop(&q,&transferred);assert(owned);owned=false;
    esp_extractor_release_frame(&f,&transferred);
    hosino_flv_startup_release_local(&f,&transferred,&owned);
    hosino_flv_startup_clear(&q,&f);assert(f.live==0);
    f=fixture("VA");f.audio=f.video=1;q=(hosino_flv_startup_t){0};
    assert(probe(&f,&q)==0&&f.reads==0&&q.count==0);
    f=fixture("VVVVVVVVA");expect_failure(&f,"packet_limit_missing_track",8);assert(f.reads==8);
    f=fixture("VA");f.packets[0].frame_size=HOSINO_FLV_STARTUP_BYTES+1;
    expect_failure(&f,"byte_limit",0);assert(f.released[0]==1);
    f=fixture("VA");f.read_cost=HOSINO_FLV_STARTUP_US;expect_failure(&f,"deadline",1);assert(f.reads==1);
    f=fixture("VA");f.cancel_at=1;expect_failure(&f,"cancelled",1);
    f=fixture("VA");f.error_at=1;f.read_error=ESP_EXTRACTOR_ERR_READ;expect_failure(&f,"read_frame",1);
    f=fixture("VA");f.error_at=1;f.read_error=ESP_EXTRACTOR_ERR_WAITING_OUTPUT;expect_failure(&f,"native_pool_full",1);assert(f.reads==1);
    f=fixture("VA");f.error_at=1;f.read_error=ESP_EXTRACTOR_ERR_EOS;f.eos_buffer=true;
    expect_failure(&f,"read_frame",1);assert(f.released[1]==1);
    f=fixture("VA");f.bad_format=true;expect_failure(&f,"track_format",2);
    f=fixture("VA");f.enable_error=true;expect_failure(&f,"enable_tracks",2);
    f=fixture("VA");f.video=2;expect_failure(&f,"multiple_tracks",0);
    f=fixture("VA");f.type=ESP_EXTRACTOR_TYPE_AVI;expect_failure(&f,"container",0);
    f=fixture("VA");f.count_error=true;expect_failure(&f,"track_count",0);
    assert(hosino_flv_startup_enabled(true,true,3,3,320,180,20));
    assert(hosino_flv_startup_enabled(true,true,3,3,480,270,15));
    assert(!hosino_flv_startup_enabled(false,true,3,3,320,180,20));
    assert(!hosino_flv_startup_enabled(true,false,3,3,320,180,20));
    assert(!hosino_flv_startup_enabled(true,true,2,3,320,180,20));
    assert(!hosino_flv_startup_enabled(true,true,3,3,480,320,15));
    q=(hosino_flv_startup_t){0};uint32_t wait=500;
    assert(hosino_flv_startup_read_limit(NULL,9999,300,&wait)==300&&wait==500);
    assert(hosino_flv_startup_read_limit(&q,9999,300,&wait)==300&&wait==500);
    q.deadline_us=1000000;q.input_bytes=HOSINO_FLV_STARTUP_INPUT_BYTES-10;
    assert(hosino_flv_startup_read_limit(&q,990000,100,&wait)==10&&wait==10);
    q.input_bytes=HOSINO_FLV_STARTUP_INPUT_BYTES;
    assert(hosino_flv_startup_read_limit(&q,990000,1,&wait)==0);
    q.input_bytes=0;assert(hosino_flv_startup_read_limit(&q,1000000,1,&wait)==0);
    puts("MANAGED_FLV_BIDIRECTIONAL_PRIMING_REPLAY_OWNERSHIP_AND_BOUNDS_PASS");return 0;
}
