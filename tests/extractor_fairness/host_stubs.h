#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#define strncasecmp _strnicmp
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_EXTRACT_MASK_AUDIO 1
#define ESP_EXTRACT_MASK_VIDEO 2
#define ESP_EXTRACT_MASK_AV 3
#define ESP_PLAYER_MASK_AV 3
#define ESP_PLAYER_DEC_FRAME_MODE_EXTRACTOR 0
#define ESP_PLAYER_DEC_FRAME_MODE_FILL 1
#define ESP_PLAYER_STATE_PLAYING 2
#define ESP_PLAYER_STATE_PAUSED 3
#define ESP_PLAYER_STATE_PREPARING 1
#define ESP_PLAYER_BUFFER_GATE_NONE 0
#define ESP_GMF_ERR_OK 0
#define ESP_GMF_ERR_FAIL -1
#define ESP_GMF_ERR_INVALID_ARG -2
#define ESP_GMF_NULL_CHECK(tag,arg,action) do{if(!(arg)){action;}}while(0)
#define ESP_GMF_IO_OK 0
#define ESP_GMF_IO_FAIL -1
#define ESP_GMF_IO_ABORT -2
#define ESP_GMF_JOB_ERR_OK 0
#define ESP_GMF_JOB_ERR_CONTINUE 1
#define ESP_GMF_JOB_ERR_DONE 2
#define ESP_GMF_JOB_ERR_FAIL -1
#define ESP_GMF_JOB_ERR_ABORT -2
#define ESP_GMF_MAX_DELAY -1
#define ESP_EXTRACTOR_ERR_OK 0
#define ESP_EXTRACTOR_ERR_EOS 2
#define ESP_EXTRACTOR_ERR_WAITING_OUTPUT 5
#define ESP_EXTRACTOR_ERR_ABORTED -8
#define ESP_EXTRACTOR_ERR_NOT_FOUND -3
#define ESP_EXTRACTOR_STREAM_TYPE_NONE 0
#define ESP_EXTRACTOR_STREAM_TYPE_AUDIO 1
#define ESP_EXTRACTOR_STREAM_TYPE_VIDEO 2
#define ESP_FOURCC_MJPG 0x47504a4d
#define ESP_FOURCC_H264 0x34363248
#define EXTRACTOR_FRAME_FLAG_EOS 1
#define EXTRACTOR_IS_EOS(f) (((f)&1)!=0)
#define PLAYER_EXTRACTOR_VIDEO_HEAD_WHEN_AUDIO_EMPTY 2U
#define PLAYER_EXTRACTOR_AV_FAIRNESS_HOLD_MS 10U
#define pdMS_TO_TICKS(x) (x)
enum {HOSINO_PLAYER_DIAG_DEMUX_AUDIO_HOLD,HOSINO_PLAYER_DIAG_DEMUX_POOL_WAIT,HOSINO_PLAYER_DIAG_DEMUX_VIDEO_PACKET,HOSINO_PLAYER_DIAG_DEMUX_VIDEO_DROP_AUDIO_DRY,DIAG_MAX};
typedef int esp_extractor_err_t;
typedef int esp_gmf_err_io_t;
typedef int esp_gmf_job_err_t;
typedef void *esp_extractor_handle_t;
typedef struct {uint8_t stream_type,frame_flag;uint16_t stream_idx;uint32_t pts;uint8_t *frame_buffer;uint32_t frame_size,frame_pos;} esp_extractor_frame_info_t;
typedef struct {uint16_t stream_id;} esp_extractor_stream_info_t;
typedef struct {uint8_t *buf;uint32_t buf_length,valid_size;uint64_t pts;bool is_done;uint32_t meta_flag;} esp_gmf_payload_t;
typedef struct port {struct port *next;bool audio;} *esp_gmf_port_handle_t;
typedef struct {esp_gmf_port_handle_t out;} esp_gmf_element_t;
typedef void *esp_gmf_element_handle_t;
typedef struct {void *in_ctx;} esp_extractor_config_t;
typedef struct {unsigned count;} queue_t;
typedef struct {struct {struct {uint32_t format;} video_info;} track_info;queue_t *frame_queue;} side_t;
typedef struct {int gate_state;} gate_t;
typedef struct {bool is_seeking,_is_stop,hosino_managed_mjpeg_http;int main_state,dec_frame_mode;uint8_t av_mask;void *input_handle;side_t *audio_side,*video_side;gate_t *buffer_ctrl;} esp_player_stream_t;
static esp_extractor_config_t g_cfg;
#define OBJ_GET_CFG(x) (&g_cfg)
#define ESP_GMF_ELEMENT_GET(x) ((esp_gmf_element_t *)(x))
#define ESP_GMF_PORT_ACQUIRE_OUT_CHECK(tag,ret,job,action) do{if(ret!=ESP_GMF_IO_OK){job=ret;action;}}while(0)
#define ESP_GMF_PORT_RELEASE_OUT_CHECK(tag,ret,job,action) do{if(ret!=ESP_GMF_IO_OK){job=ret;action;}}while(0)
enum {UNREAD,EXTRACTOR_OWNED,QUEUE_OWNED,RELEASED};
typedef struct {uint8_t token;int owner,type,result;uint32_t bytes;uint8_t flags;uint32_t pts;} frame_t;
static frame_t frames[32];static unsigned frame_count,next_frame,reads,releases,success_releases,queued_audio,queued_video,sleeps;
static unsigned pool_size,pool_used,pool_peak;static bool release_fails,uri_fails;static int acquire_result;
static unsigned diagnostics[DIAG_MAX],diag_errors[DIAG_MAX],diag_max[DIAG_MAX];
static esp_player_stream_t stream;static side_t audio,video;static queue_t aq,vq;static gate_t gate;
static struct port ap,vp;static esp_gmf_payload_t payload;static int64_t host_now;
static const char *uri;
static bool inflate_video_pts;
static uint32_t vendor_video_attempts;
static uint64_t last_queued_pts;
static int seek_result;
static unsigned player_frame_queue_count(queue_t *q){return q?q->count:0;}
static int esp_gmf_io_get_uri(void *handle,char **out){if(uri_fails)return -1;*out=(char*)uri;return 0;}
static void vTaskDelay(unsigned ticks){sleeps++;host_now+=ticks*1000;}
static int64_t esp_timer_get_time(void){return host_now;}
static void hosino_player_diagnostics_note(unsigned ev,int64_t us,uint32_t amount,int result){assert(ev<DIAG_MAX);diagnostics[ev]++;if(result)diag_errors[ev]++;if(amount>diag_max[ev])diag_max[ev]=amount;}
static int frame_index(uint8_t *buf){for(unsigned i=0;i<frame_count;i++)if(buf==&frames[i].token)return i;assert(!"unknown payload");return -1;}
static esp_extractor_err_t esp_extractor_read_frame(void *handle,esp_extractor_frame_info_t *out){
 reads++;if(next_frame>=frame_count)return ESP_EXTRACTOR_ERR_ABORTED;
 frame_t *f=&frames[next_frame];
 if(f->result!=ESP_EXTRACTOR_ERR_OK&&f->result!=ESP_EXTRACTOR_ERR_EOS){next_frame++;return f->result;}
 uint32_t pts=f->pts;
 if(inflate_video_pts && f->type==ESP_EXTRACTOR_STREAM_TYPE_VIDEO)pts=(uint32_t)(((uint64_t)vendor_video_attempts++*1001000)/24000);
 *out=(esp_extractor_frame_info_t){.stream_type=f->type,.stream_idx=0,.frame_flag=f->flags,.pts=pts,.frame_pos=100+next_frame*100,.frame_size=f->bytes};
 if(pool_used+f->bytes>pool_size)return ESP_EXTRACTOR_ERR_WAITING_OUTPUT;
 assert(f->owner==UNREAD);f->owner=EXTRACTOR_OWNED;next_frame++;pool_used+=f->bytes;if(pool_used>pool_peak)pool_peak=pool_used;
 out->frame_buffer=f->bytes?&f->token:NULL;return f->result;
}
static esp_extractor_err_t esp_extractor_release_frame(void *handle,esp_extractor_frame_info_t *f){
 releases++;int n=frame_index(f->frame_buffer);assert(frames[n].owner==EXTRACTOR_OWNED||frames[n].owner==QUEUE_OWNED);
 if(release_fails)return -1;frames[n].owner=RELEASED;success_releases++;assert(pool_used>=frames[n].bytes);pool_used-=frames[n].bytes;return 0;
}
static esp_gmf_err_io_t esp_gmf_port_acquire_out(esp_gmf_port_handle_t port,esp_gmf_payload_t **load,uint32_t size,int timeout){if(acquire_result)return acquire_result;memset(&payload,0,sizeof(payload));*load=&payload;return 0;}
static esp_gmf_err_io_t esp_gmf_port_release_out(esp_gmf_port_handle_t port,esp_gmf_payload_t *load,int timeout){
 last_queued_pts=load->pts;
 if(load->buf){int n=frame_index(load->buf);assert(frames[n].owner==EXTRACTOR_OWNED);if(stream._is_stop){esp_extractor_frame_info_t f={.frame_buffer=load->buf,.frame_size=load->valid_size};esp_extractor_release_frame(NULL,&f);load->buf=NULL;return ESP_GMF_IO_ABORT;}frames[n].owner=QUEUE_OWNED;load->buf=NULL;}
 if(port->audio){queued_audio++;aq.count++;}else{queued_video++;vq.count++;}return 0;
}
static int esp_extractor_parse_stream(void *h){return 0;}
static int esp_extractor_get_stream_num(void *h,int type,uint16_t *num){*num=1;return 0;}
static int esp_extractor_get_stream_info(void *h,int type,uint16_t idx,esp_extractor_stream_info_t *info){info->stream_id=0;return 0;}
static int esp_extractor_enable_stream(void *h,int type,uint16_t idx,bool enable){return 0;}
static int esp_gmf_element_notify_vid_info(void *h,void *v){return 0;}
static void player_extractor_finalize_id3(void *h){}
static int esp_extractor_seek(void *h,uint32_t pos){if(!seek_result)vendor_video_attempts=0;return seek_result;}
#if !CANDIDATE
static int player_extractor_seek(void *h,uint64_t pos){return 0;}
#endif
