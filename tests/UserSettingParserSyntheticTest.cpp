#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Data/Builders/VectorBuilder.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Model/Types.h"
#include "WallpaperEngine/Data/Model/UserSetting.h"
#include "WallpaperEngine/Data/Parsers/UserSettingParser.h"

using namespace WallpaperEngine::Data;
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Parsers;

namespace {

/**
 * Build a minimal Property map for tests. Most tests don't care about the
 * property binding, only that the parser doesn't crash when the map is empty.
 */
Properties emptyProperties () {
    return {};
}

/**
 * Build a Property map containing a single named text-property. Used by
 * tests that exercise the `user` binding path. PropertyText is the simplest
 * concrete subclass that satisfies the abstract Property interface.
 */
Properties singleTextProperty (const std::string& name) {
    Properties props;
    PropertyData pd { .name = name, .text = name };
    auto prop = std::make_shared<PropertyText> (std::move (pd));
    props.emplace (name, std::move (prop));
    return props;
}

} // namespace

// ---------- 2D / 3D / 4D vector happy paths -----------------------------------

TEST_CASE ("UserSettingParser parses 2D vector string") {
    const auto data = nlohmann::json {{"value", "1.5 2.5"}};
    auto setting = UserSettingParser::parse (data, emptyProperties ());

    REQUIRE (setting != nullptr);
    REQUIRE (setting->value->getType () == DynamicValue::UnderlyingType::Vec2);
    REQUIRE (setting->value->getVec2 ().x == 1.5f);
    REQUIRE (setting->value->getVec2 ().y == 2.5f);
}

TEST_CASE ("UserSettingParser parses 3D vector string") {
    const auto data = nlohmann::json {{"value", "1.0 2.0 3.0"}};
    auto setting = UserSettingParser::parse (data, emptyProperties ());

    REQUIRE (setting != nullptr);
    REQUIRE (setting->value->getType () == DynamicValue::UnderlyingType::Vec3);
    REQUIRE (setting->value->getVec3 ().x == 1.0f);
    REQUIRE (setting->value->getVec3 ().y == 2.0f);
    REQUIRE (setting->value->getVec3 ().z == 3.0f);
}

TEST_CASE ("UserSettingParser parses 4D vector string") {
    const auto data = nlohmann::json {{"value", "1.0 2.0 3.0 4.0"}};
    auto setting = UserSettingParser::parse (data, emptyProperties ());

    REQUIRE (setting != nullptr);
    REQUIRE (setting->value->getType () == DynamicValue::UnderlyingType::Vec4);
    REQUIRE (setting->value->getVec4 ().w == 4.0f);
}

// ---------- Bug B regression: non-vector strings must not throw --------------
//
// Before the fix in UserSettingParser.cpp, a user-setting whose string value
// was a non-vector token (e.g. "top" in some ULTRAKILL scene variants) was
// forwarded to VectorBuilder::preparseSize, which throws sLog.exception()
// on the malformed input. The whole scene then failed to load.

TEST_CASE ("UserSettingParser [bug B] does not throw on non-vector string \"top\"") {
    const auto data = nlohmann::json {{"value", "top"}};
    auto setting = UserSettingParser::parse (data, emptyProperties ());

    REQUIRE (setting != nullptr);
    REQUIRE (setting->value != nullptr);
}

TEST_CASE ("UserSettingParser [bug B] does not throw on empty string") {
    const auto data = nlohmann::json {{"value", ""}};
    auto setting = UserSettingParser::parse (data, emptyProperties ());

    REQUIRE (setting != nullptr);
    REQUIRE (setting->value != nullptr);
}

TEST_CASE ("UserSettingParser [bug B] does not throw on arbitrary non-vector token") {
    const auto data = nlohmann::json {{"value", "some-identifier"}};
    auto setting = UserSettingParser::parse (data, emptyProperties ());

    REQUIRE (setting != nullptr);
    REQUIRE (setting->value != nullptr);
}

TEST_CASE ("UserSettingParser [bug B] non-vector string leaves value at default Null type") {
    const auto data = nlohmann::json {{"value", "top"}};
    auto setting = UserSettingParser::parse (data, emptyProperties ());

    REQUIRE (setting != nullptr);
    // The fix logs an error and bails before calling preparseSize; the
    // DynamicValue stays at its constructor default (Null type).
    REQUIRE (setting->value->getType () == DynamicValue::UnderlyingType::Null);
}

// ---------- Scalar types ------------------------------------------------------

TEST_CASE ("UserSettingParser parses integer value") {
    const auto data = nlohmann::json {{"value", 42}};
    auto setting = UserSettingParser::parse (data, emptyProperties ());

    REQUIRE (setting != nullptr);
    REQUIRE (setting->value->getType () == DynamicValue::UnderlyingType::Int);
    REQUIRE (setting->value->getInt () == 42);
}

TEST_CASE ("UserSettingParser parses float value") {
    const auto data = nlohmann::json {{"value", 3.14}};
    auto setting = UserSettingParser::parse (data, emptyProperties ());

    REQUIRE (setting != nullptr);
    REQUIRE (setting->value->getType () == DynamicValue::UnderlyingType::Float);
    REQUIRE (setting->value->getFloat () == 3.14f);
}

TEST_CASE ("UserSettingParser parses boolean true") {
    const auto data = nlohmann::json {{"value", true}};
    auto setting = UserSettingParser::parse (data, emptyProperties ());

    REQUIRE (setting != nullptr);
    REQUIRE (setting->value->getType () == DynamicValue::UnderlyingType::Boolean);
    REQUIRE (setting->value->getBool () == true);
}

TEST_CASE ("UserSettingParser parses boolean false") {
    const auto data = nlohmann::json {{"value", false}};
    auto setting = UserSettingParser::parse (data, emptyProperties ());

    REQUIRE (setting != nullptr);
    REQUIRE (setting->value->getBool () == false);
}

TEST_CASE ("UserSettingParser handles null value") {
    const auto data = nlohmann::json {{"value", nullptr}};
    auto setting = UserSettingParser::parse (data, emptyProperties ());

    REQUIRE (setting != nullptr);
    // null with no script: DynamicValue stays at default Null type.
    REQUIRE (setting->value->getType () == DynamicValue::UnderlyingType::Null);
}

// ---------- Validation errors --------------------------------------------------

TEST_CASE ("UserSettingParser throws on missing 'value' field") {
    const auto data = nlohmann::json {{"type", "unspecified"}};
    REQUIRE_THROWS (UserSettingParser::parse (data, emptyProperties ()));
}

// ---------- User-property binding path ---------------------------------------

TEST_CASE ("UserSettingParser binds value to user property by name") {
    auto props = singleTextProperty ("mySetting");
    const auto data = nlohmann::json {
        {"value", "ignored"},
        {"user", "mySetting"},
    };
    auto setting = UserSettingParser::parse (data, props);

    REQUIRE (setting != nullptr);
    REQUIRE (setting->property != nullptr);
    REQUIRE (setting->condition.has_value () == false);
}

TEST_CASE ("UserSettingParser records ConditionInfo when user is an object") {
    const auto data = nlohmann::json {
        {"value", "1.0"},
        {"user", {{"name", "mySetting"}, {"condition", "property > 0"}}},
    };
    auto setting = UserSettingParser::parse (data, emptyProperties ());

    REQUIRE (setting != nullptr);
    REQUIRE (setting->condition.has_value ());
    REQUIRE (setting->condition->name == "mySetting");
    REQUIRE (setting->condition->condition == "property > 0");
}

TEST_CASE ("UserSettingParser leaves property null when user name is unknown") {
    const auto data = nlohmann::json {
        {"value", "1.0"},
        {"user", "notInProps"},
    };
    auto setting = UserSettingParser::parse (data, emptyProperties ());

    REQUIRE (setting != nullptr);
    // Unknown property name should not bind, but must not crash.
    REQUIRE (setting->property == nullptr);
}

// ---------- Script wrapper ----------------------------------------------------

TEST_CASE ("UserSettingParser wraps base value in ScriptedDynamicValue when script present") {
    const auto data = nlohmann::json {
        {"value", "1.0 2.0"},
        {"script", "someScript.js"},
    };
    auto setting = UserSettingParser::parse (data, emptyProperties ());

    REQUIRE (setting != nullptr);
    REQUIRE (setting->value != nullptr);
    // The wrapper is a ScriptedDynamicValue; the inner dynamic still holds
    // the parsed Vec2. Verify the inner is reachable via toString().
    REQUIRE_FALSE (setting->value->toString ().empty ());
}