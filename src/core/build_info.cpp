// A successful build must select the C++26 language mode. Individual optional
// features (reflection, contracts, std::execution) require separate probes when used.
#if defined(_MSVC_LANG)
static_assert(_MSVC_LANG > 202302L,
              "CodeStudio-Agent requires C++26 mode; on Windows select the verified clang-cl toolchain.");
#else
static_assert(__cplusplus > 202302L, "CodeStudio-Agent requires a compiler in C++26 mode.");
#endif
