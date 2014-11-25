// Copyright (c) 2014 Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>

#include "breakpad_googletest_includes.h"
#include "client/linux/handler/exception_handler.h"
#include "client/linux/microdump_writer/microdump_writer.h"
#include "common/linux/eintr_wrapper.h"
#include "common/linux/ignore_ret.h"
#include "common/scoped_ptr.h"
#include "common/tests/auto_tempdir.h"
#include "common/using_std_string.h"

using namespace google_breakpad;

// Length of a formatted GUID string =
// sizeof(MDGUID) * 2 + 4 (for dashes) + 1 (null terminator)
const int kGUIDStringSize = 37;

namespace {

typedef testing::Test MicrodumpWriterTest;

TEST(MicrodumpWriterTest, Setup) {
  int fds[2];
  ASSERT_NE(-1, pipe(fds));

  AutoTempDir temp_dir;
  string stderr_file = temp_dir.path() + "/stderr.log";
  int err_fd = open(stderr_file.c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  ASSERT_NE(-1, err_fd);

  const pid_t child = fork();
  if (child == 0) {
    close(fds[1]);
    char b;
    IGNORE_RET(HANDLE_EINTR(read(fds[0], &b, sizeof(b))));
    close(fds[0]);
    syscall(__NR_exit);
  }
  close(fds[0]);

  ExceptionHandler::CrashContext context;
  memset(&context, 0, sizeof(context));

  // Set a non-zero tid to avoid tripping asserts.
  context.tid = child;

  // Push some extra mapping to check the MappingList logic.
  const uint32_t memory_size = sysconf(_SC_PAGESIZE);
  const char* kMemoryName = "libfoo.so";
  const uint8_t kModuleGUID[sizeof(MDGUID)] = {
     0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
     0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
  };

  MappingInfo info;
  info.start_addr = memory_size;
  info.size = memory_size;
  info.offset = 42;
  strcpy(info.name, kMemoryName);

  MappingList mappings;
  MappingEntry mapping;
  mapping.first = info;
  memcpy(mapping.second, kModuleGUID, sizeof(MDGUID));
  mappings.push_back(mapping);

  // Redirect temporarily stderr to the stderr.log file.
  int save_err = dup(STDERR_FILENO);
  ASSERT_NE(-1, save_err);
  ASSERT_NE(-1, dup2(err_fd, STDERR_FILENO));

  ASSERT_TRUE(WriteMicrodump(child, &context, sizeof(context), mappings));

  // Revert stderr back to the console.
  dup2(save_err, STDERR_FILENO);
  close(save_err);

  // Read back the stderr file and check for the microdump marker.
  fsync(err_fd);
  lseek(err_fd, 0, SEEK_SET);
  const size_t kBufSize = 64 * 1024;
  scoped_array<char> buf(new char[kBufSize]);
  ASSERT_GT(read(err_fd, buf.get(), kBufSize), 0);

  ASSERT_NE(static_cast<char*>(0), strstr(
      buf.get(), "-----BEGIN BREAKPAD MICRODUMP-----"));
  ASSERT_NE(static_cast<char*>(0), strstr(
      buf.get(), "-----END BREAKPAD MICRODUMP-----"));
  ASSERT_NE(static_cast<char*>(0), strstr(
      buf.get(), "M 0000000000001000 000000000000002A 0000000000001000 "
      "33221100554477668899AABBCCDDEEFF0 libfoo.so"));

  close(err_fd);
  close(fds[1]);
}

}  // namespace
