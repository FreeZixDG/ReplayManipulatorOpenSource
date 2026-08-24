#include "pch.h"
#include "PlayerPreset.h"

// Everything here is deliberately hand-rolled instead of nlohmann to_json/from_json overloads.
// These are SDK types shared with the rest of the plugin, and free ADL serializers for them
// would be picked up project-wide -- this way the preset file format stays local to this file.
namespace
{
/// Bumped when the on-disk shape changes in a way older readers cannot cope with. Adding an
/// optional section does not need a bump, since a missing section already means "not stored".
constexpr int kFormatVersion = 1;

template <typename T>
T ReadNumber(const json& j, const char* key, T fallback)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number())
    {
        return fallback;
    }
    return it->get<T>();
}

json ToJson(const LinearColor& color)
{
    return json{{"r", color.R}, {"g", color.G}, {"b", color.B}, {"a", color.A}};
}

LinearColor LinearColorFromJson(const json& j)
{
    return LinearColor{
        ReadNumber(j, "r", 0.0f), ReadNumber(j, "g", 0.0f),
        ReadNumber(j, "b", 0.0f), ReadNumber(j, "a", 1.0f)
    };
}

json ToJson(const UnrealColor& color)
{
    return json{{"r", color.R}, {"g", color.G}, {"b", color.B}, {"a", color.A}};
}

UnrealColor UnrealColorFromJson(const json& j)
{
    return UnrealColor{
        ReadNumber<unsigned char>(j, "b", 0), ReadNumber<unsigned char>(j, "g", 0),
        ReadNumber<unsigned char>(j, "r", 0), ReadNumber<unsigned char>(j, "a", 0)
    };
}

json ToJson(const pluginsdk::TeamPaint& paint)
{
    return json{
        {"team", paint.team},
        {"team_color_id", paint.team_color_id},
        {"custom_color_id", paint.custom_color_id}
    };
}

pluginsdk::TeamPaint TeamPaintFromJson(const json& j)
{
    return pluginsdk::TeamPaint{
        ReadNumber<unsigned char>(j, "team", 0),
        ReadNumber<unsigned char>(j, "team_color_id", 255),
        ReadNumber<unsigned char>(j, "custom_color_id", 255)
    };
}

json ToJson(const pluginsdk::CarColors& colors)
{
    json j = json::object();
    if (colors.team_paint)
    {
        j["team_paint"] = ToJson(*colors.team_paint);
    }
    if (colors.team_color_override)
    {
        j["team_color_override"] = ToJson(*colors.team_color_override);
    }
    if (colors.custom_color_override)
    {
        j["custom_color_override"] = ToJson(*colors.custom_color_override);
    }
    return j;
}

pluginsdk::CarColors CarColorsFromJson(const json& j)
{
    pluginsdk::CarColors colors{};
    if (const auto it = j.find("team_paint"); it != j.end() && it->is_object())
    {
        colors.team_paint = TeamPaintFromJson(*it);
    }
    if (const auto it = j.find("team_color_override"); it != j.end() && it->is_object())
    {
        colors.team_color_override = LinearColorFromJson(*it);
    }
    if (const auto it = j.find("custom_color_override"); it != j.end() && it->is_object())
    {
        colors.custom_color_override = LinearColorFromJson(*it);
    }
    return colors;
}

json ToJson(const pluginsdk::ItemAttribute& attribute)
{
    return json{
        {"type", static_cast<int>(attribute.type)},
        {"value", attribute.value},
        {"color", ToJson(attribute.color)}
    };
}

pluginsdk::ItemAttribute ItemAttributeFromJson(const json& j)
{
    pluginsdk::ItemAttribute attribute{};
    const auto type = ReadNumber(j, "type", static_cast<int>(pluginsdk::ItemAttribute::AttributeType::UNKNOWN));
    if (type >= static_cast<int>(pluginsdk::ItemAttribute::AttributeType::UNKNOWN) &&
        type <= static_cast<int>(pluginsdk::ItemAttribute::AttributeType::USERCOLOR))
    {
        attribute.type = static_cast<pluginsdk::ItemAttribute::AttributeType>(type);
    }
    attribute.value = ReadNumber(j, "value", 0);
    if (const auto it = j.find("color"); it != j.end() && it->is_object())
    {
        attribute.color = UnrealColorFromJson(*it);
    }
    return attribute;
}

json ToJson(const pluginsdk::Loadout& loadout)
{
    json items = json::array();
    for (const auto& [slot, item] : loadout.items)
    {
        json attributes = json::array();
        for (const auto& attribute : item.attributes)
        {
            attributes.push_back(ToJson(attribute));
        }
        // The slot is written next to the item rather than used as an object key: json object
        // keys are strings, and a plain integer array round-trips without any parsing.
        items.push_back(json{
            {"slot", static_cast<int>(slot)},
            {"product_id", item.product_id},
            {"attributes", attributes}
        });
    }

    return json{{"items", items}, {"paint_finish", ToJson(loadout.paint_finish)}};
}

pluginsdk::Loadout LoadoutFromJson(const json& j)
{
    pluginsdk::Loadout loadout{};

    if (const auto items = j.find("items"); items != j.end() && items->is_array())
    {
        for (const auto& entry : *items)
        {
            if (!entry.is_object())
                continue;
            const auto slot_id = ReadNumber(entry, "slot", static_cast<int>(pluginsdk::Equipslot::MAX));
            if (slot_id < 0 || slot_id >= static_cast<int>(pluginsdk::Equipslot::MAX))
                continue;
            const auto slot = static_cast<pluginsdk::Equipslot>(slot_id);

            pluginsdk::ItemData item{};
            item.slot = slot;
            item.product_id = ReadNumber(entry, "product_id", 0);
            if (const auto attributes = entry.find("attributes"); attributes != entry.end() && attributes->is_array())
            {
                for (const auto& attribute : *attributes)
                {
                    if (attribute.is_object())
                    {
                        item.attributes.push_back(ItemAttributeFromJson(attribute));
                    }
                }
            }
            loadout.items[slot] = item;
        }
    }

    if (const auto paint = j.find("paint_finish"); paint != j.end() && paint->is_object())
    {
        loadout.paint_finish = CarColorsFromJson(*paint);
    }

    return loadout;
}

json ToJson(const CameraPreset& camera)
{
    const auto& s = camera.settings;
    return json{
        {"enabled", camera.enabled},
        {"fov", s.FOV},
        {"height", s.Height},
        {"pitch", s.Pitch},
        {"distance", s.Distance},
        {"stiffness", s.Stiffness},
        {"swivel_speed", s.SwivelSpeed},
        {"transition_speed", s.TransitionSpeed}
    };
}

CameraPreset CameraPresetFromJson(const json& j)
{
    CameraPreset camera{};
    if (const auto it = j.find("enabled"); it != j.end() && it->is_boolean())
    {
        camera.enabled = it->get<bool>();
    }
    camera.settings.FOV = ReadNumber(j, "fov", 90.0f);
    camera.settings.Height = ReadNumber(j, "height", 100.0f);
    camera.settings.Pitch = ReadNumber(j, "pitch", -5.0f);
    camera.settings.Distance = ReadNumber(j, "distance", 270.0f);
    camera.settings.Stiffness = ReadNumber(j, "stiffness", 0.5f);
    camera.settings.SwivelSpeed = ReadNumber(j, "swivel_speed", 5.0f);
    camera.settings.TransitionSpeed = ReadNumber(j, "transition_speed", 1.0f);
    return camera;
}

std::optional<std::string> OptionalStringFromJson(const json& j, const char* key)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string())
    {
        return std::nullopt;
    }
    return it->get<std::string>();
}
}

bool PlayerPreset::IsEmpty() const
{
    return !loadout && !custom_decal_name && !camera && !player_name && !title_id;
}

PlayerPresetSelection PlayerPresetSelection::ForPreset(const PlayerPreset& preset)
{
    return PlayerPresetSelection{
        .loadout = preset.loadout.has_value(),
        .custom_decal = preset.custom_decal_name.has_value(),
        .camera = preset.camera.has_value(),
        .player_name = preset.player_name.has_value(),
        .title = preset.title_id.has_value()
    };
}

PlayerPreset FilterPreset(const PlayerPreset& preset, const PlayerPresetSelection& selection)
{
    PlayerPreset filtered = preset;
    if (!selection.loadout)
        filtered.loadout.reset();
    if (!selection.custom_decal)
        filtered.custom_decal_name.reset();
    if (!selection.camera)
        filtered.camera.reset();
    if (!selection.player_name)
        filtered.player_name.reset();
    if (!selection.title)
        filtered.title_id.reset();
    return filtered;
}

json PlayerPresetToJson(const PlayerPreset& preset)
{
    json j{{"format_version", kFormatVersion}, {"name", preset.name}};

    if (preset.loadout)
    {
        j["loadout"] = ToJson(*preset.loadout);
    }
    if (preset.custom_decal_name)
    {
        j["custom_decal_name"] = *preset.custom_decal_name;
    }
    if (preset.camera)
    {
        j["camera"] = ToJson(*preset.camera);
    }
    if (preset.player_name)
    {
        j["player_name"] = *preset.player_name;
    }
    if (preset.title_id)
    {
        j["title_id"] = *preset.title_id;
    }

    return j;
}

PlayerPreset PlayerPresetFromJson(const json& j, const std::string& fallback_name)
{
    if (!j.is_object())
    {
        throw std::invalid_argument("the file does not hold a json object");
    }

    const auto version = ReadNumber(j, "format_version", kFormatVersion);
    if (version > kFormatVersion)
    {
        throw std::invalid_argument("the file was written by a newer version of the plugin");
    }

    PlayerPreset preset;
    preset.name = OptionalStringFromJson(j, "name").value_or(fallback_name);
    if (preset.name.empty())
    {
        preset.name = fallback_name;
    }

    if (const auto it = j.find("loadout"); it != j.end() && it->is_object())
    {
        preset.loadout = LoadoutFromJson(*it);
    }
    preset.custom_decal_name = OptionalStringFromJson(j, "custom_decal_name");
    if (const auto it = j.find("camera"); it != j.end() && it->is_object())
    {
        preset.camera = CameraPresetFromJson(*it);
    }
    preset.player_name = OptionalStringFromJson(j, "player_name");
    preset.title_id = OptionalStringFromJson(j, "title_id");

    if (preset.IsEmpty())
    {
        throw std::invalid_argument("the preset holds nothing to apply");
    }

    return preset;
}
