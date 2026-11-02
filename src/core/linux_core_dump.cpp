#include "listopad/linux_core_dump.h"

#include "listopad/file_io.h"

#include <windows.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace listopad {
namespace {

constexpr std::uint16_t kElfCore = 4;
constexpr std::uint32_t kProgramLoad = 1;
constexpr std::uint32_t kProgramNote = 4;
constexpr std::uint32_t kNotePrStatus = 1;
constexpr std::uint32_t kNoteFile = 0x46494c45;
constexpr std::uint32_t kNoteSigInfo = 0x53494749;
constexpr std::size_t kMaxMetadataBytes = 64ull * 1024ull * 1024ull;
constexpr std::size_t kMaxMappings = 200'000;
constexpr std::array<std::byte, 4> kElfMagic{
    std::byte{0x7f}, std::byte{'E'}, std::byte{'L'}, std::byte{'F'}};
constexpr std::array<std::byte, 4> kZstdMagic{
    std::byte{0x28}, std::byte{0xb5}, std::byte{0x2f}, std::byte{0xfd}};

struct ElfShape {
  std::uint8_t elf_class{0};
  bool little_endian{true};
  std::uint16_t machine{0};
  std::uint64_t program_offset{0};
  std::uint16_t program_entry_size{0};
  std::uint16_t program_count{0};
  std::size_t required_size{0};
};

template <typename T>
std::optional<T> read_integer(const std::span<const std::byte> bytes,
                              const std::size_t offset,
                              const bool little_endian) noexcept {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
    return std::nullopt;
  }
  using Unsigned = std::make_unsigned_t<T>;
  Unsigned value = 0;
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    const std::size_t source = little_endian ? index : sizeof(T) - index - 1;
    value |= static_cast<Unsigned>(
                 std::to_integer<unsigned char>(bytes[offset + source]))
             << (index * 8);
  }
  return static_cast<T>(value);
}

bool starts_with(const std::span<const std::byte> bytes,
                 const std::span<const std::byte> magic) noexcept {
  return bytes.size() >= magic.size() &&
         std::equal(magic.begin(), magic.end(), bytes.begin());
}

std::optional<std::size_t> checked_size(const std::uint64_t offset,
                                        const std::uint64_t size) noexcept {
  if (offset > kMaxMetadataBytes || size > kMaxMetadataBytes - offset) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(offset + size);
}

LinuxCoreError inspect_shape(const std::span<const std::byte> bytes,
                             ElfShape& shape) noexcept {
  if (bytes.size() < 20 || !starts_with(bytes, kElfMagic)) {
    return LinuxCoreError::NotCoreDump;
  }
  shape.elf_class = std::to_integer<std::uint8_t>(bytes[4]);
  const std::uint8_t encoding = std::to_integer<std::uint8_t>(bytes[5]);
  if ((shape.elf_class != 1 && shape.elf_class != 2) ||
      (encoding != 1 && encoding != 2)) {
    return LinuxCoreError::UnsupportedFormat;
  }
  shape.little_endian = encoding == 1;
  const auto type = read_integer<std::uint16_t>(bytes, 16,
                                                shape.little_endian);
  const auto machine = read_integer<std::uint16_t>(bytes, 18,
                                                   shape.little_endian);
  if (!type || !machine) return LinuxCoreError::InvalidData;
  if (*type != kElfCore) return LinuxCoreError::NotCoreDump;
  shape.machine = *machine;

  if (shape.elf_class == 2) {
    if (bytes.size() < 64) return LinuxCoreError::InvalidData;
    const auto offset = read_integer<std::uint64_t>(bytes, 32,
                                                    shape.little_endian);
    const auto entry_size = read_integer<std::uint16_t>(
        bytes, 54, shape.little_endian);
    const auto count = read_integer<std::uint16_t>(bytes, 56,
                                                   shape.little_endian);
    if (!offset || !entry_size || !count || *entry_size < 56 ||
        *count == std::numeric_limits<std::uint16_t>::max()) {
      return LinuxCoreError::UnsupportedFormat;
    }
    shape.program_offset = *offset;
    shape.program_entry_size = *entry_size;
    shape.program_count = *count;
  } else {
    if (bytes.size() < 52) return LinuxCoreError::InvalidData;
    const auto offset = read_integer<std::uint32_t>(bytes, 28,
                                                    shape.little_endian);
    const auto entry_size = read_integer<std::uint16_t>(
        bytes, 42, shape.little_endian);
    const auto count = read_integer<std::uint16_t>(bytes, 44,
                                                   shape.little_endian);
    if (!offset || !entry_size || !count || *entry_size < 32 ||
        *count == std::numeric_limits<std::uint16_t>::max()) {
      return LinuxCoreError::UnsupportedFormat;
    }
    shape.program_offset = *offset;
    shape.program_entry_size = *entry_size;
    shape.program_count = *count;
  }

  const std::uint64_t table_size =
      static_cast<std::uint64_t>(shape.program_entry_size) *
      shape.program_count;
  const auto table_end = checked_size(shape.program_offset, table_size);
  if (!table_end) return LinuxCoreError::MetadataTooLarge;
  shape.required_size = *table_end;
  if (bytes.size() < shape.required_size) return LinuxCoreError::None;

  for (std::size_t index = 0; index < shape.program_count; ++index) {
    const std::size_t entry = static_cast<std::size_t>(shape.program_offset) +
                              index * shape.program_entry_size;
    const auto program_type = read_integer<std::uint32_t>(
        bytes, entry, shape.little_endian);
    if (!program_type) return LinuxCoreError::InvalidData;
    if (*program_type != kProgramNote) continue;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    if (shape.elf_class == 2) {
      const auto value_offset = read_integer<std::uint64_t>(
          bytes, entry + 8, shape.little_endian);
      const auto value_size = read_integer<std::uint64_t>(
          bytes, entry + 32, shape.little_endian);
      if (!value_offset || !value_size) return LinuxCoreError::InvalidData;
      offset = *value_offset;
      size = *value_size;
    } else {
      const auto value_offset = read_integer<std::uint32_t>(
          bytes, entry + 4, shape.little_endian);
      const auto value_size = read_integer<std::uint32_t>(
          bytes, entry + 16, shape.little_endian);
      if (!value_offset || !value_size) return LinuxCoreError::InvalidData;
      offset = *value_offset;
      size = *value_size;
    }
    const auto note_end = checked_size(offset, size);
    if (!note_end) return LinuxCoreError::MetadataTooLarge;
    shape.required_size = (std::max)(shape.required_size, *note_end);
  }
  return LinuxCoreError::None;
}

std::optional<std::uint64_t> parse_unsigned(
    const std::wstring_view value) noexcept {
  std::uint64_t result = 0;
  const auto first = reinterpret_cast<const wchar_t*>(value.data());
  const auto last = first + value.size();
  // std::from_chars has no wchar_t overload; systemd filenames use ASCII
  // digits, so a bounded manual conversion is both locale-free and strict.
  if (first == last) return std::nullopt;
  for (auto current = first; current != last; ++current) {
    if (*current < L'0' || *current > L'9') return std::nullopt;
    const std::uint64_t digit = static_cast<std::uint64_t>(*current - L'0');
    if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return std::nullopt;
    }
    result = result * 10 + digit;
  }
  return result;
}

LinuxCoreSourceMetadata parse_source_name(std::wstring_view name) {
  LinuxCoreSourceMetadata metadata;
  const std::size_t slash = name.find_last_of(L"/\\");
  if (slash != std::wstring_view::npos) name.remove_prefix(slash + 1);
  if (name.size() >= 4 && name.substr(name.size() - 4) == L".zst") {
    name.remove_suffix(4);
  }
  if (!name.starts_with(L"core.")) return metadata;

  std::array<std::wstring_view, 4> suffixes{};
  std::wstring_view remaining = name;
  for (std::size_t index = suffixes.size(); index != 0; --index) {
    const std::size_t dot = remaining.find_last_of(L'.');
    if (dot == std::wstring_view::npos) return {};
    suffixes[index - 1] = remaining.substr(dot + 1);
    remaining = remaining.substr(0, dot);
  }
  if (!remaining.starts_with(L"core.") || remaining.size() <= 5) return {};
  const std::wstring_view process = remaining.substr(5);
  metadata.process.reserve(process.size());
  for (const wchar_t value : process) {
    metadata.process.push_back(
        value <= 0x7f ? static_cast<char>(value) : '?');
  }
  const auto uid = parse_unsigned(suffixes[0]);
  const auto pid = parse_unsigned(suffixes[2]);
  const auto timestamp = parse_unsigned(suffixes[3]);
  const std::wstring_view boot = suffixes[1];
  metadata.boot_id.reserve(boot.size());
  for (const wchar_t value : boot) {
    metadata.boot_id.push_back(
        value <= 0x7f ? static_cast<char>(value) : '?');
  }
  if (uid && *uid <= std::numeric_limits<std::uint32_t>::max()) {
    metadata.uid = static_cast<std::uint32_t>(*uid);
    metadata.has_uid = true;
  }
  if (pid && *pid <= std::numeric_limits<std::uint32_t>::max()) {
    metadata.pid = static_cast<std::uint32_t>(*pid);
    metadata.has_pid = true;
  }
  if (timestamp) {
    metadata.timestamp_microseconds = *timestamp;
    metadata.has_timestamp = true;
  }
  return metadata;
}

LinuxCoreArchitecture architecture_for(const std::uint16_t machine) noexcept {
  switch (machine) {
    case 3: return LinuxCoreArchitecture::X86;
    case 40: return LinuxCoreArchitecture::Arm;
    case 62: return LinuxCoreArchitecture::X86_64;
    case 183: return LinuxCoreArchitecture::AArch64;
    case 243: return LinuxCoreArchitecture::RiscV;
    default: return LinuxCoreArchitecture::Unknown;
  }
}

std::size_t align_four(const std::size_t value) noexcept {
  return (value + 3u) & ~std::size_t{3u};
}

bool add_mapping(LinuxCoreDocument& document, const std::uint64_t start,
                 const std::uint64_t end, const std::uint64_t page_offset,
                 const std::uint64_t page_size, std::string path) {
  if (document.mappings.size() >= kMaxMappings) {
    document.mappings_truncated = true;
    return false;
  }
  std::uint64_t file_offset = 0;
  if (page_size != 0 &&
      page_offset <= std::numeric_limits<std::uint64_t>::max() / page_size) {
    file_offset = page_offset * page_size;
  }
  document.mappings.push_back(
      {.start = start,
       .end = end,
       .file_offset = file_offset,
       .path = std::move(path)});
  return true;
}

void parse_file_note(const std::span<const std::byte> description,
                     const ElfShape& shape, LinuxCoreDocument& document) {
  const std::size_t word_size = shape.elf_class == 2 ? 8 : 4;
  if (description.size() < word_size * 2) return;
  const auto read_word = [&](const std::size_t offset)
      -> std::optional<std::uint64_t> {
    if (word_size == 8) {
      return read_integer<std::uint64_t>(description, offset,
                                         shape.little_endian);
    }
    const auto value = read_integer<std::uint32_t>(description, offset,
                                                   shape.little_endian);
    return value ? std::optional<std::uint64_t>{*value} : std::nullopt;
  };
  const auto count = read_word(0);
  const auto page_size = read_word(word_size);
  if (!count || !page_size) return;
  if (*count > kMaxMappings) {
    document.mappings_truncated = true;
    return;
  }
  const std::uint64_t table_bytes = *count * word_size * 3;
  if (table_bytes > description.size() - word_size * 2) return;
  const std::size_t names_offset =
      word_size * 2 + static_cast<std::size_t>(table_bytes);
  std::size_t name_cursor = names_offset;
  for (std::uint64_t index = 0; index < *count; ++index) {
    const std::size_t entry = word_size * 2 +
                              static_cast<std::size_t>(index) * word_size * 3;
    const auto start = read_word(entry);
    const auto end = read_word(entry + word_size);
    const auto offset = read_word(entry + word_size * 2);
    if (!start || !end || !offset) return;
    const std::size_t begin = name_cursor;
    while (name_cursor < description.size() &&
           description[name_cursor] != std::byte{0}) {
      ++name_cursor;
    }
    if (name_cursor >= description.size()) return;
    const auto* text = reinterpret_cast<const char*>(description.data() + begin);
    std::string path(text, name_cursor - begin);
    ++name_cursor;
    if (!add_mapping(document, *start, *end, *offset, *page_size,
                     std::move(path))) {
      return;
    }
  }
}

LinuxCoreReadResult parse_complete(const std::span<const std::byte> bytes,
                                   const ElfShape& shape,
                                   const std::wstring_view source_name,
                                   const std::stop_token stop) {
  LinuxCoreReadResult result;
  result.document.architecture = architecture_for(shape.machine);
  result.document.elf_class = shape.elf_class;
  result.document.little_endian = shape.little_endian;
  result.document.program_header_count = shape.program_count;
  result.document.source = parse_source_name(source_name);

  for (std::size_t index = 0; index < shape.program_count; ++index) {
    if (stop.stop_requested()) {
      result.error = LinuxCoreError::Cancelled;
      return result;
    }
    const std::size_t entry = static_cast<std::size_t>(shape.program_offset) +
                              index * shape.program_entry_size;
    const auto type = read_integer<std::uint32_t>(bytes, entry,
                                                  shape.little_endian);
    if (!type) {
      result.error = LinuxCoreError::InvalidData;
      return result;
    }
    if (*type == kProgramLoad) ++result.document.load_segment_count;
    if (*type != kProgramNote) continue;
    std::uint64_t note_offset = 0;
    std::uint64_t note_size = 0;
    if (shape.elf_class == 2) {
      note_offset = *read_integer<std::uint64_t>(bytes, entry + 8,
                                                 shape.little_endian);
      note_size = *read_integer<std::uint64_t>(bytes, entry + 32,
                                               shape.little_endian);
    } else {
      note_offset = *read_integer<std::uint32_t>(bytes, entry + 4,
                                                 shape.little_endian);
      note_size = *read_integer<std::uint32_t>(bytes, entry + 16,
                                               shape.little_endian);
    }
    const std::size_t end =
        static_cast<std::size_t>(note_offset + note_size);
    std::size_t cursor = static_cast<std::size_t>(note_offset);
    while (cursor <= end && end - cursor >= 12) {
      const auto name_size = read_integer<std::uint32_t>(
          bytes, cursor, shape.little_endian);
      const auto description_size = read_integer<std::uint32_t>(
          bytes, cursor + 4, shape.little_endian);
      const auto note_type = read_integer<std::uint32_t>(
          bytes, cursor + 8, shape.little_endian);
      if (!name_size || !description_size || !note_type) {
        result.error = LinuxCoreError::InvalidData;
        return result;
      }
      cursor += 12;
      const std::size_t aligned_name = align_four(*name_size);
      const std::size_t aligned_description = align_four(*description_size);
      if (aligned_name > end - cursor) {
        result.error = LinuxCoreError::InvalidData;
        return result;
      }
      cursor += aligned_name;
      if (aligned_description > end - cursor) {
        result.error = LinuxCoreError::InvalidData;
        return result;
      }
      const auto description = bytes.subspan(cursor, *description_size);
      if (*note_type == kNotePrStatus) {
        ++result.document.thread_count;
        if (result.document.signal == 0 && description.size() >= 14) {
          const auto signal = read_integer<std::uint16_t>(
              description, 12, shape.little_endian);
          if (signal) result.document.signal = *signal;
        }
      } else if (*note_type == kNoteSigInfo && description.size() >= 4) {
        const auto signal = read_integer<std::uint32_t>(
            description, 0, shape.little_endian);
        if (signal && *signal != 0) result.document.signal = *signal;
      } else if (*note_type == kNoteFile) {
        parse_file_note(description, shape, result.document);
      }
      cursor += aligned_description;
    }
  }
  return result;
}

struct ZstdDeleter {
  void operator()(ZSTD_DCtx* context) const noexcept {
    ZSTD_freeDCtx(context);
  }
};

LinuxCoreReadResult decompress_and_parse(
    const std::span<const std::byte> compressed,
    const std::wstring_view source_name, const std::stop_token stop,
    const bool probe_only) {
  LinuxCoreReadResult result;
  std::unique_ptr<ZSTD_DCtx, ZstdDeleter> context(ZSTD_createDCtx());
  if (!context) {
    result.error = LinuxCoreError::DecompressionFailed;
    return result;
  }
  std::vector<std::byte> output;
  output.reserve(256 * 1024);
  std::array<std::byte, 128 * 1024> chunk{};
  ZSTD_inBuffer input{compressed.data(), compressed.size(), 0};
  std::size_t required = 64;
  while (input.pos < input.size && output.size() < required) {
    if (stop.stop_requested()) {
      result.error = LinuxCoreError::Cancelled;
      return result;
    }
    ZSTD_outBuffer target{chunk.data(), chunk.size(), 0};
    const std::size_t status = ZSTD_decompressStream(context.get(), &target,
                                                     &input);
    if (ZSTD_isError(status) != 0) {
      result.error = LinuxCoreError::DecompressionFailed;
      return result;
    }
    if (target.pos == 0 && status != 0 && input.pos == input.size) {
      result.error = LinuxCoreError::InvalidData;
      return result;
    }
    if (output.size() > kMaxMetadataBytes - target.pos) {
      result.error = LinuxCoreError::MetadataTooLarge;
      return result;
    }
    output.insert(output.end(), chunk.begin(), chunk.begin() + target.pos);
    if (output.size() >= 20) {
      ElfShape shape;
      const LinuxCoreError inspected = inspect_shape(output, shape);
      if (inspected != LinuxCoreError::None) {
        result.error = inspected;
        return result;
      }
      required = probe_only ? (shape.elf_class == 2 ? 64 : 52)
                            : shape.required_size;
      if (required > kMaxMetadataBytes) {
        result.error = LinuxCoreError::MetadataTooLarge;
        return result;
      }
      if (probe_only && output.size() >= required) {
        result.document.compressed = true;
        return result;
      }
      if (!probe_only && output.size() >= required) {
        result = parse_linux_core(output, source_name, stop);
        result.document.compressed = result.ok();
        return result;
      }
    }
    if (status == 0 && output.size() < required) {
      result.error = LinuxCoreError::InvalidData;
      return result;
    }
  }
  result.error = LinuxCoreError::InvalidData;
  return result;
}

}  // namespace

bool looks_like_linux_core(const std::span<const std::byte> bytes) noexcept {
  ElfShape shape;
  return inspect_shape(bytes, shape) == LinuxCoreError::None;
}

bool looks_like_linux_core_file(const std::filesystem::path& path) noexcept {
  MappedFile file;
  if (!file.open(path) || !file.data() || file.size() < 20 ||
      file.size() > (std::numeric_limits<std::size_t>::max)()) {
    return false;
  }
  const auto bytes = std::span<const std::byte>(
      file.data(), static_cast<std::size_t>(file.size()));
  if (starts_with(bytes, kZstdMagic)) {
    return decompress_and_parse(bytes, path.filename().wstring(), {}, true)
        .ok();
  }
  return looks_like_linux_core(bytes.first(
      (std::min)(bytes.size(), static_cast<std::size_t>(256 * 1024))));
}

LinuxCoreReadResult parse_linux_core(const std::span<const std::byte> bytes,
                                     const std::wstring_view source_name,
                                     const std::stop_token stop) {
  LinuxCoreReadResult result;
  ElfShape shape;
  result.error = inspect_shape(bytes, shape);
  if (!result.ok()) return result;
  if (bytes.size() < shape.required_size) {
    result.error = LinuxCoreError::InvalidData;
    return result;
  }
  return parse_complete(bytes, shape, source_name, stop);
}

LinuxCoreReadResult read_linux_core_file(const std::filesystem::path& path,
                                         const std::stop_token stop) {
  LinuxCoreReadResult result;
  MappedFile file;
  if (!file.open(path)) {
    result.error = LinuxCoreError::OpenFailed;
    result.system_error = file.error();
    return result;
  }
  if (!file.data() || file.size() < 20 ||
      file.size() > (std::numeric_limits<std::size_t>::max)()) {
    result.error = LinuxCoreError::InvalidData;
    return result;
  }
  const auto bytes = std::span<const std::byte>(
      file.data(), static_cast<std::size_t>(file.size()));
  if (starts_with(bytes, kZstdMagic)) {
    return decompress_and_parse(bytes, path.filename().wstring(), stop,
                                false);
  }
  return parse_linux_core(bytes, path.filename().wstring(), stop);
}

std::string_view linux_core_architecture_name(
    const LinuxCoreArchitecture architecture) noexcept {
  switch (architecture) {
    case LinuxCoreArchitecture::X86: return "x86";
    case LinuxCoreArchitecture::X86_64: return "x86-64";
    case LinuxCoreArchitecture::Arm: return "ARM";
    case LinuxCoreArchitecture::AArch64: return "AArch64";
    case LinuxCoreArchitecture::RiscV: return "RISC-V";
    case LinuxCoreArchitecture::Unknown: return "unknown";
  }
  return "unknown";
}

std::string_view linux_signal_name(const std::uint32_t signal) noexcept {
  switch (signal) {
    case 4: return "SIGILL";
    case 5: return "SIGTRAP";
    case 6: return "SIGABRT";
    case 7: return "SIGBUS";
    case 8: return "SIGFPE";
    case 9: return "SIGKILL";
    case 11: return "SIGSEGV";
    case 13: return "SIGPIPE";
    case 15: return "SIGTERM";
    case 31: return "SIGSYS";
    default: return "unknown";
  }
}

LinuxCoreLayout calculate_linux_core_layout(const int width, const int height,
                                             const int dpi) noexcept {
  const int padding = (std::max)(1, MulDiv(8, dpi, 96));
  const int summary_height = (std::max)(MulDiv(82, dpi, 96), 1);
  return {
      .summary = {padding, padding, (std::max)(0, width - padding * 2),
                  (std::min)(summary_height, (std::max)(0, height - padding))},
      .mappings = {padding, padding + summary_height,
                   (std::max)(0, width - padding * 2),
                   (std::max)(0, height - summary_height - padding * 2)},
  };
}

}  // namespace listopad
