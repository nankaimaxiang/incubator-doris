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

#ifndef DORIS_BE_SRC_OLAP_HLL_H
#define DORIS_BE_SRC_OLAP_HLL_H

#include <math.h>
#include <stdio.h>

#include <map>
#include <set>
#include <string>

#include "gutil/macros.h"

namespace doris {

struct Slice;

const static int HLL_COLUMN_PRECISION = 14;
const static int HLL_ZERO_COUNT_BITS = (64 - HLL_COLUMN_PRECISION);
const static int HLL_EXPLICIT_INT64_NUM = 160;
const static int HLL_EXPLICIT_INT64_NUM_DOUBLE = HLL_EXPLICIT_INT64_NUM * 2;
const static int HLL_SPARSE_THRESHOLD = 4096;
const static int HLL_REGISTERS_COUNT = 16 * 1024;
// maximum size in byte of serialized HLL: type(1) + registers (2^14)
const static int HLL_COLUMN_DEFAULT_LEN = HLL_REGISTERS_COUNT + 1;

// 1 for type; 1 for hash values count; 8 for hash value
const static int HLL_SINGLE_VALUE_SIZE = 10;
const static int HLL_EMPTY_SIZE = 1;

// Hyperloglog distinct estimate algorithm.
// See these papers for more details.
// 1) Hyperloglog: The analysis of a near-optimal cardinality estimation
// algorithm (2007)
// 2) HyperLogLog in Practice (paper from google with some improvements)

// Each HLL value is a set of values. To save space, Doris store HLL value
// in different format according to its cardinality.
//
// HLL_DATA_EMPTY: when set is empty.
//
// HLL_DATA_EXPLICIT: when there is only few values in set, store these values explicit.
// If the number of hash values is not greater than 160, set is encoded in this format.
// The max space occupied is (1 + 1 + 160 * 8) = 1282. I don't know why 160 is chosen,
// maybe can be other number. If you are interested, you can try other number and see
// if it will be better.
//
// HLL_DATA_SPARSE: only store non-zero registers. If the number of non-zero registers
// is not greater than 4096, set is encoded in this format. The max space occupied is
// (1 + 4 + 3 * 4096) = 12293.
//
// HLL_DATA_FULL: most space-consuming, store all registers
//
// A HLL value will change in the sequence empty -> explicit -> sparse -> full, and not
// allow reverse.
//
// NOTE: This values are persisted in storage devices, so don't change exist
// enum values.
enum HllDataType {
    HLL_DATA_EMPTY = 0,
    HLL_DATA_EXPLICIT = 1,
    HLL_DATA_SPARSE = 2,
    HLL_DATA_FULL = 3,
};

class HyperLogLog {
public:
    HyperLogLog() = default;
    explicit HyperLogLog(uint64_t hash_value) : _type(HLL_DATA_EXPLICIT) {
        _explicit_data = new uint64_t[HLL_EXPLICIT_INT64_NUM_DOUBLE];
        _explicit_data[0] = hash_value;
        _explicit_data_num = 1;
    }

    HyperLogLog(const HyperLogLog& other) {
        this->_type = other._type;
        switch (other._type) {
        case HLL_DATA_EMPTY:
            break;
        case HLL_DATA_EXPLICIT: {
            this->_explicit_data_num = other._explicit_data_num;
            _explicit_data = new uint64_t[HLL_EXPLICIT_INT64_NUM_DOUBLE];
            memcpy(_explicit_data, other._explicit_data,
                   sizeof(*_explicit_data) * _explicit_data_num);
            break;
        }
        case HLL_DATA_SPARSE:
        case HLL_DATA_FULL: {
            _registers = new uint8_t[HLL_REGISTERS_COUNT];
            memcpy(_registers, other._registers, HLL_REGISTERS_COUNT);
            break;
        default:
            break;
        }
        }
    }

    HyperLogLog(HyperLogLog&& other) {
        this->_type = other._type;
        switch (other._type) {
        case HLL_DATA_EMPTY:
            break;
        case HLL_DATA_EXPLICIT: {
            this->_explicit_data_num = other._explicit_data_num;
            this->_explicit_data = other._explicit_data;
            other._explicit_data_num = 0;
            other._explicit_data = nullptr;
            other._type = HLL_DATA_EMPTY;
            break;
        }
        case HLL_DATA_SPARSE:
        case HLL_DATA_FULL: {
            this->_registers = other._registers;
            other._registers = nullptr;
            other._type = HLL_DATA_EMPTY;
            break;
        default:
            break;
        }
        }
    }

    HyperLogLog& operator=(HyperLogLog&& other) {
        if (this != &other) {
            if (_registers) {
                delete[] _registers;
                _registers = nullptr;
            }
            if (_explicit_data) {
                delete[] _explicit_data;
                _explicit_data = nullptr;
            }

            _explicit_data_num = 0;
            this->_type = other._type;
            switch (other._type) {
            case HLL_DATA_EMPTY:
                break;
            case HLL_DATA_EXPLICIT: {
                this->_explicit_data_num = other._explicit_data_num;
                this->_explicit_data = other._explicit_data;
                other._explicit_data_num = 0;
                other._explicit_data = nullptr;
                other._type = HLL_DATA_EMPTY;
                break;
            }
            case HLL_DATA_SPARSE:
            case HLL_DATA_FULL: {
                this->_registers = other._registers;
                other._registers = nullptr;
                other._type = HLL_DATA_EMPTY;
                break;
            default:
                break;
            }
            }
        }
        return *this;
    }

    HyperLogLog& operator=(const HyperLogLog& other) {
        if (this != &other) {
            if (_registers) {
                delete[] _registers;
                _registers = nullptr;
            }
            if (_explicit_data) {
                delete[] _explicit_data;
                _explicit_data = nullptr;
            }

            _explicit_data_num = 0;
            this->_type = other._type;
            switch (other._type) {
            case HLL_DATA_EMPTY:
                break;
            case HLL_DATA_EXPLICIT: {
                this->_explicit_data_num = other._explicit_data_num;
                _explicit_data = new uint64_t[HLL_EXPLICIT_INT64_NUM_DOUBLE];
                memcpy(_explicit_data, other._explicit_data,
                       sizeof(*_explicit_data) * _explicit_data_num);
                break;
            }
            case HLL_DATA_SPARSE:
            case HLL_DATA_FULL: {
                _registers = new uint8_t[HLL_REGISTERS_COUNT];
                memcpy(_registers, other._registers, HLL_REGISTERS_COUNT);
                break;
            default:
                break;
            }
            }
        }
        return *this;
    }

    explicit HyperLogLog(const Slice& src);

    ~HyperLogLog() { clear(); }

    void clear() {
        _type = HLL_DATA_EMPTY;
        delete[] _registers;
        _registers = nullptr;
        delete[] _explicit_data;
        _explicit_data = nullptr;
        _explicit_data_num = 0;
    }

    typedef uint8_t SetTypeValueType;
    typedef int32_t SparseLengthValueType;
    typedef uint16_t SparseIndexType;
    typedef uint8_t SparseValueType;

    // Add a hash value to this HLL value
    // NOTE: input must be a hash_value
    void update(uint64_t hash_value);

    void merge(const HyperLogLog& other);

    // Return max size of serialized binary
    size_t max_serialized_size() const;

    size_t memory_consumed() const {
        size_t size = sizeof(*this);
        if (_explicit_data) size += HLL_EXPLICIT_INT64_NUM_DOUBLE;
        if (_registers) size += HLL_REGISTERS_COUNT;
        return size;
    }

    // Input slice should has enough capacity for serialize, which
    // can be get through max_serialized_size(). If insufficient buffer
    // is given, this will cause process crash.
    // Return actual size of serialized binary.
    size_t serialize(uint8_t* dst) const;

    // Now, only empty HLL support this function.
    bool deserialize(const Slice& slice);

    int64_t estimate_cardinality() const;

    static std::string empty() {
        static HyperLogLog hll;
        std::string buf;
        buf.resize(HLL_EMPTY_SIZE);
        hll.serialize((uint8_t*)buf.c_str());
        return buf;
    }

    // Check if input slice is a valid serialized binary of HyperLogLog.
    // This function only check the encoded type in slice, whose complex
    // function is O(1).
    static bool is_valid(const Slice& slice);

    // only for debug
    std::string to_string() {
        switch (_type) {
        case HLL_DATA_EMPTY:
            return {};
        case HLL_DATA_EXPLICIT:
        case HLL_DATA_SPARSE:
        case HLL_DATA_FULL: {
            std::string str {"hash set size: "};
            str.append(std::to_string((size_t)_explicit_data_num));
            str.append("\ncardinality:\t");
            str.append(std::to_string(estimate_cardinality()));
            str.append("\ntype:\t");
            str.append(std::to_string(_type));
            return str;
        }
        default:
            return {};
        }
    }

private:
    HllDataType _type = HLL_DATA_EMPTY;

    uint32_t _explicit_data_num = 0;
    uint64_t* _explicit_data = nullptr;

    // This field is much space consuming(HLL_REGISTERS_COUNT), we create
    // it only when it is really needed.
    uint8_t* _registers = nullptr;

private:

    void _convert_explicit_to_register();

    // update one hash value into this registers
    inline void _update_registers(uint64_t hash_value) {
        // Use the lower bits to index into the number of streams and then
        // find the first 1 bit after the index bits.
        int idx = hash_value % HLL_REGISTERS_COUNT;
        hash_value >>= HLL_COLUMN_PRECISION;
        // make sure max first_one_bit is HLL_ZERO_COUNT_BITS + 1
        hash_value |= ((uint64_t)1 << HLL_ZERO_COUNT_BITS);
        uint8_t first_one_bit = __builtin_ctzl(hash_value) + 1;
        _registers[idx] = _registers[idx] > first_one_bit ? _registers[idx] : first_one_bit;
    }

    // absorb other registers into this registers
    void _merge_registers(const uint8_t* other) {
        for (int i = 0; i < HLL_REGISTERS_COUNT; ++i) {
            _registers[i] = _registers[i] < other[i] ? other[i] : _registers[i];
        }
    }

    bool _explicit_data_insert(uint64_t data) {
        //find insert pos
        int32_t i = (int32_t)_explicit_data_num - 1;
        while (i >= 0) {
            if (_explicit_data[i] == data) {
                return false;
            } else if (_explicit_data[i] < data) {
                break;
            } else {
                --i;
            }
        }

        ++i; //now, i is the insert position

        size_t n = (_explicit_data_num - i) * sizeof(*_explicit_data);
        if (n) {
            memmove(_explicit_data + i + 1, _explicit_data + i, n);
        }

        //insert data
        _explicit_data[i] = data;
        _explicit_data_num++;
        return true;
    }
};

// todo(kks): remove this when dpp_sink class was removed
class HllSetResolver {
public:
    HllSetResolver()
            : _buf_ref(nullptr),
              _buf_len(0),
              _set_type(HLL_DATA_EMPTY),
              _full_value_position(nullptr),
              _explicit_value(nullptr),
              _explicit_num(0) {}

    ~HllSetResolver() {}

    typedef uint8_t SetTypeValueType;
    typedef uint8_t ExplicitLengthValueType;
    typedef int32_t SparseLengthValueType;
    typedef uint16_t SparseIndexType;
    typedef uint8_t SparseValueType;

    // only save pointer
    void init(char* buf, int len) {
        this->_buf_ref = buf;
        this->_buf_len = len;
    }

    // hll set type
    HllDataType get_hll_data_type() { return _set_type; }

    // explicit value num
    int get_explicit_count() { return (int)_explicit_num; }

    // get explicit index value 64bit
    uint64_t get_explicit_value(int index) {
        if (index >= _explicit_num) {
            return -1;
        }
        return _explicit_value[index];
    }

    // get full register value
    char* get_full_value() { return _full_value_position; }

    // get (index, value) map
    std::map<SparseIndexType, SparseValueType>& get_sparse_map() { return _sparse_map; }

    // parse set , call after copy() or init()
    void parse();

private:
    char* _buf_ref;        // set
    int _buf_len;          // set len
    HllDataType _set_type; //set type
    char* _full_value_position;
    uint64_t* _explicit_value;
    ExplicitLengthValueType _explicit_num;
    std::map<SparseIndexType, SparseValueType> _sparse_map;
    SparseLengthValueType* _sparse_count;
};

// todo(kks): remove this when dpp_sink class was removed
class HllSetHelper {
public:
    static void set_sparse(char* result, const std::map<int, uint8_t>& index_to_value, int& len);
    static void set_explicit(char* result, const std::set<uint64_t>& hash_value_set, int& len);
    static void set_full(char* result, const std::map<int, uint8_t>& index_to_value,
                         const int set_len, int& len);
};

} // namespace doris

#endif // DORIS_BE_SRC_OLAP_HLL_H
