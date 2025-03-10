// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#ifdef STARROCKS_WITH_AWS

#include "env/env_s3.h"

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/utils/threading/Executor.h>
#include <aws/s3/model/BucketLocationConstraint.h>
#include <aws/s3/model/CreateBucketRequest.h>
#include <aws/s3/model/DeleteBucketRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListObjectsRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/transfer/TransferHandle.h>
#include <fmt/core.h>
#include <time.h>

#include <limits>

#include "common/config.h"
#include "common/s3_uri.h"
#include "gutil/strings/substitute.h"
#include "io/s3_output_stream.h"
#include "io/s3_random_access_file.h"
#include "util/hdfs_util.h"
#include "util/random.h"

namespace starrocks {

// Wrap a `starrocks::io::RandomAccessFile` into `starrocks::RandomAccessFile`.
// this wrapper will be deleted after we merged `starrocks::RandomAccessFile` and
// `starrocks::io:RandomAccessFile` into one class,
class RandomAccessFileAdapter : public RandomAccessFile {
public:
    explicit RandomAccessFileAdapter(std::unique_ptr<io::RandomAccessFile> input, std::string path)
            : _input(std::move(input)), _object_path(std::move(path)), _object_size(0) {}

    ~RandomAccessFileAdapter() override = default;

    Status read(uint64_t offset, Slice* res) const override;
    Status read_at(uint64_t offset, const Slice& res) const override;
    Status readv_at(uint64_t offset, const Slice* res, size_t res_cnt) const override;
    Status size(uint64_t* size) const override;
    const std::string& file_name() const override { return _object_path; }

private:
    std::unique_ptr<io::RandomAccessFile> _input;
    std::string _object_path;
    mutable uint64_t _object_size;
};

Status RandomAccessFileAdapter::read(uint64_t offset, Slice* res) const {
    if (UNLIKELY(offset > std::numeric_limits<int64_t>::max())) return Status::NotSupported("offset overflow");
    ASSIGN_OR_RETURN(res->size, _input->read_at(static_cast<int64_t>(offset), res->data, res->size));
    return Status::OK();
}

Status RandomAccessFileAdapter::read_at(uint64_t offset, const Slice& res) const {
    if (UNLIKELY(offset > std::numeric_limits<int64_t>::max())) return Status::NotSupported("offset overflow");
    return _input->read_at_fully(static_cast<int64_t>(offset), res.data, res.size);
}

Status RandomAccessFileAdapter::readv_at(uint64_t offset, const Slice* res, size_t res_cnt) const {
    for (size_t i = 0; i < res_cnt; i++) {
        RETURN_IF_ERROR(RandomAccessFileAdapter::read_at(offset, res[i]));
        offset += res[i].size;
    }
    return Status::OK();
}

Status RandomAccessFileAdapter::size(uint64_t* size) const {
    if (_object_size == 0) {
        ASSIGN_OR_RETURN(_object_size, _input->get_size());
    }
    *size = _object_size;
    return Status::OK();
}

StatusOr<std::unique_ptr<RandomAccessFile>> EnvS3::new_random_access_file(const std::string& path) {
    return EnvS3::new_random_access_file(RandomAccessFileOptions(), path);
}

// // Wrap a `starrocks::io::OutputStream` into `starrocks::WritableFile`.
class OutputStreamAdapter : public WritableFile {
public:
    explicit OutputStreamAdapter(std::unique_ptr<io::OutputStream> os, std::string name)
            : _os(std::move(os)), _name(std::move(name)), _bytes_written(0) {}

    Status append(const Slice& data) override {
        auto st = _os->write(data.data, data.size);
        _bytes_written += st.ok() ? data.size : 0;
        return st;
    }

    Status appendv(const Slice* data, size_t cnt) override {
        for (size_t i = 0; i < cnt; i++) {
            RETURN_IF_ERROR(append(data[i]));
        }
        return Status::OK();
    }

    Status pre_allocate(uint64_t size) override { return Status::NotSupported("OutputStreamAdapter::pre_allocate"); }

    Status close() override { return _os->close(); }

    // NOTE: unlike posix file, the file cannot be writen anymore after `flush`ed.
    Status flush(FlushMode mode) override { return _os->close(); }

    // NOTE: unlike posix file, the file cannot be writen anymore after `sync`ed.
    Status sync() override { return _os->close(); }

    uint64_t size() const { return _bytes_written; }

    const std::string& filename() const override { return _name; }

private:
    std::unique_ptr<io::OutputStream> _os;
    std::string _name;
    uint64_t _bytes_written;
};

bool operator==(const Aws::Client::ClientConfiguration& lhs, const Aws::Client::ClientConfiguration& rhs) {
    return lhs.endpointOverride == rhs.endpointOverride && lhs.region == rhs.region &&
           lhs.maxConnections == rhs.maxConnections && lhs.scheme == rhs.scheme;
}

class S3ClientFactory {
public:
    using ClientConfiguration = Aws::Client::ClientConfiguration;
    using S3Client = Aws::S3::S3Client;
    using S3ClientPtr = std::shared_ptr<S3Client>;

    static S3ClientFactory& instance() {
        static S3ClientFactory obj;
        return obj;
    }

    ~S3ClientFactory() = default;

    S3ClientFactory(const S3ClientFactory&) = delete;
    void operator=(const S3ClientFactory&) = delete;
    S3ClientFactory(S3ClientFactory&&) = delete;
    void operator=(S3ClientFactory&&) = delete;

    S3ClientPtr new_client(const ClientConfiguration& config);

private:
    S3ClientFactory();

    constexpr static int kMaxItems = 8;

    std::mutex _lock;
    int _items;
    // _configs[i] is the client configuration of |_clients[i].
    ClientConfiguration _configs[kMaxItems];
    S3ClientPtr _clients[kMaxItems];
    Random _rand;
};

S3ClientFactory::S3ClientFactory() : _items(0), _rand((int)::time(NULL)) {}

S3ClientFactory::S3ClientPtr S3ClientFactory::new_client(const ClientConfiguration& config) {
    std::lock_guard l(_lock);

    for (size_t i = 0; i < _items; i++) {
        if (_configs[i] == config) return _clients[i];
    }

    S3ClientPtr client;
    const auto& access_key_id = config::object_storage_access_key_id;
    const auto& secret_access_key = config::object_storage_secret_access_key;
    if (!access_key_id.empty() && !secret_access_key.empty()) {
        auto credentials = std::make_shared<Aws::Auth::SimpleAWSCredentialsProvider>(access_key_id, secret_access_key);
        client = std::make_shared<Aws::S3::S3Client>(credentials, config);
    } else {
        // if not cred provided, we can use default cred in aws profile.
        client = std::make_shared<Aws::S3::S3Client>(config);
    }

    if (UNLIKELY(_items >= kMaxItems)) {
        int idx = _rand.Uniform(kMaxItems);
        _configs[idx] = config;
        _clients[idx] = client;
    } else {
        _configs[_items] = config;
        _clients[_items] = client;
        _items++;
    }
    return client;
}

static std::shared_ptr<Aws::S3::S3Client> new_s3client(const S3URI& uri) {
    Aws::Client::ClientConfiguration config;
    config.scheme = Aws::Http::Scheme::HTTP; // TODO: use the scheme in uri
    if (!uri.endpoint().empty()) {
        config.endpointOverride = uri.endpoint();
    } else if (!config::object_storage_endpoint.empty()) {
        config.endpointOverride = config::object_storage_endpoint;
    }
    config.maxConnections = config::object_storage_max_connection;
    return S3ClientFactory::instance().new_client(config);
}

StatusOr<std::unique_ptr<RandomAccessFile>> EnvS3::new_random_access_file(const RandomAccessFileOptions& opts,
                                                                          const std::string& path) {
    S3URI uri;
    if (!uri.parse(path)) {
        return Status::InvalidArgument(fmt::format("Invalid S3 URI: {}", path));
    }
    auto client = new_s3client(uri);
    auto input_file = std::make_unique<io::S3RandomAccessFile>(std::move(client), uri.bucket(), uri.key());
    return std::make_unique<RandomAccessFileAdapter>(std::move(input_file), path);
}

StatusOr<std::unique_ptr<WritableFile>> EnvS3::new_writable_file(const std::string& fname) {
    return new_writable_file(WritableFileOptions(), fname);
}

StatusOr<std::unique_ptr<WritableFile>> EnvS3::new_writable_file(const WritableFileOptions& opts,
                                                                 const std::string& fname) {
    S3URI uri;
    if (!uri.parse(fname)) {
        return Status::InvalidArgument(fmt::format("Invalid S3 URI {}", fname));
    }
    if (opts.mode != Env::CREATE_OR_OPEN_WITH_TRUNCATE) {
        return Status::NotSupported(fmt::format("EnvS3 does not support open mode {}", opts.mode));
    }
    auto client = new_s3client(uri);
    auto ostream = std::make_unique<io::S3OutputStream>(std::move(client), uri.bucket(), uri.key(),
                                                        config::experimental_s3_max_single_part_size,
                                                        config::experimental_s3_min_upload_part_size);
    return std::make_unique<OutputStreamAdapter>(std::move(ostream), fname);
}

} // namespace starrocks

#endif
