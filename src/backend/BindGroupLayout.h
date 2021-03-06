// Copyright 2017 The NXT Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef BACKEND_BINDGROUPLAYOUT_H_
#define BACKEND_BINDGROUPLAYOUT_H_

#include "backend/Builder.h"
#include "backend/Forward.h"
#include "backend/RefCounted.h"
#include "common/Constants.h"

#include "nxt/nxtcpp.h"

#include <array>
#include <bitset>

namespace backend {

    class BindGroupLayoutBase : public RefCounted {
      public:
        BindGroupLayoutBase(BindGroupLayoutBuilder* builder, bool blueprint = false);
        ~BindGroupLayoutBase() override;

        struct LayoutBindingInfo {
            std::array<nxt::ShaderStageBit, kMaxBindingsPerGroup> visibilities;
            std::array<nxt::BindingType, kMaxBindingsPerGroup> types;
            std::bitset<kMaxBindingsPerGroup> mask;
        };
        const LayoutBindingInfo& GetBindingInfo() const;

      private:
        DeviceBase* mDevice;
        LayoutBindingInfo mBindingInfo;
        bool mIsBlueprint = false;
    };

    class BindGroupLayoutBuilder : public Builder<BindGroupLayoutBase> {
      public:
        BindGroupLayoutBuilder(DeviceBase* device);

        const BindGroupLayoutBase::LayoutBindingInfo& GetBindingInfo() const;

        // NXT API
        void SetBindingsType(nxt::ShaderStageBit visibility,
                             nxt::BindingType bindingType,
                             uint32_t start,
                             uint32_t count);

      private:
        friend class BindGroupLayoutBase;

        BindGroupLayoutBase* GetResultImpl() override;

        BindGroupLayoutBase::LayoutBindingInfo mBindingInfo;
    };

    // Implements the functors necessary for the unordered_set<BGL*>-based cache.
    struct BindGroupLayoutCacheFuncs {
        // The hash function
        size_t operator()(const BindGroupLayoutBase* bgl) const;

        // The equality predicate
        bool operator()(const BindGroupLayoutBase* a, const BindGroupLayoutBase* b) const;
    };

}  // namespace backend

#endif  // BACKEND_BINDGROUPLAYOUT_H_
