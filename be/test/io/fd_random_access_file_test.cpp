// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "io/fd_random_access_file.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstdlib>

#include "common/logging.h"
#include "testutil/assert.h"
#include "testutil/parallel_test.h"

namespace starrocks::io {

static int open_temp_file() {
    char tmpl[] = "/tmp/fd_random_access_file_testXXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) {
        std::cerr << "mkstemp() failed: " << strerror(errno) << '\n';
        PLOG(FATAL) << "mkstemp() failed";
    }
    int ret = ::unlink(tmpl);
    if (ret < 0) {
        std::cerr << "unlink() failed: " << strerror(errno) << '\n';
        PLOG(FATAL) << "unlink() failed";
    }
    return fd;
}

static void pwrite_or_die(int fd, const void* buff, size_t count, off_t offset) {
    int r = ::pwrite(fd, buff, count, offset);
    if (r != count) {
        PLOG(FATAL) << "pwrite() failed";
    }
}

// NOLINTNEXTLINE
PARALLEL_TEST(FdRandomAccessFileTest, test_read_empty) {
    int fd = open_temp_file();
    FdRandomAccessFile in(fd);
    in.set_close_on_delete(true);

    char buff[1];
    ASSERT_EQ(0, *in.get_size());
    ASSERT_EQ(0, *in.position());
    ASSERT_EQ(0, *in.read(buff, 1));
    ASSERT_EQ(0, *in.read_at(0, buff, 1));
}

// NOLINTNEXTLINE
PARALLEL_TEST(FdRandomAccessFileTest, test_read) {
    int fd = open_temp_file();
    pwrite_or_die(fd, "0123456789", 10, 0);

    FdRandomAccessFile in(fd);
    in.set_close_on_delete(true);
    ASSERT_EQ(10, *in.get_size());
    ASSERT_EQ(0, *in.position());

    char buff[10];

    ASSERT_EQ(1, *in.read(buff, 1));
    ASSERT_EQ("0", std::string_view(buff, 1));
    ASSERT_EQ(1, *in.position());

    ASSERT_EQ(5, *in.read(buff + 1, 5));
    ASSERT_EQ("012345", std::string_view(buff, 6));
    ASSERT_EQ(6, *in.position());

    ASSERT_EQ(4, *in.read(buff + 6, 10));
    ASSERT_EQ("0123456789", std::string_view(buff, 10));
    ASSERT_EQ(10, *in.position());
}

// NOLINTNEXTLINE
PARALLEL_TEST(FdRandomAccessFileTest, test_read_at) {
    int fd = open_temp_file();
    pwrite_or_die(fd, "0123456789", 10, 0);

    FdRandomAccessFile in(fd);
    in.set_close_on_delete(true);
    ASSERT_EQ(10, *in.get_size());
    ASSERT_EQ(0, *in.position());

    char buff[10];

    ASSERT_EQ(1, *in.read_at(0, buff, 1));
    ASSERT_EQ("0", std::string_view(buff, 1));
    ASSERT_EQ(0, *in.position());

    ASSERT_EQ(5, *in.read_at(1, buff + 1, 5));
    ASSERT_EQ("012345", std::string_view(buff, 6));
    ASSERT_EQ(0, *in.position());

    ASSERT_EQ(4, *in.read_at(6, buff + 6, 10));
    ASSERT_EQ("0123456789", std::string_view(buff, 10));
    ASSERT_EQ(0, *in.position());

    ASSERT_EQ(0, in.get_errno());
}

// NOLINTNEXTLINE
PARALLEL_TEST(FdRandomAccessFileTest, test_seek) {
    int fd = open_temp_file();
    pwrite_or_die(fd, "0123456789", 10, 0);

    FdRandomAccessFile in(fd);
    in.set_close_on_delete(true);
    ASSERT_EQ(10, *in.get_size());
    ASSERT_EQ(0, *in.position());

    char buff[10];

    ASSERT_EQ(1, *in.seek(1, SEEK_SET));
    ASSERT_EQ(1, *in.position());
    ASSERT_EQ(5, *in.read(buff, 5));
    ASSERT_EQ("12345", std::string_view(buff, 5));
    ASSERT_EQ(6, *in.position());

    ASSERT_EQ(16, *in.seek(10, SEEK_CUR));
    ASSERT_EQ(16, *in.position());
    ASSERT_EQ(0, *in.read(buff, 1));

    ASSERT_EQ(6, *in.seek(-4, SEEK_END));
    ASSERT_EQ(6, *in.position());
    ASSERT_EQ(4, *in.read(buff, 10));
    ASSERT_EQ(10, *in.position());
    ASSERT_EQ("6789", std::string_view(buff, 4));

    ASSERT_EQ(0, in.get_errno());
}

// NOLINTNEXTLINE
PARALLEL_TEST(FdRandomAccessFileTest, test_skip) {
    int fd = open_temp_file();
    pwrite_or_die(fd, "0123456789", 10, 0);

    char buff[10];
    FdRandomAccessFile in(fd);
    in.set_close_on_delete(true);
    ASSERT_OK(in.skip(2));
    ASSERT_EQ(2, *in.read(buff, 2));
    ASSERT_EQ(4, *in.position());
    ASSERT_EQ("23", std::string_view(buff, 2));

    ASSERT_OK(in.skip(10));
    ASSERT_EQ(0, *in.read(buff, 2));
    ASSERT_EQ(0, in.get_errno());
}

// NOLINTNEXTLINE
PARALLEL_TEST(FdRandomAccessFileTest, test_op_after_close) {
    int fd = open_temp_file();
    pwrite_or_die(fd, "0123456789", 10, 0);

    char buff[10];
    FdRandomAccessFile in(fd);
    ASSERT_OK(in.close());

    auto res = in.read(buff, 2);
    ASSERT_ERROR(res.status());

    res = in.read_at(0, buff, 2);
    ASSERT_ERROR(res.status());

    res = in.seek(0, SEEK_CUR);
    ASSERT_ERROR(res.status());

    res = in.skip(10);
    ASSERT_ERROR(res.status());

    res = in.get_size();
    ASSERT_ERROR(res.status());

    res = in.position();
    ASSERT_ERROR(res.status());

    ASSERT_ERROR(in.close());
}

} // namespace starrocks::io
