#ifdef _WIN32
#define EXPORTED __declspec(dllexport)
#else
#define EXPORTED __attribute__((visibility("default")))
#endif

extern "C" EXPORTED int codestudio_plugin_dependency_value() { return 7331; }
