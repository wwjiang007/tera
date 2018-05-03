// Copyright (c) 2015, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>

#include "nfs.h"
#include "nfs_wrapper.h"
#include "util/hash.h"
#include "util/mutexlock.h"
#include "util/string_ext.h"
#include "../common/timer.h"
#include "../utils/counter.h"

namespace leveldb {

static const char* (*printVersion)();
static int (*nfsInit)(const char* mountpoint, const char* config_file_path);
static void (*nfsSetComlogLevel)(int loglevel);
static int (*nfsGetErrno)();

static int (*nfsMkdir)(const char* path);
static int (*nfsRmdir)(const char* path);
static nfs::NFSDIR* (*nfsOpendir)(const char* path);
static struct ::dirent* (*nfsReaddir)(nfs::NFSDIR* dir);
static int (*nfsClosedir)(nfs::NFSDIR* dir);
static int (*nfsSetDirOwner)(const char* path);
static int (*nfsClearDirOwner)(const char* path);

static int (*nfsStat)(const char* path, struct ::stat* stat);
static int (*nfsUnlink)(const char* path);
static int (*nfsAccess)(const char* path, int mode);
static int (*nfsRename)(const char* oldpath, const char* newpath);

static nfs::NFSFILE* (*nfsOpen)(const char* path, const char* mode);
static int (*nfsClose)(nfs::NFSFILE* stream);
static int (*nfsForceRelease)(const char* path);

static ssize_t (*nfsRead)(nfs::NFSFILE* stream, void* ptr, size_t size);
static ssize_t (*nfsPRead)(nfs::NFSFILE* stream, void* ptr, size_t size,
                        uint64_t offset);
static ssize_t (*nfsWrite)(nfs::NFSFILE* stream, const void* ptr, size_t size);

static int (*nfsFsync)(nfs::NFSFILE* stream);
static int64_t (*nfsTell)(nfs::NFSFILE* stream);
static int (*nfsSeek)(nfs::NFSFILE* stream, uint64_t offset);

static void (*nfsSetAssignNamespaceIdFunc)(nfs::AssignNamespaceIdFunc func);

static void* dl = NULL;

void* ResolveSymbol(void* dl, const char* sym) {
  dlerror();
  void* sym_ptr = dlsym(dl, sym);
  const char* error = dlerror();
  if (strcmp(sym,"SetAssignNamespaceIdFunc") == 0 && error != NULL) {
      fprintf(stderr, "libnfs.so does not support federation\n");
      return NULL;
  }
  if (strcmp(sym,"SetDirOwner") == 0 && error != NULL) {
      fprintf(stderr, "libnfs.so does not support SetDirOwner\n");
      return NULL;
  }
  if (strcmp(sym,"ClearDirOwner") == 0 && error != NULL) {
      fprintf(stderr, "libnfs.so does not support ClearDirOwner\n");
      return NULL;
  }

  if (strcmp(sym,"ForceRelease") == 0 && error != NULL) {
      fprintf(stderr, "libnfs.so does not support ForceRelease\n");
      return NULL;
  }
  if (error != NULL) {
    fprintf(stderr, "resolve symbol %s from libnfs.so error: %s\n",
            sym, error);
    abort();
  }
  return sym_ptr;
}

void Nfs::LoadSymbol() {
  dlerror();
  dl = dlopen("libnfs.so", RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
  if (dl == NULL) {
    fprintf(stderr, "dlopen libnfs.so error: %s\n", dlerror());
    abort();
  }

  *(void**)(&printVersion) = ResolveSymbol(dl, "PrintNfsVersion");
  fprintf(stderr, "libnfs.so version: \n%s\n\n", (*printVersion)());

  *(void**)(&nfsInit) = ResolveSymbol(dl, "Init");
  *(void**)(&nfsSetComlogLevel) = ResolveSymbol(dl, "SetComlogLevel");
  *(void**)(&nfsGetErrno) = ResolveSymbol(dl, "GetErrno");
  *(void**)(&nfsMkdir) = ResolveSymbol(dl, "Mkdir");
  *(void**)(&nfsRmdir) = ResolveSymbol(dl, "Rmdir");
  *(void**)(&nfsOpendir) = ResolveSymbol(dl, "Opendir");
  *(void**)(&nfsReaddir) = ResolveSymbol(dl, "Readdir");
  *(void**)(&nfsClosedir) = ResolveSymbol(dl, "Closedir");
  *(void**)(&nfsSetDirOwner) = ResolveSymbol(dl, "SetDirOwner");
  *(void**)(&nfsClearDirOwner) = ResolveSymbol(dl, "ClearDirOwner");
  *(void**)(&nfsStat) = ResolveSymbol(dl, "Stat");
  *(void**)(&nfsUnlink) = ResolveSymbol(dl, "Unlink");
  *(void**)(&nfsAccess) = ResolveSymbol(dl, "Access");
  *(void**)(&nfsRename) = ResolveSymbol(dl, "Rename");
  *(void**)(&nfsOpen) = ResolveSymbol(dl, "Open");
  *(void**)(&nfsClose) = ResolveSymbol(dl, "Close");
  *(void**)(&nfsForceRelease) = ResolveSymbol(dl, "ForceRelease");
  *(void**)(&nfsRead) = ResolveSymbol(dl, "Read");
  *(void**)(&nfsPRead) = ResolveSymbol(dl, "PRead");
  *(void**)(&nfsWrite) = ResolveSymbol(dl, "Write");
  *(void**)(&nfsFsync) = ResolveSymbol(dl, "Fsync");
  *(void**)(&nfsTell) = ResolveSymbol(dl, "Tell");
  *(void**)(&nfsSeek) = ResolveSymbol(dl, "Seek");
  *(void**)(&nfsSetAssignNamespaceIdFunc) = ResolveSymbol(dl, "SetAssignNamespaceIdFunc");
}

NFile::NFile(nfs::NFSFILE* file, const std::string& name)
  : file_(file), name_(name) {
}
NFile::~NFile() {
  if (file_) {
    CloseFile();
  }
}

int32_t NFile::Write(const char* buf, int32_t len) {
  int32_t retval = (*nfsWrite)(file_, buf, len);
  if (retval < 0) {
    errno = (*nfsGetErrno)();
  }
  return retval;
}
int32_t NFile::Flush() {
  int32_t retval = 0;
  // retval = hdfsFlush(fs_, file_);
  return retval;
}
int32_t NFile::Sync() {
  int32_t retval = (*nfsFsync)(file_);
  if (retval != 0) {
    errno = (*nfsGetErrno)();
  }
  return retval;
}
int32_t NFile::Read(char* buf, int32_t len) {
  int32_t retval = (*nfsRead)(file_, buf, len);
  if (retval < 0) {
    errno = (*nfsGetErrno)();
  }
  return retval;
}
int32_t NFile::Pread(int64_t offset, char* buf, int32_t len) {
  int32_t retval = (*nfsPRead)(file_, buf, len, offset);
  if (retval < 0) {
    errno = (*nfsGetErrno)();
  }
  return retval;
}
int64_t NFile::Tell() {
  int64_t retval = (*nfsTell)(file_);
  if (retval < 0) {
    errno = (*nfsGetErrno)();
  }
  return retval;
}
int32_t NFile::Seek(int64_t offset) {
  int32_t retval = (*nfsSeek)(file_, offset);
  if (retval != 0) {
    errno = (*nfsGetErrno)();
  }
  return retval;
}

int32_t NFile::CloseFile() {
  int32_t retval = 0;
  if (file_ != NULL) {
    retval = (*nfsClose)(file_);
  }
  if (retval != 0) {
    errno = (*nfsGetErrno)();
    fprintf(stderr, "[ClosFile] %s fail: %d\n", name_.c_str(), errno);
  }
  // WARNING: ignore nfs close error; may cause memory leak
  file_ = NULL;
  return retval;
}

bool Nfs::dl_init_ = false;
port::Mutex Nfs::mu_;
static Nfs* instance = NULL;

int Nfs::CalcNamespaceId(const char* c_path, int max_namespaces) {
    if (!c_path) {
      fprintf(stderr, "null path for Nfs::CalcNamespaceId\n");
      return -1;
    }
    std::string path(c_path);
    size_t pos = path.rfind("tablet");
    if (pos == std::string::npos) {
        return 0;
    }
    size_t pos2 = path.find('/', pos);
    if (pos2 == std::string::npos) {
        pos2 = path.size();
    }
    std::string hash_path = path.substr(pos, pos2 - pos);
    uint32_t index = Hash(hash_path.c_str(), hash_path.size(),
                          1984) % max_namespaces;
    return index;
}

void Nfs::Init(const std::string& mountpoint, const std::string& conf_path)
{
  MutexLock l(&mu_);
  if (!dl_init_) {
    LoadSymbol();
    dl_init_ = true;
  }
  (*nfsSetComlogLevel)(2);
  if (nfsSetAssignNamespaceIdFunc) {
      nfsSetAssignNamespaceIdFunc(&CalcNamespaceId);
  }
  if (0 != (*nfsInit)(mountpoint.c_str(), conf_path.c_str())) {
    char err[256];
    strerror_r((*nfsGetErrno)(), err, 256);
    fprintf(stderr, "init nfs fail: %s\n", err);
    abort();
  }
}

Nfs* Nfs::GetInstance() {
  MutexLock l(&mu_);
  if (instance == NULL) {
    instance = new Nfs();
  }
  return instance;
}

Nfs::Nfs() {}

Nfs::~Nfs() {}

int32_t Nfs::CreateDirectory(const std::string& name) {
  std::vector<std::string> items;
  SplitString(name, "/", &items);
  std::string path;
  if (name[0] == '/') {
    path = "/";
  }
  for (uint32_t i = 0; i < items.size(); ++i) {
    path += items[i];
    if (0 != (*nfsAccess)(path.c_str(), F_OK) && (*nfsGetErrno)() == ENOENT) {
      if (0 != (*nfsMkdir)(path.c_str()) && (*nfsGetErrno)() != EEXIST) {
        errno = (*nfsGetErrno)();
        fprintf(stderr, "[%s] Createdir %s fail: %d\n", common::timer::get_curtime_str().c_str(), name.c_str(), errno);
        return -1;
      }
    }
    path += "/";
  }
  return 0;
}
int32_t Nfs::DeleteDirectory(const std::string& name) {
  int32_t retval = (*nfsRmdir)(name.c_str());
  if (retval != 0) {
    errno = (*nfsGetErrno)();
    fprintf(stderr, "[%s] DeleteDirectory %s fail: %d\n", common::timer::get_curtime_str().c_str(), name.c_str(), errno);
  }
  return retval;
}
int32_t Nfs::Exists(const std::string& filename) {
  int32_t retval = (*nfsAccess)(filename.c_str(), F_OK);
  if (retval != 0) {
    errno = (*nfsGetErrno)();
    int errno_saved = errno;
    fprintf(stderr, "[%s] Exists %s fail: %d\n", common::timer::get_curtime_str().c_str(), filename.c_str(), errno);
    errno = errno_saved;
  }
  return retval;
}
int32_t Nfs::Delete(const std::string& filename) {
  int32_t retval = (*nfsUnlink)(filename.c_str());
  if (retval != 0) {
    errno = (*nfsGetErrno)();
    fprintf(stderr, "[%s] Delete %s fail: %d\n", common::timer::get_curtime_str().c_str(), filename.c_str(), errno);
  }
  return retval;
}
int32_t Nfs::GetFileSize(const std::string& filename, uint64_t* size) {
  struct stat fileinfo;
  int32_t retval = (*nfsStat)(filename.c_str(), &fileinfo);
  if (retval == 0) {
    *size = fileinfo.st_size;
  } else {
    errno = (*nfsGetErrno)();
    fprintf(stderr, "[%s] Getfilesize %s fail: %d\n", common::timer::get_curtime_str().c_str(), filename.c_str(), errno);
  }
  return retval;
}
int32_t Nfs::Rename(const std::string& from, const std::string& to) {
  int32_t retval = (*nfsRename)(from.c_str(), to.c_str());
  if (retval != 0) {
    errno = (*nfsGetErrno)();
    fprintf(stderr, "[%s] Rename %s to %s fail: %d\n", common::timer::get_curtime_str().c_str(), from.c_str(), to.c_str(), errno);
  }
  return retval;
}

DfsFile* Nfs::OpenFile(const std::string& filename, int32_t flags) {
  //fprintf(stderr, "OpenFile %s %d\n", filename.c_str(), flags);
  nfs::NFSFILE* file = NULL;
  if (flags == RDONLY) {
    file = (*nfsOpen)(filename.c_str(), "r");
  } else {
    file = (*nfsOpen)(filename.c_str(), "w");
  }
  if (file != NULL) {
    return new NFile(file, filename);
  }
  errno = (*nfsGetErrno)();
  fprintf(stderr, "[%s] Openfile %s fail: %d\n", common::timer::get_curtime_str().c_str(), filename.c_str(), errno);
  return NULL;
}

int32_t Nfs::Copy(const std::string& from, const std::string& to) {
  // not support
  return -1;
}
int32_t Nfs::ListDirectory(const std::string& path,
                           std::vector<std::string>* result) {
  nfs::NFSDIR* dir = (*nfsOpendir)(path.c_str());
  if (NULL == dir) {
    errno = (*nfsGetErrno)();
    int errno_saved = errno;
    fprintf(stderr, "[%s] Opendir %s fail: %d\n", common::timer::get_curtime_str().c_str(), path.c_str(), errno);
    errno = errno_saved;
    return -1;
  }
  struct ::dirent* dir_info = NULL;
  while (NULL != (dir_info = (*nfsReaddir)(dir))) {
    const char* pathname = dir_info->d_name;
    if (strcmp(pathname, ".") != 0 && strcmp(pathname, "..") != 0) {
      result->push_back(pathname);
    }
  }
  errno = (*nfsGetErrno)();
  int errno_saved = errno;
  if (0 != errno) {
    fprintf(stderr, "[%s] List %s error: %d\n", common::timer::get_curtime_str().c_str(), path.c_str(), errno);
    (*nfsClosedir)(dir);
    errno = errno_saved;
    return -1;
  }
  (*nfsClosedir)(dir);
  return 0;
}

int32_t Nfs::LockDirectory(const std::string& path) {
  int ret = (*nfsSetDirOwner)(path.c_str());
  if (ret != 0) {
    fprintf(stderr, "[LockDirectory] lock dir %s fail, errno: %d\n",
        path.c_str(), errno);
    return -1;
  }

  std::vector<std::string> files;
  ret = ListDirectory(path, &files);
  if (ret != 0) {
    fprintf(stderr, "[LockDirectory] list dir %s fail, errno: %d\n",
        path.c_str(), errno);
    return -1;
  }

  for (size_t i = 0; i < files.size(); i++) {
    if (files[i].find(".log") != std::string::npos ||
        files[i].find("MANIFEST") != std::string::npos) {
      std::string file_name = path + "/" + files[i];
      ret = (*nfsForceRelease)(file_name.c_str());
      if (ret != 0) {
        fprintf(stderr, "[LockDirectory] force release file %s fail, errno: %d\n",
            file_name.c_str(), errno);
        return -1;
      }
    }
  }
  return 0;
}

int32_t Nfs::UnlockDirectory(const std::string& path) {
  return (*nfsClearDirOwner)(path.c_str());
}

}
/* vim: set expandtab ts=2 sw=2 sts=2 tw=100: */
