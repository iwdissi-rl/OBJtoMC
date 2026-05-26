#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace objtomc::nbt {

enum class TagType : std::uint8_t {
    End        = 0,
    Byte       = 1,
    Int        = 3,
    Long       = 4,
    ByteArray  = 7,
    String     = 8,
    List       = 9,
    Compound   = 10,
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
    void push_int        (std::int32_t v);
    void push_long       (std::int64_t v);
    void push_byte_array (std::vector<std::int8_t>  v);
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
    void put_int        (std::string name, std::int32_t v);
    void put_long       (std::string name, std::int64_t v);
    void put_byte_array (std::string name, std::vector<std::int8_t>  v);
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

using TagValue = std::variant<
    std::int8_t,
    std::int32_t,
    std::int64_t,
    std::vector<std::int8_t>,
    std::string,
    List,
    Compound,
    std::vector<std::int64_t>
>;

class Tag {
public:
    Tag(std::int8_t  v);
    Tag(std::int32_t v);
    Tag(std::int64_t v);
    Tag(std::vector<std::int8_t>  v);
    Tag(std::string  v);
    Tag(const char*  v);
    Tag(List         v);
    Tag(Compound     v);
    Tag(std::vector<std::int64_t> v);

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
