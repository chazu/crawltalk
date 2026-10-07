# Working on Crawltalk

- `files/` is the supplied working Smalltalk-80 seed; `org/` is historical input.
  Build/test scripts use disposable copies. Do not modify the seed as a side
  effect of developing or testing an extension.
- Canonical extension source is in `smalltalk/*.st`. The image's Compiler loads
  these files. Preserve complete class definitions and class-side methods.
- `src/interpreter.*`, `objmemory.*`, and `bitblt.*` retain the historical VM.
  `evaluation.cpp` implements headless evaluation; `hostbridge.cpp` owns the
  Smalltalk/native adapter; `hostservices.*` owns native tasks and threads.
- Only the Interpreter owner thread may access object memory or its scheduler.
  Native services exchange copied bytes, never OOPs or Smalltalk callbacks.
  Follow `docs/native-extensions.md` when adding primitives or native services.
- Build with CMake, then run `ctest --test-dir build --output-on-failure`.
  Tests use real image compilation, GC, snapshots and local HTTP. They must not
  depend on Internet availability or modify the seed.
- For memory/lifetime changes use the address/undefined-behavior sanitizer build.
  For worker/queue changes also run the native ThreadSanitizer test, as documented
  in `docs/modernization.md`. Headless evidence does not establish GUI keyboard
  acceptance or native execution on untested operating systems.
