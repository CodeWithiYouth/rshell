// GUI 状态回归使用隐藏窗口，不发送真实键鼠输入，不读取用户的会话配置。
#include "../src/gui_main.cpp"
#include <iostream>
#include <stdexcept>

namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
std::string output(const SessionState& state) {
    std::string text;
    for (std::size_t i = 0; i < state.termData.lineCount(); ++i)
        text += state.termData.plainLine(i) + "\n";
    return text;
}
void registerClass(const wchar_t* name, WNDPROC proc) {
    WNDCLASSW wc{}; wc.hInstance = g_app.inst; wc.lpfnWndProc = proc; wc.lpszClassName = name;
    check(RegisterClassW(&wc) != 0, "register test window class");
}
void dispatchText(UINT_PTR connection, const std::string& text) {
    SendMessageW(g_app.main, WM_SHELL_DATA, connection, reinterpret_cast<LPARAM>(new std::string(text)));
}
void dispatchFs(UINT_PTR connection, const FsEvent& event) {
    SendMessageW(g_app.main, WM_FS_EVENT, connection, reinterpret_cast<LPARAM>(new FsEvent(event)));
}
void stateRegression() {
    check(g_tabs.size() == 1, "initial blank tab exists");
    g_app.currentSession = "alpha";
    g_app.termData.reset(); g_app.termData.feed("ALPHA\r\n");
    g_app.fsPath = "/alpha"; g_app.fsHome = "/home/alpha";
    g_app.currentPassword.assign(L"alpha-test-secret");
    g_app.pendingCredentialTarget = "alpha-target";
    g_app.pendingCredentialSession = "alpha";
    const auto firstConnection = g_app.connectionId, firstTab = g_app.tabId;
    const auto firstShell = g_app.shell;
    const auto firstFs = g_app.fs;
    showTerminalSearch(true); SetWindowTextW(g_app.searchEdit, L"ALPHA");
    newSessionTab();
    check(g_tabs.size() == 2 && g_activeTab == 1, "new tab becomes active");
    check(g_app.shell != firstShell && g_app.fs != firstFs, "backends belong to each tab");
    check(g_app.currentPassword.empty() && g_app.pendingCredentialTarget.empty(), "credentials do not move into a new tab");
    check(g_app.fsPath.empty() && output(g_app).find("ALPHA") == std::string::npos, "new tab starts with separate output and directory");
    g_app.currentSession = "beta"; g_app.termData.reset(); g_app.termData.feed("BETA\r\n");
    g_app.fsPath = "/beta";
    Session savedAlpha; savedAlpha.name="alpha"; savedAlpha.host="alpha.example.test";
    g_app.store.upsert(savedAlpha);
    g_mgr = SessionMgrState();
    HWND manager=CreateWindowW(L"TabsManagerProbe",L"",WS_POPUP,0,0,mgrWidth(),mgrHeight(),nullptr,nullptr,g_app.inst,nullptr);
    check(manager!=nullptr,"create hidden session manager"); g_mgr.dlg=manager;
    Session added; added.name="new-config"; added.host="new.example.test";
    mgrLoadSession(added);
    check(mgrSave(manager) && g_app.currentSession=="beta", "saving another configuration never relabels the active connection");
    const auto& saved=g_app.store.sessions();
    for (std::size_t i=0;i<saved.size();++i) if (saved[i].name=="alpha") g_mgr.selected=static_cast<int>(i);
    savedAlpha.name="renamed-alpha"; mgrLoadSession(savedAlpha);
    check(mgrSave(manager) && findSessionState(firstConnection,true)->currentSession=="renamed-alpha" && g_app.currentSession=="beta",
          "renaming a saved session updates only matching tabs");
    check(findSessionState(firstConnection,true)->pendingCredentialTarget==credentialTarget("renamed-alpha"), "pending credentials follow the correct renamed session");
    DestroyWindow(manager);
    const auto secondConnection = g_app.connectionId;
    RECT before{}; GetWindowRect(g_app.term, &before);
    dispatchText(firstConnection, "BACKGROUND_ALPHA\r\n\x1b]0;alpha-title\a");
    check(output(g_app).find("BACKGROUND_ALPHA") == std::string::npos, "background output is isolated");
    check(output(*findSessionState(firstConnection, true)).find("BACKGROUND_ALPHA") != std::string::npos, "background output is retained");
    check(findSessionState(firstConnection, true)->unread, "background activity is marked");
    FsEvent listing; listing.kind = FsEventKind::Listing; listing.path = "/alpha/logs";
    RemoteEntry entry; entry.name = "alpha.log"; entry.path = "/alpha/logs/alpha.log";
    listing.entries.push_back(entry); dispatchFs(firstConnection, listing);
    check(g_app.fsPath == "/beta" && g_app.fsEntries.empty(), "background SFTP listing cannot replace visible directory");
    check(findSessionState(firstConnection, true)->fsEntries.front().name == "alpha.log", "background directory listing is retained");
    auto* stats = new RemoteStats(); stats->valid = true; stats->host = "alpha-host";
    SendMessageW(g_app.main, WM_FS_STATS, firstConnection, reinterpret_cast<LPARAM>(stats));
    check(g_app.stats.host.empty() && findSessionState(firstConnection, true)->stats.host == "alpha-host", "remote metrics are isolated");
    RECT afterListing{}; GetWindowRect(g_app.term, &afterListing);
    check(EqualRect(&before, &afterListing), "background listings do not change foreground layout");
    FsEvent started; started.kind = FsEventKind::Started; started.path = "/same-name";
    started.message = "same-name"; started.total = 100; started.cancel = std::make_shared<std::atomic<bool>>(false);
    dispatchFs(firstConnection, started); dispatchFs(secondConnection, started);
    FsEvent progress; progress.kind = FsEventKind::Progress; progress.path = started.path;
    progress.bytes = 37; progress.total = 100; dispatchFs(firstConnection, progress);
    check(g_app.downloads.front().done == 0 && findSessionState(firstConnection, true)->downloads.front().done == 37,
          "same-name transfers cannot update another tab");
    RECT after{}; GetWindowRect(g_app.term, &after);
    // 可见标签的新任务会展开面板；背景事件不能再次改变该标签的布局。
    dispatchFs(firstConnection, progress); RECT unchanged{}; GetWindowRect(g_app.term, &unchanged);
    check(EqualRect(&after, &unchanged), "background transfer events do not relayout visible controls");
    auto* alpha = findSessionState(firstConnection, true);
    alpha->holdTermPaint = true; alpha->lastShellOutputTick = GetTickCount() - 300;
    alpha->cwdHookPhase = 3; alpha->cwdHookAcked = true;
    alpha->cwdHookDeadline = GetTickCount64() + 1000;
    SendMessageW(g_app.main, WM_TIMER, firstTab * 8 + kCwdHookTimer4, 0);
    check(!alpha->holdTermPaint && !g_app.holdTermPaint, "cwd timers are dispatched to their own tab");
    g_app.downloads.front().state = SessionState::DownloadState::Done;
    alpha->downloads.front().state = SessionState::DownloadState::Done;
    switchSessionTab(0);
    check(g_app.shell == firstShell && g_app.fs == firstFs, "switching restores original backend objects");
    check(g_app.fsPath == "/alpha/logs" && g_app.currentPassword.reveal() == L"alpha-test-secret", "directory and credentials survive switching");
    check(g_app.searchOpen && textOf(g_app.searchEdit) == L"ALPHA", "search query is retained per tab");
    check(!g_app.unread && output(g_app).find("BACKGROUND_ALPHA") != std::string::npos, "activating a tab reveals its unread output");
    switchSessionTab(1);
    const auto betaText = output(g_app);
    closeSessionTab(0);
    check(g_tabs.size() == 1 && g_app.connectionId == secondConnection, "closing another tab preserves selected connection");
    dispatchText(firstConnection, "STALE_CALLBACK"); dispatchFs(firstConnection, listing);
    check(output(g_app) == betaText && g_app.fsPath == "/beta", "late callbacks from a closed tab are discarded");
    const auto stale = g_app.connectionId;
    disconnectSession(); dispatchText(stale, "STALE_RECONNECT");
    check(output(g_app).find("STALE_RECONNECT") == std::string::npos, "callbacks from old connection generations are discarded");
    for (int i = 0; i < 16; ++i) { newSessionTab(); g_app.currentSession = "tab-" + std::to_string(i); }
    MoveWindow(g_app.main,0,0,900,650,FALSE);
    check(std::all_of(g_tabs.begin(),g_tabs.end(),[](const std::unique_ptr<SessionState>& state) {
        return !state || (state->termData.cols()==g_app.termData.cols() && state->termData.rows()==g_app.termData.rows());
    }), "resizing updates inactive terminal grids");
    for (int dpi : {96,144,192}) {
        g_app.dpi = dpi;
        for (int width : {760,1120,1600}) {
            const int pixels = dp(width); ensureActiveTabVisible(pixels);
            const auto layout = sessionTabsLayout(pixels);
            check(g_app.firstVisibleTab <= static_cast<int>(g_activeTab) &&
                  static_cast<int>(g_activeTab) < g_app.firstVisibleTab + layout.capacity, "active tab remains visible at varied DPI");
            check(!layout.items.empty() && layout.items.back().right < layout.previous.left && layout.next.right < layout.add.left,
                  "overflow navigation never overlaps tab or add button");
        }
    }
    g_app.dpi = 96; createFonts();
    cycleSessionTab(1); check(g_activeTab == 0, "next tab wraps to first");
    cycleSessionTab(-1); check(g_activeTab == g_tabs.size()-1, "previous tab wraps to last");
    while (g_tabs.size() > 1) closeSessionTab(g_tabs.size()-1);
    closeSessionTab(0);
    check(g_tabs.size() == 1 && g_app.currentSession.empty() && g_app.currentPassword.empty(), "closing last tab leaves clean blank tab");
    g_app.termData.feed("LOGIN_BANNER\r\n$ ");
    g_app.cwdHookSnapshot=std::make_unique<Terminal>(g_app.termData);
    g_app.cwdHookPhase=1; g_app.holdTermPaint=true;
    g_app.termData.feed(" stty -echo\r\n"); sendInput("echo QUEUED\r");
    {
        TerminalDisplayScope display;
        check(output(g_app).find("stty -echo")==std::string::npos && output(g_app).find("LOGIN_BANNER")!=std::string::npos,
              "a frozen tab paints its own snapshot instead of another tab's previous pixels");
    }
    check(output(g_app).find("stty -echo")!=std::string::npos, "snapshot painting preserves the live protocol decoder");
    check(g_app.cwdHookPendingInput=="echo QUEUED\r", "input is held while shell integration changes echo mode");
    handleCwdHookAck("wrong-connection:1");
    check(!g_app.cwdHookAcked, "stale integration acknowledgements are ignored");
    stopCwdHook();
    check(!g_app.holdTermPaint && g_app.cwdHookPendingInput.empty() && output(g_app).find("stty -echo")==std::string::npos,
          "disconnecting integration restores original display and drops unsent input");
}

template<typename Predicate>
void waitUntil(Predicate ready, const char* label) {
    const auto deadline = GetTickCount64() + 20000;
    while (!ready() && GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        Sleep(5);
    }
    check(ready(), label);
}

void renderTabsPreview() {
    const char* path=std::getenv("RSHELL_TABS_PREVIEW");
    if (!path) return;
    RECT client{}; GetClientRect(g_app.main,&client);
    const int width=client.right, height=g_app.titleH+dp(40);
    HDC screen=GetDC(nullptr), dc=CreateCompatibleDC(screen);
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width; info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
    void* pixels=nullptr;
    HBITMAP bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if (!bitmap) { DeleteDC(dc); ReleaseDC(nullptr,screen); throw std::runtime_error("create preview bitmap"); }
    HGDIOBJ old=SelectObject(dc,bitmap);
    fillRect(dc,RECT{0,0,width,height},ui::bgApp);
    paintTitleBar(dc,RECT{0,0,width,height}); paintSessionTabs(dc); GdiFlush();
    BITMAPFILEHEADER header{}; header.bfType=0x4D42; header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);
    header.bfSize=header.bfOffBits+width*height*4;
    std::ofstream out(utf8ToPath(path),std::ios::binary);
    out.write(reinterpret_cast<const char*>(&header),sizeof(header));
    out.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(info.bmiHeader));
    out.write(static_cast<const char*>(pixels),width*height*4);
    SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(nullptr,screen);
}

void networkRegression(int port, const std::string& upload, const std::string& download, const std::string& key) {
    Session alpha; alpha.name="alpha-ssh"; alpha.host="127.0.0.1"; alpha.port=port; alpha.user="tester";
    Session beta=alpha; beta.name="beta-ssh"; beta.keyPath=key;
    g_app.store.upsert(alpha); g_app.store.upsert(beta);
    connectToSession(alpha,L"testpass",L"");
    const UINT_PTR alphaConnection=g_app.connectionId;
    connectStoredSession(beta.name);
    check(g_tabs.size()==2 && g_activeTab==1, "opening another saved session preserves a connecting tab");

    const UINT_PTR betaConnection=g_app.connectionId;
    auto state = [](UINT_PTR id) { return findSessionState(id,true); };
    const bool testHooks=std::getenv("RSHELL_TEST_CWD_HOOKS")!=nullptr;
    bool queuedAlpha=false, queuedBeta=false;
    waitUntil([&] {
        auto queueDuringIntegration=[&](UINT_PTR connection,bool& queued,const char* command) {
            if (testHooks && !queued && state(connection)->cwdHookPhase) {
                SessionStateScope scope(state(connection)); sendInput(command); queued=true;
            }
        };
        queueDuringIntegration(alphaConnection,queuedAlpha,"echo QUEUED_ALPHA\r");
        queueDuringIntegration(betaConnection,queuedBeta,"echo QUEUED_BETA\r");
        return state(alphaConnection)->fsReady && state(betaConnection)->fsReady &&
            !state(alphaConnection)->connecting && !state(betaConnection)->connecting &&
            (!testHooks || (queuedAlpha && queuedBeta));
    }, "two real SSH and SFTP connections establish concurrently");
    check(state(alphaConnection)->shell->running() && state(betaConnection)->shell->running(), "both real shells stay connected");

    // 全局 AI 面板：换标签要跟着换目标主机，不能串台（以前两个标签的 host 会反过来）
    {
        const bool wasOpen = g_app.aiOpen;
        if (!wasOpen) toggleAiPanel();
        switchSessionTab(0);
        check(g_app.aiTab != nullptr && g_app.aiTab->hostLabel() == L"alpha-ssh",
              "the AI panel targets the first tab's host");
        switchSessionTab(1);
        check(g_app.aiTab->hostLabel() == L"beta-ssh",
              "the AI panel targets the second tab's host");

        // 工具可以按会话名直接操作另一个标签，不用 ssh 跳过去
        void* current = g_app.aiTab->resolveHostForTest("");
        check(current == g_app.fs.get(), "empty host means the current tab");
        SessionState* other = nullptr;
        for (auto& state : g_tabs)
            if (state && state->currentSession == "alpha-ssh") other = state.get();
        check(other != nullptr && g_app.aiTab->resolveHostForTest("alpha-ssh") == other->fs.get(),
              "a host name routes tools to that tab's connection");
        check(g_app.aiTab->resolveHostForTest("no-such-host") == nullptr,
              "an unknown host name resolves to nothing");
        if (!wasOpen) toggleAiPanel();
    }
    if (testHooks) {
        check(queuedAlpha && queuedBeta, "each tab accepts queued input during its own integration phase");
        waitUntil([&] { return !state(alphaConnection)->cwdHookPhase && !state(betaConnection)->cwdHookPhase &&
            output(*state(alphaConnection)).find("QUEUED_ALPHA")!=std::string::npos &&
            output(*state(betaConnection)).find("QUEUED_BETA")!=std::string::npos; },
                  "delayed split integration acknowledgements finish and release queued input");
        for (auto connection : {alphaConnection,betaConnection}) {
            const auto text=output(*state(connection));
            check(text.find("stty -echo")==std::string::npos && text.find("__rshell_cwd")==std::string::npos,
                  "integration commands never remain in visible output or history");
            check(text.find("rshell-test-shell")!=std::string::npos, "login banner is retained through integration");
        }
    }
    sendInput("echo BETA_ONLY\r");
    switchSessionTab(0); writeWideInput(L"echo 中文_ALPHA\r");
    state(betaConnection)->shell->write("echo BACKGROUND_BETA\r");
    waitUntil([&] { return output(*state(alphaConnection)).find("中文_ALPHA")!=std::string::npos &&
        output(*state(betaConnection)).find("BACKGROUND_BETA")!=std::string::npos; }, "foreground Chinese and background output arrive on their own sessions");
    check(output(g_app).find("BETA_ONLY")==std::string::npos && output(*state(betaConnection)).find("中文_ALPHA")==std::string::npos,
          "real terminal input and output never cross tabs");
    fsNavigate("/alpha-dir");
    { SessionStateScope scope(state(betaConnection)); fsNavigate("/beta-dir"); }
    waitUntil([&] { return state(alphaConnection)->fsPath=="/alpha-dir" && state(betaConnection)->fsPath=="/beta-dir"; }, "two real SFTP directories stay independent");
    state(betaConnection)->fs->upload(upload,"/beta-dir/upload.bin");
    auto downloadCancel=std::make_shared<std::atomic<bool>>(false);
    addDownloadTask("/alpha-dir/alpha.txt",L"alpha.txt",23,false,downloadCancel);
    state(alphaConnection)->fs->download("/alpha-dir/alpha.txt",download,downloadCancel);
    for (int i=0;i<8;++i) { switchSessionTab(static_cast<std::size_t>(i%2)); Sleep(5); }
    auto completed = [&](UINT_PTR connection,const std::string& path) {
        const auto& tasks=state(connection)->downloads;
        return std::any_of(tasks.begin(),tasks.end(),[&](const SessionState::DownloadTask& task) {
            return task.remote==path && task.state==SessionState::DownloadState::Done;
        });
    };
    waitUntil([&] { return completed(alphaConnection,"/alpha-dir/alpha.txt") && completed(betaConnection,"/beta-dir/upload.bin"); },
              "background upload and download finish while switching tabs");
    std::ifstream file(utf8ToPath(download),std::ios::binary);
    const std::string downloaded((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
    check(downloaded=="alpha-download-content\n", "download bytes come from the intended server path");
    switchSessionTab(0); renderTabsPreview(); closeSessionTab(1);
    check(g_app.connectionId==alphaConnection && g_app.shell->running(), "closing beta leaves alpha live");
    sendInput("echo ALPHA_AFTER_CLOSE\r");
    waitUntil([&] { return output(g_app).find("ALPHA_AFTER_CLOSE")!=std::string::npos; }, "remaining shell works after closing another tab");
    disconnectSession(); connectToSession(alpha,L"testpass",L"");
    const auto reconnected=g_app.connectionId;
    waitUntil([&] { return !g_app.connecting && g_app.fsReady; }, "active tab reconnects successfully");
    dispatchText(alphaConnection,"STALE_OLD_SSH");
    check(output(g_app).find("STALE_OLD_SSH")==std::string::npos && reconnected!=alphaConnection, "old real-connection callbacks cannot reach reconnected session");
    newSessionTab(); connectToSession(beta,L"testpass",L"");
    std::weak_ptr<SshShell> pending=g_app.shell;
    closeSessionTab(1);
    waitUntil([&] { return pending.expired(); }, "closing a connecting tab releases its backend without touching active shell");
    check(g_app.shell->running(), "active connection survives closing a pending connection");

    // 断线自动重连：用和读线程失败时完全相同的那条消息模拟掉线
    if (std::getenv("RSHELL_TEST_RECONNECT")) {
        const UINT_PTR before = g_app.connectionId;
        // 真的把通道关掉（读线程退出），再补上它掉线时会发的那条消息
        g_app.shell->close();
        PostMessageW(g_app.main, WM_SHELL_CLOSED, before,
                     reinterpret_cast<LPARAM>(new std::string("网络读取失败（-9）")));
        waitUntil([&] { return g_app.reconnectPending; }, "a network drop arms the auto reconnect");
        waitUntil([&] {
            return !g_app.reconnectPending && g_app.shell->running() &&
                   g_app.connectionId != before;
        }, "auto reconnect establishes a new session");
        check(output(g_app).find("连接已自动恢复") != std::string::npos,
              "terminal reports the recovered connection");
        waitUntil([&] { return g_app.fsReady; }, "file channel comes back after the reconnect");

        // exec 通道连续调用必须稳定（曾经「第一条成功、第二条就超时」）
        for (int i = 0; i < 5; ++i) {
            const std::string command = "echo exec-" + std::to_string(i);
            const RemoteFs::ExecOutcome outcome = g_app.fs->execSync(command, 8000, 4096);
            check(outcome.ok && outcome.output.find("exec-" + std::to_string(i)) != std::string::npos,
                  "repeated exec on the same session keeps working");
        }
        // stderr 也要读掉，否则远端写满管道就卡住（&& 在 cmd 和 sh 下都能用）
        {
            const RemoteFs::ExecOutcome outcome =
                g_app.fs->execSync("echo merged-out && echo merged-err 1>&2 && exit 3", 8000, 4096);
            check(outcome.ok && outcome.output.find("merged-out") != std::string::npos &&
                      outcome.output.find("merged-err") != std::string::npos,
                  "stderr is drained into the output");
            check(outcome.exitCode == 3,
                  ("exit status is reported for exec, got " +
                   std::to_string(outcome.exitCode) + " output=[" + outcome.output + "]").c_str());
        }
    }
}

void sessionMgrLayoutRegression() {
    const MgrLayout l = mgrLayout();
    const int width = mgrWidth();
    const int height = mgrHeight();

    auto inside = [&](const RECT& r, const char* what) {
        check(r.left >= 0 && r.top >= 0 && r.right <= width && r.bottom <= height, what);
        check(r.right > r.left && r.bottom > r.top, what);
    };
    inside(l.name, "name field inside dialog");
    inside(l.proxyUrl, "proxy url field inside dialog");
    inside(l.proxyUser, "proxy user field inside dialog");
    inside(l.proxyPass, "proxy password field inside dialog");
    inside(l.keepAliveBox, "keepalive checkbox inside dialog");
    inside(l.keepAliveSec, "keepalive interval inside dialog");
    inside(l.hint, "hint block inside dialog");
    inside(l.pwdState, "password state inside dialog");
    inside(l.pwdClear, "clear password button inside dialog");
    inside(l.save, "save button inside dialog");
    inside(l.remove, "delete button inside dialog");
    inside(l.btnNew, "new button inside dialog");
    inside(l.btnClose, "close button inside dialog");
    inside(l.btnConnect, "connect button inside dialog");

    // 纵向顺序：代理两行在备注之后，状态/按钮在代理之后
    check(l.proxyUrl.top >= l.note.bottom, "proxy row sits below note");
    check(l.proxyUser.top >= l.proxyUrl.bottom, "proxy credentials below proxy url");
    check(l.proxyPass.top == l.proxyUser.top, "proxy user and password share a row");
    check(l.proxyUser.right < l.proxyPass.left, "proxy user and password do not overlap");
    check(l.keepAliveSec.top >= l.proxyPass.bottom, "keepalive row below proxy fields");
    check(l.hint.top >= l.keepAliveSec.bottom, "hint sits below keepalive row");
    check(l.pwdState.top >= l.hint.bottom, "password state below hint");
    check(l.save.top >= l.pwdState.bottom, "action buttons below password state");
    check(l.btnNew.top >= l.save.bottom, "bottom buttons below actions");
    check(l.proxyPassLabelX > l.proxyUser.right && l.proxyPassLabelX < l.proxyPass.left,
          "proxy password label sits between the two fields");
}

void panelScrollRegression() {
    ai::ChatPanel panel;
    ai::PanelFonts fonts;
    fonts.ui = g_app.uiFont;
    fonts.bold = g_app.uiFontBold;
    fonts.mono = g_app.font;
    fonts.caption = g_app.tinyFont;
    panel.setFonts(fonts);
    panel.setBounds(RECT{0, 0, 420, 300});

    // 造出明显超过一屏的内容
    for (int i = 0; i < 40; ++i) panel.addUser(L"第 " + std::to_wstring(i) + L" 条消息");

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, 420, 300);
    HGDIOBJ old = SelectObject(mem, bmp);

    panel.paint(mem);
    check(panel.stickToBottom() && panel.scrollOffset() == 0, "chat starts pinned to the bottom");

    // 滚轮向上 = 看更早内容
    panel.onMouseWheel(WHEEL_DELTA);
    panel.paint(mem);
    check(panel.scrollOffset() > 0, "wheel up scrolls towards older messages");
    check(!panel.stickToBottom(), "scrolling up releases bottom lock");

    // 滚轮向下回到底部后重新锁定
    panel.onMouseWheel(-WHEEL_DELTA);
    panel.paint(mem);
    check(panel.scrollOffset() == 0, "wheel down returns to the bottom");
    check(panel.stickToBottom(), "reaching the bottom re-locks");

    // 滚上去之后新增内容不应该把视图拽回底部
    panel.onMouseWheel(WHEEL_DELTA);
    panel.paint(mem);
    const int parked = panel.scrollOffset();
    panel.addTool(L"读取 /etc/hosts", L"127.0.0.1 localhost", true, L"0.1 KB");
    panel.paint(mem);
    check(panel.scrollOffset() >= parked, "new content does not yank a scrolled-up view");

    // 贴底状态下新增内容要跟随
    panel.onMouseWheel(-WHEEL_DELTA);
    panel.paint(mem);
    panel.addTool(L"读取 /etc/os-release", L"NAME=Linux", true, L"0.2 KB");
    panel.paint(mem);
    check(panel.scrollOffset() == 0, "new content follows when pinned");

    // 思考过程默认展开
    panel.appendReasoning(L"先看磁盘，再看日志");
    const auto& items = panel.items();
    check(!items.empty() && items.back().kind == ai::ItemKind::Reasoning, "reasoning item created");
    check(items.back().expanded, "reasoning is expanded by default");

    // 输入法组字串：拼音必须看得见，光标要停在组字串内部而不是末尾
    panel.focusInput();
    panel.setInput(L"");
    panel.paint(mem);
    const int emptyCaret = panel.caretRect().left;
    panel.setPreedit(L"nihao", 5);
    check(panel.preedit() == L"nihao", "preedit string is stored");
    panel.paint(mem);
    const int composedCaret = panel.caretRect().left;
    check(composedCaret > emptyCaret, "pinyin pushes the caret right so the composition is visible");
    panel.setPreedit(L"nihao", 2);
    panel.paint(mem);
    check(panel.caretRect().left < composedCaret, "composition caret follows the pinyin cursor");
    panel.setPreedit(L"nihao", 99);  // 越界的组字光标要夹回来
    panel.paint(mem);
    check(panel.caretRect().left == composedCaret, "out-of-range composition caret is clamped");
    panel.clearPreedit();
    check(panel.preedit().empty(), "committing the composition clears the preedit");
    panel.paint(mem);
    check(panel.caretRect().left == emptyCaret, "caret returns to the input after committing");

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void aiTabRegression() {    auto visible = [](HWND window) {
        return window != nullptr &&
               (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0;
    };

    toggleAiPanel();
    ai::AiTab* docked = g_app.aiTab;
    check(docked != nullptr, "AI dock panel is created");
    check(g_app.aiOpen, "Ctrl+J opens the docked panel");
    check(visible(docked->window()), "docked panel is visible");

    RECT dockRect{}; GetWindowRect(docked->window(), &dockRect);
    const int dockWidth = dockRect.right - dockRect.left;
    // 窗口只有 900px 宽时，给终端留出最小宽度后 AI 面板会落到下限 280
    check(dockWidth >= 270 && dockWidth < 600,
          ("docked panel keeps a bounded width, got " + std::to_string(dockWidth)).c_str());

    setAiPanelFull(true);
    check(g_app.aiFull, "full screen flag is set");
    RECT fullRect{}; GetWindowRect(docked->window(), &fullRect);
    check(fullRect.right - fullRect.left > dockWidth, "full screen is wider than docked");

    // 全屏下直接关面板：必须连带退出全屏，否则窗口布局会把会话控件永久隐藏
    toggleAiPanel();
    check(!g_app.aiOpen && !g_app.aiFull,
          "closing while full screen also clears the full screen flag");
    check(visible(g_app.term) && visible(g_app.list),
          "terminal and file list come back after closing the full screen panel");
    toggleAiPanel();
    check(g_app.aiOpen && !g_app.aiFull, "the panel reopens docked, not full screen");
    RECT reopened{}; GetWindowRect(docked->window(), &reopened);
    check(reopened.right - reopened.left <= dockWidth + dp(8),
          "the reopened panel keeps the docked width");

    setAiPanelFull(true);
    setAiPanelFull(false);
    check(!g_app.aiFull, "back to docked mode");

    // 面板是全局的一份：新开标签不会新建面板，也不会把它藏起来
    newSessionTab();
    check(g_app.aiTab == docked, "a new session keeps the same global AI panel");
    check(visible(docked->window()), "the panel stays visible on the new tab");
    check(g_app.aiTab->hostLabel() != L"", "the panel has a host label for the new tab");

    for (std::size_t i = 0; i < g_tabs.size(); ++i) {
        if (g_tabs[i] && g_tabs[i]->tabId == g_app.tabId) continue;
        switchSessionTab(i);
        break;
    }
    check(g_app.aiTab == docked && visible(docked->window()),
          "switching tabs keeps the single panel");

    toggleAiPanel();
    check(!g_app.aiOpen && !visible(docked->window()), "Ctrl+J hides the docked panel");
    toggleAiPanel();
    check(g_app.aiOpen && visible(docked->window()), "Ctrl+J reopens it");

    // 关掉一个标签也不会把面板一起销毁
    const std::size_t closedTab = g_activeTab;
    closeSessionTab(closedTab);
    check(g_app.aiTab == docked, "closing a session keeps the global AI panel");
    if (g_app.aiOpen) toggleAiPanel();  // 留下干净状态给后面的用例
}

// 停靠 AI 面板会把终端压窄：必须折行（不是截断），收起后要能并回原样
void terminalDockReflowRegression() {
    std::string original;
    for (int i = 0; i < 120; ++i) original.push_back(static_cast<char>('a' + i % 26));
    g_app.termData.reset();
    g_app.termData.feed(original);
    const int colsWide = g_app.termData.cols();
    check(colsWide > 0, "terminal reports a column count");

    toggleAiPanel();
    check(g_app.aiOpen, "AI panel is docked for the reflow check");
    const int colsDocked = g_app.termData.cols();
    check(colsDocked > 0 && colsDocked < colsWide, "docking the AI panel narrows the terminal");
    check(g_app.termData.cols() == colsDocked, "terminal keeps the docked width while the panel is open");

    toggleAiPanel();
    check(!g_app.aiOpen, "panel closed again");
    check(g_app.termData.cols() == colsWide, "terminal width returns after closing the panel");

    std::string joined;
    for (std::size_t i = 0; i < g_app.termData.lineCount(); ++i)
        joined += g_app.termData.plainLine(i);
    check(joined.compare(0, original.size(), original) == 0,
          "terminal text survives narrowing and widening");
}

// 保险库已锁时点「解锁」，用户在密码框上点「取消」不能跳进 AI 设置表单
void aiUnlockCancelRegression() {
    ai::AiTabContext context;
    context.vault = &g_app.vault;
    context.uiFont = g_app.uiFont;
    context.boldFont = g_app.uiFontBold;
    context.monoFont = g_app.font;
    context.captionFont = g_app.tinyFont;
    context.vaultPresent = []() { return true; };

    int prompts = 0;
    context.ensureVaultUnlocked = [&prompts]() { ++prompts; return false; };  // 用户点了取消
    ai::AiTab* cancelled = ai::AiTab::create(g_app.main, context);
    check(cancelled != nullptr, "standalone AI tab is created");
    cancelled->setBounds(RECT{0, 0, 420, 600});
    cancelled->startSettings(true);
    check(prompts == 1, "the unlock prompt is shown once");
    check(!cancelled->settingsOpen(), "cancelling the unlock prompt keeps the settings form closed");

    // 没有 vault.dat 时不该弹解锁框，直接进设置页填 Key
    int promptsWithoutVault = 0;
    ai::AiTabContext noVault = context;
    noVault.vaultPresent = []() { return false; };
    noVault.ensureVaultUnlocked = [&promptsWithoutVault]() { ++promptsWithoutVault; return false; };
    ai::AiTab* direct = ai::AiTab::create(g_app.main, noVault);
    check(direct != nullptr, "second standalone AI tab is created");
    direct->setBounds(RECT{0, 0, 420, 600});
    direct->startSettings(true);
    check(promptsWithoutVault == 0, "no unlock prompt when the vault file does not exist");
    check(direct->settingsOpen(), "without a vault the settings form opens directly");

    ai::AiTab::destroy(cancelled);
    ai::AiTab::destroy(direct);
}

// 状态栏详情浮窗：悬停时出现，鼠标在状态栏内继续移动不应把它抖没
void statusHintRegression() {
    RECT client{};
    GetClientRect(g_app.main, &client);
    const int statusY = client.bottom - dp(8);
    const int bodyY = client.bottom / 2;
    const auto hover = [&](int x, int y) {
        SendMessageW(g_app.main, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
        SendMessageW(g_app.main, WM_MOUSEHOVER, 0, MAKELPARAM(x, y));
    };
    const auto hintShown = []() {
        return g_app.statusHint != nullptr && IsWindowVisible(g_app.statusHint) != FALSE;
    };

    hover(client.right / 2, statusY);
    check(g_app.statusHint != nullptr, "hovering the status bar creates the details popup");
    check(hintShown(), "the details popup is visible after the hover delay");

    // 手指轻微移动（还在状态栏里）不能让它一闪而过
    SendMessageW(g_app.main, WM_MOUSEMOVE, 0, MAKELPARAM(client.right / 2 + 6, statusY - 2));
    check(hintShown(), "moving inside the status bar keeps the popup visible");

    // 移出状态栏才收起
    SendMessageW(g_app.main, WM_MOUSEMOVE, 0, MAKELPARAM(client.right / 2, bodyY));
    check(!hintShown(), "leaving the status bar hides the popup");
}

// 断线自动重连：排程 / 取消 / 凭据保留
void autoReconnectRegression() {
    Session target;
    target.name = "auto-reconnect-probe";
    target.host = "127.0.0.1";
    target.port = 1;  // 不会真的连上，测试在定时器触发前就取消
    g_app.store.upsert(target);
    g_app.currentSession = target.name;
    g_app.currentPassword.assign(L"probe-secret");

    check(armAutoReconnect(), "auto reconnect arms after a network drop");
    check(g_app.reconnectPending, "reconnect is pending");
    check(g_app.reconnectAttempt == 1, "the first retry is already scheduled");
    check(g_app.reconnectTimerSet, "the retry timer is running");
    check(g_app.currentPassword.reveal() == L"probe-secret",
          "credentials are kept for the retry window");

    cancelAutoReconnect(true);
    check(!g_app.reconnectPending && !g_app.reconnectTimerSet, "cancel stops the retry");
    check(g_app.currentPassword.reveal().empty(), "cancel wipes the retained password");
}

// 高延迟下列目录：从发出请求到结果回来之间，面板要处于「正在加载」
void fsLoadingRegression() {
    const UINT_PTR connection = g_app.connectionId;
    g_app.fsReady = true;
    g_app.store.upsert(Session{});
    fsNavigate("/");
    fsNavigate("/tmp");
    check(g_app.fsLoading, "a directory switch marks the panel as loading");
    check(g_app.fsEntries.empty(), "the old listing is cleared while loading");

    FsEvent listing;
    listing.kind = FsEventKind::Listing;
    listing.path = "/tmp";
    dispatchFs(connection, listing);
    check(!g_app.fsLoading, "the listing result clears the loading state");

    // 刷新（列表还在）也要标出来
    g_app.fsEntries.push_back(RemoteEntry{});
    fsRefresh();
    check(g_app.fsLoading, "a refresh is marked as loading too");
    FsEvent refreshed = listing;
    refreshed.entries = {RemoteEntry{}};
    dispatchFs(connection, refreshed);
    check(!g_app.fsLoading, "a refresh result clears the loading state");

    // 列目录失败也要退出加载态，别一直转圈
    g_app.fsLoading = true;
    FsEvent failure;
    failure.kind = FsEventKind::Error;
    failure.path = "/tmp";
    failure.message = "permission denied";
    g_app.fsQuietPath = "/tmp";
    dispatchFs(connection, failure);
    check(!g_app.fsLoading, "a failed listing clears the loading state");
    g_app.fsError.clear();
    g_app.fsEntries.clear();
    g_app.fsQuietPath.clear();
}

// 分栏分隔条：能拖、拖完记住宽度；SFTP 面板有刷新按钮
void splitterRegression() {
    RECT client{};
    GetClientRect(g_app.main, &client);
    const int beforeSidebar = g_app.sidebarW;
    const int midY = (g_app.fsSplitter.top + g_app.fsSplitter.bottom) / 2;
    const int splitX = (g_app.fsSplitter.left + g_app.fsSplitter.right) / 2;
    check(splitX > 0, "the file splitter has a hit zone");
    check(splitterAt(POINT{splitX, midY}) == 1, "the file splitter is hit-tested");

    SendMessageW(g_app.main, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(splitX, midY));
    check(g_app.splitDrag == 1, "pressing the splitter starts a drag");
    SendMessageW(g_app.main, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(splitX + dp(60), midY));
    check(g_app.sidebarWanted > 0, "dragging sets an explicit sidebar width");
    SendMessageW(g_app.main, WM_LBUTTONUP, 0, MAKELPARAM(splitX + dp(60), midY));
    check(g_app.splitDrag == 0, "releasing ends the drag");
    check(g_app.sidebarW > beforeSidebar, "the sidebar actually got wider");
    check(std::abs(g_app.sidebarW - (beforeSidebar + dp(60))) <= dp(4),
          "the sidebar follows the drag distance");

    // 恢复自动宽度，别影响后面的布局断言
    g_app.sidebarWanted = 0;
    layoutMain(g_app.main);

    // 刷新按钮在侧栏标题行里，且只有连上才可用
    bool found = false;
    for (const auto& button : g_app.buttons) {
        if (button.id != ID_BTN_FS_REFRESH) continue;
        found = true;
        check(button.rc.left >= 0 && button.rc.right <= client.right, "refresh button inside the window");
        check(button.rc.top >= 0 && button.rc.bottom < g_app.contentTop + dp(34),
              "refresh button sits in the sidebar header");
    }
    check(found, "the file panel has a refresh button");
    const bool fsReadyBefore = g_app.fsReady;
    g_app.fsReady = false;
    check(!buttonEnabled(ID_BTN_FS_REFRESH), "refresh is disabled while disconnected");
    g_app.fsReady = true;
    check(buttonEnabled(ID_BTN_FS_REFRESH), "refresh is enabled once connected");
    g_app.fsReady = fsReadyBefore;
}

void titleAiButtonRegression() {
    RECT client{}; GetClientRect(g_app.main, &client);
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    paintTitleBar(dc, RECT{0, 0, client.right, g_app.titleH});
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);

    const RECT& ai = g_app.titleAi;
    check(ai.right > ai.left, "title bar has an AI button");
    check(ai.right <= g_app.titleButtons[0].left, "AI button sits left of the menu button");
    check(ai.left > 0 && ai.right < client.right, "AI button stays inside the title bar");
    check(ai.bottom - ai.top == g_app.titleH, "AI button fills the title bar height");
    check(g_app.titleButtons[0].left - ai.right <= dp(8), "AI button keeps a tight gap to the menu");

    const POINT center{(ai.left + ai.right) / 2, (ai.top + ai.bottom) / 2};
    const bool before = g_app.aiOpen;
    SendMessageW(g_app.main, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(center.x, center.y));
    SendMessageW(g_app.main, WM_LBUTTONUP, 0, MAKELPARAM(center.x, center.y));
    check(g_app.aiOpen != before, "clicking the AI button toggles the panel");
    SendMessageW(g_app.main, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(center.x, center.y));
    SendMessageW(g_app.main, WM_LBUTTONUP, 0, MAKELPARAM(center.x, center.y));
    check(g_app.aiOpen == before, "clicking the AI button again restores the state");
    check(hitAiButton(center), "AI button hit test matches its rectangle");
    check(!hitAiButton(POINT{g_app.titleButtons[0].left + dp(4), center.y}),
          "AI hit test does not swallow the menu button");
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--history-commands") {
        g_app.connectionId=31337;
        std::ofstream out(utf8ToPath(argv[2]),std::ios::binary);
        out << cwdHookInput("stty -echo; "+cwdHookAckCommand(1),1) << '\n'
            << cwdHookInput(buildCwdHookScript(2)+"; "+cwdHookAckCommand(2),2) << '\n'
            << cwdHookInput(buildCwdHookRestoreEcho()+"; "+cwdHookAckCommand(3),3) << '\n'
            << cwdHookInput("stty echo",4) << '\n';
        return out ? 0 : 1;
    }
    HWND window = nullptr;
    const auto settings=std::filesystem::temp_directory_path() / ("rshell-tabs-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+".ini");
    const auto aiSettings=std::filesystem::temp_directory_path() / ("rshell-ai-"+std::to_string(GetCurrentProcessId())+".ini");
    SetEnvironmentVariableW(L"RSHELL_AI_CONFIG", aiSettings.c_str());
    try {
        _putenv_s("RSHELL_NO_UI_SAVE", "1");  // 探针不改用户真实布局
        g_app.inst = GetModuleHandleW(nullptr); g_app.dpi = 96;
        auto runtime = std::make_shared<Libssh2Runtime>(); g_app.runtime = runtime;
        g_app.store=SessionStore::load(pathToUtf8(settings));
        startGdiplus();
        registerClass(kMainClass, mainProc); registerClass(kListClass, fileListProc); registerClass(kTermClass, terminalProc);
        registerClass(L"TabsManagerProbe",sessionMgrProc);
        window = CreateWindowExW(0,kMainClass,L"tab-regression",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
            0,0,1120,720,nullptr,nullptr,g_app.inst,nullptr);
        check(window != nullptr,"create hidden GUI window");
        stateRegression();
        panelScrollRegression();
        sessionMgrLayoutRegression();
        aiTabRegression();
        aiUnlockCancelRegression();
        statusHintRegression();
        autoReconnectRegression();
        fsLoadingRegression();
        splitterRegression();
        titleAiButtonRegression();
        terminalDockReflowRegression();
        if (argc == 5) networkRegression(std::stoi(argv[1]),argv[2],argv[3],argv[4]);
        DestroyWindow(window); window = nullptr;
        stopGdiplus(); g_app.runtime.reset();
        std::error_code error; std::filesystem::remove(settings,error);
        std::cout << "PASS: " << checks << " multi-tab state, event routing, cleanup and DPI checks\n";
        return 0;
    } catch (const std::exception& ex) {
        if (window) DestroyWindow(window);
        std::error_code error; std::filesystem::remove(settings,error);
        std::cerr << "FAIL after " << checks << " checks: " << ex.what() << '\n';
        return 1;
    }
}
