// Copyright 2004-present Facebook. All Rights Reserved.

#include "fboss/agent/state/MirrorMap.h"
#include "fboss/agent/state/Mirror.h"
#include "fboss/agent/state/NodeMap-defs.h"

namespace facebook {
namespace fboss {

MirrorMap::MirrorMap() {}

MirrorMap::~MirrorMap() {}

std::shared_ptr<Mirror> MirrorMap::getMirrorIf(const std::string& name) const {
  for (const auto& mirror : *this) {
    if (mirror->getID() == name) {
      return mirror;
    }
  }
  return nullptr;
}

void MirrorMap::addMirror(const std::shared_ptr<Mirror>& mirror) {
  return addNode(mirror);
}

FBOSS_INSTANTIATE_NODE_MAP(MirrorMap, MirrorMapTraits);

} // namespace fboss
} // namespace facebook
