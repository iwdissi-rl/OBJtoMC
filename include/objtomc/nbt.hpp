#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace objtomc::nbt {

static_assert(std::numeric_limits<float>::is_iec559,
              "objtomc::nbt requires IEEE 754 binary32 float on host");
static_assert(std::numeric_limits<double>::is_iec559,
              "objtomc::nbt requires IEEE 754 binary64 double on host");

enum class TagType : std::uint8_t {
    End        = 0,
    Byte       = 1,
    Short      = 2,
    Int        = 3,
    Long       = 4,
    Float      = 5,
    Double     = 6,
    ByteArray  = 7,
    String     = 8,
    List       = 9,
    Compound   = 10,
    IntArray   = 11,
    LongArray  = 12,
};

class nbt_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

template <typename W>
concept Writer = requires(W w, const std::uint8_t* p, std::size_t n) {
    { w.write(p, n) };
};

class VectorWriter {
public:
    explicit VectorWriter(std::vector<std::uint8_t>& buf) noexcept : buf_(&buf) {}

    void write(const std::uint8_t* data, std::size_t n) {
        buf_->insert(buf_->end(), data, data + n);
    }

private:
    std::vector<std::uint8_t>* buf_;
};

class OstreamWriter {
public:
    explicit OstreamWriter(std::ostream& os) noexcept : os_(&os) {}

    void write(const std::uint8_t* data, std::size_t n) {
        os_->write(reinterpret_cast<const char*>(data),
                   static_cast<std::streamsize>(n));
    }

private:
    std::ostream* os_;
};

class Tag;
class List;
class Compound;

class List {
public:
    explicit List(TagType element_type);

    TagType        element_type() const noexcept;
    std::size_t    size()         const noexcept;
    bool           empty()        const noexcept;
    const Tag&     operator[](std::size_t i) const;

    void push_byte       (std::int8_t  v);
    void push_short      (std::int16_t v);
    void push_int        (std::int32_t v);
    void push_long       (std::int64_t v);
    void push_float      (float        v);
    void push_double     (double       v);
    void push_byte_array (std::vector<std::int8_t>  v);
    void push_int_array  (std::vector<std::int32_t> v);
    void push_long_array (std::vector<std::int64_t> v);
    void push_string     (std::string  v);
    void push_list       (List         v);
    void push_compound   (Compound     v);

    void push(Tag v);

private:
    TagType            type_;
    std::vector<Tag>   items_;
};

class Compound {
public:
    using entry = std::pair<std::string, Tag>;

    Compound() = default;

    void put_byte       (std::string name, std::int8_t  v);
    void put_short      (std::string name, std::int16_t v);
    void put_int        (std::string name, std::int32_t v);
    void put_long       (std::string name, std::int64_t v);
    void put_float      (std::string name, float        v);
    void put_double     (std::string name, double       v);
    void put_byte_array (std::string name, std::vector<std::int8_t>  v);
    void put_int_array  (std::string name, std::vector<std::int32_t> v);
    void put_long_array (std::string name, std::vector<std::int64_t> v);
    void put_string     (std::string name, std::string  v);
    void put_list       (std::string name, List         v);
    void put_compound   (std::string name, Compound     v);
    void put            (std::string name, Tag          v);

    bool           contains(std::string_view name) const noexcept;
    const Tag&     at      (std::string_view name) const;
    std::size_t    size()                          const noexcept;
    bool           empty()                         const noexcept;

    std::vector<entry>::const_iterator begin() const noexcept;
    std::vector<entry>::const_iterator end()   const noexcept;

private:
    std::vector<entry> entries_;
};

// New alternatives (Short, Float, Double, Int_Array) are appended at the
// end of the variant so that the existing alternative indices used by
// Tag::type()'s switch (and indirectly by the byte fixture in
// nbt_worked_example) remain unchanged.  See research/nbt_api_proposal.md §9.5.
using TagValue = std::variant<
    std::int8_t,                    // index 0  — TAG_Byte
    std::int32_t,                   // index 1  — TAG_Int
    std::int64_t,                   // index 2  — TAG_Long
    std::vector<std::int8_t>,       // index 3  — TAG_Byte_Array
    std::string,                    // index 4  — TAG_String
    List,                           // index 5  — TAG_List
    Compound,                       // index 6  — TAG_Compound
    std::vector<std::int64_t>,      // index 7  — TAG_Long_Array
    std::int16_t,                   // index 8  — TAG_Short
    float,                          // index 9  — TAG_Float
    double,                         // index 10 — TAG_Double
    std::vector<std::int32_t>       // index 11 — TAG_Int_Array
>;

class Tag {
public:
    Tag(std::int8_t  v);
    Tag(std::int16_t v);
    Tag(std::int32_t v);
    Tag(std::int64_t v);
    Tag(float        v);
    Tag(double       v);
    Tag(std::vector<std::int8_t>  v);
    Tag(std::vector<std::int32_t> v);
    Tag(std::vector<std::int64_t> v);
    Tag(std::string  v);
    Tag(const char*  v);
    Tag(List         v);
    Tag(Compound     v);

    TagType          type()  const noexcept;
    const TagValue&  value() const noexcept;

private:
    TagValue v_;
};

template <Writer W>
void write_named(W& out, std::string_view name, const Tag& root);

extern template void write_named<VectorWriter>(VectorWriter&, std::string_view, const Tag&);
extern template void write_named<OstreamWriter>(OstreamWriter&, std::string_view, const Tag&);

std::vector<std::uint8_t> to_bytes(std::string_view name, const Tag& root);

}  // namespace objtomc::nbt
