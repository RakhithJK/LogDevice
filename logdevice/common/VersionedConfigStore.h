/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <unordered_map>

#include <folly/Function.h>
#include <folly/Optional.h>
#include <folly/Synchronized.h>

#include "logdevice/common/types_internal.h"
#include "logdevice/include/Err.h"

namespace facebook { namespace logdevice {

// Defines a key-value like API for a versioned config store.
class VersionedConfigStore {
 public:
  using version_t = vcs_config_version_t;

  // The status codes may be one of the following if the callback is invoked:
  //   OK
  //   NOTFOUND: key not found, corresponds to ZNONODE
  //   VERSION_MISMATCH: corresponds to ZBADVERSION
  //   ACCESS: permission denied, corresponds to ZNOAUTH
  //   UPTODATE: current version is up-to-date for conditional get
  //   AGAIN: transient errors (including connection closed ZCONNECTIONLOSS,
  //          timed out ZOPERATIONTIMEOUT, throttled ZWRITETHROTTLE)
  using value_callback_t = folly::Function<void(Status, std::string)>;
  using write_callback_t =
      folly::Function<void(Status, version_t, std::string)>;

  /*
   *  @param folly::Optional<std::string>
   *         The current value in the config store.
   *         If there is no current value for the key, then folly::none
   *  @return std::pair<Status, folly::Optional<std::string>
   *         <Status,Value> after mutation. If Status::OK, mutation will
   *         continue with the update using the returned value, else value
   *         will be ignored.
   */
  using mutation_callback_t = folly::Function<std::pair<Status, std::string>(
      folly::Optional<std::string>)>;

  // Function that VersionedConfigStore could call on stored values to
  // extract the corresponding membership version. If value is invalid, the
  // function should return folly::none.
  //
  // This function should be synchronous, relatively fast, and should not
  // consume the value string (folly::StringPiece does not take ownership of the
  // string).
  using extract_version_fn =
      folly::Function<folly::Optional<version_t>(folly::StringPiece) const>;

  VersionedConfigStore() = delete;
  explicit VersionedConfigStore(extract_version_fn fn)
      : extract_fn_(std::move(fn)) {}

  virtual ~VersionedConfigStore() {}

  /*
   * read
   *
   * @param key: key of the config
   * @param cb:
   *   callback void(Status, std::string value) that will be invoked with
   *   status OK, NOTFOUND, ACCESS, INVALID_PARAM, INVALID_CONFIG, AGAIN, or
   *   SHUTDOWN. If status is OK, cb will be invoked with the value. Otherwise,
   *   the value parameter is meaningless (but default-constructed).
   * @param base_version
   *   an optional conditional version. If set, it instructs the store only
   *   delivers config if its version is > base_version. Otherwise the cb will
   *   be called with status == E::UPTODATE with an empty string.
   *   Implementations can take advantage of such conditional fetch to reduce
   *   the amount of data transmissions.
   *
   * Note that the reads do not need to be linearizable with the writes.
   */
  virtual void
  getConfig(std::string key,
            value_callback_t cb,
            folly::Optional<version_t> base_version = {}) const = 0;

  /*
   * synchronous read
   *
   * @param key: key of the config
   * @param value_out: the config (string)
   *   If the status is OK, value will be set to the returned config. Otherwise,
   *   it will be untouched.
   * @param base_version   see getConfig() above.
   *
   * @return status is one of:
   *   OK // == 0
   *   NOTFOUND
   *   ACCESS
   *   AGAIN
   *   INVALID_PARAM
   *   INVALID_CONFIG
   *   UPTODATE
   *   SHUTDOWN
   */
  virtual Status
  getConfigSync(std::string key,
                std::string* value_out,
                folly::Optional<version_t> base_version = {}) const;

  /*
   * strongly consistent read. Ensures that the returned config reflects any
   * config updates that completed before this method is invoked.
   *
   * NOTE: This has higher chance of failing than getConfig as it favors
   * consistency over availability. Unless you can't tolerate stale configs, you
   * should use getConfig instead. It's also more expensive. This function is
   * meant to be used sparingly.
   *
   * Same params as getConfig.
   */
  virtual void getLatestConfig(std::string key, value_callback_t cb) const = 0;

  /*
   * VersionedConfigStore provides strict conditional update semantics--it
   * will only update the value for a key if the base_version matches the latest
   * version in the store.
   *
   * @param key: key of the config
   * @param value:
   *   value to be stored. Note that the callsite need not guarantee the
   *   validity of the underlying buffer till callback is invoked.
   * @param base_version:
   *   base_version == folly::none =>
   *     overwrites the corresponding config for key with value, regardless of
   *     its current version. Also used for the initial config.
   *     Implementation note: this is different from the Zookeeper setData call,
   *     which would complain ZNONODE if the znode has not already been created.
   *   base_version.hasValue() =>
   *     strict conditional update: only update the config when the existing
   *     version matches base_version.
   * @param cb:
   *   callback void(Status, version_t, std::string value) that will be invoked
   *   if the status is one of:
   *     OK
   *     NOTFOUND // only possible when base_version.hasValue()
   *     VERSION_MISMATCH
   *     ACCESS
   *     AGAIN
   *     BADMSG // see implementation notes below
   *     INVALID_PARAM // see implementation notes below
   *     INVALID_CONFIG // see implementation notes below
   *     SHUTDOWN
   *   If status is OK, cb will be invoked with the version of the newly written
   *   config. If status is VERSION_MISMATCH, cb will be invoked with the
   *   version that caused the mismatch as well as the existing config, if
   *   available (i.e., always check in the callback whether version is
   *   EMPTY_VERSION). Otherwise, the version and value parameter(s) are
   *   meaningless (default-constructed).
   *
   */
  virtual void updateConfig(std::string key,
                            std::string value,
                            folly::Optional<version_t> base_version,
                            write_callback_t cb);

  /*
   * Read-Modify-Write a configuration value using a given key.
   * @brief
   *   On read:
   *      - If value is read or key not found, mutation callback will be called
   *        with folly::Optional<std::string> for value.
   *        If the key is not found., mutation callback will be called with
   *        folly::none
   *      - If there is any error during the read, write_callback cb will be
   *        invoked with the error.
   *   Mutation callback.
   *       - Mutation callback is used to read the current value and return
   *         the new value to be written for a given key and status.
   *       - Mutation callback should return with status,value to continue
   *         to the write phaste.
   *       - If the status returned is OK, the new value will be written to the
   *         store. Else, the write_callback wb will be invoked with the
   *         returned status and value.
   *         The value will be ignored if status != Status::OK
   *   Write callback.
   *     Write callback is the final callback for the readModifyWriteUpdate
   *     api. All errors for this api should be handled by the user as part
   *     this callback.
   *      Error path:
   *        - Write callback with the error will be called during
   *          read/mutate/write path. The errors returned by mutation cb
   *          will be propagated to write callback.
   *      Success:
   *        - On success, cb will be invoked with the version of the newly
   *          written config. The version of new value to be written should be
   *          greater than the version of the read value.
   *
   * @param key: key of the config
   * @param mcb:
   *     mcb callback :
   *   	 callback std::pair<Status,std::string>(folly::Optional<std::string>)
   *   	 will be invoked with
   *   	     - Current value if it exists in config. Else, folly::none
   *   	 mcb should return std::pair<Status, std::string> ==>
   *   	     Status: Status of the mutator function. If OK, the associated
   *   	     std::string (value) will be updated in the config. Else, cb with
   *   	     status will be invoked with the status and value returned by
   *         mutator function.
   *   	     Value (std::string): Value to be updated in config.
   *         The mutator function is allowed to return the following status.
   *            - Status::OK, Status::VERSION_MISMATCH and Status::SHUTDOWN.
   * @param cb:
   *  Write callback.
   *     Write callback is the final callback for the readModifyWriteUpdate
   *     api. All errors for this api should be handled by the user as part
   *     this callback.
   *      Error path:
   *        - Write callback will the error will be called during
   *          read/mutate/write path. The errors propagated from mutation cb
   *          will be propagated to write callback.
   *   callback void(Status, version_t, std::string value) that will be invoked
   *   if the status is one of:
   *     OK
   *     NOTFOUND // only possible when base_version.hasValue()
   *     VERSION_MISMATCH
   *     ACCESS
   *     AGAIN
   *     BADMSG // see implementation notes below
   *     INVALID_PARAM // see implementation notes below
   *     INVALID_CONFIG // see implementation notes below
   *     SHUTDOWN
   *   If status is OK, cb will be invoked with the version of the newly written
   *   config. If status is VERSION_MISMATCH, cb will be invoked with the
   *   version that caused the mismatch as well as the existing config, if
   *   available (i.e., always check in the callback whether version is
   *   EMPTY_VERSION). Otherwise, the version and value parameter(s) are
   *   meaningless (default-constructed).
   */
  virtual void readModifyWriteConfig(std::string key,
                                     mutation_callback_t mcb,
                                     write_callback_t cb) = 0;

  /*
   * Synchronous updateConfig
   *
   * See params for updateConfig()
   *
   * @param version_out:
   *   If not nullptr and status is OK or VERSION_MISMATCH, *version_out
   *   will be set to the version of the newly written config or the version
   *   that caused the mismatch, respectively. Otherwise, *version_out is
   *   untouched.
   * @param value_out:
   *   If not nullptr and status is VERSION_MISMATCH, *value_out will be set to
   *   the existing config. Otherwise, *value_out is untouched.
   * @return status: status will be one of:
   *   OK // == 0
   *   NOTFOUND // only possible when base_version.hasValue()
   *   VERSION_MISMATCH
   *   ACCESS
   *   AGAIN
   *   BADMSG
   *   INVALID_PARAM
   *   INVALID_CONFIG
   *   SHUTDOWN
   */
  virtual Status updateConfigSync(std::string key,
                                  std::string value,
                                  folly::Optional<version_t> base_version,
                                  version_t* version_out = nullptr,
                                  std::string* value_out = nullptr);

  /*
   * After shutdown returns, VCS will guarantee (1) not to accept new read/write
   * requests; (2) no more outstanding user-supplied callbacks will be invoked,
   * (e.g., either by invoking all outstanding callbacks with E::SHUTDOWN, or by
   * joining / destroying all the threads / EventBases the VCS has spawned.)
   *
   * This method may block, so it should be called on the main / Processor
   * shutdown thread to avoid deadlocks. It should only be called once.
   *
   * Reads and writes (from other threads) to VCS during shutdown (or started
   * before shutdown) will either complete normally or complete with E::SHUTDOWN
   * (depending on shutdown progress). (This means no additional synchronization
   * between the shutdown thread and VCS user thread(s) is necessary.)
   *
   * VCS methods should not be called _after shutdown() returns_.
   */
  virtual void shutdown() = 0;

  // TODO: add subscription API

 protected:
  extract_version_fn extract_fn_;
};

}} // namespace facebook::logdevice