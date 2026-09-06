#include "baldr/graphreader.h"
#include "baldr/graphtile.h"
#include "baldr/rapidjson_utils.h"
#include "config.h"
#include "exceptions.h"
#include "js_tile_getter.h"
#include "midgard/logging.h"
#include "tyr/actor.h"

#include <boost/property_tree/ptree.hpp>
#include <emscripten/bind.h>
#include <emscripten/em_js.h>

#include <functional>
#include <memory>
#include <sstream>
#include <string>

// A C++ exception escaping into JS surfaces as an opaque WebAssembly.Exception, so every
// entry point converts to a real JS Error before unwinding.
EM_JS(void, throw_valhalla_error, (const char* message, int code, int http_code), {
  const err = new Error(UTF8ToString(message));
  err.name = 'ValhallaError';
  if (code) {
    err.code = code;
  }
  if (http_code) {
    err.httpCode = http_code;
  }
  throw err;
});

// The only channel that reaches a worker blocked inside a synchronous wasm call: the main
// thread's Atomics.store lands in shared memory, which needs no event loop to observe.
// clang-format off
EM_JS(int, cancel_requested, (), {
  return Module.cancelFlag &&
         Atomics.load(Module.cancelFlag, 0) === Module.currentRequestId ? 1 : 0;
});
// clang-format on

EM_JS(void, throw_abort_error, (), { throw new DOMException('request cancelled', 'AbortError'); });

namespace {

boost::property_tree::ptree configure(const std::string& config) {
  boost::property_tree::ptree pt;
  std::stringstream stream(config);
  rapidjson::read_json(stream, pt);
  valhalla::midgard::logging::ConfigureFromPtree(pt);
  return pt;
}

} // namespace

// Wraps tyr::actor_t. Every action takes a JSON request string and returns a JSON response
// string; valhalla errors come back as the same JSON error body the HTTP service produces,
// so callers can branch on the presence of `error_code`.
class Actor {
public:
  explicit Actor(const std::string& config) {
    try {
      config_ = configure(config);
      const auto tile_url = config_.get<std::string>("mjolnir.tile_url", "");
      if (tile_url.empty()) {
        actor_ = std::make_unique<valhalla::tyr::actor_t>(config_, true);
        return;
      }
      const bool tar_mode =
          tile_url.find(valhalla::baldr::GraphTile::kTilePathPattern) == std::string::npos;
      const auto gzipped = config_.get<bool>("mjolnir.tile_url_gz", false);
      using valhalla::bindings::wasm::js_tile_getter_t;
      auto getter = std::make_unique<js_tile_getter_t>(gzipped, tar_mode);
      reader_ = std::make_unique<valhalla::baldr::GraphReader>(config_.get_child("mjolnir"),
                                                               std::move(getter));
      // a per-tile URL has no index, so GetTileSet() has nothing to enumerate and the
      // connectivity map comes out empty, which makes loki reject every route as unconnected
      if (!tar_mode) {
        config_.put("loki.use_connectivity", false);
      }
      actor_ = std::make_unique<valhalla::tyr::actor_t>(config_, *reader_, true);
    } catch (const valhalla::valhalla_exception_t& e) {
      throw_valhalla_error(e.message.c_str(), e.code, e.http_code);
    } catch (const std::exception& e) { throw_valhalla_error(e.what(), 0, 0); }
  }

#define VALHALLA_WASM_ACTION(name)                                                                   \
  std::string name(const std::string& request) {                                                     \
    try {                                                                                            \
      return actor_->name(request, &interrupt_);                                                     \
    } catch (const valhalla::interrupt_exception_t&) {                                               \
      throw_abort_error();                                                                           \
    } catch (const valhalla::valhalla_exception_t& e) {                                              \
      throw_valhalla_error(e.message.c_str(), e.code, e.http_code);                                  \
    } catch (const std::exception& e) { throw_valhalla_error(e.what(), 0, 0); }                      \
    return {};                                                                                       \
  }

  VALHALLA_WASM_ACTION(route)
  VALHALLA_WASM_ACTION(locate)
  VALHALLA_WASM_ACTION(matrix)
  VALHALLA_WASM_ACTION(optimized_route)
  VALHALLA_WASM_ACTION(isochrone)
  VALHALLA_WASM_ACTION(trace_route)
  VALHALLA_WASM_ACTION(trace_attributes)
  VALHALLA_WASM_ACTION(height)
  VALHALLA_WASM_ACTION(transit_available)
  VALHALLA_WASM_ACTION(expansion)
  VALHALLA_WASM_ACTION(centroid)
  VALHALLA_WASM_ACTION(status)
#undef VALHALLA_WASM_ACTION

private:
  boost::property_tree::ptree config_;
  // actor_t holds a non-owning pointer to the reader, so it must be declared first and die last
  std::unique_ptr<valhalla::baldr::GraphReader> reader_;
  std::unique_ptr<valhalla::tyr::actor_t> actor_;
  const std::function<void()> interrupt_ = [] {
    if (cancel_requested()) {
      throw valhalla::interrupt_exception_t{};
    }
  };
};

std::string version() {
  return VALHALLA_PRINT_VERSION;
}

EMSCRIPTEN_BINDINGS(valhalla) {
  emscripten::function("version", &version);

  emscripten::class_<Actor>("Actor")
      .constructor<std::string>()
      .function("route", &Actor::route)
      .function("locate", &Actor::locate)
      .function("matrix", &Actor::matrix)
      .function("optimizedRoute", &Actor::optimized_route)
      .function("isochrone", &Actor::isochrone)
      .function("traceRoute", &Actor::trace_route)
      .function("traceAttributes", &Actor::trace_attributes)
      .function("height", &Actor::height)
      .function("transitAvailable", &Actor::transit_available)
      .function("expansion", &Actor::expansion)
      .function("centroid", &Actor::centroid)
      .function("status", &Actor::status);
}
