/* Copyright (c) Edgeless Systems GmbH

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#include "syscall_handler.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <regex>
#include <stdexcept>
#include <string>

#include "syscall_file.h"

using namespace std;
using namespace edb;

static const regex re_folder(R"(\./[^./]+/?)");
static const regex re_path_to_known_file(R"(\./[^./]+/(db\.opt|[^./]+\.frm))");
static const regex re_path_to_temp_frm_file(R"(\./[^./]+/[^./]+\.frm~)");
static constexpr string_view temp_frm_ext = ".frm~";

static bool StrEndsWith(string_view str, string_view suffix) {
  return str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static bool IsKnownExtension(string_view path) {
  return StrEndsWith(path, ".frm") || StrEndsWith(path, ".opt");
}

static string_view GetCf(string_view path) {
  if (StrEndsWith(path, ".frm"))
    return kCfNameFrm;
  if (StrEndsWith(path, ".opt"))
    return kCfNameDb;
  throw runtime_error("unexpected path");
}

static string NormalizePath(string_view path) {
  const string_view datadir = "/data/";
  if (path.compare(0, datadir.size(), datadir) != 0)
    return string(path);
  if (path == datadir)
    return ".";
  return "./" + string(path.substr(datadir.size()));
}

static string ReadFile(string_view path) {
  const auto file = fopen(string(path).c_str(), "rb");
  if (!file)
    throw runtime_error("can't open file");
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  string buf(size, '\0');
  const size_t res = fread(buf.data(), size, 1, file);
  fclose(file);
  if (res != 1)
    throw runtime_error("can't read file");
  return buf;
}

SyscallHandler::SyscallHandler(StorePtr store)
    : store_(move(store)) {
}

std::optional<int> SyscallHandler::Syscall(long number, long x1, long x2) {
  switch (number) {
    case SYS_open:
      return Open(reinterpret_cast<char*>(x1), static_cast<int>(x2));
    case SYS_stat:
      return Stat(reinterpret_cast<char*>(x1), x2);
    case SYS_access:
      return Access(reinterpret_cast<char*>(x1));
    case SYS_rename:
      return Rename(reinterpret_cast<char*>(x1), reinterpret_cast<char*>(x2));
    case SYS_unlink:
      return Unlink(reinterpret_cast<char*>(x1));
    default:
      return {};
  }
}

std::vector<std::string> SyscallHandler::Dir(std::string_view pathname) const {
  const string path = NormalizePath(pathname);

  const bool is_db = path == ".";
  if (!is_db && !regex_match(path.cbegin(), path.cend(), re_folder))
    throw invalid_argument("unexpected path");

  vector<string> result;

  {
    const lock_guard lock(mutex_);
    if (is_db)
      result = store_->GetKeys(kCfNameDb, {});
    else
      result = store_->GetKeys(kCfNameFrm, path);
  }

  if (is_db)
    for (auto& x : result) {
      x.erase(x.rfind('/'));  // remove /db.opt
      x.erase(0, 2);          // remove ./
    }
  else
    for (auto& x : result)
      x.erase(0, x.rfind('/') + 1);  // remove path before filename

  return result;
}

size_t SyscallHandler::Read(std::string_view path, void* buf, size_t count, size_t offset) const {
  const string_view cf = GetCf(path);
  optional<string> value;

  {
    const lock_guard lock(mutex_);
    value = store_->Get(cf, path);
  }

  if (!value)
    throw logic_error("not found");

  if (value->size() <= offset)
    return 0;

  count = min(count, value->size() - offset);
  memcpy(buf, value->data() + offset, count);
  return count;
}

void SyscallHandler::Write(std::string_view path, std::string_view buf, size_t offset) {
  const string_view cf = GetCf(path);

  const lock_guard lock(mutex_);

  string value = store_->Get(cf, path).value_or(string());

  const size_t required_size = offset + buf.size();
  if (required_size < offset)
    throw overflow_error("write offset overflow");
  if (value.size() < required_size)
    value.resize(required_size);

  memcpy(value.data() + offset, buf.data(), buf.size());

  store_->Put(cf, path, value);
}

size_t SyscallHandler::Size(std::string_view path) const {
  const string_view cf = GetCf(path);
  const lock_guard lock(mutex_);
  return store_->Get(cf, path).value_or(string()).size();
}

std::optional<int> SyscallHandler::Open(const char* pathname, int flags) {
  assert(pathname && *pathname);
  const string path = NormalizePath(pathname);

  if (!IsKnownExtension(path)) {
    // if it's a temporary frm file, make sure the directory exists
    if (StrEndsWith(path, temp_frm_ext)) {
      if (!regex_match(path.cbegin(), path.cend(), re_path_to_temp_frm_file))
        throw invalid_argument("unexpected pathname");
      mkdir(string(path, 0, path.rfind('/')).c_str(), 0777);
    }
    return {};
  }

  if (!regex_match(path.cbegin(), path.cend(), re_path_to_known_file))
    throw invalid_argument("unexpected pathname");

  if (!(flags & O_CREAT) && !Exists(path)) {
    errno = ENOENT;
    return -1;
  }

  // don't create .frm file if db doesn't exist
  if (StrEndsWith(path, ".frm") && !Exists(string(path, 0, path.rfind('/') + 1) + "db.opt")) {
    errno = ENOENT;
    return -1;
  }

  return RedirectOpenFile(path, this);
}

std::optional<int> SyscallHandler::Stat(const char* pathname, long statbuf) const {
  assert(pathname && *pathname);
  assert(statbuf);

  const string_view path = pathname;
  if (!IsKnownExtension(path))
    return {};
  if (!regex_match(path.cbegin(), path.cend(), re_path_to_known_file))
    throw invalid_argument("unexpected pathname");

  const string_view cf = GetCf(path);
  optional<string> value;

  {
    const lock_guard lock(mutex_);
    value = store_->Get(cf, path);
  }

  if (!value) {
    errno = ENOENT;
    return -1;
  }

  auto& st = *reinterpret_cast<struct stat*>(statbuf);
  memset(&st, 0, sizeof st);
  st.st_size = value->size();
  return 0;
}

std::optional<int> SyscallHandler::Access(const char* pathname) const {
  assert(pathname && *pathname);

  string path = pathname;
  const bool known_ext = IsKnownExtension(path);

  if (known_ext) {
    if (!regex_match(path.cbegin(), path.cend(), re_path_to_known_file)) {
      path.insert(0, "./");
      if (!regex_match(path.cbegin(), path.cend(), re_path_to_known_file))
        throw invalid_argument("unexpected pathname");
    }
  } else if (regex_match(path.cbegin(), path.cend(), re_folder)) {
    // It might be a db folder. Check if db.opt exists.
    if (path.back() != '/')
      path += '/';
    path += "db.opt";
  } else
    return {};

  if (Exists(path))
    return 0;
  if (!known_ext)
    return {};

  errno = ENOENT;
  return -1;
}

std::optional<int> SyscallHandler::Rename(const char* oldpath, const char* newpath) {
  if (StrEndsWith(oldpath, ".frm") && StrEndsWith(newpath, ".frm")) {
    if (!regex_match(oldpath, re_path_to_known_file))
      throw invalid_argument("unexpected oldpath");
    if (!regex_match(newpath, re_path_to_known_file))
      throw invalid_argument("unexpected newpath");

    // both old and new are in the store
    const lock_guard lock(mutex_);
    const auto value = store_->Get(kCfNameFrm, oldpath);
    store_->Put(kCfNameFrm, newpath, value.value());
    store_->Delete(kCfNameFrm, oldpath);
    return 0;
  }

  if (StrEndsWith(oldpath, temp_frm_ext)) {
    if (!regex_match(newpath, re_path_to_known_file))
      throw invalid_argument("unexpected newpath");

    // temp frm files are in memfs and should be moved into the store
    const lock_guard lock(mutex_);
    store_->Put(kCfNameFrm, newpath, ReadFile(oldpath));
    remove(oldpath);
    return 0;
  }

  return {};
}

std::optional<int> SyscallHandler::Unlink(const char* pathname) {
  assert(pathname && *pathname);

  const string_view path = pathname;
  if (!IsKnownExtension(path))
    return {};

  const string_view cf = GetCf(path);

  const lock_guard lock(mutex_);
  store_->Delete(cf, path);
  return 0;
}

bool SyscallHandler::Exists(std::string_view path) const {
  const string_view cf = GetCf(path);
  const lock_guard lock(mutex_);
  return store_->Get(cf, path).has_value();
}
