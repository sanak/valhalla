#include "baldr/graphreader.h"
#include "baldr/tilegetter.h"
#include "test.h"

#include <boost/property_tree/ptree.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace vb = valhalla::baldr;

class TestGraphReader : vb::GraphReader {
public:
  using vb::GraphReader::GetGraphTile;
  using vb::GraphReader::GetGraphTileHeader;
  using vb::GraphReader::GraphReader;
  using vb::GraphReader::tile_extract_;
};

auto config_tar = test::make_config("test/data/utrecht_tiles",
                                    {{"mjolnir.tile_extract", "test/data/utrecht_tiles/tiles.tar"}});
auto config_dir = test::make_config("test/data/utrecht_tiles");

TEST(TarIndexer, TestTrafficTar) {
  // read the tile headers from tar & dir tiles and memcmp them
  TestGraphReader reader_tar(config_tar.get_child("mjolnir"));
  vb::GraphReader reader_dir(config_dir.get_child("mjolnir"));

  for (const auto& tile_id : reader_dir.GetTileSet()) {
    auto dir_tile = reader_dir.GetGraphTile(tile_id);
    auto tar_tile = reader_tar.GetGraphTile(tile_id);

    ASSERT_EQ(memcmp(reinterpret_cast<const char*>(dir_tile->header()),
                     reinterpret_cast<const char*>(tar_tile->header()), sizeof(vb::GraphTileHeader)),
              0);
  }
}

TEST(TarIndexer, GetGraphTileHeader) {
  TestGraphReader reader_tar(config_tar.get_child("mjolnir"));
  vb::GraphReader reader_dir(config_dir.get_child("mjolnir"));

  EXPECT_FALSE(reader_dir.GetGraphTileHeader(vb::GraphId{}));
  // tile_dir graph, but no file on disk for this tile
  EXPECT_FALSE(reader_dir.GetGraphTileHeader(vb::GraphId{999999, 2, 0}));
  EXPECT_FALSE(reader_tar.GetGraphTileHeader(vb::GraphId{999999, 2, 0}));

  // tile_dir graph with a file too small to hold a header
  auto truncated_id = vb::GraphId{999998, 2, 0};
  std::filesystem::path truncated_path{"test/data/utrecht_tiles"};
  truncated_path /= vb::GraphTile::FileSuffix(truncated_id);
  {
    std::ofstream truncated_file(truncated_path, std::ios::binary);
    truncated_file << "not a tile";
  }
  EXPECT_FALSE(reader_dir.GetGraphTileHeader(truncated_id));
  std::filesystem::remove(truncated_path);

  for (const auto& tile_id : reader_dir.GetTileSet()) {
    // nothing cached yet: read from the file's header span / the mmapped extract
    auto dir_header = reader_dir.GetGraphTileHeader(tile_id);
    auto tar_header = reader_tar.GetGraphTileHeader(tile_id);
    ASSERT_TRUE(dir_header && tar_header);

    auto tile = reader_dir.GetGraphTile(tile_id);
    EXPECT_EQ(memcmp(&*dir_header, tile->header(), sizeof(vb::GraphTileHeader)), 0);
    EXPECT_EQ(memcmp(&*tar_header, tile->header(), sizeof(vb::GraphTileHeader)), 0);

    // the tile is cached now
    auto cached_header = reader_dir.GetGraphTileHeader(tile_id);
    ASSERT_TRUE(cached_header);
    EXPECT_EQ(memcmp(&*cached_header, tile->header(), sizeof(vb::GraphTileHeader)), 0);
  }
}

TEST(TarIndexer, CheckScanTar) {
  config_tar.add("mjolnir.data_processing.scan_tar", true);
  TestGraphReader reader_tar(config_tar.get_child("mjolnir"));

  ASSERT_NE(reader_tar.tile_extract_->checksum, 0);
}

// serves byte ranges of a local tar, standing in for a server that honours Range requests
class file_range_getter_t : public vb::tile_getter_t {
public:
  explicit file_range_getter_t(std::string path) : path_(std::move(path)) {
  }
  GET_response_t get(const std::string&, const uint64_t offset, const uint64_t size) override {
    std::ifstream file(path_, std::ios::binary);
    file.seekg(static_cast<std::streamoff>(offset));
    GET_response_t response;
    response.bytes_.resize(size);
    file.read(response.bytes_.data(), static_cast<std::streamsize>(size));
    response.bytes_.resize(static_cast<size_t>(file.gcount()));
    response.status_ = status_code_t::SUCCESS;
    response.http_code_ = 206;
    return response;
  }
  HEAD_response_t head(const std::string&, header_mask_t) override {
    return {};
  }

private:
  std::string path_;
};

TEST(TarIndexer, RemoteTarTileSetMatchesExtract) {
  auto remote = config_tar.get_child("mjolnir");
  remote.erase("tile_extract");
  remote.put("tile_dir", "");
  remote.put("tile_url", "http://localhost/tiles.tar");
  vb::GraphReader reader_remote(remote, std::make_unique<file_range_getter_t>(
                                            "test/data/utrecht_tiles/tiles.tar"));
  vb::GraphReader reader_tar(config_tar.get_child("mjolnir"));

  const auto expected = reader_tar.GetTileSet();
  ASSERT_FALSE(expected.empty());
  EXPECT_EQ(reader_remote.GetTileSet(), expected);
  for (uint8_t level = 0; level <= 2; ++level) {
    EXPECT_EQ(reader_remote.GetTileSet(level), reader_tar.GetTileSet(level)) << int(level);
  }
}
