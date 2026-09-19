#include "baldr/graphreader.h"
#include "test.h"

#include <boost/property_tree/ptree.hpp>

#include <filesystem>
#include <fstream>

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

const std::string kGzTar = "test/data/utrecht_tiles/tiles_gz.tar";

TEST(TarIndexer, GzippedExtractIsNotMapped) {
  auto config = test::make_config("test/data/utrecht_tiles", {{"mjolnir.tile_extract", kGzTar}});
  TestGraphReader reader(config.get_child("mjolnir"));

  EXPECT_TRUE(reader.tile_extract_->tiles.empty());
}

// Serves byte ranges straight out of a local file, standing in for a tar behind an HTTP server.
class file_range_getter_t : public vb::tile_getter_t {
public:
  explicit file_range_getter_t(const std::string& path) : file_(path, std::ios::binary) {
  }

  GET_response_t get(const std::string&, const uint64_t offset, const uint64_t size) override {
    GET_response_t result;
    result.bytes_.resize(size);
    file_.seekg(static_cast<std::streamoff>(offset));
    file_.read(result.bytes_.data(), static_cast<std::streamsize>(size));
    result.status_ = file_ ? status_code_t::SUCCESS : status_code_t::FAILURE;
    result.http_code_ = file_ ? 206 : 416;
    return result;
  }

  HEAD_response_t head(const std::string&, header_mask_t) override {
    return {};
  }

  bool gzipped() const override {
    return true;
  }

private:
  std::ifstream file_;
};

TEST(TarIndexer, RemoteGzippedTarMatchesTileDir) {
  boost::property_tree::ptree pt;
  pt.put("tile_url", "http://localhost/tiles_gz.tar");
  vb::GraphReader reader_gz(pt, std::make_unique<file_range_getter_t>(kGzTar));
  vb::GraphReader reader_dir(config_dir.get_child("mjolnir"));

  ASSERT_TRUE(reader_gz.HasRemoteTileIndex());
  EXPECT_EQ(reader_gz.GetTileSet(), reader_dir.GetTileSet());
  for (const auto& tile_id : reader_dir.GetTileSet()) {
    auto dir_tile = reader_dir.GetGraphTile(tile_id);
    auto gz_tile = reader_gz.GetGraphTile(tile_id);
    ASSERT_TRUE(gz_tile) << tile_id;
    EXPECT_EQ(dir_tile->header()->end_offset(), gz_tile->header()->end_offset());
    EXPECT_EQ(memcmp(dir_tile->header(), gz_tile->header(), sizeof(vb::GraphTileHeader)), 0);
  }
}
