# Windows LPAC build process investigation

Investigation date: 2026-10-21. This directory contains standalone diagnostic
code, not a product backend or an XMake target. The probe stages only its own
static-runtime executable and does not change original directory/file ACLs.

## Verified result

On this Windows host, a zero-capability LPAC process cannot create a descendant
with ordinary `CreateProcessW`: it returns `ERROR_ACCESS_DENIED` (5), even with
the Job active-process limit raised to 16. Adding exactly the named Windows
capability **`registryRead`** makes that identical call succeed. The descendant
retains the same unique AppContainer SID, low integrity (4096), LPAC access-check
semantics (`granted=2`), and one capability. No process-launch broker, process
ACL changes, token DACL changes, or special descendant attributes were required.

The capability SID returned by Windows on this host is:

```text
S-1-15-3-1024-1065365936-1281604716-3511738428-1654721687-432734479-3232135806-4053264122-3456934681
```

This establishes native descendant-launch feasibility. It does not itself prove
that all XMake/compiler/toolchain dependencies have been staged or that a complete
build succeeds; those require the product build-backend integration tests.

## Reproduction

Run from this directory in PowerShell:

```powershell
cmd /c compile_probe.cmd
.\lpac_probe.exe lpac noinherit plain
.\lpac_probe.exe lpac noinherit plain none registryRead
.\lpac_probe.exe lpac noinherit breakaway none registryRead
```

The first command returns a failing inner process exit of 5; the second returns
0. Captured outputs are `zero-capabilities-results.txt` and
`registry-read-results.txt`. The build script uses the locally installed
VS 2022 Build Tools headers/libraries and LLVM `clang-cl`, with `/MT`; it does not
run or change the shared project build.

With `registryRead`, the probe additionally demonstrated:

| Operation | Observed result |
| --- | --- |
| Host connects to its live loopback listener (control) | Success |
| LPAC reads host private file | Access denied 5 |
| LPAC writes host private file | Access denied 5 |
| LPAC opens exact host private `HKEY_USERS\\<owner>\\Software\\...` key for read | Access denied 5 |
| LPAC opens that key for write | Access denied 5 |
| LPAC opens `\\?\GLOBALROOT\Device\Afd` | Access denied 5 |
| LPAC initializes Winsock | Success |
| LPAC creates TCP socket | WSAEACCES 10013; loopback connection cannot be attempted |
| LPAC creates ordinary child | Success; same LPAC token/SID/capability/low integrity |
| LPAC requests `CREATE_BREAKAWAY_FROM_JOB` | Access denied 5 |

The private registry key and private test file are newly created probe objects.
The process does not read real credentials. `registryRead` does not imply that
all host registry reads are denied: keys with the corresponding capability ACL
are intentionally readable. The tested private key has only owner/SYSTEM access.

## Disproved alternatives

The following individual changes did **not** fix zero-capability LPAC creation:

- Turning standard-handle inheritance off (the minimal baseline already does so).
- Explicit child `ALL_APPLICATION_PACKAGES_OPT_OUT` attribute.
- Explicit child `SECURITY_CAPABILITIES` attribute, alone or together with LPAC.
- Granting the initial process/thread objects a private unique-SID ACL and low label.
- Granting the child process/thread objects those explicit ACLs.
- Changing the child token object DACL or `TokenDefaultDacl` before resumption.
- `CreateProcessAsUserW` with the same restricted token.
- Adding only `lpacAppExperience`.

The LPAC process could already open its staged executable, `kernelbase.dll`,
`apphelp.dll`, and `ntdll.dll` with read/execute access. An ordinary AppContainer
control could launch a child, but its weaker access-check semantics (`granted=3`)
make it unsuitable as a substitute for LPAC.

## Product implementation guidance

1. Preserve the existing zero-capability, one-process sandbox profile.
2. For the separate workspace-build profile, derive only `registryRead` using
   `DeriveCapabilitySidsFromName`. The probe resolves this function dynamically
   from the already loaded `kernelbase.dll`; fail closed if unavailable.
3. Keep the derived SID memory alive through process creation. Copy the returned
   capability SID into `SID_AND_ATTRIBUTES` with `SE_GROUP_ENABLED`. Free each
   returned SID and the two returned arrays using `LocalFree` after ownership is
   no longer needed. Do not include the capability-group SID array as capabilities.
4. Verify the suspended initial token contains exactly this single capability,
   the intended unique AppContainer SID, low integrity, and LPAC AccessCheck
   semantics. Verify atomic assignment to the intended Job as before.
5. Raise the build Job's process limit to a bounded value, retaining kill-on-close
   and prohibiting breakaway. Ordinary descendants inherit LPAC and the capability.
6. Stage toolchain files separately with read/execute-only ACLs; stage the source
   copy and output/cache/temp directories privately. Never grant access on the
   original workspace or installed toolchain paths.
7. Use a minimal synthetic environment and an explicit inherited-handle allowlist.
   Do not inherit host sockets, open host files, registry handles, or credentials.
8. Keep explicit private-file, private-registry, AFD/socket, Job-breakaway, child
   token, cleanup, and actual XMake compile/link coverage in product tests.

## Source evidence

Chromium's Windows sandbox policy unconditionally adds `kRegistryRead` to its
AppContainer configuration. The corresponding named constant is `registryRead`.
These are corroborating implementation references; the security conclusions
above come from the local executable probe.

- https://github.com/chromium/chromium/blob/main/sandbox/policy/win/sandbox_win.cc
- https://github.com/chromium/chromium/blob/main/sandbox/policy/win/lpac_capability.h
- https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-derivecapabilitysidsfromname

Snapshots fetched on 2026-10-21:

| Local snapshot | SHA-256 |
| --- | --- |
| `references/chromium-sandbox_win.cc.txt` | `3B8720F6CF3722431AB0B30034E5F48A1D961E418429416132FA863C641F1E42` |
| `references/chromium-lpac_capability.h.txt` | `B5A5DAAC8C2AB607A3718240D9ED5FED1C072EA112CBB9A7978FEEE331AF410E` |

`main` URLs can change; the local snapshots retain the examined bytes.
