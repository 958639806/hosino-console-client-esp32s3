#include <assert.h>
#include <stdlib.h>
#include "hosino_avi_retry_clock.h"

static unsigned checks;
#define CHECK(x) do { checks++; assert(x); } while (0)
static uint8_t data[70000];
static uint32_t used;
static void u32(uint32_t at, uint32_t value)
{ for (unsigned i=0;i<4;i++) data[at+i]=(uint8_t)(value>>(8*i)); }
static void fourcc(const char *id) { memcpy(data+used,id,4);used+=4; }
static uint32_t chunk(const char *id)
{ fourcc(id);uint32_t size=used;u32(used,0);used+=4;return size; }
static void end_chunk(uint32_t size)
{ u32(size,used-size-4);if(used&1)data[used++]=0; }
static uint32_t stream_header(const char *type,const char *codec,uint32_t scale,uint32_t rate)
{
 uint32_t list=chunk("LIST");fourcc("strl");uint32_t h=chunk("strh"),p=used;
 memset(data+p,0,56);used+=56;memcpy(data+p,type,4);memcpy(data+p+4,codec,4);
 u32(p+20,scale);u32(p+24,rate);end_chunk(h);end_chunk(list);return p;
}
static FILE *fixture(void)
{
 FILE *f=fopen("fixture.avi","w+b");CHECK(f!=NULL);CHECK(fwrite(data,1,used,f)==used);return f;
}
static void header_tests(void)
{
 used=0;memset(data,0,sizeof(data));uint32_t riff=chunk("RIFF");fourcc("AVI ");
 uint32_t hdrl=chunk("LIST");fourcc("hdrl");uint32_t junk=chunk("JUNK");data[used++]=1;end_chunk(junk);
 stream_header("auds","\0\0\0\0",1,44100);
 uint32_t video0=stream_header("vids","MJPG",1001,24000);
 uint32_t video1=stream_header("vids","MJPG",1,30);
 end_chunk(hdrl);end_chunk(riff);FILE *f=fixture();hosino_avi_video_rate_t rate={0};
 CHECK(hosino_avi_read_video_rate(f,0,&rate));CHECK(rate.scale==1001&&rate.rate==24000&&rate.stream_id==1);
 CHECK(hosino_avi_read_video_rate(f,1,&rate));CHECK(rate.scale==1&&rate.rate==30&&rate.stream_id==2);
 CHECK(!hosino_avi_read_video_rate(f,2,&rate));fclose(f);
 u32(video0+20,0);f=fixture();CHECK(!hosino_avi_read_video_rate(f,0,&rate));fclose(f);u32(video0+20,1001);
 u32(video0+24,0);f=fixture();CHECK(!hosino_avi_read_video_rate(f,0,&rate));fclose(f);u32(video0+24,24000);
 u32(video0+24,1000);f=fixture();CHECK(!hosino_avi_read_video_rate(f,0,&rate));fclose(f);u32(video0+24,24000);
 memcpy(data+video1+4,"H264",4);f=fixture();CHECK(!hosino_avi_read_video_rate(f,1,&rate));fclose(f);
 u32(video0-4,UINT32_MAX);f=fixture();CHECK(!hosino_avi_read_video_rate(f,0,&rate));fclose(f);
 /* No scanning of payloads or oversized headers, even for valid RIFF magic. */
 used=0;riff=chunk("RIFF");fourcc("AVI ");hdrl=chunk("LIST");fourcc("hdrl");
 for(unsigned i=0;i<520;i++){junk=chunk("JUNK");end_chunk(junk);}
 stream_header("vids","MJPG",1,30);end_chunk(hdrl);end_chunk(riff);
 f=fixture();CHECK(!hosino_avi_read_video_rate(f,0,&rate));fclose(f);
 u32(hdrl,HOSINO_AVI_HEADER_LIMIT);f=fixture();CHECK(!hosino_avi_read_video_rate(f,0,&rate));fclose(f);
 used=11;f=fixture();CHECK(!hosino_avi_read_video_rate(f,0,&rate));fclose(f);
}
static void rational_run(uint32_t scale,uint32_t rate,unsigned frames)
{
 hosino_avi_retry_clock_t clock={.timebase={scale,rate,3},.enabled=true};uint32_t attempts=0;
 for(unsigned i=0;i<frames;i++){
  unsigned retries=(i*13U)%17U;
  uint32_t pos=1000+i*100U;
  for(unsigned j=0;j<retries;j++){
   uint32_t pts=(uint32_t)(((uint64_t)attempts++*scale*1000U)/rate);
   CHECK(hosino_avi_retry_wait(&clock,3,pos,123,pts,false));
  }
  uint32_t pts=(uint32_t)(((uint64_t)attempts++*scale*1000U)/rate);
  CHECK(hosino_avi_retry_correct(&clock,3,pos,123,&pts));
  uint32_t wanted=(uint32_t)(((uint64_t)i*scale*1000U)/rate);
  CHECK(pts>=wanted&&pts-wanted<=1U);CHECK(clock.enabled&&clock.pending_count==0);
 }
}
static void guard_tests(void)
{
 hosino_avi_retry_clock_t clock={.timebase={1001,24000,2},.enabled=true};uint32_t pts=500;
 CHECK(hosino_avi_retry_wait(&clock,1,100,100,0,false));CHECK(clock.pending_count==0);
 CHECK(hosino_avi_retry_correct(&clock,1,100,100,&pts));CHECK(pts==500);
 CHECK(hosino_avi_retry_wait(&clock,2,100,100,0,false));
 CHECK(hosino_avi_retry_wait(&clock,2,100,100,41,false));pts=83;
 CHECK(hosino_avi_retry_correct(&clock,2,100,100,&pts));CHECK(pts<=1&&clock.wait_count==2);
 hosino_avi_retry_seek_reset(&clock);CHECK(clock.enabled&&clock.wait_count==0&&clock.pending_count==0);
 pts=600000;CHECK(hosino_avi_retry_correct(&clock,2,200,100,&pts));CHECK(pts==600000);
 CHECK(hosino_avi_retry_wait(&clock,2,300,100,600041,false));
 pts=600083;CHECK(!hosino_avi_retry_correct(&clock,2,400,100,&pts));CHECK(pts==600083&&!clock.enabled);
 hosino_avi_retry_clear(&clock);CHECK(!clock.enabled&&clock.wait_count==0&&clock.timebase.rate==0);
 clock=(hosino_avi_retry_clock_t){.timebase={1,30,0},.enabled=true};
 CHECK(!hosino_avi_retry_wait(&clock,0,1,100,0,true));CHECK(!clock.enabled);
 clock=(hosino_avi_retry_clock_t){.timebase={1,30,0},.enabled=true,.wait_count=UINT32_MAX};
 CHECK(!hosino_avi_retry_wait(&clock,0,1,100,0,false));CHECK(!clock.enabled);
 clock=(hosino_avi_retry_clock_t){.timebase={UINT32_MAX,UINT32_MAX,0},.enabled=true};
 CHECK(hosino_avi_retry_bias(&clock,UINT32_MAX)==(uint64_t)UINT32_MAX*1000U);
 clock.wait_count=1;pts=0;CHECK(!hosino_avi_retry_correct(&clock,0,1,100,&pts));CHECK(pts==0&&!clock.enabled);
}
int main(void)
{
 header_tests();rational_run(1001,24000,10000);rational_run(1001,30000,10000);
 rational_run(1,30,10000);rational_run(1,60,10000);rational_run(3,2,1000);
 guard_tests();printf("PASS AVI retry clock: %u checks; exact rational cumulative correction, metadata/overflow guards, audio-first/multi-video RIFF bounds\n",checks);return 0;
}
