// Included by the real production-UI host preview. The online worker/network/PMU
// below are simulations: not evidence of TLS, signature verification or OTA I/O.
static void online_mock_finish(folio_online_state_t state,const char* message) {
    mock_online.busy=false;mock_online.state=state;++mock_online.revision;
    snprintf(mock_online.message,sizeof(mock_online.message),"%s",message);
    refresh_online();
}
static void online_mock_check(void) {
    unsigned stations=mock_https_station_calls,connects=mock_native_connect_calls;
    online_connect(1);assert(s_online_wait && mock_network.running);
    assert(mock_https_station_calls==stations+1 && mock_native_connect_calls==connects);
    refresh_online();assert(mock_online.busy && s_online_snapshot.state==FOLIO_ONLINE_CHECKING);
}
static void online_mock_ready(void) {
    online_mock_check();
    snprintf(mock_online.version,sizeof(mock_online.version),"0.8.33-test");
    mock_online.total=3000000;
    online_mock_finish(FOLIO_ONLINE_AVAILABLE,"已找到比当前版本更新的签名发布。");
    assert(!online_active() && !mock_network.running);
    unsigned stations=mock_https_station_calls,connects=mock_native_connect_calls;
    tap_rect(done_rect());assert(s_online_wait==2 && !online_installs);
    assert(mock_https_station_calls==stations+1 && mock_native_connect_calls==connects);
    refresh_online();assert(mock_online.state==FOLIO_ONLINE_DOWNLOADING);
    mock_online.received=mock_online.total;
    online_mock_finish(FOLIO_ONLINE_READY,"签名、SHA-256 与固件格式验证成功。");
    assert(!online_active() && !mock_network.running && !s_online_confirm);
}
static void test_online_emergency_isolated(void) {
    fflush(NULL);pid_t pid=fork();assert(pid>=0);
    if(pid==0) {
        unsigned syncs=card_sync_calls,replies=emergency_replies,presentations=frames;
        folio_font_t* font=s_font;folio_activity_t* activity=s_activity;
        mock_shutdown_pending=true;
        handle_power_event(FOLIO_POWER_SHUTDOWN_NOW);
        assert(mock_online_cancel && mock_online.busy && s_font==font && s_activity==activity);
        assert(card_sync_calls==syncs && emergency_replies==replies && frames==presentations);
        assert(mock_network.running); // Never stop the worker's connection early.
        mock_online.busy=false;mock_online.state=FOLIO_ONLINE_CANCELLED;
        emergency_shutdown();
        assert(!s_font && !s_reader && !s_activity && !s_catalog);
        assert(!mock_network.running && card_sync_calls==syncs+1 && emergency_replies==replies+1);
        assert(frames==presentations);fflush(NULL);_exit(0);
    }
    int status;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
}
static void test_online_install_faults_isolated(void) {
    for(unsigned mode=0;mode<7;++mode) {
        fflush(NULL);pid_t pid=fork();assert(pid>=0);
        if(pid==0) {
            unsigned installs=online_installs,syncs=card_sync_calls,restarts=restart_calls;
            if(mode==6) {
                folio_online_state_t before=mock_online.state;
                handle_tap(480,364);assert(mock_online.state==before);
                assert(online_installs==installs);
                fflush(NULL);_exit(0);
            }
            if(mode==0)mock_online_start_fail=true;
            if(mode==1)fail_all_fonts=true;
            if(mode==2)mock_card_unmounted=true;
            if(mode==3){open_book_absolute(0,false);enter_update();fail_font_reader_save=true;}
            if(mode==4){mock_power_event_at=power_polls+2;mock_power_event=FOLIO_POWER_OFF_REQUEST;}
            if(mode==5)mock_power_unsafe_at=power_polls+2;
            tap_rect(done_rect());assert(s_online_confirm);tap_rect(done_rect());
            assert(!mock_online.busy && !s_online_install_owned && !s_online_confirm);
            assert(online_installs==installs+(mode==0?1:0));
            assert(card_sync_calls==syncs && restart_calls==restarts);
            if(mode==0 || mode>=4)assert(s_font && s_activity && s_catalog);
            if(mode==2)assert(!s_card_ready && !s_activity && !s_catalog);
            if(mode==3)assert(s_reader && s_view==VIEW_ERROR);
            if(mode==4)assert(s_deferred_power==FOLIO_POWER_OFF_REQUEST && !s_suspend_pending);
            if(mode==5)assert(!mock_update_power_safe && strstr(s_online_message,"20%"));
            fflush(NULL);_exit(0);
        }
        int status;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
}
static void test_online_update_ui(const char* directory,const char* font_path) {
    s_power_initialized=true;s_ui_task=xTaskGetCurrentTaskHandle();
    bool previous_credentials=mock_network.has_credentials;
    enter_update();assert(!s_update_local && !online_active());
    unsigned checks=online_checks,downloads=online_downloads,installs=online_installs;
    save_pgm(directory,"190-online-idle-ui-mock");
    for(unsigned i=0;i<5;++i){advance_clock(60);refresh_online();}
    assert(online_checks==checks && !mock_network.running); // No background polling.
    handle_tap(480,364);assert(mock_online.state==FOLIO_ONLINE_IDLE);
    save_pgm(directory,"191-online-stable-ui-mock");
    char channel_error[80];
    assert(!folio_online_update_start_check("beta",channel_error,sizeof(channel_error)));
    assert(strstr(channel_error,"更新通道无效") && !mock_online.busy && online_checks==checks);
    checks=online_checks;mock_network.has_credentials=previous_credentials;
    s_card_ready=false;tap_rect(done_rect());assert(online_checks==checks && strstr(s_online_message,"TF"));
    save_pgm(directory,"192-online-no-card-ui-mock");s_card_ready=true;
    mock_network.has_credentials=false;tap_rect(done_rect());
    assert(!online_active() && online_checks==checks && strstr(s_online_message,"Wi-Fi"));
    save_pgm(directory,"193-online-network-error-ui-mock");
    handle_tap(180,1032);assert(s_view==VIEW_WIFI && s_online_wifi_setup);
    go_back();assert(s_view==VIEW_UPDATE && !s_online_wifi_setup && !mock_network.running && online_checks==checks);
    mock_network.has_credentials=true;mock_network_delay_connect=true;
    tap_rect(done_rect());assert(s_online_wait==1 && !mock_online.busy);
    save_pgm(directory,"194-online-connecting-ui-mock");
    advance_clock(301);assert(!idle_lock_due(false)); // Connection wait also inhibits idle lock.
    go_back();assert(s_view==VIEW_UPDATE && s_online_leaving && !mock_online.busy);
    refresh_online();assert(s_view==VIEW_DEVICE && !online_active() && !mock_network.running);
    mock_network_delay_connect=false;enter_update();online_mock_check();
    assert(!strcmp(mock_online.channel,"stable"));
    save_pgm(directory,"195-online-checking-ui-mock");
    advance_clock(301);assert(!idle_lock_due(false));
    // Worker callbacks are atomic-only, including generic I/O cancellation routing.
    unsigned polls=power_polls;void* ui=s_ui_task;s_ui_task=(void*)2;mock_online_worker=true;mock_wr_cancelled=true;
    assert(!folio_main_io_cancelled() && online_card_ready(NULL));
    char error[192];assert(!online_install_allowed(NULL,error,sizeof(error)));
    assert(power_polls==polls);mock_online_worker=false;assert(folio_main_io_cancelled());
    mock_wr_cancelled=false;s_ui_task=ui;
    test_online_emergency_isolated();
    // No-release (404) is honest, not a fabricated 'up to date' success.
    online_mock_finish(FOLIO_ONLINE_NO_RELEASE,"此通道尚无发布（HTTP 404），请稍后重试。");
    assert(!online_active() && !mock_network.running);
    save_pgm(directory,"196-online-no-release-ui-mock");
    const char* errors[]={"HTTPS 证书验证失败，请检查日期与时间。","设备时间无效，请先在日期与时间中校时。",
        "发布签名验证失败，未接受更新文件。","网络连接失败，请设置 Wi-Fi 后重试。"};
    for(unsigned i=0;i<4;++i) {
        online_mock_check();online_mock_finish(FOLIO_ONLINE_FAILED,errors[i]);
        char name[80];snprintf(name,sizeof(name),"197-online-failure-%u-ui-mock",i);save_pgm(directory,name);
        assert(!online_active() && !mock_network.running && online_installs==installs);
    }
    online_mock_check();snprintf(mock_online.version,sizeof(mock_online.version),"0.8.33-test");
    snprintf(mock_online.notes,sizeof(mock_online.notes),"改善阅读体验。此画面为 UI 状态模拟。");mock_online.total=3000000;
    online_mock_finish(FOLIO_ONLINE_AVAILABLE,"签名发布有效，点击下载后才会保存更新文件。");
    save_pgm(directory,"198-online-available-ui-mock");
    assert(online_downloads==downloads && online_installs==installs); // Checking never downloads.
    tap_rect(done_rect());refresh_online();assert(online_downloads==downloads+1 && online_installs==installs);
    mock_online.received=1200000;advance_clock(2);refresh_online();
    save_pgm(directory,"199-online-downloading-ui-mock");
    unsigned presentations=frames;
    for(unsigned i=0;i<20;++i){mock_online.received+=1000;refresh_online();}
    assert(frames==presentations); // No per-chunk refresh.
    handle_tap(490,268);assert(!s_update_local && mock_online.busy); // Disabled local tab.
    s_view=VIEW_DEVICE;handle_tap(180,512);assert(s_view==VIEW_DEVICE); // Busy blocks storage scan.
    show_root(VIEW_HOME);assert(s_view==VIEW_DEVICE && s_online_leaving && mock_online.busy);
    assert(mock_online_cancel && mock_network.running);
    online_mock_finish(FOLIO_ONLINE_CANCELLED,"下载已取消。");assert(s_view==VIEW_HOME && !online_active());
    enter_update();online_mock_check();fail_transfer_stop=true;
    online_mock_finish(FOLIO_ONLINE_FAILED,"模拟网络失败。");
    assert(online_active() && mock_network.running && !mock_online.busy);
    advance_clock(301);assert(!idle_lock_due(false));handle_key(1,false);
    fail_transfer_stop=false;refresh_online();assert(!online_active() && s_view==VIEW_DEVICE);
    enter_update();online_mock_ready();
    save_pgm(directory,"200-online-ready-ui-mock");
    test_online_install_faults_isolated();
    tap_rect(done_rect());assert(s_online_confirm && online_installs==installs);
    save_pgm(directory,"201-online-confirm-ui-mock");
    handle_key(1,false);assert(!s_online_confirm && s_view==VIEW_UPDATE && online_installs==installs);
    tap_rect(done_rect());mock_update_power_safe=false;tap_rect(done_rect());
    assert(online_installs==installs && !s_online_confirm && strstr(s_online_message,"20%"));
    save_pgm(directory,"202-online-power-refused-ui-mock");mock_update_power_safe=true;
    // Keep a real external preference; installation uses only a temporary built-in.
    snprintf(s_settings.font_path,sizeof(s_settings.font_path),"%s",font_path);
    char preference[256];snprintf(preference,sizeof(preference),"%s",s_settings.font_path);
    tap_rect(done_rect());tap_rect(done_rect());
    assert(online_installs==installs+1 && s_online_install_owned && mock_online.busy);
    assert(!s_reader && !s_activity && !s_catalog && !strcmp(s_active_font,FOLIO_FONT_BUILTIN_PATH));
    assert(!strcmp(s_settings.font_path,preference));
    save_pgm(directory,"203-online-installing-ui-mock");
    unsigned syncs=card_sync_calls;folio_font_t* fallback=s_font;
    fflush(NULL);pid_t guard=fork();assert(guard>=0);
    if(guard==0) {
        assert(!prepare_card_change() && s_font==fallback && card_sync_calls==syncs && mock_online.busy);
        fflush(NULL);_exit(0);
    }
    int guard_status;assert(waitpid(guard,&guard_status,0)==guard && WIFEXITED(guard_status) && WEXITSTATUS(guard_status)==0);
    assert(!mock_online_cancel && online_install_allowed(NULL,error,sizeof(error)));
    // Unsafe PMU state revokes the worker's final install authorization.
    mock_update_power_safe=false;refresh_online();
    assert(mock_online_cancel && !online_install_allowed(NULL,error,sizeof(error)) && !s_activity);
    online_mock_finish(FOLIO_ONLINE_CANCELLED,"电量不安全，安装已取消。");
    assert(!s_online_install_owned && s_activity && s_catalog && !strcmp(s_settings.font_path,preference));
    assert(!strcmp(s_active_font,preference));mock_update_power_safe=true;
    save_pgm(directory,"204-online-cancelled-ui-mock");
    // KEY2 and an error-page return both wait for cancellation before leaving.
    online_mock_check();handle_key(1,true);
    assert(s_online_leaving && mock_online.busy && mock_network.running);
    online_mock_finish(FOLIO_ONLINE_CANCELLED,"检查已取消。");assert(s_view==VIEW_DEVICE);
    enter_update();online_mock_check();show_error("模拟可恢复错误",VIEW_UPDATE);go_back();
    assert(s_view==VIEW_ERROR && mock_online.busy && s_online_leaving);
    online_mock_finish(FOLIO_ONLINE_CANCELLED,"检查已取消。");assert(s_view==VIEW_DEVICE);
    // Short power waits for the worker, then locks without unmounting its card.
    enter_update();online_mock_check();unsigned suspends=power_suspend_calls;
    handle_power_event(FOLIO_POWER_LOCK);assert(s_suspend_pending && mock_online.busy && power_suspend_calls==suspends);
    online_mock_finish(FOLIO_ONLINE_CANCELLED,"检查已取消。");
    mock_lock_success=true;suspend_device(s_suspend_power_off);mock_lock_success=false;
    assert(!s_suspend_pending && power_suspend_calls==suspends+1 && card_sync_calls==syncs);
    // Long power is only a confirmation. It may not silently become power-off.
    enter_update();online_mock_check();unsigned off=voluntary_off_calls;
    handle_power_event(FOLIO_POWER_OFF_REQUEST);
    assert(s_view==VIEW_OFF_CONFIRM && mock_online_cancel && !s_suspend_pending);
    online_mock_finish(FOLIO_ONLINE_CANCELLED,"检查已取消。");assert(s_view==VIEW_OFF_CONFIRM && voluntary_off_calls==off);
    go_back();assert(s_view==VIEW_UPDATE && voluntary_off_calls==off);
    online_mock_check();fallback=s_font;assert(!handle_card_lost());
    assert(s_online_card_lost && mock_online.busy && s_font==fallback && !online_card_ready(NULL));
    online_mock_finish(FOLIO_ONLINE_CANCELLED,"TF 卡不可用。");
    assert(!s_card_ready && !s_online_card_lost && s_view==VIEW_UPDATE && !online_active());
    save_pgm(directory,"205-online-card-lost-ui-mock");
    load_card(false,false);assert(s_card_ready && s_font);
    // Successful OTA is mocked, but actual UI close/sync/reboot ordering is tested.
    enter_update();online_mock_check();
    snprintf(mock_online.version,sizeof(mock_online.version),"0.8.33-test");
    online_mock_finish(FOLIO_ONLINE_AVAILABLE,"签名发布可下载。");
    tap_rect(done_rect());refresh_online();online_mock_finish(FOLIO_ONLINE_READY,"完整校验成功。");
    tap_rect(done_rect());tap_rect(done_rect());unsigned restarts=restart_calls;syncs=card_sync_calls;
    // The worker finishes just BEFORE the UI processes a short-power event.
    // Quiesce must not erase INSTALLED or reopen consumers and lose the reboot.
    mock_online.busy=false;mock_online.state=FOLIO_ONLINE_INSTALLED;
    suspends=power_suspend_calls;handle_power_event(FOLIO_POWER_LOCK);
    assert(s_suspend_pending && s_online_install_owned && s_online_operation);
    assert(power_suspend_calls==suspends && restart_calls==restarts && card_sync_calls==syncs);
    refresh_online();
    assert(restart_calls==restarts+1 && card_sync_calls==syncs+1 && !s_font && !s_activity && !s_catalog);
    s_suspend_pending=false; // Mock esp_restart returns; real hardware reboots.
    load_card(false,false);assert(s_font && s_card_ready);
    s_settings.font_path[0]=0;mock_network.has_credentials=previous_credentials;
    puts("Online UI SIMULATION passed: manual stable-only, local mode, STA wait, honest failures, separate download + double install confirmation, cached power revoke, cancellation/back/KEY2/error/root/card-loss, PMU priority, no premature unmount, font restore, idle guard and throttled progress. No real network/signature/OTA in this UI test.");
}
