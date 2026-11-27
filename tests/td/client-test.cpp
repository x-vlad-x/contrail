// The client, strings and login rules on a scripted transport: no network, no account.
#include "td/client.hpp"
#include "td/login_errors.hpp"
#include "td/session.hpp"
#include "td/strings.hpp"

#include <glib.h>

#include <deque>

namespace Contrail {
namespace {

namespace api = Td::api;

class ScriptedTransport final : public Td::Transport {
 public:
  struct State {
    Sink sink;
    std::vector<std::pair<std::uint64_t, std::int32_t>> sent;
  };

  explicit ScriptedTransport(State &state) : _state(state) {}
  void start(Sink sink) override { _state.sink = std::move(sink); }
  void send(std::uint64_t id, Td::FunctionPtr function) override { _state.sent.emplace_back(id, function->get_id()); }

 private:
  State &_state;
};

Td::ObjectPtr Text(const char *value) {
  return api::make_object<api::text>(value);
}

void AnswerReachesItsRequest() {
  auto state = ScriptedTransport::State();
  auto client = Td::Client(std::make_unique<ScriptedTransport>(state));
  auto lifetime = Td::Lifetime();
  auto first = std::string();
  auto second = std::string();
  client.request<api::text>(api::make_object<api::getCountryCode>(), lifetime, [&](api::text &text) { first = text.text_; });
  client.request<api::text>(api::make_object<api::getCountryCode>(), lifetime, [&](api::text &text) { second = text.text_; });
  g_assert_cmpuint(state.sent.size(), ==, 2);
  state.sink(state.sent[1].first, Text("second"));
  state.sink(state.sent[0].first, Text("first"));
  g_assert_cmpstr(first.c_str(), ==, "first");
  g_assert_cmpstr(second.c_str(), ==, "second");
}

void AnswerAfterTheViewClosedIsDropped() {
  auto state = ScriptedTransport::State();
  auto client = Td::Client(std::make_unique<ScriptedTransport>(state));
  auto called = false;
  {
    auto lifetime = Td::Lifetime();
    client.request<api::text>(api::make_object<api::getCountryCode>(), lifetime, [&](api::text &) { called = true; });
  }
  state.sink(state.sent[0].first, Text("late"));
  g_assert_false(called);
}

void ResetDropsWhatWasAskedBefore() {
  auto state = ScriptedTransport::State();
  auto client = Td::Client(std::make_unique<ScriptedTransport>(state));
  auto lifetime = Td::Lifetime();
  auto called = false;
  client.request<api::text>(api::make_object<api::getCountryCode>(), lifetime, [&](api::text &) { called = true; });
  lifetime.reset();
  state.sink(state.sent[0].first, Text("late"));
  g_assert_false(called);
}

void ErrorsGoToTheirHandler() {
  auto state = ScriptedTransport::State();
  auto client = Td::Client(std::make_unique<ScriptedTransport>(state));
  auto lifetime = Td::Lifetime();
  auto specific = 0;
  auto general = std::string();
  client.setErrorHandler([&](api::error &error) { general = error.message_; });
  client.request<api::text>(api::make_object<api::getCountryCode>(), lifetime, [](api::text &) { g_assert_not_reached(); },
                            [&](api::error &error) { specific = error.code_; });
  client.request<api::text>(api::make_object<api::getCountryCode>(), lifetime, [](api::text &) { g_assert_not_reached(); });
  state.sink(state.sent[0].first, api::make_object<api::error>(400, "A"));
  state.sink(state.sent[1].first, api::make_object<api::error>(420, "FLOOD_WAIT_30"));
  g_assert_cmpint(specific, ==, 400);
  g_assert_cmpstr(general.c_str(), ==, "FLOOD_WAIT_30");
  g_assert_cmpint(Td::FloodWaitSeconds(general), ==, 30);
  g_assert_cmpint(Td::FloodWaitSeconds("PHONE_CODE_INVALID"), ==, 0);
}

void UpdatesFollowTheirSubscribers() {
  auto state = ScriptedTransport::State();
  auto client = Td::Client(std::make_unique<ScriptedTransport>(state));
  auto seen = 0;
  auto lifetime = std::make_unique<Td::Lifetime>();
  client.subscribe([&](api::Update &) { ++seen; }, *lifetime);
  const auto update = [&] {
    state.sink(0, api::make_object<api::updateAuthorizationState>(api::make_object<api::authorizationStateReady>()));
  };
  update();
  g_assert_cmpint(seen, ==, 1);
  lifetime = nullptr;
  update();
  g_assert_cmpint(seen, ==, 1);
}

void SessionAsksForTheParametersAndTellsALoss() {
  auto state = ScriptedTransport::State();
  auto session = Td::Session(std::make_unique<ScriptedTransport>(state),
                             {g_get_tmp_dir() + std::string("/contrail-client-test"), "ru", "test"});
  auto closed = -1;
  session.setClosedHandler([&](bool lost) { closed = lost; });
  const auto authorization = [&](auto state2) {
    state.sink(0, api::make_object<api::updateAuthorizationState>(std::move(state2)));
  };
  authorization(api::make_object<api::authorizationStateWaitTdlibParameters>());
  g_assert_cmpint(state.sent.back().second, ==, api::setTdlibParameters::ID);
  authorization(api::make_object<api::authorizationStateLoggingOut>());
  authorization(api::make_object<api::authorizationStateClosed>());
  g_assert_cmpint(closed, ==, 1);
}

void LoggingOutOnPurposeIsNotALoss() {
  auto state = ScriptedTransport::State();
  auto session = Td::Session(std::make_unique<ScriptedTransport>(state), {"", "en", "test"});
  auto closed = -1;
  session.setClosedHandler([&](bool lost) { closed = lost; });
  session.logOut();
  state.sink(0, api::make_object<api::updateAuthorizationState>(api::make_object<api::authorizationStateLoggingOut>()));
  state.sink(0, api::make_object<api::updateAuthorizationState>(api::make_object<api::authorizationStateClosed>()));
  g_assert_cmpint(closed, ==, 0);
}

void PluralRules() {
  // kShift: 1 one, 3 few, 4 many, 5 other.
  g_assert_cmpint(PluralForm("en", 1), ==, 1);
  g_assert_cmpint(PluralForm("en", 2), ==, 5);
  g_assert_cmpint(PluralForm("ru", 1), ==, 1);
  g_assert_cmpint(PluralForm("ru", 21), ==, 1);
  g_assert_cmpint(PluralForm("ru", 3), ==, 3);
  g_assert_cmpint(PluralForm("ru", 11), ==, 4);
  g_assert_cmpint(PluralForm("ru", 25), ==, 4);
  g_assert_cmpint(PluralForm("uk", 2), ==, 3);
  g_assert_cmpint(PluralForm("pl", 5), ==, 4);
  g_assert_cmpint(PluralForm("ar", 0), ==, 0);
  g_assert_cmpint(PluralForm("pt-br", 1), ==, 1);
  g_assert_cmpint(PluralForm("ja", 1), ==, 5);
}

void BuiltInStrings() {
  // In variables: g_assert_cmpstr would keep the pointer of a temporary past its end.
  const auto plain = Tr("lng_cancel");
  const auto unknown = Tr("lng_no_such_key_at_all");
  const auto formatted = Tr("lng_signin_hint", {{"password_hint", "cat"}});
  g_assert_cmpstr(plain.c_str(), ==, "Cancel");
  g_assert_cmpstr(unknown.c_str(), ==, "lng_no_such_key_at_all");
  g_assert_cmpstr(formatted.c_str(), ==, "Hint: cat");
}

void LoginErrors() {
  using Td::ClassifyLoginError;
  using Td::LoginError;
  g_assert_true(ClassifyLoginError(400, "PHONE_CODE_INVALID") == LoginError::BadCode);
  g_assert_true(ClassifyLoginError(400, "PHONE_CODE_EXPIRED") == LoginError::ExpiredCode);
  g_assert_true(ClassifyLoginError(400, "PASSWORD_HASH_INVALID") == LoginError::BadPassword);
  g_assert_true(ClassifyLoginError(400, "PHONE_NUMBER_INVALID") == LoginError::BadPhone);
  g_assert_true(ClassifyLoginError(400, "PHONE_NUMBER_BANNED") == LoginError::BannedPhone);
  g_assert_true(ClassifyLoginError(429, "Too Many Requests: retry after 60") == LoginError::Flood);
  g_assert_true(ClassifyLoginError(500, "Something") == LoginError::Other);
}

}  // namespace
}  // namespace Contrail

int main(int argc, char **argv) {
  using namespace Contrail;
  g_test_init(&argc, &argv, nullptr);
  g_test_add_func("/client/answers", AnswerReachesItsRequest);
  g_test_add_func("/client/closed-view", AnswerAfterTheViewClosedIsDropped);
  g_test_add_func("/client/reset", ResetDropsWhatWasAskedBefore);
  g_test_add_func("/client/errors", ErrorsGoToTheirHandler);
  g_test_add_func("/client/updates", UpdatesFollowTheirSubscribers);
  g_test_add_func("/session/lost", SessionAsksForTheParametersAndTellsALoss);
  g_test_add_func("/session/logout", LoggingOutOnPurposeIsNotALoss);
  g_test_add_func("/strings/plurals", PluralRules);
  g_test_add_func("/strings/built-in", BuiltInStrings);
  g_test_add_func("/login/errors", LoginErrors);
  return g_test_run();
}
