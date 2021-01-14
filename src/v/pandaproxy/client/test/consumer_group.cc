/*
 * Copyright 2020 Vectorized, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#include "bytes/bytes.h"
#include "kafka/errors.h"
#include "kafka/groups/group.h"
#include "kafka/requests/describe_groups_request.h"
#include "kafka/requests/fetch_request.h"
#include "kafka/requests/find_coordinator_request.h"
#include "kafka/requests/heartbeat_request.h"
#include "kafka/requests/join_group_request.h"
#include "kafka/requests/leave_group_request.h"
#include "kafka/requests/list_groups_request.h"
#include "kafka/requests/list_offsets_request.h"
#include "kafka/requests/metadata_request.h"
#include "kafka/requests/offset_fetch_request.h"
#include "kafka/requests/request_reader.h"
#include "kafka/requests/response_writer.h"
#include "kafka/requests/schemata/join_group_request.h"
#include "kafka/requests/schemata/join_group_response.h"
#include "kafka/requests/schemata/offset_fetch_response.h"
#include "kafka/requests/sync_group_request.h"
#include "kafka/types.h"
#include "model/fundamental.h"
#include "model/metadata.h"
#include "pandaproxy/client/client.h"
#include "pandaproxy/client/configuration.h"
#include "pandaproxy/client/test/pandaproxy_client_fixture.h"
#include "pandaproxy/client/test/utils.h"
#include "redpanda/tests/fixture.h"
#include "utils/unresolved_address.h"
#include "vassert.h"

#include <seastar/core/loop.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/log.hh>
#include <seastar/util/noncopyable_function.hh>

#include <absl/container/flat_hash_map.h>
#include <boost/test/tools/old/interface.hpp>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <memory>
#include <vector>

namespace ppc = pandaproxy::client;

FIXTURE_TEST(pandaproxy_consumer_group, ppc_test_fixture) {
    using namespace std::chrono_literals;

    info("Waiting for leadership");
    wait_for_controller_leadership().get();

    info("Connecting client");
    ppc::shard_local_cfg().retry_base_backoff.set_value(10ms);
    ppc::shard_local_cfg().retries.set_value(size_t(10));
    auto client = make_connected_client();
    client.connect().get();

    info("Adding known topic");
    int partition_count = 3;
    int topic_count = 3;
    std::vector<model::topic_namespace> topics_namespaces;
    topics_namespaces.reserve(topic_count);
    for (int i = 0; i < topic_count; ++i) {
        topics_namespaces.push_back(
          make_data(model::revision_id(2), partition_count, i));
    }

    info("Waiting for topic data");
    for (int t = 0; t < topic_count; ++t) {
        for (int p = 0; p < partition_count; ++p) {
            const auto& tp_ns = topics_namespaces[t];
            wait_for_partition_offset(
              model::ntp(tp_ns.ns, tp_ns.tp, model::partition_id{p}),
              model::offset{0})
              .get();
        }
    }

    kafka::group_id group_id{"test_group_id"};

    static auto find_coordinator_request_builder = [group_id]() mutable {
        return
          [group_id]() { return kafka::find_coordinator_request(group_id); };
    };

    static auto list_groups_request_builder = []() {
        return []() { return kafka::list_groups_request{}; };
    };

    static auto describe_group_request_builder = [group_id]() mutable {
        return [group_id]() {
            kafka::describe_groups_request req;
            req.data.groups.push_back(group_id);
            return req;
        };
    };

    info("Find coordinator for {}", group_id);
    auto find_res = client.dispatch(find_coordinator_request_builder()).get();
    info("Find coordinator res: {}", find_res);
    BOOST_REQUIRE_EQUAL(find_res.data.error_code, kafka::error_code::none);

    info("Waiting for group coordinator");
    kafka::describe_groups_response desc_res{};
    tests::cooperative_spin_wait_with_timeout(
      10s,
      [this, &group_id, &client, &desc_res] {
          return client.dispatch(describe_group_request_builder())
            .then([&desc_res](kafka::describe_groups_response res) {
                desc_res = std::move(res);
                info("Describe group res: {}", desc_res);
                return desc_res.data.groups.size() == 1
                       && desc_res.data.groups[0].error_code
                            != kafka::error_code::not_coordinator;
            });
      })
      .get();

    auto check_group_response = [](
                                  const kafka::describe_groups_response& res,
                                  kafka::group_state state,
                                  size_t size) {
        BOOST_REQUIRE_EQUAL(res.data.groups.size(), 1);
        BOOST_REQUIRE_EQUAL(
          res.data.groups[0].error_code, kafka::error_code::none);
        BOOST_REQUIRE_EQUAL(
          res.data.groups[0].group_state,
          kafka::group_state_to_kafka_name(state));
        BOOST_REQUIRE_EQUAL(res.data.groups[0].members.size(), size);
    };

    BOOST_TEST_CONTEXT("Group not started") {
        check_group_response(desc_res, kafka::group_state::dead, 0);
    }

    std::vector<model::topic> topics;
    topics.reserve(topic_count);
    for (const auto& tp_ns : topics_namespaces) {
        topics.push_back(tp_ns.tp);
    }

    info("Joining Consumers: 0,1");
    std::vector<kafka::member_id> members;
    members.reserve(3);
    {
        auto [mem_0, mem_1] = ss::when_all_succeed(
                                client.create_consumer(group_id),
                                client.create_consumer(group_id))
                                .get();
        members.push_back(mem_0);
        members.push_back(mem_1);
    }
    info("Joined Consumers: 0,1");

    desc_res = client.dispatch(describe_group_request_builder()).get();
    BOOST_TEST_CONTEXT("Group size = 2") {
        check_group_response(desc_res, kafka::group_state::stable, 2);
    }

    ss::when_all_succeed(
      client.subscribe_consumer(group_id, members[0], {topics[0]}),
      client.subscribe_consumer(group_id, members[1], {topics[1]}))
      .get();

    desc_res = client.dispatch(describe_group_request_builder()).get();
    BOOST_TEST_CONTEXT("Group size = 2") {
        check_group_response(desc_res, kafka::group_state::stable, 2);
    }

    info("Joining Consumer 2");
    auto mem_2 = client.create_consumer(group_id).get();
    members.push_back(mem_2);
    client.subscribe_consumer(group_id, mem_2, {topics[2]}).get();
    info("Joined Consumer 2");

    desc_res = client.dispatch(describe_group_request_builder()).get();
    BOOST_TEST_CONTEXT("Group size = 3") {
        check_group_response(desc_res, kafka::group_state::stable, 3);
    }

    auto list_res = client.dispatch(list_groups_request_builder()).get();
    info("list res: {}", list_res);

    desc_res = client.dispatch(describe_group_request_builder()).get();
    info("Describe group res: {}", desc_res);

    ss::when_all_succeed(
      client.remove_consumer(group_id, members[0]),
      client.remove_consumer(group_id, members[1]),
      client.remove_consumer(group_id, members[2]))
      .get();

    client.stop().get();
}