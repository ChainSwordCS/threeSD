// Copyright 2017 Citra Emulator Project / 2019 threeSD Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cinttypes>
#include <cryptopp/rsa.h>
#include <cryptopp/sha.h>
#include <fmt/format.h>
#include "common/alignment.h"
#include "common/assert.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/string_util.h"
#include "core/file_sys/certificate.h"
#include "core/file_sys/cia_common.h"
#include "core/file_sys/title_metadata.h"

namespace Core {

bool TitleMetadata::Load(const std::vector<u8> file_data, std::size_t offset) {
    std::size_t total_size = static_cast<std::size_t>(file_data.size() - offset);
    if (total_size < sizeof(u32_be))
        return false;

    if (!signature.Load(file_data, offset)) {
        return false;
    }
    const auto signature_size = signature.GetSize();
    std::size_t body_end = signature_size + sizeof(Body);
    if (total_size < body_end)
        return false;

    // Read TMD body, then load the amount of ContentChunks specified
    std::memcpy(&tmd_body, &file_data[offset + signature_size], sizeof(TitleMetadata::Body));

    std::size_t expected_size = signature_size + sizeof(Body) +
                                static_cast<u16>(tmd_body.content_count) * sizeof(ContentChunk);
    if (total_size < expected_size) {
        LOG_ERROR(Service_FS, "Malformed TMD, expected size 0x{:x}, got 0x{:x}!", expected_size,
                  total_size);
        return false;
    }

    for (u16 i = 0; i < tmd_body.content_count; i++) {
        ContentChunk chunk;

        std::memcpy(&chunk, &file_data[offset + body_end + (i * sizeof(ContentChunk))],
                    sizeof(ContentChunk));
        tmd_chunks.push_back(chunk);
    }

    return true;
}

bool TitleMetadata::Save(FileUtil::IOFile& file) {
    if (!file.IsOpen())
        return false;

    if (!signature.Save(file)) {
        return false;
    }

    // Write our TMD body, then write each of our ContentChunks
    if (file.WriteBytes(&tmd_body, sizeof(TitleMetadata::Body)) != sizeof(TitleMetadata::Body))
        return false;

    for (u16 i = 0; i < tmd_body.content_count; i++) {
        ContentChunk chunk = tmd_chunks[i];
        if (file.WriteBytes(&chunk, sizeof(ContentChunk)) != sizeof(ContentChunk))
            return false;
    }

    return true;
}

bool TitleMetadata::Save(const std::string& file_path) {
    FileUtil::IOFile file(file_path, "wb");
    return Save(file);
}

void TitleMetadata::FixHashes() {
    // Update our TMD body values and hashes
    tmd_body.content_count = static_cast<u16>(tmd_chunks.size());

    // TODO(shinyquagsire23): Do TMDs with more than one contentinfo exist?
    // For now we'll just adjust the first index to hold all content chunks
    // and ensure that no further content info data exists.
    tmd_body.contentinfo = {};
    tmd_body.contentinfo[0].index = 0;
    tmd_body.contentinfo[0].command_count = static_cast<u16>(tmd_chunks.size());

    CryptoPP::SHA256 chunk_hash;
    for (u16 i = 0; i < tmd_body.content_count; i++) {
        chunk_hash.Update(reinterpret_cast<u8*>(&tmd_chunks[i]), sizeof(ContentChunk));
    }
    chunk_hash.Final(tmd_body.contentinfo[0].hash.data());

    CryptoPP::SHA256 contentinfo_hash;
    for (std::size_t i = 0; i < tmd_body.contentinfo.size(); i++) {
        contentinfo_hash.Update(reinterpret_cast<u8*>(&tmd_body.contentinfo[i]),
                                sizeof(ContentInfo));
    }
    contentinfo_hash.Final(tmd_body.contentinfo_hash.data());
}

bool TitleMetadata::VerifyHashes() const {
    // This can probably be simplified to just checking contentinfo 0 (as above), but do the full
    // thing for completeness.
    // TODO: Is this what index and command_count actually mean?
    CryptoPP::SHA256 contentinfo_hash;
    for (std::size_t i = 0; i < tmd_body.contentinfo.size(); i++) {
        contentinfo_hash.Update(reinterpret_cast<const u8*>(&tmd_body.contentinfo[i]),
                                sizeof(ContentInfo));
        const std::size_t offset = static_cast<u64>(tmd_body.contentinfo[i].index);
        const std::size_t count = static_cast<u64>(tmd_body.contentinfo[i].command_count);
        if (count == 0) {
            continue;
        }
        if (offset + count > tmd_body.content_count) { // sanity check
            LOG_INFO(Core, "Index / count out of bound for content info record {}", i);
            return false;
        }
        CryptoPP::SHA256 chunk_hash;
        for (std::size_t j = offset; j < offset + count; ++j) {
            chunk_hash.Update(reinterpret_cast<const u8*>(&tmd_chunks[j]), sizeof(ContentChunk));
        }
        if (!chunk_hash.Verify(tmd_body.contentinfo[i].hash.data())) {
            LOG_ERROR(Core, "Chunk hash dismatch for content info record {}", i);
            return false;
        }
    }
    if (!contentinfo_hash.Verify(tmd_body.contentinfo_hash.data())) {
        LOG_ERROR(Core, "Content info hash dismatch");
        return false;
    }
    return true;
}

bool TitleMetadata::ValidateSignature() const {
    const auto issuer =
        Common::StringFromFixedZeroTerminatedBuffer(tmd_body.issuer.data(), tmd_body.issuer.size());
    return signature.Verify(issuer, [this](auto* message) {
        static_assert(offsetof(Body, contentinfo) == 0xC4, "Signed data length is not correct");
        message->Update(reinterpret_cast<const u8*>(&tmd_body), offsetof(Body, contentinfo));
    });
}

std::size_t TitleMetadata::GetSize() const {
    return signature.GetSize() + sizeof(TitleMetadata::Body) +
           sizeof(ContentChunk) * tmd_chunks.size();
}

u64 TitleMetadata::GetTitleID() const {
    return tmd_body.title_id;
}

u32 TitleMetadata::GetTitleType() const {
    return tmd_body.title_type;
}

u16 TitleMetadata::GetTitleVersion() const {
    return tmd_body.title_version;
}

std::string TitleMetadata::GetTitleVersionString() const {
    const auto title_version = static_cast<u16>(tmd_body.title_version);
    return fmt::format("{}.{}.{}", (title_version >> 10) & 0x3F, (title_version >> 4) & 0x3F,
                       title_version & 0xF);
}

u64 TitleMetadata::GetSystemVersion() const {
    return tmd_body.system_version;
}

size_t TitleMetadata::GetContentCount() const {
    return tmd_chunks.size();
}

u32 TitleMetadata::GetBootContentID() const {
    return tmd_chunks[TMDContentIndex::Main].id;
}

u32 TitleMetadata::GetManualContentID() const {
    return tmd_chunks[TMDContentIndex::Manual].id;
}

u32 TitleMetadata::GetDLPContentID() const {
    return tmd_chunks[TMDContentIndex::DLP].id;
}

u32 TitleMetadata::GetContentIDByIndex(u16 index) const {
    return tmd_chunks[index].id;
}

u16 TitleMetadata::GetContentTypeByIndex(u16 index) const {
    return tmd_chunks[index].type;
}

u64 TitleMetadata::GetContentSizeByIndex(u16 index) const {
    return tmd_chunks[index].size;
}

std::array<u8, 16> TitleMetadata::GetContentCTRByIndex(u16 index) const {
    std::array<u8, 16> ctr{};
    std::memcpy(ctr.data(), &tmd_chunks[index].index, sizeof(u16));
    return ctr;
}

void TitleMetadata::SetTitleID(u64 title_id) {
    tmd_body.title_id = title_id;
}

void TitleMetadata::SetTitleType(u32 type) {
    tmd_body.title_type = type;
}

void TitleMetadata::SetTitleVersion(u16 version) {
    tmd_body.title_version = version;
}

void TitleMetadata::SetSystemVersion(u64 version) {
    tmd_body.system_version = version;
}

TitleMetadata::ContentChunk& TitleMetadata::GetContentChunkByID(u32 content_id) {
    const auto it =
        std::find_if(tmd_chunks.begin(), tmd_chunks.end(),
                     [content_id](const ContentChunk& chunk) { return chunk.id == content_id; });
    ASSERT(it != tmd_chunks.end());
    return *it;
}

const TitleMetadata::ContentChunk& TitleMetadata::GetContentChunkByID(u32 content_id) const {
    const auto it =
        std::find_if(tmd_chunks.begin(), tmd_chunks.end(),
                     [content_id](const ContentChunk& chunk) { return chunk.id == content_id; });
    ASSERT(it != tmd_chunks.end());
    return *it;
}

bool TitleMetadata::HasContentID(u32 content_id) const {
    const auto it =
        std::find_if(tmd_chunks.begin(), tmd_chunks.end(),
                     [content_id](const ContentChunk& chunk) { return chunk.id == content_id; });
    return it != tmd_chunks.end();
}

void TitleMetadata::AddContentChunk(const ContentChunk& chunk) {
    tmd_chunks.push_back(chunk);
}

void TitleMetadata::Print() const {
    LOG_DEBUG(Service_FS, "{} chunks", static_cast<u32>(tmd_body.content_count));

    // Content info describes ranges of content chunks
    LOG_DEBUG(Service_FS, "Content info:");
    for (std::size_t i = 0; i < tmd_body.contentinfo.size(); i++) {
        if (tmd_body.contentinfo[i].command_count == 0)
            break;

        LOG_DEBUG(Service_FS, "    Index {:04X}, Command Count {:04X}",
                  static_cast<u32>(tmd_body.contentinfo[i].index),
                  static_cast<u32>(tmd_body.contentinfo[i].command_count));
    }

    // For each content info, print their content chunk range
    for (std::size_t i = 0; i < tmd_body.contentinfo.size(); i++) {
        u16 index = static_cast<u16>(tmd_body.contentinfo[i].index);
        u16 count = static_cast<u16>(tmd_body.contentinfo[i].command_count);

        if (count == 0)
            continue;

        LOG_DEBUG(Service_FS, "Content chunks for content info index {}:", i);
        for (u16 j = index; j < static_cast<u16>(index + count); j++) {
            // Don't attempt to print content we don't have
            if (j > tmd_body.content_count)
                break;

            const ContentChunk& chunk = tmd_chunks[j];
            LOG_DEBUG(Service_FS, "    ID {:08X}, Index {:04X}, Type {:04x}, Size {:016X}",
                      static_cast<u32>(chunk.id), static_cast<u32>(chunk.index),
                      static_cast<u32>(chunk.type), static_cast<u64>(chunk.size));
        }
    }
}
} // namespace Core
