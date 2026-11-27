#include "audio_route.hpp"
#include <glib.h>
#include <glib/gstdio.h>
#include <filesystem>
#include <sys/stat.h>

using namespace Contrail;

namespace {
std::string Read(const std::string &path) {
  gchar *text = nullptr;
  if (!g_file_get_contents(path.c_str(), &text, nullptr, nullptr)) return {};
  auto result = std::string(text);
  g_free(text);
  return result;
}
void Wait(const std::function<bool()> &condition) {
  auto *loop = g_main_loop_new(nullptr, FALSE);
  auto state = std::pair(loop, &condition);
  const auto poll = g_timeout_add(10, +[](gpointer data) -> gboolean {
    auto &pending = *static_cast<std::pair<GMainLoop *, const std::function<bool()> *> *>(data);
    if ((*pending.second)()) g_main_loop_quit(pending.first);
    return G_SOURCE_CONTINUE;
  }, &state);
  const auto timeout = g_timeout_add_seconds(5, +[](gpointer) -> gboolean {
    g_error("Audio route did not settle"); return G_SOURCE_REMOVE;
  }, nullptr);
  g_main_loop_run(loop);
  g_source_remove(poll);
  g_source_remove(timeout);
  g_main_loop_unref(loop);
}
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, nullptr);
  auto *temporary = g_dir_make_tmp("contrail-route-test-XXXXXX", nullptr);
  g_assert_nonnull(temporary);
  const auto directory = std::string(temporary);
  g_free(temporary);
  g_setenv("CONTRAIL_ROUTE_TEST_DIR", directory.c_str(), TRUE);
  g_setenv("PATH", (directory + ":" + g_getenv("PATH")).c_str(), TRUE);
  // A translated pactl subscription used to prevent noticing volume changes and new headsets.
  g_setenv("LC_ALL", "ru_RU.UTF-8", TRUE);
  const auto script = directory + "/pactl";
  const char code[] = R"python(#!/usr/bin/python3
import json, os, sys
root = os.environ['CONTRAIL_ROUTE_TEST_DIR']
path = root + '/state.json'
args = sys.argv[1:]
if args == ['subscribe']:
    fd = os.open(root + '/events', os.O_RDWR)
    open(root + '/monitor-locale', 'w').write(os.environ.get('LC_ALL', ''))
    for line in os.fdopen(fd):
        print("Event 'change' on sink #0", flush=True)
    sys.exit(0)
state = json.load(open(path))
if args == ['--format=json', 'info']:
    print(json.dumps({'default_sink_name': 'phone'}))
elif args == ['--format=json', 'list', 'sinks']:
    print(json.dumps(state))
else:
    if args[0] == 'set-sink-port': state[0]['active_port'] = args[2]
    elif args[0] == 'set-sink-volume': state[0]['volume']['mono']['value'] = int(args[2])
    else: sys.exit(1)
    json.dump(state, open(path + '.new', 'w'))
    os.replace(path + '.new', path)
    try:
        fd = os.open(root + '/events', os.O_WRONLY | os.O_NONBLOCK)
        os.write(fd, b'change\n')
        os.close(fd)
    except OSError: pass
)python";
  g_assert_true(g_file_set_contents(script.c_str(), code, -1, nullptr));
  g_assert_cmpint(g_chmod(script.c_str(), 0700), ==, 0);
  g_assert_cmpint(mkfifo((directory + "/events").c_str(), 0600), ==, 0);
  const char sinks[] = R"json([{"name":"phone","description":"Output","active_port":"output-speaker",
"properties":{},"volume":{"mono":{"value":15000}},"ports":[
{"name":"output-earpiece","availability":"available"},
{"name":"output-speaker","availability":"available"},
{"name":"output-headphones","availability":"not available"}]},
{"name":"sink.fake.sco","description":null,"active_port":null,"properties":{},"ports":[]}])json";
  g_assert_true(g_file_set_contents((directory + "/state.json").c_str(), sinks, -1, nullptr));
  {
    AudioRoute route([] {}, directory + "/volumes.ini");
    route.begin(false);
    Wait([&] { return !route.busy() && route.current() && route.current()->kind == AudioRoute::Kind::Earpiece; });
    g_assert_cmpuint(route.outputs().size(), ==, 2);
    Wait([&] { return !Read(directory + "/monitor-locale").empty(); });
    g_assert_true(Read(directory + "/monitor-locale") == "C");
    auto *command = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, nullptr, script.c_str(), "set-sink-volume", "phone", "30000", nullptr);
    g_assert_nonnull(command);
    g_assert_true(g_subprocess_wait_check(command, nullptr, nullptr));
    g_object_unref(command);
    Wait([&] { return Read(directory + "/volumes.ini").find("output-earpiece=30000") != std::string::npos; });
    route.select("phone/output-speaker");
    Wait([&] { return !route.busy(); });
    g_assert_true(Read(directory + "/state.json").find("15000") != std::string::npos);
    route.select("phone/output-earpiece");
    Wait([&] { return !route.busy(); });
    g_assert_true(Read(directory + "/state.json").find("30000") != std::string::npos);
    route.end();
    Wait([&] { return !route.busy(); });
    const auto restored = Read(directory + "/state.json");
    g_assert_true(restored.find("\"active_port\": \"output-speaker\"") != std::string::npos);
    g_assert_true(restored.find("15000") != std::string::npos);
  }
  std::filesystem::remove_all(directory);
  g_print("audio-route-test: PASS\n");
}
