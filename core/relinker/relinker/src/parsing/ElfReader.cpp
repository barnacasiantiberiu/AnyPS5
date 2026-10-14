#include <relinker/parsing/ElfReader.hpp>
#include <elfpatcher/general/ElfConstants.hpp>
#include <relinker/domain/Types.hpp>
#include <algorithm>
#include <cstring>

namespace Relinker {

using namespace Elfpatcher;

namespace {

bool _rangeFits(const std::uint64_t offset, const std::uint64_t length, const std::size_t size) {
    return offset <= size && length <= size - offset;
}

}

ElfReader::ElfReader(std::vector<std::uint8_t> fileBuffer)
    : _fileBuffer(DecodeExecutable(std::move(fileBuffer)))
{
}

std::vector<std::uint8_t> ElfReader::DecodeExecutable(std::vector<std::uint8_t> bytes) {
    const auto read = [&](std::uint64_t offset, unsigned size) {
        if (!_rangeFits(offset, size, bytes.size())) throw RelinkerException("Truncated SELF container", offset);
        std::uint64_t value = 0;
        for (unsigned i = 0; i < size; ++i) value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
        return value;
    };
    if (bytes.size() < 4 || (read(0, 4) != 0x1d3d154f && read(0, 4) != 0xeef51454)) return bytes;
    if (bytes.size() < 32) throw RelinkerException("Truncated SELF header");
    if (bytes[4] != 0 || bytes[6] != 1) throw RelinkerException("Unsupported SELF version or byte order");
    const auto headerSize = read(12, 2);
    const auto payloadStart = headerSize + read(14, 2);
    const auto fileSize = read(16, 8);
    const auto count = read(24, 2);
    const auto elfOffset = 32 + count * 32;
    if (fileSize > bytes.size() || payloadStart > fileSize || !_rangeFits(elfOffset, 64, headerSize))
        throw RelinkerException("Invalid SELF header or file size");
    if (read(elfOffset, 4) != 0x464c457f || read(elfOffset + 4, 3) != 0x010102 ||
        read(elfOffset + 18, 2) != 62 || read(elfOffset + 20, 4) != 1 || read(elfOffset + 52, 2) != 64)
        throw RelinkerException("SELF must contain a little-endian x86-64 ELF64 executable");
    const auto phOffset = read(elfOffset + 32, 8);
    const auto phCount = read(elfOffset + 56, 2);
    if (phCount == 0 || read(elfOffset + 54, 2) != 56 || phOffset < 64 ||
        !_rangeFits(phOffset, phCount * 56, headerSize - elfOffset))
        throw RelinkerException("Invalid SELF embedded program headers");
    const auto elfHeaderSize = phOffset + phCount * 56;
    struct Range { std::uint64_t offset, size; };
    struct Copy { std::uint64_t source, destination, size; };
    std::vector<Range> programs;
    std::vector<Range> covered{{0, elfHeaderSize}};
    std::vector<Copy> copies{{elfOffset, 0, elfHeaderSize}};
    std::vector<bool> seen(phCount, false);
    std::vector<std::uint64_t> omittedMetadata;
    std::uint64_t outputSize = elfHeaderSize;
    constexpr std::uint64_t maxImageSize = 1ull << 30;
    for (std::uint64_t i = 0; i < phCount; ++i) {
        const auto ph = elfOffset + phOffset + i * 56;
        const auto offset = read(ph + 8, 8);
        const auto size = read(ph + 32, 8);
        if (size && !_rangeFits(offset, size, maxImageSize)) throw RelinkerException("SELF reconstructed ELF exceeds the 1 GiB limit");
        programs.push_back({offset, size});
    }
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto entry = 32 + i * 32;
        const auto flags = read(entry, 8);
        const auto source = read(entry + 8, 8);
        const auto size = read(entry + 16, 8);
        if (!_rangeFits(source, size, fileSize) || (size && source < payloadStart))
            throw RelinkerException("SELF segment is outside the payload", entry);
        if ((flags & 0x800) == 0) continue;
        if (flags & 2) throw RelinkerException("Encrypted SELF payload: provide a decrypted ELF or unencrypted SELF; AnyPS5 cannot decrypt this file");
        if (flags & 8) throw RelinkerException("Compressed SELF payload is not supported yet; provide its uncompressed SELF or ELF counterpart");
        if (flags & 0xf0000) throw RelinkerException("Unsupported SELF data segment flags", entry);
        const auto index = (flags >> 20) & 0xffff;
        if (index >= programs.size() || seen[index]) throw RelinkerException("Invalid or duplicate SELF program segment index", entry);
        seen[index] = true;
        const auto destination = programs[index].offset;
        if (size != read(entry + 24, 8) || size != programs[index].size)
            throw RelinkerException("SELF payload size does not match its ELF program segment", entry);
        if (size == 0) continue;
        for (const auto& previous : copies) {
            const auto first = std::max(destination, previous.destination);
            const auto last = std::min(destination + size, previous.destination + previous.size);
            if (first < last && !std::equal(bytes.begin() + source + first - destination,
                    bytes.begin() + source + last - destination, bytes.begin() + previous.source + first - previous.destination))
                throw RelinkerException("Conflicting SELF payload mappings", entry);
        }
        copies.push_back({source, destination, size});
        covered.push_back({destination, size});
        outputSize = std::max(outputSize, destination + size);
    }
    std::sort(covered.begin(), covered.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& program = programs[i];
        if (program.size == 0) continue;
        auto end = program.offset;
        for (const auto& range : covered) {
            if (range.offset > end) break;
            end = std::max(end, range.offset + range.size);
        }
        if (end < program.offset + program.size) {
            const auto ph = elfOffset + phOffset + i * 56;
            const auto type = read(ph, 4);
            if ((type == 4 || type == 0x6fffff01) && read(ph + 16, 8) == 0 && !seen[i]) omittedMetadata.push_back(phOffset + i * 56);
            else throw RelinkerException("SELF is missing an ELF program segment payload", program.offset);
        }
    }
    std::vector<std::uint8_t> result(outputSize);
    for (const auto& copy : copies) std::copy_n(bytes.begin() + copy.source, copy.size, result.begin() + copy.destination);
    std::fill_n(result.begin() + 40, 8, 0);
    std::fill_n(result.begin() + 58, 6, 0);
    for (const auto ph : omittedMetadata) {
        std::fill_n(result.begin() + ph + 8, 8, 0);
        std::fill_n(result.begin() + ph + 32, 16, 0);
    }
    return result;
}

const std::vector<std::uint8_t>& ElfReader::GetRawBytes() const {
    return _fileBuffer;
}

std::string ElfReader::formatMagic() const {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (std::size_t index = 0; index < 4; ++index) {
        if (index != 0) text += ' ';
        text += digits[_fileBuffer[index] >> 4];
        text += digits[_fileBuffer[index] & 0x0f];
    }
    return text;
}

std::uint8_t ElfReader::_readU8At(FileByteOffset fileByteOffset) const {
    if (fileByteOffset >= _fileBuffer.size()) {
        throw RelinkerException("FileByteOffset out of bounds", fileByteOffset);
    }
    return _fileBuffer[fileByteOffset];
}

std::uint16_t ElfReader::_readU16At(FileByteOffset fileByteOffset) const {
    if (!_rangeFits(fileByteOffset, 2, _fileBuffer.size())) {
        throw RelinkerException("FileByteOffset out of bounds", fileByteOffset);
    }
    std::uint16_t value;
    std::memcpy(&value, _fileBuffer.data() + fileByteOffset, 2);
    return value;
}

std::uint32_t ElfReader::_readU32At(FileByteOffset fileByteOffset) const {
    if (!_rangeFits(fileByteOffset, 4, _fileBuffer.size())) {
        throw RelinkerException("FileByteOffset out of bounds", fileByteOffset);
    }
    std::uint32_t value;
    std::memcpy(&value, _fileBuffer.data() + fileByteOffset, 4);
    return value;
}

std::uint64_t ElfReader::_readU64At(FileByteOffset fileByteOffset) const {
    if (!_rangeFits(fileByteOffset, 8, _fileBuffer.size())) {
        throw RelinkerException("FileByteOffset out of bounds", fileByteOffset);
    }
    std::uint64_t value;
    std::memcpy(&value, _fileBuffer.data() + fileByteOffset, 8);
    return value;
}

ElfHeader ElfReader::ReadHeader() const {
    if (_fileBuffer.size() < 0x40) {
        throw RelinkerException("File too small for ELF header");
    }

    if (_fileBuffer[0] != 0x7f || _fileBuffer[1] != 'E' ||
        _fileBuffer[2] != 'L' || _fileBuffer[3] != 'F') {
        throw RelinkerException("Invalid ELF magic number: " + formatMagic());
    }
    if (_fileBuffer[4] != 2 || _fileBuffer[5] != 1 || _fileBuffer[6] != 1)
        throw RelinkerException("Expected little-endian ELF64 version 1");
    if (_readU16At(0x12) != 62)
        throw RelinkerException("Expected an x86-64 ELF");

    ElfHeader header{};
    header.Machine = _readU16At(0x12);
    header.Type = _readU16At(0x10);
    header.OsAbi = _readU8At(0x07);
    header.AbiVersion = _readU8At(0x08);
    header.EntryPoint = _readU64At(0x18);
    header.ProgramHeaderOffset = _readU64At(0x20);
    header.SectionHeaderOffset = _readU64At(0x28);
    header.ProgramHeaderEntrySize = _readU16At(0x36);
    header.ProgramHeaderCount = _readU16At(0x38);
    header.SectionHeaderEntrySize = _readU16At(0x3a);
    header.SectionHeaderCount = _readU16At(0x3c);
    header.SectionHeaderStringIndex = _readU16At(0x3e);

    return header;
}

std::vector<ProgramHeader> ElfReader::ReadProgramHeaders() const {
    const ElfHeader header = ReadHeader();
    if (header.ProgramHeaderCount != 0 && header.ProgramHeaderEntrySize != 56) {
        throw RelinkerException("Invalid ELF program header entry size: expected 56 bytes", 0x36);
    }

    std::vector<ProgramHeader> headers;
    FileByteOffset offset = header.ProgramHeaderOffset;

    for (std::uint16_t i = 0; i < header.ProgramHeaderCount; ++i) {
        ProgramHeader ph{};
        ph.Type = _readU32At(offset);
        ph.Flags = _readU32At(offset + 0x04);
        ph.Offset = _readU64At(offset + 0x08);
        ph.MappedAddress = _readU64At(offset + 0x10);
        ph.PhysicalAddress = _readU64At(offset + 0x18);
        ph.FileSize = _readU64At(offset + 0x20);
        ph.MemorySize = _readU64At(offset + 0x28);
        ph.Alignment = _readU64At(offset + 0x30);

        headers.push_back(ph);
        offset += header.ProgramHeaderEntrySize;
    }

    for (auto& relro : headers) {
        if (relro.Type != PT_OS_RELRO || relro.MemorySize == 0) continue;
        bool covered = false;
        for (const auto& load : headers) {
            if (load.Type != PT_LOAD || load.MemorySize == 0) continue;
            const bool overlaps = relro.MappedAddress < load.MappedAddress ? load.MappedAddress - relro.MappedAddress < relro.MemorySize : relro.MappedAddress - load.MappedAddress < load.MemorySize;
            if (!overlaps) continue;
            if (relro.MappedAddress < load.MappedAddress || relro.MemorySize > load.MemorySize - (relro.MappedAddress - load.MappedAddress))
                throw RelinkerException("PT_SCE_RELRO partially overlaps a PT_LOAD segment", relro.MappedAddress);
            covered = true;
        }
        if (!covered) {
            relro.Type = PT_LOAD;
            relro.Flags = PF_R | PF_W;
        }
    }

    return headers;
}

std::vector<SectionHeader> ElfReader::ReadSectionHeaders() const {
    const ElfHeader header = ReadHeader();
    if (header.SectionHeaderCount != 0 && header.SectionHeaderEntrySize != kShdrEntrySize) {
        throw RelinkerException("Invalid ELF section header entry size: expected 64 bytes", 0x3a);
    }
    const ByteCount tableSize = static_cast<ByteCount>(header.SectionHeaderCount) * header.SectionHeaderEntrySize;
    if (!_rangeFits(header.SectionHeaderOffset, tableSize, _fileBuffer.size())) {
        throw RelinkerException("Section header table out of bounds", header.SectionHeaderOffset);
    }
    if (header.SectionHeaderStringIndex != 0 && header.SectionHeaderStringIndex >= header.SectionHeaderCount) {
        throw RelinkerException("Section header string table index out of bounds", 0x3e);
    }

    std::vector<SectionHeader> headers;
    FileByteOffset offset = header.SectionHeaderOffset;

    for (std::uint16_t i = 0; i < header.SectionHeaderCount; ++i) {
        SectionHeader sh;
        const std::uint32_t nameOffset = _readU32At(offset);
        sh.Type = _readU32At(offset + 0x04);
        sh.Flags = _readU64At(offset + 0x08);
        sh.MappedAddress = _readU64At(offset + 0x10);
        sh.Offset = _readU64At(offset + 0x18);
        sh.SectionSize = _readU64At(offset + 0x20);
        sh.Link = _readU32At(offset + 0x28);
        sh.Info = _readU32At(offset + 0x2c);
        sh.EntrySize = _readU64At(offset + kShdrEntSizeOffset);

        sh.Name = _resolveShdrName(nameOffset, header);

        headers.push_back(sh);
        offset += header.SectionHeaderEntrySize;
    }

    return headers;
}

std::string ElfReader::_resolveShdrName(std::uint32_t nameOffset, const ElfHeader& header) const {
    if (header.SectionHeaderStringIndex == 0) {
        return "";
    }

    FileByteOffset shstrOffset = header.SectionHeaderOffset +
                        (header.SectionHeaderStringIndex * header.SectionHeaderEntrySize);

    if (_readU32At(shstrOffset + kShdrTypeOffset) != SHT_STRTAB) {
        throw RelinkerException("Section names do not reference a string table", shstrOffset);
    }
    const FileByteOffset strTableOffset = _readU64At(shstrOffset + 0x18);
    const ByteCount strTableSize = _readU64At(shstrOffset + kShdrSizeOffset);
    if (!_rangeFits(strTableOffset, strTableSize, _fileBuffer.size())) {
        throw RelinkerException("Section name string table out of bounds", strTableOffset);
    }
    if (strTableSize == 0 && nameOffset == 0) {
        return "";
    }
    if (nameOffset >= strTableSize) {
        throw RelinkerException("Section name offset out of bounds", nameOffset);
    }
    const auto first = _fileBuffer.begin() + strTableOffset + nameOffset;
    const auto end = _fileBuffer.begin() + strTableOffset + strTableSize;
    const auto terminator = std::find(first, end, 0);
    if (terminator == end) {
        throw RelinkerException("Unterminated section name", strTableOffset + nameOffset);
    }
    return std::string(first, terminator);
}

std::vector<DynamicTag> ElfReader::ReadDynamicTags(const ProgramHeader& dynamicHeader) const {
    if (!_rangeFits(dynamicHeader.Offset, dynamicHeader.FileSize, _fileBuffer.size())) {
        throw RelinkerException("Dynamic segment out of bounds", dynamicHeader.Offset);
    }
    if (dynamicHeader.FileSize % 16 != 0) {
        throw RelinkerException("Invalid dynamic segment size", dynamicHeader.Offset);
    }

    std::vector<DynamicTag> tags;
    FileByteOffset offset = dynamicHeader.Offset;
    const FileByteOffset end = dynamicHeader.Offset + dynamicHeader.FileSize;

    while (offset < end) {
        DynamicTag tag;
        tag.Tag = static_cast<std::int64_t>(_readU64At(offset));
        tag.Value = _readU64At(offset + 0x08);

        if (tag.Tag == 0) {
            return tags;
        }

        tags.push_back(tag);
        offset += 16;
    }

    throw RelinkerException("Unterminated dynamic segment", dynamicHeader.Offset);
}

FileByteOffset ElfReader::TranslateVirtualAddress(VirtualAddress address) const {
    for (const auto& segment : ReadProgramHeaders()) {
        if (segment.Type == PT_LOAD && address >= segment.MappedAddress &&
            address - segment.MappedAddress < segment.FileSize) {
            if (!_rangeFits(segment.Offset, segment.FileSize, _fileBuffer.size())) {
                throw RelinkerException("PT_LOAD file mapping out of bounds", segment.Offset);
            }
            return segment.Offset + (address - segment.MappedAddress);
        }
    }

    throw RelinkerException("Virtual address not mapped by any PT_LOAD segment", address);
}

std::vector<std::uint8_t> ElfReader::ReadSection(const SectionHeader& header) const {
    if (!_rangeFits(header.Offset, header.SectionSize, _fileBuffer.size())) {
        throw RelinkerException("Section offset out of bounds", header.Offset);
    }

    return std::vector<std::uint8_t>(
        _fileBuffer.begin() + header.Offset,
        _fileBuffer.begin() + header.Offset + header.SectionSize);
}

std::vector<std::uint8_t> ElfReader::ReadSegment(const ProgramHeader& header) const {
    if (!_rangeFits(header.Offset, header.FileSize, _fileBuffer.size())) {
        throw RelinkerException("Segment offset out of bounds", header.Offset);
    }

    return std::vector<std::uint8_t>(
        _fileBuffer.begin() + header.Offset,
        _fileBuffer.begin() + header.Offset + header.FileSize);
}

std::uint64_t ElfReader::GetFileSize() const {
    return _fileBuffer.size();
}

}
