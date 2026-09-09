#pragma once
#include <cstdint>
#include <string>
#include <vector>

#pragma pack(push, 1)
struct MFTHeaderRaw {
    char signature[4];        // "FILE" or "BAAD"
    uint16_t usa_offset;      // Update Sequence Array Offset
    uint16_t usa_count;       // Size in words of USA
    uint64_t lsn;             // $LogFile Sequence Number
    uint16_t sequence_number;
    uint16_t hard_link_count;
    uint16_t first_attr_offset;
    uint16_t flags;           // 0x01 = InUse, 0x02 = Directory
    uint32_t real_size;
    uint32_t allocated_size;
    uint64_t base_record_ref;
    uint16_t next_attr_id;
    uint16_t align;
    uint32_t record_number;
};

struct AttributeHeaderRaw {
    uint32_t type;            // e.g., 0x30 ($FILE_NAME), 0x80 ($DATA)
    uint32_t length;
    uint8_t non_resident;
    uint8_t name_length;
    uint16_t name_offset;
    uint16_t flags;           // 0x0001 = Compressed
    uint16_t attribute_id;
};

struct ResidentAttrHeaderRaw {
    AttributeHeaderRaw common;
    uint32_t value_length;
    uint16_t value_offset;
    uint8_t flags;
    uint8_t reserved;
};

struct NonResidentAttrHeaderRaw {
    AttributeHeaderRaw common;
    uint64_t start_vcn;
    uint64_t end_vcn;
    uint16_t data_runs_offset;
    uint16_t compression_unit_size; // 2^n clusters (typically 4 -> 16 clusters)
    uint32_t padding;
    uint64_t allocated_size;
    uint64_t real_size;
    uint64_t initialized_size;
};
#pragma pack(pop)

struct DataRun {
    uint64_t length_in_clusters;
    int64_t lcn_offset; // 0 = sparse
};

struct ParsedRecord {
    uint64_t record_num = 0;
    uint64_t parent_record_num = 0;
    std::string filename;
    bool is_directory = false;
    bool is_resident = true;
    bool is_compressed = false;
    uint16_t compression_unit = 0; // typically 4 (16 clusters)
    uint64_t file_size = 0;
    std::vector<uint8_t> resident_data;
    std::vector<DataRun> data_runs;
    bool valid = false;
};
