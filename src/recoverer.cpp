#pragma once
#include "ntfs_types.hpp"
#include <unordered_map>
#include <string>
#include <filesystem>
#include <functional>
#include <vector>

struct RecoveryItem {
    ParsedRecord record;
    std::filesystem::path target_path;
    bool is_directory;
};

class NTFSRecoverer {
public:
    NTFSRecoverer();
    ~NTFSRecoverer();

    bool open_device(const std::string& path, uint64_t partition_offset = 0, uint32_t cluster_size = 4096);
    void close_device();

    void scan_mft(std::function<void(size_t records_found)> progress_callback);
    bool extract_file(const ParsedRecord& rec, const std::filesystem::path& out_path);

    void collect_items(uint64_t record_num, 
                       const std::filesystem::path& base_path, 
                       std::vector<RecoveryItem>& items) const;

    std::string get_full_path(uint64_t record_num) const;
    const std::unordered_map<uint64_t, ParsedRecord>& get_records() const { return records_; }
    const std::unordered_map<uint64_t, std::vector<uint64_t>>& get_tree() const { return tree_; }

    static bool decompress_lznt1(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len);

private:
    bool extract_compressed_nonresident(const ParsedRecord& rec, std::ofstream& out);
    bool extract_uncompressed_nonresident(const ParsedRecord& rec, std::ofstream& out);

    int fd_ = -1;
    uint64_t partition_offset_ = 0;
    uint32_t cluster_size_ = 4096;
    std::unordered_map<uint64_t, ParsedRecord> records_;
    std::unordered_map<uint64_t, std::vector<uint64_t>> tree_;
};
