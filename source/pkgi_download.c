#include "pkgi_download.h"
#include "pkgi_dialog.h"
#include "pkgi.h"
#include "pkgi_utils.h"

#include <sys/stat.h>
#include <sys/file.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <mini18n.h>


static char root[256];
static char resume_file[256];

static pkgi_http* http;
static const DbItem* db_item;

static uint64_t initial_offset;  // zero for fresh downloads
static uint64_t download_offset; // pkg absolute offset
static uint64_t download_size;   // pkg total size (from http request)

static void* item_file;     // current file handle
static char item_name[256]; // current file name
static char item_path[256]; // current file path

#define DOWNLOAD_CACHE_SIZE (8u * 1024u * 1024u)
static unsigned char* download_cache;
static size_t cache_used;

static int flush_download_cache(void)
{
    if (cache_used && !pkgi_write(item_file, download_cache, cache_used))
        return 0;
    cache_used = 0;
    return 1;
}


// pkg header
static uint64_t total_size;

// UI stuff
static char dialog_extra[256];
static char dialog_eta[256];
static uint32_t info_start;
static uint32_t info_update;


static void calculate_eta(uint32_t speed)
{
    uint64_t seconds = (total_size - download_offset) / speed;
    if (seconds < 60)
    {
        pkgi_snprintf(dialog_eta, sizeof(dialog_eta), "%s: %us", _("ETA"), (uint32_t)seconds);
    }
    else if (seconds < 3600)
    {
        pkgi_snprintf(dialog_eta, sizeof(dialog_eta), "%s: %um %02us", _("ETA"), (uint32_t)(seconds / 60), (uint32_t)(seconds % 60));
    }
    else
    {
        uint32_t hours = (uint32_t)(seconds / 3600);
        uint32_t minutes = (uint32_t)((seconds - hours * 3600) / 60);
        pkgi_snprintf(dialog_eta, sizeof(dialog_eta), "%s: %uh %02um", _("ETA"), hours, minutes);
    }
}

/* follow the CURLOPT_XFERINFOFUNCTION callback definition */
static int update_progress(void *p, int64_t dltotal, int64_t dlnow, int64_t ultotal, int64_t ulnow)
{
    uint32_t info_now = pkgi_time_msec();

    if (info_now >= info_update)
    {
        char text[256];
        pkgi_snprintf(text, sizeof(text), "%s", item_name);

        {
            // report download speed
            uint32_t speed = (uint32_t)(((download_offset - initial_offset) * 1000) / ((info_now - info_start) ? (info_now - info_start) : 1));
            if (speed > 10 * 1000 * 1024)
            {
                pkgi_snprintf(dialog_extra, sizeof(dialog_extra), "%u %s/s", speed / 1024 / 1024, _("MB"));
            }
            else if (speed > 1000)
            {
                pkgi_snprintf(dialog_extra, sizeof(dialog_extra), "%u %s/s", speed / 1024, _("KB"));
            }

            if (speed != 0)
            {
                // report ETA
                calculate_eta(speed);
            }
        }

        float percent = total_size ? (float)((double)download_offset / total_size) : 0.f;

        pkgi_dialog_update_progress(text, dialog_extra, dialog_eta, percent);
        info_update = info_now + 500;
        progress_screen_refresh();
    }

    return (pkgi_dialog_is_cancelled() || pkgi_exit_requested());
}

static size_t write_download_data(void *buffer, size_t size, size_t nmemb, void *stream)
{
    if (size && nmemb > SIZE_MAX / size) return 0;
    size_t length = size * nmemb;
    size_t remaining = length;
    const unsigned char* data = buffer;
    while (remaining)
    {
        size_t count = DOWNLOAD_CACHE_SIZE - cache_used;
        if (count > remaining) count = remaining;
        memcpy(download_cache + cache_used, data, count);
        cache_used += count;
        data += count;
        remaining -= count;
        if (cache_used == DOWNLOAD_CACHE_SIZE && !flush_download_cache())
            return 0;
    }
    download_offset += length;
    return length;
}

static int download_data(void)
{
    if (!http)
    {
        LOG("requesting %s @ %llu", db_item->url, initial_offset);
        http = pkgi_http_get(db_item->url, db_item->content, initial_offset);
        if (!http)
        {
            pkgi_dialog_error(_("Could not send HTTP request"));
            return 0;
        }

        int64_t http_length;
        if (!pkgi_http_response_length(http, &http_length))
        {
            pkgi_dialog_error(_("HTTP request failed"));
            return 0;
        }
        if (http_length < 0)
        {
            pkgi_dialog_error(_("HTTP response has unknown length"));
            return 0;
        }

        download_size = http_length;
        total_size = initial_offset + download_size;

        if (!pkgi_check_free_space(http_length))
        {
            LOG("error! out of space");
            return 0;
        }

        LOG("http response length = %lld, total pkg size = %llu", http_length, total_size);
        info_start = pkgi_time_msec();
        info_update = pkgi_time_msec() + 500;
    }

    if (!pkgi_http_read(http, &write_download_data, &update_progress))
    {

        if (!pkgi_dialog_is_cancelled())
        {
            pkgi_dialog_error(_("HTTP download error"));
        }
        return 0;
    }

    return 1;
}

// this includes creating of all the parent folders necessary to actually create file
static int create_file(void)
{
    char folder[256];
    pkgi_strncpy(folder, sizeof(folder), item_path);
    char* last = pkgi_strrchr(folder, '/');
    *last = 0;

    if (!pkgi_mkdirs(folder))
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "%s %s", _("cannot create folder"), folder);
        pkgi_dialog_error(error);
        return 0;
    }

    LOG("creating %s file", item_name);
    item_file = pkgi_create(item_path);
    if (!item_file)
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "%s %s", _("cannot create file"), item_name);
        pkgi_dialog_error(error);
        return 0;
    }

    return 1;
}

static int download_pkg_file(void)
{
    int result = 0;

    pkgi_strncpy(item_name, sizeof(item_name), root);
    pkgi_snprintf(item_path, sizeof(item_path), "%s%s/%s", pkgi_get_storage_device(), pkgi_get_temp_folder(), root);
    LOG("downloading %s", item_name);

    download_cache = malloc(DOWNLOAD_CACHE_SIZE);
    cache_used = 0;
    if (!download_cache)
    {
        pkgi_dialog_error("Cannot allocate 8 MiB download cache");
        return 0;
    }
    if (!create_file()) goto bail;
    if (!download_data()) goto bail;
    if (download_offset != total_size)
    {
        pkgi_dialog_error("Incomplete download");
        goto bail;
    }
    if (!flush_download_cache())
    {
        pkgi_dialog_error("Memory Stick write error");
        goto bail;
    }

    LOG("%s downloaded", item_path);
    result = 1;

bail:
    if (item_file != NULL)
    {
        // Include the final stdio flush in success/failure handling.
        if (fclose((FILE*)item_file) != 0)
        {
            pkgi_dialog_error("Memory Stick write error");
            result = 0;
        }
        item_file = NULL;
    }
    free(download_cache);
    download_cache = NULL;
    cache_used = 0;
    if (!result) pkgi_rm(item_path);
    return result;
}

static int create_rap(const char* contentid, const uint8_t* rap)
{
    LOG("creating %s.rap", contentid);
    pkgi_dialog_update_progress(_("Creating RAP file"), NULL, NULL, 1.f);

    char path[256];
    pkgi_snprintf(path, sizeof(path), "%s%s/%s.rap", pkgi_get_storage_device(), PKGI_RAP_FOLDER, contentid);

    if (!pkgi_save(path, rap, PKGI_RAP_SIZE))
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "%s %s.rap", _("Cannot save"), contentid);
        pkgi_dialog_error(error);
        return 0;
    }

    LOG("RAP file created");
    return 1;
}

static int is_zip(const char* filename)
{
    const char* extension = pkgi_strrchr(filename, '.');
    return extension && (pkgi_stricmp(extension, ".zip") == 0);
}

int pkgi_download(const DbItem* item)
{
    int result = 0;

    pkgi_snprintf(root, sizeof(root), "%s.%s", item->content, is_zip(item->url) ? "zip" : "pkg");
    LOG("package installation file: %s", root);

    pkgi_snprintf(resume_file, sizeof(resume_file), "%s%s/%s.resume", pkgi_get_storage_device(), pkgi_get_temp_folder(), item->content);
    if (item->type == ContentLocal)
    {
        pkgi_strncpy(root, sizeof(root), item->url);
        pkgi_snprintf(item_path, sizeof(item_path), "%s%s/%s", pkgi_get_storage_device(), pkgi_get_temp_folder(), root);
        return 1;
    }
    // Old builds may have left resume metadata; this build always starts at zero.
    pkgi_rm(resume_file);
    pkgi_dialog_set_progress_title(_("Downloading..."));

    http = NULL;
    item_file = NULL;
    total_size = 0;
    download_size = 0;
    download_offset = 0;
    initial_offset = 0;
    db_item = item;

    dialog_extra[0] = 0;
    dialog_eta[0] = 0;
    info_start = pkgi_time_msec();
    info_update = info_start + 1000;

    if (item->rap)
    {
        if (!create_rap(item->content, item->rap)) goto finish;
    }

    if (!download_pkg_file()) goto finish;

    pkgi_rm(resume_file);
    result = 1;

finish:
    if (http)
    {
        pkgi_http_close(http);
    }

    return result;
}

void update_install_progress(const char *filename, int64_t progress)
{
    download_offset = progress;
    if (filename) pkgi_strncpy(item_name, sizeof(item_name), filename);
    update_progress(NULL, 0, 0, 0, 0);
}

int pkgi_install(int iso_mode, int remove_pkg)
{
    int result;

    download_size = pkgi_get_size(item_path);
    info_start = pkgi_time_msec();

    initial_offset = 0;
    download_offset = 0;
    total_size = download_size;

    // check if it's a zip file
    if (is_zip(item_path))
        result = extract_zip(item_path);
    else
        result = iso_mode ? convert_psp_pkg_iso(item_path, (iso_mode == 2)) : install_psp_pkg(item_path);

    if (result && remove_pkg)
    {
        pkgi_rm(item_path);
    }

    return (result);
}
