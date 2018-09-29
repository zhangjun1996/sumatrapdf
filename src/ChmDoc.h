/* Copyright 2018 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

class ChmDoc {
    struct chmFile* chmHandle;

    // Data parsed from /#WINDOWS, /#STRINGS, /#SYSTEM files inside CHM file
    AutoFree title;
    AutoFree tocPath;
    AutoFree indexPath;
    AutoFree homePath;
    AutoFree creator;
    UINT codepage;

    void ParseWindowsData();
    bool ParseSystemData();
    bool ParseTocOrIndex(EbookTocVisitor* visitor, const char* path, bool isIndex);
    void FixPathCodepage(AutoFree& path, UINT& fileCP);

    bool Load(const WCHAR* fileName);

  public:
    ChmDoc() : chmHandle(nullptr), codepage(0) {}
    ~ChmDoc();

    bool HasData(const char* fileName);
    unsigned char* GetData(const char* fileName, size_t* lenOut);
    char* ResolveTopicID(unsigned int id);

    char* ToUtf8(const unsigned char* text, UINT overrideCP = 0);
    WCHAR* ToStr(const char* text);

    WCHAR* GetProperty(DocumentProperty prop);
    const char* GetHomePath();
    Vec<char*>* GetAllPaths();

    bool HasToc() const;
    bool ParseToc(EbookTocVisitor* visitor);
    bool HasIndex() const;
    bool ParseIndex(EbookTocVisitor* visitor);

    static bool IsSupportedFile(const WCHAR* fileName, bool sniff = false);
    static ChmDoc* CreateFromFile(const WCHAR* fileName);
};
