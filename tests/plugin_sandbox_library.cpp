#ifdef _WIN32
#define EXPORTED __declspec(dllexport)
extern "C" __declspec(dllimport) int codestudio_plugin_dependency_value();
#else
#define EXPORTED __attribute__((visibility("default")))
extern "C" int codestudio_plugin_dependency_value();
#endif

// A real dynamic dependency proves that staging covers the package's loader
// graph, rather than only the file explicitly opened by the executable.
extern "C" EXPORTED int codestudio_plugin_fixture_value() { return codestudio_plugin_dependency_value(); }
