/*
 * Copyright (C) 2022 The Android Open Source Project
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

package android.hardware.audio.effect;

import android.media.audio.common.AudioProfile;

/**
 * Equalizer specific definitions.
 */
@VintfStability
union Equalizer {
    /**
     * Defines Equalizer implementation capabilities, it MUST be supported by all equalizer
     * implementations.
     *
     * Equalizer.Capability definition is used by android.hardware.audio.effect.Capability.
     */
    @VintfStability
    parcelable Capability {
        /**
         * Equalizer capability extension, vendor can use this extension in case existing capability
         * definition not enough.
         */
        ParcelableHolder extension;
    }

    // Vendor Equalizer implementation definition for additional parameters.
    @VintfStability
    parcelable VendorExtension {
        ParcelableHolder extension;
    }
    VendorExtension vendor;
}
