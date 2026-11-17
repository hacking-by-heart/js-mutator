#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

extern "C" size_t LLVMFuzzerCustomMutator(uint8_t* data, size_t size,
                                           size_t max_size, unsigned int seed);

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

}  // namespace

int main() {
  const std::string parent = "function f() { return 1 + 2; }\n";
  std::array<uint8_t, 128> data{};
  bool changed = false;
  for (unsigned seed = 1; seed <= 32; ++seed) {
    std::memcpy(data.data(), parent.data(), parent.size());
    const size_t size = LLVMFuzzerCustomMutator(
        data.data(), parent.size(), data.size(), seed);
    Require(size <= data.size(), "adapter exceeded MaxSize");
    if (std::string(reinterpret_cast<char*>(data.data()), size) != parent) {
      changed = true;
    }
  }
  Require(changed, "adapter did not mutate the supplied seed");
  Require(LLVMFuzzerCustomMutator(data.data(), 1, 0, 1) == 0,
          "adapter did not honor MaxSize=0");
  Require(LLVMFuzzerCustomMutator(nullptr, 0, 8, 1) == 0,
          "adapter must not write through a null buffer");
  return 0;
}
