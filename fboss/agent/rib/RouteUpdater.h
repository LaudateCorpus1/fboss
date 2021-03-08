/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#pragma once

#include "fboss/agent/types.h"

#include "fboss/agent/rib/NetworkToRouteMap.h"
#include "fboss/agent/rib/Route.h"

#include <folly/IPAddress.h>

namespace facebook::fboss {

/**
 * Expected behavior of RibRouteUpdater::resolve():
 *
 * RibRouteUpdater::resolve() resolves the route table forwarding information
 * based on the RIB, by doing recursively route table lookup. At the end of
 * the process, every route will be either unresolved or resolved with a
 * ECMP group.
 *
 * There are clear expectation on resolving FIB for a route, when all
 * nexthops are resolved to actual IPs. However, if is not clearly
 * defined and documented expectation if ECMP group has mix of action(s)
 * (i.e. DROP, TO_CPU) and IP nexthops.
 *
 * The following is the current implentation of resolve():
 * 1. No weighted ECMP. Each entry in the ECMP group is unique and has equal
 *    weight.
 * 2. A ECMP group could have either DROP, TO_CPU, or a set of IP nexthops.
 * 3. If DROP and other types (i.e. TO_CPU and IP nexthops) are part of the
 *    results of route resolve process. The finally FIB will be DROP.
 * 4. If TO_CPU and IP nexthops are part of the results of resolving process,
 *    only IP nexthops will be in the final ECMP group.
 * 5. If and only if TO_CPU is the only nexthop (directly or indirectly) of
 *    a route, TO_CPU action will be only path in the resolved ECMP group.
 */
class RibRouteUpdater {
 public:
  RibRouteUpdater(
      IPv4NetworkToRouteMap* v4Routes,
      IPv6NetworkToRouteMap* v6Routes);

  struct RouteEntry {
    folly::CIDRNetwork prefix;
    ClientID client;
    RouteNextHopEntry nhopEntry;
  };
  /*
   * Will return previous route on replacement, nullopt
   * otherwise
   */
  std::optional<RouteEntry> addOrReplaceRoute(
      const folly::IPAddress& network,
      uint8_t mask,
      ClientID clientID,
      RouteNextHopEntry entry);
  // No return value, since we always add the same
  // link local route. So there is no replacing of
  // routes here
  void addLinkLocalRoutes();
  std::optional<RouteEntry> addOrReplaceInterfaceRoute(
      const folly::IPAddress& network,
      uint8_t mask,
      const folly::IPAddress& address,
      InterfaceID interface);
  std::optional<RouteEntry>
  delRoute(const folly::IPAddress& network, uint8_t mask, ClientID clientID);
  std::vector<RouteEntry> removeAllRoutesForClient(ClientID clientID);

  void updateDone();

 private:
  template <typename AddressT>
  using Prefix = RoutePrefix<AddressT>;

  template <typename AddressT>
  static std::optional<RouteNextHopEntry> addOrReplaceRouteImpl(
      const Prefix<AddressT>& prefix,
      NetworkToRouteMap<AddressT>* routes,
      ClientID clientID,
      RouteNextHopEntry entry);
  template <typename AddressT>
  static std::optional<RouteNextHopEntry> delRouteImpl(
      const Prefix<AddressT>& prefix,
      NetworkToRouteMap<AddressT>* routes,
      ClientID clientID);
  template <typename AddressT>
  static void removeAllRoutesFromClientImpl(
      NetworkToRouteMap<AddressT>* routes,
      ClientID clientID,
      std::vector<RouteEntry>* deleted);

  template <typename AddressT>
  void updateDoneImpl(NetworkToRouteMap<AddressT>* routes);

  template <typename AddressT>
  void resolve(NetworkToRouteMap<AddressT>* routes);
  template <typename AddressT>
  void resolveOne(RibRoute<AddressT>* route);

  template <typename AddressT>
  void getFwdInfoFromNhop(
      NetworkToRouteMap<AddressT>* routes,
      const AddressT& nh,
      const std::optional<LabelForwardingAction>& labelAction,
      bool* hasToCpu,
      bool* hasDrop,
      RouteNextHopSet& fwd);

  IPv4NetworkToRouteMap* v4Routes_{nullptr};
  IPv6NetworkToRouteMap* v6Routes_{nullptr};
};

} // namespace facebook::fboss
