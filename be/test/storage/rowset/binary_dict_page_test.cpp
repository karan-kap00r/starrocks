// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/test/olap/rowset/segment_v2/binary_dict_page_test.cpp

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

#include "storage/rowset/binary_dict_page.h"

#include <gtest/gtest.h>

#include <fstream>
#include <iostream>

#include "common/logging.h"
#include "gen_cpp/segment.pb.h"
#include "runtime/mem_pool.h"
#include "runtime/mem_tracker.h"
#include "storage/olap_common.h"
#include "storage/rowset/binary_plain_page.h"
#include "storage/rowset/page_builder.h"
#include "storage/rowset/page_decoder.h"
#include "storage/rowset/storage_page_decoder.h"
#include "storage/types.h"
#include "storage/vectorized/chunk_helper.h"
#include "util/debug_util.h"

namespace starrocks {

class BinaryDictPageTest : public testing::Test {
public:
    void test_by_small_data_size(const std::vector<Slice>& slices) {
        // encode
        PageBuilderOptions options;
        options.data_page_size = 256 * 1024;
        options.dict_page_size = 256 * 1024;
        BinaryDictPageBuilder page_builder(options);
        size_t count = slices.size();
        Status status;

        const Slice* ptr = &slices[0];
        count = page_builder.add(reinterpret_cast<const uint8_t*>(ptr), count);

        auto s = page_builder.finish()->build();
        ASSERT_EQ(slices.size(), page_builder.count());
        ASSERT_FALSE(page_builder.is_page_full());

        //check first value and last value
        Slice first_value;
        page_builder.get_first_value(&first_value);
        ASSERT_EQ(slices[0], first_value);
        Slice last_value;
        page_builder.get_last_value(&last_value);
        ASSERT_EQ(slices[count - 1], last_value);

        // construct dict page
        OwnedSlice dict_slice = page_builder.get_dictionary_page()->build();
        PageDecoderOptions dict_decoder_options;
        auto dict_page_decoder = std::make_unique<BinaryPlainPageDecoder<OLAP_FIELD_TYPE_VARCHAR>>(
                dict_slice.slice(), dict_decoder_options);
        status = dict_page_decoder->init();
        ASSERT_TRUE(status.ok());
        // because every slice is unique
        ASSERT_EQ(slices.size(), dict_page_decoder->count());

        // decode
        Slice encoded_data = s.slice();
        PageFooterPB footer;
        footer.set_type(DATA_PAGE);
        DataPageFooterPB* data_page_footer = footer.mutable_data_page_footer();
        data_page_footer->set_nullmap_size(0);
        std::unique_ptr<char[]> page = nullptr;

        Status st = StoragePageDecoder::decode_page(&footer, 0, starrocks::DICT_ENCODING, &page, &encoded_data);
        ASSERT_TRUE(st.ok());

        PageDecoderOptions decoder_options;
        BinaryDictPageDecoder<OLAP_FIELD_TYPE_VARCHAR> page_decoder(encoded_data, decoder_options);
        page_decoder.set_dict_decoder(dict_page_decoder.get());

        status = page_decoder.init();
        ASSERT_TRUE(status.ok());
        ASSERT_EQ(slices.size(), page_decoder.count());

        //check values
        MemPool pool;
        TypeInfoPtr type_info = get_type_info(OLAP_FIELD_TYPE_VARCHAR);
        size_t size = slices.size();
        std::unique_ptr<ColumnVectorBatch> cvb;
        ColumnVectorBatch::create(size, false, type_info, nullptr, &cvb);
        ColumnBlock column_block(cvb.get(), &pool);
        ColumnBlockView block_view(&column_block);

        status = page_decoder.next_batch(&size, &block_view);
        Slice* values = reinterpret_cast<Slice*>(column_block.data());
        ASSERT_TRUE(status.ok());
        ASSERT_EQ(slices.size(), size);
        ASSERT_EQ("Individual", values[0].to_string());
        ASSERT_EQ("Lifetime", values[1].to_string());
        ASSERT_EQ("Objective", values[2].to_string());
        ASSERT_EQ("Value", values[3].to_string());
        ASSERT_EQ("Evolution", values[4].to_string());
        ASSERT_EQ("Nature", values[5].to_string());
        ASSERT_EQ("Captain", values[6].to_string());
        ASSERT_EQ("Xmas", values[7].to_string());

        status = page_decoder.seek_to_position_in_page(5);
        ASSERT_TRUE(status.ok()) << status.to_string();
        status = page_decoder.next_batch(&size, &block_view);
        ASSERT_TRUE(status.ok()) << status.to_string();
        // read 3 items
        ASSERT_EQ(3, size);
        ASSERT_EQ("Nature", values[0].to_string());
        ASSERT_EQ("Captain", values[1].to_string());
        ASSERT_EQ("Xmas", values[2].to_string());

        page_decoder.seek_to_position_in_page(0);
        ASSERT_EQ(0, page_decoder.current_index());
        auto column = vectorized::ChunkHelper::column_from_field_type(OLAP_FIELD_TYPE_VARCHAR, false);
        vectorized::SparseRange read_range;
        read_range.add(vectorized::Range(0, 2));
        read_range.add(vectorized::Range(4, 7));
        status = page_decoder.next_batch(read_range, column.get());
        ASSERT_TRUE(status.ok());
        ASSERT_EQ(5, column->size());
        ASSERT_EQ("Individual", column->get(0).get_slice().to_string());
        ASSERT_EQ("Lifetime", column->get(1).get_slice().to_string());
        ASSERT_EQ("Evolution", column->get(2).get_slice().to_string());
        ASSERT_EQ("Nature", column->get(3).get_slice().to_string());
        ASSERT_EQ("Captain", column->get(4).get_slice().to_string());
    }

    void test_with_large_data_size(const std::vector<Slice>& contents) {
        Status status;
        // encode
        PageBuilderOptions options;
        // page size: 16M
        options.data_page_size = 1 * 1024 * 1024;
        options.dict_page_size = 1 * 1024 * 1024;
        BinaryDictPageBuilder page_builder(options);
        size_t count = contents.size();
        std::vector<OwnedSlice> results;
        std::vector<size_t> page_start_ids;
        size_t total_size = 0;
        page_start_ids.push_back(0);
        for (int i = 0; i < count;) {
            size_t add_num = 1;
            const Slice* ptr = &contents[i];
            add_num = page_builder.add(reinterpret_cast<const uint8_t*>(ptr), add_num);
            if (page_builder.is_page_full()) {
                OwnedSlice s = page_builder.finish()->build();
                total_size += s.slice().size;
                results.emplace_back(std::move(s));
                page_builder.reset();
                page_start_ids.push_back(i + 1);
            }
            i += add_num;
        }
        OwnedSlice s = page_builder.finish()->build();
        total_size += s.slice().size;
        results.emplace_back(std::move(s));

        page_start_ids.push_back(count);

        OwnedSlice dict_slice = page_builder.get_dictionary_page()->build();
        size_t data_size = total_size;
        total_size += dict_slice.slice().size;
        ASSERT_TRUE(status.ok());
        LOG(INFO) << "total size:" << total_size << ", data size:" << data_size
                  << ", dict size:" << dict_slice.slice().size << " result page size:" << results.size();

        // validate
        // random 100 times to validate
        srand(time(nullptr));
        for (int i = 0; i < 100; ++i) {
            int slice_index = random() % results.size();
            //int slice_index = 1;
            PageDecoderOptions dict_decoder_options;
            auto dict_page_decoder = std::make_unique<BinaryPlainPageDecoder<OLAP_FIELD_TYPE_VARCHAR>>(
                    dict_slice.slice(), dict_decoder_options);
            status = dict_page_decoder->init();
            ASSERT_TRUE(status.ok());

            // decode
            Slice encoded_data = results[slice_index].slice();
            PageFooterPB footer;
            footer.set_type(DATA_PAGE);
            DataPageFooterPB* data_page_footer = footer.mutable_data_page_footer();
            data_page_footer->set_nullmap_size(0);
            std::unique_ptr<char[]> page = nullptr;

            Status st = StoragePageDecoder::decode_page(&footer, 0, starrocks::DICT_ENCODING, &page, &encoded_data);
            ASSERT_TRUE(st.ok());

            PageDecoderOptions decoder_options;
            BinaryDictPageDecoder<OLAP_FIELD_TYPE_VARCHAR> page_decoder(encoded_data, decoder_options);
            status = page_decoder.init();
            page_decoder.set_dict_decoder(dict_page_decoder.get());
            ASSERT_TRUE(status.ok());

            //check values
            MemPool pool;
            TypeInfoPtr type_info = get_type_info(OLAP_FIELD_TYPE_VARCHAR);
            std::unique_ptr<ColumnVectorBatch> cvb;
            ColumnVectorBatch::create(1, false, type_info, nullptr, &cvb);
            ColumnBlock column_block(cvb.get(), &pool);
            ColumnBlockView block_view(&column_block);
            Slice* values = reinterpret_cast<Slice*>(column_block.data());

            size_t num = 1;
            size_t pos = random() % (page_start_ids[slice_index + 1] - page_start_ids[slice_index]);
            //size_t pos = 613631;
            status = page_decoder.seek_to_position_in_page(pos);
            ASSERT_TRUE(status.ok());
            status = page_decoder.next_batch(&num, &block_view);
            ASSERT_TRUE(status.ok());
            std::string expect = contents[page_start_ids[slice_index] + pos].to_string();
            std::string actual = values[0].to_string();
            ASSERT_EQ(expect, actual) << "slice index:" << slice_index << ", pos:" << pos
                                      << ", expect:" << hexdump((char*)expect.data(), expect.size())
                                      << ", actual:" << hexdump((char*)actual.data(), actual.size())
                                      << ", line number:" << page_start_ids[slice_index] + pos + 1;

            status = page_decoder.seek_to_position_in_page(0);
            ASSERT_TRUE(status.ok());
            size_t slice_num = page_start_ids[slice_index + 1] - page_start_ids[slice_index];
            auto dst = vectorized::ChunkHelper::column_from_field_type(OLAP_FIELD_TYPE_VARCHAR, false);
            vectorized::SparseRange read_range;
            read_range.add(vectorized::Range(0, slice_num / 3));
            read_range.add(vectorized::Range(slice_num / 2, (slice_num * 2 / 3)));
            read_range.add(vectorized::Range((slice_num * 3 / 4), slice_num));
            size_t read_num = read_range.span_size();

            status = page_decoder.next_batch(read_range, dst.get());
            ASSERT_TRUE(status.ok());
            ASSERT_EQ(read_num, dst->size());

            size_t offset = 0;
            vectorized::SparseRangeIterator read_iter = read_range.new_iterator();
            while (read_iter.has_more()) {
                vectorized::Range r = read_iter.next(read_num);
                for (int i = 0; i < r.span_size(); ++i) {
                    std::string expect = contents[page_start_ids[slice_index] + r.begin() + i].to_string();
                    std::string actual = dst->get(i + offset).get_slice().to_string();
                    ASSERT_EQ(expect, actual) << "slice index:" << slice_index << ", pos:" << offset + i
                                              << ", expect:" << hexdump((char*)expect.data(), expect.size())
                                              << ", actual:" << hexdump((char*)actual.data(), actual.size())
                                              << ", line number:" << page_start_ids[slice_index] + offset + i + 1;
                }
                offset += r.span_size();
            }
        }
    }
};

// NOLINTNEXTLINE
TEST_F(BinaryDictPageTest, TestBySmallDataSize) {
    std::vector<Slice> slices;
    slices.emplace_back("Individual");
    slices.emplace_back("Lifetime");
    slices.emplace_back("Objective");
    slices.emplace_back("Value");
    slices.emplace_back("Evolution");
    slices.emplace_back("Nature");
    slices.emplace_back("Captain");
    slices.emplace_back("Xmas");
    test_by_small_data_size(slices);
}

// NOLINTNEXTLINE
TEST_F(BinaryDictPageTest, TestEncodingRatio) {
    std::vector<Slice> slices;
    std::vector<std::string> src_strings;
    std::string file = "./be/test/storage/test_data/dict_encoding_data.dat";
    std::string line;
    std::ifstream infile(file.c_str());
    while (getline(infile, line)) {
        src_strings.emplace_back(line);
    }
    for (int i = 0; i < 10000; ++i) {
        for (const auto& src_string : src_strings) {
            slices.push_back(src_string);
        }
    }

    LOG(INFO) << "source line number:" << slices.size();
    test_with_large_data_size(slices);
}

} // namespace starrocks
