/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <cutils/properties.h>
#include <tinyxml2.h>

#include "effect-impl/EffectTypes.h"

namespace aidl::android::hardware::audio::effect {

/**
 *  Library contains a mapping from library name to path.
 *  Effect contains a mapping from effect name to Libraries and implementation UUID.
 *  Pre/post processor contains a mapping from processing name to effect names.
 */
class EffectConfig {
  public:
    explicit EffectConfig(const std::string& file);

    // <library>
    struct Library {
        std::string name;
        std::string path;
    };
    struct LibraryUuid {
        std::string name;  // library name
        ::aidl::android::media::audio::common::AudioUuid uuid;
    };
    // <effects>
    struct EffectLibraries {
        std::optional<struct LibraryUuid> proxyLibrary;
        std::vector<struct LibraryUuid> libraries;
    };

    int getSkippedElements() const { return mSkippedElements; }
    const std::unordered_map<std::string, std::string> getLibraryMap() const { return mLibraryMap; }
    const std::unordered_map<std::string, struct EffectLibraries> getEffectsMap() const {
        return mEffectsMap;
    }
    const std::unordered_map<std::string, std::vector<std::string>> getProcessingMap() const {
        return mProcessingMap;
    }

    static bool findUuid(const std::string& xmlEffectName,
                         ::aidl::android::media::audio::common::AudioUuid* uuid);

  private:
    static constexpr const char* kEffectLibPath[] =
#ifdef __LP64__
            {"/odm/lib64/soundfx", "/vendor/lib64/soundfx", "/system/lib64/soundfx"};
#else
            {"/odm/lib/soundfx", "/vendor/lib/soundfx", "/system/lib/soundfx"};
#endif

    int mSkippedElements;
    /* Parsed Libraries result */
    std::unordered_map<std::string, std::string> mLibraryMap;
    /* Parsed Effects result */
    std::unordered_map<std::string, struct EffectLibraries> mEffectsMap;
    /* Parsed pre/post processing result */
    std::unordered_map<std::string, std::vector<std::string>> mProcessingMap;

    /** @return all `node`s children that are elements and match the tag if provided. */
    std::vector<std::reference_wrapper<const tinyxml2::XMLElement>> getChildren(
            const tinyxml2::XMLNode& node, const char* childTag = nullptr);

    /** Parse a library xml note and push the result in mLibraryMap or return false on failure. */
    bool parseLibrary(const tinyxml2::XMLElement& xml);

    /** Parse an effect from an xml element describing it.
     * @return true and pushes the effect in mEffectsMap on success, false on failure.
     */
    bool parseEffect(const tinyxml2::XMLElement& xml);

    bool parseStream(const tinyxml2::XMLElement& xml);

    // Function to parse effect.library name and effect.uuid from xml
    bool parseLibraryUuid(const tinyxml2::XMLElement& xml, struct LibraryUuid& libraryUuid,
                          bool isProxy = false);

    const char* dump(const tinyxml2::XMLElement& element,
                     tinyxml2::XMLPrinter&& printer = {}) const;

    bool resolveLibrary(const std::string& path, std::string* resolvedPath);
};

}  // namespace aidl::android::hardware::audio::effect