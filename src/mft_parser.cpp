#include "mft_parser.hpp"
#include <cstring>

bool MFTParser::apply_fixup(uint8_t* buffer, size_t record_size) {
    if (record_size < 1024) return false;
    auto* header = reinterpret_cast<MFTHeaderRaw*>(buffer);
    
    if (std::memcmp(header->signature, "FILE", 4) != 0) return false;

    uint16_t usa_offset = header->usa_offset;
    uint16_t usa_count = header->usa_count;

    if (usa_offset + (usa_count * 2) > record_size) return false;

    uint16_t check_val = *reinterpret_cast<uint16_t*>(buffer + usa_offset);

    for (size_t i = 1; i < usa_count; ++i) {
        size_t sector_end = (i * 512) - 2;
        uint16_t* sector_fixup_pos = reinterpret_cast<uint16_t*>(buffer + sector_end);

        if (*sector_fixup_pos != check_val) {
            return false;
        }

        uint16_t replacement = *reinterpret_cast<uint16_t*>(buffer + usa_offset + (i * 2));
        *sector_fixup_pos = replacement;
    }
    return true;
}

std::string MFTParser::utf16le_to_utf8(const uint16_t* str, size_t len) {
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        uint16_t c = str[i];
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back('?');
        }
    }
    return out;
}

std::vector<DataRun> MFTParser::parse_data_runs(const uint8_t* run_ptr, size_t max_len) {
    std::vector<DataRun> runs;
    size_t idx = 0;
    int64_t prev_lcn = 0;

    while (idx < max_len && run_ptr[idx] != 0) {
        uint8_t header = run_ptr[idx++];
        uint8_t len_bytes = header & 0x0F;
        uint8_t off_bytes = (header >> 4) & 0x0F;

        if (idx + len_bytes + off_bytes > max_len) break;

        uint64_t run_len = 0;
        for (int i = 0; i < len_bytes; ++i) {
            run_len |= static_cast<uint64_t>(run_ptr[idx++]) << (i * 8);
        }

        int64_t run_offset = 0;
        if (off_bytes > 0) {
            for (int i = 0; i < off_bytes; ++i) {
                run_offset |= static_cast<int64_t>(run_ptr[idx++]) << (i * 8);
            }
            if (run_ptr[idx - 1] & 0x80) {
                for (int i = off_bytes; i < 8; ++i) {
                    run_offset |= (static_cast<int64_t>(0xFF) << (i * 8));
                }
            }
            prev_lcn += run_offset;
        } else {
            prev_lcn = 0;
        }

        runs.push_back({run_len, prev_lcn});
    }
    return runs;
}

ParsedRecord MFTParser::parse_record(const uint8_t* buffer, size_t record_size) {
    ParsedRecord rec;
    std::vector<uint8_t> work_buf(buffer, buffer + record_size);

    if (!apply_fixup(work_buf.data(), record_size)) {
        return rec;
    }

    auto* header = reinterpret_cast<MFTHeaderRaw*>(work_buf.data());
    rec.record_num = header->record_number;
    rec.is_directory = (header->flags & 0x02) != 0;

    uint32_t offset = header->first_attr_offset;
    while (offset + sizeof(AttributeHeaderRaw) <= record_size) {
        auto* attr = reinterpret_cast<AttributeHeaderRaw*>(work_buf.data() + offset);
        if (attr->type == 0xFFFFFFFF || attr->length == 0) break;
        if (offset + attr->length > record_size) break;

        if (attr->type == 0x30 && attr->non_resident == 0) { // $FILE_NAME
            auto* res = reinterpret_cast<ResidentAttrHeaderRaw*>(attr);
            const uint8_t* payload = work_buf.data() + offset + res->value_offset;
            
            uint64_t parent_ref = *reinterpret_cast<const uint64_t*>(payload);
            rec.parent_record_num = parent_ref & 0x0000FFFFFFFFFFFFULL;

            uint8_t name_len = payload[0x40];
            uint8_t ns = payload[0x41];
            if (ns != 2 || rec.filename.empty()) {
                const uint16_t* name_ptr = reinterpret_cast<const uint16_t*>(payload + 0x42);
                rec.filename = utf16le_to_utf8(name_ptr, name_len);
            }
        } else if (attr->type == 0x80) { // $DATA
            if ((attr->flags & 0x0001) != 0) {
                rec.is_compressed = true;
            }

            if (attr->non_resident == 0) {
                auto* res = reinterpret_cast<ResidentAttrHeaderRaw*>(attr);
                rec.is_resident = true;
                rec.file_size = res->value_length;
                const uint8_t* data_ptr = work_buf.data() + offset + res->value_offset;
                rec.resident_data.assign(data_ptr, data_ptr + res->value_length);
            } else {
                auto* non_res = reinterpret_cast<NonResidentAttrHeaderRaw*>(attr);
                rec.is_resident = false;
                rec.file_size = non_res->real_size;
                rec.compression_unit = non_res->compression_unit_size;
                if (rec.compression_unit > 0) {
                    rec.is_compressed = true;
                }
                const uint8_t* run_ptr = work_buf.data() + offset + non_res->data_runs_offset;
                size_t max_run_bytes = attr->length - non_res->data_runs_offset;
                rec.data_runs = parse_data_runs(run_ptr, max_run_bytes);
            }
        }
        offset += attr->length;
    }

    if (!rec.filename.empty()) {
        rec.valid = true;
    }
    return rec;
}
