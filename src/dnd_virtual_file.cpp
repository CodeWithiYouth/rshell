// dnd_virtual_file.cpp - 虚拟文件拖放（FileGroupDescriptorW + FileContents）
#include "dnd_virtual_file.h"

#ifdef _WIN32

#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace rshell::dnd {

UINT fileDescriptorFormat() {
    // "FileGroupDescriptorW"：告诉目标是「一组虚拟文件」，附带名字和大小
    static const UINT format = RegisterClipboardFormatW(L"FileGroupDescriptorW");
    return format;
}

UINT fileContentsFormat() {
    // "FileContents"：目标按 lindex 逐个要文件内容（TYMED_ISTREAM）
    static const UINT format = RegisterClipboardFormatW(L"FileContents");
    return format;
}

namespace {

// 一次向底层要多少字节、最多在内存里攒几块（预取窗口）
constexpr std::size_t kChunkSize = 256 * 1024;
constexpr std::size_t kMaxBuffered = 4;
// 消费者等数据的上限：120 * 250ms = 30s 一块都没等到就认定这条流坏了
constexpr int kStallTicks = 120;

// 存活计数：流的析构要关掉远端句柄，泄漏就等于泄漏 SFTP 句柄
std::atomic<int>& liveStreams() {
    static std::atomic<int> count{0};
    return count;
}

// 「现下现给」的流。
//
// 线程模型是这里最要紧的地方：跨进程拖放时 OLE 会把 IStream 代理到复制引擎
// 那边，回程的 Read 默认只肯在创建对象的线程（我们的 UI 线程）的消息循环里
// 执行。若 Read 里同步等 SFTP，整段下载期间 UI 线程都在 Read 里出不来，
// 窗口就会「卡死到下载结束」。所以：
//   1. 用自由线程封送（FTM）声明对象线程安全，复制线程可以直接调进来；
//   2. 真正的字节由一条预取线程从底层读，Read 只从缓冲区拷贝，最多等到
//      「下一块到达」，不会把调用方按住整个下载时长。
class RemoteStream : public IStream {
public:
    RemoteStream(VirtualFile file, std::size_t index, ReaderFactory factory, EventSink sink)
        : file_(std::move(file)), index_(index), factory_(std::move(factory)),
          sink_(std::move(sink)) {
        // FTM 不持有外层引用（实测创建前后引用数不变），析构里放掉它即可，
        // 不会形成环。
        if (FAILED(CoCreateFreeThreadedMarshaler(static_cast<IUnknown*>(this), &marshaler_))) {
            marshaler_ = nullptr;
        }
        ++liveStreams();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_INVALIDARG;
        *out = nullptr;
        if (id == IID_IUnknown || id == IID_IStream || id == IID_ISequentialStream) {
            *out = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        // CoMarshalInterface / 复制引擎会问 IMarshal，交给自己的 FTM
        if (id == IID_IMarshal && marshaler_) return marshaler_->QueryInterface(id, out);
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++ref_); }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG left = --ref_;
        if (left <= 0) {
            // 0：最后一个外部引用；负数只可能来自 FTM 收尾时补的一次，忽略即可
            if (left == 0) delete this;
            return 0;
        }
        return static_cast<ULONG>(left);
    }

    HRESULT STDMETHODCALLTYPE Read(void* buffer, ULONG count, ULONG* read) override {
        if (read) *read = 0;
        if (!buffer) return count == 0 ? S_OK : STG_E_INVALIDPOINTER;
        if (count == 0) return S_OK;
        // 用户在下载列表里点了取消：连预取线程都不必起
        if (cancelled()) {
            finishCancelled();
            return E_ABORT;
        }
        if (!startProducer()) return STG_E_READFAULT;

        std::unique_lock<std::mutex> lock(mutex_);
        int stalls = 0;
        while (buffers_.empty() && !producerDone_ && !aborted_) {
            const bool ready = cv_.wait_for(lock, std::chrono::milliseconds(250), [this] {
                return !buffers_.empty() || producerDone_ || aborted_ || cancelled();
            });
            if (!ready && ++stalls >= kStallTicks) {
                lock.unlock();
                finishFailed("远端一直没有数据（读取超时）");
                return STG_E_READFAULT;
            }
            if (cancelled()) {
                lock.unlock();
                finishCancelled();
                return E_ABORT;
            }
        }

        if (buffers_.empty()) {
            // 预取线程收尾了：要么出错，要么就是读完了
            lock.unlock();
            return aborted_ ? STG_E_READFAULT : S_OK;
        }

        std::string& front = buffers_.front();
        const std::size_t take =
            std::min<std::size_t>(static_cast<std::size_t>(count), front.size() - frontOffset_);
        memcpy(buffer, front.data() + frontOffset_, take);
        frontOffset_ += take;
        if (frontOffset_ >= front.size()) {
            buffers_.pop_front();
            frontOffset_ = 0;
        }
        delivered_ += take;
        lock.unlock();
        cv_.notify_all();  // 缓冲腾出空位，叫醒预取线程
        if (read) *read = static_cast<ULONG>(take);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override {
        return STG_E_ACCESSDENIED;
    }

    // 复制引擎偶尔会先把流倒回开头；整段重来代价太大，只放行「原地不动」的情况。
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin,
                                   ULARGE_INTEGER* newPosition) override {
        std::lock_guard<std::mutex> lock(mutex_);
        LONGLONG target = move.QuadPart;
        if (origin == STREAM_SEEK_CUR) {
            target += static_cast<LONGLONG>(delivered_);
        } else if (origin == STREAM_SEEK_END) {
            target += static_cast<LONGLONG>(file_.size);
        } else if (origin != STREAM_SEEK_SET) {
            return STG_E_INVALIDFUNCTION;
        }
        if (target < 0) return STG_E_INVALIDFUNCTION;
        if (target == static_cast<LONGLONG>(delivered_)) {
            if (newPosition) newPosition->QuadPart = static_cast<ULONGLONG>(target);
            return S_OK;
        }
        if (target == 0 && delivered_ == 0) {
            if (newPosition) newPosition->QuadPart = 0;
            return S_OK;
        }
        return STG_E_INVALIDFUNCTION;
    }

    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*,
                                     ULARGE_INTEGER*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override {
        if (!stat) return STG_E_INVALIDPOINTER;
        *stat = STATSTG{};
        stat->type = STGTY_STREAM;
        stat->grfMode = STGM_READ;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stat->cbSize.QuadPart = file_.size;
        }
        if (flags & STATFLAG_NONAME) return S_OK;
        const std::size_t bytes = (file_.name.size() + 1) * sizeof(wchar_t);
        auto* name = static_cast<wchar_t*>(CoTaskMemAlloc(bytes));
        if (!name) return E_OUTOFMEMORY;
        memcpy(name, file_.name.c_str(), bytes);
        stat->pwcsName = name;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    ~RemoteStream() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            aborted_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (marshaler_) {
            IUnknown* marshaler = marshaler_;
            marshaler_ = nullptr;
            marshaler->Release();
        }
        --liveStreams();
    }


    bool cancelled() const { return file_.cancel && file_.cancel->load(); }

    // 第一次要数据时才起预取线程：此刻文件已经在目的目录里了
    bool startProducer() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) return true;
        started_ = true;
        worker_ = std::thread([this]() { producerLoop(); });
        return true;
    }

    // 预取线程：把底层读到的字节攒进缓冲区，顺便把 Started/Progress/Done 抛出去
    void producerLoop() {
        std::string error;
        Reader reader = factory_ ? factory_(file_.key, &error) : Reader();
        if (!reader.read) {
            finishFailed(error.empty() ? std::string("无法打开远端文件") : error);
            return;
        }
        if (reader.size) {
            const std::uint64_t real = reader.size();
            if (real > 0) {
                std::lock_guard<std::mutex> lock(mutex_);
                file_.size = real;  // 打开后以真实大小为准
            }
        }
        emit(EventKind::Started, 0, std::string());

        while (true) {
            if (cancelled()) {
                finishCancelled();
                return;
            }
            {
                // 缓冲区满了就等消费者拿走，避免把整个文件堆在内存里
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] {
                    return buffers_.size() < kMaxBuffered || aborted_ || cancelled();
                });
                if (aborted_) return;  // 取消/失败已经由触发方报告过
            }

            std::string chunk(kChunkSize, '\0');
            std::string readError;
            const std::size_t got = reader.read(chunk.data(), chunk.size(), &readError);
            if (!readError.empty()) {
                finishFailed(readError);
                return;
            }
            if (got == 0) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    producerDone_ = true;
                }
                cv_.notify_all();
                emit(EventKind::Done, produced_, std::string());
                return;
            }
            chunk.resize(got);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                produced_ += got;
                buffers_.push_back(std::move(chunk));
            }
            cv_.notify_all();
            reportProgress();
        }
    }

    void reportProgress() {
        const DWORD now = GetTickCount();
        if (now - lastReport_ < 100) return;
        lastReport_ = now;
        emit(EventKind::Progress, produced_, std::string());
    }

    // 取消：谁先发现谁报告，保证只抛一次
    void finishCancelled() {
        std::uint64_t done = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (finished_) return;
            finished_ = true;
            aborted_ = true;
            producerDone_ = true;
            done = produced_;
        }
        cv_.notify_all();
        emit(EventKind::Cancelled, done, std::string());
    }

    void finishFailed(const std::string& error) {
        std::uint64_t done = 0;
        std::string text = error.empty() ? std::string("下载失败") : error;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (finished_) return;
            finished_ = true;
            aborted_ = true;
            producerDone_ = true;
            done = produced_;
        }
        cv_.notify_all();
        emit(EventKind::Failed, done, "下载失败：" + text);
    }

    void emit(EventKind kind, std::uint64_t done, const std::string& message) {
        if (!sink_) return;
        Event event;
        event.kind = kind;
        event.index = index_;
        event.key = file_.key;
        event.name = file_.name;
        event.done = done;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            event.total = file_.size;
        }
        event.message = message;
        event.cancel = file_.cancel;
        sink_(event);
    }

    VirtualFile file_;
    std::size_t index_ = 0;
    ReaderFactory factory_;
    EventSink sink_;
    IUnknown* marshaler_ = nullptr;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> buffers_;  // 已下载、还没交给调用方的数据块
    std::size_t frontOffset_ = 0;      // 队首块里已经拷走的字节
    std::uint64_t produced_ = 0;       // 从远端读到的总字节
    std::uint64_t delivered_ = 0;      // 已经交给调用方的总字节
    std::thread worker_;
    bool started_ = false;
    bool producerDone_ = false;
    bool aborted_ = false;   // 出错或取消，预取线程该收手了
    bool finished_ = false;  // 结果事件只报一次
    DWORD lastReport_ = 0;

    LONG ref_ = 1;
};

}  // namespace

int liveStreamCount() { return liveStreams().load(); }

VirtualFileData::VirtualFileData(std::vector<VirtualFile> files, ReaderFactory factory,
                                 EventSink sink)
    : files_(std::move(files)), factory_(std::move(factory)), sink_(std::move(sink)) {
    // 与流一样打上自由线程标记：拖动结束后资源管理器会在自己的线程上
    // GetData/EnumFormatEtc，不该再借 UI 线程的腿。
    // 注意：类现在有两个 IUnknown 基类，必须指明走 IDataObject 那一个。
    if (FAILED(CoCreateFreeThreadedMarshaler(static_cast<IDataObject*>(this), &marshaler_))) {
        marshaler_ = nullptr;
    }
}

VirtualFileData::~VirtualFileData() {
    if (marshaler_) {
        IUnknown* marshaler = marshaler_;
        marshaler_ = nullptr;
        marshaler->Release();
    }
}

HRESULT STDMETHODCALLTYPE VirtualFileData::QueryInterface(REFIID id, void** out) {
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    if (id == IID_IUnknown || id == IID_IDataObject) {
        *out = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }
    // 放置目标（资源管理器 / 桌面）先问这个接口，再决定要不要后台取数
    if (id == IID_IDataObjectAsyncCapability) {
        *out = static_cast<IDataObjectAsyncCapability*>(this);
        AddRef();
        return S_OK;
    }
    if (id == IID_IMarshal && marshaler_) return marshaler_->QueryInterface(id, out);
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE VirtualFileData::AddRef() { return static_cast<ULONG>(++ref_); }

ULONG STDMETHODCALLTYPE VirtualFileData::Release() {
    const LONG left = --ref_;
    if (left <= 0) {
        if (left == 0) delete this;
        return 0;
    }
    return static_cast<ULONG>(left);
}

HRESULT STDMETHODCALLTYPE VirtualFileData::GetData(FORMATETC* format, STGMEDIUM* medium) {
    if (!format || !medium) return E_INVALIDARG;
    if (format->cfFormat == static_cast<CLIPFORMAT>(fileDescriptorFormat()) &&
        (format->tymed & TYMED_HGLOBAL)) {
        return descriptorMedium(medium);
    }
    if (format->cfFormat == static_cast<CLIPFORMAT>(fileContentsFormat()) &&
        (format->tymed & TYMED_ISTREAM)) {
        if (format->lindex < 0 || static_cast<std::size_t>(format->lindex) >= files_.size()) {
            return DV_E_LINDEX;
        }
        const std::size_t index = static_cast<std::size_t>(format->lindex);
        auto* stream = new RemoteStream(files_[index], index, factory_, sink_);
        medium->tymed = TYMED_ISTREAM;
        medium->pstm = stream;  // 引用计数 1，直接交给调用方
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }
    return DV_E_FORMATETC;
}

HRESULT STDMETHODCALLTYPE VirtualFileData::GetDataHere(FORMATETC*, STGMEDIUM*) {
    return E_NOTIMPL;
}



HRESULT STDMETHODCALLTYPE VirtualFileData::QueryGetData(FORMATETC* format) {
    if (!format) return E_INVALIDARG;
    if (format->cfFormat == static_cast<CLIPFORMAT>(fileDescriptorFormat()) &&
        (format->tymed & TYMED_HGLOBAL)) {
        return S_OK;
    }
    if (format->cfFormat == static_cast<CLIPFORMAT>(fileContentsFormat()) &&
        (format->tymed & TYMED_ISTREAM)) {
        if (format->lindex < 0 || static_cast<std::size_t>(format->lindex) >= files_.size()) {
            return DV_E_LINDEX;
        }
        return S_OK;
    }
    return DV_E_FORMATETC;
}

// 按接口约定返回「等价格式 + ptd=nullptr」：返回 E_NOTIMPL 会让部分目标
// （资源管理器、桌面）判定数据对象不完整。
HRESULT STDMETHODCALLTYPE VirtualFileData::GetCanonicalFormatEtc(FORMATETC* in,
                                                                 FORMATETC* out) {
    if (!out) return E_INVALIDARG;
    if (in) *out = *in;
    out->ptd = nullptr;
    return DATA_S_SAMEFORMATETC;
}

HRESULT STDMETHODCALLTYPE VirtualFileData::SetData(FORMATETC*, STGMEDIUM*, BOOL) {
    return E_NOTIMPL;
}

// 必须能枚举出两种格式：放置目标会先摸清数据对象提供什么，再决定收不收这次拖放。
HRESULT STDMETHODCALLTYPE VirtualFileData::EnumFormatEtc(DWORD direction,
                                                         IEnumFORMATETC** out) {
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    if (direction != DATADIR_GET) return E_NOTIMPL;
    FORMATETC formats[2]{};
    formats[0].cfFormat = static_cast<CLIPFORMAT>(fileDescriptorFormat());
    formats[0].dwAspect = DVASPECT_CONTENT;
    formats[0].lindex = -1;
    formats[0].tymed = TYMED_HGLOBAL;
    formats[1].cfFormat = static_cast<CLIPFORMAT>(fileContentsFormat());
    formats[1].dwAspect = DVASPECT_CONTENT;
    formats[1].lindex = -1;
    formats[1].tymed = TYMED_ISTREAM;
    return SHCreateStdEnumFmtEtc(2, formats, out);
}

HRESULT STDMETHODCALLTYPE VirtualFileData::DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) {
    return OLE_E_ADVISENOTSUPPORTED;
}

HRESULT STDMETHODCALLTYPE VirtualFileData::DUnadvise(DWORD) {
    return OLE_E_ADVISENOTSUPPORTED;
}

HRESULT STDMETHODCALLTYPE VirtualFileData::EnumDAdvise(IEnumSTATDATA**) {
    return OLE_E_ADVISENOTSUPPORTED;
}

// ---- IDataObjectAsyncCapability -------------------------------------------
//
// 协议（见 MSDN "Handling Shell Data Transfer Scenarios"）：
//   源：DoDragDrop 之前 SetAsyncMode(TRUE)；DoDragDrop 返回后 InOperation()，
//       返回 TRUE 说明目标还在后台取数，此时不能当作已经传完。
//   目标：Drop 里 QI 本接口 → GetAsyncMode() 为 TRUE 就起一条线程 →
//       StartOperation() → 立刻从 Drop 返回（DoDragDrop 随之返回，源端 UI 解锁）
//       → 后台线程慢慢要数据 → EndOperation()。
// 这正是「下载必须异步、不能占住 UI 线程」在 OLE 协议里的正式做法。

HRESULT STDMETHODCALLTYPE VirtualFileData::SetAsyncMode(BOOL doOpAsync) {
    asyncMode_ = doOpAsync != FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE VirtualFileData::GetAsyncMode(BOOL* isOpAsync) {
    if (!isOpAsync) return E_INVALIDARG;
    *isOpAsync = asyncMode_.load() ? TRUE : FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE VirtualFileData::StartOperation(IBindCtx*) {
    inOperation_ = true;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE VirtualFileData::InOperation(BOOL* inAsyncOp) {
    if (!inAsyncOp) return E_INVALIDARG;
    *inAsyncOp = inOperation_.load() ? TRUE : FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE VirtualFileData::EndOperation(HRESULT, IBindCtx*, DWORD) {
    // 只负责记账：真正的「传完了」以流上的 Done/Failed 事件为准
    inOperation_ = false;
    return S_OK;
}



// FILEGROUPDESCRIPTORW：cItems + 每个文件的 FILEDESCRIPTORW
HRESULT VirtualFileData::descriptorMedium(STGMEDIUM* medium) {
    const std::size_t bytes = sizeof(UINT) + files_.size() * sizeof(FILEDESCRIPTORW);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!memory) return E_OUTOFMEMORY;
    if (void* target = GlobalLock(memory)) {
        auto* group = static_cast<FILEGROUPDESCRIPTORW*>(target);
        group->cItems = static_cast<UINT>(files_.size());
        for (std::size_t i = 0; i < files_.size(); ++i) {
            FILEDESCRIPTORW& fd = group->fgd[i];
            // 不带 FD_PROGRESSUI：进度由 rshell 自己的下载列表负责，
            // 免得资源管理器再弹一个自己的进度框
            fd.dwFlags =
                static_cast<DWORD>(FD_FILESIZE | FD_ATTRIBUTES | FD_WRITESTIME | FD_UNICODE);
            fd.dwFileAttributes = static_cast<DWORD>(FILE_ATTRIBUTE_NORMAL);
            fd.nFileSizeHigh = static_cast<DWORD>(files_[i].size >> 32);
            fd.nFileSizeLow = static_cast<DWORD>(files_[i].size & 0xFFFFFFFFull);
            const std::wstring name = files_[i].name.substr(0, 259);
            memcpy(fd.cFileName, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
        }
        GlobalUnlock(memory);
    }
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = memory;
    medium->pUnkForRelease = nullptr;
    return S_OK;
}

}  // namespace rshell::dnd

#endif  // _WIN32
