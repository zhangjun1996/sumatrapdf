class ScopedCritSec {
    CRITICAL_SECTION* cs;

  public:
    explicit ScopedCritSec(CRITICAL_SECTION* cs) : cs(cs) { EnterCriticalSection(cs); }
    ~ScopedCritSec() { LeaveCriticalSection(cs); }
};

class ScopedHandle {
    HANDLE handle;

  public:
    explicit ScopedHandle(HANDLE handle) : handle(handle) {}
    ~ScopedHandle() {
		if (IsValid())
			CloseHandle(handle); 
	}
    operator HANDLE() const { return handle; }
    bool IsValid() const { return handle != NULL && handle != INVALID_HANDLE_VALUE; }
};

template <class T>
class ScopedComPtr {
  protected:
    T* ptr;

  public:
    ScopedComPtr() : ptr(nullptr) {}
    explicit ScopedComPtr(T* ptr) : ptr(ptr) {}
    ~ScopedComPtr() {
        if (ptr)
            ptr->Release();
    }
    bool Create(const CLSID clsid) {
        CrashIf(ptr);
        if (ptr)
            return false;
        HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&ptr));
        return SUCCEEDED(hr);
    }
    T* Get() const { return ptr; }
    operator T*() const { return ptr; }
    T** operator&() { return &ptr; }
    T* operator->() const { return ptr; }
    T* operator=(T* newPtr) {
        if (ptr)
            ptr->Release();
        return (ptr = newPtr);
    }
};

template <class T>
class ScopedComQIPtr {
  protected:
    T* ptr;

  public:
    ScopedComQIPtr() : ptr(nullptr) {}
    explicit ScopedComQIPtr(IUnknown* unk) {
        HRESULT hr = unk->QueryInterface(&ptr);
        if (FAILED(hr))
            ptr = nullptr;
    }
    ~ScopedComQIPtr() {
        if (ptr)
            ptr->Release();
    }
    bool Create(const CLSID clsid) {
        CrashIf(ptr);
        if (ptr)
            return false;
        HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&ptr));
        return SUCCEEDED(hr);
    }
    T* operator=(IUnknown* newUnk) {
        if (ptr)
            ptr->Release();
        HRESULT hr = newUnk->QueryInterface(&ptr);
        if (FAILED(hr))
            ptr = nullptr;
        return ptr;
    }
    operator T*() const { return ptr; }
    T** operator&() { return &ptr; }
    T* operator->() const { return ptr; }
    T* operator=(T* newPtr) {
        if (ptr)
            ptr->Release();
        return (ptr = newPtr);
    }
};

template <typename T>
class ScopedGdiObj {
    T obj;

  public:
    explicit ScopedGdiObj(T obj) : obj(obj) {}
    ~ScopedGdiObj() { DeleteObject(obj); }
    operator T() const { return obj; }
};
typedef ScopedGdiObj<HFONT> ScopedFont;
typedef ScopedGdiObj<HPEN> ScopedPen;
typedef ScopedGdiObj<HBRUSH> ScopedBrush;

class ScopedHDC {
    HDC hdc;

  public:
    explicit ScopedHDC(HDC hdc) : hdc(hdc) {}
    ~ScopedHDC() { DeleteDC(hdc); }
    operator HDC() const { return hdc; }
};

class ScopedHdcSelect {
    HDC hdc;
    HGDIOBJ prev;

  public:
    ScopedHdcSelect(HDC hdc, HGDIOBJ obj) : hdc(hdc) { prev = SelectObject(hdc, obj); }
    ~ScopedHdcSelect() { SelectObject(hdc, prev); }
};

class ScopedCom {
  public:
    ScopedCom() { (void)CoInitialize(nullptr); }
    ~ScopedCom() { CoUninitialize(); }
};

class ScopedOle {
  public:
    ScopedOle() { (void)OleInitialize(nullptr); }
    ~ScopedOle() { OleUninitialize(); }
};

class ScopedGdiPlus {
  protected:
    Gdiplus::GdiplusStartupInput si;
    Gdiplus::GdiplusStartupOutput so;
    ULONG_PTR token, hookToken;
    bool noBgThread;

  public:
    // suppress the GDI+ background thread when initiating in WinMain,
    // as that thread causes DDE messages to be sent too early and
    // thus causes unexpected timeouts
    explicit ScopedGdiPlus(bool inWinMain = false) : noBgThread(inWinMain) {
        si.SuppressBackgroundThread = noBgThread;
        Gdiplus::GdiplusStartup(&token, &si, &so);
        if (noBgThread)
            so.NotificationHook(&hookToken);
    }
    ~ScopedGdiPlus() {
        if (noBgThread)
            so.NotificationUnhook(hookToken);
        Gdiplus::GdiplusShutdown(token);
    }
};
