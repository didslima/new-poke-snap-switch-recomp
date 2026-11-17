// hle::Json (runtime/hle/json_lite.h) behaves the same on every standard library: parse, query, build, copy, move.
// Built by the top-level CMakeLists.txt (ctest json_lite). The header holds a vector of members of its own, which libstdc++
// rejects while the type is incomplete, so this test also keeps that shape compiling.
#include <cstdio>
#include <string>
#include <utility>

#include "json_lite.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main() {
  using hle::Json;
  Json j = Json::object();
  j.set("name", Json::string("snap"));
  j.set("n", Json::number(2.5));
  j.set("on", Json::boolean_(true));
  Json arr = Json::array();
  arr.a.push_back(Json::number(1));
  arr.a.push_back(Json::string("x"));
  j.set("list", std::move(arr));

  CHECK(j.type == Json::Obj);
  CHECK(j.get("name") && j.get("name")->str() == "snap");
  CHECK(j.get("n") && j.get("n")->num() == 2.5);
  CHECK(j.get("on") && j.get("on")->boolean() == true);
  CHECK(j.get("list") && j.get("list")->a.size() == 2);
  CHECK(j.get("missing") == nullptr);
  CHECK(j.get("missing") == nullptr && Json().str("d") == "d");

  j.set("n", Json::number(3));                       // set replaces in place
  CHECK(j.o.size() == 4 && j.get("n")->num() == 3);
  CHECK(j.o[0].first == "name" && j.o[0].second.str() == "snap");   // members keep insertion order and the .first/.second names

  Json copy = j;                                     // deep copy
  copy.set("name", Json::string("other"));
  CHECK(j.get("name")->str() == "snap" && copy.get("name")->str() == "other");
  Json moved = std::move(copy);
  CHECK(moved.get("name")->str() == "other");

  Json parsed;
  std::string err;
  CHECK(hle::json_parse("{\"a\": [1, 2, {\"b\": \"c\"}], \"t\": true, \"s\": \"hi\"}", parsed, err));
  CHECK(parsed.get("a") && parsed.get("a")->a.size() == 3 && parsed.get("a")->a[2].get("b")->str() == "c");
  CHECK(parsed.get("t") && parsed.get("t")->boolean() && parsed.get("s")->str() == "hi");
  Json bad;
  CHECK(!hle::json_parse("{\"a\": ", bad, err) && !err.empty());

  std::printf(g_fail ? "json_lite: %d failure(s)\n" : "json_lite: OK\n", g_fail);
  return g_fail ? 1 : 0;
}
