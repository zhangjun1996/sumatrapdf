/* Copyright 2018 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#if defined(__clang__)

#elif defined(__GNUC__) || defined(__GNUG__)
#define NOINLINE __attribute__((noinline))
#define OPTS_OFF _Pragma("GCC push_options") _Pragma("GCC optimize (\"O0\")")
#define OPTS_ON #pragma GCC pop_options
#elif defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#define OPTS_OFF __pragma(optimize("", off))
#define OPTS_ON __pragma(optimize("", on))
#endif

class IatHook {
  public:
    IatHook(const std::string& dllName, const std::string& apiName, const char* fnCallback,
            uint64_t* userOrigVar, const std::wstring& moduleName);
    IatHook(const std::string& dllName, const std::string& apiName, const uint64_t fnCallback,
            uint64_t* userOrigVar, const std::wstring& moduleName);
    bool hook();
    bool unHook();

  private:
    std::string m_dllName;
    std::string m_apiName;
    std::wstring m_moduleName;

    uint64_t m_fnCallback;
    uint64_t m_origFunc;
    uint64_t* m_userOrigVar;

    bool m_hooked;
};
