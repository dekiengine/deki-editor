#pragma once

#include <deki/reflection/Property.h>
#include <deki/reflection/ObjectRef.h>
#include <deki/Component.h>
#include <deki/Color.h>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace DekiEditor
{

// Read a property's value at `offset` into JSON, and write JSON back into it.
using CaptureFunc = nlohmann::json (*)(const Deki::Component* comp, size_t offset);
using ApplyFunc = void (*)(Deki::Component* comp, size_t offset, const nlohmann::json& value);

struct PropertyTypeHandler
{
    CaptureFunc capture;
    ApplyFunc apply;
};

// Capture functions for each type
inline nlohmann::json CaptureInt8(const Deki::Component* comp, size_t offset)
{
    return static_cast<int32_t>(*reinterpret_cast<const int8_t*>(reinterpret_cast<const char*>(comp) + offset));
}

inline nlohmann::json CaptureInt16(const Deki::Component* comp, size_t offset)
{
    return static_cast<int32_t>(*reinterpret_cast<const int16_t*>(reinterpret_cast<const char*>(comp) + offset));
}

inline nlohmann::json CaptureInt32(const Deki::Component* comp, size_t offset)
{
    return *reinterpret_cast<const int32_t*>(reinterpret_cast<const char*>(comp) + offset);
}

inline nlohmann::json CaptureInt64(const Deki::Component* comp, size_t offset)
{
    return *reinterpret_cast<const int64_t*>(reinterpret_cast<const char*>(comp) + offset);
}

inline nlohmann::json CaptureUInt8(const Deki::Component* comp, size_t offset)
{
    return static_cast<int32_t>(*reinterpret_cast<const uint8_t*>(reinterpret_cast<const char*>(comp) + offset));
}

inline nlohmann::json CaptureUInt16(const Deki::Component* comp, size_t offset)
{
    return static_cast<int32_t>(*reinterpret_cast<const uint16_t*>(reinterpret_cast<const char*>(comp) + offset));
}

inline nlohmann::json CaptureUInt32(const Deki::Component* comp, size_t offset)
{
    return *reinterpret_cast<const uint32_t*>(reinterpret_cast<const char*>(comp) + offset);
}

inline nlohmann::json CaptureUInt64(const Deki::Component* comp, size_t offset)
{
    return *reinterpret_cast<const uint64_t*>(reinterpret_cast<const char*>(comp) + offset);
}

inline nlohmann::json CaptureFloat(const Deki::Component* comp, size_t offset)
{
    return *reinterpret_cast<const float*>(reinterpret_cast<const char*>(comp) + offset);
}

inline nlohmann::json CaptureDouble(const Deki::Component* comp, size_t offset)
{
    return *reinterpret_cast<const double*>(reinterpret_cast<const char*>(comp) + offset);
}

inline nlohmann::json CaptureBool(const Deki::Component* comp, size_t offset)
{
    return *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(comp) + offset);
}

inline nlohmann::json CaptureString(const Deki::Component* comp, size_t offset)
{
    return *reinterpret_cast<const std::string*>(reinterpret_cast<const char*>(comp) + offset);
}

inline nlohmann::json CaptureColor(const Deki::Component* comp, size_t offset)
{
    const uint8_t* colorPtr = reinterpret_cast<const uint8_t*>(reinterpret_cast<const char*>(comp) + offset);
    return nlohmann::json::array({ colorPtr[0], colorPtr[1], colorPtr[2], colorPtr[3] });
}

inline nlohmann::json CaptureVector2(const Deki::Component* comp, size_t offset)
{
    const float* ptr = reinterpret_cast<const float*>(reinterpret_cast<const char*>(comp) + offset);
    return nlohmann::json::array({ ptr[0], ptr[1] });
}

inline nlohmann::json CaptureVector3(const Deki::Component* comp, size_t offset)
{
    const float* ptr = reinterpret_cast<const float*>(reinterpret_cast<const char*>(comp) + offset);
    return nlohmann::json::array({ ptr[0], ptr[1], ptr[2] });
}

inline nlohmann::json CaptureEnum(const Deki::Component* comp, size_t offset)
{
    return *reinterpret_cast<const int*>(reinterpret_cast<const char*>(comp) + offset);
}

// Reads only `enumSize` bytes (Deki::PropertyInfo::enumSize). Use it over
// CaptureEnum whenever the size is known: CaptureEnum always reads 4 bytes,
// which takes in the next fields when the enum is uint8_t- or uint16_t-backed.
inline nlohmann::json CaptureEnumSized(const Deki::Component* comp, size_t offset, uint8_t enumSize)
{
    const char* base = reinterpret_cast<const char*>(comp) + offset;
    switch (enumSize)
    {
        case 1: return static_cast<int>(*reinterpret_cast<const uint8_t*>(base));
        case 2: return static_cast<int>(*reinterpret_cast<const uint16_t*>(base));
        default: return *reinterpret_cast<const int32_t*>(base);
    }
}

inline nlohmann::json CaptureObjectRef(const Deki::Component* comp, size_t offset)
{
    const Deki::ObjectRefBase& ref =
        *reinterpret_cast<const Deki::ObjectRefBase*>(reinterpret_cast<const char*>(comp) + offset);
    if (ref.componentTypeId != 0)
    {
        return nlohmann::json{ { "id", ref.id }, { "typeId", ref.componentTypeId } };
    }
    return nlohmann::json(ref.id);
}

inline nlohmann::json CaptureNoop(const Deki::Component*, size_t)
{
    return nlohmann::json();
}

// Apply functions for each type
inline void ApplyInt8(Deki::Component* comp, size_t offset, const nlohmann::json& value)
{
    if (value.is_number_integer())
    {
        *reinterpret_cast<int8_t*>(reinterpret_cast<char*>(comp) + offset) = static_cast<int8_t>(value.get<int32_t>());
    }
}

inline void ApplyInt16(Deki::Component* comp, size_t offset, const nlohmann::json& value)
{
    if (value.is_number_integer())
    {
        *reinterpret_cast<int16_t*>(reinterpret_cast<char*>(comp) + offset) =
            static_cast<int16_t>(value.get<int32_t>());
    }
}

inline void ApplyInt32(Deki::Component* comp, size_t offset, const nlohmann::json& value)
{
    if (value.is_number_integer())
    {
        *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(comp) + offset) = value.get<int32_t>();
    }
}

inline void ApplyInt64(Deki::Component* comp, size_t offset, const nlohmann::json& value)
{
    if (value.is_number_integer())
    {
        *reinterpret_cast<int64_t*>(reinterpret_cast<char*>(comp) + offset) = value.get<int64_t>();
    }
}

inline void ApplyUInt8(Deki::Component* comp, size_t offset, const nlohmann::json& value)
{
    if (value.is_number_integer())
    {
        *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(comp) + offset) =
            static_cast<uint8_t>(value.get<int32_t>());
    }
}

inline void ApplyUInt16(Deki::Component* comp, size_t offset, const nlohmann::json& value)
{
    if (value.is_number_integer())
    {
        *reinterpret_cast<uint16_t*>(reinterpret_cast<char*>(comp) + offset) =
            static_cast<uint16_t>(value.get<int32_t>());
    }
}

inline void ApplyUInt32(Deki::Component* comp, size_t offset, const nlohmann::json& value)
{
    if (value.is_number_unsigned() || value.is_number_integer())
    {
        *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(comp) + offset) = value.get<uint32_t>();
    }
}

inline void ApplyUInt64(Deki::Component* comp, size_t offset, const nlohmann::json& value)
{
    if (value.is_number_unsigned() || value.is_number_integer())
    {
        *reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(comp) + offset) = value.get<uint64_t>();
    }
}

inline void ApplyFloat(Deki::Component* comp, size_t offset, const nlohmann::json& value)
{
    if (value.is_number())
    {
        *reinterpret_cast<float*>(reinterpret_cast<char*>(comp) + offset) = value.get<float>();
    }
}

inline void ApplyDouble(Deki::Component* comp, size_t offset, const nlohmann::json& value)
{
    if (value.is_number())
    {
        *reinterpret_cast<double*>(reinterpret_cast<char*>(comp) + offset) = value.get<double>();
    }
}

inline void ApplyBool(Deki::Component* comp, size_t offset, const nlohmann::json& value)
{
    if (value.is_boolean())
    {
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(comp) + offset) = value.get<bool>();
    }
}

inline void ApplyString(Deki::Component* comp, size_t offset, const nlohmann::json& value)
{
    if (value.is_string())
    {
        *reinterpret_cast<std::string*>(reinterpret_cast<char*>(comp) + offset) = value.get<std::string>();
    }
}

inline void ApplyColor(Deki::Component* comp, size_t offset, const nlohmann::json& value)
{
    uint8_t* colorPtr = reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(comp) + offset);
    if (value.is_array() && value.size() >= 4)
    {
        colorPtr[0] = value[0].get<uint8_t>();
        colorPtr[1] = value[1].get<uint8_t>();
        colorPtr[2] = value[2].get<uint8_t>();
        colorPtr[3] = value[3].get<uint8_t>();
    }
    else if (value.is_string())
    {
        // "#RRGGBBAA" or "#RRGGBB", as scene files write colours.
        const std::string hex = value.get<std::string>();
        if ((hex.size() != 7 && hex.size() != 9) || hex[0] != '#' ||
            hex.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos)
        {
            return;
        }
        for (size_t i = 0; i < 4; ++i)
        {
            colorPtr[i] =
                1 + 2 * i < hex.size() ? static_cast<uint8_t>(std::stoul(hex.substr(1 + 2 * i, 2), nullptr, 16)) : 255;
        }
    }
}

inline void ApplyVector2(Deki::Component* comp, size_t offset, const nlohmann::json& value)
{
    if (value.is_array() && value.size() >= 2)
    {
        float* ptr = reinterpret_cast<float*>(reinterpret_cast<char*>(comp) + offset);
        ptr[0] = value[0].get<float>();
        ptr[1] = value[1].get<float>();
    }
}

inline void ApplyVector3(Deki::Component* comp, size_t offset, const nlohmann::json& value)
{
    if (value.is_array() && value.size() >= 3)
    {
        float* ptr = reinterpret_cast<float*>(reinterpret_cast<char*>(comp) + offset);
        ptr[0] = value[0].get<float>();
        ptr[1] = value[1].get<float>();
        ptr[2] = value[2].get<float>();
    }
}

inline void ApplyEnum(Deki::Component* comp, size_t offset, const nlohmann::json& value)
{
    if (value.is_number_integer())
    {
        *reinterpret_cast<int*>(reinterpret_cast<char*>(comp) + offset) = value.get<int>();
    }
}

// Writes only `enumSize` bytes, so a uint8_t-backed enum does not overwrite
// the 3 bytes after it in the struct.
inline void ApplyEnumSized(Deki::Component* comp, size_t offset, const nlohmann::json& value, uint8_t enumSize)
{
    if (!value.is_number_integer())
    {
        return;
    }
    int v = value.get<int>();
    char* base = reinterpret_cast<char*>(comp) + offset;
    switch (enumSize)
    {
        case 1: *reinterpret_cast<uint8_t*>(base) = static_cast<uint8_t>(v); break;
        case 2: *reinterpret_cast<uint16_t*>(base) = static_cast<uint16_t>(v); break;
        default: *reinterpret_cast<int32_t*>(base) = static_cast<int32_t>(v); break;
    }
}

inline void ApplyObjectRef(Deki::Component* comp, size_t offset, const nlohmann::json& value)
{
    Deki::ObjectRefBase& ref = *reinterpret_cast<Deki::ObjectRefBase*>(reinterpret_cast<char*>(comp) + offset);
    if (value.is_object())
    {
        ref.id = value.value("id", 0u);
        ref.componentTypeId = value.value("typeId", 0u);
    }
    else if (value.is_number_unsigned() || value.is_number_integer())
    {
        ref.id = value.get<uint32_t>();
        ref.componentTypeId = 0;
    }
    ref.resolved = nullptr;
}

// Reads an array property, by the element type in the property info.
inline nlohmann::json CaptureArray(const Deki::Component* comp, const Deki::PropertyInfo& prop)
{
    const void* ptr = reinterpret_cast<const char*>(comp) + prop.offset;
    switch (prop.elementType)
    {
        case Deki::PropertyType::String:
        {
            const auto& vec = *reinterpret_cast<const std::vector<std::string>*>(ptr);
            return nlohmann::json(vec);
        }
        case Deki::PropertyType::Int32:
        {
            const auto& vec = *reinterpret_cast<const std::vector<int32_t>*>(ptr);
            return nlohmann::json(vec);
        }
        case Deki::PropertyType::Float:
        {
            const auto& vec = *reinterpret_cast<const std::vector<float>*>(ptr);
            return nlohmann::json(vec);
        }
        case Deki::PropertyType::Bool:
        {
            const auto& vec = *reinterpret_cast<const std::vector<bool>*>(ptr);
            return nlohmann::json(vec);
        }
        case Deki::PropertyType::ObjectRef:
        {
            const auto& vec = *reinterpret_cast<const std::vector<Deki::ObjectRefBase>*>(ptr);
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& ref : vec)
            {
                if (ref.componentTypeId != 0)
                {
                    arr.push_back(nlohmann::json{ { "id", ref.id }, { "typeId", ref.componentTypeId } });
                }
                else
                {
                    arr.push_back(ref.id);
                }
            }
            return arr;
        }
        default: return nlohmann::json::array();
    }
}

// Writes an array property, by the element type in the property info.
inline void ApplyArray(Deki::Component* comp, const Deki::PropertyInfo& prop, const nlohmann::json& value)
{
    if (!value.is_array())
    {
        return;
    }
    void* ptr = reinterpret_cast<char*>(comp) + prop.offset;
    switch (prop.elementType)
    {
        case Deki::PropertyType::String:
        {
            auto& vec = *reinterpret_cast<std::vector<std::string>*>(ptr);
            vec.clear();
            for (const auto& item : value)
            {
                if (item.is_string())
                {
                    vec.push_back(item.get<std::string>());
                }
            }
            break;
        }
        case Deki::PropertyType::Int32:
        {
            auto& vec = *reinterpret_cast<std::vector<int32_t>*>(ptr);
            vec.clear();
            for (const auto& item : value)
            {
                if (item.is_number_integer())
                {
                    vec.push_back(item.get<int32_t>());
                }
            }
            break;
        }
        case Deki::PropertyType::Float:
        {
            auto& vec = *reinterpret_cast<std::vector<float>*>(ptr);
            vec.clear();
            for (const auto& item : value)
            {
                if (item.is_number())
                {
                    vec.push_back(item.get<float>());
                }
            }
            break;
        }
        case Deki::PropertyType::Bool:
        {
            auto& vec = *reinterpret_cast<std::vector<bool>*>(ptr);
            vec.clear();
            for (const auto& item : value)
            {
                if (item.is_boolean())
                {
                    vec.push_back(item.get<bool>());
                }
            }
            break;
        }
        case Deki::PropertyType::ObjectRef:
        {
            auto& vec = *reinterpret_cast<std::vector<Deki::ObjectRefBase>*>(ptr);
            vec.clear();
            for (const auto& item : value)
            {
                Deki::ObjectRefBase ref;
                ref.resolved = nullptr;
                if (item.is_object())
                {
                    ref.id = item.value("id", 0u);
                    ref.componentTypeId = item.value("typeId", 0u);
                }
                else if (item.is_number_unsigned() || item.is_number_integer())
                {
                    ref.id = item.get<uint32_t>();
                    ref.componentTypeId = 0;
                }
                vec.push_back(ref);
            }
            break;
        }
        default: break;
    }
}

inline void ApplyNoop(Deki::Component*, size_t, const nlohmann::json&)
{
}

// The handler for a property type. One case per enumerator, by name, so
// reordering Deki::PropertyType cannot pair a type with another type's
// handlers. Must not become a table indexed by the enum value.
inline const PropertyTypeHandler& GetPropertyTypeHandler(Deki::PropertyType type)
{
    static const PropertyTypeHandler kInt8 = { CaptureInt8, ApplyInt8 };
    static const PropertyTypeHandler kInt16 = { CaptureInt16, ApplyInt16 };
    static const PropertyTypeHandler kInt32 = { CaptureInt32, ApplyInt32 };
    static const PropertyTypeHandler kInt64 = { CaptureInt64, ApplyInt64 };
    static const PropertyTypeHandler kUInt8 = { CaptureUInt8, ApplyUInt8 };
    static const PropertyTypeHandler kUInt16 = { CaptureUInt16, ApplyUInt16 };
    static const PropertyTypeHandler kUInt32 = { CaptureUInt32, ApplyUInt32 };
    static const PropertyTypeHandler kUInt64 = { CaptureUInt64, ApplyUInt64 };
    static const PropertyTypeHandler kFloat = { CaptureFloat, ApplyFloat };
    static const PropertyTypeHandler kDouble = { CaptureDouble, ApplyDouble };
    static const PropertyTypeHandler kBool = { CaptureBool, ApplyBool };
    static const PropertyTypeHandler kString = { CaptureString, ApplyString };
    static const PropertyTypeHandler kColor = { CaptureColor, ApplyColor };
    static const PropertyTypeHandler kVector2 = { CaptureVector2, ApplyVector2 };
    static const PropertyTypeHandler kVector3 = { CaptureVector3, ApplyVector3 };
    static const PropertyTypeHandler kEnum = { CaptureEnum, ApplyEnum };
    static const PropertyTypeHandler kObjectRef = { CaptureObjectRef, ApplyObjectRef };
    static const PropertyTypeHandler kNoop = { CaptureNoop, ApplyNoop };

    switch (type)
    {
        case Deki::PropertyType::Int8: return kInt8;
        case Deki::PropertyType::Int16: return kInt16;
        case Deki::PropertyType::Int32: return kInt32;
        case Deki::PropertyType::Int64: return kInt64;
        case Deki::PropertyType::UInt8: return kUInt8;
        case Deki::PropertyType::UInt16: return kUInt16;
        case Deki::PropertyType::UInt32: return kUInt32;
        case Deki::PropertyType::UInt64: return kUInt64;
        case Deki::PropertyType::Float: return kFloat;
        case Deki::PropertyType::Double: return kDouble;
        case Deki::PropertyType::Bool: return kBool;
        case Deki::PropertyType::String: return kString;
        case Deki::PropertyType::Color: return kColor;
        case Deki::PropertyType::Vector2: return kVector2;
        case Deki::PropertyType::Vector3: return kVector3;
        case Deki::PropertyType::Enum: return kEnum;
        case Deki::PropertyType::ObjectRef: return kObjectRef;
        case Deki::PropertyType::Array:     // CaptureArray/ApplyArray, with the property info
        case Deki::PropertyType::AssetRef:  // handled separately
        case Deki::PropertyType::NodeRef:
        case Deki::PropertyType::PropertyRef:
        default: return kNoop;
    }
}

}  // namespace DekiEditor
