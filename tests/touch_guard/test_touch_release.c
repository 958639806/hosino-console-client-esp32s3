#define main legacy_protocol_main
#include "test_touch_v21.c"
#undef main
static FILE *state_trace;

static lv_indev_data_t service(lv_indev_t *dev,bool hardware_irq)
{
    tx_calls=rx_calls=delay_calls=0;
    if (hardware_irq) {
        esp_lv_adapter_touch_config_t *cfg=&cfg_seen[dev->id];
        cfg->callbacks.on_interrupt(cfg->handle,cfg->callbacks.user_ctx);
        dev->pending=true;
    }
    lv_indev_data_t data={0};CHECK(dev->read_cb==guarded_input_read);
    dev->read_cb(dev,&data);
    if(state_trace)fprintf(state_trace,"%lld %d %d %d\n",(long long)now,data.state,data.point.x,data.point.y);
    return data;
}
static void fresh(touch_guard_t *guard,lv_indev_t *dev)
{
    memset(&guard->primary,0,sizeof(guard->primary));
    memset(&guard->last_packet,0,sizeof(guard->last_packet));
    memset(&guard->stats,0,sizeof(guard->stats));
    memset(&guard->diag_state,0,sizeof(guard->diag_state));
    guard->release_deadline_us=0;guard->release_due=false;
    guard->last_irq_generation=irq_generation(guard->irq_slot);
    guard->last_log_us=now;
    dev->pending=false;memset(&dev->last,0,sizeof(dev->last));
    rx_result=get_result=ESP_OK;fail_tx=0;read_duration=1000;notify_calls=0;
}
static void down(lv_indev_t *dev,unsigned id,unsigned x,unsigned y)
{
    empty();wire[5]=1;put(0,id,6,x,y);now+=8000;
    lv_indev_data_t data=service(dev,true);CHECK(data.state==LV_INDEV_STATE_PRESSED);
}
static void first_zero(lv_indev_t *dev,touch_guard_t *guard)
{
    empty();now+=8000;lv_indev_data_t data=service(dev,true);
    CHECK(data.state==LV_INDEV_STATE_PRESSED&&guard->release_deadline_us>now);
    CHECK(tx_calls==2&&rx_calls==1&&guard->stats.zero_pending==1);
}
int main(int argc,char **argv)
{
    CHECK(legacy_protocol_main()==0);
    native_mode=true;now=10000000;
    struct esp_lcd_touch_s hw={.config={.int_gpio_num=11,.x_max=480,.y_max=480}};
    esp_lv_adapter_touch_config_t cfg={.disp=(void*)5,.handle=&hw,.scale={1,1},.multi_touch={0,1}};
    lv_indev_t *dev=__wrap_esp_lv_adapter_register_touch(&cfg);CHECK(dev);
    touch_guard_t *g=cfg_seen[dev->id].callbacks.user_ctx;
    CHECK(g->debounce_enabled&&g->adapter_read==fake_adapter_read);
    /* One transient valid zero report must not split a held same-primary drag. */
    fresh(g,dev);down(dev,0,100,110);first_zero(dev,g);
    down(dev,0,120,130);CHECK(!g->release_deadline_us&&g->stats.zero_resumed==1);
    CHECK(g->stats.reported_down==1&&g->stats.reported_up==0);
    /* True UP with no following IRQ is released by existing 8ms read service,
     * without speculative I2C transactions or an extra timer allocation. */
    fresh(g,dev);down(dev,0,100,100);first_zero(dev,g);
    const int64_t deadline=g->release_deadline_us;
    now=deadline-1;CHECK(service(dev,false).state==LV_INDEV_STATE_PRESSED);
    CHECK(tx_calls==0&&rx_calls==0&&notify_calls==0);
    now=deadline;CHECK(service(dev,false).state==LV_INDEV_STATE_RELEASED);
    CHECK(tx_calls==0&&rx_calls==0&&notify_calls==1&&!g->release_deadline_us);
    CHECK(g->stats.zero_confirmed==1&&g->stats.zero_synthetic==1&&g->stats.reported_up==1);
    now+=8000;CHECK(service(dev,false).state==LV_INDEV_STATE_RELEASED&&notify_calls==1);
    /* More zero frames never extend the first pending-release deadline. */
    fresh(g,dev);down(dev,0,100,100);first_zero(dev,g);
    int64_t fixed=g->release_deadline_us;
    now+=8000;CHECK(service(dev,true).state==LV_INDEV_STATE_PRESSED&&g->release_deadline_us==fixed);
    now=fixed;CHECK(service(dev,false).state==LV_INDEV_STATE_RELEASED);
    /* A real IRQ and fresh packet coalesced with the due synthetic notification
     * wins: exactly one real packet is read, and no synthetic release leaks. */
    fresh(g,dev);down(dev,0,100,100);first_zero(dev,g);
    now=g->release_deadline_us+1;empty();wire[5]=1;put(0,0,6,140,150);
    CHECK(service(dev,true).state==LV_INDEV_STATE_PRESSED);
    CHECK(tx_calls==2&&rx_calls==1&&notify_calls==1&&!g->release_deadline_us);
    CHECK(g->stats.zero_resumed==1&&g->stats.zero_synthetic==0);
    CHECK(service(dev,false).state==LV_INDEV_STATE_PRESSED&&tx_calls==0);
    /* IO/protocol errors remain fail closed, cancelling any deferred release. */
    fresh(g,dev);down(dev,0,100,100);first_zero(dev,g);
    wire[6]=0;now+=8000;CHECK(service(dev,true).state==LV_INDEV_STATE_RELEASED);
    CHECK(!g->release_deadline_us&&g->primary.block_until_all_up&&g->stats.faults[TOUCH_FAULT_MARKER]==1);
    empty();wire[5]=1;put(0,0,6,120,120);CHECK(service(dev,true).state==LV_INDEV_STATE_RELEASED);
    empty();CHECK(service(dev,true).state==LV_INDEV_STATE_RELEASED);
    down(dev,0,120,120);
    /* Missing primary keeps the old all-up handoff barrier; no cached new ID. */
    fresh(g,dev);down(dev,0,100,100);first_zero(dev,g);
    empty();wire[5]=1;put(0,1,6,150,150);CHECK(service(dev,true).state==LV_INDEV_STATE_RELEASED);
    CHECK(!g->release_deadline_us&&g->primary.block_until_all_up);
    /* Two-contact topology and final pinch release bypass one-finger grace. */
    fresh(g,dev);down(dev,0,100,100);first_zero(dev,g);
    empty();wire[5]=2;put(0,0,6,110,110);put(1,1,6,250,250);
    CHECK(service(dev,true).state==LV_INDEV_STATE_PRESSED&&adapter_count==2&&!g->release_deadline_us);
    empty();CHECK(service(dev,true).state==LV_INDEV_STATE_RELEASED&&!g->release_deadline_us);
    /* Header-zero and inactive status records are measured separately. */
    fresh(g,dev);down(dev,0,100,100);empty();wire[5]=1;put(0,0,0,100,100);
    CHECK(service(dev,true).state==LV_INDEV_STATE_PRESSED);
    CHECK(g->stats.raw_inactive_records==1&&g->stats.raw_header_zero==0);
    now=g->release_deadline_us;CHECK(service(dev,false).state==LV_INDEV_STATE_RELEASED);
    /* Coordinate transforms are applied exactly once to a cached raw packet. */
    hw.config.flags.mirror_y=hw.config.flags.swap_xy=1;
    fresh(g,dev);down(dev,0,100,200);CHECK(dev->last.point.x==280&&dev->last.point.y==100);
    first_zero(dev,g);CHECK(dev->last.point.x==280&&dev->last.point.y==100);
    hw.config.flags.mirror_y=hw.config.flags.swap_xy=0;
    /* Export only deterministic synthetic test input for a separate REAL
     * pinned LVGL pointer-dispatch test, never physical user coordinates. */
    CHECK(argc==2);state_trace=fopen(argv[1],"w");CHECK(state_trace);
    fresh(g,dev);down(dev,0,80,250);down(dev,0,80,220);
    for(unsigned i=0;i<3;i++){
        empty();now+=8000;CHECK(service(dev,true).state==LV_INDEV_STATE_PRESSED);
        down(dev,0,80,190-i*40);
    }
    empty();now+=8000;CHECK(service(dev,true).state==LV_INDEV_STATE_PRESSED);
    now=g->release_deadline_us;CHECK(service(dev,false).state==LV_INDEV_STATE_RELEASED);
    fclose(state_trace);state_trace=NULL;
    /* A separate handle's hardware IRQ cannot steal this handle's synthetic
     * release; per-source static generations are isolated. */
    struct esp_lcd_touch_s other_hw={.config={.int_gpio_num=11,.x_max=480,.y_max=480}};
    esp_lv_adapter_touch_config_t other_cfg=cfg;other_cfg.handle=&other_hw;
    lv_indev_t *other=__wrap_esp_lv_adapter_register_touch(&other_cfg);CHECK(other);
    fresh(g,dev);down(dev,0,100,100);first_zero(dev,g);
    observe_touch_irq(&other_hw,NULL);now=g->release_deadline_us;
    CHECK(service(dev,false).state==LV_INDEV_STATE_RELEASED&&tx_calls==0&&rx_calls==0);
    CHECK(__wrap_esp_lv_adapter_unregister_touch(other)==ESP_OK);
    /* If the LVGL hook cannot be installed, preserve the original immediate
     * release behavior: never start a deadline that has no servicing wrapper. */
    lock_result=ESP_FAIL;other=__wrap_esp_lv_adapter_register_touch(&other_cfg);lock_result=ESP_OK;
    touch_guard_t *unhooked=cfg_seen[other->id].callbacks.user_ctx;
    CHECK(!unhooked->debounce_enabled&&other->read_cb==fake_adapter_read);
    esp_lcd_touch_point_data_t direct[2];uint8_t direct_count=0;
    empty();wire[5]=1;put(0,0,6,100,100);CHECK(read_cfg(&cfg_seen[other->id],direct,&direct_count)==ESP_OK&&direct_count==1);
    empty();CHECK(read_cfg(&cfg_seen[other->id],direct,&direct_count)==ESP_OK&&direct_count==0&&!unhooked->release_deadline_us);
    CHECK(__wrap_esp_lv_adapter_unregister_touch(other)==ESP_OK);
    /* Failed unregistration preserves a usable wrapper and pending deadline. */
    fresh(g,dev);down(dev,0,100,100);first_zero(dev,g);
    unregister_result=ESP_FAIL;CHECK(__wrap_esp_lv_adapter_unregister_touch(dev)==ESP_FAIL);
    CHECK(!g->unregistering);now=g->release_deadline_us;CHECK(service(dev,false).state==LV_INDEV_STATE_RELEASED);
    /* Success removes the guard before free; the observer refers only to static
     * source slots, so even a late observer call never dereferences that guard. */
    unregister_result=ESP_OK;CHECK(__wrap_esp_lv_adapter_unregister_touch(dev)==ESP_OK);
    CHECK(!s_guards);observe_touch_irq(&hw,NULL);CHECK(allocs==frees);
    for(unsigned i=0;i<HOSINO_TOUCH_IRQ_SLOTS;i++)CHECK(!s_irq_sources[i].owners);
    printf("PASS bounded release: %d assertions; real IRQ vs synthetic notifications, 32ms no-IRQ UP, no extra I2C/timer, short-zero continuity, errors/ID/pinch/transform/lifecycle isolation\n",checks);
    return 0;
}
