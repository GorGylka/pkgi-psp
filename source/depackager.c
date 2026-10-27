/*
* based on depackager by qwikrazor87
*/

#include <pspkernel.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <mbedtls/aes.h>

#include "pkgi.h"
#include "pkgi_download.h"
#include "pkgi_pair_crypto.h"
#include "pkgi_install_profile.h"

#define DEPACKAGER_VER 3

/*
typedef struct {
	u32 magic;
	u16 pkg_revision;
	u16 pkg_type;
	u32 pkg_metadata_offset;
	u32 pkg_metadata_count;
	u32 pkg_metadata_size;
	u32 item_count;
	u64 total_size;
	u64 data_offset;
	u64 data_size;
	u8 contentid[0x30];
	u8 digest[0x10];
	u8 pkg_data_riv[0x10];
	u8 pkg_header_digest[0x40];
} PKG_HEADER;

typedef struct {
	u32 magic;                             // 0x7F657874 (".ext")
	u32 unknown_1;                         // Maybe version. Always 1.
	u32 ext_hdr_size;                      // Extended header size. ex: 0x40
	u32 ext_data_size;                     // ex: 0x180
	u32 main_and_ext_headers_hmac_offset;  // ex: 0x100
	u32 metadata_header_hmac_offset;       // ex: 0x360, 0x390, 0x490 
	u64 tail_offset;                       // Tail size seems to be always 0x1A0
	u32 padding1;
	u32 pkg_key_id;                        // Id of the AES key used for decryption. PSP = 0x1, PS Vita = 0xC0000002, PSM = 0xC0000004
	u32 full_header_hmac_offset;           // ex: none (old pkg): 0, 0x930
	u8 padding2[0x14];
} PKG_EXT_HEADER;
*/


static u8 static_public_key[16];

static const u8 PSPAESKey[16] __attribute__((aligned(16))) = {
	0x07, 0xF2, 0xC6, 0x82, 0x90, 0xB5, 0x0D, 0x2C, 0x33, 0x81, 0x8D, 0x70, 0x9B, 0x60, 0xE6, 0x2B
};


// The key schedule is prepared once per installation, not once per block.
static void AES128_ECB_encrypt(mbedtls_aes_context* ctx, const uint8_t* input, uint8_t* output)
{
    mbedtls_aes_crypt_ecb(ctx, MBEDTLS_AES_ENCRYPT, input, output);
}

static u32 toU32(char *_buf)
{
	u8 *buf = (u8 *)_buf;
	u32 b1 = buf[0] << 24;
	u32 b2 = buf[1] << 16;
	u32 b3 = buf[2] << 8;
	u32 size = b1 | b2 | b3 | buf[3];

	return size;
}

static int is_pkg_supported(const char *file)
{
	char buf[16];

	SceUID fd = sceIoOpen(file, PSP_O_RDONLY, 0777);
	if (fd >= 0) {
		sceIoRead(fd, buf, 4);
		sceIoClose(fd);

		return memcmp(buf, "\x7FPKG", 4) == 0;
	} else
		return 0;
}

static int is_pkg_type_supported(const char *file)
{
	char buf[16];

	SceUID fd = sceIoOpen(file, PSP_O_RDONLY, 0777);
	if (fd >= 0) {
		sceIoRead(fd, buf, 8);
		sceIoClose(fd);

		return memcmp(buf, "\x7FPKG\x80\x00\x00\x02", 8) == 0;
	} else
		return 0;
}

static void view_pkg_info(const char *file)
{
	char buf[256];

	LOG("PKG info viewer");

	SceUID fd = sceIoOpen(file, PSP_O_RDONLY, 0777);
	sceIoRead(fd, buf, sizeof(buf));
	sceIoClose(fd);

	u8 version = (u8)buf[4];

	LOG("PKG type:       %s", buf[7] == 1 ? "PS3 (currently unsupported)" : buf[7] == 2 ? "PSP" : "unknown");
	LOG("PKG version:    %s", (version == 0x80) ? "retail" : (version == 0x90) ? "debug (currently unsupported)" : "unknown");
	LOG("Content ID:     %s", buf + 0x30);
	LOG("PKG file count: %d", toU32(buf + 0x14));
	LOG("PKG size:       %d bytes", toU32(buf + 0x1C));
	LOG("Encrypted size: %d bytes", toU32(buf + 0x2C));
}

static void xor128(u8 *dst, u8 *xor1, u8 *xor2)
{
	int i;

	for (i = 0; i < 16; i++)
		dst[i] = xor1[i] ^ xor2[i];
}

static void iter128(u8 *buf)
{
	int i;

	for (i = 15; i >= 0; i--) {
		buf[i]++;

		if (buf[i])
			break;
	}
}

static void setiter128(u8 *dst, u32 blocks)
{
    memcpy(dst, static_public_key, 16);
    // Add the block offset to the big-endian 128-bit counter, modulo 2^128.
    // Include carry propagation through all 16 bytes.
    for (int i = 15; i >= 0 && blocks; --i) {
        u32 sum = (u32)dst[i] + (blocks & 0xffu);
        dst[i] = (u8)sum;
        blocks = (blocks >> 8) + (sum >> 8);
    }
}

static int read_exact(SceUID fd, void* buffer, unsigned size)
{
    unsigned done = 0;
    while (done < size) {
        int n = sceIoRead(fd, (char*)buffer + done, size - done);
        if (n <= 0 || (unsigned)n > size - done) return 0;
        done += n;
    }
    return 1;
}

static int write_exact(SceUID fd, const void* buffer, unsigned size)
{
    unsigned done = 0;
    while (done < size) {
        int n = sceIoWrite(fd, (const char*)buffer + done, size - done);
        if (n <= 0 || (unsigned)n > size - done) return 0;
        done += n;
    }
    return 1;
}

int install_psp_pkg(const char *file)
{
	char *tmpBuf;
	char gameid[10];
	char pkgBuf[256];
	u8 public_key[16], xor_key[16];
	SceOff progress = 0;

	LOG("PSP depackager v%d", DEPACKAGER_VER);
	LOG("PSP PKG installer");

	if(is_pkg_supported(file))
		view_pkg_info(file);

	if (!is_pkg_type_supported(file)) {
		LOG("Unsupported PKG type detected.");
		return 0;
	}

	SceUID fd = sceIoOpen(file, PSP_O_RDONLY, 0777);
	SceSize fdsize = sceIoLseek(fd, 0, PSP_SEEK_END);
	sceIoLseek(fd, 0, PSP_SEEK_SET);
	sceIoRead(fd, pkgBuf, 256);

	if (toU32(pkgBuf + 0x1C) != fdsize) {
		sceIoClose(fd);
		LOG("Corrupt PKG detected");
		LOG("detected size: %d bytes", fdsize);
		LOG("expected size: %d bytes", toU32(pkgBuf + 0x1C));
		return 0;
	}

	sceIoLseek(fd, toU32(pkgBuf + 8) + 0x14, PSP_SEEK_SET);
	sceIoRead(fd, gameid, 4);

	if (toU32(gameid) == 9) {
		sceIoClose(fd);
		LOG("Theme PKG detected");
		return (convert_psp_pkg_iso(file, 0));
	}

	//1 MiB buffer
	tmpBuf = (char *)malloc(1024 * 1024);
	if (!tmpBuf) {
		LOG("Error allocating memory: 0x%08X", 1024 * 1024);
        sceIoClose(fd);
		return 0;
	}

    mbedtls_aes_context aes_ctx;
    mbedtls_aes_init(&aes_ctx);
    if (mbedtls_aes_setkey_enc(&aes_ctx, PSPAESKey, 128) != 0) {
        mbedtls_aes_free(&aes_ctx);
        free(tmpBuf);
        sceIoClose(fd);
        return 0;
    }

	sceIoLseek(fd, toU32(pkgBuf + 8) + 0x48, PSP_SEEK_SET);
	sceIoRead(fd, gameid, 9);
	gameid[9] = 0;

	memcpy(public_key, pkgBuf + 0x70, 16);
	memcpy(static_public_key, pkgBuf + 0x70, 16);

	u32 enc_start = toU32(pkgBuf + 0x24);
	u32 files = toU32(pkgBuf + 0x14);

	sceIoLseek(fd, enc_start, PSP_SEEK_SET);
	sceIoRead(fd, tmpBuf, files * 32);

	int i, j, pspcount = 0;
	u32 file_name[files], file_name_len[files], file_offset[files], file_size[files], is_file[files];

	for (i = 0; i < (int)(files * 2); i++) {
		AES128_ECB_encrypt(&aes_ctx, public_key, xor_key);
		xor128((u8 *)(tmpBuf + (i * 16)), (u8 *)(tmpBuf + (i * 16)), xor_key);
		iter128(public_key);
	}

	for (i = 0; i < (int)files; i++) {
		if (((u8 *)tmpBuf)[(i * 32) + 24] == 0x90) {
			file_name[pspcount] = toU32(tmpBuf + i * 32);
			file_name_len[pspcount] = toU32(tmpBuf + i * 32 + 4);
			file_offset[pspcount] = toU32(tmpBuf + i * 32 + 12);
			file_size[pspcount] = toU32(tmpBuf + i * 32 + 20);
			is_file[pspcount] = ((tmpBuf[i * 32 + 27] != 4) && file_size[pspcount]);
			pspcount++;
		}
	}

	int files_extracted = 0;
    int install_ok = 1;
    pkgi_profile_begin();
    uint64_t tick = pkgi_profile_now();
    int use_me = pkgi_me_begin(PSPAESKey);
    pkgi_profile_add(PROF_START, tick);

	for (i = 0; i < pspcount; i++) {
        if (pkgi_exit_requested()) { install_ok = 0; goto install_cleanup; }
		sceIoLseek(fd, enc_start + file_name[i], PSP_SEEK_SET);
		int namesize = (file_name_len[i] + 15) & -16;
		sceIoRead(fd, tmpBuf, namesize);
		memcpy(public_key, pkgBuf + 0x70, 16);

		setiter128(public_key, file_name[i] >> 4);

		for (j = 0; j < (namesize >> 4); j++) {
			AES128_ECB_encrypt(&aes_ctx, public_key, xor_key);
			xor128((u8 *)(tmpBuf + (j * 16)), (u8 *)(tmpBuf + (j * 16)), xor_key);
			iter128(public_key);
		}

		char path[256];
		snprintf(path, sizeof(path), "%s%s/%s/%s", pkgi_get_storage_device(), PKGI_INSTALL_FOLDER, gameid, tmpBuf + 15);
		char* slash = strrchr(path, '/');
		*slash = 0;
		pkgi_mkdirs(path);
		*slash = '/';

		if (is_file[i]) {
			LOG("Currently extracting: %s", path);
			files_extracted++;

            uint64_t source_offset = (uint64_t)enc_start + file_offset[i];
            progress = sceIoLseek(fd, source_offset, PSP_SEEK_SET);
            if (progress != (SceOff)source_offset) { install_ok = 0; goto install_cleanup; }
            update_install_progress(path + 14, progress);
            SceUID dstfd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
            if (dstfd < 0) { install_ok = 0; goto install_cleanup; }
            uint32_t done = 0;
            while (done < file_size[i]) {
                if (pkgi_exit_requested()) { install_ok = 0; break; }
                uint32_t size = file_size[i] - done;
                if (size > 2u * PKGI_ME_BLOCK_SIZE) size = 2u * PKGI_ME_BLOCK_SIZE;
                tick = pkgi_profile_now();
                int stage_ok = read_exact(fd, tmpBuf, size);
                pkgi_profile_add(PROF_READ, tick);
                if (stage_ok) {
                    tick = pkgi_profile_now();
                    stage_ok = pkgi_decrypt_pair(&aes_ctx, static_public_key,
                        ((uint64_t)file_offset[i] + done) / 16,
                        (uint8_t*)tmpBuf, size, use_me);
                    pkgi_profile_add(PROF_DECRYPT, tick);
                }
                if (stage_ok) {
                    tick = pkgi_profile_now();
                    stage_ok = write_exact(dstfd, tmpBuf, size);
                    pkgi_profile_add(PROF_WRITE, tick);
                }
                if (!stage_ok) { install_ok = 0; break; }
                pkgi_profile_bytes(size);
                done += size;
                tick = pkgi_profile_now();
                update_install_progress(NULL, progress + done);
                pkgi_profile_add(PROF_UI, tick);
            }
            if (pkgi_exit_requested()) install_ok = 0;
            if (sceIoClose(dstfd) < 0) install_ok = 0;
            if (!install_ok) {
                sceIoRemove(path);
                goto install_cleanup;
            }
		}
	}

install_cleanup:
    tick = pkgi_profile_now();
    pkgi_me_end();
    pkgi_profile_add(PROF_STOP, tick);
    if (install_ok) update_install_progress(file + 9, fdsize);
	sceIoClose(fd);
	free(tmpBuf);
    mbedtls_aes_free(&aes_ctx);

	LOG("files extracted: %d", files_extracted);
	LOG("Installation complete");

    pkgi_profile_finish(install_ok, use_me);
	return install_ok;
}
