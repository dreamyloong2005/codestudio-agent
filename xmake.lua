set_project("CodeStudio-Agent")
set_version("0.1.0")
set_xmakever("3.0.5")
set_languages("cxx26")
set_policy("package.requires_lock", true)
add_rules("mode.debug", "mode.release")
if is_plat("windows") then
    add_cxxflags("/utf-8", {tools = {"cl", "clang_cl"}})
end

option("system_deps")
    set_default(false)
    set_showmenu(true)
    set_description("Use Linux distribution development packages through pkg-config")
option_end()

if is_plat("linux") then
    add_requires("pkgconfig::libsecret-1", {alias = "libsecret", system = true})
end

if is_plat("linux") and has_config("system_deps") then
    add_requires("pkgconfig::nlohmann_json", {alias = "nlohmann_json", system = true})
    add_requires("pkgconfig::sqlite3", {alias = "sqlite3", system = true})
    add_requires("pkgconfig::libcurl", {alias = "libcurl", system = true})
    add_requires("pkgconfig::openssl", {alias = "openssl", system = true})
else
    add_requires("nlohmann_json 3.12.0")
    add_requires("sqlite3 3.50.0+400", {configs = {shared = false}})
    if not is_plat("windows") then
        add_requires("libcurl 8.15.0", {configs = {shared = false}})
        add_requires("openssl", {configs = {shared = false}})
    end
end

target("agent-core")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_packages("nlohmann_json", "sqlite3", {public = true})
    add_files("src/core/*.cpp", "src/provider/*.cpp", "src/tools/*.cpp", "src/platform/*.cpp", "src/mcp/*.cpp", "src/application/*.cpp", "src/execution/*.cpp")
    add_files("src/plugins/*.cpp")
    if is_plat("windows") then
        add_syslinks("winhttp", "userenv", "advapi32", "bcrypt", "ole32", "taskschd", "oleaut32", "shell32", "ws2_32", {public = true})
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN", {public = true})
        add_cxxflags("/utf-8", {tools = {"cl", "clang_cl"}})
    else
        add_packages("libcurl", "openssl", {public = true})
        add_syslinks("pthread", {public = true})
        if is_plat("linux") then
            add_packages("libsecret", {public = true})
        end
    end
target_end()

target("codestudio-agent")
    set_kind("binary")
    set_rundir("$(projectdir)")
    add_files("src/cli/*.cpp")
    add_deps("agent-core")
target_end()

target("process-fixture")
    set_kind("binary")
    set_default(false)
    add_files("tests/process_fixture.cpp")
target_end()

target("line-process-fixture")
    set_kind("binary")
    set_default(false)
    add_files("tests/line_process_fixture.cpp")
target_end()

target("mcp-fixture")
    set_kind("binary")
    set_default(false)
    add_files("tests/mcp_fixture.cpp")
    add_packages("nlohmann_json")
target_end()

target("sandbox-fixture")
    set_kind("binary")
    set_default(false)
    add_files("tests/sandbox_fixture.cpp")
    add_packages("nlohmann_json")
    if is_plat("windows") then
        set_runtimes("MT")
        add_syslinks("ws2_32", "advapi32", "userenv", "ole32")
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
    end
target_end()

target("plugin-sandbox-dependency")
    set_kind("shared")
    set_default(false)
    add_files("tests/plugin_sandbox_dependency.cpp")
    if is_plat("windows") then set_runtimes("MT") end
target_end()

target("plugin-sandbox-library")
    set_kind("shared")
    set_default(false)
    add_files("tests/plugin_sandbox_library.cpp")
    add_deps("plugin-sandbox-dependency")
    if is_plat("windows") then set_runtimes("MT") end
target_end()

target("plugin-sandbox-fixture")
    set_kind("binary")
    set_default(false)
    add_files("tests/plugin_sandbox_fixture.cpp")
    add_packages("nlohmann_json")
    add_deps("plugin-sandbox-library", {inherit = false})
    if is_plat("windows") then
        set_runtimes("MT")
        add_syslinks("advapi32", "ws2_32")
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
    else
        add_syslinks("dl", "pthread")
    end
target_end()

target("agent-tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/core_tests.cpp", "tests/tools_tests.cpp", "tests/process_tests.cpp", "tests/responses_tests.cpp", "tests/messages_tests.cpp", "tests/line_process_tests.cpp", "tests/mcp_tests.cpp")
    add_files("tests/checkpoint_store_tests.cpp", "tests/resume_kernel_tests.cpp")
    add_files("tests/scheduler_store_tests.cpp")
    add_files("tests/credentials_tests.cpp")
    add_files("tests/cron_tests.cpp", "tests/cron_store_tests.cpp", "tests/autostart_tests.cpp", "tests/process_observation_tests.cpp")
    add_files("tests/sandbox_tests.cpp")
    add_files("tests/state_reader_tests.cpp")
    add_files("tests/build_workspace_tests.cpp")
    add_files("tests/workspace_execution_tests.cpp")
    add_files("tests/build_proposal_tests.cpp", "tests/build_decision_tests.cpp")
    add_deps("agent-core", "process-fixture", "line-process-fixture", "mcp-fixture", "sandbox-fixture", "build-fixture", "plugin-sandbox-fixture")
    if is_plat("windows") then
        add_syslinks("ws2_32")
    end
target_end()

target("build-fixture")
    set_kind("binary")
    set_default(false)
    add_files("tests/build_fixture.cpp")
    add_packages("nlohmann_json")
    if is_plat("windows") then
        set_runtimes("MT")
        add_syslinks("advapi32", "ws2_32")
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
    end
target_end()

target("provider-fixture")
    set_kind("binary")
    set_default(false)
    add_files("tests/provider_fixture.cpp")
    add_deps("agent-core")
target_end()
