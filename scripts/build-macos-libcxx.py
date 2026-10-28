#!/usr/bin/env python3
"""Build a workspace-local LLVM 22 libc++ with chrono tzdb for macOS arm64.

Requires an existing LLVM 22.1.8 compiler, CMake 3.20+, Ninja, Xcode SDK, and
the system IANA tzdata.zi/leapseconds files. Nothing is installed system-wide.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile


VERSION = "22.1.8"
SOURCE_NAME = f"llvm-project-{VERSION}.src.tar.xz"
SOURCE_SHA256 = "922f1817a0df7b1489272d18134ee0087a8b068828f87ac63b9861b1a9965888"
SOURCE_URL = f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{VERSION}/{SOURCE_NAME}"
TZDB_BEFORE = '#if defined(__linux__)\n  return "/usr/share/zoneinfo/";'
TZDB_AFTER = '#if defined(__linux__) || defined(__APPLE__)\n  return "/usr/share/zoneinfo/";'
PROBE = r"""
#include <chrono>
#include <format>
#include <iostream>
#include <stdexcept>
static_assert(__cplusplus > 202302L, "C++26 mode is required");
int main() {
    using namespace std::chrono;
    if (std::format("{}-{}", "format", 26) != "format-26") return 1;
    const auto* zone = locate_zone("America/New_York");
    const auto gap = zone->get_info(local_days{2024y/March/10} + 2h + 30min);
    const auto fold = zone->get_info(local_days{2024y/November/3} + 1h + 30min);
    if (gap.result != local_info::nonexistent || fold.result != local_info::ambiguous) return 2;
    if (locate_zone("Asia/Shanghai")->get_info(sys_days{2024y/January/1}).offset != 8h) return 3;
    std::cout << std::format("C++{} libc++={} tzdb={} current_zone={}\n",
        __cplusplus, _LIBCPP_VERSION, get_tzdb().version, current_zone()->name());
}
"""


def run(command, log):
    print("Running:", " ".join(str(part) for part in command), flush=True)
    with log.open("w") as output:
        result = subprocess.run([str(part) for part in command], stdout=output,
                                stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}); see {log}")


def sha256(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-root", required=True, type=Path)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ninja", default="ninja")
    parser.add_argument("--build-root", type=Path,
                        default=Path(__file__).resolve().parents[1] / "build/toolchains")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 6))
    args = parser.parse_args()
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        parser.error("This toolchain is for native macOS arm64 only")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    compiler = args.llvm_root.resolve() / "bin/clang++"
    compiler_version = subprocess.check_output([str(compiler), "--version"], text=True)
    if f"clang version {VERSION}" not in compiler_version:
        parser.error(f"LLVM {VERSION} is required")
    cmake, ninja = shutil.which(args.cmake), shutil.which(args.ninja)
    if not cmake or not ninja:
        parser.error("Provide working CMake and Ninja executables")
    for filename in ("tzdata.zi", "leapseconds"):
        if not (Path("/usr/share/zoneinfo") / filename).is_file():
            parser.error(f"System IANA {filename} is missing; no timezone fallback is used")
    root = args.build_root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    archive = root / SOURCE_NAME
    if not archive.is_file():
        descriptor, temporary = tempfile.mkstemp(prefix=SOURCE_NAME + ".", dir=root)
        os.close(descriptor)
        temporary = Path(temporary)
        try:
            run(["curl", "--fail", "--location", "--retry", "2", "--silent", "--show-error",
                 SOURCE_URL, "--output", temporary], root / "libcxx-download.log")
            if sha256(temporary) != SOURCE_SHA256:
                raise RuntimeError("LLVM source checksum mismatch")
            temporary.replace(archive)
        finally:
            temporary.unlink(missing_ok=True)
    if sha256(archive) != SOURCE_SHA256:
        raise RuntimeError(f"LLVM source checksum mismatch: {archive}")
    source = root / f"llvm-project-{VERSION}.src"
    stamp = source / ".codestudio-tzdb-patch.json"
    if not stamp.is_file():
        members = [str(Path(source.name) / name) for name in
                   ("runtimes", "cmake", "libc", "libcxx", "libcxxabi", "llvm/cmake")]
        run(["tar", "-xf", archive, "-C", root, *members], root / "libcxx-extract.log")
        tzdb = source / "libcxx/src/experimental/tzdb.cpp"
        original = tzdb.read_text()
        if original.count(TZDB_BEFORE) != 1:
            raise RuntimeError("Upstream libc++ timezone directory changed; refusing an unchecked patch")
        tzdb.write_text(original.replace(TZDB_BEFORE, TZDB_AFTER))
        stamp.write_text(json.dumps({"source_sha256": SOURCE_SHA256,
                                     "patch": "darwin-system-tzdb-directory-v1"}, indent=2) + "\n")
    if TZDB_AFTER not in (source / "libcxx/src/experimental/tzdb.cpp").read_text():
        raise RuntimeError("The extracted libc++ source does not contain the expected Darwin patch")
    build = root / "libcxx-build"
    install = root / f"libcxx-{VERSION}-tzdb"
    sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
    run([cmake, "-G", "Ninja", "-S", source / "runtimes", "-B", build,
         f"-DCMAKE_MAKE_PROGRAM={ninja}", "-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi",
         f"-DCMAKE_C_COMPILER={compiler.parent / 'clang'}", f"-DCMAKE_CXX_COMPILER={compiler}",
         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_OSX_ARCHITECTURES=arm64",
         "-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0", f"-DCMAKE_OSX_SYSROOT={sdk}",
         f"-DCMAKE_INSTALL_PREFIX={install}", "-DLIBCXX_ENABLE_TIME_ZONE_DATABASE=ON",
         "-DLIBCXX_INCLUDE_TESTS=OFF", "-DLIBCXXABI_INCLUDE_TESTS=OFF",
         "-DLLVM_INCLUDE_TESTS=OFF", "-DLIBCXXABI_USE_LLVM_UNWINDER=OFF",
         "-DLIBCXX_ENABLE_SHARED=ON", "-DLIBCXX_ENABLE_STATIC=ON",
         "-DLIBCXXABI_ENABLE_SHARED=ON", "-DLIBCXXABI_ENABLE_STATIC=ON"], root / "libcxx-configure.log")
    run([cmake, "--build", build, "--parallel", args.jobs], root / "libcxx-build.log")
    run([cmake, "--install", build], root / "libcxx-install.log")
    probe_source, probe = root / "libcxx-probe.cpp", root / "libcxx-probe"
    probe_source.write_text(PROBE)
    run([compiler, "-std=c++26", "-mmacosx-version-min=15.0", "-isysroot", sdk,
         "-nostdinc++", "-isystem", install / "include/c++/v1", "-D_LIBCPP_ENABLE_EXPERIMENTAL",
         probe_source, "-L" + str(install / "lib"), "-Wl,-rpath," + str(install / "lib"),
         "-lc++experimental", "-lc++", "-lc++abi", "-o", probe], root / "libcxx-probe-build.log")
    run([probe], root / "libcxx-probe.log")
    result = {"llvm_version": VERSION, "compiler": str(compiler),
              "source_sha256": SOURCE_SHA256, "runtime": str(install),
              "deployment_target": "15.0", "tzdb_directory": "/usr/share/zoneinfo",
              "patch": "darwin-system-tzdb-directory-v1",
              "probe": (root / "libcxx-probe.log").read_text().strip()}
    receipt = root / "libcxx-toolchain.json"
    receipt.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
