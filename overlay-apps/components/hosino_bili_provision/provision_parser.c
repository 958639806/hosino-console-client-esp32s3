/* SPDX-License-Identifier: Apache-2.0 */
#include "provision_parser.h"
#include "hosino_network_policy.h"
#include <string.h>
#include <stdio.h>

void bili_provision_clear(void *memory,size_t size)
{
    volatile unsigned char *p=memory;
    while(size--) *p++=0;
}
bili_line_result_t bili_provision_feed(bili_provision_line_t *line,unsigned char byte)
{
    if(byte=='\n') {
        if(line->dropping) {
            const bool recognized=line->recognized;
            bili_provision_clear(line,sizeof(*line));
            return recognized?BILI_LINE_TOO_LONG:BILI_LINE_NONE;
        }
        line->bytes[line->length]=0;
        return BILI_LINE_READY;
    }
    if(line->dropping) return BILI_LINE_NONE;
    if(line->length+1>=sizeof(line->bytes)) {
        line->recognized=strncmp(line->bytes,"BILI_CONFIG ",12)==0;
        line->dropping=true;
        bili_provision_clear(line->bytes,sizeof(line->bytes));
        return BILI_LINE_NONE;
    }
    /* Embedded NUL must never hide a following command or token suffix. */
    if(!byte) {
        line->recognized=strncmp(line->bytes,"BILI_CONFIG ",12)==0;
        line->dropping=true;
        bili_provision_clear(line->bytes,sizeof(line->bytes));
        return BILI_LINE_NONE;
    }
    line->bytes[line->length++]=(char)byte;
    return BILI_LINE_NONE;
}
static void space(const char **p) { while(**p==' '||**p=='\t'||**p=='\r'||**p=='\n') ++*p; }
static int hex(unsigned char c)
{
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    if(c>='A'&&c<='F') return c-'A'+10;
    return -1;
}
/* This protocol has two config strings and an optional16hex request ID. No recursion, arrays,
 * numbers or object allocation; JSON escapes are decoded before policy checks. */
static bool string(const char **cursor,char *out,size_t capacity)
{
    const char *p=*cursor;
    size_t used=0;
    if(*p++!='"') return false;
    while(*p&&*p!='"') {
        unsigned char c=(unsigned char)*p++;
        if(c<32||c>=127) return false;
        if(c=='\\') {
            const unsigned char escape=(unsigned char)*p++;
            switch(escape) {
            case '"':c='"';break; case '\\':c='\\';break;case '/':c='/';break;
            case 'b':c='\b';break;case 'f':c='\f';break;case 'n':c='\n';break;
            case 'r':c='\r';break;case 't':c='\t';break;
            case 'u': {
                unsigned value=0;
                for(unsigned i=0;i<4;++i) { if(!*p) return false;const int digit=hex((unsigned char)*p++);if(digit<0) return false;value=value*16+(unsigned)digit; }
                if(value==0||value>=127) return false;
                c=(unsigned char)value;break;
            }
            default:return false;
            }
        }
        if(!c||used+1>=capacity) return false;
        out[used++]=(char)c;
    }
    if(*p!='"') return false;
    out[used]=0;*cursor=p+1;return true;
}
static bool request_id_valid(const char *id)
{
    if(hosino_network_length(id,17)!=16) return false;
    for(unsigned i=0;i<16;++i) if(hex((unsigned char)id[i])<0) return false;
    return true;
}
bool bili_provision_sync_id(const char *line,char request_id[17])
{
    if(!line||!request_id) return false;
    request_id[0]=0;
    size_t n=hosino_network_length(line,BILI_PROVISION_LINE_CAPACITY);
    if(n&&n<BILI_PROVISION_LINE_CAPACITY&&line[n-1]=='\r') --n;
    if(n!=26||strncmp(line,"BILI_SYNC ",10)) return false;
    memcpy(request_id,line+10,16);request_id[16]=0;
    if(request_id_valid(request_id)) return true;
    bili_provision_clear(request_id,17);return false;
}
bili_provision_result_t bili_provision_parse(const char *line,bili_provision_config_t *config)
{
    char request_id[17];
    return bili_provision_parse_request(line,config,request_id);
}
bili_provision_result_t bili_provision_parse_request(const char *line,bili_provision_config_t *config,char request_id[17])
{
    if(!line||!config||!request_id) return BILI_PROVISION_INVALID;
    bili_provision_clear(config,sizeof(*config));
    bili_provision_clear(request_id,17);
    const size_t length=hosino_network_length(line,BILI_PROVISION_LINE_CAPACITY);
    if(length>=BILI_PROVISION_LINE_CAPACITY) return BILI_PROVISION_TOO_LONG;
    if(strncmp(line,"BILI_CONFIG ",12)) return BILI_PROVISION_IGNORE;
    const char *p=line+12;space(&p);
    if(*p++!='{') return BILI_PROVISION_INVALID;
    bool base=false,token=false,id=false,closed=false;
    for(unsigned count=0;count<3;++count) {
        char key[24];space(&p);
        if(!string(&p,key,sizeof(key))) goto invalid;
        space(&p);if(*p++!=':') goto invalid;space(&p);
        if(!strcmp(key,"base_url")&&!base) { if(!string(&p,config->base,sizeof(config->base))) goto invalid;base=true; }
        else if(!strcmp(key,"device_token")&&!token) { if(!string(&p,config->token,sizeof(config->token))) goto invalid;token=true; }
        else if(!strcmp(key,"request_id")&&!id) {
            if(!string(&p,request_id,17)||!request_id_valid(request_id)) {bili_provision_clear(request_id,17);goto invalid;}
            id=true;
        }
        else goto invalid;
        space(&p);
        if(*p=='}') {++p;closed=true;break;}
        if(count==2||*p++!=',') goto invalid;
    }
    space(&p);if(*p||!closed||!base||!token) goto invalid;
    if(!bili_provision_validate_config(config)) goto invalid;
    return BILI_PROVISION_OK;
invalid:
    bili_provision_clear(config,sizeof(*config));
    return BILI_PROVISION_INVALID;
}
bool bili_provision_validate_config(bili_provision_config_t *config)
{
    if(!config||hosino_network_length(config->base,sizeof(config->base))>=sizeof(config->base)||
       hosino_network_length(config->token,sizeof(config->token))>=sizeof(config->token)) return false;
    size_t n=strlen(config->base);
    while(n>8&&config->base[n-1]=='/') config->base[--n]=0;
    if(n>240||!hosino_network_token_valid(config->token)) return false;
    char probe[320];snprintf(probe,sizeof(probe),"%s/v1/streams/probe",config->base);
    return hosino_network_url_allowed(probe,config->base);
}
const char *bili_provision_ack(bili_provision_result_t result)
{
    switch(result) {
    case BILI_PROVISION_OK:return "BILI_CONFIG_OK";
    case BILI_PROVISION_INVALID:return "BILI_CONFIG_ERROR_INVALID";
    case BILI_PROVISION_TOO_LONG:return "BILI_CONFIG_ERROR_SIZE";
    case BILI_PROVISION_STORAGE:return "BILI_CONFIG_ERROR_STORAGE";
    default:return NULL;
    }
}
