#include "objtomc/nbt.hpp"

#include <bit>
#include <limits>
#include <type_traits>
#include <utility>

namespace objtomc::nbt {

namespace {

constexpr std::size_t kMaxNameOrStringLen =
    static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max());
constexpr std::size_t kMaxArrayLen =
    static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());

template <class> inline constexpr bool always_false_v = false;

template <Writer W>
inline void write_u8(W& out, std::uint8_t v) {
    out.write(&v, 1);
}

template <Writer W>
inline void write_u16_be(W& out, std::uint16_t v) {
    const std::uint8_t buf[2] = {
        static_cast<std::uint8_t>((v >> 8) & 0xFF),
        static_cast<std::uint8_t>(v & 0xFF),
    };
    out.write(buf, 2);
}

template <Writer W>
inline void write_u32_be(W& out, std::uint32_t v) {
    const std::uint8_t buf[4] = {
        static_cast<std::uint8_t>((v >> 24) & 0xFF),
        static_cast<std::uint8_t>((v >> 16) & 0xFF),
        static_cast<std::uint8_t>((v >> 8)  & 0xFF),
        static_cast<std::uint8_t>( v        & 0xFF),
    };
    out.write(buf, 4);
}

template <Writer W>
inline void write_u64_be(W& out, std::uint64_t v) {
    const std::uint8_t buf[8] = {
        static_cast<std::uint8_t>((v >> 56) & 0xFF),
        static_cast<std::uint8_t>((v >> 48) & 0xFF),
        static_cast<std::uint8_t>((v >> 40) & 0xFF),
        static_cast<std::uint8_t>((v >> 32) & 0xFF),
        static_cast<std::uint8_t>((v >> 24) & 0xFF),
        static_cast<std::uint8_t>((v >> 16) & 0xFF),
        static_cast<std::uint8_t>((v >> 8)  & 0xFF),
        static_cast<std::uint8_t>( v        & 0xFF),
    };
    out.write(buf, 8);
}

template <Writer W>
inline void write_i8(W& out, std::int8_t v) {
    write_u8(out, static_cast<std::uint8_t>(v));
}

template <Writer W>
inline void write_i16_be(W& out, std::int16_t v) {
    write_u16_be(out, static_cast<std::uint16_t>(v));
}

template <Writer W>
inline void write_i32_be(W& out, std::int32_t v) {
    write_u32_be(out, static_cast<std::uint32_t>(v));
}

template <Writer W>
inline void write_i64_be(W& out, std::int64_t v) {
    write_u64_be(out, static_cast<std::uint64_t>(v));
}

template <Writer W>
inline void write_f32_be(W& out, float v) {
    write_u32_be(out, std::bit_cast<std::uint32_t>(v));
}

template <Writer W>
inline void write_f64_be(W& out, double v) {
    write_u64_be(out, std::bit_cast<std::uint64_t>(v));
}

template <Writer W>
inline void write_bytes(W& out, const std::uint8_t* data, std::size_t n) {
    if (n > 0) out.write(data, n);
}

template <Writer W>
inline void write_string_payload(W& out, std::string_view s) {
    if (s.size() > kMaxNameOrStringLen) {
        throw nbt_error("nbt: string longer than 65535 bytes");
    }
    write_u16_be(out, static_cast<std::uint16_t>(s.size()));
    write_bytes(out, reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

template <Writer W> void write_payload(W& out, const Tag& tag);

template <Writer W>
inline void write_byte_array_payload(W& out, const std::vector<std::int8_t>& v) {
    if (v.size() > kMaxArrayLen) {
        throw nbt_error("nbt: byte array length exceeds int32 max");
    }
    write_i32_be(out, static_cast<std::int32_t>(v.size()));
    write_bytes(out, reinterpret_cast<const std::uint8_t*>(v.data()), v.size());
}

template <Writer W>
inline void write_int_array_payload(W& out, const std::vector<std::int32_t>& v) {
    if (v.size() > kMaxArrayLen) {
        throw nbt_error("nbt: int array length exceeds int32 max");
    }
    write_i32_be(out, static_cast<std::int32_t>(v.size()));
    for (std::int32_t x : v) {
        write_i32_be(out, x);
    }
}

template <Writer W>
inline void write_long_array_payload(W& out, const std::vector<std::int64_t>& v) {
    if (v.size() > kMaxArrayLen) {
        throw nbt_error("nbt: long array length exceeds int32 max");
    }
    write_i32_be(out, static_cast<std::int32_t>(v.size()));
    for (std::int64_t x : v) {
        write_i64_be(out, x);
    }
}

template <Writer W>
inline void write_list_payload(W& out, const List& l) {
    if (l.size() > kMaxArrayLen) {
        throw nbt_error("nbt: list length exceeds int32 max");
    }
    write_u8(out, static_cast<std::uint8_t>(l.element_type()));
    write_i32_be(out, static_cast<std::int32_t>(l.size()));
    for (std::size_t i = 0; i < l.size(); ++i) {
        write_payload(out, l[i]);
    }
}

template <Writer W>
inline void write_compound_payload(W& out, const Compound& c) {
    for (const auto& [name, tag] : c) {
        write_u8(out, static_cast<std::uint8_t>(tag.type()));
        write_string_payload(out, name);
        write_payload(out, tag);
    }
    write_u8(out, 0);  // TAG_End
}

template <Writer W>
void write_payload(W& out, const Tag& tag) {
    std::visit([&out](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::int8_t>) {
            write_i8(out, v);
        } else if constexpr (std::is_same_v<T, std::int16_t>) {
            write_i16_be(out, v);
        } else if constexpr (std::is_same_v<T, std::int32_t>) {
            write_i32_be(out, v);
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            write_i64_be(out, v);
        } else if constexpr (std::is_same_v<T, float>) {
            write_f32_be(out, v);
        } else if constexpr (std::is_same_v<T, double>) {
            write_f64_be(out, v);
        } else if constexpr (std::is_same_v<T, std::vector<std::int8_t>>) {
            write_byte_array_payload(out, v);
        } else if constexpr (std::is_same_v<T, std::string>) {
            write_string_payload(out, v);
        } else if constexpr (std::is_same_v<T, List>) {
            write_list_payload(out, v);
        } else if constexpr (std::is_same_v<T, Compound>) {
            write_compound_payload(out, v);
        } else if constexpr (std::is_same_v<T, std::vector<std::int32_t>>) {
            write_int_array_payload(out, v);
        } else if constexpr (std::is_same_v<T, std::vector<std::int64_t>>) {
            write_long_array_payload(out, v);
        } else {
            static_assert(always_false_v<T>, "unhandled tag payload type");
        }
    }, tag.value());
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// List
// ---------------------------------------------------------------------------

List::List(TagType element_type) : type_(element_type) {}

TagType     List::element_type() const noexcept { return type_; }
std::size_t List::size()         const noexcept { return items_.size(); }
bool        List::empty()        const noexcept { return items_.empty(); }

const Tag& List::operator[](std::size_t i) const { return items_[i]; }

void List::push_byte(std::int8_t v) {
    if (type_ != TagType::Byte) {
        throw nbt_error("nbt: List::push_byte on list of different element type");
    }
    items_.emplace_back(v);
}

void List::push_short(std::int16_t v) {
    if (type_ != TagType::Short) {
        throw nbt_error("nbt: List::push_short on list of different element type");
    }
    items_.emplace_back(v);
}

void List::push_int(std::int32_t v) {
    if (type_ != TagType::Int) {
        throw nbt_error("nbt: List::push_int on list of different element type");
    }
    items_.emplace_back(v);
}

void List::push_long(std::int64_t v) {
    if (type_ != TagType::Long) {
        throw nbt_error("nbt: List::push_long on list of different element type");
    }
    items_.emplace_back(v);
}

void List::push_float(float v) {
    if (type_ != TagType::Float) {
        throw nbt_error("nbt: List::push_float on list of different element type");
    }
    items_.emplace_back(v);
}

void List::push_double(double v) {
    if (type_ != TagType::Double) {
        throw nbt_error("nbt: List::push_double on list of different element type");
    }
    items_.emplace_back(v);
}

void List::push_byte_array(std::vector<std::int8_t> v) {
    if (type_ != TagType::ByteArray) {
        throw nbt_error("nbt: List::push_byte_array on list of different element type");
    }
    items_.emplace_back(std::move(v));
}

void List::push_int_array(std::vector<std::int32_t> v) {
    if (type_ != TagType::IntArray) {
        throw nbt_error("nbt: List::push_int_array on list of different element type");
    }
    items_.emplace_back(std::move(v));
}

void List::push_long_array(std::vector<std::int64_t> v) {
    if (type_ != TagType::LongArray) {
        throw nbt_error("nbt: List::push_long_array on list of different element type");
    }
    items_.emplace_back(std::move(v));
}

void List::push_string(std::string v) {
    if (type_ != TagType::String) {
        throw nbt_error("nbt: List::push_string on list of different element type");
    }
    items_.emplace_back(std::move(v));
}

void List::push_list(List v) {
    if (type_ != TagType::List) {
        throw nbt_error("nbt: List::push_list on list of different element type");
    }
    items_.emplace_back(std::move(v));
}

void List::push_compound(Compound v) {
    if (type_ != TagType::Compound) {
        throw nbt_error("nbt: List::push_compound on list of different element type");
    }
    items_.emplace_back(std::move(v));
}

void List::push(Tag v) {
    if (type_ == TagType::End) {
        throw nbt_error("nbt: cannot push into List of type End");
    }
    if (v.type() != type_) {
        throw nbt_error("nbt: List::push Tag type does not match element type");
    }
    items_.push_back(std::move(v));
}

// ---------------------------------------------------------------------------
// Compound
// ---------------------------------------------------------------------------

void Compound::put_byte(std::string name, std::int8_t v) {
    put(std::move(name), Tag(v));
}

void Compound::put_short(std::string name, std::int16_t v) {
    put(std::move(name), Tag(v));
}

void Compound::put_int(std::string name, std::int32_t v) {
    put(std::move(name), Tag(v));
}

void Compound::put_long(std::string name, std::int64_t v) {
    put(std::move(name), Tag(v));
}

void Compound::put_float(std::string name, float v) {
    put(std::move(name), Tag(v));
}

void Compound::put_double(std::string name, double v) {
    put(std::move(name), Tag(v));
}

void Compound::put_byte_array(std::string name, std::vector<std::int8_t> v) {
    put(std::move(name), Tag(std::move(v)));
}

void Compound::put_int_array(std::string name, std::vector<std::int32_t> v) {
    put(std::move(name), Tag(std::move(v)));
}

void Compound::put_long_array(std::string name, std::vector<std::int64_t> v) {
    put(std::move(name), Tag(std::move(v)));
}

void Compound::put_string(std::string name, std::string v) {
    put(std::move(name), Tag(std::move(v)));
}

void Compound::put_list(std::string name, List v) {
    put(std::move(name), Tag(std::move(v)));
}

void Compound::put_compound(std::string name, Compound v) {
    put(std::move(name), Tag(std::move(v)));
}

void Compound::put(std::string name, Tag v) {
    if (contains(name)) {
        throw nbt_error("nbt: duplicate key in Compound: " + name);
    }
    entries_.emplace_back(std::move(name), std::move(v));
}

bool Compound::contains(std::string_view name) const noexcept {
    for (const auto& [k, _] : entries_) {
        if (k == name) return true;
    }
    return false;
}

const Tag& Compound::at(std::string_view name) const {
    for (const auto& [k, v] : entries_) {
        if (k == name) return v;
    }
    throw nbt_error("nbt: key not found in Compound");
}

std::size_t Compound::size()  const noexcept { return entries_.size(); }
bool        Compound::empty() const noexcept { return entries_.empty(); }

std::vector<Compound::entry>::const_iterator Compound::begin() const noexcept {
    return entries_.begin();
}

std::vector<Compound::entry>::const_iterator Compound::end() const noexcept {
    return entries_.end();
}

// ---------------------------------------------------------------------------
// Tag
// ---------------------------------------------------------------------------

Tag::Tag(std::int8_t  v) : v_(v) {}
Tag::Tag(std::int16_t v) : v_(v) {}
Tag::Tag(std::int32_t v) : v_(v) {}
Tag::Tag(std::int64_t v) : v_(v) {}
Tag::Tag(float        v) : v_(v) {}
Tag::Tag(double       v) : v_(v) {}
Tag::Tag(std::vector<std::int8_t>  v) : v_(std::move(v)) {}
Tag::Tag(std::vector<std::int32_t> v) : v_(std::move(v)) {}
Tag::Tag(std::vector<std::int64_t> v) : v_(std::move(v)) {}
Tag::Tag(std::string  v) : v_(std::move(v)) {}
Tag::Tag(const char*  v) : v_(std::string(v)) {}
Tag::Tag(List         v) : v_(std::move(v)) {}
Tag::Tag(Compound     v) : v_(std::move(v)) {}

TagType Tag::type() const noexcept {
    switch (v_.index()) {
        case 0:  return TagType::Byte;
        case 1:  return TagType::Int;
        case 2:  return TagType::Long;
        case 3:  return TagType::ByteArray;
        case 4:  return TagType::String;
        case 5:  return TagType::List;
        case 6:  return TagType::Compound;
        case 7:  return TagType::LongArray;
        case 8:  return TagType::Short;
        case 9:  return TagType::Float;
        case 10: return TagType::Double;
        case 11: return TagType::IntArray;
    }
    return TagType::End;  // unreachable; variant index is one of 0..11
}

const TagValue& Tag::value() const noexcept { return v_; }

// ---------------------------------------------------------------------------
// Top-level serialization
// ---------------------------------------------------------------------------

template <Writer W>
void write_named(W& out, std::string_view name, const Tag& root) {
    write_u8(out, static_cast<std::uint8_t>(root.type()));
    write_string_payload(out, name);
    write_payload(out, root);
}

template void write_named<VectorWriter>(VectorWriter&, std::string_view, const Tag&);
template void write_named<OstreamWriter>(OstreamWriter&, std::string_view, const Tag&);

std::vector<std::uint8_t> to_bytes(std::string_view name, const Tag& root) {
    std::vector<std::uint8_t> out;
    VectorWriter vw(out);
    write_named(vw, name, root);
    return out;
}

}  // namespace objtomc::nbt
