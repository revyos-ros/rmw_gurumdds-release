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

#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include "rcutils/logging_macros.h"
#include "rcutils/error_handling.h"

#include "rmw/get_service_names_and_types.h"
#include "rmw/names_and_types.h"
#include "rmw/allocators.h"
#include "rmw/rmw.h"

#include "rmw_gurumdds_shared_cpp/rmw_common.hpp"
#include "rmw_gurumdds_shared_cpp/types.hpp"
#include "rmw_gurumdds_shared_cpp/qos.hpp"
#include "rmw_gurumdds_shared_cpp/namespace_prefix.hpp"

#include "rmw_gurumdds_cpp/identifier.hpp"
#include "rmw_gurumdds_cpp/types.hpp"

#include "./type_support_service.hpp"

extern "C"
{
rmw_service_t *
rmw_create_service(
  const rmw_node_t * node,
  const rosidl_service_type_support_t * type_supports,
  const char * service_name, const rmw_qos_profile_t * qos_policies)
{
  if (node == nullptr) {
    RMW_SET_ERROR_MSG("node handle is null");
    return nullptr;
  }

  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node handle, node->implementation_identifier,
    gurum_gurumdds_identifier, return nullptr)

  if (service_name == nullptr || strlen(service_name) == 0) {
    RMW_SET_ERROR_MSG("service topic is null or empty string");
    return nullptr;
  }

  if (qos_policies == nullptr) {
    RMW_SET_ERROR_MSG("qos_profile is null");
    return nullptr;
  }

  GurumddsNodeInfo * node_info = static_cast<GurumddsNodeInfo *>(node->data);
  if (node_info == nullptr) {
    RMW_SET_ERROR_MSG("node info handle is null");
    return nullptr;
  }

  dds_DomainParticipant * participant = node_info->participant;
  if (participant == nullptr) {
    RMW_SET_ERROR_MSG("participant handle is null");
    return nullptr;
  }

  const rosidl_service_type_support_t * type_support =
    get_service_typesupport_handle(type_supports, rosidl_typesupport_introspection_c__identifier);
  if (type_support == nullptr) {
    rcutils_reset_error();
    type_support = get_service_typesupport_handle(
      type_supports, rosidl_typesupport_introspection_cpp::typesupport_identifier);
    if (type_support == nullptr) {
      rcutils_reset_error();
      RMW_SET_ERROR_MSG("type support not from this implementation");
      return nullptr;
    }
  }

  GurumddsServiceInfo * service_info = nullptr;
  rmw_service_t * rmw_service = nullptr;

  dds_SubscriberQos subscriber_qos;
  dds_PublisherQos publisher_qos;
  dds_DataReaderQos datareader_qos;
  dds_DataWriterQos datawriter_qos;

  dds_Subscriber * dds_subscriber = nullptr;
  dds_Publisher * dds_publisher = nullptr;
  dds_DataReader * request_reader = nullptr;
  dds_DataWriter * response_writer = nullptr;
  dds_ReadCondition * read_condition = nullptr;
  dds_TypeSupport * request_typesupport = nullptr;
  dds_TypeSupport * response_typesupport = nullptr;

  dds_TopicDescription * topic_desc = nullptr;
  dds_Topic * request_topic = nullptr;
  dds_Topic * response_topic = nullptr;

  dds_ReturnCode_t ret;
  rmw_ret_t rmw_ret;

  std::pair<std::string, std::string> service_type_name;
  std::pair<std::string, std::string> service_metastring;
  std::string request_topic_name;
  std::string response_topic_name;
  std::string request_type_name;
  std::string response_type_name;
  std::string request_metastring;
  std::string response_metastring;

  // Create topic and type name strings
  service_type_name =
    create_service_type_name(type_support->data, type_support->typesupport_identifier);
  request_type_name = service_type_name.first;
  response_type_name = service_type_name.second;
  if (request_type_name.empty() || response_type_name.empty()) {
    RMW_SET_ERROR_MSG("failed to create type name");
    return nullptr;
  }

  request_topic_name.reserve(256);
  response_topic_name.reserve(256);

  if (!qos_policies->avoid_ros_namespace_conventions) {
    request_topic_name += ros_service_requester_prefix;
    response_topic_name += ros_service_response_prefix;
  }

  request_topic_name += service_name;
  response_topic_name += service_name;
  request_topic_name += "Request";
  response_topic_name += "Reply";

  service_metastring =
    create_service_metastring(type_support->data, type_support->typesupport_identifier);
  request_metastring = service_metastring.first;
  response_metastring = service_metastring.second;
  if (request_metastring.empty() || response_metastring.empty()) {
    RMW_SET_ERROR_MSG("failed to create metastring");
    return nullptr;
  }

  // Set infos for this service(server)
  service_info = new(std::nothrow) GurumddsServiceInfo();
  if (service_info == nullptr) {
    RMW_SET_ERROR_MSG("failed to allocate GurumddsServiceInfo");
    goto fail;
  }

  service_info->participant = participant;
  service_info->implementation_identifier = gurum_gurumdds_identifier;
  service_info->service_typesupport = type_support;

  request_typesupport = dds_TypeSupport_create(request_metastring.c_str());
  if (request_typesupport == nullptr) {
    RMW_SET_ERROR_MSG("failed to create typesupport");
    goto fail;
  }

  ret =
    dds_TypeSupport_register_type(request_typesupport, participant, request_type_name.c_str());
  if (ret != dds_RETCODE_OK) {
    RMW_SET_ERROR_MSG("failed to register type");
    goto fail;
  }

  response_typesupport = dds_TypeSupport_create(response_metastring.c_str());
  if (response_typesupport == nullptr) {
    RMW_SET_ERROR_MSG("failed to create typesupport");
    goto fail;
  }

  ret =
    dds_TypeSupport_register_type(response_typesupport, participant, response_type_name.c_str());
  if (ret != dds_RETCODE_OK) {
    RMW_SET_ERROR_MSG("failed to register type");
    goto fail;
  }

  // Create topics

  // Look for request topic
  topic_desc =
    dds_DomainParticipant_lookup_topicdescription(participant, request_topic_name.c_str());
  if (topic_desc == nullptr) {
    dds_TopicQos topic_qos;
    ret = dds_DomainParticipant_get_default_topic_qos(participant, &topic_qos);
    if (ret != dds_RETCODE_OK) {
      RMW_SET_ERROR_MSG("failed to get default topic qos");
      goto fail;
    }

    request_topic = dds_DomainParticipant_create_topic(
      participant, request_topic_name.c_str(), request_type_name.c_str(), &topic_qos, nullptr, 0);
    if (request_topic == nullptr) {
      RMW_SET_ERROR_MSG("failed to create topic");
      dds_TopicQos_finalize(&topic_qos);
      goto fail;
    }

    ret = dds_TopicQos_finalize(&topic_qos);
    if (ret != dds_RETCODE_OK) {
      RMW_SET_ERROR_MSG("failed to finalize topic qos");
      goto fail;
    }
  } else {
    dds_Duration_t timeout;
    timeout.sec = 0;
    timeout.nanosec = 1;
    request_topic = dds_DomainParticipant_find_topic(
      participant, request_topic_name.c_str(), &timeout);
    if (request_topic == nullptr) {
      RMW_SET_ERROR_MSG("failed to find topic");
      goto fail;
    }
  }

  // Look for response topic
  topic_desc =
    dds_DomainParticipant_lookup_topicdescription(participant, response_topic_name.c_str());
  if (topic_desc == nullptr) {
    dds_TopicQos topic_qos;
    ret = dds_DomainParticipant_get_default_topic_qos(participant, &topic_qos);
    if (ret != dds_RETCODE_OK) {
      RMW_SET_ERROR_MSG("failed to get default topic qos");
      goto fail;
    }

    response_topic = dds_DomainParticipant_create_topic(
      participant, response_topic_name.c_str(), response_type_name.c_str(), &topic_qos, nullptr, 0);
    if (response_topic == nullptr) {
      RMW_SET_ERROR_MSG("failed to create topic");
      dds_TopicQos_finalize(&topic_qos);
      goto fail;
    }

    ret = dds_TopicQos_finalize(&topic_qos);
    if (ret != dds_RETCODE_OK) {
      RMW_SET_ERROR_MSG("failed to finalize topic qos");
      goto fail;
    }
  } else {
    dds_Duration_t timeout;
    timeout.sec = 0;
    timeout.nanosec = 1;
    response_topic =
      dds_DomainParticipant_find_topic(participant, response_topic_name.c_str(), &timeout);
    if (response_topic == nullptr) {
      RMW_SET_ERROR_MSG("failed to find topic");
      goto fail;
    }
  }

  // Create datareader for request
  ret = dds_DomainParticipant_get_default_subscriber_qos(participant, &subscriber_qos);
  if (ret != dds_RETCODE_OK) {
    RMW_SET_ERROR_MSG("failed to get default subscriber qos");
    goto fail;
  }

  dds_subscriber =
    dds_DomainParticipant_create_subscriber(participant, &subscriber_qos, nullptr, 0);
  if (dds_subscriber == nullptr) {
    RMW_SET_ERROR_MSG("failed to create subscriber");
    dds_SubscriberQos_finalize(&subscriber_qos);
    goto fail;
  }
  service_info->dds_subscriber = dds_subscriber;

  ret = dds_SubscriberQos_finalize(&subscriber_qos);
  if (ret != dds_RETCODE_OK) {
    RMW_SET_ERROR_MSG("failed to finalize subscriber qos");
    goto fail;
  }

  if (!get_datareader_qos(dds_subscriber, qos_policies, &datareader_qos)) {
    // Error message already set
    goto fail;
  }

  request_reader = dds_Subscriber_create_datareader(
    dds_subscriber, request_topic, &datareader_qos, nullptr, 0);
  if (request_reader == nullptr) {
    RMW_SET_ERROR_MSG("failed to create datareader");
    dds_DataReaderQos_finalize(&datareader_qos);
    goto fail;
  }
  service_info->request_reader = request_reader;

  ret = dds_DataReaderQos_finalize(&datareader_qos);
  if (ret != dds_RETCODE_OK) {
    RMW_SET_ERROR_MSG("failed to finalize datareader qos");
    goto fail;
  }

  read_condition = dds_DataReader_create_readcondition(
    request_reader, dds_ANY_SAMPLE_STATE, dds_ANY_VIEW_STATE, dds_ANY_INSTANCE_STATE);
  if (read_condition == nullptr) {
    RMW_SET_ERROR_MSG("failed to create read condition");
    goto fail;
  }
  service_info->read_condition = read_condition;

  // Create datawriter for response
  ret = dds_DomainParticipant_get_default_publisher_qos(participant, &publisher_qos);
  if (ret != dds_RETCODE_OK) {
    RMW_SET_ERROR_MSG("failed to get default publisher qos");
    goto fail;
  }

  dds_publisher = dds_DomainParticipant_create_publisher(participant, &publisher_qos, nullptr, 0);
  if (dds_publisher == nullptr) {
    RMW_SET_ERROR_MSG("failed to create subscriber");
    dds_PublisherQos_finalize(&publisher_qos);
    goto fail;
  }
  service_info->dds_publisher = dds_publisher;

  ret = dds_PublisherQos_finalize(&publisher_qos);
  if (ret != dds_RETCODE_OK) {
    RMW_SET_ERROR_MSG("failed to finalize publisher qos");
    goto fail;
  }

  if (!get_datawriter_qos(dds_publisher, qos_policies, &datawriter_qos)) {
    // Error message already set
    goto fail;
  }

  response_writer = dds_Publisher_create_datawriter(
    dds_publisher, response_topic, &datawriter_qos, nullptr, 0);
  if (response_writer == nullptr) {
    RMW_SET_ERROR_MSG("failed to create datawriter");
    dds_DataWriterQos_finalize(&datawriter_qos);
    goto fail;
  }
  service_info->response_writer = response_writer;

  ret = dds_DataWriterQos_finalize(&datawriter_qos);
  if (ret != dds_RETCODE_OK) {
    RMW_SET_ERROR_MSG("failed to finalize datawriter qos");
    goto fail;
  }
  rmw_service = rmw_service_allocate();
  if (rmw_service == nullptr) {
    RMW_SET_ERROR_MSG("failed to allocate memory for service");
    goto fail;
  }
  memset(rmw_service, 0, sizeof(rmw_service_t));
  rmw_service->implementation_identifier = gurum_gurumdds_identifier;
  rmw_service->data = service_info;
  rmw_service->service_name =
    reinterpret_cast<const char *>(rmw_allocate(strlen(service_name) + 1));
  if (rmw_service->service_name == nullptr) {
    RMW_SET_ERROR_MSG("failed to allocate memory for service name");
    goto fail;
  }
  memcpy(const_cast<char *>(rmw_service->service_name), service_name, strlen(service_name) + 1);

  rmw_ret = rmw_trigger_guard_condition(node_info->graph_guard_condition);
  if (rmw_ret != RMW_RET_OK) {
    // Error message already set
    goto fail;
  }

  dds_TypeSupport_delete(request_typesupport);
  request_typesupport = nullptr;
  dds_TypeSupport_delete(response_typesupport);
  response_typesupport = nullptr;

  std::this_thread::sleep_for(std::chrono::milliseconds(5));

  RCUTILS_LOG_DEBUG_NAMED(
    "rmw_gurumdds_cpp",
    "Created server with service '%s' on node '%s%s%s'",
    service_name, node->namespace_,
    node->namespace_[strlen(node->namespace_) - 1] == '/' ? "" : "/", node->name);

  return rmw_service;

fail:
  if (rmw_service != nullptr) {
    rmw_service_free(rmw_service);
  }

  if (dds_subscriber != nullptr) {
    if (request_reader != nullptr) {
      if (read_condition != nullptr) {
        dds_DataReader_delete_readcondition(request_reader, read_condition);
      }
      dds_Subscriber_delete_datareader(dds_subscriber, request_reader);
    }
    dds_DomainParticipant_delete_subscriber(participant, dds_subscriber);
  }

  if (dds_publisher != nullptr) {
    if (response_writer != nullptr) {
      dds_Publisher_delete_datawriter(dds_publisher, response_writer);
    }
    dds_DomainParticipant_delete_publisher(participant, dds_publisher);
  }

  if (request_topic != nullptr) {
    dds_DomainParticipant_delete_topic(participant, request_topic);
  }

  if (response_topic != nullptr) {
    dds_DomainParticipant_delete_topic(participant, response_topic);
  }

  if (request_typesupport != nullptr) {
    dds_TypeSupport_delete(request_typesupport);
  }

  if (response_typesupport != nullptr) {
    dds_TypeSupport_delete(response_typesupport);
  }

  if (service_info != nullptr) {
    delete service_info;
  }
  return nullptr;
}

rmw_ret_t
rmw_destroy_service(rmw_node_t * node, rmw_service_t * service)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node handle,
    node->implementation_identifier,
    gurum_gurumdds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  RMW_CHECK_ARGUMENT_FOR_NULL(service, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    service handle,
    service->implementation_identifier,
    gurum_gurumdds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  GurumddsNodeInfo * node_info = static_cast<GurumddsNodeInfo *>(node->data);
  if (node_info == nullptr) {
    RMW_SET_ERROR_MSG("node info handle is null");
    return RMW_RET_ERROR;
  }

  dds_ReturnCode_t ret;
  GurumddsServiceInfo * service_info = static_cast<GurumddsServiceInfo *>(service->data);

  if (service_info != nullptr) {
    if (service_info->participant != nullptr) {
      if (service_info->dds_subscriber != nullptr) {
        if (service_info->request_reader != nullptr) {
          if (service_info->read_condition != nullptr) {
            ret = dds_DataReader_delete_readcondition(
              service_info->request_reader, service_info->read_condition);
            if (ret != dds_RETCODE_OK) {
              RMW_SET_ERROR_MSG("failed to delete readcondition");
              return RMW_RET_ERROR;
            }
          }
          ret = dds_Subscriber_delete_datareader(
            service_info->dds_subscriber, service_info->request_reader);
          if (ret != dds_RETCODE_OK) {
            RMW_SET_ERROR_MSG("failed to delete datareader");
            return RMW_RET_ERROR;
          }
        } else if (service_info->read_condition != nullptr) {
          RMW_SET_ERROR_MSG("cannot delete readcondition because the datareader is null");
          return RMW_RET_ERROR;
        }
        ret = dds_DomainParticipant_delete_subscriber(
          service_info->participant, service_info->dds_subscriber);
        if (ret != dds_RETCODE_OK) {
          RMW_SET_ERROR_MSG("failed to delete subscriber");
          return RMW_RET_ERROR;
        }
      } else if (service_info->request_reader != nullptr) {
        RMW_SET_ERROR_MSG("cannot delete datareader because the subscriber is null");
        return RMW_RET_ERROR;
      }

      if (service_info->dds_publisher != nullptr) {
        if (service_info->response_writer != nullptr) {
          ret = dds_Publisher_delete_datawriter(
            service_info->dds_publisher, service_info->response_writer);
          if (ret != dds_RETCODE_OK) {
            RMW_SET_ERROR_MSG("failed to delete datawriter");
            return RMW_RET_ERROR;
          }
        }
        ret = dds_DomainParticipant_delete_publisher(
          service_info->participant, service_info->dds_publisher);
        if (ret != dds_RETCODE_OK) {
          RMW_SET_ERROR_MSG("failed to delete publisher");
          return RMW_RET_ERROR;
        }
      } else if (service_info->response_writer != nullptr) {
        RMW_SET_ERROR_MSG("cannot delete datawriter because the publisher is null");
        return RMW_RET_ERROR;
      }

    } else if (service_info->dds_subscriber != nullptr || service_info->dds_publisher != nullptr) {
      RMW_SET_ERROR_MSG(
        "cannot delete publisher and subscriber because the domain participant is null");
      return RMW_RET_ERROR;
    }

    delete service_info;
    service->data = nullptr;
    if (service->service_name != nullptr) {
      RCUTILS_LOG_DEBUG_NAMED(
        "rmw_gurumdds_cpp",
        "Deleted server with service '%s' on node '%s%s%s'",
        service->service_name, node->namespace_,
        node->namespace_[strlen(node->namespace_) - 1] == '/' ? "" : "/", node->name);

      rmw_free(const_cast<char *>(service->service_name));
    }
  }

  rmw_service_free(service);

  rmw_ret_t rmw_ret = rmw_trigger_guard_condition(node_info->graph_guard_condition);

  return rmw_ret;
}
}  // extern "C"
