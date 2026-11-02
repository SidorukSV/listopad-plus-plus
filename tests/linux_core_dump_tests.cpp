#include "listopad/linux_core_dump.h"

#include <catch2/catch_test_macros.hpp>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

using namespace listopad;

namespace {

template <typename T>
void write_le(std::vector<std::byte>& bytes, const std::size_t offset,
              const T value) {
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    bytes[offset + index] = std::byte{
        static_cast<unsigned char>((value >> (index * 8)) & 0xff)};
  }
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
  const std::size_t offset = bytes.size();
  bytes.resize(offset + 4);
  write_le(bytes, offset, value);
}

void append_u64(std::vector<std::byte>& bytes, const std::uint64_t value) {
  const std::size_t offset = bytes.size();
  bytes.resize(offset + 8);
  write_le(bytes, offset, value);
}

void append_note(std::vector<std::byte>& notes, const std::uint32_t type,
                 const std::span<const std::byte> description) {
  append_u32(notes, 5);
  append_u32(notes, static_cast<std::uint32_t>(description.size()));
  append_u32(notes, type);
  for (const char value : std::string_view{"CORE\0", 5}) {
    notes.push_back(std::byte{static_cast<unsigned char>(value)});
  }
  while (notes.size() % 4 != 0) notes.push_back(std::byte{0});
  notes.insert(notes.end(), description.begin(), description.end());
  while (notes.size() % 4 != 0) notes.push_back(std::byte{0});
}

std::vector<std::byte> elf64_fixture() {
  std::vector<std::byte> notes;
  std::array<std::byte, 16> status{};
  status[12] = std::byte{11};
  append_note(notes, 1, status);
  std::array<std::byte, 12> signal{};
  signal[0] = std::byte{11};
  append_note(notes, 0x53494749, signal);

  std::vector<std::byte> files;
  append_u64(files, 2);
  append_u64(files, 4096);
  append_u64(files, 0x400000);
  append_u64(files, 0x410000);
  append_u64(files, 1);
  append_u64(files, 0x7f000000);
  append_u64(files, 0x7f010000);
  append_u64(files, 2);
  constexpr std::string_view names{"/opt/app\0/lib/libc.so.6\0", 24};
  for (const char value : names) {
    files.push_back(std::byte{static_cast<unsigned char>(value)});
  }
  append_note(notes, 0x46494c45, files);

  constexpr std::size_t note_offset = 120;
  std::vector<std::byte> bytes(note_offset + notes.size());
  bytes[0] = std::byte{0x7f};
  bytes[1] = std::byte{'E'};
  bytes[2] = std::byte{'L'};
  bytes[3] = std::byte{'F'};
  bytes[4] = std::byte{2};
  bytes[5] = std::byte{1};
  bytes[6] = std::byte{1};
  write_le<std::uint16_t>(bytes, 16, 4);
  write_le<std::uint16_t>(bytes, 18, 62);
  write_le<std::uint64_t>(bytes, 32, 64);
  write_le<std::uint16_t>(bytes, 52, 64);
  write_le<std::uint16_t>(bytes, 54, 56);
  write_le<std::uint16_t>(bytes, 56, 1);
  write_le<std::uint32_t>(bytes, 64, 4);
  write_le<std::uint64_t>(bytes, 72, note_offset);
  write_le<std::uint64_t>(bytes, 96, notes.size());
  std::copy(notes.begin(), notes.end(), bytes.begin() + note_offset);
  return bytes;
}

std::vector<std::byte> elf32_fixture() {
  std::vector<std::byte> notes;
  std::array<std::byte, 16> status{};
  status[12] = std::byte{31};
  append_note(notes, 1, status);

  std::vector<std::byte> files;
  append_u32(files, 1);
  append_u32(files, 4096);
  append_u32(files, 0x08048000);
  append_u32(files, 0x08058000);
  append_u32(files, 3);
  constexpr std::string_view name{"/usr/sbin/aksusbd\0", 18};
  for (const char value : name) {
    files.push_back(std::byte{static_cast<unsigned char>(value)});
  }
  append_note(notes, 0x46494c45, files);

  constexpr std::size_t note_offset = 84;
  std::vector<std::byte> bytes(note_offset + notes.size());
  bytes[0] = std::byte{0x7f};
  bytes[1] = std::byte{'E'};
  bytes[2] = std::byte{'L'};
  bytes[3] = std::byte{'F'};
  bytes[4] = std::byte{1};
  bytes[5] = std::byte{1};
  bytes[6] = std::byte{1};
  write_le<std::uint16_t>(bytes, 16, 4);
  write_le<std::uint16_t>(bytes, 18, 3);
  write_le<std::uint32_t>(bytes, 28, 52);
  write_le<std::uint16_t>(bytes, 40, 52);
  write_le<std::uint16_t>(bytes, 42, 32);
  write_le<std::uint16_t>(bytes, 44, 1);
  write_le<std::uint32_t>(bytes, 52, 4);
  write_le<std::uint32_t>(bytes, 56, note_offset);
  write_le<std::uint32_t>(bytes, 68,
                          static_cast<std::uint32_t>(notes.size()));
  std::copy(notes.begin(), notes.end(), bytes.begin() + note_offset);
  return bytes;
}

}  // namespace

TEST_CASE("Linux core parser reads ELF64 notes and systemd filename") {
  const std::vector<std::byte> bytes = elf64_fixture();
  const LinuxCoreReadResult parsed = parse_linux_core(
      bytes,
      L"core.1cv8c.1000.122b0f3058c34eeebab4c771e84c607c.720.1793096951000000.zst");

  REQUIRE(parsed.ok());
  CHECK(parsed.document.architecture == LinuxCoreArchitecture::X86_64);
  CHECK(parsed.document.elf_class == 2);
  CHECK(parsed.document.signal == 11);
  CHECK(parsed.document.thread_count == 1);
  REQUIRE(parsed.document.mappings.size() == 2);
  CHECK(parsed.document.mappings[0].path == "/opt/app");
  CHECK(parsed.document.mappings[0].file_offset == 4096);
  CHECK(parsed.document.mappings[1].path == "/lib/libc.so.6");
  CHECK(parsed.document.source.process == "1cv8c");
  CHECK(parsed.document.source.uid == 1000);
  CHECK(parsed.document.source.pid == 720);
  CHECK(parsed.document.source.has_timestamp);
}

TEST_CASE("Linux core parser rejects non-core ELF") {
  std::vector<std::byte> bytes = elf64_fixture();
  write_le<std::uint16_t>(bytes, 16, 2);
  CHECK(parse_linux_core(bytes).error == LinuxCoreError::NotCoreDump);
}

TEST_CASE("Linux core parser reads ELF32 mappings and signal") {
  const LinuxCoreReadResult parsed = parse_linux_core(
      elf32_fixture(),
      L"core.aksusbd.0.boot.720.1793096951000000.zst");

  REQUIRE(parsed.ok());
  CHECK(parsed.document.architecture == LinuxCoreArchitecture::X86);
  CHECK(parsed.document.elf_class == 1);
  CHECK(parsed.document.signal == 31);
  CHECK(parsed.document.thread_count == 1);
  REQUIRE(parsed.document.mappings.size() == 1);
  CHECK(parsed.document.mappings.front().path == "/usr/sbin/aksusbd");
  CHECK(parsed.document.mappings.front().file_offset == 3 * 4096);
}

TEST_CASE("Linux core parser reports an oversized mapping table") {
  std::vector<std::byte> bytes = elf64_fixture();
  constexpr std::size_t note_offset = 120;
  constexpr std::size_t prstatus_note_size = 36;
  constexpr std::size_t siginfo_note_size = 32;
  constexpr std::size_t file_description =
      note_offset + prstatus_note_size + siginfo_note_size + 12 + 8;
  write_le<std::uint64_t>(bytes, file_description, 200'001);

  const LinuxCoreReadResult parsed = parse_linux_core(bytes);

  REQUIRE(parsed.ok());
  CHECK(parsed.document.mappings.empty());
  CHECK(parsed.document.mappings_truncated);
}

TEST_CASE("Linux core reader handles a zstd stream without full extraction") {
  const std::vector<std::byte> source = elf64_fixture();
  std::vector<std::byte> compressed(ZSTD_compressBound(source.size()));
  const std::size_t size = ZSTD_compress(
      compressed.data(), compressed.size(), source.data(), source.size(), 1);
  REQUIRE(ZSTD_isError(size) == 0);
  compressed.resize(size);

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "core.test.0.boot.42.1.zst";
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(reinterpret_cast<const char*>(compressed.data()),
                 static_cast<std::streamsize>(compressed.size()));
  }
  const LinuxCoreReadResult parsed = read_linux_core_file(path);
  std::error_code ignored;
  std::filesystem::remove(path, ignored);

  REQUIRE(parsed.ok());
  CHECK(parsed.document.compressed);
  CHECK(parsed.document.signal == 11);
  CHECK(parsed.document.source.process == "test");
}

TEST_CASE("Linux core layout keeps the mapping table below the summary") {
  const LinuxCoreLayout layout = calculate_linux_core_layout(1000, 700, 144);
  CHECK(layout.summary.x > 0);
  CHECK(layout.summary.width > 0);
  CHECK(layout.mappings.y >= layout.summary.y + layout.summary.height);
  CHECK(layout.mappings.height > 0);
}
