// SPDX-License-Identifier: MIT
// Live settings (project code, ours). An options row that applies with no restart reaches the game the same way a restart row does, through its
// environment variable: hle/settings.cpp writes the variable again the moment the row changes (export_live) and then moves the generation below.
// Code that keeps a parsed copy of such a variable asks changed() where it uses the copy and parses again when it says so. One atomic load per
// use; nothing else runs until a setting really changes.
//
//   static std::atomic<unsigned> seen{0};
//   if (hle::live::changed(seen)) { hle::live::EnvRead r; g_value = parse(getenv("SNAP_X")); }
//
// Header-only so the NVN layer, the host mods and the UI share it without a build-list entry.
#pragma once
#include <atomic>
#include <shared_mutex>

namespace hle::live {
// Moves every time a setting was written. Starts at 1, so a reader whose `seen` starts at 0 parses on its first use.
inline std::atomic<unsigned>& gen() { static std::atomic<unsigned> g{1}; return g; }
// The environment block. The C runtime frees a variable's old text when it is replaced, so the writer holds this exclusively while it replaces
// variables and a reader holds it shared from its getenv until it is done with the returned text.
inline std::shared_mutex& env_mu() { static std::shared_mutex m; return m; }
// Nests (a parser that calls another live getter): only the outermost one on a thread takes the lock, a second shared lock could wait behind a writer forever.
struct EnvRead {
  bool top;
  EnvRead() : top(depth()++ == 0) { if (top) env_mu().lock_shared(); }
  ~EnvRead() { if (top) env_mu().unlock_shared(); --depth(); }
  EnvRead(const EnvRead&) = delete;
  EnvRead& operator=(const EnvRead&) = delete;
  static int& depth() { thread_local int d = 0; return d; }
};
// True once per change for the owner of `seen` (the first call is true when `seen` starts at 0).
inline bool changed(std::atomic<unsigned>& seen) {
  const unsigned g = gen().load(std::memory_order_acquire);
  if (seen.load(std::memory_order_relaxed) == g) return false;
  seen.store(g, std::memory_order_relaxed);
  return true;
}
inline void bump() { gen().fetch_add(1, std::memory_order_release); }
// A parsed copy that follows the settings: parse() runs on the first use and again after every change.
template <class T, class F> T cached(std::atomic<unsigned>& seen, std::atomic<T>& v, F&& parse) {
  if (changed(seen)) { EnvRead r; v.store(parse(), std::memory_order_relaxed); }
  return v.load(std::memory_order_relaxed);
}
}  // namespace hle::live
// SNAP_LIVE(float, Strength("SNAP_BOKEH_DOF")): the old `static const float v = ...; return v;` that also follows a changed setting.
#define SNAP_LIVE(T, expr)   ([&]() -> T { static std::atomic<unsigned> seen_{0}; static std::atomic<T> v_{}; return ::hle::live::cached<T>(seen_, v_, [&]() -> T { return (expr); }); }())
