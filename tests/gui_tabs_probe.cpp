// GUI 状态回归使用隐藏窗口，不发送真实键鼠输入，不读取用户的会话配置。
#include "../src/gui_main.cpp"
#include "../src/md_render.h"
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

// 远端会话历史：打开面板自动接上最近一次，点「历史」按钮能切换到旧对话
class ProbeSessionBackend : public ai::ToolBackend {
public:
    std::map<std::string, std::string> files;

    std::string homeDir() override { return "/home/probe"; }
    bool makeDir(const std::string&, std::string&) override { return true; }
    CommandResult runCommand(const std::string&, int, const std::string&) override {
        return CommandResult{};
    }
    bool readFile(const std::string& path, std::size_t, std::string& out, bool& truncated,
                  std::string& error, const std::string&) override {
        const auto it = files.find(path);
        if (it == files.end()) {
            error = "missing " + path;
            return false;
        }
        out = it->second;
        truncated = false;
        return true;
    }
    bool writeFile(const std::string& path, const std::string& content, std::string&,
                   const std::string&) override {
        files[path] = content;
        return true;
    }
    bool listDir(const std::string& path, std::vector<ai::DirEntryView>& out, std::string&,
                 const std::string&) override {
        const std::string prefix = path + "/";
        for (const auto& entry : files) {
            if (entry.first.compare(0, prefix.size(), prefix) != 0) continue;
            const std::string name = entry.first.substr(prefix.size());
            if (name.find('/') != std::string::npos) continue;
            ai::DirEntryView view;
            view.name = name;
            view.size = entry.second.size();
            out.push_back(view);
        }
        return true;
    }
};

void aiSessionHistoryRegression() {
    ProbeSessionBackend store;
    const std::string dir = "/home/probe/.rshell/sessions";

    ai::SessionRecord older;
    older.title = "旧对话";
    older.tags = {"nginx"};
    older.created = older.updated = 1700000000;
    older.messages.push_back({true, "旧的问题"});
    older.messages.push_back({false, "旧回答"});

    ai::SessionRecord newer;
    newer.title = "新对话";
    newer.tags = {"磁盘"};
    newer.created = newer.updated = 1700003600;
    newer.messages.push_back({true, "新的问题"});
    newer.messages.push_back({false, "新回答"});

    store.files[dir + "/" + ai::sessionFileName(older, older.created)] = ai::serializeSession(older);
    store.files[dir + "/" + ai::sessionFileName(newer, newer.created)] = ai::serializeSession(newer);

    ai::AiTabContext context;
    context.uiFont = g_app.uiFont;
    context.boldFont = g_app.uiFontBold;
    context.monoFont = g_app.font;
    context.captionFont = g_app.tinyFont;
    context.hosts = [&store]() {
        ai::AiHostInfo host;
        host.id = 7;
        host.label = L"probe-host";
        host.store = std::shared_ptr<ai::ToolBackend>(&store, [](ai::ToolBackend*) {});
        return std::vector<ai::AiHostInfo>{host};
    };
    context.activeHostId = []() { return static_cast<std::uintptr_t>(7); };
    std::vector<std::wstring> labels;
    int picks = 0;
    context.pickFromList = [&](POINT, const std::vector<std::wstring>& list) {
        labels = list;
        ++picks;
        return 1;  // 选第二行（较旧的那段）
    };

    ai::AiTab* tab = ai::AiTab::create(g_app.main, context);
    check(tab != nullptr, "history panel is created");
    tab->setBounds(RECT{0, 0, 460, 640});
    tab->show(true);

    // 打开面板自动接上最近的「新对话」
    waitUntil([&]() { return tab->transcript().size() >= 2; },
              "the newest conversation is restored automatically");
    check(tab->transcript().front().kind == ai::ItemKind::User, "restored log starts with the user turn");
    check(tab->transcript().front().body == L"新的问题", "the newest conversation is the one restored");

    // 「历史」按钮：连上主机才有，点一下弹列表。
    // 探针窗口是隐藏的，RedrawWindow 不会派发 WM_PAINT，所以直接发一次让面板算出按钮位置。
    SendMessageW(tab->window(), WM_PAINT, 0, 0);
    const RECT button = tab->historyButton();
    check(!IsRectEmpty(&button), "the history button appears once a host is connected");
    const LPARAM at = MAKELPARAM((button.left + button.right) / 2, (button.top + button.bottom) / 2);
    SendMessageW(tab->window(), WM_LBUTTONDOWN, MK_LBUTTON, at);
    SendMessageW(tab->window(), WM_LBUTTONUP, 0, at);
    waitUntil([&]() { return picks == 1; }, "clicking history asks the host for a picker");
    check(labels.size() == 2, "the picker lists both stored conversations");
    check(labels[0].find(L"新对话") != std::wstring::npos, "newest conversation is listed first");
    check(labels[0].find(L"2 条") != std::wstring::npos, "list shows the message count");
    check(labels[0].find(L"#磁盘") != std::wstring::npos, "list shows the tags");

    // 选第二行 → 换成旧对话
    waitUntil([&]() { return tab->transcript().front().body == L"旧的问题"; },
              "picking an older conversation replaces the log");
    check(tab->transcript().size() == 2, "the replaced log holds the stored turns only");

    ai::AiTab::destroy(tab);
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

// 远端文件面板：折叠成窄栏 / 展开
void fsCollapseRegression() {
    const auto visible = [](HWND window) {
        return window != nullptr && (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0;
    };
    g_app.fsCollapsed = false;
    layoutMain(g_app.main);
    const int expandedW = g_app.sidebarW;
    check(expandedW > dp(200), "the file panel starts expanded");
    check(g_app.fsPathRect.right > g_app.fsPathRect.left, "path bar visible when expanded");
    check(g_app.fsSplitter.right > g_app.fsSplitter.left, "file splitter active when expanded");

    SendMessageW(g_app.main, WM_COMMAND, MAKEWPARAM(ID_BTN_FS_TOGGLE, BN_CLICKED), 0);
    check(g_app.fsCollapsed, "toggle collapses the file panel");
    check(g_app.sidebarW < dp(60), "collapsed panel becomes a narrow rail");
    check(g_app.fsPathRect.right == g_app.fsPathRect.left, "path bar hidden when collapsed");
    check(!visible(g_app.list), "file list hidden when collapsed");
    check(g_app.fsSplitter.right == g_app.fsSplitter.left, "no splitter while collapsed");

    SendMessageW(g_app.main, WM_COMMAND, MAKEWPARAM(ID_BTN_FS_TOGGLE, BN_CLICKED), 0);
    check(!g_app.fsCollapsed, "toggling again expands the panel");
    check(g_app.sidebarW == expandedW, "expanded width comes back");
    check(g_app.fsPathRect.right > g_app.fsPathRect.left, "path bar is back");
    check(visible(g_app.list), "file list is back");
}

// 粘贴/复制/全选之后焦点必须回到终端：从菜单触发时弹窗拿走了焦点，
// 不还回去用户就得先点一下终端才能按回车。
void pasteFocusRegression() {
    // 测试会动剪贴板，先把原来的文本存下来，完事再放回去
    std::wstring previous;
    const bool hadText = clipboardText(previous);

    if (!copyTextToClipboard(L"rshell-focus-probe")) {
        // 剪贴板被别的程序（剪贴板管理器）占着时不要判失败：换台机器就能跑
        std::cout << "SKIP clipboard is busy, focus regression skipped\n";
        return;
    }
    SetFocus(g_app.main);
    doPaste();
    check(GetFocus() == g_app.term, "pasting hands focus back to the terminal");

    SetFocus(g_app.main);
    doSelectAll();
    check(GetFocus() == g_app.term, "select-all hands focus back to the terminal");
    check(g_app.hasSelection, "select-all still selects the screen");

    SetFocus(g_app.main);
    doCopy();
    check(GetFocus() == g_app.term, "copy hands focus back to the terminal");

    if (hadText && !previous.empty()) copyTextToClipboard(previous);
}

// ☰ 主菜单改成二级菜单后：顶层只有分组，命令一个都不能丢
void mainMenuRegression() {
    const auto items = buildMainMenu();
    check(items.size() == 8, "main menu keeps six groups, one separator and exit");
    const wchar_t* groups[] = {L"会话", L"标签", L"编辑", L"视图", L"工具", L"帮助"};
    for (int i = 0; i < 6; ++i) {
        const auto& group = items[static_cast<std::size_t>(i)];
        check(group.text == groups[i], "groups keep their order");
        check(group.id == 0 && !group.separator, "a group row only opens its submenu");
        check(!group.children.empty(), "every group has a submenu");
        check(!group.children.front().separator, "a submenu never starts with a separator");
        check(!group.children.back().separator, "a submenu never ends with a separator");
    }
    check(items[6].separator, "exit is separated from the groups");
    check(!items[7].separator && items[7].id == IDM_EXIT && items[7].children.empty(),
          "exit stays a plain top-level item");

    // 叶子项总数与原来的一字排开菜单一致，而且没有重复 id
    int leaves = 0;
    std::vector<int> ids;
    for (const auto& item : items) {
        if (item.separator) continue;
        if (item.children.empty()) {
            ++leaves;
            ids.push_back(item.id);
        }
        for (const auto& child : item.children) {
            if (child.separator) continue;
            ++leaves;
            ids.push_back(child.id);
        }
    }
    check(leaves == 24, "every command of the old flat menu is still reachable");
    check(std::find(ids.begin(), ids.end(), 0) == ids.end(), "no reachable row is a placeholder id");
    std::sort(ids.begin(), ids.end());
    check(std::adjacent_find(ids.begin(), ids.end()) == ids.end(), "no command appears twice");

    // 顶层高度：233dp 左右，不能再撑满整屏（老菜单 708dp）
    const int height = popup::contentHeight(items);
    HDC dc = GetDC(g_app.main);
    const int width = popup::menuWidth(dc, items);
    ReleaseDC(g_app.main, dc);
    check(height <= dp(250), "the top-level menu no longer fills the window");
    check(height >= dp(180), "the top-level menu still shows all six groups");
    check(width >= dp(200), "the menu keeps a clickable minimum width");

    // 分组里的动态文案跟着状态走
    const auto& session = items[0].children;
    check(session[0].id == IDM_OPEN && session[0].shortcut == L"Ctrl+O",
          "session management keeps its shortcut");
    check(!g_app.shell->running(), "the probe is not connected");
    check(session[2].id == IDM_CONNECT && session[2].text == L"连接…",
          "connect/disconnect label follows the connection state");
    const auto& tools = items[4].children;
    check(tools.size() == 3 && tools[0].id == IDM_CHECK_UPDATE, "tools keeps the update commands");
    check(tools[0].text == L"立即检查更新…", "the manual check is labelled as immediate");
    check(tools[1].text == (g_app.updateCheckOnStart ? L"启动时检查更新 · 已开启"
                                                     : L"启动时检查更新 · 已关闭"),
          "the startup check toggle shows its state");
    check(tools[2].id == IDM_FS_EDITOR &&
              tools[2].text == (g_app.editorPath.empty() ? L"选择文本编辑器…"
                                                         : L"更换文本编辑器…"),
          "the editor setting follows whether an editor was chosen");
}

// 二级菜单的悬停修正：父项左右内边距仍然算“还在这一项上”，
// 鼠标划向子菜单时不会因为经过内边距被判定为空白而把子菜单关掉。
void submenuHoverRegression() {
    HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 220, 120,
                                  g_app.main, nullptr, g_app.inst, nullptr);
    check(window != nullptr, "create submenu hover probe window");

    const popup::State saved = popup::g_popup;
    popup::g_popup = popup::State();
    popup::g_popup.hwnd = window;

    popup::Item group;
    group.text = L"工具";
    group.children.push_back(popup::Item{});
    popup::g_popup.items.push_back(group);
    popup::g_popup.openIndex = 0;

    RECT client{};
    GetClientRect(window, &client);
    const RECT row = popup::itemRect(popup::g_popup.items, client, 0);
    const int midY = (row.top + row.bottom) / 2;
    check(popup::hitParentItem(popup::g_popup.items, window, POINT{dp(2), midY}) == 0,
          "left padding counts as the open row");
    check(popup::hitParentItem(popup::g_popup.items, window,
                               POINT{client.right - dp(2), midY}) == 0,
          "right padding counts as the open row");
    check(popup::hitParentItem(popup::g_popup.items, window,
                               POINT{dp(2), row.bottom + dp(2)}) == -1,
          "outside the open row is not hover");

    popup::g_popup = saved;
    DestroyWindow(window);
}

// 远端文件编辑：右键入口、本地副本命名、文本判定和回传状态机
void fileEditRegression() {
    const bool savedReady = g_app.fsReady;
    const std::vector<RemoteEntry> savedEntries = g_app.fsEntries;
    const int savedSel = g_app.fsSel;
    const std::wstring savedEditor = g_app.editorPath;
    const std::vector<App::EditedFile> savedEdits = g_app.editedFiles;
    const std::wstring savedStatus = g_app.statusText;
    const bool savedStatusError = g_app.statusError;
    const ULONGLONG savedStatusUntil = g_app.statusUntil;

    g_app.fsReady = true;
    RemoteEntry file;
    file.name = "main.c";
    file.path = "/srv/app/main.c";
    file.mode = LIBSSH2_SFTP_S_IFREG;
    file.size = 120;
    RemoteEntry folder;
    folder.name = "build";
    folder.path = "/srv/app/build";
    folder.mode = LIBSSH2_SFTP_S_IFDIR;
    g_app.fsEntries = {file, folder};
    g_app.fsSel = 0;
    g_app.editorPath.clear();

    auto findItem = [](const std::vector<popup::Item>& items, int id) -> const popup::Item* {
        for (const auto& item : items)
            if (item.id == id) return &item;
        return nullptr;
    };
    auto items = buildFileContextMenu();
    const popup::Item* edit = findItem(items, IDM_FS_EDIT);
    check(edit && edit->text == L"编辑" && edit->enabled, "a selected file can be edited");
    check(findItem(items, IDM_FS_EDITOR) != nullptr, "the editor picker is always reachable");
    check(findItem(items, IDM_FS_EDIT_SYSTEM) == nullptr,
          "the system-default entry only appears after an editor is configured");

    g_app.editorPath = L"C:\\Tools\\Notepad++.exe";
    items = buildFileContextMenu();
    check(findItem(items, IDM_FS_EDIT_SYSTEM) != nullptr,
          "a configured editor still leaves a system-default escape hatch");
    check(findItem(items, IDM_FS_EDITOR)->text == L"更换文本编辑器…",
          "the menu offers to change the configured editor");

    g_app.fsSel = 1;
    items = buildFileContextMenu();
    check(!findItem(items, IDM_FS_EDIT)->enabled, "directories cannot be edited as text");

    // 本地副本：不同目录的同名文件分开放，Windows 非法字符会被替换
    check(editLocalName("/srv/a/con.txt") == L"_con.txt", "reserved device names are escaped");
    check(editLocalName("/srv/a/we:ird?.txt") == L"we_ird_.txt",
          "Windows-illegal characters are replaced");
    check(editLocalPathFor("/srv/a/notes.txt") != editLocalPathFor("/srv/b/notes.txt"),
          "same-named files from different directories do not overwrite each other");
    check(editLocalPathFor("/srv/a/notes.txt") == editLocalPathFor("/srv/a/notes.txt"),
          "the local copy path is stable for the same remote file");

    // 文本判定：NUL 字节当二进制；普通文本可编辑
    const fs::path root = std::filesystem::temp_directory_path() /
        ("rshell-edit-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(GetTickCount64()));
    std::error_code ec;
    fs::create_directories(root, ec);
    const fs::path textFile = root / "text.txt";
    const fs::path binaryFile = root / "binary.bin";
    {
        std::ofstream out(textFile, std::ios::binary);
        out << "hello\nworld\n";
    }
    {
        std::ofstream out(binaryFile, std::ios::binary);
        const char data[] = {'a', 'b', '\0', 'c', 'd'};
        out.write(data, sizeof(data));
    }
    check(looksLikeTextFile(textFile), "plain text is accepted for editing");
    check(!looksLikeTextFile(binaryFile), "binary content is rejected before opening an editor");

    // 回传状态机：成功更新基线；失败保留 dirty 并安排重试
    std::uint64_t size = 0;
    FILETIME write{};
    check(readLocalFileState(textFile.wstring(), size, write), "read the local edit copy state");
    g_app.editedFiles.clear();
    App::EditedFile entry;
    entry.connectionId = g_app.connectionId;
    entry.remote = "/srv/app/main.c";
    entry.local = textFile.wstring();
    entry.name = L"main.c";
    entry.uploading = true;
    entry.uploadSize = size;
    entry.uploadWrite = write;
    entry.syncedSize = size;
    entry.syncedWrite = write;
    g_app.editedFiles.push_back(entry);
    check(finishEditUpload(entry.remote, true), "an active edit upload is completed");
    check(!g_app.editedFiles.back().uploading && !g_app.editedFiles.back().dirty,
          "a successful upload becomes the new baseline");
    g_app.editedFiles.back().uploading = true;
    check(finishEditUpload(entry.remote, false), "a failed upload is still owned by the edit session");
    check(g_app.editedFiles.back().dirty &&
              g_app.editedFiles.back().retryAt > GetTickCount64(),
          "a failed upload keeps the local change and schedules a retry");

    g_app.editedFiles.clear();
    fs::remove_all(root, ec);
    g_app.editedFiles = savedEdits;
    g_app.editorPath = savedEditor;
    g_app.fsEntries = savedEntries;
    g_app.fsSel = savedSel;
    g_app.fsReady = savedReady;
    g_app.statusText = savedStatus;
    g_app.statusError = savedStatusError;
    g_app.statusUntil = savedStatusUntil;
}


// 开关本身和它写到状态栏的提示：文案必须明确是“更新检查”，并且停留够久
void updateToggleRegression() {
    const bool before = g_app.updateCheckOnStart;
    SendMessageW(g_app.main, WM_COMMAND, MAKEWPARAM(IDM_TOGGLE_UPDATE_CHECK, 0), 0);
    check(g_app.updateCheckOnStart != before, "the startup check toggle flips the setting");
    check(g_app.statusText == (g_app.updateCheckOnStart ? L"启动更新检查：开"
                                                        : L"启动更新检查：关"),
          "the toggle reports a clear update-check message");
    check(statusNoticeActive(GetTickCount64()), "the toggle message is active");
    check(g_app.statusUntil > GetTickCount64() + 6000, "the toggle message stays long enough");
    {
        // 最小窗口宽度下，这条提示也必须放得下，不能被状态栏两侧的固定块挤没
        const RECT narrow{0, 0, dp(760), dp(460)};
        const auto layout = statusGeometry(narrow);
        HDC dc = GetDC(g_app.main);
        const int textW = textWidth(dc, g_app.statusText, g_app.uiFont);
        ReleaseDC(g_app.main, dc);
        check(layout.content.right - layout.content.left >= textW,
              "the toggle message fits the status bar at the minimum window width");
    }
    {
        // 光有状态文本还不够：真正画一遍状态栏，确认中间区域确实有文字像素
        RECT client{};
        GetClientRect(g_app.main, &client);
        const int h = g_app.statusH;
        check(h > 0, "the status bar has a height");
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, client.right, h);
        HGDIOBJ old = SelectObject(mem, bmp);
        paintStatusBar(mem, RECT{0, 0, client.right, h});
        const RECT content = statusGeometry(RECT{0, 0, client.right, h}).content;
        int bright = 0;
        for (int y = content.top; y < content.bottom; y += 2) {
            for (int x = content.left; x < content.right; x += 2) {
                const COLORREF c = GetPixel(mem, x, y);
                if (GetRValue(c) + GetGValue(c) + GetBValue(c) > 300) ++bright;
            }
        }
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        check(bright > 0, "the toggle message is painted in the status bar");
    }
    SendMessageW(g_app.main, WM_COMMAND, MAKEWPARAM(IDM_TOGGLE_UPDATE_CHECK, 0), 0);
    check(g_app.updateCheckOnStart == before, "toggling twice restores the setting");
    setStatus(L"");
}

// 绘制耗时基线：RSHELL_AI_PAINT_BENCH=1 时跑一次，只打印数字，不做断言。
// 用来对比「整窗重绘」和「只重绘最后一个气泡」的成本，不参与 CI 判定。
void aiPanelPaintBench() {
    const char* enabled = std::getenv("RSHELL_AI_PAINT_BENCH");
    if (!enabled) return;

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, 460, 760);
    HGDIOBJ old = SelectObject(dc, bmp);

    const std::wstring longBody =
        L"这一行是普通的说明文字，用来模拟真实回答里的长段落，包含中文与 English words 混排。\n\n"
        L"- 列表项一：**加粗** 与 `code`\n"
        L"- 列表项二：看看断行是否正确\n\n"
        L"```bash\nsystemctl status nginx --no-pager\nnginx -t\n```\n";
    const std::wstring shortBody =
        L"先看一下端口有没有在听：`ss -lntp | grep 8080`，如果没有输出说明上游没起来。\n";

    auto run = [&](const char* label, int items, const std::wstring& body, int repeats) {
        ai::ChatPanel panel;
        ai::PanelFonts fonts;
        fonts.ui = g_app.uiFont;
        fonts.bold = g_app.uiFontBold;
        fonts.mono = g_app.font;
        fonts.caption = g_app.tinyFont;
        panel.setFonts(fonts);
        panel.setBounds(RECT{0, 0, 460, 760});
        for (int i = 0; i < items; ++i) {
            panel.addUser(L"第 " + std::to_wstring(i + 1) + L" 个问题：帮我看看服务状态");
            const std::size_t index = panel.beginAssistant();
            for (int k = 0; k < repeats; ++k) panel.appendAssistant(index, body);
            panel.finishAssistant(index, true);
        }

        auto measure = [&](int rounds, bool partial) -> double {
            const ULONGLONG started = GetTickCount64();
            for (int i = 0; i < rounds; ++i) {
                if (partial) {
                    const RECT area = panel.lastItemRect();
                    panel.paint(dc, &area);
                } else {
                    panel.paint(dc);
                }
            }
            return static_cast<double>(GetTickCount64() - started) / rounds;
        };

        panel.paint(dc);  // 预热：把字体/布局缓存建好
        const double full = measure(20, false);
        const double part = measure(20, true);
        std::cout << "BENCH " << label << " items=" << items * 2 << " full=" << full
                  << "ms partial=" << part << "ms\n";
    };

    run("long ", 60, longBody, 4);
    run("short", 12, shortBody, 1);

    // 排版器本身：build 与 draw 分开计时（160KB 的极端文本）
    {
        md::Style style;
        style.normal = g_app.uiFont;
        style.bold = g_app.uiFontBold;
        style.italic = g_app.uiFont;
        style.mono = g_app.font;
        for (auto& font : style.heading) font = g_app.uiFontBold;
        style.bodyHeight = 20;

        std::wstring big;
        for (int i = 0; i < 40; ++i) big += longBody;
        md::LayoutCache cache;
        ULONGLONG t0 = GetTickCount64();
        const int h = cache.build(dc, 420, big, style);
        const ULONGLONG buildMs = GetTickCount64() - t0;
        t0 = GetTickCount64();
        for (int i = 0; i < 5; ++i) cache.draw(dc, 0, 0);
        const double drawMs = static_cast<double>(GetTickCount64() - t0) / 5;
        std::cout << "BENCH md build=" << buildMs << "ms draw=" << drawMs
                  << "ms height=" << h << " bytes=" << (big.size() / 1024) << "KB\n";
    }

    // 单条回答随长度增长时的单帧成本（流式时最关键的一条曲线）
    {
        ai::ChatPanel panel;
        ai::PanelFonts fonts;
        fonts.ui = g_app.uiFont;
        fonts.bold = g_app.uiFontBold;
        fonts.mono = g_app.font;
        fonts.caption = g_app.tinyFont;
        panel.setFonts(fonts);
        panel.setBounds(RECT{0, 0, 460, 760});
        panel.addUser(L"流式回答");
        const std::size_t index = panel.beginAssistant();
        std::size_t bodyBytes = 0;
        for (int step = 0; step < 5; ++step) {
            while (bodyBytes < (1u << (10 + step))) {
                panel.appendAssistant(index, longBody);
                bodyBytes += longBody.size();
            }
            const ULONGLONG started = GetTickCount64();
            for (int i = 0; i < 5; ++i) panel.paint(dc);
            const double ms = static_cast<double>(GetTickCount64() - started) / 5;
            std::cout << "BENCH single body=" << (bodyBytes / 1024) << "KB paint=" << ms << "ms\n";
        }
        panel.finishAssistant(index, true);
    }

    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}

// Markdown 排版缓存：增量喂入与一次性排版的最终结果必须一致
void markdownCacheRegression() {
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, 420, 600);
    HGDIOBJ old = SelectObject(dc, bmp);

    md::Style style;
    style.normal = g_app.uiFont;
    style.bold = g_app.uiFontBold;
    style.italic = g_app.uiFont;
    style.mono = g_app.font;
    for (auto& font : style.heading) font = g_app.uiFontBold;
    style.bodyHeight = 20;

    std::wstring text =
        L"先看一下服务状态，端口是否在听。\n\n"
        L"- 第一项：**加粗** 与 `code`\n"
        L"- 第二项：普通文字\n\n"
        L"```bash\nsystemctl status nginx\nss -lntp | grep 8080\n```\n\n"
        L"最后一段收尾，用来验证多次追加后的高度和一次排完一致。\n";

    md::LayoutCache whole;
    const int expected = whole.build(dc, 380, text, style);
    check(expected > 0, "markdown layout reports a height");

    // 同一份内容再 build 一次：不应重新折行
    check(whole.build(dc, 380, text, style) == expected, "rebuild with the same text keeps the height");
    check(whole.lastRebuiltBlocks() == 0, "unchanged text rebuilds no blocks");

    // 模拟流式：每次追加几个字符，块级缓存应当只重排变化的块
    md::LayoutCache incremental;
    int maxRebuilt = 0;
    for (std::size_t cut = 1; cut <= text.size(); cut += 7) {
        const std::wstring prefix = text.substr(0, cut);
        incremental.build(dc, 380, prefix, style);
        maxRebuilt = std::max(maxRebuilt, static_cast<int>(incremental.lastRebuiltBlocks()));
    }
    incremental.build(dc, 380, text, style);
    check(incremental.height() == expected, "incremental layout matches a single pass");
    check(maxRebuilt <= 4, "streaming rebuilds only the changed tail blocks");

    // 换宽度必须重排
    const int narrow = incremental.build(dc, 300, text, style);
    check(narrow >= expected, "narrower width never shrinks the text");
    check(incremental.lastRebuiltBlocks() > 0, "a width change rebuilds every block");
    check(incremental.height() == narrow, "height follows the width change");

    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}

// 流式平滑：假服务器每 350ms 推一批 25 字，界面必须分很多小步展示。
// 由 tests/ai_stream_e2e.py 设置环境变量后触发，平时跳过。
void aiStreamSmoothingRegression() {
    if (!std::getenv("RSHELL_AI_STREAM_TEST")) return;

    ProbeSessionBackend store;
    ai::AiTabContext context;
    context.uiFont = g_app.uiFont;
    context.boldFont = g_app.uiFontBold;
    context.monoFont = g_app.font;
    context.captionFont = g_app.tinyFont;
    context.hosts = [&store]() {
        ai::AiHostInfo host;
        host.id = 7;
        host.label = L"stream-probe";
        host.backend = &store;  // 假模型不调用工具，不会被当成 RemoteFs 用
        return std::vector<ai::AiHostInfo>{host};
    };
    context.activeHostId = []() { return static_cast<std::uintptr_t>(7); };
    context.apiKeyOverride = "test-key";

    ai::AiTab* tab = ai::AiTab::create(g_app.main, context);
    check(tab != nullptr, "stream panel is created");
    tab->setBounds(RECT{0, 0, 520, 760});
    tab->show(true);
    tab->sendMessage(L"验证平滑输出");

    std::size_t last = 0;
    int steps = 0;
    ULONGLONG lastChange = GetTickCount64();
    const ULONGLONG deadline = GetTickCount64() + 30000;
    while (GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(5);

        const ai::ChatItem* assistant = nullptr;
        for (const auto& item : tab->transcript())
            if (item.kind == ai::ItemKind::Assistant) assistant = &item;
        const std::size_t size = assistant ? assistant->body.size() : 0;
        const bool streaming = assistant ? assistant->streaming : true;
        if (size != last) {
            last = size;
            ++steps;
            lastChange = GetTickCount64();
        }
        if (!streaming && last > 0 && GetTickCount64() - lastChange > 600) break;
    }

    const char* expectedEnv = std::getenv("RSHELL_AI_EXPECT");
    const std::size_t expected =
        expectedEnv ? static_cast<std::size_t>(std::strtoull(expectedEnv, nullptr, 10)) : 0;
    std::cout << "STREAM steps=" << steps << " chars=" << last << " expected=" << expected << "\n";
    for (const auto& item : tab->transcript()) {
        std::string body(item.body.begin(), item.body.end());
        if (body.size() > 120) body.resize(120);
        std::cout << "  item kind=" << static_cast<int>(item.kind) << " body=" << body << "\n";
    }
    check(expected > 0 && last == expected, "every streamed character reaches the panel");
    check(steps >= 20, "text is revealed in many small steps, not one jump per batch");
    ai::AiTab::destroy(tab);
}

// 局部重绘：只画最后一个气泡的区域时，其余条目要跳过但不能出错
void aiPanelPartialPaintRegression() {
    ai::ChatPanel panel;
    ai::PanelFonts fonts;
    fonts.ui = g_app.uiFont;
    fonts.bold = g_app.uiFontBold;
    fonts.mono = g_app.font;
    fonts.caption = g_app.tinyFont;
    panel.setFonts(fonts);
    panel.setBounds(RECT{0, 0, 420, 600});
    panel.addUser(L"先问一句");
    for (int i = 0; i < 40; ++i) panel.addNotice(L"提示 " + std::to_wstring(i));
    panel.addUser(L"最后一条");

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, 420, 600);
    HGDIOBJ old = SelectObject(dc, bmp);

    panel.paint(dc);
    const int fullHeight = panel.contentHeight();
    check(panel.contentOverflows(), "many messages overflow the view");

    const RECT view = panel.viewRect();
    check(view.top >= 60 && view.bottom <= 600 - 60, "view rect sits between header and composer");
    const RECT last = panel.lastItemRect();
    check(!IsRectEmpty(&last), "last item rect is available for partial repaint");
    check(last.top >= view.top && last.bottom <= view.bottom, "last item rect stays inside the view");

    // 只画最后一条的区域：不能崩、不能改变测量结果
    panel.paint(dc, &last);
    check(panel.contentHeight() == fullHeight, "partial repaint keeps the measured height");

    // 再全量画一次，确保局部重绘后回到完整状态
    panel.paint(dc);
    check(panel.contentHeight() == fullHeight, "full repaint keeps the measured height");

    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}

// 检查更新：发现新版本时，标题栏出现可点击的提示胶囊
void updateCheckRegression() {
    ReleaseInfo newer;
    newer.ok = true;
    newer.tag = "v99.9.9";
    newer.url = "https://example.invalid/rshell";
    newer.newer = true;
    g_app.updateCheckManual = false;
    handleUpdateResult(newer);
    check(g_app.updateTag == "v99.9.9", "a newer release is remembered");
    check(g_app.updateUrl == "https://example.invalid/rshell", "its page url is remembered");
    {
        // 提示胶囊是在 paintTitleBar 里算出来的，探针窗口不显示，得自己画一次
        RECT client{};
        GetClientRect(g_app.main, &client);
        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        paintTitleBar(dc, RECT{0, 0, client.right, g_app.titleH});
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
    }
    check(!IsRectEmpty(&g_app.updateChip), "the title bar shows an update chip");
    check(g_app.updateChip.left > 0 && g_app.updateChip.right < g_app.titleAi.left,
          "the update chip sits left of the AI button");

    // 同版本不提示
    g_app.updateTag.clear();
    g_app.updateUrl.clear();
    ReleaseInfo same;
    same.ok = true;
    same.tag = std::string("v") + appVersion();
    handleUpdateResult(same);
    check(g_app.updateTag.empty(), "the same version does not raise a chip");

    // 请求失败只在日志里记一笔
    ReleaseInfo failed;
    failed.error = "network down";
    handleUpdateResult(failed);
    check(g_app.updateTag.empty(), "a failed check keeps the UI quiet");
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
        aiSessionHistoryRegression();
        aiUnlockCancelRegression();
        statusHintRegression();
        autoReconnectRegression();
        fsLoadingRegression();
        splitterRegression();
        fsCollapseRegression();
        pasteFocusRegression();
        aiPanelPaintBench();
        aiStreamSmoothingRegression();
        markdownCacheRegression();
        aiPanelPartialPaintRegression();
        mainMenuRegression();
        submenuHoverRegression();
        updateToggleRegression();
        fileEditRegression();
        updateCheckRegression();
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
