// The libFuzzer callbacks honor their buffers and limits.
#include "test_util.h"

#include <array>
#include <cstring>

extern "C" size_t LLVMFuzzerCustomMutator(uint8_t* data, size_t size, size_t max_size,
                                           unsigned int seed);
extern "C" size_t LLVMFuzzerCustomCrossOver(const uint8_t* data1, size_t size1,
                                             const uint8_t* data2, size_t size2,
                                             uint8_t* out, size_t max_out_size,
                                             unsigned int seed);
using test::Require;

int main() {
  test::Syntax syntax;
  std::array<uint8_t, 4096> data{};
  std::array<uint8_t, 4096> out{};
  const std::string parent = "function total(price, qty) { return price * qty; }";
  const std::string donor = "class Item { static { this.n = 1; } }";
  std::set<std::string> outputs;
  for (unsigned seed = 1; seed <= 256; ++seed) {
    std::memcpy(data.data(), parent.data(), parent.size());
    const size_t size = LLVMFuzzerCustomMutator(data.data(), parent.size(), data.size(), seed);
    Require(size > 0 && size <= data.size(), "adapter output size out of range");
    const std::string mutated(reinterpret_cast<const char*>(data.data()), size);
    Require(syntax.Parses(mutated), "adapter output did not parse");
    outputs.insert(mutated);

    const size_t crossed = LLVMFuzzerCustomCrossOver(
        reinterpret_cast<const uint8_t*>(parent.data()), parent.size(),
        reinterpret_cast<const uint8_t*>(donor.data()), donor.size(), out.data(),
        out.size(), seed);
    Require(crossed > 0 && crossed <= out.size(), "crossover size out of range");
    Require(syntax.Parses(std::string(reinterpret_cast<const char*>(out.data()), crossed)),
            "crossover output did not parse");
  }
  Require(outputs.size() >= 180, "adapter outputs are not diverse");
  Require(LLVMFuzzerCustomMutator(data.data(), 1, 0, 1) == 0, "MaxSize=0 was not honored");
  Require(LLVMFuzzerCustomMutator(nullptr, 0, 8, 1) == 0, "null buffer was written");
  Require(LLVMFuzzerCustomCrossOver(data.data(), 1, data.data(), 1, out.data(), 0, 1) == 0,
          "crossover MaxSize=0 was not honored");
  std::cout << "adapter: ok\n";
  return 0;
}
