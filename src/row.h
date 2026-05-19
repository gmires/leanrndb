#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <sstream>

/**
 * Schema di una colonna: nome e tipo SQL (es. "INT", "VARCHAR", "TEXT", "BOOL").
 */
struct ColumnSchema {
    std::string name;
    std::string type;

    bool is_int() const { return type == "INT"; }
    bool is_varchar() const { return type == "VARCHAR" || type == "TEXT"; }
};

/**
 * Valore di una cella: può essere NULL, intero (int64) o stringa.
 * Usato sia per i valori nelle righe che per le costanti nel parser.
 */
struct Value {
    enum Type : uint8_t { NULL_VAL = 0, INT_VAL = 1, STR_VAL = 2 };

    Type type = NULL_VAL;
    int64_t int_val = 0;
    std::string str_val;

    Value() = default;
    explicit Value(int64_t v) : type(INT_VAL), int_val(v) {}
    explicit Value(std::string v) : type(STR_VAL), str_val(std::move(v)) {}
    explicit Value(const char* v) : type(STR_VAL), str_val(v) {}

    std::string to_string() const {
        switch (type) {
            case NULL_VAL: return "NULL";
            case INT_VAL: return std::to_string(int_val);
            case STR_VAL: return str_val;
        }
        return "?";
    }
};

/**
 * Una riga del database: rowid auto-incrementante + array di valori.
 */
struct Row {
    int64_t rowid = 0;
    std::vector<Value> values;
};

// ── serializzazione riga → byte-array ────────────────────────────

/**
 * Converte una Row in un blob binario da salvare nel B-tree.
 *
 * Formato little-endian:
 *   [2 byte] numero colonne
 *   per ogni colonna:
 *     [1 byte] tipo (0=NULL, 1=INT, 2=STRING)
 *     INT:     [8 byte] valore int64
 *     STRING:  [4 byte] lunghezza stringa + [N byte] dati
 *     NULL:    (nessun dato extra)
 */
inline std::vector<uint8_t> serialize_row(const Row& row) {
    std::vector<uint8_t> buf;
    auto put_u16 = [&](uint16_t v) {
        buf.push_back((uint8_t)(v & 0xFF));
        buf.push_back((uint8_t)((v >> 8) & 0xFF));
    };
    auto put_u32 = [&](uint32_t v) {
        buf.push_back((uint8_t)(v & 0xFF));
        buf.push_back((uint8_t)((v >> 8) & 0xFF));
        buf.push_back((uint8_t)((v >> 16) & 0xFF));
        buf.push_back((uint8_t)((v >> 24) & 0xFF));
    };
    auto put_u64 = [&](uint64_t v) {
        for (int i = 0; i < 8; i++) {
            buf.push_back((uint8_t)(v & 0xFF));
            v >>= 8;
        }
    };

    put_u16((uint16_t)row.values.size());
    for (const auto& val : row.values) {
        buf.push_back((uint8_t)val.type);
        switch (val.type) {
            case Value::INT_VAL:
                put_u64((uint64_t)val.int_val);
                break;
            case Value::STR_VAL:
                put_u32((uint32_t)val.str_val.size());
                for (char c : val.str_val)
                    buf.push_back((uint8_t)c);
                break;
            default:
                break;
        }
    }
    return buf;
}

/**
 * Ricostruisce una Row da un blob binario (formato serialize_row).
 */
inline Row deserialize_row(const std::vector<uint8_t>& data,
                           const std::vector<ColumnSchema>&) {
    Row row;
    size_t pos = 0;
    if (pos + 2 > data.size()) return row;

    auto read_u16 = [&]() -> uint16_t {
        if (pos + 2 > data.size()) return 0;
        uint16_t v = (uint16_t)data[pos] | ((uint16_t)data[pos + 1] << 8);
        pos += 2;
        return v;
    };
    auto read_u32 = [&]() -> uint32_t {
        if (pos + 4 > data.size()) return 0;
        uint32_t v = (uint32_t)data[pos] | ((uint32_t)data[pos + 1] << 8) |
                     ((uint32_t)data[pos + 2] << 16) | ((uint32_t)data[pos + 3] << 24);
        pos += 4;
        return v;
    };
    auto read_u64 = [&]() -> uint64_t {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) {
            if (pos >= data.size()) return v;
            v |= ((uint64_t)data[pos] << (i * 8));
            pos++;
        }
        return v;
    };

    uint16_t num_cols = read_u16();
    row.values.reserve(num_cols);
    for (uint16_t i = 0; i < num_cols; i++) {
        if (pos >= data.size()) break;
        Value v;
        v.type = (Value::Type)data[pos++];
        switch (v.type) {
            case Value::INT_VAL:
                v.int_val = (int64_t)read_u64();
                break;
            case Value::STR_VAL: {
                uint32_t len = read_u32();
                v.str_val.assign(data.begin() + pos, data.begin() + pos + len);
                pos += len;
                break;
            }
            default:
                break;
        }
        row.values.push_back(std::move(v));
    }
    return row;
}
