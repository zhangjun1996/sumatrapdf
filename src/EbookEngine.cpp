/* Copyright 2018 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// engines which render flowed ebook formats into fixed pages through the BaseEngine API
// (pages are mostly layed out the same as for a "B Format" paperback: 5.12" x 7.8")

#include "BaseUtil.h"
#include "ScopedWin.h"
#include "Archive.h"
#include "Dpi.h"
#include "FileUtil.h"
#include "GdiPlusUtil.h"
#include "HtmlParserLookup.h"
#include "HtmlPullParser.h"
#include "Mui.h"
#include "PalmDbReader.h"
#include "TrivialHtmlParser.h"
#include "WinUtil.h"
#include "ZipUtil.h"

#include "BaseEngine.h"
#include "EbookEngine.h"
#include "EbookBase.h"
#include "EbookDoc.h"
#include "HtmlFormatter.h"
#include "EbookFormatter.h"

static AutoFreeW gDefaultFontName;
static float gDefaultFontSize = 10.f;

static const WCHAR* GetDefaultFontName() {
    return gDefaultFontName ? gDefaultFontName : L"Georgia";
}

static float GetDefaultFontSize() {
    // fonts are scaled at higher DPI settings,
    // undo this here for (mostly) consistent results
    return gDefaultFontSize * 96.0f / DpiGetPreciseY(HWND_DESKTOP);
}

void SetDefaultEbookFont(const WCHAR* name, float size) {
    // intentionally don't validate the input
    gDefaultFontName.SetCopy(name);
    // use a somewhat smaller size than in the EbookUI, since fit page/width
    // is likely to be above 100% for the paperback page dimensions
    gDefaultFontSize = size * 0.8f;
}

/* common classes for EPUB, FictionBook2, Mobi, PalmDOC, CHM, HTML and TXT engines */

struct PageAnchor {
    DrawInstr* instr;
    int pageNo;

    explicit PageAnchor(DrawInstr* instr = nullptr, int pageNo = -1) : instr(instr), pageNo(pageNo) {}
};

class EbookAbortCookie : public AbortCookie {
  public:
    bool abort;
    EbookAbortCookie() : abort(false) {}
    void Abort() override { abort = true; }
};

class EbookEngine : public BaseEngine {
  public:
    EbookEngine();
    virtual ~EbookEngine();

    int PageCount() const override { return pages ? (int)pages->size() : 0; }

    RectD PageMediabox(int pageNo) override {
        UNUSED(pageNo);
        return pageRect;
    }
    RectD PageContentBox(int pageNo, RenderTarget target = RenderTarget::View) override {
        UNUSED(target);
        RectD mbox = PageMediabox(pageNo);
        mbox.Inflate(-pageBorder, -pageBorder);
        return mbox;
    }

    RenderedBitmap* RenderBitmap(int pageNo, float zoom, int rotation,
                                 RectD* pageRect = nullptr, /* if nullptr: defaults to the page's mediabox */
                                 RenderTarget target = RenderTarget::View, AbortCookie** cookie_out = nullptr) override;

    PointD Transform(PointD pt, int pageNo, float zoom, int rotation, bool inverse = false) override;
    RectD Transform(RectD rect, int pageNo, float zoom, int rotation, bool inverse = false) override;

    u8* GetFileData(size_t* cbCount) override {
        if (!fileName) {
            return nullptr;
        }
        OwnedData data(file::ReadFile(fileName));
        if (cbCount) {
            *cbCount = data.size;
        }
        return (u8*)data.StealData();
    }
    bool SaveFileAs(const char* copyFileName, bool includeUserAnnots = false) override {
        UNUSED(includeUserAnnots);
        if (!fileName) {
            return false;
        }
        AutoFreeW path(str::conv::FromUtf8(copyFileName));
        return fileName ? CopyFileW(fileName, path, FALSE) : false;
    }
    WCHAR* ExtractPageText(int pageNo, const WCHAR* lineSep, RectI** coordsOut = nullptr,
                           RenderTarget target = RenderTarget::View) override;
    // make RenderCache request larger tiles than per default
    bool HasClipOptimizations(int pageNo) override {
        UNUSED(pageNo);
        return false;
    }
    PageLayoutType PreferredLayout() override { return Layout_Book; }

    bool SupportsAnnotation(bool forSaving = false) const override { return !forSaving; }
    void UpdateUserAnnotations(Vec<PageAnnotation>* list) override;

    Vec<PageElement*>* GetElements(int pageNo) override;
    PageElement* GetElementAtPos(int pageNo, PointD pt) override;

    PageDestination* GetNamedDest(const WCHAR* name) override;

    bool BenchLoadPage(int pageNo) override {
        UNUSED(pageNo);
        return true;
    }

  protected:
    Vec<HtmlPage*>* pages = nullptr;
    Vec<PageAnchor> anchors;
    // contains for each page the last anchor indicating
    // a break between two merged documents
    Vec<DrawInstr*> baseAnchors;
    // needed so that memory allocated by ResolveHtmlEntities isn't leaked
    PoolAllocator allocator;
    // TODO: still needed?
    CRITICAL_SECTION pagesAccess;
    // access to userAnnots is protected by pagesAccess
    Vec<PageAnnotation> userAnnots;
    // page dimensions can vary between filetypes
    RectD pageRect;
    float pageBorder;

    void GetTransform(Matrix& m, float zoom, int rotation) {
        GetBaseTransform(m, pageRect.ToGdipRectF(), zoom, rotation);
    }
    bool ExtractPageAnchors();
    WCHAR* ExtractFontList();

    virtual PageElement* CreatePageLink(DrawInstr* link, RectI rect, int pageNo);

    Vec<DrawInstr>* GetHtmlPage(int pageNo) {
        CrashIf(pageNo < 1 || PageCount() < pageNo);
        if (pageNo < 1 || PageCount() < pageNo)
            return nullptr;
        return &pages->at(pageNo - 1)->instructions;
    }
};

class SimpleDest2 : public PageDestination {
  protected:
    int pageNo;
    RectD rect;
    AutoFreeW value;

  public:
    SimpleDest2(int pageNo, RectD rect, WCHAR* value = nullptr) : pageNo(pageNo), rect(rect), value(value) {}

    PageDestType GetDestType() const override { return value ? PageDestType::LaunchURL : PageDestType::ScrollTo; }
    int GetDestPageNo() const override { return pageNo; }
    RectD GetDestRect() const override { return rect; }
    WCHAR* GetDestValue() const override { return str::Dup(value); }
};

class EbookLink : public PageElement, public PageDestination {
    PageDestination* dest; // required for internal links, nullptr for external ones
    DrawInstr* link;       // owned by *EngineImpl::pages
    RectI rect;
    int pageNo;
    bool showUrl;

  public:
    EbookLink() : dest(nullptr), link(nullptr), pageNo(-1), showUrl(false) {}
    EbookLink(DrawInstr* link, RectI rect, PageDestination* dest, int pageNo = -1, bool showUrl = false)
        : link(link), rect(rect), dest(dest), pageNo(pageNo), showUrl(showUrl) {}
    virtual ~EbookLink() { delete dest; }

    PageElementType GetType() const override { return PageElementType::Link; }
    int GetPageNo() const override { return pageNo; }
    RectD GetRect() const override { return rect.Convert<double>(); }
    WCHAR* GetValue() const override {
        if (!dest || showUrl)
            return str::conv::FromHtmlUtf8(link->str.s, link->str.len);
        return nullptr;
    }
    virtual PageDestination* AsLink() { return dest ? dest : this; }

    PageDestType GetDestType() const override { return PageDestType::LaunchURL; }
    int GetDestPageNo() const override { return 0; }
    RectD GetDestRect() const override { return RectD(); }
    WCHAR* GetDestValue() const override { return GetValue(); }
};

class ImageDataElement : public PageElement {
    int pageNo;
    ImageData* id; // owned by *EngineImpl::pages
    RectI bbox;

  public:
    ImageDataElement(int pageNo, ImageData* id, RectI bbox) : pageNo(pageNo), id(id), bbox(bbox) {}

    virtual PageElementType GetType() const { return PageElementType::Image; }
    virtual int GetPageNo() const { return pageNo; }
    virtual RectD GetRect() const { return bbox.Convert<double>(); }
    virtual WCHAR* GetValue() const { return nullptr; }

    virtual RenderedBitmap* GetImage() {
        HBITMAP hbmp;
        Bitmap* bmp = BitmapFromData(id->data, id->len);
        if (!bmp || bmp->GetHBITMAP((ARGB)Color::White, &hbmp) != Ok) {
            delete bmp;
            return nullptr;
        }
        SizeI size(bmp->GetWidth(), bmp->GetHeight());
        delete bmp;
        return new RenderedBitmap(hbmp, size);
    }
};

class EbookTocItem : public DocTocItem {
    PageDestination* dest;

  public:
    EbookTocItem(WCHAR* title, PageDestination* dest)
        : DocTocItem(title, dest ? dest->GetDestPageNo() : 0), dest(dest) {}
    ~EbookTocItem() { delete dest; }

    virtual PageDestination* GetLink() { return dest; }
};

EbookEngine::EbookEngine() {
    // "B Format" paperback
    pageRect = RectD(0, 0, 5.12 * GetFileDPI(), 7.8 * GetFileDPI());
    pageBorder = 0.4f * GetFileDPI();
    InitializeCriticalSection(&pagesAccess);
}

EbookEngine::~EbookEngine() {
    EnterCriticalSection(&pagesAccess);

    if (pages)
        DeleteVecMembers(*pages);
    delete pages;

    LeaveCriticalSection(&pagesAccess);
    DeleteCriticalSection(&pagesAccess);
}

bool EbookEngine::ExtractPageAnchors() {
    ScopedCritSec scope(&pagesAccess);

    DrawInstr* baseAnchor = nullptr;
    for (int pageNo = 1; pageNo <= PageCount(); pageNo++) {
        Vec<DrawInstr>* pageInstrs = GetHtmlPage(pageNo);
        if (!pageInstrs)
            return false;

        for (size_t k = 0; k < pageInstrs->size(); k++) {
            DrawInstr* i = &pageInstrs->at(k);
            if (InstrAnchor != i->type)
                continue;
            anchors.Append(PageAnchor(i, pageNo));
            if (k < 2 && str::StartsWith(i->str.s + i->str.len, "\" page_marker />"))
                baseAnchor = i;
        }
        baseAnchors.Append(baseAnchor);
    }

    CrashIf(baseAnchors.size() != pages->size());
    return true;
}

PointD EbookEngine::Transform(PointD pt, int pageNo, float zoom, int rotation, bool inverse) {
    RectD rect = Transform(RectD(pt, SizeD()), pageNo, zoom, rotation, inverse);
    return PointD(rect.x, rect.y);
}

RectD EbookEngine::Transform(RectD rect, int pageNo, float zoom, int rotation, bool inverse) {
    UNUSED(pageNo);
    geomutil::RectT<REAL> rcF = rect.Convert<REAL>();
    PointF pts[2] = {PointF(rcF.x, rcF.y), PointF(rcF.x + rcF.dx, rcF.y + rcF.dy)};
    Matrix m;
    GetTransform(m, zoom, rotation);
    if (inverse)
        m.Invert();
    m.TransformPoints(pts, 2);
    return RectD::FromXY(pts[0].X, pts[0].Y, pts[1].X, pts[1].Y);
}

// TODO: use AdjustLightness instead to compensate for the alpha?
static Gdiplus::Color Unblend(PageAnnotation::Color c, BYTE alpha) {
    alpha = (BYTE)(alpha * c.a / 255.f);
    BYTE R = (BYTE)floorf(std::max(c.r - (255 - alpha), 0) * 255.0f / alpha + 0.5f);
    BYTE G = (BYTE)floorf(std::max(c.g - (255 - alpha), 0) * 255.0f / alpha + 0.5f);
    BYTE B = (BYTE)floorf(std::max(c.b - (255 - alpha), 0) * 255.0f / alpha + 0.5f);
    return Gdiplus::Color(alpha, R, G, B);
}

static inline Gdiplus::Color FromColor(PageAnnotation::Color c) {
    return Gdiplus::Color(c.a, c.r, c.g, c.b);
}

static void DrawAnnotations(Graphics& g, Vec<PageAnnotation>& userAnnots, int pageNo) {
    for (size_t i = 0; i < userAnnots.size(); i++) {
        PageAnnotation& annot = userAnnots.at(i);
        if (annot.pageNo != pageNo)
            continue;
        PointF p1, p2;
        switch (annot.type) {
            case PageAnnotType::Highlight: {
                SolidBrush tmpBrush(Unblend(annot.color, 119));
                g.FillRectangle(&tmpBrush, annot.rect.ToGdipRectF());
            } break;
            case PageAnnotType::Underline:
                p1 = PointF((float)annot.rect.x, (float)annot.rect.BR().y);
                p2 = PointF((float)annot.rect.BR().x, p1.Y);
                {
                    Pen tmpPen(FromColor(annot.color));
                    g.DrawLine(&tmpPen, p1, p2);
                }
                break;
            case PageAnnotType::StrikeOut:
                p1 = PointF((float)annot.rect.x, (float)annot.rect.y + (float)annot.rect.dy / 2);
                p2 = PointF((float)annot.rect.BR().x, p1.Y);
                {
                    Pen tmpPen(FromColor(annot.color));
                    g.DrawLine(&tmpPen, p1, p2);
                }
                break;
            case PageAnnotType::Squiggly: {
                Pen p(FromColor(annot.color), 0.5f);
                REAL dash[2] = {2, 2};
                p.SetDashPattern(dash, dimof(dash));
                p.SetDashOffset(1);
                p1 = PointF((float)annot.rect.x, (float)annot.rect.BR().y - 0.25f);
                p2 = PointF((float)annot.rect.BR().x, p1.Y);
                g.DrawLine(&p, p1, p2);
                p.SetDashOffset(3);
                p1.Y += 0.5f;
                p2.Y += 0.5f;
                g.DrawLine(&p, p1, p2);
            } break;
        }
    }
}

RenderedBitmap* EbookEngine::RenderBitmap(int pageNo, float zoom, int rotation, RectD* pageRect, RenderTarget target,
                                          AbortCookie** cookieOut) {
    UNUSED(target);
    RectD pageRc = pageRect ? *pageRect : PageMediabox(pageNo);
    RectI screen = Transform(pageRc, pageNo, zoom, rotation).Round();
    PointI screenTL = screen.TL();
    screen.Offset(-screen.x, -screen.y);

    HANDLE hMap = nullptr;
    HBITMAP hbmp = CreateMemoryBitmap(screen.Size(), &hMap);
    HDC hDC = CreateCompatibleDC(nullptr);
    DeleteObject(SelectObject(hDC, hbmp));

    Graphics g(hDC);
    mui::InitGraphicsMode(&g);

    Color white(0xFF, 0xFF, 0xFF);
    SolidBrush tmpBrush(white);
    Rect screenR(screen.ToGdipRect());
    screenR.Inflate(1, 1);
    g.FillRectangle(&tmpBrush, screenR);

    Matrix m;
    GetTransform(m, zoom, rotation);
    m.Translate((REAL)-screenTL.x, (REAL)-screenTL.y, MatrixOrderAppend);
    g.SetTransform(&m);

    EbookAbortCookie* cookie = nullptr;
    if (cookieOut)
        *cookieOut = cookie = new EbookAbortCookie();

    ScopedCritSec scope(&pagesAccess);

    mui::ITextRender* textDraw = mui::TextRenderGdiplus::Create(&g);
    DrawHtmlPage(&g, textDraw, GetHtmlPage(pageNo), pageBorder, pageBorder, false, Color((ARGB)Color::Black),
                 cookie ? &cookie->abort : nullptr);
    DrawAnnotations(g, userAnnots, pageNo);
    delete textDraw;
    DeleteDC(hDC);

    if (cookie && cookie->abort) {
        DeleteObject(hbmp);
        CloseHandle(hMap);
        return nullptr;
    }

    return new RenderedBitmap(hbmp, screen.Size(), hMap);
}

static RectI GetInstrBbox(DrawInstr& instr, float pageBorder) {
    geomutil::RectT<float> bbox(instr.bbox.X, instr.bbox.Y, instr.bbox.Width, instr.bbox.Height);
    bbox.Offset(pageBorder, pageBorder);
    return bbox.Round();
}

WCHAR* EbookEngine::ExtractPageText(int pageNo, const WCHAR* lineSep, RectI** coordsOut, RenderTarget target) {
    UNUSED(target);
    ScopedCritSec scope(&pagesAccess);

    str::Str<WCHAR> content;
    Vec<RectI> coords;
    bool insertSpace = false;

    Vec<DrawInstr>* pageInstrs = GetHtmlPage(pageNo);
    for (DrawInstr& i : *pageInstrs) {
        RectI bbox = GetInstrBbox(i, pageBorder);
        switch (i.type) {
            case InstrString:
                if (coords.size() > 0 &&
                    (bbox.x < coords.Last().BR().x || bbox.y > coords.Last().y + coords.Last().dy * 0.8)) {
                    content.Append(lineSep);
                    coords.AppendBlanks(str::Len(lineSep));
                    CrashIf(*lineSep && !coords.Last().IsEmpty());
                } else if (insertSpace && coords.size() > 0) {
                    int swidth = bbox.x - coords.Last().BR().x;
                    if (swidth > 0) {
                        content.Append(' ');
                        coords.Append(RectI(bbox.x - swidth, bbox.y, swidth, bbox.dy));
                    }
                }
                insertSpace = false;
                {
                    AutoFreeW s(str::conv::FromHtmlUtf8(i.str.s, i.str.len));
                    content.Append(s);
                    size_t len = str::Len(s);
                    double cwidth = 1.0 * bbox.dx / len;
                    for (size_t k = 0; k < len; k++)
                        coords.Append(RectI((int)(bbox.x + k * cwidth), bbox.y, (int)cwidth, bbox.dy));
                }
                break;
            case InstrRtlString:
                if (coords.size() > 0 &&
                    (bbox.BR().x > coords.Last().x || bbox.y > coords.Last().y + coords.Last().dy * 0.8)) {
                    content.Append(lineSep);
                    coords.AppendBlanks(str::Len(lineSep));
                    CrashIf(*lineSep && !coords.Last().IsEmpty());
                } else if (insertSpace && coords.size() > 0) {
                    int swidth = coords.Last().x - bbox.BR().x;
                    if (swidth > 0) {
                        content.Append(' ');
                        coords.Append(RectI(bbox.BR().x, bbox.y, swidth, bbox.dy));
                    }
                }
                insertSpace = false;
                {
                    AutoFreeW s(str::conv::FromHtmlUtf8(i.str.s, i.str.len));
                    content.Append(s);
                    size_t len = str::Len(s);
                    double cwidth = 1.0 * bbox.dx / len;
                    for (size_t k = 0; k < len; k++)
                        coords.Append(RectI((int)(bbox.x + (len - k - 1) * cwidth), bbox.y, (int)cwidth, bbox.dy));
                }
                break;
            case InstrElasticSpace:
            case InstrFixedSpace:
                insertSpace = true;
                break;
        }
    }
    if (content.size() > 0 && !str::EndsWith(content.Get(), lineSep)) {
        content.Append(lineSep);
        coords.AppendBlanks(str::Len(lineSep));
    }

    if (coordsOut) {
        CrashIf(coords.size() != content.size());
        *coordsOut = coords.StealData();
    }
    return content.StealData();
}

void EbookEngine::UpdateUserAnnotations(Vec<PageAnnotation>* list) {
    ScopedCritSec scope(&pagesAccess);
    if (list) {
        userAnnots = *list;
    } else {
        userAnnots.Reset();
    }
}

PageElement* EbookEngine::CreatePageLink(DrawInstr* link, RectI rect, int pageNo) {
    AutoFreeW url(str::conv::FromHtmlUtf8(link->str.s, link->str.len));
    if (url::IsAbsolute(url)) {
        return new EbookLink(link, rect, nullptr, pageNo);
    }

    DrawInstr* baseAnchor = baseAnchors.at(pageNo - 1);
    if (baseAnchor) {
        AutoFree basePath(str::DupN(baseAnchor->str.s, baseAnchor->str.len));
        AutoFree relPath(ResolveHtmlEntities(link->str.s, link->str.len));
        AutoFree absPath(NormalizeURL(relPath, basePath));
        url.Set(str::conv::FromUtf8(absPath));
    }

    PageDestination* dest = GetNamedDest(url);
    if (!dest) {
        return nullptr;
    }
    return new EbookLink(link, rect, dest, pageNo);
}

Vec<PageElement*>* EbookEngine::GetElements(int pageNo) {
    Vec<PageElement*>* els = new Vec<PageElement*>();

    Vec<DrawInstr>* pageInstrs = GetHtmlPage(pageNo);
    for (DrawInstr& i : *pageInstrs) {
        if (InstrImage == i.type) {
            els->Append(new ImageDataElement(pageNo, &i.img, GetInstrBbox(i, pageBorder)));
        } else if (InstrLinkStart == i.type && !i.bbox.IsEmptyArea()) {
            PageElement* link = CreatePageLink(&i, GetInstrBbox(i, pageBorder), pageNo);
            if (link) {
                els->Append(link);
            }
        }
    }

    return els;
}

PageElement* EbookEngine::GetElementAtPos(int pageNo, PointD pt) {
    Vec<PageElement*>* els = GetElements(pageNo);
    if (!els)
        return nullptr;

    PageElement* el = nullptr;
    for (size_t i = 0; i < els->size() && !el; i++)
        if (els->at(i)->GetRect().Contains(pt))
            el = els->at(i);

    if (el)
        els->Remove(el);
    DeleteVecMembers(*els);
    delete els;

    return el;
}

PageDestination* EbookEngine::GetNamedDest(const WCHAR* name) {
    OwnedData name_utf8(str::conv::ToUtf8(name));
    const char* id = name_utf8.Get();
    if (str::FindChar(id, '#'))
        id = str::FindChar(id, '#') + 1;

    // if the name consists of both path and ID,
    // try to first skip to the page with the desired
    // path before looking for the ID to allow
    // for the same ID to be reused on different pages
    DrawInstr* baseAnchor = nullptr;
    int basePageNo = 0;
    if (id > name_utf8.Get() + 1) {
        size_t base_len = id - name_utf8.Get() - 1;
        for (size_t i = 0; i < baseAnchors.size(); i++) {
            DrawInstr* anchor = baseAnchors.at(i);
            if (anchor && base_len == anchor->str.len && str::EqNI(name_utf8.Get(), anchor->str.s, base_len)) {
                baseAnchor = anchor;
                basePageNo = (int)i + 1;
                break;
            }
        }
    }

    size_t id_len = str::Len(id);
    for (size_t i = 0; i < anchors.size(); i++) {
        PageAnchor* anchor = &anchors.at(i);
        if (baseAnchor) {
            if (anchor->instr == baseAnchor)
                baseAnchor = nullptr;
            continue;
        }
        // note: at least CHM treats URLs as case-independent
        if (id_len == anchor->instr->str.len && str::EqNI(id, anchor->instr->str.s, id_len)) {
            RectD rect(0, anchor->instr->bbox.Y + pageBorder, pageRect.dx, 10);
            rect.Inflate(-pageBorder, 0);
            return new SimpleDest2(anchor->pageNo, rect);
        }
    }

    // don't fail if an ID doesn't exist in a merged document
    if (basePageNo != 0) {
        RectD rect(0, pageBorder, pageRect.dx, 10);
        rect.Inflate(-pageBorder, 0);
        return new SimpleDest2(basePageNo, rect);
    }

    return nullptr;
}

WCHAR* EbookEngine::ExtractFontList() {
    ScopedCritSec scope(&pagesAccess);

    Vec<mui::CachedFont*> seenFonts;
    WStrVec fonts;

    for (int pageNo = 1; pageNo <= PageCount(); pageNo++) {
        Vec<DrawInstr>* pageInstrs = GetHtmlPage(pageNo);
        if (!pageInstrs)
            continue;

        for (DrawInstr& i : *pageInstrs) {
            if (InstrSetFont != i.type || seenFonts.Contains(i.font))
                continue;
            seenFonts.Append(i.font);

            FontFamily family;
            if (!i.font->font) {
                // TODO: handle gdi
                CrashIf(!i.font->GetHFont());
                continue;
            }
            Status ok = i.font->font->GetFamily(&family);
            if (ok != Ok)
                continue;
            WCHAR fontName[LF_FACESIZE];
            ok = family.GetFamilyName(fontName);
            if (ok != Ok || fonts.FindI(fontName) != -1)
                continue;
            fonts.Append(str::Dup(fontName));
        }
    }
    if (fonts.size() == 0)
        return nullptr;

    fonts.SortNatural();
    return fonts.Join(L"\n");
}

static void AppendTocItem(EbookTocItem*& root, EbookTocItem* item, int level) {
    if (!root) {
        root = item;
        return;
    }
    // find the last child at each level, until finding the parent of the new item
    DocTocItem* r2 = root;
    while (--level > 0) {
        for (; r2->next; r2 = r2->next)
            ;
        if (r2->child)
            r2 = r2->child;
        else {
            r2->child = item;
            return;
        }
    }
    r2->AddSibling(item);
}

class EbookTocBuilder : public EbookTocVisitor {
    BaseEngine* engine;
    EbookTocItem* root;
    int idCounter;
    bool isIndex;

  public:
    explicit EbookTocBuilder(BaseEngine* engine) : engine(engine), root(nullptr), idCounter(0), isIndex(false) {}

    virtual void Visit(const WCHAR* name, const WCHAR* url, int level) {
        PageDestination* dest;
        if (!url)
            dest = nullptr;
        else if (url::IsAbsolute(url))
            dest = new SimpleDest2(0, RectD(), str::Dup(url));
        else {
            dest = engine->GetNamedDest(url);
            if (!dest && str::FindChar(url, '%')) {
                AutoFreeW decodedUrl(str::Dup(url));
                url::DecodeInPlace(decodedUrl);
                dest = engine->GetNamedDest(decodedUrl);
            }
        }

        EbookTocItem* item = new EbookTocItem(str::Dup(name), dest);
        item->id = ++idCounter;
        if (isIndex) {
            item->pageNo = 0;
            level++;
        }
        AppendTocItem(root, item, level);
    }

    EbookTocItem* GetRoot() { return root; }
    void SetIsIndex(bool value) { isIndex = value; }
};

/* BaseEngine for handling EPUB documents */

class EpubEngineImpl : public EbookEngine {
  public:
    EpubEngineImpl() : EbookEngine() {}
    virtual ~EpubEngineImpl();
    BaseEngine* Clone() override {
        if (stream) {
            return CreateFromStream(stream);
        }
        if (FileName()) {
            return CreateFromFile(FileName());
        }
        return nullptr;
    }

    unsigned char* GetFileData(size_t* cbCount) override;
    bool SaveFileAs(const char* copyFileName, bool includeUserAnnots = false) override;

    PageLayoutType PreferredLayout() override;

    WCHAR* GetProperty(DocumentProperty prop) override {
        return prop != DocumentProperty::FontList ? doc->GetProperty(prop) : ExtractFontList();
    }
    const WCHAR* GetDefaultFileExt() const override { return L".epub"; }

    bool HasTocTree() const override { return doc->HasToc(); }
    DocTocItem* GetTocTree() override;

    static BaseEngine* CreateFromFile(const WCHAR* fileName);
    static BaseEngine* CreateFromStream(IStream* stream);

  protected:
    EpubDoc* doc = nullptr;
    IStream* stream = nullptr;

    bool Load(const WCHAR* fileName);
    bool Load(IStream* stream);
    bool FinishLoading();
};

EpubEngineImpl::~EpubEngineImpl() {
    delete doc;
    if (stream)
        stream->Release();
}

bool EpubEngineImpl::Load(const WCHAR* fileName) {
    this->fileName.SetCopy(fileName);
    if (dir::Exists(fileName)) {
        // load uncompressed documents as a recompressed ZIP stream
        ScopedComPtr<IStream> zipStream(OpenDirAsZipStream(fileName, true));
        if (!zipStream)
            return false;
        return Load(zipStream);
    }
    doc = EpubDoc::CreateFromFile(fileName);
    return FinishLoading();
}

bool EpubEngineImpl::Load(IStream* stream) {
    stream->AddRef();
    this->stream = stream;
    doc = EpubDoc::CreateFromStream(stream);
    return FinishLoading();
}

bool EpubEngineImpl::FinishLoading() {
    if (!doc)
        return false;

    HtmlFormatterArgs args;
    args.htmlStr = doc->GetHtmlData();
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    args.SetFontName(GetDefaultFontName());
    args.fontSize = GetDefaultFontSize();
    args.textAllocator = &allocator;
    args.textRenderMethod = mui::TextRenderMethodGdiplusQuick;

    pages = EpubFormatter(&args, doc).FormatAllPages(false);
    if (!ExtractPageAnchors())
        return false;

    return pages->size() > 0;
}

u8* EpubEngineImpl::GetFileData(size_t* cbCount) {
    return GetStreamOrFileData(stream, fileName, cbCount);
}

bool EpubEngineImpl::SaveFileAs(const char* copyFileName, bool includeUserAnnots) {
    UNUSED(includeUserAnnots);
    AutoFreeW dstPath(str::conv::FromUtf8(copyFileName));

    if (stream) {
        OwnedData data = GetDataFromStream(stream);
        if (!data.IsEmpty() && file::WriteFile(dstPath, data.Get(), data.size)) {
            return true;
        }
    }
    if (!fileName) {
        return false;
    }
    return CopyFileW(fileName, dstPath, FALSE);
}

PageLayoutType EpubEngineImpl::PreferredLayout() {
    if (doc->IsRTL()) {
        return (PageLayoutType)(Layout_Book | Layout_R2L);
    }
    return Layout_Book;
}

DocTocItem* EpubEngineImpl::GetTocTree() {
    EbookTocBuilder builder(this);
    doc->ParseToc(&builder);
    EbookTocItem* root = builder.GetRoot();
    if (root) {
        root->OpenSingleNode();
    }
    return root;
}

BaseEngine* EpubEngineImpl::CreateFromFile(const WCHAR* fileName) {
    EpubEngineImpl* engine = new EpubEngineImpl();
    if (!engine->Load(fileName)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

BaseEngine* EpubEngineImpl::CreateFromStream(IStream* stream) {
    EpubEngineImpl* engine = new EpubEngineImpl();
    if (!engine->Load(stream)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

namespace EpubEngine {

bool IsSupportedFile(const WCHAR* fileName, bool sniff) {
    if (sniff && dir::Exists(fileName)) {
        AutoFreeW mimetypePath(path::Join(fileName, L"mimetype"));
        return file::StartsWith(mimetypePath, "application/epub+zip");
    }
    return EpubDoc::IsSupportedFile(fileName, sniff);
}

BaseEngine* CreateFromFile(const WCHAR* fileName) {
    return EpubEngineImpl::CreateFromFile(fileName);
}

BaseEngine* CreateFromStream(IStream* stream) {
    return EpubEngineImpl::CreateFromStream(stream);
}

} // namespace EpubEngine

/* BaseEngine for handling FictionBook2 documents */

class Fb2EngineImpl : public EbookEngine {
  public:
    Fb2EngineImpl() : EbookEngine(), doc(nullptr) {}
    virtual ~Fb2EngineImpl() { delete doc; }
    BaseEngine* Clone() override { return fileName ? CreateFromFile(fileName) : nullptr; }

    WCHAR* GetProperty(DocumentProperty prop) override {
        return prop != DocumentProperty::FontList ? doc->GetProperty(prop) : ExtractFontList();
    }
    const WCHAR* GetDefaultFileExt() const override { return doc->IsZipped() ? L".fb2z" : L".fb2"; }

    bool HasTocTree() const override { return doc->HasToc(); }
    DocTocItem* GetTocTree() override;

    static BaseEngine* CreateFromFile(const WCHAR* fileName);
    static BaseEngine* CreateFromStream(IStream* stream);

  protected:
    Fb2Doc* doc;

    bool Load(const WCHAR* fileName);
    bool Load(IStream* stream);
    bool FinishLoading();
};

bool Fb2EngineImpl::Load(const WCHAR* fileName) {
    this->fileName.SetCopy(fileName);
    doc = Fb2Doc::CreateFromFile(fileName);
    return FinishLoading();
}

bool Fb2EngineImpl::Load(IStream* stream) {
    doc = Fb2Doc::CreateFromStream(stream);
    return FinishLoading();
}

bool Fb2EngineImpl::FinishLoading() {
    if (!doc) {
        return false;
    }

    HtmlFormatterArgs args;
    args.htmlStr = doc->GetXmlData();
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    args.SetFontName(GetDefaultFontName());
    args.fontSize = GetDefaultFontSize();
    args.textAllocator = &allocator;
    args.textRenderMethod = mui::TextRenderMethodGdiplusQuick;

    pages = Fb2Formatter(&args, doc).FormatAllPages(false);
    if (!ExtractPageAnchors()) {
        return false;
    }

    return pages->size() > 0;
}

DocTocItem* Fb2EngineImpl::GetTocTree() {
    EbookTocBuilder builder(this);
    doc->ParseToc(&builder);
    EbookTocItem* root = builder.GetRoot();
    if (root) {
        root->OpenSingleNode();
    }
    return root;
}

BaseEngine* Fb2EngineImpl::CreateFromFile(const WCHAR* fileName) {
    Fb2EngineImpl* engine = new Fb2EngineImpl();
    if (!engine->Load(fileName)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

BaseEngine* Fb2EngineImpl::CreateFromStream(IStream* stream) {
    Fb2EngineImpl* engine = new Fb2EngineImpl();
    if (!engine->Load(stream)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

namespace Fb2Engine {

bool IsSupportedFile(const WCHAR* fileName, bool sniff) {
    return Fb2Doc::IsSupportedFile(fileName, sniff);
}

BaseEngine* CreateFromFile(const WCHAR* fileName) {
    return Fb2EngineImpl::CreateFromFile(fileName);
}

BaseEngine* CreateFromStream(IStream* stream) {
    return Fb2EngineImpl::CreateFromStream(stream);
}

} // namespace Fb2Engine

/* BaseEngine for handling Mobi documents */

#include "MobiDoc.h"

class MobiEngineImpl : public EbookEngine {
  public:
    MobiEngineImpl() : EbookEngine(), doc(nullptr) {}
    ~MobiEngineImpl() override { delete doc; }
    BaseEngine* Clone() override { return fileName ? CreateFromFile(fileName) : nullptr; }

    WCHAR* GetProperty(DocumentProperty prop) override {
        return prop != DocumentProperty::FontList ? doc->GetProperty(prop) : ExtractFontList();
    }
    const WCHAR* GetDefaultFileExt() const override { return L".mobi"; }

    PageDestination* GetNamedDest(const WCHAR* name) override;
    bool HasTocTree() const override { return doc->HasToc(); }
    DocTocItem* GetTocTree() override;

    static BaseEngine* CreateFromFile(const WCHAR* fileName);
    static BaseEngine* CreateFromStream(IStream* stream);

  protected:
    MobiDoc* doc;

    bool Load(const WCHAR* fileName);
    bool Load(IStream* stream);
    bool FinishLoading();
};

bool MobiEngineImpl::Load(const WCHAR* fileName) {
    this->fileName.SetCopy(fileName);
    doc = MobiDoc::CreateFromFile(fileName);
    return FinishLoading();
}

bool MobiEngineImpl::Load(IStream* stream) {
    doc = MobiDoc::CreateFromStream(stream);
    return FinishLoading();
}

bool MobiEngineImpl::FinishLoading() {
    if (!doc || PdbDocType::Mobipocket != doc->GetDocType()) {
        return false;
    }

    HtmlFormatterArgs args;
    args.htmlStr = doc->GetHtmlData();
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    args.SetFontName(GetDefaultFontName());
    args.fontSize = GetDefaultFontSize();
    args.textAllocator = &allocator;
    args.textRenderMethod = mui::TextRenderMethodGdiplusQuick;

    pages = MobiFormatter(&args, doc).FormatAllPages();
    if (!ExtractPageAnchors()) {
        return false;
    }

    return pages->size() > 0;
}

PageDestination* MobiEngineImpl::GetNamedDest(const WCHAR* name) {
    int filePos = _wtoi(name);
    if (filePos < 0 || 0 == filePos && *name != '0') {
        return nullptr;
    }
    int pageNo;
    for (pageNo = 1; pageNo < PageCount(); pageNo++) {
        if (pages->at(pageNo)->reparseIdx > filePos) {
            break;
        }
    }
    CrashIf(pageNo < 1 || pageNo > PageCount());

    const std::string htmlData = doc->GetHtmlData();
    size_t htmlLen = htmlData.size();
    const char* start = htmlData.data();
    if ((size_t)filePos > htmlLen) {
        return nullptr;
    }

    ScopedCritSec scope(&pagesAccess);
    Vec<DrawInstr>* pageInstrs = GetHtmlPage(pageNo);
    // link to the bottom of the page, if filePos points
    // beyond the last visible DrawInstr of a page
    float currY = (float)pageRect.dy;
    for (DrawInstr& i : *pageInstrs) {
        if ((InstrString == i.type || InstrRtlString == i.type) && i.str.s >= start && i.str.s <= start + htmlLen &&
            i.str.s - start >= filePos) {
            currY = i.bbox.Y;
            break;
        }
    }
    RectD rect(0, currY + pageBorder, pageRect.dx, 10);
    rect.Inflate(-pageBorder, 0);
    return new SimpleDest2(pageNo, rect);
}

DocTocItem* MobiEngineImpl::GetTocTree() {
    EbookTocBuilder builder(this);
    doc->ParseToc(&builder);
    EbookTocItem* root = builder.GetRoot();
    if (root)
        root->OpenSingleNode();
    return root;
}

BaseEngine* MobiEngineImpl::CreateFromFile(const WCHAR* fileName) {
    MobiEngineImpl* engine = new MobiEngineImpl();
    if (!engine->Load(fileName)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

BaseEngine* MobiEngineImpl::CreateFromStream(IStream* stream) {
    MobiEngineImpl* engine = new MobiEngineImpl();
    if (!engine->Load(stream)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

namespace MobiEngine {

bool IsSupportedFile(const WCHAR* fileName, bool sniff) {
    return MobiDoc::IsSupportedFile(fileName, sniff);
}

BaseEngine* CreateFromFile(const WCHAR* fileName) {
    return MobiEngineImpl::CreateFromFile(fileName);
}

BaseEngine* CreateFromStream(IStream* stream) {
    return MobiEngineImpl::CreateFromStream(stream);
}

} // namespace MobiEngine

/* BaseEngine for handling PalmDOC documents (and extensions such as TealDoc) */

class PdbEngineImpl : public EbookEngine {
  public:
    PdbEngineImpl() : EbookEngine(), doc(nullptr) {}
    virtual ~PdbEngineImpl() { delete doc; }
    BaseEngine* Clone() override { return fileName ? CreateFromFile(fileName) : nullptr; }

    WCHAR* GetProperty(DocumentProperty prop) override {
        return prop != DocumentProperty::FontList ? doc->GetProperty(prop) : ExtractFontList();
    }
    const WCHAR* GetDefaultFileExt() const override { return L".pdb"; }

    bool HasTocTree() const override { return doc->HasToc(); }
    DocTocItem* GetTocTree() override;

    static BaseEngine* CreateFromFile(const WCHAR* fileName);

  protected:
    PalmDoc* doc;

    bool Load(const WCHAR* fileName);
};

bool PdbEngineImpl::Load(const WCHAR* fileName) {
    this->fileName.SetCopy(fileName);

    doc = PalmDoc::CreateFromFile(fileName);
    if (!doc)
        return false;

    HtmlFormatterArgs args;
    args.htmlStr = doc->GetHtmlData();
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    args.SetFontName(GetDefaultFontName());
    args.fontSize = GetDefaultFontSize();
    args.textAllocator = &allocator;
    args.textRenderMethod = mui::TextRenderMethodGdiplusQuick;

    pages = HtmlFormatter(&args).FormatAllPages();
    if (!ExtractPageAnchors())
        return false;

    return pages->size() > 0;
}

DocTocItem* PdbEngineImpl::GetTocTree() {
    EbookTocBuilder builder(this);
    doc->ParseToc(&builder);
    return builder.GetRoot();
}

BaseEngine* PdbEngineImpl::CreateFromFile(const WCHAR* fileName) {
    PdbEngineImpl* engine = new PdbEngineImpl();
    if (!engine->Load(fileName)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

namespace PdbEngine {

bool IsSupportedFile(const WCHAR* fileName, bool sniff) {
    return PalmDoc::IsSupportedFile(fileName, sniff);
}

BaseEngine* CreateFromFile(const WCHAR* fileName) {
    return PdbEngineImpl::CreateFromFile(fileName);
}

} // namespace PdbEngine

/* formatting extensions for CHM */

#include "ChmDoc.h"

class ChmDataCache {
    ChmDoc* doc; // owned by creator
    AutoFree html;
    Vec<ImageData2> images;

  public:
    ChmDataCache(ChmDoc* doc, char* html) : doc(doc), html(html) {}
    ~ChmDataCache() {
        for (size_t i = 0; i < images.size(); i++) {
            free(images.at(i).base.data);
            free(images.at(i).fileName);
        }
    }

    std::string GetHtmlData() { return {html, str::Len(html)}; }

    // TODO: remove
    const char* GetHtmlData(size_t* lenOut) {
        *lenOut = str::Len(html);
        return html;
    }

    ImageData* GetImageData(const char* id, const char* pagePath) {
        AutoFree url(NormalizeURL(id, pagePath));
        for (size_t i = 0; i < images.size(); i++) {
            if (str::Eq(images.at(i).fileName, url))
                return &images.at(i).base;
        }

        ImageData2 data = {0};
        data.base.data = (char*)doc->GetData(url, &data.base.len);
        if (!data.base.data)
            return nullptr;
        data.fileName = url.StealData();
        images.Append(data);
        return &images.Last().base;
    }

    char* GetFileData(const char* relPath, const char* pagePath, size_t* lenOut) {
        AutoFree url(NormalizeURL(relPath, pagePath));
        return (char*)doc->GetData(url, lenOut);
    }
};

class ChmFormatter : public HtmlFormatter {
  protected:
    virtual void HandleTagImg(HtmlToken* t);
    virtual void HandleTagPagebreak(HtmlToken* t);
    virtual void HandleTagLink(HtmlToken* t);

    ChmDataCache* chmDoc;
    AutoFree pagePath;

  public:
    ChmFormatter(HtmlFormatterArgs* args, ChmDataCache* doc) : HtmlFormatter(args), chmDoc(doc) {}
};

void ChmFormatter::HandleTagImg(HtmlToken* t) {
    CrashIf(!chmDoc);
    if (t->IsEndTag())
        return;
    bool needAlt = true;
    AttrInfo* attr = t->GetAttrByName("src");
    if (attr) {
        AutoFree src(str::DupN(attr->val, attr->valLen));
        url::DecodeInPlace(src);
        ImageData* img = chmDoc->GetImageData(src, pagePath);
        needAlt = !img || !EmitImage(img);
    }
    if (needAlt && (attr = t->GetAttrByName("alt")) != nullptr)
        HandleText(attr->val, attr->valLen);
}

void ChmFormatter::HandleTagPagebreak(HtmlToken* t) {
    AttrInfo* attr = t->GetAttrByName("page_path");
    if (!attr || pagePath)
        ForceNewPage();
    if (attr) {
        RectF bbox(0, currY, pageDx, 0);
        currPage->instructions.Append(DrawInstr::Anchor(attr->val, attr->valLen, bbox));
        pagePath.Set(str::DupN(attr->val, attr->valLen));
        // reset CSS style rules for the new document
        styleRules.Reset();
    }
}

void ChmFormatter::HandleTagLink(HtmlToken* t) {
    CrashIf(!chmDoc);
    if (t->IsEndTag())
        return;
    AttrInfo* attr = t->GetAttrByName("rel");
    if (!attr || !attr->ValIs("stylesheet"))
        return;
    attr = t->GetAttrByName("type");
    if (attr && !attr->ValIs("text/css"))
        return;
    attr = t->GetAttrByName("href");
    if (!attr)
        return;

    size_t len;
    AutoFree src(str::DupN(attr->val, attr->valLen));
    url::DecodeInPlace(src);
    AutoFree data(chmDoc->GetFileData(src, pagePath, &len));
    if (data)
        ParseStyleSheet(data, len);
}

/* BaseEngine for handling CHM documents */

class ChmEmbeddedDest;

class ChmEngineImpl : public EbookEngine {
    friend ChmEmbeddedDest;

  public:
    ChmEngineImpl() : EbookEngine(), doc(nullptr), dataCache(nullptr) {
        // ISO 216 A4 (210mm x 297mm)
        pageRect = RectD(0, 0, 8.27 * GetFileDPI(), 11.693 * GetFileDPI());
    }
    virtual ~ChmEngineImpl() {
        delete dataCache;
        delete doc;
    }
    BaseEngine* Clone() override { return fileName ? CreateFromFile(fileName) : nullptr; }

    WCHAR* GetProperty(DocumentProperty prop) override {
        return prop != DocumentProperty::FontList ? doc->GetProperty(prop) : ExtractFontList();
    }
    const WCHAR* GetDefaultFileExt() const override { return L".chm"; }

    PageLayoutType PreferredLayout() override { return Layout_Single; }

    PageDestination* GetNamedDest(const WCHAR* name) override;
    bool HasTocTree() const override { return doc->HasToc() || doc->HasIndex(); }
    DocTocItem* GetTocTree() override;

    static BaseEngine* CreateFromFile(const WCHAR* fileName);

  protected:
    ChmDoc* doc;
    ChmDataCache* dataCache;

    bool Load(const WCHAR* fileName);

    virtual PageElement* CreatePageLink(DrawInstr* link, RectI rect, int pageNo);
    bool SaveEmbedded(LinkSaverUI& saveUI, const char* path);
};

// cf. http://www.w3.org/TR/html4/charset.html#h-5.2.2
static UINT ExtractHttpCharset(const char* html, size_t htmlLen) {
    if (!strstr(html, "charset="))
        return 0;

    HtmlPullParser parser(html, std::min(htmlLen, (size_t)1024));
    HtmlToken* tok;
    while ((tok = parser.Next()) != nullptr && !tok->IsError()) {
        if (tok->tag != Tag_Meta)
            continue;
        AttrInfo* attr = tok->GetAttrByName("http-equiv");
        if (!attr || !attr->ValIs("Content-Type"))
            continue;
        attr = tok->GetAttrByName("content");
        AutoFree mimetype, charset;
        if (!attr || !str::Parse(attr->val, attr->valLen, "%S;%_charset=%S", &mimetype, &charset))
            continue;

        static struct {
            const char* name;
            UINT codepage;
        } codepages[] = {
            {"ISO-8859-1", 1252},  {"Latin1", 1252},   {"CP1252", 1252},   {"Windows-1252", 1252},
            {"ISO-8859-2", 28592}, {"Latin2", 28592},  {"CP1251", 1251},   {"Windows-1251", 1251},
            {"KOI8-R", 20866},     {"shift-jis", 932}, {"x-euc", 932},     {"euc-kr", 949},
            {"Big5", 950},         {"GB2312", 936},    {"UTF-8", CP_UTF8},
        };
        for (int i = 0; i < dimof(codepages); i++) {
            if (str::EqI(charset, codepages[i].name))
                return codepages[i].codepage;
        }
        break;
    }

    return 0;
}

class ChmHtmlCollector : public EbookTocVisitor {
    ChmDoc* doc;
    WStrList added;
    str::Str<char> html;

  public:
    explicit ChmHtmlCollector(ChmDoc* doc) : doc(doc) {}

    char* GetHtml() {
        // first add the homepage
        const char* index = doc->GetHomePath();
        AutoFreeW url(doc->ToStr(index));
        Visit(nullptr, url, 0);

        // then add all pages linked to from the table of contents
        doc->ParseToc(this);

        // finally add all the remaining HTML files
        Vec<char*>* paths = doc->GetAllPaths();
        for (size_t i = 0; i < paths->size(); i++) {
            char* path = paths->at(i);
            if (str::EndsWithI(path, ".htm") || str::EndsWithI(path, ".html")) {
                if (*path == '/')
                    path++;
                url.Set(str::conv::FromUtf8(path));
                Visit(nullptr, url, -1);
            }
        }
        paths->FreeMembers();
        delete paths;

        return html.StealData();
    }

    virtual void Visit(const WCHAR* name, const WCHAR* url, int level) {
        UNUSED(name);
        UNUSED(level);
        if (!url || url::IsAbsolute(url))
            return;
        AutoFreeW plainUrl(url::GetFullPath(url));
        if (added.FindI(plainUrl) != -1)
            return;
        OwnedData urlUtf8(str::conv::ToUtf8(plainUrl));
        size_t pageHtmlLen;
        ScopedMem<unsigned char> pageHtml(doc->GetData(urlUtf8.Get(), &pageHtmlLen));
        if (!pageHtml)
            return;
        html.AppendFmt("<pagebreak page_path=\"%s\" page_marker />", urlUtf8.Get());
        html.AppendAndFree(doc->ToUtf8(pageHtml, ExtractHttpCharset((const char*)pageHtml.Get(), pageHtmlLen)));
        added.Append(plainUrl.StealData());
    }
};

bool ChmEngineImpl::Load(const WCHAR* fileName) {
    this->fileName.SetCopy(fileName);
    doc = ChmDoc::CreateFromFile(fileName);
    if (!doc)
        return false;

    char* html = ChmHtmlCollector(doc).GetHtml();
    dataCache = new ChmDataCache(doc, html);

    HtmlFormatterArgs args;
    args.htmlStr = dataCache->GetHtmlData();
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    args.SetFontName(GetDefaultFontName());
    args.fontSize = GetDefaultFontSize();
    args.textAllocator = &allocator;
    args.textRenderMethod = mui::TextRenderMethodGdiplusQuick;

    pages = ChmFormatter(&args, dataCache).FormatAllPages(false);
    if (!ExtractPageAnchors())
        return false;

    return pages->size() > 0;
}

PageDestination* ChmEngineImpl::GetNamedDest(const WCHAR* name) {
    PageDestination* dest = EbookEngine::GetNamedDest(name);
    if (!dest) {
        unsigned int topicID;
        if (str::Parse(name, L"%u%$", &topicID)) {
            AutoFree urlUtf8(doc->ResolveTopicID(topicID));
            if (urlUtf8) {
                AutoFreeW url(str::conv::FromUtf8(urlUtf8));
                dest = EbookEngine::GetNamedDest(url);
            }
        }
    }
    return dest;
}

DocTocItem* ChmEngineImpl::GetTocTree() {
    EbookTocBuilder builder(this);
    doc->ParseToc(&builder);
    if (doc->HasIndex()) {
        // TODO: ToC code doesn't work too well for displaying an index,
        //       so this should really become a tree of its own (which
        //       doesn't rely on entries being in the same order as pages)
        builder.Visit(L"Index", nullptr, 1);
        builder.SetIsIndex(true);
        doc->ParseIndex(&builder);
    }
    EbookTocItem* root = builder.GetRoot();
    if (root)
        root->OpenSingleNode();
    return root;
}

class ChmEmbeddedDest : public PageDestination {
    ChmEngineImpl* engine;
    AutoFree path;

  public:
    ChmEmbeddedDest(ChmEngineImpl* engine, const char* path) : engine(engine), path(str::Dup(path)) {}

    PageDestType GetDestType() const override { return PageDestType::LaunchEmbedded; }
    int GetDestPageNo() const override { return 0; }
    RectD GetDestRect() const override { return RectD(); }
    WCHAR* GetDestValue() const override { return str::conv::FromUtf8(path::GetBaseName(path)); }

    bool SaveEmbedded(LinkSaverUI& saveUI) override { return engine->SaveEmbedded(saveUI, path); }
};

PageElement* ChmEngineImpl::CreatePageLink(DrawInstr* link, RectI rect, int pageNo) {
    PageElement* linkEl = EbookEngine::CreatePageLink(link, rect, pageNo);
    if (linkEl)
        return linkEl;

    DrawInstr* baseAnchor = baseAnchors.at(pageNo - 1);
    AutoFree basePath(str::DupN(baseAnchor->str.s, baseAnchor->str.len));
    AutoFree url(str::DupN(link->str.s, link->str.len));
    url.Set(NormalizeURL(url, basePath));
    if (!doc->HasData(url))
        return nullptr;

    PageDestination* dest = new ChmEmbeddedDest(this, url);
    return new EbookLink(link, rect, dest, pageNo);
}

bool ChmEngineImpl::SaveEmbedded(LinkSaverUI& saveUI, const char* path) {
    size_t len;
    ScopedMem<unsigned char> data(doc->GetData(path, &len));
    if (!data)
        return false;
    return saveUI.SaveEmbedded(data, len);
}

BaseEngine* ChmEngineImpl::CreateFromFile(const WCHAR* fileName) {
    ChmEngineImpl* engine = new ChmEngineImpl();
    if (!engine->Load(fileName)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

namespace ChmEngine {

bool IsSupportedFile(const WCHAR* fileName, bool sniff) {
    return ChmDoc::IsSupportedFile(fileName, sniff);
}

BaseEngine* CreateFromFile(const WCHAR* fileName) {
    return ChmEngineImpl::CreateFromFile(fileName);
}

} // namespace ChmEngine

/* BaseEngine for handling HTML documents */
/* (mainly to allow creating minimal regression test testcases more easily) */

class HtmlEngineImpl : public EbookEngine {
  public:
    HtmlEngineImpl() : EbookEngine(), doc(nullptr) {
        // ISO 216 A4 (210mm x 297mm)
        pageRect = RectD(0, 0, 8.27 * GetFileDPI(), 11.693 * GetFileDPI());
    }
    virtual ~HtmlEngineImpl() { delete doc; }
    BaseEngine* Clone() override { return fileName ? CreateFromFile(fileName) : nullptr; }

    WCHAR* GetProperty(DocumentProperty prop) override {
        return prop != DocumentProperty::FontList ? doc->GetProperty(prop) : ExtractFontList();
    }
    const WCHAR* GetDefaultFileExt() const override { return L".html"; }
    PageLayoutType PreferredLayout() override { return Layout_Single; }

    static BaseEngine* CreateFromFile(const WCHAR* fileName);

  protected:
    HtmlDoc* doc;

    bool Load(const WCHAR* fileName);

    virtual PageElement* CreatePageLink(DrawInstr* link, RectI rect, int pageNo);
};

bool HtmlEngineImpl::Load(const WCHAR* fileName) {
    this->fileName.SetCopy(fileName);

    doc = HtmlDoc::CreateFromFile(fileName);
    if (!doc)
        return false;

    HtmlFormatterArgs args;
    args.htmlStr = doc->GetHtmlData();
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    args.SetFontName(GetDefaultFontName());
    args.fontSize = GetDefaultFontSize();
    args.textAllocator = &allocator;
    args.textRenderMethod = mui::TextRenderMethodGdiplus;

    pages = HtmlFileFormatter(&args, doc).FormatAllPages(false);
    if (!ExtractPageAnchors())
        return false;

    return pages->size() > 0;
}

class RemoteHtmlDest : public SimpleDest2 {
    AutoFreeW name;

  public:
    explicit RemoteHtmlDest(const WCHAR* relativeURL) : SimpleDest2(0, RectD()) {
        const WCHAR* id = str::FindChar(relativeURL, '#');
        if (id) {
            value.Set(str::DupN(relativeURL, id - relativeURL));
            name.SetCopy(id);
        } else
            value.SetCopy(relativeURL);
    }

    virtual PageDestType GetDestType() const { return PageDestType::LaunchFile; }
    virtual WCHAR* GetDestName() const { return str::Dup(name); }
};

PageElement* HtmlEngineImpl::CreatePageLink(DrawInstr* link, RectI rect, int pageNo) {
    if (0 == link->str.len)
        return nullptr;

    AutoFreeW url(str::conv::FromHtmlUtf8(link->str.s, link->str.len));
    if (url::IsAbsolute(url) || '#' == *url)
        return EbookEngine::CreatePageLink(link, rect, pageNo);

    PageDestination* dest = new RemoteHtmlDest(url);
    return new EbookLink(link, rect, dest, pageNo, true);
}

BaseEngine* HtmlEngineImpl::CreateFromFile(const WCHAR* fileName) {
    HtmlEngineImpl* engine = new HtmlEngineImpl();
    if (!engine->Load(fileName)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

namespace HtmlEngine {

bool IsSupportedFile(const WCHAR* fileName, bool sniff) {
    return HtmlDoc::IsSupportedFile(fileName, sniff);
}

BaseEngine* CreateFromFile(const WCHAR* fileName) {
    return HtmlEngineImpl::CreateFromFile(fileName);
}

} // namespace HtmlEngine

/* BaseEngine for handling TXT documents */

class TxtEngineImpl : public EbookEngine {
  public:
    TxtEngineImpl() : EbookEngine(), doc(nullptr) {
        // ISO 216 A4 (210mm x 297mm)
        pageRect = RectD(0, 0, 8.27 * GetFileDPI(), 11.693 * GetFileDPI());
    }
    virtual ~TxtEngineImpl() { delete doc; }
    BaseEngine* Clone() override { return fileName ? CreateFromFile(fileName) : nullptr; }

    WCHAR* GetProperty(DocumentProperty prop) override {
        return prop != DocumentProperty::FontList ? doc->GetProperty(prop) : ExtractFontList();
    }
    const WCHAR* GetDefaultFileExt() const override { return fileName ? path::GetExt(fileName) : L".txt"; }
    PageLayoutType PreferredLayout() override { return Layout_Single; }

    bool HasTocTree() const override { return doc->HasToc(); }
    DocTocItem* GetTocTree() override;

    static BaseEngine* CreateFromFile(const WCHAR* fileName);

  protected:
    TxtDoc* doc;

    bool Load(const WCHAR* fileName);
};

bool TxtEngineImpl::Load(const WCHAR* fileName) {
    this->fileName.SetCopy(fileName);

    doc = TxtDoc::CreateFromFile(fileName);
    if (!doc)
        return false;

    if (doc->IsRFC()) {
        // RFCs are targeted at letter size pages
        pageRect = RectD(0, 0, 8.5 * GetFileDPI(), 11 * GetFileDPI());
    }

    HtmlFormatterArgs args;
    args.htmlStr = doc->GetHtmlData();
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    args.SetFontName(GetDefaultFontName());
    args.fontSize = GetDefaultFontSize();
    args.textAllocator = &allocator;
    args.textRenderMethod = mui::TextRenderMethodGdiplus;

    pages = TxtFormatter(&args).FormatAllPages(false);
    if (!ExtractPageAnchors())
        return false;

    return pages->size() > 0;
}

DocTocItem* TxtEngineImpl::GetTocTree() {
    EbookTocBuilder builder(this);
    doc->ParseToc(&builder);
    return builder.GetRoot();
}

BaseEngine* TxtEngineImpl::CreateFromFile(const WCHAR* fileName) {
    TxtEngineImpl* engine = new TxtEngineImpl();
    if (!engine->Load(fileName)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

namespace TxtEngine {

bool IsSupportedFile(const WCHAR* fileName, bool sniff) {
    return TxtDoc::IsSupportedFile(fileName, sniff);
}

BaseEngine* CreateFromFile(const WCHAR* fileName) {
    return TxtEngineImpl::CreateFromFile(fileName);
}

} // namespace TxtEngine
