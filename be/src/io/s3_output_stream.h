// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#ifdef STARROCKS_WITH_AWS

#pragma once

#include <aws/s3/S3Client.h>

#include "io/output_stream.h"

namespace starrocks::io {

class S3OutputStream : public OutputStream {
public:
    explicit S3OutputStream(std::shared_ptr<Aws::S3::S3Client> client, std::string bucket, std::string object,
                            int64_t max_single_part_size, int64_t min_upload_part_size);

    ~S3OutputStream() override = default;

    // Disallow copy and assignment
    S3OutputStream(const S3OutputStream&) = delete;
    void operator=(const S3OutputStream&) = delete;

    // Disallow move, because no usage now
    S3OutputStream(S3OutputStream&&) = delete;
    void operator=(S3OutputStream&&) = delete;

    Status write(const void* data, int64_t size) override;

    [[nodiscard]] bool allows_aliasing() const override { return false; }

    Status write_aliased(const void* data, int64_t size) override;

    Status skip(int64_t count) override;

    StatusOr<Buffer> get_direct_buffer() override;

    StatusOr<Position> get_direct_buffer_and_advance(int64_t size) override;

    Status close() override;

private:
    Status create_multipart_upload();
    Status multipart_upload();
    Status singlepart_upload();
    Status complete_multipart_upload();

    std::shared_ptr<Aws::S3::S3Client> _client;
    const Aws::String _bucket;
    const Aws::String _object;
    const int64_t _max_single_part_size;
    const int64_t _min_upload_part_size;
    Aws::String _buffer;
    Aws::String _upload_id;
    std::vector<Aws::String> _etags;
};

} // namespace starrocks::io

#endif // STARROCKS_WITH_AWS
