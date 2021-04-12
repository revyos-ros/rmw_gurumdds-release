// Copyright 2019 GurumNetworks, Inc.
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

#include "rmw/error_handling.h"
#include "rmw/rmw.h"
#include "rmw/types.h"
#include "rmw/impl/cpp/macros.hpp"

#include "rmw_gurumdds_shared_cpp/rmw_common.hpp"
#include "rmw_gurumdds_shared_cpp/types.hpp"

#include "rmw_gurumdds_cpp/identifier.hpp"
#include "rmw_gurumdds_cpp/types.hpp"

#include "./type_support_service.hpp"

extern "C"
{
rmw_ret_t
rmw_send_request(
  const rmw_client_t * client,
  const void * ros_request,
  int64_t * sequence_id)
{
  if (client == nullptr) {
    RMW_SET_ERROR_MSG("client handle is null");
    return RMW_RET_ERROR;
  }

  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    client handle,
    client->implementation_identifier, gurum_gurumdds_identifier,
    return RMW_RET_ERROR)

  if (ros_request == nullptr) {
    RMW_SET_ERROR_MSG("ros request handle is null");
    return RMW_RET_ERROR;
  }

  GurumddsClientInfo * client_info = static_cast<GurumddsClientInfo *>(client->data);
  if (client_info == nullptr) {
    RMW_SET_ERROR_MSG("client info handle is null");
    return RMW_RET_ERROR;
  }

  dds_DataWriter * request_writer = client_info->request_writer;
  if (request_writer == nullptr) {
    RMW_SET_ERROR_MSG("request writer is null");
    return RMW_RET_ERROR;
  }

  auto type_support = client_info->service_typesupport;
  if (type_support == nullptr) {
    RMW_SET_ERROR_MSG("typesupport handle is null");
    return RMW_RET_ERROR;
  }

  size_t size = 0;

  void * dds_request = allocate_request(
    type_support->data,
    type_support->typesupport_identifier,
    ros_request,
    &size
  );

  if (dds_request == nullptr) {
    // Error message already set
    return RMW_RET_ERROR;
  }

  bool res = serialize_request(
    type_support->data,
    type_support->typesupport_identifier,
    ros_request,
    dds_request,
    size,
    ++client_info->sequence_number,
    client_info->writer_guid
  );

  if (!res) {
    RMW_SET_ERROR_MSG("failed to serialize message");
    free(dds_request);
    return RMW_RET_ERROR;
  }

  if (dds_DataWriter_raw_write(request_writer, dds_request, size) != dds_RETCODE_OK) {
    RMW_SET_ERROR_MSG("failed to publish data");
    free(dds_request);
    return RMW_RET_ERROR;
  }

  *sequence_id = client_info->sequence_number;
  free(dds_request);

  return RMW_RET_OK;
}

rmw_ret_t
rmw_take_request(
  const rmw_service_t * service,
  rmw_request_id_t * request_header,
  void * ros_request,
  bool * taken)
{
  if (service == nullptr) {
    RMW_SET_ERROR_MSG("service handle is null");
    return RMW_RET_ERROR;
  }

  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    service handle,
    service->implementation_identifier, gurum_gurumdds_identifier,
    return RMW_RET_ERROR)

  if (ros_request == nullptr) {
    RMW_SET_ERROR_MSG("ros request handle is null");
    return RMW_RET_ERROR;
  }

  if (taken == nullptr) {
    RMW_SET_ERROR_MSG("boolean flag for taken is null");
    return RMW_RET_ERROR;
  }

  *taken = false;

  GurumddsServiceInfo * service_info = static_cast<GurumddsServiceInfo *>(service->data);
  if (service_info == nullptr) {
    RMW_SET_ERROR_MSG("service info handle is null");
    return RMW_RET_ERROR;
  }

  dds_DataReader * request_reader = service_info->request_reader;
  if (request_reader == nullptr) {
    RMW_SET_ERROR_MSG("request reader is null");
    return RMW_RET_ERROR;
  }

  auto type_support = service_info->service_typesupport;
  if (type_support == nullptr) {
    RMW_SET_ERROR_MSG("typesupport handle is null");
    return RMW_RET_ERROR;
  }

  service_info->queue_mutex.lock();
  auto msg = service_info->message_queue.front();
  service_info->message_queue.pop();
  if (service_info->message_queue.empty()) {
    dds_GuardCondition_set_trigger_value(service_info->queue_guard_condition, false);
  }
  service_info->queue_mutex.unlock();

  if (msg.info->valid_data) {
    if (msg.sample == nullptr) {
      RMW_SET_ERROR_MSG("Received invalid message");
      dds_free(msg.info);
      return RMW_RET_ERROR;
    }
    int64_t sequence_number = 0;
    int8_t client_guid[16] = {0};
    bool res = deserialize_request(
      type_support->data,
      type_support->typesupport_identifier,
      ros_request,
      msg.sample,
      static_cast<size_t>(msg.size),
      &sequence_number,
      client_guid
    );

    if (!res) {
      // Error message already set
      dds_free(msg.sample);
      dds_free(msg.info);
      return RMW_RET_ERROR;
    }

    // Sequence number and guid are needed to match responses and requests
    request_header->sequence_number = sequence_number;
    memcpy(request_header->writer_guid, client_guid, 16);

    *taken = true;
  }

  if (msg.sample != nullptr) {
    dds_free(msg.sample);
  }
  if (msg.info != nullptr) {
    dds_free(msg.info);
  }

  return RMW_RET_OK;
}
}  // extern "C"
