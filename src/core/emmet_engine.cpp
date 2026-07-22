#include "listopad/emmet_engine.h"

#include "emmet_bundle.js.h"

#include <quickjs.h>

#include <algorithm>
#include <charconv>
#include <mutex>

namespace listopad {
namespace {

std::string exception_text(JSContext* context) {
  JSValue exception = JS_GetException(context);
  const char* text = JS_ToCString(context, exception);
  std::string result = text ? text : "Unknown JavaScript exception";
  if (text) JS_FreeCString(context, text);
  JS_FreeValue(context, exception);
  return result;
}

EmmetExpansion parse_fields(std::string source) {
  EmmetExpansion result; result.ok = true;
  for (std::size_t cursor = 0; cursor < source.size();) {
    if (source[cursor] != '$') { result.text.push_back(source[cursor++]); continue; }
    const std::size_t marker = cursor++;
    unsigned index = 0;
    std::string placeholder;
    if (cursor < source.size() && source[cursor] == '{') {
      const std::size_t close = source.find('}', ++cursor);
      if (close == std::string::npos) { result.text.push_back('$'); cursor = marker + 1; continue; }
      const std::size_t colon = source.find(':', cursor);
      const std::size_t number_end = colon != std::string::npos && colon < close ? colon : close;
      const auto [end, error] = std::from_chars(source.data() + cursor, source.data() + number_end, index);
      if (error != std::errc{} || end != source.data() + number_end) {
        result.text.append(source, marker, close - marker + 1); cursor = close + 1; continue;
      }
      if (colon != std::string::npos && colon < close) placeholder = source.substr(colon + 1, close - colon - 1);
      cursor = close + 1;
    } else {
      const std::size_t begin = cursor;
      while (cursor < source.size() && source[cursor] >= '0' && source[cursor] <= '9') ++cursor;
      if (cursor == begin) { result.text.push_back('$'); continue; }
      std::from_chars(source.data() + begin, source.data() + cursor, index);
    }
    result.fields.push_back({index, result.text.size(), placeholder.size()});
    result.text += placeholder;
  }
  std::stable_sort(result.fields.begin(), result.fields.end(), [](const EmmetField& left, const EmmetField& right) {
    if (left.index == 0) return false;
    if (right.index == 0) return true;
    return left.index < right.index;
  });
  return result;
}

}  // namespace

struct EmmetEngine::Impl {
  JSRuntime* runtime{nullptr};
  JSContext* context{nullptr};
  bool loaded{false};
  std::string load_error;
  std::mutex mutex;

  ~Impl() {
    if (context) JS_FreeContext(context);
    if (runtime) JS_FreeRuntime(runtime);
  }
  bool ensure_loaded() {
    if (loaded) return load_error.empty();
    loaded = true;
    runtime = JS_NewRuntime();
    if (!runtime) { load_error = "Unable to create QuickJS runtime"; return false; }
    JS_SetMemoryLimit(runtime, 32 * 1024 * 1024);
    JS_SetMaxStackSize(runtime, 1024 * 1024);
    context = JS_NewContext(runtime);
    if (!context) { load_error = "Unable to create QuickJS context"; return false; }
    JSValue evaluated = JS_Eval(context, kEmmetBundle, sizeof(kEmmetBundle) - 1,
                                "emmet.bundle.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(evaluated)) load_error = exception_text(context);
    JS_FreeValue(context, evaluated);
    return load_error.empty();
  }
};

EmmetEngine::EmmetEngine() : impl_(std::make_unique<Impl>()) {}
EmmetEngine::~EmmetEngine() = default;

EmmetExpansion EmmetEngine::expand(const std::string_view abbreviation,
                                    const std::string_view syntax) {
  std::scoped_lock lock(impl_->mutex);
  if (!impl_->ensure_loaded()) return {false, {}, {}, impl_->load_error};
  JSValue global = JS_GetGlobalObject(impl_->context);
  JSValue function = JS_GetPropertyStr(impl_->context, global, "listopadExpand");
  JSValue args[2]{
      JS_NewStringLen(impl_->context, abbreviation.data(), abbreviation.size()),
      JS_NewStringLen(impl_->context, syntax.data(), syntax.size())};
  JSValue value = JS_Call(impl_->context, function, global, 2, args);
  JS_FreeValue(impl_->context, args[0]); JS_FreeValue(impl_->context, args[1]);
  JS_FreeValue(impl_->context, function); JS_FreeValue(impl_->context, global);
  if (JS_IsException(value)) {
    const std::string error = exception_text(impl_->context);
    JS_FreeValue(impl_->context, value);
    return {false, {}, {}, error};
  }
  std::size_t length = 0;
  const char* text = JS_ToCStringLen(impl_->context, &length, value);
  if (!text) { JS_FreeValue(impl_->context, value); return {false, {}, {}, "Emmet returned no text"}; }
  EmmetExpansion result = parse_fields(std::string(text, length));
  JS_FreeCString(impl_->context, text); JS_FreeValue(impl_->context, value);
  return result;
}

}  // namespace listopad
