// dnd_virtual_file.h - 拖出下载用的「虚拟文件」数据对象
//
// 把远端文件拖到资源管理器/桌面时，我们不在本地落临时文件：
//   1. 先只提供文件名和大小（FileGroupDescriptorW）
//   2. 松手后资源管理器在目的目录建好文件，再按块向 FileContents 的 IStream 要数据
// 于是下载发生在「松开之后」，本地也不会多出一份 %TEMP% 副本。
//
// 线程约定（很重要）：
//   1. 数据对象和流都实现了自由线程封送（FTM），因为它们会被资源管理器的复制
//      线程直接调用；没有 FTM，调用会被封送回创建对象的那条线程（GUI 里就是
//      UI 线程）的消息循环。
//   2. 数据对象还实现了 IDataObjectAsyncCapability（旧名 IAsyncOperation）：
//      放置目标据此可以「另起后台线程取数，然后立刻从 Drop 返回」。缺了这个
//      接口，资源管理器会在 Drop 里同步取完数据才返回，发起方的 UI 线程整段
//      卡在 DoDragDrop 的模态循环里——表现出来就是「下载时窗口卡死」。
//   3. Read 从预取缓冲区拷贝，最多只等「下一块到达」，不会按住调用方整段下载。
//
// 这一层只做 OLE 协议，真正的字节从哪来由 ReaderFactory 决定（GUI 里是 RemoteFs），
// 所以可以脱离 GUI 单测。
#pragma once

#ifdef _WIN32

#include <windows.h>

// IDataObject/IStream 与 STGMEDIUM 在 WIN32_LEAN_AND_MEAN 下不会由 windows.h 带进来；
// FILEDESCRIPTORW / FD_* 在 shlobj.h 里。
#include <objidl.h>
#include <ole2.h>
#include <shlobj.h>
// IDataObjectAsyncCapability 只在 shldisp.h 里（shlobj.h 不带它）
#include <shldisp.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rshell::dnd {

// 一次拖动里的一个文件
struct VirtualFile {
    std::string key;                            // 远端路径，原样交给 ReaderFactory
    std::wstring name;                          // 目标文件名
    std::uint64_t size = 0;                     // 拖动时列表里的大小（打开后会校正）
    std::shared_ptr<std::atomic<bool>> cancel;  // 可空；置 true 中断这次读取
};

// 顺序读取器：读一块，返回字节数；0 表示结束；error 非空表示失败
struct Reader {
    std::function<std::uint64_t()> size;                                  // 可空
    std::function<std::size_t(char*, std::size_t, std::string*)> read;    // 必需
    std::function<void()> close;                                          // 可空
};

// 打开失败时返回 read 为空的 Reader，并在 error 里写原因
using ReaderFactory = std::function<Reader(const std::string& key, std::string* error)>;

enum class EventKind {
    Started,    // 真正开始拉数据（此刻文件已经在目的目录）
    Progress,   // done/total 是已传/总量
    Done,       // 读完
    Cancelled,  // 被 cancel 打断
    Failed,     // 读失败，message 是原因
};

struct Event {
    EventKind kind = EventKind::Started;
    std::size_t index = 0;
    std::string key;
    std::wstring name;
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    std::string message;
    // 这一项的取消开关（和 VirtualFile::cancel 是同一个对象）：
    // 资源管理器是异步复制的，Started 可能晚于拖放调用返回，
    // 所以开关只能随事件走，UI 侧没法按路径回头去找。
    std::shared_ptr<std::atomic<bool>> cancel;
};

// 可能在资源管理器的复制线程上被调用，实现方自己切到 UI 线程（PostMessage）
using EventSink = std::function<void(const Event&)>;

class VirtualFileData : public IDataObject, public IDataObjectAsyncCapability {
public:
    VirtualFileData(std::vector<VirtualFile> files, ReaderFactory factory, EventSink sink);
    ~VirtualFileData();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDataObject
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override;
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC* format, STGMEDIUM* medium) override;
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override;
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC* in, FORMATETC* out) override;
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override;
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override;
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC* format, DWORD flags, IAdviseSink* sink,
                                      DWORD* connection) override;
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD connection) override;
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA** out) override;

    // IDataObjectAsyncCapability（旧名 IAsyncOperation）：放置目标据此把取数
    // 挪到后台线程，Drop 立刻返回，发起方的 UI 线程就能马上回到消息循环。
    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL doOpAsync) override;
    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* isOpAsync) override;
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx* bindContext) override;
    HRESULT STDMETHODCALLTYPE InOperation(BOOL* inAsyncOp) override;
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT result, IBindCtx* bindContext,
                                           DWORD effects) override;

private:
    HRESULT descriptorMedium(STGMEDIUM* medium);

    LONG ref_ = 1;
    IUnknown* marshaler_ = nullptr;  // 自由线程封送器（构造时创建）
    // 默认就允许异步取数：即使目标没有先调 SetAsyncMode，GetAsyncMode 也报 true
    std::atomic<bool> asyncMode_{true};
    // StartOperation 之后、EndOperation 之前为 true（DoDragDrop 返回后据此判断
    // 数据是不是还在后台传，此时不能当作已经完成）
    std::atomic<bool> inOperation_{false};
    std::vector<VirtualFile> files_;
    ReaderFactory factory_;
    EventSink sink_;
};

// 剪贴板格式（"FileGroupDescriptorW" / "FileContents"），首次调用时注册
UINT fileDescriptorFormat();
UINT fileContentsFormat();

// 仅供测试：当前存活的流对象个数。流析构时会关掉远端句柄，
// 数量不归零说明 FTM 的自引用把对象（连同句柄）留住了。
int liveStreamCount();

}  // namespace rshell::dnd

#endif  // _WIN32
