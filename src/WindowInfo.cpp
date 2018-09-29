/* Copyright 2018 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "BaseUtil.h"
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#include "ScopedWin.h"
#include "FileUtil.h"
#include "FrameRateWnd.h"
#include "WinUtil.h"
#include "TreeCtrl.h"

#include "BaseEngine.h"
#include "EngineManager.h"
#include "Doc.h"
#include "SettingsStructs.h"
#include "Controller.h"
#include "ChmModel.h"
#include "DisplayModel.h"
#include "EbookController.h"
#include "GlobalPrefs.h"
#include "TextSelection.h"
#include "TextSearch.h"
#include "SumatraPDF.h"
#include "WindowInfo.h"
#include "TabInfo.h"
#include "resource.h"
#include "Caption.h"
#include "Notifications.h"
#include "Selection.h"
#include "StressTesting.h"
#include "Translations.h"
#include "uia/Provider.h"

WindowInfo::WindowInfo(HWND hwnd) {
    hwndFrame = hwnd;
    touchState.panStarted = false;
    linkHandler = new LinkHandler(this);
    notifications = new Notifications();
    fwdSearchMark.show = false;
}

WindowInfo::~WindowInfo() {
    FinishStressTest(this);

    CrashIf(tabs.size() > 0);
    CrashIf(ctrl || linkOnLastButtonDown);

    // release our copy of UIA provider
    // the UI automation still might have a copy somewhere
    if (uia_provider) {
        if (AsFixed()) {
            uia_provider->OnDocumentUnload();
        }
        uia_provider->Release();
    }

    delete linkHandler;
    delete buffer;
    delete notifications;
    delete tabSelectionHistory;
    delete caption;
    DeleteVecMembers(tabs);
    // cbHandler is passed into Controller and must be deleted afterwards
    // (all controllers should have been deleted prior to WindowInfo, though)
    delete cbHandler;

    DeleteFrameRateWnd(frameRateWnd);
    DeleteTreeCtrl(tocTreeCtrl);
    free(sidebarSplitter);
    free(favSplitter);
    free(tocLabelWithClose);
    free(favLabelWithClose);
}

bool WindowInfo::IsAboutWindow() const {
    return nullptr == currentTab;
}

bool WindowInfo::IsDocLoaded() const {
    CrashIf(!this->ctrl != !(currentTab && currentTab->ctrl));
    return this->ctrl != nullptr;
}

DisplayModel* WindowInfo::AsFixed() const {
    return ctrl ? ctrl->AsFixed() : nullptr;
}
ChmModel* WindowInfo::AsChm() const {
    return ctrl ? ctrl->AsChm() : nullptr;
}
EbookController* WindowInfo::AsEbook() const {
    return ctrl ? ctrl->AsEbook() : nullptr;
}

// Notify both display model and double-buffer (if they exist)
// about a potential change of available canvas size
void WindowInfo::UpdateCanvasSize() {
    RectI rc = ClientRect(hwndCanvas);
    if (buffer && canvasRc == rc)
        return;
    canvasRc = rc;

    // create a new output buffer and notify the model
    // about the change of the canvas size
    delete buffer;
    buffer = new DoubleBuffer(hwndCanvas, canvasRc);

    if (IsDocLoaded()) {
        // the display model needs to know the full size (including scroll bars)
        ctrl->SetViewPortSize(GetViewPortSize());
    }
    if (currentTab) {
        currentTab->canvasRc = canvasRc;
    }

    // keep the notifications visible (only needed for right-to-left layouts)
    if (IsUIRightToLeft())
        notifications->Relayout();
}

SizeI WindowInfo::GetViewPortSize() {
    SizeI size = canvasRc.Size();

    DWORD style = GetWindowLong(hwndCanvas, GWL_STYLE);
    if ((style & WS_VSCROLL)) {
        size.dx += GetSystemMetrics(SM_CXVSCROLL);
    }
    if ((style & WS_HSCROLL)) {
        size.dy += GetSystemMetrics(SM_CYHSCROLL);
    }
    CrashIf((style & (WS_VSCROLL | WS_HSCROLL)) && !AsFixed());

    return size;
}

void WindowInfo::RedrawAll(bool update) {
    InvalidateRect(this->hwndCanvas, nullptr, false);
    if (this->AsEbook()) {
        this->AsEbook()->RequestRepaint();
    }
    if (update) {
        UpdateWindow(this->hwndCanvas);
    }
}

void WindowInfo::ChangePresentationMode(PresentationMode mode) {
    presentation = mode;
    if (PM_BLACK_SCREEN == mode || PM_WHITE_SCREEN == mode) {
        DeleteInfotip();
    }
    RedrawAll();
}

void WindowInfo::Focus() {
    win::ToForeground(hwndFrame);
    // set focus to an owned modal dialog if there is one
    HWND hwnd = nullptr;
    while ((hwnd = FindWindowEx(HWND_DESKTOP, hwnd, nullptr, nullptr)) != nullptr) {
        if (GetWindow(hwnd, GW_OWNER) == hwndFrame && (GetWindowStyle(hwnd) & WS_DLGFRAME)) {
            SetFocus(hwnd);
            return;
        }
    }
    SetFocus(hwndFrame);
}

void WindowInfo::ToggleZoom() {
    CrashIf(!this->ctrl);
    if (!this->IsDocLoaded())
        return;

    if (ZOOM_FIT_PAGE == this->ctrl->GetZoomVirtual())
        this->ctrl->SetZoomVirtual(ZOOM_FIT_WIDTH, nullptr);
    else if (ZOOM_FIT_WIDTH == this->ctrl->GetZoomVirtual())
        this->ctrl->SetZoomVirtual(ZOOM_FIT_CONTENT, nullptr);
    else
        this->ctrl->SetZoomVirtual(ZOOM_FIT_PAGE, nullptr);
}

void WindowInfo::MoveDocBy(int dx, int dy) {
    CrashIf(!this->AsFixed());
    if (!this->AsFixed())
        return;
    CrashIf(this->linkOnLastButtonDown);
    if (this->linkOnLastButtonDown)
        return;
    DisplayModel* dm = this->ctrl->AsFixed();
    if (0 != dx)
        dm->ScrollXBy(dx);
    if (0 != dy)
        dm->ScrollYBy(dy, false);
}

#define MULTILINE_INFOTIP_WIDTH_PX 500

void WindowInfo::CreateInfotip(const WCHAR* text, RectI& rc, bool multiline) {
    if (str::IsEmpty(text)) {
        this->DeleteInfotip();
        return;
    }

    TOOLINFO ti = {0};
    ti.cbSize = sizeof(ti);
    ti.hwnd = this->hwndCanvas;
    ti.uFlags = TTF_SUBCLASS;
    ti.lpszText = (WCHAR*)text;
    ti.rect = rc.ToRECT();

    if (multiline || str::FindChar(text, '\n'))
        SendMessage(this->hwndInfotip, TTM_SETMAXTIPWIDTH, 0, MULTILINE_INFOTIP_WIDTH_PX);
    else
        SendMessage(this->hwndInfotip, TTM_SETMAXTIPWIDTH, 0, -1);

    SendMessage(this->hwndInfotip, this->infotipVisible ? TTM_NEWTOOLRECT : TTM_ADDTOOL, 0, (LPARAM)&ti);
    this->infotipVisible = true;
}

void WindowInfo::DeleteInfotip() {
    if (!infotipVisible)
        return;

    TOOLINFO ti = {0};
    ti.cbSize = sizeof(ti);
    ti.hwnd = hwndCanvas;

    SendMessage(hwndInfotip, TTM_DELTOOL, 0, (LPARAM)&ti);
    infotipVisible = false;
}

void WindowInfo::ShowNotification(const WCHAR* message, int options, NotificationGroup groupId) {
    int timeoutMS = (options & NOS_PERSIST) ? 0 : 3000;
    bool highlight = (options & NOS_HIGHLIGHT);

    NotificationWnd* wnd = new NotificationWnd(hwndCanvas, message, timeoutMS, highlight, [this](NotificationWnd* wnd) {
        this->notifications->RemoveNotification(wnd);
    });
    if (NG_CURSOR_POS_HELPER == groupId) {
        wnd->shrinkLimit = 0.7f;
    }
    notifications->Add(wnd, groupId);
}

bool WindowInfo::CreateUIAProvider() {
    if (!uia_provider) {
        uia_provider = new SumatraUIAutomationProvider(this->hwndCanvas);
        if (!uia_provider)
            return false;
        // load data to provider
        if (AsFixed())
            uia_provider->OnDocumentLoad(AsFixed());
    }

    return true;
}

class RemoteDestination : public PageDestination {
    PageDestType type;
    int pageNo;
    RectD rect;
    AutoFreeW value;
    AutoFreeW name;

  public:
    RemoteDestination(PageDestination* dest)
        : type(dest->GetDestType()),
          pageNo(dest->GetDestPageNo()),
          rect(dest->GetDestRect()),
          value(dest->GetDestValue()),
          name(dest->GetDestName()) {}
    virtual ~RemoteDestination() {}

    PageDestType GetDestType() const override { return type; }
    int GetDestPageNo() const override { return pageNo; }
    RectD GetDestRect() const override { return rect; }
    WCHAR* GetDestValue() const override { return str::Dup(value); }
    WCHAR* GetDestName() const override { return str::Dup(name); }
};

void LinkHandler::GotoLink(PageDestination* link) {
    CrashIf(!owner || owner->linkHandler != this);
    if (!link || !owner->IsDocLoaded())
        return;

    TabInfo* tab = owner->currentTab;
    AutoFreeW path(link->GetDestValue());
    PageDestType type = link->GetDestType();
    if (PageDestType::ScrollTo == type) {
        // TODO: respect link->ld.gotor.new_window for PDF documents ?
        ScrollTo(link);
    } else if (PageDestType::LaunchURL == type) {
        if (!path)
            /* ignore missing URLs */;
        else {
            WCHAR* colon = str::FindChar(path, ':');
            WCHAR* hash = str::FindChar(path, '#');
            if (!colon || (hash && colon > hash)) {
                // treat relative URIs as file paths (without fragment identifier)
                if (hash)
                    *hash = '\0';
                str::TransChars(path.Get(), L"/", L"\\");
                url::DecodeInPlace(path.Get());
                // LaunchFile will reject unsupported file types
                LaunchFile(path, nullptr);
            } else {
                // LaunchBrowser will reject unsupported URI schemes
                // TODO: support file URIs?
                LaunchBrowser(path);
            }
        }
    } else if (PageDestType::LaunchEmbedded == type) {
        // open embedded PDF documents in a new window
        if (path && str::StartsWith(path.Get(), tab->filePath.Get())) {
            WindowInfo* newWin = FindWindowInfoByFile(path, true);
            if (!newWin) {
                LoadArgs args(path, owner);
                newWin = LoadDocument(args);
            }
            if (newWin)
                newWin->Focus();
        }
        // offer to save other attachments to a file
        else {
            LinkSaver linkSaverTmp(tab, owner->hwndFrame, path);
            link->SaveEmbedded(linkSaverTmp);
        }
    } else if (PageDestType::LaunchFile == type) {
        if (path) {
            // LaunchFile only opens files inside SumatraPDF
            // (except for allowed perceived file types)
            LaunchFile(path, link);
        }
    }
    // predefined named actions
    else if (PageDestType::NextPage == type)
        tab->ctrl->GoToNextPage();
    else if (PageDestType::PrevPage == type)
        tab->ctrl->GoToPrevPage();
    else if (PageDestType::FirstPage == type)
        tab->ctrl->GoToFirstPage();
    else if (PageDestType::LastPage == type)
        tab->ctrl->GoToLastPage();
    // Adobe Reader extensions to the spec, cf. http://www.tug.org/applications/hyperref/manual.html
    else if (PageDestType::FindDialog == type)
        PostMessage(owner->hwndFrame, WM_COMMAND, IDM_FIND_FIRST, 0);
    else if (PageDestType::FullScreen == type)
        PostMessage(owner->hwndFrame, WM_COMMAND, IDM_VIEW_PRESENTATION_MODE, 0);
    else if (PageDestType::GoBack == type)
        tab->ctrl->Navigate(-1);
    else if (PageDestType::GoForward == type)
        tab->ctrl->Navigate(1);
    else if (PageDestType::GoToPageDialog == type)
        PostMessage(owner->hwndFrame, WM_COMMAND, IDM_GOTO_PAGE, 0);
    else if (PageDestType::PrintDialog == type)
        PostMessage(owner->hwndFrame, WM_COMMAND, IDM_PRINT, 0);
    else if (PageDestType::SaveAsDialog == type)
        PostMessage(owner->hwndFrame, WM_COMMAND, IDM_SAVEAS, 0);
    else if (PageDestType::ZoomToDialog == type)
        PostMessage(owner->hwndFrame, WM_COMMAND, IDM_ZOOM_CUSTOM, 0);
    else
        CrashIf(PageDestType::None != type);
}

void LinkHandler::ScrollTo(PageDestination* dest) {
    CrashIf(!owner || owner->linkHandler != this);
    if (!dest || !owner->IsDocLoaded())
        return;

    int pageNo = dest->GetDestPageNo();
    if (pageNo > 0)
        owner->ctrl->ScrollToLink(dest);
}

void LinkHandler::LaunchFile(const WCHAR* path, PageDestination* link) {
    // for safety, only handle relative paths and only open them in SumatraPDF
    // (unless they're of an allowed perceived type) and never launch any external
    // file in plugin mode (where documents are supposed to be self-contained)
    WCHAR drive;
    if (str::StartsWith(path, L"\\") || str::Parse(path, L"%c:\\", &drive) || gPluginMode) {
        return;
    }

    // TODO: link is deleted when opening the document in a new tab
    RemoteDestination* remoteLink = nullptr;
    if (link) {
        remoteLink = new RemoteDestination(link);
        link = nullptr;
    }

    AutoFreeW fullPath(path::GetDir(owner->ctrl->FilePath()));
    fullPath.Set(path::Join(fullPath, path));
    fullPath.Set(path::Normalize(fullPath));
    // TODO: respect link->ld.gotor.new_window for PDF documents ?
    WindowInfo* newWin = FindWindowInfoByFile(fullPath, true);
    // TODO: don't show window until it's certain that there was no error
    if (!newWin) {
        LoadArgs args(fullPath, owner);
        newWin = LoadDocument(args);
        if (!newWin) {
            delete remoteLink;
            return;
        }
    }

    if (!newWin->IsDocLoaded()) {
        CloseTab(newWin);
        // OpenFileExternally rejects files we'd otherwise
        // have to show a notification to be sure (which we
        // consider bad UI and thus simply don't)
        bool ok = OpenFileExternally(fullPath);
        if (!ok) {
            AutoFreeW msg(str::Format(_TR("Error loading %s"), fullPath));
            owner->ShowNotification(msg, NOS_HIGHLIGHT);
        }
        delete remoteLink;
        return;
    }

    newWin->Focus();
    if (!remoteLink)
        return;

    AutoFreeW destName(remoteLink->GetDestName());
    if (destName) {
        PageDestination* dest = newWin->ctrl->GetNamedDest(destName);
        if (dest) {
            newWin->linkHandler->ScrollTo(dest);
            delete dest;
        }
    } else {
        newWin->linkHandler->ScrollTo(remoteLink);
    }
    delete remoteLink;
}

// normalizes case and whitespace in the string
// caller needs to free() the result
static WCHAR* NormalizeFuzzy(const WCHAR* str) {
    WCHAR* dup = str::Dup(str);
    CharLower(dup);
    str::NormalizeWS(dup);
    // cf. AddTocItemToView
    return dup;
}

static bool MatchFuzzy(const WCHAR* s1, const WCHAR* s2, bool partially = false) {
    if (!partially)
        return str::Eq(s1, s2);

    // only match at the start of a word (at the beginning and after a space)
    for (const WCHAR* last = s1; (last = str::Find(last, s2)) != nullptr; last++) {
        if (last == s1 || *(last - 1) == ' ')
            return true;
    }
    return false;
}

// finds the first ToC entry that (partially) matches a given normalized name
// (ignoring case and whitespace differences)
PageDestination* LinkHandler::FindTocItem(DocTocItem* item, const WCHAR* name, bool partially) {
    for (; item; item = item->next) {
        AutoFreeW fuzTitle(NormalizeFuzzy(item->title));
        if (MatchFuzzy(fuzTitle, name, partially))
            return item->GetLink();
        PageDestination* dest = FindTocItem(item->child, name, partially);
        if (dest)
            return dest;
    }
    return nullptr;
}

void LinkHandler::GotoNamedDest(const WCHAR* name) {
    CrashIf(!owner || owner->linkHandler != this);
    Controller* ctrl = owner->ctrl;
    if (!ctrl)
        return;

    // Match order:
    // 1. Exact match on internal destination name
    // 2. Fuzzy match on full ToC item title
    // 3. Fuzzy match on a part of a ToC item title
    // 4. Exact match on page label
    PageDestination* dest = ctrl->GetNamedDest(name);
    bool hasDest = dest != NULL;
    if (dest) {
        ScrollTo(dest);
        delete dest;
    } else if (ctrl->HasTocTree()) {
        DocTocItem* root = ctrl->GetTocTree();
        AutoFreeW fuzName(NormalizeFuzzy(name));
        dest = FindTocItem(root, fuzName);
        if (!dest)
            dest = FindTocItem(root, fuzName, true);
        if (dest) {
            ScrollTo(dest);
            hasDest = true;
        }
        delete root;
    }
    if (!hasDest && ctrl->HasPageLabels()) {
        int pageNo = ctrl->GetPageByLabel(name);
        if (ctrl->ValidPageNo(pageNo))
            ctrl->GoToPage(pageNo, true);
    }
}
