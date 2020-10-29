/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "common/tarutil.hpp"

#include <fcntl.h>
#include <libarchive/archive.h>
#include <libarchive/archive_entry.h>
#include <boost/filesystem.hpp>
#include "common/ffi.hpp"
#include "common/logger.hpp"

namespace fs = boost::filesystem;

namespace fc::common {

  static Logger logger = createLogger("tar util");

  int copy_data(struct archive *ar, struct archive *aw) {
    int r;
    const void *buff;
    size_t size;
    la_int64_t offset;

    for (;;) {
      r = archive_read_data_block(ar, &buff, &size, &offset);
      if (r == ARCHIVE_EOF) {
        return (ARCHIVE_OK);
      }
      if (r < ARCHIVE_OK) {
        return (r);
      }
      r = archive_write_data_block(aw, buff, size, offset);
      if (r < ARCHIVE_OK) {
        return (r);
      }
    }
  }

  void createDir(const fs::path &absolute_path, const std::string &base) {}

  outcome::result<int> zipTar(const std::string &input_path) {
    if (!fs::exists(input_path)) {
      logger->error("Zip tar: {} doesn't exists", input_path);
      return TarErrors::kCannotZipTarArchive;
    }

    int p[2];

    if (pipe(p) < 0) {
      return TarErrors::kCannotZipTarArchive;
    }

    std::function<void()> clear = [&]() {
      close(p[1]);
      close(p[0]);
    };
    auto _ = gsl::finally([&]() { clear(); });

    auto a = ffi::wrap(archive_write_new(), archive_write_free);
    archive_write_set_format_v7tar(a.get());
    int r = archive_write_open_fd(a.get(), p[1]);
    if (r < ARCHIVE_OK) {
      if (r < ARCHIVE_WARN) {
        logger->error("Zip tar: {}", archive_error_string(a.get()));
        return TarErrors::kCannotZipTarArchive;
      } else {
        logger->warn("Zip tar: {}", archive_error_string(a.get()));
      }
    }
    std::function<outcome::result<void>(const fs::path &, const fs::path &)>
        zipDir = [&](const fs::path &absolute_path,
                     const fs::path &relative_path) -> outcome::result<void> {
      struct archive_entry *entry;
      struct stat st {};
      char buff[8192];
      fs::directory_iterator dir_iter(absolute_path), end;

      while (dir_iter != end) {
        if (fs::is_directory(dir_iter->path())) {
          if (fs::directory_iterator(dir_iter->path())
              != fs::directory_iterator()) {
            OUTCOME_TRY(zipDir(dir_iter->path(),
                               relative_path / dir_iter->path().filename()));
          } else {
            stat(dir_iter->path().c_str(), &st);
            entry = archive_entry_new();  // Note 2
            archive_entry_set_pathname(
                entry, (relative_path / dir_iter->path().filename()).c_str());
            archive_entry_set_size(entry, st.st_size);  // Note 3
            archive_entry_set_filetype(entry, AE_IFDIR);
            archive_entry_set_perm(entry, 0644);
            r = archive_write_header(a.get(), entry);
            if (r < ARCHIVE_OK) {
              if (r < ARCHIVE_WARN) {
                logger->error("Zip tar: {}", archive_error_string(a.get()));
                return TarErrors::kCannotZipTarArchive;
              } else {
                logger->warn("Zip tar: {}", archive_error_string(a.get()));
              }
            }
            archive_entry_free(entry);
          }
        } else if (fs::is_regular_file(dir_iter->path())) {
          if (stat(dir_iter->path().c_str(), &st) < 0) {
            return TarErrors::kCannotZipTarArchive;
          }
          entry = archive_entry_new();
          archive_entry_set_pathname(
              entry, (relative_path / dir_iter->path().filename()).c_str());
          archive_entry_set_size(entry, st.st_size);
          archive_entry_set_filetype(entry, AE_IFREG);
          archive_entry_set_perm(entry, 0644);
          r = archive_write_header(a.get(), entry);
          if (r < ARCHIVE_OK) {
            if (r < ARCHIVE_WARN) {
              logger->error("Zip tar: {}", archive_error_string(a.get()));
              return TarErrors::kCannotZipTarArchive;
            } else {
              logger->warn("Zip tar: {}", archive_error_string(a.get()));
            }
          }
          int fd = open(dir_iter->path().c_str(), O_RDONLY);
          int len = read(fd, buff, sizeof(buff));
          while (len > 0) {
            archive_write_data(a.get(), buff, len);
            len = read(fd, buff, sizeof(buff));
          }
          close(fd);
          archive_entry_free(entry);
        } else {
          logger->warn("Unsupported entry type of {}", dir_iter->path());
        }

        dir_iter++;
      }
      return outcome::success();
    };

    fs::path base = fs::path(input_path).filename();
    OUTCOME_TRY(zipDir(input_path, base));
    archive_write_close(a.get());

    clear = [&]() { close(p[1]); };

    return p[0];
  }

  outcome::result<void> extractTar(const std::string &tar_path,
                                   const std::string &output_path) {
    if (!fs::exists(output_path)) {
      boost::system::error_code ec;
      if (!fs::create_directories(output_path, ec)) {
        if (ec.failed()) {
          logger->error("Extract tar: {}", ec.message());
        }
        return TarErrors::kCannotCreateDir;
      }
    }

    struct archive_entry *entry;
    int flags;
    int r;

    /* Select which attributes we want to restore. */
    flags = ARCHIVE_EXTRACT_TIME;
    flags |= ARCHIVE_EXTRACT_PERM;
    flags |= ARCHIVE_EXTRACT_ACL;
    flags |= ARCHIVE_EXTRACT_FFLAGS;

    auto a = ffi::wrap(archive_read_new(), archive_read_free);
    archive_read_support_format_tar(a.get());
    auto ext = ffi::wrap(archive_write_disk_new(), archive_write_free);
    archive_write_disk_set_options(ext.get(), flags);
    archive_write_disk_set_standard_lookup(ext.get());
    if (archive_read_open_filename(a.get(), tar_path.c_str(), kTarBlockSize)
        != ARCHIVE_OK) {
      logger->error("Extract tar: {}", archive_error_string(a.get()));
      return TarErrors::kCannotUntarArchive;
    }
    for (;;) {
      r = archive_read_next_header(a.get(), &entry);
      if (r == ARCHIVE_EOF) {
        break;
      }
      if (r < ARCHIVE_WARN) {
        logger->error("Extract tar: {}", archive_error_string(a.get()));
        return TarErrors::kCannotUntarArchive;
      }
      if (r < ARCHIVE_OK) {
        logger->warn("Extract tar: {}", archive_error_string(a.get()));
      }

      std::string currentFile(archive_entry_pathname(entry));
      archive_entry_set_pathname(entry,
                                 (fs::path(output_path) / currentFile).c_str());
      r = archive_write_header(ext.get(), entry);
      if (r < ARCHIVE_OK) {
        if (r < ARCHIVE_WARN) {
          logger->error("Extract tar: {}", archive_error_string(a.get()));
        } else {
          logger->warn("Extract tar: {}", archive_error_string(a.get()));
        }
      } else if (archive_entry_size(entry) > 0) {
        r = copy_data(a.get(), ext.get());
        if (r < ARCHIVE_WARN) {
          logger->error("Extract tar: {}", archive_error_string(a.get()));
          return TarErrors::kCannotUntarArchive;
        }
        if (r < ARCHIVE_OK) {
          logger->warn("Extract tar: {}", archive_error_string(a.get()));
        }
      }
      r = archive_write_finish_entry(ext.get());
      if (r < ARCHIVE_WARN) {
        logger->error("Extract tar: {}", archive_error_string(a.get()));
        return TarErrors::kCannotUntarArchive;
      }
      if (r < ARCHIVE_OK) {
        logger->warn("Extract tar: {}", archive_error_string(a.get()));
      }
    }
    archive_read_close(a.get());
    archive_write_close(ext.get());

    return outcome::success();
  }

}  // namespace fc::common

OUTCOME_CPP_DEFINE_CATEGORY(fc::common, TarErrors, e) {
  using fc::common::TarErrors;
  switch (e) {
    case (TarErrors::kCannotCreateDir):
      return "Tar Util: cannot create output dir";
    case (TarErrors::kCannotUntarArchive):
      return "Tar Util: cannot untar archive";
    case (TarErrors::kCannotZipTarArchive):
      return "Tar Util: cannot zip tar archive";
    default:
      return "Tar Util: unknown error";
  }
}
