#include "recoverer.hpp"
#include "mft_parser.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <set>
#include <cstring>
#include <algorithm>

NTFSRecoverer::NTFSRecoverer() = default;

NTFSRecoverer::~NTFSRecoverer() {
    close_device();
}

bool NTFSRecoverer::open_device(const std::string& path, uint64_t partition_offset, uint32_t cluster_size) {
    close_device();
    fd_ = ::open(path.c_str(), O_RDONLY | O_LARGEFILE);
    if (fd_ < 0) return false;

    partition_offset_ = partition_offset;
    cluster_size_ = cluster_size;
    return true;
}

void NTFSRecoverer::close_device() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    records_.clear();
    tree_.clear();
}

void NTFSRecoverer::scan_mft(std::function<void(size_t)> progress_callback) {
    if (fd_ < 0) return;

    constexpr size_t BUFFER_SIZE = 4 * 1024 * 1024; // 4 MB Read Buffers
    std::vector<uint8_t> buffer(BUFFER_SIZE);
    uint64_t current_offset = partition_offset_;
    ssize_t bytes_read = 0;

    while ((bytes_read = ::pread(fd_, buffer.data(), BUFFER_SIZE, current_offset)) > 0) {
        for (size_t i = 0; i + 1024 <= static_cast<size_t>(bytes_read); i += 512) {
            if (std::memcmp(buffer.data() + i, "FILE", 4) == 0) {
                ParsedRecord rec = MFTParser::parse_record(buffer.data() + i, 1024);
                if (rec.valid) {
                    records_[rec.record_num] = rec;
                }
            }
        }
        current_offset += bytes_read;
        if (progress_callback) progress_callback(records_.size());
    }

    // Build directory tree
    tree_.clear();
    for (const auto& [id, rec] : records_) {
        tree_[rec.parent_record_num].push_back(id);
    }
}

std::string NTFSRecoverer::get_full_path(uint64_t record_num) const {
    std::string path;
    uint64_t curr = record_num;
    std::set<uint64_t> visited;

    while (records_.count(curr) && !visited.count(curr)) {
        visited.insert(curr);
        const auto& rec = records_.at(curr);
        path = "/" + rec.filename + path;
        if (curr == 5 || curr == rec.parent_record_num) break; // Record 5 is NTFS Root
        curr = rec.parent_record_num;
    }

    if (curr != 5 && curr != record_num) {
        path = "/[Orphaned Files]" + path;
    }
    return path;
}

void NTFSRecoverer::collect_items(uint64_t record_num, 
                                 const std::filesystem::path& base_path, 
                                 std::vector<RecoveryItem>& items) const {
    if (!records_.count(record_num)) return;
    const auto& rec = records_.at(record_num);
    std::filesystem::path item_path = base_path / rec.filename;

    items.push_back({rec, item_path, rec.is_directory});

    if (rec.is_directory && tree_.count(record_num)) {
        for (uint64_t child_id : tree_.at(record_num)) {
            if (child_id != record_num) {
                collect_items(child_id, item_path, items);
            }
        }
    }
}

bool NTFSRecoverer::extract_file(const ParsedRecord& rec, const std::filesystem::path& out_path) {
    if (rec.is_directory) return false;

    std::filesystem::create_directories(out_path.parent_path());
    std::ofstream out(out_path, std::ios::binary);
    if (!out) return false;

    if (rec.is_resident) {
        out.write(reinterpret_cast<const char*>(rec.resident_data.data()), rec.resident_data.size());
        return true;
    }

    uint64_t bytes_written = 0;
    std::vector<uint8_t> cluster_buf(cluster_size_);

    for (const auto& run : rec.data_runs) {
        if (bytes_written >= rec.file_size) break;

        for (uint64_t c = 0; c < run.length_in_clusters; ++c) {
            if (bytes_written >= rec.file_size) break;

            uint64_t to_write = std::min<uint64_t>(cluster_size_, rec.file_size - bytes_written);

            if (run.lcn_offset == 0) {
                std::vector<char> zero_buf(to_write, 0);
                out.write(zero_buf.data(), to_write);
            } else {
                uint64_t cluster_idx = run.lcn_offset + c;
                off64_t dev_offset = partition_offset_ + (cluster_idx * cluster_size_);
                
                ssize_t read_bytes = ::pread(fd_, cluster_buf.data(), cluster_size_, dev_offset);
                if (read_bytes <= 0) break;

                out.write(reinterpret_cast<const char*>(cluster_buf.data()), to_write);
            }
            bytes_written += to_write;
        }
    }
    return true;
}