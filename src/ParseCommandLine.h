/* Copyright 2018 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct PageRange {
    PageRange() : start(1), end(INT_MAX) {}
    PageRange(int start, int end) : start(start), end(end) {}

    int start, end; // end == INT_MAX means to the last page
};

class CommandLineInfo {
  public:
    WStrVec fileNames;
    // pathsToBenchmark contain 2 strings per each file to benchmark:
    // - name of the file to benchmark
    // - optional (nullptr if not available) string that represents which pages
    //   to benchmark. It can also be a string "loadonly" which means we'll
    //   only benchmark loading of the catalog
    WStrVec pathsToBenchmark;
    bool makeDefault;
    bool exitWhenDone;
    bool printDialog;
    AutoFreeW printerName;
    AutoFreeW printSettings;
    AutoFreeW forwardSearchOrigin;
    int forwardSearchLine;
    bool reuseDdeInstance;
    AutoFreeW destName;
    int pageNumber;
    bool restrictedUse;
    bool enterPresentation;
    bool enterFullScreen;
    DisplayMode startView;
    float startZoom;
    PointI startScroll;
    bool showConsole;
    HWND hwndPluginParent;
    AutoFreeW pluginURL;
    bool exitImmediately;
    bool silent;
    AutoFreeW appdataDir;
    AutoFreeW inverseSearchCmdLine;
    bool invertColors;

    // stress-testing related
    AutoFreeW stressTestPath;
    AutoFreeW stressTestFilter; // nullptr is equivalent to "*" (i.e. all files)
    AutoFreeW stressTestRanges;
    int stressTestCycles;
    int stressParallelCount;
    bool stressRandomizeFiles;

    // related to testing
    bool testRenderPage;
    bool testExtractPage;
    int testPageNo;

    bool crashOnOpen;

    // deprecated flags
    AutoFree lang;
    WStrVec globalPrefArgs;

    CommandLineInfo()
        : makeDefault(false),
          exitWhenDone(false),
          printDialog(false),
          printerName(nullptr),
          printSettings(nullptr),
          reuseDdeInstance(false),
          lang(nullptr),
          destName(nullptr),
          pageNumber(-1),
          restrictedUse(false),
          pluginURL(nullptr),
          enterPresentation(false),
          enterFullScreen(false),
          hwndPluginParent(nullptr),
          startView(DM_AUTOMATIC),
          startZoom(INVALID_ZOOM),
          startScroll(PointI(-1, -1)),
          showConsole(false),
          exitImmediately(false),
          silent(false),
          forwardSearchOrigin(nullptr),
          forwardSearchLine(0),
          stressTestPath(nullptr),
          stressTestFilter(nullptr),
          stressTestRanges(nullptr),
          stressTestCycles(1),
          stressParallelCount(1),
          stressRandomizeFiles(false),
          testRenderPage(false),
          testExtractPage(false),
          appdataDir(nullptr),
          inverseSearchCmdLine(nullptr),
          invertColors(false),
          crashOnOpen(false) {}

    ~CommandLineInfo() {}

    void ParseCommandLine(const WCHAR* cmdLine);
};

void ParseColor(COLORREF* destColor, const WCHAR* txt);
bool IsValidPageRange(const WCHAR* ranges);
bool IsBenchPagesInfo(const WCHAR* s);
bool ParsePageRanges(const WCHAR* ranges, Vec<PageRange>& result);
