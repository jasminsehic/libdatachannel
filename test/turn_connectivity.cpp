/**
 * Copyright (c) 2019 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtc/rtc.hpp"
#include "test.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

using namespace rtc;
using namespace std;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

TestResult test_turn_connectivity() {
	InitLogger(LogLevel::Debug);

	Configuration config1;
	config1.iceTransportPolicy = TransportPolicy::Relay; // force relay

	// The test runner enables this test only when a local TURN fixture is started.
	char const* turnUrl = std::getenv("RTC_TEST_TURN_URL");
	if (!turnUrl)
		return TestResult(false, "RTC_TEST_TURN_URL is not set");
	config1.iceServers.emplace_back(turnUrl);

	PeerConnection pc1(config1);

	Configuration config2;


	PeerConnection pc2(config2);

	pc1.onStateChange([](PeerConnection::State state) { cout << "State 1: " << state << endl; });

	pc1.onIceStateChange(
	    [](PeerConnection::IceState state) { cout << "ICE state 1: " << state << endl; });

	pc1.onGatheringStateChange([&pc1, &pc2](PeerConnection::GatheringState state) {
		cout << "Gathering state 1: " << state << endl;
		if (state == PeerConnection::GatheringState::Complete) {
			auto sdp = pc1.localDescription().value();
			cout << "Description 1: " << sdp << endl;
			pc2.setRemoteDescription(string(sdp));
		}
	});

	pc1.onSignalingStateChange([](PeerConnection::SignalingState state) {
		cout << "Signaling state 1: " << state << endl;
	});

	pc2.onLocalDescription([&pc1](Description sdp) {
		cout << "Description 2: " << sdp << endl;
		pc1.setRemoteDescription(string(sdp));
	});

	pc2.onLocalCandidate([&pc1](Candidate candidate) {
		// A loopback host candidate is sufficient for the other peer.
		cout << "Candidate 2: " << candidate << endl;
		pc1.addRemoteCandidate(string(candidate));
	});

	pc2.onStateChange([](PeerConnection::State state) { cout << "State 2: " << state << endl; });

	pc2.onIceStateChange(
	    [](PeerConnection::IceState state) { cout << "ICE state 2: " << state << endl; });

	pc2.onGatheringStateChange([](PeerConnection::GatheringState state) {
		cout << "Gathering state 2: " << state << endl;
	});

	pc2.onSignalingStateChange([](PeerConnection::SignalingState state) {
		cout << "Signaling state 2: " << state << endl;
	});

	shared_ptr<DataChannel> dc2;
	pc2.onDataChannel([&dc2](shared_ptr<DataChannel> dc) {
		cout << "DataChannel 2: Received with label \"" << dc->label() << "\"" << endl;
		if (dc->label() != "test") {
			cerr << "Wrong DataChannel label" << endl;
			return;
		}

		dc->onOpen([wdc = make_weak_ptr(dc)]() {
			if (auto dc = wdc.lock()) {
				cout << "DataChannel 2: Open" << endl;
				dc->send("Hello from 2");
			}
		});

		dc->onMessage([](variant<binary, string> message) {
			if (holds_alternative<string>(message)) {
				cout << "Message 2: " << get<string>(message) << endl;
			}
		});

		std::atomic_store(&dc2, dc);
	});

	auto dc1 = pc1.createDataChannel("test");
	dc1->onOpen([wdc1 = make_weak_ptr(dc1)]() {
		auto dc1 = wdc1.lock();
		if (!dc1)
			return;

		cout << "DataChannel 1: Open" << endl;
		dc1->send("Hello from 1");
	});

	dc1->onClosed([]() { cout << "DataChannel 1: Closed" << endl; });

	dc1->onMessage([](const variant<binary, string> &message) {
		if (holds_alternative<string>(message)) {
			cout << "Message 1: " << get<string>(message) << endl;
		}
	});

	// Wait a bit
	int attempts = 10;
	shared_ptr<DataChannel> adc2;
	while ((!(adc2 = std::atomic_load(&dc2)) || !adc2->isOpen() || !dc1->isOpen()) && attempts--)
		this_thread::sleep_for(1s);

	if (pc1.state() != PeerConnection::State::Connected ||
	    pc2.state() != PeerConnection::State::Connected)
		return TestResult(false, "PeerConnection is not connected");

	if ((pc1.iceState() != PeerConnection::IceState::Connected &&
	     pc1.iceState() != PeerConnection::IceState::Completed) ||
	    (pc2.iceState() != PeerConnection::IceState::Connected &&
	     pc2.iceState() != PeerConnection::IceState::Completed))
		return TestResult(false, "ICE is not connected");

	if (!adc2 || !adc2->isOpen() || !dc1->isOpen())
		return TestResult(false, "DataChannel is not open");

	if (auto addr = pc1.localAddress())
		cout << "Local address 1:  " << *addr << endl;
	if (auto addr = pc1.remoteAddress())
		cout << "Remote address 1: " << *addr << endl;
	if (auto addr = pc2.localAddress())
		cout << "Local address 2:  " << *addr << endl;
	if (auto addr = pc2.remoteAddress())
		cout << "Remote address 2: " << *addr << endl;

	Candidate local, remote;
	if (!pc1.getSelectedCandidatePair(&local, &remote))
		return TestResult(false, "getSelectedCandidatePair failed");

	cout << "Local candidate 1:  " << local << endl;
	cout << "Remote candidate 1: " << remote << endl;

	if (local.type() != Candidate::Type::Relayed)
		return TestResult(false, "Connection is not relayed as expected");

	// Try to open a second data channel with another label
	shared_ptr<DataChannel> second2;
	pc2.onDataChannel([&second2](shared_ptr<DataChannel> dc) {
		cout << "Second DataChannel 2: Received with label \"" << dc->label() << "\"" << endl;
		if (dc->label() != "second") {
			cerr << "Wrong second DataChannel label" << endl;
			return;
		}

		dc->onOpen([wdc = make_weak_ptr(dc)]() {
			if (auto dc = wdc.lock())
				dc->send("Second hello from 2");
		});

		dc->onMessage([](variant<binary, string> message) {
			if (holds_alternative<string>(message)) {
				cout << "Second Message 2: " << get<string>(message) << endl;
			}
		});

		std::atomic_store(&second2, dc);
	});

	auto second1 = pc1.createDataChannel("second");

	second1->onOpen([wsecond1 = make_weak_ptr(second1)]() {
		auto second1 = wsecond1.lock();
		if (!second1)
			return;

		cout << "Second DataChannel 1: Open" << endl;
		second1->send("Second hello from 1");
	});

	second1->onClosed([]() { cout << "Second DataChannel 1: Closed" << endl; });

	second1->onMessage([](const variant<binary, string> &message) {
		if (holds_alternative<string>(message)) {
			cout << "Second Message 1: " << get<string>(message) << endl;
		}
	});

	// Wait a bit
	attempts = 10;
	shared_ptr<DataChannel> asecond2;
	while (
	    (!(asecond2 = std::atomic_load(&second2)) || !asecond2->isOpen() || !second1->isOpen()) &&
	    attempts--)
		this_thread::sleep_for(1s);

	if (!asecond2 || !asecond2->isOpen() || !second1->isOpen())
		return TestResult(false, "Second DataChannel is not open");

	// Try to open a negotiated channel
	DataChannelInit init;
	init.negotiated = true;
	init.id = 42;
	auto negotiated1 = pc1.createDataChannel("negotiated", init);
	auto negotiated2 = pc2.createDataChannel("negoctated", init);

	if (!negotiated1->isOpen() || !negotiated2->isOpen())
		return TestResult(false, "Negotiated DataChannel is not open");

	std::atomic<bool> received = false;
	negotiated2->onMessage([&received](const variant<binary, string> &message) {
		if (holds_alternative<string>(message)) {
			cout << "Second Message 2: " << get<string>(message) << endl;
			received = true;
		}
	});

	negotiated1->send("Hello from negotiated channel");

	// Wait a bit
	attempts = 5;
	while (!received && attempts--)
		this_thread::sleep_for(1s);

	if (!received)
		return TestResult(false, "Negotiated DataChannel failed");

	// Delay close of peer 2 to check closing works properly
	pc1.close();
	this_thread::sleep_for(1s);
	pc2.close();
	this_thread::sleep_for(1s);

	return TestResult(true);
}


TestResult test_turn_relay_to_relay() {
	// Both ICE agents must use their own TURN allocations. This exercises
	// relay-to-relay connectivity rather than a relay-to-host candidate pair.
	char const *turnUrl = std::getenv("RTC_TEST_TURN_URL");
	if (!turnUrl)
		return TestResult(false, "RTC_TEST_TURN_URL is not set");

	Configuration config;
	config.iceTransportPolicy = TransportPolicy::Relay;
	config.iceServers.emplace_back(turnUrl);
	PeerConnection pc1(config);
	PeerConnection pc2(config);

	std::atomic<bool> gathered1{false}, gathered2{false};
	std::atomic<bool> leaked1{false}, leaked2{false};
	std::atomic<bool> received1{false}, received2{false};

	pc1.onLocalCandidate([&](Candidate candidate) {
		if (candidate.type() != Candidate::Type::Relayed)
			leaked1 = true;
	});
	pc2.onLocalCandidate([&](Candidate candidate) {
		if (candidate.type() != Candidate::Type::Relayed)
			leaked2 = true;
	});

	// Exchange complete descriptions to avoid depending on trickle timing.
	pc1.onGatheringStateChange([&](PeerConnection::GatheringState state) {
		if (state == PeerConnection::GatheringState::Complete) {
			gathered1 = true;
			if (auto sdp = pc1.localDescription())
				pc2.setRemoteDescription(string(*sdp));
		}
	});
	pc2.onGatheringStateChange([&](PeerConnection::GatheringState state) {
		if (state == PeerConnection::GatheringState::Complete) {
			gathered2 = true;
			if (auto sdp = pc2.localDescription())
				pc1.setRemoteDescription(string(*sdp));
		}
	});

	std::shared_ptr<DataChannel> remoteChannel;
	pc2.onDataChannel([&](std::shared_ptr<DataChannel> dc) {
		dc->onMessage([&, wdc = make_weak_ptr(dc)](const variant<binary, string> &msg) {
			if (holds_alternative<string>(msg) && get<string>(msg) == "relay ping") {
				received2 = true;
				if (auto channel = wdc.lock())
					channel->send("relay pong");
			}
		});
		std::atomic_store(&remoteChannel, dc);
	});

	auto dc1 = pc1.createDataChannel("relay-to-relay");
	dc1->onOpen([wdc1 = make_weak_ptr(dc1)] {
		if (auto channel = wdc1.lock())
			channel->send("relay ping");
	});
	dc1->onMessage([&](const variant<binary, string> &msg) {
		if (holds_alternative<string>(msg) && get<string>(msg) == "relay pong")
			received1 = true;
	});

	for (int i = 0; i < 200 && !(received1 && received2); ++i)
		this_thread::sleep_for(chrono::milliseconds(100));

	auto sdp1 = pc1.localDescription();
	auto sdp2 = pc2.localDescription();
	auto relayedOnly = [](const optional<Description> &sdp) {
		if (!sdp || sdp->candidates().empty())
			return false;
		for (auto const &candidate : sdp->candidates()) {
			if (candidate.type() != Candidate::Type::Relayed)
				return false;
		}
		return true;
	};

	Candidate local1, remote1, local2, remote2;
	bool const pair1 = pc1.getSelectedCandidatePair(&local1, &remote1);
	bool const pair2 = pc2.getSelectedCandidatePair(&local2, &remote2);
	bool const valid = gathered1 && gathered2 && !leaked1 && !leaked2 &&
	                   relayedOnly(sdp1) && relayedOnly(sdp2) &&
	                   pair1 && pair2 &&
	                   local1.type() == Candidate::Type::Relayed &&
	                   local2.type() == Candidate::Type::Relayed &&
	                   received1 && received2;

	pc1.resetCallbacks();
	pc2.resetCallbacks();
	pc1.close();
	pc2.close();

	if (!valid)
		return TestResult(false, "Relay-to-relay ICE, candidates or bidirectional data failed");
	return TestResult(true);
}

TestResult test_turn_invalid_credentials() {
	// An unreachable allocation must not trigger direct host ICE fallback.
	char const *turnUrl = std::getenv("RTC_TEST_TURN_URL");
	if (!turnUrl)
		return TestResult(false, "RTC_TEST_TURN_URL is not set");

	IceServer badServer(turnUrl);
	badServer.password = "invalid-turn-password";

	Configuration relayConfig;
	relayConfig.iceTransportPolicy = TransportPolicy::Relay;
	relayConfig.iceServers.push_back(std::move(badServer));

	PeerConnection relay(relayConfig);
	PeerConnection direct(Configuration{});

	std::atomic<bool> relayGathered{false}, directGathered{false};
	std::atomic<bool> leakedCandidate{false}, connected{false};

	relay.onLocalCandidate([&](Candidate) { leakedCandidate = true; });
	relay.onGatheringStateChange([&](PeerConnection::GatheringState state) {
		if (state == PeerConnection::GatheringState::Complete) {
			relayGathered = true;
			if (auto sdp = relay.localDescription())
				direct.setRemoteDescription(string(*sdp));
		}
	});
	direct.onGatheringStateChange([&](PeerConnection::GatheringState state) {
		if (state == PeerConnection::GatheringState::Complete)
			directGathered = true;
	});
	direct.onLocalCandidate([&](Candidate candidate) {
		relay.addRemoteCandidate(std::move(candidate));
	});
	direct.onLocalDescription([&](Description sdp) {
		relay.setRemoteDescription(std::move(sdp));
	});

	auto onIceState = [&](PeerConnection::IceState state) {
		if (state == PeerConnection::IceState::Connected ||
		    state == PeerConnection::IceState::Completed)
			connected = true;
	};
	relay.onIceStateChange(onIceState);
	direct.onIceStateChange(onIceState);

	auto dc = relay.createDataChannel("invalid-turn-credentials");
	dc->onOpen([&] { connected = true; });

	// Invalid TURN credentials can take several allocation retries. Wait for
	// ICE gathering on both peers, then allow time for direct ICE fallback.
	bool gatheringComplete = false;
	for (int i = 0; i < 450; ++i) {
		if (relayGathered && directGathered) {
			gatheringComplete = true;
			break;
		}
		this_thread::sleep_for(chrono::milliseconds(100));
	}
	if (gatheringComplete)
		this_thread::sleep_for(chrono::seconds(2));

	Candidate local, remote;
	bool const pair = relay.getSelectedCandidatePair(&local, &remote);
	auto const sdp = relay.localDescription();
	bool const advertised = sdp && !sdp->candidates().empty();
	bool const established = connected ||
	    relay.iceState() == PeerConnection::IceState::Connected ||
	    relay.iceState() == PeerConnection::IceState::Completed ||
	    direct.iceState() == PeerConnection::IceState::Connected ||
	    direct.iceState() == PeerConnection::IceState::Completed;

	relay.resetCallbacks();
	direct.resetCallbacks();
	relay.close();
	direct.close();

	if (!gatheringComplete)
		return TestResult(false, "ICE gathering did not complete with invalid TURN credentials");
	if (leakedCandidate || advertised || pair || established)
		return TestResult(false, "Relay-only ICE exposed or selected a direct candidate");
	return TestResult(true);
}
