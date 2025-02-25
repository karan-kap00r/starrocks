// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/util/file_cache.cpp

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "util/file_cache.h"

#include <utility>

#include "env/env.h"
#include "gutil/strings/substitute.h"

namespace starrocks {

template <class FileType>
FileCache<FileType>::FileCache(std::string cache_name, int max_open_files)
        : _cache_name(std::move(cache_name)), _cache(new_lru_cache(max_open_files)), _is_cache_own(true) {}

template <class FileType>
FileCache<FileType>::FileCache(std::string cache_name, std::shared_ptr<Cache> cache)
        : _cache_name(std::move(cache_name)), _cache(std::move(cache)) {}

template <class FileType>
bool FileCache<FileType>::lookup(const std::string& file_name, OpenedFileHandle<FileType>* file_handle) {
    DCHECK(_cache != nullptr);
    CacheKey key(file_name);
    auto lru_handle = _cache->lookup(key);
    if (lru_handle == nullptr) {
        return false;
    }
    *file_handle = OpenedFileHandle<FileType>(_cache.get(), lru_handle);
    return true;
}

template <class FileType>
void FileCache<FileType>::insert(const std::string& file_name, FileType* file,
                                 OpenedFileHandle<FileType>* file_handle) {
    DCHECK(_cache != nullptr);
    auto deleter = [](const CacheKey& key, void* value) { delete (FileType*)value; };
    CacheKey key(file_name);
    auto lru_handle = _cache->insert(key, file, 1, deleter);
    *file_handle = OpenedFileHandle<FileType>(_cache.get(), lru_handle);
}

template <class FileType>
void FileCache<FileType>::erase(const std::string& file_name) {
    DCHECK(_cache != nullptr);
    CacheKey key(file_name);
    _cache->erase(key);
}

// Explicit specialization for callers outside this compilation unit.
template class FileCache<RandomAccessFile>;

} // namespace starrocks
