# Zevvoryn Plugin System

This guide documents the C++ C ABI, optional Luau VM, and optional Java/JNI
runtimes. This documentation-only publication does not include their runtime
source code. Runtime packages belong beside the installed server or in an
isolated test build, not in the source repository root.

> **Trust boundary:** native C++ plugins and Java plugins execute code inside
> the server process. Permissions restrict only capabilities exposed by the
> Zevvoryn API; they do not sandbox arbitrary native or Java code. Use only
> trusted C++ modules and JARs. Luau is the sandboxed scripting runtime.

## Runtime package layout

Place each plugin in its runtime-specific package directory:

```text
plugins/
  cpp/<plugin-id>/plugin.json + native library (.dll/.so)
  luau/<plugin-id>/plugin.json + entry script (.luau)
  jvm/<plugin-id>/plugin.json + JAR
  permissions.json
  data/<plugin-id>/                 # private Luau VFS root
```

These are runtime files beside a deployed server, not files to create in the
repository root. Build outputs and fixtures belong in an out-of-source build
directory.

## Native C++ plugin

The native boundary is a versioned C ABI: opaque handles, fixed-width C types,
and function pointers only. Do not pass STL types, C++ classes, exceptions, or
allocator-owned objects across the ABI. The entry symbol is
`zevvoryn_plugin_init`; export it with `ZEVVORYN_PLUGIN_EXPORT`. Windows uses
`LoadLibraryW`/`GetProcAddress`; Linux uses `dlopen`/`dlsym`.

A minimal entry point looks like this:

```cpp
#include "plugin_api.hpp"

namespace {
int32_t onLoad(void*) { return 1; }
void onTick(void*, double) {}
void onDisable(void*) {}
}

extern "C" ZEVVORYN_PLUGIN_EXPORT int32_t zevvoryn_plugin_init(
    const ZevvorynHostApiV1* host, ZevvorynPluginApiV1* plugin) {
    if (!host || !plugin ||
        host->abi_version != ZEVVORYN_PLUGIN_ABI_VERSION ||
        host->struct_size < sizeof(ZevvorynHostApiV1)) return 0;

    plugin->abi_version = ZEVVORYN_PLUGIN_ABI_VERSION;
    plugin->name = "hello-native"; // Must match plugin.json
    plugin->version = "1.0.0";     // Must match plugin.json
    plugin->on_load = &onLoad;
    plugin->on_tick = &onTick;
    plugin->on_disable = &onDisable;
    return 1;
}
```

Compile a Linux module with a C++20 compiler (adapt paths and output location):

```sh
c++ -std=c++20 -fPIC -shared -fvisibility=hidden -Isrc hello.cpp \
  -o /path/to/server/plugins/cpp/hello-native/hello_plugin.so
```

For Windows, use MSVC `/LD`, include the plugin API header, and name the output
`hello_plugin.dll`. Keep the generated package outside the source tree. A full
example should cover commands, event filters, generation-safe entity handles,
world reads, and background tasks.

The lifecycle is `on_load` → `on_enable` → main-thread `on_tick` and event
callbacks → `on_disable` on reload/shutdown → `on_unload` at final teardown.
Native libraries remain mapped because the command registry may retain callback
pointers. Reload disables and re-enables native callbacks without adding duplicate
subscriptions.

## Optional Luau runtime

Enable Luau at configure time with `-DZEVVORYN_ENABLE_LUAU=ON`; it is off by
default and can also be disabled in server settings. Each plugin gets its own
VM. Host-dangerous libraries and file loaders (`os`, `io`, `debug`,
`package`, `require`) are removed. A script entry returns a hook table, such
as `onPlayerBlockBreak`; event data is passed as a compact view.

The sandbox has an instruction/time watchdog and a per-VM memory limit.
VFS `readFile` / `writeFile` are exposed only when their permissions are
approved and are confined to `plugins/data/<plugin-id>/`. Paths must be
relative, normalized, and remain under that plugin's root, including after
symlink resolution. These controls apply to Luau scripts, not native code.

## Optional Java/JNI runtime

Enable JNI at configure time with `-DZEVVORYN_ENABLE_JVM=ON` (JDK/JNI required);
it is disabled by default. The build creates the plugin API JAR and an example.
Implement `dev.zevvoryn.api.ZevvorynPlugin`, compile against
`plugins/jvm/api/zevvoryn-plugin-api.jar`, and place the JAR plus manifest in
`plugins/jvm/<plugin-id>/`. `PluginContext` exposes only the documented host
methods, but the Java process is **not** sandboxed.

The optional block-break hook is registered from `onLoad` with
`context.subscribePlayerBlockBreak()`; it requires approved `blocks.read`.
World access is main-thread-only. Event cancellation requires `blocks.modify`.

## Manifest and administrator permissions

Example `plugin.json`:

```json
{
  "schema_version": 1,
  "name": "hello-native",
  "version": "1.0.0",
  "runtime": "cpp",
  "entry": "hello_plugin",
  "api": { "min": 1, "max": 1 },
  "permissions": ["commands.register", "blocks.read"]
}
```

`entry` is a relative library/script/JAR path (the native loader resolves the
platform library extension). For Java, also specify `main_class`. The loader
checks the manifest schema, runtime, API range, entry path, and descriptor
name/version.

The manifest requests permissions; it does not grant them. Administrators grant
the intersection of requested permissions and `plugins/permissions.json`:

```json
{
  "hello-native": ["commands.register", "blocks.read"],
  "moderation": ["blocks.read", "blocks.modify"]
}
```

No approval file means no permissions are granted. Command registration requires
`commands.register`; block-break subscription requires `blocks.read`, while
cancellation also requires `blocks.modify`. Entity handles use an ID plus
generation to reject stale entities. World access is main-thread-only; worker
tasks must not read or mutate the world and must return results through the
main-thread completion callback. Chat and network permissions are declarations
only until corresponding host capabilities are implemented.

## Events, reload, and diagnostics

Events are dispatched by numeric type ID with priorities
`LOWEST` → `LOW` → `NORMAL` → `HIGH` → `HIGHEST` → `MONITOR`.
Filters run before the plugin callback; cancellable events expose their cancel
state. The plugin manager prints an `ENABLED` or `FAILED` line per candidate,
including the plugin name and failure reason. `/plugins` lists statuses;
`/reload` performs a soft server reload; `/stop` cleanly stops the server.
There is no `/reboot` command.

A crash context records the currently executing plugin. This can identify the
source of a crash, but it cannot recover from native `segfault`/`abort` or
contain arbitrary native code. Hard isolation requires a separate process.

## Java dependency security and Log4Shell

Java permissions are not a Java sandbox. A plugin may use file I/O, sockets,
reflection, threads, JNI, or native libraries directly. Run untrusted plugins
in a separate OS account/container/VM with restricted filesystem, network, CPU,
and memory access.

Log4Shell is a vulnerability in affected Apache Log4j 2 versions involving
JNDI lookups; it is not a JVM permission or a Zevvoryn runtime vulnerability.
Review the full transitive dependency tree/SBOM for every plugin, use supported
patched releases, and do not log untrusted data through unsafe lookup or
configuration paths. Zevvoryn does not bundle Log4j and cannot repair a
vulnerable dependency packaged inside a plugin.

References: [JDK 24 Security Manager status](https://docs.oracle.com/en/java/javase/24/security/security-manager-is-permanently-disabled.html),
[Apache Logging Services security advisories](https://logging.apache.org/security.html).
