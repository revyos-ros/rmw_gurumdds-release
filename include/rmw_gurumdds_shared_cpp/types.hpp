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

#ifndef RMW_GURUMDDS_SHARED_CPP__TYPES_HPP_
#define RMW_GURUMDDS_SHARED_CPP__TYPES_HPP_

#include <cassert>
#include <exception>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <atomic>

#include "rmw/rmw.h"
#include "rmw/ret_types.h"

#include "rmw_gurumdds_shared_cpp/dds_include.hpp"
#include "rmw_gurumdds_shared_cpp/guid.hpp"
#include "rmw_gurumdds_shared_cpp/qos.hpp"
#include "rmw_gurumdds_shared_cpp/rmw_common.hpp"
#include "rmw_gurumdds_shared_cpp/topic_cache.hpp"
#include "rmw_gurumdds_shared_cpp/visibility_control.h"

enum EntityType {Publisher, Subscriber};

typedef struct _ListenerContext
{
  std::mutex * mutex_;
  TopicCache<GuidPrefix_t> * topic_cache;
  rmw_guard_condition_t * graph_guard_condition;
  const char * implementation_identifier;
} ListenerContext;

static void pub_on_data_available(const dds_DataReader * a_reader)
{
  dds_DataReader * reader = const_cast<dds_DataReader *>(a_reader);
  ListenerContext * context =
    reinterpret_cast<ListenerContext *>(dds_DataReader_get_listener_context(reader));
  if (context == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(*context->mutex_);
  dds_DataSeq * samples = dds_DataSeq_create(8);
  if (samples == nullptr) {
    fprintf(stderr, "failed to create data sample sequence\n");
    return;
  }
  dds_SampleInfoSeq * infos = dds_SampleInfoSeq_create(8);
  if (infos == nullptr) {
    dds_DataSeq_delete(samples);
    fprintf(stderr, "failed to create sample info sequence\n");
    return;
  }

  dds_ReturnCode_t ret = dds_DataReader_take(
    reader, samples, infos, dds_LENGTH_UNLIMITED,
    dds_ANY_SAMPLE_STATE, dds_ANY_VIEW_STATE, dds_ANY_INSTANCE_STATE);
  if (ret == dds_RETCODE_NO_DATA) {
    dds_DataReader_return_loan(reader, samples, infos);
    dds_DataSeq_delete(samples);
    dds_SampleInfoSeq_delete(infos);
    return;
  }
  if (ret != dds_RETCODE_OK) {
    fprintf(stderr, "failed to access data from the built-in reader\n");
    dds_DataReader_return_loan(reader, samples, infos);
    dds_DataSeq_delete(samples);
    dds_SampleInfoSeq_delete(infos);
    return;
  }

  for (dds_UnsignedLong i = 0; i < dds_DataSeq_length(samples); ++i) {
    std::string topic_name, type_name;
    GuidPrefix_t guid, participant_guid;
    dds_PublicationBuiltinTopicData * pbtd =
      reinterpret_cast<dds_PublicationBuiltinTopicData *>(dds_DataSeq_get(samples, i));
    dds_SampleInfo * info = dds_SampleInfoSeq_get(infos, i);
    if (reinterpret_cast<void *>(info->instance_handle) == NULL) {
      continue;
    }
    memcpy(guid.value, reinterpret_cast<void *>(info->instance_handle), 16);
    if (info->valid_data && info->instance_state == dds_ALIVE_INSTANCE_STATE) {
      dds_BuiltinTopicKey_to_GUID(&participant_guid, pbtd->participant_key);
      topic_name = std::string(pbtd->topic_name);
      type_name = std::string(pbtd->type_name);
      rmw_qos_profile_t qos = {
        RMW_QOS_POLICY_HISTORY_UNKNOWN,  // TODO(clemjh): pbtd doesn't contain history qos policy
        RMW_QOS_POLICY_DEPTH_SYSTEM_DEFAULT,
        convert_reliability(pbtd->reliability),
        convert_durability(pbtd->durability),
        convert_deadline(pbtd->deadline),
        convert_lifespan(pbtd->lifespan),
        convert_liveliness(pbtd->liveliness),
        convert_liveliness_lease_duration(pbtd->liveliness),
        false,
      };
      context->topic_cache->add_topic(participant_guid, guid, topic_name, type_name, qos);
    } else {
      context->topic_cache->remove_topic(guid);
    }
  }

  if (dds_DataSeq_length(samples) > 0) {
    rmw_ret_t rmw_ret = shared__rmw_trigger_guard_condition(
      context->implementation_identifier, context->graph_guard_condition);
    if (rmw_ret != RMW_RET_OK) {
      fprintf(stderr, "failed to trigger graph guard condition: %s\n", rmw_get_error_string().str);
    }
  }

  dds_DataReader_return_loan(reader, samples, infos);

  dds_DataSeq_delete(samples);
  dds_SampleInfoSeq_delete(infos);

  dds_DataReader_set_listener_context(reader, context);
}

static void sub_on_data_available(const dds_DataReader * a_reader)
{
  dds_DataReader * reader = const_cast<dds_DataReader *>(a_reader);
  ListenerContext * context =
    reinterpret_cast<ListenerContext *>(dds_DataReader_get_listener_context(reader));
  if (context == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(*context->mutex_);
  dds_DataSeq * samples = dds_DataSeq_create(8);
  if (samples == nullptr) {
    fprintf(stderr, "failed to create data sample sequence\n");
    return;
  }
  dds_SampleInfoSeq * infos = dds_SampleInfoSeq_create(8);
  if (infos == nullptr) {
    dds_DataSeq_delete(samples);
    fprintf(stderr, "failed to create sample info sequence\n");
    return;
  }

  dds_ReturnCode_t ret = dds_DataReader_take(
    reader, samples, infos, dds_LENGTH_UNLIMITED,
    dds_ANY_SAMPLE_STATE, dds_ANY_VIEW_STATE, dds_ANY_INSTANCE_STATE);
  if (ret == dds_RETCODE_NO_DATA) {
    dds_DataReader_return_loan(reader, samples, infos);
    dds_DataSeq_delete(samples);
    dds_SampleInfoSeq_delete(infos);
    return;
  }
  if (ret != dds_RETCODE_OK) {
    fprintf(stderr, "failed to access data from the built-in reader\n");
    dds_DataReader_return_loan(reader, samples, infos);
    dds_DataSeq_delete(samples);
    dds_SampleInfoSeq_delete(infos);
    return;
  }

  for (dds_UnsignedLong i = 0; i < dds_DataSeq_length(samples); ++i) {
    std::string topic_name, type_name;
    GuidPrefix_t guid, participant_guid;
    dds_SubscriptionBuiltinTopicData * sbtd =
      reinterpret_cast<dds_SubscriptionBuiltinTopicData *>(dds_DataSeq_get(samples, i));
    dds_SampleInfo * info = dds_SampleInfoSeq_get(infos, i);
    if (reinterpret_cast<void *>(info->instance_handle) == NULL) {
      continue;
    }
    memcpy(guid.value, reinterpret_cast<void *>(info->instance_handle), 16);
    if (info->valid_data && info->instance_state == dds_ALIVE_INSTANCE_STATE) {
      dds_BuiltinTopicKey_to_GUID(&participant_guid, sbtd->participant_key);
      topic_name = sbtd->topic_name;
      type_name = sbtd->type_name;
      rmw_qos_profile_t qos = {
        RMW_QOS_POLICY_HISTORY_UNKNOWN,  // TODO(clemjh): sbtd doesn't contain history qos policy
        RMW_QOS_POLICY_DEPTH_SYSTEM_DEFAULT,
        convert_reliability(sbtd->reliability),
        convert_durability(sbtd->durability),
        convert_deadline(sbtd->deadline),
        RMW_QOS_LIFESPAN_DEFAULT,
        convert_liveliness(sbtd->liveliness),
        convert_liveliness_lease_duration(sbtd->liveliness),
        false,
      };
      context->topic_cache->add_topic(participant_guid, guid, std::move(topic_name), std::move(type_name), qos);
    } else {
      context->topic_cache->remove_topic(guid);
    }
  }

  if (dds_DataSeq_length(samples) > 0) {
    rmw_ret_t rmw_ret = shared__rmw_trigger_guard_condition(
      context->implementation_identifier, context->graph_guard_condition);
    if (rmw_ret != RMW_RET_OK) {
      fprintf(stderr, "failed to trigger graph guard condition: %s\n", rmw_get_error_string().str);
    }
  }

  dds_DataReader_return_loan(reader, samples, infos);

  dds_DataSeq_delete(samples);
  dds_SampleInfoSeq_delete(infos);

  dds_DataReader_set_listener_context(reader, context);
}

class GurumddsDataReaderListener
{
public:
  explicit GurumddsDataReaderListener(
    const char * implementation_identifier, rmw_guard_condition_t * graph_guard_condition)
  : graph_guard_condition(graph_guard_condition),
    implementation_identifier(implementation_identifier)
  {}

  virtual ~GurumddsDataReaderListener() = default;

  RMW_GURUMDDS_SHARED_CPP_PUBLIC
  virtual void add_information(
    const GuidPrefix_t & participant_guid,
    const GuidPrefix_t & topic_guid,
    const std::string & topic_name,
    const std::string & type_name,
    rmw_qos_profile_t & qos,
    EntityType entity_type);

  RMW_GURUMDDS_SHARED_CPP_PUBLIC
  virtual void remove_information(
    const GuidPrefix_t & topic_guid,
    const EntityType entity_type);

  RMW_GURUMDDS_SHARED_CPP_PUBLIC
  virtual void trigger_graph_guard_condition(void);

  size_t count_topic(const char * topic_name);

  void fill_topic_names_and_types(
    bool no_demangle,
    std::map<std::string, std::set<std::string>> & topic_names_to_types);

  void fill_service_names_and_types(
    std::map<std::string, std::set<std::string>> & services);

  void fill_topic_names_and_types_by_guid(
    bool no_demangle,
    std::map<std::string, std::set<std::string>> & topic_names_to_types_by_guid,
    GuidPrefix_t & participant_guid);

  void fill_service_names_and_types_by_guid(
    std::map<std::string, std::set<std::string>> & services,
    GuidPrefix_t & participant_guid,
    const std::string suffix);

  dds_DataReaderListener dds_listener;
  ListenerContext context;
  dds_DataReader * dds_reader;

  std::mutex mutex_;
  TopicCache<GuidPrefix_t> topic_cache;
  rmw_guard_condition_t * graph_guard_condition;

  const char * implementation_identifier;

protected:
private:
};

class GurumddsPublisherListener : public GurumddsDataReaderListener
{
public:
  GurumddsPublisherListener(
    const char * implementation_identifier, rmw_guard_condition_t * graph_guard_condition)
  : GurumddsDataReaderListener(implementation_identifier, graph_guard_condition)
  {
    context.mutex_ = &(this->mutex_);
    context.topic_cache = &(this->topic_cache);
    context.graph_guard_condition = this->graph_guard_condition;
    context.implementation_identifier = this->implementation_identifier;
    dds_listener.on_data_available = pub_on_data_available;
  }

  ~GurumddsPublisherListener() {}
};

class GurumddsSubscriberListener : public GurumddsDataReaderListener
{
public:
  GurumddsSubscriberListener(
    const char * implementation_identifier, rmw_guard_condition_t * graph_guard_condition)
  : GurumddsDataReaderListener(implementation_identifier, graph_guard_condition)
  {
    context.mutex_ = &(this->mutex_);
    context.topic_cache = &(this->topic_cache);
    context.graph_guard_condition = this->graph_guard_condition;
    context.implementation_identifier = this->implementation_identifier;
    dds_listener.on_data_available = sub_on_data_available;
  }

  ~GurumddsSubscriberListener() {}
};

typedef struct _GurumddsNodeInfo
{
  dds_DomainParticipant * participant;
  rmw_guard_condition_t * graph_guard_condition;
  GurumddsPublisherListener * pub_listener;
  GurumddsSubscriberListener * sub_listener;
} GurumddsNodeInfo;

typedef struct _GurumddsWaitSetInfo
{
  dds_WaitSet * wait_set;
  dds_ConditionSeq * active_conditions;
  dds_ConditionSeq * attached_conditions;
} GurumddsWaitSetInfo;

typedef struct _GurumddsEventInfo
{
  virtual ~_GurumddsEventInfo() = default;
  virtual rmw_ret_t get_status(const dds_StatusMask mask, void * event) = 0;
  virtual dds_StatusCondition * get_statuscondition() = 0;
  virtual dds_StatusMask get_status_changes() = 0;
} GurumddsEventInfo;

#endif  // RMW_GURUMDDS_SHARED_CPP__TYPES_HPP_
