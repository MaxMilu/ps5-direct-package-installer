# singleDPI 0.2.2 debug-log build

This branch adds early-startup diagnostics to `singleDPI`.

The log is written to:

```text
/data/singleDPI/singleDPI-debug.log
```

Each record is written and flushed immediately. The log includes the startup
stage, kstuff RWX probing, AuthID preparation, AppInst initialization, and
9090/12800 socket creation results. The build also installs handlers for
common process-fatal signals. A panic record has this form:

```text
PANIC signal=11 code=1 errno=0 address=0x... context=0x... stage=before_appinst
```

The `address` is the fault address and `stage` is the last startup boundary
reached. `context` identifies the platform signal context for further
debugging; it is not itself a stack trace.

If the log file does not exist at all, the ELF likely failed before entering
`main()` (for example in the loader or dynamic relocation stage), or the
console could not create `/data/singleDPI`. In that case the Payload Manager
load result or system/kernel log is required; singleDPI cannot log a failure
that occurs before its code executes.

Please send the complete log, the complete 64-character ELF SHA-256, PS5
firmware, kstuff-lite version, and Payload Manager load result. Do not include
PKG URLs containing credentials.
