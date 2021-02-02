// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "SyncState.h"

#include <algorithm>
#include <array>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

#include "Account.h"
#include "Transaction.h"
#include "graphqlservice/GraphQLSchema.h"
#include "graphqlservice/GraphQLService.h"
#include "graphqlservice/JSONResponse.h"
#include "graphqlservice/introspection/Introspection.h"

using namespace std::literals;

namespace graphql {
namespace taraxa {

SyncState::SyncState(std::shared_ptr<dev::rpc::Eth::NodeAPI> node_api) : node_api_(node_api) {}

service::FieldResult<response::Value> SyncState::getStartingBlock(service::FieldParams&&) const {
  auto sync = node_api_->syncStatus();
  if (sync.state == dev::eth::SyncState::Idle || !sync.majorSyncing) return response::Value(false);
  return response::Value((response::IntType)sync.startBlockNumber);
}

service::FieldResult<response::Value> SyncState::getCurrentBlock(service::FieldParams&&) const {
  auto sync = node_api_->syncStatus();
  if (sync.state == dev::eth::SyncState::Idle || !sync.majorSyncing) return response::Value(false);
  return response::Value((response::IntType)sync.currentBlockNumber);
}

service::FieldResult<response::Value> SyncState::getHighestBlock(service::FieldParams&&) const {
  auto sync = node_api_->syncStatus();
  if (sync.state == dev::eth::SyncState::Idle || !sync.majorSyncing) return response::Value(false);
  return response::Value((response::IntType)sync.highestBlockNumber);
}

} /* namespace taraxa */
} /* namespace graphql */
