# PKGi PSP 1.1.0-scehttp8

Base: bucanero/pkgi-psp commit ed4f25e16c71fff1913fa43410833c5b3198b6a4.

## Changes

- All initial `http://` URLs use Sony sceHttp: packages, database refreshes, and downloads into memory. Scheme dispatch is case insensitive.
- HTTPS/FTP/FTPS retain curl. No silent curl fallback for HTTP.
- Native HTTP reads request up to 100 KiB per call. HTTP module heap: 128 KiB.
- Package/ZIP downloads use an exactly 8 MiB application write cache, flushed in order and at completion. Small database and metadata downloads do not allocate this file-write cache.
- SHA-256 updates and final digest comparison removed from the download path, including curl downloads. Database checksum fields remain readable for format compatibility but are ignored by this variant. Crypto needed by package installation is unchanged.
- No resume for any download backend. Old `.resume` data is ignored and removed for the selected download. New transfers truncate the package file and start at zero; failures/cancellation remove the new partial file. Local-file installations are preserved.
- HTTP status, received length, callback errors and final file-close errors remain checked. These are transport/storage checks, not cryptographic verification.
- HTTP request/connection/template resources are deleted on failure and completion; modules are unloaded at shutdown.

## Build

Use the project's existing PSPDEV dependencies and `.github/workflows/build.yml` workflow (see actual workflow filename in the archive). The workflow supports manual dispatch and produces standard and OFW packages. The source archive itself contains NO compiled EBOOT.PBP.

Typical commands in a configured PSPDEV environment, with dbglogger installed:

```sh
mkdir build
cd build
psp-cmake .. -DCMAKE_BUILD_TYPE=Release
make -j2
make createzip
```

CMake now links `psphttp` and compiles `source/pkgi_native_http.c`.

## Validation performed

Host GCC syntax checks against current PSPSDK HTTP declarations; mock-API tests with AddressSanitizer and UndefinedBehaviorSanitizer:

- successful multi-block and empty transfers;
- incomplete/oversized response, missing Content-Length, HTTP failure;
- cancellation, read failure, short writer result;
- cleanup after every request-creation failure;
- cache spanning multiple 8 MiB boundaries, final partial flush, write failure and multiplication overflow.

Reproduce using a host C compiler and a PSPSDK source checkout:

```sh
PSPSDK_SOURCE=/absolute/path/to/pspsdk sh tests/host/run.sh
```

LeakSanitizer is disabled in the host script because it cannot run under the execution environment's ptrace. AddressSanitizer and UBSan remain enabled. These tests mock PSP runtime calls and are NOT console or full PSP build tests.

## Remaining hardware checks / limitations

The original HTTP patch was not cross-built at delivery. This CPU + ME prototype now includes successful PSP builds; see ME_PAIR_VALIDATION.md for current validation limits. No physical PSP was available for ME runtime tests.

Native HTTP enables sceHttp redirects. An HTTP URL redirecting to HTTPS is not handed to curl: native HTTPS/TLS is not initialized by this patch, so such links may fail; use the final HTTPS URL instead. Native HTTP also does not copy curl's explicit proxy configuration.

Downloads to files still require Content-Length, as upstream does. Downloads to memory may read unknown-length responses to EOF. Cancellation is checked between reads; a blocked native receive can take up to its 10-second timeout. An 8 MiB allocation failure reports an error rather than silently reducing the requested cache.

Before replacing your installed app, retain its original EBOOT.PBP. Test the same sufficiently large file from the same HTTP URL and measure wall-clock time through final file close. Also test cancellation, disconnect/retry, database refresh and a direct HTTPS download. No claim of 550 KiB/s has been verified.
