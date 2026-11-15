"""Exercise the verbatim production extractor initializer with both source flags.

This checks configuration wiring only; it does not run native data-cache code.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[3]

def main():
    pipeline=(ROOT/"overlay-apps/components/esp_player_compat/esp_player/src/pipeline/player_pipeline.c").read_text('utf-8')
    match=re.search(r"esp_extractor_config_t extractor_cfg = \{(.*?)\n\s*\};",pipeline,re.S)
    assert match, 'Production initializer missing'
    initializer='esp_extractor_config_t extractor_cfg = {'+match[1]+'\n};'
    http=(ROOT/"overlay-apps/components/gmf_io_compat/gmf_io/esp_gmf_io_http.c").read_text('utf-8')
    assert 'if (cfg->hosino_managed_live) return ESP_GMF_ERR_FAIL;' in http, 'Managed HTTP seek rejection changed'
    native=Path("C:/esp/hosino-qspi/project-apps/managed_components/espressif__esp_extractor/include/esp_extractor.h").read_text('utf-8')
    assert 'Will read data until destination position if not provided' in native
    fixture=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef struct { bool hosino_managed_mjpeg_http; uint8_t av_mask; } stream_t;
typedef struct {
 int (*in_read_cb)(void *,uint32_t,void *);
 int (*in_seek_cb)(uint32_t,void *);
 uint32_t (*in_size_cb)(void *);
 void *in_ctx;
 uint32_t out_pool_size;
 uint16_t out_align;
 uint32_t type;
 uint8_t extract_mask;
} esp_extractor_config_t;
static int _extractor_read(void *b,uint32_t n,void *c){(void)b;(void)c;return (int)n;}
static int _extractor_seek(uint32_t p,void *c){(void)p;(void)c;return 0;}
static int _extractor_managed_seek(uint32_t p,void *c){(void)p;(void)c;return 0;}
static uint32_t _extractor_total_size(void *c){(void)c;return 0;}
static uint32_t player_cfg_extractor_pool_size(stream_t *s,bool v){(void)s;(void)v;return 262144;}
static uint32_t player_current_format(stream_t *s){(void)s;return 0;}
#define EXTRACTOR_OUT_ALIGN 16
static esp_extractor_config_t make(stream_t *stream) {
INITIALIZER
return extractor_cfg;
}
int main(void) {
 stream_t ordinary={false,3},managed={true,3};
 esp_extractor_config_t a=make(&ordinary),b=make(&managed);
 assert(a.in_seek_cb==_extractor_seek && b.in_seek_cb==_extractor_managed_seek);
 assert(a.in_read_cb==_extractor_read && b.in_read_cb==_extractor_read);
 assert(a.in_size_cb==_extractor_total_size && b.in_size_cb==_extractor_total_size);
 assert(a.in_ctx==&ordinary && b.in_ctx==&managed);
 assert(a.type==0 && b.type==0 && a.out_pool_size==b.out_pool_size);
 assert(a.out_align==b.out_align && a.extract_mask==b.extract_mask);
 puts("PRODUCTION_MANAGED_FORWARD_ONLY_WIRING_AND_ORDINARY_SOURCE_PRESERVATION_PASS");return 0;
}
'''.replace('INITIALIZER',initializer)
    compiler=ROOT/"tests/fatfs_dual_format/.tools/tcc-0.9.27/tcc/tcc.exe"
    with tempfile.TemporaryDirectory(prefix="managed_flv_forward_") as folder:
        folder=Path(folder);(folder/'guard.c').write_text(fixture,'utf-8')
        build=subprocess.run([str(compiler),'-Wall','guard.c','-o','guard.exe'],cwd=folder,capture_output=True,text=True)
        print(build.stdout+build.stderr,end='')
        if build.returncode:return build.returncode
        result=subprocess.run([str(folder/'guard.exe')],cwd=folder,capture_output=True,text=True)
        print(result.stdout+result.stderr,end='');return result.returncode

if __name__=='__main__':raise SystemExit(main())
