static esp_player_extractor_t extractor;
static unsigned assertions;
#define CHECK(c) do{assertions++;assert(c);}while(0)

static void setup(void)
{
 memset(&extractor,0,sizeof(extractor));memset(&stream,0,sizeof(stream));memset(frames,0,sizeof(frames));
 memset(diagnostics,0,sizeof(diagnostics));memset(diag_errors,0,sizeof(diag_errors));memset(diag_max,0,sizeof(diag_max));
 frame_count=next_frame=reads=releases=success_releases=queued_audio=queued_video=sleeps=0;
 pool_size=256*1024;pool_used=pool_peak=0;release_fails=uri_fails=false;acquire_result=0;host_now=0;
 inflate_video_pts=false;vendor_video_attempts=0;last_queued_pts=0;
 seek_result=0;
 aq.count=0;vq.count=2;gate.gate_state=0;uri="/sdcard/fake.avi";
 audio.frame_queue=&aq;video.frame_queue=&vq;video.track_info.video_info.format=ESP_FOURCC_MJPG;
 stream=(esp_player_stream_t){.main_state=ESP_PLAYER_STATE_PLAYING,.dec_frame_mode=ESP_PLAYER_DEC_FRAME_MODE_EXTRACTOR,.av_mask=ESP_PLAYER_MASK_AV,.input_handle=(void*)1,.audio_side=&audio,.video_side=&video};
 g_cfg.in_ctx=&stream;ap=(struct port){.next=&vp,.audio=true};vp=(struct port){.next=NULL,.audio=false};extractor.parent.out=&ap;
 extractor.extract_mask=ESP_EXTRACT_MASK_AV;extractor.eos_mask=ESP_EXTRACT_MASK_AV;extractor.extractor_handle=(void*)1;
 extractor.video_stream_num=extractor.audio_stream_num=1;
 extractor.audio_selected_idx=extractor.audio_current_idx=0;extractor.video_selected_idx=extractor.video_current_idx=0;extractor.is_parsed=extractor.is_notify_info=true;
}
static void add_frame(int type,uint32_t bytes,int result,uint8_t flags)
{assert(frame_count<32);frames[frame_count]=(frame_t){.type=type,.bytes=bytes,.result=result,.flags=flags,.pts=frame_count*40};frame_count++;}
static int process(void){return player_extractor_process(&extractor,NULL);}
static void cleanup(void)
{
 for(unsigned i=0;i<frame_count;i++)if(frames[i].owner==QUEUE_OWNED){esp_extractor_frame_info_t f={.frame_buffer=&frames[i].token,.frame_size=frames[i].bytes};CHECK(esp_extractor_release_frame(NULL,&f)==0);}
}
int main(void)
{
 setup();add_frame(2,40000,0,0);add_frame(2,42000,0,0);add_frame(1,3528,0,0);
 for(unsigned i=0;i<3;i++)CHECK(process()!=ESP_GMF_JOB_ERR_FAIL);
#if !CANDIDATE
 CHECK(reads==0);CHECK(queued_audio==0);CHECK(sleeps==3);CHECK(diagnostics[HOSINO_PLAYER_DIAG_DEMUX_AUDIO_HOLD]==3);
 printf("PASS baseline: audio dry + video head causes three holds, zero demux reads, missing later audio remains unreachable (%u checks)\n",assertions);return 0;
#else
 /* Opt-in bridge FLV gets the same MJPEG pressure policy. Unknown network
  * sources and non-MJPEG still retain their existing conservative policy. */
 setup();uri="https://bridge.example.test/bili/v1/streams/test";
 CHECK(!player_extractor_local_mjpeg_av(&extractor,&extractor));
 stream.hosino_managed_mjpeg_http=true;
 CHECK(player_extractor_local_mjpeg_av(&extractor,&extractor));
 video.track_info.video_info.format=0;
 CHECK(!player_extractor_local_mjpeg_av(&extractor,&extractor));
 setup();uri="https://bridge.example.test/bili/v1/streams/test";stream.hosino_managed_mjpeg_http=true;
 add_frame(2,40000,0,0);add_frame(2,42000,0,0);add_frame(1,3528,0,0);
 for(unsigned i=0;i<3;i++)CHECK(process()!=ESP_GMF_JOB_ERR_FAIL);
 CHECK(reads==3&&queued_audio==1&&queued_video==0&&success_releases==2);
 cleanup();CHECK(pool_used==0);
 /* Re-run the unchanged local case after the opt-in network scenario. */
 setup();add_frame(2,40000,0,0);add_frame(2,42000,0,0);add_frame(1,3528,0,0);
 for(unsigned i=0;i<3;i++)CHECK(process()!=ESP_GMF_JOB_ERR_FAIL);
 CHECK(reads==3);CHECK(sleeps==0);CHECK(success_releases==2);CHECK(queued_audio==1);CHECK(queued_video==0);
 CHECK(frames[0].owner==RELEASED&&frames[1].owner==RELEASED&&frames[2].owner==QUEUE_OWNED);
 CHECK(diagnostics[HOSINO_PLAYER_DIAG_DEMUX_VIDEO_DROP_AUDIO_DRY]==2);CHECK(diag_max[HOSINO_PLAYER_DIAG_DEMUX_VIDEO_PACKET]==42000);
 cleanup();CHECK(pool_used==0);CHECK(success_releases==3);
 /* A queued frame retains its producer reference until consumer cleanup. */
 setup();aq.count=1;add_frame(2,40000,0,0);CHECK(process()==0);CHECK(queued_video==1&&success_releases==0);CHECK(frames[0].owner==QUEUE_OWNED);cleanup();CHECK(success_releases==1);
 /* Release failure is fatal once, never queued or routed to port cleanup. */
 setup();release_fails=true;add_frame(2,40000,0,0);CHECK(process()==ESP_GMF_JOB_ERR_FAIL);CHECK(releases==1&&success_releases==0);CHECK(!queued_video&&!queued_audio);CHECK(diag_errors[HOSINO_PLAYER_DIAG_DEMUX_VIDEO_DROP_AUDIO_DRY]==1);CHECK(frames[0].owner==EXTRACTOR_OWNED);
 /* EOS video, final extractor result and every audio packet bypass discard. */
 setup();add_frame(2,40000,0,EXTRACTOR_FRAME_FLAG_EOS);CHECK(process()==0);CHECK(queued_video==1&&releases==0);CHECK(!(extractor.eos_mask&ESP_EXTRACT_MASK_VIDEO));cleanup();
 setup();add_frame(1,3528,0,0);CHECK(process()==0);CHECK(queued_audio==1&&releases==0);CHECK(diagnostics[HOSINO_PLAYER_DIAG_DEMUX_VIDEO_DROP_AUDIO_DRY]==0);cleanup();
 setup();esp_extractor_frame_info_t final={.stream_type=2,.frame_buffer=(uint8_t*)1,.frame_size=1};CHECK(!player_extractor_should_discard_video(&extractor,&extractor,&final,ESP_EXTRACTOR_ERR_EOS));
 setup();extractor.eos_mask=ESP_EXTRACT_MASK_VIDEO;add_frame(2,40000,ESP_EXTRACTOR_ERR_EOS,0);CHECK(process()==ESP_GMF_JOB_ERR_DONE);CHECK(queued_video==1&&releases==0);CHECK(extractor.eos_mask==0);cleanup();
 setup();add_frame(0,0,ESP_EXTRACTOR_ERR_EOS,0);CHECK(process()==ESP_GMF_JOB_ERR_DONE);CHECK(queued_audio==1&&releases==0);CHECK(!diagnostics[HOSINO_PLAYER_DIAG_DEMUX_VIDEO_DROP_AUDIO_DRY]);
 /* Audio EOS must preserve the video-only tail despite the empty audio queue. */
 setup();extractor.eos_mask=ESP_EXTRACT_MASK_VIDEO;add_frame(2,40000,0,0);CHECK(process()==0);CHECK(queued_video==1&&releases==0);cleanup();
 /* Seek, stop, network, inter-frame codec, gate and pause retain their guards. */
 setup();stream.is_seeking=true;add_frame(2,40000,0,0);CHECK(process()==0);CHECK(queued_video==1&&releases==0);cleanup();
 setup();stream._is_stop=true;add_frame(2,40000,0,0);CHECK(process()==ESP_GMF_IO_ABORT);CHECK(releases==1&&!queued_video);CHECK(!diagnostics[HOSINO_PLAYER_DIAG_DEMUX_VIDEO_DROP_AUDIO_DRY]);
 setup();uri="https://example.invalid/fake.avi";add_frame(2,40000,0,0);CHECK(process()==ESP_GMF_JOB_ERR_CONTINUE);CHECK(!reads&&sleeps==1);
 setup();video.track_info.video_info.format=ESP_FOURCC_H264;add_frame(2,40000,0,0);CHECK(process()==ESP_GMF_JOB_ERR_CONTINUE);CHECK(!reads&&sleeps==1);
 setup();stream.buffer_ctrl=&gate;gate.gate_state=1;add_frame(2,40000,0,0);CHECK(process()==0);CHECK(queued_video==1&&releases==0);cleanup();
 setup();stream.main_state=ESP_PLAYER_STATE_PAUSED;add_frame(2,40000,0,0);CHECK(process()==ESP_GMF_JOB_ERR_CONTINUE);CHECK(!reads&&sleeps==1);
 setup();stream.main_state=ESP_PLAYER_STATE_PREPARING;add_frame(2,40000,0,0);CHECK(process()==ESP_GMF_JOB_ERR_CONTINUE);CHECK(!reads&&sleeps==1);
 /* Unknown URI/IO errors cannot silently enable a new drop policy. */
 setup();uri="custom:opaque";CHECK(!player_extractor_local_mjpeg_av(&extractor,&extractor));
 setup();uri_fails=true;CHECK(!player_extractor_local_mjpeg_av(&extractor,&extractor));
 setup();uri="FiLe://fake.avi";CHECK(player_extractor_local_mjpeg_av(&extractor,&extractor));
 setup();stream.av_mask=ESP_EXTRACT_MASK_AUDIO;CHECK(!player_extractor_local_mjpeg_av(&extractor,&extractor));
 setup();stream.av_mask=ESP_EXTRACT_MASK_VIDEO;CHECK(!player_extractor_local_mjpeg_av(&extractor,&extractor));
 setup();stream.dec_frame_mode=ESP_PLAYER_DEC_FRAME_MODE_FILL;CHECK(!player_extractor_local_mjpeg_av(&extractor,&extractor));
 setup();extractor.audio_selected_idx=-1;CHECK(!player_extractor_local_mjpeg_av(&extractor,&extractor));
 /* The same read-before-drop policy needs headroom for a candidate frame. */
 setup();pool_size=100000;pool_used=80000;add_frame(2,40000,0,0);CHECK(process()==ESP_GMF_JOB_ERR_CONTINUE);CHECK(sleeps==1&&next_frame==0&&releases==0);CHECK(diagnostics[HOSINO_PLAYER_DIAG_DEMUX_POOL_WAIT]==1);
 setup();pool_used=80000;add_frame(2,40000,0,0);add_frame(1,3528,0,0);CHECK(process()==ESP_GMF_JOB_ERR_CONTINUE);CHECK(success_releases==1&&pool_used==80000);CHECK(process()==0);CHECK(queued_audio==1);CHECK(pool_peak<=pool_size);cleanup();CHECK(pool_used==80000);
 /* Fake the pinned AVI preview before allocation: every unsuccessful attempt
  * advances its raw counter although the same chunk is still unread. The real
  * production process must compensate before queueing or intentionally shedding. */
 setup();inflate_video_pts=true;aq.count=1;pool_size=100000;pool_used=80000;
 extractor.avi_retry_checked=true;extractor.avi_retry_selected_idx=0;
 extractor.avi_retry_clock=(hosino_avi_retry_clock_t){.enabled=true,.timebase={1001,24000,0}};
 add_frame(2,40000,0,0);add_frame(2,40000,0,0);add_frame(2,40000,0,0);add_frame(1,3528,0,0);add_frame(2,40000,0,0);
 CHECK(process()==ESP_GMF_JOB_ERR_CONTINUE);CHECK(process()==ESP_GMF_JOB_ERR_CONTINUE);
 CHECK(extractor.avi_retry_clock.pending_count==2&&releases==0&&next_frame==0);
 pool_used=0;CHECK(process()==0);CHECK(last_queued_pts<=1);CHECK(extractor.avi_retry_clock.wait_count==2);
 CHECK(process()==0);CHECK(last_queued_pts>=41&&last_queued_pts<=42);
 aq.count=0;pool_size=256*1024;CHECK(process()==ESP_GMF_JOB_ERR_CONTINUE);CHECK(frames[2].owner==RELEASED);
 CHECK(extractor.last_pts>=83&&extractor.last_pts<=84);
 CHECK(process()==0);CHECK(last_queued_pts==120); /* Audio PTS untouched. */
 aq.count=1;pool_size=256*1024;CHECK(process()==0);CHECK(last_queued_pts>=125&&last_queued_pts<=126);
 CHECK(extractor.avi_retry_clock.wait_count==2&&extractor.avi_retry_clock.enabled);cleanup();
 CHECK(pool_used==0);
 /* A single parser retains its counter across track enables. Never lose its
  * accumulated bias, and keep observing video if audio is temporarily off. */
 extractor.video_selected_idx=-1;player_extractor_avi_retry_prepare(&extractor,&extractor);
 CHECK(extractor.avi_retry_clock.enabled&&extractor.avi_retry_clock.wait_count==2);
 extractor.video_selected_idx=0;extractor.audio_selected_idx=-1;
 player_extractor_avi_retry_prepare(&extractor,&extractor);
 CHECK(extractor.avi_retry_clock.enabled&&extractor.avi_retry_clock.wait_count==2);
 extractor.audio_selected_idx=0;player_extractor_avi_retry_prepare(&extractor,&extractor);
 CHECK(extractor.avi_retry_clock.wait_count==2);
 setup();extractor.video_stream_num=2;player_extractor_avi_retry_prepare(&extractor,&extractor);
 CHECK(extractor.avi_retry_checked&&!extractor.avi_retry_clock.enabled);
 setup();extractor.audio_selected_idx=-1;player_extractor_avi_retry_prepare(&extractor,&extractor);
 CHECK(!extractor.avi_retry_checked); /* Startup eligibility can become ready. */
 /* Once native reads have advanced, late eligibility must not assume W==0. */
 add_frame(2,40000,0,0);aq.count=1;pool_size=100000;pool_used=80000;
 CHECK(process()==ESP_GMF_JOB_ERR_CONTINUE);CHECK(extractor.avi_retry_read_started);
 extractor.audio_selected_idx=0;player_extractor_avi_retry_prepare(&extractor,&extractor);
 CHECK(extractor.avi_retry_checked&&!extractor.avi_retry_clock.enabled);
 extractor.avi_retry_clock=(hosino_avi_retry_clock_t){.enabled=true,.timebase={1001,24000,0},.wait_count=2};
 seek_result=-1;CHECK(player_extractor_seek(&extractor,1000)==ESP_GMF_ERR_FAIL);
 CHECK(extractor.avi_retry_clock.wait_count==2&&extractor.avi_retry_read_started);
 seek_result=0;CHECK(player_extractor_seek(&extractor,1000)==ESP_GMF_ERR_OK);
 CHECK(extractor.avi_retry_clock.enabled&&extractor.avi_retry_clock.wait_count==0);
 CHECK(!extractor.avi_retry_read_started&&!extractor.avi_retry_checked);
 CHECK(player_extractor_ops_close(&extractor,NULL)==ESP_GMF_JOB_ERR_OK);
 CHECK(!extractor.avi_retry_clock.enabled&&!extractor.avi_retry_read_started&&!extractor.is_parsed);
 printf("PASS candidate: %u checks; actual demux progress, exact ownership, EOS/audio/seek/stop/gate/codec/source guards and bounded-pool headroom\n",assertions);
 return 0;
#endif
}
