/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/hw/bcm/BcmAPI.h"

#include "fboss/agent/FbossError.h"
#include "fboss/agent/hw/bcm/BcmError.h"
#include "fboss/agent/hw/bcm/BcmFacebookAPI.h"
#include "fboss/agent/hw/bcm/BcmPlatform.h"
#include "fboss/agent/hw/bcm/BcmUnit.h"
#include "fboss/agent/hw/bcm/BcmWarmBootHelper.h"
#include "fboss/lib/AlertLogger.h"

#include <folly/experimental/StringKeyedUnorderedMap.h>

#include <folly/Memory.h>
#include <folly/String.h>
#include <folly/io/Cursor.h>
#include <folly/logging/xlog.h>
#include <glog/logging.h>

#include <atomic>
#include <unordered_map>

extern "C" {
#include <sal/core/boot.h>

#if (!defined(BCM_VER_MAJOR))
#include <soc/opensoc.h>
#define SYS_BE_PACKET 0
#else
#include <sal/core/thread.h>
#include <soc/defs.h>
#include <soc/scache.h>
#endif

#include <soc/cmext.h>
#include <systems/bde/linux/linux-bde.h>
}

using folly::StringPiece;
using std::make_unique;
using std::string;

DEFINE_string(
    l2xmsg_mode,
    "1",
    "Deliver L2 learning update callback via interrupt,"
    "drain L2 Mod FIFO on delivering callback");

/*
 * bde_create() must be defined as a symbol when linking against BRCM libs.
 * It should never be invoked in our setup though. So return a error
 */
extern "C" int bde_create() {
  XLOG(ERR) << "unexpected call to bde_create(): probe invoked "
               "via diag shell command?";
  return BCM_E_UNAVAIL;
}
/*
 * We don't set any default values.
 */
extern "C" void sal_config_init_defaults() {}

namespace {

/*
 * TODO (skhare)
 * Configerator change D18746658 introduces l2xmsg_mode to BCM config. It would
 * be activated as part of next disruptive upgrade which could take of the
 * order of several months/a year.
 *
 * We need l2xmst_mode setting sooner: MH-NIC queue-per-host L2 fix requires
 * it. Thus, temporarily hard code l2xmsg mode here. We also provide
 * FLAGS_l2xmsg_mode to disable this if needed.
 *
 * Broadcom has explicitly confirmed that setting l2xmsg_mode is safe across
 * the warmboot, and we have BCM tests that verify it.
 *
 * This logic can be removed on a fleet wide disruptive upgrade after D18746658
 * lands.
 *
 * The map is explicitly named as kBcmConfigsSafeAcrossWarmboot as only BCM
 * configs that can be safely applied post warmboot could be added here as
 * temporary workaround.
 */
const std::map<StringPiece, std::string> kBcmConfigsSafeAcrossWarmboot = {
    /*
     * Configure to get the callback via interrupts. Default is polling mode
     * which is expensive as a thread must periodically poll for the L2 table
     * updates. It is particularly wasteful given that the L2 table would likely
     * not change that often.
     * L2 MOD FIFO is used to queue up callbacks. If l2xmsg_mode is set to 1,
     * the L2 MOD FIFO is dequeued whenever a callback is delivered, otherwise
     * L2 MOD FIFO gets built up.
     */
    {"l2xmsg_mode", FLAGS_l2xmsg_mode},
};

constexpr auto kSDK6MMUStateKey = "mmu_lossless";
constexpr auto kSDK6L3ALPMState = "l3_alpm_enable";
constexpr auto kSDK6Is128ByteIpv6Enabled = "ipv6_lpm_128b_enable";
constexpr auto kSDK6ConfigStableSize = "stable_size";

constexpr auto kHSDKBcmDeviceKey = "bcm_device";
constexpr auto kHSDKDevice0Key = "0";
constexpr auto kHSDKBcmDeviceGlobalKey = "global";
constexpr auto kHSDKL3ALPMState = "l3_alpm_template";
constexpr auto kHSDKIs128ByteIpv6Enabled = "ipv6_lpm_128b_enable";
constexpr auto kHSDKDeviceKey = "device";
constexpr auto kHSDKTMTHDConfigKey = "TM_THD_CONFIG";
constexpr auto kHSDKThresholsModeKey = "THRESHOLD_MODE";
} // namespace

namespace facebook::fboss {

std::atomic<BcmUnit*> bcmUnits[SOC_MAX_NUM_SWITCH_DEVICES];
extern std::atomic<BcmUnit*> bcmUnits[];

std::atomic<bool> BcmAPI::bcmInitialized_ = false;

void BcmAPI::initConfig(const std::map<std::string, std::string>& config) {
  // Store the configuration settings
  getHwConfig().clear();
  for (const auto& entry : config) {
    getHwConfig().emplace(entry.first, entry.second);
  }
}

const char* FOLLY_NULLABLE BcmAPI::getConfigValue(StringPiece name) {
  auto it = getHwConfig().find(name);
  if (it != getHwConfig().end()) {
    return it->second.c_str();
  }

  /*
   * If a config is not part of bcmConfig, check the list of hard coded
   * configs, see comment at the top of kBcmConfigsSafeAcrossWarmboot for
   * additional context.
   */
  auto it2 = kBcmConfigsSafeAcrossWarmboot.find(name);
  if (it2 != kBcmConfigsSafeAcrossWarmboot.end()) {
    return it2->second.c_str();
  }

  return nullptr;
}

BcmMmuState BcmAPI::getMmuState() {
  if (BcmAPI::isHwUsingHSDK()) {
    auto mode = getYamlConfigValue<std::string>(
        getTMThresholdYamlNode(), kHSDKThresholsModeKey);
    if (mode) {
      XLOG(INFO) << "MMU state is " << *mode;
      if (*mode == "LOSSY") {
        return BcmMmuState::MMU_LOSSY;
      } else if (*mode == "LOSSLESS") {
        return BcmMmuState::MMU_LOSSLESS;
      }
    }
    return BcmMmuState::UNKNOWN;
  } else {
    auto lossless = BcmAPI::getConfigValue(kSDK6MMUStateKey);
    if (!lossless) {
      return BcmMmuState::UNKNOWN;
    }
    return std::string(lossless) == "0x1" ? BcmMmuState::MMU_LOSSLESS
                                          : BcmMmuState::MMU_LOSSY;
  }
}

bool BcmAPI::is128ByteIpv6Enabled() {
  if (BcmAPI::isHwUsingHSDK()) {
    auto state = BcmAPI::getYamlConfigValue<int>(
        getGlobalBcmDeviceYamlNode(), kHSDKIs128ByteIpv6Enabled);
    if (!state || *state != 1) {
      return false;
    }
    return true;
  } else {
    auto state = BcmAPI::getConfigValue(kSDK6Is128ByteIpv6Enabled);
    if (!state) {
      return false;
    }
    return std::stoul(state, nullptr, 0) == 1;
  }
}

bool BcmAPI::isAlpmEnabled() {
  if (BcmAPI::isHwUsingHSDK()) {
    auto state = BcmAPI::getYamlConfigValue<int>(
        getGlobalBcmDeviceYamlNode(), kHSDKL3ALPMState);
    if (!state || *state == 0) {
      return false;
    }
    // 1: combined mode, 2: parallel mode. But both are alpm enabled
    return true;
  } else {
    auto state = BcmAPI::getConfigValue(kSDK6L3ALPMState);
    if (!state) {
      return false;
    }
    // Use 2 for enabled state
    return std::stoul(state, nullptr, 0) == 2;
  }
}

uint64_t BcmAPI::getConfigStableSize() {
  auto stableSize = SOC_DEFAULT_LVL2_STABLE_SIZE;
  if (BcmAPI::isHwUsingHSDK()) {
    // TODO(joseph) Needs to confirm w/ Broadcom whether HSDK still use this
    // for warmboot
  } else {
    auto configStableSize = BcmAPI::getConfigValue(kSDK6ConfigStableSize);
    if (configStableSize) {
      stableSize = std::stoul(configStableSize, nullptr, 0);
    }
  }
  return stableSize;
}

BcmAPI::HwConfigMap& BcmAPI::getHwConfig() {
  // Avoid static initialization disaster
  // (https://isocpp.org/wiki/faq/ctors#static-init-order)
  static BcmAPI::HwConfigMap bcmConfig;
  return bcmConfig;
}

void BcmAPI::initYamlConfig(const std::string& yamlConfig) {
  // We usually keep some of the global settings in bcm_device:0:global node
  auto yamlNodes = YAML::LoadAll(yamlConfig);
  for (auto yamlNode : yamlNodes) {
    // Only care about bcm_device:0:global node
    if (auto node = yamlNode[kHSDKBcmDeviceKey]) {
      if (auto deviceNode = node[kHSDKDevice0Key]) {
        if (auto globalNode = deviceNode[kHSDKBcmDeviceGlobalKey]) {
          XLOG(DBG1) << "Found bcm_device:0:global yaml node";
          getGlobalBcmDeviceYamlNode().reset(globalNode);
        }
      }
    } else if (auto node = yamlNode[kHSDKDeviceKey]) {
      if (auto deviceNode = node[kHSDKDevice0Key]) {
        if (auto thresholdNode = deviceNode[kHSDKTMTHDConfigKey]) {
          XLOG(DBG1) << "Found device:0:TM_THD_CONFIG yaml node";
          getTMThresholdYamlNode().reset(thresholdNode);
        }
      }
    }
  }
  getHwYamlConfig().assign(yamlConfig);
}

std::string& BcmAPI::getHwYamlConfig() {
  static string bcmYamlConfig;
  return bcmYamlConfig;
}

YAML::Node& BcmAPI::getGlobalBcmDeviceYamlNode() {
  static YAML::Node globalYamlNode;
  return globalYamlNode;
}

YAML::Node& BcmAPI::getTMThresholdYamlNode() {
  static YAML::Node thresholdNode;
  return thresholdNode;
}

template <typename ValueT>
std::optional<ValueT> BcmAPI::getYamlConfigValue(
    const YAML::Node& node,
    const std::string& name) {
  auto valueNode = node[name];
  if (valueNode) {
    return valueNode.as<ValueT>();
  }
  return std::nullopt;
}

std::unique_ptr<BcmUnit> BcmAPI::createUnit(
    int deviceIndex,
    BcmPlatform* platform) {
  auto unitObj = make_unique<BcmUnit>(deviceIndex, platform);
  int unit = unitObj->getNumber();
  BcmUnit* expectedUnit{nullptr};
  if (!bcmUnits[unit].compare_exchange_strong(
          expectedUnit, unitObj.get(), std::memory_order_acq_rel)) {
    throw FbossError("a BcmUnit already exists for unit number ", unit);
  }
  platform->onUnitCreate(unit);

  return unitObj;
}

void BcmAPI::initUnit(int unit, BcmPlatform* platform) {
  BcmUnit* unitObj = getUnit(unit);
  if (platform->getWarmBootHelper()->canWarmBoot()) {
    unitObj->warmBootAttach();
  } else {
    unitObj->coldBootAttach();
  }
  platform->onUnitAttach(unit);
}

void BcmAPI::bdeCreate() {
  int rv = 0;

  if (!isHwInSimMode()) {
    // Initialize the BDE singleton (Broadcom Device Enumerator)
    linux_bde_bus_t bus;
    bus.be_pio = (folly::Endian::order == folly::Endian::Order::BIG) ? 1 : 0;
    bus.be_packet =
        SYS_BE_PACKET; // Always false, regardless of host byte order
    bus.be_other = (folly::Endian::order == folly::Endian::Order::BIG) ? 1 : 0;
    rv = linux_bde_create(&bus, &bde);
    bcmCheckError(rv, "failed to initialize BDE");
  } else {
    XLOG(INFO) << "Bcm running in SIM mode";
    bdeCreateSim();
  }
}

void BcmAPI::init(const std::map<std::string, std::string>& config) {
  if (bcmInitialized_.load(std::memory_order_acquire)) {
    return;
  }

  initConfig(config);

  BcmFacebookAPI::initBSL();

  // Initialize the Broadcom core support libraries
  int rv = sal_core_init();
  bcmCheckError(rv, "error initializing core SAL");

  rv = soc_cm_init();

  bcmCheckError(rv, "error initializing config manager");

  BcmAPI::initImpl();

  bdeCreate();

  bcmInitialized_.store(true, std::memory_order_release);
}

void BcmAPI::initHSDK(const std::string& yamlConfig) {
  if (bcmInitialized_.load(std::memory_order_acquire)) {
    return;
  }

  initHSDKImpl(yamlConfig);

  bcmInitialized_.store(true, std::memory_order_release);
}

bool BcmAPI::isHwInSimMode() {
  static const bool isSimMode_ = std::getenv("BCM_SIM_PATH");
  return isSimMode_;
}

std::unique_ptr<BcmUnit> BcmAPI::createOnlyUnit(BcmPlatform* platform) {
  auto numDevices = BcmAPI::getNumSwitches();
  if (numDevices == 0) {
    throw FbossError(
        PlatformAlert().str() + "no Broadcom switching ASIC found");
  } else if (numDevices > 1) {
    throw FbossError("found more than 1 Broadcom switching ASIC");
  }
  return createUnit(0, platform);
}

void BcmAPI::unitDestroyed(BcmUnit* unit) {
  int num = unit->getNumber();
  BcmUnit* expectedUnit{unit};
  if (!bcmUnits[num].compare_exchange_strong(
          expectedUnit, nullptr, std::memory_order_acq_rel)) {
    XLOG(FATAL) << "inconsistency in BCM unit array for unit " << num
                << ": expected " << (void*)unit << " but found "
                << (void*)expectedUnit;
  }
  bcmInitialized_.store(false, std::memory_order_release);
}

BcmUnit* BcmAPI::getUnit(int unit) {
  if (unit < 0 || unit > getMaxSwitches()) {
    throw FbossError("invalid BCM unit number ", unit);
  }
  BcmUnit* unitObj = bcmUnits[unit].load(std::memory_order_acquire);
  if (!unitObj) {
    throw FbossError("no BcmUnit created for unit number ", unit);
  }
  return unitObj;
}

/*
 * Get the number of Broadcom switching devices in this system.
 */
size_t BcmAPI::getMaxSwitches() {
  return SOC_MAX_NUM_SWITCH_DEVICES;
}

/*
 * Get the thread name defined for this thread by the Broadcom SDK.
 */
std::string BcmAPI::getThreadName() {
  auto thread = sal_thread_self();
  if (thread == sal_thread_main_get()) {
    return "Main";
  }
  char threadName[80];
  sal_thread_name(thread, threadName, sizeof(threadName));
  return threadName;
}
} // namespace facebook::fboss
