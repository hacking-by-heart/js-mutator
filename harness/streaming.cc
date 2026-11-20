// Streaming-compile target for js-mutator, mirroring d8's
// --streaming-compile path (Shell::CompileSource).
//
// The input is handed to ScriptCompiler::StartStreaming through an
// ExternalSourceStream that returns chunks of 1, 2, 3, 7 or 64 bytes or the
// rest, in one of four encodings: UTF8 70% (half of those with a BOM),
// TWO_BYTE, ONE_BYTE and WINDOWS_1252 10% each. All choices come from the
// input hash. The streaming task runs on its own thread and is joined, then
// the script is compiled with a source string decoded exactly as the stream
// was. Nothing runs. A leading /*M*/ streams a module.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <iterator>
#include <memory>
#include <vector>

#include "include/v8-context.h"
#include "include/v8-exception.h"
#include "include/v8-isolate.h"
#include "include/v8-local-handle.h"
#include "include/v8-primitive.h"
#include "include/v8-script.h"
#include "src/base/logging.h"
#include "src/base/macros.h"
#include "src/base/platform/platform.h"
#include "test/fuzzer/fuzzer-support.h"
#include "common.h"

namespace jsm = js_mutator_harness;
using Encoding = v8::ScriptCompiler::StreamedSource::Encoding;

namespace {

// windows-1252 bytes 0x80-0x9F; every other byte maps to itself. Matches
// kWindows1252ToUC16 in src/parsing/scanner-character-streams.cc.
constexpr uint16_t kCp1252High[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

constexpr size_t kChunkSizes[] = {1, 2, 3, 7, 64, 0};  // 0: the rest

uint64_t SplitMix(uint64_t* state) {
  uint64_t z = (*state += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

class ChunkedSource final : public v8::ScriptCompiler::ExternalSourceStream {
 public:
  ChunkedSource(std::vector<uint8_t> bytes, uint64_t seed, size_t unit)
      : bytes_(std::move(bytes)), state_(seed), unit_(unit) {}

  size_t GetMoreData(const uint8_t** src) override {
    size_t left = bytes_.size() - pos_;
    if (left == 0) return 0;
    size_t n = kChunkSizes[SplitMix(&state_) % std::size(kChunkSizes)] * unit_;
    if (n == 0 || n > left) n = left;
    uint8_t* chunk = new uint8_t[n];  // V8 takes ownership.
    memcpy(chunk, bytes_.data() + pos_, n);
    pos_ += n;
    *src = chunk;
    return n;
  }

 private:
  std::vector<uint8_t> bytes_;
  size_t pos_ = 0;
  uint64_t state_;
  size_t unit_;  // TWO_BYTE chunks must have even length.
};

class StreamerThread final : public v8::base::Thread {
 public:
  explicit StreamerThread(v8::ScriptCompiler::ScriptStreamingTask* task)
      : Thread(Options("jsm-streamer")), task_(task) {}
  void Run() override { task_->Run(); }

 private:
  v8::ScriptCompiler::ScriptStreamingTask* task_;
};

Encoding PickEncoding(uint64_t hash) {
  switch (hash % 10) {
    case 7:
      return Encoding::TWO_BYTE;
    case 8:
      return Encoding::ONE_BYTE;
    case 9:
      return Encoding::WINDOWS_1252;
    default:
      return Encoding::UTF8;
  }
}

// Builds the stream bytes and the equivalent source string.
bool Prepare(v8::Isolate* isolate, const uint8_t* data, size_t size,
             Encoding encoding, bool bom, std::vector<uint8_t>* bytes,
             v8::Local<v8::String>* source) {
  int length = static_cast<int>(size);
  switch (encoding) {
    case Encoding::ONE_BYTE:
      bytes->assign(data, data + size);
      return v8::String::NewFromOneByte(isolate, data,
                                        v8::NewStringType::kNormal, length)
          .ToLocal(source);
    case Encoding::WINDOWS_1252: {
      bytes->assign(data, data + size);
      std::vector<uint16_t> units(size);
      for (size_t i = 0; i < size; ++i) {
        uint8_t c = data[i];
        units[i] = (c >= 0x80 && c < 0xA0) ? kCp1252High[c - 0x80] : c;
      }
      return v8::String::NewFromTwoByte(isolate, units.data(),
                                        v8::NewStringType::kNormal, length)
          .ToLocal(source);
    }
    case Encoding::TWO_BYTE: {
      // The UTF-16 code units of the lossy UTF-8 decoding, as d8's
      // DummySourceStream hands them out.
      if (!v8::String::NewFromUtf8(isolate,
                                   reinterpret_cast<const char*>(data),
                                   v8::NewStringType::kNormal, length)
               .ToLocal(source)) {
        return false;
      }
      uint32_t units = (*source)->Length();
      std::vector<uint16_t> buffer(units);
      (*source)->Write(isolate, 0, units, buffer.data());
      bytes->resize(units * 2);
      memcpy(bytes->data(), buffer.data(), bytes->size());
      return true;
    }
    case Encoding::UTF8:
    default:
      // The stream skips a leading BOM, so the source string has none.
      if (bom) bytes->assign({0xEF, 0xBB, 0xBF});
      bytes->insert(bytes->end(), data, data + size);
      return v8::String::NewFromUtf8(isolate,
                                     reinterpret_cast<const char*>(data),
                                     v8::NewStringType::kNormal, length)
          .ToLocal(source);
  }
}

}  // namespace

V8_SYMBOL_USED extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
  v8_fuzzer::FuzzerSupport::InitializeFuzzerSupport(argc, argv);
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > jsm::kMaxInputSize) return 0;

  v8_fuzzer::FuzzerSupport* support = v8_fuzzer::FuzzerSupport::Get();
  v8::Isolate* isolate = support->GetIsolate();
  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = support->GetContext();
  v8::Context::Scope context_scope(context);
  v8::TryCatch try_catch(isolate);

  uint64_t hash = jsm::InputHash(data, size);
  Encoding encoding = PickEncoding(hash >> 1);
  bool bom = ((hash >> 8) & 1) != 0;
  bool is_module = jsm::GoalOf(data, size) == jsm::Goal::kModule;

  std::vector<uint8_t> bytes;
  v8::Local<v8::String> source_string;
  if (Prepare(isolate, data, size, encoding, bom, &bytes, &source_string)) {
    size_t unit = encoding == Encoding::TWO_BYTE ? 2 : 1;
    v8::ScriptCompiler::StreamedSource streamed_source(
        std::make_unique<ChunkedSource>(std::move(bytes), hash >> 16, unit),
        encoding);
    std::unique_ptr<v8::ScriptCompiler::ScriptStreamingTask> task(
        v8::ScriptCompiler::StartStreaming(
            isolate, &streamed_source,
            is_module ? v8::ScriptType::kModule : v8::ScriptType::kClassic));
    if (task) {
      StreamerThread thread(task.get());
      CHECK(thread.Start());
      thread.Join();
      v8::ScriptOrigin origin(
          v8::String::NewFromUtf8Literal(isolate, "fuzz.js"), 0, 0, false,
          -1, v8::Local<v8::Value>(), false, false, is_module);
      if (is_module) {
        USE(v8::ScriptCompiler::CompileModule(context, &streamed_source,
                                              source_string, origin));
      } else {
        USE(v8::ScriptCompiler::Compile(context, &streamed_source,
                                        source_string, origin));
      }
    }
  }
  try_catch.Reset();
  jsm::MaybeCollectGarbage(isolate);
  return 0;
}
