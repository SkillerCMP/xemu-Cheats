// Shared Microsoft symbol-display helper golden tests.
#include "label-symbol-utils.hh"

#include <cstdio>
#include <string>

static int fail(const char *what)
{
    std::fprintf(stderr, "FAIL: %s\n", what);
    return 1;
}

int main()
{
    using namespace XemuLabelSymbolUtils;

    if (clean_c_symbol("__imp__helper@4") != "helper" ||
        clean_c_symbol("@Thunk@8") != "Thunk" ||
        clean_c_symbol("_g_Value") != "g_Value" ||
        clean_c_symbol("Plain") != "Plain") {
        return fail("C/stdcall/import cleanup");
    }

    if (simple_msvc_name("?Pause@cBackgroundLoader@@UAEXXZ") !=
            "cBackgroundLoader::Pause" ||
        simple_msvc_name("??0cBackgroundLoader@@QAE@H@Z") !=
            "cBackgroundLoader::cBackgroundLoader" ||
        simple_msvc_name("??1cBackgroundLoader@@QAE@XZ") !=
            "cBackgroundLoader::~cBackgroundLoader" ||
        simple_msvc_name("?Run@Inner@Outer@@SAXXZ") !=
            "Outer::Inner::Run") {
        return fail("simple MSVC name cleanup");
    }

    if (!simple_msvc_name("??2@YAPAXI@Z").empty() ||
        !simple_msvc_name("?Func@?$Thing@H@@SAXXZ").empty() ||
        !simple_msvc_name("not_msvc").empty()) {
        return fail("unsupported MSVC forms stay untouched by caller");
    }

    std::puts("PASS: shared Microsoft symbol-display helper cases");
    return 0;
}
