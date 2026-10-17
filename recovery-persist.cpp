/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Strictly to deal with reboot into system after OTA after /data
// mounts to pull the last pmsg file data and place it
// into /data/misc/recovery/ directory, rotating it in.
//
// Usage: recovery-persist [--force-persist]
//
//    On systems without /cache mount, all file content representing in the
//    recovery/ directory stored in /sys/fs/pstore/pmsg-ramoops-0 in logger
//    format that reside in the LOG_ID_SYSTEM buffer at ANDROID_LOG_INFO
//    priority or higher is transfered to the /data/misc/recovery/ directory.
//    The content is matched and rotated in as need be.
//
//    --force-persist  ignore /cache mount, always rotate in the contents.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <string>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/unique_fd.h>
#include <private/android_logger.h> /* private pmsg functions */

#include "recovery_utils/logging.h"
#include "recovery_utils/parse_install_logs.h"

constexpr const char* LAST_LOG_FILE = "/data/misc/recovery/last_log";
constexpr const char* LAST_PMSG_FILE = "/sys/fs/pstore/pmsg-ramoops-0";
constexpr const char* LAST_KMSG_FILE = "/data/misc/recovery/last_kmsg";
constexpr const char* LAST_CONSOLE_FILE = "/sys/fs/pstore/console-ramoops-0";
constexpr const char* ALT_LAST_CONSOLE_FILE = "/sys/fs/pstore/console-ramoops";
constexpr const char* BLK_LAST_PMSG_FILE = "/sys/fs/pstore/pmsg-pstore_blk-0";
constexpr const char* BLK_LAST_CONSOLE_FILE = "/sys/fs/pstore/console-pstore_blk-0";

struct PstoreFiles {
    const char* pmsg = nullptr;
    const char* console = nullptr;
    bool error = false;
    const char* backend = nullptr;
};

// Missing records are normal; permission and I/O errors must not choose a
// different backend and silently mix histories.
static int pstore_file_state(const char* path) {
    if (access(path, R_OK) == 0) return 1;
    if (errno == ENOENT) return 0;
    PLOG(ERROR) << "Can't read " << path;
    return -1;
}

static PstoreFiles wait_for_pstore(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        int pmsg = pstore_file_state(LAST_PMSG_FILE);
        int console = pstore_file_state(LAST_CONSOLE_FILE);
        int alternate = pstore_file_state(ALT_LAST_CONSOLE_FILE);
        if (pmsg < 0 || console < 0 || alternate < 0) return {nullptr, nullptr, true};
        // Ramoops keeps its existing preference, including console-only boots.
        if (pmsg || console || alternate) {
            return {pmsg ? LAST_PMSG_FILE : nullptr,
                    console ? LAST_CONSOLE_FILE : (alternate ? ALT_LAST_CONSOLE_FILE : nullptr),
                    false, "ramoops"};
        }
        pmsg = pstore_file_state(BLK_LAST_PMSG_FILE);
        console = pstore_file_state(BLK_LAST_CONSOLE_FILE);
        if (pmsg < 0 || console < 0) return {nullptr, nullptr, true};
        auto now = std::chrono::steady_clock::now();
        // Registration can create the records one at a time. Give an incomplete
        // pair the remaining budget, then retain whichever record exists.
        if ((pmsg && console) || now >= deadline) {
            return {pmsg ? BLK_LAST_PMSG_FILE : nullptr,
                    console ? BLK_LAST_CONSOLE_FILE : nullptr, false, "pstore_blk"};
        }
        std::this_thread::sleep_for(std::min(deadline - now,
                std::chrono::steady_clock::duration(std::chrono::milliseconds(100))));
    }
}

// close a file, log an error if the error indicator is set
static void check_and_fclose(FILE *fp, const char *name) {
    fflush(fp);
    if (ferror(fp)) {
        PLOG(ERROR) << "Error in " << name;
    }
    fclose(fp);
}

static void copy_file(const char* source, const char* destination) {
  FILE* source_fp = fopen(source, "re");
  if (source_fp == nullptr) {
    PLOG(ERROR) << "Can't open " << source;
    return;
  }
  FILE* dest_fp = fopen(destination, "we");
  if (dest_fp == nullptr) {
    PLOG(ERROR) << "Can't open " << destination;
  } else {
    char buf[4096];
    size_t bytes;
    while ((bytes = fread(buf, 1, sizeof(buf), source_fp)) != 0) {
      fwrite(buf, 1, bytes, dest_fp);
    }
    check_and_fclose(dest_fp, destination);
  }
  check_and_fclose(source_fp, source);
}

static bool file_exists(const char* filename) {
  return access(filename, R_OK) == 0;
}

static bool rotated = false;

ssize_t logsave(
        log_id_t /* logId */,
        char /* prio */,
        const char *filename,
        const char *buf, size_t len,
        void * /* arg */) {

    std::string destination("/data/misc/");
    destination += filename;

    std::string buffer(buf, len);

    {
        std::string content;
        android::base::ReadFileToString(destination, &content);

        if (buffer.compare(content) == 0) {
            return len;
        }
    }

    // ToDo: Any others that match? Are we pulling in multiple
    // already-rotated files? Algorithm thus far is KISS: one file,
    // one rotation allowed.

    rotate_logs(LAST_LOG_FILE, LAST_KMSG_FILE);
    rotated = true;

    return android::base::WriteStringToFile(buffer, destination.c_str());
}

size_t file_size(const char* path) {
    struct stat st {};
    if (stat(path, &st) < 0) {
        return 0;
    }

    return st.st_size;
}

bool compare_file(const char* file1, const char* file2) {
    if (!file_exists(file1) || !file_exists(file2)) {
        return false;
    }
    if (file_size(file1) != file_size(file2)) {
        return false;
    }
    std::array<uint8_t, 1024 * 16> buf1{};
    std::array<uint8_t, 1024 * 16> buf2{};
    android::base::unique_fd fd1(open(file1, O_RDONLY));
    android::base::unique_fd fd2(open(file2, O_RDONLY));
    if (fd1 < 0 || fd2 < 0) {
        PLOG(ERROR) << "Failed to open files for comparison";
        return false;
    }
    auto bytes_remain = file_size(file1);
    while (bytes_remain > 0) {
        const auto bytes_to_read = std::min<size_t>(bytes_remain, buf1.size());

        if (!android::base::ReadFully(fd1, buf1.data(), bytes_to_read)) {
            LOG(ERROR) << "Failed to read from " << file1;
            return false;
        }
        if (!android::base::ReadFully(fd2, buf2.data(), bytes_to_read)) {
            LOG(ERROR) << "Failed to read from " << file2;
            return false;
        }
        if (memcmp(buf1.data(), buf2.data(), bytes_to_read) != 0) {
            return false;
        }
        bytes_remain -= bytes_to_read;
    }
    return true;
}

void rotate_last_kmsg(const char* console) {
    if (rotated || console == nullptr) return;
    if (!compare_file(LAST_KMSG_FILE, console)) {
        rotate_logs(LAST_LOG_FILE, LAST_KMSG_FILE);
        rotated = true;
    }
}

int main(int argc, char **argv) {

    /* Is /cache a mount?, we have been delivered where we are not wanted */
    /*
     * Following code halves the size of the executable as compared to:
     *
     *    load_volume_table();
     *    has_cache = volume_for_path(CACHE_ROOT) != nullptr;
     */
    bool has_cache = false;
    static const char mounts_file[] = "/proc/mounts";
    FILE* fp = fopen(mounts_file, "re");
    if (!fp) {
        PLOG(ERROR) << "failed to open " << mounts_file;
    } else {
        char *line = NULL;
        size_t len = 0;
        ssize_t read{};
        while ((read = getline(&line, &len, fp)) != -1) {
            if (strstr(line, " /cache ")) {
                has_cache = true;
                break;
            }
        }
        free(line);
        fclose(fp);
    }

    if (has_cache) {
      // Collects and reports the non-a/b update metrics from last_install; and removes the file
      // to avoid duplicate report.
      if (file_exists(LAST_INSTALL_FILE_IN_CACHE) && unlink(LAST_INSTALL_FILE_IN_CACHE) == -1) {
        PLOG(ERROR) << "Failed to unlink " << LAST_INSTALL_FILE_IN_CACHE;
      }

      // TBD: Future location to move content from /cache/recovery to /data/misc/recovery/
      // if --force-persist flag, then transfer pmsg data anyways
      if ((argc <= 1) || !argv[1] || strcmp(argv[1], "--force-persist")) {
        return 0;
      }
    }

    // post-fs-data does not wait for a backend started asynchronously at
    // post-fs. This process already runs through exec_background, so waiting
    // for records does not block init. Do not erase pstore files after copying.
    const auto pstore = wait_for_pstore(std::chrono::seconds(30));
    if (pstore.error) return 1;
    if (pstore.pmsg == nullptr && pstore.console == nullptr) return 0;

    // Open only the selected backend, even if the other one appears or this
    // record disappears after discovery. Do not silently fall back.
    if (pstore.pmsg != nullptr) {
        ssize_t result = __android_log_pmsg_file_read_from_backend(
            pstore.backend, LOG_ID_SYSTEM, ANDROID_LOG_INFO, "recovery/", logsave, NULL);
        if (result < 0) {
            LOG(ERROR) << "Failed to read " << pstore.pmsg << ": " << strerror(-result);
            return 1;
        }
    }

    // For those device without /cache, the last_install file has been copied to
    // /data/misc/recovery from pmsg. Looks for the sideload history only.
    if (!has_cache) {
      if (file_exists(LAST_INSTALL_FILE) && unlink(LAST_INSTALL_FILE) == -1) {
        PLOG(ERROR) << "Failed to unlink " << LAST_INSTALL_FILE;
      }
    }
    rotate_last_kmsg(pstore.console);

    /* Is there a last console log too? */
    if (rotated && pstore.console != nullptr) {
      copy_file(pstore.console, LAST_KMSG_FILE);
    }

    return 0;
}
