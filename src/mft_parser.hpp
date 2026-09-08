#pragma once
#include "ntfs_types.hpp"
#include <vector>
#include <cstdint>

class MFTParser {
public:
    static bool apply_fixup(uint8_t* buffer, size_t record_size = 1024);
    static ParsedRecord parse_record(const uint8_t* buffer, size_t record_size = 1024);
private:
    static std::vector<DataRun> parse_data_runs(const uint8_t* run_ptr, size_t max_len);
    static std::string utf16le_to_utf8(const uint16_t* str, size_t len);
};