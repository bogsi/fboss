/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/AddressUtil.h"
#include "fboss/agent/ApplyThriftConfig.h"
#include "fboss/agent/FbossError.h"
#include "fboss/agent/test/TestUtils.h"
#include "fboss/agent/hw/mock/MockPlatform.h"
#include "fboss/agent/state/DeltaFunctions.h"
#include "fboss/agent/state/Interface.h"
#include "fboss/agent/state/InterfaceMap.h"
#include "fboss/agent/state/Route.h"
#include "fboss/agent/state/RouteDelta.h"
#include "fboss/agent/state/RouteTableRib.h"
#include "fboss/agent/state/RouteTable.h"
#include "fboss/agent/state/RouteTableMap.h"
#include "fboss/agent/state/RouteUpdater.h"
#include "fboss/agent/state/NodeMapDelta.h"
#include "fboss/agent/state/NodeMapDelta-defs.h"
#include "fboss/agent/state/StateDelta.h"
#include "fboss/agent/state/StateUtils.h"
#include "fboss/agent/state/SwitchState.h"
#include "fboss/agent/state/SwitchState-defs.h"
#include "fboss/agent/gen-cpp2/switch_config_types.h"

#include <folly/logging/xlog.h>
#include <gtest/gtest.h>

using namespace facebook::fboss;
using folly::IPAddress;
using folly::IPAddressV4;
using folly::IPAddressV6;
using std::make_shared;
using std::shared_ptr;
using ::testing::Return;

//
// Helper functions
//
template <typename AddrT>
void EXPECT_FWD_INFO(
    std::shared_ptr<Route<AddrT>> rt,
    InterfaceID intf,
    std::string ipStr) {
  const auto& fwds = rt->getForwardInfo().getNextHopSet();
  EXPECT_EQ(1, fwds.size());
  const auto& fwd = *fwds.begin();
  EXPECT_EQ(intf, fwd.intf());
  EXPECT_EQ(IPAddress(ipStr), fwd.addr());
}

template <typename AddrT>
void EXPECT_RESOLVED(std::shared_ptr<Route<AddrT>> rt) {
  ASSERT_NE(nullptr, rt);
  EXPECT_TRUE(rt->isResolved());
  EXPECT_FALSE(rt->isUnresolvable());
  EXPECT_FALSE(rt->needResolve());
}

template <typename AddrT>
void EXPECT_NODEMAP_MATCH(const std::shared_ptr<RouteTableRib<AddrT>>& rib) {
  const auto& radixTree = rib->routesRadixTree();
  EXPECT_EQ(rib->size(), radixTree.size());
  for (const auto& route : *(rib->routes())) {
    auto match =
        radixTree.exactMatch(route->prefix().network, route->prefix().mask);
    ASSERT_NE(match, radixTree.end());
    // should be the same shared_ptr
    EXPECT_EQ(route, match->value());
  }
}

void EXPECT_NODEMAP_MATCH(const std::shared_ptr<RouteTableMap>& routeTables) {
  for (const auto& rt : *routeTables) {
    if (rt->getRibV4()) {
      EXPECT_NODEMAP_MATCH<IPAddressV4>(rt->getRibV4());
    }
    if (rt->getRibV6()) {
      EXPECT_NODEMAP_MATCH<IPAddressV6>(rt->getRibV6());
    }
  }
}

template <typename AddrT>
void EXPECT_ROUTETABLERIB_MATCH(
    const std::shared_ptr<RouteTableRib<AddrT>>& rib1,
    const std::shared_ptr<RouteTableRib<AddrT>>& rib2) {
  EXPECT_EQ(rib1->size(), rib2->size());
  EXPECT_EQ(rib1->routesRadixTree().size(), rib2->routesRadixTree().size());
  for (const auto& route : *(rib1->routes())) {
    auto match = rib2->exactMatch(route->prefix());
    ASSERT_NE(nullptr, match);
    EXPECT_TRUE(match->isSame(route.get()));
  }
}

//
// Tests
//
#define CLIENT_A ClientID(1001)
#define CLIENT_B ClientID(1002)
#define CLIENT_C ClientID(1003)
#define CLIENT_D ClientID(1004)
#define CLIENT_E ClientID(1005)

constexpr AdminDistance DISTANCE = AdminDistance::MAX_ADMIN_DISTANCE;

std::shared_ptr<SwitchState> applyInitConfig() {
  auto platform = createMockPlatform();
  auto stateV0 = make_shared<SwitchState>();
  auto tablesV0 = stateV0->getRouteTables();

  cfg::SwitchConfig config;
  config.vlans.resize(4);
  config.vlans[0].id = 1;
  config.vlans[1].id = 2;
  config.vlans[2].id = 3;
  config.vlans[3].id = 4;

  config.interfaces.resize(4);
  config.interfaces[0].intfID = 1;
  config.interfaces[0].vlanID = 1;
  config.interfaces[0].routerID = 0;
  config.interfaces[0].__isset.mac = true;
  config.interfaces[0].mac = "00:00:00:00:00:11";
  config.interfaces[0].ipAddresses.resize(2);
  config.interfaces[0].ipAddresses[0] = "1.1.1.1/24";
  config.interfaces[0].ipAddresses[1] = "1::1/48";

  config.interfaces[1].intfID = 2;
  config.interfaces[1].vlanID = 2;
  config.interfaces[1].routerID = 0;
  config.interfaces[1].__isset.mac = true;
  config.interfaces[1].mac = "00:00:00:00:00:22";
  config.interfaces[1].ipAddresses.resize(2);
  config.interfaces[1].ipAddresses[0] = "2.2.2.2/24";
  config.interfaces[1].ipAddresses[1] = "2::1/48";

  config.interfaces[2].intfID = 3;
  config.interfaces[2].vlanID = 3;
  config.interfaces[2].routerID = 0;
  config.interfaces[2].__isset.mac = true;
  config.interfaces[2].mac = "00:00:00:00:00:33";
  config.interfaces[2].ipAddresses.resize(2);
  config.interfaces[2].ipAddresses[0] = "3.3.3.3/24";
  config.interfaces[2].ipAddresses[1] = "3::1/48";

  config.interfaces[3].intfID = 4;
  config.interfaces[3].vlanID = 4;
  config.interfaces[3].routerID = 0;
  config.interfaces[3].__isset.mac = true;
  config.interfaces[3].mac = "00:00:00:00:00:44";
  config.interfaces[3].ipAddresses.resize(2);
  config.interfaces[3].ipAddresses[0] = "4.4.4.4/24";
  config.interfaces[3].ipAddresses[1] = "4::1/48";

  auto stateV1 = publishAndApplyConfig(stateV0, &config, platform.get());
  stateV1->publish();
  return stateV1;
}

TEST(Route, resolve) {
  auto stateV1 = applyInitConfig();
  ASSERT_NE(nullptr, stateV1);

  auto rid = RouterID(0);
  // recursive lookup
  {
    RouteUpdater u1(stateV1->getRouteTables());
    RouteNextHopSet nexthops1 =
        makeNextHops({"1.1.1.10"}); // resolved by intf 1
    u1.addRoute(
        rid,
        IPAddress("1.1.3.0"),
        24,
        CLIENT_A,
        RouteNextHopEntry(nexthops1, DISTANCE));
    RouteNextHopSet nexthops2 = makeNextHops({"1.1.3.10"}); // rslvd. by
                                                            // '1.1.3/24'
    u1.addRoute(
        rid,
        IPAddress("8.8.8.0"),
        24,
        CLIENT_A,
        RouteNextHopEntry(nexthops2, DISTANCE));
    auto tables2 = u1.updateDone();
    ASSERT_NE(nullptr, tables2);
    EXPECT_NODEMAP_MATCH(tables2);
    tables2->publish();

    auto r21 = GET_ROUTE_V4(tables2, rid, "1.1.3.0/24");
    EXPECT_RESOLVED(r21);
    EXPECT_FALSE(r21->isConnected());

    auto r22 = GET_ROUTE_V4(tables2, rid, "8.8.8.0/24");
    EXPECT_RESOLVED(r22);
    EXPECT_FALSE(r22->isConnected());
    // r21 and r22 are different routes
    EXPECT_NE(r21, r22);
    EXPECT_NE(r21->prefix(), r22->prefix());
    // check the forwarding info
    RouteNextHopSet expFwd2;
    expFwd2.emplace(
        ResolvedNextHop(IPAddress("1.1.1.10"), InterfaceID(1), ECMP_WEIGHT));
    EXPECT_EQ(expFwd2, r21->getForwardInfo().getNextHopSet());
    EXPECT_EQ(expFwd2, r22->getForwardInfo().getNextHopSet());
  }
  // recursive lookup loop
  {
    // create a route table w/ the following 3 routes
    // 1. 30/8 -> 20.1.1.1
    // 2. 20/8 -> 10.1.1.1
    // 3. 10/8 -> 30.1.1.1
    // The above 3 routes causes lookup loop, which should result in
    // all unresolvable.
    RouteUpdater u1(stateV1->getRouteTables());
    u1.addRoute(
        rid,
        IPAddress("30.0.0.0"),
        8,
        CLIENT_A,
        RouteNextHopEntry(makeNextHops({"20.1.1.1"}), DISTANCE));
    u1.addRoute(
        rid,
        IPAddress("20.0.0.0"),
        8,
        CLIENT_A,
        RouteNextHopEntry(makeNextHops({"10.1.1.1"}), DISTANCE));
    u1.addRoute(
        rid,
        IPAddress("10.0.0.0"),
        8,
        CLIENT_A,
        RouteNextHopEntry(makeNextHops({"30.1.1.1"}), DISTANCE));
    auto tables2 = u1.updateDone();
    ASSERT_NE(nullptr, tables2);
    EXPECT_NODEMAP_MATCH(tables2);
    tables2->publish();

    auto verifyPrefix = [&](std::string prefixStr) {
      auto route = GET_ROUTE_V4(tables2, rid, prefixStr);
      EXPECT_FALSE(route->isResolved());
      EXPECT_TRUE(route->isUnresolvable());
      EXPECT_FALSE(route->isConnected());
      EXPECT_FALSE(route->needResolve());
      EXPECT_FALSE(route->isProcessing());
    };
    verifyPrefix("10.0.0.0/8");
    verifyPrefix("20.0.0.0/8");
    verifyPrefix("30.0.0.0/8");
  }
  // recursive lookup across 2 updates
  {
    RouteUpdater u1(stateV1->getRouteTables());
    RouteNextHopSet nexthops1 = makeNextHops({"50.0.0.1"});
    u1.addRoute(
        rid,
        IPAddress("40.0.0.0"),
        8,
        CLIENT_A,
        RouteNextHopEntry(nexthops1, DISTANCE));

    auto tables2 = u1.updateDone();
    ASSERT_NE(nullptr, tables2);
    tables2->publish();

    // 40.0.0.0/8 should be unresolved
    auto r21 = GET_ROUTE_V4(tables2, rid, "40.0.0.0/8");
    EXPECT_FALSE(r21->isResolved());
    EXPECT_TRUE(r21->isUnresolvable());
    EXPECT_FALSE(r21->isConnected());
    EXPECT_FALSE(r21->needResolve());

    // Resolve 50.0.0.1 this should also resolve 40.0.0.0/8
    RouteUpdater u2(tables2);
    u2.addRoute(
        rid,
        IPAddress("50.0.0.0"),
        8,
        CLIENT_A,
        RouteNextHopEntry(makeNextHops({"1.1.1.1"}), DISTANCE));
    auto tables3 = u2.updateDone();
    ASSERT_NE(nullptr, tables3);
    EXPECT_NODEMAP_MATCH(tables3);
    tables3->publish();

    // 40.0.0.0/8 should be resolved
    auto rib3 = tables3->getRouteTableIf(rid)->getRibV4();
    auto r31 = GET_ROUTE_V4(tables3, rid, "40.0.0.0/8");
    EXPECT_RESOLVED(r31);
    EXPECT_FALSE(r31->isConnected());

    // 50.0.0.1/32 should be resolved
    auto r31NextHops = r31->getBestEntry().second->getNextHopSet();
    EXPECT_EQ(1, r31NextHops.size());
    auto r32 = rib3->longestMatch(r31NextHops.begin()->addr().asV4());
    EXPECT_RESOLVED(r32);
    EXPECT_FALSE(r32->isConnected());

    // 50.0.0.0/8 should be resolved
    auto r33 = GET_ROUTE_V4(tables3, rid, "50.0.0.0/8");
    EXPECT_RESOLVED(r33);
    EXPECT_FALSE(r33->isConnected());
  }
}

TEST(Route, resolveDropToCPUMix) {
  auto stateV1 = applyInitConfig();
  ASSERT_NE(nullptr, stateV1);

  auto rid = RouterID(0);

  // add a DROP route and a ToCPU route
  RouteUpdater u1(stateV1->getRouteTables());
  u1.addRoute(
      rid,
      IPAddress("11.1.1.0"),
      24,
      CLIENT_A,
      RouteNextHopEntry(RouteForwardAction::DROP, DISTANCE));
  u1.addRoute(
      rid,
      IPAddress("22.1.1.0"),
      24,
      CLIENT_A,
      RouteNextHopEntry(RouteForwardAction::TO_CPU, DISTANCE));
  // then, add a route for 4 nexthops. One to each interface, one
  // to the DROP and one to the ToCPU
  RouteNextHopSet nhops = makeNextHops({"1.1.1.10", // intf 1
                                        "2.2.2.10", // intf 2
                                        "11.1.1.10", // DROP
                                        "22.1.1.10"}); // ToCPU
  u1.addRoute(
      rid,
      IPAddress("8.8.8.0"),
      24,
      CLIENT_A,
      RouteNextHopEntry(nhops, DISTANCE));
  auto table2 = u1.updateDone();
  ASSERT_NE(nullptr, table2);
  EXPECT_NODEMAP_MATCH(table2);
  table2->publish();
  {
    auto r2 = GET_ROUTE_V4(table2, rid, "8.8.8.0/24");
    EXPECT_RESOLVED(r2);
    EXPECT_FALSE(r2->isDrop());
    EXPECT_FALSE(r2->isToCPU());
    EXPECT_FALSE(r2->isConnected());
    const auto& fwd = r2->getForwardInfo();
    EXPECT_EQ(RouteForwardAction::NEXTHOPS, fwd.getAction());
    EXPECT_EQ(2, fwd.getNextHopSet().size());
  }

  // now update the route with just DROP and ToCPU, expect ToCPU to win
  RouteUpdater u2(table2);
  RouteNextHopSet nhops2 = makeNextHops({"11.1.1.10", // DROP
                                         "22.1.1.10"}); // ToCPU
  u2.addRoute(
      rid,
      IPAddress("8.8.8.0"),
      24,
      CLIENT_A,
      RouteNextHopEntry(nhops2, DISTANCE));
  auto table3 = u2.updateDone();
  ASSERT_NE(nullptr, table3);
  EXPECT_NODEMAP_MATCH(table3);
  table3->publish();
  {
    auto r2 = GET_ROUTE_V4(table3, rid, "8.8.8.0/24");
    EXPECT_RESOLVED(r2);
    EXPECT_FALSE(r2->isDrop());
    EXPECT_TRUE(r2->isToCPU());
    EXPECT_FALSE(r2->isConnected());
    const auto& fwd = r2->getForwardInfo();
    EXPECT_EQ(RouteForwardAction::TO_CPU, fwd.getAction());
    EXPECT_EQ(0, fwd.getNextHopSet().size());
  }

  // now update the route with just DROP
  RouteUpdater u3(table3);
  RouteNextHopSet nhops3 = makeNextHops({"11.1.1.10"}); // DROP
  u3.addRoute(
      rid,
      IPAddress("8.8.8.0"),
      24,
      CLIENT_A,
      RouteNextHopEntry(nhops3, DISTANCE));
  auto table4 = u3.updateDone();
  ASSERT_NE(nullptr, table4);
  EXPECT_NODEMAP_MATCH(table4);
  table4->publish();
  {
    auto r2 = GET_ROUTE_V4(table4, rid, "8.8.8.0/24");
    EXPECT_RESOLVED(r2);
    EXPECT_TRUE(r2->isDrop());
    EXPECT_FALSE(r2->isToCPU());
    EXPECT_FALSE(r2->isConnected());
    const auto& fwd = r2->getForwardInfo();
    EXPECT_EQ(RouteForwardAction::DROP, fwd.getAction());
    EXPECT_EQ(0, fwd.getNextHopSet().size());
  }
}

// Testing add and delete ECMP routes
TEST(Route, addDel) {
  auto stateV1 = applyInitConfig();
  ASSERT_NE(nullptr, stateV1);

  auto rid = RouterID(0);

  RouteNextHopSet nexthops = makeNextHops({"1.1.1.10", // intf 1
                                           "2::2", // intf 2
                                           "1.1.2.10"}); // un-resolvable
  RouteNextHopSet nexthops2 = makeNextHops({"1.1.3.10", // un-resolvable
                                            "11:11::1"}); // un-resolvable

  RouteUpdater u1(stateV1->getRouteTables());
  u1.addRoute(
      rid,
      IPAddress("10.1.1.1"),
      24,
      CLIENT_A,
      RouteNextHopEntry(nexthops, DISTANCE));
  u1.addRoute(
      rid,
      IPAddress("2001::1"),
      48,
      CLIENT_A,
      RouteNextHopEntry(nexthops, DISTANCE));
  auto tables2 = u1.updateDone();
  ASSERT_NE(nullptr, tables2);
  EXPECT_NODEMAP_MATCH(tables2);
  tables2->publish();

  // v4 route
  auto r2 = GET_ROUTE_V4(tables2, rid, "10.1.1.0/24");
  EXPECT_RESOLVED(r2);
  EXPECT_FALSE(r2->isDrop());
  EXPECT_FALSE(r2->isToCPU());
  EXPECT_FALSE(r2->isConnected());
  // v6 route
  auto r2v6 = GET_ROUTE_V6(tables2, rid, "2001::0/48");
  EXPECT_RESOLVED(r2v6);
  EXPECT_FALSE(r2v6->isDrop());
  EXPECT_FALSE(r2v6->isToCPU());
  EXPECT_FALSE(r2v6->isConnected());
  // forwarding info
  EXPECT_EQ(RouteForwardAction::NEXTHOPS, r2->getForwardInfo().getAction());
  EXPECT_EQ(RouteForwardAction::NEXTHOPS, r2v6->getForwardInfo().getAction());
  const auto& fwd2 = r2->getForwardInfo().getNextHopSet();
  const auto& fwd2v6 = r2v6->getForwardInfo().getNextHopSet();
  EXPECT_EQ(2, fwd2.size());
  EXPECT_EQ(2, fwd2v6.size());
  RouteNextHopSet expFwd2;
  expFwd2.emplace(
      ResolvedNextHop(IPAddress("1.1.1.10"), InterfaceID(1), ECMP_WEIGHT));
  expFwd2.emplace(
      ResolvedNextHop(IPAddress("2::2"), InterfaceID(2), ECMP_WEIGHT));
  EXPECT_EQ(expFwd2, fwd2);
  EXPECT_EQ(expFwd2, fwd2v6);

  // change the nexthops of the V4 route
  RouteUpdater u2(tables2);
  u2.addRoute(
      rid,
      IPAddress("10.1.1.1"),
      24,
      CLIENT_A,
      RouteNextHopEntry(nexthops2, DISTANCE));
  auto tables3 = u2.updateDone();
  ASSERT_NE(nullptr, tables3);
  EXPECT_NODEMAP_MATCH(tables3);
  tables3->publish();

  auto r3 = GET_ROUTE_V4(tables3, rid, "10.1.1.0/24");
  ASSERT_NE(nullptr, r3);
  EXPECT_FALSE(r3->isResolved());
  EXPECT_TRUE(r3->isUnresolvable());
  EXPECT_FALSE(r3->isConnected());
  EXPECT_FALSE(r3->needResolve());

  // re-add the same route does not cause change
  RouteUpdater u3(tables3);
  u3.addRoute(
      rid,
      IPAddress("10.1.1.1"),
      24,
      CLIENT_A,
      RouteNextHopEntry(nexthops2, DISTANCE));
  auto tables4 = u3.updateDone();
  EXPECT_EQ(nullptr, tables4);

  // now delete the V4 route
  RouteUpdater u4(tables3);
  u4.delRoute(rid, IPAddress("10.1.1.1"), 24, CLIENT_A);
  auto tables5 = u4.updateDone();
  ASSERT_NE(nullptr, tables5);
  EXPECT_NODEMAP_MATCH(tables5);
  tables5->publish();

  auto rib5 = tables5->getRouteTableIf(rid)->getRibV4();
  auto r5 = rib5->exactMatch({IPAddressV4("10.1.1.0"), 24});
  EXPECT_EQ(nullptr, r5);

  // change an old route to punt to CPU, add a new route to DROP
  RouteUpdater u5(tables3);
  u5.addRoute(
      rid,
      IPAddress("10.1.1.0"),
      24,
      CLIENT_A,
      RouteNextHopEntry(RouteForwardAction::TO_CPU, DISTANCE));
  u5.addRoute(
      rid,
      IPAddress("10.1.2.0"),
      24,
      CLIENT_A,
      RouteNextHopEntry(RouteForwardAction::DROP, DISTANCE));
  auto tables6 = u5.updateDone();
  EXPECT_NODEMAP_MATCH(tables6);
  EXPECT_FALSE(GET_ROUTE_V4(tables6, rid, "10.1.1.0/24")->isPublished());
  EXPECT_FALSE(GET_ROUTE_V4(tables6, rid, "10.1.2.0/24")->isPublished());

  auto r6_1 = GET_ROUTE_V4(tables6, rid, "10.1.1.0/24");
  EXPECT_RESOLVED(r6_1);
  EXPECT_FALSE(r6_1->isConnected());
  EXPECT_TRUE(r6_1->isToCPU());
  EXPECT_FALSE(r6_1->isDrop());
  EXPECT_EQ(RouteForwardAction::TO_CPU, r6_1->getForwardInfo().getAction());

  auto r6_2 = GET_ROUTE_V4(tables6, rid, "10.1.2.0/24");
  EXPECT_RESOLVED(r6_2);
  EXPECT_FALSE(r6_2->isConnected());
  EXPECT_FALSE(r6_2->isToCPU());
  EXPECT_TRUE(r6_2->isDrop());
  EXPECT_EQ(RouteForwardAction::DROP, r6_2->getForwardInfo().getAction());
}

// Test interface routes
TEST(Route, Interface) {
  auto platform = createMockPlatform();
  RouterID rid = RouterID(0);
  auto stateV0 = make_shared<SwitchState>();
  auto tablesV0 = stateV0->getRouteTables();

  cfg::SwitchConfig config;
  config.vlans.resize(2);
  config.vlans[0].id = 1;
  config.vlans[1].id = 2;

  config.interfaces.resize(2);
  config.interfaces[0].intfID = 1;
  config.interfaces[0].vlanID = 1;
  config.interfaces[0].routerID = 0;
  config.interfaces[0].__isset.mac = true;
  config.interfaces[0].mac = "00:00:00:00:00:11";
  config.interfaces[0].ipAddresses.resize(2);
  config.interfaces[0].ipAddresses[0] = "1.1.1.1/24";
  config.interfaces[0].ipAddresses[1] = "1::1/48";
  config.interfaces[1].intfID = 2;
  config.interfaces[1].vlanID = 2;
  config.interfaces[1].routerID = 0;
  config.interfaces[1].__isset.mac = true;
  config.interfaces[1].mac = "00:00:00:00:00:22";
  config.interfaces[1].ipAddresses.resize(2);
  config.interfaces[1].ipAddresses[0] = "2.2.2.2/24";
  config.interfaces[1].ipAddresses[1] = "2::1/48";

  auto stateV1 = publishAndApplyConfig(stateV0, &config, platform.get());
  ASSERT_NE(nullptr, stateV1);
  stateV1->publish();
  auto tablesV1 = stateV1->getRouteTables();
  EXPECT_NODEMAP_MATCH(tablesV1);
  EXPECT_NE(tablesV0, tablesV1);
  EXPECT_EQ(1, tablesV1->getGeneration());
  EXPECT_EQ(1, tablesV1->size());
  EXPECT_EQ(2, tablesV1->getRouteTableIf(rid)->getRibV4()->size());
  EXPECT_EQ(3, tablesV1->getRouteTableIf(rid)->getRibV6()->size());
  // verify the ipv4 interface route
  {
    auto rt = GET_ROUTE_V4(tablesV1, rid, "1.1.1.0/24");
    EXPECT_EQ(0, rt->getGeneration());
    EXPECT_RESOLVED(rt);
    EXPECT_TRUE(rt->isConnected());
    EXPECT_FALSE(rt->isToCPU());
    EXPECT_FALSE(rt->isDrop());
    EXPECT_EQ(RouteForwardAction::NEXTHOPS, rt->getForwardInfo().getAction());
    EXPECT_FWD_INFO(rt, InterfaceID(1), "1.1.1.1");
  }
  // verify the ipv6 interface route
  {
    auto rt = GET_ROUTE_V6(tablesV1, rid, "2::0/48");
    EXPECT_EQ(0, rt->getGeneration());
    EXPECT_RESOLVED(rt);
    EXPECT_TRUE(rt->isConnected());
    EXPECT_FALSE(rt->isToCPU());
    EXPECT_FALSE(rt->isDrop());
    EXPECT_EQ(RouteForwardAction::NEXTHOPS, rt->getForwardInfo().getAction());
    EXPECT_FWD_INFO(rt, InterfaceID(2), "2::1");
  }

  {
    // verify v6 link local route
    auto rt = GET_ROUTE_V6(tablesV1, rid, "fe80::/64");
    EXPECT_EQ(0, rt->getGeneration());
    EXPECT_RESOLVED(rt);
    EXPECT_FALSE(rt->isConnected());
    EXPECT_TRUE(rt->isToCPU());
    EXPECT_EQ(RouteForwardAction::TO_CPU, rt->getForwardInfo().getAction());
    const auto& fwds = rt->getForwardInfo().getNextHopSet();
    EXPECT_EQ(0, fwds.size());
  }

  // swap the interface addresses which causes route change
  config.interfaces[1].ipAddresses[0] = "1.1.1.1/24";
  config.interfaces[1].ipAddresses[1] = "1::1/48";
  config.interfaces[0].ipAddresses[0] = "2.2.2.2/24";
  config.interfaces[0].ipAddresses[1] = "2::1/48";

  auto stateV2 = publishAndApplyConfig(stateV1, &config, platform.get());
  ASSERT_NE(nullptr, stateV2);
  stateV2->publish();
  auto tablesV2 = stateV2->getRouteTables();
  EXPECT_NODEMAP_MATCH(tablesV2);
  EXPECT_NE(tablesV1, tablesV2);
  EXPECT_EQ(2, tablesV2->getGeneration());
  EXPECT_EQ(1, tablesV2->size());
  EXPECT_EQ(2, tablesV2->getRouteTableIf(rid)->getRibV4()->size());
  EXPECT_EQ(3, tablesV2->getRouteTableIf(rid)->getRibV6()->size());

  {
    auto rib4 = tablesV1->getRouteTableIf(rid)->getRibV4();
    auto rib6 = tablesV1->getRouteTableIf(rid)->getRibV6();
    auto rib4V2 = tablesV2->getRouteTableIf(rid)->getRibV4();
    auto rib6V2 = tablesV2->getRouteTableIf(rid)->getRibV6();
    EXPECT_NE(rib4, rib4V2);
    EXPECT_NE(rib6, rib6V2);
  }
  // verify the ipv4 route
  {
    auto rt = GET_ROUTE_V4(tablesV2, rid, "1.1.1.0/24");
    EXPECT_EQ(1, rt->getGeneration());
    EXPECT_FWD_INFO(rt, InterfaceID(2), "1.1.1.1");
  }
  // verify the ipv6 route
  {
    auto rt = GET_ROUTE_V6(tablesV2, rid, "2::0/48");
    EXPECT_EQ(1, rt->getGeneration());
    EXPECT_FWD_INFO(rt, InterfaceID(1), "2::1");
  }
}

// Test interface routes when we have more than one address per
// address family in an interface
TEST(Route, MultipleAddressInterface) {
  auto platform = createMockPlatform();
  auto rid = RouterID(0);
  auto stateV0 = make_shared<SwitchState>();
  auto tablesV0 = stateV0->getRouteTables();

  cfg::SwitchConfig config;
  config.vlans.resize(1);
  config.vlans[0].id = 1;

  config.interfaces.resize(1);
  config.interfaces[0].intfID = 1;
  config.interfaces[0].vlanID = 1;
  config.interfaces[0].routerID = 0;
  config.interfaces[0].__isset.mac = true;
  config.interfaces[0].mac = "00:00:00:00:00:11";
  config.interfaces[0].ipAddresses.resize(4);
  config.interfaces[0].ipAddresses[0] = "1.1.1.1/24";
  config.interfaces[0].ipAddresses[1] = "1.1.1.2/24";
  config.interfaces[0].ipAddresses[2] = "1::1/48";
  config.interfaces[0].ipAddresses[3] = "1::2/48";

  auto stateV1 = publishAndApplyConfig(stateV0, &config, platform.get());
  ASSERT_NE(nullptr, stateV1);
  stateV1->publish();
  auto tablesV1 = stateV1->getRouteTables();
  EXPECT_NODEMAP_MATCH(tablesV1);
  EXPECT_NE(tablesV0, tablesV1);
  EXPECT_EQ(1, tablesV1->getGeneration());
  EXPECT_EQ(1, tablesV1->size());
  EXPECT_EQ(1, tablesV1->getRouteTableIf(rid)->getRibV4()->size());
  EXPECT_EQ(2, tablesV1->getRouteTableIf(rid)->getRibV6()->size());
  // verify the ipv4 route
  {
    auto rt = GET_ROUTE_V4(tablesV1, rid, "1.1.1.0/24");
    EXPECT_EQ(0, rt->getGeneration());
    EXPECT_RESOLVED(rt);
    EXPECT_TRUE(rt->isConnected());
    EXPECT_FALSE(rt->isToCPU());
    EXPECT_FALSE(rt->isDrop());
    EXPECT_EQ(RouteForwardAction::NEXTHOPS, rt->getForwardInfo().getAction());
    EXPECT_FWD_INFO(rt, InterfaceID(1), "1.1.1.2");
  }
  // verify the ipv6 route
  {
    auto rt = GET_ROUTE_V6(tablesV1, rid, "1::0/48");
    EXPECT_EQ(0, rt->getGeneration());
    EXPECT_RESOLVED(rt);
    EXPECT_TRUE(rt->isConnected());
    EXPECT_FALSE(rt->isToCPU());
    EXPECT_FALSE(rt->isDrop());
    EXPECT_EQ(RouteForwardAction::NEXTHOPS, rt->getForwardInfo().getAction());
    EXPECT_FWD_INFO(rt, InterfaceID(1), "1::2");
  }
}

// Test interface + static routes
TEST(Route, InterfaceAndStatic) {
  auto platform = createMockPlatform();
  RouterID rid = RouterID(0);
  auto stateV0 = make_shared<SwitchState>();
  auto tablesV0 = stateV0->getRouteTables();

  cfg::SwitchConfig config;
  config.vlans.resize(2);
  config.vlans[0].id = 1;
  config.vlans[1].id = 2;

  config.interfaces.resize(2);
  config.interfaces[0].intfID = 1;
  config.interfaces[0].vlanID = 1;
  config.interfaces[0].routerID = 0;
  config.interfaces[0].__isset.mac = true;
  config.interfaces[0].mac = "00:00:00:00:00:11";
  config.interfaces[0].ipAddresses.resize(2);
  config.interfaces[0].ipAddresses[0] = "1.1.1.1/24";
  config.interfaces[0].ipAddresses[1] = "1::1/48";
  config.interfaces[1].intfID = 2;
  config.interfaces[1].vlanID = 2;
  config.interfaces[1].routerID = 0;
  config.interfaces[1].__isset.mac = true;
  config.interfaces[1].mac = "00:00:00:00:00:22";
  config.interfaces[1].ipAddresses.resize(2);
  config.interfaces[1].ipAddresses[0] = "2.2.2.2/24";
  config.interfaces[1].ipAddresses[1] = "2::1/48";
  // Add v4/v6 static routes with nhops
  config.__isset.staticRoutesWithNhops = true;
  config.staticRoutesWithNhops.resize(2);
  config.staticRoutesWithNhops[0].nexthops.resize(1);
  config.staticRoutesWithNhops[0].prefix = "2001::/64";
  config.staticRoutesWithNhops[0].nexthops[0] = "2::2";
  config.staticRoutesWithNhops[1].nexthops.resize(1);
  config.staticRoutesWithNhops[1].prefix = "20.20.20.0/24";
  config.staticRoutesWithNhops[1].nexthops[0] = "2.2.2.3";

  auto insertStaticNoNhopRoutes = [=](auto& staticRouteNoNhops,
                                      int prefixStartIdx) {
    staticRouteNoNhops.resize(2);
    staticRouteNoNhops[0].prefix = folly::sformat("240{}::/64", prefixStartIdx);
    staticRouteNoNhops[1].prefix =
        folly::sformat("30.30.{}.0/24", prefixStartIdx);
  };
  // Add v4/v6 static routes to CPU/NULL
  config.__isset.staticRoutesToCPU = true;
  insertStaticNoNhopRoutes(config.staticRoutesToCPU, 1);
  config.__isset.staticRoutesToNull = true;
  insertStaticNoNhopRoutes(config.staticRoutesToNull, 2);

  auto stateV1 = publishAndApplyConfig(stateV0, &config, platform.get());
  ASSERT_NE(nullptr, stateV1);
  stateV1->publish();
  auto tablesV1 = stateV1->getRouteTables();
  EXPECT_NODEMAP_MATCH(tablesV1);
  EXPECT_NE(tablesV0, tablesV1);
  // 5 = 2 (interface routes) + 1 (static routes with nhops) +
  // 1 static routes to CPU) + 1 (static routes to NULL)
  EXPECT_EQ(5, tablesV1->getRouteTableIf(rid)->getRibV4()->size());
  // 6 = 2 (interface routes) + 1 (static routes with nhops) +
  // 1 (static routes to CPU) + 1 (static routes to NULL) + 1 (link local route)
  EXPECT_EQ(6, tablesV1->getRouteTableIf(rid)->getRibV6()->size());
}

// Test adding and deleting per-client nexthops lists
TEST(Route, modRoutes) {
  auto stateV1 = make_shared<SwitchState>();
  stateV1->publish();
  auto tables1 = stateV1->getRouteTables();
  auto rid = RouterID(0);
  RouteUpdater u1(tables1);

  RouteV4::Prefix prefix10{IPAddressV4("10.10.10.10"), 32};
  RouteV4::Prefix prefix99{IPAddressV4("99.99.99.99"), 32};

  RouteNextHopSet nexthops1 = newNextHops(3, "1.1.1.");
  RouteNextHopSet nexthops2 = newNextHops(3, "2.2.2.");
  RouteNextHopSet nexthops3 = newNextHops(3, "3.3.3.");

  u1.addRoute(
      rid,
      IPAddress("10.10.10.10"),
      32,
      CLIENT_A,
      RouteNextHopEntry(nexthops1, DISTANCE));
  u1.addRoute(
      rid,
      IPAddress("10.10.10.10"),
      32,
      CLIENT_B,
      RouteNextHopEntry(nexthops2, DISTANCE));
  u1.addRoute(
      rid,
      IPAddress("99.99.99.99"),
      32,
      CLIENT_A,
      RouteNextHopEntry(nexthops3, DISTANCE));
  tables1 = u1.updateDone();
  EXPECT_NODEMAP_MATCH(tables1);
  tables1->publish();

  RouteUpdater u2(tables1);
  auto t1rt10 = tables1->getRouteTable(rid)->getRibV4()->exactMatch(prefix10);
  auto t1rt99 = tables1->getRouteTable(rid)->getRibV4()->exactMatch(prefix99);
  // Table1 has route 10 with two nexthop sets, and route 99 with one set
  EXPECT_TRUE(t1rt10->has(CLIENT_A, RouteNextHopEntry(nexthops1, DISTANCE)));
  EXPECT_TRUE(t1rt10->has(CLIENT_B, RouteNextHopEntry(nexthops2, DISTANCE)));
  EXPECT_TRUE(t1rt99->has(CLIENT_A, RouteNextHopEntry(nexthops3, DISTANCE)));

  u2.delRoute(rid, IPAddress("10.10.10.10"), 32, CLIENT_A);
  auto tables2 = u2.updateDone();
  EXPECT_NODEMAP_MATCH(tables2);
  auto t2rt10 = tables2->getRouteTable(rid)->getRibV4()->exactMatch(prefix10);
  auto t2rt99 = tables2->getRouteTable(rid)->getRibV4()->exactMatch(prefix99);
  // Table2 should only be missing the 10.10.10.10 route for client CLIENT_A
  EXPECT_FALSE(t2rt10->has(CLIENT_A, RouteNextHopEntry(nexthops1, DISTANCE)));
  EXPECT_TRUE(t2rt10->has(CLIENT_B, RouteNextHopEntry(nexthops2, DISTANCE)));
  EXPECT_TRUE(t2rt99->has(CLIENT_A, RouteNextHopEntry(nexthops3, DISTANCE)));
  EXPECT_EQ(t2rt10->getEntryForClient(CLIENT_A), nullptr);
  EXPECT_NE(t2rt10->getEntryForClient(CLIENT_B), nullptr);

  // Delete the second client/nexthop pair from table2.
  // The route & prefix should disappear altogether.
  RouteUpdater u3(tables2);
  u3.delRoute(rid, IPAddress("10.10.10.10"), 32, CLIENT_B);
  auto tables3 = u3.updateDone();
  EXPECT_NODEMAP_MATCH(tables3);
  auto t3rt10 = tables3->getRouteTable(rid)->getRibV4()->exactMatch(prefix10);
  EXPECT_EQ(t3rt10, nullptr);
}

// Test adding empty nextHops lists
TEST(Route, disallowEmptyNexthops) {
  auto stateV1 = make_shared<SwitchState>();
  stateV1->publish();
  auto tables1 = stateV1->getRouteTables();
  auto rid = RouterID(0);
  RouteUpdater u1(tables1);

  // It's illegal to add an empty nextHops list to a route

  // Test the case where the empty list is the first to be added to the Route
  ASSERT_THROW(
      u1.addRoute(
          rid,
          IPAddress("5.5.5.5"),
          32,
          CLIENT_A,
          RouteNextHopEntry(newNextHops(0, "20.20.20."), DISTANCE)),
      FbossError);

  // Test the case where the empty list is the second to be added to the Route
  u1.addRoute(
      rid,
      IPAddress("10.10.10.10"),
      32,
      CLIENT_A,
      RouteNextHopEntry(newNextHops(3, "10.10.10."), DISTANCE));
  ASSERT_THROW(
      u1.addRoute(
          rid,
          IPAddress("10.10.10.10"),
          32,
          CLIENT_B,
          RouteNextHopEntry(newNextHops(0, "20.20.20."), DISTANCE)),
      FbossError);
}

// Test deleting routes
TEST(Route, delRoutes) {
  auto stateV1 = make_shared<SwitchState>();
  stateV1->publish();
  auto tables1 = stateV1->getRouteTables();
  auto rid = RouterID(0);
  RouteUpdater u1(tables1);

  RouteV4::Prefix prefix10{IPAddressV4("10.10.10.10"), 32};
  RouteV4::Prefix prefix22{IPAddressV4("22.22.22.22"), 32};

  u1.addRoute(
      rid,
      IPAddress("10.10.10.10"),
      32,
      CLIENT_A,
      RouteNextHopEntry(newNextHops(3, "1.1.1."), DISTANCE));
  u1.addRoute(
      rid,
      IPAddress("22.22.22.22"),
      32,
      CLIENT_B,
      RouteNextHopEntry(TO_CPU, DISTANCE));
  tables1 = u1.updateDone();
  EXPECT_NODEMAP_MATCH(tables1);

  // Both routes should be present
  auto ribV4 = tables1->getRouteTable(rid)->getRibV4();
  EXPECT_TRUE(nullptr != ribV4->exactMatch(prefix10));
  EXPECT_TRUE(nullptr != ribV4->exactMatch(prefix22));

  // delRoute() should work for the route with TO_CPU.
  RouteUpdater u2(tables1);
  u2.delRoute(rid, IPAddress("22.22.22.22"), 32, CLIENT_B);
  auto tables2 = u2.updateDone();
  EXPECT_NODEMAP_MATCH(tables2);

  // Route for 10.10.10.10 should still be there,
  // but route for 22.22.22.22 should be gone
  ribV4 = tables2->getRouteTable(rid)->getRibV4();
  EXPECT_TRUE(nullptr != ribV4->exactMatch(prefix10));
  EXPECT_TRUE(nullptr == ribV4->exactMatch(prefix22));
}

// Test equality of RouteNextHopsMulti.
TEST(Route, equality) {
  // Create two identical RouteNextHopsMulti, and compare
  RouteNextHopsMulti nhm1;
  nhm1.update(CLIENT_A, RouteNextHopEntry(newNextHops(3, "1.1.1."), DISTANCE));
  nhm1.update(CLIENT_B, RouteNextHopEntry(newNextHops(3, "2.2.2."), DISTANCE));

  RouteNextHopsMulti nhm2;
  nhm2.update(CLIENT_A, RouteNextHopEntry(newNextHops(3, "1.1.1."), DISTANCE));
  nhm2.update(CLIENT_B, RouteNextHopEntry(newNextHops(3, "2.2.2."), DISTANCE));

  EXPECT_TRUE(nhm1 == nhm2);

  // Delete data for CLIENT_C.  But there wasn't any.  Two objs still equal
  nhm1.delEntryForClient(CLIENT_C);
  EXPECT_TRUE(nhm1 == nhm2);

  // Delete obj1's CLIENT_B.  Now, objs should be NOT equal
  nhm1.delEntryForClient(CLIENT_B);
  EXPECT_FALSE(nhm1 == nhm2);

  // Now replace obj1's CLIENT_B list with a shorter list.
  // Objs should be NOT equal.
  nhm1.update(CLIENT_B, RouteNextHopEntry(newNextHops(2, "2.2.2."), DISTANCE));
  EXPECT_FALSE(nhm1 == nhm2);

  // Now replace obj1's CLIENT_B list with the original list.
  // But construct the list in opposite order.
  // Objects should still be equal, despite the order of construction.
  RouteNextHopSet nextHopsRev;
  nextHopsRev.emplace(
      UnresolvedNextHop(IPAddress("2.2.2.12"), UCMP_DEFAULT_WEIGHT));
  nextHopsRev.emplace(
      UnresolvedNextHop(IPAddress("2.2.2.11"), UCMP_DEFAULT_WEIGHT));
  nextHopsRev.emplace(
      UnresolvedNextHop(IPAddress("2.2.2.10"), UCMP_DEFAULT_WEIGHT));
  nhm1.update(CLIENT_B, RouteNextHopEntry(nextHopsRev, DISTANCE));
  EXPECT_TRUE(nhm1 == nhm2);
}

// Test that a copy of a RouteNextHopsMulti is a deep copy, and that the
// resulting objects can be modified independently.
TEST(Route, deepCopy) {
  // Create two identical RouteNextHopsMulti, and compare
  RouteNextHopsMulti nhm1;
  auto origHops = newNextHops(3, "1.1.1.");
  nhm1.update(CLIENT_A, RouteNextHopEntry(origHops, DISTANCE));
  nhm1.update(CLIENT_B, RouteNextHopEntry(newNextHops(3, "2.2.2."), DISTANCE));

  // Copy it
  RouteNextHopsMulti nhm2 = nhm1;

  // The two should be identical
  EXPECT_TRUE(nhm1 == nhm2);

  // Now modify the underlying nexthop list.
  // Should be changed in nhm1, but not nhm2.
  auto newHops = newNextHops(4, "10.10.10.");
  nhm1.update(CLIENT_A, RouteNextHopEntry(newHops, DISTANCE));

  EXPECT_FALSE(nhm1 == nhm2);

  EXPECT_TRUE(nhm1.isSame(CLIENT_A, RouteNextHopEntry(newHops, DISTANCE)));
  EXPECT_TRUE(nhm2.isSame(CLIENT_A, RouteNextHopEntry(origHops, DISTANCE)));
}

// Test serialization of RouteNextHopsMulti.
TEST(Route, serializeRouteNextHopsMulti) {
  // This function tests [de]serialization of:
  // RouteNextHopsMulti
  // RouteNextHopEntry
  // NextHop

  // test for new format to new format
  RouteNextHopsMulti nhm1;
  nhm1.update(CLIENT_A, RouteNextHopEntry(newNextHops(3, "1.1.1."), DISTANCE));
  nhm1.update(CLIENT_B, RouteNextHopEntry(newNextHops(1, "2.2.2."), DISTANCE));
  nhm1.update(CLIENT_C, RouteNextHopEntry(newNextHops(4, "3.3.3."), DISTANCE));
  nhm1.update(CLIENT_D, RouteNextHopEntry(RouteForwardAction::DROP, DISTANCE));
  nhm1.update(
      CLIENT_E, RouteNextHopEntry(RouteForwardAction::TO_CPU, DISTANCE));

  auto serialized = nhm1.toFollyDynamic();

  auto nhm2 = RouteNextHopsMulti::fromFollyDynamic(serialized);

  EXPECT_TRUE(nhm1 == nhm2);
}

// Test priority ranking of nexthop lists within a RouteNextHopsMulti.
TEST(Route, listRanking) {
  auto list00 = newNextHops(3, "0.0.0.");
  auto list07 = newNextHops(3, "7.7.7.");
  auto list01 = newNextHops(3, "1.1.1.");
  auto list20 = newNextHops(3, "20.20.20.");
  auto list30 = newNextHops(3, "30.30.30.");

  RouteNextHopsMulti nhm;
  nhm.update(ClientID(20), RouteNextHopEntry(list20, AdminDistance::EBGP));
  nhm.update(
      ClientID(1), RouteNextHopEntry(list01, AdminDistance::STATIC_ROUTE));
  nhm.update(
      ClientID(30),
      RouteNextHopEntry(list30, AdminDistance::MAX_ADMIN_DISTANCE));
  EXPECT_TRUE(nhm.getBestEntry().second->getNextHopSet() == list01);

  nhm.update(
      ClientID(10),
      RouteNextHopEntry(list00, AdminDistance::DIRECTLY_CONNECTED));
  EXPECT_TRUE(nhm.getBestEntry().second->getNextHopSet() == list00);

  nhm.delEntryForClient(ClientID(10));
  EXPECT_TRUE(nhm.getBestEntry().second->getNextHopSet() == list01);

  nhm.delEntryForClient(ClientID(30));
  EXPECT_TRUE(nhm.getBestEntry().second->getNextHopSet() == list01);

  nhm.delEntryForClient(ClientID(1));
  EXPECT_TRUE(nhm.getBestEntry().second->getNextHopSet() == list20);

  nhm.delEntryForClient(ClientID(20));
  EXPECT_THROW(nhm.getBestEntry().second->getNextHopSet(), FbossError);
}

bool stringStartsWith(std::string s1, std::string prefix) {
  return s1.compare(0, prefix.size(), prefix) == 0;
}

void assertClientsNotPresent(
    std::shared_ptr<RouteTableMap>& tables,
    RouterID rid,
    RouteV4::Prefix prefix,
    std::vector<int16_t> clientIds) {
  for (int16_t clientId : clientIds) {
    const auto& route =
        tables->getRouteTable(rid)->getRibV4()->exactMatch(prefix);
    auto entry = route->getEntryForClient(ClientID(clientId));
    EXPECT_EQ(nullptr, entry);
  }
}

void assertClientsPresent(
    std::shared_ptr<RouteTableMap>& tables,
    RouterID rid,
    RouteV4::Prefix prefix,
    std::vector<int16_t> clientIds) {
  for (int16_t clientId : clientIds) {
    const auto& route =
        tables->getRouteTable(rid)->getRibV4()->exactMatch(prefix);
    auto entry = route->getEntryForClient(ClientID(clientId));
    EXPECT_NE(nullptr, entry);
  }
}

void expectFwdInfo(
    std::shared_ptr<RouteTableMap>& tables,
    RouterID rid,
    RouteV4::Prefix prefix,
    std::string ipPrefix) {
  const auto& fwd = tables->getRouteTable(rid)
                        ->getRibV4()
                        ->exactMatch(prefix)
                        ->getForwardInfo();
  const auto& nhops = fwd.getNextHopSet();
  // Expect the fwd'ing info to be 3 IPs all starting with 'ipPrefix'
  EXPECT_EQ(3, nhops.size());
  for (auto const& it : nhops) {
    EXPECT_TRUE(stringStartsWith(it.addr().str(), ipPrefix));
  }
}

std::shared_ptr<RouteTableMap>& addNextHopsForClient(
    std::shared_ptr<RouteTableMap>& tables,
    RouterID rid,
    RouteV4::Prefix prefix,
    int16_t clientId,
    std::string ipPrefix,
    AdminDistance adminDistance = AdminDistance::MAX_ADMIN_DISTANCE) {
  RouteUpdater u(tables);
  u.addRoute(
      rid,
      prefix.network,
      prefix.mask,
      ClientID(clientId),
      RouteNextHopEntry(newNextHops(3, ipPrefix), adminDistance));
  tables = u.updateDone();
  EXPECT_NODEMAP_MATCH(tables);
  tables->publish();
  return tables;
}

std::shared_ptr<RouteTableMap>& deleteNextHopsForClient(
    std::shared_ptr<RouteTableMap>& tables,
    RouterID rid,
    RouteV4::Prefix prefix,
    int16_t clientId) {
  RouteUpdater u(tables);
  u.delRoute(rid, prefix.network, prefix.mask, ClientID(clientId));
  tables = u.updateDone();
  EXPECT_NODEMAP_MATCH(tables);
  tables->publish();
  return tables;
}

// Add and remove per-client NextHop lists to the same route, and make sure
// the lowest admin distance is the one that determines the forwarding info
TEST(Route, fwdInfoRanking) {
  auto stateV1 = make_shared<SwitchState>();
  stateV1->publish();
  auto tables = stateV1->getRouteTables();
  auto rid = RouterID(0);

  // We'll be adding and removing a bunch of nexthops for this Network & Mask
  auto network = IPAddressV4("22.22.22.22");
  uint8_t mask = 32;
  RouteV4::Prefix prefix{network, mask};

  // Add client 30, plus an interface for resolution.
  RouteUpdater u1(tables);
  // This is the route all the others will resolve to.
  u1.addRoute(
      rid,
      IPAddress("10.10.0.0"),
      16,
      StdClientIds2ClientID(StdClientIds::INTERFACE_ROUTE),
      RouteNextHopEntry(
          ResolvedNextHop(
              IPAddress("10.10.0.1"), InterfaceID(9), UCMP_DEFAULT_WEIGHT),
          AdminDistance::DIRECTLY_CONNECTED));
  u1.addRoute(
      rid,
      network,
      mask,
      ClientID(30),
      RouteNextHopEntry(newNextHops(3, "10.10.30."), DISTANCE));
  tables = u1.updateDone();
  EXPECT_NODEMAP_MATCH(tables);
  tables->publish();

  // Expect fwdInfo based on client 30
  assertClientsPresent(tables, rid, prefix, {30});
  assertClientsNotPresent(tables, rid, prefix, {10, 20, 40, 50, 999});
  expectFwdInfo(tables, rid, prefix, "10.10.30.");

  // Add client 20
  tables = addNextHopsForClient(
      tables, rid, prefix, 20, "10.10.20.", AdminDistance::EBGP);

  // Expect fwdInfo based on client 20
  assertClientsPresent(tables, rid, prefix, {20, 30});
  assertClientsNotPresent(tables, rid, prefix, {10, 40, 50, 999});
  expectFwdInfo(tables, rid, prefix, "10.10.20.");

  // Add client 40
  tables = addNextHopsForClient(tables, rid, prefix, 40, "10.10.40.");

  // Expect fwdInfo still based on client 20
  assertClientsPresent(tables, rid, prefix, {20, 30, 40});
  assertClientsNotPresent(tables, rid, prefix, {10, 50, 999});
  expectFwdInfo(tables, rid, prefix, "10.10.20.");

  // Add client 10
  tables = addNextHopsForClient(
      tables, rid, prefix, 10, "10.10.10.", AdminDistance::STATIC_ROUTE);

  // Expect fwdInfo based on client 10
  assertClientsPresent(tables, rid, prefix, {10, 20, 30, 40});
  assertClientsNotPresent(tables, rid, prefix, {50, 999});
  expectFwdInfo(tables, rid, prefix, "10.10.10.");

  // Remove client 20
  tables = deleteNextHopsForClient(tables, rid, prefix, 20);

  // Winner should still be 10
  assertClientsPresent(tables, rid, prefix, {10, 30, 40});
  assertClientsNotPresent(tables, rid, prefix, {20, 50, 999});
  expectFwdInfo(tables, rid, prefix, "10.10.10.");

  // Remove client 10
  tables = deleteNextHopsForClient(tables, rid, prefix, 10);

  // Winner should now be 30
  assertClientsPresent(tables, rid, prefix, {30, 40});
  assertClientsNotPresent(tables, rid, prefix, {10, 20, 50, 999});
  expectFwdInfo(tables, rid, prefix, "10.10.30.");

  // Remove client 30
  tables = deleteNextHopsForClient(tables, rid, prefix, 30);

  // Winner should now be 40
  assertClientsPresent(tables, rid, prefix, {40});
  assertClientsNotPresent(tables, rid, prefix, {10, 20, 30, 50, 999});
  expectFwdInfo(tables, rid, prefix, "10.10.40.");
}

TEST(Route, dropRoutes) {
  auto stateV1 = make_shared<SwitchState>();
  stateV1->publish();
  auto tables1 = stateV1->getRouteTables();
  auto rid = RouterID(0);
  RouteUpdater u1(tables1);
  u1.addRoute(
      rid,
      IPAddress("10.10.10.10"),
      32,
      CLIENT_A,
      RouteNextHopEntry(DROP, DISTANCE));
  u1.addRoute(
      rid,
      IPAddress("2001::0"),
      128,
      CLIENT_A,
      RouteNextHopEntry(DROP, DISTANCE));
  // Check recursive resolution for drop routes
  RouteNextHopSet v4nexthops = makeNextHops({"10.10.10.10"});
  u1.addRoute(
      rid,
      IPAddress("20.20.20.0"),
      24,
      CLIENT_A,
      RouteNextHopEntry(v4nexthops, DISTANCE));
  RouteNextHopSet v6nexthops = makeNextHops({"2001::0"});
  u1.addRoute(
      rid,
      IPAddress("2001:1::"),
      64,
      CLIENT_A,
      RouteNextHopEntry(v6nexthops, DISTANCE));

  auto tables2 = u1.updateDone();
  ASSERT_NE(nullptr, tables2);
  EXPECT_NODEMAP_MATCH(tables2);

  // Check routes
  auto r1 = GET_ROUTE_V4(tables2, rid, "10.10.10.10/32");
  EXPECT_RESOLVED(r1);
  EXPECT_FALSE(r1->isConnected());
  EXPECT_TRUE(r1->has(CLIENT_A, RouteNextHopEntry(DROP, DISTANCE)));
  EXPECT_EQ(r1->getForwardInfo(), RouteNextHopEntry(DROP, DISTANCE));

  auto r2 = GET_ROUTE_V4(tables2, rid, "20.20.20.0/24");
  EXPECT_RESOLVED(r2);
  EXPECT_FALSE(r2->isConnected());
  EXPECT_FALSE(r2->has(CLIENT_A, RouteNextHopEntry(DROP, DISTANCE)));
  EXPECT_EQ(r2->getForwardInfo(), RouteNextHopEntry(DROP, DISTANCE));

  auto r3 = GET_ROUTE_V6(tables2, rid, "2001::0/128");
  EXPECT_RESOLVED(r3);
  EXPECT_FALSE(r3->isConnected());
  EXPECT_TRUE(r3->has(CLIENT_A, RouteNextHopEntry(DROP, DISTANCE)));
  EXPECT_EQ(r3->getForwardInfo(), RouteNextHopEntry(DROP, DISTANCE));

  auto r4 = GET_ROUTE_V6(tables2, rid, "2001:1::/64");
  EXPECT_RESOLVED(r4);
  EXPECT_FALSE(r4->isConnected());
  EXPECT_EQ(r4->getForwardInfo(), RouteNextHopEntry(DROP, DISTANCE));
}

TEST(Route, toCPURoutes) {
  auto stateV1 = make_shared<SwitchState>();
  stateV1->publish();
  auto tables1 = stateV1->getRouteTables();
  auto rid = RouterID(0);
  RouteUpdater u1(tables1);
  u1.addRoute(
      rid,
      IPAddress("10.10.10.10"),
      32,
      CLIENT_A,
      RouteNextHopEntry(TO_CPU, DISTANCE));
  u1.addRoute(
      rid,
      IPAddress("2001::0"),
      128,
      CLIENT_A,
      RouteNextHopEntry(TO_CPU, DISTANCE));
  // Check recursive resolution for to_cpu routes
  RouteNextHopSet v4nexthops = makeNextHops({"10.10.10.10"});
  u1.addRoute(
      rid,
      IPAddress("20.20.20.0"),
      24,
      CLIENT_A,
      RouteNextHopEntry(v4nexthops, DISTANCE));
  RouteNextHopSet v6nexthops = makeNextHops({"2001::0"});
  u1.addRoute(
      rid,
      IPAddress("2001:1::"),
      64,
      CLIENT_A,
      RouteNextHopEntry(v6nexthops, DISTANCE));

  auto tables2 = u1.updateDone();
  ASSERT_NE(nullptr, tables2);
  EXPECT_NODEMAP_MATCH(tables2);

  // Check routes
  auto r1 = GET_ROUTE_V4(tables2, rid, "10.10.10.10/32");
  EXPECT_RESOLVED(r1);
  EXPECT_FALSE(r1->isConnected());
  EXPECT_TRUE(r1->has(CLIENT_A, RouteNextHopEntry(TO_CPU, DISTANCE)));
  EXPECT_EQ(r1->getForwardInfo(), RouteNextHopEntry(TO_CPU, DISTANCE));

  auto r2 = GET_ROUTE_V4(tables2, rid, "20.20.20.0/24");
  EXPECT_RESOLVED(r2);
  EXPECT_FALSE(r2->isConnected());
  EXPECT_EQ(r2->getForwardInfo(), RouteNextHopEntry(TO_CPU, DISTANCE));

  auto r3 = GET_ROUTE_V6(tables2, rid, "2001::0/128");
  EXPECT_RESOLVED(r3);
  EXPECT_FALSE(r3->isConnected());
  EXPECT_TRUE(r3->has(CLIENT_A, RouteNextHopEntry(TO_CPU, DISTANCE)));
  EXPECT_EQ(r3->getForwardInfo(), RouteNextHopEntry(TO_CPU, DISTANCE));

  auto r5 = GET_ROUTE_V6(tables2, rid, "2001:1::/64");
  EXPECT_RESOLVED(r5);
  EXPECT_FALSE(r5->isConnected());
  EXPECT_EQ(r5->getForwardInfo(), RouteNextHopEntry(TO_CPU, DISTANCE));
}

// Very basic test for serialization/deseralization of Routes
TEST(Route, serializeRoute) {
  ClientID clientId = ClientID(1);
  auto nxtHops = makeNextHops({"10.10.10.10", "11.11.11.11"});
  Route<IPAddressV4> rt(makePrefixV4("1.2.3.4/32"));
  rt.update(clientId, RouteNextHopEntry(nxtHops, DISTANCE));

  // to folly dynamic
  folly::dynamic obj = rt.toFollyDynamic();
  // to string
  folly::json::serialization_opts serOpts;
  serOpts.allow_non_string_keys = true;
  std::string json = folly::json::serialize(obj, serOpts);
  // back to folly dynamic
  folly::dynamic obj2 = folly::parseJson(json, serOpts);
  // back to Route object
  auto rt2 = Route<IPAddressV4>::fromFollyDynamic(obj2);
  ASSERT_TRUE(rt2->has(clientId, RouteNextHopEntry(nxtHops, DISTANCE)));
}

TEST(Route, serializeRouteTable) {
  auto stateV1 = make_shared<SwitchState>();
  stateV1->publish();

  auto rid = RouterID(0);
  // 2 different nexthops
  RouteNextHopSet nhop1 = makeNextHops({"1.1.1.10"}); // resolved by intf 1
  RouteNextHopSet nhop2 = makeNextHops({"2.2.2.10"}); // resolved by intf 2
  // 4 prefixes
  RouteV4::Prefix r1{IPAddressV4("10.1.1.0"), 24};
  RouteV4::Prefix r2{IPAddressV4("20.1.1.0"), 24};
  RouteV6::Prefix r3{IPAddressV6("1001::0"), 48};
  RouteV6::Prefix r4{IPAddressV6("2001::0"), 48};

  RouteUpdater u2(stateV1->getRouteTables());
  u2.addRoute(
      rid, r1.network, r1.mask, CLIENT_A, RouteNextHopEntry(nhop1, DISTANCE));
  u2.addRoute(
      rid, r2.network, r2.mask, CLIENT_A, RouteNextHopEntry(nhop2, DISTANCE));
  u2.addRoute(
      rid, r3.network, r3.mask, CLIENT_A, RouteNextHopEntry(nhop1, DISTANCE));
  u2.addRoute(
      rid, r4.network, r4.mask, CLIENT_A, RouteNextHopEntry(nhop2, DISTANCE));
  auto tables2 = u2.updateDone();
  ASSERT_NE(nullptr, tables2);
  EXPECT_NODEMAP_MATCH(tables2);

  // to folly dynamic
  folly::dynamic obj = tables2->toFollyDynamic();
  // to string
  folly::json::serialization_opts serOpts;
  serOpts.allow_non_string_keys = true;
  std::string json = folly::json::serialize(obj, serOpts);
  // back to folly dynamic
  folly::dynamic obj2 = folly::parseJson(json, serOpts);
  // back to Route object
  auto tables = RouteTableMap::fromFollyDynamic(obj2);
  EXPECT_NODEMAP_MATCH(tables);
  auto origRt = tables2->getRouteTable(rid);
  auto desRt = tables->getRouteTable(rid);
  EXPECT_ROUTETABLERIB_MATCH(origRt->getRibV4(), desRt->getRibV4());
  EXPECT_ROUTETABLERIB_MATCH(origRt->getRibV6(), desRt->getRibV6());
}

// Test utility functions for converting RouteNextHopSet to thrift and back
TEST(RouteTypes, toFromRouteNextHops) {
  RouteNextHopSet nhs;
  // Non v4 link-local address without interface scoping
  nhs.emplace(
      UnresolvedNextHop(folly::IPAddress("10.0.0.1"), UCMP_DEFAULT_WEIGHT));

  // v4 link-local address without interface scoping
  nhs.emplace(
      UnresolvedNextHop(folly::IPAddress("169.254.0.1"), UCMP_DEFAULT_WEIGHT));

  // Non v6 link-local address without interface scoping
  nhs.emplace(
      UnresolvedNextHop(folly::IPAddress("face:b00c::1"), UCMP_DEFAULT_WEIGHT));

  // v6 link-local address with interface scoping
  nhs.emplace(ResolvedNextHop(
      folly::IPAddress("fe80::1"), InterfaceID(4), UCMP_DEFAULT_WEIGHT));

  // v6 link-local address without interface scoping
  EXPECT_THROW(
      nhs.emplace(UnresolvedNextHop(folly::IPAddress("fe80::1"), 42)),
      FbossError);

  // Convert to thrift object
  auto nhts = util::fromRouteNextHopSet(nhs);
  ASSERT_EQ(4, nhts.size());

  auto verify = [&](const std::string& ipaddr,
                    folly::Optional<InterfaceID> intf) {
    auto bAddr = facebook::network::toBinaryAddress(folly::IPAddress(ipaddr));
    if (intf.hasValue()) {
      bAddr.__isset.ifName = true;
      bAddr.ifName = util::createTunIntfName(intf.value());
    }
    bool found = false;
    for (const auto& entry : nhts) {
      if (entry.address == bAddr) {
        if (intf.hasValue()) {
          EXPECT_TRUE(entry.address.__isset.ifName);
          EXPECT_EQ(bAddr.ifName, entry.address.ifName);
        }
        found = true;
        break;
      }
    }
    XLOG(INFO) << "**** " << ipaddr;
    EXPECT_TRUE(found);
  };

  verify("10.0.0.1", folly::none);
  verify("169.254.0.1", folly::none);
  verify("face:b00c::1", folly::none);
  verify("fe80::1", InterfaceID(4));

  // Convert back to RouteNextHopSet
  auto newNhs = util::toRouteNextHopSet(nhts);
  EXPECT_EQ(nhs, newNhs);

  //
  // Some ignore cases
  //

  facebook::network::thrift::BinaryAddress addr;

  addr = facebook::network::toBinaryAddress(folly::IPAddress("10.0.0.1"));
  addr.__isset.ifName = true;
  addr.ifName = "fboss10";
  NextHopThrift nht;
  nht.address = addr;
  {
    NextHop nh = util::fromThrift(nht);
    EXPECT_EQ(folly::IPAddress("10.0.0.1"), nh.addr());
    EXPECT_EQ(folly::none, nh.intfID());
  }

  addr = facebook::network::toBinaryAddress(folly::IPAddress("face::1"));
  addr.__isset.ifName = true;
  addr.ifName = "fboss10";
  nht.address = addr;
  {
    NextHop nh = util::fromThrift(nht);
    EXPECT_EQ(folly::IPAddress("face::1"), nh.addr());
    EXPECT_EQ(folly::none, nh.intfID());
  }

  addr = facebook::network::toBinaryAddress(folly::IPAddress("fe80::1"));
  addr.__isset.ifName = true;
  addr.ifName = "fboss10";
  nht.address = addr;
  {
    NextHop nh = util::fromThrift(nht);
    EXPECT_EQ(folly::IPAddress("fe80::1"), nh.addr());
    EXPECT_EQ(InterfaceID(10), nh.intfID());
  }
}

TEST(Route, nextHopTest) {
  folly::IPAddress addr("1.1.1.1");
  NextHop unh = UnresolvedNextHop(addr, 0);
  NextHop rnh = ResolvedNextHop(addr, InterfaceID(1), 0);
  EXPECT_FALSE(unh.isResolved());
  EXPECT_TRUE(rnh.isResolved());
  EXPECT_EQ(unh.addr(), addr);
  EXPECT_EQ(rnh.addr(), addr);
  EXPECT_THROW(unh.intf(), folly::OptionalEmptyException);
  EXPECT_EQ(rnh.intf(), InterfaceID(1));
  EXPECT_NE(unh, rnh);
  auto unh2 = unh;
  EXPECT_EQ(unh, unh2);
  auto rnh2 = rnh;
  EXPECT_EQ(rnh, rnh2);
  EXPECT_FALSE(rnh < unh && unh < rnh);
  EXPECT_TRUE(rnh < unh || unh < rnh);
  EXPECT_TRUE(unh < rnh && rnh > unh);
}

/*
 * Class that makes it easy to run tests with the following
 * configurable entities:
 * Four interfaces: I1, I2, I3, I4
 * Three routes which require resolution: R1, R2, R3
 */
class UcmpTest : public ::testing::Test {
 public:
  void SetUp() override {
    stateV1_ = applyInitConfig();
    ASSERT_NE(nullptr, stateV1_);
    rid_ = RouterID(0);
  }
  void resolveRoutes(std::vector<std::pair<folly::IPAddress, RouteNextHopSet>>
                         networkAndNextHops) {
    RouteUpdater ru(stateV1_->getRouteTables());
    for (const auto& nnhs : networkAndNextHops) {
      folly::IPAddress net = nnhs.first;
      RouteNextHopSet nhs = nnhs.second;
      ru.addRoute(rid_, net, mask, CLIENT_A, RouteNextHopEntry(nhs, DISTANCE));
    }
    auto tables = ru.updateDone();
    ASSERT_NE(nullptr, tables);
    EXPECT_NODEMAP_MATCH(tables);
    tables->publish();
    for (const auto& nnhs : networkAndNextHops) {
      folly::IPAddress net = nnhs.first;
      auto pfx = folly::sformat("{}/{}", net.str(), mask);
      auto r = GET_ROUTE_V4(tables, rid_, pfx);
      EXPECT_RESOLVED(r);
      EXPECT_FALSE(r->isConnected());
      resolvedRoutes.push_back(r);
    }
  }

  void runRecursiveTest(
      const std::vector<RouteNextHopSet>& routeUnresolvedNextHops,
      const std::vector<NextHopWeight>& resolvedWeights) {
    std::vector<std::pair<folly::IPAddress, RouteNextHopSet>>
        networkAndNextHops;
    auto netsIter = nets.begin();
    for (const auto& nhs : routeUnresolvedNextHops) {
      networkAndNextHops.push_back({*netsIter, nhs});
      netsIter++;
    }
    resolveRoutes(networkAndNextHops);

    RouteNextHopSet expFwd1;
    uint8_t i = 0;
    for (const auto& w : resolvedWeights) {
      expFwd1.emplace(ResolvedNextHop(intfIps[i], InterfaceID(i+1), w));
      ++i;
    }
    EXPECT_EQ(expFwd1, resolvedRoutes[0]->getForwardInfo().getNextHopSet());
  }

  void runTwoDeepRecursiveTest(
      const std::vector<std::vector<NextHopWeight>>& unresolvedWeights,
      const std::vector<NextHopWeight>& resolvedWeights) {
    std::vector<RouteNextHopSet> routeUnresolvedNextHops;
    auto rsIter = rnhs.begin();
    for (const auto& ws : unresolvedWeights) {
      auto nhIter = rsIter->begin();
      RouteNextHopSet nexthops;
      for (const auto& w : ws) {
        nexthops.insert(UnresolvedNextHop(*nhIter, w));
        nhIter++;
      }
      routeUnresolvedNextHops.push_back(nexthops);
      rsIter++;
    }
    runRecursiveTest(routeUnresolvedNextHops, resolvedWeights);
  }

  void runVaryFromHundredTest(
      NextHopWeight w,
      const std::vector<NextHopWeight>& resolvedWeights) {
    runRecursiveTest(
        {{UnresolvedNextHop(intfIp1, 100),
          UnresolvedNextHop(intfIp2, 100),
          UnresolvedNextHop(intfIp3, 100),
          UnresolvedNextHop(intfIp4, w)}},
        resolvedWeights);
  }

  std::vector<std::shared_ptr<Route<folly::IPAddressV4>>> resolvedRoutes;
  const folly::IPAddress intfIp1{"1.1.1.10"};
  const folly::IPAddress intfIp2{"2.2.2.20"};
  const folly::IPAddress intfIp3{"3.3.3.30"};
  const folly::IPAddress intfIp4{"4.4.4.40"};
  const std::array<folly::IPAddress, 4> intfIps{{intfIp1,
                                                intfIp2,
                                                intfIp3,
                                                intfIp4}};
  const folly::IPAddress r2Nh{"42.42.42.42"};
  const folly::IPAddress r3Nh{"43.43.43.43"};
  std::array<folly::IPAddress, 2> r1Nhs{{r2Nh, r3Nh}};
  std::array<folly::IPAddress, 2> r2Nhs{{intfIp1, intfIp2}};
  std::array<folly::IPAddress, 2> r3Nhs{{intfIp3, intfIp4}};
  const std::array<std::array<folly::IPAddress, 2>, 3> rnhs{{r1Nhs,
                                                            r2Nhs,
                                                            r3Nhs}};
  const folly::IPAddress r1Net{"41.41.41.0"};
  const folly::IPAddress r2Net{"42.42.42.0"};
  const folly::IPAddress r3Net{"43.43.43.0"};
  const std::array<folly::IPAddress, 3> nets{{r1Net, r2Net, r3Net}};
  const uint8_t mask{24};
 private:
  RouterID rid_;
  std::shared_ptr<SwitchState> stateV1_;
};

/*
 * Four interfaces: I1, I2, I3, I4
 * Three routes which require resolution: R1, R2, R3
 * R1 has R2 and R3 as next hops with weights 3 and 2
 * R2 has I1 and I2 as next hops with weights 5 and 4
 * R3 has I3 and I4 as next hops with weights 3 and 2
 * expect R1 to resolve to I1:25, I2:20, I3:18, I4:12
 */
TEST_F(UcmpTest, recursiveUcmp) {
  runTwoDeepRecursiveTest({{3, 2}, {5, 4}, {3, 2}}, {25, 20, 18, 12});
}

/*
 * Two interfaces: I1, I2
 * Three routes which require resolution: R1, R2, R3
 * R1 has R2 and R3 as next hops with weights 2 and 1
 * R2 has I1 and I2 as next hops with weights 1 and 1
 * R3 has I1 as next hop with weight 1
 * expect R1 to resolve to I1:2, I2:1
 */
TEST_F(UcmpTest, recursiveUcmpDuplicateIntf) {
  runRecursiveTest(
      {{UnresolvedNextHop(r2Nh, 2), UnresolvedNextHop(r3Nh, 1)},
       {UnresolvedNextHop(intfIp1, 1), UnresolvedNextHop(intfIp2, 1)},
       {UnresolvedNextHop(intfIp1, 1)}},
      {2, 1});
}

/*
 * Two interfaces: I1, I2
 * Three routes which require resolution: R1, R2, R3
 * R1 has R2 and R3 as next hops with ECMP
 * R2 has I1 and I2 as next hops with ECMP
 * R3 has I1 as next hop with weight 1
 * expect R1 to resolve to ECMP
 */
TEST_F(UcmpTest, recursiveEcmpDuplicateIntf) {
  runRecursiveTest(
      {{UnresolvedNextHop(r2Nh, ECMP_WEIGHT),
        UnresolvedNextHop(r3Nh, ECMP_WEIGHT)},
       {UnresolvedNextHop(intfIp1, ECMP_WEIGHT),
        UnresolvedNextHop(intfIp2, ECMP_WEIGHT)},
       {UnresolvedNextHop(intfIp1, 1)}},
      {ECMP_WEIGHT, ECMP_WEIGHT});
}

/*
 * Two interfaces: I1, I2
 * One route which requires resolution: R1
 * R1 has I1 and I2 as next hops with weights 0 (ECMP) and 1
 * expect R1 to resolve to ECMP between I1, I2
 */
TEST_F(UcmpTest, mixedUcmpVsEcmp_EcmpWins) {
  runRecursiveTest(
      {{UnresolvedNextHop(intfIp1, ECMP_WEIGHT),
        UnresolvedNextHop(intfIp2, 1)}},
      {ECMP_WEIGHT, ECMP_WEIGHT});
}

/*
 * Four interfaces: I1, I2, I3, I4
 * Three routes which require resolution: R1, R2, R3
 * R1 has R2 and R3 as next hops with weights 3 and 2
 * R2 has I1 and I2 as next hops with weights 5 and 4
 * R3 has I3 and I4 as next hops with ECMP
 * expect R1 to resolve to ECMP between I1, I2, I3, I4
 */
TEST_F(UcmpTest, recursiveEcmpPropagatesUp) {
  runTwoDeepRecursiveTest({{3, 2}, {5, 4}, {0, 0}}, {0, 0, 0, 0});
}

/*
 * Four interfaces: I1, I2, I3, I4
 * Three routes which require resolution: R1, R2, R3
 * R1 has R2 and R3 as next hops with weights 3 and 2
 * R2 has I1 and I2 as next hops with weights 5 and 4
 * R3 has I3 and I4 as next hops with weights 0 (ECMP) and 1
 * expect R1 to resolve to ECMP between I1, I2, I3, I4
 */
TEST_F(UcmpTest, recursiveMixedEcmpPropagatesUp) {
  runTwoDeepRecursiveTest({{3, 2}, {5, 4}, {0, 1}}, {0, 0, 0, 0});
}

/*
 * Four interfaces: I1, I2, I3, I4
 * Three routes which require resolution: R1, R2, R3
 * R1 has R2 and R3 as next hops with ECMP
 * R2 has I1 and I2 as next hops with weights 5 and 4
 * R3 has I3 and I4 as next hops with weights 3 and 2
 * expect R1 to resolve to ECMP between I1, I2, I3, I4
 */
TEST_F(UcmpTest, recursiveEcmpPropagatesDown) {
  runTwoDeepRecursiveTest({{0, 0}, {5, 4}, {3, 2}}, {0, 0, 0, 0});
}

/*
 * Two interfaces: I1, I2
 * Two routes which require resolution: R1, R2
 * R1 has I1 and I2 as next hops with ECMP
 * R2 has I1 and I2 as next hops with weights 2 and 1
 * expect R1 to resolve to ECMP between I1, I2
 * expect R2 to resolve to I1:2, I2: 1
 */
TEST_F(UcmpTest, separateEcmpUcmp) {
  runRecursiveTest(
      {{UnresolvedNextHop(intfIp1, ECMP_WEIGHT),
        UnresolvedNextHop(intfIp2, ECMP_WEIGHT)},
       {UnresolvedNextHop(intfIp1, 2), UnresolvedNextHop(intfIp2, 1)}},
      {ECMP_WEIGHT, ECMP_WEIGHT});
  RouteNextHopSet route2ExpFwd;
  route2ExpFwd.emplace(ResolvedNextHop(IPAddress("1.1.1.10"), InterfaceID(1), 2));
  route2ExpFwd.emplace(ResolvedNextHop(IPAddress("2.2.2.20"), InterfaceID(2), 1));
  EXPECT_EQ(route2ExpFwd, resolvedRoutes[1]->getForwardInfo().getNextHopSet());
}

// The following set of tests will start with 4 next hops all weight 100
// then vary one next hop by 10 weight increments to 90, 80, ... , 10

TEST_F(UcmpTest, Hundred) {
  runVaryFromHundredTest(100, {1, 1, 1, 1});
}

TEST_F(UcmpTest, Ninety) {
  runVaryFromHundredTest(90, {10, 10, 10, 9});
}

TEST_F(UcmpTest, Eighty) {
  runVaryFromHundredTest(80, {5, 5, 5, 4});
}

TEST_F(UcmpTest, Seventy) {
  runVaryFromHundredTest(70, {10, 10, 10, 7});
}

TEST_F(UcmpTest, Sixty) {
  runVaryFromHundredTest(60, {5, 5, 5, 3});
}

TEST_F(UcmpTest, Fifty) {
  runVaryFromHundredTest(50, {2, 2, 2, 1});
}

TEST_F(UcmpTest, Forty) {
  runVaryFromHundredTest(40, {5, 5, 5, 2});
}

TEST_F(UcmpTest, Thirty) {
  runVaryFromHundredTest(30, {10, 10, 10, 3});
}

TEST_F(UcmpTest, Twenty) {
  runVaryFromHundredTest(20, {5, 5, 5, 1});
}

TEST_F(UcmpTest, Ten) {
  runVaryFromHundredTest(10, {10, 10, 10, 1});
}