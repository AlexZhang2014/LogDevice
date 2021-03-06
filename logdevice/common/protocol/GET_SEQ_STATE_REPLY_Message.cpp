/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "logdevice/common/protocol/GET_SEQ_STATE_REPLY_Message.h"

#include <folly/Memory.h>

#include "logdevice/common/GetSeqStateRequest.h"
#include "logdevice/common/Sender.h"
#include "logdevice/common/Worker.h"
#include "logdevice/common/protocol/ProtocolReader.h"
#include "logdevice/common/protocol/ProtocolWriter.h"

namespace facebook { namespace logdevice {

MessageReadResult
GET_SEQ_STATE_REPLY_Message::deserialize(ProtocolReader& reader) {
  GET_SEQ_STATE_REPLY_Header header;
  header.flags = 0;
  reader.read(&header, sizeof(header));

  auto msg = std::make_unique<GET_SEQ_STATE_REPLY_Message>(header);

  reader.read(&msg->request_id_);
  reader.read(&msg->status_);
  reader.read(&msg->redirect_);

  if (header.flags & GET_SEQ_STATE_REPLY_Header::INCLUDES_TAIL_ATTRIBUTES) {
    uint64_t timestamp;
    reader.read(&msg->tail_attributes_.last_released_real_lsn);
    reader.read(&timestamp);
    if (reader.proto() >=
        Compatibility::GET_SEQ_STATE_REPLY_MESSAGE_SUPPORT_OFFSET_MAP) {
      OffsetMap offsets;
      offsets.deserialize(reader, false /* unused */);
      msg->tail_attributes_.offsets = OffsetMap::toRecord(std::move(offsets));
    } else {
      uint64_t byte_offset = BYTE_OFFSET_INVALID;
      reader.read(&byte_offset);
      msg->tail_attributes_.offsets.setCounter(BYTE_OFFSET, byte_offset);
    }
    msg->tail_attributes_.last_timestamp = std::chrono::milliseconds(timestamp);
  }

  if (header.flags & GET_SEQ_STATE_REPLY_Header::INCLUDES_EPOCH_OFFSET) {
    if (reader.proto() >=
        Compatibility::GET_SEQ_STATE_REPLY_MESSAGE_SUPPORT_OFFSET_MAP) {
      OffsetMap epoch_offsets;
      epoch_offsets.deserialize(reader, false /* unused */);
      msg->epoch_offsets_ = std::move(epoch_offsets);
    } else {
      uint64_t epoch_offset = BYTE_OFFSET_INVALID;
      reader.read(&epoch_offset);
      msg->epoch_offsets_.setCounter(BYTE_OFFSET, epoch_offset);
    }
  }

  if (header.flags & GET_SEQ_STATE_REPLY_Header::INCLUDES_HISTORICAL_METADATA) {
    if (reader.proto() < Compatibility::HISTORICAL_METADATA_IN_GSS_REPLY) {
      RATELIMIT_ERROR(std::chrono::seconds(10),
                      10,
                      "Bad GET_SEQ_STATE_REPLY_Message: "
                      "HISTORICAL_METADATA_IN_GSS_REPLY flag received in "
                      "protocol %d",
                      reader.proto());
      return reader.errorResult(E::BADMSG);
    }

    // TODO(TT15517759): do not need log_id and server config for constructing
    // epoch metadata
    auto server_config = Worker::onThisThread()->getServerConfig();
    msg->metadata_map_ =
        EpochMetaDataMap::deserialize(reader,
                                      /*evbuffer_zero_copy=*/false,
                                      header.log_id,
                                      *server_config);
  }

  if (header.flags & GET_SEQ_STATE_REPLY_Header::INCLUDES_TAIL_RECORD) {
    if (reader.proto() < Compatibility::TAIL_RECORD_IN_GSS_REPLY) {
      RATELIMIT_ERROR(std::chrono::seconds(10),
                      10,
                      "Bad GET_SEQ_STATE_REPLY_Message: "
                      "INCLUDES_TAIL_RECORD flag received in "
                      "protocol %d",
                      reader.proto());
      return reader.errorResult(E::BADMSG);
    }
    msg->tail_record_ = std::make_shared<TailRecord>();
    msg->tail_record_->deserialize(reader, /*zero_copy*/ true);
  }

  return reader.resultMsg(std::move(msg));
}

void GET_SEQ_STATE_REPLY_Message::serialize(ProtocolWriter& writer) const {
  writer.write(&header_, sizeof(header_));
  writer.write(request_id_);
  writer.write(status_);
  writer.write(redirect_);

  if (header_.flags & GET_SEQ_STATE_REPLY_Header::INCLUDES_TAIL_ATTRIBUTES) {
    writer.write(tail_attributes_.last_released_real_lsn);
    uint64_t timestamp = tail_attributes_.last_timestamp.count();
    writer.write(timestamp);
    if (writer.proto() >=
        Compatibility::GET_SEQ_STATE_REPLY_MESSAGE_SUPPORT_OFFSET_MAP) {
      OffsetMap offsets =
          OffsetMap::fromRecord(std::move(tail_attributes_.offsets));
      offsets.serialize(writer);
    } else {
      writer.write(tail_attributes_.offsets.getCounter(BYTE_OFFSET));
    }
  }

  if (header_.flags & GET_SEQ_STATE_REPLY_Header::INCLUDES_EPOCH_OFFSET) {
    if (writer.proto() >=
        Compatibility::GET_SEQ_STATE_REPLY_MESSAGE_SUPPORT_OFFSET_MAP) {
      epoch_offsets_.serialize(writer);
    } else {
      writer.write(epoch_offsets_.getCounter(BYTE_OFFSET));
    }
  }

  if (header_.flags &
      GET_SEQ_STATE_REPLY_Header::INCLUDES_HISTORICAL_METADATA) {
    if (writer.proto() < Compatibility::HISTORICAL_METADATA_IN_GSS_REPLY) {
      RATELIMIT_CRITICAL(std::chrono::seconds(10),
                         10,
                         "Sending GET_SEQ_STATE_REPLY_Message with "
                         "HISTORICAL_METADATA_IN_GSS_REPLY flag in "
                         "protocol %d.",
                         writer.proto());
      // this implies that we are replying to a connection w/ lower protocol
      // version with the new feature, this is not possible as we clear the
      // GET_SEQ_STATE_Message::INCLUDE_HISTORICAL_METADATA flag on receiving
      // the initial GSS request message
      writer.setError(E::BADMSG);
      ld_check(false);
      return;
    }

    ld_check(metadata_map_ != nullptr);
    metadata_map_->serialize(writer);
  }

  if (header_.flags & GET_SEQ_STATE_REPLY_Header::INCLUDES_TAIL_RECORD) {
    if (writer.proto() < Compatibility::TAIL_RECORD_IN_GSS_REPLY) {
      RATELIMIT_CRITICAL(std::chrono::seconds(10),
                         10,
                         "Sending GET_SEQ_STATE_REPLY_Message with "
                         "INCLUDES_TAIL_RECORD flag in protocol %d.",
                         writer.proto());
      // for the same reason above
      writer.setError(E::BADMSG);
      ld_check(false);
      return;
    }

    ld_check(tail_record_ != nullptr);
    tail_record_->serialize(writer);
  }
}

Message::Disposition
GET_SEQ_STATE_REPLY_Message::onReceived(const Address& from) {
  if (from.isClientAddress()) {
    RATELIMIT_ERROR(std::chrono::seconds(5),
                    5,
                    "PROTOCOL ERROR: received a GET_SEQ_STATE_REPLY message "
                    "for log %lu from a client address %s.",
                    header_.log_id.val_,
                    Sender::describeConnection(from).c_str());
    err = E::PROTO;
    return Disposition::ERROR;
  }

  if (status_ == E::REDIRECTED || status_ == E::PREEMPTED) {
    if (!redirect_.isNodeID()) {
      RATELIMIT_ERROR(std::chrono::seconds(1),
                      1,
                      "PROTOCOL ERROR: got GET_SEQ_STATE_REPLY from %s with "
                      "status %s and invalid redirect",
                      Sender::describeConnection(from).c_str(),
                      error_name(status_));
      err = E::PROTO;
      return Disposition::ERROR;
    }
  }

  if (!epoch_valid_or_unset(lsn_to_epoch(header_.last_released_lsn))) {
    // note: last released lsn can be LSN_INVALID if recovery hasn't finished
    RATELIMIT_CRITICAL(std::chrono::seconds(10),
                       10,
                       "Got GET_SEQ_STATE_REPLY message from %s with invalid "
                       "lsn %lu for log %lu!",
                       Sender::describeConnection(from).c_str(),
                       header_.last_released_lsn,
                       header_.log_id.val_);
    err = E::BADMSG;
    return Message::Disposition::ERROR;
  }

  if (!Worker::onThisThread()->isAcceptingWork()) {
    // Ignore the message if shutting down.
    return Disposition::NORMAL;
  }

  auto& requestMap = Worker::onThisThread()->runningGetSeqState();
  auto entry = requestMap.getGssEntryFromRequestId(header_.log_id, request_id_);

  if (entry != nullptr && entry->request != nullptr) {
    entry->request->onReply(from.id_.node_, *this);
  } else {
    ld_debug("Request for log %lu with rqid:%lu is not present in the map",
             header_.log_id.val_,
             request_id_.val());
  }

  return Disposition::NORMAL;
}

std::vector<std::pair<std::string, folly::dynamic>>
GET_SEQ_STATE_REPLY_Message::getDebugInfo() const {
  std::vector<std::pair<std::string, folly::dynamic>> res;

  auto flagsToString = [](GET_SEQ_STATE_REPLY_flags_t flags) {
    folly::small_vector<std::string, 4> strings;
#define FLAG(x)                                \
  if (flags & GET_SEQ_STATE_REPLY_Header::x) { \
    strings.emplace_back(#x);                  \
  }
    FLAG(INCLUDES_TAIL_ATTRIBUTES)
    FLAG(INCLUDES_EPOCH_OFFSET)
    FLAG(INCLUDES_HISTORICAL_METADATA)
    FLAG(INCLUDES_TAIL_RECORD)
#undef FLAG
    return folly::join('|', strings);
  };

  auto add = [&res](const char* key, folly::dynamic val) {
    res.emplace_back(key, std::move(val));
  };
  add("log_id", toString(header_.log_id));
  add("flags", flagsToString(header_.flags));
  add("rqid", request_id_.val());
  add("status", toString(status_));
  add("redirect", toString(redirect_));
  add("last_released_lsn", lsn_to_string(header_.last_released_lsn));
  add("next_lsn", lsn_to_string(header_.next_lsn));
  if (header_.flags & GET_SEQ_STATE_REPLY_Header::INCLUDES_TAIL_ATTRIBUTES) {
    add("tail_attributes", tail_attributes_.toString());
  }
  if (header_.flags & GET_SEQ_STATE_REPLY_Header::INCLUDES_EPOCH_OFFSET) {
    add("epoch_offsets", epoch_offsets_.toString().c_str());
  }
  if (header_.flags &
      GET_SEQ_STATE_REPLY_Header::INCLUDES_HISTORICAL_METADATA) {
    add("metadata_map", metadata_map_ ? metadata_map_->toString() : "null");
  }
  if (header_.flags & GET_SEQ_STATE_REPLY_Header::INCLUDES_TAIL_RECORD) {
    add("tail_record", tail_record_ ? tail_record_->toString() : "null");
  }
  return res;
}

}} // namespace facebook::logdevice
