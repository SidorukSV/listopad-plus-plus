#include "listopad/encoding.h"

#include "listopad/strings.h"

#include <windows.h>
#include <uchardet/uchardet.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>

namespace listopad {
namespace {

bool valid_utf8(const std::span<const std::byte> bytes) {
  if (bytes.empty()) return true;
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                             reinterpret_cast<const char*>(bytes.data()),
                             static_cast<int>(bytes.size()), nullptr, 0) > 0;
}

unsigned charset_to_code_page(std::string charset) {
  std::transform(charset.begin(), charset.end(), charset.begin(),
                 [](const unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
  static const std::unordered_map<std::string, unsigned> pages{
      {"WINDOWS-1251", 1251}, {"CP1251", 1251}, {"WINDOWS-1252", 1252},
      {"CP1252", 1252},       {"IBM866", 866},   {"CP866", 866},
      {"KOI8-R", 20866},      {"ISO-8859-5", 28595}, {"SHIFT_JIS", 932},
      {"GB18030", 54936},     {"BIG5", 950},    {"EUC-KR", 51949},
  };
  const auto found = pages.find(charset);
  return found != pages.end() ? found->second : GetACP();
}

std::string bytes_to_utf8(const std::span<const std::byte> bytes, const unsigned page) {
  if (bytes.empty()) return {};
  DWORD flags = page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
  int wide_length = MultiByteToWideChar(page, flags,
                                        reinterpret_cast<const char*>(bytes.data()),
                                        static_cast<int>(bytes.size()), nullptr, 0);
  if (wide_length <= 0 && flags) {
    flags = 0;
    wide_length = MultiByteToWideChar(page, flags,
                                      reinterpret_cast<const char*>(bytes.data()),
                                      static_cast<int>(bytes.size()), nullptr, 0);
  }
  if (wide_length <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
  MultiByteToWideChar(page, flags, reinterpret_cast<const char*>(bytes.data()),
                      static_cast<int>(bytes.size()), wide.data(), wide_length);
  return wide_to_utf8(wide);
}

bool looks_binary(const std::span<const std::byte> bytes, const bool utf16) {
  if (utf16) return false;
  const std::size_t limit = std::min<std::size_t>(bytes.size(), 8192);
  std::size_t controls = 0;
  for (std::size_t index = 0; index < limit; ++index) {
    const unsigned value = std::to_integer<unsigned>(bytes[index]);
    if (value == 0) return true;
    if (value < 9 || (value > 13 && value < 32)) ++controls;
  }
  return limit > 0 && controls * 20 > limit;
}

}  // namespace

std::string Encoding::name() const {
  switch (kind) {
    case EncodingKind::Utf8: return bom ? "UTF-8 BOM" : "UTF-8";
    case EncodingKind::Utf16Le: return "UTF-16 LE";
    case EncodingKind::Utf16Be: return "UTF-16 BE";
    case EncodingKind::WindowsCodePage: return "Windows-" + std::to_string(code_page);
  }
  return "UTF-8";
}

DecodedText decode_text(const std::span<const std::byte> bytes, const Encoding* forced) {
  DecodedText result;
  std::span<const std::byte> content = bytes;
  if (forced) {
    result.encoding = *forced;
    if (result.encoding.kind == EncodingKind::Utf8 && bytes.size() >= 3 &&
        bytes[0] == std::byte{0xef} && bytes[1] == std::byte{0xbb} &&
        bytes[2] == std::byte{0xbf}) {
      result.encoding.bom = true;
      content = bytes.subspan(3);
    } else if (result.encoding.kind == EncodingKind::Utf16Le && bytes.size() >= 2 &&
               bytes[0] == std::byte{0xff} && bytes[1] == std::byte{0xfe}) {
      result.encoding.bom = true;
      content = bytes.subspan(2);
    } else if (result.encoding.kind == EncodingKind::Utf16Be && bytes.size() >= 2 &&
               bytes[0] == std::byte{0xfe} && bytes[1] == std::byte{0xff}) {
      result.encoding.bom = true;
      content = bytes.subspan(2);
    }
  } else if (bytes.size() >= 3 && bytes[0] == std::byte{0xef} &&
             bytes[1] == std::byte{0xbb} && bytes[2] == std::byte{0xbf}) {
    result.encoding = {EncodingKind::Utf8, CP_UTF8, true};
    content = bytes.subspan(3);
  } else if (bytes.size() >= 2 && bytes[0] == std::byte{0xff} && bytes[1] == std::byte{0xfe}) {
    result.encoding = {EncodingKind::Utf16Le, 1200, true};
    content = bytes.subspan(2);
  } else if (bytes.size() >= 2 && bytes[0] == std::byte{0xfe} && bytes[1] == std::byte{0xff}) {
    result.encoding = {EncodingKind::Utf16Be, 1201, true};
    content = bytes.subspan(2);
  } else if (valid_utf8(bytes)) {
    result.encoding = {EncodingKind::Utf8, CP_UTF8, false};
  } else {
    uchardet_t detector = uchardet_new();
    std::string charset;
    if (detector) {
      const std::size_t sample = std::min<std::size_t>(bytes.size(), 256 * 1024);
      if (uchardet_handle_data(detector, reinterpret_cast<const char*>(bytes.data()), sample) == 0) {
        uchardet_data_end(detector);
        charset = uchardet_get_charset(detector);
      }
      uchardet_delete(detector);
    }
    result.encoding = {EncodingKind::WindowsCodePage, charset_to_code_page(charset), false};
  }

  result.likely_binary = looks_binary(content,
      result.encoding.kind == EncodingKind::Utf16Le || result.encoding.kind == EncodingKind::Utf16Be);

  if (result.encoding.kind == EncodingKind::Utf16Le ||
      result.encoding.kind == EncodingKind::Utf16Be) {
    if (content.size() % 2 != 0) content = content.first(content.size() - 1);
    std::wstring wide(content.size() / 2, L'\0');
    for (std::size_t index = 0; index < wide.size(); ++index) {
      const unsigned first = std::to_integer<unsigned>(content[index * 2]);
      const unsigned second = std::to_integer<unsigned>(content[index * 2 + 1]);
      wide[index] = static_cast<wchar_t>(result.encoding.kind == EncodingKind::Utf16Le
                                             ? first | (second << 8)
                                             : (first << 8) | second);
    }
    result.utf8 = wide_to_utf8(wide);
  } else {
    result.utf8 = bytes_to_utf8(content, result.encoding.code_page);
  }
  result.eol = detect_eol(result.utf8);
  return result;
}

EncodingResult encode_text(const std::string_view utf8, const Encoding& encoding) {
  EncodingResult result;
  const std::wstring wide = utf8_to_wide(utf8);
  if (!utf8.empty() && wide.empty()) {
    result.error = "Input is not valid UTF-8";
    return result;
  }
  if (encoding.kind == EncodingKind::Utf16Le || encoding.kind == EncodingKind::Utf16Be) {
    if (encoding.bom) {
      result.bytes.push_back(encoding.kind == EncodingKind::Utf16Le ? std::byte{0xff} : std::byte{0xfe});
      result.bytes.push_back(encoding.kind == EncodingKind::Utf16Le ? std::byte{0xfe} : std::byte{0xff});
    }
    result.bytes.reserve(result.bytes.size() + wide.size() * 2);
    for (const wchar_t ch : wide) {
      const auto value = static_cast<unsigned>(ch);
      const std::byte low{static_cast<unsigned char>(value & 0xff)};
      const std::byte high{static_cast<unsigned char>((value >> 8) & 0xff)};
      if (encoding.kind == EncodingKind::Utf16Le) {
        result.bytes.push_back(low); result.bytes.push_back(high);
      } else {
        result.bytes.push_back(high); result.bytes.push_back(low);
      }
    }
    result.ok = true;
    return result;
  }

  const unsigned page = encoding.kind == EncodingKind::Utf8 ? CP_UTF8 : encoding.code_page;
  BOOL used_default = FALSE;
  const DWORD flags = page == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
  const int length = WideCharToMultiByte(page, flags, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr,
                                         page == CP_UTF8 ? nullptr : &used_default);
  if ((!wide.empty() && length == 0) || used_default) {
    result.error = "Text contains characters that are not representable in " + encoding.name();
    return result;
  }
  if (encoding.kind == EncodingKind::Utf8 && encoding.bom) {
    result.bytes = {std::byte{0xef}, std::byte{0xbb}, std::byte{0xbf}};
  }
  const std::size_t offset = result.bytes.size();
  result.bytes.resize(offset + static_cast<std::size_t>(length));
  used_default = FALSE;
  WideCharToMultiByte(page, flags, wide.data(), static_cast<int>(wide.size()),
                      reinterpret_cast<char*>(result.bytes.data() + offset), length,
                      nullptr, page == CP_UTF8 ? nullptr : &used_default);
  if (used_default) {
    result.bytes.clear();
    result.error = "Text contains characters that are not representable in " + encoding.name();
    return result;
  }
  result.ok = true;
  return result;
}

EolMode detect_eol(const std::string_view text) {
  std::size_t crlf = 0, lf = 0, cr = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n') { ++crlf; ++i; }
      else ++cr;
    } else if (text[i] == '\n') {
      ++lf;
    }
  }
  const int kinds = static_cast<int>(crlf > 0) + static_cast<int>(lf > 0) + static_cast<int>(cr > 0);
  if (kinds > 1) return EolMode::Mixed;
  if (lf > 0) return EolMode::Lf;
  if (cr > 0) return EolMode::Cr;
  return EolMode::CrLf;
}

std::string_view eol_text(const EolMode mode) {
  if (mode == EolMode::Lf) return "\n";
  if (mode == EolMode::Cr) return "\r";
  return "\r\n";
}

Encoding encoding_from_name(std::string_view name) {
  std::string lower(name);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  if (lower == "utf-8-bom" || lower == "utf8bom" ||
      lower == "utf-8 bom")
    return {EncodingKind::Utf8, CP_UTF8, true};
  if (lower == "utf-16" || lower == "utf-16le" ||
      lower == "utf-16 le")
    return {EncodingKind::Utf16Le, 1200, true};
  if (lower == "utf-16be" || lower == "utf-16 be")
    return {EncodingKind::Utf16Be, 1201, true};
  if (lower == "windows-1251" || lower == "cp1251") return {EncodingKind::WindowsCodePage, 1251, false};
  if (lower == "windows-1252" || lower == "cp1252") return {EncodingKind::WindowsCodePage, 1252, false};
  if (lower == "cp866" || lower == "ibm866") return {EncodingKind::WindowsCodePage, 866, false};
  return {EncodingKind::Utf8, CP_UTF8, false};
}

}  // namespace listopad
