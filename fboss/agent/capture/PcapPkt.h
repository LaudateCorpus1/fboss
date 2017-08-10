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

#include <chrono>
#include <vector>
#include <folly/io/IOBuf.h>
#include "fboss/agent/types.h"
#include "fboss/pcap_distribution_service/if/gen-cpp2/pcap_pubsub_types.h"

namespace facebook { namespace fboss {

class RxPacket;
class TxPacket;
class RxPacketData;
class TxPacketData;

/*
 * PcapPkt represents a packet captured on the wire.
 */
class PcapPkt {
 public:
  typedef std::chrono::system_clock::time_point TimePoint;

  /*
   * Create an uninitialized PcapPkt
   */
  PcapPkt();

  /*
   * Create a PcapPkt from an RxPacket
   */
  explicit PcapPkt(const RxPacket* pkt);
  PcapPkt(const RxPacket* pkt, TimePoint timestamp);

  /*
   * Create a PcapPkt from a TxPacket
   */
  explicit PcapPkt(const TxPacket* pkt);
  PcapPkt(const TxPacket* pkt, TimePoint timestamp);

  /*
   * Create a PcapPkt from distribution service data
   */
  explicit PcapPkt(const RxPacketData* pkt);
  PcapPkt(const RxPacketData* pkt, TimePoint timestamp);

  /*
   * Create a PcapPkt from distribution service data
   */
  explicit PcapPkt(const TxPacketData* pkt);
  PcapPkt(const TxPacketData* pkt, TimePoint timestamp);

  bool initialized() const {
    return initialized_;
  }
  bool isRx() const {
    return rx_;
  }
  bool isTx() const {
    return !rx_;
  }
  PortID port() const {
    return port_;
  }
  VlanID vlan() const {
    return vlan_;
  }
  TimePoint timestamp() const {
    return timestamp_;
  }
  const folly::IOBuf* buf() const {
    return &buf_;
  }
  std::vector<RxReason> getReasons(){
    return reasons_;
  }

  // Move assignment
  PcapPkt(PcapPkt&& other) noexcept {
    *this = std::move(other);
  }
  PcapPkt& operator=(PcapPkt&& other) noexcept {
    initialized_ = other.initialized_;
    other.initialized_ = false;

    rx_ = other.rx_;
    port_ = other.port_;
    vlan_ = other.vlan_;
    timestamp_ = other.timestamp_;
    buf_ = std::move(other.buf_);
    reasons_ = std::move(other.reasons_);
    return *this;
  }

  PcapPkt(PcapPkt const&) = default;
  PcapPkt& operator=(PcapPkt const&) = default;

 private:
  bool initialized_{false};
  // Whether or not we received this packet, or are sending it.
  bool rx_{false};
  // The port the packet was sent or received on.
  //
  // We may want to change this to a port bitmap in the future, since TX
  // packets can be sent to multiple ports.  For TX packets, the software can
  // send packets to port bitmaps, to a VLAN, or to an L3 router ID.  For
  // sending to a VLAN or L3 router ID we should ideally compute the ports it
  // will egress from.
  PortID port_{0};
  // The VLAN the packet was sent or received on.
  VlanID vlan_{0};
  TimePoint timestamp_;
  // The packet contents, starting from the ethernet header.
  folly::IOBuf buf_;
  // Reasons for sending packet to CPU
  std::vector<RxReason> reasons_;
};

}} // facebook::fboss
