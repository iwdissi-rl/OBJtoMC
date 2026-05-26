#include <cstdio>
#include <zlib.h>

int main() {
    std::printf("objtomc toolchain smoke test\n");
    std::printf("__cplusplus = %ld\n", static_cast<long>(__cplusplus));
    std::printf("zlib version = %s\n", zlibVersion());
    return 0;
}
