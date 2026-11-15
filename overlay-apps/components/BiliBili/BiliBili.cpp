// SPDX-License-Identifier: Apache-2.0
#include "BiliBili.hpp"
#include "BiliClient.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include "esp_heap_caps.h"
#include "hosino_ui_fonts.h"
#include "hosino_chat_fonts.h"

LV_IMAGE_DECLARE(bili_icon);
namespace esp_brookesia::apps {
namespace {
constexpr uint32_t Background=0x101218, Surface=0x21242e, Pink=0xfb7299, Muted=0xabb0c0;
void copyText(char *to,size_t size,const char *from) { strlcpy(to,from?from:"",size); }
const lv_font_t *bodyFont() { return &hosino_chat_font_16; }
// HOST_NOTICE_BOUNDARY_BEGIN
static bool shouldClearOldNotice(const char *visible,const char *previousModelError) {
    if(!previousModelError[0]||strcmp(visible,previousModelError)) return false;
    /* These four explicit action-success notices share Model.error for legacy
     * compatibility. A later successful request must not erase their feedback. */
    static const char *success[]={"设置已保存，返回后刷新首页","已退出B站登录","已从收藏夹移除","已添加到收藏夹"};
    for(size_t i=0;i<sizeof(success)/sizeof(success[0]);++i) if(!strcmp(visible,success[i])) return false;
    return true;
}
// HOST_NOTICE_BOUNDARY_END
// HOST_PRESENTATION_BOUNDARY_BEGIN
static bool pointerBusy() {
    for(lv_indev_t *input=lv_indev_get_next(nullptr);input;input=lv_indev_get_next(input))
        if(lv_indev_get_type(input)==LV_INDEV_TYPE_POINTER &&
                (lv_indev_get_state(input)==LV_INDEV_STATE_PRESSED||lv_indev_get_scroll_obj(input))) return true;
    return false;
}
static bool presentationBusy(lv_obj_t *body) { return pointerBusy()||(body&&lv_obj_is_scrolling(body)); }
static bool mayApplySnapshot(bool pressed,bool playerPage) { return !pressed||playerPage; }
static bool mayRebuild(bool pressed) { return !pressed; }
// HOST_PRESENTATION_BOUNDARY_END
const lv_font_t *withFallback(const lv_font_t *font) {
    static const lv_font_t large=[] { auto f=hosino_font_20;f.fallback=bodyFont();return f; }();
    static const lv_font_t small=[] { auto f=hosino_font_14;f.fallback=bodyFont();return f; }();
    if(font==&hosino_font_20) return &large;
    if(font==&hosino_font_14) return &small;
    return font?font:bodyFont();
}
}
BiliBili *BiliBili::_instance=nullptr;
BiliBili *BiliBili::requestInstance(bool status,bool navigation) {
    if(!_instance) _instance=new(std::nothrow) BiliBili(status,navigation);
    return _instance;
}
BiliBili::BiliBili(bool status,bool navigation):App("哔哩哔哩",&bili_icon,true,status,navigation) {}
BiliBili::~BiliBili() { close(); heap_caps_free(_model); }
void BiliBili::setMediaHooks(const BiliMediaHooks &hooks) { if(!_worker||_worker->done.load()) _hooks=hooks; }
bool BiliBili::init() { return true; }
bool BiliBili::deinit() { return close(); }
bool BiliBili::cleanResource() { return close(); }
bool BiliBili::ensureWorker() {
    if(!_model) {
        void *raw=heap_caps_calloc(1,sizeof(bili::Model),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!raw) return false;
        _model=new(raw) bili::Model;
    }
    if(_worker&&_worker->stop.load()) {
        if(!_worker->done.load()) { showNotice("正在结束上次播放，请稍候"); return false; }
        _worker.reset();
    }
    if(!_worker) _worker=bili::Worker::create(_hooks);
    return static_cast<bool>(_worker);
}
bool BiliBili::run() {
    _foreground=true; _view=View::Home; _revision=0; _rebuild=true; _page=1; _loaded=false; _waitingFor=0;
    auto *screen=lv_screen_active(); lv_obj_set_style_bg_color(screen,lv_color_hex(Background),0);
    lv_obj_remove_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    const auto &area=getVisualArea();
    _width=std::max(1,static_cast<int>(lv_area_get_width(&area))-56);
    _height=std::max(1,static_cast<int>(lv_area_get_height(&area))-40);
    _root=lv_obj_create(screen); lv_obj_remove_style_all(_root);
    lv_obj_set_pos(_root,28,8); lv_obj_set_size(_root,_width,_height);
    lv_obj_remove_flag(_root,LV_OBJ_FLAG_SCROLLABLE);
    if(!ensureWorker()) copyText(_noticeText,sizeof(_noticeText),"应用内存不足，或上次播放仍在退出");
    else _worker->foreground.store(true);
    render();
    _timer=lv_timer_create(tick,50,this);
    return _timer!=nullptr;
}
bool BiliBili::pause() {
    if(_view==View::Search&&_edit) copyText(_query,sizeof(_query),lv_textarea_get_text(_edit));
    if(_waitingFor) { _waitingFor=0; _view=View::Home; }
    _foreground=false;
    if(_timer) lv_timer_pause(_timer);
    detachCanvas();
    if(_worker) { _worker->foreground.store(false); _worker->frames.store(false); }
    return true;
}
bool BiliBili::resume() {
    _foreground=true;
    if(_worker) _worker->foreground.store(true);
    if(_view==View::Player||_view==View::Options) _view=View::Detail;
    _rebuild=true;
    if(_view==View::Home) refresh();
    if(_timer) { lv_timer_resume(_timer); lv_timer_ready(_timer); }
    return true;
}
bool BiliBili::close() {
    pause();
    if(_worker) { _worker->stop.store(true); _worker->frames.store(false); }
    // Brookesia owns and deletes timers created in run().
    _timer=nullptr; _root=_body=_notice=_canvas=_edit=_tokenEdit=_keyboard=_ime=nullptr;
    _time=_progress=_playLabel=_playerControls=nullptr;
    return true;
}
bool BiliBili::back() {
    if(_presentedView==View::Home) return notifyCoreClosed();
    action(Action::Back,0); return true;
}
void BiliBili::detachCanvas() {
    if(_canvas&&lv_obj_is_valid(_canvas)) lv_canvas_set_buffer(_canvas,&_placeholder,1,1,LV_COLOR_FORMAT_RGB565);
    if(_worker&&xSemaphoreTake(_worker->mutex,pdMS_TO_TICKS(10))==pdTRUE) {
        _worker->displayed=-1; _worker->pending=-1; xSemaphoreGive(_worker->mutex);
    }
}
bool BiliBili::send(bili::Command &cmd) {
    cmd.serial=++_nextSerial;
    if(!_worker||!_worker->submit(cmd)) { showNotice("操作仍在处理中，请稍候再试"); return false; }
    if(cmd.op==bili::Op::Feed||cmd.op==bili::Op::Search||cmd.op==bili::Op::Detail||cmd.op==bili::Op::Me||cmd.op==bili::Op::Folders||cmd.op==bili::Op::Favorites||cmd.op==bili::Op::Qr) {
        _waitingFor=cmd.serial; _rebuild=true;
    }
    return true;
}
void BiliBili::showNotice(const char *message) {
    copyText(_noticeText,sizeof(_noticeText),message);
    if(_notice&&lv_obj_is_valid(_notice)) lv_label_set_text(_notice,_noticeText);
}
lv_obj_t *BiliBili::label(lv_obj_t *parent,const char *message,int x,int y,int width,const lv_font_t *font) {
    auto *object=lv_label_create(parent); lv_label_set_text(object,message);
    lv_obj_set_pos(object,x,y); lv_obj_set_width(object,width);
    lv_obj_set_style_text_font(object,withFallback(font),0);
    lv_obj_set_style_text_color(object,lv_color_hex(0xf3f5fa),0);
    return object;
}
lv_obj_t *BiliBili::button(lv_obj_t *parent,const char *message,int x,int y,int width,int height,Action a,int value,bool accent) {
    auto *object=lv_button_create(parent); lv_obj_set_pos(object,x,y); lv_obj_set_size(object,width,height);
    lv_obj_set_style_radius(object,12,0); lv_obj_set_style_bg_color(object,lv_color_hex(accent?Pink:Surface),0);
    lv_obj_set_style_border_width(object,0,0); lv_obj_set_style_pad_all(object,6,0);
    lv_obj_set_style_shadow_width(object,0,0);
    lv_obj_set_user_data(object,reinterpret_cast<void *>((static_cast<uintptr_t>(a)<<16)|(static_cast<uint16_t>(value))));
    lv_obj_add_event_cb(object,event,LV_EVENT_CLICKED,this);
    auto *title=label(object,message,0,0,width-12); lv_label_set_long_mode(title,LV_LABEL_LONG_MODE_DOTS);
    lv_obj_center(title); lv_obj_set_style_text_align(title,LV_TEXT_ALIGN_CENTER,0);
    return object;
}
void BiliBili::event(lv_event_t *e) {
    auto *self=static_cast<BiliBili *>(lv_event_get_user_data(e));
    if(!self||!self->_foreground) return;
    auto *target=static_cast<lv_obj_t *>(lv_event_get_target(e));
    if(target==self->_canvas) {
        self->_chromeUntil=lv_tick_get()+4000;
        if(self->_playerControls) lv_obj_remove_flag(self->_playerControls,LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const uintptr_t code=reinterpret_cast<uintptr_t>(lv_obj_get_user_data(target));
    const auto action=static_cast<Action>(code>>16);
    self->action(action,action==Action::Volume?lv_slider_get_value(target):static_cast<int16_t>(code&0xffff));
}
void BiliBili::tick(lv_timer_t *timer) {
    auto *self=static_cast<BiliBili *>(lv_timer_get_user_data(timer));
    if(!self||!self->_foreground) return;
    if(!self->_worker||self->_worker->stop.load()) self->ensureWorker();
    const int oldProfile=self->_model?self->_model->profile:-1, oldRate=self->_model?self->_model->bitrate:-1;
    char previousModelError[sizeof(self->_noticeText)]{};
    if(self->_model) copyText(previousModelError,sizeof(previousModelError),self->_model->error);
    const bool pressed=presentationBusy(self->_body);
    /* Buttons carry row indices into the PRESENTED model. Keep the model and
     * its widgets together until release; merely delaying lv_obj_clean would
     * let an old visible card select a new row at that index. Player has no
     * indexed cards and keeps its clock/volume/progress snapshots live. */
    if(self->_worker&&self->_model&&mayApplySnapshot(pressed,self->_view==View::Player&&self->_presentedView==View::Player)&&self->_worker->copy(*self->_model)) {
        const auto &m=*self->_model;
        if(self->_waitingFor&&m.completed_serial>=self->_waitingFor) { self->_waitingFor=0; self->_rebuild=true; }
        if(m.revision!=self->_revision) {
            self->_revision=m.revision;
            if(m.error[0]) self->showNotice(m.error);
            else if(shouldClearOldNotice(self->_noticeText,previousModelError)) self->showNotice("");
            if(!m.busy) {
                if(self->_view!=View::Player&&self->_view!=View::Options&&self->_view!=View::Settings&&self->_view!=View::Search) {
                    // A completed QR poll updates only its status. Preserve the QR
                    // and touch targets until the login state actually changes.
                    if(self->_view==View::Login&&m.qr_url[0]&&!strcmp(self->_qrRendered,m.qr_url)) {
                        if(!strcmp(m.qr_status,"confirmed")) self->showNotice("登录成功，请返回查看个人信息");
                        else if(!strcmp(m.qr_status,"scanned")) self->showNotice("已扫码，请在手机B站确认");
                        else if(!strcmp(m.qr_status,"expired")) self->showNotice("二维码已过期，请刷新");
                    } else self->_rebuild=true;
                }
                if(self->_view==View::Options&&(oldProfile!=m.profile||oldRate!=m.bitrate)) self->_rebuild=true;
            }
        }
        if(m.ready&&!self->_loaded) { self->_loaded=true; if(m.configured) self->refresh(); }
        if(m.ready&&!m.configured&&self->_view==View::Home) { self->_view=View::Settings; self->_rebuild=true; }
        if(self->_view==View::Login&&m.qr_url[0]&&strcmp(m.qr_status,"expired")&&strcmp(m.qr_status,"confirmed")&&!m.busy&&lv_tick_get()>=self->_qrNext) {
            bili::Command cmd; cmd.op=bili::Op::PollQr;
            if(self->send(cmd)) self->_qrNext=lv_tick_get()+3000;
        }
    }
    if(self->_rebuild&&mayRebuild(pressed)) self->render();
    if(self->_view==View::Player) self->playerUi();
}
void BiliBili::navigate(View view) {
    if(_view==View::Player&&view!=View::Options) detachCanvas();
    _view=view; _rebuild=true;
}
void BiliBili::refresh() {
    bili::Command cmd; cmd.page=_page;
    if(_view==View::Home) cmd.op=bili::Op::Feed;
    else if(_view==View::Videos) {
        cmd.op=_folder?bili::Op::Favorites:(_searchList?bili::Op::Search:bili::Op::Feed);
        cmd.folder=_folder; copyText(cmd.text,sizeof(cmd.text),_query);
    } else if(_view==View::Profile) cmd.op=bili::Op::Me;
    else if(_view==View::Folders||_view==View::FavoritePick) cmd.op=bili::Op::Folders;
    else return;
    send(cmd);
}
void BiliBili::render() {
    if(!mayRebuild(presentationBusy(_body))) { _rebuild=true; return; }
    if(!_root||!_foreground) return;
    const bool restoreScroll=_body&&_view==_presentedView&&_page==_presentedPage&&_folder==_presentedFolder;
    const int32_t scrollX=restoreScroll?lv_obj_get_scroll_x(_body):0;
    const int32_t scrollY=restoreScroll?lv_obj_get_scroll_y(_body):0;
    _presentedView=_view;_presentedPage=_page;_presentedFolder=_folder;
    _rebuild=false; detachCanvas();
    // The IME owns its keyboard and candidate panel.
    if(_ime) { lv_obj_delete(_ime); _ime=nullptr; _keyboard=nullptr; }
    lv_obj_clean(_root); _canvas=_edit=_tokenEdit=_keyboard=_body=_notice=nullptr;
    _time=_progress=_playLabel=_playerControls=nullptr;
    button(_root,_view==View::Home?"刷新":"返回",0,0,62,38,_view==View::Home?Action::Home:Action::Back);
    const char *title="哔哩哔哩";
    switch(_view) {
    case View::Videos:title=_folder?"收藏视频":"搜索结果";break;
    case View::Detail:title="视频详情";break; case View::Profile:title="我的";break;
    case View::Folders:title="收藏夹";break; case View::FavoritePick:title="收藏到…";break;
    case View::Login:title="扫码登录";break; case View::Settings:title="桥接设置";break;
    case View::Search:title="全键拼音搜索";break; case View::Player:title="正在播放";break;
    case View::Options:title="播放设置";break; default:break;
    }
    label(_root,title,74,8,_width-160,&hosino_font_20);
    if(_view==View::Home) button(_root,"我的",_width-70,0,70,38,Action::Profile);
    else if(_view==View::Profile) button(_root,"设置",_width-70,0,70,38,Action::Settings);
    else if(_view==View::Settings) button(_root,"保存",_width-70,0,70,38,Action::Save,0,true);
    else if(_view==View::Search) button(_root,"搜索",_width-70,0,70,38,Action::Query,0,true);
    _notice=label(_root,_noticeText,0,_height-26,_width,&hosino_font_14);
    lv_obj_set_style_text_color(_notice,lv_color_hex(Muted),0); lv_obj_set_height(_notice,24);
    lv_label_set_long_mode(_notice,LV_LABEL_LONG_MODE_DOTS);
    _body=lv_obj_create(_root); lv_obj_remove_style_all(_body);
    lv_obj_set_pos(_body,0,46); lv_obj_set_size(_body,_width,std::max(1,_height-78));
    lv_obj_set_scroll_dir(_body,LV_DIR_VER); lv_obj_set_style_pad_bottom(_body,10,0);
    if(!_model) { label(_body,"内存不足，请先关闭其他应用",0,20,_width); return; }
    const auto &m=*_model;
    if(_waitingFor&&_view!=View::Player&&_view!=View::Options&&_view!=View::Search&&_view!=View::Settings) {
        label(_body,"正在读取…",0,20,_width); return;
    }
    if(_view==View::Home||_view==View::Videos||_view==View::Folders||_view==View::FavoritePick) {
        int y=0;
        if(_view==View::Home) { button(_body,"搜索视频 / BV号",0,0,_width,48,Action::Search,0,true); y=58; }
        if(!m.ready) label(_body,"正在加载连接配置…",0,y,_width);
        else if(!m.count) label(_body,m.busy?"正在读取…":"暂无内容；点刷新或检查连接设置",0,y,_width);
        else for(unsigned i=0;i<m.count;++i) {
            const auto &row=m.rows[i];
            const bool folder=_view==View::Folders||_view==View::FavoritePick;
            auto *card=button(_body,row.video.title,0,y,_width,76,folder?Action::PickFolder:Action::Select,i);
            auto *name=lv_obj_get_child(card,0); lv_obj_set_pos(name,10,4); lv_obj_set_width(name,_width-32);
            lv_obj_set_height(name,40); lv_label_set_long_mode(name,LV_LABEL_LONG_MODE_WRAP); lv_obj_set_style_text_align(name,LV_TEXT_ALIGN_LEFT,0);
            char subtitle[144];
            if(folder) snprintf(subtitle,sizeof(subtitle),"%u 个视频",row.count);
            else snprintf(subtitle,sizeof(subtitle),"%s · %d:%02d",row.video.author,static_cast<int>(row.video.duration)/60,static_cast<int>(row.video.duration)%60);
            auto *sub=label(card,subtitle,10,46,_width-32,&hosino_font_14); lv_obj_set_style_text_color(sub,lv_color_hex(Muted),0);
            y+=84;
        }
        char page[40]; snprintf(page,sizeof(page),"第 %u 页",m.page); label(_body,page,76,y+10,_width-152);
        if(m.page>1) button(_body,"上一页",0,y,84,40,Action::Previous);
        if(m.more) button(_body,"下一页",_width-84,y,84,40,Action::Next);
    } else if(_view==View::Detail) {
        if(!m.selected.bvid[0]||strcmp(m.selected.bvid,_detailBvid)) { label(_body,"正在获取视频详情…",0,20,_width); return; }
        auto *name=label(_body,m.selected.title,0,0,_width,&hosino_font_20); lv_label_set_long_mode(name,LV_LABEL_LONG_MODE_WRAP);
        lv_obj_update_layout(name); int y=lv_obj_get_height(name)+12;
        label(_body,m.selected.author,0,y,_width); y+=30;
        button(_body,"播放",0,y,(_width-8)/2,46,Action::Play,0,true);
        button(_body,"收藏",(_width+8)/2,y,(_width-8)/2,46,Action::Favorite); y+=58;
        if(_folder) { button(_body,"从当前收藏夹移除",0,y,_width,42,Action::Remove); y+=52; }
        for(unsigned i=0;i<m.part_count;++i) { char line[168]; snprintf(line,sizeof(line),"P%u  %s",i+1,m.parts[i].title); button(_body,line,0,y,_width,46,Action::Part,i); y+=54; }
    } else if(_view==View::Profile) {
        if(!m.logged_in) { label(_body,"扫码登录后查看个人信息和收藏夹",0,8,_width); button(_body,"使用手机B站扫码",0,58,_width,50,Action::Login,0,true); }
        else {
            label(_body,m.name,0,8,_width,&hosino_font_20);
            char line[180]; snprintf(line,sizeof(line),"UID %llu · LV%d\n%s\n硬币 %.1f",static_cast<unsigned long long>(m.mid),m.level,m.vip,m.coins);
            label(_body,line,0,44,_width);
            button(_body,"我的收藏夹",0,130,_width,50,Action::Folders,0,true);
            button(_body,"退出B站登录",0,194,_width,44,Action::Logout);
        }
    } else if(_view==View::Login) {
#if LV_USE_QRCODE
        if(m.qr_url[0]) {
            copyText(_qrRendered,sizeof(_qrRendered),m.qr_url);
            auto *qr=lv_qrcode_create(_body); const int size=std::min(224,std::max(80,_height-160));
            lv_qrcode_set_size(qr,size); lv_qrcode_set_dark_color(qr,lv_color_hex(0x000000)); lv_qrcode_set_light_color(qr,lv_color_hex(0xffffff));
            lv_obj_set_pos(qr,(_width-size)/2,0);
            if(lv_qrcode_update(qr,m.qr_url,strlen(m.qr_url))!=LV_RESULT_OK) showNotice("二维码过长，无法显示；请刷新");
            button(_body,"刷新二维码",0,size+12,_width,42,Action::Login);
        } else label(_body,"正在申请登录二维码…",0,20,_width);
#else
        label(_body,"固件未启用二维码组件",0,20,_width);
#endif
    } else if(_view==View::Settings||_view==View::Search) {
        const bool settings=_view==View::Settings;
        _edit=lv_textarea_create(_body); lv_obj_set_pos(_edit,0,0); lv_obj_set_size(_edit,_width,44);
        lv_textarea_set_one_line(_edit,true); lv_textarea_set_max_length(_edit,settings?240:80);
        lv_obj_set_style_text_font(_edit,bodyFont(),0);
        lv_textarea_set_placeholder_text(_edit,settings?"https://域名/bili":"输入拼音，点候选字后搜索");
        lv_textarea_set_text(_edit,settings?m.base:_query);
        if(settings) {
            _tokenEdit=lv_textarea_create(_body); lv_obj_set_pos(_tokenEdit,0,48); lv_obj_set_size(_tokenEdit,_width,44);
            lv_textarea_set_one_line(_tokenEdit,true); lv_textarea_set_password_mode(_tokenEdit,true); lv_textarea_set_max_length(_tokenEdit,192);
            lv_textarea_set_placeholder_text(_tokenEdit,m.token_present?"已保存令牌；留空则保留":"设备令牌（不是B站密码）");
            lv_obj_set_style_text_font(_tokenEdit,bodyFont(),0);
        }
        _keyboard=lv_keyboard_create(_body); lv_obj_set_align(_keyboard,LV_ALIGN_TOP_LEFT);
        const int keyboardY=settings?100:96;
        lv_obj_set_pos(_keyboard,0,keyboardY); lv_obj_set_size(_keyboard,_width,std::max(100,_height-78-keyboardY));
        lv_obj_set_style_pad_all(_keyboard,0,0); lv_obj_set_style_pad_gap(_keyboard,2,0);
        lv_obj_set_style_text_font(_keyboard,bodyFont(),LV_PART_ITEMS); lv_keyboard_set_textarea(_keyboard,_edit);
        if(settings) {
            // Focus events switch the same keyboard between URL and masked token.
            lv_obj_add_event_cb(_edit,[](lv_event_t *e){auto *self=static_cast<BiliBili *>(lv_event_get_user_data(e));lv_keyboard_set_textarea(self->_keyboard,self->_edit);},LV_EVENT_FOCUSED,this);
            lv_obj_add_event_cb(_tokenEdit,[](lv_event_t *e){auto *self=static_cast<BiliBili *>(lv_event_get_user_data(e));lv_keyboard_set_textarea(self->_keyboard,self->_tokenEdit);},LV_EVENT_FOCUSED,this);
        } else {
            button(_body,_keyboardMode?"切换拼音":"切换英文",_width-100,48,100,34,Action::Keyboard);
#if LV_USE_IME_PINYIN
            if(!_keyboardMode) {
                _ime=lv_ime_pinyin_create(_body); lv_obj_set_style_text_font(_ime,bodyFont(),0);
                lv_ime_pinyin_set_keyboard(_ime,_keyboard); lv_ime_pinyin_set_mode(_ime,LV_IME_PINYIN_MODE_K26);
                auto *panel=lv_ime_pinyin_get_cand_panel(_ime); lv_obj_set_align(panel,LV_ALIGN_TOP_LEFT);
                lv_obj_set_pos(panel,0,48); lv_obj_set_size(panel,_width-108,40);
                lv_obj_set_style_text_font(panel,bodyFont(),LV_PART_ITEMS);
            }
#endif
        }
    } else if(_view==View::Player) {
        lv_obj_remove_flag(_body,LV_OBJ_FLAG_SCROLLABLE);
        _canvas=lv_canvas_create(_body); lv_canvas_set_buffer(_canvas,&_placeholder,1,1,LV_COLOR_FORMAT_RGB565);
        lv_obj_center(_canvas); lv_obj_add_flag(_canvas,LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(_canvas,event,LV_EVENT_CLICKED,this);
        _playerControls=lv_obj_create(_body); lv_obj_remove_style_all(_playerControls);
        lv_obj_set_size(_playerControls,_width,94); lv_obj_align(_playerControls,LV_ALIGN_BOTTOM_MID,0,0);
        lv_obj_set_style_bg_color(_playerControls,lv_color_hex(Background),0); lv_obj_set_style_bg_opa(_playerControls,LV_OPA_90,0);
        _progress=lv_slider_create(_playerControls); lv_obj_set_pos(_progress,12,4); lv_obj_set_size(_progress,_width-24,12); lv_slider_set_range(_progress,0,1000);
        lv_obj_set_user_data(_progress,reinterpret_cast<void *>(static_cast<uintptr_t>(Action::Seek)<<16));
        lv_obj_add_event_cb(_progress,event,LV_EVENT_RELEASED,this);
        _time=label(_playerControls,"正在准备播放…",8,24,_width-16,&hosino_font_14);
        const int cell=(_width-18)/4;
        auto *pause=button(_playerControls,"暂停",0,48,cell,40,Action::Pause); _playLabel=lv_obj_get_child(pause,0);
        button(_playerControls,"-10秒",cell+6,48,cell,40,Action::Rewind);
        button(_playerControls,"+10秒",2*(cell+6),48,cell,40,Action::Forward);
        button(_playerControls,"设置",3*(cell+6),48,cell,40,Action::Options);
        _chromeUntil=lv_tick_get()+5000;
    } else if(_view==View::Options) {
        const int qualityWidth=(_width-8)/2;
        button(_body,"标清\n320×180 / 20帧",0,0,qualityWidth,64,Action::Quality,0,!m.profile);
        button(_body,"清晰\n480×270 / 15帧",qualityWidth+8,0,qualityWidth,64,Action::Quality,1,m.profile);
        int y=78; label(_body,"视频码率（音频另计）",0,y,_width); y+=32;
        const int rates[]={0,800,1200,1800,2500,3500};
        for(int i=0;i<6;++i) {
            char name[32]; if(!rates[i]) copyText(name,sizeof(name),"推荐"); else snprintf(name,sizeof(name),"%d kbps",rates[i]);
            button(_body,name,(i%2)*((_width+8)/2),y+(i/2)*48,(_width-8)/2,40,Action::Rate,rates[i],m.bitrate==rates[i]);
        }
        y+=154; label(_body,"音量",0,y,_width); y+=36;
        auto *volume=lv_slider_create(_body); lv_obj_set_pos(volume,14,y); lv_obj_set_size(volume,_width-28,16); lv_slider_set_range(volume,0,100); lv_slider_set_value(volume,m.volume,LV_ANIM_OFF);
        lv_obj_set_user_data(volume,reinterpret_cast<void *>(static_cast<uintptr_t>(Action::Volume)<<16)); lv_obj_add_event_cb(volume,event,LV_EVENT_RELEASED,this);
        button(_body,_fit?"画面：适应（点此原尺寸）":"画面：原尺寸（点此适应）",0,y+40,_width,44,Action::Fit);
    }
    if(restoreScroll) { lv_obj_update_layout(_body);lv_obj_scroll_to(_body,scrollX,scrollY,LV_ANIM_OFF); }
}
void BiliBili::playerUi() {
    if(!_worker||!_model||!_canvas) return;
    if(xSemaphoreTake(_worker->mutex,0)==pdTRUE) {
        const int index=_worker->pending;
        if(index>=0&&_worker->pixels[index]) {
            _worker->displayed=index; _worker->pending=-1;
            lv_canvas_set_buffer(_canvas,_worker->pixels[index],_worker->width[index],_worker->height[index],LV_COLOR_FORMAT_RGB565);
            lv_obj_set_size(_canvas,_worker->width[index],_worker->height[index]);
            const int w=_worker->width[index],h=_worker->height[index];
            const int scale=_fit?std::min(_width*256/w,std::max(1,_height-78)*256/h):256;
            lv_image_set_pivot(_canvas,w/2,h/2); lv_image_set_scale(_canvas,scale); lv_obj_center(_canvas);
            lv_obj_invalidate(_canvas);
        }
        xSemaphoreGive(_worker->mutex);
    }
    const auto &m=*_model;
    if(_time) {
        const int p=static_cast<int>(m.position),d=static_cast<int>(m.duration);
        char status[96]; snprintf(status,sizeof(status),"%d:%02d / %d:%02d%s",p/60,p%60,d/60,d%60,m.media_state==HOSINO_MEDIA_OPENING?" · 服务器准备中":""); lv_label_set_text(_time,status);
    }
    if(_progress&&m.duration>0&&!lv_slider_is_dragged(_progress)) lv_slider_set_value(_progress,std::min(1000,static_cast<int>(1000*m.position/m.duration)),LV_ANIM_OFF);
    if(_playLabel) lv_label_set_text(_playLabel,m.paused?"继续":"暂停");
    if(_playerControls&&lv_tick_get()>_chromeUntil&&m.playing&&!presentationBusy(_body)) lv_obj_add_flag(_playerControls,LV_OBJ_FLAG_HIDDEN);
}
void BiliBili::action(Action a,int value) {
    if(!_model) return;
    bili::Command cmd; const auto &m=*_model;
    switch(a) {
    case Action::Back:
        if(_presentedView==View::Home) { notifyCoreClosed(); return; }
        if(_presentedView==View::Player) { cmd.op=bili::Op::Stop; send(cmd); navigate(View::Detail); }
        else if(_presentedView==View::Options) navigate(View::Player);
        else if(_presentedView==View::Detail) { navigate(_listView); refresh(); }
        else { navigate(View::Home); _page=1; _folder=0; refresh(); }
        break;
    case Action::Home: navigate(View::Home); _page=1; _folder=0; refresh(); break;
    case Action::Search: navigate(View::Search); break;
    case Action::Profile: navigate(View::Profile); refresh(); break;
    case Action::Settings: navigate(View::Settings); break;
    case Action::Save:
        if(!_edit||!_tokenEdit) break;
        cmd.op=bili::Op::Configure; copyText(cmd.text,sizeof(cmd.text),lv_textarea_get_text(_edit)); copyText(cmd.token,sizeof(cmd.token),lv_textarea_get_text(_tokenEdit));
        if(send(cmd)) { lv_textarea_set_text(_tokenEdit,""); showNotice("正在验证并保存连接设置…"); } memset(cmd.token,0,sizeof(cmd.token)); break;
    case Action::Query:
        if(!_edit) break;
        copyText(_query,sizeof(_query),lv_textarea_get_text(_edit)); _folder=0; _page=1; _searchList=true; navigate(View::Videos); refresh(); break;
    case Action::Keyboard: if(_edit) copyText(_query,sizeof(_query),lv_textarea_get_text(_edit)); _keyboardMode=1-_keyboardMode; _rebuild=true; break;
    case Action::Previous: if(_page>1) --_page; refresh(); break;
    case Action::Next: if(m.more) { ++_page; refresh(); } break;
    case Action::Select:
        if(value<0||static_cast<unsigned>(value)>=m.count) break;
        _listView=_presentedView; cmd.op=bili::Op::Detail; copyText(cmd.bvid,sizeof(cmd.bvid),m.rows[value].video.bvid);
        if(send(cmd)) { copyText(_detailBvid,sizeof(_detailBvid),cmd.bvid); navigate(View::Detail); }
        break;
    case Action::Play:
        cmd.op=bili::Op::Play;copyText(cmd.bvid,sizeof(cmd.bvid),m.selected.bvid);cmd.cid=m.selected.cid;
        if(send(cmd)) navigate(View::Player);
        break;
    case Action::Part:
        if(value<0||static_cast<unsigned>(value)>=m.part_count) break;
        cmd.op=bili::Op::Play;copyText(cmd.bvid,sizeof(cmd.bvid),m.selected.bvid);
        cmd.cid=m.parts[value].cid; if(send(cmd)) navigate(View::Player); break;
    case Action::Login: cmd.op=bili::Op::Qr; if(send(cmd)) { navigate(View::Login); _qrNext=lv_tick_get()+3000; } break;
    case Action::Logout: cmd.op=bili::Op::Logout; send(cmd); break;
    case Action::Folders: _page=1; navigate(View::Folders); refresh(); break;
    case Action::Favorite: _page=1; navigate(View::FavoritePick); refresh(); break;
    case Action::Remove:
        if(!_folder) break;
        cmd.op=bili::Op::Favorite; cmd.folder=_folder; cmd.value=1; copyText(cmd.bvid,sizeof(cmd.bvid),m.selected.bvid);
        if(send(cmd)) showNotice("已请求从当前收藏夹移除；请检查服务端结果");
        break;
    case Action::PickFolder:
        if(value<0||static_cast<unsigned>(value)>=m.count) break;
        if(_presentedView==View::FavoritePick) {
            cmd.op=bili::Op::Favorite; cmd.folder=m.rows[value].folder; copyText(cmd.bvid,sizeof(cmd.bvid),m.selected.bvid);
            if(send(cmd)) { navigate(View::Detail); showNotice("正在添加收藏…"); }
        } else { _folder=m.rows[value].folder; _page=1; _searchList=false; navigate(View::Videos); refresh(); }
        break;
    case Action::Pause: cmd.op=bili::Op::Pause; send(cmd); _chromeUntil=lv_tick_get()+5000; break;
    case Action::Rewind: case Action::Forward: case Action::Seek:
        cmd.op=bili::Op::Seek;
        cmd.position=a==Action::Seek&&_progress?m.duration*lv_slider_get_value(_progress)/1000.0:std::max(0.0,m.position+(a==Action::Forward?10:-10));
        if(m.duration>0) cmd.position=std::min(cmd.position,std::max(0.0,m.duration-0.1));
        send(cmd); _chromeUntil=lv_tick_get()+5000; break;
    case Action::Options: navigate(View::Options); break;
    case Action::Quality:
        if(_model&&value==_model->profile) break;
        cmd.op=bili::Op::Quality; cmd.value=value; send(cmd); _rebuild=true; break;
    case Action::Rate: cmd.op=bili::Op::Rate; cmd.value=value; send(cmd); _rebuild=true; break;
    case Action::Volume:
        cmd.op=bili::Op::Volume;
        cmd.value=value;
        send(cmd); break;
    case Action::Fit: _fit=!_fit; _rebuild=true; break;
    default: break;
    }
}
} // namespace esp_brookesia::apps
