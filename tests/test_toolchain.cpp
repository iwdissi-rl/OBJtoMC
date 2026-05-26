#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>

int main() {
    const char* ver = zlibVersion();
    assert(ver != nullptr);
    assert(std::strlen(ver) > 0);

    const std::string input = "hello";
    const uLong src_len = static_cast<uLong>(input.size());

    const uLong dest_cap = compressBound(src_len);
    std::vector<Bytef> compressed(dest_cap);
    uLong compressed_len = dest_cap;

    int rc = compress(compressed.data(), &compressed_len,
                      reinterpret_cast<const Bytef*>(input.data()), src_len);
    assert(rc == Z_OK);
    assert(compressed_len > 0);

    std::vector<Bytef> decompressed(src_len);
    uLong decompressed_len = src_len;
    rc = uncompress(decompressed.data(), &decompressed_len,
                    compressed.data(), compressed_len);
    assert(rc == Z_OK);
    assert(decompressed_len == src_len);

    const std::string out(reinterpret_cast<const char*>(decompressed.data()),
                          decompressed_len);
    assert(out == input);

    return 0;
}
