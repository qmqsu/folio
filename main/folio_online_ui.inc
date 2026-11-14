// Native online-update orchestration. No worker callback touches UI/PMU state.
static bool online_active(void) {
    return s_online_wait || s_online_operation || s_online_install_owned ||
        s_online_network || s_online_leaving || folio_online_update_busy();
}
static void online_publish_safety(void) {
    unsigned flags=atomic_load(&s_online_safety)&(ONLINE_AUTH|ONLINE_CANCEL);
    if(s_card_ready && !s_online_card_lost)flags|=ONLINE_CARD;
    if(s_power_initialized && folio_power_update_safe())flags|=ONLINE_POWER;
    if(folio_power_shutdown_pending())flags|=ONLINE_CANCEL;
    atomic_store(&s_online_safety,flags);
}
static bool online_card_ready(void* context) {
    (void)context;return (atomic_load(&s_online_safety)&ONLINE_CARD)!=0;
}
static bool online_storage_lock(void* context,uint32_t timeout) {
    (void)context;return folio_storage_lock(timeout);
}
static void online_storage_unlock(void* context) {(void)context;folio_storage_unlock();}
static bool online_install_allowed(void* context,char* error,size_t cap) {
    (void)context;unsigned flags=atomic_load(&s_online_safety);
    if((flags&(ONLINE_CARD|ONLINE_POWER|ONLINE_AUTH|ONLINE_CANCEL))==(ONLINE_CARD|ONLINE_POWER|ONLINE_AUTH))return true;
    if(error && cap)snprintf(error,cap,"%s",!(flags&ONLINE_CARD)?"TF 卡不可用，安装已停止。":
        !(flags&ONLINE_POWER)?"电量状态不安全：需要有效电量至少 20%，且无低电量警告。":"安装授权已取消，请重新确认。");
    return false;
}
static bool online_cancelled(void* context) {
    (void)context;return (atomic_load(&s_online_safety)&ONLINE_CANCEL)!=0;
}
static bool online_init(void) {
    if(s_online_initialized)return true;
    folio_online_hooks_t hooks={.card_ready=online_card_ready,.storage_lock=online_storage_lock,
        .storage_unlock=online_storage_unlock,.install_allowed=online_install_allowed,.cancelled=online_cancelled};
    s_online_initialized=folio_online_update_init(&hooks);
    return s_online_initialized;
}
static void online_cancel(void) {
    if(!online_active())return;
    s_online_wait=0;s_online_confirm=false;s_online_stopping=true;
    atomic_fetch_or(&s_online_safety,ONLINE_CANCEL);
    atomic_fetch_and(&s_online_safety,~(unsigned)ONLINE_AUTH);
    folio_online_update_cancel();
}
static void online_restore_consumers(void) {
    if(!s_online_install_owned)return;
    s_online_install_owned=false;
    // Never reopen a missing card or delay an urgent PMU acknowledgement.
    if(folio_power_shutdown_pending() || s_online_card_lost)return;
    folio_font_close(s_font);s_font=NULL;s_active_font[0]=0;
    load_system_font();
    if(s_card_ready) {
        s_catalog=folio_catalog_open();s_activity=folio_activity_open();
        s_activity_save_failed=false;
        s_activity_load_failed=!s_activity || *folio_activity_error(s_activity);
        catalog_select();
        if(!s_catalog || *folio_catalog_error(s_catalog) || s_activity_load_failed)
            snprintf(s_online_message,sizeof(s_online_message),"更新已停止，阅读记录暂未加载，请重新扫描 TF 卡。");
    }
}
// This is deliberately not a wait loop. Call again from the UI event loop.
static bool online_quiesce(void) {
    if(!online_active())return true;
    online_cancel();
    if(folio_online_update_busy())return false;
    folio_online_snapshot_t completed;folio_online_update_get_snapshot(&completed);
    // A completion racing a lock request must still reach the UI's successful
    // sync/reboot path. Only a genuine PMU shutdown may take priority over it.
    if(s_online_install_owned && completed.state==FOLIO_ONLINE_INSTALLED && !folio_power_shutdown_pending())return false;
    if(s_online_operation)s_online_snapshot=completed;
    s_online_operation=false;
    if(s_online_network && !folio_network_stop())return false;
    s_online_network=false;s_online_leaving=false;
    online_restore_consumers();
    return true;
}
static bool online_leave(view_t target) {
    s_online_confirm=false;
    if(!online_active())return true;
    s_online_leaving=true;s_online_target=target;online_cancel();
    if(s_view==VIEW_UPDATE)show_view(VIEW_UPDATE,false);
    return false;
}
static void online_error(const char* message) {
    snprintf(s_online_message,sizeof(s_online_message),"%s",message&&*message?message:"操作未完成，请检查网络后重试。");
    s_online_confirm=false;
}
static void online_start_worker(unsigned action) {
    char error[192]={0};s_online_wait=0;s_online_message[0]=0;
    atomic_fetch_and(&s_online_safety,~(unsigned)(ONLINE_CANCEL|ONLINE_AUTH));
    online_publish_safety();
    bool okay=action==1?folio_online_update_start_check(s_online_beta?"beta":"stable",error,sizeof(error)):
        folio_online_update_start_download(error,sizeof(error));
    s_online_operation=okay;
    if(!okay){online_error(error);}
    folio_online_update_get_snapshot(&s_online_snapshot);
    s_online_draw_at=esp_timer_get_time();show_view(VIEW_UPDATE,true);
}
static void online_connect(unsigned action) {
    if(online_active())return;
    if(!online_init()){online_error("更新任务未能初始化，请重试。");show_view(VIEW_UPDATE,false);return;}
    if(!folio_update_available()){online_error("当前分区不支持在线安装。首次安装请使用 USB 完整发布包。");show_view(VIEW_UPDATE,false);return;}
    if(!s_card_ready){online_error("请先插入可用的 TF 卡，再检查或下载更新。");show_view(VIEW_UPDATE,false);return;}
    // Stop competing workers before claiming the network, not before install.
    if(!stop_transfer()){online_error("其他网络任务正在停止，请稍后重试。");show_view(VIEW_UPDATE,false);return;}
    online_publish_safety();
    if(folio_power_shutdown_pending()){emergency_shutdown();return;}
    s_online_stopping=false;s_online_confirm=false;s_online_message[0]=0;
    atomic_fetch_and(&s_online_safety,~(unsigned)(ONLINE_CANCEL|ONLINE_AUTH));
    char error[192]={0};
    // connect() is asynchronous STA-only and uses the saved native Wi-Fi profile.
    s_online_network=true;
    if(!folio_network_connect(error,sizeof(error))) {
        online_error(*error?error:"无法连接已保存网络，请先设置 Wi-Fi。");
        if(folio_network_stop())s_online_network=false;
        show_view(VIEW_UPDATE,true);return;
    }
    s_online_wait=action;s_online_deadline=esp_timer_get_time()+35000000;
    show_view(VIEW_UPDATE,true);
}
static void online_install(void) {
    char error[192]={0};
    if(online_active() || !s_online_confirm)return;
    s_online_confirm=false;
    folio_online_update_get_snapshot(&s_online_snapshot);
    if(s_online_snapshot.state!=FOLIO_ONLINE_READY){online_error("更新文件尚未完整验证，请重新下载。");show_view(VIEW_UPDATE,true);return;}
    // A fresh UI-thread PMU poll is mandatory at the second confirmation.
    folio_power_event_t event=folio_power_poll();
    if(event>s_deferred_power)s_deferred_power=event;
    event=s_deferred_power;
    online_publish_safety();
    if(folio_power_shutdown_pending()){emergency_shutdown();return;}
    if(event==FOLIO_POWER_LOCK || event==FOLIO_POWER_OFF_REQUEST) {
        online_error("收到电源操作，安装尚未开始。");show_view(VIEW_UPDATE,true);return;
    }
    if(!(atomic_load(&s_online_safety)&ONLINE_POWER)) {
        online_error("无法确认安全电量。需要有效电量至少 20%，且无低电量警告。");show_view(VIEW_UPDATE,true);return;
    }
    read_pico_sd_info_t card={0};
    if(!s_card_ready || read_pico_sd_get_info(&card)!=ESP_OK || !card.mounted) {
        (void)handle_card_lost();online_publish_safety();
        online_error("TF 卡不可用，请重新插入后下载。");show_view(VIEW_UPDATE,true);return;
    }
    if(!save_reader())return;
    activity_record_book();
    if(!activity_flush() || !catalog_save()) {
        online_error("阅读记录未能保存，尚未开始安装。请检查 TF 卡。");show_view(VIEW_UPDATE,true);return;
    }
    // Unlike prepare_card_change(), do not stop the download's network first or
    // hold a UI storage lock across start_install(). The worker takes its lock.
    close_reader();activity_close();catalog_close();folio_cover_clear_cache();
    folio_font_close(s_font);s_font=NULL;s_active_font[0]=0;
    bool fallback_ready=open_active_font(FOLIO_FONT_BUILTIN_PATH); // Preference remains unchanged.
    s_online_install_owned=true;
    if(!fallback_ready) {
        online_error("内置字体加载失败，尚未开始安装，请重试。");
        online_restore_consumers();show_view(VIEW_UPDATE,true);return;
    }
    // Saving and loading the temporary font can take time. Recheck the PMU
    // after those operations, before publishing the one-install authorization.
    event=folio_power_poll();if(event>s_deferred_power)s_deferred_power=event;
    if(s_deferred_power!=FOLIO_POWER_NONE || folio_power_shutdown_pending()) {
        online_error("收到电源操作，安装尚未开始。");online_restore_consumers();
        if(folio_power_shutdown_pending())emergency_shutdown();else show_view(VIEW_UPDATE,true);
        return;
    }
    atomic_fetch_and(&s_online_safety,~(unsigned)ONLINE_CANCEL);
    atomic_fetch_or(&s_online_safety,ONLINE_AUTH);
    online_publish_safety();
    if(!online_install_allowed(NULL,error,sizeof(error))) {
        online_error(error);online_restore_consumers();show_view(VIEW_UPDATE,true);return;
    }
    s_online_operation=folio_online_update_start_install(error,sizeof(error));
    if(!s_online_operation) {
        atomic_fetch_and(&s_online_safety,~(unsigned)ONLINE_AUTH);
        online_error(error);online_restore_consumers();
    }
    folio_online_update_get_snapshot(&s_online_snapshot);
    s_online_draw_at=esp_timer_get_time();show_view(VIEW_UPDATE,true);
}
static bool online_setup_back(void) {
    if(!s_online_wifi_setup || s_view!=VIEW_WIFI)return false;
    if(!folio_network_stop()){snprintf(s_setup_message,sizeof(s_setup_message),"正在关闭网络，请稍后返回。");show_view(VIEW_WIFI,false);return true;}
    s_wifi_scanning=s_wifi_connecting=false;memset(s_wifi_password,0,sizeof(s_wifi_password));
    s_online_wifi_setup=false;
    online_error("Wi-Fi 设置已返回。请手动重试检查或下载。");show_view(VIEW_UPDATE,true);return true;
}
static void online_tap(int x,int y) {
    if(online_active()) {
        if(hit(done_rect(),x,y)) {
            online_cancel();show_view(VIEW_UPDATE,false);
        }
        return;
    }
    if(s_online_confirm) {
        if(hit((EpdRect){48,984,588,96},x,y)){s_online_confirm=false;show_view(VIEW_UPDATE,true);}
        else if(hit(done_rect(),x,y))online_install();
        return;
    }
    if(hit((EpdRect){48,232,282,72},x,y) && s_update_local) {
        s_update_local=false;show_view(VIEW_UPDATE,true);return;
    }
    if(hit((EpdRect){354,232,282,72},x,y) && !s_update_local) {
        s_update_local=true;inspect_local_update();return;
    }
    if(s_update_local){if(hit(done_rect(),x,y))install_update();return;}
    if(hit((EpdRect){48,328,282,72},x,y) || hit((EpdRect){354,328,282,72},x,y)) {
        bool beta=x>=354;
        if(s_online_beta!=beta) {
            s_online_beta=beta;s_online_message[0]=0;
            // Invalidate a candidate from the other channel; never install it.
            if(online_init())(void)folio_online_update_init(&(folio_online_hooks_t){
                .card_ready=online_card_ready,.storage_lock=online_storage_lock,.storage_unlock=online_storage_unlock,
                .install_allowed=online_install_allowed,.cancelled=online_cancelled});
            memset(&s_online_snapshot,0,sizeof(s_online_snapshot));show_view(VIEW_UPDATE,true);
        }
        return;
    }
    if(hit((EpdRect){48,984,282,96},x,y)) {
        s_online_wifi_setup=true;enter_wifi_for(VIEW_UPDATE);return;
    }
    if(hit((EpdRect){354,984,282,96},x,y)){online_connect(1);return;}
    if(!hit(done_rect(),x,y))return;
    if(s_online_snapshot.state==FOLIO_ONLINE_READY) {
        s_online_message[0]=0;s_online_confirm=true;show_view(VIEW_UPDATE,true);
    } else online_connect(s_online_snapshot.state==FOLIO_ONLINE_AVAILABLE?2:1);
}
static void refresh_online(void) {
    if(!online_active())return;
    online_publish_safety();
    if(s_online_install_owned && !(atomic_load(&s_online_safety)&ONLINE_POWER)) {
        online_error("电量状态不安全，正在停止安装；不会继续写入启动目标。");online_cancel();
    }
    if(s_online_wait) {
        folio_network_status_t net;folio_network_get_status(&net);
        if(net.connected)online_start_worker(s_online_wait);
        else if(!net.connecting || esp_timer_get_time()>=s_online_deadline) {
            s_online_wait=0;online_error(net.message[0]?net.message:"连接超时，请设置 Wi-Fi 后重试。");
        }
    }
    folio_online_snapshot_t current;folio_online_update_get_snapshot(&current);
    bool changed=current.state!=s_online_snapshot.state || current.busy!=s_online_snapshot.busy;
    if(current.busy) {
        // E-ink: no per-chunk redraw. At most once/second, in 5% increments.
        unsigned old=s_online_snapshot.total?(unsigned)((uint64_t)s_online_snapshot.received*20/s_online_snapshot.total):0;
        unsigned progress=current.total?(unsigned)((uint64_t)current.received*20/current.total):0;
        int64_t now=esp_timer_get_time();
        if(s_view==VIEW_UPDATE && (changed || (now-s_online_draw_at>=1000000 && old!=progress))) {
            s_online_snapshot=current;s_online_draw_at=now;show_view(VIEW_UPDATE,false);
        }
        return;
    }
    if(s_online_wait)return;
    bool finished=s_online_operation;
    if(finished){s_online_snapshot=current;s_online_operation=false;}
    atomic_fetch_and(&s_online_safety,~(unsigned)ONLINE_AUTH);
    if(s_online_network && !folio_network_stop())return;
    s_online_network=false;
    if(s_online_install_owned && s_online_snapshot.state==FOLIO_ONLINE_INSTALLED && !folio_power_shutdown_pending()) {
        s_online_install_owned=false;
        close_reader();activity_close();catalog_close();folio_cover_clear_cache();
        folio_font_close(s_font);s_font=NULL;s_active_font[0]=0;
        esp_err_t error=read_pico_sd_sync();
        if(error==ESP_OK){esp_restart();return;}
        load_card(true,false);show_error("固件已安装，但 TF 卡同步失败。请检查卡后重新启动。",VIEW_DEVICE);return;
    }
    online_restore_consumers();
    if(s_online_card_lost) {
        s_online_card_lost=false;s_online_leaving=false;
        (void)handle_card_lost();online_error("TF 卡已移除，更新已取消。请重新插入并扫描后重试。");
        show_view(VIEW_UPDATE,true);return;
    }
    if(s_online_leaving) {
        view_t target=s_online_target;s_online_leaving=false;
        if(target==VIEW_HOME || target==VIEW_LIBRARY || target==VIEW_SYSTEM)show_root(target);
        else show_view(target,true);
        return;
    }
    if(s_online_stopping && !finished)online_error("操作已取消，可重新检查更新。");
    s_online_stopping=false;
    if(s_view==VIEW_UPDATE)show_view(VIEW_UPDATE,false);
}
static void draw_online_update(void) {
    char current[80];snprintf(current,sizeof(current),"当前版本  %s",esp_app_get_description()->version);
    page_header(s_online_confirm?"取消":"返回",s_online_confirm?"确认安装":"固件升级",current);
    if(s_online_confirm) {
        panel((EpdRect){48,256,588,656});
        label(72,328,612,28,MUTED,"已完成签名与文件完整校验");
        label(72,404,612,42,INK,s_online_snapshot.version);
        label(72,468,612,26,INK,s_online_beta?"测试通道 · 尚未全面验收":"稳定通道");
        paragraph(72,552,612,30,46,4,INK,"确认后开始安装并重新启动。请保持供电，不要拔出 TF 卡。");
        paragraph(72,780,612,24,36,3,MUTED,"需要有效电量至少 20%，且无低电量警告。书籍、字体资源和分区不会被更新。");
        text_action((EpdRect){48,984,588,96},"暂不安装");action(done_rect(),"确认安装并重启",true);return;
    }
    bool busy=online_active();
    action_state((EpdRect){48,232,282,72},"在线更新",true,!busy);
    action_state((EpdRect){354,232,282,72},"本地 TF 卡",false,!busy);
    action_state((EpdRect){48,328,282,72},"稳定通道",!s_online_beta,!busy);
    action_state((EpdRect){354,328,282,72},"测试通道",s_online_beta,!busy);
    label(48,440,636,24,MUTED,s_online_beta?"测试版本尚未全面验收，请谨慎选择":"仅手动检查，结束后关闭 Wi-Fi");
    panel((EpdRect){48,480,588,448});
    const char* title="手动检查更新";
    switch(s_online_snapshot.state) {
        case FOLIO_ONLINE_CHECKING:title="正在检查签名发布";break;
        case FOLIO_ONLINE_AVAILABLE:title="发现新版本";break;
        case FOLIO_ONLINE_DOWNLOADING:title="正在下载并校验";break;
        case FOLIO_ONLINE_READY:title="文件已验证，可以安装";break;
        case FOLIO_ONLINE_INSTALLING:title="正在安装，请保持供电";break;
        case FOLIO_ONLINE_INSTALLED:title="安装成功，正在重新启动";break;
        case FOLIO_ONLINE_NO_RELEASE:title="暂无可用发布";break;
        case FOLIO_ONLINE_FAILED:title="更新未完成";break;
        case FOLIO_ONLINE_CANCELLED:title="操作已取消";break;
        default:break;
    }
    if(s_online_wait)title="正在连接已保存网络";
    if(s_online_stopping && busy)title="正在安全停止，请稍候";
    label(72,540,612,32,INK,title);
    if(s_online_snapshot.version[0])label(72,600,612,36,INK,s_online_snapshot.version);
    const char* message=s_online_message[0]?s_online_message:s_online_wait?"连接成功后继续。可以随时取消或返回。":
        s_online_snapshot.message[0]?s_online_snapshot.message:"使用已保存的 Wi-Fi 检查更新。下载不会自动安装，安装前还需再次确认。";
    paragraph(72,664,612,26,40,4,INK,message);
    if(s_online_snapshot.total && s_online_snapshot.state==FOLIO_ONLINE_DOWNLOADING) {
        unsigned percent=(unsigned)((uint64_t)s_online_snapshot.received*100/s_online_snapshot.total);if(percent>100)percent=100;
        char progress[48];snprintf(progress,sizeof(progress),"%u%% · %s",percent,percent==100?"正在完成校验":"处理中");
        label(72,858,612,24,MUTED,progress);
        epd_fill_rect((EpdRect){72,884,540,8},RULE,fb());
        if(percent)epd_fill_rect((EpdRect){72,884,(int)(540*percent/100),8},INK,fb());
    } else if(s_online_snapshot.state==FOLIO_ONLINE_INSTALLING)
        label(72,860,612,24,MUTED,"正在复验与安装，请勿拔卡");
    else if(s_online_snapshot.notes[0] && !s_online_message[0])paragraph(72,844,612,22,30,2,MUTED,s_online_snapshot.notes);
    if(busy) {
        label(48,1036,636,24,MUTED,"返回或 KEY2 同样会取消，等待安全停止");
        action_state(done_rect(),s_online_stopping?"正在取消":"取消操作",false,!s_online_stopping);
    } else {
        action((EpdRect){48,984,282,96},"Wi-Fi 设置",false);
        action((EpdRect){354,984,282,96},"重新检查",false);
        const char* button=s_online_snapshot.state==FOLIO_ONLINE_READY?"安装此版本":
            s_online_snapshot.state==FOLIO_ONLINE_AVAILABLE?"下载此版本":"检查更新";
        action(done_rect(),button,true);
    }
}
