// SPDX-License-Identifier: GPL-3.0-or-later
// Asset-free checks of the public packet replay, town confirmation and entry callback APIs.
#include "net.hpp"
#include <thread>

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); } } while (0)

static void U16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value)); out.push_back(static_cast<uint8_t>(value >> 8));
}
static void U32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
static void Text(std::vector<uint8_t>& out, const std::string& value) {
    out.insert(out.end(), value.begin(), value.end()); out.push_back(0);
}
static void Feed(net::Client& client, const std::vector<uint8_t>& packet) {
    client.Feed(packet.data(), static_cast<int>(packet.size()));
}
static void Object(net::Client& client, uint8_t kind, uint16_t id, const std::vector<uint8_t>& payload, bool removed = false) {
    static uint16_t sequence = 1;
    std::vector<uint8_t> packet{0x00};
    U16(packet, sequence++); U16(packet, 0); U32(packet, 0); U16(packet, 0);
    packet.push_back(0); // no stream chunk
    packet.push_back(1); // one reliable object
    packet.push_back(removed ? 0 : 1);
    packet.push_back(kind); U16(packet, id);
    packet.insert(packet.end(), payload.begin(), payload.end());
    Feed(client, packet);
}
static void Player(net::Client& client, uint16_t id, const char* name, uint32_t state, bool host = false) {
    std::vector<uint8_t> payload;
    Text(payload, name); Text(payload, "hero"); U32(payload, state);
    payload.push_back(host ? 1 : 0); U32(payload, 0); U32(payload, 0);
    Object(client, 1, id, payload);
}
static void Accepted(net::Client& client) { Feed(client, {0x06, 1, 0, 0, 0}); }
static void Connect(net::Client& client) { CHECK(client.Connect("127.0.0.1", 9)); }

static size_t ReturnStatus(const net::Client& client, const char* status) {
    const std::string prefix = std::string("host return: ") + status;
    return static_cast<size_t>(std::count_if(client.log.begin(), client.log.end(),
        [&](const std::string& line) { return line.compare(0, prefix.size(), prefix) == 0; }));
}

static void StartReturn(net::Client& client) {
    client.SetCharacter("bot", "hero", 0, {0});
    Connect(client); Accepted(client); Player(client, 4, "bot", 1);
    std::vector<uint8_t> chosen; Text(chosen, "testquest"); Object(client, 9, 10, chosen);
    std::vector<uint8_t> open; Text(open, "testquest_2"); Object(client, 8, 11, open);
    client.Update(); // Lobby -> Loading
    const char* delay = std::getenv("UM_BOT_ENTRYDELAY");
    const bool hadDelay = delay != nullptr;
    const std::string oldDelay = delay ? delay : "";
#ifdef _WIN32
    _putenv_s("UM_BOT_ENTRYDELAY", "0");
#else
    setenv("UM_BOT_ENTRYDELAY", "0", 1);
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    client.Update(); // Loading -> Entering, using the existing research override
#ifdef _WIN32
    _putenv_s("UM_BOT_ENTRYDELAY", hadDelay ? oldDelay.c_str() : "");
#else
    if (hadDelay) setenv("UM_BOT_ENTRYDELAY", oldDelay.c_str(), 1); else unsetenv("UM_BOT_ENTRYDELAY");
#endif
    // A server snapshot carrying a new hero id drives the real stream decoder into InWorld.
    const uint32_t hero = 1000000013u;
    std::vector<uint8_t> snapshot(80, 0);
    std::memcpy(snapshot.data(), &hero, sizeof hero);
    const auto packed = mp::Pack(snapshot, 0, 0);
    std::vector<uint8_t> stream;
    U32(stream, static_cast<uint32_t>(4 + packed.size() - 17));
    U32(stream, static_cast<uint32_t>(snapshot.size()));
    stream.insert(stream.end(), packed.begin() + 17, packed.end());
    std::vector<uint8_t> packet{0x00};
    U16(packet, 200); U16(packet, 0); U32(packet, 0); U16(packet, 0);
    packet.push_back(static_cast<uint8_t>(stream.size()));
    packet.insert(packet.end(), stream.begin(), stream.end()); packet.push_back(0);
    Feed(client, packet);
    CHECK(client.worldUnit == hero);
    Player(client, 1, "host", 1, true);
    client.Update(); // start the existing host-gone grace period
}

static void ReturnLifecycles() {
    // Independent clients share the two existing grace-period waits.
    net::Client clients[3];
    for (auto& client : clients) StartReturn(client);
    std::this_thread::sleep_for(std::chrono::milliseconds(3100));
    for (auto& client : clients) {
        client.Update();
        CHECK(ReturnStatus(client, "IN PROGRESS") == 1);
        CHECK(ReturnStatus(client, "EXECUTED") == 0);
        Player(client, 1, "host", 1, true);
        Player(client, 1, "host", 1, true);
        client.Update();
        CHECK(ReturnStatus(client, "IN PROGRESS") == 1);
        CHECK(ReturnStatus(client, "EXECUTED") == 0);
    }
    auto& cancelled = clients[0];
    auto& completed = clients[1];
    auto& rejected = clients[2];
    // GUI Stop cancels the action but still lets the already-pending final save finish.
    cancelled.CancelHostReturn("user disconnected");
    cancelled.LeaveWorld(); // already leaving: must not revive the cancelled action
    cancelled.CancelHostReturn("user disconnected");
    CHECK(ReturnStatus(cancelled, "CANCELLED") == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    for (auto& client : clients) {
        client.Update();
        CHECK(client.state == net::Client::State::Idle && client.reconnect);
        CHECK(client.worldUnit == 0);
        CHECK(ReturnStatus(client, "EXECUTED") == 0); // leave/disconnect is not success
        Player(client, 4, "bot", 1); // a late old-connection record is not confirmation
        CHECK(ReturnStatus(client, "EXECUTED") == 0);
        client.SetCharacter("bot", "hero", 0, {0});
        Connect(client);
        CHECK(client.players.empty() && !client.InTown());
        CHECK(ReturnStatus(client, "EXECUTED") == 0);
    }

    Accepted(completed);
    CHECK(ReturnStatus(completed, "EXECUTED") == 0); // handshake alone is insufficient
    Player(completed, 1, "host", 1, true);
    CHECK(ReturnStatus(completed, "EXECUTED") == 0); // the host's lobby is not ours
    Player(completed, 4, "bot", 3);
    CHECK(ReturnStatus(completed, "EXECUTED") == 0); // our non-lobby record is insufficient
    Player(completed, 4, "bot", 1);
    CHECK(completed.InTown() && ReturnStatus(completed, "EXECUTED") == 1);
    Player(completed, 4, "bot", 1);
    Player(completed, 1, "host", 1, true);
    Player(completed, 4, "bot", 0);
    Player(completed, 4, "bot", 1);
    CHECK(ReturnStatus(completed, "IN PROGRESS") == 1);
    CHECK(ReturnStatus(completed, "EXECUTED") == 1);
    CHECK(ReturnStatus(completed, "FAILED") == 0 && ReturnStatus(completed, "CANCELLED") == 0);

    Feed(rejected, {0x07, 1, 0, 0, 0, 3});
    CHECK(rejected.state == net::Client::State::Rejected);
    CHECK(ReturnStatus(rejected, "FAILED") == 1);
    CHECK(ReturnStatus(rejected, "EXECUTED") == 0);
    Player(rejected, 4, "bot", 1);
    CHECK(ReturnStatus(rejected, "EXECUTED") == 0);
    // A later successful manual connection cannot turn a failed/cancelled return into success.
    for (net::Client* client : {&cancelled, &rejected}) {
        client->SetCharacter("bot", "hero", 0, {0});
        Connect(*client); Accepted(*client); Player(*client, 4, "bot", 1);
        CHECK(client->InTown());
        CHECK(ReturnStatus(*client, "EXECUTED") == 0);
    }
    CHECK(ReturnStatus(cancelled, "CANCELLED") == 1 && ReturnStatus(cancelled, "FAILED") == 0);
    CHECK(ReturnStatus(rejected, "FAILED") == 1 && ReturnStatus(rejected, "CANCELLED") == 0);
    for (auto& client : clients) client.Disconnect();
}

int main() {
    net::Client client;
    client.autoEnter = false;
    client.SetCharacter("bot", "hero", 0, {0});
    CHECK(!client.InTown());
    Connect(client);
    // Updates from before acceptance cannot become this connection's town confirmation.
    Player(client, 4, "bot", 1);
    Accepted(client);
    CHECK(client.players.empty());
    CHECK(!client.InTown());
    Player(client, 1, "host", 1, true);
    CHECK(!client.InTown());
    Player(client, 4, "bot", 0);
    CHECK(client.joined && !client.InTown() && client.worldUnit == 0);
    Player(client, 4, "bot", 1);
    CHECK(client.InTown());
    Player(client, 5, "bot", 3);
    CHECK(!client.InTown()); // the older ghost's lobby record must not grant editing
    Player(client, 4, "bot", 1);
    CHECK(!client.InTown());
    Player(client, 5, "bot", 1);
    CHECK(client.InTown());
    client.reconnect = true;
    CHECK(!client.InTown());
    client.reconnect = false;
    Object(client, 1, 5, {}, true);
    CHECK(!client.InTown()); // removing the current own record does not revive its ghost
    Player(client, 6, "bot", 1);
    CHECK(client.InTown());

    // Starting a new connection invalidates every previous own record, even for the same character.
    Connect(client);
    CHECK(client.players.empty() && !client.joined && !client.InTown());
    Accepted(client);
    Player(client, 1, "host", 1, true);
    CHECK(!client.InTown());
    Player(client, 6, "bot", 1);
    CHECK(client.InTown());
    client.SetCharacter("bot", "hero", 0, {0});
    CHECK(!client.InTown());
    Player(client, 6, "bot", 1);
    CHECK(client.InTown());

    // The callback sees the confirmed lobby and runs once, before entry changes its phase.
    std::vector<uint8_t> chosen; Text(chosen, "testquest"); Object(client, 9, 10, chosen);
    std::vector<uint8_t> open; Text(open, "testquest_2"); Object(client, 8, 11, open);
    int callbacks = 0;
    client.beforeQuestEntry = [&]() { CHECK(client.InTown()); ++callbacks; };
    client.holdEntry = true;
    client.autoEnter = true;
    client.Update();
    CHECK(callbacks == 0 && client.InTown());
    client.holdEntry = false;
    client.Update();
    CHECK(callbacks == 1 && !client.InTown() && client.worldUnit == 0);
    client.Update();
    CHECK(callbacks == 1);
    client.Disconnect();
    CHECK(!client.InTown() && !client.joined && !client.reconnect);
    // A late old update after explicit disconnect cannot restore town access.
    Player(client, 6, "bot", 1);
    CHECK(!client.InTown());
    ReturnLifecycles();

    std::printf("net lifecycle: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
