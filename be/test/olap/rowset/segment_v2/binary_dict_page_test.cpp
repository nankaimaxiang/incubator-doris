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

#include "olap/rowset/segment_v2/binary_dict_page.h"

#include <gtest/gtest.h>

#include <fstream>
#include <iostream>

#include "common/logging.h"
#include "olap/olap_common.h"
#include "olap/rowset/segment_v2/binary_plain_page.h"
#include "olap/rowset/segment_v2/page_builder.h"
#include "olap/rowset/segment_v2/page_decoder.h"
#include "olap/types.h"
#include "runtime/mem_pool.h"
#include "runtime/mem_tracker.h"
#include "util/debug_util.h"
#include "test_util/test_util.h"

namespace doris {
namespace segment_v2 {

class BinaryDictPageTest : public testing::Test {
public:
    void test_by_small_data_size(const std::vector<Slice>& slices) {
        // encode
        PageBuilderOptions options;
        options.data_page_size = 256 * 1024;
        options.dict_page_size = 256 * 1024;
        BinaryDictPageBuilder page_builder(options);
        size_t count = slices.size();

        const Slice* ptr = &slices[0];
        Status ret = page_builder.add(reinterpret_cast<const uint8_t*>(ptr), &count);
        ASSERT_TRUE(ret.ok());

        OwnedSlice s = page_builder.finish();
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
        OwnedSlice dict_slice;
        Status status = page_builder.get_dictionary_page(&dict_slice);
        ASSERT_TRUE(status.ok());
        PageDecoderOptions dict_decoder_options;
        std::unique_ptr<BinaryPlainPageDecoder> dict_page_decoder(
                new BinaryPlainPageDecoder(dict_slice.slice(), dict_decoder_options));
        status = dict_page_decoder->init();
        ASSERT_TRUE(status.ok());
        // because every slice is unique
        ASSERT_EQ(slices.size(), dict_page_decoder->count());

        StringRef dict_word_info[dict_page_decoder->_num_elems];
        dict_page_decoder->get_dict_word_info(dict_word_info);

        // decode
        PageDecoderOptions decoder_options;
        BinaryDictPageDecoder page_decoder(s.slice(), decoder_options);

        page_decoder.set_dict_decoder(dict_page_decoder.get(), dict_word_info);

        status = page_decoder.init();
        ASSERT_TRUE(status.ok());
        ASSERT_EQ(slices.size(), page_decoder.count());

        //check values
        auto tracker = std::make_shared<MemTracker>();
        MemPool pool(tracker.get());
        auto type_info = get_scalar_type_info(OLAP_FIELD_TYPE_VARCHAR);
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
        status = page_decoder.next_batch(&size, &block_view);
        ASSERT_TRUE(status.ok());
        // read 3 items
        ASSERT_EQ(3, size);
        ASSERT_EQ("Nature", values[0].to_string());
        ASSERT_EQ("Captain", values[1].to_string());
        ASSERT_EQ("Xmas", values[2].to_string());
    }

    void test_with_large_data_size(const std::vector<Slice>& contents) {
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
            Status ret = page_builder.add(reinterpret_cast<const uint8_t*>(ptr), &add_num);
            ASSERT_TRUE(ret.ok());
            if (page_builder.is_page_full()) {
                OwnedSlice s = page_builder.finish();
                total_size += s.slice().size;
                results.emplace_back(std::move(s));
                page_builder.reset();
                page_start_ids.push_back(i + 1);
            }
            i += add_num;
        }
        OwnedSlice s = page_builder.finish();
        total_size += s.slice().size;
        results.emplace_back(std::move(s));

        page_start_ids.push_back(count);

        OwnedSlice dict_slice;
        Status status = page_builder.get_dictionary_page(&dict_slice);
        size_t data_size = total_size;
        total_size += dict_slice.slice().size;
        ASSERT_TRUE(status.ok());
        LOG(INFO) << "total size:" << total_size << ", data size:" << data_size
                  << ", dict size:" << dict_slice.slice().size
                  << " result page size:" << results.size();

        // validate
        // random 100 times to validate
        srand(time(nullptr));
        for (int i = 0; i < 100; ++i) {
            int slice_index = random() % results.size();
            //int slice_index = 1;
            PageDecoderOptions dict_decoder_options;
            std::unique_ptr<BinaryPlainPageDecoder> dict_page_decoder(
                    new BinaryPlainPageDecoder(dict_slice.slice(), dict_decoder_options));
            status = dict_page_decoder->init();
            ASSERT_TRUE(status.ok());

            StringRef dict_word_info[dict_page_decoder->_num_elems];
            dict_page_decoder->get_dict_word_info(dict_word_info);

            // decode
            PageDecoderOptions decoder_options;
            BinaryDictPageDecoder page_decoder(results[slice_index].slice(), decoder_options);
            status = page_decoder.init();

            page_decoder.set_dict_decoder(dict_page_decoder.get(), dict_word_info);
            ASSERT_TRUE(status.ok());

            //check values
            auto tracker = std::make_shared<MemTracker>();
            MemPool pool(tracker.get());
            auto type_info = get_scalar_type_info(OLAP_FIELD_TYPE_VARCHAR);
            std::unique_ptr<ColumnVectorBatch> cvb;
            ColumnVectorBatch::create(1, false, type_info, nullptr, &cvb);
            ColumnBlock column_block(cvb.get(), &pool);
            ColumnBlockView block_view(&column_block);
            Slice* values = reinterpret_cast<Slice*>(column_block.data());

            size_t num = 1;
            size_t pos = random() % (page_start_ids[slice_index + 1] - page_start_ids[slice_index]);
            //size_t pos = 613631;
            status = page_decoder.seek_to_position_in_page(pos);
            status = page_decoder.next_batch(&num, &block_view);
            ASSERT_TRUE(status.ok());
            std::string expect = contents[page_start_ids[slice_index] + pos].to_string();
            std::string actual = values[0].to_string();
            ASSERT_EQ(expect, actual) << "slice index:" << slice_index << ", pos:" << pos
                                      << ", expect:" << hexdump((char*)expect.data(), expect.size())
                                      << ", actual:" << hexdump((char*)actual.data(), actual.size())
                                      << ", line number:" << page_start_ids[slice_index] + pos + 1;
        }
    }
};

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

TEST_F(BinaryDictPageTest, TestEncodingRatio) {
    std::vector<Slice> slices;
    std::vector<std::string> src_strings;
    std::string file = "./be/test/olap/test_data/dict_encoding_data.dat";
    std::string line;
    std::ifstream infile(file.c_str());
    while (getline(infile, line)) {
        src_strings.emplace_back(line);
    }
    for (int i = 0; i < LOOP_LESS_OR_MORE(100, 10000); ++i) {
        for (const auto& src_string : src_strings) {
            slices.push_back(src_string);
        }
    }

    LOG(INFO) << "source line number:" << slices.size();
    test_with_large_data_size(slices);
}

} // namespace segment_v2
} // namespace doris

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
