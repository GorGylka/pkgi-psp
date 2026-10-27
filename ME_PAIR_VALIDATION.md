# Validation of CPU + ME pair prototype v1

## Completed

- Full ME library build, including kernel bridge and embedded PRX, with PSPDEV v20261001 / GCC 15.2.0.
- Full PKGi PSP cross-compilation, link, import fixup and EBOOT.PBP packaging for PKGI_ENABLE_ME=ON and OFF.
- Host end-to-end synthetic PKG extraction with AddressSanitizer and UndefinedBehaviorSanitizer (LeakSanitizer disabled in this ptrace environment).
- Synthetic fixture includes a >2 MiB file, multiple CPU/ME pairs, short final blocks, a 17-byte file, a large counter carry, and multiple files at distinct aligned offsets.
- Exact output comparison against known plaintext encrypted independently with Python cryptography AES-CTR.
- CPU-only and mocked-ME paths, short read/write retries, failed ME submit, failed ME completion, and failed output write.
- Failure cases return failure and remove the current partial output file; no half pair is committed after an ME error.

## Not completed / not claimed

- No physical PSP or ME runtime test, no speed result, no guarantee of ARK-5 compatibility.
- Host tests mock PSP file I/O and ME scheduling. They do not emulate Allegrex cache coherence, memory mapping, kernel privileges, clocks, reset, interrupts, or firmware callbacks.
- On-device self-test compares ME with CPU before real file contents are entrusted to ME. Kernel startup itself can still fail or hang.
- Sleep/exit interactions of the experimental external ME library need hardware validation. Normal suspend is locked while the installer owns ME. HOME issue from the prior build remains deferred.

## Reproducing host tests

From source/pkgi-psp, with gcc, OpenSSL development headers and Python cryptography installed:

    python3 tests/me/run.py

The run uses temporary output directories and independently generated fixture data. No game content is bundled.
