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

    constexpr size_t BUFFER_SIZE = 4 * 1024 * 1024;
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
        if (curr == 5 || curr == rec.parent_record_num) break;
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

// Full LZNT1 Decompressor implementation (MS-XCA Section 2.5)
bool NTFSRecoverer::decompress_lznt1(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len) {
    const uint8_t* src_cur = src;
    const uint8_t* src_end = src + src_len;
    uint8_t* dst_cur = dst;
    uint8_t* dst_end = dst + dst_len;

    while (src_cur < src_end && dst_cur < dst_end) {
        if (src_cur + sizeof(uint16_t) > src_end) break;
        uint16_t header = *reinterpret_cast<const uint16_t*>(src_cur);
        src_cur += sizeof(uint16_t);

        if (header == 0) break; // End of chunk stream

        size_t chunk_size = (header & 0x0FFF) + 1;
        bool is_compressed = (header & 0x8000) != 0;

        if (src_cur + chunk_size > src_end) {
            chunk_size = src_end - src_cur;
        }

        const uint8_t* chunk_end = src_cur + chunk_size;

        if (!is_compressed) {
            size_t to_copy = std::min<size_t>(chunk_size, dst_end - dst_cur);
            std::memcpy(dst_cur, src_cur, to_copy);
            dst_cur += to_copy;
            src_cur = chunk_end;
        } else {
            uint8_t* chunk_dst_start = dst_cur;
            while (src_cur < chunk_end && dst_cur < dst_end) {
                uint16_t flags = 0x8000 | (*src_cur++);

                while ((flags & 0xFF00) && src_cur < chunk_end && dst_cur < dst_end) {
                    if (flags & 1) { // Backwards reference
                        if (src_cur + sizeof(uint16_t) > chunk_end) return false;
                        uint16_t code = *reinterpret_cast<const uint16_t*>(src_cur);
                        src_cur += sizeof(uint16_t);

                        // Find length / displacement bits
                        size_t displacement_bits = 12;
                        while (displacement_bits > 4) {
                            if ((1 << (displacement_bits - 1)) < (dst_cur - chunk_dst_start)) {
                                break;
                            }
                            displacement_bits--;
                        }
                        size_t length_bits = 16 - displacement_bits;
                        size_t code_length = (code & ((1 << length_bits) - 1)) + 3;
                        size_t code_displacement = (code >> length_bits) + 1;

                        if (dst_cur < chunk_dst_start + code_displacement) {
                            return false; // Malformed reference
                        }

                        const uint8_t* back_ptr = dst_cur - code_displacement;
                        for (size_t k = 0; k < code_length && dst_cur < dst_end; ++k) {
                            *dst_cur++ = *back_ptr++;
                        }
                    } else { // Literal byte
                        if (src_cur >= chunk_end) break;
                        *dst_cur++ = *src_cur++;
                    }
                    flags >>= 1;
                }
            }
            src_cur = chunk_end;
        }
    }
    return true;
}

bool NTFSRecoverer::extract_compressed_nonresident(const ParsedRecord& rec, std::ofstream& out) {
    uint32_t cu_clusters = 1 << (rec.compression_unit > 0 ? rec.compression_unit : 4); // Default 16 clusters
    uint64_t cu_bytes = static_cast<uint64_t>(cu_clusters) * cluster_size_;
    uint64_t bytes_written = 0;

    std::vector<uint8_t> compressed_unit_buf;
    std::vector<uint8_t> decompressed_unit_buf(cu_bytes, 0);

    size_t run_idx = 0;
    while (run_idx < rec.data_runs.size() && bytes_written < rec.file_size) {
        uint32_t clusters_in_cu = 0;
        compressed_unit_buf.clear();
        bool is_sparse_unit = true;

        // Group runs belonging to this Compression Unit
        while (run_idx < rec.data_runs.size() && clusters_in_cu < cu_clusters) {
            const auto& run = rec.data_runs[run_idx];
            uint32_t take_clusters = std::min<uint32_t>(run.length_in_clusters, cu_clusters - clusters_in_cu);

            if (run.lcn_offset != 0) {
                is_sparse_unit = false;
                size_t old_size = compressed_unit_buf.size();
                compressed_unit_buf.resize(old_size + (take_clusters * cluster_size_));

                off64_t dev_offset = partition_offset_ + (run.lcn_offset * cluster_size_);
                ::pread(fd_, compressed_unit_buf.data() + old_size, take_clusters * cluster_size_, dev_offset);
            }
            clusters_in_cu += take_clusters;
            run_idx++;
        }

        uint64_t write_size = std::min<uint64_t>(cu_bytes, rec.file_size - bytes_written);

        if (is_sparse_unit) {
            std::vector<char> zero_buf(write_size, 0);
            out.write(zero_buf.data(), write_size);
        } else if (clusters_in_cu < cu_clusters || compressed_unit_buf.size() < cu_bytes) {
            // Compressed unit: decompress with LZNT1
            std::fill(decompressed_unit_buf.begin(), decompressed_unit_buf.end(), 0);
            decompress_lznt1(compressed_unit_buf.data(), compressed_unit_buf.size(), decompressed_unit_buf.data(), cu_bytes);
            out.write(reinterpret_cast<const char*>(decompressed_unit_buf.data()), write_size);
        } else {
            // Unit did not compress: write raw bytes
            out.write(reinterpret_cast<const char*>(compressed_unit_buf.data()), write_size);
        }

        bytes_written += write_size;
    }
    return true;
}

bool NTFSRecoverer::extract_uncompressed_nonresident(const ParsedRecord& rec, std::ofstream& out) {
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

bool NTFSRecoverer::extract_file(const ParsedRecord& rec, const std::filesystem::path& out_path) {
    if (rec.is_directory) return false;

    std::filesystem::create_directories(out_path.parent_path());
    std::ofstream out(out_path, std::ios::binary);
    if (!out) return false;

    // Handle Resident Data
    if (rec.is_resident) {
        if (rec.is_compressed) {
            std::vector<uint8_t> decomp(rec.file_size, 0);
            decompress_lznt1(rec.resident_data.data(), rec.resident_data.size(), decomp.data(), rec.file_size);
            out.write(reinterpret_cast<const char*>(decomp.data()), rec.file_size);
        } else {
            out.write(reinterpret_cast<const char*>(rec.resident_data.data()), rec.resident_data.size());
        }
        return true;
    }

    // Handle Non-Resident Data
    if (rec.is_compressed) {
        return extract_compressed_nonresident(rec, out);
    } else {
        return extract_uncompressed_nonresident(rec, out);
    }
}
