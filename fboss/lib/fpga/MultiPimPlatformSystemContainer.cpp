/*
 *  Copyright (c) 2018-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/lib/fpga/MultiPimPlatformSystemContainer.h"

#include "fboss/agent/FbossError.h"
#include "fboss/lib/fpga/FpgaDevice.h"

namespace facebook::fboss {

MultiPimPlatformSystemContainer::MultiPimPlatformSystemContainer(
    std::unique_ptr<FpgaDevice> fpgaDevice)
    : fpgaDevice_(std::move(fpgaDevice)) {}

MultiPimPlatformSystemContainer::~MultiPimPlatformSystemContainer() {}

MultiPimPlatformPimContainer* MultiPimPlatformSystemContainer::getPimContainer(
    int pim) const {
  if (auto pimItr = pims_.find(pim); pimItr != pims_.end()) {
    return pimItr->second.get();
  }
  throw FbossError("Can't access pim container. Pim:", pim, " doesn't exist");
}

void MultiPimPlatformSystemContainer::setPimContainer(
    int pim,
    std::unique_ptr<MultiPimPlatformPimContainer> pimContainer) {
  // Always replace with new pim container. Although it shouldn't happen in
  // prod.
  pims_[pim] = std::move(pimContainer);
}

MultiPimPlatformPimContainer::PimType
MultiPimPlatformSystemContainer::getPimType(int /* pim */) {
  // TODO(joseph5wu) Make it pure virtual after moving getPimType() from
  // Controller to here for Minipack family
  throw FbossError("Must be implemented in child class");
}
} // namespace facebook::fboss
