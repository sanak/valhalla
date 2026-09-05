#pragma once

#include <valhalla/baldr/tilegetter.h>
#include <valhalla/exceptions.h>

#include <emscripten/val.h>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace valhalla {
namespace bindings {
namespace wasm {

// Delegates each range request to a JS hook installed as Module.tileFetch, so the same getter
// serves sync XHR in a browser worker and a file read under node.
class js_tile_getter_t : public baldr::tile_getter_t {
public:
  js_tile_getter_t(bool gzipped, bool tar_mode) : gzipped_(gzipped), tar_mode_(tar_mode) {
  }

  GET_response_t get(const std::string& url,
                     const uint64_t range_offset = 0,
                     const uint64_t range_size = 0) override {
    if (interrupt_) {
      (*interrupt_)();
    }

    auto hook = emscripten::val::module_property("tileFetch");
    if (hook.isUndefined() || hook.isNull()) {
      throw std::runtime_error("Module.tileFetch is not installed");
    }

    auto response = hook(url, static_cast<double>(range_offset), static_cast<double>(range_size));

    GET_response_t result;
    result.http_code_ = response["httpCode"].as<int>();
    if (result.http_code_ == 200 || result.http_code_ == 206) {
      result.bytes_ = emscripten::convertJSArrayToNumberVector<char>(response["body"]);
      result.status_ = status_code_t::SUCCESS;
      return result;
    }

    // index.bin already promised this tile, so a miss is a broken tar, never a tileset edge.
    // loki turns a plain std::exception into a 171, which reads as "nothing to route to here"
    if (tar_mode_) {
      throw valhalla_exception_t(0,
                                 "Couldn't read from " + url + " with HTTP status " +
                                     std::to_string(result.http_code_),
                                 500, "Internal Server Error", "");
    }
    return result;
  }

  HEAD_response_t head(const std::string&, header_mask_t) override {
    return {};
  }

  bool gzipped() const override {
    return gzipped_;
  }

  void set_interrupt(const interrupt_t* interrupt) override {
    interrupt_ = interrupt;
  }

private:
  const bool gzipped_;
  const bool tar_mode_;
  const interrupt_t* interrupt_ = nullptr;
};

} // namespace wasm
} // namespace bindings
} // namespace valhalla
